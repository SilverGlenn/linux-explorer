#pragma once

#include <QDateTime>
#include <QString>
#include <QUrl>
#include <QtGlobal>

#include <KFileItem>

// A read-only snapshot of the metadata needed by Explorer's Properties UI.
// File changes belong to FileOps/KIO; this type never writes to the filesystem.
struct PropertiesData {
    enum class Kind {
        File,
        Folder,
        Drive,
        Multiple,
    };

    Kind kind = Kind::File;
    QString displayName; // UI caption; may be branded or the UI-only root label
    QString realName;    // actual item basename to use as a rename input
    QString typeName;
    QString location;
    QString mimeType;
    QDateTime created;
    QDateTime modified;
    QDateTime accessed;
    quint64 totalSize = 0;
    quint64 availableSize = 0;
    quint64 usedSize = 0;
    quint64 size = 0;        // direct file size; directory totals are asynchronous
    quint64 sizeOnDisk = 0;  // allocated bytes from st_blocks when available
    bool allocatedSizeKnown = false;
    int count = 0;
    QString owner;
    QString group;
    uint permissions = 0;
    bool canRename = false;
    bool canChangePermissions = false;
    bool hidden = false;
    bool readOnly = false;
    QString fileSystem;
    QString mountPoint;
    QString device;
    QUrl singleUrl;

    // For a multi-selection, size sums regular files only. Folder sizes are
    // intentionally left for the UI's asynchronous KIO::directorySize job.
    static PropertiesData fromItems(const KFileItemList &items);
};
