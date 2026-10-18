#include "FileManagerService.h"
#include "FileOps.h"
#include "MainWindow.h"

#include <KIO/Global>

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusReply>
#include <QDir>
#include <QFile>
#include <QDBusMessage>
#include <QCoreApplication>
#include <QFileInfo>
#include <QList>
#include <QMap>
#include <QUrl>

namespace {

// Explorer's own name, and whoever holds it is the primary instance
const QLatin1String kAppService("io.github.LinuxExplorer.Win7");

// The shared name and its specified object path, the object being exported
// once so both names route to it
const QLatin1String kSharedService("org.freedesktop.FileManager1");
const QLatin1String kObjectPath("/org/freedesktop/FileManager1");
const QLatin1String kInterface("org.freedesktop.FileManager1");

QString g_trustedExecutable;

// A path as comparable text: symlinks resolved, and the suffix the kernel adds
// when the file was replaced under a running process dropped, so an upgrade
// does not lock the old instance out of its own launcher
QString normalizedExecutable(QString path)
{
    static const QLatin1String deleted(" (deleted)");
    if (path.endsWith(deleted))
        path.chop(deleted.size());
    const QString canonical = QFileInfo(path).canonicalFilePath();
    return canonical.isEmpty() ? QDir::cleanPath(path) : canonical;
}

// Whether a bus peer is another Explorer process, which is how the launcher
// reaches the running instance. Anything it cannot establish is not trusted.
bool peerIsTrusted(const QDBusConnection &bus, const QString &service)
{
    if (!bus.interface() || service.isEmpty())
        return false;
    const QDBusReply<uint> pid = bus.interface()->servicePid(service);
    if (!pid.isValid() || pid.value() == 0)
        return false;
    const QString peerExe = QFile::symLinkTarget(
        QStringLiteral("/proc/%1/exe").arg(pid.value()));
    if (peerExe.isEmpty())
        return false;
    const QString ownExe = g_trustedExecutable.isEmpty()
        ? QCoreApplication::applicationFilePath() : g_trustedExecutable;
    return normalizedExecutable(peerExe) == normalizedExecutable(ownExe);
}

// A startup id is a token handed to the window system, so it is printable
// ASCII of a sane length or it is ignored
QString sanitizedStartupId(const QString &id)
{
    if (id.size() > 256)
        return {};
    for (const QChar c : id) {
        if (c.unicode() < 0x21 || c.unicode() > 0x7e)
            return {};
    }
    return id;
}

// Anything unparseable is dropped rather than guessed at, and the list is
// capped. A peer that is not another Explorer may only name local files and
// the trash, not the remote or privileged schemes (smb, sftp, fish, admin...)
// that opening a window would go and connect to.
QList<QUrl> toUrls(const QStringList &uriList, bool trusted)
{
    QList<QUrl> urls;
    const int count = qMin(int(uriList.size()), FileManagerService::maxUris);
    urls.reserve(count);
    for (int i = 0; i < count; ++i) {
        const QString &uri = uriList.at(i);
        if (uri.size() > 4096)
            continue;
        const QUrl url(uri, QUrl::StrictMode);
        if (!url.isValid() || url.scheme().isEmpty())
            continue;
        if (!trusted) {
            const QString scheme = url.scheme();
            const bool localFile = scheme == QLatin1String("file")
                && (url.host().isEmpty() || url.host() == QLatin1String("localhost"));
            if (!localFile && scheme != QLatin1String("trash"))
                continue;
        }
        urls.append(url);
    }
    return urls;
}

} // namespace

FileManagerService::FileManagerService(QObject *parent)
    : QObject(parent)
{
}

void FileManagerService::setTrustedExecutable(const QString &path)
{
    g_trustedExecutable = path;
}

bool FileManagerService::callerIsTrusted() const
{
    return !calledFromDBus() || peerIsTrusted(connection(), message().service());
}

QString FileManagerService::startupId()
{
    // The Wayland activation token first, then the X11 startup notification id
    const QByteArray wayland = qgetenv("XDG_ACTIVATION_TOKEN");
    if (!wayland.isEmpty())
        return QString::fromLocal8Bit(wayland);
    return QString::fromLocal8Bit(qgetenv("DESKTOP_STARTUP_ID"));
}

bool FileManagerService::claim(bool registerSharedService)
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected())
        return true;   // no session bus, so run standalone rather than refuse to start

    // The private name decides primacy, so it is claimed before anything is
    // exported, and there is no queuing since a launch that waits is a hang
    QDBusConnectionInterface *busInterface = bus.interface();
    if (!busInterface)
        return true;
    const auto reply = busInterface->registerService(
        kAppService, QDBusConnectionInterface::DontQueueService,
        QDBusConnectionInterface::DontAllowReplacement);
    const bool gotAppName = reply.isValid()
        && reply.value() == QDBusConnectionInterface::ServiceRegistered;
    if (!gotAppName)
        return false;

    // Scriptable slots only, so nothing but the three specified methods is
    // reachable. ExportAllSlots would put QObject's deleteLater() on the bus
    // as well.
    bus.registerObject(kObjectPath, this, QDBusConnection::ExportScriptableSlots);

    if (!registerSharedService)
        return true;

    // Replacement allowed, so another file manager can take it over without a
    // restart, and failure here is ordinary
    const auto shared = busInterface->registerService(
        kSharedService, QDBusConnectionInterface::DontQueueService,
        QDBusConnectionInterface::AllowReplacement);
    m_ownsSharedName = shared.isValid()
        && shared.value() == QDBusConnectionInterface::ServiceRegistered;

    return true;
}

bool FileManagerService::forward(const QStringList &uris, bool reveal)
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected())
        return false;

    const QString method = reveal ? QStringLiteral("ShowItems")
                                  : QStringLiteral("ShowFolders");
    QDBusMessage call = QDBusMessage::createMethodCall(kAppService, kObjectPath,
                                                       kInterface, method);
    call.setArguments({uris, startupId()});

    // Blocking, since this process is about to exit and returning early would
    // race the window against our own teardown
    const QDBusMessage reply = bus.call(call);
    return reply.type() != QDBusMessage::ErrorMessage;
}

void FileManagerService::ShowFolders(const QStringList &uriList,
                                     const QString &startupId)
{
    const QString token = sanitizedStartupId(startupId);
    const QList<QUrl> urls = toUrls(uriList, callerIsTrusted());
    if (urls.isEmpty()) {
        // An empty request opens the default window, but one whose every
        // location was refused opens nothing
        if (uriList.isEmpty())
            MainWindow::openWindow(QUrl(), {}, token);
        return;
    }
    for (const QUrl &url : urls)
        MainWindow::openWindow(url, {}, token);
}

void FileManagerService::ShowItems(const QStringList &uriList,
                                   const QString &startupId)
{
    // Each file is revealed in its parent and selected, and several in one
    // folder become one window with a multiple selection
    QMap<QUrl, QList<QUrl>> byParent;
    const QString token = sanitizedStartupId(startupId);
    const QList<QUrl> urls = toUrls(uriList, callerIsTrusted());
    for (const QUrl &url : urls)
        byParent[KIO::upUrl(url)].append(url);

    for (auto it = byParent.cbegin(); it != byParent.cend(); ++it)
        MainWindow::openWindow(it.key(), it.value(), token);
}

void FileManagerService::ShowItemProperties(const QStringList &uriList,
                                            const QString &startupId)
{
    Q_UNUSED(startupId)
    const QList<QUrl> urls = toUrls(uriList, callerIsTrusted());
    if (urls.isEmpty())
        return;

    KFileItemList items;
    for (const QUrl &url : urls)
        items.append(KFileItem(url));
    // No parent, this process possibly having no window open
    FileOps::showProperties(items, nullptr);
}
