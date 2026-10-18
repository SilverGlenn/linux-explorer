// Real Xwayland caller against a native Wayland portal backend. Run only on
// tests/run-portal-mixed-display-test.sh's private compositor, bus and runtime.
#include "PortalDBusTypes.h"

#include <QApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>
#include <QWidget>
#include <memory>
#include <signal.h>

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>

namespace {
const QString backend = QStringLiteral("org.freedesktop.impl.portal.desktop.explorerwin7");
const QString frontend = QStringLiteral("org.freedesktop.portal.Desktop");
const QString chooserInterface = QStringLiteral("org.freedesktop.impl.portal.FileChooser");
const QString requestInterface = QStringLiteral("org.freedesktop.impl.portal.Request");

QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// All properties come from the actual native client windows, never UI mocks.
QByteArray bytesProperty(Display *display, Window window, const char *name)
{
    Atom actualType;
    int actualFormat;
    unsigned long count = 0, remaining = 0;
    unsigned char *data = nullptr;
    const int status = XGetWindowProperty(display, window, XInternAtom(display, name, False),
        0, 4096, False, AnyPropertyType, &actualType, &actualFormat, &count, &remaining, &data);
    QByteArray result;
    if (status == Success && data && actualFormat == 8)
        result = QByteArray(reinterpret_cast<const char *>(data), count);
    if (data) XFree(data);
    return result;
}

QList<unsigned long> cardinalProperty(Display *display, Window window, const char *name)
{
    Atom actualType;
    int actualFormat;
    unsigned long count = 0, remaining = 0;
    unsigned char *data = nullptr;
    const int status = XGetWindowProperty(display, window, XInternAtom(display, name, False),
        0, 4096, False, AnyPropertyType, &actualType, &actualFormat, &count, &remaining, &data);
    QList<unsigned long> result;
    if (status == Success && data && actualFormat == 32) {
        const auto *values = reinterpret_cast<unsigned long *>(data);
        for (unsigned long i = 0; i < count; ++i) result.append(values[i]);
    }
    if (data) XFree(data);
    return result;
}

// KWin may destroy a window between enumeration and property lookup.
int ignoreVanishedWindow(Display *, XErrorEvent *) { return 0; }
} // namespace

class PortalMixedDisplayTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void initTestCase()
    {
        QCOMPARE(QGuiApplication::platformName(), QStringLiteral("xcb"));
        QVERIFY(!qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY"));
        registerPortalDBusTypes();
        m_display = XOpenDisplay(nullptr);
        QVERIFY(m_display);
        m_previousErrorHandler = XSetErrorHandler(ignoreVanishedWindow);
        m_parent.setWindowTitle(QStringLiteral("Private Xwayland portal caller"));
        m_parent.resize(400, 200);
        m_parent.show();
        QVERIFY(QTest::qWaitForWindowExposed(&m_parent));
        m_parentId = m_parent.winId();
        QVERIFY(m_parentId);
        QVERIFY(m_data.isValid());
    }

    void init()
    {
        m_connectionName = QStringLiteral("mixed-display-frontend-%1").arg(++m_serial);
        m_bus = std::make_unique<QDBusConnection>(QDBusConnection::connectToBus(
            QDBusConnection::SessionBus, m_connectionName));
        QVERIFY(m_bus->isConnected());
        QVERIFY(m_bus->registerService(frontend));
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("wayland"));
        environment.insert(QStringLiteral("WAYLAND_DEBUG"), QStringLiteral("client"));
        environment.remove(QStringLiteral("XDG_ACTIVATION_TOKEN"));
        environment.remove(QStringLiteral("DESKTOP_STARTUP_ID"));
        const QString tracePrefix = qEnvironmentVariable("EXPLORER_PORTAL_MIXED_TRACE");
        m_tracePath = tracePrefix.isEmpty()
            ? m_data.filePath(QStringLiteral("backend-%1.log").arg(m_serial))
            : tracePrefix + QStringLiteral("-%1.log").arg(m_serial);
        m_portal.setProcessEnvironment(environment);
        m_portal.setStandardErrorFile(m_tracePath, QIODevice::Truncate);
        m_portal.setStandardOutputFile(QProcess::nullDevice());
        m_portal.start(qEnvironmentVariable("EXPLORER_PORTAL_BINARY"));
        QVERIFY(m_portal.waitForStarted(5000));
        QTRY_VERIFY_WITH_TIMEOUT(QDBusConnection::sessionBus().interface()->isServiceRegistered(backend).value(), 10000);
        // QT_QPA_PLATFORM=wayland was consumed by the actual backend process.
        QTRY_VERIFY_WITH_TIMEOUT(readFile(m_tracePath).contains(".get_registry("), 5000);
    }

    void x11ParentFormats_data()
    {
        QTest::addColumn<bool>("prefix");
        QTest::newRow("hexadecimal-without-prefix") << false;
        QTest::newRow("chatgpt-optional-0x-prefix") << true;
    }

    void x11ParentFormats()
    {
        QFETCH(bool, prefix);
        const QString title = QStringLiteral("Add folder with %1").arg(prefix ? "0x" : "plain hex");
        QString path;
        auto pending = open(title, directoryOptions(), prefix, &path);
        Window window = 0;
        QTRY_VERIFY_WITH_TIMEOUT((window = findWindow(title)) != 0, 10000);
        verifyChild(window, title);
        QVERIFY(!QTest::currentTestFailed());
        QVERIFY(!pending->isFinished());
        auto closed = close(path);
        QTRY_VERIFY_WITH_TIMEOUT(closed->isFinished(), 5000);
        QCOMPARE(closed->reply().type(), QDBusMessage::ReplyMessage);
        QTRY_VERIFY_WITH_TIMEOUT(pending->isFinished(), 5000);
        QCOMPARE(response(*pending), 1u);
        QTRY_VERIFY_WITH_TIMEOUT(findWindow(title) == 0, 5000);
        verifyBackendAlive();
    }

    void closingOneConcurrentRequestPreservesOther()
    {
        const QString titleA = QStringLiteral("Concurrent mixed chooser A");
        const QString titleB = QStringLiteral("Concurrent mixed chooser B");
        QString pathA, pathB;
        auto first = open(titleA, directoryOptions(), true, &pathA);
        auto second = open(titleB, directoryOptions(), false, &pathB);
        Window windowA = 0, windowB = 0;
        QTRY_VERIFY_WITH_TIMEOUT((windowA = findWindow(titleA)) != 0, 10000);
        QTRY_VERIFY_WITH_TIMEOUT((windowB = findWindow(titleB)) != 0, 10000);
        verifyChild(windowA, titleA);
        QVERIFY(!QTest::currentTestFailed());
        verifyChild(windowB, titleB);
        QVERIFY(!QTest::currentTestFailed());
        const auto pidA = cardinalProperty(m_display, windowA, "_NET_WM_PID");
        const auto pidB = cardinalProperty(m_display, windowB, "_NET_WM_PID");
        QVERIFY(pidA != pidB);
        auto closedA = close(pathA);
        QTRY_VERIFY(closedA->isFinished());
        QCOMPARE(closedA->reply().type(), QDBusMessage::ReplyMessage);
        QTRY_VERIFY(first->isFinished());
        QCOMPARE(response(*first), 1u);
        QTRY_VERIFY(findWindow(titleA) == 0);
        QTRY_VERIFY(!processExists(pidA.first()));
        QCOMPARE(findWindow(titleB), windowB);
        QVERIFY(!second->isFinished());
        verifyBackendAlive();
        auto closedB = close(pathB);
        QTRY_VERIFY(closedB->isFinished());
        QCOMPARE(closedB->reply().type(), QDBusMessage::ReplyMessage);
        QTRY_VERIFY(second->isFinished());
        QCOMPARE(response(*second), 1u);
        QTRY_VERIFY(findWindow(titleB) == 0);
        QTRY_VERIFY(!processExists(pidB.first()));
        verifyBackendAlive();
    }

    void selectedDirectoryAndTypedOptionsSurviveChildBoundary()
    {
        QTemporaryDir folder(m_data.filePath(QStringLiteral("selected folder-XXXXXX")));
        QVERIFY(folder.isValid());
        const PortalFilters filters{{"Text files", {{0u, "*.txt"}}}, {"Everything", {{0u, "*"}}}};
        const PortalChoices choices{{"encoding", "Encoding", {{"utf8", "Unicode"}, {"latin1", "Western"}}, "latin1"},
                                   {"remember", "Remember folder", {}, "true"}};
        auto options = directoryOptions();
        auto encodedPath = QFile::encodeName(folder.path());
        encodedPath.append('\0');
        options.insert(QStringLiteral("current_folder"), encodedPath);
        options.insert(QStringLiteral("filters"), QVariant::fromValue(filters));
        options.insert(QStringLiteral("current_filter"), QVariant::fromValue(filters.last()));
        options.insert(QStringLiteral("choices"), QVariant::fromValue(choices));
        const QString title = QStringLiteral("Select directory through mixed presenter");
        QString path;
        auto pending = open(title, options, true, &path);
        Window window = 0;
        QTRY_VERIFY_WITH_TIMEOUT((window = findWindow(title)) != 0, 10000);
        verifyChild(window, title);
        QVERIFY(!QTest::currentTestFailed());
        // The dialog initially focuses its empty filename field and exposes a
        // default Select Folder action. A real X11 Return selects current_folder.
        // No private object access, widget coordinates, or production test hooks.
        sendReturn(window);
        QTRY_VERIFY_WITH_TIMEOUT(pending->isFinished(), 10000);
        QCOMPARE(response(*pending), 0u);
        const auto results = qdbus_cast<QVariantMap>(pending->reply().arguments().at(1));
        QCOMPARE(qdbus_cast<QStringList>(results.value(QStringLiteral("uris"))),
                 QStringList{QUrl::fromLocalFile(folder.path()).toString(QUrl::FullyEncoded)});
        const auto filter = qdbus_cast<PortalFilter>(results.value(QStringLiteral("current_filter")));
        QCOMPARE(filter.name, filters.last().name);
        QCOMPARE(filter.rules.size(), 1);
        QCOMPARE(filter.rules.first().type, 0u);
        QCOMPARE(filter.rules.first().value, QStringLiteral("*"));
        const auto selected = qdbus_cast<PortalChoiceResults>(results.value(QStringLiteral("choices")));
        QCOMPARE(selected.size(), 2);
        QCOMPARE(selected.at(0).id, QStringLiteral("encoding"));
        QCOMPARE(selected.at(0).selected, QStringLiteral("latin1"));
        QCOMPARE(selected.at(1).id, QStringLiteral("remember"));
        QCOMPARE(selected.at(1).selected, QStringLiteral("true"));
        QTRY_VERIFY(findWindow(title) == 0);
        verifyBackendAlive();
    }

    void frontendDisconnectReapsPresentationChildren()
    {
        const QString title = QStringLiteral("Disconnected frontend mixed chooser");
        QString path;
        auto pending = open(title, directoryOptions(), true, &path);
        Window window = 0;
        QTRY_VERIFY_WITH_TIMEOUT((window = findWindow(title)) != 0, 10000);
        verifyChild(window, title);
        QVERIFY(!QTest::currentTestFailed());
        const auto pid = cardinalProperty(m_display, window, "_NET_WM_PID").first();
        m_bus.reset();
        QDBusConnection::disconnectFromBus(m_connectionName);
        QTRY_VERIFY_WITH_TIMEOUT(findWindow(title) == 0, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!processExists(pid), 5000);
        verifyBackendAlive();
        // Claiming the frontend with a fresh connection must still work after
        // cleanup; this also proves the long-lived backend is reusable.
        m_connectionName += QStringLiteral("-replacement");
        m_bus = std::make_unique<QDBusConnection>(QDBusConnection::connectToBus(
            QDBusConnection::SessionBus, m_connectionName));
        QVERIFY(m_bus->registerService(frontend));
        const QString replacementTitle = QStringLiteral("Replacement frontend mixed chooser");
        auto replacement = open(replacementTitle, directoryOptions(), true, &path);
        QTRY_VERIFY_WITH_TIMEOUT(findWindow(replacementTitle) != 0, 10000);
        auto closed = close(path);
        QTRY_VERIFY(closed->isFinished());
        QCOMPARE(closed->reply().type(), QDBusMessage::ReplyMessage);
        QTRY_VERIFY(replacement->isFinished());
        QCOMPARE(response(*replacement), 1u);
        QTRY_VERIFY(findWindow(replacementTitle) == 0);
        verifyBackendAlive();
    }

    void saveFilePreservesSuggestedNameAndFilterExtension()
    {
        QTemporaryDir folder(m_data.filePath(QStringLiteral("save file-XXXXXX")));
        QVERIFY(folder.isValid());
        const QString suggested = QString::fromUtf8("draft #% é");
        const PortalFilters filters{{"Images", {{0u, "*.png"}}},
                                    {"Text documents", {{0u, "*.txt"}, {1u, "text/plain"}}}};
        auto encodedPath = QFile::encodeName(folder.path());
        encodedPath.append('\0');
        const QVariantMap options{{QStringLiteral("modal"), true},
            {QStringLiteral("current_folder"), encodedPath}, {QStringLiteral("current_name"), suggested},
            {QStringLiteral("filters"), QVariant::fromValue(filters)},
            {QStringLiteral("current_filter"), QVariant::fromValue(filters.last())}};
        const QString title = QStringLiteral("Save file through mixed presenter");
        QString path;
        auto pending = open(title, options, true, &path, QStringLiteral("SaveFile"));
        Window window = 0;
        QTRY_VERIFY_WITH_TIMEOUT((window = findWindow(title)) != 0, 10000);
        verifyChild(window, title);
        QVERIFY(!QTest::currentTestFailed());
        sendReturn(window);
        QTRY_VERIFY_WITH_TIMEOUT(pending->isFinished(), 10000);
        QCOMPARE(response(*pending), 0u);
        const auto results = qdbus_cast<QVariantMap>(pending->reply().arguments().at(1));
        const QString destination = folder.filePath(suggested + QStringLiteral(".txt"));
        QCOMPARE(qdbus_cast<QStringList>(results.value(QStringLiteral("uris"))),
                 QStringList{QUrl::fromLocalFile(destination).toString(QUrl::FullyEncoded)});
        const auto filter = qdbus_cast<PortalFilter>(results.value(QStringLiteral("current_filter")));
        QCOMPARE(filter.name, filters.last().name);
        QCOMPARE(filter.rules.size(), 2);
        QCOMPARE(filter.rules.at(0).type, 0u);
        QCOMPARE(filter.rules.at(0).value, QStringLiteral("*.txt"));
        QCOMPARE(filter.rules.at(1).type, 1u);
        QCOMPARE(filter.rules.at(1).value, QStringLiteral("text/plain"));
        QVERIFY(!QFile::exists(destination));
        QVERIFY(QDir(folder.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());
        QTRY_VERIFY(findWindow(title) == 0);
        verifyBackendAlive();
    }

    void saveFilesPreservesNamesCollisionOrderAndDoesNotWrite()
    {
        QTemporaryDir folder(m_data.filePath(QStringLiteral("save batch-XXXXXX")));
        QVERIFY(folder.isValid());
        const QString occupied = folder.filePath(QStringLiteral("a.txt"));
        {
            QFile file(occupied);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write("original"), 8);
        }
        const QString special = QString::fromUtf8("space #% é.txt");
        const QStringList supplied{QStringLiteral("a.txt"), QStringLiteral("a.txt"),
                                   QStringLiteral("a (1).txt"), special};
        PortalFileNames names;
        for (const auto &name : supplied) {
            auto bytes = QFile::encodeName(name);
            bytes.append('\0');
            names.append(bytes);
        }
        auto encodedPath = QFile::encodeName(folder.path());
        encodedPath.append('\0');
        const QVariantMap options{{QStringLiteral("modal"), true},
            {QStringLiteral("current_folder"), encodedPath}, {QStringLiteral("files"), QVariant::fromValue(names)}};
        const QString title = QStringLiteral("Save files through mixed presenter");
        QString path;
        auto pending = open(title, options, true, &path, QStringLiteral("SaveFiles"));
        Window window = 0;
        QTRY_VERIFY_WITH_TIMEOUT((window = findWindow(title)) != 0, 10000);
        verifyChild(window, title);
        QVERIFY(!QTest::currentTestFailed());
        sendReturn(window);
        QTRY_VERIFY_WITH_TIMEOUT(pending->isFinished(), 10000);
        QCOMPARE(response(*pending), 0u);
        const auto results = qdbus_cast<QVariantMap>(pending->reply().arguments().at(1));
        const QStringList leaves{QStringLiteral("a (1).txt"), QStringLiteral("a (2).txt"),
                                 QStringLiteral("a (1) (1).txt"), special};
        QStringList expected;
        for (const auto &leaf : leaves) {
            expected.append(QUrl::fromLocalFile(folder.filePath(leaf)).toString(QUrl::FullyEncoded));
            QVERIFY(!QFile::exists(folder.filePath(leaf)));
        }
        QCOMPARE(qdbus_cast<QStringList>(results.value(QStringLiteral("uris"))), expected);
        QCOMPARE(QDir(folder.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot),
                 QStringList{QStringLiteral("a.txt")});
        QFile file(occupied);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("original"));
        QTRY_VERIFY(findWindow(title) == 0);
        verifyBackendAlive();
    }

    void crashedPresentationFailsOnlyItsRequest()
    {
        const QString title = QStringLiteral("Crash only this private presentation child");
        QString path;
        auto pending = open(title, directoryOptions(), true, &path);
        Window window = 0;
        QTRY_VERIFY_WITH_TIMEOUT((window = findWindow(title)) != 0, 10000);
        verifyChild(window, title);
        QVERIFY(!QTest::currentTestFailed());
        const auto pid = cardinalProperty(m_display, window, "_NET_WM_PID").first();
        QVERIFY(isPresentationChild(pid));
        QCOMPARE(::kill(static_cast<pid_t>(pid), SIGKILL), 0);
        QTRY_VERIFY_WITH_TIMEOUT(pending->isFinished(), 5000);
        QCOMPARE(response(*pending), 2u);
        QVERIFY(qdbus_cast<QVariantMap>(pending->reply().arguments().at(1)).isEmpty());
        QTRY_VERIFY(findWindow(title) == 0);
        QTRY_VERIFY(!processExists(pid));
        verifyBackendAlive();

        const QString replacementTitle = QStringLiteral("Backend still presents after child crash");
        auto replacement = open(replacementTitle, directoryOptions(), true, &path);
        QTRY_VERIFY_WITH_TIMEOUT(findWindow(replacementTitle) != 0, 10000);
        auto closed = close(path);
        QTRY_VERIFY(closed->isFinished());
        QCOMPARE(closed->reply().type(), QDBusMessage::ReplyMessage);
        QTRY_VERIFY(replacement->isFinished());
        QCOMPARE(response(*replacement), 1u);
        QTRY_VERIFY(findWindow(replacementTitle) == 0);
        verifyBackendAlive();
    }

    void malformedParentNeverCreatesPresentation_data()
    {
        QTest::addColumn<QString>("parent");
        QTest::newRow("zero-with-prefix") << QStringLiteral("x11:0x0");
        QTest::newRow("prefix-without-id") << QStringLiteral("x11:0x");
        QTest::newRow("overflow-xid") << QStringLiteral("x11:0x100000000");
        QTest::newRow("malformed-prefix") << QStringLiteral("x11:0xnothex");
    }

    void malformedParentNeverCreatesPresentation()
    {
        QFETCH(QString, parent);
        const QString title = QStringLiteral("Invalid mixed parent must stay invisible");
        QString path;
        auto pending = openWithParent(title, directoryOptions(), parent, &path);
        QTRY_VERIFY_WITH_TIMEOUT(pending->isFinished(), 5000);
        QCOMPARE(response(*pending), 2u);
        QVERIFY(qdbus_cast<QVariantMap>(pending->reply().arguments().at(1)).isEmpty());
        QCOMPARE(findWindow(title), 0ul);
        QVERIFY(presentationChildren().isEmpty());
        verifyBackendAlive();
    }

    void abruptBackendDeathClosesPresentationOnPipeEof()
    {
        const QString title = QStringLiteral("Backend death must close its private presentation");
        QString path;
        auto pending = open(title, directoryOptions(), true, &path);
        Window window = 0;
        QTRY_VERIFY_WITH_TIMEOUT((window = findWindow(title)) != 0, 10000);
        verifyChild(window, title);
        QVERIFY(!QTest::currentTestFailed());
        const auto pid = cardinalProperty(m_display, window, "_NET_WM_PID").first();
        QVERIFY(isPresentationChild(pid));
        QVERIFY(!pending->isFinished());
        // Default SIGTERM ends the QProcess without running Qt destructors.
        // The presenter must notice its private input pipe's EOF on its own.
        m_portal.terminate();
        QVERIFY(m_portal.waitForFinished(5000));
        QCOMPARE(m_portal.exitStatus(), QProcess::CrashExit);
        QTRY_VERIFY_WITH_TIMEOUT(findWindow(title) == 0, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(processExited(pid), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(pending->isFinished(), 5000);
        QCOMPARE(pending->reply().type(), QDBusMessage::ErrorMessage);
        QVERIFY(response(*pending) != 0u);
        QTRY_VERIFY_WITH_TIMEOUT(!QDBusConnection::sessionBus().interface()->isServiceRegistered(backend).value(), 5000);
    }

    void cleanup()
    {
        if (m_bus) {
            m_bus.reset();
            QDBusConnection::disconnectFromBus(m_connectionName);
        }
        // QTest invokes cleanup even when an assertion exits a test early.
        // Only direct --present-chooser children of this QProcess are eligible.
        // Let normal owner-loss cleanup run before using this final safeguard.
        QTest::qWait(100);
        const auto children = presentationChildren();
        for (const auto pid : children)
            if (isPresentationChild(pid)) ::kill(static_cast<pid_t>(pid), SIGTERM);
        if (!children.isEmpty()) QTest::qWait(100);
        for (const auto pid : children)
            if (isPresentationChild(pid)) ::kill(static_cast<pid_t>(pid), SIGKILL);
        // Terminate only this test's direct QProcess child. Owner loss above
        // disposes request children through the production cancellation path.
        m_portal.terminate();
        if (m_portal.state() != QProcess::NotRunning && !m_portal.waitForFinished(5000)) {
            m_portal.kill();
            m_portal.waitForFinished(5000);
        }
    }

    void cleanupTestCase()
    {
        m_parent.hide();
        if (m_display) XCloseDisplay(m_display);
        XSetErrorHandler(m_previousErrorHandler);
    }

private:
    QVariantMap directoryOptions() const
    {
        return {{QStringLiteral("directory"), true}, {QStringLiteral("multiple"), true}, {QStringLiteral("modal"), true}};
    }

    std::unique_ptr<QDBusPendingCallWatcher> open(const QString &title, const QVariantMap &options,
                                                bool prefix, QString *path,
                                                const QString &method = QStringLiteral("OpenFile"))
    {
        const QString parent = QStringLiteral("x11:") + (prefix ? QStringLiteral("0x") : QString())
            + QString::number(m_parentId, 16);
        return openWithParent(title, options, parent, path, method);
    }

    std::unique_ptr<QDBusPendingCallWatcher> openWithParent(const QString &title, const QVariantMap &options,
                                                          const QString &parent, QString *path,
                                                          const QString &method = QStringLiteral("OpenFile"))
    {
        *path = QStringLiteral("/org/freedesktop/portal/desktop/request/mixed/r%1").arg(++m_requestSerial);
        auto message = QDBusMessage::createMethodCall(backend, QStringLiteral("/org/freedesktop/portal/desktop"),
                                                      chooserInterface, method);
        message.setArguments({QVariant::fromValue(QDBusObjectPath(*path)), QStringLiteral("org.example.MixedDisplay"),
                              parent, title, options});
        return std::make_unique<QDBusPendingCallWatcher>(m_bus->asyncCall(message, 30000));
    }

    std::unique_ptr<QDBusPendingCallWatcher> close(const QString &path)
    {
        auto message = QDBusMessage::createMethodCall(backend, path, requestInterface, QStringLiteral("Close"));
        return std::make_unique<QDBusPendingCallWatcher>(m_bus->asyncCall(message, 5000));
    }

    uint response(const QDBusPendingCallWatcher &pending) const
    {
        const auto reply = pending.reply();
        if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().size() != 2)
            return 999u;
        return reply.arguments().first().toUInt();
    }

    Window findWindow(const QString &title) const
    {
        for (const auto window : cardinalProperty(m_display, DefaultRootWindow(m_display), "_NET_CLIENT_LIST")) {
            if (QString::fromUtf8(bytesProperty(m_display, window, "_NET_WM_NAME")) == title)
                return window;
        }
        return 0;
    }

    void verifyChild(Window window, const QString &title)
    {
        QCOMPARE(QString::fromUtf8(bytesProperty(m_display, window, "_NET_WM_NAME")), title);
        Window parent = 0;
        QTRY_VERIFY(XGetTransientForHint(m_display, window, &parent));
        QCOMPARE(parent, static_cast<Window>(m_parentId));
        QTRY_VERIFY(cardinalProperty(m_display, window, "_NET_WM_STATE")
                        .contains(XInternAtom(m_display, "_NET_WM_STATE_MODAL", False)));
        QTRY_VERIFY(bytesProperty(m_display, window, "WM_CLASS").contains("linux-explorer-win7-chooser-dialog"));
        QTRY_COMPARE(cardinalProperty(m_display, window, "_NET_WM_PID").size(), 1);
        const auto pid = cardinalProperty(m_display, window, "_NET_WM_PID").first();
        QVERIFY(pid != static_cast<unsigned long>(QCoreApplication::applicationPid()));
        QVERIFY(pid != static_cast<unsigned long>(m_portal.processId()));
        // Prove it is this backend's private child, rather than another picker.
        const QByteArray status = readFile(QStringLiteral("/proc/%1/status").arg(pid));
        bool correctParent = false;
        for (const auto &line : status.split('\n'))
            if (line.startsWith("PPid:")) correctParent = line.mid(5).trimmed().toLongLong() == m_portal.processId();
        QVERIFY(correctParent);
    }

    void sendReturn(Window window)
    {
        // The picker ignores Enter briefly after it appears, so that a window
        // popping up under someone typing elsewhere is not accepted; a person
        // pressing Enter on purpose does so well after that
        QTest::qWait(600);   // FileChooserDialog::inputGuardMs (500) and a margin
        XEvent event{};
        event.xkey.display = m_display;
        event.xkey.window = window;
        event.xkey.root = DefaultRootWindow(m_display);
        event.xkey.same_screen = True;
        event.xkey.keycode = XKeysymToKeycode(m_display, XK_Return);
        event.type = KeyPress;
        QVERIFY(XSendEvent(m_display, window, True, KeyPressMask, &event));
        event.type = KeyRelease;
        QVERIFY(XSendEvent(m_display, window, True, KeyReleaseMask, &event));
        XFlush(m_display);
    }

    bool processExists(unsigned long pid) const { return QFile::exists(QStringLiteral("/proc/%1").arg(pid)); }
    bool processExited(unsigned long pid) const
    {
        const auto status = readFile(QStringLiteral("/proc/%1/status").arg(pid));
        if (status.isEmpty()) return true;
        for (const auto &line : status.split('\n')) {
            // A child orphaned by abrupt backend death is reaped by the host's
            // init/subreaper, whose timing is outside the chooser contract.
            if (line.startsWith("State:")) {
                const auto state = line.mid(6).trimmed();
                return state.startsWith('Z') || state.startsWith('X');
            }
        }
        return false;
    }
    bool isPresentationChild(unsigned long pid) const
    {
        if (m_portal.state() != QProcess::Running) return false;
        const auto command = readFile(QStringLiteral("/proc/%1/cmdline").arg(pid)).split('\0');
        if (!command.contains(QByteArray("--present-chooser"))) return false;
        const auto status = readFile(QStringLiteral("/proc/%1/status").arg(pid));
        for (const auto &line : status.split('\n'))
            if (line.startsWith("PPid:")) return line.mid(5).trimmed().toLongLong() == m_portal.processId();
        return false;
    }
    QList<unsigned long> presentationChildren() const
    {
        QList<unsigned long> result;
        if (m_portal.state() != QProcess::Running) return result;
        const auto entries = readFile(QStringLiteral("/proc/%1/task/%1/children").arg(m_portal.processId())).split(' ');
        for (const auto &entry : entries) {
            bool ok = false;
            const auto pid = entry.trimmed().toULong(&ok);
            if (ok && isPresentationChild(pid)) result.append(pid);
        }
        return result;
    }
    void verifyBackendAlive()
    {
        QCOMPARE(m_portal.state(), QProcess::Running);
        QVERIFY(QDBusConnection::sessionBus().interface()->isServiceRegistered(backend).value());
    }

    QTemporaryDir m_data;
    QWidget m_parent;
    WId m_parentId = 0;
    Display *m_display = nullptr;
    XErrorHandler m_previousErrorHandler = nullptr;
    QProcess m_portal;
    QString m_tracePath;
    QString m_connectionName;
    std::unique_ptr<QDBusConnection> m_bus;
    int m_serial = 0;
    int m_requestSerial = 0;
};

QTEST_MAIN(PortalMixedDisplayTest)
#include "PortalMixedDisplayTest.moc"
