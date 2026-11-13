#pragma once

#include <KUrlMimeData>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMimeData>
#include <QRandomGenerator>
#include <QStandardPaths>

#include <sys/stat.h>

namespace PortalExport {

// Sandboxed receivers (Office in the Bottles Flatpak, other Flatpaks) cannot
// open the plain file:// paths in a drag or on the clipboard. The document
// portal hands them per-file access instead: the files are registered with an
// org.freedesktop.portal.FileTransfer and its key travels as
// application/vnd.portal.filetransfer next to the ordinary URI list. The
// transfer ends when the mime data is destroyed, so it lives exactly as long
// as the drag or the clipboard contents.
//
// Registering opens every file, so very large selections, which in practice
// are moves inside the file manager, skip the portal and keep starting fast.
inline constexpr qsizetype maximumFiles = 256;

// xdg-desktop-portal up to at least 1.22.1 keeps the parent folder's device
// and inode in an int and refuses the file when they do not survive that
// (fixed upstream in c007f57e, "Don't truncate parent inode numbers"). rclone
// hands out 64-bit inode numbers, so every file on the Google Drive mount is
// refused. Such files are offered as a local copy instead, the mount being
// read-only anyway; the copy is made on the spot, so it is capped.
inline constexpr qint64 maximumStagedBytes = 32ll * 1024 * 1024;
inline constexpr qint64 stagingLifetimeSecs = 24 * 60 * 60;

inline bool portalAcceptsParent(const QString &path)
{
    struct stat st;
    if (::stat(QFile::encodeName(QFileInfo(path).absolutePath()).constData(), &st) != 0)
        return true;
    return st.st_ino == ino_t(int(st.st_ino)) && st.st_dev == dev_t(int(st.st_dev));
}

inline QString newStagingDir()
{
    const QString root = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation)
        + QStringLiteral("/linux-explorer-win7/portal-staging");
    const QDateTime expired = QDateTime::currentDateTime().addSecs(-stagingLifetimeSecs);
    const QFileInfoList old = QDir(root).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo &entry : old)
        if (entry.lastModified() < expired)
            QDir(entry.absoluteFilePath()).removeRecursively();

    const QString dir = root + QLatin1Char('/') + QString::number(QDateTime::currentMSecsSinceEpoch())
        + QLatin1Char('-') + QString::number(QRandomGenerator::global()->generate(), 16);
    if (!QDir().mkpath(dir)) return {};
    QFile::setPermissions(dir, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    return dir;
}

inline void offer(QMimeData *mime)
{
    if (!mime || !mime->hasUrls()) return;
    const QList<QUrl> urls = mime->urls();
    if (urls.size() > maximumFiles) return;

    QList<QUrl> exported;
    QString stagingDir;
    qint64 stagedBytes = 0;
    for (qsizetype i = 0; i < urls.size(); ++i) {
        const QUrl &url = urls.at(i);
        if (!url.isLocalFile() || portalAcceptsParent(url.toLocalFile())) {
            exported << url;
            continue;
        }
        const QFileInfo info(url.toLocalFile());
        stagedBytes += info.size();
        if (!info.isFile() || stagedBytes > maximumStagedBytes) return;
        if (stagingDir.isEmpty() && (stagingDir = newStagingDir()).isEmpty()) return;
        const QString slot = stagingDir + QLatin1Char('/') + QString::number(i);
        const QString copy = slot + QLatin1Char('/') + info.fileName();
        if (!QDir().mkpath(slot) || !QFile::copy(info.absoluteFilePath(), copy)) return;
        exported << QUrl::fromLocalFile(copy);
    }

    if (stagingDir.isEmpty()) {
        KUrlMimeData::exportUrlsToPortal(mime);
        return;
    }

    // The transfer is tied to the carrier's lifetime, and the carrier to the
    // real mime data, so it still ends with the drag or the clipboard contents
    auto *carrier = new QMimeData;
    carrier->setParent(mime);
    carrier->setUrls(exported);
    const QString key = QStringLiteral("application/vnd.portal.filetransfer");
    if (KUrlMimeData::exportUrlsToPortal(carrier))
        mime->setData(key, carrier->data(key));
}

}
