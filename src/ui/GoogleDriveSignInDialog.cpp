#include "GoogleDriveSignInDialog.h"

#include "../data/GoogleDriveConnection.h"
#include "../data/GoogleDriveService.h"
#include "../model/Locations.h"
#include "aero/text.h"

#include <KFilePlacesModel>

#include <QCheckBox>
#include <QKeySequence>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

GoogleDriveSignInDialog::GoogleDriveSignInDialog(KFilePlacesModel *places,
                                                 QWidget *parent,
                                                 GoogleDriveConnection *connection)
    : Aero::TaskDialog(parent)
    , m_places(places)
    , m_connection(connection ? connection : GoogleDriveConnection::sharedConnection())
{
    setWindowTitle(tr("Google Drive"));
    setAccessibleName(tr("Google Drive sign-in"));
    setWindowModality(Qt::WindowModal);

    m_intro = Aero::label(tr("Sign in with Google to browse your Drive files."), 9);
    m_intro->setWordWrap(true);
    contentLayout()->addWidget(m_intro);

    m_showInNetwork = new QCheckBox(tr("Show in &Network"));
    m_showInNetwork->setObjectName(QStringLiteral("googleDriveShowInNetwork"));
    m_showInNetwork->setAccessibleName(tr("Show Google Drive in Network"));
    m_showInNetwork->setAccessibleDescription(
        tr("Adds a shortcut to Google Drive under Network after it connects."));
    m_showInNetwork->setChecked(true);
    Aero::setPointSize(m_showInNetwork, 9);
    contentLayout()->addWidget(m_showInNetwork);

    m_autoReconnect = new QCheckBox(tr("Reconnect Google Drive automatically when Explorer starts"));
    m_autoReconnect->setObjectName(QStringLiteral("googleDriveAutoReconnect"));
    m_autoReconnect->setAccessibleName(tr("Reconnect Google Drive automatically when Explorer starts"));
    m_autoReconnect->setAccessibleDescription(
        tr("Reconnects using the saved account when Explorer starts. Sign-in never opens automatically."));
    m_autoReconnect->setChecked(m_connection->autoReconnectEnabled());
    Aero::setPointSize(m_autoReconnect, 9);
    contentLayout()->addWidget(m_autoReconnect);

    m_status = Aero::label(QString(), 9, Aero::Palette::MutedText);
    m_status->setObjectName(QStringLiteral("googleDriveSignInStatus"));
    m_status->setAccessibleDescription(tr("Google Drive sign-in status"));
    m_status->setTextFormat(Qt::PlainText);
    m_status->setWordWrap(true);
    m_status->setMinimumHeight(0);
    m_status->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    contentLayout()->addWidget(m_status);
    contentLayout()->addStretch(1);

    m_signIn = addButton(tr("Sign in with &Google"));
    m_signIn->setObjectName(QStringLiteral("googleDriveSignIn"));
    m_signIn->setAccessibleName(tr("Sign in with Google"));
    m_signIn->setText(QString());
    m_signIn->setIcon(QIcon(QStringLiteral(":/google/google-signin.png")));
    m_signIn->setIconSize(QSize(180, 40));
    m_signIn->setFixedSize(184, 44);
    m_signIn->setStyleSheet(QStringLiteral(
        "QPushButton#googleDriveSignIn { background: transparent; border: 2px solid transparent;"
        " border-radius: 3px; padding: 0px; margin: 0px; }"
        "QPushButton#googleDriveSignIn:focus { border-color: #0067b1; }"));
    m_signIn->setShortcut(QKeySequence(Qt::ALT | Qt::Key_G));
    m_signIn->setDefault(true);
    m_signInAgain = addButton(tr("Sign in again"));
    m_signInAgain->setObjectName(QStringLiteral("googleDriveSignInAgain"));
    m_signInAgain->setAccessibleName(tr("Sign in to Google Drive again"));
    m_open = addButton(tr("&Open Drive"));
    m_open->setObjectName(QStringLiteral("googleDriveOpen"));
    m_open->setAccessibleName(tr("Open Google Drive"));
    m_disconnect = addButton(tr("Disconnect"));
    m_disconnect->setObjectName(QStringLiteral("googleDriveDisconnect"));
    m_disconnect->setAccessibleName(tr("Disconnect Google Drive"));
    m_signOut = addButton(tr("Sign out"));
    m_signOut->setObjectName(QStringLiteral("googleDriveSignOut"));
    m_signOut->setAccessibleName(tr("Sign out of Google Drive"));
    m_cancel = addButton(tr("Cancel"));
    m_cancel->setObjectName(QStringLiteral("googleDriveSignInCancel"));
    m_cancel->setAccessibleName(tr("Cancel Google Drive sign-in"));

    connect(m_signIn, &QPushButton::clicked, this, &GoogleDriveSignInDialog::signIn);
    connect(m_signInAgain, &QPushButton::clicked,
            this, &GoogleDriveSignInDialog::retrySignIn);
    connect(m_open, &QPushButton::clicked, this, &GoogleDriveSignInDialog::openDrive);
    connect(m_disconnect, &QPushButton::clicked,
            this, &GoogleDriveSignInDialog::disconnectDrive);
    connect(m_signOut, &QPushButton::clicked,
            this, &GoogleDriveSignInDialog::signOut);
    connect(m_cancel, &QPushButton::clicked, this, &GoogleDriveSignInDialog::reject);
    connect(m_autoReconnect, &QCheckBox::toggled,
            m_connection, &GoogleDriveConnection::setAutoReconnectEnabled);
    connect(m_connection, &GoogleDriveConnection::statusChanged,
            this, [this](const QString &message) {
                // The shared connection may also be doing startup reconnect or
                // work initiated by another Explorer window. Only surface its
                // progress while this dialog owns an explicit operation.
                if (m_operation != Operation::None && !m_rejecting)
                    setStatus(message);
            });
    connect(m_connection, &GoogleDriveConnection::stateChanged,
            this, [this](GoogleDriveConnection::ConnectionState state) {
                if (m_rejecting || m_operation != Operation::None)
                    return;
                switch (state) {
                case GoogleDriveConnection::ConnectionState::Connected:
                    if (m_connection->isConnected()) {
                        const QUrl mount = Locations::googleDriveMount();
                        m_connected = true;
                        m_canRetry = false;
                        m_hasFailure = false;
                        setConnectedState(mount);
                        setStatus(tr("Google Drive is connected."));
                    }
                    break;
                case GoogleDriveConnection::ConnectionState::Connecting:
                    if (m_connection->isAutomaticReconnectInProgress())
                        setStatus(tr("Reconnecting to Google Drive…"));
                    break;
                case GoogleDriveConnection::ConnectionState::Offline:
                    m_connected = false;
                    m_mapped.clear();
                    m_showInNetwork->show();
                    setStatus(tr("Google Drive is offline. Explorer will try again automatically."));
                    break;
                case GoogleDriveConnection::ConnectionState::WalletLocked:
                    m_connected = false;
                    m_mapped.clear();
                    m_showInNetwork->show();
                    setStatus(tr("Unlock your desktop account store to reconnect to Google Drive."));
                    break;
                case GoogleDriveConnection::ConnectionState::SignInRequired:
                    m_connected = false;
                    m_mapped.clear();
                    m_canRetry = true;
                    m_showInNetwork->show();
                    setStatus(tr("Sign in again to reconnect to Google Drive."));
                    break;
                case GoogleDriveConnection::ConnectionState::Unavailable:
                    m_connected = false;
                    m_mapped.clear();
                    setStatus(m_connection->unavailableReason());
                    break;
                case GoogleDriveConnection::ConnectionState::Disconnected:
                    if (!m_connection->isConnected()) {
                        m_connected = false;
                        m_mapped.clear();
                        m_showInNetwork->show();
                        setStatus(tr("Google Drive is disconnected. Sign in with Google to connect again."));
                    }
                    break;
                }
                updateControls();
            });
    connect(m_connection, &GoogleDriveConnection::connected,
            this, &GoogleDriveSignInDialog::connected);
    connect(m_connection, &GoogleDriveConnection::failed,
            this, &GoogleDriveSignInDialog::failed);
    connect(m_connection, &GoogleDriveConnection::disconnected,
            this, &GoogleDriveSignInDialog::disconnected);
    connect(m_connection, &GoogleDriveConnection::signedOut,
            this, &GoogleDriveSignInDialog::signedOut);

    setTabOrder(m_showInNetwork, m_autoReconnect);
    setTabOrder(m_autoReconnect, m_signIn);
    setTabOrder(m_signIn, m_signInAgain);
    setTabOrder(m_signInAgain, m_open);
    setTabOrder(m_open, m_disconnect);
    setTabOrder(m_disconnect, m_signOut);
    setTabOrder(m_signOut, m_cancel);

    // Establish the wrapping width before status text is measured.
    setMinimumWidth(420);
    setMaximumWidth(500);
    resize(460, sizeHint().height());

    m_available = m_connection->isAvailable();
    m_connected = m_available && m_connection->isConnected();
    if (!m_available) {
        setStatus(m_connection->unavailableReason().trimmed().isEmpty()
                      ? tr("Google Drive sign-in is unavailable in this build. Contact the application distributor.")
                      : m_connection->unavailableReason());
    } else if (m_connected) {
        m_mapped = Locations::googleDriveMount();
        setStatus(tr("Google Drive is connected."));
    } else {
        setStatus(tr("Your browser will open so you can sign in securely with Google."));
    }

    updateControls();
    (m_connected ? m_open : m_available ? m_signIn : m_cancel)
        ->setFocus(Qt::OtherFocusReason);
}

GoogleDriveSignInDialog::~GoogleDriveSignInDialog()
{
    disconnect(m_connection, nullptr, this, nullptr);
    if (m_operation == Operation::Connect || m_operation == Operation::Retry)
        m_connection->cancel();
}

void GoogleDriveSignInDialog::signIn()
{
    if (!m_available || m_operation != Operation::None || m_rejecting)
        return;
    m_operation = Operation::Connect;
    m_canRetry = false;
    m_hasFailure = false;
    m_signOutCompleted = false;
    m_signOutWarningPending = false;
    setStatus(tr("Connecting to Google Drive…"));
    updateControls();
    m_connection->connectDrive();
}

void GoogleDriveSignInDialog::retrySignIn()
{
    if (!m_available || m_operation != Operation::None || m_rejecting)
        return;
    m_operation = Operation::Retry;
    m_canRetry = false;
    m_hasFailure = false;
    m_signOutCompleted = false;
    m_signOutWarningPending = false;
    setStatus(tr("Waiting for Google sign-in…"));
    updateControls();
    m_connection->signInAgain();
}

void GoogleDriveSignInDialog::disconnectDrive()
{
    if (!m_connected || m_operation != Operation::None || m_rejecting)
        return;
    m_operation = Operation::Disconnect;
    setStatus(tr("Disconnecting from Google Drive…"));
    updateControls();
    m_connection->disconnectDrive();
}

void GoogleDriveSignInDialog::signOut()
{
    if ((!m_available && !m_connected) || m_operation != Operation::None || m_rejecting)
        return;
    m_operation = Operation::SignOut;
    m_hasFailure = false;
    m_signOutCompleted = false;
    m_signOutWarningPending = true;
    setStatus(tr("Signing out of Google Drive…"));
    updateControls();
    m_connection->signOut();
}

void GoogleDriveSignInDialog::connected(const QUrl &url)
{
    if (m_rejecting)
        return;
    const QUrl expectedMount = Locations::googleDriveMount();
    if (!url.isValid() || url.isEmpty() || !url.isLocalFile()
        || url != expectedMount) {
        if (m_operation == Operation::Connect || m_operation == Operation::Retry)
            failed(tr("Google Drive returned an invalid mounted location."));
        return;
    }

    const bool ownsConnect = m_operation == Operation::Connect
        || m_operation == Operation::Retry;
    if (!ownsConnect) {
        // A shared startup reconnect (or another window's Connect) can update
        // this dialog, but it must never accept it or create a Network place.
        m_connected = m_connection->isConnected();
        if (m_connected) {
            setConnectedState(url);
            setStatus(tr("Google Drive is connected."));
        }
        updateControls();
        return;
    }

    m_operation = Operation::None;
    m_hasFailure = false;
    setConnectedState(url);
    setStatus(tr("Google Drive is connected."));
    if (m_showInNetwork->isChecked() && m_places
        && !GoogleDriveService::rememberMountedDrive(m_places)) {
        setStatus(tr("Connected, but Google Drive could not be added to Network."));
        updateControls();
        return;
    }
    updateControls();
    accept();
}

void GoogleDriveSignInDialog::setConnectedState(const QUrl &url)
{
    m_connected = true;
    m_mapped = url;
    m_showInNetwork->hide();
    if (m_status->text().isEmpty())
        setStatus(tr("Google Drive is connected."));
}

void GoogleDriveSignInDialog::failed(const QString &message)
{
    if (m_rejecting)
        return;
    const bool ownedOperation = m_operation == Operation::Connect || m_operation == Operation::Retry
        || m_operation == Operation::Disconnect || m_operation == Operation::SignOut;
    const bool pendingSignOutWarning = m_operation == Operation::None
        && m_signOutWarningPending;
    if (!ownedOperation && !pendingSignOutWarning)
        return;
    m_operation = Operation::None;
    m_signOutWarningPending = false;
    m_canRetry = !m_signOutCompleted;
    m_hasFailure = true;
    m_signInAgain->show();
    setStatus(message.trimmed().isEmpty()
                  ? tr("Could not connect to Google Drive. Try again.")
                  : message);
    updateControls();
    m_signInAgain->setFocus(Qt::OtherFocusReason);
}

void GoogleDriveSignInDialog::disconnected()
{
    if (m_rejecting)
        return;
    m_connected = false;
    if (m_operation == Operation::Disconnect) {
        m_operation = Operation::None;
        m_canRetry = false;
    }
    m_mapped.clear();
    m_showInNetwork->show();
    if (!m_hasFailure)
        setStatus(tr("Google Drive is disconnected. Sign in with Google to connect again."));
    updateControls();
}

void GoogleDriveSignInDialog::signedOut()
{
    if (m_rejecting)
        return;
    m_operation = Operation::None;
    m_connected = false;
    m_canRetry = false;
    m_signOutCompleted = true;
    m_mapped.clear();
    m_showInNetwork->show();
    if (!m_hasFailure)
        setStatus(tr("You are signed out of Google Drive."));
    updateControls();
}

void GoogleDriveSignInDialog::openDrive()
{
    if (m_connected && m_mapped.isValid() && !m_mapped.isEmpty())
        accept();
}

void GoogleDriveSignInDialog::setStatus(const QString &message)
{
    m_status->setText(message);
    m_status->setMinimumHeight(0);
    m_status->updateGeometry();
    contentLayout()->activate();
    layout()->activate();
    const int fallbackWidth = qMax(1, width() - 48);
    const int statusWidth = qMax(1, m_status->width() > 0 ? m_status->width() : fallbackWidth);
    m_status->setMinimumHeight(qMax(m_status->fontMetrics().height(),
                                    m_status->heightForWidth(statusWidth)));
    contentLayout()->parentWidget()->setMinimumHeight(0);
    contentLayout()->activate();
    contentLayout()->parentWidget()->setMinimumHeight(contentLayout()->minimumSize().height());
    layout()->activate();
    const int requiredHeight = minimumSizeHint().height();
    if (height() < requiredHeight)
        resize(width(), requiredHeight);
}

void GoogleDriveSignInDialog::updateControls()
{
    const bool hasConnection = m_connected;
    m_intro->setText(hasConnection ? tr("Browse your Google Drive files in Explorer.")
                                 : tr("Sign in with Google to browse your Drive files."));
    m_intro->setAccessibleName(m_intro->text());
    m_open->setVisible(hasConnection);
    m_disconnect->setVisible(hasConnection);
    m_signOut->setVisible(m_available || hasConnection);
    m_signIn->setVisible(!hasConnection && !m_canRetry);
    m_signInAgain->setVisible(!hasConnection && m_canRetry);
    m_showInNetwork->setVisible(!hasConnection);
    m_autoReconnect->setVisible(m_available);
    const bool busy = m_operation != Operation::None
        || m_connection->isAutomaticReconnectInProgress();
    m_signIn->setEnabled(m_available && !busy && !hasConnection);
    m_signInAgain->setEnabled(m_available && !busy && !hasConnection);
    m_open->setEnabled(hasConnection && !busy && m_mapped.isValid() && !m_mapped.isEmpty());
    m_disconnect->setEnabled(hasConnection && !busy);
    m_signOut->setEnabled((m_available || hasConnection) && !busy);
    m_cancel->setEnabled(true);
}

void GoogleDriveSignInDialog::reject()
{
    if (!m_rejecting) {
        m_rejecting = true;
        disconnect(m_connection, nullptr, this, nullptr);
        if (m_operation == Operation::Connect || m_operation == Operation::Retry)
            m_connection->cancel();
        m_operation = Operation::None;
    }
    QDialog::reject();
}
