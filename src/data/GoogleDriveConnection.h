#pragma once

#include <QObject>
#include <QByteArray>
#include <QPointer>
#include <QUrl>
#include <memory>

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

public Q_SLOTS:
    virtual void connectDrive();
    virtual void cancel();
    virtual void disconnectDrive();
    virtual void signOut();
    virtual void signInAgain();
    // Reads bounded kernel mount metadata only; never opens the keychain or
    // starts authorization. Observers use this separately from connectDrive.
    void refreshMountState();

Q_SIGNALS:
    void statusChanged(const QString &message);
    void connected(const QUrl &url);
    void failed(const QString &message);
    void disconnected();
    void signedOut();
    void mountStateChanged(bool connected);

private:
    friend class GoogleDriveConnectionTest;
    enum class Operation { None, Connect, Disconnect, SignOutDisconnect, Export, Forget };
    QString helperPath() const;
    QString appConfigPath() const;
    bool lockOperation();
    void releaseOperation();
    GoogleDriveAuth *auth();
    void beginSignIn(bool forceBrowser);
    void startHelper(const QStringList &arguments, QByteArray input = {});
    void helperFinished(Operation operation, int exitCode, bool normalExit);
    void stopHelper();
    void finishFailure(const QString &message);
    QByteArray envelope(const QByteArray &token, const QString &password) const;

    QPointer<GoogleDriveAuth> m_auth;
    QPointer<GoogleDriveAuth> m_injectedAuth;
    QString m_helperOverride;
    QPointer<QProcess> m_process;
    std::unique_ptr<QLockFile> m_operationLock;
    QTimer *m_timeout = nullptr;
    QTimer *m_mountTimer = nullptr;
    bool m_lastMountState = false;
    QByteArray m_output;
    QString m_signOutWarning;
    Operation m_operation = Operation::None;
    bool m_cancelled = false;
    bool m_exportInvalid = false;
    bool m_forgetPending = false;
    quint64 m_generation = 0;
};
