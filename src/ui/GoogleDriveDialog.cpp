#include "GoogleDriveDialog.h"

#include "../data/GoogleDriveService.h"
#include "aero/palette.h"
#include "aero/text.h"

#include <KFilePlacesModel>

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include <utility>

namespace {

QUrl normalizedUrl(QUrl url)
{
    url = url.adjusted(QUrl::NormalizePathSegments);
    QString path = url.path();
    while (path.size() > 1 && path.endsWith(QLatin1Char('/')))
        path.chop(1);
    url.setPath(path);
    return url;
}

bool sameUrl(const QUrl &left, const QUrl &right)
{
    return normalizedUrl(left) == normalizedUrl(right);
}

} // namespace

GoogleDriveDialog::GoogleDriveDialog(KFilePlacesModel *places, QWidget *parent,
                                     GoogleDriveService *service)
    : Aero::TaskDialog(parent)
    , m_places(places)
    , m_service(service ? service : new GoogleDriveService(this))
{
    setWindowTitle(tr("Connect to Google Drive"));
    setAccessibleName(tr("Connect to Google Drive"));
    // Aero::TaskDialog defaults to application modality, but connecting a
    // network location must block only the Explorer window that opened it.
    setWindowModality(Qt::WindowModal);

    m_service->setWindow(this);

    QLabel *intro = Aero::label(
        tr("Sign in to Google Drive to browse your files here."), 9);
    intro->setWordWrap(true);
    intro->setAccessibleName(tr("Sign in to Google Drive to browse your files here"));
    contentLayout()->addWidget(intro);

    auto *form = new QFormLayout;
    form->setContentsMargins(0, 6, 0, 0);
    form->setSpacing(8);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    m_accounts = new QComboBox;
    m_accounts->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_accounts->setMinimumContentsLength(24);
    m_accounts->setAccessibleName(tr("Google Drive account"));
    m_accounts->setAccessibleDescription(
        tr("Choose the Google Drive account to connect."));
    Aero::setPointSize(m_accounts, 9);
    auto *accountLabel = Aero::label(tr("&Account:"), 9);
    accountLabel->setBuddy(m_accounts);
    form->addRow(accountLabel, m_accounts);
    contentLayout()->addLayout(form);

    m_showInNetwork = new QCheckBox(tr("Show this account in &Network"));
    m_showInNetwork->setAccessibleName(tr("Show this account in Network"));
    m_showInNetwork->setToolTip(
        tr("Saves a shortcut under Network, shared with other desktop file managers. "
           "Removing the shortcut does not sign out of Google."));
    m_showInNetwork->setChecked(true);
    Aero::setPointSize(m_showInNetwork, 9);
    contentLayout()->addWidget(m_showInNetwork);

    m_status = Aero::label(QString(), 9, Aero::Palette::MutedText);
    m_status->setObjectName(QStringLiteral("googleDriveStatus"));
    // QLabel exposes its current text as its accessible name, so errors and
    // progress remain readable instead of being masked by a fixed name.
    m_status->setAccessibleDescription(tr("Google Drive connection status"));
    m_status->setTextFormat(Qt::PlainText);
    m_status->setWordWrap(true);
    contentLayout()->addWidget(m_status);
    contentLayout()->addStretch(1);

    m_connect = addButton(tr("&Connect"));
    m_connect->setObjectName(QStringLiteral("googleDriveConnect"));
    m_connect->setAccessibleName(tr("Connect to the selected Google Drive account"));
    m_connect->setDefault(true);
    m_add = addButton(tr("Add acc&ount..."));
    m_add->setObjectName(QStringLiteral("googleDriveAddAccount"));
    m_add->setAccessibleName(tr("Add a Google Drive account"));
    m_reload = addButton(tr("&Reload accounts"));
    m_reload->setObjectName(QStringLiteral("googleDriveReloadAccounts"));
    m_reload->setAccessibleName(tr("Reload Google Drive accounts"));
    m_cancel = addButton(tr("Cancel"));
    m_cancel->setObjectName(QStringLiteral("googleDriveCancel"));
    m_cancel->setAccessibleName(tr("Cancel Google Drive connection"));

    connect(m_connect, &QPushButton::clicked,
            this, &GoogleDriveDialog::connectSelectedAccount);
    connect(m_add, &QPushButton::clicked, this, &GoogleDriveDialog::addAccount);
    connect(m_reload, &QPushButton::clicked, this, &GoogleDriveDialog::loadAccounts);
    connect(m_cancel, &QPushButton::clicked, this, &GoogleDriveDialog::reject);
    connect(m_service, &GoogleDriveService::accountsReady,
            this, &GoogleDriveDialog::accountsReady);
    connect(m_service, &GoogleDriveService::connected,
            this, &GoogleDriveDialog::connected);
    connect(m_service, &GoogleDriveService::failed,
            this, &GoogleDriveDialog::failed);

    setTabOrder(m_accounts, m_connect);
    setTabOrder(m_connect, m_add);
    setTabOrder(m_add, m_reload);
    setTabOrder(m_reload, m_showInNetwork);
    setTabOrder(m_showInNetwork, m_cancel);

    m_available = m_service->isAvailable();
    if (m_available) {
        m_status->setText(tr("Select Connect to choose an existing account, or "
                             "Add account to sign in. "
                             "Your desktop handles sign-in and account credentials."));
        m_accounts->setEnabled(false);
        m_reload->hide();
        m_connect->setEnabled(true);
    } else {
        m_status->setText(tr("Google Drive support is unavailable. Install the "
                             "Qt 6 / KDE Frameworks 6 version of kio-gdrive using your "
                             "distribution's package manager, then reopen this dialog."));
        m_accounts->setEnabled(false);
        m_connect->setEnabled(false);
        m_add->hide();
        m_reload->hide();
    }

    setMinimumWidth(460);
    resize(460, sizeHint().height());
    updateControls();
    (m_available ? m_connect : m_cancel)->setFocus(Qt::OtherFocusReason);
}

GoogleDriveDialog::~GoogleDriveDialog()
{
    disconnect(m_service, nullptr, this, nullptr);
    m_service->cancel();
    m_service->setWindow(nullptr);
}

void GoogleDriveDialog::loadAccounts()
{
    if (!m_available || m_busy)
        return;

    m_operation = Operation::Load;
    setBusy(true, tr("Loading Google Drive accounts…"));
    m_service->loadAccounts();
}

void GoogleDriveDialog::addAccount()
{
    if (!m_available || m_busy)
        return;

    m_operation = Operation::Add;
    setBusy(true, tr("Waiting for Google Drive sign-in…"));
    m_service->addAccount();
}

void GoogleDriveDialog::connectSelectedAccount()
{
    if (!m_available || m_busy)
        return;

    if (!m_accountsLoaded) {
        loadAccounts();
        return;
    }

    const QUrl url = m_accounts->currentData().toUrl();
    if (!url.isValid() || url.isEmpty())
        return;

    m_operation = Operation::Verify;
    setBusy(true, tr("Connecting to Google Drive…"));
    m_service->verifyAccount(url);
}

void GoogleDriveDialog::accountsReady(const QList<GoogleDriveAccount> &accounts,
                                      const QUrl &suggested)
{
    if (m_rejecting || !m_busy
        || (m_operation != Operation::Load && m_operation != Operation::Add))
        return;
    const QUrl previous = m_accounts->currentData().toUrl();
    const bool adding = m_operation == Operation::Add;
    QList<GoogleDriveAccount> merged = adding ? m_accountList : QList<GoogleDriveAccount>();

    for (const GoogleDriveAccount &account : accounts) {
        if (!GoogleDriveService::isAccountUrl(account.url))
            continue;
        const int existing = [&merged, &account] {
            for (int i = 0; i < merged.size(); ++i) {
                if (sameUrl(merged.at(i).url, account.url))
                    return i;
            }
            return -1;
        }();
        if (existing >= 0)
            merged[existing] = account;
        else
            merged.append(account);
    }

    if (GoogleDriveService::isAccountUrl(suggested)) {
        bool found = false;
        for (const GoogleDriveAccount &account : std::as_const(merged)) {
            if (sameUrl(account.url, suggested)) {
                found = true;
                break;
            }
        }
        if (!found)
            merged.append({tr("Google Drive"), suggested});
    }

    m_accountList = merged;
    m_accounts->clear();
    for (const GoogleDriveAccount &account : std::as_const(m_accountList)) {
        const QString name = account.name.trimmed().isEmpty()
            ? tr("Google Drive") : account.name.trimmed();
        m_accounts->addItem(name, account.url);
    }

    m_accountsLoaded = true;
    int selected = -1;
    if (suggested.isValid() && !suggested.isEmpty())
        selected = accountIndex(suggested);
    if (selected < 0 && previous.isValid() && !previous.isEmpty())
        selected = accountIndex(previous);
    if (selected < 0 && m_accounts->count() > 0)
        selected = 0;
    if (selected >= 0)
        m_accounts->setCurrentIndex(selected);

    m_operation = Operation::None;
    setBusy(false);
    if (m_accounts->count() == 0) {
        m_status->setText(tr("No Google Drive accounts are connected. Select "
                             "Add account to sign in."));
    } else if (suggested.isValid() && !suggested.isEmpty()) {
        m_status->setText(tr("Account added. Select Connect to browse its files."));
    } else {
        m_status->setText(tr("Select an account, then select Connect to browse its files."));
    }
    updateControls();
    if (m_accounts->count() > 0) {
        m_connect->setDefault(true);
        m_accounts->setFocus(Qt::OtherFocusReason);
    } else {
        m_add->setFocus(Qt::OtherFocusReason);
    }
}

void GoogleDriveDialog::connected(const QUrl &url)
{
    if (m_rejecting || !m_busy || m_operation != Operation::Verify)
        return;
    if (!GoogleDriveService::isAccountUrl(url)
        || !sameUrl(url, m_accounts->currentData().toUrl())) {
        failed(tr("Google Drive returned an unexpected account location."));
        return;
    }
    m_operation = Operation::None;
    setBusy(false);
    if (!url.isValid() || url.isEmpty()) {
        failed(tr("Google Drive returned an invalid account location."));
        return;
    }

    if (m_showInNetwork->isChecked() && m_places
        && !GoogleDriveService::rememberAccount(m_places, url, accountName(url))) {
        m_status->setText(tr("Connected, but the account could not be added to Network."));
        updateControls();
        return;
    }

    m_mapped = url;
    accept();
}

void GoogleDriveDialog::failed(const QString &message)
{
    if (m_rejecting)
        return;
    m_operation = Operation::None;
    setBusy(false, message.trimmed().isEmpty()
                       ? tr("Could not connect to Google Drive. Try again.")
                       : message);
    updateControls();
    if (m_connect->isEnabled()) {
        m_connect->setDefault(true);
        m_connect->setFocus(Qt::OtherFocusReason);
    }
}

void GoogleDriveDialog::reject()
{
    if (!m_rejecting && m_service) {
        m_rejecting = true;
        disconnect(m_service, nullptr, this, nullptr);
        m_service->cancel();
    }
    QDialog::reject();
}

void GoogleDriveDialog::setBusy(bool busy, const QString &message)
{
    m_busy = busy;
    if (!message.isNull())
        m_status->setText(message);
    updateControls();
}

void GoogleDriveDialog::updateControls()
{
    const bool usable = m_available && !m_busy;
    const bool haveAccount = m_accounts->currentIndex() >= 0;
    m_accounts->setEnabled(usable && m_accountsLoaded && m_accounts->count() > 0);
    m_connect->setEnabled(usable && (!m_accountsLoaded || haveAccount));
    m_add->setVisible(m_available);
    m_reload->setVisible(m_available && m_accountsLoaded);
    m_add->setEnabled(usable);
    m_reload->setEnabled(usable && m_accountsLoaded);
    m_showInNetwork->setEnabled(!m_busy);
    m_cancel->setEnabled(true);

    // Status text changes after discovery and on failure. QLabel's default
    // minimum height permits clipping wrapped lines when the dialog was sized
    // for a shorter message, especially at larger display scales.
    layout()->activate();
    m_status->setMinimumHeight(m_status->heightForWidth(qMax(1, m_status->width())));
    contentLayout()->invalidate();
    contentLayout()->activate();
    contentLayout()->parentWidget()->setMinimumHeight(contentLayout()->minimumSize().height());
    layout()->invalidate();
    layout()->activate();
    resize(width(), qMax(height(), minimumSizeHint().height()));
}

int GoogleDriveDialog::accountIndex(const QUrl &url) const
{
    for (int i = 0; i < m_accounts->count(); ++i) {
        if (sameUrl(m_accounts->itemData(i).toUrl(), url))
            return i;
    }
    return -1;
}

QString GoogleDriveDialog::accountName(const QUrl &url) const
{
    const int index = accountIndex(url);
    if (index >= 0)
        return m_accounts->itemText(index);
    return tr("Google Drive");
}
