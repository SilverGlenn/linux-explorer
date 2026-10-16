#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <functional>
#include <memory>

class GoogleDriveAuth;
class QProcess;
class QTimer;
class QLockFile;

// Presentation-independent client of the single per-user sync worker. Private
// credential envelopes only travel through an anonymous child-process pipe.
class GoogleDriveSyncController : public QObject
{
    Q_OBJECT
public:
    explicit GoogleDriveSyncController(QObject *parent = nullptr);
    ~GoogleDriveSyncController() override;
    static GoogleDriveSyncController *sharedController();

    virtual bool isAvailable() const;
    virtual bool isBusy() const;
    virtual bool workerRunning() const;
    virtual bool hasWritePermission() const;
    // True when write access was configured previously, even if the worker
    // needs an explicit resume after Explorer restarts.
    virtual bool hasSavedPermission() const;
    virtual QString statusMessage() const;
    virtual QJsonArray pairs() const;

public Q_SLOTS:
    virtual void refresh();
    // Saved-account-only restoration: never opens a browser or wallet prompt.
    virtual void restoreWorker(bool explicitConnect = false);
    // Explicit consent action from the dialog; may open the system browser.
    virtual void requestWritePermission();
    // Explicit reauthorization for an expired/revoked saved sync grant.
    virtual void signInAgain();
    virtual void listFolders(const QString &parentId = QStringLiteral("root"));
    virtual void previewPair(const QString &remoteFolderId, const QString &localPath);
    virtual void addPair(const QString &remoteFolderId, const QString &localPath);
    virtual void pausePair(const QString &pairId);
    virtual void resumePair(const QString &pairId);
    virtual void syncNow(const QString &pairId);
    virtual void removePair(const QString &pairId);
    virtual void reviewPair(const QString &pairId);
    virtual void resumeReviewed(const QString &pairId, const QString &reviewToken);
    virtual void stopWorker();
    virtual void cancelSetup();

public:
    // Disconnect/sign-out must stop uploads before revoking/removing accounts.
    void prepareDisconnect(bool signOut, std::function<void(bool, QString)> completion);
    void clearLocalAccount(std::function<void(bool, QString)> completion);

Q_SIGNALS:
    void changed();
    void foldersListed(const QString &parentId, const QJsonArray &folders);
    void previewReady(const QJsonObject &preview);
    void reviewReady(const QJsonObject &review);
    void failed(const QString &message);

protected:
    virtual QString helperPath() const;

private:
    friend class GoogleDriveSyncControllerTest;
    enum class AuthStage { None, Browse, Sync };
    void ensureAuth();
    void beginWorker(bool interactive, bool explicitConnect = false);
    void startWorker(const QByteArray &token, const QString &password);
    void runHelper(const QString &command, QJsonObject request,
                   std::function<void(bool, QJsonObject)> completion, int timeoutMs = 15000);
    void request(QJsonObject request, std::function<void(bool, QJsonObject)> completion = {});
    void acceptStatus(const QJsonObject &status, bool preserveMessage = false);
    void fail(const QString &message);
    void clearSecrets();
    bool lockAuthorization();
    void releaseAuthorization();
    void forgetSync(std::function<void(bool, QString)> completion);

    QPointer<GoogleDriveAuth> m_browseAuth;
    QPointer<GoogleDriveAuth> m_syncAuth;
    QPointer<QProcess> m_process;
    std::shared_ptr<QLockFile> m_authorizationLock;
    QTimer *m_poll = nullptr;
    QJsonObject m_status;
    QString m_message;
    QString m_browsePassword;
    AuthStage m_authStage = AuthStage::None;
    bool m_interactive = false;
    bool m_forceSignIn = false;
    bool m_available = false;
    bool m_workerRunning = false;
    bool m_stopping = false;
    bool m_restoreAttempted = false;
    bool m_preserveError = false;
    int m_restoreFailures = 0;
    QJsonObject m_pendingRequest;
    std::function<void(bool, QJsonObject)> m_pendingCompletion;
    quint64 m_generation = 0;
};
