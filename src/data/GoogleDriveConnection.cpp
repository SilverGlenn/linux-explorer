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
#include <unistd.h>

namespace {
QPointer<GoogleDriveConnection> activeConnection;
constexpr qsizetype kMaxPrivateOutput = 64 * 1024;
}

GoogleDriveConnection::GoogleDriveConnection(QObject *parent) : QObject(parent)
{
    m_lastMountState = Locations::googleDriveMounted();
    m_mountTimer = new QTimer(this);
    m_mountTimer->setInterval(2000);
    m_mountTimer->setTimerType(Qt::CoarseTimer);
    connect(m_mountTimer, &QTimer::timeout, this, &GoogleDriveConnection::refreshMountState);
    m_mountTimer->start();
    m_timeout = new QTimer(this);
    m_timeout->setSingleShot(true);
    connect(m_timeout, &QTimer::timeout, this, [this] {
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
    cancel();
    if (m_auth && m_auth->isBusy()) {
        // A keychain write or requested revocation may already be committed.
        // Let its secure completion finish if an explicitly owned controller
        // is destroyed; no detached operation opens a browser or starts a mount.
        auto *pending = m_auth.data();
        pending->setParent(qApp);
        const auto retainedLock = std::shared_ptr<QLockFile>(m_operationLock.release());
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
    return Locations::googleDriveMounted();
}

void GoogleDriveConnection::refreshMountState()
{
    const bool mounted = isConnected();
    if (mounted == m_lastMountState)
        return;
    m_lastMountState = mounted;
    Q_EMIT mountStateChanged(mounted);
}

QString GoogleDriveConnection::appConfigPath() const
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
        + QStringLiteral("/linux-explorer-win7/google-drive/app-rclone.conf");
}

bool GoogleDriveConnection::lockOperation()
{
    const QString runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    const QFileInfo directory(runtime);
    const auto publicPermissions = QFileDevice::ReadGroup | QFileDevice::WriteGroup
        | QFileDevice::ExeGroup | QFileDevice::ReadOther | QFileDevice::WriteOther
        | QFileDevice::ExeOther;
    if (runtime.isEmpty() || directory.isSymLink() || !directory.isDir()
        || directory.ownerId() != ::getuid() || (directory.permissions() & publicPermissions)) {
        Q_EMIT failed(tr("The private desktop session directory is unavailable. Sign in to your desktop again and retry."));
        return false;
    }
    auto lock = std::make_unique<QLockFile>(QDir(runtime).filePath(
        QStringLiteral("explorer-win7-google-drive-auth.lock")));
    lock->setStaleLockTime(0);
    if (!lock->tryLock()) {
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
        m_operation = Operation::Forget;
        startHelper({QStringLiteral("app-forget")});
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
            Q_EMIT connected(Locations::googleDriveMount());
        }
        return;
    }
    if (!lockOperation())
        return;
    m_cancelled = false;
    m_operation = Operation::Connect;
    if (forceBrowser || !QFileInfo::exists(appConfigPath()))
        auth()->startSignIn();
    else
        auth()->connectExisting();
}

void GoogleDriveConnection::connectDrive() { beginSignIn(false); }
void GoogleDriveConnection::signInAgain() { beginSignIn(true); }

void GoogleDriveConnection::disconnectDrive()
{
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
    startHelper({QStringLiteral("disconnect")});
}

void GoogleDriveConnection::signOut()
{
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
    startHelper({QStringLiteral("disconnect")});
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
        m_operation = Operation::Forget;
        startHelper({QStringLiteral("app-forget")});
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
        if (isConnected())
            Q_EMIT connected(Locations::googleDriveMount());
        else
            Q_EMIT failed(tr("Google Drive did not provide a verified read-only mount."));
    } else if (operation == Operation::Disconnect) {
        Q_EMIT disconnected();
    } else if (operation == Operation::Forget) {
        Q_EMIT signedOut();
        if (!m_signOutWarning.isEmpty())
            Q_EMIT failed(m_signOutWarning);
        m_signOutWarning.clear();
    }
}

void GoogleDriveConnection::finishFailure(const QString &message)
{
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
    if (m_operation == Operation::Export || m_operation == Operation::Forget)
        return;
    m_cancelled = true;
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
}
