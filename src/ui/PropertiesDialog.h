#pragma once

#include "PropertiesData.h"
#include "FolderAllocation.h"

#include <QDialog>
#include <QFutureWatcher>
#include <QList>
#include <QPointer>

#include <KFileItem>
#include <KService>

class KJob;
class QCheckBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTabWidget;
class QTreeWidget;

// Explorer's ordinary file, folder and volume property sheet. KIO remains the
// backend for changes and the desktop's full editor remains available.
class PropertiesDialog : public QDialog {
    Q_OBJECT

public:
    explicit PropertiesDialog(const KFileItemList &items, QWidget *parent = nullptr);
    ~PropertiesDialog() override;

    QUrl url() const;
    FolderAllocation::Result allocationResult() const { return m_allocationResult; }
    bool isAllocationCalculating() const { return bool(m_allocationWatcher); }

public Q_SLOTS:
    void accept() override;
    void reject() override;

Q_SIGNALS:
    void applied(const QUrl &url);
    void renamed(const QUrl &oldUrl, const QUrl &newUrl);

private:
    QWidget *buildGeneralPage();
    QWidget *buildSecurityPage();
    QWidget *buildDetailsPage();
    void refresh();
    void refreshDetails();
    void refreshSecurity();
    void refreshAssociation();
    void startSizeCalculation();
    void stopSizeCalculation();
    void updateSizeLabels();
    void updateDirtyState();
    bool isDirty() const;
    void applyChanges(bool closeAfter);
    void applyPermissions();
    void finishApply();
    void setBusy(bool busy);
    void showError(const QString &message);
    void chooseApplication();
    void openDesktopProperties(bool permissionsPage);
    int labelWidth() const;

    KFileItemList m_items;
    PropertiesData m_data;
    QTabWidget *m_tabs = nullptr;
    QDialogButtonBox *m_buttons = nullptr;
    QLineEdit *m_name = nullptr;
    QCheckBox *m_readOnly = nullptr;
    QCheckBox *m_hidden = nullptr;
    QLabel *m_error = nullptr;
    QLabel *m_size = nullptr;
    QLabel *m_sizeOnDisk = nullptr;
    QLabel *m_contains = nullptr;
    QLabel *m_type = nullptr;
    QLabel *m_location = nullptr;
    QLabel *m_created = nullptr;
    QLabel *m_modified = nullptr;
    QLabel *m_accessed = nullptr;
    QLabel *m_opensWith = nullptr;
    QTreeWidget *m_security = nullptr;
    QLabel *m_securityObject = nullptr;
    QTreeWidget *m_details = nullptr;
    KService::Ptr m_pendingService;
    QPointer<KJob> m_changeJob;
    QList<QPointer<KJob>> m_sizeJobs;
    QPointer<QFutureWatcher<FolderAllocation::Result>> m_allocationWatcher;
    std::shared_ptr<std::atomic_bool> m_allocationCancel;
    FolderAllocation::Result m_allocationResult;
    quint64 m_allocationGeneration = 0;
    bool m_allocationAvailable = false;
    quint64 m_totalBytes = 0;
    quint64 m_totalFiles = 0;
    quint64 m_totalFolders = 0;
    int m_sizesRemaining = 0;
    bool m_sizeIncomplete = false;
    bool m_busy = false;
    bool m_closeAfterApply = false;
    bool m_applyReadOnly = false;
    bool m_requestedReadOnly = false;
};
