#include "GoogleDriveSyncDialog.h"

#include "../data/GoogleDriveSyncController.h"
#include "../model/Locations.h"
#include "aero/palette.h"
#include "aero/text.h"

#include <QDesktopServices>
#include <QAbstractItemView>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QHeaderView>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QStringList>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <limits>

namespace {

constexpr int kFolderIdRole = Qt::UserRole + 1;
constexpr int kFolderLoadedRole = Qt::UserRole + 2;
constexpr int kPairIdRole = Qt::UserRole + 3;

QString pairStateText(const QString &state)
{
    if (state == QLatin1String("initializing"))
        return QObject::tr("Setting up…");
    if (state == QLatin1String("syncing"))
        return QObject::tr("Syncing…");
    if (state == QLatin1String("idle"))
        return QObject::tr("Up to date");
    if (state == QLatin1String("paused"))
        return QObject::tr("Paused");
    if (state == QLatin1String("needs-review"))
        return QObject::tr("Needs attention");
    if (state == QLatin1String("error"))
        return QObject::tr("Could not sync");
    return state.isEmpty() ? QObject::tr("Checking status…") : state;
}

QString initialCopySummary(const QJsonObject &preview)
{
    const QString name = preview.value(QStringLiteral("name")).toString();
    const quint64 count = preview.value(QStringLiteral("file_count")).toVariant().toULongLong();
    const quint64 bytes = preview.value(QStringLiteral("bytes")).toVariant().toULongLong();
    const QString size = QLocale().formattedDataSize(qint64(qMin<quint64>(bytes, quint64(std::numeric_limits<qint64>::max()))));
    return QObject::tr("“%1” contains %2 files (%3). The first sync will copy these files from Google Drive into the empty local folder. Later changes on either side will be synchronized. If both sides change a file, both copies are kept for review.")
        .arg(name.isEmpty() ? QObject::tr("Selected folder") : name)
        .arg(QLocale().toString(count), size);
}

QPushButton *smallButton(const QString &text, const QString &name, QWidget *parent)
{
    auto *button = new QPushButton(text, parent);
    button->setObjectName(name);
    Aero::setPointSize(button, 9);
    return button;
}

} // namespace

GoogleDriveSyncDialog::GoogleDriveSyncDialog(QWidget *parent,
                                             GoogleDriveSyncController *controller)
    : Aero::TaskDialog(parent)
    , m_controller(controller ? controller : GoogleDriveSyncController::sharedController())
{
    setWindowTitle(tr("Google Drive synchronization"));
    setAccessibleName(tr("Google Drive folder synchronization"));
    setWindowModality(Qt::WindowModal);

    m_pages = new QStackedWidget;
    contentLayout()->addWidget(m_pages, 1);

    m_managerPage = new QWidget;
    auto *managerLayout = new QVBoxLayout(m_managerPage);
    managerLayout->setContentsMargins(0, 0, 0, 0);
    managerLayout->setSpacing(7);
    auto *managerIntro = Aero::label(
        tr("Choose individual Drive folders to keep synchronized with folders on this computer."), 9);
    managerIntro->setWordWrap(true);
    managerLayout->addWidget(managerIntro);

    m_pairs = new QTreeWidget;
    m_pairs->setObjectName(QStringLiteral("googleDriveSyncPairs"));
    m_pairs->setAccessibleName(tr("Google Drive folders being synchronized"));
    m_pairs->setColumnCount(3);
    m_pairs->setHeaderLabels({tr("Google Drive folder"), tr("Local folder"), tr("Status")});
    m_pairs->setRootIsDecorated(false);
    m_pairs->setAlternatingRowColors(true);
    m_pairs->setMinimumHeight(150);
    m_pairs->setSelectionMode(QAbstractItemView::SingleSelection);
    managerLayout->addWidget(m_pairs, 1);

    auto *pairActions = new QHBoxLayout;
    pairActions->setContentsMargins(0, 0, 0, 0);
    pairActions->setSpacing(5);
    m_pauseResume = smallButton(tr("Pause"), QStringLiteral("googleDriveSyncPauseResume"), m_managerPage);
    m_pauseResume->setAccessibleName(tr("Pause or resume the selected folder synchronization"));
    m_syncNow = smallButton(tr("Sync now"), QStringLiteral("googleDriveSyncNow"), m_managerPage);
    m_syncNow->setAccessibleName(tr("Synchronize the selected folders now"));
    m_openLocal = smallButton(tr("Open local folder"), QStringLiteral("googleDriveSyncOpenLocal"), m_managerPage);
    m_openLocal->setAccessibleName(tr("Open the selected local synchronization folder"));
    m_remove = smallButton(tr("Remove from sync"), QStringLiteral("googleDriveSyncRemove"), m_managerPage);
    m_remove->setAccessibleName(tr("Remove the selected folder from synchronization"));
    m_remove->setAccessibleDescription(tr("Stops synchronization and keeps the local files."));
    m_remove->setToolTip(tr("Stops synchronization and keeps the local files."));
    pairActions->addWidget(m_pauseResume);
    pairActions->addWidget(m_syncNow);
    pairActions->addWidget(m_openLocal);
    pairActions->addWidget(m_remove);
    managerLayout->addLayout(pairActions);
    m_pages->addWidget(m_managerPage);

    m_setupPage = new QWidget;
    auto *setupLayout = new QVBoxLayout(m_setupPage);
    setupLayout->setContentsMargins(0, 0, 0, 0);
    setupLayout->setSpacing(6);
    auto *setupIntro = Aero::label(
        tr("Choose one Drive folder and an existing empty folder on this computer. Explorer copies the Drive files first, then keeps both folders synchronized."), 9);
    setupIntro->setWordWrap(true);
    setupLayout->addWidget(setupIntro);

    auto *remoteLabel = Aero::label(tr("Google Drive folder:"), 9);
    setupLayout->addWidget(remoteLabel);
    m_remoteFolders = new QTreeWidget;
    m_remoteFolders->setObjectName(QStringLiteral("googleDriveSyncRemoteFolders"));
    m_remoteFolders->setAccessibleName(tr("Select one Google Drive folder"));
    m_remoteFolders->setHeaderHidden(true);
    m_remoteFolders->setRootIsDecorated(true);
    m_remoteFolders->setMinimumHeight(120);
    setupLayout->addWidget(m_remoteFolders, 1);

    auto *localRow = new QHBoxLayout;
    localRow->setContentsMargins(0, 0, 0, 0);
    localRow->setSpacing(5);
    auto *localLabel = Aero::label(tr("Local folder:"), 9);
    m_localFolder = new QLineEdit;
    m_localFolder->setObjectName(QStringLiteral("googleDriveSyncLocalFolder"));
    m_localFolder->setAccessibleName(tr("Existing empty local folder"));
    m_localFolder->setPlaceholderText(tr("Choose an existing empty folder"));
    auto *browse = smallButton(tr("Browse…"), QStringLiteral("googleDriveSyncBrowseLocal"), m_setupPage);
    browse->setAccessibleName(tr("Browse for an existing empty local folder"));
    localRow->addWidget(localLabel);
    localRow->addWidget(m_localFolder, 1);
    localRow->addWidget(browse);
    setupLayout->addLayout(localRow);

    auto *permissionRow = new QHBoxLayout;
    permissionRow->setContentsMargins(0, 0, 0, 0);
    permissionRow->setSpacing(7);
    m_permissionStatus = Aero::label(QString(), 9, Aero::Palette::MutedText);
    m_permissionStatus->setWordWrap(true);
    m_permissionStatus->setObjectName(QStringLiteral("googleDriveSyncPermissionStatus"));
    m_permission = smallButton(tr("Allow changes in Google Drive…"),
                               QStringLiteral("googleDriveSyncPermission"), m_setupPage);
    m_permission->setAccessibleName(tr("Allow Explorer to change files in Google Drive"));
    m_permission->setAccessibleDescription(
        tr("Google may ask you to approve file changes in the browser."));
    permissionRow->addWidget(m_permissionStatus, 1);
    permissionRow->addWidget(m_permission);
    m_signInAgain = smallButton(tr("Sign in again…"),
                                QStringLiteral("googleDriveSyncSignInAgain"), m_setupPage);
    m_signInAgain->setAccessibleName(tr("Sign in to Google again for synchronization"));
    m_signInAgain->setAccessibleDescription(
        tr("Refresh saved synchronization access using the same Google account as the Drive mount."));
    permissionRow->addWidget(m_signInAgain);
    setupLayout->addLayout(permissionRow);

    auto *previewLayout = new QVBoxLayout;
    previewLayout->setContentsMargins(0, 0, 0, 0);
    previewLayout->setSpacing(3);
    auto *previewButtonRow = new QHBoxLayout;
    previewButtonRow->setContentsMargins(0, 0, 0, 0);
    previewButtonRow->setSpacing(7);
    m_previewButton = smallButton(tr("Preview initial copy"),
                                  QStringLiteral("googleDriveSyncPreview"), m_setupPage);
    m_previewButton->setAccessibleName(tr("Review the initial copy before starting synchronization"));
    m_preview = Aero::label(QString(), 9, Aero::Palette::MutedText);
    m_preview->setObjectName(QStringLiteral("googleDriveSyncPreviewSummary"));
    m_preview->setWordWrap(true);
    m_preview->setTextFormat(Qt::PlainText);
    m_preview->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    previewButtonRow->addWidget(m_previewButton);
    previewButtonRow->addStretch(1);
    previewLayout->addLayout(previewButtonRow);
    previewLayout->addWidget(m_preview);
    setupLayout->addLayout(previewLayout);
    m_pages->addWidget(m_setupPage);

    m_status = Aero::label(QString(), 9, Aero::Palette::MutedText);
    m_status->setObjectName(QStringLiteral("googleDriveSyncStatus"));
    m_status->setAccessibleDescription(tr("Google Drive synchronization status"));
    m_status->setWordWrap(true);
    m_status->setTextFormat(Qt::PlainText);
    contentLayout()->addWidget(m_status);

    m_add = addButton(tr("&Add folder"));
    m_add->setObjectName(QStringLiteral("googleDriveSyncAdd"));
    m_add->setAccessibleName(tr("Add a selected Google Drive folder to synchronization"));
    m_start = addButton(tr("Start synchronization"));
    m_start->setObjectName(QStringLiteral("googleDriveSyncStart"));
    m_start->setAccessibleName(tr("Start synchronization after reviewing the initial copy"));
    m_start->setDefault(true);
    m_back = addButton(tr("Back"));
    m_back->setObjectName(QStringLiteral("googleDriveSyncBack"));
    m_back->setAccessibleName(tr("Return to synchronized folders"));
    m_close = addButton(tr("Close"));
    m_close->setObjectName(QStringLiteral("googleDriveSyncClose"));
    m_close->setAccessibleName(tr("Close Google Drive synchronization"));

    connect(m_add, &QPushButton::clicked, this, &GoogleDriveSyncDialog::beginSetup);
    connect(m_start, &QPushButton::clicked, this, &GoogleDriveSyncDialog::startInitialSync);
    connect(m_back, &QPushButton::clicked, this, &GoogleDriveSyncDialog::showManager);
    connect(m_close, &QPushButton::clicked, this, &QDialog::accept);
    connect(browse, &QPushButton::clicked, this, &GoogleDriveSyncDialog::browseLocalFolder);
    connect(m_permission, &QPushButton::clicked,
            this, &GoogleDriveSyncDialog::requestWritePermission);
    connect(m_signInAgain, &QPushButton::clicked,
            this, &GoogleDriveSyncDialog::signInAgain);
    connect(m_previewButton, &QPushButton::clicked,
            this, &GoogleDriveSyncDialog::previewInitialSync);
    connect(m_remoteFolders, &QTreeWidget::itemExpanded,
            this, &GoogleDriveSyncDialog::remoteFolderExpanded);
    connect(m_remoteFolders, &QTreeWidget::currentItemChanged,
            this, [this] { clearPreview(); updateControls(); });
    connect(m_localFolder, &QLineEdit::textChanged,
            this, [this] { clearPreview(); updateControls(); });
    connect(m_pairs, &QTreeWidget::currentItemChanged,
            this, &GoogleDriveSyncDialog::updateSelectionActions);
    connect(m_pauseResume, &QPushButton::clicked,
            this, &GoogleDriveSyncDialog::pauseOrResumeSelected);
    connect(m_syncNow, &QPushButton::clicked,
            this, &GoogleDriveSyncDialog::syncSelectedNow);
    connect(m_openLocal, &QPushButton::clicked,
            this, &GoogleDriveSyncDialog::openSelectedLocalFolder);
    connect(m_remove, &QPushButton::clicked,
            this, &GoogleDriveSyncDialog::removeSelectedPair);

    connect(m_controller, &GoogleDriveSyncController::changed,
            this, [this] {
                updatePairs();
                if (m_pages->currentWidget() == m_setupPage && m_requestingPermission
                    && m_controller->hasWritePermission()) {
                    m_requestingPermission = false;
                    setStatus(tr("Google Drive editing permission is ready."));
                    loadRemoteFolders(QStringLiteral("root"));
                }
                if (m_addPending) {
                    const QString id = m_reviewedRemoteId;
                    const QString path = m_reviewedLocalPath;
                    for (const QJsonValue &value : m_controller->pairs()) {
                        const QJsonObject pair = value.toObject();
                        if (pair.value(QStringLiteral("remote_folder_id")).toString() == id
                            && QDir::cleanPath(pair.value(QStringLiteral("local_path")).toString())
                                == QDir::cleanPath(path)) {
                            m_addPending = false;
                            m_reviewedPreview = {};
                            showManager();
                            setStatus(tr("Synchronization is starting. Your local files are kept if you remove this pair later."));
                            break;
                        }
                    }
                }
                if (m_pairActionPending && !m_controller->isBusy())
                    m_pairActionPending = false;
                // Keep this dialog's progress message for work it started.
                // Background status polling can report a stale transient error
                // while a preview or folder request is still in flight.
                const bool ownedOperationPending = m_previewPending || m_addPending
                    || m_folderRequestPending || m_pairActionPending;
                if (!ownedOperationPending && !m_controller->statusMessage().isEmpty())
                    setStatus(m_controller->statusMessage());
                updateControls();
            });
    connect(m_controller, &GoogleDriveSyncController::foldersListed,
            this, &GoogleDriveSyncDialog::populateFolders);
    connect(m_controller, &GoogleDriveSyncController::previewReady,
            this, &GoogleDriveSyncDialog::showPreview);
    connect(m_controller, &GoogleDriveSyncController::reviewReady,
            this, [this](const QJsonObject &review) {
                const QString pairId = review.value(QStringLiteral("pair_id")).toString();
                if (m_pendingReviewPairId.isEmpty() || pairId != m_pendingReviewPairId)
                    return;
                m_pendingReviewPairId.clear();
                m_pairActionPending = false;
                if (m_pages->currentWidget() == m_managerPage
                    && selectedPairId() == pairId)
                    showReviewConfirmation(review);
                updateControls();
            });
    connect(m_controller, &GoogleDriveSyncController::failed,
            this, [this](const QString &message) {
                if (!m_previewPending && !m_addPending && !m_requestingPermission
                    && !m_folderRequestPending && !m_pairActionPending)
                    return;
                if (m_previewPending)
                    clearPreview();
                m_addPending = false;
                m_requestingPermission = false;
                m_folderRequestPending = false;
                m_pairActionPending = false;
                m_pendingReviewPairId.clear();
                if (!m_requestedFolderId.isEmpty() && m_requestedFolderId != QLatin1String("root")) {
                    if (QTreeWidgetItem *item = findFolderItem(m_requestedFolderId))
                        item->setData(0, kFolderLoadedRole, false);
                }
                m_requestedFolderId.clear();
                setStatus(message.trimmed().isEmpty()
                              ? tr("Google Drive synchronization could not be completed.")
                              : message);
                updateControls();
            });

    setMinimumWidth(620);
    resize(680, 560);
    m_pages->setCurrentWidget(m_managerPage);
    // Status is polled quickly only while this dialog is open to show it
    m_controller->watch();
    m_controller->refresh();
    // Cached pair metadata remains useful while the worker is stopped or
    // needs authorization; refresh will replace it when the current status
    // query completes.
    updatePairs();
    if (!m_controller->statusMessage().isEmpty())
        setStatus(m_controller->statusMessage());
    else if (!m_controller->isAvailable())
        setStatus(tr("Google Drive folder synchronization is unavailable in this build."));
    updateControls();
}

GoogleDriveSyncDialog::~GoogleDriveSyncDialog()
{
    disconnect(m_controller, nullptr, this, nullptr);
    m_controller->unwatch();
    if (m_previewPending || m_requestingPermission || m_folderRequestPending)
        m_controller->cancelSetup();
}

void GoogleDriveSyncDialog::beginSetup()
{
    if (!m_controller->isAvailable() || m_controller->isBusy())
        return;
    m_remoteFolders->clear();
    m_localFolder->clear();
    clearPreview();
    m_pages->setCurrentWidget(m_setupPage);
    setStatus(tr("Select one Drive folder. The entire Drive cannot be synchronized as one pair."));
    // A saved grant may need the worker restarted before Drive can answer.
    // Keep the Resume button available instead of queueing a folder request
    // that would make the page look busy and block the user from resuming.
    if (m_controller->hasWritePermission() && m_controller->workerRunning())
        loadRemoteFolders(QStringLiteral("root"));
    updateControls();
    m_permission->setFocus(Qt::OtherFocusReason);
}

void GoogleDriveSyncDialog::browseLocalFolder()
{
    const QString selected = QFileDialog::getExistingDirectory(
        this, tr("Choose an existing empty local folder"), m_localFolder->text());
    if (!selected.isEmpty())
        m_localFolder->setText(QDir::cleanPath(selected));
}

void GoogleDriveSyncDialog::requestWritePermission()
{
    if (!m_controller->isAvailable() || m_controller->isBusy()
        || m_controller->hasWritePermission())
        return;
    m_requestingPermission = true;
    setStatus(m_controller->hasSavedPermission()
                  ? tr("Resuming your saved Google Drive synchronization…")
                  : tr("Waiting for Google to approve file changes…"));
    m_controller->requestWritePermission();
    updateControls();
}

void GoogleDriveSyncDialog::signInAgain()
{
    if (!m_controller->isAvailable() || m_controller->isBusy()
        || !m_controller->hasSavedPermission() || m_controller->workerRunning())
        return;
    m_requestingPermission = true;
    setStatus(tr("Sign in with Google again to refresh access. Use the same account as the Google Drive mount."));
    m_controller->signInAgain();
    updateControls();
}

void GoogleDriveSyncDialog::loadRemoteFolders(const QString &parentId)
{
    if (m_controller->isBusy() || !m_controller->hasWritePermission()
        || !m_controller->workerRunning())
        return;
    m_folderRequestPending = true;
    m_requestedFolderId = parentId;
    m_controller->listFolders(parentId);
}

void GoogleDriveSyncDialog::remoteFolderExpanded(QTreeWidgetItem *item)
{
    if (!item || item->data(0, kFolderLoadedRole).toBool() || m_controller->isBusy())
        return;
    const QString id = item->data(0, kFolderIdRole).toString();
    if (id.isEmpty())
        return;
    item->setData(0, kFolderLoadedRole, true);
    loadRemoteFolders(id);
}

void GoogleDriveSyncDialog::previewInitialSync()
{
    if (!m_controller->isAvailable() || m_controller->isBusy())
        return;
    if (!m_controller->hasSavedPermission()) {
        setStatus(tr("Allow Google Drive changes before previewing synchronization."));
        return;
    }
    const QString validation = localFolderValidationError();
    if (!validation.isEmpty()) {
        setStatus(validation);
        return;
    }
    const QString remoteId = selectedRemoteFolderId();
    if (remoteId.isEmpty()) {
        setStatus(tr("Select one Google Drive folder to continue."));
        return;
    }
    m_reviewedRemoteId = remoteId;
    m_reviewedLocalPath = QDir::cleanPath(m_localFolder->text().trimmed());
    m_previewPending = true;
    m_preview->setText(tr("Checking the initial copy…"));
    setStatus(tr("Reviewing the selected Drive folder…"));
    m_controller->previewPair(m_reviewedRemoteId, m_reviewedLocalPath);
    updateControls();
}

void GoogleDriveSyncDialog::startInitialSync()
{
    if (m_reviewedPreview.isEmpty() || m_controller->isBusy()
        || !m_controller->hasWritePermission()
        || m_reviewedRemoteId != selectedRemoteFolderId()
        || QDir::cleanPath(m_reviewedLocalPath)
            != QDir::cleanPath(m_localFolder->text().trimmed()))
        return;
    m_addPending = true;
    setStatus(tr("Starting the initial copy…"));
    m_controller->addPair(m_reviewedRemoteId, m_reviewedLocalPath);
    updateControls();
}

void GoogleDriveSyncDialog::populateFolders(const QString &parentId,
                                           const QJsonArray &folders)
{
    if (m_pages->currentWidget() != m_setupPage)
        return;
    QTreeWidgetItem *parent = parentId == QLatin1String("root")
        ? nullptr : findFolderItem(parentId);
    if (parentId != QLatin1String("root") && !parent)
        return;
    if (parentId == m_requestedFolderId) {
        m_folderRequestPending = false;
        m_requestedFolderId.clear();
    }
    if (parent)
        parent->setData(0, kFolderLoadedRole, true);

    for (const QJsonValue &value : folders) {
        const QJsonObject folder = value.toObject();
        const QString id = folder.value(QStringLiteral("id")).toString();
        const QString name = folder.value(QStringLiteral("name")).toString();
        if (id.isEmpty() || name.isEmpty() || findFolderItem(id))
            continue;
        auto *item = new QTreeWidgetItem;
        item->setText(0, name);
        item->setData(0, kFolderIdRole, id);
        item->setData(0, kFolderLoadedRole, false);
        item->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
        if (parent)
            parent->addChild(item);
        else
            m_remoteFolders->addTopLevelItem(item);
    }
    if (!parent && m_remoteFolders->topLevelItemCount() == 0)
        setStatus(tr("No Drive folders were found. Choose a folder that contains the files you want to synchronize."));
    updateControls();
}

void GoogleDriveSyncDialog::showPreview(const QJsonObject &preview)
{
    if (!m_previewPending || m_pages->currentWidget() != m_setupPage)
        return;
    m_previewPending = false;
    if (preview.value(QStringLiteral("folder_id")).toString() != m_reviewedRemoteId
        || m_reviewedLocalPath != QDir::cleanPath(m_localFolder->text().trimmed())) {
        clearPreview();
        setStatus(tr("The folder choices changed. Preview the initial copy again."));
        updateControls();
        return;
    }
    m_reviewedPreview = preview;
    m_preview->setText(initialCopySummary(preview));
    setStatus(tr("Review the initial copy, then select Start synchronization."));
    updateControls();
}

void GoogleDriveSyncDialog::showReviewConfirmation(const QJsonObject &review)
{
    const QString pairId = review.value(QStringLiteral("pair_id")).toString();
    const QString token = review.value(QStringLiteral("token")).toString();
    const QJsonArray localDeleted = review.value(QStringLiteral("local_deleted")).toArray();
    const QJsonArray remoteDeleted = review.value(QStringLiteral("remote_deleted")).toArray();
    if (pairId.isEmpty() || token.isEmpty()) {
        const QString message = review.value(QStringLiteral("message")).toString().trimmed();
        setStatus(message.isEmpty()
                      ? tr("This change cannot be resumed automatically. Review the Drive and local folders.")
                      : message);
        return;
    }

    Aero::TaskDialog confirmation(this, tr("Review folder changes"));
    confirmation.setWindowModality(Qt::WindowModal);
    confirmation.setAccessibleName(tr("Review Google Drive synchronization changes"));
    auto *intro = Aero::label(
        review.value(QStringLiteral("message")).toString().trimmed().isEmpty()
            ? tr("Review these removals before resuming. Continuing applies each removal to the other folder; recoverable backups are retained.")
            : review.value(QStringLiteral("message")).toString(), 9);
    intro->setWordWrap(true);
    intro->setTextFormat(Qt::PlainText);
    confirmation.contentLayout()->addWidget(intro);

    auto addRemovalList = [&confirmation](const QString &heading, const QString &direction,
                                         const QJsonArray &entries, const QString &name) {
        auto *label = Aero::label(tr("%1 (%2)\n%3").arg(heading).arg(entries.size()).arg(direction), 9);
        label->setWordWrap(true);
        confirmation.contentLayout()->addWidget(label);
        auto *list = new QPlainTextEdit;
        list->setObjectName(name);
        list->setReadOnly(true);
        list->setAccessibleName(heading);
        list->setLineWrapMode(QPlainTextEdit::WidgetWidth);
        QStringList paths;
        paths.reserve(entries.size());
        for (const QJsonValue &entry : entries)
            if (entry.isString())
                paths.append(entry.toString());
        list->setPlainText(paths.isEmpty() ? QObject::tr("No files in this list.")
                                           : paths.join(QLatin1Char('\n')));
        list->setMinimumHeight(64);
        list->setMaximumHeight(110);
        confirmation.contentLayout()->addWidget(list);
    };
    addRemovalList(tr("Removed from this computer"),
                   tr("Matching copies in Google Drive will be removed."),
                   localDeleted, QStringLiteral("googleDriveSyncLocalRemovals"));
    addRemovalList(tr("Removed from Google Drive"),
                   tr("Matching copies in the local folder will be removed."),
                   remoteDeleted, QStringLiteral("googleDriveSyncDriveRemovals"));

    auto *cancel = confirmation.addButton(tr("Cancel"));
    cancel->setObjectName(QStringLiteral("googleDriveSyncReviewCancel"));
    cancel->setAccessibleName(tr("Cancel and keep synchronization unchanged"));
    cancel->setDefault(true);
    QObject::connect(cancel, &QPushButton::clicked, &confirmation, &QDialog::reject);
    auto *resume = confirmation.addButton(tr("Resume synchronization"));
    resume->setObjectName(QStringLiteral("googleDriveSyncReviewResume"));
    resume->setAccessibleName(tr("Confirm reviewed changes and resume synchronization"));
    QObject::connect(resume, &QPushButton::clicked, &confirmation, &QDialog::accept);
    confirmation.setMinimumWidth(560);
    confirmation.resize(620, 520);

    if (confirmation.exec() == QDialog::Accepted) {
        m_pairActionPending = true;
        setStatus(tr("Resuming synchronization after your review…"));
        m_controller->resumeReviewed(pairId, token);
        updateControls();
    } else {
        setStatus(tr("This folder still needs your review. Synchronization was not resumed."));
    }
}

void GoogleDriveSyncDialog::updatePairs()
{
    const QString selectedId = selectedPairId();
    const QSignalBlocker blocker(m_pairs);
    m_pairs->clear();
    for (const QJsonValue &value : m_controller->pairs()) {
        const QJsonObject pair = value.toObject();
        const QString id = pair.value(QStringLiteral("id")).toString();
        if (id.isEmpty())
            continue;
        auto *item = new QTreeWidgetItem(m_pairs);
        item->setData(0, kPairIdRole, id);
        item->setText(0, pair.value(QStringLiteral("remote_name")).toString());
        item->setText(1, pair.value(QStringLiteral("local_path")).toString());
        item->setText(2, pairStateText(pair.value(QStringLiteral("state")).toString()));
        QStringList details;
        const QString error = pair.value(QStringLiteral("error")).toString().trimmed();
        if (!error.isEmpty())
            details.append(error);
        const QString lastSuccess = pair.value(QStringLiteral("last_success")).toString().trimmed();
        const QDateTime lastSuccessTime = QDateTime::fromString(lastSuccess, Qt::ISODate);
        if (lastSuccessTime.isValid())
            details.append(tr("Last successful sync: %1")
                               .arg(QLocale().toString(lastSuccessTime.toLocalTime(), QLocale::ShortFormat)));
        item->setToolTip(2, details.join(QLatin1Char('\n')));
        if (id == selectedId)
            m_pairs->setCurrentItem(item);
    }
    m_pairs->resizeColumnToContents(0);
    m_pairs->resizeColumnToContents(2);
    m_pairs->header()->setStretchLastSection(true);
    updateSelectionActions();
}

void GoogleDriveSyncDialog::updateSelectionActions()
{
    const QString id = selectedPairId();
    QJsonObject selected;
    for (const QJsonValue &value : m_controller->pairs()) {
        const QJsonObject pair = value.toObject();
        if (pair.value(QStringLiteral("id")).toString() == id) {
            selected = pair;
            break;
        }
    }
    const QString state = selected.value(QStringLiteral("state")).toString();
    const bool hasPair = !selected.isEmpty();
    const bool busy = m_controller->isBusy() || m_pairActionPending;
    const bool paused = state == QLatin1String("paused");
    const bool reviewRequired = state == QLatin1String("needs-review");
    const bool canResume = selected.value(QStringLiteral("can_resume")).toBool();
    m_pauseResume->setText(reviewRequired ? tr("Review…") : paused ? tr("Resume") : tr("Pause"));
    m_pauseResume->setAccessibleName(reviewRequired
        ? tr("Review file removals before resuming synchronization")
        : paused ? tr("Resume the selected folder synchronization")
                 : tr("Pause the selected folder synchronization"));
    m_pauseResume->setEnabled(hasPair && !busy
                              && ((paused && canResume) || (reviewRequired && canResume)
                                  || state == QLatin1String("idle")
                                  || state == QLatin1String("syncing")));
    m_syncNow->setText(state == QLatin1String("error") ? tr("Try again") : tr("Sync now"));
    m_syncNow->setAccessibleName(state == QLatin1String("error")
        ? tr("Try synchronization again for the selected folder")
        : tr("Synchronize the selected folders now"));
    m_syncNow->setEnabled(hasPair && !busy && state != QLatin1String("syncing")
                          && state != QLatin1String("initializing") && !reviewRequired
                          && canResume);
    m_openLocal->setEnabled(hasPair && QFileInfo(selected.value(QStringLiteral("local_path")).toString()).isDir());
    m_remove->setEnabled(hasPair && !busy);
}

void GoogleDriveSyncDialog::pauseOrResumeSelected()
{
    const QJsonObject pair = [&] {
        for (const QJsonValue &value : m_controller->pairs())
            if (value.toObject().value(QStringLiteral("id")).toString() == selectedPairId())
                return value.toObject();
        return QJsonObject();
    }();
    const QString id = pair.value(QStringLiteral("id")).toString();
    if (id.isEmpty() || m_controller->isBusy() || m_pairActionPending)
        return;
    if (pair.value(QStringLiteral("state")).toString() == QLatin1String("needs-review")) {
        if (!pair.value(QStringLiteral("can_resume")).toBool())
            return;
        m_pendingReviewPairId = id;
        m_pairActionPending = true;
        setStatus(tr("Checking the changes that need your review…"));
        updateControls();
        m_controller->reviewPair(id);
        return;
    }
    if (pair.value(QStringLiteral("state")).toString() == QLatin1String("paused")) {
        if (!pair.value(QStringLiteral("can_resume")).toBool())
            return;
        m_pairActionPending = true;
        updateControls();
        m_controller->resumePair(id);
    } else {
        m_pairActionPending = true;
        updateControls();
        m_controller->pausePair(id);
    }
}

void GoogleDriveSyncDialog::syncSelectedNow()
{
    const QString id = selectedPairId();
    if (!id.isEmpty() && !m_controller->isBusy() && !m_pairActionPending) {
        m_pairActionPending = true;
        updateControls();
        m_controller->syncNow(id);
    }
}

void GoogleDriveSyncDialog::openSelectedLocalFolder()
{
    const QString id = selectedPairId();
    for (const QJsonValue &value : m_controller->pairs()) {
        const QJsonObject pair = value.toObject();
        if (pair.value(QStringLiteral("id")).toString() == id) {
            const QString path = pair.value(QStringLiteral("local_path")).toString();
            if (QFileInfo(path).isDir())
                QDesktopServices::openUrl(QUrl::fromLocalFile(path));
            return;
        }
    }
}

void GoogleDriveSyncDialog::removeSelectedPair()
{
    const QString id = selectedPairId();
    if (!id.isEmpty() && !m_controller->isBusy() && !m_pairActionPending) {
        m_pairActionPending = true;
        setStatus(tr("Removing this folder from sync. Local files will be kept."));
        updateControls();
        m_controller->removePair(id);
    }
}

void GoogleDriveSyncDialog::showManager()
{
    const bool cancelledSetupRequest = m_previewPending || m_requestingPermission
        || m_folderRequestPending;
    if (cancelledSetupRequest)
        m_controller->cancelSetup();
    m_previewPending = false;
    m_addPending = false;
    m_requestingPermission = false;
    m_folderRequestPending = false;
    m_requestedFolderId.clear();
    clearPreview();
    m_pages->setCurrentWidget(m_managerPage);
    if (cancelledSetupRequest)
        setStatus(QString());
    updatePairs();
    updateControls();
    m_add->setFocus(Qt::OtherFocusReason);
}

void GoogleDriveSyncDialog::updateControls()
{
    const bool setup = m_pages->currentWidget() == m_setupPage;
    const bool available = m_controller->isAvailable();
    const bool busy = m_controller->isBusy() || m_previewPending || m_addPending
        || m_requestingPermission || m_folderRequestPending || m_pairActionPending;
    m_add->setVisible(!setup);
    m_start->setVisible(setup);
    m_back->setVisible(setup);
    m_close->setVisible(true);
    m_add->setEnabled(available && !busy);
    m_back->setEnabled(!m_addPending);
    m_close->setEnabled(true);

    const bool activePermission = m_controller->hasWritePermission();
    const bool savedPermission = m_controller->hasSavedPermission();
    m_permission->setVisible(setup && !activePermission);
    m_permission->setEnabled(available && !busy && !m_requestingPermission);
    m_permission->setText(savedPermission ? tr("Resume synchronization")
                                          : tr("Allow changes in Google Drive…"));
    m_permission->setAccessibleName(savedPermission
        ? tr("Resume synchronization using the saved Google Drive permission")
        : tr("Allow Explorer to change files in Google Drive"));
    m_permission->setAccessibleDescription(savedPermission
        ? tr("Restores saved synchronization access. Google may ask you to approve again if needed.")
        : tr("Google may ask you to approve file changes in the browser."));
    m_signInAgain->setVisible(setup && savedPermission && !m_controller->workerRunning());
    m_signInAgain->setEnabled(available && !busy);
    m_permissionStatus->setText(activePermission
        ? tr("Google Drive editing permission is ready.")
        : savedPermission ? tr("Resume saved access, or sign in again with the same Google account as the Drive mount.")
                          : tr("Google will ask you to approve file changes."));
    m_remoteFolders->setEnabled(setup && available && activePermission
                                && m_controller->workerRunning() && !busy);
    m_localFolder->setEnabled(setup && available && !busy);
    m_previewButton->setEnabled(setup && available && activePermission
                                && m_controller->workerRunning()
                                && !busy && localFolderValidationError().isEmpty()
                                && !selectedRemoteFolderId().isEmpty());
    const bool previewStillMatches = !m_reviewedPreview.isEmpty()
        && m_reviewedRemoteId == selectedRemoteFolderId()
        && QDir::cleanPath(m_reviewedLocalPath)
            == QDir::cleanPath(m_localFolder->text().trimmed());
    QString previewHint = localFolderValidationError();
    if (previewHint.isEmpty() && selectedRemoteFolderId().isEmpty())
        previewHint = tr("Select one Google Drive folder first.");
    if (previewHint.isEmpty() && !activePermission)
        previewHint = savedPermission ? tr("Resume synchronization first.")
                                     : tr("Allow changes in Google Drive first.");
    m_previewButton->setToolTip(previewHint);
    m_previewButton->setAccessibleDescription(previewHint.isEmpty()
        ? tr("Review the initial copy before starting synchronization.") : previewHint);
    m_start->setEnabled(setup && available && savedPermission
                        && !busy && previewStillMatches);

    m_managerPage->setEnabled(!setup);
    m_setupPage->setEnabled(setup);
    m_pairs->setVisible(!setup || m_controller->pairs().isEmpty());
    updateSelectionActions();
}

void GoogleDriveSyncDialog::setStatus(const QString &message)
{
    m_status->setText(message);
    m_status->setMinimumHeight(0);
    m_preview->setMinimumHeight(0);
    m_status->updateGeometry();
    m_preview->updateGeometry();
    contentLayout()->activate();
    layout()->activate();
    const int availableWidth = qMax(1, m_status->width() > 0 ? m_status->width() : width() - 48);
    m_status->setMinimumHeight(qMax(m_status->fontMetrics().height(),
                                    m_status->heightForWidth(availableWidth)));
    if (!m_preview->text().isEmpty()) {
        const int previewWidth = qMax(1, m_preview->width() > 0
                                          ? m_preview->width() : availableWidth);
        const int wrappedHeight = m_preview->fontMetrics().boundingRect(
            QRect(0, 0, previewWidth, 10000), Qt::TextWordWrap, m_preview->text()).height();
        m_preview->setMinimumHeight(qMax(m_preview->fontMetrics().height(), wrappedHeight + 2));
    }
    contentLayout()->activate();
    layout()->activate();
    if (height() < minimumSizeHint().height())
        resize(width(), minimumSizeHint().height());
}

void GoogleDriveSyncDialog::clearPreview()
{
    m_reviewedPreview = {};
    m_previewPending = false;
    m_preview->clear();
}

QString GoogleDriveSyncDialog::selectedRemoteFolderId() const
{
    const QTreeWidgetItem *item = m_remoteFolders->currentItem();
    return item ? item->data(0, kFolderIdRole).toString() : QString();
}

QString GoogleDriveSyncDialog::selectedPairId() const
{
    const QTreeWidgetItem *item = m_pairs->currentItem();
    return item ? item->data(0, kPairIdRole).toString() : QString();
}

QString GoogleDriveSyncDialog::localFolderValidationError() const
{
    const QString path = m_localFolder->text().trimmed();
    if (path.isEmpty())
        return tr("Choose an existing empty local folder.");
    const QFileInfo info(path);
    if (!info.isAbsolute() || !info.exists() || !info.isDir() || info.isSymLink())
        return tr("Choose an existing local folder that is not a shortcut.");
    const QString driveRoot = QDir::cleanPath(Locations::googleDriveMount().toLocalFile());
    const QString canonicalPath = QDir::cleanPath(info.canonicalFilePath());
    if (canonicalPath == driveRoot || canonicalPath.startsWith(driveRoot + QDir::separator()))
        return tr("Choose a local folder outside Google Drive.");
    QDir directory(path);
    const QStringList entries = directory.entryList(QDir::AllEntries | QDir::Hidden
                                                     | QDir::System | QDir::NoDotAndDotDot);
    if (!entries.isEmpty())
        return tr("The local folder must be empty. Choose another folder so existing files are not changed.");
    return {};
}

QTreeWidgetItem *GoogleDriveSyncDialog::findFolderItem(const QString &folderId) const
{
    const auto find = [&folderId](QTreeWidgetItem *item, const auto &self) -> QTreeWidgetItem * {
        if (item->data(0, kFolderIdRole).toString() == folderId)
            return item;
        for (int i = 0; i < item->childCount(); ++i)
            if (QTreeWidgetItem *found = self(item->child(i), self))
                return found;
        return nullptr;
    };
    for (int i = 0; i < m_remoteFolders->topLevelItemCount(); ++i)
        if (QTreeWidgetItem *found = find(m_remoteFolders->topLevelItem(i), find))
            return found;
    return nullptr;
}
