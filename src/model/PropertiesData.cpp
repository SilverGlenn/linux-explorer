#include "PropertiesData.h"

#include "Branding.h"

#include <KFileItemListProperties>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStorageInfo>

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

QString displayUrl(const QUrl &url)
{
    return url.toDisplayString(QUrl::PreferLocalFile);
}

bool isFilesystemRoot(const KFileItem &item, QStorageInfo *storageOut)
{
    if (!item.url().isLocalFile() || !item.isDir() || item.isLink())
        return false;

    const QString path = QDir::cleanPath(item.url().toLocalFile());
    const QFileInfo info(path);
    if (info.isSymLink())
        return false;

    // A canonical-path check also excludes a mount reached through a symlink
    // in one of its parent components.
    const QString canonicalPath = info.canonicalFilePath();
    if (canonicalPath.isEmpty() || QDir::cleanPath(canonicalPath) != path)
        return false;

    QStorageInfo storage(path);
    if (!storage.isValid() || !storage.isReady())
        return false;

    const QString rootPath = QDir::cleanPath(storage.rootPath());
    if (rootPath != path)
        return false;

    if (storageOut)
        *storageOut = storage;
    return true;
}

bool allocatedSizeFor(const KFileItem &item, quint64 *size)
{
    if (!item.url().isLocalFile())
        return false;

    struct stat info {};
    const QByteArray encodedPath = QFile::encodeName(item.url().toLocalFile());
    if (::lstat(encodedPath.constData(), &info) != 0 || info.st_blocks < 0)
        return false;

    if (size)
        *size = quint64(info.st_blocks) * 512;
    return true;
}

QString parentLocation(const QUrl &url)
{
    if (url.isLocalFile()) {
        const QString path = QDir::cleanPath(url.toLocalFile());
        const QString parent = QFileInfo(path).absolutePath();
        return parent == path ? path : parent;
    }

    return displayUrl(url.adjusted(QUrl::RemoveFilename | QUrl::StripTrailingSlash));
}

bool filesystemAllowsChanges(const QUrl &url)
{
    if (!url.isLocalFile())
        return true;

    const QString parentPath = QFileInfo(url.toLocalFile()).absolutePath();
    const QStorageInfo storage(parentPath);
    return storage.isValid() && storage.isReady() && !storage.isReadOnly();
}

} // namespace

PropertiesData PropertiesData::fromItems(const KFileItemList &items)
{
    PropertiesData data;
    data.count = items.size();
    if (items.isEmpty())
        return data;

    if (items.size() > 1) {
        data.kind = Kind::Multiple;
        data.displayName = QObject::tr("%1 items").arg(items.size());
        data.typeName = QObject::tr("Multiple items");
        data.mimeType = KFileItemListProperties(items).mimeType();
        data.allocatedSizeKnown = true;
        for (const KFileItem &item : items) {
            if (!item.isFile())
                continue;

            data.size += item.size();
            quint64 allocated = 0;
            if (allocatedSizeFor(item, &allocated)) {
                data.sizeOnDisk += allocated;
            } else {
                data.allocatedSizeKnown = false;
            }
        }
        return data;
    }

    const KFileItem &item = items.constFirst();
    data.singleUrl = item.url();
    data.realName = item.name();
    data.displayName = item.text();
    data.mimeType = item.mimetype();
    data.created = item.time(KFileItem::CreationTime);
    data.modified = item.time(KFileItem::ModificationTime);
    data.accessed = item.time(KFileItem::AccessTime);
    data.owner = item.user();
    data.group = item.group();
    data.permissions = uint(item.permissions());
    data.hidden = item.isHidden();
    data.readOnly = !item.isWritable();

    QStorageInfo storage;
    const bool drive = isFilesystemRoot(item, &storage);
    if (drive) {
        data.kind = Kind::Drive;
        data.location = QDir::cleanPath(storage.rootPath());
        data.mountPoint = data.location;
        data.device = QString::fromLocal8Bit(storage.device());
        data.fileSystem = QString::fromLocal8Bit(storage.fileSystemType());

        // C: is only a presentation label for the system root. Other Linux
        // mount points keep their real volume label and device identity.
        if (data.location == QLatin1String("/")) {
            data.displayName = QObject::tr("Local Disk (C:)");
            data.typeName = QObject::tr("Local Disk");
        } else {
            data.displayName = storage.name();
            if (data.displayName.isEmpty())
                data.displayName = item.text();
            if (data.displayName.isEmpty())
                data.displayName = QFileInfo(data.location).fileName();

            const QString deviceNode = QFileInfo(data.device).fileName();
            if (!deviceNode.isEmpty()
                && !data.displayName.contains(QLatin1Char('(') + deviceNode + QLatin1Char(')'))) {
                data.displayName += QStringLiteral(" (%1)").arg(deviceNode);
            }
            data.typeName = QObject::tr("Drive");
        }
        data.totalSize = quint64(qMax<qint64>(0, storage.bytesTotal()));
        data.availableSize = quint64(qMax<qint64>(0, storage.bytesAvailable()));
        data.usedSize = data.totalSize > data.availableSize
            ? data.totalSize - data.availableSize : 0;
        data.readOnly = storage.isReadOnly()
            || !(data.permissions & uint(S_IWUSR));
        data.canRename = false;
        data.canChangePermissions = false;
        return data;
    }

    data.kind = item.isDir() && !item.isLink() ? Kind::Folder : Kind::File;
    if (data.kind == Kind::Folder) {
        data.displayName = Branding::displayName(item.url(), item.text());
        data.typeName = QObject::tr("File folder");
    } else {
        data.typeName = item.mimeComment();
        if (data.typeName.isEmpty())
            data.typeName = QObject::tr("File");
        data.size = item.isFile() ? quint64(item.size()) : 0;
    }
    data.location = parentLocation(item.url());

    quint64 allocated = 0;
    if (allocatedSizeFor(item, &allocated)) {
        data.sizeOnDisk = allocated;
        data.allocatedSizeKnown = true;
    }

    const KFileItemListProperties capabilities(items);
    const bool isRootPath = item.url().isLocalFile()
        && QDir::cleanPath(item.url().toLocalFile()) == QLatin1String("/");
    data.canRename = item.url().scheme() != QLatin1String("trash")
        && !isRootPath && capabilities.supportsMoving();
    if (data.canRename && item.url().isLocalFile()) {
        const QString parentPath = QFileInfo(item.url().toLocalFile()).absolutePath();
        const QFileInfo parentInfo(parentPath);
        const QStorageInfo parentStorage(parentPath);
        data.canRename = parentInfo.isWritable()
            && (!parentStorage.isValid() || !parentStorage.isReadOnly());
    }

    // The dialog toggles the owner's write bit; effective root/group access
    // must not make a file look writable when that bit is absent.
    data.readOnly = !(data.permissions & uint(S_IWUSR));
    data.canChangePermissions = !item.isLink() && filesystemAllowsChanges(item.url())
        && (!item.isLocalFile()
            || item.userId() == int(::geteuid()) || ::geteuid() == 0);
    return data;
}
