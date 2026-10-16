#pragma once

#include "aero/taskdialog.h"

#include <QUrl>

class GoogleDriveConnection;
class KFilePlacesModel;
class QCheckBox;
class QLabel;
class QPushButton;

// App-owned, one-step Google sign-in flow. The connection object owns all
// authentication and mount work; this dialog only presents its state.
class GoogleDriveSignInDialog : public Aero::TaskDialog {
    Q_OBJECT

public:
    explicit GoogleDriveSignInDialog(KFilePlacesModel *places, QWidget *parent = nullptr,
                                     GoogleDriveConnection *connection = nullptr);
    ~GoogleDriveSignInDialog() override;

    QUrl mappedUrl() const { return m_mapped; }

public Q_SLOTS:
    void reject() override;

private Q_SLOTS:
    void signIn();
    void retrySignIn();
    void disconnectDrive();
    void signOut();
    void openDrive();
    void connected(const QUrl &url);
    void failed(const QString &message);
    void disconnected();
    void signedOut();

private:
    enum class Operation { None, Connect, Retry, Disconnect, SignOut };
    void setConnectedState(const QUrl &url);
    void setStatus(const QString &message);
    void updateControls();

    KFilePlacesModel *m_places = nullptr;
    GoogleDriveConnection *m_connection = nullptr;
    QLabel *m_intro = nullptr;
    QLabel *m_status = nullptr;
    QCheckBox *m_showInNetwork = nullptr;
    QCheckBox *m_autoReconnect = nullptr;
    QPushButton *m_signIn = nullptr;
    QPushButton *m_signInAgain = nullptr;
    QPushButton *m_open = nullptr;
    QPushButton *m_disconnect = nullptr;
    QPushButton *m_signOut = nullptr;
    QPushButton *m_cancel = nullptr;
    QUrl m_mapped;
    bool m_available = false;
    Operation m_operation = Operation::None;
    bool m_connected = false;
    bool m_canRetry = false;
    bool m_hasFailure = false;
    bool m_signOutCompleted = false;
    bool m_signOutWarningPending = false;
    bool m_rejecting = false;
};
