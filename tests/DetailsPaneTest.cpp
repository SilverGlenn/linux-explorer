#include "DetailsPane.h"

#include <QDir>
#include <QFile>
#include <QLabel>
#include <QTemporaryDir>
#include <QtTest>
#include <KFileItem>
#include <KIO/UDSEntry>
#include <sys/stat.h>

class DetailsPaneTest : public QObject {
    Q_OBJECT
private slots:
    void selectedDirectoryUsesFlexibleReadableFields()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.path() + QStringLiteral("/A directory name long enough to need room");
        QVERIFY(QDir().mkpath(path));

        DetailsPane pane;
        pane.resize(700, 54);
        pane.show();
        pane.showSelection({KFileItem(QUrl::fromLocalFile(path))});
        QCoreApplication::processEvents();

        auto *name = pane.findChild<QLabel *>(QStringLiteral("detailsPrimary"));
        auto *type = pane.findChild<QLabel *>(QStringLiteral("detailsSecondary"));
        auto *date = pane.findChild<QLabel *>(QStringLiteral("detailsTopMetadata"));
        QVERIFY(name && type && date);
        QCOMPARE(name->toolTip(), QStringLiteral("A directory name long enough to need room"));
        QCOMPARE(name->accessibleName(), name->toolTip());
        QCOMPARE(type->toolTip(), QStringLiteral("File folder"));
        QVERIFY(date->toolTip().startsWith(QStringLiteral("Date modified:")));
        QVERIFY(name->width() > 100);
        QCOMPARE(name->geometry().center().y(), date->geometry().center().y());

        pane.resize(260, 54);
        QCoreApplication::processEvents();
        QCOMPARE(name->toolTip(), QStringLiteral("A directory name long enough to need room"));
        QCOMPARE(type->toolTip(), QStringLiteral("File folder"));
    }

    void creationTimeIsShownForFilesButNeverDirectories()
    {
        const QDateTime modified = QDateTime::fromSecsSinceEpoch(1700000000);
        const QDateTime created = QDateTime::fromSecsSinceEpoch(1600000000);
        const QUrl parentUrl(QStringLiteral("test:/parent"));
        auto makeItem = [&](const QString &name, mode_t type, const QString &mime) {
            KIO::UDSEntry entry;
            entry.fastInsert(KIO::UDSEntry::UDS_NAME, name);
            entry.fastInsert(KIO::UDSEntry::UDS_FILE_TYPE, type);
            entry.fastInsert(KIO::UDSEntry::UDS_ACCESS, 0755);
            entry.fastInsert(KIO::UDSEntry::UDS_MODIFICATION_TIME, modified.toSecsSinceEpoch());
            entry.fastInsert(KIO::UDSEntry::UDS_CREATION_TIME, created.toSecsSinceEpoch());
            if (!mime.isEmpty())
                entry.fastInsert(KIO::UDSEntry::UDS_MIME_TYPE, mime);
            return KFileItem(entry, parentUrl, false, true);
        };
        const KFileItem directory = makeItem(QStringLiteral("folder"), S_IFDIR, QString());
        const KFileItem file = makeItem(QStringLiteral("note.txt"), S_IFREG, QStringLiteral("text/plain"));
        QVERIFY(directory.isDir());
        QVERIFY(file.isFile());
        QCOMPARE(directory.time(KFileItem::ModificationTime), modified);
        QCOMPARE(directory.time(KFileItem::CreationTime), created);
        QCOMPARE(file.time(KFileItem::CreationTime), created);

        DetailsPane pane;
        auto *createdField = pane.findChild<QLabel *>(QStringLiteral("detailsCreatedMetadata"));
        QVERIFY(createdField);
        pane.showSelection({directory});
        QCOMPARE(createdField->toolTip(), QString());
        QVERIFY(createdField->isHidden());
        pane.showSelection({file});
        QVERIFY(createdField->toolTip().startsWith(QStringLiteral("Date created:")));
        QVERIFY(!createdField->isHidden());
    }

    void multipleSelectionAndComputerContextsRemainInformative()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString filePath = temporary.path() + QStringLiteral("/note.txt");
        QFile file(filePath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("note"), qint64(4));
        file.close();
        const QString directoryPath = temporary.path() + QStringLiteral("/folder");
        QVERIFY(QDir().mkpath(directoryPath));

        DetailsPane pane;
        pane.showSelection({KFileItem(QUrl::fromLocalFile(filePath)),
                            KFileItem(QUrl::fromLocalFile(directoryPath))});
        QCOMPARE(pane.findChild<QLabel *>(QStringLiteral("detailsPrimary"))->toolTip(), QStringLiteral("2 items selected"));
        QVERIFY(pane.findChild<QLabel *>(QStringLiteral("detailsBottomMetadata"))->toolTip().contains(QStringLiteral("files only")));
        QCOMPARE(pane.findChild<QLabel *>(QStringLiteral("detailsTopMetadata"))->toolTip(), QString());

        pane.showComputerSummary();
        QVERIFY(!pane.findChild<QLabel *>(QStringLiteral("detailsPrimary"))->toolTip().isEmpty());
    }

    void summariesClearSelectionMetadata()
    {
        DetailsPane pane;
        pane.showSelection({KFileItem(QUrl::fromLocalFile(QDir::homePath()))});
        pane.showFolderSummary(3);
        QCOMPARE(pane.findChild<QLabel *>(QStringLiteral("detailsPrimary"))->toolTip(), QStringLiteral("3 items"));
        QCOMPARE(pane.findChild<QLabel *>(QStringLiteral("detailsTopMetadata"))->toolTip(), QString());
        QCOMPARE(pane.findChild<QLabel *>(QStringLiteral("detailsBottomMetadata"))->toolTip(), QString());

        pane.showLibrary(QIcon(), QStringLiteral("Documents"));
        QCOMPARE(pane.findChild<QLabel *>(QStringLiteral("detailsPrimary"))->toolTip(), QStringLiteral("Documents"));
        QCOMPARE(pane.findChild<QLabel *>(QStringLiteral("detailsSecondary"))->toolTip(), QStringLiteral("Library"));
        pane.showMessage(QStringLiteral("Unavailable"));
        QCOMPARE(pane.findChild<QLabel *>(QStringLiteral("detailsPrimary"))->toolTip(), QStringLiteral("Unavailable"));
        QCOMPARE(pane.height(), 54);
    }
};

QTEST_MAIN(DetailsPaneTest)
#include "DetailsPaneTest.moc"
