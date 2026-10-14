#include "DirectoryModel.h"
#include "FileView.h"
#include "Settings.h"

#include <KFilePreviewGenerator>
#include <KIO/CopyJob>
#include <KIO/PreviewJob>
#include <KIO/SimpleJob>
#include <KJob>

#include <QAbstractItemView>
#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QIcon>
#include <QImage>
#include <QItemSelectionModel>
#include <QLineEdit>
#include <QSignalSpy>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

namespace {
// Real PNGs, deliberately unlike a MIME icon. Every test gets new local URLs,
// and run-ui-tests.sh provides an empty, private thumbnail cache.
bool writeImage(const QString &path, const QColor &color)
{
    QImage image(320, 240, QImage::Format_RGB32);
    image.fill(color);
    return image.save(path, "PNG");
}

bool hasColor(const QModelIndex &index, const QColor &color, int size = 96)
{
    const QImage image = qvariant_cast<QIcon>(index.data(Qt::DecorationRole))
                             .pixmap(size, size).toImage();
    if (image.isNull()) return false;
    const QColor pixel = image.pixelColor(image.rect().center());
    return qAbs(pixel.red() - color.red()) < 8
        && qAbs(pixel.green() - color.green()) < 8
        && qAbs(pixel.blue() - color.blue()) < 8;
}

void saveCapture(FileView &view, const QString &name)
{
    const QString folder = qEnvironmentVariable("EXPLORER_SNAPSHOTS");
    if (folder.isEmpty()) return;
    QDir().mkpath(folder);
    view.grab().save(folder + QLatin1Char('/') + name + QStringLiteral(".png"));
}
} // namespace

class ThumbnailLoadingTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(KIO::PreviewJob::availablePlugins().contains(QStringLiteral("imagethumbnail")));
        Settings::setShowHiddenFiles(false);
        Settings::setHideKnownExtensions(false);
        Settings::setSingleClickToOpen(false);
    }

    void realProviderPopulatesAndReusesLocalCache()
    {
        QTemporaryDir files;
        QVERIFY(files.isValid());
        const QUrl url = QUrl::fromLocalFile(files.filePath(QStringLiteral("provider.png")));
        const QColor color(224, 36, 70), cachedColor(30, 170, 120);
        QVERIFY(writeImage(url.toLocalFile(), color));
        const QString hash = QString::fromLatin1(QCryptographicHash::hash(
            url.toEncoded(), QCryptographicHash::Md5).toHex());
        const QString cache = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation)
            + QStringLiteral("/thumbnails/normal/") + hash + QStringLiteral(".png");
        QVERIFY(!QFile::exists(cache));
        const QStringList plugins{QStringLiteral("imagethumbnail")};
        for (int pass = 0; pass < 2; ++pass) {
            const KFileItem item(url, QStringLiteral("image/png"), 0100000);
            auto *job = KIO::filePreview(KFileItemList{item}, QSize(128, 128), &plugins);
            QSignalSpy previews(job, &KIO::PreviewJob::gotPreview);
            QSignalSpy finished(job, &KJob::result);
            QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 10000);
            QCOMPARE(previews.count(), 1);
            const QImage image = qvariant_cast<QPixmap>(previews.first().at(1)).toImage();
            QCOMPARE(image.pixelColor(image.rect().center()), pass == 0 ? color : cachedColor);
            QVERIFY(QFile::exists(cache));
            QImage cached(cache);
            QVERIFY(!cached.isNull());
            QCOMPARE(cached.text(QStringLiteral("Thumb::URI")), url.toString(QUrl::FullyEncoded));
            if (pass == 0) {
                // Keep the real provider's URI/mtime metadata, changing only
                // its private cached pixels. A second request must read this
                // valid cache entry, not decode the original PNG again.
                cached.fill(cachedColor);
                QVERIFY(cached.save(cache, "PNG"));
            }
        }
    }

    void oneGeneratorOwnsTheSharedModel()
    {
        DirectoryModel model;
        FileView view(&model);
        view.resize(600, 320);
        view.show();
        for (int i = 0; i <= int(Settings::ViewMode::Content); ++i) {
            view.setViewMode(Settings::ViewMode(i));
            QCOMPARE(view.findChildren<KFilePreviewGenerator *>().size(), 1);
            QVERIFY(view.findChild<KFilePreviewGenerator *>()->isPreviewShown());
        }
    }

    void coldFolderShowsDistinctStablePreviewsWithoutInteraction()
    {
        QTemporaryDir files;
        QVERIFY(files.isValid());
        const QColor red(220, 30, 70), green(30, 180, 90);
        const QUrl first = QUrl::fromLocalFile(files.filePath(QStringLiteral("First.png")));
        const QUrl second = QUrl::fromLocalFile(files.filePath(QStringLiteral("Second.png")));
        QVERIFY(writeImage(first.toLocalFile(), red));
        QVERIFY(writeImage(second.toLocalFile(), green));
        DirectoryModel model;
        FileView view(&model);
        view.setViewMode(Settings::ViewMode::LargeIcons);
        view.resize(600, 320);
        view.show();
        QSignalSpy changes(model.model(), &QAbstractItemModel::dataChanged);
        model.setUrl(QUrl::fromLocalFile(files.path()));
        QTRY_COMPARE(model.rowCount(), 2);
        // A real MIME icon remains available before asynchronous decoding.
        QVERIFY(!qvariant_cast<QIcon>(model.indexForUrl(first).data(Qt::DecorationRole)).isNull());
        QTRY_VERIFY_WITH_TIMEOUT(hasColor(model.indexForUrl(first), red), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(hasColor(model.indexForUrl(second), green), 10000);
        const int settled = changes.count();
        QTest::qWait(1200);
        QVERIFY(hasColor(model.indexForUrl(first), red));
        QVERIFY(hasColor(model.indexForUrl(second), green));
        // Decoration writes must not wake another generator on the same model.
        QVERIFY2(changes.count() - settled <= 2, "Preview decoration updates kept retriggering work");
        saveCapture(view, QStringLiteral("thumbnails-cold-folder"));
    }

    void iconSizeChangesRegenerateAtTheNewSizeAndPreserveSelection()
    {
        QTemporaryDir files;
        QVERIFY(files.isValid());
        const QUrl url = QUrl::fromLocalFile(files.filePath(QStringLiteral("Resize.png")));
        const QColor color(45, 130, 225);
        QVERIFY(writeImage(url.toLocalFile(), color));
        DirectoryModel model;
        FileView view(&model);
        view.resize(600, 320);
        view.setViewMode(Settings::ViewMode::LargeIcons);
        view.show();
        model.setUrl(QUrl::fromLocalFile(files.path()));
        QTRY_COMPARE(model.rowCount(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(hasColor(model.indexForUrl(url), color), 10000);
        QVERIFY(view.selectUrl(url));
        view.setViewMode(Settings::ViewMode::ExtraLargeIcons);
        QTRY_VERIFY_WITH_TIMEOUT(qvariant_cast<QIcon>(model.indexForUrl(url).data(Qt::DecorationRole))
                                    .actualSize(QSize(256, 256)).width() >= 200, 10000);
        for (int i = 0; i <= int(Settings::ViewMode::Content); ++i) {
            view.setViewMode(Settings::ViewMode(i));
            const auto selected = model.itemsForIndexes(view.selectedIndexes());
            QCOMPARE(selected.size(), 1);
            QCOMPARE(selected.first().url(), url);
            QTRY_VERIFY_WITH_TIMEOUT(hasColor(model.indexForUrl(url), color,
                                             Settings::iconSizeFor(Settings::ViewMode(i))), 10000);
        }
        view.setViewMode(Settings::ViewMode::ExtraLargeIcons);
        QTRY_VERIFY_WITH_TIMEOUT(qvariant_cast<QIcon>(model.indexForUrl(url).data(Qt::DecorationRole))
                                    .actualSize(QSize(256, 256)).width() >= 200, 10000);
        saveCapture(view, QStringLiteral("thumbnails-extra-large"));
    }

    void loadingPageRevealDoesNotNeedScrollingOrRevisiting()
    {
        QTemporaryDir files;
        QVERIFY(files.isValid());
        const QColor color(230, 95, 30);
        for (int i = 0; i < 120; ++i)
            QVERIFY(writeImage(files.filePath(QStringLiteral("Image-%1.png").arg(i, 3, 10, QLatin1Char('0'))), color));
        DirectoryModel model;
        QStackedWidget pages;
        auto *view = new FileView(&model);
        view->setViewMode(Settings::ViewMode::LargeIcons);
        pages.addWidget(view);
        auto *loading = new QWidget;
        pages.addWidget(loading);
        pages.resize(600, 320);
        pages.show();
        connect(&model, &DirectoryModel::loadingStarted, &pages, [&] { pages.setCurrentWidget(loading); });
        connect(&model, &DirectoryModel::loadingFinished, &pages, [&] { pages.setCurrentWidget(view); });
        model.setUrl(QUrl::fromLocalFile(files.path()));
        QTRY_COMPARE(model.rowCount(), 120);
        QTRY_VERIFY(view->isVisible());
        const QUrl first = QUrl::fromLocalFile(files.filePath(QStringLiteral("Image-000.png")));
        QTRY_VERIFY_WITH_TIMEOUT(hasColor(model.indexForUrl(first), color), 10000);
        saveCapture(*view, QStringLiteral("thumbnails-loading-page-reveal"));
    }

    void rapidNavigationAndGroupingDiscardObsoleteWork()
    {
        QTemporaryDir files;
        QVERIFY(files.isValid());
        const QString oldFolder = files.filePath(QStringLiteral("Old"));
        const QString currentFolder = files.filePath(QStringLiteral("Current"));
        QVERIFY(QDir().mkdir(oldFolder));
        QVERIFY(QDir().mkdir(currentFolder));
        for (int i = 0; i < 80; ++i)
            QVERIFY(writeImage(oldFolder + QStringLiteral("/Old-%1.png").arg(i), QColor(230, 40, 40)));
        const QUrl current = QUrl::fromLocalFile(currentFolder + QStringLiteral("/Current.png"));
        const QColor green(30, 200, 100);
        QVERIFY(writeImage(current.toLocalFile(), green));
        DirectoryModel model;
        FileView view(&model);
        view.setViewMode(Settings::ViewMode::LargeIcons);
        view.resize(600, 320);
        view.show();
        model.setUrl(QUrl::fromLocalFile(oldFolder));
        QTRY_COMPARE(model.rowCount(), 80);
        for (int i = 0; i < 6; ++i) {
            model.setUrl(QUrl::fromLocalFile(i % 2 ? currentFolder : oldFolder));
            view.setViewMode(i % 2 ? Settings::ViewMode::ExtraLargeIcons : Settings::ViewMode::Tiles);
            QTest::qWait(5);
        }
        QTRY_COMPARE(model.rowCount(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(hasColor(model.indexForUrl(current), green), 10000);
        QVERIFY(view.selectUrl(current));
        view.setGroupColumn(DirectoryModel::Type);
        QCOMPARE(view.viewMode(), Settings::ViewMode::Details);
        QCOMPARE(view.findChildren<KFilePreviewGenerator *>().size(), 1);
        QVERIFY(view.findChild<KFilePreviewGenerator *>()->isPreviewShown());
        // QIcon::actualSize() uses physical pixels for these cached pixmaps.
        QTRY_VERIFY_WITH_TIMEOUT(qvariant_cast<QIcon>(model.indexForUrl(current).data(Qt::DecorationRole))
                                    .actualSize(QSize(256, 256)).width() <= 16 * view.devicePixelRatioF(), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(hasColor(model.indexForUrl(current), green, 16), 10000);
        QVERIFY(view.selectUrl(current));
        const auto selected = model.itemsForIndexes(view.selectedIndexes());
        QCOMPARE(selected.size(), 1);
        QCOMPARE(selected.first().url(), current);
        view.setViewMode(Settings::ViewMode::LargeIcons);
        QCOMPARE(view.groupColumn(), -1);
        QTRY_VERIFY_WITH_TIMEOUT(hasColor(model.indexForUrl(current), green), 10000);
        QTest::qWait(300);
        QCOMPARE(model.rowCount(), 1);
        QVERIFY(hasColor(model.indexForUrl(current), green));
    }

    void thumbnailSelectionRenameAndCopyUseRealKioUrls()
    {
        QTemporaryDir files;
        QVERIFY(files.isValid());
        const QString source = files.filePath(QStringLiteral("Source"));
        const QString destination = files.filePath(QStringLiteral("Copy"));
        QVERIFY(QDir().mkdir(source));
        QVERIFY(QDir().mkdir(destination));
        const QUrl original = QUrl::fromLocalFile(source + QStringLiteral("/Original.png"));
        const QUrl renamed = QUrl::fromLocalFile(source + QStringLiteral("/Renamed.png"));
        const QColor color(130, 65, 220);
        QVERIFY(writeImage(original.toLocalFile(), color));
        DirectoryModel model;
        FileView view(&model);
        view.setViewMode(Settings::ViewMode::LargeIcons);
        view.resize(600, 320);
        view.show();
        model.setUrl(QUrl::fromLocalFile(source));
        QTRY_COMPARE(model.rowCount(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(hasColor(model.indexForUrl(original), color), 10000);
        QVERIFY(view.selectUrl(original));
        QSignalSpy renameRequest(&view, &FileView::renameRequested);
        view.renameItem(original);
        QTRY_VERIFY(view.findChild<QLineEdit *>());
        auto *editor = view.findChild<QLineEdit *>();
        QTRY_COMPARE(editor->selectedText(), QStringLiteral("Original"));
        QTest::keyClick(editor, Qt::Key_Escape);
        QCOMPARE(renameRequest.count(), 0);
        QVERIFY(QFile::exists(original.toLocalFile()));
        auto *rename = KIO::rename(original, renamed, KIO::HideProgressInfo);
        QSignalSpy renameFinished(rename, &KJob::result);
        QTRY_COMPARE(renameFinished.count(), 1);
        QTRY_VERIFY(model.indexForUrl(renamed).isValid());
        QVERIFY(!model.indexForUrl(original).isValid());
        QVERIFY(view.selectUrl(renamed));
        const auto selected = model.itemsForIndexes(view.selectedIndexes());
        QCOMPARE(selected.size(), 1);
        QCOMPARE(selected.first().url(), renamed);
        auto *copy = KIO::copy(selected.first().url(), QUrl::fromLocalFile(destination), KIO::HideProgressInfo);
        QSignalSpy copyFinished(copy, &KJob::result);
        QTRY_COMPARE(copyFinished.count(), 1);
        const QString copied = destination + QStringLiteral("/Renamed.png");
        QTRY_VERIFY(QFile::exists(copied));
        QCOMPARE(QImage(copied).pixelColor(100, 100), color);
        QTRY_VERIFY_WITH_TIMEOUT(hasColor(model.indexForUrl(renamed), color), 10000);
    }
};

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("linux-explorer-win7-tests"));
    app.setApplicationName(QStringLiteral("thumbnail-loading-tests"));
    QIcon::setThemeName(QStringLiteral("breeze"));
    ThumbnailLoadingTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "ThumbnailLoadingTest.moc"
