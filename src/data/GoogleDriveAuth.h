#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>
#include <functional>

class QNetworkAccessManager;
class QNetworkReply;
class QOAuth2AuthorizationCodeFlow;
class GoogleDriveLoopbackReply;

// An asynchronous seam for tests; the production implementation uses QtKeychain
// with insecure fallback explicitly disabled. Injected stores must outlive auth.
class GoogleDriveCredentialStore : public QObject
{
public:
    enum class Result { Success, Missing, Failed };
    using Completion = std::function<void(Result, QByteArray)>;
    using QObject::QObject;
    virtual void read(Completion completion) = 0;
    virtual void write(const QByteArray &record, Completion completion) = 0;
    virtual void remove(Completion completion) = 0;
};

class GoogleDriveAuth : public QObject
{
    Q_OBJECT
public:
    struct ClientConfig { QString clientId; QString clientSecret; };
    struct Dependencies {
        GoogleDriveCredentialStore *store = nullptr;
        std::function<bool(const QUrl &)> openBrowser;
        // Returned managers must be owned by the supplied parent.
        std::function<QNetworkAccessManager *(QObject *)> networkFactory;
    };

    explicit GoogleDriveAuth(ClientConfig config, QObject *parent = nullptr);
    GoogleDriveAuth(ClientConfig config, Dependencies dependencies, QObject *parent = nullptr);
    ~GoogleDriveAuth() override;
    bool available() const;
    bool isBusy() const;
    static QString requiredScope();

public slots:
    void startSignIn();
    void connectExisting();
    void cancel();
    void signOut();
    void completeSignOut(const QByteArray &currentRcloneToken);

signals:
    void authorized(const QByteArray &rcloneToken, const QString &configPassword);
    void failed(const QString &message);
    void statusChanged(const QString &message);
    void disconnected();
    void revocationTokenRequested(const QString &configPassword);
    void signedOutLocally(const QString &warning);
    void idle();

private:
    friend class GoogleDriveAuthTest;
    friend class GoogleDriveBoundedNetwork;
    enum class Operation { Idle, Reading, Authorizing, Writing, AwaitingRevocationToken, Revoking, Deleting };
    void makeFlow();
    void clearFlow();
    void fail(const QString &message);
    void readRecord(bool forSignOut, bool forSignIn = false);
    void beginAuthorization();
    void finishTokens(const QVariantMap &values);
    void saveRecord();
    void revokeAndDelete();
    void deleteRecord(const QString &revocationError = {});
    static bool validateToken(const QJsonObject &token);
    static void limitReplyBuffer(QNetworkReply *reply);
    bool decodeRecord(const QByteArray &record, QJsonObject *token, QString *password) const;
    QByteArray encodeRecord() const;

    ClientConfig m_config;
    Dependencies m_dependencies;
    GoogleDriveCredentialStore *m_store = nullptr;
    QPointer<QOAuth2AuthorizationCodeFlow> m_flow;
    QPointer<QNetworkAccessManager> m_network;
    GoogleDriveLoopbackReply *m_reply = nullptr;
    QTimer m_timeout;
    Operation m_operation = Operation::Idle;
    QJsonObject m_token;
    QString m_password;
    quint64 m_generation = 0;
    bool m_validTokenResponse = false;
    bool m_cancelWrite = false;
    bool m_pendingSignOut = false;
};
