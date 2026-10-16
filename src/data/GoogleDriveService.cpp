#include "GoogleDriveService.h"
#include "Locations.h"

#include <KFilePlacesModel>
#include <KIO/JobUiDelegateFactory>
#include <KIO/ListJob>
#include <KProtocolInfo>
#include <QSet>
#include <QStandardPaths>
#include <QWidget>
#include <memory>
#include <sys/stat.h>

namespace {
constexpr int kMaxAccounts = 256;

bool isRoot(const QUrl &url)
{
    return url.isValid() && url.scheme() == QLatin1String("gdrive")
        && url.authority().isEmpty() && !url.hasQuery() && !url.hasFragment()
        && (url.path().isEmpty() || url.path() == QLatin1String("/"));
}

QUrl canonicalAccount(const QUrl &url)
{
    QUrl canonical;
    canonical.setScheme(url.scheme());
    canonical.setPath(url.adjusted(QUrl::StripTrailingSlash).path());
    return canonical;
}
}

GoogleDriveService::GoogleDriveService(QObject *parent) : QObject(parent) {}

GoogleDriveService::~GoogleDriveService()
{
    cancel();
}

bool GoogleDriveService::isAvailable() const
{
    return KProtocolInfo::isKnownProtocol(QStringLiteral("gdrive"));
}

void GoogleDriveService::setWindow(QWidget *window)
{
    m_window = window;
}

QUrl GoogleDriveService::accountUrl(const QString &name)
{
    if (name.isEmpty() || name.size() > 256 || name == QLatin1String(".")
        || name == QLatin1String("..") || name == QLatin1String("new-account")
        || name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\')))
        return {};
    for (QChar ch : name)
        if (ch.unicode() < 0x20 || ch.unicode() == 0x7f)
            return {};

    QUrl url;
    url.setScheme(QStringLiteral("gdrive"));
    url.setPath(QLatin1Char('/') + name);
    return url;
}

bool GoogleDriveService::isAccountUrl(const QUrl &url)
{
    if (!url.isValid() || url.scheme() != QLatin1String("gdrive")
        || !url.authority().isEmpty() || url.hasQuery() || url.hasFragment())
        return false;
    const QString path = canonicalAccount(url).path();
    return path.startsWith(QLatin1Char('/'))
        && accountUrl(path.mid(1)).isValid();
}

QUrl GoogleDriveService::managedDriveLocation(const QUrl &url)
{
    // A cloud mount never needs an elevated worker. Keep its normal identity
    // so offline and read-only handling also apply to a typed admin: URL.
    if (url.scheme() == QLatin1String("admin")) {
        QUrl local = url;
        local.setScheme(QStringLiteral("file"));
        if (Locations::isGoogleDriveMount(local))
            return local;
    }
    return isRoot(url) || isAccountUrl(url) ? Locations::googleDriveMount() : url;
}

QList<GoogleDriveAccount> GoogleDriveService::accountsForListing(
    const QUrl &url, const KIO::UDSEntryList &entries)
{
    // First-time setup redirects straight into the account. Its entries are
    // files and folders, and must never be interpreted as account identities.
    if (isAccountUrl(url))
        return {{canonicalAccount(url).path().mid(1), canonicalAccount(url)}};
    if (!isRoot(url))
        return {};

    QList<GoogleDriveAccount> accounts;
    QSet<QUrl> seen;
    for (const KIO::UDSEntry &entry : entries) {
        if (!S_ISDIR(entry.numberValue(KIO::UDSEntry::UDS_FILE_TYPE)))
            continue;
        const QString name = entry.stringValue(KIO::UDSEntry::UDS_NAME);
        const QUrl account = accountUrl(name);
        if (!account.isValid() || seen.contains(account))
            continue;
        seen.insert(account);
        // The identity, rather than UDS_URL or arbitrary rich display text,
        // supplies both the picker label and the credential-free location.
        accounts.append({name, account});
        if (accounts.size() == kMaxAccounts)
            break;
    }
    return accounts;
}

bool GoogleDriveService::rememberAccount(KFilePlacesModel *places, const QUrl &url,
                                        const QString &name)
{
    if (!places || !isAccountUrl(url))
        return false;
    const QUrl account = canonicalAccount(url);
    for (int row = 0; row < places->rowCount(); ++row) {
        const QModelIndex index = places->index(row, 0);
        if (places->url(index).matches(account, QUrl::StripTrailingSlash)) {
            places->setPlaceHidden(index, false);
            return true; // Preserve a user's existing name and icon.
        }
    }
    const QString label = name.isEmpty() ? account.path().mid(1) : name;
    places->addPlace(tr("Google Drive (%1)").arg(label), account,
                     QStringLiteral("folder-gdrive"));
    return true;
}

void GoogleDriveService::loadAccounts()
{
    start(QUrl(QStringLiteral("gdrive:/")), Operation::Accounts);
}

QString GoogleDriveService::connectionError(int error, const QString &detail)
{
    if (error == KIO::ERR_USER_CANCELED)
        return tr("Google Drive sign-in was canceled. You can try again.");
    if (error == KIO::ERR_ACCESS_DENIED)
        return tr("Google Drive denied access to this account.\n\n"
                  "Some KDE Google account providers do not request Drive permission. "
                  "Sign-in can succeed while Drive access remains unavailable. "
                  "Check your desktop's Google Drive support before signing in again.");
    return detail.isEmpty() ? tr("Could not connect to Google Drive.") : detail;
}

QString GoogleDriveService::personalConfigPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
        + QStringLiteral("/linux-explorer-win7/google-drive/rclone.conf");
}

bool GoogleDriveService::rememberMountedDrive(KFilePlacesModel *places)
{
    if (!places || !Locations::googleDriveMounted())
        return false;
    const QUrl mount = Locations::googleDriveMount();
    for (int row = 0; row < places->rowCount(); ++row) {
        const QModelIndex index = places->index(row, 0);
        if (places->url(index).matches(mount, QUrl::StripTrailingSlash)) {
            places->setPlaceHidden(index, false);
            return true;
        }
    }
    places->addPlace(tr("Google Drive"), mount, QStringLiteral("folder-gdrive"));
    return true;
}

void GoogleDriveService::addAccount()
{
    start(QUrl(QStringLiteral("gdrive:/new-account")), Operation::AddAccount);
}

void GoogleDriveService::verifyAccount(const QUrl &url)
{
    if (!isAccountUrl(url)) {
        cancel();
        Q_EMIT failed(tr("Select a Google Drive account to connect."));
        return;
    }
    start(canonicalAccount(url), Operation::Verify);
}

void GoogleDriveService::cancel()
{
    if (!m_job)
        return;
    auto *job = m_job.data();
    m_job.clear();
    disconnect(job, nullptr, this, nullptr);
    job->kill(KJob::Quietly);
}

void GoogleDriveService::start(const QUrl &url, Operation operation)
{
    cancel();
    if (!isAvailable()) {
        Q_EMIT failed(tr("Google Drive support is unavailable. Install the Qt 6 / "
                         "KDE Frameworks 6 version of kio-gdrive from your distribution, "
                         "then reopen this dialog."));
        return;
    }

    auto *job = KIO::listDir(url, KIO::HideProgressInfo);
    m_job = job;
    job->setUiDelegate(KIO::createDefaultJobUiDelegate(
        KJobUiDelegate::AutoHandlingDisabled, m_window));
    struct Listing {
        QUrl url;
        KIO::UDSEntryList entries;
    };
    auto listing = std::make_shared<Listing>();
    listing->url = url;
    connect(job, &KIO::ListJob::redirection, this,
            [this, job, listing, operation, url](KIO::Job *, const QUrl &target) {
        if (m_job != job)
            return;
        // Never follow a provider response into another scheme/authority or
        // an operation path. Verification must stay on the selected account.
        if ((!isRoot(target) && !isAccountUrl(target))
            || (operation == Operation::Verify
                && canonicalAccount(target) != canonicalAccount(url))) {
            cancel();
            Q_EMIT failed(tr("Google Drive returned an unexpected account location."));
            return;
        }
        listing->url = target;
        listing->entries.clear();
    });
    connect(job, &KIO::ListJob::entries, this,
            [this, job, listing](KIO::Job *, const KIO::UDSEntryList &entries) {
        if (m_job != job || !isRoot(listing->url))
            return; // Never retain account file metadata during verification.
        for (const auto &entry : entries) {
            if (listing->entries.size() >= kMaxAccounts + 3)
                break;
            listing->entries.append(entry);
        }
    });
    connect(job, &KJob::result, this, [this, job, listing, operation](KJob *) {
        if (m_job != job)
            return;
        m_job.clear();
        if (job->error()) {
            Q_EMIT failed(connectionError(job->error(), job->errorString()));
            return;
        }
        if (operation == Operation::Verify) {
            Q_EMIT connected(canonicalAccount(listing->url));
            return;
        }
        const auto accounts = accountsForListing(listing->url, listing->entries);
        Q_EMIT accountsReady(accounts, isAccountUrl(listing->url)
            ? canonicalAccount(listing->url) : QUrl());
    });
}
