#pragma once

#include <QObject>
#include <QByteArray>
#include <QPointer>
#include <QUrl>
#include <QElapsedTimer>
#include <memory>
#include <functional>

class GoogleDriveAuth;
class QProcess;
class QTimer;
class QLockFile;

// Coordinates browser authorization, the desktop keychain, and the mount
// helper. Credential envelopes use private process pipes, never arguments.
class GoogleDriveConnection : public QObject {
    Q_OBJECT
public:
    explicit GoogleDriveConnection(QObject *parent = nullptr);
    ~GoogleDriveConnection() override;
    static GoogleDriveConnection *sharedConnection();

    virtual bool isAvailable() const;
    virtual QString unavailableReason() const;
    virtual bool isConnected() const;
    virtual bool hasStorageQuota() const;
    virtual quint64 storageTotalBytes() const;
    virtual quint64 storageAvailableBytes() const;
    enum class ConnectionState { Disconnected, Connecting, Connected, Offline, WalletLocked, SignInRequired, Unavailable };
    Q_ENUM(ConnectionState)
    ConnectionState connectionState() const;
    bool isAutomaticReconnectInProgress() const;
    bool autoReconnectEnabled() const;
    bool autoReconnectSuppressed() const;
    using LifecycleCompletion = std::function<void(bool, QString)>;
    using PrepareDisconnect = std::function<void(bool, LifecycleCompletion)>;
    using CleanupAccount = std::function<void(LifecycleCompletion)>;
    // Installed only by ordinary Explorer; passive chooser/portal instances
    // neither start nor manage a sync worker.
    void setSyncLifecycleHooks(PrepareDisconnect prepare, CleanupAccount cleanup);

public Q_SLOTS:
    virtual void connectDrive();
    virtual void cancel();
    virtual void disconnectDrive();
    virtual void signOut();
    virtual void signInAgain();
    void reconnectSavedDrive();
    void setAutoReconnectEnabled(bool enabled);
    // Reads bounded kernel mount metadata only; never opens the keychain or
    // starts authorization. Observers use this separately from connectDrive.
    void refreshMountState();
    virtual void refreshStorageQuota(bool force = false);

Q_SIGNALS:
    void statusChanged(const QString &message);
    void connected(const QUrl &url);
    void failed(const QString &message);
    void disconnected();
    void signedOut();
    void mountStateChanged(bool connected);
    void storageQuotaChanged();
    void stateChanged(GoogleDriveConnection::ConnectionState state);
    void autoReconnectPreferenceChanged(bool enabled);

protected:
    // Injection seam for isolated tests. Production only reads kernel metadata.
    virtual QString storageMountId() const;

private:
    friend class GoogleDriveConnectionTest;
    enum class Operation { None, Connect, Disconnect, SignOutDisconnect, Export, Cleanup, Forget };
    QString helperPath() const;
    QString appConfigPath() const;
    bool lockOperation(bool quiet = false);
    void releaseOperation();
    GoogleDriveAuth *auth();
    void beginSignIn(bool forceBrowser);
    void startHelper(const QStringList &arguments, QByteArray input = {});
    void helperFinished(Operation operation, int exitCode, bool normalExit);
    void stopHelper();
    void finishFailure(const QString &message);
    QByteArray envelope(const QByteArray &token, const QString &password) const;
    void cancelStorageQuery();
    void clearStorageQuota();
    void finishStorageQuery(QProcess *process, quint64 generation, bool successful);
    void startDisconnectHelper();
    void prepareDisconnect(bool signOut);
    void beginLocalAccountCleanup();
    void setConnectionState(ConnectionState state);
    void suppressAutoReconnect();
    void scheduleReconnectRetry();
    void finishAutomaticFailure(ConnectionState state);

    QPointer<GoogleDriveAuth> m_auth;
    QPointer<GoogleDriveAuth> m_injectedAuth;
    QString m_helperOverride;
    QString m_appConfigOverride;
    QPointer<QProcess> m_process;
    std::shared_ptr<QLockFile> m_operationLock;
    QTimer *m_timeout = nullptr;
    QTimer *m_mountTimer = nullptr;
    QTimer *m_reconnectTimer = nullptr;
    ConnectionState m_connectionState = ConnectionState::Disconnected;
    bool m_automaticReconnect = false;
    bool m_reconnectStarted = false;
    int m_reconnectAttempts = 0;
    bool m_lastMountState = false;
    QString m_observedMountId;
    QPointer<QProcess> m_quotaProcess;
    QPointer<QProcess> m_stoppingQuotaProcess;
    QTimer *m_quotaTimeout = nullptr;
    QByteArray m_quotaOutput;
    QString m_quotaMountId;
    QString m_requestedQuotaMountId;
    QElapsedTimer m_quotaAge;
    quint64 m_quotaGeneration = 0;
    quint64 m_quotaTotal = 0;
    quint64 m_quotaAvailable = 0;
    bool m_quotaKnown = false;
    QByteArray m_output;
    QString m_signOutWarning;
    Operation m_operation = Operation::None;
    bool m_cancelled = false;
    bool m_exportInvalid = false;
    bool m_forgetPending = false;
    PrepareDisconnect m_prepareDisconnect;
    CleanupAccount m_cleanupAccount;
    quint64 m_generation = 0;
};
