// Public portal proof: the system frontend runs in a separate process, with
// the real backend/dialog in this process solely to drive deterministic UI.
#include "PortalChooserService.h"
#include "PortalDBusTypes.h"
#include "FileChooserDialog.h"
#include "FileView.h"
#include "DirectoryModel.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDialogButtonBox>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>

namespace {
constexpr auto frontend = "org.freedesktop.portal.Desktop";
constexpr auto root = "/org/freedesktop/portal/desktop";
constexpr auto iface = "org.freedesktop.portal.FileChooser";
constexpr auto requestIface = "org.freedesktop.portal.Request";
QByteArray bytes(const QString &path) { return QFile::encodeName(path) + '\0'; }
QDBusMessage call(const QDBusConnection &bus, const QString &path, const QString &interface,
                  const QString &method, const QList<QVariant> &args = {})
{
    auto message = QDBusMessage::createMethodCall(frontend, path, interface, method);
    message.setArguments(args);
    QDBusPendingCallWatcher watcher(bus.asyncCall(message, 10000));
    QEventLoop loop;
    QObject::connect(&watcher, &QDBusPendingCallWatcher::finished, &loop, &QEventLoop::quit);
    if (!watcher.isFinished()) loop.exec();
    return watcher.reply();
}
void writeFile(const QString &path)
{
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write("untouched"), qint64(9));
}
QString encoded(const QString &path) { return QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded); }
}

class PublicRequest : public QObject {
    Q_OBJECT
public:
    QDBusConnection bus;
    QString path;
    int responses = 0;
    uint status = 99;
    QVariantMap results;
    explicit PublicRequest(const QDBusConnection &connection) : bus(connection) {}
    bool begin(const QString &method, QVariantMap options = {})
    {
        static int serial = 0;
        const QString token = QStringLiteral("frontend_%1").arg(++serial);
        QString sender = bus.baseService().mid(1);
        sender.replace('.', '_');
        path = QString::fromLatin1(root) + "/request/" + sender + '/' + token;
        options.insert("handle_token", token);
        if (!bus.connect(frontend, path, requestIface, "Response", this, SLOT(response(uint,QVariantMap)))) return false;
        const auto reply = call(bus, root, iface, method, {QString(), QStringLiteral("Private frontend integration"), options});
        return reply.type() == QDBusMessage::ReplyMessage
            && qvariant_cast<QDBusObjectPath>(reply.arguments().value(0)).path() == path;
    }
public Q_SLOTS:
    void response(uint value, const QVariantMap &map) { ++responses; status = value; results = map; }
};

class PortalFrontendTest : public QObject {
    Q_OBJECT
    QDBusConnection m_backend = QDBusConnection::sessionBus();
    PortalChooserService *m_service = nullptr;
    QProcess m_frontend;
    QDBusConnection m_client{QStringLiteral("invalid")};
    int m_serial = 0;
    FileChooserDialog *dialog(const PublicRequest &request) { return m_service->dialogForRequest(request.path); }
    void accept(FileChooserDialog *chooser)
    {
        auto *box = chooser->findChild<QDialogButtonBox *>();
        QVERIFY(box);
        for (auto *button : box->buttons()) {
            if (box->buttonRole(button) == QDialogButtonBox::AcceptRole) {
                QTest::mouseClick(qobject_cast<QPushButton *>(button), Qt::LeftButton);
                return;
            }
        }
        QFAIL("No real chooser accept button");
    }
    void checkNoNames()
    {
        for (const auto *name : {"org.freedesktop.FileManager1", "io.github.LinuxExplorer.Win7"})
            QVERIFY(!m_backend.interface()->isServiceRegistered(name).value());
    }
private Q_SLOTS:
    void initTestCase()
    {
        QApplication::setQuitOnLastWindowClosed(false);
        registerPortalDBusTypes();
        m_service = new PortalChooserService(m_backend);
        QVERIFY(m_service->claim());
        m_frontend.setStandardOutputFile(QStringLiteral(PORTAL_FRONTEND_LOG));
        m_frontend.setStandardErrorFile(QStringLiteral(PORTAL_FRONTEND_LOG), QIODevice::Append);
        m_frontend.start(QStringLiteral(PORTAL_FRONTEND), {"--verbose"});
        QVERIFY(m_frontend.waitForStarted(5000));
        QTRY_VERIFY_WITH_TIMEOUT(m_backend.interface()->isServiceRegistered(frontend).value(), 15000);
        const auto reply = call(m_backend, root, "org.freedesktop.DBus.Introspectable", "Introspect");
        QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
        QVERIFY(reply.arguments().first().toString().contains(iface));
        checkNoNames();
    }
    void init()
    {
        m_client = QDBusConnection::connectToBus(QDBusConnection::SessionBus, QStringLiteral("public-client-%1").arg(++m_serial));
        QVERIFY(m_client.isConnected());
    }
    void cleanup()
    {
        QTRY_COMPARE(m_service->requestCount(), 0);
        const QString name = m_client.name();
        m_client = QDBusConnection(QStringLiteral("invalid"));
        QDBusConnection::disconnectFromBus(name);
        checkNoNames();
    }
    void defaultSingleFileAndEncodedTypedIdentity()
    {
        QTemporaryDir dir;
        const QString name = QString::fromUtf8("literal # % Ω name.txt");
        writeFile(dir.filePath(name));
        PublicRequest request(m_client);
        QVERIFY(request.begin("OpenFile", {{"current_folder", bytes(dir.path())}}));
        QTRY_VERIFY(dialog(request));
        auto *chooser = dialog(request);
        chooser->findChild<QLineEdit *>("chooserFileName")->setText(encoded(dir.filePath(name)));
        accept(chooser);
        QTRY_COMPARE(request.responses, 1);
        QCOMPARE(request.status, 0u);
        QCOMPARE(qdbus_cast<QStringList>(request.results.value("uris")), QStringList{encoded(dir.filePath(name))});
        QFile file(dir.filePath(name)); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), QByteArray("untouched"));
    }
    void multipleFilesAndFolders_data()
    {
        QTest::addColumn<bool>("folders");
        QTest::newRow("files") << false;
        QTest::newRow("folders") << true;
    }
    void multipleFilesAndFolders()
    {
        QFETCH(bool, folders);
        QTemporaryDir dir;
        const QStringList names{"first space", "second # %"};
        QList<QUrl> urls;
        for (const auto &name : names) {
            if (folders) QVERIFY(QDir(dir.path()).mkdir(name)); else writeFile(dir.filePath(name));
            urls.append(QUrl::fromLocalFile(dir.filePath(name)));
        }
        PublicRequest request(m_client);
        QVERIFY(request.begin("OpenFile", {{"current_folder", bytes(dir.path())}, {"multiple", true}, {"directory", folders}}));
        QTRY_VERIFY(dialog(request));
        auto *chooser = dialog(request);
        auto *view = chooser->findChild<FileView *>(); QVERIFY(view);
        QTRY_VERIFY(view->selectUrls(urls).isEmpty());
        accept(chooser);
        QTRY_COMPARE(request.responses, 1);
        QCOMPARE(request.status, 0u);
        auto actual = qdbus_cast<QStringList>(request.results.value("uris")); actual.sort();
        QStringList expected; for (const auto &url : urls) expected.append(url.toString(QUrl::FullyEncoded)); expected.sort();
        QCOMPARE(actual, expected);
    }
    void currentFolder()
    {
        QTemporaryDir dir;
        PublicRequest request(m_client);
        QVERIFY(request.begin("OpenFile", {{"current_folder", bytes(dir.path())}, {"directory", true}}));
        QTRY_VERIFY(dialog(request));
        accept(dialog(request));
        QTRY_COMPARE(request.responses, 1);
        QCOMPARE(request.status, 0u);
        QCOMPARE(qdbus_cast<QStringList>(request.results.value("uris")), QStringList{encoded(dir.path())});
    }
    void saveChoicesFiltersAndNoWrites()
    {
        QTemporaryDir dir;
        const PortalFilter filter{"Logs", {{0, "*.log"}}};
        const PortalChoices choices{{"encoding", "Encoding", {{"utf8", "Unicode"}, {"latin", "Western"}}, "utf8"},
                                     {"flag", "Flag", {}, "false"}};
        PublicRequest request(m_client);
        QVERIFY(request.begin("SaveFile", {{"current_folder", bytes(dir.path())}, {"current_name", "new # % Ω"},
            {"filters", QVariant::fromValue(PortalFilters{filter})}, {"current_filter", QVariant::fromValue(filter)},
            {"choices", QVariant::fromValue(choices)}}));
        QTRY_VERIFY(dialog(request));
        auto *chooser = dialog(request);
        for (auto *combo : chooser->findChildren<QComboBox *>())
            if (combo->findData("latin") >= 0) combo->setCurrentIndex(combo->findData("latin"));
        auto boxes = chooser->findChildren<QCheckBox *>(); QVERIFY(!boxes.isEmpty()); boxes.first()->setChecked(true);
        accept(chooser);
        QTRY_COMPARE(request.responses, 1);
        QCOMPARE(request.status, 0u);
        QCOMPARE(qdbus_cast<QStringList>(request.results.value("uris")), QStringList{encoded(dir.filePath("new # % Ω.log"))});
        const auto selected = qdbus_cast<PortalFilter>(request.results.value("current_filter"));
        QCOMPARE(selected.name, filter.name); QCOMPARE(selected.rules.size(), 1); QCOMPARE(selected.rules.first().value, QStringLiteral("*.log"));
        const auto resultChoices = qdbus_cast<PortalChoiceResults>(request.results.value("choices"));
        QCOMPARE(resultChoices.size(), 2); QCOMPARE(resultChoices[0].selected, QStringLiteral("latin")); QCOMPARE(resultChoices[1].selected, QStringLiteral("true"));
        QVERIFY(QDir(dir.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());
    }
    void saveFilesCollisionOrderNoWrites()
    {
        QTemporaryDir dir; writeFile(dir.filePath("one.txt"));
        PublicRequest request(m_client);
        QVERIFY(request.begin("SaveFiles", {{"current_folder", bytes(dir.path())},
            {"files", QVariant::fromValue(PortalFileNames{bytes("one.txt"), bytes("two # %.txt")})}}));
        QTRY_VERIFY(dialog(request)); accept(dialog(request));
        QTRY_COMPARE(request.responses, 1); QCOMPARE(request.status, 0u);
        QCOMPARE(qdbus_cast<QStringList>(request.results.value("uris")),
            (QStringList{encoded(dir.filePath("one (1).txt")), encoded(dir.filePath("two # %.txt"))}));
        QCOMPARE(QDir(dir.path()).entryList(QDir::Files), QStringList{"one.txt"});
    }
    void cancelCloseDisconnectAndIsolation()
    {
        PublicRequest first(m_client);
        QVERIFY(first.begin("OpenFile")); QTRY_VERIFY(dialog(first));
        const QString secondName = "public-concurrent";
        auto secondBus = QDBusConnection::connectToBus(QDBusConnection::SessionBus, secondName);
        PublicRequest second(secondBus);
        QVERIFY(second.begin("SaveFile")); QTRY_VERIFY(dialog(second));
        QCOMPARE(m_service->requestCount(), 2);
        auto denied = call(secondBus, first.path, requestIface, "Close");
        QCOMPARE(denied.type(), QDBusMessage::ErrorMessage);
        QCOMPARE(m_service->requestCount(), 2);
        QPointer<FileChooserDialog> firstDialog = dialog(first), secondDialog = dialog(second);
        QCOMPARE(call(m_client, first.path, requestIface, "Close").type(), QDBusMessage::ReplyMessage);
        QTRY_VERIFY(firstDialog.isNull()); QVERIFY(secondDialog);
        QTest::qWait(100); QCOMPARE(first.responses, 0); // Close has no Response.
        secondDialog->reject(); QTRY_COMPARE(second.responses, 1); QCOMPARE(second.status, 1u);
        QCOMPARE(first.responses, 0);
        secondBus = QDBusConnection(QStringLiteral("invalid"));
        // PublicRequest owns a connection copy; release that too before disconnect.
        second.bus = QDBusConnection(QStringLiteral("invalid"));
        QDBusConnection::disconnectFromBus(secondName);
        const QString goneName = "public-gone";
        auto goneBus = QDBusConnection::connectToBus(QDBusConnection::SessionBus, goneName);
        PublicRequest gone(goneBus); QVERIFY(gone.begin("OpenFile")); QTRY_VERIFY(dialog(gone));
        QPointer<FileChooserDialog> goneDialog = dialog(gone);
        goneBus = QDBusConnection(QStringLiteral("invalid")); gone.bus = QDBusConnection(QStringLiteral("invalid"));
        QDBusConnection::disconnectFromBus(goneName);
        QTRY_VERIFY_WITH_TIMEOUT(goneDialog.isNull(), 5000);
        QCOMPARE(gone.responses, 0);
    }
    void standardClientAcceptedAndCanceled()
    {
        QTemporaryDir dir; writeFile(dir.filePath("CLI # %.txt"));
        QProcess client;
        client.start(QStringLiteral(PORTAL_CLIENT), {"open", "--start", encoded(dir.path()), "--title", "CLI accepted"});
        QVERIFY(client.waitForStarted());
        FileChooserDialog *chooser = nullptr;
        QTRY_VERIFY(([&] { for (auto *widget : QApplication::topLevelWidgets()) {
            auto *candidate = qobject_cast<FileChooserDialog *>(widget);
            if (candidate && candidate->windowTitle() == QLatin1String("CLI accepted")) { chooser = candidate; return true; }
        } return false; })());
        chooser->findChild<QLineEdit *>("chooserFileName")->setText("CLI # %.txt"); accept(chooser);
        QTRY_COMPARE(client.state(), QProcess::NotRunning); QCOMPARE(client.exitCode(), 0);
        auto output = QJsonDocument::fromJson(client.readAllStandardOutput()).object();
        QCOMPARE(output.value("state").toString(), QStringLiteral("accepted"));
        QVERIFY(output.value("uris").toArray().contains(encoded(dir.filePath("CLI # %.txt"))));
        client.start(QStringLiteral(PORTAL_CLIENT), {"save-many", "--file", "never-written.txt", "--start", dir.path(), "--cancel-after", "25"});
        QVERIFY(client.waitForStarted()); QTRY_COMPARE(client.state(), QProcess::NotRunning); QCOMPARE(client.exitCode(), 1);
        output = QJsonDocument::fromJson(client.readAllStandardOutput()).object();
        QVERIFY(output.value("canceled_by_client").toBool()); QVERIFY(!QFile::exists(dir.filePath("never-written.txt")));
    }
    void cleanupTestCase()
    {
        m_frontend.terminate();
        if (!m_frontend.waitForFinished(5000)) { m_frontend.kill(); m_frontend.waitForFinished(); }
        delete m_service;
    }
};
QTEST_MAIN(PortalFrontendTest)
#include "PortalFrontendTest.moc"
