#pragma once

#include "aero/taskdialog.h"

#include <QJsonArray>
#include <QJsonObject>

class GoogleDriveSyncController;
class QLabel;
class QLineEdit;
class QPushButton;
class QStackedWidget;
class QTreeWidget;
class QTreeWidgetItem;
class QWidget;

// Manages selected-folder two-way sync pairs. The controller owns sync work;
// this dialog only gathers the user's choices and presents controller state.
class GoogleDriveSyncDialog : public Aero::TaskDialog
{
    Q_OBJECT

public:
    explicit GoogleDriveSyncDialog(QWidget *parent = nullptr,
                                   GoogleDriveSyncController *controller = nullptr);
    ~GoogleDriveSyncDialog() override;

private Q_SLOTS:
    void beginSetup();
    void browseLocalFolder();
    void requestWritePermission();
    void signInAgain();
    void loadRemoteFolders(const QString &parentId);
    void remoteFolderExpanded(QTreeWidgetItem *item);
    void previewInitialSync();
    void startInitialSync();
    void updatePairs();
    void updateSelectionActions();
    void pauseOrResumeSelected();
    void syncSelectedNow();
    void openSelectedLocalFolder();
    void removeSelectedPair();
    void showManager();
    void updateControls();

private:
    void setStatus(const QString &message);
    void populateFolders(const QString &parentId, const QJsonArray &folders);
    void clearPreview();
    void showPreview(const QJsonObject &preview);
    void showReviewConfirmation(const QJsonObject &review);
    QString selectedRemoteFolderId() const;
    QString selectedPairId() const;
    QString localFolderValidationError() const;
    QTreeWidgetItem *findFolderItem(const QString &folderId) const;

    GoogleDriveSyncController *m_controller = nullptr;
    QStackedWidget *m_pages = nullptr;
    QWidget *m_managerPage = nullptr;
    QWidget *m_setupPage = nullptr;
    QTreeWidget *m_pairs = nullptr;
    QTreeWidget *m_remoteFolders = nullptr;
    QLineEdit *m_localFolder = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_permissionStatus = nullptr;
    QLabel *m_preview = nullptr;
    QPushButton *m_add = nullptr;
    QPushButton *m_pauseResume = nullptr;
    QPushButton *m_syncNow = nullptr;
    QPushButton *m_openLocal = nullptr;
    QPushButton *m_remove = nullptr;
    QPushButton *m_permission = nullptr;
    QPushButton *m_signInAgain = nullptr;
    QPushButton *m_previewButton = nullptr;
    QPushButton *m_start = nullptr;
    QPushButton *m_back = nullptr;
    QPushButton *m_close = nullptr;
    QJsonObject m_reviewedPreview;
    QString m_reviewedRemoteId;
    QString m_reviewedLocalPath;
    QString m_requestedFolderId;
    QString m_pendingReviewPairId;
    bool m_previewPending = false;
    bool m_addPending = false;
    bool m_requestingPermission = false;
    bool m_folderRequestPending = false;
    bool m_pairActionPending = false;
};
