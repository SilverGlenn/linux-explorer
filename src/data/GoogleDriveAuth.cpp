#include "GoogleDriveAuth.h"
#include "GoogleDriveSecretStore.h"

#include <QAbstractOAuthReplyHandler>
#include <QDateTime>
#include <QDesktopServices>
#include <QHostAddress>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QOAuth2AuthorizationCodeFlow>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSet>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrlQuery>
#include <qt6keychain/keychain.h>
#include <cstring>

namespace {
constexpr auto walletService = "explorer-win7-google-drive";
constexpr auto walletKey = "desktop-account-v1";
constexpr auto canonicalWalletKey = "desktop-account-v2";
constexpr int maxRecordSize = 65536;

QString randomSecret()
{
    QByteArray bytes(32, Qt::Uninitialized);
    // system() uses the operating system's cryptographic random source.
    for (int i = 0; i < bytes.size(); i += 4) {
        const quint32 value = QRandomGenerator::system()->generate();
        memcpy(bytes.data() + i, &value, sizeof(value));
    }
    return QString::fromLatin1(bytes.toBase64(QByteArray::Base64UrlEncoding
                                            | QByteArray::OmitTrailingEquals));
}

bool boundedSecret(const QJsonValue &value)
{
    if (!value.isString())
        return false;
    const QString text = value.toString();
    if (text.isEmpty() || text.size() > 16384)
        return false;
    for (const QChar c : text) {
        if (c.unicode() < 0x21 || c.unicode() > 0x7e)
            return false;
    }
    return true;
}

class KeychainStore final : public GoogleDriveCredentialStore
{
public:
    explicit KeychainStore(bool sync, QObject *parent) : GoogleDriveCredentialStore(parent)
        , m_key(sync ? QStringLiteral("desktop-sync-account-v1") : QString::fromLatin1(canonicalWalletKey))
        , m_legacyKey(sync ? QStringLiteral("desktop-sync-account-legacy-v1") : QString::fromLatin1(walletKey))
    {}
    void read(Completion completion) override
    {
        GoogleDriveSecretStore::run(this, m_key,
            GoogleDriveSecretStore::Action::Read, false, {}, [this, completion](Result result, QByteArray record) {
                m_requiresMigration = false;
                if (result == Result::Missing || result == Result::Unavailable)
                    readLegacy(completion);
                else
                    completion(result, record);
            });
    }
    void readSilently(Completion completion) override
    {
        GoogleDriveSecretStore::run(this, m_key,
            GoogleDriveSecretStore::Action::Read, true, {},
            [this, completion](Result result, QByteArray record) {
                if (result != Result::Missing) {
                    completion(result, record);
                    return;
                }
                // Older Secret Service records are also safe to reuse while
                // already unlocked. Native KWallet-only records still defer;
                // automatic mode never invokes QtKeychain or migrates data.
                GoogleDriveSecretStore::run(this, m_legacyKey,
                    GoogleDriveSecretStore::Action::Read, true, {}, completion);
            });
    }
    bool requiresMigration() const override { return m_requiresMigration; }
    void readLegacy(Completion completion)
    {
        auto *job = new QKeychain::ReadPasswordJob(QString::fromLatin1(walletService), this);
        configure(job);
        connect(job, &QKeychain::Job::finished, this, [this, job, completion](QKeychain::Job *) {
            const Result result = job->error() == QKeychain::NoError ? Result::Success
                : job->error() == QKeychain::EntryNotFound ? Result::Missing : Result::Failed;
            if (result != Result::Success) {
                completion(result, {});
                return;
            }
            m_requiresMigration = true;
            completion(Result::Success, job->binaryData());
        });
        job->start();
    }
    void write(const QByteArray &record, Completion completion) override
    {
        writeCanonical(record, [this, record, completion](Result result, QByteArray) {
            if (result != Result::Unavailable) {
                completion(result, {});
                return;
            }
            auto *job = new QKeychain::WritePasswordJob(QString::fromLatin1(walletService), this);
            configure(job);
            job->setBinaryData(record);
            connect(job, &QKeychain::Job::finished, this, [job, completion](QKeychain::Job *) {
                completion(job->error() == QKeychain::NoError ? Result::Success : Result::Failed, {});
            });
            job->start();
        });
    }
    void remove(Completion completion) override
    {
        GoogleDriveSecretStore::run(this, m_key,
            GoogleDriveSecretStore::Action::Remove, false, {}, [this, completion](Result result, QByteArray) {
                removeLegacy([result, completion](Result legacy, QByteArray) {
                    completion(result == Result::Success && legacy == Result::Success
                        ? Result::Success : Result::Failed, {});
                });
            });
    }
private:
    void writeCanonical(const QByteArray &record, Completion completion)
    {
        GoogleDriveSecretStore::run(this, m_key,
            GoogleDriveSecretStore::Action::Write, false, record, [this, completion](Result result, QByteArray) {
                if (result != Result::Success) {
                    completion(result, {});
                    return;
                }
                removeLegacy([this, completion](Result removed, QByteArray) {
                    if (removed == Result::Success) {
                        m_requiresMigration = false;
                        completion(Result::Success, {});
                        return;
                    }
                    // Do not leave a silently duplicated account when a legacy
                    // wallet refuses deletion. Keep the original record intact.
                    GoogleDriveSecretStore::run(this, m_key,
                        GoogleDriveSecretStore::Action::Remove, false, {},
                        [completion](Result, QByteArray) { completion(Result::Failed, {}); });
                });
            });
    }
    void removeLegacy(Completion completion)
    {
        auto *job = new QKeychain::DeletePasswordJob(QString::fromLatin1(walletService), this);
        configure(job);
        connect(job, &QKeychain::Job::finished, this, [job, completion](QKeychain::Job *) {
            completion(job->error() == QKeychain::NoError || job->error() == QKeychain::EntryNotFound
                           ? Result::Success : Result::Failed, {});
        });
        job->start();
    }
    void configure(QKeychain::Job *job)
    {
        job->setKey(m_legacyKey);
        job->setInsecureFallback(false);
        job->setAutoDelete(true);
    }
    bool m_requiresMigration = false;
    QString m_key;
    QString m_legacyKey;
};
}

// Bound the unread body while it is arriving, rather than trusting a length
// header or waiting until the completed response has been fully buffered.
class GoogleDriveBoundedNetwork final : public QNetworkAccessManager
{
public:
    using QNetworkAccessManager::QNetworkAccessManager;
protected:
    QNetworkReply *createRequest(Operation operation, const QNetworkRequest &request, QIODevice *body) override
    {
        auto *reply = QNetworkAccessManager::createRequest(operation, request, body);
        GoogleDriveAuth::limitReplyBuffer(reply);
        return reply;
    }
};

// Qt's flow owns the protocol state/PKCE exchange. This small HTTP boundary
// rejects malformed, unrelated and repeated requests before forwarding a
// callback to Qt, including error callbacks (which Qt handles before state).
class GoogleDriveLoopbackReply final : public QAbstractOAuthReplyHandler
{
public:
    explicit GoogleDriveLoopbackReply(QObject *parent) : QAbstractOAuthReplyHandler(parent)
    {
        m_server.setMaxPendingConnections(4);
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (m_server.hasPendingConnections()) {
                auto *socket = m_server.nextPendingConnection();
                if (findChildren<QTcpSocket *>().size() >= 4) {
                    socket->abort();
                    socket->deleteLater();
                    continue;
                }
                socket->setParent(this);
                socket->setReadBufferSize(8193);
                auto *timer = new QTimer(socket);
                timer->setSingleShot(true);
                connect(timer, &QTimer::timeout, socket, &QTcpSocket::abort);
                timer->start(5000);
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] { readRequest(socket); });
            }
        });
    }
    bool listen()
    {
        m_path = "/oauth/" + randomSecret().toLatin1();
        if (!m_server.listen(QHostAddress::LocalHost, 0))
            return false;
        m_authority = "127.0.0.1:" + QByteArray::number(m_server.serverPort());
        return true;
    }
    void close()
    {
        m_server.close();
        for (auto *socket : findChildren<QTcpSocket *>())
            socket->abort();
    }
    QString callback() const override
    {
        return QString::fromLatin1("http://" + m_authority + m_path);
    }
    void setExpectedState(const QString &state) { m_state = state; }
    std::function<void(const QVariantMap &)> acceptTokens;
    std::function<void(const QString &)> rejectTokens;

    void networkReplyFinished(QNetworkReply *reply) override
    {
        // Never forward provider error descriptions or raw responses to logs.
        if (reply->error() != QNetworkReply::NoError
            || reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200
            || reply->url() != QUrl(QStringLiteral("https://oauth2.googleapis.com/token"))
            || reply->bytesAvailable() > maxRecordSize) {
            rejectTokens(QObject::tr("Google could not authorize this account. Sign in again or try later."));
            return;
        }
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(reply->readAll(), &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()
            || document.object().contains(QStringLiteral("error"))) {
            rejectTokens(QObject::tr("Google returned an invalid authorization response."));
            return;
        }
        const QVariantMap values = document.object().toVariantMap();
        acceptTokens(values);
        // acceptTokens may reject the response and dispose of the whole flow.
        if (m_tokenAccepted)
            emit tokensReceived(values);
    }
    void setTokenAccepted(bool accepted) { m_tokenAccepted = accepted; }

private:
    static void respond(QTcpSocket *socket, bool accepted)
    {
        const QByteArray body = accepted
            ? "<!doctype html><title>Google Drive</title>You can close this tab and return to Explorer."
            : "<!doctype html><title>Google Drive</title>This sign-in response was not accepted.";
        socket->write(QByteArray(accepted ? "HTTP/1.1 200 OK\r\n" : "HTTP/1.1 400 Bad Request\r\n")
            + "Content-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\n"
              "Content-Security-Policy: default-src 'none'; frame-ancestors 'none'\r\n"
              "Connection: close\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body);
        socket->flush();
        socket->disconnectFromHost();
    }
    void readRequest(QTcpSocket *socket)
    {
        QByteArray data = socket->property("oauthRequest").toByteArray() + socket->readAll();
        if (data.size() > 8192) {
            respond(socket, false);
            return;
        }
        const int end = data.indexOf("\r\n\r\n");
        if (end < 0) {
            socket->setProperty("oauthRequest", data);
            return;
        }
        // One complete GET, no body, pipelining, chunking, or duplicate headers.
        bool valid = !m_used && !m_state.isEmpty() && socket->peerAddress() == QHostAddress::LocalHost
            && end + 4 == data.size();
        const QList<QByteArray> lines = data.left(end).split('\n');
        const QList<QByteArray> first = lines.value(0).trimmed().split(' ');
        valid = valid && first.size() == 3 && first.value(0) == "GET"
            && first.value(2) == "HTTP/1.1";
        QSet<QByteArray> headers;
        bool host = false;
        for (int i = 1; i < lines.size(); ++i) {
            const QByteArray rawLine = lines.at(i);
            const QByteArray line = rawLine.trimmed();
            const int colon = line.indexOf(':');
            const QByteArray name = line.left(colon).toLower();
            const QByteArray value = line.mid(colon + 1).trimmed();
            if (colon < 1 || headers.contains(name) || rawLine.startsWith(' ') || rawLine.startsWith('\t'))
                valid = false;
            headers.insert(name);
            if (name == "host") {
                host = value == m_authority;
            } else if (name == "transfer-encoding" || (name == "content-length" && value != "0")) {
                valid = false;
            }
        }
        const QUrl target = QUrl::fromEncoded(first.value(1), QUrl::StrictMode);
        valid = valid && host && target.isValid() && target.isRelative() && target.authority().isEmpty()
            && !target.hasFragment() && target.path(QUrl::FullyEncoded).toLatin1() == m_path;
        QVariantMap values;
        const auto items = QUrlQuery(target).queryItems(QUrl::FullyDecoded);
        for (const auto &item : items) {
            if (values.contains(item.first) || item.first.isEmpty())
                valid = false;
            values.insert(item.first, item.second);
        }
        valid = valid && values.value(QStringLiteral("state")).toString() == m_state
            && (values.contains(QStringLiteral("code")) != values.contains(QStringLiteral("error")))
            && (!values.contains(QStringLiteral("code")) || !values.value(QStringLiteral("code")).toString().isEmpty());
        respond(socket, valid);
        if (!valid)
            return;
        m_used = true;
        m_server.close();
        // Only these validated fields enter Qt; descriptions from untrusted
        // callbacks cannot reach Qt's diagnostic logging.
        QVariantMap safe {{QStringLiteral("state"), m_state}};
        if (values.contains(QStringLiteral("error")))
            safe.insert(QStringLiteral("error"), QStringLiteral("access_denied"));
        else
            safe.insert(QStringLiteral("code"), values.value(QStringLiteral("code")));
        emit callbackReceived(safe);
    }
    QTcpServer m_server;
    QByteArray m_path;
    QByteArray m_authority;
    QString m_state;
    bool m_used = false;
    bool m_tokenAccepted = false;
};

GoogleDriveAuth::GoogleDriveAuth(ClientConfig config, QObject *parent)
    : GoogleDriveAuth(std::move(config), Dependencies{}, AccountPurpose::Browse, parent) {}

GoogleDriveAuth::GoogleDriveAuth(ClientConfig config, AccountPurpose purpose, QObject *parent)
    : GoogleDriveAuth(std::move(config), Dependencies{}, purpose, parent) {}

GoogleDriveAuth::GoogleDriveAuth(ClientConfig config, Dependencies dependencies, QObject *parent)
    : GoogleDriveAuth(std::move(config), std::move(dependencies), AccountPurpose::Browse, parent) {}

GoogleDriveAuth::GoogleDriveAuth(ClientConfig config, Dependencies dependencies, AccountPurpose purpose, QObject *parent)
    : QObject(parent), m_config(std::move(config)), m_purpose(purpose), m_dependencies(std::move(dependencies))
{
    m_store = m_dependencies.store ? m_dependencies.store : new KeychainStore(purpose == AccountPurpose::Sync, this);
    if (!m_dependencies.openBrowser)
        m_dependencies.openBrowser = [](const QUrl &url) { return QDesktopServices::openUrl(url); };
    if (!m_dependencies.networkFactory)
        m_dependencies.networkFactory = [](QObject *owner) { return new GoogleDriveBoundedNetwork(owner); };
    m_timeout.setSingleShot(true);
    connect(&m_timeout, &QTimer::timeout, this, [this] {
        if (m_silentRead && m_operation == Operation::Reading) {
            finishSilentRead(SavedAccountResult::Unavailable);
        } else if (m_operation == Operation::Revoking || m_operation == Operation::AwaitingRevocationToken) {
            clearFlow();
            deleteRecord(tr("Signed out locally, but Google could not confirm access was revoked. Remove Explorer in your Google account's app permissions."));
        } else {
            fail(tr("Google sign-in timed out. Please try again."));
        }
    });
}

GoogleDriveAuth::~GoogleDriveAuth() { clearFlow(); }

QString GoogleDriveAuth::requiredScope() { return QStringLiteral("https://www.googleapis.com/auth/drive.readonly"); }
QString GoogleDriveAuth::syncScope() { return QStringLiteral("https://www.googleapis.com/auth/drive"); }
QString GoogleDriveAuth::authorizationScope() const { return m_purpose == AccountPurpose::Sync ? syncScope() : requiredScope(); }
bool GoogleDriveAuth::available() const
{
    static const QRegularExpression id(QStringLiteral("\\A[A-Za-z0-9_-]{1,1024}\\.apps\\.googleusercontent\\.com\\z"));
    static const QRegularExpression secret(QStringLiteral("\\A[A-Za-z0-9._~-]{1,4096}\\z"));
    return id.match(m_config.clientId).hasMatch() && secret.match(m_config.clientSecret).hasMatch();
}
bool GoogleDriveAuth::isBusy() const { return m_operation != Operation::Idle; }

void GoogleDriveAuth::limitReplyBuffer(QNetworkReply *reply)
{
    // One extra byte lets us distinguish a complete maximum-sized body from
    // an oversized stream without allowing unbounded Qt reply buffering.
    reply->setReadBufferSize(maxRecordSize + 1);
    QObject::connect(reply, &QIODevice::readyRead, reply, [reply] {
        if (reply->bytesAvailable() > maxRecordSize)
            reply->abort();
    });
    QObject::connect(reply, &QNetworkReply::metaDataChanged, reply, [reply] {
        bool validLength = false;
        const qint64 length = reply->header(QNetworkRequest::ContentLengthHeader).toLongLong(&validLength);
        if (validLength && length > maxRecordSize)
            reply->abort();
    });
}

void GoogleDriveAuth::clearFlow()
{
    m_timeout.stop();
    if (m_reply)
        m_reply->close();
    m_reply = nullptr;
    if (m_network) {
        for (auto *reply : m_network->findChildren<QNetworkReply *>()) {
            reply->disconnect();
            reply->abort();
        }
    }
    if (m_flow) {
        m_flow->disconnect(this);
        m_flow->deleteLater();
    }
    m_flow = nullptr;
    m_network = nullptr;
    m_validTokenResponse = false;
}

void GoogleDriveAuth::fail(const QString &message)
{
    ++m_generation;
    clearFlow();
    m_operation = Operation::Idle;
    m_cancelWrite = false;
    m_pendingSignOut = false;
    m_pendingForget = false;
    emit failed(message);
    emit idle();
}

void GoogleDriveAuth::makeFlow()
{
    clearFlow();
    m_flow = new QOAuth2AuthorizationCodeFlow(this);
    m_network = m_dependencies.networkFactory(m_flow);
    m_network->setRedirectPolicy(QNetworkRequest::ManualRedirectPolicy);
    m_network->setTransferTimeout(30000);
    m_flow->setNetworkAccessManager(m_network);
    m_flow->setClientIdentifier(m_config.clientId);
    m_flow->setClientIdentifierSharedKey(m_config.clientSecret);
    m_flow->setAuthorizationUrl(QUrl(QStringLiteral("https://accounts.google.com/o/oauth2/v2/auth")));
    m_flow->setAccessTokenUrl(QUrl(QStringLiteral("https://oauth2.googleapis.com/token")));
    m_flow->setScope(authorizationScope());
    m_flow->setState(randomSecret());
    m_flow->setPkceMethod(QOAuth2AuthorizationCodeFlow::PkceMethod::S256, 64);
    m_reply = new GoogleDriveLoopbackReply(m_flow);
    m_reply->acceptTokens = [this](const QVariantMap &values) { finishTokens(values); };
    m_reply->rejectTokens = [this](const QString &error) { fail(error); };
    m_flow->setReplyHandler(m_reply);
    m_flow->setModifyParametersFunction([](QAbstractOAuth::Stage stage, QMultiMap<QString, QVariant> *parameters) {
        if (stage == QAbstractOAuth::Stage::RequestingAuthorization) {
            parameters->insert(QStringLiteral("access_type"), QStringLiteral("offline"));
            parameters->insert(QStringLiteral("prompt"), QStringLiteral("consent select_account"));
        }
    });
    connect(m_flow, &QAbstractOAuth::authorizeWithBrowser, this, [this](const QUrl &url) {
        m_reply->setExpectedState(m_flow->state());
        if (!m_dependencies.openBrowser(url))
            fail(tr("The system browser could not be opened. Please check your default browser."));
    });
    connect(m_flow, &QAbstractOAuth::requestFailed, this, [this](QAbstractOAuth::Error) {
        fail(tr("Google sign-in was cancelled or could not be completed. Please try again."));
    });
    connect(m_flow, &QAbstractOAuth::granted, this, [this] {
        if (m_validTokenResponse)
            saveRecord();
    });
}

void GoogleDriveAuth::startSignIn()
{
    if (isBusy())
        return;
    if (!available()) {
        emit failed(tr("Google sign-in is unavailable in this build. The application maintainer must provide its Google Desktop client."));
        return;
    }
    readRecord(false, true);
}

void GoogleDriveAuth::beginAuthorization()
{
    // Reauthorization keeps the saved encryption key so an interrupted helper
    // configuration replacement cannot make the previous config unreadable.
    m_token = {};
    makeFlow();
    if (!m_reply->listen()) {
        fail(tr("Explorer could not open its local Google sign-in callback. Please try again."));
        return;
    }
    m_operation = Operation::Authorizing;
    m_timeout.start(180000);
    emit statusChanged(tr("Finish signing in with Google in your browser."));
    m_flow->grant();
}

void GoogleDriveAuth::connectExisting()
{
    if (isBusy())
        return;
    if (!available()) {
        emit failed(tr("Google sign-in is unavailable in this build. The application maintainer must provide its Google Desktop client."));
        return;
    }
    readRecord(false);
}

void GoogleDriveAuth::connectSavedAccount()
{
    if (isBusy())
        return;
    if (!available()) {
        finishSilentRead(SavedAccountResult::Unavailable);
        return;
    }
    readRecord(false, false, true, true);
}

void GoogleDriveAuth::connectSavedAccountInteractively()
{
    if (isBusy())
        return;
    if (!available()) {
        finishSilentRead(SavedAccountResult::Unavailable);
        return;
    }
    readRecord(false, false, true, false);
}

void GoogleDriveAuth::finishSilentRead(SavedAccountResult result)
{
    ++m_generation;
    m_timeout.stop();
    m_silentRead = false;
    m_operation = Operation::Idle;
    m_token = {};
    m_password.clear();
    emit savedAccountUnavailable(result);
    emit idle();
}

bool GoogleDriveAuth::validateToken(const QJsonObject &token)
{
    static const QSet<QString> expected {QStringLiteral("access_token"), QStringLiteral("refresh_token"),
        QStringLiteral("token_type"), QStringLiteral("expiry")};
    static const QRegularExpression expiry(QStringLiteral("\\A[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}(?:\\.[0-9]{1,9})?(?:Z|[+-][0-9]{2}:[0-9]{2})\\z"));
    const QStringList keys = token.keys();
    return QSet<QString>(keys.begin(), keys.end()) == expected
        && boundedSecret(token.value(QStringLiteral("access_token")))
        && boundedSecret(token.value(QStringLiteral("refresh_token")))
        && token.value(QStringLiteral("token_type")).toString().compare(QStringLiteral("Bearer"), Qt::CaseInsensitive) == 0
        && expiry.match(token.value(QStringLiteral("expiry")).toString()).hasMatch()
        && QDateTime::fromString(token.value(QStringLiteral("expiry")).toString(), Qt::ISODateWithMs).isValid();
}

bool GoogleDriveAuth::decodeRecord(const QByteArray &record, QJsonObject *token, QString *password) const
{
    if (record.isEmpty() || record.size() > maxRecordSize)
        return false;
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(record, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return false;
    const auto object = document.object();
    const QString secret = object.value(QStringLiteral("config_password")).toString();
    if (object.value(QStringLiteral("version")).toInt() != 1
        || object.value(QStringLiteral("client_id")).toString() != m_config.clientId
        || object.value(QStringLiteral("scope")).toString() != authorizationScope()
        || secret.size() != 43 || !validateToken(object.value(QStringLiteral("token")).toObject()))
        return false;
    for (const QChar c : secret) {
        if (!(c.isLetterOrNumber() && c.unicode() < 128) && c != u'-' && c != u'_')
            return false;
    }
    *token = object.value(QStringLiteral("token")).toObject();
    *password = secret;
    return true;
}

QByteArray GoogleDriveAuth::encodeRecord() const
{
    return QJsonDocument(QJsonObject {{QStringLiteral("version"), 1},
        {QStringLiteral("client_id"), m_config.clientId}, {QStringLiteral("scope"), authorizationScope()},
        {QStringLiteral("token"), m_token}, {QStringLiteral("config_password"), m_password}}).toJson(QJsonDocument::Compact);
}

void GoogleDriveAuth::readRecord(bool forSignOut, bool forSignIn, bool savedOnly, bool silent)
{
    const quint64 generation = ++m_generation;
    m_operation = Operation::Reading;
    m_silentRead = silent;
    if (silent)
        m_timeout.start(10000);
    else
        emit statusChanged(tr("Opening the desktop credential wallet…"));
    const QPointer<GoogleDriveAuth> guard(this);
    auto completion = [guard, generation, forSignOut, forSignIn, savedOnly, silent](GoogleDriveCredentialStore::Result result, QByteArray record) {
        if (!guard || generation != guard->m_generation)
            return;
        auto *self = guard.data();
        self->m_timeout.stop();
        self->m_silentRead = false;
        if (savedOnly && result != GoogleDriveCredentialStore::Result::Success) {
            self->finishSilentRead(result == GoogleDriveCredentialStore::Result::Missing
                ? SavedAccountResult::Missing
                : result == GoogleDriveCredentialStore::Result::InteractionRequired
                    ? SavedAccountResult::WalletLocked : SavedAccountResult::Unavailable);
            return;
        }
        if (result == GoogleDriveCredentialStore::Result::Failed
            || result == GoogleDriveCredentialStore::Result::Unavailable
            || result == GoogleDriveCredentialStore::Result::InteractionRequired) {
            self->fail(tr("The desktop credential wallet is unavailable or locked. Unlock it and try again."));
            return;
        }
        if (result == GoogleDriveCredentialStore::Result::Missing) {
            self->m_operation = Operation::Idle;
            if (forSignOut) {
                self->deleteRecord(tr("No local Google credentials remain. Google access could not be confirmed as revoked; remove Explorer in your Google account's app permissions if needed."));
            } else {
                self->m_password = randomSecret();
                self->beginAuthorization();
            }
            return;
        }
        if (!self->decodeRecord(record, &self->m_token, &self->m_password)) {
            if (savedOnly)
                self->finishSilentRead(SavedAccountResult::Invalid);
            else if (forSignOut)
                self->deleteRecord(tr("Saved credentials were invalid and were removed locally. Google access could not be revoked; remove Explorer in your Google account's app permissions."));
            else
                self->fail(tr("Saved Google credentials do not match this application or the requested Drive access. Sign out and sign in again."));
            return;
        }
        if (forSignOut) {
            // rclone's encrypted config contains the latest refresh token,
            // which may have rotated while mounted. Never revoke the older
            // initial token kept in the wallet record.
            self->m_operation = Operation::AwaitingRevocationToken;
            self->m_timeout.start(45000);
            emit self->revocationTokenRequested(self->m_password);
        } else if (forSignIn) {
            self->beginAuthorization();
        } else if (!silent && self->m_store->requiresMigration()) {
            // Migration is a real wallet write. Use Writing's cancellation and
            // process-lock retention, and never replay the older wallet token
            // over rclone's canonical refreshed token during reconnect.
            self->saveRecord(true);
        } else {
            self->m_silentRead = false;
            self->m_operation = Operation::Idle;
            emit self->authorized({}, self->m_password);
            emit self->idle();
        }
    };
    if (silent)
        m_store->readSilently(std::move(completion));
    else
        m_store->read(std::move(completion));
}

void GoogleDriveAuth::finishTokens(const QVariantMap &values)
{
    const QStringList scopes = values.value(QStringLiteral("scope")).toString().split(u' ', Qt::SkipEmptyParts);
    bool expiryOk = false;
    const int seconds = values.value(QStringLiteral("expires_in")).toInt(&expiryOk);
    // Google supports granular consent. Never infer a grant from our request,
    // including when Qt would otherwise assume a missing scope was granted.
    if (scopes.size() != 1 || scopes.first() != authorizationScope() || !expiryOk || seconds <= 0 || seconds > 86400) {
        fail(tr("Google did not confirm the requested Drive permission. Sign in again and allow that permission."));
        return;
    }
    QJsonObject token {{QStringLiteral("access_token"), values.value(QStringLiteral("access_token")).toString()},
        {QStringLiteral("refresh_token"), values.value(QStringLiteral("refresh_token"), m_token.value(QStringLiteral("refresh_token")).toVariant()).toString()},
        {QStringLiteral("token_type"), values.value(QStringLiteral("token_type")).toString()},
        {QStringLiteral("expiry"), QDateTime::currentDateTimeUtc().addSecs(seconds).toString(Qt::ISODateWithMs)}};
    if (!validateToken(token)) {
        fail(tr("Google did not return valid offline credentials. Please sign in again."));
        return;
    }
    m_token = token;
    m_validTokenResponse = true;
    m_reply->setTokenAccepted(true);
}

void GoogleDriveAuth::saveRecord(bool reconnect)
{
    clearFlow();
    m_operation = Operation::Writing;
    m_cancelWrite = false;
    const quint64 generation = m_generation;
    const QPointer<GoogleDriveAuth> guard(this);
    m_store->write(encodeRecord(), [guard, generation, reconnect](GoogleDriveCredentialStore::Result result, QByteArray) {
        if (!guard || generation != guard->m_generation)
            return;
        auto *self = guard.data();
        if (self->m_pendingForget && !self->m_pendingSignOut) {
            self->m_pendingForget = false;
            self->m_operation = Operation::Idle;
            self->forgetLocalAccount();
            return;
        }
        if (result != GoogleDriveCredentialStore::Result::Success) {
            self->fail(tr("Google credentials could not be saved securely in the desktop wallet. Unlock the wallet and try again."));
            return;
        }
        self->m_operation = Operation::Idle;
        if (self->m_pendingSignOut) {
            self->m_pendingSignOut = false;
            self->m_pendingForget = false;
            self->signOut();
            return;
        }
        if (self->m_cancelWrite) {
            self->m_cancelWrite = false;
            emit self->statusChanged(tr("Google sign-in cancelled. Google may retain permission you already granted."));
            emit self->idle();
            return;
        }
        emit self->statusChanged(tr("Google account connected."));
        emit self->authorized(reconnect ? QByteArray() : QJsonDocument(self->m_token).toJson(QJsonDocument::Compact), self->m_password);
        emit self->idle();
    });
}

void GoogleDriveAuth::cancel()
{
    if (m_operation == Operation::Writing) {
        m_cancelWrite = true;
        emit statusChanged(tr("Cancelling Google sign-in…"));
        return;
    }
    // A requested sign-out must finish removing the saved record.
    if (m_operation == Operation::AwaitingRevocationToken || m_operation == Operation::Revoking || m_operation == Operation::Deleting)
        return;
    ++m_generation;
    clearFlow();
    m_silentRead = false;
    m_operation = Operation::Idle;
    emit statusChanged(tr("Google sign-in cancelled."));
    emit idle();
}

void GoogleDriveAuth::signOut()
{
    if (m_operation == Operation::Writing) {
        m_pendingSignOut = true;
        return;
    }
    if (m_operation == Operation::AwaitingRevocationToken || m_operation == Operation::Revoking || m_operation == Operation::Deleting)
        return;
    cancel();
    readRecord(true);
}

void GoogleDriveAuth::forgetLocalAccount()
{
    if (m_operation == Operation::Writing) {
        m_pendingForget = true;
        return;
    }
    if (m_operation == Operation::Deleting)
        return;
    ++m_generation;
    clearFlow();
    m_silentRead = false;
    m_pendingForget = false;
    m_pendingSignOut = false;
    deleteRecord(); // deliberately no wallet read, browser, or Google revocation
}

void GoogleDriveAuth::completeSignOut(const QByteArray &currentRcloneToken)
{
    if (m_operation != Operation::AwaitingRevocationToken)
        return;
    m_timeout.stop();
    const auto document = QJsonDocument::fromJson(currentRcloneToken);
    if (currentRcloneToken.size() > maxRecordSize || !document.isObject()
        || !validateToken(document.object())) {
        deleteRecord(tr("Signed out locally, but the current Google token could not be read and access could not be revoked. Remove Explorer in your Google account's app permissions."));
        return;
    }
    m_token = document.object();
    revokeAndDelete();
}

void GoogleDriveAuth::revokeAndDelete()
{
    clearFlow();
    // A flow object supplies ownership for this operation's network manager.
    m_flow = new QOAuth2AuthorizationCodeFlow(this);
    m_network = m_dependencies.networkFactory(m_flow);
    m_network->setRedirectPolicy(QNetworkRequest::ManualRedirectPolicy);
    m_network->setTransferTimeout(30000);
    m_operation = Operation::Revoking;
    emit statusChanged(tr("Signing out of Google Drive…"));
    QNetworkRequest request(QUrl(QStringLiteral("https://oauth2.googleapis.com/revoke")));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded"));
    QUrlQuery body;
    body.addQueryItem(QStringLiteral("token"), m_token.value(QStringLiteral("refresh_token")).toString());
    auto *reply = m_network->post(request, body.toString(QUrl::FullyEncoded).toUtf8());
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        const bool revoked = reply->error() == QNetworkReply::NoError
            && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200;
        clearFlow();
        deleteRecord(revoked ? QString() : tr("Signed out locally, but Google could not confirm access was revoked. Remove Explorer in your Google account's app permissions."));
    });
    m_timeout.start(45000);
}

void GoogleDriveAuth::deleteRecord(const QString &revocationError)
{
    m_operation = Operation::Deleting;
    const quint64 generation = m_generation;
    const QPointer<GoogleDriveAuth> guard(this);
    m_store->remove([guard, generation, revocationError](GoogleDriveCredentialStore::Result result, QByteArray) {
        if (!guard || generation != guard->m_generation)
            return;
        auto *self = guard.data();
        self->m_operation = Operation::Idle;
        self->m_pendingForget = false;
        self->m_pendingSignOut = false;
        self->m_token = {};
        self->m_password.clear();
        if (result != GoogleDriveCredentialStore::Result::Success) {
            emit self->failed(tr("The saved Google credentials could not be removed from the desktop wallet. Unlock it and try signing out again."));
            emit self->idle();
            return;
        }
        emit self->disconnected();
        emit self->signedOutLocally(revocationError);
        if (revocationError.isEmpty())
            emit self->statusChanged(tr("Google account signed out."));
        emit self->idle();
    });
}
