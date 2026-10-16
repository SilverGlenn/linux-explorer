#include "GoogleDriveAuth.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSignalSpy>
#include <QTcpSocket>
#include <QTest>
#include <QUrlQuery>
#include <cstring>

namespace {
class MemoryStore final : public GoogleDriveCredentialStore
{
public:
    QByteArray record;
    Result readResult = Result::Success;
    Result writeResult = Result::Success;
    Result removeResult = Result::Success;
    int reads = 0;
    int silentReads = 0;
    Result silentReadResult = Result::Success;
    int writes = 0;
    int removes = 0;
    bool delayWrite = false;
    bool migrate = false;
    bool requiresMigration() const override { return migrate; }
    Completion pendingWrite;
    QByteArray pendingRecord;
    void read(Completion completion) override
    {
        ++reads;
        const auto value = record;
        const auto result = readResult == Result::Success && value.isEmpty() ? Result::Missing : readResult;
        QTimer::singleShot(0, this, [completion, result, value] { completion(result, value); });
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
        const auto value = record;
        const auto result = silentReadResult == Result::Success && value.isEmpty()
            ? Result::Missing : silentReadResult;
        QTimer::singleShot(0, this, [completion, result, value] { completion(result, value); });
    }
    void finishWrite()
    {
        if (writeResult == Result::Success)
            record = pendingRecord;
        auto completion = std::move(pendingWrite);
        completion(writeResult, {});
    }
    void remove(Completion completion) override
    {
        ++removes;
        QTimer::singleShot(0, this, [this, completion] {
            if (removeResult == Result::Success)
                record.clear();
            completion(removeResult, {});
        });
    }
};

class FakeReply : public QNetworkReply
{
public:
    FakeReply(const QNetworkRequest &request, QByteArray body, int status, QObject *parent)
        : QNetworkReply(parent), m_body(std::move(body))
    {
        setRequest(request);
        setUrl(request.url());
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, status);
        setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        QTimer::singleShot(0, this, [this] {
            emit readyRead();
            setFinished(true);
            emit finished();
        });
    }
    std::function<void()> aborted;
    void abort() override
    {
        if (!isFinished()) {
            setError(QNetworkReply::OperationCanceledError, QStringLiteral("Fixture request cancelled"));
            if (aborted)
                aborted();
        }
        setFinished(true);
    }
    qint64 bytesAvailable() const override { return m_body.size() - m_offset + QNetworkReply::bytesAvailable(); }
protected:
    qint64 readData(char *destination, qint64 maximum) override
    {
        const auto size = qMin(maximum, qint64(m_body.size() - m_offset));
        if (size <= 0)
            return -1;
        memcpy(destination, m_body.constData() + m_offset, size);
        m_offset += size;
        return size;
    }
private:
    QByteArray m_body;
    qint64 m_offset = 0;
};

struct NetworkState {
    QByteArray tokenResponse;
    QByteArray revokeResponse = "{}";
    int revokeStatus = 200;
    int abortedReplies = 0;
    std::function<void(QNetworkReply *)> configureReply;
    QList<QByteArray> requestBodies;
    QList<QUrl> requestUrls;
};
class FakeNetwork final : public QNetworkAccessManager
{
public:
    FakeNetwork(NetworkState *state, QObject *parent) : QNetworkAccessManager(parent), m_state(state) {}
protected:
    QNetworkReply *createRequest(Operation operation, const QNetworkRequest &request, QIODevice *data) override
    {
        Q_UNUSED(operation);
        m_state->requestUrls.append(request.url());
        m_state->requestBodies.append(data ? data->readAll() : QByteArray());
        const bool revoke = request.url().path() == QStringLiteral("/revoke");
        auto *reply = new FakeReply(request, revoke ? m_state->revokeResponse : m_state->tokenResponse,
                                   revoke ? m_state->revokeStatus : 200, this);
        reply->aborted = [state = m_state] { ++state->abortedReplies; };
        if (m_state->configureReply)
            m_state->configureReply(reply);
        return reply;
    }
private:
    NetworkState *m_state;
};

GoogleDriveAuth::ClientConfig client()
{
    return {QStringLiteral("test-desktop.apps.googleusercontent.com"), QStringLiteral("public-desktop-client-secret")};
}

QJsonObject sampleToken()
{
    return {{QStringLiteral("access_token"), QStringLiteral("access-secret")},
        {QStringLiteral("refresh_token"), QStringLiteral("refresh-secret")},
        {QStringLiteral("token_type"), QStringLiteral("Bearer")},
        {QStringLiteral("expiry"), QDateTime::currentDateTimeUtc().addSecs(3600).toString(Qt::ISODateWithMs)}};
}

QByteArray recordWith(const QJsonObject &changes = {})
{
    QJsonObject record {{QStringLiteral("version"), 1}, {QStringLiteral("client_id"), client().clientId},
        {QStringLiteral("scope"), GoogleDriveAuth::requiredScope()},
        {QStringLiteral("config_password"), QString(43, u'a')}, {QStringLiteral("token"), sampleToken()}};
    for (auto it = changes.begin(); it != changes.end(); ++it)
        record.insert(it.key(), it.value());
    return QJsonDocument(record).toJson(QJsonDocument::Compact);
}

QByteArray responseWithScope(const QString &scope = GoogleDriveAuth::requiredScope())
{
    return QJsonDocument(QJsonObject {{QStringLiteral("access_token"), QStringLiteral("new-access-secret")},
        {QStringLiteral("refresh_token"), QStringLiteral("new-refresh-secret")},
        {QStringLiteral("token_type"), QStringLiteral("Bearer")}, {QStringLiteral("expires_in"), 3600},
        {QStringLiteral("scope"), scope}}).toJson(QJsonDocument::Compact);
}

GoogleDriveAuth::Dependencies dependencies(MemoryStore *store, NetworkState *network, QUrl *browser)
{
    return {store, [browser](const QUrl &url) { *browser = url; return true; },
        [network](QObject *parent) { return new FakeNetwork(network, parent); }};
}

QUrl callbackUrl(const QUrl &browser)
{
    const QUrlQuery query(browser);
    QUrl callback(query.queryItemValue(QStringLiteral("redirect_uri"), QUrl::FullyDecoded));
    QUrlQuery result;
    result.addQueryItem(QStringLiteral("state"), query.queryItemValue(QStringLiteral("state"), QUrl::FullyDecoded));
    result.addQueryItem(QStringLiteral("code"), QStringLiteral("fake-code"));
    callback.setQuery(result);
    return callback;
}

QByteArray requestFor(const QUrl &callback)
{
    return "GET " + callback.path(QUrl::FullyEncoded).toLatin1() + '?'
        + callback.query(QUrl::FullyEncoded).toLatin1()
        + " HTTP/1.1\r\nHost: 127.0.0.1:" + QByteArray::number(callback.port()) + "\r\n\r\n";
}
}

class GoogleDriveAuthTest : public QObject
{
    Q_OBJECT
private:
    static QByteArray sendCallback(const QUrl &callback, QByteArray request = {})
    {
        QTcpSocket socket;
        socket.connectToHost(QStringLiteral("127.0.0.1"), callback.port());
        if (!socket.waitForConnected(1000))
            return {};
        socket.write(request.isEmpty() ? requestFor(callback) : request);
        QByteArray result;
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 1000) {
            QCoreApplication::processEvents();
            result += socket.readAll();
            if (socket.state() == QAbstractSocket::UnconnectedState)
                break;
        }
        return result;
    }
private slots:
    void accountPurposeEnforcesSeparateScopes()
    {
        MemoryStore store;
        store.record = recordWith();
        NetworkState network;
        QUrl browser;
        GoogleDriveAuth sync(client(), dependencies(&store, &network, &browser), GoogleDriveAuth::AccountPurpose::Sync);
        QCOMPARE(sync.authorizationScope(), GoogleDriveAuth::syncScope());
        QSignalSpy unavailable(&sync, &GoogleDriveAuth::savedAccountUnavailable);
        sync.connectSavedAccount();
        QTRY_COMPARE(unavailable.size(), 1);
        QCOMPARE(unavailable.first().first().value<GoogleDriveAuth::SavedAccountResult>(), GoogleDriveAuth::SavedAccountResult::Invalid);
        store.record = recordWith({{QStringLiteral("scope"), GoogleDriveAuth::syncScope()}});
        QSignalSpy authorized(&sync, &GoogleDriveAuth::authorized);
        sync.connectSavedAccount();
        QTRY_COMPARE(authorized.size(), 1);
        GoogleDriveAuth browse(client(), dependencies(&store, &network, &browser));
        QCOMPARE(browse.authorizationScope(), GoogleDriveAuth::requiredScope());
        QSignalSpy rejected(&browse, &GoogleDriveAuth::savedAccountUnavailable);
        browse.connectSavedAccount();
        QTRY_COMPARE(rejected.size(), 1);
        QCOMPARE(rejected.first().first().value<GoogleDriveAuth::SavedAccountResult>(), GoogleDriveAuth::SavedAccountResult::Invalid);
        QVERIFY(browser.isEmpty());
        QVERIFY(network.requestUrls.isEmpty());
        QCOMPARE(store.writes, 0);
    }
    void explicitSyncConsentRequestsAndStoresOnlyFullDriveScope()
    {
        MemoryStore store;
        NetworkState network;
        network.tokenResponse = responseWithScope(GoogleDriveAuth::syncScope());
        QUrl browser;
        GoogleDriveAuth auth(client(), dependencies(&store, &network, &browser), GoogleDriveAuth::AccountPurpose::Sync);
        QSignalSpy authorized(&auth, &GoogleDriveAuth::authorized);
        auth.startSignIn();
        QTRY_VERIFY(browser.isValid());
        QCOMPARE(QUrlQuery(browser).queryItemValue(QStringLiteral("scope"), QUrl::FullyDecoded), GoogleDriveAuth::syncScope());
        QVERIFY(sendCallback(callbackUrl(browser)).startsWith("HTTP/1.1 200"));
        QTRY_COMPARE(authorized.size(), 1);
        QCOMPARE(QJsonDocument::fromJson(store.record).object().value(QStringLiteral("scope")).toString(), GoogleDriveAuth::syncScope());
        QCOMPARE(store.writes, 1);
    }
    void localForgetNeverReadsWalletOrRevokesGoogle()
    {
        MemoryStore store;
        store.record = recordWith({{QStringLiteral("scope"), GoogleDriveAuth::syncScope()}});
        NetworkState network;
        QUrl browser;
        GoogleDriveAuth auth(client(), dependencies(&store, &network, &browser), GoogleDriveAuth::AccountPurpose::Sync);
        QSignalSpy removed(&auth, &GoogleDriveAuth::signedOutLocally);
        auth.forgetLocalAccount();
        QTRY_COMPARE(removed.size(), 1);
        QCOMPARE(store.reads, 0);
        QCOMPARE(store.silentReads, 0);
        QCOMPARE(store.removes, 1);
        QVERIFY(store.record.isEmpty());
        QVERIFY(browser.isEmpty());
        QVERIFY(network.requestUrls.isEmpty());
    }
    void localForgetWaitsForCommittedWrite()
    {
        MemoryStore store;
        store.record = recordWith();
        store.migrate = true;
        store.delayWrite = true;
        NetworkState network;
        QUrl browser;
        GoogleDriveAuth auth(client(), dependencies(&store, &network, &browser));
        QSignalSpy authorized(&auth, &GoogleDriveAuth::authorized);
        QSignalSpy removed(&auth, &GoogleDriveAuth::signedOutLocally);
        auth.connectExisting();
        QTRY_COMPARE(store.writes, 1);
        auth.forgetLocalAccount();
        QVERIFY(auth.isBusy());
        QCOMPARE(store.removes, 0);
        store.finishWrite();
        QTRY_COMPARE(removed.size(), 1);
        QCOMPARE(store.removes, 1);
        QCOMPARE(authorized.size(), 0);
        QVERIFY(store.record.isEmpty());
        QVERIFY(network.requestUrls.isEmpty());
    }
    void signOutSupersedesQueuedForgetWithoutErasingFutureSignIn()
    {
        MemoryStore store;
        store.record = recordWith();
        store.migrate = true;
        store.delayWrite = true;
        NetworkState network;
        network.tokenResponse = responseWithScope();
        QUrl browser;
        GoogleDriveAuth auth(client(), dependencies(&store, &network, &browser));
        QSignalSpy currentToken(&auth, &GoogleDriveAuth::revocationTokenRequested);
        QSignalSpy removed(&auth, &GoogleDriveAuth::signedOutLocally);
        QSignalSpy authorized(&auth, &GoogleDriveAuth::authorized);
        auth.connectExisting();
        QTRY_COMPARE(store.writes, 1);
        auth.forgetLocalAccount();
        auth.signOut();
        store.finishWrite();
        QTRY_COMPARE(currentToken.size(), 1);
        auth.completeSignOut(QJsonDocument(sampleToken()).toJson(QJsonDocument::Compact));
        QTRY_COMPARE(removed.size(), 1);
        QCOMPARE(store.removes, 1);
        store.migrate = false;
        store.delayWrite = false;
        auth.startSignIn();
        QTRY_VERIFY(browser.isValid());
        QVERIFY(sendCallback(callbackUrl(browser)).startsWith("HTTP/1.1 200"));
        QTRY_COMPARE(authorized.size(), 1);
        QCOMPARE(store.removes, 1);
        QVERIFY(!store.record.isEmpty());
    }
    void savedOnlyReadNeverFallsBackToBrowser_data()
    {
        QTest::addColumn<int>("storeResult");
        QTest::addColumn<int>("expected");
        QTest::newRow("missing") << int(GoogleDriveCredentialStore::Result::Missing) << int(GoogleDriveAuth::SavedAccountResult::Missing);
        QTest::newRow("locked") << int(GoogleDriveCredentialStore::Result::InteractionRequired) << int(GoogleDriveAuth::SavedAccountResult::WalletLocked);
        QTest::newRow("unavailable") << int(GoogleDriveCredentialStore::Result::Unavailable) << int(GoogleDriveAuth::SavedAccountResult::Unavailable);
    }
    void savedOnlyReadNeverFallsBackToBrowser()
    {
        QFETCH(int, storeResult);
        QFETCH(int, expected);
        MemoryStore store;
        store.silentReadResult = GoogleDriveCredentialStore::Result(storeResult);
        NetworkState network;
        QUrl browser;
        GoogleDriveAuth auth(client(), dependencies(&store, &network, &browser));
        QSignalSpy unavailable(&auth, &GoogleDriveAuth::savedAccountUnavailable);
        QSignalSpy failed(&auth, &GoogleDriveAuth::failed);
        auth.connectSavedAccount();
        QTRY_COMPARE(unavailable.size(), 1);
        QCOMPARE(int(unavailable.first().first().value<GoogleDriveAuth::SavedAccountResult>()), expected);
        QCOMPARE(store.reads, 0);
        QCOMPARE(store.silentReads, 1);
        QCOMPARE(store.writes, 0);
        QCOMPARE(store.removes, 0);
        QCOMPARE(failed.size(), 0);
        QVERIFY(browser.isEmpty());
        QVERIFY(network.requestUrls.isEmpty());
        QVERIFY(!auth.isBusy());
    }
    void savedOnlySuccessUsesCanonicalTokenWithoutWalletPrompt()
    {
        MemoryStore store;
        store.record = recordWith();
        store.migrate = true; // automatic reads must never trigger migration writes
        NetworkState network;
        QUrl browser;
        GoogleDriveAuth auth(client(), dependencies(&store, &network, &browser));
        QSignalSpy authorized(&auth, &GoogleDriveAuth::authorized);
        auth.connectSavedAccount();
        QTRY_COMPARE(authorized.size(), 1);
        QVERIFY(authorized.first().first().toByteArray().isEmpty());
        QCOMPARE(store.reads, 0);
        QCOMPARE(store.silentReads, 1);
        QCOMPARE(store.writes, 0);
        QVERIFY(browser.isEmpty());
        QVERIFY(network.requestUrls.isEmpty());
    }
    void explicitMigrationDoesNotReplaySavedTokenAndCancellationWaitsForWrite()
    {
        MemoryStore store;
        store.record = recordWith();
        store.migrate = true;
        store.delayWrite = true;
        NetworkState network;
        QUrl browser;
        GoogleDriveAuth auth(client(), dependencies(&store, &network, &browser));
        QSignalSpy authorized(&auth, &GoogleDriveAuth::authorized);
        auth.connectExisting();
        QTRY_COMPARE(store.writes, 1);
        auth.cancel();
        QVERIFY(auth.isBusy());
        store.finishWrite();
        QTRY_VERIFY(!auth.isBusy());
        QCOMPARE(authorized.size(), 0);
        QVERIFY(browser.isEmpty());
        QVERIFY(network.requestUrls.isEmpty());
        auth.connectExisting();
        QTRY_COMPARE(store.writes, 2);
        store.finishWrite();
        QTRY_COMPARE(authorized.size(), 1);
        QVERIFY(authorized.first().first().toByteArray().isEmpty());
    }
    void interactiveWalletSavedOnlyMissingStillDoesNotAuthorize()
    {
        MemoryStore store;
        NetworkState network;
        QUrl browser;
        GoogleDriveAuth auth(client(), dependencies(&store, &network, &browser));
        QSignalSpy unavailable(&auth, &GoogleDriveAuth::savedAccountUnavailable);
        auth.connectSavedAccountInteractively();
        QTRY_COMPARE(unavailable.size(), 1);
        QCOMPARE(store.reads, 1);
        QCOMPARE(store.silentReads, 0);
        QVERIFY(browser.isEmpty());
        QVERIFY(network.requestUrls.isEmpty());
    }
    void savedOnlyInvalidRecordDoesNotModifyCredentials()
    {
        MemoryStore store;
        store.record = "invalid saved record";
        NetworkState network;
        QUrl browser;
        GoogleDriveAuth auth(client(), dependencies(&store, &network, &browser));
        QSignalSpy unavailable(&auth, &GoogleDriveAuth::savedAccountUnavailable);
        auth.connectSavedAccount();
        QTRY_COMPARE(unavailable.size(), 1);
        QCOMPARE(unavailable.first().first().value<GoogleDriveAuth::SavedAccountResult>(), GoogleDriveAuth::SavedAccountResult::Invalid);
        QCOMPARE(store.writes, 0);
        QCOMPARE(store.removes, 0);
        QVERIFY(browser.isEmpty());
    }
    void missingClientDoesNotOpenBrowserOrWallet()
    {
        MemoryStore store;
        NetworkState network;
        QUrl browser;
        GoogleDriveAuth auth({}, dependencies(&store, &network, &browser));
        QSignalSpy failed(&auth, &GoogleDriveAuth::failed);
        QVERIFY(browser.isEmpty());
        QCOMPARE(store.reads, 0);
        auth.startSignIn();
        QCOMPARE(failed.size(), 1);
        QVERIFY(browser.isEmpty());
        QCOMPARE(store.reads, 0);
    }

    void existingUsesCanonicalEncryptedConfigWithoutNetwork()
    {
        MemoryStore store;
        store.record = recordWith();
        NetworkState network;
        QUrl browser;
        GoogleDriveAuth auth(client(), dependencies(&store, &network, &browser));
        QSignalSpy authorized(&auth, &GoogleDriveAuth::authorized);
        auth.connectExisting();
        QTRY_COMPARE(authorized.size(), 1);
        QVERIFY(authorized.first().at(0).toByteArray().isEmpty());
        QCOMPARE(authorized.first().at(1).toString(), QString(43, u'a'));
        QVERIFY(network.requestUrls.isEmpty());
        QVERIFY(browser.isEmpty());
    }

    void explicitConnectWithNoSavedAccountOpensBrowser()
    {
        MemoryStore store;
        NetworkState network;
        QUrl browser;
        GoogleDriveAuth auth(client(), dependencies(&store, &network, &browser));
        QVERIFY(browser.isEmpty());
        QCOMPARE(store.reads, 0);
        auth.connectExisting();
        QTRY_VERIFY(browser.isValid());
        QCOMPARE(store.reads, 1);
        QCOMPARE(browser.host(), QStringLiteral("accounts.google.com"));
        QVERIFY(auth.isBusy());
    }

    void validatesBundledClientStrictly_data()
    {
        QTest::addColumn<QString>("id");
        QTest::addColumn<QString>("secret");
        QTest::newRow("missing-secret") << client().clientId << QString();
        QTest::newRow("id-newline") << client().clientId + u'\n' << client().clientSecret;
        QTest::newRow("id-space") << QStringLiteral("bad client.apps.googleusercontent.com") << client().clientSecret;
        QTest::newRow("secret-control") << client().clientId << QStringLiteral("secret\n");
        QTest::newRow("secret-space") << client().clientId << QStringLiteral("with spaces");
    }
    void validatesBundledClientStrictly()
    {
        QFETCH(QString, id);
        QFETCH(QString, secret);
        MemoryStore store;
        NetworkState network;
        QUrl browser;
        GoogleDriveAuth auth({id, secret}, dependencies(&store, &network, &browser));
        QVERIFY(!auth.available());
        auth.startSignIn();
        QCOMPARE(store.reads, 0);
        QVERIFY(browser.isEmpty());
    }

    void rejectsInvalidSavedRecord_data()
    {
        QTest::addColumn<QByteArray>("record");
        QTest::newRow("different-client") << recordWith({{QStringLiteral("client_id"), QStringLiteral("other-client")}});
        QTest::newRow("missing-scope") << recordWith({{QStringLiteral("scope"), QString()}});
        QTest::newRow("reduced-scope") << recordWith({{QStringLiteral("scope"), QStringLiteral("https://www.googleapis.com/auth/drive.metadata.readonly")}});
        QTest::newRow("broader-scope") << recordWith({{QStringLiteral("scope"), QStringLiteral("https://www.googleapis.com/auth/drive")}});
        QTest::newRow("bad-password") << recordWith({{QStringLiteral("config_password"), QString(43, u'!')}});
        auto token = sampleToken();
        token.insert(QStringLiteral("expiry"), QStringLiteral("2026-10-16T10:20:30"));
        QTest::newRow("timezone-missing") << recordWith({{QStringLiteral("token"), token}});
        QTest::newRow("malformed") << QByteArray("{broken");
        QTest::newRow("oversized") << QByteArray(65537, 'a');
    }
    void rejectsInvalidSavedRecord()
    {
        QFETCH(QByteArray, record);
        MemoryStore store;
        store.record = record;
        NetworkState network;
        QUrl browser;
        GoogleDriveAuth auth(client(), dependencies(&store, &network, &browser));
        QSignalSpy failed(&auth, &GoogleDriveAuth::failed);
        QSignalSpy authorized(&auth, &GoogleDriveAuth::authorized);
        auth.connectExisting();
        QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(authorized.size(), 0);
        QVERIFY(network.requestUrls.isEmpty());
    }

    void browserFlowUsesPkceAndPersistsBeforeAuthorization()
    {
        MemoryStore store;
        store.delayWrite = true;
        NetworkState network;
        network.tokenResponse = responseWithScope();
        QUrl browser;
        GoogleDriveAuth auth(client(), dependencies(&store, &network, &browser));
        QSignalSpy authorized(&auth, &GoogleDriveAuth::authorized);
        auth.startSignIn();
        QTRY_VERIFY(browser.isValid());
        const QUrlQuery query(browser);
        QCOMPARE(browser.host(), QStringLiteral("accounts.google.com"));
        QCOMPARE(query.queryItemValue(QStringLiteral("code_challenge_method")), QStringLiteral("S256"));
        QCOMPARE(query.queryItemValue(QStringLiteral("scope"), QUrl::FullyDecoded), GoogleDriveAuth::requiredScope());
        QVERIFY(query.queryItemValue(QStringLiteral("state")).size() >= 32);
        const QUrl callback = callbackUrl(browser);
        QCOMPARE(callback.host(), QStringLiteral("127.0.0.1"));
        QVERIFY(callback.port() > 0);
        QVERIFY(sendCallback(callback).startsWith("HTTP/1.1 200"));
        QTRY_COMPARE(store.writes, 1);
        QCOMPARE(authorized.size(), 0);
        const QUrlQuery tokenRequest(QString::fromUtf8(network.requestBodies.first()));
        const QByteArray verifier = tokenRequest.queryItemValue(QStringLiteral("code_verifier")).toLatin1();
        QVERIFY(verifier.size() >= 43);
        QCOMPARE(QString::fromLatin1(QCryptographicHash::hash(verifier, QCryptographicHash::Sha256)
            .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals)),
            query.queryItemValue(QStringLiteral("code_challenge")));
        store.finishWrite();
        QCOMPARE(authorized.size(), 1);
        QCOMPARE(authorized.first().at(1).toString().size(), 43);
        QJsonObject decoded;
        QString password;
        QVERIFY(auth.decodeRecord(store.record, &decoded, &password));
        QCOMPARE(decoded.value(QStringLiteral("refresh_token")).toString(), QStringLiteral("new-refresh-secret"));
        QCOMPARE(network.requestUrls.size(), 1);
        QVERIFY(sendCallback(callback).isEmpty()); // consumed listener cannot be replayed
        QCOMPARE(network.requestUrls.size(), 1);
    }

    void rejectsMalformedCallbacksWithoutConsumingValidAttempt_data()
    {
        QTest::addColumn<QByteArray>("mutation");
        for (const auto &value : {"state", "error-state", "duplicate", "path", "host", "method", "body", "duplicate-host"})
            QTest::newRow(value) << QByteArray(value);
    }
    void rejectsMalformedCallbacksWithoutConsumingValidAttempt()
    {
        QFETCH(QByteArray, mutation);
        MemoryStore store;
        NetworkState network;
        network.tokenResponse = responseWithScope();
        QUrl browser;
        GoogleDriveAuth auth(client(), dependencies(&store, &network, &browser));
        QSignalSpy authorized(&auth, &GoogleDriveAuth::authorized);
        QSignalSpy failed(&auth, &GoogleDriveAuth::failed);
        auth.startSignIn();
        QTRY_VERIFY(browser.isValid());
        const QUrl callback = callbackUrl(browser);
        QUrl wrong = callback;
        QUrlQuery query(wrong);
        if (mutation == "state" || mutation == "error-state") {
            query.removeAllQueryItems(QStringLiteral("state"));
            query.addQueryItem(QStringLiteral("state"), QStringLiteral("attacker-state"));
            if (mutation == "error-state") {
                query.removeAllQueryItems(QStringLiteral("code"));
                query.addQueryItem(QStringLiteral("error"), QStringLiteral("access_denied"));
            }
        } else if (mutation == "duplicate") {
            query.addQueryItem(QStringLiteral("state"), QStringLiteral("attacker-state"));
        }
        wrong.setQuery(query);
        if (mutation == "path")
            wrong.setPath(QStringLiteral("/wrong"));
        QByteArray request = requestFor(wrong);
        if (mutation == "host")
            request.replace("Host: 127.0.0.1:", "Host: evil.example:");
        if (mutation == "method")
            request.replace("GET ", "POST ");
        if (mutation == "body")
            request += "body";
        if (mutation == "duplicate-host")
            request.replace("\r\n\r\n", "\r\nHost: evil.example\r\n\r\n");
        QVERIFY(sendCallback(callback, request).startsWith("HTTP/1.1 400"));
        QCOMPARE(failed.size(), 0);
        QCOMPARE(network.requestUrls.size(), 0);
        QVERIFY(sendCallback(callback).startsWith("HTTP/1.1 200"));
        QTRY_COMPARE(authorized.size(), 1);
    }

    void missingOrReducedGrantedScopeNeverPersists_data()
    {
        QTest::addColumn<QString>("scope");
        QTest::newRow("missing") << QString();
        QTest::newRow("reduced") << QStringLiteral("https://www.googleapis.com/auth/drive.metadata.readonly");
        QTest::newRow("broader") << QStringLiteral("https://www.googleapis.com/auth/drive");
    }
    void missingOrReducedGrantedScopeNeverPersists()
    {
        QFETCH(QString, scope);
        MemoryStore store;
        NetworkState network;
        network.tokenResponse = responseWithScope(scope);
        QUrl browser;
        GoogleDriveAuth auth(client(), dependencies(&store, &network, &browser));
        QSignalSpy failed(&auth, &GoogleDriveAuth::failed);
        QSignalSpy authorized(&auth, &GoogleDriveAuth::authorized);
        auth.startSignIn();
        QTRY_VERIFY(browser.isValid());
        sendCallback(callbackUrl(browser));
        QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(store.writes, 0);
        QCOMPARE(authorized.size(), 0);
    }

    void cancelledAttemptCannotFinish()
    {
        MemoryStore store;
        NetworkState network;
        QUrl browser;
        GoogleDriveAuth auth(client(), dependencies(&store, &network, &browser));
        QSignalSpy authorized(&auth, &GoogleDriveAuth::authorized);
        auth.startSignIn();
        QTRY_VERIFY(browser.isValid());
        const QUrl callback = callbackUrl(browser);
        auth.cancel();
        QVERIFY(sendCallback(callback).isEmpty());
        QCOMPARE(authorized.size(), 0);
        QCOMPARE(store.writes, 0);
        QVERIFY(network.requestUrls.isEmpty());
    }

    void walletWriteFailureNeverAuthorizes()
    {
        MemoryStore store;
        store.writeResult = GoogleDriveCredentialStore::Result::Failed;
        NetworkState network;
        network.tokenResponse = responseWithScope();
        QUrl browser;
        GoogleDriveAuth auth(client(), dependencies(&store, &network, &browser));
        QSignalSpy failed(&auth, &GoogleDriveAuth::failed);
        QSignalSpy authorized(&auth, &GoogleDriveAuth::authorized);
        auth.startSignIn();
        QTRY_VERIFY(browser.isValid());
        sendCallback(callbackUrl(browser));
        QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(authorized.size(), 0);
        QVERIFY(store.record.isEmpty());
    }

    void reauthorizationPreservesEncryptionKeyAndCancellationDoesNotRevoke()
    {
        MemoryStore store;
        store.record = recordWith();
        store.delayWrite = true;
        NetworkState network;
        network.tokenResponse = responseWithScope();
        QUrl browser;
        GoogleDriveAuth auth(client(), dependencies(&store, &network, &browser));
        QSignalSpy authorized(&auth, &GoogleDriveAuth::authorized);
        QSignalSpy idle(&auth, &GoogleDriveAuth::idle);
        auth.startSignIn();
        QTRY_VERIFY(browser.isValid());
        sendCallback(callbackUrl(browser));
        QTRY_COMPARE(store.writes, 1);
        auth.cancel();
        QVERIFY(auth.isBusy());
        store.finishWrite();
        QVERIFY(!auth.isBusy());
        QCOMPARE(idle.size(), 1);
        QCOMPARE(authorized.size(), 0);
        QCOMPARE(store.removes, 0);
        QCOMPARE(network.requestUrls.size(), 1);
        QJsonObject decoded;
        QString password;
        QVERIFY(auth.decodeRecord(store.record, &decoded, &password));
        QCOMPARE(password, QString(43, u'a'));
    }

    void authorizationTimeoutClosesListener()
    {
        MemoryStore store;
        NetworkState network;
        QUrl browser;
        GoogleDriveAuth auth(client(), dependencies(&store, &network, &browser));
        QSignalSpy failed(&auth, &GoogleDriveAuth::failed);
        auth.startSignIn();
        QTRY_VERIFY(browser.isValid());
        const QUrl callback = callbackUrl(browser);
        auth.m_timeout.start(1);
        QTRY_COMPARE(failed.size(), 1);
        QVERIFY(!auth.isBusy());
        QVERIFY(sendCallback(callback).isEmpty());
        QCOMPARE(store.writes, 0);
    }

    void signOutUsesCurrentCanonicalToken_data()
    {
        QTest::addColumn<int>("status");
        QTest::newRow("revoked") << 200;
        QTest::newRow("unconfirmed") << 400;
    }
    void signOutUsesCurrentCanonicalToken()
    {
        QFETCH(int, status);
        MemoryStore store;
        store.record = recordWith();
        NetworkState network;
        network.revokeStatus = status;
        QUrl browser;
        GoogleDriveAuth auth(client(), dependencies(&store, &network, &browser));
        QSignalSpy requested(&auth, &GoogleDriveAuth::revocationTokenRequested);
        QSignalSpy signedOut(&auth, &GoogleDriveAuth::signedOutLocally);
        QSignalSpy failed(&auth, &GoogleDriveAuth::failed);
        auth.signOut();
        QTRY_COMPARE(requested.size(), 1);
        QCOMPARE(requested.first().first().toString(), QString(43, u'a'));
        QVERIFY(network.requestUrls.isEmpty());
        QJsonObject token = sampleToken();
        token.insert(QStringLiteral("refresh_token"), QStringLiteral("rotated-current-secret"));
        auth.completeSignOut(QJsonDocument(token).toJson(QJsonDocument::Compact));
        QTRY_COMPARE(signedOut.size(), 1);
        QCOMPARE(failed.size(), 0);
        QCOMPARE(signedOut.first().first().toString().isEmpty(), status == 200);
        QCOMPARE(network.requestUrls.first(), QUrl(QStringLiteral("https://oauth2.googleapis.com/revoke")));
        QCOMPARE(QUrlQuery(QString::fromUtf8(network.requestBodies.first())).queryItemValue(QStringLiteral("token")), QStringLiteral("rotated-current-secret"));
        QVERIFY(store.record.isEmpty());
        QVERIFY(browser.isEmpty());
    }

    void signOutExportFailureClearsOnlyWithWarning()
    {
        MemoryStore store;
        store.record = recordWith();
        NetworkState network;
        QUrl browser;
        GoogleDriveAuth auth(client(), dependencies(&store, &network, &browser));
        QSignalSpy requested(&auth, &GoogleDriveAuth::revocationTokenRequested);
        QSignalSpy signedOut(&auth, &GoogleDriveAuth::signedOutLocally);
        auth.signOut();
        QTRY_COMPARE(requested.size(), 1);
        auth.completeSignOut({});
        QTRY_COMPARE(signedOut.size(), 1);
        QVERIFY(!signedOut.first().first().toString().isEmpty());
        QVERIFY(network.requestUrls.isEmpty());
        QVERIFY(store.record.isEmpty());
    }

    void deletionFailureDoesNotClaimSignedOut()
    {
        MemoryStore store;
        store.record = recordWith();
        store.removeResult = GoogleDriveCredentialStore::Result::Failed;
        NetworkState network;
        QUrl browser;
        GoogleDriveAuth auth(client(), dependencies(&store, &network, &browser));
        QSignalSpy requested(&auth, &GoogleDriveAuth::revocationTokenRequested);
        QSignalSpy signedOut(&auth, &GoogleDriveAuth::signedOutLocally);
        QSignalSpy failed(&auth, &GoogleDriveAuth::failed);
        auth.signOut();
        QTRY_COMPARE(requested.size(), 1);
        auth.completeSignOut({});
        QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(signedOut.size(), 0);
        QVERIFY(!store.record.isEmpty());
    }

    void oversizedTokenBodyAbortsBeforeItCanBeGranted()
    {
        MemoryStore store;
        NetworkState network;
        QJsonObject response = QJsonDocument::fromJson(responseWithScope()).object();
        response.insert(QStringLiteral("padding"), QString(70000, u'x'));
        network.tokenResponse = QJsonDocument(response).toJson(QJsonDocument::Compact);
        network.configureReply = [](QNetworkReply *reply) {
            GoogleDriveAuth::limitReplyBuffer(reply);
            QCOMPARE(reply->readBufferSize(), qint64(65537));
        };
        QUrl browser;
        GoogleDriveAuth auth(client(), dependencies(&store, &network, &browser));
        QSignalSpy failed(&auth, &GoogleDriveAuth::failed);
        QSignalSpy authorized(&auth, &GoogleDriveAuth::authorized);
        auth.startSignIn();
        QTRY_VERIFY(browser.isValid());
        QVERIFY(sendCallback(callbackUrl(browser)).startsWith("HTTP/1.1 200"));
        QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(network.abortedReplies, 1);
        QCOMPARE(authorized.size(), 0);
        QCOMPARE(store.writes, 0);
    }

    void oversizedRevocationBodyAbortsAndReportsLocalOnlySignOut()
    {
        MemoryStore store;
        store.record = recordWith();
        NetworkState network;
        network.revokeResponse = QByteArray(70000, 'x');
        network.configureReply = [](QNetworkReply *reply) { GoogleDriveAuth::limitReplyBuffer(reply); };
        QUrl browser;
        GoogleDriveAuth auth(client(), dependencies(&store, &network, &browser));
        QSignalSpy requested(&auth, &GoogleDriveAuth::revocationTokenRequested);
        QSignalSpy signedOut(&auth, &GoogleDriveAuth::signedOutLocally);
        QSignalSpy failed(&auth, &GoogleDriveAuth::failed);
        auth.signOut();
        QTRY_COMPARE(requested.size(), 1);
        auth.completeSignOut(QJsonDocument(sampleToken()).toJson(QJsonDocument::Compact));
        QTRY_COMPARE(signedOut.size(), 1);
        QCOMPARE(network.abortedReplies, 1);
        QVERIFY(!signedOut.first().first().toString().isEmpty());
        QCOMPARE(failed.size(), 0);
        QVERIFY(store.record.isEmpty());
    }

    void excessiveDeclaredResponseLengthAbortsImmediately()
    {
        QNetworkRequest request(QUrl(QStringLiteral("https://oauth2.googleapis.com/token")));
        int aborted = 0;
        // A fixture subclass exposes metadata through QObject's signal path;
        // the body cap itself is also exercised without any length header.
        struct MetadataReply final : FakeReply {
            using FakeReply::FakeReply;
            void announceLength(qint64 value)
            {
                setHeader(QNetworkRequest::ContentLengthHeader, value);
                emit metaDataChanged();
            }
        };
        MetadataReply oversized(request, "{}", 200, nullptr);
        oversized.aborted = [&] { ++aborted; };
        GoogleDriveAuth::limitReplyBuffer(&oversized);
        oversized.announceLength(65537);
        QCOMPARE(aborted, 1);
        QCOMPARE(oversized.error(), QNetworkReply::OperationCanceledError);
    }
};

QTEST_GUILESS_MAIN(GoogleDriveAuthTest)
#include "GoogleDriveAuthTest.moc"
