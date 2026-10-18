#pragma once

#include <QDir>
#include <QByteArray>
#include <QFile>
#include <QString>
#include <QStandardPaths>
#include <QUrl>
#include <QUrlQuery>

// The synthetic Computer location and the search results location
//
// KIO has no protocol that lists a machine's drives, so the Computer location
// is a marker rather than something it can resolve, and navigating to it swaps
// the file list over to the drive model instead, which means every path
// forwarding a location to a job has to check for it first
namespace Locations {

namespace detail {

inline QByteArray decodeMountInfoField(const QByteArray &field)
{
    QByteArray decoded;
    decoded.reserve(field.size());
    for (qsizetype i = 0; i < field.size(); ++i) {
        if (i + 3 < field.size() && field.at(i) == '\\') {
            const QByteArray escape = field.mid(i, 4);
            char value = '\0';
            if (escape == QByteArrayLiteral("\\040")) value = ' ';
            else if (escape == QByteArrayLiteral("\\011")) value = '\t';
            else if (escape == QByteArrayLiteral("\\012")) value = '\n';
            else if (escape == QByteArrayLiteral("\\134")) value = '\\';
            if (value != '\0') {
                decoded.append(value);
                i += 3;
                continue;
            }
            decoded.append(field.at(i));
        } else {
            decoded.append(field.at(i));
        }
    }
    return decoded;
}

} // namespace detail

// User-owned rclone mountpoint. It remains an ordinary local path so KIO's
// local worker can browse the FUSE filesystem after rclone has mounted it.
inline QUrl googleDriveMount()
{
    const QString dataRoot = QStandardPaths::writableLocation(
        QStandardPaths::GenericDataLocation);
    if (dataRoot.isEmpty())
        return {};
    return QUrl::fromLocalFile(QDir::cleanPath(
        QDir(dataRoot).filePath(QStringLiteral("linux-explorer-win7/mounts/google-drive"))));
}

// Lexical containment only: never resolve symlinks or touch the mount path.
// The separator check keeps similarly prefixed siblings outside the mount.
inline bool isGoogleDriveMount(const QUrl &url)
{
    if (!url.isLocalFile())
        return false;
    const QUrl mount = googleDriveMount();
    if (!mount.isLocalFile())
        return false;
    const QString root = QDir::cleanPath(mount.toLocalFile());
    const QString path = QDir::cleanPath(url.toLocalFile());
    if (path == root)
        return true;
    const QString prefix = root.endsWith(QLatin1Char('/'))
        ? root : root + QLatin1Char('/');
    return path.startsWith(prefix);
}

// What one reading of mountinfo says about the Drive mount: whether exactly one
// valid read-only rclone mount sits at the target, and its kernel mount ID. A
// kernel mount ID distinguishes a replacement mount at the same path, which
// prevents a late quota response from being applied to a newly connected Drive.
struct GoogleDriveMountState {
    bool mounted = false;
    QString mountId;
};

// Parse mountinfo records without probing mount paths, in one pass. Mountinfo
// encodes spaces, tabs, newlines, and backslashes as octal escapes.
inline GoogleDriveMountState googleDriveMountStateIn(const QByteArray &mountInfo)
{
    const QUrl mount = googleDriveMount();
    if (!mount.isLocalFile())
        return {};
    const QString root = QDir::cleanPath(mount.toLocalFile());
    int recordsAtMount = 0;
    bool validRcloneMount = false;
    // From the first record at the target, which is the one that is judged
    QByteArray firstId;
    const QList<QByteArray> records = mountInfo.split('\n');
    for (QByteArray record : records) {
        if (record.endsWith('\r'))
            record.chop(1);
        QList<QByteArray> fields = record.split(' ');
        fields.removeAll(QByteArray());
        if (fields.size() <= 5)
            continue;
        const QString mountPoint = QDir::cleanPath(QString::fromLocal8Bit(
            detail::decodeMountInfoField(fields.at(4))));
        if (mountPoint != root)
            continue;
        if (++recordsAtMount == 1)
            firstId = fields.first();

        const qsizetype separator = fields.indexOf(QByteArrayLiteral("-"));
        if (separator < 6 || fields.size() <= separator + 2
            || fields.at(separator + 1) != QByteArrayLiteral("fuse.rclone")
            || fields.at(separator + 2) != QByteArrayLiteral("explorer-google-drive:"))
            continue;
        const QList<QByteArray> options = fields.at(5).split(',');
        validRcloneMount = options.contains(QByteArrayLiteral("ro"))
            && !options.contains(QByteArrayLiteral("rw"));
    }
    GoogleDriveMountState state;
    state.mounted = recordsAtMount == 1 && validRcloneMount;
    if (!state.mounted)
        return state;

    // Only a plain positive decimal is an ID
    if (firstId.isEmpty() || firstId.size() > 20 || firstId.startsWith('0'))
        return state;
    for (char ch : firstId)
        if (ch < '0' || ch > '9') return state;
    bool valid = false;
    const auto value = firstId.toULongLong(&valid);
    if (valid && value)
        state.mountId = QString::fromLatin1(firstId);
    return state;
}

inline bool googleDriveMountedIn(const QByteArray &mountInfo)
{
    return googleDriveMountStateIn(mountInfo).mounted;
}

inline QString googleDriveMountIdIn(const QByteArray &mountInfo)
{
    return googleDriveMountStateIn(mountInfo).mountId;
}

// The bounded procfs read is metadata-only: unlike statfs/QStorageInfo it
// cannot wait on a stalled FUSE mount. One read answers both questions, so a
// caller wanting the state and the ID should ask once, not twice.
inline GoogleDriveMountState googleDriveMountState()
{
    constexpr qint64 kMaxMountInfoBytes = 1024 * 1024;
    QFile mountInfo(QStringLiteral("/proc/self/mountinfo"));
    if (!mountInfo.open(QIODevice::ReadOnly))
        return {};
    const QByteArray data = mountInfo.read(kMaxMountInfoBytes);
    // Reaching the cap leaves the record set potentially incomplete, so it
    // cannot establish that there is exactly one mount at our target.
    if (data.size() == kMaxMountInfoBytes)
        return {};
    return googleDriveMountStateIn(data);
}

inline bool googleDriveMounted()
{
    return googleDriveMountState().mounted;
}

inline QString googleDriveMountId()
{
    return googleDriveMountState().mountId;
}

inline QUrl computer()
{
    return QUrl(QStringLiteral("computer:///"));
}

inline bool isComputer(const QUrl &url)
{
    return url.scheme() == QLatin1String("computer");
}

inline QUrl libraries()
{
    return QUrl(QStringLiteral("libraries:///"));
}

inline bool isLibraries(const QUrl &url)
{
    return url.scheme() == QLatin1String("libraries");
}

// A recursive search of a folder, and unlike Computer this is a real KIO
// location, a worker walking the tree and reporting hits as an ordinary listing
//
// Searching contents is opt in, since without an indexer it reads every file it
// walks past
inline QUrl search(const QUrl &folder, const QString &term, bool contents = false)
{
    QUrl url;
    url.setScheme(QStringLiteral("filenamesearch"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("search"), term);
    query.addQueryItem(QStringLiteral("url"), folder.url());
    if (contents)
        query.addQueryItem(QStringLiteral("checkContent"), QStringLiteral("yes"));
    // Echoed back as the listing's display name
    query.addQueryItem(QStringLiteral("title"), term);
    url.setQuery(query);
    return url;
}

// So the search again strip can show which scope is in force
inline bool isContentSearch(const QUrl &url)
{
    return QUrlQuery(url).queryItemValue(QStringLiteral("checkContent"))
           == QLatin1String("yes");
}

inline bool isSearch(const QUrl &url)
{
    return url.scheme() == QLatin1String("filenamesearch");
}

inline QUrl searchFolder(const QUrl &url)
{
    if (!isSearch(url))
        return {};
    return QUrl(QUrlQuery(url).queryItemValue(QStringLiteral("url"),
                                              QUrl::FullyDecoded));
}

inline QString searchTerm(const QUrl &url)
{
    if (!isSearch(url))
        return {};
    return QUrlQuery(url).queryItemValue(QStringLiteral("search"),
                                         QUrl::FullyDecoded);
}

} // namespace Locations
