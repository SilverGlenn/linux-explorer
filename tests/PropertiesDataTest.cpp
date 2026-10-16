#include "PropertiesData.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStorageInfo>
#include <QTemporaryDir>
#include <QtTest>

class PropertiesDataTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void snapshotsFileMetadata()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        const QString path = directory.filePath(QStringLiteral("notes.txt"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("hello"), qint64(5));
        file.close();

        const PropertiesData data = PropertiesData::fromItems(
            KFileItemList{KFileItem(QUrl::fromLocalFile(path))});

        QCOMPARE(data.kind, PropertiesData::Kind::File);
        QCOMPARE(data.realName, QStringLiteral("notes.txt"));
        QCOMPARE(data.size, quint64(5));
        QVERIFY(data.singleUrl.isLocalFile());
        QCOMPARE(data.singleUrl.toLocalFile(), path);
        QVERIFY(data.modified.isValid());
        QVERIFY(data.allocatedSizeKnown);
    }

    void folderIsNotARecursiveSize()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QFile file(directory.filePath(QStringLiteral("child.bin")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(QByteArray(32, 'x')), qint64(32));
        file.close();

        const PropertiesData data = PropertiesData::fromItems(
            KFileItemList{KFileItem(QUrl::fromLocalFile(directory.path()))});

        QCOMPARE(data.kind, PropertiesData::Kind::Folder);
        QCOMPARE(data.size, quint64(0));
        QCOMPARE(data.location, QFileInfo(directory.path()).absolutePath());
    }

    void multipleSelectionSumsFilesOnly()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString firstPath = directory.filePath(QStringLiteral("first.bin"));
        const QString secondPath = directory.filePath(QStringLiteral("second.bin"));
        QFile first(firstPath);
        QVERIFY(first.open(QIODevice::WriteOnly));
        QCOMPARE(first.write(QByteArray(3, 'a')), qint64(3));
        first.close();
        QFile second(secondPath);
        QVERIFY(second.open(QIODevice::WriteOnly));
        QCOMPARE(second.write(QByteArray(7, 'b')), qint64(7));
        second.close();
        QVERIFY(QDir().mkdir(directory.filePath(QStringLiteral("folder"))));

        const PropertiesData data = PropertiesData::fromItems(KFileItemList{
            KFileItem(QUrl::fromLocalFile(firstPath)),
            KFileItem(QUrl::fromLocalFile(secondPath)),
            KFileItem(QUrl::fromLocalFile(directory.filePath(QStringLiteral("folder")))),
        });

        QCOMPARE(data.kind, PropertiesData::Kind::Multiple);
        QCOMPARE(data.count, 3);
        QCOMPARE(data.size, quint64(10));
    }

    void symlinkToFilesystemRootIsNotADrive()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString linkPath = directory.filePath(QStringLiteral("root-link"));
        if (!QFile::link(QStringLiteral("/"), linkPath))
            QSKIP("The filesystem does not allow creating a symlink here");

        const PropertiesData data = PropertiesData::fromItems(
            KFileItemList{KFileItem(QUrl::fromLocalFile(linkPath))});

        QVERIFY(data.kind != PropertiesData::Kind::Drive);
    }

    void onlySystemRootClaimsCDriveName()
    {
        const PropertiesData root = PropertiesData::fromItems(
            KFileItemList{KFileItem(QUrl::fromLocalFile(QStringLiteral("/")))});
        QCOMPARE(root.kind, PropertiesData::Kind::Drive);
        QCOMPARE(root.displayName, QStringLiteral("Local Disk (C:)"));

        for (const QStorageInfo &storage : QStorageInfo::mountedVolumes()) {
            const QString mountPoint = QDir::cleanPath(storage.rootPath());
            if (!storage.isValid() || !storage.isReady()
                || mountPoint.isEmpty() || mountPoint == QLatin1String("/")) {
                continue;
            }
            const QFileInfo mountInfo(mountPoint);
            if (!mountInfo.isDir() || mountInfo.isSymLink())
                continue;

            const PropertiesData mounted = PropertiesData::fromItems(
                KFileItemList{KFileItem(QUrl::fromLocalFile(mountPoint))});
            if (mounted.kind != PropertiesData::Kind::Drive)
                continue;

            QVERIFY(mounted.displayName != QStringLiteral("Local Disk (C:)"));
            QCOMPARE(mounted.mountPoint, mountPoint);
            return;
        }

        QSKIP("No accessible non-root filesystem mount is available");
    }

    void readOnlyTracksOwnerWriteBit()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("readonly.bin"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("data"), qint64(4));
        file.close();

        QVERIFY(QFile::setPermissions(path, QFile::ReadOwner));
        const PropertiesData readOnly = PropertiesData::fromItems(
            KFileItemList{KFileItem(QUrl::fromLocalFile(path))});
        QVERIFY(readOnly.readOnly);

        QVERIFY(QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner));
        const PropertiesData writable = PropertiesData::fromItems(
            KFileItemList{KFileItem(QUrl::fromLocalFile(path))});
        QVERIFY(!writable.readOnly);
    }
};

QTEST_GUILESS_MAIN(PropertiesDataTest)
#include "PropertiesDataTest.moc"
