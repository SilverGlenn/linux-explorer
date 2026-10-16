#include "GoogleDriveSyncController.h"

#include "GoogleDriveAuth.h"
#include "GoogleDriveBuildConfig.h"
#include "GoogleDriveConnection.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QDir>
#include <QJsonDocument>
#include <QLockFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTimer>
#include <memory>
#include <utility>
#include <unistd.h>

namespace {
constexpr qsizetype maxResponse = 1024 * 1024;
constexpr qsizetype maxCredentials = 64 * 1024;
// The worker allows bounded, read-only Drive inventory requests to run for
// longer than ordinary local commands. Keep the client outside its RPC budget.
constexpr int metadataRequestTimeoutMs = 195000;

bool isMetadataAction(const QString &action)
{
    return action == QLatin1String("preview") || action == QLatin1String("list-folders")
        || action == QLatin1String("review") || action == QLatin1String("resume-reviewed");
}

GoogleDriveAuth::ClientConfig clientConfig()
{
    return {QString::fromLatin1(GoogleDriveBuildConfig::ClientId),
            QString::fromLatin1(GoogleDriveBuildConfig::ClientSecret)};
}

QJsonObject credentialEnvelope(const QByteArray &token, const QString &password)
{
    return {{QStringLiteral("version"), 1},
            {QStringLiteral("client_id"), QString::fromLatin1(GoogleDriveBuildConfig::ClientId)},
            {QStringLiteral("client_secret"), QString::fromLatin1(GoogleDriveBuildConfig::ClientSecret)},
            {QStringLiteral("token"), token.isEmpty() ? QJsonValue(QJsonValue::Null)
                                                    : QJsonValue(QJsonDocument::fromJson(token).object())},
            {QStringLiteral("config_password"), password}};
}
}

GoogleDriveSyncController::GoogleDriveSyncController(QObject *parent) : QObject(parent)
{
    m_poll = new QTimer(this);
    m_poll->setInterval(5000);
    m_poll->setTimerType(Qt::CoarseTimer);
    connect(m_poll, &QTimer::timeout, this, &GoogleDriveSyncController::refresh);
}

GoogleDriveSyncController::~GoogleDriveSyncController()
{
    ++m_generation;
    if (m_process)
        m_process->kill();
    if (m_browseAuth)
        m_browseAuth->cancel();
    if (m_syncAuth)
        m_syncAuth->cancel();
    for (GoogleDriveAuth *auth : {m_browseAuth.data(), m_syncAuth.data()}) {
        if (auth && auth->isBusy()) {
            auth->disconnect(this);
            auth->setParent(qApp);
            const auto retainedLock = m_authorizationLock;
            connect(auth, &GoogleDriveAuth::idle, auth, [auth, retainedLock] {
                auth->deleteLater();
            });
        }
    }
    clearSecrets();
    // The worker owns its session and continues independently of this client.
}

GoogleDriveSyncController *GoogleDriveSyncController::sharedController()
{
    static QPointer<GoogleDriveSyncController> instance;
    if (!instance)
        instance = new GoogleDriveSyncController(qApp);
    return instance;
}

QString GoogleDriveSyncController::helperPath() const
{
    return QCoreApplication::applicationDirPath() + QStringLiteral("/explorer-win7-google-drive-sync");
}

bool GoogleDriveSyncController::isAvailable() const { return m_available; }
bool GoogleDriveSyncController::isBusy() const
{
    return m_process || m_authStage != AuthStage::None || m_stopping || m_authorizationLock
        || (!m_pendingRequest.isEmpty() && m_workerRunning);
}
bool GoogleDriveSyncController::workerRunning() const { return m_workerRunning; }
bool GoogleDriveSyncController::hasWritePermission() const { return m_workerRunning; }
bool GoogleDriveSyncController::hasSavedPermission() const
{
    return m_status.value(QStringLiteral("sync_configured")).toBool();
}
QString GoogleDriveSyncController::statusMessage() const { return m_message; }
QJsonArray GoogleDriveSyncController::pairs() const { return m_status.value(QStringLiteral("pairs")).toArray(); }

void GoogleDriveSyncController::clearSecrets()
{
    m_browsePassword.fill(QChar(0));
    m_browsePassword.clear();
}

bool GoogleDriveSyncController::lockAuthorization()
{
    const QString runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    const QFileInfo directory(runtime);
    const auto publicPermissions = QFileDevice::ReadGroup | QFileDevice::WriteGroup
        | QFileDevice::ExeGroup | QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther;
    if (runtime.isEmpty() || directory.isSymLink() || !directory.isDir()
        || directory.ownerId() != ::getuid() || (directory.permissions() & publicPermissions))
        return false;
    auto lock = std::make_shared<QLockFile>(QDir(runtime).filePath(
        QStringLiteral("explorer-win7-google-drive-auth.lock")));
    lock->setStaleLockTime(0);
    if (!lock->tryLock())
        return false;
    m_authorizationLock = std::move(lock);
    return true;
}

void GoogleDriveSyncController::releaseAuthorization()
{
    if (m_authStage == AuthStage::None && !m_process
        && (!m_browseAuth || !m_browseAuth->isBusy()) && (!m_syncAuth || !m_syncAuth->isBusy()))
        m_authorizationLock.reset();
}

void GoogleDriveSyncController::fail(const QString &message)
{
    m_authStage = AuthStage::None;
    m_pendingRequest = {};
    m_pendingCompletion = {};
    clearSecrets();
    releaseAuthorization();
    m_message = message.isEmpty() ? tr("Google Drive synchronization could not complete. Try again.")
                                  : message.left(2048);
    m_preserveError = m_interactive;
    Q_EMIT changed();
    if (m_interactive)
        Q_EMIT failed(m_message);
    else if (++m_restoreFailures < 3) {
        QTimer::singleShot(m_restoreFailures == 1 ? 15000 : 60000, this, [this] {
            auto *connection = GoogleDriveConnection::sharedConnection();
            if (!isBusy() && !m_workerRunning && connection->autoReconnectEnabled()
                && !connection->autoReconnectSuppressed() && connection->isConnected())
                restoreWorker();
        });
    }
}

void GoogleDriveSyncController::runHelper(const QString &command, QJsonObject request,
                                         std::function<void(bool, QJsonObject)> completion, int timeoutMs)
{
    if (m_process) {
        QTimer::singleShot(0, this, [completion] { completion(false, {}); });
        return;
    }
    if (!QFileInfo(helperPath()).isExecutable()) {
        QTimer::singleShot(0, this, [completion] {
            completion(false, {{QStringLiteral("message"), tr("The Google Drive synchronization helper is missing or cannot run.")}});
        });
        return;
    }
    const QString action = request.value(QStringLiteral("action")).toString();
    QByteArray encodedInput = request.isEmpty() ? QByteArray()
        : QJsonDocument(request).toJson(QJsonDocument::Compact);
    request = {};
    if (encodedInput.size() > maxCredentials) {
        encodedInput.fill('\0');
        QTimer::singleShot(0, this, [completion] { completion(false, {}); });
        return;
    }
    auto *process = new QProcess(qApp);
    process->setProperty("syncAction", action);
    m_process = process;
    auto *timer = new QTimer(process);
    timer->setSingleShot(true);
    auto output = std::make_shared<QByteArray>();
    auto input = std::make_shared<QByteArray>(std::move(encodedInput));
    const quint64 generation = m_generation;
    const QPointer<GoogleDriveSyncController> owner(this);
    auto completed = std::make_shared<bool>(false);
    auto failureMessage = std::make_shared<QString>(tr("The synchronization helper returned an invalid response. Try again."));
    auto finish = [owner, process, timer, output, input, generation, completed, completion, failureMessage](bool success) {
        if (*completed)
            return;
        *completed = true;
        timer->stop();
        input->fill('\0');
        input->clear();
        if (owner && owner->m_process == process)
            owner->m_process.clear();
        if (owner)
            owner->releaseAuthorization();
        QJsonObject result;
        if (success && output->size() <= maxResponse) {
            QJsonParseError error;
            const auto document = QJsonDocument::fromJson(*output, &error);
            if (error.error == QJsonParseError::NoError && document.isObject()
                && document.object().value(QStringLiteral("version")).toInt() == 1)
                result = document.object();
            else
                success = false;
        }
        output->fill('\0');
        output->clear();
        process->deleteLater();
        const bool validResponse = success && !result.isEmpty();
        if (!validResponse)
            result = {{QStringLiteral("message"), *failureMessage}};
        if (owner && owner->m_generation == generation)
            completion(validResponse, result);
        else if (owner)
            Q_EMIT owner->changed();
    };
    process->setProcessChannelMode(QProcess::SeparateChannels);
    process->setStandardErrorFile(QProcess::nullDevice());
    auto environment = QProcessEnvironment::systemEnvironment();
    for (const QString &key : environment.keys()) {
        if (key.startsWith(QStringLiteral("RCLONE_")))
            environment.remove(key);
    }
    process->setProcessEnvironment(environment);
    connect(process, &QProcess::started, process, [process, input] {
        process->write(*input);
        input->fill('\0');
        input->clear();
        process->closeWriteChannel();
    });
    connect(process, &QProcess::readyReadStandardOutput, process, [process, output, failureMessage] {
        *output += process->read(maxResponse + 1 - output->size());
        if (output->size() > maxResponse) {
            *failureMessage = tr("The synchronization response exceeds the safety limit.");
            process->kill();
        }
    });
    connect(process, &QProcess::errorOccurred, process, [finish, failureMessage](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            *failureMessage = tr("The Google Drive synchronization helper could not start.");
            finish(false);
        }
    });
    connect(process, &QProcess::finished, process,
            [process, output, finish](int code, QProcess::ExitStatus exit) {
        *output += process->read(maxResponse + 1 - output->size());
        Q_UNUSED(code);
        // A normal error exit still carries the versioned, sanitized error
        // response. A killed/timed-out helper must never report success.
        finish(exit == QProcess::NormalExit && output->size() <= maxResponse);
    });
    connect(timer, &QTimer::timeout, process, [process, command, action, failureMessage] {
        *failureMessage = isMetadataAction(action)
            ? tr("The folder check did not finish in time. Try a smaller folder or check your connection, then preview again.")
            : command == QLatin1String("status")
                ? tr("Synchronization status could not be checked. The worker may still be busy; try again shortly.")
                : tr("Google Drive did not respond in time. Check the synchronization status before trying again.");
        process->kill();
    });
    process->start(helperPath(), {command});
    timer->start(timeoutMs);
    Q_EMIT changed();
}

void GoogleDriveSyncController::acceptStatus(const QJsonObject &status, bool preserveMessage)
{
    if (!status.value(QStringLiteral("pairs")).isArray()
        || status.value(QStringLiteral("pairs")).toArray().size() > 100)
        return;
    m_status = status;
    m_available = status.value(QStringLiteral("available")).toBool();
    m_workerRunning = status.value(QStringLiteral("worker_running")).toBool();
    if (!preserveMessage) {
        m_preserveError = false;
        m_message = status.value(QStringLiteral("message")).toString().left(2048);
        if (m_message.isEmpty())
            m_message = m_workerRunning ? tr("Google Drive synchronization is ready.")
                                       : tr("Allow editing in Google Drive to set up synchronization.");
    }
    Q_EMIT changed();
}

void GoogleDriveSyncController::refresh()
{
    if (isBusy())
        return;
    if (!m_poll->isActive())
        m_poll->start();
    runHelper(QStringLiteral("status"), {}, [this](bool ok, QJsonObject result) {
        if (ok)
            acceptStatus(result, m_preserveError);
        else {
            // A missed poll says nothing about a worker's lifetime or saved
            // permission. Keep the last verified state until a valid response.
            if (!m_preserveError)
                m_message = tr("Synchronization status could not be checked. The worker may still be busy; try again shortly.");
            Q_EMIT changed();
        }
    });
}

void GoogleDriveSyncController::ensureAuth()
{
    if (m_browseAuth)
        return;
    m_browseAuth = new GoogleDriveAuth(clientConfig(), this);
    m_syncAuth = new GoogleDriveAuth(clientConfig(), GoogleDriveAuth::AccountPurpose::Sync, this);
    connect(m_browseAuth, &GoogleDriveAuth::authorized, this,
            [this](const QByteArray &, const QString &password) {
        if (m_authStage != AuthStage::Browse)
            return;
        m_browsePassword = password;
        m_authStage = AuthStage::Sync;
        if (!m_interactive)
            m_syncAuth->connectSavedAccount();
        else if (hasSavedPermission() && !m_forceSignIn)
            m_syncAuth->connectSavedAccountInteractively();
        else
            m_syncAuth->startSignIn();
    });
    connect(m_syncAuth, &GoogleDriveAuth::authorized, this,
            [this](const QByteArray &token, const QString &password) {
        if (m_authStage == AuthStage::Sync)
            startWorker(token, password);
    });
    for (GoogleDriveAuth *auth : {m_browseAuth.data(), m_syncAuth.data()}) {
        connect(auth, &GoogleDriveAuth::idle, this, &GoogleDriveSyncController::releaseAuthorization);
        connect(auth, &GoogleDriveAuth::failed, this, [this](const QString &message) {
            if (m_authStage != AuthStage::None)
                fail(message);
        });
        connect(auth, &GoogleDriveAuth::savedAccountUnavailable, this,
                [this, auth](GoogleDriveAuth::SavedAccountResult result) {
            if (m_authStage != AuthStage::None) {
                if (result == GoogleDriveAuth::SavedAccountResult::Missing
                    || result == GoogleDriveAuth::SavedAccountResult::Invalid)
                    fail(auth == m_syncAuth
                        ? tr("Choose Sign in again to restore your synchronization account.")
                        : tr("Connect Google Drive before setting up synchronization."));
                else
                    fail(tr("Unlock the desktop wallet, then resume synchronization."));
            }
        });
        connect(auth, &GoogleDriveAuth::statusChanged, this, [this](const QString &message) {
            if (m_authStage != AuthStage::None) {
                m_message = message;
                Q_EMIT changed();
            }
        });
    }
}

void GoogleDriveSyncController::beginWorker(bool interactive, bool explicitConnect)
{
    if (isBusy() || m_workerRunning)
        return;
    m_interactive = interactive;
    m_preserveError = false;
    if (!interactive) {
        auto *connection = GoogleDriveConnection::sharedConnection();
        if ((!explicitConnect && !connection->autoReconnectEnabled()) || connection->autoReconnectSuppressed())
            return;
    }
    if (!lockAuthorization()) {
        fail(tr("Google Drive is finishing another account operation. Try again when it finishes."));
        return;
    }
    ensureAuth();
    m_authStage = AuthStage::Browse;
    m_message = tr("Restoring Google Drive synchronization…");
    Q_EMIT changed();
    if (interactive)
        m_browseAuth->connectSavedAccountInteractively();
    else
        m_browseAuth->connectSavedAccount();
}

void GoogleDriveSyncController::startWorker(const QByteArray &token, const QString &password)
{
    auto envelope = credentialEnvelope(token, password);
    envelope.insert(QStringLiteral("browse_config_password"), m_browsePassword);
    clearSecrets();
    m_authStage = AuthStage::None;
    m_message = tr("Starting Google Drive synchronization…");
    runHelper(QStringLiteral("start"), envelope, [this](bool ok, QJsonObject result) {
        if (ok && result.value(QStringLiteral("ok")).toBool()) {
            m_restoreFailures = 0;
            acceptStatus(result);
            m_poll->start();
            if (!m_pendingRequest.isEmpty()) {
                const auto pending = std::exchange(m_pendingRequest, {});
                auto completion = std::exchange(m_pendingCompletion, {});
                request(pending, std::move(completion));
            }
        } else {
            fail(result.value(QStringLiteral("message")).toString().isEmpty()
                    ? tr("Google Drive synchronization could not start. Check your connection and try again.")
                    : result.value(QStringLiteral("message")).toString());
        }
    }, 120000);
}

void GoogleDriveSyncController::restoreWorker(bool explicitConnect)
{
    if (isBusy())
        return;
    m_restoreAttempted = true;
    m_interactive = false;
    runHelper(QStringLiteral("status"), {}, [this, explicitConnect](bool ok, QJsonObject result) {
        if (!ok)
        {
            m_restoreAttempted = false;
            return;
        }
        acceptStatus(result);
        if (m_workerRunning || !m_available
            || !result.value(QStringLiteral("sync_configured")).toBool())
            return;
        bool active = false;
        for (const auto &value : pairs()) {
            const QString state = value.toObject().value(QStringLiteral("state")).toString();
            if (state != QStringLiteral("paused") && state != QStringLiteral("needs-review"))
                active = true;
        }
        if (active)
            beginWorker(false, explicitConnect);
    });
}

void GoogleDriveSyncController::requestWritePermission()
{
    m_forceSignIn = false;
    beginWorker(true);
}

void GoogleDriveSyncController::signInAgain()
{
    if (isBusy() || m_workerRunning)
        return;
    m_forceSignIn = true;
    beginWorker(true);
}

void GoogleDriveSyncController::request(QJsonObject object,
                                       std::function<void(bool, QJsonObject)> completion)
{
    if (isBusy())
        return;
    if (!m_workerRunning && object.value(QStringLiteral("action")).toString() != QStringLiteral("stop")) {
        if (!hasSavedPermission()) {
            m_interactive = true;
            fail(tr("Allow editing in Google Drive before starting synchronization."));
            return;
        }
        m_pendingRequest = object;
        m_pendingCompletion = std::move(completion);
        m_forceSignIn = false;
        beginWorker(true);
        return;
    }
    object.insert(QStringLiteral("version"), 1);
    m_preserveError = false;
    const int timeoutMs = isMetadataAction(object.value(QStringLiteral("action")).toString())
        ? metadataRequestTimeoutMs : 45000;
    runHelper(QStringLiteral("request"), object, [this, completion](bool ok, QJsonObject result) {
        const bool success = ok && result.value(QStringLiteral("ok")).toBool();
        if (ok)
            acceptStatus(result);
        if (!success) {
            m_interactive = true;
            fail(result.value(QStringLiteral("message")).toString().isEmpty()
                    ? tr("The synchronization worker did not respond. Try again.")
                    : result.value(QStringLiteral("message")).toString());
        }
        if (completion)
            completion(success, result);
    }, timeoutMs);
}

void GoogleDriveSyncController::listFolders(const QString &parentId)
{
    request({{QStringLiteral("action"), QStringLiteral("list-folders")},
             {QStringLiteral("parent_id"), parentId}}, [this, parentId](bool ok, QJsonObject result) {
        if (ok)
            Q_EMIT foldersListed(parentId, result.value(QStringLiteral("folders")).toArray());
    });
}

void GoogleDriveSyncController::previewPair(const QString &folderId, const QString &path)
{
    request({{QStringLiteral("action"), QStringLiteral("preview")},
             {QStringLiteral("remote_folder_id"), folderId}, {QStringLiteral("local_path"), path}},
            [this](bool ok, QJsonObject result) {
        if (ok)
            Q_EMIT previewReady(result.value(QStringLiteral("preview")).toObject());
    });
}

void GoogleDriveSyncController::addPair(const QString &folderId, const QString &path)
{
    request({{QStringLiteral("action"), QStringLiteral("add")},
             {QStringLiteral("remote_folder_id"), folderId}, {QStringLiteral("local_path"), path}});
}

void GoogleDriveSyncController::pausePair(const QString &id)
{
    request({{QStringLiteral("action"), QStringLiteral("pause")}, {QStringLiteral("pair_id"), id}});
}
void GoogleDriveSyncController::resumePair(const QString &id)
{
    request({{QStringLiteral("action"), QStringLiteral("resume")}, {QStringLiteral("pair_id"), id}});
}
void GoogleDriveSyncController::syncNow(const QString &id)
{
    request({{QStringLiteral("action"), QStringLiteral("sync-now")}, {QStringLiteral("pair_id"), id}});
}
void GoogleDriveSyncController::removePair(const QString &id)
{
    request({{QStringLiteral("action"), QStringLiteral("remove")}, {QStringLiteral("pair_id"), id}});
}
void GoogleDriveSyncController::reviewPair(const QString &id)
{
    request({{QStringLiteral("action"), QStringLiteral("review")}, {QStringLiteral("pair_id"), id}},
            [this](bool ok, QJsonObject result) {
        if (ok)
            Q_EMIT reviewReady(result.value(QStringLiteral("review")).toObject());
    });
}
void GoogleDriveSyncController::resumeReviewed(const QString &id, const QString &token)
{
    request({{QStringLiteral("action"), QStringLiteral("resume-reviewed")},
             {QStringLiteral("pair_id"), id}, {QStringLiteral("review_token"), token}});
}
void GoogleDriveSyncController::stopWorker()
{
    request({{QStringLiteral("action"), QStringLiteral("stop")}});
}

void GoogleDriveSyncController::cancelSetup()
{
    const QString action = m_process ? m_process->property("syncAction").toString() : QString();
    const bool readingSetup = action == QLatin1String("preview") || action == QLatin1String("list-folders");
    if (m_authStage == AuthStage::None && !readingSetup)
        return;
    if (readingSetup) {
        ++m_generation;
        m_process->kill();
    }
    m_authStage = AuthStage::None;
    m_pendingRequest = {};
    m_pendingCompletion = {};
    if (m_browseAuth)
        m_browseAuth->cancel();
    if (m_syncAuth)
        m_syncAuth->cancel();
    clearSecrets();
    releaseAuthorization();
    m_message = tr("Synchronization setup cancelled.");
    m_preserveError = false;
    Q_EMIT changed();
}

void GoogleDriveSyncController::forgetSync(std::function<void(bool, QString)> completion)
{
    m_stopping = true;
    ensureAuth();
    auto finished = std::make_shared<bool>(false);
    auto connections = std::make_shared<QList<QMetaObject::Connection>>();
    auto done = [this, completion, finished, connections](bool success, const QString &message) {
        if (std::exchange(*finished, true))
            return;
        for (const auto &connection : *connections)
            disconnect(connection);
        if (!success) {
            m_stopping = false;
            completion(false, message);
            return;
        }
        runHelper(QStringLiteral("forget"), {}, [this, completion, message](bool ok, QJsonObject result) {
            m_stopping = false;
            completion(ok && result.value(QStringLiteral("ok")).toBool(),
                       ok ? message : tr("The saved synchronization configuration could not be removed."));
        });
    };
    connections->append(connect(m_syncAuth, &GoogleDriveAuth::signedOutLocally, this,
                                [done](const QString &warning) { done(true, warning); }));
    connections->append(connect(m_syncAuth, &GoogleDriveAuth::failed, this,
                                [done](const QString &message) { done(false, message); }));
    // Revocation is project-wide. The connection coordinator revokes once,
    // after unmounting G:, before asking us to erase this local-purpose record.
    m_syncAuth->forgetLocalAccount();
}

void GoogleDriveSyncController::clearLocalAccount(std::function<void(bool, QString)> completion)
{
    if (m_workerRunning || isBusy()) {
        completion(false, tr("Stop Google Drive synchronization before removing its saved account."));
        return;
    }
    forgetSync(std::move(completion));
}

void GoogleDriveSyncController::prepareDisconnect(bool signOut,
                                                  std::function<void(bool, QString)> completion)
{
    m_poll->stop();
    cancelSetup();
    const QString setupAction = m_process ? m_process->property("syncAction").toString() : QString();
    const bool retiringReadOnlySetup = setupAction == QLatin1String("preview")
        || setupAction == QLatin1String("list-folders");
    if (m_process && !m_stopping
        && (m_process->arguments() == QStringList{QStringLiteral("status")} || retiringReadOnlySetup)) {
        // Polling and read-only setup have no account effects. Wait for the
        // cancelled client to exit before taking over for disconnect/sign-out.
        connect(m_process, &QProcess::finished, this, [this, signOut, completion] {
            QTimer::singleShot(0, this, [this, signOut, completion] {
                prepareDisconnect(signOut, completion);
            });
        }, Qt::SingleShotConnection);
        m_process->kill();
        return;
    }
    if (m_process || m_stopping) {
        completion(false, tr("Google Drive synchronization is finishing an operation. Try again shortly."));
        return;
    }
    // Never trust a stale UI worker-running flag when stopping uploads.
    m_stopping = true;
    runHelper(QStringLiteral("status"), {}, [this, signOut, completion](bool ok, QJsonObject result) {
        if (!ok) {
            m_stopping = false;
            // Older installations with no sync helper have nothing to stop.
            completion(!QFileInfo(helperPath()).isExecutable(),
                       tr("Synchronization status could not be checked. Try disconnecting again."));
            return;
        }
        acceptStatus(result);
        auto stopped = [this, signOut, completion](bool success) {
            m_stopping = false;
            if (!success) {
                completion(false, tr("Synchronization could not stop safely. Try disconnecting again."));
            } else {
                completion(true, {});
            }
        };
        if (!m_workerRunning) {
            stopped(true);
            return;
        }
        runHelper(QStringLiteral("request"), {{QStringLiteral("version"), 1},
                  {QStringLiteral("action"), QStringLiteral("stop")}},
                  [this, stopped](bool success, QJsonObject response) {
            if (success)
                acceptStatus(response);
            if (!success || !response.value(QStringLiteral("ok")).toBool()) {
                stopped(false);
                return;
            }
            runHelper(QStringLiteral("status"), {}, [this, stopped](bool ok, QJsonObject verified) {
                if (ok)
                    acceptStatus(verified);
                stopped(ok && !verified.value(QStringLiteral("worker_running")).toBool(true));
            });
        }, 240000);
    });
}
