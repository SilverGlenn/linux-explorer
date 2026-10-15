#include "PortalChooserService.h"
#include "PortalChooserOptions.h"
#include "PortalDBusTypes.h"
#include "DirectoryModel.h"
#include "FileChooserDialog.h"
#include "FileView.h"

#include <QApplication>
#include <QCheckBox>
#include <QChildEvent>
#include <QComboBox>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusPendingCallWatcher>
#include <QDBusVariant>
#include <QDialogButtonBox>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QLineEdit>
#include <QLabel>
#include <QMessageBox>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QXmlStreamReader>
#include <memory>
#include <utility>
#include <vector>

namespace {
const QString backend = QStringLiteral("org.freedesktop.impl.portal.desktop.explorerwin7");
const QString frontend = QStringLiteral("org.freedesktop.portal.Desktop");
const QString rootPath = QStringLiteral("/org/freedesktop/portal/desktop");
const QString chooserInterface = QStringLiteral("org.freedesktop.impl.portal.FileChooser");
const QString requestInterface = QStringLiteral("org.freedesktop.impl.portal.Request");

QByteArray pathBytes(const QString &path) { return QFile::encodeName(path) + '\0'; }
QVariant filtersValue(const PortalFilters &filters) { return QVariant::fromValue(filters); }
QVariant choicesValue(const PortalChoices &choices) { return QVariant::fromValue(choices); }
QVariant namesValue(const PortalFileNames &names) { return QVariant::fromValue(names); }

QDBusMessage call(const QDBusConnection &bus, const QString &path, const QString &interface,
                  const QString &method, const QList<QVariant> &args = {})
{
    auto message = QDBusMessage::createMethodCall(backend, path, interface, method);
    message.setArguments(args);
    QDBusPendingCallWatcher watcher(bus.asyncCall(message, 5000));
    QEventLoop loop;
    QObject::connect(&watcher, &QDBusPendingCallWatcher::finished, &loop, &QEventLoop::quit);
    if (!watcher.isFinished())
        loop.exec();
    return watcher.reply();
}

void writeFile(const QString &path, const QByteArray &content = "untouched")
{
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(content), content.size());
}

QPushButton *acceptButton(FileChooserDialog *dialog)
{
    auto *buttons = dialog->findChild<QDialogButtonBox *>();
    if (!buttons)
        return nullptr;
    for (auto *button : buttons->buttons()) {
        if (buttons->buttonRole(button) == QDialogButtonBox::AcceptRole)
            return qobject_cast<QPushButton *>(button);
    }
    return nullptr;
}

// Hold only delivery of real GetNameOwner completions. ChildAdded runs before
// a watcher can emit finished; replay its blocked signal once the real reply
// has arrived. No production scheduling hook or fabricated authorization.
class AuthorizationBarrier : public QObject {
public:
    explicit AuthorizationBarrier(QObject *service) : m_service(service) { service->installEventFilter(this); }
    ~AuthorizationBarrier() override { m_service->removeEventFilter(this); releaseAll(); }
    QList<QDBusPendingCallWatcher *> watchers() const
    {
        QList<QDBusPendingCallWatcher *> result;
        for (const auto &child : m_children) {
            if (auto *watcher = qobject_cast<QDBusPendingCallWatcher *>(child.data()))
                result.append(watcher);
        }
        return result;
    }
    bool ready(int count) const
    {
        const auto pending = watchers();
        if (pending.size() != count)
            return false;
        for (const auto *watcher : pending) {
            if (!watcher->isFinished())
                return false;
        }
        return true;
    }
    void release(QDBusPendingCallWatcher *watcher)
    {
        m_children.removeAll(watcher);
        watcher->blockSignals(false);
        if (watcher->isFinished()) {
            watcher->finished(watcher);
            // isFinished() can precede native signal delivery. Suppress that
            // queued emission after our replay; the handler deletes later.
            watcher->blockSignals(true);
        }
    }
    void releaseAll()
    {
        for (auto *watcher : watchers())
            release(watcher);
        for (const auto &child : std::as_const(m_children)) {
            if (child)
                child->blockSignals(false);
        }
        m_children.clear();
    }
protected:
    bool eventFilter(QObject *, QEvent *event) override
    {
        if (event->type() == QEvent::ChildAdded) {
            auto *child = static_cast<QChildEvent *>(event)->child();
            child->blockSignals(true);
            m_children.append(child);
        }
        return false;
    }
private:
    QObject *m_service;
    QList<QPointer<QObject>> m_children;
};

class DialogShows : public QObject {
public:
    int count = 0;
    DialogShows() { qApp->installEventFilter(this); }
    ~DialogShows() override { qApp->removeEventFilter(this); }
protected:
    bool eventFilter(QObject *object, QEvent *event) override
    {
        if (event->type() == QEvent::Show && qobject_cast<FileChooserDialog *>(object))
            ++count;
        return false;
    }
};

struct Pending {
    QString path;
    QDBusPendingCallWatcher *watcher;
    uint status() const { return watcher->reply().arguments().value(0).toUInt(); }
    QVariantMap results() const { return qdbus_cast<QVariantMap>(watcher->reply().arguments().value(1)); }
};
} // namespace

class PortalChooserTest : public QObject {
    Q_OBJECT
    QDBusConnection m_bus{QStringLiteral("invalid")};
    QDBusConnection m_frontend{QStringLiteral("invalid")};
    QDBusConnection m_app{QStringLiteral("invalid")};
    PortalChooserService *m_service = nullptr;
    QString m_suffix;
    int m_serial = 0;
    int m_requestSerial = 0;

    Pending begin(const QString &method = QStringLiteral("OpenFile"), const QVariantMap &options = {},
                  const QString &appId = QStringLiteral("org.example.App"), const QString &parent = {},
                  const QString &path = {}, bool authorized = true)
    {
        const QString handle = path.isEmpty() ? rootPath + QStringLiteral("/request/1_42/r%1").arg(++m_requestSerial) : path;
        auto message = QDBusMessage::createMethodCall(backend, rootPath, chooserInterface, method);
        message.setArguments({QVariant::fromValue(QDBusObjectPath(handle)), appId, parent, QStringLiteral("Portal chooser test"), options});
        return {handle, new QDBusPendingCallWatcher((authorized ? m_frontend : m_app).asyncCall(message, 10000), this)};
    }
    QDBusPendingCallWatcher *beginClose(const Pending &request, bool authorized = true)
    {
        auto message = QDBusMessage::createMethodCall(backend, request.path, requestInterface, QStringLiteral("Close"));
        return new QDBusPendingCallWatcher((authorized ? m_frontend : m_app).asyncCall(message, 10000), this);
    }
    void close(const Pending &request)
    {
        QCOMPARE(call(m_frontend, request.path, requestInterface, QStringLiteral("Close")).type(), QDBusMessage::ReplyMessage);
    }

private Q_SLOTS:
    void initTestCase()
    {
        QApplication::setQuitOnLastWindowClosed(false);
        QApplication::setQuitLockEnabled(false);
        registerPortalDBusTypes();
    }
    void init()
    {
        m_suffix = QString::number(++m_serial);
        m_bus = QDBusConnection::connectToBus(QDBusConnection::SessionBus, QStringLiteral("portal-service-") + m_suffix);
        m_frontend = QDBusConnection::connectToBus(QDBusConnection::SessionBus, QStringLiteral("portal-frontend-") + m_suffix);
        m_app = QDBusConnection::connectToBus(QDBusConnection::SessionBus, QStringLiteral("portal-app-") + m_suffix);
        QVERIFY(m_bus.isConnected());
        QVERIFY(m_frontend.isConnected());
        QVERIFY(m_app.isConnected());
        QVERIFY(m_frontend.registerService(frontend));
        m_service = new PortalChooserService(m_bus);
        QVERIFY(m_service->claim());
    }
    void cleanup()
    {
        delete m_service;
        m_service = nullptr;
        m_bus = QDBusConnection(QStringLiteral("invalid"));
        m_frontend = QDBusConnection(QStringLiteral("invalid"));
        m_app = QDBusConnection(QStringLiteral("invalid"));
        for (const auto &prefix : {QStringLiteral("portal-service-"), QStringLiteral("portal-frontend-"), QStringLiteral("portal-app-")})
            QDBusConnection::disconnectFromBus(prefix + m_suffix);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        qDeleteAll(findChildren<QDBusPendingCallWatcher *>());
    }
    void wireAndOnlyFileChooserInterface()
    {
        QCOMPARE(QByteArray(QDBusMetaType::typeToSignature(QMetaType::fromType<PortalFilters>())), QByteArray("a(sa(us))"));
        QCOMPARE(QByteArray(QDBusMetaType::typeToSignature(QMetaType::fromType<PortalFilter>())), QByteArray("(sa(us))"));
        QCOMPARE(QByteArray(QDBusMetaType::typeToSignature(QMetaType::fromType<PortalChoices>())), QByteArray("a(ssa(ss)s)"));
        QCOMPARE(QByteArray(QDBusMetaType::typeToSignature(QMetaType::fromType<PortalChoiceResults>())), QByteArray("a(ss)"));
        QCOMPARE(QByteArray(QDBusMetaType::typeToSignature(QMetaType::fromType<PortalFileNames>())), QByteArray("aay"));
        auto reply = call(m_app, rootPath, QStringLiteral("org.freedesktop.DBus.Introspectable"), QStringLiteral("Introspect"));
        QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
        const QString xml = reply.arguments().first().toString();
        QVERIFY(xml.contains(chooserInterface));
        for (const auto &method : {QStringLiteral("OpenFile"), QStringLiteral("SaveFile"), QStringLiteral("SaveFiles")})
            QVERIFY(xml.contains(QStringLiteral("name=\"%1\"").arg(method)));
        QVERIFY(!xml.contains(QStringLiteral("FileManager")));
        QVERIFY(!xml.contains(QStringLiteral("impl.portal.Settings")));
        QVERIFY(!xml.contains(QStringLiteral("busDisconnected")));
        QXmlStreamReader reader(xml);
        QString method, inputs, outputs;
        int methods = 0;
        while (!reader.atEnd()) {
            reader.readNext();
            if (reader.isStartElement() && reader.name() == QLatin1String("method")) {
                method = reader.attributes().value("name").toString();
                inputs.clear(); outputs.clear();
            } else if (reader.isStartElement() && reader.name() == QLatin1String("arg")) {
                if (reader.attributes().value("direction") == QLatin1String("in"))
                    inputs += reader.attributes().value("type");
                else
                    outputs += reader.attributes().value("type");
            } else if (reader.isEndElement() && reader.name() == QLatin1String("method")
                       && (method == QLatin1String("OpenFile") || method == QLatin1String("SaveFile") || method == QLatin1String("SaveFiles"))) {
                QCOMPARE(inputs, QStringLiteral("osssa{sv}"));
                QCOMPARE(outputs, QStringLiteral("ua{sv}"));
                ++methods;
            }
        }
        QVERIFY(!reader.hasError());
        QCOMPARE(methods, 3);
        reply = call(m_app, rootPath, QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("Get"),
                     {chooserInterface, QStringLiteral("version")});
        QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
        QCOMPARE(qvariant_cast<QDBusVariant>(reply.arguments().first()).variant().metaType(), QMetaType::fromType<uint>());
        QCOMPARE(qvariant_cast<QDBusVariant>(reply.arguments().first()).variant().toUInt(), 4u);
        QVERIFY(QFile::exists(QStringLiteral(":/win7/commandbar.png")));
    }
    void delayedCancelCloseAndAuthentication()
    {
        const Pending denied = begin(QStringLiteral("OpenFile"), {}, {}, {}, {}, false);
        QTRY_VERIFY(denied.watcher->isFinished());
        QCOMPARE(denied.watcher->reply().errorName(), QStringLiteral("org.freedesktop.DBus.Error.AccessDenied"));
        QCOMPARE(m_service->requestCount(), 0);
        // Standard modal defaults true, but parentless callers must work.
        const Pending pending = begin();
        QTRY_VERIFY(m_service->dialogForRequest(pending.path));
        QPointer<FileChooserDialog> dialog = m_service->dialogForRequest(pending.path);
        QVERIFY(dialog->isVisible());
        QCOMPARE(dialog->windowModality(), Qt::NonModal);
        QVERIFY(!pending.watcher->isFinished());
        const auto xml = call(m_app, pending.path, QStringLiteral("org.freedesktop.DBus.Introspectable"), QStringLiteral("Introspect")).arguments().first().toString();
        QVERIFY(xml.contains(requestInterface));
        QVERIFY(!xml.contains(QStringLiteral("Finished")));
        QVERIFY(!xml.contains(QStringLiteral("GetResult")));
        const auto unauthorized = call(m_app, pending.path, requestInterface, QStringLiteral("Close"));
        QCOMPARE(unauthorized.errorName(), QStringLiteral("org.freedesktop.DBus.Error.AccessDenied"));
        QVERIFY(!pending.watcher->isFinished());
        QVERIFY(dialog->isVisible());
        close(pending);
        QTRY_VERIFY(pending.watcher->isFinished());
        QCOMPARE(pending.watcher->reply().signature(), QStringLiteral("ua{sv}"));
        QCOMPARE(pending.status(), 1u);
        QVERIFY(pending.results().isEmpty());
        QTRY_VERIFY(dialog.isNull());
        QCOMPARE(m_service->requestCount(), 0);
        QCOMPARE(call(m_frontend, pending.path, requestInterface, QStringLiteral("Close")).type(), QDBusMessage::ErrorMessage);
    }
    void pendingCloseBlocksUiAndLateCloseCannotDeleteReusedHandle_data()
    {
        QTest::addColumn<bool>("openAuthFirst");
        QTest::newRow("open-auth-first") << true;
        QTest::newRow("close-auth-first") << false;
    }
    void pendingCloseBlocksUiAndLateCloseCannotDeleteReusedHandle()
    {
        QFETCH(bool, openAuthFirst);
        AuthorizationBarrier barrier(m_service);
        DialogShows shows;
        const Pending first = begin("OpenFile", {}, {}, {}, "/nonstandard/early_close");
        QSignalSpy replied(first.watcher, &QDBusPendingCallWatcher::finished);
        auto *close1 = beginClose(first);
        auto *close2 = beginClose(first);
        QSignalSpy close1Replied(close1, &QDBusPendingCallWatcher::finished);
        QSignalSpy close2Replied(close2, &QDBusPendingCallWatcher::finished);
        QTRY_VERIFY(barrier.ready(3));
        QCOMPARE(m_service->requestCount(), 1);
        QVERIFY(!m_service->dialogForRequest(first.path));
        QVERIFY(!close1->isFinished()); QVERIFY(!close2->isFinished());
        const auto queries = barrier.watchers();
        if (openAuthFirst)
            barrier.release(queries.at(0)); // Close is already pending, so no UI
        QTest::qWait(20);
        QCOMPARE(shows.count, 0);
        QVERIFY(!first.watcher->isFinished());
        barrier.release(queries.at(2)); // reply to the second Close first
        QTRY_VERIFY(close2->isFinished());
        QTRY_VERIFY(first.watcher->isFinished());
        QCOMPARE(close2->reply().type(), QDBusMessage::ReplyMessage);
        QCOMPARE(first.status(), 1u); QVERIFY(first.results().isEmpty());
        QCOMPARE(m_service->requestCount(), 0);
        QCOMPARE(shows.count, 0);
        QVERIFY(m_frontend.isConnected());
        const Pending current = begin("OpenFile", {}, {}, {}, first.path);
        QTRY_VERIFY(barrier.ready(openAuthFirst ? 2 : 3));
        barrier.release(barrier.watchers().last());
        QTRY_VERIFY(m_service->dialogForRequest(current.path));
        QPointer<FileChooserDialog> dialog = m_service->dialogForRequest(current.path);
        if (!openAuthFirst)
            barrier.release(queries.at(0)); // late original auth also belongs to the old identity
        barrier.release(queries.at(1)); // stale Close belongs to the old identity
        QTRY_VERIFY(close1->isFinished());
        QCOMPARE(close1->reply().type(), QDBusMessage::ReplyMessage);
        QTRY_COMPARE(replied.count(), 1);
        QTRY_COMPARE(close1Replied.count(), 1); QTRY_COMPARE(close2Replied.count(), 1);
        QVERIFY(!current.watcher->isFinished()); QVERIFY(dialog->isVisible());
        QCOMPARE(m_service->requestCount(), 1);
        auto *closed = beginClose(current);
        QTRY_VERIFY(barrier.ready(1));
        barrier.releaseAll();
        QTRY_VERIFY(closed->isFinished()); QTRY_VERIFY(current.watcher->isFinished());
        QCOMPARE(current.status(), 1u);
        QTRY_VERIFY(dialog.isNull());
    }
    void pendingDeniedOpenAndCloseCannotAffectReusedHandle()
    {
        AuthorizationBarrier barrier(m_service);
        DialogShows shows;
        const Pending denied = begin("OpenFile", {}, {}, {}, {}, false);
        QSignalSpy replied(denied.watcher, &QDBusPendingCallWatcher::finished);
        auto *deniedClose = beginClose(denied, false);
        QTRY_VERIFY(barrier.ready(2));
        QCOMPARE(m_service->requestCount(), 1);
        QCOMPARE(shows.count, 0);
        const auto queries = barrier.watchers();
        barrier.release(queries.at(0));
        QTRY_VERIFY(denied.watcher->isFinished());
        QCOMPARE(denied.watcher->reply().errorName(), QStringLiteral("org.freedesktop.DBus.Error.AccessDenied"));
        QCOMPARE(m_service->requestCount(), 0);
        const Pending current = begin("OpenFile", {}, {}, {}, denied.path);
        QTRY_VERIFY(barrier.ready(2));
        barrier.release(barrier.watchers().last());
        QTRY_VERIFY(m_service->dialogForRequest(current.path));
        barrier.release(queries.at(1));
        QTRY_VERIFY(deniedClose->isFinished());
        QCOMPARE(deniedClose->reply().errorName(), QStringLiteral("org.freedesktop.DBus.Error.AccessDenied"));
        QCOMPARE(replied.count(), 1);
        QVERIFY(!current.watcher->isFinished());
        QVERIFY(m_service->dialogForRequest(current.path)->isVisible());
        auto *closed = beginClose(current);
        QTRY_VERIFY(barrier.ready(1)); barrier.releaseAll();
        QTRY_VERIFY(closed->isFinished()); QTRY_VERIFY(current.watcher->isFinished());
    }
    void pendingOwnerChangeAndOriginDisconnectCleanUpImmediately()
    {
        AuthorizationBarrier barrier(m_service);
        DialogShows shows;
        const Pending old = begin();
        QSignalSpy replied(old.watcher, &QDBusPendingCallWatcher::finished);
        auto *oldClose = beginClose(old);
        QTRY_VERIFY(barrier.ready(2));
        const auto stale = barrier.watchers();
        QVERIFY(m_frontend.unregisterService(frontend));
        QTRY_COMPARE(m_service->requestCount(), 0); // auth delivery is still held
        QTRY_VERIFY(old.watcher->isFinished());
        QCOMPARE(old.watcher->reply().errorName(), QStringLiteral("org.freedesktop.DBus.Error.AccessDenied"));
        QCOMPARE(shows.count, 0);
        QVERIFY(m_app.registerService(frontend));
        const Pending current = begin("OpenFile", {}, {}, {}, old.path, false);
        QTRY_VERIFY(barrier.ready(3));
        // Current owner alone is insufficient: Close must also match origin.
        auto *wrongOrigin = beginClose(current);
        QTRY_VERIFY(wrongOrigin->isFinished());
        QCOMPARE(wrongOrigin->reply().errorName(), QStringLiteral("org.freedesktop.DBus.Error.AccessDenied"));
        barrier.release(barrier.watchers().last());
        QTRY_VERIFY(m_service->dialogForRequest(current.path));
        barrier.release(stale.at(0)); barrier.release(stale.at(1));
        QTRY_VERIFY(oldClose->isFinished());
        QCOMPARE(oldClose->reply().errorName(), QStringLiteral("org.freedesktop.DBus.Error.AccessDenied"));
        QCOMPARE(replied.count(), 1);
        QVERIFY(!current.watcher->isFinished());
        QVERIFY(m_service->dialogForRequest(current.path)->isVisible());
        auto *closed = beginClose(current, false);
        QTRY_VERIFY(barrier.ready(1)); barrier.releaseAll();
        QTRY_VERIFY(closed->isFinished()); QTRY_VERIFY(current.watcher->isFinished());
        QVERIFY(m_app.unregisterService(frontend));
        QVERIFY(m_frontend.registerService(frontend));
        const Pending disconnected = begin("OpenFile", {}, {}, {}, {}, false);
        QTRY_VERIFY(barrier.ready(1));
        QCOMPARE(m_service->requestCount(), 1);
        m_app = QDBusConnection(QStringLiteral("invalid"));
        QDBusConnection::disconnectFromBus(QStringLiteral("portal-app-") + m_suffix);
        QTRY_COMPARE(m_service->requestCount(), 0); // don't wait for auth timeout
        barrier.releaseAll();
        QTest::qWait(20);
        QCOMPARE(m_service->requestCount(), 0);
        QCOMPARE(shows.count, 1); // only the explicitly authorized current request
    }
    void pendingRequestsAreBoundedAndLeaveRoomForClose()
    {
        AuthorizationBarrier barrier(m_service);
        DialogShows shows;
        QList<Pending> requests;
        QList<QDBusPendingCallWatcher *> closes;
        std::vector<std::unique_ptr<QSignalSpy>> replies;
        auto countReply = [&](QDBusPendingCallWatcher *watcher) {
            replies.push_back(std::make_unique<QSignalSpy>(watcher, &QDBusPendingCallWatcher::finished));
        };
        for (int i = 0; i < 64; ++i) {
            requests.append(begin());
            countReply(requests.last().watcher);
        }
        QTRY_VERIFY(barrier.ready(64));
        QCOMPARE(m_service->requestCount(), 64);
        // Resolve overflow before reaching the daemon's 128 pending-reply
        // limit: this must be the authorized frontend's backend rejection.
        const Pending overflow = begin();
        QTRY_VERIFY(barrier.ready(65));
        barrier.release(barrier.watchers().last());
        QTRY_VERIFY(overflow.watcher->isFinished());
        QCOMPARE(overflow.watcher->reply().type(), QDBusMessage::ReplyMessage);
        QCOMPARE(overflow.status(), 2u);
        // Capacity must leave room for a legitimate early Close per Open.
        for (const auto &request : requests) {
            closes.append(beginClose(request));
            countReply(closes.last());
        }
        QTRY_VERIFY(barrier.ready(128));
        const Pending authOverflow = begin("OpenFile", {}, {}, {}, {}, false);
        QTRY_VERIFY(authOverflow.watcher->isFinished());
        QCOMPARE(authOverflow.watcher->reply().errorName(), QStringLiteral("org.freedesktop.DBus.Error.AccessDenied"));
        const auto queries = barrier.watchers();
        for (int i = 0; i < 64; ++i)
            barrier.release(queries.at(i));
        QTest::qWait(20);
        QCOMPARE(m_service->requestCount(), 64);
        QCOMPARE(shows.count, 0); // even when all Open authorizations win
        barrier.releaseAll();
        QTRY_VERIFY(([&] {
            for (const auto &spy : replies) {
                if (spy->count() != 1)
                    return false;
            }
            return true;
        })());
        for (const auto &request : requests) {
            QCOMPARE(request.status(), 1u);
            QVERIFY(request.results().isEmpty());
        }
        for (const auto *closed : std::as_const(closes))
            QCOMPARE(closed->reply().type(), QDBusMessage::ReplyMessage);
        QCOMPARE(m_service->requestCount(), 0);
        QVERIFY(m_frontend.isConnected());
        QTest::qWait(20);
        QCOMPARE(m_service->requestCount(), 0);
        QCOMPARE(shows.count, 0);
    }
    void virtualRoutingPreservesWireValidationAndOccupiedObjects()
    {
        auto reply = call(m_frontend, rootPath, chooserInterface, "OpenFile", {QStringLiteral("wrong type")});
        QCOMPARE(reply.errorName(), QStringLiteral("org.freedesktop.DBus.Error.InvalidArgs"));
        reply = call(m_frontend, rootPath, "org.freedesktop.DBus.Properties", "GetAll", {chooserInterface});
        QCOMPARE(reply.signature(), QStringLiteral("a{sv}"));
        QCOMPARE(qdbus_cast<QVariantMap>(reply.arguments().first()).value("version").toUInt(), 4u);
        reply = call(m_frontend, rootPath, "org.freedesktop.DBus.Properties", "Set",
                     {chooserInterface, QStringLiteral("version"), QVariant::fromValue(QDBusVariant(1u))});
        QCOMPARE(reply.errorName(), QStringLiteral("org.freedesktop.DBus.Error.PropertyReadOnly"));
        QCOMPARE(call(m_frontend, rootPath, "org.freedesktop.DBus.Peer", "Ping").type(), QDBusMessage::ReplyMessage);
        QCOMPARE(call(m_frontend, rootPath, "org.freedesktop.DBus.Peer", "GetMachineId").signature(), QStringLiteral("s"));
        QCOMPARE(call(m_frontend, "/unknown/handle", requestInterface, "Close").errorName(),
                 QStringLiteral("org.freedesktop.DBus.Error.UnknownObject"));
        const Pending pending = begin("OpenFile", {}, {}, {}, "/nonstandard/queued/handle");
        QTRY_VERIFY(m_service->dialogForRequest(pending.path));
        reply = call(m_frontend, pending.path, requestInterface, "Close", {1u});
        QCOMPARE(reply.errorName(), QStringLiteral("org.freedesktop.DBus.Error.InvalidArgs"));
        reply = call(m_frontend, pending.path, chooserInterface, "Close");
        QCOMPARE(reply.errorName(), QStringLiteral("org.freedesktop.DBus.Error.UnknownMethod"));
        reply = call(m_frontend, pending.path, "org.freedesktop.DBus.Properties", "Get", {requestInterface, QStringLiteral("version")});
        QCOMPARE(reply.errorName(), QStringLiteral("org.freedesktop.DBus.Error.UnknownProperty"));
        QVERIFY(!pending.watcher->isFinished());
        close(pending);
        QTRY_VERIFY(pending.watcher->isFinished());
        delete m_service; m_service = nullptr;
        auto restore = qScopeGuard([&] {
            m_bus.unregisterObject(QStringLiteral("/occupied_object"));
            m_service = new PortalChooserService(m_bus);
            QVERIFY(m_service->claim());
        });
        QVERIFY(m_bus.registerObject(QStringLiteral("/occupied_object"), this));
        PortalChooserService blocked(m_bus);
        QVERIFY(!blocked.claim()); // virtual root must never clobber an existing export
        QCOMPARE(m_bus.objectRegisteredAt(QStringLiteral("/occupied_object")), this);
    }
    void cancelFromActualDialog()
    {
        const Pending pending = begin();
        QTRY_VERIFY(m_service->dialogForRequest(pending.path));
        m_service->dialogForRequest(pending.path)->reject();
        QTRY_VERIFY(pending.watcher->isFinished());
        QCOMPARE(pending.status(), 1u);
        QVERIFY(pending.results().isEmpty());
        QCOMPARE(m_service->requestCount(), 0);
    }
    void invalidOptions_data()
    {
        QTest::addColumn<QString>("method");
        QTest::addColumn<QVariantMap>("options");
        auto row = [](const char *name, const QVariantMap &options, const QString &method = QStringLiteral("OpenFile")) {
            QTest::newRow(name) << method << options;
        };
        row("multiple-type", {{"multiple", 1}});
        row("directory-type", {{"directory", QStringLiteral("true")}});
        row("modal-type", {{"modal", 1}});
        row("accept-label-type", {{"accept_label", true}});
        row("token-type", {{"activation_token", 2u}});
        row("name-type", {{"current_name", true}});
        row("folder-string", {{"current_folder", QStringLiteral("/tmp")}});
        row("folder-empty-array", {{"current_folder", QByteArray()}});
        row("folder-unterminated", {{"current_folder", QByteArray("/tmp")}});
        row("folder-relative", {{"current_folder", QByteArray("tmp\0", 4)}});
        row("folder-embedded-nul", {{"current_folder", QByteArray("/tmp\0hidden\0", 12)}});
        row("folder-lossy-bytes", {{"current_folder", QByteArray("/tmp/\xff\0", 7)}});
        row("folder-too-large", {{"current_folder", QByteArray(17000, 'x') + '\0'}});
        row("file-type", {{"current_file", true}}, "SaveFile");
        row("file-relative", {{"current_file", QByteArray("leaf\0", 5)}}, "SaveFile");
        row("file-oversized-leaf", {{"current_file", QByteArray("/tmp/") + QByteArray(256, 'x') + '\0'}}, "SaveFile");
        row("file-embedded-nul", {{"current_file", QByteArray("/tmp/x\0y\0", 9)}}, "SaveFile");
        row("name-nonleaf", {{"current_name", QStringLiteral("../escape")}}, "SaveFile");
        row("name-dot", {{"current_name", QStringLiteral(".")}}, "SaveFile");
        row("save-multiple", {{"multiple", true}}, "SaveFile");
        row("save-directory", {{"directory", true}}, "SaveFile");
        row("filter-type", {{"filters", QStringList{"*.txt"}}});
        row("current-filter-type", {{"current_filter", true}});
        row("filter-rule-type", {{"filters", filtersValue({{"Unknown", {{2u, "*.txt"}}}})}});
        row("filter-empty-rules", {{"filters", filtersValue({{"Empty", {}}})}});
        row("filter-empty-name", {{"filters", filtersValue({{"", {{0u, "*.txt"}}}})}});
        row("filter-empty-pattern", {{"filters", filtersValue({{"Empty", {{0u, ""}}}})}});
        row("filter-mime-syntax", {{"filters", filtersValue({{"Bad", {{1u, "image/**"}}}})}});
        row("filter-too-many", {{"filters", filtersValue(PortalFilters(65, {"Text", {{0u, "*.txt"}}}))}});
        row("filter-too-many-rules", {{"filters", filtersValue({{"Text", QList<PortalFilterRule>(257, {0u, "*.txt"})}})}});
        row("filter-oversized-pattern", {{"filters", filtersValue({{"Text", {{0u, QString(257, 'x')}}}})}});
        row("current-filter-not-exact", {{"filters", filtersValue({{"Text", {{0u, "*.txt"}}}})},
            {"current_filter", QVariant::fromValue(PortalFilter{"Text", {{0u, "*.TXT"}}})}});
        row("choice-type", {{"choices", QStringList{"utf8"}}});
        row("choice-empty-id", {{"choices", choicesValue({{"", "Label", {}, ""}})}});
        row("choice-empty-label", {{"choices", choicesValue({{"a", "", {}, ""}})}});
        row("choice-duplicate", {{"choices", choicesValue({{"a", "A", {}, ""}, {"a", "B", {}, ""}})}});
        row("choice-bad-bool", {{"choices", choicesValue({{"a", "A", {}, "yes"}})}});
        row("choice-unknown-selected", {{"choices", choicesValue({{"a", "A", {{"b", "B"}}, "c"}})}});
        row("choice-duplicate-option", {{"choices", choicesValue({{"a", "A", {{"b", "B"}, {"b", "Again"}}, ""}})}});
        row("choice-too-many", {{"choices", choicesValue(PortalChoices(65, {"a", "A", {}, ""}))}});
        row("choice-too-many-options", {{"choices", choicesValue({{"a", "A", QList<PortalChoiceOption>(257, {"a", "A"}), ""}})}});
        row("batch-missing", {}, "SaveFiles");
        row("batch-wrong-type", {{"files", QStringList{"leaf.txt"}}}, "SaveFiles");
        row("batch-empty", {{"files", namesValue({})}}, "SaveFiles");
        row("batch-empty-name", {{"files", namesValue({QByteArray(1, '\0')})}}, "SaveFiles");
        row("batch-no-terminator", {{"files", namesValue({"leaf.txt"})}}, "SaveFiles");
        row("batch-parent", {{"files", namesValue({QByteArray("../x\0", 5)})}}, "SaveFiles");
        row("batch-uri", {{"files", namesValue({QByteArray("file:///tmp/x\0", 14)})}}, "SaveFiles");
        row("batch-dotdot", {{"files", namesValue({QByteArray("..\0", 3)})}}, "SaveFiles");
        row("batch-nul", {{"files", namesValue({QByteArray("a\0b\0", 4)})}}, "SaveFiles");
        row("batch-too-many", {{"files", namesValue(PortalFileNames(4097, QByteArray("a\0", 2)))}}, "SaveFiles");
    }
    void invalidOptions()
    {
        QFETCH(QString, method);
        QFETCH(QVariantMap, options);
        const Pending pending = begin(method, options);
        QTRY_VERIFY(pending.watcher->isFinished());
        QCOMPARE(pending.watcher->reply().type(), QDBusMessage::ReplyMessage);
        QCOMPARE(pending.status(), 2u);
        QVERIFY(pending.results().isEmpty());
        QCOMPARE(m_service->requestCount(), 0);
    }
    void stringsWithNulAreRejectedBeforeMarshalling()
    {
        // D-Bus strings cannot carry NUL (Qt truncates them on the wire). Test
        // the converter directly, not an impossible malformed wire request.
        const auto config = portalChooserOptions(ChooserMode::SaveFile, {}, {},
            {{"current_name", QString(QChar::Null)}});
        QVERIFY(!config.error.isEmpty());
    }
    void parentMismatchAndUniqueHandle()
    {
        if (QGuiApplication::platformName() == QLatin1String("offscreen")) {
            for (const auto &parent : {QStringLiteral("x11:123"), QStringLiteral("wayland:handle"), QStringLiteral("bogus:123")}) {
                const Pending pending = begin("OpenFile", {}, {}, parent);
                QTRY_VERIFY(pending.watcher->isFinished());
                QCOMPARE(pending.status(), 2u);
            }
        }
        for (const auto &reserved : {QStringLiteral("/"), rootPath}) {
            const Pending bad = begin("OpenFile", {}, {}, {}, reserved);
            QTRY_VERIFY(bad.watcher->isFinished());
            QCOMPARE(bad.status(), 2u);
        }
        // The protocol accepts a supplied object path, not just today's
        // frontend request-prefix implementation detail.
        const Pending nonstandard = begin("OpenFile", {}, {}, {}, "/a_portal_request/valid_handle");
        QTRY_VERIFY(m_service->dialogForRequest(nonstandard.path));
        close(nonstandard);
        QTRY_VERIFY(nonstandard.watcher->isFinished());
        QCOMPARE(nonstandard.status(), 1u);
        const QString occupied = QStringLiteral("/occupied_object");
        // The virtual subtree must not allow another export to replace it.
        QVERIFY(!m_bus.registerObject(occupied, this));
        const Pending first = begin();
        QTRY_VERIFY(m_service->dialogForRequest(first.path));
        const Pending duplicate = begin("SaveFile", {}, "org.example.Other", {}, first.path);
        QTRY_VERIFY(duplicate.watcher->isFinished());
        QCOMPARE(duplicate.status(), 2u);
        QVERIFY(!first.watcher->isFinished());
        close(first);
    }
    void concurrentAppSelectionsAreIndependent()
    {
        QTemporaryDir dir;
        const QString special = QString::fromUtf8("report:2026 a space #% é.txt");
        writeFile(dir.filePath(special));
        const QVariantMap options{{"current_folder", pathBytes(dir.path())}, {"future_backend_option", QStringLiteral("ignored")}};
        const Pending first = begin("OpenFile", options, "org.example.First");
        const Pending second = begin("SaveFile", {{"current_folder", pathBytes(dir.path())}, {"current_name", "second.txt"}}, "org.example.Second");
        QTRY_VERIFY(m_service->dialogForRequest(first.path));
        QTRY_VERIFY(m_service->dialogForRequest(second.path));
        auto *dialog = m_service->dialogForRequest(first.path);
        QVERIFY(dialog != m_service->dialogForRequest(second.path));
        dialog->findChild<QLineEdit *>("chooserFileName")->setText(special);
        QVERIFY(acceptButton(dialog));
        QTest::mouseClick(acceptButton(dialog), Qt::LeftButton);
        QTRY_VERIFY(first.watcher->isFinished());
        QCOMPARE(first.status(), 0u);
        QCOMPARE(first.results().value("uris").toStringList(), QStringList{QUrl::fromLocalFile(dir.filePath(special)).toString(QUrl::FullyEncoded)});
        QVERIFY(!second.watcher->isFinished());
        auto *save = m_service->dialogForRequest(second.path);
        QTest::mouseClick(acceptButton(save), Qt::LeftButton);
        QTRY_VERIFY(second.watcher->isFinished());
        QCOMPARE(second.status(), 0u);
        QCOMPARE(second.results().value("uris").toStringList(), QStringList{QUrl::fromLocalFile(dir.filePath("second.txt")).toString(QUrl::FullyEncoded)});
        QVERIFY(!QFile::exists(dir.filePath("second.txt")));
        QCOMPARE(m_service->requestCount(), 0);
    }
    void openModes_data()
    {
        QTest::addColumn<bool>("multiple");
        QTest::addColumn<bool>("directory");
        QTest::newRow("file") << false << false;
        QTest::newRow("files") << true << false;
        QTest::newRow("folder") << false << true;
        QTest::newRow("folders") << true << true;
    }
    void openModes()
    {
        QFETCH(bool, multiple);
        QFETCH(bool, directory);
        QTemporaryDir dir;
        const QStringList leaves{QString::fromUtf8("a space #% é"), QString::fromUtf8("日本語")};
        QList<QUrl> urls;
        for (const auto &leaf : leaves) {
            if (directory)
                QVERIFY(QDir(dir.path()).mkdir(leaf));
            else
                writeFile(dir.filePath(leaf));
            urls.append(QUrl::fromLocalFile(dir.filePath(leaf)));
        }
        if (!multiple)
            urls.removeLast();
        const Pending pending = begin("OpenFile", {{"current_folder", pathBytes(dir.path())}, {"multiple", multiple}, {"directory", directory}});
        QTRY_VERIFY(m_service->dialogForRequest(pending.path));
        auto *dialog = m_service->dialogForRequest(pending.path);
        auto *model = dialog->findChild<DirectoryModel *>();
        QTRY_COMPARE(model->rowCount(), 2);
        QVERIFY(dialog->findChild<FileView *>()->selectUrls(urls).isEmpty());
        QTest::mouseClick(acceptButton(dialog), Qt::LeftButton);
        QTRY_VERIFY(pending.watcher->isFinished());
        QCOMPARE(pending.status(), 0u);
        QStringList expected;
        for (const auto &url : urls)
            expected.append(url.toString(QUrl::FullyEncoded));
        auto actual = pending.results().value("uris").toStringList();
        actual.sort(); expected.sort();
        QCOMPARE(actual, expected);
    }
    void folderCanAcceptCurrentLocation()
    {
        QTemporaryDir dir;
        const Pending pending = begin("OpenFile", {{"current_folder", pathBytes(dir.path())}, {"directory", true}});
        QTRY_VERIFY(m_service->dialogForRequest(pending.path));
        QTest::mouseClick(acceptButton(m_service->dialogForRequest(pending.path)), Qt::LeftButton);
        QTRY_VERIFY(pending.watcher->isFinished());
        QCOMPARE(pending.status(), 0u);
        QCOMPARE(pending.results().value("uris").toStringList(), QStringList{QUrl::fromLocalFile(dir.path()).toString(QUrl::FullyEncoded)});
    }
    void filtersAreSensitiveOrMimeAndPreserveExactTuple()
    {
        QTemporaryDir dir;
        writeFile(dir.filePath("lower.txt"));
        writeFile(dir.filePath("upper.TXT"));
        writeFile(dir.filePath("sound.wav"), QByteArray("RIFF\x24\0\0\0WAVEfmt ", 16));
        const PortalFilter textFilter{"Same display name", {{0u, "*.txt"}}};
        const PortalFilter mixed{"Same display name", {{0u, "*.[tT][xX][tT]"}, {1u, "audio/*"}}};
        const Pending pending = begin("OpenFile", {{"current_folder", pathBytes(dir.path())},
            {"filters", filtersValue({textFilter, mixed})}, {"current_filter", QVariant::fromValue(textFilter)}});
        QTRY_VERIFY(m_service->dialogForRequest(pending.path));
        auto *dialog = m_service->dialogForRequest(pending.path);
        auto *model = dialog->findChild<DirectoryModel *>();
        QTRY_COMPARE(model->rowCount(), 1);
        QVERIFY(model->indexForUrl(QUrl::fromLocalFile(dir.filePath("lower.txt"))).isValid());
        QVERIFY(!model->indexForUrl(QUrl::fromLocalFile(dir.filePath("upper.TXT"))).isValid());
        dialog->findChild<QComboBox *>("chooserFileType")->setCurrentIndex(1);
        QTRY_COMPARE(model->rowCount(), 3);
        dialog->findChild<QLineEdit *>("chooserFileName")->setText("upper.TXT");
        QTest::mouseClick(acceptButton(dialog), Qt::LeftButton);
        QTRY_VERIFY(pending.watcher->isFinished());
        QCOMPARE(pending.status(), 0u);
        const auto selected = qdbus_cast<PortalFilter>(pending.results().value("current_filter"));
        QCOMPARE(selected.name, mixed.name);
        QCOMPARE(selected.rules.size(), mixed.rules.size());
        for (int i = 0; i < selected.rules.size(); ++i) {
            QCOMPARE(selected.rules.at(i).type, mixed.rules.at(i).type);
            QCOMPARE(selected.rules.at(i).value, mixed.rules.at(i).value);
        }
    }
    void unconditionalFilterAndSaveExtension()
    {
        QTemporaryDir dir;
        const PortalFilter filter{"Text", {{0u, "*.txt"}, {1u, "text/plain"}}};
        const Pending pending = begin("SaveFile", {{"current_folder", pathBytes(dir.path())}, {"current_name", "new"},
            {"current_filter", QVariant::fromValue(filter)}, {"filters", filtersValue({})}});
        QTRY_VERIFY(m_service->dialogForRequest(pending.path));
        QTest::mouseClick(acceptButton(m_service->dialogForRequest(pending.path)), Qt::LeftButton);
        QTRY_VERIFY(pending.watcher->isFinished());
        QCOMPARE(pending.status(), 0u);
        QCOMPARE(pending.results().value("uris").toStringList(), QStringList{QUrl::fromLocalFile(dir.filePath("new.txt")).toString(QUrl::FullyEncoded)});
        const auto selected = qdbus_cast<PortalFilter>(pending.results().value("current_filter"));
        QCOMPARE(selected.name, filter.name);
        QCOMPARE(selected.rules.size(), 2);
        QVERIFY(!QFile::exists(dir.filePath("new.txt")));
    }
    void choicesDefaultsChangesAndMnemonic_data()
    {
        QTest::addColumn<bool>("change");
        QTest::newRow("defaults") << false;
        QTest::newRow("changed") << true;
    }
    void choicesDefaultsChangesAndMnemonic()
    {
        QFETCH(bool, change);
        QTemporaryDir dir;
        const PortalChoices choices{{"encoding", "Encoding & format", {{"utf8", "Unicode"}, {"latin15", "Western"}}, ""},
            {"reencode", "Reencode", {}, ""}, {"preset", "Preset", {{"a", "A"}, {"b", "B"}}, "b"},
            {"enabled", "Enabled", {}, "true"}};
        const Pending pending = begin("SaveFile", {{"current_folder", pathBytes(dir.path())}, {"current_name", "out"},
            {"accept_label", "_Save & Keep__Copy"}, {"choices", choicesValue(choices)}});
        QTRY_VERIFY(m_service->dialogForRequest(pending.path));
        auto *dialog = m_service->dialogForRequest(pending.path);
        QCOMPARE(acceptButton(dialog)->text(), QStringLiteral("&Save && Keep_Copy"));
        QCOMPARE(dialog->selectedChoices().value("encoding"), QStringLiteral("utf8"));
        QCOMPARE(dialog->selectedChoices().value("reencode"), QStringLiteral("false"));
        QCOMPARE(dialog->selectedChoices().value("preset"), QStringLiteral("b"));
        QCOMPARE(dialog->selectedChoices().value("enabled"), QStringLiteral("true"));
        if (change) {
            for (auto *combo : dialog->findChildren<QComboBox *>()) {
                if (combo->findData("latin15") >= 0)
                    combo->setCurrentIndex(combo->findData("latin15"));
            }
            for (auto *check : dialog->findChildren<QCheckBox *>()) {
                if (check->text() == QLatin1String("Reencode"))
                    check->setChecked(true);
            }
        }
        QTest::mouseClick(acceptButton(dialog), Qt::LeftButton);
        QTRY_VERIFY(pending.watcher->isFinished());
        QCOMPARE(pending.status(), 0u);
        const auto selected = qdbus_cast<PortalChoiceResults>(pending.results().value("choices"));
        QCOMPARE(selected.size(), 4);
        QCOMPARE(selected.at(0).id, QStringLiteral("encoding"));
        QCOMPARE(selected.at(0).selected, change ? QStringLiteral("latin15") : QStringLiteral("utf8"));
        QCOMPARE(selected.at(1).id, QStringLiteral("reencode"));
        QCOMPARE(selected.at(1).selected, change ? QStringLiteral("true") : QStringLiteral("false"));
    }
    void choiceLabelsArePlainTextNotImageMarkup()
    {
        QTemporaryDir dir;
        const QString caption = QStringLiteral("<b>Literal</b> <img src=\"file://%1/image.png\">").arg(dir.path());
        const Pending pending = begin("SaveFile", {{"current_folder", pathBytes(dir.path())}, {"current_name", "out.txt"},
            {"choices", choicesValue({{"markup", caption, {{"a", "A"}}, "a"}})}});
        QTRY_VERIFY(m_service->dialogForRequest(pending.path));
        auto *dialog = m_service->dialogForRequest(pending.path);
        auto *area = dialog->findChild<QWidget *>("chooserChoices");
        QVERIFY(area);
        const auto labels = area->findChildren<QLabel *>();
        QCOMPARE(labels.size(), 1);
        QCOMPARE(labels.first()->text(), caption);
        // PlainText forbids QTextDocument image/rich-text interpretation; the
        // original caption is retained instead of stripping/escaping markup.
        QCOMPARE(labels.first()->textFormat(), Qt::PlainText);
        QTest::mouseClick(acceptButton(dialog), Qt::LeftButton);
        QTRY_VERIFY(pending.watcher->isFinished());
        QCOMPARE(pending.status(), 0u);
        const auto selected = qdbus_cast<PortalChoiceResults>(pending.results().value("choices"));
        QCOMPARE(selected.size(), 1);
        QCOMPARE(selected.first().selected, QStringLiteral("a"));
    }
    void repeatedCloseAndHandleReuseAreIdentitySafe()
    {
        const QString path = QStringLiteral("/reusable_portal_request");
        const Pending old = begin("OpenFile", {}, {}, {}, path);
        QTRY_VERIFY(m_service->dialogForRequest(path));
        auto message = QDBusMessage::createMethodCall(backend, path, requestInterface, "Close");
        QDBusPendingCallWatcher close1(m_frontend.asyncCall(message, 5000));
        QDBusPendingCallWatcher close2(m_frontend.asyncCall(message, 5000));
        QTRY_VERIFY(old.watcher->isFinished());
        QCOMPARE(old.status(), 1u);
        const Pending current = begin("SaveFile", {}, {}, {}, path);
        QTRY_VERIFY(m_service->dialogForRequest(path));
        QTRY_VERIFY(close1.isFinished());
        QTRY_VERIFY(close2.isFinished());
        QCOMPARE(close1.reply().type(), QDBusMessage::ReplyMessage);
        QCOMPARE(close2.reply().type(), QDBusMessage::ReplyMessage);
        QVERIFY(!current.watcher->isFinished());
        QVERIFY(m_service->dialogForRequest(path)->isVisible());
        close(current);
        QTRY_VERIFY(current.watcher->isFinished());
        QCOMPARE(current.status(), 1u);
    }
    void saveSuggestionPriorityAndEmptySuggestions()
    {
        QTemporaryDir dir;
        QVERIFY(QDir(dir.path()).mkdir("existing-folder"));
        QVERIFY(QDir(dir.path()).mkdir("other-folder"));
        const QString file = dir.filePath("existing-folder/existing.txt");
        writeFile(file);
        const Pending pending = begin("SaveFile", {{"current_folder", pathBytes(dir.filePath("other-folder"))},
            {"current_file", pathBytes(file)}, {"current_name", "replacement.txt"}});
        QTRY_VERIFY(m_service->dialogForRequest(pending.path));
        auto *dialog = m_service->dialogForRequest(pending.path);
        QCOMPARE(dialog->currentUrl(), QUrl::fromLocalFile(dir.filePath("existing-folder")));
        QCOMPARE(dialog->findChild<QLineEdit *>("chooserFileName")->text(), QStringLiteral("replacement.txt"));
        QTest::mouseClick(acceptButton(dialog), Qt::LeftButton);
        QTRY_VERIFY(pending.watcher->isFinished());
        QCOMPARE(pending.status(), 0u);
        QCOMPARE(pending.results().value("uris").toStringList(), QStringList{QUrl::fromLocalFile(dir.filePath("existing-folder/replacement.txt")).toString(QUrl::FullyEncoded)});
        const Pending empty = begin("SaveFile", {{"current_folder", QByteArray(1, '\0')}, {"current_file", QByteArray(1, '\0')}, {"current_name", ""}});
        QTRY_VERIFY(m_service->dialogForRequest(empty.path));
        QVERIFY(!empty.watcher->isFinished());
        QCOMPARE(m_service->dialogForRequest(empty.path)->currentUrl(), QUrl::fromLocalFile(QDir::homePath()));
        close(empty);
        const Pending retainedName = begin("SaveFile", {{"current_file", pathBytes(file)}, {"current_name", ""}});
        QTRY_VERIFY(m_service->dialogForRequest(retainedName.path));
        QCOMPARE(m_service->dialogForRequest(retainedName.path)->findChild<QLineEdit *>("chooserFileName")->text(), QStringLiteral("existing.txt"));
        close(retainedName);
    }
    void saveOverwriteCloseNeverWrites()
    {
        QTemporaryDir dir;
        const QString file = dir.filePath("<b>keep me.txt");
        writeFile(file);
        const Pending pending = begin("SaveFile", {{"current_file", pathBytes(file)}});
        QTRY_VERIFY(m_service->dialogForRequest(pending.path));
        QPointer<FileChooserDialog> dialog = m_service->dialogForRequest(pending.path);
        QTest::mouseClick(acceptButton(dialog), Qt::LeftButton);
        QTRY_VERIFY(dialog->findChild<QMessageBox *>("chooserOverwrite"));
        QPointer<QMessageBox> confirmation = dialog->findChild<QMessageBox *>("chooserOverwrite");
        QCOMPARE(confirmation->textFormat(), Qt::PlainText);
        QVERIFY(confirmation->text().startsWith(QStringLiteral("<b>keep me.txt already exists.")));
        QCOMPARE(confirmation->defaultButton(), confirmation->button(QMessageBox::No));
        QVERIFY(!pending.watcher->isFinished());
        QTest::mouseClick(confirmation->button(QMessageBox::No), Qt::LeftButton);
        QTRY_VERIFY(confirmation.isNull());
        QVERIFY(!pending.watcher->isFinished());
        { QFile original(file); QVERIFY(original.open(QIODevice::ReadOnly)); QCOMPARE(original.readAll(), QByteArray("untouched")); }
        QTest::mouseClick(acceptButton(dialog), Qt::LeftButton);
        QTRY_VERIFY(dialog->findChild<QMessageBox *>("chooserOverwrite"));
        QVERIFY(!pending.watcher->isFinished());
        close(pending);
        QTRY_VERIFY(pending.watcher->isFinished());
        QCOMPARE(pending.status(), 1u);
        QTRY_VERIFY(dialog.isNull());
        QFile original(file);
        QVERIFY(original.open(QIODevice::ReadOnly));
        QCOMPARE(original.readAll(), QByteArray("untouched"));
    }
    void batchNamesCollisionsOrderAndNoWrites()
    {
        QTemporaryDir dir;
        writeFile(dir.filePath("a.txt"));
        QVERIFY(QFile::link(dir.filePath("missing"), dir.filePath("broken.txt")));
        const QString special = QString::fromUtf8("space #% é.txt");
        const PortalFileNames names{pathBytes("a.txt"), pathBytes("a.txt"), pathBytes("a (1).txt"), pathBytes("broken.txt"), pathBytes(special)};
        const Pending pending = begin("SaveFiles", {{"current_folder", pathBytes(dir.path())}, {"files", namesValue(names)},
            {"choices", choicesValue({{"flag", "Flag", {}, ""}})}});
        QTRY_VERIFY(m_service->dialogForRequest(pending.path));
        QTest::mouseClick(acceptButton(m_service->dialogForRequest(pending.path)), Qt::LeftButton);
        QTRY_VERIFY(pending.watcher->isFinished());
        QCOMPARE(pending.status(), 0u);
        const QStringList leaves{"a (1).txt", "a (2).txt", "a (1) (1).txt", "broken (1).txt", special};
        QStringList expected;
        for (const auto &leaf : leaves) {
            expected.append(QUrl::fromLocalFile(dir.filePath(leaf)).toString(QUrl::FullyEncoded));
            QVERIFY(!QFileInfo::exists(dir.filePath(leaf)));
        }
        QCOMPARE(pending.results().value("uris").toStringList(), expected);
        const auto choices = qdbus_cast<PortalChoiceResults>(pending.results().value("choices"));
        QCOMPARE(choices.size(), 1);
        QCOMPARE(choices.first().selected, QStringLiteral("false"));
        QCOMPARE(QDir(dir.path()).entryList(QDir::Files | QDir::System | QDir::NoDotAndDotDot).size(), 2);
    }
    void frontendDisconnectAndOwnerReplacementCleanUp()
    {
        const Pending first = begin();
        const Pending second = begin("SaveFile");
        QTRY_VERIFY(m_service->dialogForRequest(first.path));
        QTRY_VERIFY(m_service->dialogForRequest(second.path));
        QPointer<FileChooserDialog> firstDialog = m_service->dialogForRequest(first.path);
        QPointer<FileChooserDialog> secondDialog = m_service->dialogForRequest(second.path);
        QVERIFY(m_frontend.unregisterService(frontend));
        QTRY_COMPARE(m_service->requestCount(), 0);
        QTRY_VERIFY(firstDialog.isNull());
        QTRY_VERIFY(secondDialog.isNull());
        QTRY_VERIFY(first.watcher->isFinished());
        QTRY_VERIFY(second.watcher->isFinished());
        QCOMPARE(first.status(), 1u);
        QCOMPARE(second.status(), 1u);
        QVERIFY(m_app.registerService(frontend));
        const Pending oldOwner = begin();
        QTRY_VERIFY(oldOwner.watcher->isFinished());
        QCOMPARE(oldOwner.watcher->reply().errorName(), QStringLiteral("org.freedesktop.DBus.Error.AccessDenied"));
        const Pending newOwner = begin("OpenFile", {}, {}, {}, {}, false);
        QTRY_VERIFY(m_service->dialogForRequest(newOwner.path));
        QPointer<FileChooserDialog> newDialog = m_service->dialogForRequest(newOwner.path);
        m_app = QDBusConnection(QStringLiteral("invalid"));
        QDBusConnection::disconnectFromBus(QStringLiteral("portal-app-") + m_suffix);
        QTRY_COMPARE(m_service->requestCount(), 0);
        QTRY_VERIFY(newDialog.isNull());
    }
    void executablePersistsAndQuitsOnPrivateBusLoss()
    {
        // A second, disposable bus: never disconnect or test against the desktop.
        QProcess daemon;
        daemon.start(QStringLiteral("dbus-daemon"), {QStringLiteral("--nofork"), QStringLiteral("--print-address=1"),
                     QStringLiteral("--config-file=") + QStringLiteral(PORTAL_TEST_BUS_CONFIG)});
        QVERIFY(daemon.waitForStarted(5000));
        QTRY_VERIFY(daemon.canReadLine());
        const QByteArray address = daemon.readLine().trimmed();
        QVERIFY(address.startsWith("unix:"));
        QProcess process;
        auto env = QProcessEnvironment::systemEnvironment();
        env.insert("DBUS_SESSION_BUS_ADDRESS", QString::fromUtf8(address));
        process.setProcessEnvironment(env);
        process.start(QStringLiteral(PORTAL_BINARY));
        QVERIFY(process.waitForStarted(5000));
        QDBusConnection bus = QDBusConnection::connectToBus(QString::fromUtf8(address), "portal-process-test");
        QVERIFY(bus.isConnected());
        QDBusMessage reply;
        QTRY_VERIFY((reply = call(bus, rootPath, QStringLiteral("org.freedesktop.DBus.Introspectable"), QStringLiteral("Introspect"))).type() == QDBusMessage::ReplyMessage);
        QVERIFY(bus.registerService(frontend));
        auto message = QDBusMessage::createMethodCall(backend, rootPath, chooserInterface, "OpenFile");
        const QString path = rootPath + "/request/1_99/process";
        message.setArguments({QVariant::fromValue(QDBusObjectPath(path)), QStringLiteral("org.example.Process"), QString(), QStringLiteral("Process test"), QVariantMap()});
        QDBusPendingCallWatcher pending(bus.asyncCall(message, 10000));
        QTRY_VERIFY(call(bus, path, QStringLiteral("org.freedesktop.DBus.Introspectable"), QStringLiteral("Introspect")).type() == QDBusMessage::ReplyMessage);
        QVERIFY(!pending.isFinished());
        QCOMPARE(call(bus, path, requestInterface, "Close").type(), QDBusMessage::ReplyMessage);
        QTRY_VERIFY(pending.isFinished());
        QTest::qWait(150);
        QCOMPARE(process.state(), QProcess::Running);
        // Also leave an interaction pending when the bus dies.
        message.setArguments({QVariant::fromValue(QDBusObjectPath(path)), QStringLiteral("org.example.Process"), QString(), QStringLiteral("Bus loss"), QVariantMap()});
        QDBusPendingCallWatcher dying(bus.asyncCall(message, 10000));
        QTRY_VERIFY(call(bus, path, QStringLiteral("org.freedesktop.DBus.Introspectable"), QStringLiteral("Introspect")).type() == QDBusMessage::ReplyMessage);
        daemon.terminate();
        QVERIFY(daemon.waitForFinished(5000));
        QTRY_COMPARE(process.state(), QProcess::NotRunning);
        QCOMPARE(process.exitStatus(), QProcess::NormalExit);
        QCOMPARE(process.exitCode(), 0);
        bus = QDBusConnection(QStringLiteral("invalid"));
        QDBusConnection::disconnectFromBus("portal-process-test");
    }
};

QTEST_MAIN(PortalChooserTest)
#include "PortalChooserTest.moc"
