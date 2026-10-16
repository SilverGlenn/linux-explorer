#include "ApplicationAppearance.h"
#include "DirectoryModel.h"
#include "FileOps.h"
#include "FileView.h"
#include "MainWindow.h"
#include "PropertiesDialog.h"
#include "Settings.h"

#include <KPropertiesDialog>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFontInfo>
#include <QLabel>
#include <QLineEdit>
#include <QMimeData>
#include <QPushButton>
#include <QProcess>
#include <QSignalSpy>
#include <QTabWidget>
#include <QTabBar>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>

namespace {
bool writeFile(const QString &path, const QByteArray &content = "Properties reference\n")
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(content) == content.size();
}

QDialogButtonBox *buttons(PropertiesDialog &dialog)
{
    return dialog.findChild<QDialogButtonBox *>(QStringLiteral("propertiesButtons"));
}

QLineEdit *nameEditor(PropertiesDialog &dialog)
{
    return dialog.findChild<QLineEdit *>(QStringLiteral("propertiesName"));
}

uint permissions(const QString &path)
{
    struct stat status {};
    return ::stat(QFile::encodeName(path).constData(), &status) == 0 ? uint(status.st_mode & 07777) : 0;
}

quint64 allocatedBytes(const QString &path)
{
    struct stat status {};
    return ::lstat(QFile::encodeName(path).constData(), &status) == 0
        ? quint64(status.st_blocks) * 512 : std::numeric_limits<quint64>::max();
}
} // namespace

class PropertiesDialogTest : public QObject {
    Q_OBJECT
private:
    void snapshot(QWidget &widget, const QString &name)
    {
        const QString output = qEnvironmentVariable("PROPERTIES_SNAPSHOTS");
        if (output.isEmpty())
            return;
        QVERIFY(QDir().mkpath(output));
        QCoreApplication::processEvents();
        QVERIFY(widget.grab().save(output + QLatin1Char('/') + name + QStringLiteral(".png")));
    }

private Q_SLOTS:
    void initTestCase()
    {
        if (qEnvironmentVariableIsSet("EXPLORER_NATIVE_THEME"))
            Explorer::configureApplicationAppearance(qobject_cast<QApplication *>(QApplication::instance()));
        QIcon::setThemeName(QStringLiteral("breeze"));
        Settings::setNotifyUnmountedDrives(false);
        Settings::setDefaultViewMode(Settings::ViewMode::Details);
    }

    void fileSheetUsesCompactTabsAndExplicitApply()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("Readme.txt"));
        QVERIFY(writeFile(path));
        PropertiesDialog dialog({KFileItem(QUrl::fromLocalFile(path))});
        dialog.show();
        QTest::qWait(30);
        auto *tabs = dialog.findChild<QTabWidget *>(QStringLiteral("propertiesTabs"));
        QVERIFY(tabs && buttons(dialog) && nameEditor(dialog));
        QCOMPARE(tabs->tabText(0), QStringLiteral("General"));
        QCOMPARE(tabs->tabText(1), QStringLiteral("Security"));
        QCOMPARE(tabs->tabText(2), QStringLiteral("Details"));
        QCOMPARE(dialog.windowTitle(), QStringLiteral("Readme.txt Properties"));
        QCOMPARE(nameEditor(dialog)->text(), QStringLiteral("Readme.txt"));
        QVERIFY(dialog.width() < 600);
        QVERIFY(dialog.height() <= 620);
        QVERIFY(buttons(dialog)->button(QDialogButtonBox::Ok)->isDefault());
        QVERIFY(!buttons(dialog)->button(QDialogButtonBox::Apply)->isEnabled());
        snapshot(dialog, QStringLiteral("file-general"));
        tabs->setCurrentIndex(1);
        snapshot(dialog, QStringLiteral("file-security"));
        tabs->setCurrentIndex(2);
        snapshot(dialog, QStringLiteral("file-details"));
        tabs->setCurrentIndex(0);
        const uint original = permissions(path);
        nameEditor(dialog)->setText(QStringLiteral("Cancelled.txt"));
        QVERIFY(buttons(dialog)->button(QDialogButtonBox::Apply)->isEnabled());
        buttons(dialog)->button(QDialogButtonBox::Cancel)->click();
        QVERIFY(QFileInfo::exists(path));
        QVERIFY(!QFileInfo::exists(directory.filePath(QStringLiteral("Cancelled.txt"))));
        QCOMPARE(permissions(path), original);
    }

    void propertySheetUsesTheLegacyDialogFontAcrossEveryTab()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("Readme.txt"));
        QVERIFY(writeFile(path));
        QWidget parent;
        QFont explorerFont(QStringLiteral("Segoe UI"), 9);
        parent.setFont(explorerFont);
        PropertiesDialog dialog({KFileItem(QUrl::fromLocalFile(path))}, &parent);
        dialog.show();
        QTest::qWait(30);
        const bool referenceFontAvailable = QFontDatabase::families().contains(
            QStringLiteral("Microsoft Sans Serif"), Qt::CaseInsensitive);
        auto *tabs = dialog.findChild<QTabWidget *>(QStringLiteral("propertiesTabs"));
        QVERIFY(tabs);
        for (int index = 0; index < tabs->count(); ++index) {
            tabs->setCurrentIndex(index);
            QCoreApplication::processEvents();
            for (QWidget *control : dialog.findChildren<QWidget *>()) {
                if (!control->isVisible()) continue;
                const QFont font = control->font();
                QCOMPARE(font.pointSizeF(), 8.0);
                QCOMPARE(font.weight(), QFont::Normal);
                QVERIFY(!font.italic());
                if (referenceFontAvailable)
                    QCOMPARE(QFontInfo(font).family(), QStringLiteral("Microsoft Sans Serif"));
            }
        }
        QCOMPARE(parent.font(), explorerFont);
        auto *box = buttons(dialog);
        QVERIFY(box->button(QDialogButtonBox::Ok)->x() < box->button(QDialogButtonBox::Cancel)->x());
        QVERIFY(box->button(QDialogButtonBox::Cancel)->x() < box->button(QDialogButtonBox::Apply)->x());
    }

    void applyRenameUpdatesTheDialogUrl()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("Before.txt"));
        const QString destination = directory.filePath(QStringLiteral("After.txt"));
        QVERIFY(writeFile(path));
        PropertiesDialog dialog({KFileItem(QUrl::fromLocalFile(path))});
        dialog.show();
        QSignalSpy renamed(&dialog, &PropertiesDialog::renamed);
        QSignalSpy applied(&dialog, &PropertiesDialog::applied);
        nameEditor(dialog)->setText(QStringLiteral("After.txt"));
        buttons(dialog)->button(QDialogButtonBox::Apply)->click();
        QTRY_COMPARE(applied.count(), 1);
        QCOMPARE(renamed.count(), 1);
        QCOMPARE(renamed.first().at(0).toUrl(), QUrl::fromLocalFile(path));
        QCOMPARE(renamed.first().at(1).toUrl(), QUrl::fromLocalFile(destination));
        QCOMPARE(dialog.url(), QUrl::fromLocalFile(destination));
        QVERIFY(QFileInfo::exists(destination));
        QVERIFY(!QFileInfo::exists(path));
        QVERIFY(dialog.isVisible());
        QVERIFY(!buttons(dialog)->button(QDialogButtonBox::Apply)->isEnabled());
    }

    void invalidAndOccupiedNamesNeverReplaceFiles()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("Before.txt"));
        const QString occupied = directory.filePath(QStringLiteral("Occupied.txt"));
        QVERIFY(writeFile(path, "original"));
        QVERIFY(writeFile(occupied, "keep"));
        PropertiesDialog dialog({KFileItem(QUrl::fromLocalFile(path))});
        dialog.show();
        QSignalSpy applied(&dialog, &PropertiesDialog::applied);
        auto *error = dialog.findChild<QLabel *>(QStringLiteral("propertiesError"));
        QVERIFY(error);
        for (const QString &invalid : {QStringLiteral("sub/file"), QStringLiteral("Occupied.txt")}) {
            nameEditor(dialog)->setText(invalid);
            buttons(dialog)->button(QDialogButtonBox::Apply)->click();
            QTRY_VERIFY(error->isVisible() && !error->text().isEmpty());
            QCOMPARE(applied.count(), 0);
            QCOMPARE(dialog.url(), QUrl::fromLocalFile(path));
            QVERIFY(QFileInfo::exists(path));
            QFile file(occupied);
            QVERIFY(file.open(QIODevice::ReadOnly));
            QCOMPARE(file.readAll(), QByteArray("keep"));
        }
    }

    void readOnlyChangesOnlyTheOwnerWriteBit()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("Program.sh"));
        QVERIFY(writeFile(path));
        QVERIFY(::chmod(QFile::encodeName(path).constData(), 0754) == 0);
        PropertiesDialog dialog({KFileItem(QUrl::fromLocalFile(path))});
        dialog.show();
        auto *readOnly = dialog.findChild<QCheckBox *>(QStringLiteral("propertiesReadOnly"));
        QVERIFY(readOnly && readOnly->isEnabled());
        QVERIFY(!readOnly->isChecked());
        QSignalSpy applied(&dialog, &PropertiesDialog::applied);
        readOnly->click();
        buttons(dialog)->button(QDialogButtonBox::Apply)->click();
        QTRY_COMPARE(applied.count(), 1);
        QCOMPARE(permissions(path), uint(0554));
        readOnly->click();
        buttons(dialog)->button(QDialogButtonBox::Apply)->click();
        QTRY_COMPARE(applied.count(), 2);
        QCOMPARE(permissions(path), uint(0754));
    }

    void hiddenAttributeUsesTheNativeDotName()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("notes.txt"));
        const QString hiddenPath = directory.filePath(QStringLiteral(".notes.txt"));
        QVERIFY(writeFile(path));
        PropertiesDialog dialog({KFileItem(QUrl::fromLocalFile(path))});
        dialog.show();
        auto *hidden = dialog.findChild<QCheckBox *>(QStringLiteral("propertiesHidden"));
        QVERIFY(hidden && hidden->isEnabled());
        QSignalSpy applied(&dialog, &PropertiesDialog::applied);
        hidden->click();
        buttons(dialog)->button(QDialogButtonBox::Apply)->click();
        QTRY_COMPARE(applied.count(), 1);
        QCOMPARE(dialog.url(), QUrl::fromLocalFile(hiddenPath));
        QVERIFY(QFileInfo::exists(hiddenPath));
        hidden->click();
        buttons(dialog)->button(QDialogButtonBox::Apply)->click();
        QTRY_COMPARE(applied.count(), 2);
        QCOMPARE(dialog.url(), QUrl::fromLocalFile(path));
        QVERIFY(QFileInfo::exists(path));
    }

    void folderTotalsAreCalculatedWithoutBlockingTheDialog()
    {
        QTemporaryDir directory;
        const QString folder = directory.filePath(QStringLiteral("Explorer Reference"));
        QVERIFY(QDir().mkpath(folder + QStringLiteral("/Subfolder")));
        QVERIFY(writeFile(folder + QStringLiteral("/notes.txt"), "12345"));
        QVERIFY(writeFile(folder + QStringLiteral("/Subfolder/data.bin"), "abc"));
        PropertiesDialog dialog({KFileItem(QUrl::fromLocalFile(folder))});
        dialog.show();
        auto *contains = dialog.findChild<QLabel *>(QStringLiteral("propertiesContains"));
        auto *size = dialog.findChild<QLabel *>(QStringLiteral("propertiesSize"));
        auto *disk = dialog.findChild<QLabel *>(QStringLiteral("propertiesSizeOnDisk"));
        QVERIFY(contains && size && disk);
        QVERIFY(dialog.isAllocationCalculating());
        QCOMPARE(disk->text(), QStringLiteral("Calculating..."));
        QTRY_VERIFY(contains->text().contains(QLatin1Char('2')));
        QVERIFY(size->text().contains(QLatin1Char('8')));
        QTRY_VERIFY(!dialog.isAllocationCalculating());
        const quint64 expected = allocatedBytes(folder)
            + allocatedBytes(folder + QStringLiteral("/Subfolder"))
            + allocatedBytes(folder + QStringLiteral("/notes.txt"))
            + allocatedBytes(folder + QStringLiteral("/Subfolder/data.bin"));
        QCOMPARE(dialog.allocationResult().bytes, expected);
        QVERIFY(!dialog.allocationResult().incomplete);
        QVERIFY(disk->text() != QStringLiteral("Not available"));
        QVERIFY(!buttons(dialog)->button(QDialogButtonBox::Apply)->isEnabled());
        snapshot(dialog, QStringLiteral("folder-general"));
    }

    void allocationIncludesEmptyDirectoriesSparseFilesAndUniqueInodes()
    {
        QTemporaryDir directory;
        const QString folder = directory.filePath(QStringLiteral("Selected"));
        const QString nested = folder + QStringLiteral("/Nested");
        const QString empty = nested + QStringLiteral("/Empty");
        const QString second = directory.filePath(QStringLiteral("Also selected"));
        const QString outside = directory.filePath(QStringLiteral("Outside"));
        QVERIFY(QDir().mkpath(empty));
        QVERIFY(QDir().mkpath(second));
        QVERIFY(QDir().mkpath(outside));
        QVERIFY(writeFile(outside + QStringLiteral("/Excluded.bin"), QByteArray(256 * 1024, 'x')));
        const QString sparse = nested + QStringLiteral("/Sparse.bin");
        {
            QFile file(sparse);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QVERIFY(file.seek(64 * 1024 * 1024));
            QCOMPARE(file.write("x", 1), qint64(1));
        }
        const QString hardLink = second + QStringLiteral("/Same inode.bin");
        QVERIFY(::link(QFile::encodeName(sparse).constData(), QFile::encodeName(hardLink).constData()) == 0);
        const QString symlink = folder + QStringLiteral("/Outside link");
        QVERIFY(::symlink(QFile::encodeName(outside).constData(), QFile::encodeName(symlink).constData()) == 0);
        const quint64 expected = allocatedBytes(folder) + allocatedBytes(nested) + allocatedBytes(empty)
            + allocatedBytes(second) + allocatedBytes(sparse) + allocatedBytes(symlink);
        const auto result = FolderAllocation::calculate({folder, second, sparse, folder});
        QCOMPARE(result.bytes, expected);
        QCOMPARE(result.entries, quint64(6));
        QVERIFY(!result.incomplete);
        QVERIFY(!result.cancelled);
        const auto linkOnly = FolderAllocation::calculate({symlink + QLatin1Char('/')});
        QCOMPARE(linkOnly.bytes, allocatedBytes(symlink));
        QCOMPARE(linkOnly.entries, quint64(1));
        QVERIFY(!linkOnly.incomplete);
        // GNU du independently uses allocated blocks and deduplicates hard links.
        QProcess du;
        du.start(QStringLiteral("du"), {QStringLiteral("-s"), QStringLiteral("-B1"),
                                       QStringLiteral("-c"), folder, second});
        QVERIFY(du.waitForFinished());
        QCOMPARE(du.exitCode(), 0);
        const auto total = du.readAllStandardOutput().trimmed().split('\n').last().split('\t').first();
        QCOMPARE(total.toULongLong(), expected);

        PropertiesDialog multiple({KFileItem(QUrl::fromLocalFile(sparse)),
                                   KFileItem(QUrl::fromLocalFile(hardLink))});
        QTRY_VERIFY(!multiple.isAllocationCalculating());
        QCOMPARE(multiple.allocationResult().bytes, allocatedBytes(sparse));
    }

    void allocationCancellationAndMissingEntriesAreIncomplete()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("present.txt"));
        QVERIFY(writeFile(path));
        const auto cancel = std::make_shared<std::atomic_bool>(true);
        const auto cancelled = FolderAllocation::calculate({directory.path()}, cancel);
        QVERIFY(cancelled.cancelled);
        QVERIFY(cancelled.incomplete);
        QCOMPARE(cancelled.bytes, quint64(0));
        const auto missing = FolderAllocation::calculate({path, directory.filePath(QStringLiteral("deleted"))});
        QVERIFY(missing.incomplete);
        QVERIFY(!missing.cancelled);
        QCOMPARE(missing.bytes, allocatedBytes(path));
    }

    void allocationUnreadableDirectoryIsPartial()
    {
        if (::geteuid() == 0)
            QSKIP("Root can enumerate directories without read permission");
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("Unreadable"));
        QVERIFY(QDir().mkpath(path));
        QVERIFY(writeFile(path + QStringLiteral("/hidden.txt")));
        QVERIFY(::chmod(QFile::encodeName(path).constData(), 0000) == 0);
        const auto result = FolderAllocation::calculate({path});
        const int restored = ::chmod(QFile::encodeName(path).constData(), 0700);
        QVERIFY(restored == 0);
        QVERIFY(result.incomplete);
        QCOMPARE(result.bytes, allocatedBytes(path));
    }

    void folderAllocationRestartsAfterRenameAndDialogCanCloseDuringScan()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("Before"));
        const QString destination = directory.filePath(QStringLiteral("After"));
        QVERIFY(QDir().mkpath(path));
        QVERIFY(writeFile(path + QStringLiteral("/payload.bin"), "small"));
        PropertiesDialog dialog({KFileItem(QUrl::fromLocalFile(path))});
        QSignalSpy applied(&dialog, &PropertiesDialog::applied);
        connect(&dialog, &PropertiesDialog::renamed, &dialog, [destination] {
            QVERIFY(writeFile(destination + QStringLiteral("/payload.bin"), QByteArray(128 * 1024, 'x')));
        });
        nameEditor(dialog)->setText(QStringLiteral("After"));
        buttons(dialog)->button(QDialogButtonBox::Apply)->click();
        QTRY_COMPARE(applied.count(), 1);
        QTRY_VERIFY(!dialog.isAllocationCalculating());
        QCOMPARE(dialog.allocationResult().bytes,
                 allocatedBytes(destination) + allocatedBytes(destination + QStringLiteral("/payload.bin")));
        QVERIFY(!dialog.allocationResult().incomplete);
        // Deleting a sheet before queued completion must never invoke its callbacks.
        auto *closing = new PropertiesDialog({KFileItem(QUrl::fromLocalFile(destination))});
        QVERIFY(closing->isAllocationCalculating());
        delete closing;
        QCoreApplication::processEvents();
    }

    void systemDriveShowsCapacityAndProtectsTheUiAlias()
    {
        PropertiesDialog dialog({KFileItem(QUrl::fromLocalFile(QStringLiteral("/")))});
        dialog.show();
        QTest::qWait(30);
        QVERIFY(nameEditor(dialog));
        QVERIFY(nameEditor(dialog)->isReadOnly());
        QCOMPARE(dialog.windowTitle(), QStringLiteral("Local Disk (C:) Properties"));
        QCOMPARE(dialog.url(), QUrl::fromLocalFile(QStringLiteral("/")));
        QVERIFY(!dialog.findChild<QLabel *>(QStringLiteral("propertiesContains")));
        QVERIFY(!buttons(dialog)->button(QDialogButtonBox::Apply)->isEnabled());
        snapshot(dialog, QStringLiteral("drive-general"));
    }

    void specialItemsKeepKioEditors()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("notes.txt"));
        const QString link = directory.filePath(QStringLiteral("notes-link"));
        QVERIFY(writeFile(path));
        QVERIFY(QFile::link(path, link));
        QWidget parent;
        QDialog *dialog = FileOps::showProperties({KFileItem(QUrl::fromLocalFile(link))}, &parent);
        QVERIFY(qobject_cast<KPropertiesDialog *>(dialog));
        dialog->close();
    }

    void securityOpensTheFullPermissionEditorAndPreservesDraftName()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("notes.txt"));
        QVERIFY(writeFile(path));
        PropertiesDialog dialog({KFileItem(QUrl::fromLocalFile(path))});
        dialog.show();
        nameEditor(dialog)->setText(QStringLiteral("pending.txt"));
        auto *tabs = dialog.findChild<QTabWidget *>(QStringLiteral("propertiesTabs"));
        tabs->setCurrentIndex(1);
        auto *edit = dialog.findChild<QPushButton *>(QStringLiteral("propertiesEditPermissions"));
        QVERIFY(edit);
        edit->click();
        auto *native = dialog.findChild<KPropertiesDialog *>();
        QVERIFY(native && native->isVisible());
        QCOMPARE(native->windowModality(), Qt::WindowModal);
        QVERIFY(native->currentPage());
        QCOMPARE(native->currentPage()->name().remove(QLatin1Char('&')), QStringLiteral("Permissions"));
        // Exercise the parent refresh path after a committed desktop edit.
        native->applied();
        QCOMPARE(nameEditor(dialog)->text(), QStringLiteral("pending.txt"));
        QVERIFY(buttons(dialog)->button(QDialogButtonBox::Apply)->isEnabled());
        native->close();
        dialog.reject();
        QVERIFY(QFileInfo::exists(path));
        QVERIFY(!QFileInfo::exists(directory.filePath(QStringLiteral("pending.txt"))));
    }

    void propertiesRenameKeepsExplorerSelectionAndClipboardCurrent_data()
    {
        QTest::addColumn<bool>("folder");
        QTest::newRow("file") << false;
        QTest::newRow("folder") << true;
    }

    void propertiesRenameKeepsExplorerSelectionAndClipboardCurrent()
    {
        QFETCH(bool, folder);
        QTemporaryDir directory;
        const QString before = folder ? QStringLiteral("Before") : QStringLiteral("Before.txt");
        const QString after = folder ? QStringLiteral("After") : QStringLiteral("After.txt");
        const QString path = directory.filePath(before);
        const QString destination = directory.filePath(after);
        QVERIFY(folder ? QDir().mkpath(path) : writeFile(path));
        MainWindow window(QUrl::fromLocalFile(directory.path()));
        window.setAttribute(Qt::WA_DeleteOnClose, false);
        window.show();
        auto *view = window.findChild<FileView *>();
        auto *model = window.findChild<DirectoryModel *>();
        QTRY_VERIFY(model->indexForUrl(QUrl::fromLocalFile(path)).isValid());
        QVERIFY(view->selectUrl(QUrl::fromLocalFile(path)));
        QAction *properties = nullptr;
        for (auto *action : window.findChildren<QAction *>())
            if (action->text() == QLatin1String("Properties")) properties = action;
        QVERIFY(properties);
        properties->trigger();
        auto *dialog = window.findChild<PropertiesDialog *>();
        QVERIFY(dialog);
        nameEditor(*dialog)->setText(after);
        buttons(*dialog)->button(QDialogButtonBox::Apply)->click();
        QTRY_VERIFY(model->indexForUrl(QUrl::fromLocalFile(destination)).isValid());
        const auto selection = [view, model] {
            QList<QUrl> urls;
            for (const KFileItem &item : model->itemsForIndexes(view->selectedIndexes()))
                urls.append(item.url());
            return urls;
        };
        QTRY_VERIFY(selection().contains(QUrl::fromLocalFile(destination)));
        QVERIFY(!selection().contains(QUrl::fromLocalFile(path)));
        for (auto *action : window.findChildren<QAction *>())
            if (action->text() == QLatin1String("Copy")) action->trigger();
        QVERIFY(QApplication::clipboard()->mimeData()->urls().contains(QUrl::fromLocalFile(destination)));
        QApplication::clipboard()->clear();
        dialog->close();
    }

    void renamingTheCurrentFolderNavigatesToItsNewLocation()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("Before"));
        const QString destination = directory.filePath(QStringLiteral("After"));
        QVERIFY(QDir().mkpath(path));
        QVERIFY(writeFile(path + QStringLiteral("/notes.txt")));
        MainWindow window(QUrl::fromLocalFile(path));
        window.setAttribute(Qt::WA_DeleteOnClose, false);
        window.show();
        auto *model = window.findChild<DirectoryModel *>();
        QTRY_COMPARE(model->rowCount(), 1);
        QAction *properties = nullptr;
        for (auto *action : window.findChildren<QAction *>())
            if (action->text() == QLatin1String("Properties")) properties = action;
        QVERIFY(properties);
        properties->trigger();
        auto *dialog = window.findChild<PropertiesDialog *>();
        QVERIFY(dialog);
        nameEditor(*dialog)->setText(QStringLiteral("After"));
        buttons(*dialog)->button(QDialogButtonBox::Apply)->click();
        QTRY_VERIFY(model->url().matches(QUrl::fromLocalFile(destination), QUrl::StripTrailingSlash));
        QTRY_COMPARE(model->rowCount(), 1);
        QVERIFY(model->indexForUrl(QUrl::fromLocalFile(destination + QStringLiteral("/notes.txt"))).isValid());
        dialog->close();
    }
};

QTEST_MAIN(PropertiesDialogTest)
#include "PropertiesDialogTest.moc"
