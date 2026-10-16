#include "ComputerModel.h"
#include "DriveLabel.h"
#include "GoogleDriveConnection.h"
#include "Locations.h"
#include "aero/icons.h"

#include <KFilePlacesModel>
#include <KIO/FileSystemFreeSpaceJob>

#include <algorithm>

ComputerModel::ComputerModel(KFilePlacesModel *places, QObject *parent)
    : ComputerModel(places, GoogleDriveConnection::sharedConnection(), parent)
{
}

ComputerModel::ComputerModel(KFilePlacesModel *places, GoogleDriveConnection *driveConnection,
                             QObject *parent)
    : QAbstractTableModel(parent)
    , m_places(places)
    , m_driveConnection(driveConnection)
{
    // The places model populates asynchronously and on every plug or unplug,
    // so the drive list is rebuilt rather than patched
    connect(m_places, &QAbstractItemModel::modelReset, this, &ComputerModel::rebuild);
    connect(m_places, &QAbstractItemModel::rowsInserted, this, &ComputerModel::rebuild);
    connect(m_places, &QAbstractItemModel::rowsRemoved, this, &ComputerModel::rebuild);
    // Mounting inserts no row, connecting only filling in the location, which
    // arrives as a data change on the row the drive already had
    connect(m_places, &QAbstractItemModel::dataChanged, this, &ComputerModel::rebuild);
    if (m_driveConnection) {
        connect(m_driveConnection, &GoogleDriveConnection::mountStateChanged,
                this, [this] {
            // A true notification can also identify a replacement kernel
            // mount observed between polls without an intervening false.
            m_googleQuotaRequested = false;
            rebuild();
        });
        connect(m_driveConnection, &GoogleDriveConnection::storageQuotaChanged,
                this, &ComputerModel::updateGoogleDriveQuota);
    }

    rebuild();
}

void ComputerModel::rebuild()
{
    QList<Device> devices;
    int unmounted = 0;
    const bool googleConnected = m_driveConnection && m_driveConnection->isConnected();
    if (!googleConnected)
        m_googleQuotaRequested = false;

    for (int row = 0; row < m_places->rowCount(); ++row) {
        const QModelIndex index = m_places->index(row, 0);
        if (m_places->isHidden(index))
            continue;

        // The managed cloud mount is represented once below, independently of
        // bookmarks or Solid's classification of the FUSE filesystem.
        if (Locations::isGoogleDriveMount(m_places->url(index)))
            continue;

        const KFilePlacesModel::GroupType group = m_places->groupType(index);
        if (group != KFilePlacesModel::DevicesType
            && group != KFilePlacesModel::RemovableDevicesType) {
            continue;
        }

        // An unmounted drive has nothing to open, so it is counted instead and
        // the view offers to connect it
        if (m_places->setupNeeded(index)) {
            ++unmounted;
            continue;
        }

        Device device;
        device.name = DriveLabel::forPlace(m_places, index);
        device.icon = m_places->icon(index);
        device.url = m_places->url(index);
        device.placeRow = row;
        device.removable = (group == KFilePlacesModel::RemovableDevicesType);
        if (device.removable)
            device.type = tr("Removable Disk");
        else
            device.type = tr("Local Disk");
        device.fsType = DriveLabel::fileSystemType(m_places, index);

        // Any data change rebuilds the whole list, so starting each size from
        // scratch would drop the page back to an unknown figure every time
        for (const Device &existing : std::as_const(m_devices)) {
            if (existing.sizeKnown && existing.url == device.url) {
                device.total = existing.total;
                device.available = existing.available;
                device.sizeKnown = true;
                break;
            }
        }

        devices.append(device);
    }

    if (googleConnected) {
        Device drive;
        drive.name = tr("Google Drive (G:)");
        drive.type = tr("Google Drive");
        drive.fsType = tr("Google Drive");
        drive.url = Locations::googleDriveMount();
        drive.icon = Aero::googleDriveIcon();
        readGoogleDriveQuota(drive);
        devices.append(drive);
    }

    applySort(devices);

    // Nothing the page draws has moved, which is the common case, and
    // resetting anyway would lose the selection
    if (devices != m_devices || unmounted != m_unmountedCount) {
        beginResetModel();
        m_devices = devices;
        m_unmountedCount = unmounted;
        endResetModel();

        // Only the drives with no figure yet, the rest being carried over.
        for (const Device &device : std::as_const(m_devices)) {
            if (!device.sizeKnown && !Locations::isGoogleDriveMount(device.url))
                queryFreeSpace(device.url);
        }
    }
    // Request once per observed mount lifecycle. A failed/unknown quota must
    // not become a request loop when Places emits unrelated data changes.
    if (googleConnected && !m_googleQuotaRequested) {
        m_googleQuotaRequested = true;
        m_driveConnection->refreshStorageQuota();
    }
}

void ComputerModel::refresh()
{
    if (m_driveConnection && m_driveConnection->isConnected()) {
        m_googleQuotaRequested = true;
        m_driveConnection->refreshStorageQuota(true);
    }
    rebuild();

    // A rebuild keeps the figures it has, and a drive filling up changes no row
    for (const Device &device : std::as_const(m_devices)) {
        if (!Locations::isGoogleDriveMount(device.url))
            queryFreeSpace(device.url);
    }
}

void ComputerModel::readGoogleDriveQuota(Device &drive) const
{
    drive.total = 0;
    drive.available = 0;
    drive.sizeKnown = false;
    if (!m_driveConnection || !m_driveConnection->isConnected()
        || !m_driveConnection->hasStorageQuota())
        return;
    const quint64 total = m_driveConnection->storageTotalBytes();
    const quint64 available = m_driveConnection->storageAvailableBytes();
    if (total == 0 || available > total || !m_driveConnection->hasStorageQuota())
        return;
    drive.total = total;
    drive.available = available;
    drive.sizeKnown = true;
}

void ComputerModel::updateGoogleDriveQuota()
{
    for (int row = 0; row < m_devices.size(); ++row) {
        if (m_devices.at(row).url != Locations::googleDriveMount())
            continue;
        Device updated = m_devices.at(row);
        readGoogleDriveQuota(updated);
        if (updated == m_devices.at(row))
            return;
        QList<Device> devices = m_devices;
        devices[row] = updated;
        if (m_sortKey == SortBySize || m_sortKey == SortByFree)
            applySort(devices);
        bool reordered = false;
        for (int i = 0; i < devices.size(); ++i) {
            if (devices.at(i).url != m_devices.at(i).url) {
                reordered = true;
                break;
            }
        }
        if (reordered) {
            beginResetModel();
            m_devices = devices;
            endResetModel();
        } else {
            m_devices[row] = updated;
            // The tile delegate reads figures from Name, so include that cell
            // as well as the details columns without disturbing selection.
            Q_EMIT dataChanged(index(row, Name), index(row, FreeSpace),
                               {Qt::DisplayRole, TotalSizeRole, AvailableSizeRole, SizeKnownRole});
        }
        return;
    }
}

void ComputerModel::setSortKey(SortKey key, Qt::SortOrder order)
{
    if (m_sortKey == key && m_sortOrder == order)
        return;
    m_sortKey = key;
    m_sortOrder = order;
    beginResetModel();
    applySort(m_devices);
    endResetModel();
}

void ComputerModel::sort(int column, Qt::SortOrder order)
{
    switch (column) {
    case Name:
        setSortKey(SortByName, order);
        break;
    case Type:
        setSortKey(SortByType, order);
        break;
    case TotalSize:
        setSortKey(SortBySize, order);
        break;
    case FreeSpace:
        setSortKey(SortByFree, order);
        break;
    default:
        break;
    }
}

void ComputerModel::applySort(QList<Device> &devices) const
{
    const bool descending = (m_sortOrder == Qt::DescendingOrder);
    const QUrl root = QUrl::fromLocalFile(QStringLiteral("/"));

    std::stable_sort(devices.begin(), devices.end(),
                     [this, descending, root](const Device &left, const Device &right) {
        // Pin the actual system root before applying any requested ordering.
        // A renamed disk or a label that resembles C: cannot change identity.
        if ((left.url == root) != (right.url == root))
            return left.url == root;
        // Drives with no figure sink to the bottom, before the order is
        // applied so reversing does not float them to the top
        if (m_sortKey == SortBySize || m_sortKey == SortByFree) {
            if (left.sizeKnown != right.sizeKnown)
                return left.sizeKnown;
        }

        // Negative when left comes first, positive when right does
        int comparison = 0;
        switch (m_sortKey) {
        case SortBySize:
            if (left.sizeKnown && left.total != right.total)
                comparison = left.total < right.total ? -1 : 1;
            break;
        case SortByFree:
            if (left.sizeKnown && left.available != right.available)
                comparison = left.available < right.available ? -1 : 1;
            break;
        case SortByType:
            comparison = QString::compare(left.type, right.type, Qt::CaseInsensitive);
            break;
        case SortByName:
        default:
            break;
        }

        // Name breaks every tie, rather than discovery order
        if (comparison == 0)
            comparison = QString::compare(left.name, right.name, Qt::CaseInsensitive);

        if (descending)
            return comparison > 0;
        return comparison < 0;
    });
}

QModelIndex ComputerModel::placeIndexFor(const QUrl &url) const
{
    // The virtual drive has no Solid device to mount, eject, or rename, even
    // when the user also keeps a bookmark to its local mount path.
    if (Locations::isGoogleDriveMount(url))
        return {};

    for (const Device &device : m_devices) {
        if (device.url == url && device.placeRow >= 0)
            return m_places->index(device.placeRow, 0);
    }

    // Normal for an unmounted drive, those being counted rather than listed
    for (int row = 0; row < m_places->rowCount(); ++row) {
        const QModelIndex index = m_places->index(row, 0);
        if (m_places->url(index).matches(url, QUrl::StripTrailingSlash))
            return index;
    }
    return {};
}

void ComputerModel::queryFreeSpace(const QUrl &url)
{
    if (!url.isValid() || Locations::isGoogleDriveMount(url))
        return;

    KIO::FileSystemFreeSpaceJob *job = KIO::fileSystemFreeSpace(url);
    connect(job, &KJob::result, this, [this, job, url] {
        if (job->error())
            return;   // an unmounted or unreadable device simply shows no size

        // By location rather than a captured row, since a plug or unplug in
        // flight would leave the index on another drive
        for (int row = 0; row < m_devices.size(); ++row) {
            if (m_devices.at(row).url != url)
                continue;

            m_devices[row].total = job->size();
            m_devices[row].available = job->availableSize();
            m_devices[row].sizeKnown = true;

            // A figure arriving can reorder the list, which is structural, and
            // a data change would leave the old order with the new numbers
            if (m_sortKey == SortBySize || m_sortKey == SortByFree) {
                beginResetModel();
                applySort(m_devices);
                endResetModel();
            } else {
                Q_EMIT dataChanged(index(row, TotalSize), index(row, FreeSpace));
            }
            return;
        }
    });
}

int ComputerModel::rowCount(const QModelIndex &parent) const
{
    // A flat table, so nothing has children
    if (parent.isValid())
        return 0;
    return m_devices.size();
}

int ComputerModel::columnCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return ColumnCount;
}

QVariant ComputerModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_devices.size())
        return {};

    const Device &device = m_devices.at(index.row());

    switch (role) {
    case Qt::DecorationRole:
        if (index.column() != Name)
            return {};
        return device.icon;
    case UrlRole:
        return device.url;
    case TotalSizeRole:
        return QVariant::fromValue(device.total);
    case AvailableSizeRole:
        return QVariant::fromValue(device.available);
    case SizeKnownRole:
        return device.sizeKnown;
    case RemovableRole:
        return device.removable;
    case FileSystemTypeRole:
        return device.fsType;
    default:
        break;
    }

    if (role != Qt::DisplayRole)
        return {};

    switch (index.column()) {
    case Name:
        return device.name;
    case Type:
        return device.type;
    case TotalSize:
        if (!device.sizeKnown)
            return QString();
        return KIO::convertSize(device.total);
    case FreeSpace:
        if (!device.sizeKnown)
            return QString();
        return KIO::convertSize(device.available);
    default:
        return {};
    }
}

QVariant ComputerModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return QAbstractTableModel::headerData(section, orientation, role);

    switch (section) {
    case Name:
        return tr("Name");
    case Type:
        return tr("Type");
    case TotalSize:
        return tr("Total Size");
    case FreeSpace:
        return tr("Free Space");
    default:
        return {};
    }
}

int ComputerModel::unmountedCount() const
{
    return m_unmountedCount;
}

QUrl ComputerModel::urlForIndex(const QModelIndex &index) const
{
    if (!index.isValid() || index.row() >= m_devices.size())
        return {};
    return m_devices.at(index.row()).url;
}
