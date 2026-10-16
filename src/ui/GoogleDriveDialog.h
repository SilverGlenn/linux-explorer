#pragma once

#include "aero/taskdialog.h"
#include "../data/GoogleDriveService.h"
#include <QUrl>

class KFilePlacesModel;
class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;

// Connects to Google Drive through the installed KIO worker. Authentication
// and account tokens are handled by that worker, not by the Explorer UI.
class GoogleDriveDialog : public Aero::TaskDialog {
    Q_OBJECT

public:
    explicit GoogleDriveDialog(KFilePlacesModel *places, QWidget *parent = nullptr,
                               GoogleDriveService *service = nullptr);
    ~GoogleDriveDialog() override;

    QUrl mappedUrl() const { return m_mapped; }

public Q_SLOTS:
    void reject() override;

private Q_SLOTS:
    void loadAccounts();
    void addAccount();
    void connectSelectedAccount();
    void accountsReady(const QList<GoogleDriveAccount> &accounts,
                       const QUrl &suggested);
    void connected(const QUrl &url);
    void failed(const QString &message);

private:
    enum class Operation { None, Load, Add, Verify };

    void setBusy(bool busy, const QString &message = QString());
    void updateControls();
    int accountIndex(const QUrl &url) const;
    QString accountName(const QUrl &url) const;

    KFilePlacesModel *m_places = nullptr;
    GoogleDriveService *m_service = nullptr;
    QComboBox *m_accounts = nullptr;
    QLabel *m_status = nullptr;
    QCheckBox *m_showInNetwork = nullptr;
    QPushButton *m_connect = nullptr;
    QPushButton *m_add = nullptr;
    QPushButton *m_reload = nullptr;
    QPushButton *m_cancel = nullptr;
    QList<GoogleDriveAccount> m_accountList;
    QUrl m_mapped;
    bool m_available = false;
    bool m_accountsLoaded = false;
    bool m_busy = false;
    bool m_rejecting = false;
    Operation m_operation = Operation::None;
};
