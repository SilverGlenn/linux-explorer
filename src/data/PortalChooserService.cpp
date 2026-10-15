#include "PortalChooserService.h"
#include "PortalChooserOptions.h"
#include "PortalDBusTypes.h"
#include "FileChooserDialog.h"

#include <QDBusConnectionInterface>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QRegularExpression>
#include <QTimer>
#include <utility>

namespace {
const QString backendName = QStringLiteral("org.freedesktop.impl.portal.desktop.explorerwin7");
const QString backendPath = QStringLiteral("/org/freedesktop/portal/desktop");
const QString frontendName = QStringLiteral("org.freedesktop.portal.Desktop");
constexpr int maxRequests = 64;
} // namespace

class PortalChooserRequest : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.impl.portal.Request")
public:
    PortalChooserRequest(PortalChooserService *service, QDBusMessage call, QString path, PortalChooserOptions config)
        : QObject(service), m_service(service), m_call(std::move(call)), m_path(std::move(path)), m_config(std::move(config)) {}
    ~PortalChooserRequest() override { dismiss(); }
    FileChooserDialog *dialog() const { return m_dialog; }

    void start()
    {
        if (m_terminal)
            return;
        m_dialog = new FileChooserDialog(m_config.chooser);
        m_dialog->setAttribute(Qt::WA_DeleteOnClose, false);
        connect(m_dialog, &QDialog::finished, this, [this](int status) {
            if (m_terminal)
                return;
            if (status != QDialog::Accepted) {
                finish(1);
                return;
            }
            QVariantMap results;
            if (!selection(&results)) {
                finish(2);
                return;
            }
            finish(0, results);
        });
        connect(m_dialog, &QObject::destroyed, this, [this] {
            m_dialog = nullptr;
            if (!m_terminal)
                finish(2);
        });
        Explorer::showChooserWindow(m_dialog, m_config.window);
    }

    void finish(uint response, const QVariantMap &results = {})
    {
        if (m_terminal)
            return;
        m_terminal = true;
        m_service->reply(m_call, response, results);
        m_service->removeRequest(m_path, this);
    }

    void dismiss()
    {
        if (!m_dialog)
            return;
        QPointer<FileChooserDialog> dialog = m_dialog;
        m_dialog = nullptr;
        // Reject pending overwrite confirmation before disposing the real UI.
        QList<QPointer<QDialog>> children;
        for (QDialog *child : dialog->findChildren<QDialog *>())
            children.prepend(child);
        for (const auto &child : children) {
            if (child)
                child->reject();
        }
        if (dialog) {
            dialog->reject();
            dialog->deleteLater();
        }
    }

public Q_SLOTS:
    void Close()
    {
        if (!calledFromDBus())
            return;
        const QDBusMessage call = message();
        setDelayedReply(true);
        QPointer<PortalChooserRequest> guard(this);
        PortalChooserService *service = m_service;
        m_service->authorize(call, [guard, service, call] {
            service->m_connection.send(call.createReply());
            if (guard)
                guard->finish(1);
        });
    }

private:
    bool selection(QVariantMap *results)
    {
        const auto &options = m_config.chooser;
        const QList<QUrl> urls = m_dialog->selectedUrls();
        if (urls.isEmpty() || urls.size() > 4096
            || (options.mode == ChooserMode::SaveFiles && urls.size() != options.saveNames.size())
            || (options.mode != ChooserMode::SaveFiles && !options.multiple && urls.size() != 1))
            return false;
        QStringList uris;
        for (const QUrl &url : urls) {
            const QString path = url.toLocalFile();
            if (!url.isValid() || url.scheme() != QLatin1String("file") || !url.host().isEmpty()
                || !url.userInfo().isEmpty() || url.port() != -1 || url.hasQuery() || url.hasFragment()
                || path.contains(QChar::Null) || !QDir::isAbsolutePath(path)
                || QFile::decodeName(QFile::encodeName(path)) != path)
                return false;
            const QFileInfo info(path);
            if (options.mode == ChooserMode::OpenFile
                ? (options.directory ? !info.isDir() : !info.isFile()) : info.isDir())
                return false;
            uris.append(url.toString(QUrl::FullyEncoded));
        }
        results->insert(QStringLiteral("uris"), uris);
        if (!m_config.filters.isEmpty()) {
            bool ok = false;
            const int selected = m_dialog->selectedFilter().toInt(&ok);
            if (!ok || selected < 0 || selected >= m_config.filters.size())
                return false;
            results->insert(QStringLiteral("current_filter"), QVariant::fromValue(m_config.filters.at(selected)));
        }
        if (!options.choices.isEmpty()) {
            const auto selected = m_dialog->selectedChoices();
            PortalChoiceResults choices;
            for (const ChooserChoice &choice : options.choices) {
                const QString value = selected.value(choice.id);
                bool valid = choice.options.isEmpty()
                    && (value == QLatin1String("true") || value == QLatin1String("false"));
                for (const ChooserChoiceOption &option : choice.options)
                    valid = valid || value == option.id;
                if (!valid)
                    return false;
                choices.append({choice.id, value});
            }
            results->insert(QStringLiteral("choices"), QVariant::fromValue(choices));
        }
        return true;
    }

    PortalChooserService *m_service;
    QDBusMessage m_call;
    QString m_path;
    PortalChooserOptions m_config;
    QPointer<FileChooserDialog> m_dialog;
    bool m_terminal = false;
};

PortalChooserService::PortalChooserService(const QDBusConnection &connection, QObject *parent)
    : QObject(parent), m_connection(connection), m_watcher(new QDBusServiceWatcher(frontendName, connection,
          QDBusServiceWatcher::WatchForOwnerChange, this))
{
    registerPortalDBusTypes();
    connect(m_watcher, &QDBusServiceWatcher::serviceOwnerChanged, this,
            [this](const QString &, const QString &, const QString &) {
        ++m_frontendGeneration;
        cancelRequests();
    });
    m_connection.connect(QString(), QStringLiteral("/org/freedesktop/DBus/Local"),
        QStringLiteral("org.freedesktop.DBus.Local"), QStringLiteral("Disconnected"), this, SLOT(busDisconnected()));
}

PortalChooserService::~PortalChooserService()
{
    cancelRequests();
    if (m_claimed) {
        m_connection.unregisterObject(backendPath);
        m_connection.unregisterService(backendName);
    }
}

bool PortalChooserService::claim()
{
    if (m_claimed)
        return true;
    if (!m_connection.isConnected() || !m_connection.interface())
        return false;
    const auto reply = m_connection.interface()->registerService(backendName,
        QDBusConnectionInterface::DontQueueService, QDBusConnectionInterface::DontAllowReplacement);
    if (!reply.isValid() || reply.value() != QDBusConnectionInterface::ServiceRegistered)
        return false;
    if (!m_connection.registerObject(backendPath, this,
        QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllProperties)) {
        m_connection.unregisterService(backendName);
        return false;
    }
    m_claimed = true;
    return true;
}

FileChooserDialog *PortalChooserService::dialogForRequest(const QString &path) const
{
    const auto request = m_requests.value(path);
    return request ? request->dialog() : nullptr;
}

void PortalChooserService::authorize(const QDBusMessage &call, std::function<void()> accepted)
{
    if (m_authorizations >= maxRequests || !call.service().startsWith(QLatin1Char(':'))) {
        m_connection.send(call.createErrorReply(QDBusError::AccessDenied, QStringLiteral("Portal frontend required.")));
        return;
    }
    ++m_authorizations;
    const quint64 generation = m_frontendGeneration;
    auto query = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"),
        QStringLiteral("org.freedesktop.DBus"), QStringLiteral("GetNameOwner"));
    query.setArguments({frontendName});
    auto *watcher = new QDBusPendingCallWatcher(m_connection.asyncCall(query, 5000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, call, generation, accepted = std::move(accepted)] {
        --m_authorizations;
        const QDBusPendingReply<QString> owner = *watcher;
        watcher->deleteLater();
        if (owner.isError() || owner.value() != call.service() || generation != m_frontendGeneration) {
            m_connection.send(call.createErrorReply(QDBusError::AccessDenied, QStringLiteral("Only the current portal frontend may use this backend.")));
            return;
        }
        accepted();
    });
}

uint PortalChooserService::OpenFile(const QDBusObjectPath &handle, const QString &appId, const QString &parentWindow,
                                  const QString &title, const QVariantMap &options, QVariantMap &results)
{
    Q_UNUSED(results)
    return createRequest(ChooserMode::OpenFile, handle, appId, parentWindow, title, options);
}
uint PortalChooserService::SaveFile(const QDBusObjectPath &handle, const QString &appId, const QString &parentWindow,
                                  const QString &title, const QVariantMap &options, QVariantMap &results)
{
    Q_UNUSED(results)
    return createRequest(ChooserMode::SaveFile, handle, appId, parentWindow, title, options);
}
uint PortalChooserService::SaveFiles(const QDBusObjectPath &handle, const QString &appId, const QString &parentWindow,
                                   const QString &title, const QVariantMap &options, QVariantMap &results)
{
    Q_UNUSED(results)
    return createRequest(ChooserMode::SaveFiles, handle, appId, parentWindow, title, options);
}

uint PortalChooserService::createRequest(ChooserMode mode, const QDBusObjectPath &handle, const QString &appId,
                                       const QString &parentWindow, const QString &title, const QVariantMap &options)
{
    if (!calledFromDBus())
        return 2;
    const QDBusMessage call = message();
    setDelayedReply(true);
    authorize(call, [this, mode, handle, appId, parentWindow, title, options, call] {
        const QString path = handle.path();
        static const QRegularExpression requestPath(QStringLiteral("^(?:/[A-Za-z0-9_]+)+$"));
        if (path.size() > 1024 || !requestPath.match(path).hasMatch() || path == backendPath || m_requests.contains(path)
            || m_requests.size() >= maxRequests || appId.size() > 4096 || appId.contains(QChar::Null)) {
            reply(call, 2);
            return;
        }
        const auto config = portalChooserOptions(mode, parentWindow, title, options);
        if (!config.error.isEmpty()) {
            reply(call, 2);
            return;
        }
        auto *request = new PortalChooserRequest(this, call, path, config);
        if (!m_connection.registerObject(path, request, QDBusConnection::ExportAllSlots)) {
            delete request;
            reply(call, 2);
            return;
        }
        m_requests.insert(path, request);
        QTimer::singleShot(0, request, [request] { request->start(); });
    });
    return 0;
}

void PortalChooserService::reply(const QDBusMessage &call, uint response, const QVariantMap &results)
{
    // A method reply is destination-addressed. Never broadcast file results.
    m_connection.send(call.createReply({QVariant::fromValue(response), results}));
}

void PortalChooserService::removeRequest(const QString &path, PortalChooserRequest *request)
{
    // A delayed Close/authentication callback can outlive an old request. Never
    // unregister a newer request which has reused the same frontend handle.
    if (m_requests.value(path) != request)
        return;
    m_requests.remove(path);
    m_connection.unregisterObject(path);
    request->dismiss();
    request->deleteLater();
}

void PortalChooserService::cancelRequests()
{
    const auto requests = m_requests;
    for (auto *request : requests)
        request->finish(1);
}

void PortalChooserService::busDisconnected()
{
    ++m_frontendGeneration;
    cancelRequests();
}

#include "PortalChooserService.moc"
