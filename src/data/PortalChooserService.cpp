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
#include <QDBusVariant>
#include <QDBusVirtualObject>
#include <QSet>
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
const QString chooserInterface = QStringLiteral("org.freedesktop.impl.portal.FileChooser");
const QString requestInterface = QStringLiteral("org.freedesktop.impl.portal.Request");
constexpr int maxRequests = 64;
// Leave room for one in-flight Close per pending Open, even at capacity.
constexpr int maxAuthorizations = 2 * maxRequests;
} // namespace

class PortalChooserRequest : public QObject {
public:
    PortalChooserRequest(PortalChooserService *service, QDBusMessage call, QString path)
        : QObject(service), m_service(service), m_call(std::move(call)), m_path(std::move(path)) {}
    ~PortalChooserRequest() override { dismiss(); }
    FileChooserDialog *dialog() const { return m_dialog; }
    QString owner() const { return m_call.service(); }

    void authorized(PortalChooserOptions config)
    {
        if (m_terminal)
            return;
        m_config = std::move(config);
        m_authorized = true;
        scheduleStart();
    }

    void rejectAuthorization()
    {
        if (m_terminal)
            return;
        m_terminal = true;
        m_service->deny(m_call);
        m_service->removeRequest(m_path, this);
    }

    void cancel()
    {
        if (m_authorized)
            finish(1);
        else
            rejectAuthorization();
    }

    void scheduleStart()
    {
        QTimer::singleShot(0, this, [this] { start(); });
    }

    void start()
    {
        if (m_terminal || !m_authorized || m_pendingCloses || m_dialog)
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

    void close(const QDBusMessage &call)
    {
        call.setDelayedReply(true);
        // Even a new frontend owner cannot close an old owner's request.
        if (call.service() != owner()) {
            m_service->deny(call);
            return;
        }
        ++m_pendingCloses;
        QPointer<PortalChooserRequest> guard(this);
        PortalChooserService *service = m_service;
        m_service->authorize(call, [guard, service, call](bool authorized) {
            if (authorized)
                service->m_connection.send(call.createReply());
            else
                service->deny(call);
            // Every Close gets its own reply, even if another Close or an
            // owner change already removed this exact request.
            if (guard) {
                --guard->m_pendingCloses;
                if (authorized)
                    guard->finish(1);
                else
                    guard->scheduleStart();
            }
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
    int m_pendingCloses = 0;
    bool m_authorized = false;
    bool m_terminal = false;
};

// Qt resolves ordinary QObject paths on its D-Bus thread, before queued Open
// reaches the GUI thread. A single, GUI-affine virtual subtree keeps queued
// Open/Close in order; per-request QObject export alone is too late.
class PortalChooserRouter : public QDBusVirtualObject {
public:
    explicit PortalChooserRouter(PortalChooserService *service) : QDBusVirtualObject(service), m_service(service) {}

    QString introspect(const QString &path) const override
    {
        // Pure immutable XML: Qt may call this virtual from its D-Bus thread.
        // Existence and child-node enumeration happen only in handleMessage.
        if (path == backendPath) {
            QString xml = QStringLiteral("<interface name=\"org.freedesktop.impl.portal.FileChooser\">");
            for (const auto *method : {"OpenFile", "SaveFile", "SaveFiles"}) {
                xml += QStringLiteral("<method name=\"%1\"><arg type=\"o\" direction=\"in\"/>"
                    "<arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/>"
                    "<arg type=\"s\" direction=\"in\"/><arg type=\"a{sv}\" direction=\"in\"/>"
                    "<arg type=\"u\" direction=\"out\"/><arg type=\"a{sv}\" direction=\"out\"/></method>").arg(QLatin1String(method));
            }
            return xml + QStringLiteral("<property name=\"version\" type=\"u\" access=\"read\">"
                "<annotation name=\"org.freedesktop.DBus.Property.EmitsChangedSignal\" value=\"const\"/></property></interface>");
        }
        return QStringLiteral("<interface name=\"org.freedesktop.impl.portal.Request\"><method name=\"Close\"/></interface>");
    }

    bool handleMessage(const QDBusMessage &call, const QDBusConnection &connection) override
    {
        const QString path = call.path();
        const auto *request = m_service->m_requests.value(path);
        QSet<QString> children;
        const QString prefix = path == QLatin1String("/") ? path : path + QLatin1Char('/');
        auto addChild = [&](const QString &candidate) {
            if (candidate.startsWith(prefix))
                children.insert(candidate.mid(prefix.size()).section(QLatin1Char('/'), 0, 0));
        };
        addChild(backendPath);
        for (auto it = m_service->m_requests.cbegin(); it != m_service->m_requests.cend(); ++it)
            addChild(it.key());
        auto error = [&](QDBusError::ErrorType type, const QString &text) {
            connection.send(call.createErrorReply(type, text));
            return true;
        };
        if (path != backendPath && !request && children.isEmpty())
            return error(QDBusError::UnknownObject, QStringLiteral("No such portal request or object."));
        const QString interface = call.interface();
        const QString method = call.member();
        auto matches = [&](const QString &name) { return interface.isEmpty() || interface == name; };
        if (matches(QStringLiteral("org.freedesktop.DBus.Introspectable")) && method == QLatin1String("Introspect")) {
            if (!call.signature().isEmpty())
                return error(QDBusError::InvalidArgs, QStringLiteral("Introspect takes no arguments."));
            QString xml = QStringLiteral("<node>");
            if (path == backendPath || request)
                xml += introspect(path);
            xml += QStringLiteral(
                "<interface name=\"org.freedesktop.DBus.Introspectable\"><method name=\"Introspect\">"
                "<arg type=\"s\" direction=\"out\"/></method></interface>"
                "<interface name=\"org.freedesktop.DBus.Peer\"><method name=\"Ping\"/>"
                "<method name=\"GetMachineId\"><arg type=\"s\" direction=\"out\"/></method></interface>"
                "<interface name=\"org.freedesktop.DBus.Properties\"><method name=\"Get\">"
                "<arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/>"
                "<arg type=\"v\" direction=\"out\"/></method><method name=\"GetAll\">"
                "<arg type=\"s\" direction=\"in\"/><arg type=\"a{sv}\" direction=\"out\"/></method>"
                "<method name=\"Set\"><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/>"
                "<arg type=\"v\" direction=\"in\"/></method>"
                "<signal name=\"PropertiesChanged\"><arg type=\"s\"/><arg type=\"a{sv}\"/><arg type=\"as\"/></signal></interface>");
            for (const auto &child : children)
                xml += QStringLiteral("<node name=\"%1\"/>").arg(child);
            connection.send(call.createReply(QList<QVariant>{xml + QStringLiteral("</node>")}));
            return true;
        }
        if (matches(QStringLiteral("org.freedesktop.DBus.Properties"))
            && (method == QLatin1String("Get") || method == QLatin1String("GetAll") || method == QLatin1String("Set"))) {
            const QString signature = method == QLatin1String("GetAll") ? QStringLiteral("s")
                : method == QLatin1String("Get") ? QStringLiteral("ss") : QStringLiteral("ssv");
            if (call.signature() != signature)
                return error(QDBusError::InvalidArgs, QStringLiteral("Invalid property arguments."));
            const auto args = call.arguments();
            const QString objectInterface = path == backendPath ? chooserInterface : request ? requestInterface : QString();
            if (args.at(0).toString() != objectInterface || objectInterface.isEmpty())
                return error(QDBusError::UnknownInterface, QStringLiteral("No such interface."));
            if (method == QLatin1String("GetAll")) {
                QVariantMap properties;
                if (path == backendPath)
                    properties.insert(QStringLiteral("version"), m_service->version());
                connection.send(call.createReply(QList<QVariant>{properties}));
            } else if (path != backendPath || args.at(1).toString() != QLatin1String("version")) {
                return error(QDBusError::UnknownProperty, QStringLiteral("No such property."));
            } else if (method == QLatin1String("Set")) {
                return error(QDBusError::PropertyReadOnly, QStringLiteral("Version is read-only."));
            } else {
                connection.send(call.createReply(QList<QVariant>{QVariant::fromValue(QDBusVariant(m_service->version()))}));
            }
            return true;
        }
        if (path == backendPath && matches(chooserInterface)
            && (method == QLatin1String("OpenFile") || method == QLatin1String("SaveFile") || method == QLatin1String("SaveFiles"))) {
            if (call.signature() != QLatin1String("osssa{sv}"))
                return error(QDBusError::InvalidArgs, QStringLiteral("Expected osssa{sv}."));
            const auto args = call.arguments();
            const ChooserMode mode = method == QLatin1String("OpenFile") ? ChooserMode::OpenFile
                : method == QLatin1String("SaveFile") ? ChooserMode::SaveFile : ChooserMode::SaveFiles;
            m_service->createRequest(call, mode, qvariant_cast<QDBusObjectPath>(args.at(0)), args.at(1).toString(),
                args.at(2).toString(), args.at(3).toString(), qdbus_cast<QVariantMap>(args.at(4)));
            return true;
        }
        if (request && matches(requestInterface) && method == QLatin1String("Close")) {
            if (!call.signature().isEmpty())
                return error(QDBusError::InvalidArgs, QStringLiteral("Close takes no arguments."));
            m_service->m_requests.value(path)->close(call);
            return true;
        }
        return error(QDBusError::UnknownMethod, QStringLiteral("No such method on this portal object."));
    }
private:
    PortalChooserService *m_service;
};

PortalChooserService::PortalChooserService(const QDBusConnection &connection, QObject *parent)
    : QObject(parent), m_connection(connection), m_watcher(new QDBusServiceWatcher(frontendName, connection,
          QDBusServiceWatcher::WatchForOwnerChange, this)), m_router(new PortalChooserRouter(this))
{
    registerPortalDBusTypes();
    connect(m_watcher, &QDBusServiceWatcher::serviceOwnerChanged, this,
            [this](const QString &name, const QString &, const QString &newOwner) {
        if (name == frontendName) {
            ++m_frontendGeneration;
            cancelRequests();
        } else if (newOwner.isEmpty()) {
            const auto requests = m_requests;
            for (auto *request : requests) {
                if (request->owner() == name)
                    request->cancel();
            }
        }
    });
    m_connection.connect(QString(), QStringLiteral("/org/freedesktop/DBus/Local"),
        QStringLiteral("org.freedesktop.DBus.Local"), QStringLiteral("Disconnected"), this, SLOT(busDisconnected()));
}

PortalChooserService::~PortalChooserService()
{
    cancelRequests();
    if (m_claimed) {
        m_connection.unregisterObject(QStringLiteral("/"), QDBusConnection::UnregisterTree);
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
    // Supplied handles may be outside backendPath. A virtual root also fails
    // safely if this connection already exports an object; never clobber it.
    if (!m_connection.registerVirtualObject(QStringLiteral("/"), m_router, QDBusConnection::SubPath)) {
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

void PortalChooserService::deny(const QDBusMessage &call)
{
    m_connection.send(call.createErrorReply(QDBusError::AccessDenied,
        QStringLiteral("Only the current portal frontend may use this backend.")));
}

void PortalChooserService::authorize(const QDBusMessage &call, std::function<void(bool)> completed)
{
    if (m_authorizations >= maxAuthorizations || !call.service().startsWith(QLatin1Char(':'))) {
        completed(false);
        return;
    }
    ++m_authorizations;
    const quint64 generation = m_frontendGeneration;
    auto query = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"),
        QStringLiteral("org.freedesktop.DBus"), QStringLiteral("GetNameOwner"));
    query.setArguments({frontendName});
    auto *watcher = new QDBusPendingCallWatcher(m_connection.asyncCall(query, 5000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, call, generation, completed = std::move(completed)] {
        --m_authorizations;
        const QDBusPendingReply<QString> owner = *watcher;
        watcher->deleteLater();
        completed(!owner.isError() && owner.value() == call.service() && generation == m_frontendGeneration);
    });
}

void PortalChooserService::createRequest(const QDBusMessage &call, ChooserMode mode, const QDBusObjectPath &handle, const QString &appId,
                                       const QString &parentWindow, const QString &title, const QVariantMap &options)
{
    call.setDelayedReply(true);
    const QString path = handle.path();
    static const QRegularExpression requestPath(QStringLiteral("^(?:/[A-Za-z0-9_]+)+$"));
    auto invalid = [this, call](bool authorized) {
        if (authorized)
            reply(call, 2);
        else
            deny(call);
    };
    if (path.size() > 1024 || !requestPath.match(path).hasMatch() || path == backendPath || m_requests.contains(path)
        || m_requests.size() >= maxRequests || appId.size() > 4096 || appId.contains(QChar::Null)) {
        authorize(call, invalid);
        return;
    }
    // Reserve a bounded, UI-less request before the first async authorization
    // yield. The router serializes even already-queued Open and Close calls.
    auto *request = new PortalChooserRequest(this, call, path);
    m_requests.insert(path, request);
    m_watcher->addWatchedService(call.service());
    QPointer<PortalChooserRequest> guard(request);
    authorize(call, [guard, mode, parentWindow, title, options](bool authorized) {
        if (!guard)
            return;
        if (!authorized) {
            guard->rejectAuthorization();
            return;
        }
        auto config = portalChooserOptions(mode, parentWindow, title, options);
        if (!config.error.isEmpty())
            guard->finish(2);
        else
            guard->authorized(std::move(config));
    });
}

void PortalChooserService::reply(const QDBusMessage &call, uint response, const QVariantMap &results)
{
    // A method reply is destination-addressed. Never broadcast file results.
    m_connection.send(call.createReply({QVariant::fromValue(response), results}));
}

void PortalChooserService::removeRequest(const QString &path, PortalChooserRequest *request)
{
    // A delayed Close/authentication callback can outlive an old request. Never
    // remove a newer request which has reused the same frontend handle.
    if (m_requests.value(path) != request)
        return;
    m_requests.remove(path);
    const QString owner = request->owner();
    bool ownerInUse = false;
    for (auto *remaining : std::as_const(m_requests))
        ownerInUse = ownerInUse || remaining->owner() == owner;
    if (!ownerInUse)
        m_watcher->removeWatchedService(owner);
    request->dismiss();
    request->deleteLater();
}

void PortalChooserService::cancelRequests()
{
    const auto requests = m_requests;
    for (auto *request : requests)
        request->cancel();
}

void PortalChooserService::busDisconnected()
{
    ++m_frontendGeneration;
    cancelRequests();
}
