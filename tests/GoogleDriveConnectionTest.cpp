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

bool writeFile(const QString &path, const QByteArray &value)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(value) == value.size();
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
