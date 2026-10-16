#include "GoogleDriveAuth.h"
#include "GoogleDriveConnection.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QProcess>
#include <QSignalSpy>
#include <QSettings>
#include <QStandardPaths>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUrlQuery>
#include <cstring>

namespace {
class FixtureStore final : public GoogleDriveCredentialStore
{
public:
    QByteArray record;
    bool delayWrite = false;
    int reads = 0;
    int silentReads = 0;
    Result silentResult = Result::Success;
    int writes = 0;
    int removes = 0;
    Completion pendingWrite;
    QByteArray pendingRecord;
    void read(Completion completion) override
    {
        ++reads;
        const QByteArray snapshot = record;
        QTimer::singleShot(0, this, [completion, snapshot] {
            completion(snapshot.isEmpty() ? Result::Missing : Result::Success, snapshot);
        });
    }
    void write(const QByteArray &value, Completion completion) override
    {
        ++writes;
        pendingRecord = value;
        pendingWrite = completion;
        if (!delayWrite)
            QTimer::singleShot(0, this, [this] { finishWrite(); });
    }
    void readSilently(Completion completion) override
    {
        ++silentReads;
        const auto snapshot = record;
        const auto result = snapshot.isEmpty() && silentResult == Result::Success ? Result::Missing : silentResult;
        QTimer::singleShot(0, this, [completion, snapshot, result] { completion(result, snapshot); });
    }
    void finishWrite()
    {
        record = pendingRecord;
        auto completion = std::move(pendingWrite);
        completion(Result::Success, {});
    }
    void remove(Completion completion) override
    {
        ++removes;
        QTimer::singleShot(0, this, [this, completion] {
            record.clear();
            completion(Result::Success, {});
        });
    }
};

QJsonObject fixtureToken()
{
    return {{QStringLiteral("access_token"), QStringLiteral("fixture-access-token")},
        {QStringLiteral("refresh_token"), QStringLiteral("fixture-refresh-token")},
        {QStringLiteral("token_type"), QStringLiteral("Bearer")},
        {QStringLiteral("expiry"), QDateTime::currentDateTimeUtc().addSecs(3600).toString(Qt::ISODateWithMs)}};
}

GoogleDriveAuth::ClientConfig fixtureClient()
{
    return {QStringLiteral("fixture-client.apps.googleusercontent.com"), QStringLiteral("fixture-client-secret")};
}

QByteArray fixtureRecord()
{
    return QJsonDocument(QJsonObject {{QStringLiteral("version"), 1},
        {QStringLiteral("client_id"), fixtureClient().clientId},
        {QStringLiteral("scope"), GoogleDriveAuth::requiredScope()},
        {QStringLiteral("config_password"), QString(43, u'a')},
        {QStringLiteral("token"), fixtureToken()}}).toJson(QJsonDocument::Compact);
}

class FixtureReply final : public QNetworkReply
{
public:
    FixtureReply(const QNetworkRequest &request, QObject *parent) : QNetworkReply(parent)
    {
        setUrl(request.url());
        setRequest(request);
        setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, 200);
        if (request.url().path() == QStringLiteral("/token")) {
            m_data = QJsonDocument(QJsonObject {
                {QStringLiteral("access_token"), QStringLiteral("fixture-access-token")},
                {QStringLiteral("refresh_token"), QStringLiteral("fixture-refresh-token")},
                {QStringLiteral("token_type"), QStringLiteral("Bearer")},
                {QStringLiteral("scope"), GoogleDriveAuth::requiredScope()},
                {QStringLiteral("expires_in"), 3600}}).toJson(QJsonDocument::Compact);
        } else {
            m_data = "{}";
        }
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        QTimer::singleShot(0, this, [this] {
            setFinished(true);
            emit readyRead();
            emit finished();
        });
    }
    void abort() override { setFinished(true); }
    qint64 bytesAvailable() const override { return m_data.size() - m_offset + QNetworkReply::bytesAvailable(); }
protected:
    qint64 readData(char *data, qint64 maximum) override
    {
        const qint64 size = qMin(maximum, qint64(m_data.size() - m_offset));
        if (size <= 0)
            return -1;
        memcpy(data, m_data.constData() + m_offset, size);
        m_offset += size;
        return size;
    }
private:
    QByteArray m_data;
    qint64 m_offset = 0;
};

class FixtureNetwork final : public QNetworkAccessManager
{
public:
    explicit FixtureNetwork(QObject *parent) : QNetworkAccessManager(parent) {}
protected:
    QNetworkReply *createRequest(Operation, const QNetworkRequest &request, QIODevice *) override
    {
        // No request reaches the network, including the Google revoke endpoint.
        return new FixtureReply(request, this);
    }
};

GoogleDriveAuth::Dependencies fixtureDependencies(FixtureStore *store, QUrl *browser)
{
    return {store, [browser](const QUrl &url) { *browser = url; return true; },
        [](QObject *parent) { return new FixtureNetwork(parent); }};
}

class FixtureConnection final : public GoogleDriveConnection
{
public:
    // Fixtures never inspect an actual mount or require production credentials.
    bool isAvailable() const override { return true; }
    bool mounted = false;
    bool isConnected() const override { return mounted; }
};

class QuotaFixtureConnection final : public GoogleDriveConnection
{
public:
    QString mountId = QStringLiteral("41");
    bool isConnected() const override { return !mountId.isEmpty(); }
    void connectDrive() override { ++authenticationRequests; }
    void signInAgain() override { ++authenticationRequests; }
    int authenticationRequests = 0;
protected:
    QString storageMountId() const override { return mountId; }
};

bool writeFile(const QString &path, const QByteArray &value)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(value) == value.size();
}

QString quotaHelper(QTemporaryDir &directory, const QByteArray &response, double delay = 0)
{
    const QString path = directory.filePath(QStringLiteral("quota-helper.py"));
    const QString python = QStandardPaths::findExecutable(QStringLiteral("python3"));
    const QByteArray source = "#!" + python.toUtf8() + R"PY(
import base64, pathlib, sys, time
root = pathlib.Path(__file__).parent
assert sys.argv[1:] == ['app-quota']
assert sys.stdin.buffer.read() == b''
with (root / 'calls').open('a') as calls:
    calls.write('quota\n')
(root / 'started').touch()
)PY" + "time.sleep(" + QByteArray::number(delay) + ")\n" +
        "sys.stdout.buffer.write(base64.b64decode('" + response.toBase64() + "'))\n";
    if (python.isEmpty() || !writeFile(path, source)
        || !QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner))
        return {};
    return path;
}

QByteArray quotaResponse(const QString &id = QStringLiteral("41"),
                        const QString &available = QStringLiteral("12884901888"))
{
    return QJsonDocument(QJsonObject{{QStringLiteral("mount_id"), id},
        {QStringLiteral("total"), QStringLiteral("16106127360")},
        {QStringLiteral("available"), available}}).toJson(QJsonDocument::Compact);
}

class HelperFixture
{
public:
    QTemporaryDir directory;
    QString script() const { return directory.filePath(QStringLiteral("helper.py")); }
    QString marker(const QString &name) const { return directory.filePath(name); }
    bool create(const QByteArray &mode)
    {
        const QString python = QStandardPaths::findExecutable(QStringLiteral("python3"));
        if (!directory.isValid() || python.isEmpty()
            || !writeFile(marker(QStringLiteral("mode")), mode))
            return false;
        const QByteArray source = "#!" + python.toUtf8() + R"PY(
import datetime
import json
import os
import pathlib
import sys
import time

root = pathlib.Path(__file__).parent
command = sys.argv[1]
data = sys.stdin.buffer.read()
envelope = json.loads(data) if data else {}
secrets = ('fixture-access-token', 'fixture-refresh-token', 'a' * 43)
entry = {
    'argv': sys.argv[1:],
    'keys': sorted(envelope),
    'token_null': envelope.get('token') is None,
    'password_valid': envelope.get('config_password') == 'a' * 43,
    'secret_in_environment': any(secret in value for secret in secrets for value in os.environ.values()),
}
with (root / 'calls.jsonl').open('a') as output:
    output.write(json.dumps(entry) + '\n')
if command == 'app-export-token':
    (root / 'export-started').touch()
    mode = (root / 'mode').read_text()
    if mode == 'overflow':
        sys.stdout.write('x' * 70000)
        sys.stdout.flush()
        time.sleep(60)
    elif mode == 'hang':
        time.sleep(60)
    else:
        expiry = (datetime.datetime.now(datetime.timezone.utc) + datetime.timedelta(hours=1)).isoformat()
        print(json.dumps({'access_token': 'fixture-access-token', 'refresh_token': 'fixture-refresh-token',
                         'token_type': 'Bearer', 'expiry': expiry}))
elif command == 'app-connect' and (root / 'mode').read_text() == 'slow-connect':
    time.sleep(0.3)
elif command == 'app-forget':
    (root / 'forgot').touch()
elif command not in ('disconnect', 'app-connect'):
    sys.exit(2)
)PY";
        return writeFile(script(), source)
            && QFile::setPermissions(script(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    }
    QList<QJsonObject> calls() const
    {
        QFile file(marker(QStringLiteral("calls.jsonl")));
        if (!file.open(QIODevice::ReadOnly))
            return {};
        QList<QJsonObject> result;
        for (const QByteArray &line : file.readAll().split('\n')) {
            if (!line.isEmpty())
                result.append(QJsonDocument::fromJson(line).object());
        }
        return result;
    }
    QStringList commands() const
    {
        QStringList result;
        for (const auto &call : calls())
            result.append(call.value(QStringLiteral("argv")).toArray().first().toString());
        return result;
    }
};
}

class GoogleDriveConnectionTest : public QObject
{
    Q_OBJECT
private:
    static void inject(FixtureConnection &connection, GoogleDriveAuth &auth, const QString &helper)
    {
        connection.m_injectedAuth = &auth;
        connection.m_helperOverride = helper;
    }
    static bool finishBrowser(const QUrl &browser)
    {
        const QUrlQuery query(browser);
        QUrl callback(query.queryItemValue(QStringLiteral("redirect_uri"), QUrl::FullyDecoded));
        QUrlQuery values;
        values.addQueryItem(QStringLiteral("state"), query.queryItemValue(QStringLiteral("state"), QUrl::FullyDecoded));
        values.addQueryItem(QStringLiteral("code"), QStringLiteral("fixture-authorization-code"));
        callback.setQuery(values);
        QTcpSocket socket;
        socket.connectToHost(QStringLiteral("127.0.0.1"), callback.port());
        if (!socket.waitForConnected(1000))
            return false;
        socket.write("GET " + callback.path(QUrl::FullyEncoded).toLatin1() + '?'
            + callback.query(QUrl::FullyEncoded).toLatin1()
            + " HTTP/1.1\r\nHost: 127.0.0.1:" + QByteArray::number(callback.port()) + "\r\n\r\n");
        QElapsedTimer elapsed;
        elapsed.start();
        QByteArray response;
        while (elapsed.elapsed() < 1000) {
            QCoreApplication::processEvents();
            response += socket.readAll();
            if (socket.state() == QAbstractSocket::UnconnectedState)
                break;
        }
        return response.startsWith("HTTP/1.1 200");
    }
    static void verifyNoCredentialExposure(const HelperFixture &helper, const QSignalSpy &errors)
    {
        for (const auto &call : helper.calls()) {
            const auto arguments = call.value(QStringLiteral("argv")).toArray();
            for (const auto &argument : arguments) {
                const QString value = argument.toString();
                QVERIFY(!value.contains(QStringLiteral("fixture-access-token")));
                QVERIFY(!value.contains(QStringLiteral("fixture-refresh-token")));
                QVERIFY(!value.contains(QString(43, u'a')));
            }
            QVERIFY(!call.value(QStringLiteral("secret_in_environment")).toBool());
            if (arguments.first().toString() == QStringLiteral("app-export-token")) {
                QVERIFY(call.value(QStringLiteral("password_valid")).toBool());
                QVERIFY(call.value(QStringLiteral("token_null")).toBool());
                QCOMPARE(call.value(QStringLiteral("keys")).toArray().size(), 5);
            }
        }
        for (const auto &error : errors) {
            const QString message = error.first().toString();
            QVERIFY(!message.contains(QStringLiteral("fixture-access-token")));
            QVERIFY(!message.contains(QStringLiteral("fixture-refresh-token")));
            QVERIFY(!message.contains(QString(43, u'a')));
        }
    }
private slots:
    void disconnectWaitsForSyncStopBeforeUnmount()
    {
        HelperFixture helper;
        QVERIFY(helper.create("success"));
        FixtureConnection connection;
        connection.m_helperOverride = helper.script();
        GoogleDriveConnection::LifecycleCompletion stopped;
        connection.setSyncLifecycleHooks([&stopped](bool signOut, auto completion) {
            QVERIFY(!signOut);
            stopped = std::move(completion);
        }, {});
        connection.disconnectDrive();
        QVERIFY(bool(stopped));
        QVERIFY(connection.m_operationLock);
        QVERIFY(helper.calls().isEmpty());
        stopped(true, {});
        stopped = {};
        QTRY_COMPARE(connection.m_operation, GoogleDriveConnection::Operation::None);
        QCOMPARE(helper.commands(), QStringList{QStringLiteral("disconnect")});
    }
    void failedSyncStopPreservesMountAndPreventsUnmount()
    {
        HelperFixture helper;
        QVERIFY(helper.create("success"));
        FixtureConnection connection;
        connection.mounted = true;
        connection.m_helperOverride = helper.script();
        connection.setSyncLifecycleHooks([](bool, auto completion) {
            completion(false, QStringLiteral("Fixture sync worker could not stop"));
        }, {});
        QSignalSpy errors(&connection, &GoogleDriveConnection::failed);
        connection.disconnectDrive();
        QCOMPARE(errors.size(), 1);
        QVERIFY(connection.isConnected());
        QVERIFY(helper.calls().isEmpty());
        QVERIFY(connection.autoReconnectSuppressed());
    }
    void signOutWaitsForLocalSyncCleanupBeforeDeclaringCompletion()
    {
        HelperFixture helper;
        QVERIFY(helper.create("success"));
        FixtureStore store;
        store.record = fixtureRecord();
        QUrl browser;
        GoogleDriveAuth auth(fixtureClient(), fixtureDependencies(&store, &browser));
        FixtureConnection connection;
        inject(connection, auth, helper.script());
        GoogleDriveConnection::LifecycleCompletion cleaned;
        int prepares = 0;
        connection.setSyncLifecycleHooks([&prepares](bool signOut, auto completion) {
            QVERIFY(signOut);
            ++prepares;
            completion(true, {});
        }, [&cleaned](auto completion) { cleaned = std::move(completion); });
        QSignalSpy signedOut(&connection, &GoogleDriveConnection::signedOut);
        connection.signOut();
        QTRY_VERIFY(bool(cleaned));
        QCOMPARE(prepares, 1);
        QCOMPARE(helper.commands(), QStringList({QStringLiteral("disconnect"), QStringLiteral("app-export-token")}));
        QCOMPARE(signedOut.size(), 0);
        QVERIFY(connection.m_operationLock);
        cleaned(true, {});
        cleaned = {};
        QTRY_COMPARE(signedOut.size(), 1);
        QCOMPARE(helper.commands().last(), QStringLiteral("app-forget"));
        QVERIFY(store.record.isEmpty());
    }
    void init()
    {
        QCoreApplication::setOrganizationName(QStringLiteral("explorer-reconnect-tests"));
        QSettings().remove(QStringLiteral("GoogleDrive"));
    }
    void automaticReconnectReusesMountWithoutWalletOrNavigation()
    {
        FixtureStore store;
        QUrl browser;
        GoogleDriveAuth auth(fixtureClient(), fixtureDependencies(&store, &browser));
        FixtureConnection connection;
        connection.mounted = true;
        inject(connection, auth, {});
        QSignalSpy connected(&connection, &GoogleDriveConnection::connected);
        connection.reconnectSavedDrive();
        QCOMPARE(connection.connectionState(), GoogleDriveConnection::ConnectionState::Connected);
        QCOMPARE(store.reads, 0);
        QCOMPARE(store.silentReads, 0);
        QVERIFY(browser.isEmpty());
        QCOMPARE(connected.size(), 0);
    }
    void automaticReconnectMissingConfigNeverReadsWallet()
    {
        HelperFixture helper;
        QVERIFY(helper.create("success"));
        FixtureStore store;
        QUrl browser;
        GoogleDriveAuth auth(fixtureClient(), fixtureDependencies(&store, &browser));
        FixtureConnection connection;
        inject(connection, auth, helper.script());
        connection.m_appConfigOverride = helper.marker(QStringLiteral("missing-config"));
        connection.reconnectSavedDrive();
        QCOMPARE(connection.connectionState(), GoogleDriveConnection::ConnectionState::SignInRequired);
        QCOMPARE(store.reads, 0);
        QCOMPARE(store.silentReads, 0);
        QVERIFY(browser.isEmpty());
        QVERIFY(helper.calls().isEmpty());
    }
    void automaticReconnectLockedWalletStopsWithoutPromptsOrRetries()
    {
        HelperFixture helper;
        QVERIFY(helper.create("success"));
        FixtureStore store;
        store.record = fixtureRecord();
        store.silentResult = GoogleDriveCredentialStore::Result::InteractionRequired;
        QUrl browser;
        GoogleDriveAuth auth(fixtureClient(), fixtureDependencies(&store, &browser));
        FixtureConnection connection;
        inject(connection, auth, helper.script());
        connection.m_appConfigOverride = helper.marker(QStringLiteral("saved-config"));
        QVERIFY(writeFile(connection.m_appConfigOverride, "encrypted fixture"));
        QSignalSpy errors(&connection, &GoogleDriveConnection::failed);
        connection.reconnectSavedDrive();
        QTRY_COMPARE(connection.connectionState(), GoogleDriveConnection::ConnectionState::WalletLocked);
        QCOMPARE(store.reads, 0);
        QCOMPARE(store.silentReads, 1);
        QVERIFY(browser.isEmpty());
        QVERIFY(helper.calls().isEmpty());
        QVERIFY(!connection.m_reconnectTimer->isActive());
        QCOMPARE(errors.size(), 0);
    }
    void automaticReconnectSuccessDoesNotEmitInteractiveSuccess()
    {
        HelperFixture helper;
        QVERIFY(helper.create("slow-connect"));
        FixtureStore store;
        store.record = fixtureRecord();
        QUrl browser;
        GoogleDriveAuth auth(fixtureClient(), fixtureDependencies(&store, &browser));
        FixtureConnection connection;
        inject(connection, auth, helper.script());
        connection.m_appConfigOverride = helper.marker(QStringLiteral("saved-config"));
        QVERIFY(writeFile(connection.m_appConfigOverride, "encrypted fixture"));
        QSignalSpy connected(&connection, &GoogleDriveConnection::connected);
        connection.reconnectSavedDrive();
        QTRY_COMPARE(helper.commands().size(), 1);
        connection.mounted = true;
        QTRY_COMPARE(connection.m_operation, GoogleDriveConnection::Operation::None);
        QCOMPARE(connection.connectionState(), GoogleDriveConnection::ConnectionState::Connected);
        QCOMPARE(connected.size(), 0);
        QCOMPARE(store.reads, 0);
        QCOMPARE(store.silentReads, 1);
        QVERIFY(browser.isEmpty());
        QVERIFY(helper.calls().first().value(QStringLiteral("token_null")).toBool());
    }
    void manualDisconnectSuppressesStartupAcrossControllers()
    {
        HelperFixture helper;
        QVERIFY(helper.create("success"));
        FixtureConnection connection;
        connection.m_helperOverride = helper.script();
        connection.disconnectDrive();
        QTRY_COMPARE(connection.m_operation, GoogleDriveConnection::Operation::None);
        FixtureConnection next;
        QVERIFY(next.autoReconnectSuppressed());
        next.reconnectSavedDrive();
        QVERIFY(!next.m_auth);
        QVERIFY(!next.m_process);
        next.setAutoReconnectEnabled(true);
        QVERIFY(!next.autoReconnectSuppressed());
    }
    void automaticRetryBudgetIsBoundedAndPreferenceStopsRetry()
    {
        FixtureConnection connection;
        connection.m_reconnectAttempts = 1;
        connection.scheduleReconnectRetry();
        QVERIFY(connection.m_reconnectTimer->isActive());
        connection.setAutoReconnectEnabled(false);
        QVERIFY(!connection.m_reconnectTimer->isActive());
        QVERIFY(!connection.autoReconnectEnabled());
        connection.setAutoReconnectEnabled(true);
        connection.m_reconnectAttempts = 3;
        connection.scheduleReconnectRetry();
        QVERIFY(!connection.m_reconnectTimer->isActive());
    }
    void connectedDriveQuotaSmokeTest()
    {
        if (!qEnvironmentVariableIsSet("EXPLORER_LIVE_DRIVE_QUOTA"))
            QSKIP("Opt-in capacity-only check of an already connected Drive.");
        GoogleDriveConnection connection;
        QVERIFY(connection.isConnected());
        connection.refreshStorageQuota(true);
        QTRY_VERIFY_WITH_TIMEOUT(connection.hasStorageQuota(), 15000);
        QVERIFY(connection.storageTotalBytes() > 0);
        QVERIFY(connection.storageAvailableBytes() <= connection.storageTotalBytes());
        QVERIFY(!connection.m_auth);
        QVERIFY(!connection.m_operationLock);
        const QString output = qEnvironmentVariable("EXPLORER_QUOTA_REPORT");
        if (!output.isEmpty()) {
            QVERIFY(writeFile(output, QJsonDocument(QJsonObject{
                {QStringLiteral("total"), QString::number(connection.storageTotalBytes())},
                {QStringLiteral("available"), QString::number(connection.storageAvailableBytes())},
                {QStringLiteral("credential_access"), false}}).toJson()));
        }
    }

    void storageQuotaIsBoundedCachedAndNeverAuthorizes()
    {
        QTemporaryDir directory;
        QuotaFixtureConnection connection;
        connection.m_helperOverride = quotaHelper(directory, quotaResponse());
        QVERIFY(!connection.m_helperOverride.isEmpty());
        QSignalSpy changes(&connection, &GoogleDriveConnection::storageQuotaChanged);
        QSignalSpy errors(&connection, &GoogleDriveConnection::failed);
        connection.refreshStorageQuota();
        auto *pending = connection.m_quotaProcess.data();
        connection.refreshStorageQuota(true);
        QCOMPARE(connection.m_quotaProcess.data(), pending);
        QTRY_VERIFY(connection.hasStorageQuota());
        QCOMPARE(connection.storageTotalBytes(), 16106127360ULL);
        QCOMPARE(connection.storageAvailableBytes(), 12884901888ULL);
        QCOMPARE(changes.size(), 1);
        connection.refreshStorageQuota();
        QVERIFY(!connection.m_quotaProcess);
        connection.refreshStorageQuota(true);
        QTRY_VERIFY(!connection.m_quotaProcess);
        QCOMPARE(changes.size(), 1); // unchanged figures do not reset views
        QFile calls(directory.filePath(QStringLiteral("calls")));
        QVERIFY(calls.open(QIODevice::ReadOnly));
        QCOMPARE(calls.readAll(), QByteArray("quota\nquota\n"));
        QVERIFY(!connection.m_auth);
        QVERIFY(!connection.m_operationLock);
        QCOMPARE(connection.authenticationRequests, 0);
        QCOMPARE(errors.size(), 0);
    }

    void storageQuotaRejectsInvalidReplies_data()
    {
        QTest::addColumn<QByteArray>("response");
        QTest::newRow("wrong-mount") << quotaResponse(QStringLiteral("42"));
        QTest::newRow("invalid-json") << QByteArray("not JSON");
        QTest::newRow("too-large") << QByteArray(5000, 'x');
        QTest::newRow("free-larger-than-total") << quotaResponse(QStringLiteral("41"), QStringLiteral("16106127361"));
        QTest::newRow("negative") << quotaResponse(QStringLiteral("41"), QStringLiteral("-1"));
        QTest::newRow("overflow") << quotaResponse(QStringLiteral("41"), QStringLiteral("18446744073709551616"));
        QTest::newRow("fraction") << quotaResponse(QStringLiteral("41"), QStringLiteral("1.5"));
        QTest::newRow("leading-zero") << quotaResponse(QStringLiteral("41"), QStringLiteral("0123"));
        QTest::newRow("non-ascii") << quotaResponse(QStringLiteral("41"), QString::fromUtf8("１２３"));
        for (const QString &field : {QStringLiteral("total"), QStringLiteral("available")}) {
            auto object = QJsonDocument::fromJson(quotaResponse()).object();
            object[field] = QString::number(quint64(1) << 50);
            QTest::newRow(qPrintable(QStringLiteral("fabricated-") + field)) << QJsonDocument(object).toJson();
        }
        auto object = QJsonDocument::fromJson(quotaResponse()).object();
        object[QStringLiteral("total")] = QStringLiteral("0");
        QTest::newRow("zero-total") << QJsonDocument(object).toJson();
        object = QJsonDocument::fromJson(quotaResponse()).object();
        object[QStringLiteral("total")] = 16106127360.0;
        QTest::newRow("numeric-instead-of-decimal-string") << QJsonDocument(object).toJson();
        object[QStringLiteral("extra")] = QStringLiteral("unexpected");
        QTest::newRow("extra-fields") << QJsonDocument(object).toJson();
    }

    void storageQuotaRejectsInvalidReplies()
    {
        QFETCH(QByteArray, response);
        QTemporaryDir directory;
        QuotaFixtureConnection connection;
        connection.m_helperOverride = quotaHelper(directory, response);
        QVERIFY(!connection.m_helperOverride.isEmpty());
        connection.refreshStorageQuota();
        QTRY_VERIFY(!connection.m_quotaProcess);
        QVERIFY(!connection.hasStorageQuota());
        QCOMPARE(connection.storageTotalBytes(), 0ULL);
        QVERIFY(connection.m_quotaOutput.isEmpty());
        connection.refreshStorageQuota(); // failed requests are throttled too
        QVERIFY(!connection.m_quotaProcess);
        QVERIFY(!connection.m_auth);
    }

    void storageQuotaDiscardedOnMountReplacementAndOffline()
    {
        QTemporaryDir directory;
        QuotaFixtureConnection connection;
        connection.m_helperOverride = quotaHelper(directory, quotaResponse(), .2);
        QVERIFY(!connection.m_helperOverride.isEmpty());
        connection.refreshMountState();
        connection.refreshStorageQuota();
        QTRY_VERIFY(QFile::exists(directory.filePath(QStringLiteral("started"))));
        connection.mountId = QStringLiteral("42");
        QTRY_VERIFY(!connection.m_quotaProcess);
        QVERIFY(!connection.hasStorageQuota());
        connection.m_helperOverride = quotaHelper(directory, quotaResponse(QStringLiteral("42"), QStringLiteral("0")));
        connection.refreshStorageQuota(true);
        QTRY_VERIFY(connection.hasStorageQuota());
        QCOMPARE(connection.storageAvailableBytes(), 0ULL); // a full account is valid
        connection.refreshMountState();
        QVERIFY(!connection.hasStorageQuota()); // observed replacement invalidates cache
        connection.refreshStorageQuota();
        QTRY_VERIFY(connection.hasStorageQuota());
        connection.mountId.clear();
        QVERIFY(!connection.hasStorageQuota());
        connection.refreshMountState();
        QVERIFY(!connection.m_quotaKnown);
        QVERIFY(!connection.m_auth);
    }

    void storageQuotaTimeoutCancelsWithoutUiErrorOrWallet()
    {
        QTemporaryDir directory;
        QuotaFixtureConnection connection;
        connection.m_helperOverride = quotaHelper(directory, quotaResponse(), 5);
        QVERIFY(!connection.m_helperOverride.isEmpty());
        QSignalSpy errors(&connection, &GoogleDriveConnection::failed);
        connection.refreshStorageQuota();
        QTRY_VERIFY(QFile::exists(directory.filePath(QStringLiteral("started"))));
        QVERIFY(connection.m_quotaTimeout->isActive());
        QVERIFY(QMetaObject::invokeMethod(connection.m_quotaTimeout, "timeout", Qt::DirectConnection));
        QVERIFY(!connection.m_quotaProcess);
        QVERIFY(!connection.hasStorageQuota());
        QVERIFY(!connection.m_auth);
        QCOMPARE(errors.size(), 0);
    }

    void storageQuotaWaitsForConnectionOperations()
    {
        using Operation = GoogleDriveConnection::Operation;
        QuotaFixtureConnection connection;
        for (const Operation operation : {Operation::Connect, Operation::Disconnect,
                Operation::SignOutDisconnect, Operation::Export, Operation::Forget}) {
            connection.m_operation = operation;
            connection.refreshStorageQuota(true);
            QVERIFY(!connection.m_quotaProcess);
            QVERIFY(!connection.m_auth);
        }
        connection.m_operation = Operation::None;
    }

    void disconnectWaitsForQuotaProcessToReleaseItsDescriptor()
    {
        QTemporaryDir directory;
        const QString python = QStandardPaths::findExecutable(QStringLiteral("python3"));
        const QString script = directory.filePath(QStringLiteral("helper.py"));
        QVERIFY(writeFile(script, "#!" + python.toUtf8() + R"PY(
import os, pathlib, sys, time
root = pathlib.Path(__file__).parent
if sys.argv[1] == 'app-quota':
    (root / 'quota-pid').write_text(str(os.getpid()))
    time.sleep(5)
elif sys.argv[1] == 'disconnect':
    try:
        os.kill(int((root / 'quota-pid').read_text()), 0)
        result = 'still-open'
    except ProcessLookupError:
        result = 'closed'
    (root / 'disconnect').write_text(result)
)PY"));
        QVERIFY(QFile::setPermissions(script, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QuotaFixtureConnection connection;
        connection.m_helperOverride = script;
        QSignalSpy errors(&connection, &GoogleDriveConnection::failed);
        connection.refreshStorageQuota();
        QTRY_VERIFY(QFile::exists(directory.filePath(QStringLiteral("quota-pid"))));
        connection.disconnectDrive();
        QVERIFY(!connection.m_process); // unmount has not been started synchronously
        connection.refreshStorageQuota(true);
        QVERIFY(!connection.m_quotaProcess);
        QTRY_VERIFY(QFile::exists(directory.filePath(QStringLiteral("disconnect"))));
        QFile marker(directory.filePath(QStringLiteral("disconnect")));
        QVERIFY(marker.open(QIODevice::ReadOnly));
        QCOMPARE(marker.readAll(), QByteArray("closed"));
        QTRY_COMPARE(connection.m_operation, GoogleDriveConnection::Operation::None);
        QCOMPARE(errors.size(), 0);
        QVERIFY(!connection.m_auth);
    }

    void mountObservationNeverStartsAuthorization()
    {
        FixtureConnection connection;
        connection.refreshMountState();
        QSignalSpy changes(&connection, &GoogleDriveConnection::mountStateChanged);
        QSignalSpy connected(&connection, &GoogleDriveConnection::connected);
        connection.mounted = true;
        connection.refreshMountState();
        connection.refreshMountState();
        QCOMPARE(changes.size(), 1);
        QCOMPARE(changes.first().first().toBool(), true);
        connection.mounted = false;
        connection.refreshMountState();
        QCOMPARE(changes.size(), 2);
        QCOMPARE(changes.last().first().toBool(), false);
        QVERIFY(!connection.m_auth);
        QVERIFY(!connection.m_process);
        QVERIFY(!connection.m_operationLock);
        QCOMPARE(connected.size(), 0);
    }

    void anotherProcessLockPreventsAnyWalletOrBrowserWork()
    {
        HelperFixture helper;
        QVERIFY(helper.create("success"));
        FixtureStore store;
        QUrl browser;
        GoogleDriveAuth auth(fixtureClient(), fixtureDependencies(&store, &browser));
        FixtureConnection connection;
        inject(connection, auth, helper.script());
        QLockFile other(QDir(QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation))
            .filePath(QStringLiteral("explorer-win7-google-drive-auth.lock")));
        other.setStaleLockTime(0);
        QVERIFY(other.tryLock());
        QSignalSpy errors(&connection, &GoogleDriveConnection::failed);
        connection.signInAgain();
        QCOMPARE(errors.size(), 1);
        QCOMPARE(store.reads, 0);
        QVERIFY(browser.isEmpty());
        QVERIFY(!auth.isBusy());
        other.unlock();
        connection.signInAgain();
        QTRY_VERIFY(browser.isValid());
        QCOMPARE(store.reads, 1);
        connection.cancel();
        QVERIFY(!connection.m_operationLock);
    }

    void cancelDuringWalletWriteThenReconnectFailsVisibly()
    {
        HelperFixture helper;
        QVERIFY(helper.create("success"));
        FixtureStore store;
        store.record = fixtureRecord();
        store.delayWrite = true;
        QUrl browser;
        GoogleDriveAuth auth(fixtureClient(), fixtureDependencies(&store, &browser));
        FixtureConnection connection;
        inject(connection, auth, helper.script());
        QSignalSpy errors(&connection, &GoogleDriveConnection::failed);
        QSignalSpy connected(&connection, &GoogleDriveConnection::connected);
        connection.signInAgain();
        QTRY_VERIFY(browser.isValid());
        QVERIFY(finishBrowser(browser));
        QTRY_COMPARE(store.writes, 1);
        connection.cancel();
        QVERIFY(auth.isBusy());
        QVERIFY(connection.m_operationLock);
        connection.connectDrive();
        QCOMPARE(errors.size(), 1);
        QVERIFY(errors.first().first().toString().contains(QStringLiteral("finishing")));
        QCOMPARE(connection.m_operation, GoogleDriveConnection::Operation::None);
        store.finishWrite();
        QVERIFY(!auth.isBusy());
        QVERIFY(!connection.m_operationLock);
        QCOMPARE(connected.size(), 0);
        QVERIFY(helper.calls().isEmpty());
    }

    void exportFailedToStartStillClearsWalletAndRunsForget()
    {
        HelperFixture helper;
        QVERIFY(helper.create("success"));
        FixtureStore store;
        store.record = fixtureRecord();
        QUrl browser;
        GoogleDriveAuth auth(fixtureClient(), fixtureDependencies(&store, &browser));
        FixtureConnection connection;
        inject(connection, auth, helper.script());
        // These connections precede the coordinator's auth wiring, making
        // only the export executable unavailable and restoring Forget's path.
        connect(&auth, &GoogleDriveAuth::revocationTokenRequested, &connection, [&] {
            connection.m_helperOverride = helper.marker(QStringLiteral("missing-executable"));
        });
        connect(&auth, &GoogleDriveAuth::signedOutLocally, &connection, [&](const QString &) {
            connection.m_helperOverride = helper.script();
        });
        QSignalSpy signedOut(&connection, &GoogleDriveConnection::signedOut);
        QSignalSpy errors(&connection, &GoogleDriveConnection::failed);
        connection.signOut();
        QTRY_COMPARE(signedOut.size(), 1);
        QVERIFY(store.record.isEmpty());
        QCOMPARE(store.removes, 1);
        QCOMPARE(helper.commands(), QStringList({QStringLiteral("disconnect"), QStringLiteral("app-forget")}));
        QVERIFY(QFile::exists(helper.marker(QStringLiteral("forgot"))));
        QCOMPARE(errors.size(), 1); // truthful unconfirmed-revocation warning
        QCOMPARE(connection.m_operation, GoogleDriveConnection::Operation::None);
        verifyNoCredentialExposure(helper, errors);
    }

    void localClearDuringRunningExportWaitsThenActuallyRunsForget()
    {
        HelperFixture helper;
        QVERIFY(helper.create("hang"));
        FixtureStore store;
        store.record = fixtureRecord();
        QUrl browser;
        GoogleDriveAuth auth(fixtureClient(), fixtureDependencies(&store, &browser));
        FixtureConnection connection;
        inject(connection, auth, helper.script());
        QSignalSpy signedOut(&connection, &GoogleDriveConnection::signedOut);
        QSignalSpy errors(&connection, &GoogleDriveConnection::failed);
        connection.signOut();
        QTRY_VERIFY(QFile::exists(helper.marker(QStringLiteral("export-started"))));
        QVERIFY(connection.m_process);
        QCOMPARE(connection.m_operation, GoogleDriveConnection::Operation::Export);
        // Simulates the backend's earlier deadline settling while export lives.
        auth.completeSignOut({});
        QTRY_COMPARE(signedOut.size(), 1);
        QCOMPARE(helper.commands(), QStringList({QStringLiteral("disconnect"), QStringLiteral("app-export-token"), QStringLiteral("app-forget")}));
        QVERIFY(QFile::exists(helper.marker(QStringLiteral("forgot"))));
        QVERIFY(store.record.isEmpty());
        QVERIFY(!connection.m_process);
        QCOMPARE(connection.m_operation, GoogleDriveConnection::Operation::None);
        QCOMPARE(errors.size(), 1);
        verifyNoCredentialExposure(helper, errors);
    }

    void oversizedExportIsTerminatedAndCleanupCompletes()
    {
        HelperFixture helper;
        QVERIFY(helper.create("overflow"));
        FixtureStore store;
        store.record = fixtureRecord();
        QUrl browser;
        GoogleDriveAuth auth(fixtureClient(), fixtureDependencies(&store, &browser));
        FixtureConnection connection;
        inject(connection, auth, helper.script());
        QSignalSpy signedOut(&connection, &GoogleDriveConnection::signedOut);
        QSignalSpy errors(&connection, &GoogleDriveConnection::failed);
        connection.signOut();
        QTRY_COMPARE(signedOut.size(), 1);
        QCOMPARE(helper.commands(), QStringList({QStringLiteral("disconnect"), QStringLiteral("app-export-token"), QStringLiteral("app-forget")}));
        QVERIFY(store.record.isEmpty());
        QVERIFY(connection.m_output.isEmpty());
        QVERIFY(!connection.m_process);
        QCOMPARE(errors.size(), 1);
        verifyNoCredentialExposure(helper, errors);
    }

    void exportDeadlineTerminatesAndContinuesLocalCleanup()
    {
        HelperFixture helper;
        QVERIFY(helper.create("hang"));
        FixtureStore store;
        store.record = fixtureRecord();
        QUrl browser;
        GoogleDriveAuth auth(fixtureClient(), fixtureDependencies(&store, &browser));
        FixtureConnection connection;
        inject(connection, auth, helper.script());
        QSignalSpy signedOut(&connection, &GoogleDriveConnection::signedOut);
        QSignalSpy errors(&connection, &GoogleDriveConnection::failed);
        connection.signOut();
        QTRY_VERIFY(QFile::exists(helper.marker(QStringLiteral("export-started"))));
        QVERIFY(connection.m_timeout->isActive());
        QCOMPARE(connection.m_timeout->interval(), 35000);
        QVERIFY(QMetaObject::invokeMethod(connection.m_timeout, "timeout", Qt::DirectConnection));
        QTRY_COMPARE(signedOut.size(), 1);
        QCOMPARE(helper.commands(), QStringList({QStringLiteral("disconnect"), QStringLiteral("app-export-token"), QStringLiteral("app-forget")}));
        QVERIFY(store.record.isEmpty());
        QCOMPARE(errors.size(), 1);
        verifyNoCredentialExposure(helper, errors);
    }

    void successfulExportRevokesPrivatelyBeforeForget()
    {
        HelperFixture helper;
        QVERIFY(helper.create("success"));
        FixtureStore store;
        store.record = fixtureRecord();
        QUrl browser;
        GoogleDriveAuth auth(fixtureClient(), fixtureDependencies(&store, &browser));
        FixtureConnection connection;
        inject(connection, auth, helper.script());
        QSignalSpy signedOut(&connection, &GoogleDriveConnection::signedOut);
        QSignalSpy errors(&connection, &GoogleDriveConnection::failed);
        connection.signOut();
        QTRY_COMPARE(signedOut.size(), 1);
        QCOMPARE(helper.commands(), QStringList({QStringLiteral("disconnect"), QStringLiteral("app-export-token"), QStringLiteral("app-forget")}));
        QCOMPARE(errors.size(), 0);
        QVERIFY(store.record.isEmpty());
        verifyNoCredentialExposure(helper, errors);
    }
};

QTEST_GUILESS_MAIN(GoogleDriveConnectionTest)
#include "GoogleDriveConnectionTest.moc"
