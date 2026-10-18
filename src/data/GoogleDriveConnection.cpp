#include "GoogleDriveConnection.h"

#include "GoogleDriveAuth.h"
#include "GoogleDriveBuildConfig.h"
#include "Locations.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>
#include <QSet>
#include <QSocketNotifier>
#include <QSettings>
#include <utility>
#include <fcntl.h>
#include <unistd.h>

namespace {
QPointer<GoogleDriveConnection> activeConnection;
constexpr qsizetype kMaxPrivateOutput = 64 * 1024;
}

GoogleDriveConnection::GoogleDriveConnection(QObject *parent) : QObject(parent)
{
    const auto initialMount = Locations::googleDriveMountState();
    m_lastMountState = initialMount.mounted;
    m_connectionState = m_lastMountState ? ConnectionState::Connected : ConnectionState::Disconnected;
    m_observedMountId = initialMount.mountId;
    m_quotaTimeout = new QTimer(this);
    m_quotaTimeout->setSingleShot(true);
    m_quotaTimeout->setInterval(15000);
    connect(m_quotaTimeout, &QTimer::timeout, this, [this] {
        cancelStorageQuery();
        clearStorageQuota();
    });
    m_mountTimer = new QTimer(this);
    m_mountTimer->setTimerType(Qt::CoarseTimer);
    connect(m_mountTimer, &QTimer::timeout, this, &GoogleDriveConnection::refreshMountState);

    // The kernel raises an exceptional condition (POLLPRI) on the mount table
    // whenever it changes, so watching that replaces polling it. A burst of
    // changes, as at login, is folded into one refresh.
    m_mountDebounce = new QTimer(this);
    m_mountDebounce->setSingleShot(true);
    m_mountDebounce->setInterval(100);
    connect(m_mountDebounce, &QTimer::timeout, this, &GoogleDriveConnection::refreshMountState);
    m_mountInfoFd = ::open("/proc/self/mountinfo", O_RDONLY | O_CLOEXEC);
    if (m_mountInfoFd >= 0) {
        m_mountNotifier = new QSocketNotifier(m_mountInfoFd, QSocketNotifier::Exception, this);
        connect(m_mountNotifier, &QSocketNotifier::activated, this, [this] {
            if (!m_mountDebounce->isActive())
                m_mountDebounce->start();
        });
    }
    // Only a safety net when the notifier is working, and the old two second
    // poll when it could not be set up
    m_mountTimer->setInterval(m_mountNotifier ? 30000 : 2000);
    m_mountTimer->start();
    m_reconnectTimer = new QTimer(this);
    m_reconnectTimer->setSingleShot(true);
    m_reconnectTimer->setTimerType(Qt::CoarseTimer);
    connect(m_reconnectTimer, &QTimer::timeout, this, [this] {
        m_reconnectStarted = false;
        reconnectSavedDrive();
    });
    m_timeout = new QTimer(this);
    m_timeout->setSingleShot(true);
    connect(m_timeout, &QTimer::timeout, this, [this] {
        if (m_automaticReconnect) {
            finishAutomaticFailure(ConnectionState::Offline);
            return;
        }
        if (m_operation == Operation::Export) {
            m_exportInvalid = true;
            stopHelper();
            return;
        }
        if (m_operation == Operation::Forget) {
            stopHelper();
            return;
        }
        cancel();
        Q_EMIT failed(tr("Google Drive did not respond in time. Try connecting again."));
    });
}

GoogleDriveConnection::~GoogleDriveConnection()
{
    delete m_mountNotifier;
    m_mountNotifier = nullptr;
    if (m_mountInfoFd >= 0)
        ::close(m_mountInfoFd);
    cancelStorageQuery();
    cancel();
    if (m_auth && m_auth->isBusy()) {
        // A keychain write or requested revocation may already be committed.
        // Let its secure completion finish if an explicitly owned controller
        // is destroyed; no detached operation opens a browser or starts a mount.
        auto *pending = m_auth.data();
        pending->setParent(qApp);
        const auto retainedLock = std::exchange(m_operationLock, {});
        connect(pending, &GoogleDriveAuth::idle, pending, [retainedLock] {
            if (retainedLock)
                retainedLock->unlock();
        });
        connect(pending, &GoogleDriveAuth::idle, pending, &QObject::deleteLater);
        m_auth.clear();
    }
}

GoogleDriveConnection *GoogleDriveConnection::sharedConnection()
{
    static QPointer<GoogleDriveConnection> connection;
    if (!connection)
        connection = new GoogleDriveConnection(qApp);
    return connection;
}

QString GoogleDriveConnection::helperPath() const
{
    if (!m_helperOverride.isEmpty())
        return m_helperOverride;
    return QCoreApplication::applicationDirPath() + QStringLiteral("/explorer-win7-google-drive");
}

QString GoogleDriveConnection::unavailableReason() const
{
    if (QString::fromLatin1(GoogleDriveBuildConfig::ClientId).isEmpty())
        return tr("Google Drive sign-in is unavailable in this build. Contact the application distributor.");
    if (!QFileInfo(helperPath()).isExecutable()
        || QStandardPaths::findExecutable(QStringLiteral("rclone")).isEmpty()
        || QStandardPaths::findExecutable(QStringLiteral("python3")).isEmpty()
        || QStandardPaths::findExecutable(QStringLiteral("fusermount3")).isEmpty())
        return tr("Google Drive support is not fully installed. Contact the application distributor.");
    return {};
}

bool GoogleDriveConnection::isAvailable() const
{
    return unavailableReason().isEmpty();
}

bool GoogleDriveConnection::isConnected() const
{
    return m_mountSnapshot ? m_mountSnapshot->first : Locations::googleDriveMounted();
}

GoogleDriveConnection::ConnectionState GoogleDriveConnection::connectionState() const { return m_connectionState; }
bool GoogleDriveConnection::isAutomaticReconnectInProgress() const { return m_automaticReconnect; }
bool GoogleDriveConnection::autoReconnectEnabled() const
{
    return QSettings().value(QStringLiteral("GoogleDrive/AutoReconnectEnabled"), true).toBool();
}
bool GoogleDriveConnection::autoReconnectSuppressed() const
{
    return QSettings().value(QStringLiteral("GoogleDrive/AutoReconnectSuppressed"), false).toBool();
}
void GoogleDriveConnection::setSyncLifecycleHooks(PrepareDisconnect prepare, CleanupAccount cleanup)
{
    m_prepareDisconnect = std::move(prepare);
    m_cleanupAccount = std::move(cleanup);
}
void GoogleDriveConnection::setConnectionState(ConnectionState state)
{
    if (m_connectionState == state)
        return;
    m_connectionState = state;
    Q_EMIT stateChanged(state);
}
void GoogleDriveConnection::setAutoReconnectEnabled(bool enabled)
{
    QSettings settings;
    settings.setValue(QStringLiteral("GoogleDrive/AutoReconnectEnabled"), enabled);
    if (enabled)
        settings.setValue(QStringLiteral("GoogleDrive/AutoReconnectSuppressed"), false);
    settings.sync();
    m_reconnectTimer->stop();
    m_reconnectStarted = false;
    m_reconnectAttempts = 0;
    if (!enabled && m_automaticReconnect)
        cancel();
    Q_EMIT autoReconnectPreferenceChanged(enabled);
}
void GoogleDriveConnection::suppressAutoReconnect()
{
    QSettings settings;
    settings.setValue(QStringLiteral("GoogleDrive/AutoReconnectSuppressed"), true);
    settings.sync();
    m_reconnectTimer->stop();
    m_reconnectStarted = true;
}
void GoogleDriveConnection::scheduleReconnectRetry()
{
    if (!autoReconnectEnabled() || autoReconnectSuppressed() || m_reconnectAttempts >= 3)
        return;
    const int delay = m_reconnectAttempts <= 1 ? 15000 : 60000;
    m_reconnectTimer->start(delay);
}
void GoogleDriveConnection::reconnectSavedDrive()
{
    if (!autoReconnectEnabled() || autoReconnectSuppressed() || m_reconnectStarted
        || m_operation != Operation::None || (m_auth && m_auth->isBusy()))
        return;
    if (isConnected()) {
        m_reconnectStarted = true;
        setConnectionState(ConnectionState::Connected);
        refreshMountState();
        return; // observers must not open the wallet for an existing mount
    }
    if (m_reconnectAttempts >= 3)
        return;
    if (!isAvailable()) {
        m_reconnectStarted = true;
        setConnectionState(ConnectionState::Unavailable);
        return;
    }
    if (!QFileInfo::exists(appConfigPath())) {
        m_reconnectStarted = true;
        setConnectionState(ConnectionState::SignInRequired);
        return;
    }
    ++m_reconnectAttempts;
    if ((activeConnection && activeConnection != this) || !lockOperation(true)) {
        scheduleReconnectRetry();
        return;
    }
    // Another process may have finished mounting before we acquired its lock.
    if (isConnected()) {
        releaseOperation();
        m_reconnectStarted = true;
        setConnectionState(ConnectionState::Connected);
        refreshMountState();
        return;
    }
    m_reconnectStarted = true;
    m_automaticReconnect = true;
    m_cancelled = false;
    m_operation = Operation::Connect;
    setConnectionState(ConnectionState::Connecting);
    auth()->connectSavedAccount();
}
void GoogleDriveConnection::finishAutomaticFailure(ConnectionState state)
{
    if (m_process) {
        stopHelper();
        ++m_generation;
        m_process.clear();
    }
    m_timeout->stop();
    m_operation = Operation::None;
    m_automaticReconnect = false;
    m_output.fill('\0');
    m_output.clear();
    releaseOperation();
    setConnectionState(state);
    if (state == ConnectionState::Offline)
        scheduleReconnectRetry();
}

void GoogleDriveConnection::refreshMountState()
{
    // One parse of the mount table for both questions
    const auto state = Locations::googleDriveMountState();
    m_mountSnapshot.emplace(state.mounted, state.mountId);
    const bool mounted = isConnected();
    const QString mountId = storageMountId();
    m_mountSnapshot.reset();
    const bool replaced = mountId != m_observedMountId;
    if (replaced) {
        m_observedMountId = mountId;
        cancelStorageQuery();
        clearStorageQuota();
        m_requestedQuotaMountId.clear();
        m_quotaAge.invalidate();
    }
    if (mounted == m_lastMountState && !replaced)
        return;
    m_lastMountState = mounted;
    Q_EMIT mountStateChanged(mounted);
    if (mounted) {
        m_reconnectTimer->stop();
        setConnectionState(ConnectionState::Connected);
    } else if (m_operation == Operation::None && m_connectionState == ConnectionState::Connected) {
        setConnectionState(ConnectionState::Disconnected);
    }
}

QString GoogleDriveConnection::storageMountId() const
{
    // The ID is only ever set for a valid mount, so no separate mounted check
    return m_mountSnapshot ? m_mountSnapshot->second
                           : Locations::googleDriveMountState().mountId;
}

bool GoogleDriveConnection::hasStorageQuota() const
{
    return m_quotaKnown && !m_quotaMountId.isEmpty() && m_quotaMountId == storageMountId();
}

quint64 GoogleDriveConnection::storageTotalBytes() const
{
    return hasStorageQuota() ? m_quotaTotal : 0;
}

quint64 GoogleDriveConnection::storageAvailableBytes() const
{
    return hasStorageQuota() ? m_quotaAvailable : 0;
}

void GoogleDriveConnection::cancelStorageQuery()
{
    ++m_quotaGeneration;
    m_quotaTimeout->stop();
    const auto process = m_quotaProcess;
    m_quotaProcess.clear();
    m_quotaOutput.clear();
    if (process) {
        m_stoppingQuotaProcess = process;
        process->kill(); // finished owns deletion; never wait on FUSE in the UI
    }
}

void GoogleDriveConnection::clearStorageQuota()
{
    const bool known = m_quotaKnown;
    m_quotaKnown = false;
    m_quotaMountId.clear();
    m_quotaTotal = m_quotaAvailable = 0;
    if (known)
        Q_EMIT storageQuotaChanged();
}

void GoogleDriveConnection::refreshStorageQuota(bool force)
{
    // A new descriptor must not race unmount or connection-failure cleanup.
    // Authentication/mount operations have priority over background figures.
    if (m_operation != Operation::None)
        return;
    if (m_stoppingQuotaProcess && m_stoppingQuotaProcess->state() != QProcess::NotRunning)
        return; // never accumulate stalled helpers after a timeout
    const QString id = storageMountId();
    if (id.isEmpty()) {
        cancelStorageQuery();
        clearStorageQuota();
        return;
    }
    if (m_quotaProcess && m_requestedQuotaMountId == id)
        return;
    if (!force && m_requestedQuotaMountId == id
        && m_quotaAge.isValid() && m_quotaAge.elapsed() < 30000)
        return;
    cancelStorageQuery();
    m_requestedQuotaMountId = id;
    m_quotaAge.start();
    auto *process = new QProcess(qApp);
    m_quotaProcess = process;
    const quint64 generation = m_quotaGeneration;
    const QPointer<GoogleDriveConnection> owner(this);
    process->setStandardInputFile(QProcess::nullDevice());
    process->setStandardErrorFile(QProcess::nullDevice());
    connect(process, &QProcess::readyReadStandardOutput, process, [owner, process, generation] {
        if (!owner || owner->m_quotaProcess != process || owner->m_quotaGeneration != generation)
            return;
        const auto data = process->read(4097 - owner->m_quotaOutput.size());
        owner->m_quotaOutput += data;
        if (owner->m_quotaOutput.size() > 4096) {
            owner->cancelStorageQuery();
            owner->clearStorageQuota();
        }
    });
    connect(process, &QProcess::finished, process,
            [owner, process, generation](int code, QProcess::ExitStatus status) {
        if (owner)
            owner->finishStorageQuery(process, generation,
                code == 0 && status == QProcess::NormalExit);
        if (owner && owner->m_stoppingQuotaProcess == process)
            owner->m_stoppingQuotaProcess.clear();
        process->deleteLater();
    });
    connect(process, &QProcess::errorOccurred, process,
            [owner, process, generation](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;
        if (owner)
            owner->finishStorageQuery(process, generation, false);
        if (owner && owner->m_stoppingQuotaProcess == process)
            owner->m_stoppingQuotaProcess.clear();
        process->deleteLater();
    });
    m_quotaTimeout->start();
    process->start(helperPath(), {QStringLiteral("app-quota")});
}

void GoogleDriveConnection::finishStorageQuery(QProcess *process, quint64 generation, bool successful)
{
    if (m_quotaProcess != process || m_quotaGeneration != generation)
        return;
    m_quotaTimeout->stop();
    m_quotaProcess.clear();
    m_quotaOutput += process->read(4097 - m_quotaOutput.size());
    const QByteArray output = std::exchange(m_quotaOutput, {});
    const auto document = QJsonDocument::fromJson(output);
    const auto object = document.object();
    const auto keys = object.keys();
    const QSet<QString> expected{QStringLiteral("mount_id"), QStringLiteral("total"), QStringLiteral("available")};
    const auto number = [&object](const QString &key, bool *valid) -> quint64 {
        const QJsonValue value = object.value(key);
        const QString text = value.toString();
        *valid = value.isString() && !text.isEmpty() && text.size() <= 20
            && (text.size() == 1 || text.front() != u'0');
        for (QChar ch : text)
            if (ch < u'0' || ch > u'9') *valid = false;
        if (!*valid) return 0;
        return text.toULongLong(valid);
    };
    bool totalValid = false, availableValid = false;
    const quint64 total = number(QStringLiteral("total"), &totalValid);
    const quint64 available = number(QStringLiteral("available"), &availableValid);
    const QString id = object.value(QStringLiteral("mount_id")).toString();
    constexpr quint64 assumedCapacity = quint64(1) << 50;
    if (!successful || output.size() > 4096 || !document.isObject()
        || QSet<QString>(keys.begin(), keys.end()) != expected
        || id != m_requestedQuotaMountId || id != storageMountId()
        || !totalValid || !availableValid || total == 0 || available > total
        || total == assumedCapacity || available == assumedCapacity) {
        clearStorageQuota();
        return;
    }
    const bool changed = !m_quotaKnown || m_quotaMountId != id
        || m_quotaTotal != total || m_quotaAvailable != available;
    m_quotaMountId = id;
    m_quotaTotal = total;
    m_quotaAvailable = available;
    m_quotaKnown = true;
    if (changed)
        Q_EMIT storageQuotaChanged();
}

QString GoogleDriveConnection::appConfigPath() const
{
    if (!m_appConfigOverride.isEmpty())
        return m_appConfigOverride;
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
        + QStringLiteral("/linux-explorer-win7/google-drive/app-rclone.conf");
}

bool GoogleDriveConnection::lockOperation(bool quiet)
{
    const QString runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    const QFileInfo directory(runtime);
    const auto publicPermissions = QFileDevice::ReadGroup | QFileDevice::WriteGroup
        | QFileDevice::ExeGroup | QFileDevice::ReadOther | QFileDevice::WriteOther
        | QFileDevice::ExeOther;
    if (runtime.isEmpty() || directory.isSymLink() || !directory.isDir()
        || directory.ownerId() != ::getuid() || (directory.permissions() & publicPermissions)) {
        if (!quiet)
            Q_EMIT failed(tr("The private desktop session directory is unavailable. Sign in to your desktop again and retry."));
        return false;
    }
    auto lock = std::make_shared<QLockFile>(QDir(runtime).filePath(
        QStringLiteral("explorer-win7-google-drive-auth.lock")));
    lock->setStaleLockTime(0);
    if (!lock->tryLock()) {
        if (!quiet)
            Q_EMIT failed(tr("Google Drive is already being changed in another Explorer session. Try again when it finishes."));
        return false;
    }
    m_operationLock = std::move(lock);
    activeConnection = this;
    return true;
}

void GoogleDriveConnection::releaseOperation()
{
    if (m_operation != Operation::None || (m_auth && m_auth->isBusy()))
        return;
    if (activeConnection == this)
        activeConnection.clear();
    m_operationLock.reset();
}

GoogleDriveAuth *GoogleDriveConnection::auth()
{
    if (m_auth)
        return m_auth;
    m_auth = m_injectedAuth ? m_injectedAuth.data() : new GoogleDriveAuth(
        {QString::fromLatin1(GoogleDriveBuildConfig::ClientId),
         QString::fromLatin1(GoogleDriveBuildConfig::ClientSecret)}, this);
    connect(m_auth, &GoogleDriveAuth::idle, this, [this] {
        releaseOperation();
    });
    connect(m_auth, &GoogleDriveAuth::statusChanged, this, &GoogleDriveConnection::statusChanged);
    connect(m_auth, &GoogleDriveAuth::savedAccountUnavailable, this,
        [this](GoogleDriveAuth::SavedAccountResult result) {
            if (!m_automaticReconnect || m_operation != Operation::Connect)
                return;
            finishAutomaticFailure(result == GoogleDriveAuth::SavedAccountResult::WalletLocked
                ? ConnectionState::WalletLocked
                : result == GoogleDriveAuth::SavedAccountResult::Unavailable
                    ? ConnectionState::Unavailable : ConnectionState::SignInRequired);
        });
    connect(m_auth, &GoogleDriveAuth::failed, this, [this](const QString &message) {
        if (m_operation != Operation::None && !m_cancelled)
            finishFailure(message);
    });
    connect(m_auth, &GoogleDriveAuth::authorized, this,
            [this](const QByteArray &token, const QString &password) {
        if (m_cancelled || m_operation != Operation::Connect)
            return;
        const QByteArray input = envelope(token, password);
        if (input.isEmpty()) {
            finishFailure(tr("Google Drive returned an incomplete authorization. Sign in again."));
            return;
        }
        Q_EMIT statusChanged(tr("Connecting to Google Drive…"));
        startHelper({QStringLiteral("app-connect"), QStringLiteral("--no-open")}, input);
    });
    connect(m_auth, &GoogleDriveAuth::revocationTokenRequested, this,
            [this](const QString &password) {
        if (m_cancelled || m_operation != Operation::Export)
            return;
        startHelper({QStringLiteral("app-export-token")}, envelope({}, password));
    });
    connect(m_auth, &GoogleDriveAuth::signedOutLocally, this,
            [this](const QString &warning) {
        if (m_cancelled || m_operation != Operation::Export)
            return;
        m_signOutWarning = warning;
        if (m_process) {
            // The auth timeout can settle before a stalled export. Wait until
            // that read-only helper exits and releases its operation lock.
            m_forgetPending = true;
            stopHelper();
            return;
        }
        beginLocalAccountCleanup();
    });
    return m_auth;
}

QByteArray GoogleDriveConnection::envelope(const QByteArray &token, const QString &password) const
{
    QJsonValue tokenValue(QJsonValue::Null);
    if (!token.isEmpty()) {
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(token, &error);
        if (error.error != QJsonParseError::NoError || !document.isObject())
            return {};
        tokenValue = document.object();
    }
    return QJsonDocument(QJsonObject{
        {QStringLiteral("version"), 1},
        {QStringLiteral("client_id"), QString::fromLatin1(GoogleDriveBuildConfig::ClientId)},
        {QStringLiteral("client_secret"), QString::fromLatin1(GoogleDriveBuildConfig::ClientSecret)},
        {QStringLiteral("token"), tokenValue},
        {QStringLiteral("config_password"), password}
    }).toJson(QJsonDocument::Compact);
}

void GoogleDriveConnection::beginSignIn(bool forceBrowser)
{
    if (m_automaticReconnect)
        cancel();
    m_reconnectTimer->stop();
    if (m_operation != Operation::None)
        return;
    if (m_auth && m_auth->isBusy()) {
        Q_EMIT failed(tr("Google Drive is finishing a credential operation. Please try again shortly."));
        return;
    }
    if (activeConnection && activeConnection != this) {
        Q_EMIT failed(tr("Google Drive is already being connected in another Explorer window."));
        return;
    }
    if (!isAvailable()) {
        Q_EMIT failed(unavailableReason());
        return;
    }
    if (isConnected()) {
        if (forceBrowser) {
            Q_EMIT failed(tr("Disconnect Google Drive before signing in again."));
        } else {
            QSettings().setValue(QStringLiteral("GoogleDrive/AutoReconnectSuppressed"), false);
            setConnectionState(ConnectionState::Connected);
            Q_EMIT connected(Locations::googleDriveMount());
        }
        return;
    }
    if (!lockOperation())
        return;
    m_cancelled = false;
    m_operation = Operation::Connect;
    setConnectionState(ConnectionState::Connecting);
    if (forceBrowser || !QFileInfo::exists(appConfigPath()))
        auth()->startSignIn();
    else
        auth()->connectExisting();
}

void GoogleDriveConnection::connectDrive() { beginSignIn(false); }
void GoogleDriveConnection::signInAgain() { beginSignIn(true); }

void GoogleDriveConnection::disconnectDrive()
{
    suppressAutoReconnect();
    if (m_automaticReconnect)
        cancel();
    if (m_operation != Operation::None)
        return;
    if (activeConnection && activeConnection != this) {
        Q_EMIT failed(tr("Google Drive is already being changed in another Explorer window."));
        return;
    }
    if (!lockOperation())
        return;
    m_cancelled = false;
    m_operation = Operation::Disconnect;
    Q_EMIT statusChanged(tr("Disconnecting Google Drive…"));
    prepareDisconnect(false);
}

void GoogleDriveConnection::signOut()
{
    suppressAutoReconnect();
    if (m_automaticReconnect)
        cancel();
    if (m_operation != Operation::None)
        return;
    if (activeConnection && activeConnection != this) {
        Q_EMIT failed(tr("Google Drive is already being changed in another Explorer window."));
        return;
    }
    if (!isAvailable()) {
        Q_EMIT failed(unavailableReason());
        return;
    }
    if (!lockOperation())
        return;
    m_cancelled = false;
    m_signOutWarning.clear();
    m_operation = Operation::SignOutDisconnect;
    Q_EMIT statusChanged(tr("Signing out of Google Drive…"));
    // Never revoke or erase credentials while a filesystem still uses them.
    prepareDisconnect(true);
}

void GoogleDriveConnection::prepareDisconnect(bool signOut)
{
    if (!m_prepareDisconnect) {
        startDisconnectHelper();
        return;
    }
    const quint64 generation = ++m_generation;
    const Operation operation = m_operation;
    const QPointer<GoogleDriveConnection> owner(this);
    const auto retainedLock = m_operationLock;
    m_prepareDisconnect(signOut, [owner, generation, operation, retainedLock](bool successful, QString message) {
        if (!owner || owner->m_cancelled || owner->m_generation != generation || owner->m_operation != operation)
            return;
        if (!successful) {
            owner->finishFailure(message.isEmpty() ? tr("Google Drive synchronization could not stop. Try again.") : message);
            return;
        }
        owner->startDisconnectHelper();
    });
    // A stop can queue behind the worker's bounded read-only folder check,
    // then wait for an active transfer to exit. Keep the outer deadline later
    // than the sync controller's stop deadline while retaining the lock.
    QTimer::singleShot(255000, this, [owner, generation, operation] {
        if (owner && owner->m_generation == generation && owner->m_operation == operation)
            owner->finishFailure(tr("Google Drive synchronization is still stopping. Try again."));
    });
}

void GoogleDriveConnection::beginLocalAccountCleanup()
{
    m_operation = Operation::Cleanup;
    if (!m_cleanupAccount) {
        m_operation = Operation::Forget;
        startHelper({QStringLiteral("app-forget")});
        return;
    }
    const quint64 generation = ++m_generation;
    const QPointer<GoogleDriveConnection> owner(this);
    const auto retainedLock = m_operationLock;
    m_cleanupAccount([owner, generation, retainedLock](bool successful, QString message) {
        if (!owner || owner->m_generation != generation || owner->m_operation != Operation::Cleanup)
            return;
        if (!successful) {
            owner->finishFailure(message.isEmpty() ? tr("Saved synchronization credentials could not be removed. Try signing out again.") : message);
            return;
        }
        owner->m_operation = Operation::Forget;
        owner->startHelper({QStringLiteral("app-forget")});
    });
    QTimer::singleShot(30000, this, [owner, generation] {
        if (owner && owner->m_generation == generation && owner->m_operation == Operation::Cleanup)
            owner->finishFailure(tr("Saved synchronization credentials are still being removed. Try signing out again."));
    });
}

void GoogleDriveConnection::startDisconnectHelper()
{
    const QPointer<QProcess> quota(m_quotaProcess ? m_quotaProcess : m_stoppingQuotaProcess);
    cancelStorageQuery();
    if (!quota || quota->state() == QProcess::NotRunning) {
        startHelper({QStringLiteral("disconnect")});
        return;
    }
    // kill() is asynchronous. Wait for the quota descriptor to be released
    // before unmounting, while keeping the UI responsive and the operation lock.
    const quint64 generation = ++m_generation;
    const Operation operation = m_operation;
    const QPointer<GoogleDriveConnection> owner(this);
    const auto advance = [owner, generation, operation] {
        if (owner && !owner->m_cancelled && owner->m_generation == generation
            && owner->m_operation == operation)
            owner->startHelper({QStringLiteral("disconnect")});
    };
    connect(quota, &QProcess::finished, this, advance);
    connect(quota, &QProcess::errorOccurred, this, [advance](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            advance();
    });
    QTimer::singleShot(5000, this, [owner, generation, operation] {
        if (owner && !owner->m_cancelled && owner->m_generation == generation
            && owner->m_operation == operation)
            owner->finishFailure(tr("Google Drive storage lookup is still stopping. Try disconnecting again."));
    });
}

void GoogleDriveConnection::startHelper(const QStringList &arguments, QByteArray input)
{
    if (m_process || m_cancelled)
        return;
    m_output.clear();
    m_exportInvalid = false;
    auto *process = new QProcess(qApp);
    m_process = process;
    const QPointer<GoogleDriveConnection> owner(this);
    const quint64 generation = ++m_generation;
    const Operation operation = m_operation;
    process->setProcessChannelMode(QProcess::SeparateChannels);
    connect(process, &QProcess::started, process, [process, input = std::move(input)]() mutable {
        if (!input.isEmpty())
            process->write(input);
        process->closeWriteChannel();
        input.fill('\0');
        input.clear();
    });
    connect(process, &QProcess::readyReadStandardOutput, process, [owner, process, generation] {
        const QByteArray data = process->readAllStandardOutput();
        if (!owner || owner->m_generation != generation
            || owner->m_operation != Operation::Export || owner->m_forgetPending)
            return;
        if (owner->m_output.size() + data.size() > kMaxPrivateOutput) {
            owner->m_exportInvalid = true;
            owner->m_output.clear();
            owner->stopHelper();
            return;
        }
        owner->m_output.append(data);
    });
    // Dependency output may contain account details. It is never displayed or
    // logged; the UI supplies bounded, task-specific errors.
    connect(process, &QProcess::readyReadStandardError, process,
            [process] { process->readAllStandardError(); });
    connect(process, &QProcess::finished, process,
            [owner, process, generation, operation](int code, QProcess::ExitStatus status) {
        if (owner && owner->m_generation == generation) {
            const QByteArray tail = process->readAllStandardOutput();
            if (owner->m_operation == Operation::Export) {
                if (owner->m_output.size() + tail.size() > kMaxPrivateOutput) {
                    owner->m_exportInvalid = true;
                    owner->m_output.clear();
                } else {
                    owner->m_output.append(tail);
                }
            }
            owner->m_process.clear();
            owner->m_timeout->stop();
            owner->helperFinished(operation, code, status == QProcess::NormalExit);
        }
        process->deleteLater();
    });
    connect(process, &QProcess::errorOccurred, process,
            [owner, process, generation, operation](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;
        if (owner && owner->m_generation == generation) {
            owner->m_process.clear();
            owner->m_timeout->stop();
            if (operation == Operation::Export)
                owner->helperFinished(operation, 1, false);
            else
                owner->finishFailure(tr("Google Drive support could not start. Check the application installation."));
        }
        process->deleteLater();
    });
    m_timeout->start(operation == Operation::Export ? 35000
        : operation == Operation::Forget ? 30000 : 120000);
    process->start(helperPath(), arguments);
}

void GoogleDriveConnection::helperFinished(Operation operation, int exitCode, bool normalExit)
{
    refreshMountState();
    if (m_cancelled)
        return;
    if (m_forgetPending && operation == Operation::Export) {
        m_forgetPending = false;
        m_output.fill('\0');
        m_output.clear();
        beginLocalAccountCleanup();
        return;
    }
    if (operation != m_operation)
        return;
    if (operation == Operation::Export) {
        QByteArray token;
        if (normalExit && exitCode == 0 && !m_exportInvalid)
            token = m_output;
        m_output.fill('\0');
        m_output.clear();
        // An empty export invokes the auth backend's truthful local-only
        // sign-out result; it never substitutes a potentially stale token.
        auth()->completeSignOut(token);
        token.fill('\0');
        return;
    }
    if (!normalExit || exitCode != 0) {
        finishFailure(operation == Operation::Connect
            ? tr("Google Drive could not connect. Check your connection, or choose Sign in again.")
            : tr("Google Drive could not be disconnected or signed out. Close files using it and try again."));
        return;
    }
    if (operation == Operation::SignOutDisconnect) {
        if (isConnected()) {
            finishFailure(tr("Google Drive is still connected. Close files using it and try again."));
            return;
        }
        m_operation = Operation::Export;
        Q_EMIT disconnected();
        auth()->signOut();
        return;
    }
    m_operation = Operation::None;
    releaseOperation();
    if (operation == Operation::Connect) {
        if (isConnected()) {
            const bool automatic = std::exchange(m_automaticReconnect, false);
            setConnectionState(ConnectionState::Connected);
            m_reconnectTimer->stop();
            if (!automatic) {
                QSettings settings;
                settings.setValue(QStringLiteral("GoogleDrive/AutoReconnectSuppressed"), false);
                if (!settings.contains(QStringLiteral("GoogleDrive/AutoReconnectEnabled")))
                    settings.setValue(QStringLiteral("GoogleDrive/AutoReconnectEnabled"), true);
                settings.sync();
                m_reconnectAttempts = 0;
                Q_EMIT autoReconnectPreferenceChanged(autoReconnectEnabled());
                Q_EMIT connected(Locations::googleDriveMount());
            }
            refreshStorageQuota();
        } else {
            if (m_automaticReconnect)
                finishAutomaticFailure(ConnectionState::Offline);
            else {
                setConnectionState(ConnectionState::Disconnected);
                Q_EMIT failed(tr("Google Drive did not provide a verified read-only mount."));
            }
        }
    } else if (operation == Operation::Disconnect) {
        setConnectionState(ConnectionState::Disconnected);
        Q_EMIT disconnected();
    } else if (operation == Operation::Forget) {
        setConnectionState(ConnectionState::SignInRequired);
        Q_EMIT signedOut();
        if (!m_signOutWarning.isEmpty())
            Q_EMIT failed(m_signOutWarning);
        m_signOutWarning.clear();
    }
}

void GoogleDriveConnection::finishFailure(const QString &message)
{
    if (m_automaticReconnect) {
        finishAutomaticFailure(ConnectionState::Offline);
        return;
    }
    if (m_process) {
        stopHelper();
        ++m_generation;
        m_process.clear();
    }
    m_timeout->stop();
    m_operation = Operation::None;
    m_output.fill('\0');
    m_output.clear();
    releaseOperation();
    setConnectionState(isConnected() ? ConnectionState::Connected : ConnectionState::Disconnected);
    Q_EMIT failed(message);
}

void GoogleDriveConnection::stopHelper()
{
    if (!m_process)
        return;
    const QPointer<QProcess> process(m_process);
    process->terminate();
    QTimer::singleShot(5000, qApp, [process] {
        if (process && process->state() != QProcess::NotRunning)
            process->kill();
    });
}

void GoogleDriveConnection::cancel()
{
    // After disconnection, an explicitly requested sign-out must finish local
    // credential removal even if its dialog closes.
    if (m_operation == Operation::Export || m_operation == Operation::Cleanup || m_operation == Operation::Forget)
        return;
    m_cancelled = true;
    m_automaticReconnect = false;
    ++m_generation;
    m_timeout->stop();
    if (m_auth)
        m_auth->cancel();
    stopHelper();
    m_process.clear();
    m_operation = Operation::None;
    m_output.fill('\0');
    m_output.clear();
    releaseOperation();
    setConnectionState(isConnected() ? ConnectionState::Connected : ConnectionState::Disconnected);
}
