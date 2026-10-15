// Optional native-protocol smoke test under a PRIVATE, virtual KWin compositor.
// This does not claim physical user-input/activation-token focus acceptance.
#include "PortalDBusTypes.h"
#include <KWaylandExtras>
#include <KWindowSystem>

#include <QApplication>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTest>
#include <QUrl>
#include <QWidget>

namespace {
constexpr auto serviceName = "org.win7explorer.FileChooser1";
constexpr auto servicePath = "/org/win7explorer/FileChooser1";
constexpr auto requestInterface = "org.win7explorer.FileChooser1.Request";

QDBusMessage call(const QString &path, const QString &interface, const QString &method,
                  const QList<QVariant> &arguments = {})
{
    auto message = QDBusMessage::createMethodCall(serviceName, path, interface, method);
    message.setArguments(arguments);
    QDBusPendingCallWatcher watcher(QDBusConnection::sessionBus().asyncCall(message, 10000));
    QEventLoop loop;
    QObject::connect(&watcher, &QDBusPendingCallWatcher::finished, &loop, &QEventLoop::quit);
    if (!watcher.isFinished()) loop.exec();
    return watcher.reply();
}

QString request(const QVariantMap &options)
{
    const auto reply = call(servicePath, serviceName, QStringLiteral("OpenFile"), {options});
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().size() != 1) return {};
    return qvariant_cast<QDBusObjectPath>(reply.arguments().first()).path();
}

QVariantMap result(const QString &path)
{
    const auto reply = call(path, requestInterface, QStringLiteral("GetResult"));
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().size() != 1) return {};
    return qdbus_cast<QVariantMap>(reply.arguments().first());
}
} // namespace

class ChooserWaylandTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(KWindowSystem::isPlatformWayland());
        const QString binary = qEnvironmentVariable("EXPLORER_CHOOSER_BINARY");
        m_trace = qEnvironmentVariable("EXPLORER_CHOOSER_WAYLAND_TRACE");
        QVERIFY(!binary.isEmpty());
        QVERIFY(!m_trace.isEmpty());
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("WAYLAND_DEBUG"), QStringLiteral("client"));
        environment.remove(QStringLiteral("XDG_ACTIVATION_TOKEN"));
        environment.remove(QStringLiteral("DESKTOP_STARTUP_ID"));
        m_service.setProcessEnvironment(environment);
        m_service.setStandardErrorFile(m_trace, QIODevice::Truncate);
        m_service.setStandardOutputFile(QProcess::nullDevice());
        m_service.start(binary);
        QVERIFY(m_service.waitForStarted(5000));
        QTRY_VERIFY_WITH_TIMEOUT(QDBusConnection::sessionBus().interface()->isServiceRegistered(serviceName).value(), 10000);
        m_parent.setWindowTitle(QStringLiteral("Isolated Explorer chooser parent test"));
        m_parent.resize(400, 200);
        m_parent.show();
        m_parent.winId();
        connect(KWaylandExtras::self(), &KWaylandExtras::windowExported, this,
                [this](QWindow *window, const QString &handle) {
            if (window == m_parent.windowHandle()) m_handle = handle;
        });
        KWaylandExtras::exportWindow(m_parent.windowHandle());
        QTRY_VERIFY_WITH_TIMEOUT(!m_handle.isEmpty(), 5000);
    }

    void realForeignParentAndNativeDialogDecorations()
    {
        const QString path = request({
            {QStringLiteral("parent_window"), QStringLiteral("wayland:") + m_handle},
            {QStringLiteral("modal"), true},
            {QStringLiteral("starting_location"), QUrl::fromLocalFile(QDir::homePath()).toString(QUrl::FullyEncoded)}
        });
        QVERIFY(!path.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(result(path).value(QStringLiteral("state")).toString(), QStringLiteral("pending"), 5000);
        // Observe the REAL separate service's protocol, not an in-process mock.
        QTRY_VERIFY_WITH_TIMEOUT(trace().contains(".import_toplevel("), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(trace().contains(".set_parent_of("), 5000);
        QVERIFY(trace().contains(m_handle.toUtf8()));
        QTRY_VERIFY_WITH_TIMEOUT(trace().contains(".set_app_id(\"linux-explorer-win7-dialog\")"), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(trace().contains(".get_toplevel_decoration("), 5000);
        // KWin/Qt use xdg-dialog for real modal metadata on this native backend.
        QTRY_VERIFY_WITH_TIMEOUT(trace().contains(".set_modal("), 5000);
        QCOMPARE(call(path, requestInterface, QStringLiteral("Cancel")).type(), QDBusMessage::ReplyMessage);
        QTRY_COMPARE(result(path).value(QStringLiteral("state")).toString(), QStringLiteral("canceled"));
        QCOMPARE(call(path, requestInterface, QStringLiteral("Close")).type(), QDBusMessage::ReplyMessage);
    }

    void backendMismatchFailsInsteadOfUnparentedFallback()
    {
        const QString path = request({{QStringLiteral("parent_window"), QStringLiteral("x11:1234")},
                                      {QStringLiteral("modal"), true}});
        QVERIFY(!path.isEmpty());
        QTRY_COMPARE(result(path).value(QStringLiteral("state")).toString(), QStringLiteral("failed"));
        QCOMPARE(result(path).value(QStringLiteral("status")).toUInt(), 2u);
        QCOMPARE(call(path, requestInterface, QStringLiteral("Close")).type(), QDBusMessage::ReplyMessage);
    }

    void portalForeignParentModalAndDecorations()
    {
        // This test owns the frontend identity only on the virtual session bus.
        // Public frontend routing is covered separately by PortalFrontendTest.
        registerPortalDBusTypes();
        auto bus = QDBusConnection::sessionBus();
        QVERIFY(bus.registerService(QStringLiteral("org.freedesktop.portal.Desktop")));
        m_portalTrace = m_trace + QStringLiteral(".portal");
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("WAYLAND_DEBUG"), QStringLiteral("client"));
        m_portal.setProcessEnvironment(environment);
        m_portal.setStandardErrorFile(m_portalTrace);
        m_portal.setStandardOutputFile(QProcess::nullDevice());
        m_portal.start(qEnvironmentVariable("EXPLORER_PORTAL_BINARY"));
        QVERIFY(m_portal.waitForStarted(5000));
        const QString backend = QStringLiteral("org.freedesktop.impl.portal.desktop.explorerwin7");
        QTRY_VERIFY_WITH_TIMEOUT(bus.interface()->isServiceRegistered(backend).value(), 10000);
        const QString path = QStringLiteral("/org/freedesktop/portal/desktop/request/virtual/parent");
        auto message = QDBusMessage::createMethodCall(backend, QStringLiteral("/org/freedesktop/portal/desktop"),
            QStringLiteral("org.freedesktop.impl.portal.FileChooser"), QStringLiteral("OpenFile"));
        message.setArguments({QVariant::fromValue(QDBusObjectPath(path)), QStringLiteral("org.example.Virtual"),
            QStringLiteral("wayland:") + m_handle, QStringLiteral("Virtual portal chooser"), QVariantMap{{QStringLiteral("modal"), true}}});
        QDBusPendingCallWatcher pending(bus.asyncCall(message, 10000));
        QTRY_VERIFY_WITH_TIMEOUT(portalTrace().contains(".import_toplevel("), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(portalTrace().contains(".set_parent_of("), 5000);
        QVERIFY(portalTrace().contains(m_handle.toUtf8()));
        QTRY_VERIFY(portalTrace().contains(".set_app_id(\"linux-explorer-win7-dialog\")"));
        QTRY_VERIFY(portalTrace().contains(".get_toplevel_decoration("));
        QTRY_VERIFY(portalTrace().contains(".set_modal("));
        QVERIFY(!pending.isFinished());
        auto close = QDBusMessage::createMethodCall(backend, path,
            QStringLiteral("org.freedesktop.impl.portal.Request"), QStringLiteral("Close"));
        QDBusPendingCallWatcher closed(bus.asyncCall(close, 5000));
        QTRY_VERIFY(closed.isFinished());
        QCOMPARE(closed.reply().type(), QDBusMessage::ReplyMessage);
        QTRY_VERIFY(pending.isFinished());
        QCOMPARE(pending.reply().arguments().first().toUInt(), 1u);
        QVERIFY(bus.unregisterService(QStringLiteral("org.freedesktop.portal.Desktop")));
    }

    void cleanupTestCase()
    {
        if (!m_handle.isEmpty()) KWaylandExtras::unexportWindow(m_parent.windowHandle());
        m_parent.hide();
        m_portal.terminate();
        if (m_portal.state() != QProcess::NotRunning && !m_portal.waitForFinished(5000)) {
            m_portal.kill(); m_portal.waitForFinished(5000);
        }
        // Only this test's QProcess child, on this test's bus/compositor.
        m_service.terminate();
        if (!m_service.waitForFinished(5000)) {
            m_service.kill();
            m_service.waitForFinished(5000);
        }
    }

private:
    QByteArray trace() const
    {
        QFile file(m_trace);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }
    QByteArray portalTrace() const
    {
        QFile file(m_portalTrace);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }
    QProcess m_portal;
    QString m_portalTrace;
    QProcess m_service;
    QWidget m_parent;
    QString m_handle;
    QString m_trace;
};

QTEST_MAIN(ChooserWaylandTest)
#include "ChooserWaylandTest.moc"
