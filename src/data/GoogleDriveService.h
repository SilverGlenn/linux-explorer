#pragma once

#include <KIO/UDSEntry>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QUrl>

class KFilePlacesModel;
class QWidget;
namespace KIO { class ListJob; }

struct GoogleDriveAccount {
    QString name;
    QUrl url;
};
Q_DECLARE_METATYPE(GoogleDriveAccount)
Q_DECLARE_METATYPE(QList<GoogleDriveAccount>)

// KIO owns authentication and credentials. This adapter only lists locations;
// it never reads tokens, launches shell commands, or writes account settings.
class GoogleDriveService : public QObject {
    Q_OBJECT
public:
    explicit GoogleDriveService(QObject *parent = nullptr);
    ~GoogleDriveService() override;

    virtual bool isAvailable() const;
    virtual void loadAccounts();
    virtual void addAccount();
    virtual void verifyAccount(const QUrl &url);
    virtual void cancel();
    void setWindow(QWidget *window);

    static QUrl accountUrl(const QString &name);
    static bool isAccountUrl(const QUrl &url);
    // Old provider entry points open Explorer's one managed Drive. Provider
    // file/folder IDs are deliberately never translated into filesystem paths.
    static QUrl managedDriveLocation(const QUrl &url);
    static QList<GoogleDriveAccount> accountsForListing(
        const QUrl &url, const KIO::UDSEntryList &entries);
    static bool rememberAccount(KFilePlacesModel *places, const QUrl &url,
                                const QString &name);
    static QString connectionError(int error, const QString &detail);
    static QString personalConfigPath();
    static bool rememberMountedDrive(KFilePlacesModel *places);

Q_SIGNALS:
    void accountsReady(const QList<GoogleDriveAccount> &accounts, const QUrl &suggested);
    void connected(const QUrl &url);
    void failed(const QString &message);

private:
    enum class Operation { Accounts, AddAccount, Verify };
    void start(const QUrl &url, Operation operation);

    QPointer<KIO::ListJob> m_job;
    QPointer<QWidget> m_window;
};
