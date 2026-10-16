#include "PortalChooserService.h"
#include "PortalChooserOptions.h"
#include "PortalDBusTypes.h"
#include "ChooserPresenter.h"
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
// Each admission class leaves room for one Close per pending Open. Untrusted
// callers cannot consume the current frontend's pending or validation budget.
constexpr int maxAuthorizations = 2 * maxRequests;
} // namespace

class PortalChooserRequest : public QObject {
public:
    PortalChooserRequest(PortalChooserService *service, QDBusMessage call, QString path, bool frontendBudget)
        : QObject(service), m_service(service), m_call(std::move(call)), m_path(std::move(path)),
          m_frontendBudget(frontendBudget) {}
    ~PortalChooserRequest() override { dismiss(); }
    FileChooserDialog *dialog() const { return m_dialog; }
    QString owner() const { return m_call.service(); }
    bool frontendBudget() const { return m_frontendBudget; }

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
        if (m_terminal || !m_authorized || m_pendingCloses || m_dialog || m_presenter)
            return;
        if (!ChooserPresenter::requiredPlatform(m_config.window).isEmpty()) {
            m_presenter = new ChooserPresenter(this);
            m_presenter->start(m_config.chooser, m_config.window, [this](uint response, const ChooserSelection &selected) {
                if (m_terminal)
                    return;
                QVariantMap results;
                if (response == 0 && !selection(selected, &results))
                    response = 2;
                finish(response, response == 0 ? results : QVariantMap());
            });
            return;
        }
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
            const ChooserSelection selected{m_dialog->selectedUrls(), m_dialog->selectedFilter(), m_dialog->selectedChoices()};
            if (!selection(selected, &results)) {
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
        if (m_presenter)
            m_presenter->cancel();
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
    bool selection(const ChooserSelection &selected, QVariantMap *results)
    {
        const auto &options = m_config.chooser;
        const QList<QUrl> &urls = selected.urls;
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
            const int filter = selected.filter.toInt(&ok);
            if (!ok || filter < 0 || filter >= m_config.filters.size())
                return false;
            results->insert(QStringLiteral("current_filter"), QVariant::fromValue(m_config.filters.at(filter)));
        }
        if (!options.choices.isEmpty()) {
            PortalChoiceResults choices;
            for (const ChooserChoice &choice : options.choices) {
                const QString value = selected.choices.value(choice.id);
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
    QPointer<ChooserPresenter> m_presenter;
    int m_pendingCloses = 0;
    bool m_authorized = false;
    bool m_terminal = false;
    const bool m_frontendBudget;
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
        auto *request = m_service->requestForCaller(call.service(), path);
        QSet<QString> children;
        const QString prefix = path == QLatin1String("/") ? path : path + QLatin1Char('/');
        auto addChild = [&](const QString &candidate) {
            if (candidate.startsWith(prefix))
                children.insert(candidate.mid(prefix.size()).section(QLatin1Char('/'), 0, 0));
        };
        addChild(backendPath);
        for (auto it = m_service->m_requests.cbegin(); it != m_service->m_requests.cend(); ++it)
            addChild(it.key());
        for (const auto &pending : std::as_const(m_service->m_pendingRequests)) {
            for (auto it = pending.cbegin(); it != pending.cend(); ++it)
                addChild(it.key());
        }
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
            request->close(call);
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
            m_frontendOwner = newOwner;
            m_ownerReady = true;
            cancelRequests();
            cancelAuthorizations();
        } else if (newOwner.isEmpty()) {
            const auto requests = m_requests;
            for (auto *request : requests) {
                if (request->owner() == name)
                    request->cancel();
            }
            const auto pending = m_pendingRequests.value(name);
            for (auto *request : pending)
                request->cancel();
            cancelAuthorizations(name);
        }
    });
    m_connection.connect(QString(), QStringLiteral("/org/freedesktop/DBus/Local"),
        QStringLiteral("org.freedesktop.DBus.Local"), QStringLiteral("Disconnected"), this, SLOT(busDisconnected()));
    // This asynchronous lookup precedes claim's existing RegisterService call
    // on the same connection. Its reply is available before backend discovery;
    // claim reads only an already-finished reply, never waits for an owner.
    auto query = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"),
        QStringLiteral("org.freedesktop.DBus"), QStringLiteral("GetNameOwner"));
    query.setArguments({frontendName});
    m_initialOwnerGeneration = m_frontendGeneration;
    m_initialOwner = new QDBusPendingCallWatcher(m_connection.asyncCall(query, 5000), this);
    connect(m_initialOwner, &QDBusPendingCallWatcher::finished, this, [this] {
        readInitialOwner();
        m_initialOwner->deleteLater();
        m_initialOwner = nullptr;
    });
}

PortalChooserService::~PortalChooserService()
{
    cancelRequests();
    cancelAuthorizations();
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
    // Do not admit calls into an uncertain budget at startup, including when
    // delivery of the lookup's finished signal has not reached the GUI yet.
    if (!readInitialOwner()) {
        m_connection.unregisterService(backendName);
        return false;
    }
    // Supplied handles may be outside backendPath. A virtual root also fails
    // safely if this connection already exports an object; never clobber it.
    if (!m_connection.registerVirtualObject(QStringLiteral("/"), m_router, QDBusConnection::SubPath)) {
        m_connection.unregisterService(backendName);
        return false;
    }
    m_claimed = true;
    return true;
}

bool PortalChooserService::readInitialOwner()
{
    if (m_ownerReady)
        return true;
    if (!m_initialOwner || !m_initialOwner->isFinished())
        return false;
    const QDBusPendingReply<QString> owner = *m_initialOwner;
    if (m_initialOwnerGeneration == m_frontendGeneration) {
        m_frontendOwner = owner.isError() ? QString() : owner.value();
        m_ownerReady = true;
    }
    return m_ownerReady;
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
    const bool frontendBudget = call.service() == m_frontendOwner;
    int &count = frontendBudget ? m_frontendAuthorizations : m_otherAuthorizations;
    if (count >= maxAuthorizations || !call.service().startsWith(QLatin1Char(':'))) {
        completed(false);
        return;
    }
    ++count;
    const quint64 generation = m_frontendGeneration;
    auto query = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"),
        QStringLiteral("org.freedesktop.DBus"), QStringLiteral("GetNameOwner"));
    query.setArguments({frontendName});
    auto *watcher = new QDBusPendingCallWatcher(m_connection.asyncCall(query, 5000), this);
    m_watcher->addWatchedService(call.service());
    m_authorizations.insert(watcher, {call.service(), [this, call, frontendBudget, completed = std::move(completed)](bool authorized) {
        --(frontendBudget ? m_frontendAuthorizations : m_otherAuthorizations);
        completed(authorized);
        unwatchUnusedSender(call.service());
    }});
    QPointer<QDBusPendingCallWatcher> guard(watcher);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, guard, call, generation] {
        if (!guard || !m_authorizations.contains(guard.data()))
            return;
        const auto authorization = m_authorizations.take(guard.data());
        const QDBusPendingReply<QString> owner = *guard;
        guard->deleteLater();
        authorization.completed(!owner.isError() && owner.value() == call.service() && generation == m_frontendGeneration);
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
    const bool frontendBudget = call.service() == m_frontendOwner;
    const int pendingCount = frontendBudget ? m_frontendPending : m_otherPending;
    if (path.size() > 1024 || !requestPath.match(path).hasMatch() || path == backendPath
        || appId.size() > 4096 || appId.contains(QChar::Null)
        || m_pendingRequests.value(call.service()).contains(path) || pendingCount >= maxRequests
        || (m_requests.contains(path) && m_requests.value(path)->owner() == call.service())) {
        authorize(call, invalid);
        return;
    }
    // Only sender-scoped pending state exists before authorization. It routes
    // an already-queued Close without reserving a global handle or UI slot.
    auto *request = new PortalChooserRequest(this, call, path, frontendBudget);
    m_pendingRequests[call.service()].insert(path, request);
    ++(frontendBudget ? m_frontendPending : m_otherPending);
    m_watcher->addWatchedService(call.service());
    QPointer<PortalChooserRequest> guard(request);
    authorize(call, [this, guard, path, mode, parentWindow, title, options](bool authorized) {
        if (!guard || m_pendingRequests.value(guard->owner()).value(path) != guard)
            return;
        if (!authorized) {
            guard->rejectAuthorization();
            return;
        }
        // Another sender's unvalidated call must never latch this decision.
        // Capacity and handle ownership become authoritative only now.
        if (m_requests.contains(path) || m_requests.size() >= maxRequests) {
            guard->finish(2);
            return;
        }
        auto config = portalChooserOptions(mode, parentWindow, title, options);
        if (!config.error.isEmpty())
            guard->finish(2);
        else {
            removePendingRequest(path, guard);
            m_requests.insert(path, guard);
            guard->authorized(std::move(config));
        }
    });
}

void PortalChooserService::reply(const QDBusMessage &call, uint response, const QVariantMap &results)
{
    // A method reply is destination-addressed. Never broadcast file results.
    m_connection.send(call.createReply({QVariant::fromValue(response), results}));
}

PortalChooserRequest *PortalChooserService::requestForCaller(const QString &sender, const QString &path) const
{
    if (auto *pending = m_pendingRequests.value(sender).value(path))
        return pending;
    if (auto *request = m_requests.value(path))
        return request;
    // Preserve existence/origin errors for other callers, without allowing a
    // foreign pending handle to shadow this sender's own pending or active UI.
    for (const auto &pending : m_pendingRequests) {
        if (auto *request = pending.value(path))
            return request;
    }
    return nullptr;
}

void PortalChooserService::removePendingRequest(const QString &path, PortalChooserRequest *request)
{
    auto sender = m_pendingRequests.find(request->owner());
    if (sender == m_pendingRequests.end() || sender->value(path) != request)
        return;
    sender->remove(path);
    if (sender->isEmpty())
        m_pendingRequests.erase(sender);
    --(request->frontendBudget() ? m_frontendPending : m_otherPending);
}

void PortalChooserService::removeRequest(const QString &path, PortalChooserRequest *request)
{
    // A delayed Close/authentication callback can outlive an old request. Never
    // remove a newer request which has reused the same frontend handle.
    if (m_requests.value(path) != request && m_pendingRequests.value(request->owner()).value(path) != request)
        return;
    if (m_requests.value(path) == request)
        m_requests.remove(path);
    removePendingRequest(path, request);
    unwatchUnusedSender(request->owner());
    request->dismiss();
    request->deleteLater();
}

void PortalChooserService::unwatchUnusedSender(const QString &sender)
{
    if (m_pendingRequests.contains(sender))
        return;
    for (auto *remaining : std::as_const(m_requests))
        if (remaining->owner() == sender)
            return;
    for (const auto &authorization : std::as_const(m_authorizations))
        if (authorization.sender == sender)
            return;
    m_watcher->removeWatchedService(sender);
}

void PortalChooserService::cancelRequests()
{
    const auto requests = m_requests;
    const auto pending = m_pendingRequests;
    for (auto *request : requests)
        request->cancel();
    for (const auto &sender : pending)
        for (auto *request : sender)
            request->cancel();
}

void PortalChooserService::cancelAuthorizations(const QString &sender)
{
    QHash<QDBusPendingCallWatcher *, Authorization> canceled;
    for (auto it = m_authorizations.begin(); it != m_authorizations.end();) {
        if (sender.isEmpty() || it->sender == sender) {
            canceled.insert(it.key(), std::move(it.value()));
            it = m_authorizations.erase(it);
        } else {
            ++it;
        }
    }
    // Completions can remove requests and watched services. Detach the whole
    // batch first so none can settle a watcher twice or mutate this traversal.
    for (auto it = canceled.begin(); it != canceled.end(); ++it) {
        disconnect(it.key(), nullptr, this, nullptr);
        it.key()->deleteLater();
        it->completed(false);
    }
}

void PortalChooserService::busDisconnected()
{
    ++m_frontendGeneration;
    cancelRequests();
    cancelAuthorizations();
}
