#pragma once

#include "ChooserOptions.h"
#include <QDBusConnection>
#include <QDBusObjectPath>
#include <QHash>
#include <QPointer>
#include <QVariantMap>
#include <functional>

class FileChooserDialog;
class PortalChooserRequest;
class PortalChooserRouter;
class QDBusMessage;
class QDBusPendingCallWatcher;
class QDBusServiceWatcher;

// FileChooser backend only. Calls/results belong to the portal frontend, not
// arbitrary applications. No browser routing, grants or document-store policy.
class PortalChooserService : public QObject {
    Q_OBJECT
public:
    explicit PortalChooserService(const QDBusConnection &connection, QObject *parent = nullptr);
    ~PortalChooserService() override;
    bool claim();
    uint version() const { return 4; }
    FileChooserDialog *dialogForRequest(const QString &path) const;
    int requestCount() const { return m_requests.size(); }

private Q_SLOTS:
    void busDisconnected();

private:
    friend class PortalChooserRequest;
    friend class PortalChooserRouter;
    void createRequest(const QDBusMessage &call, ChooserMode mode, const QDBusObjectPath &handle, const QString &appId,
                       const QString &parentWindow, const QString &title, const QVariantMap &options);
    void authorize(const QDBusMessage &call, std::function<void(bool)> completed);
    void deny(const QDBusMessage &call);
    PortalChooserRequest *requestForCaller(const QString &sender, const QString &path) const;
    void removePendingRequest(const QString &path, PortalChooserRequest *request);
    void removeRequest(const QString &path, PortalChooserRequest *request);
    void cancelRequests();
    void cancelAuthorizations(const QString &sender = {});
    void unwatchUnusedSender(const QString &sender);
    bool readInitialOwner();
    void reply(const QDBusMessage &call, uint response, const QVariantMap &results = {});

    QDBusConnection m_connection;
    QDBusServiceWatcher *m_watcher;
    PortalChooserRouter *m_router;
    QHash<QString, PortalChooserRequest *> m_requests;
    QHash<QString, QHash<QString, PortalChooserRequest *>> m_pendingRequests;
    struct Authorization {
        QString sender;
        std::function<void(bool)> completed;
    };
    QHash<QDBusPendingCallWatcher *, Authorization> m_authorizations;
    QPointer<QDBusPendingCallWatcher> m_initialOwner;
    QString m_frontendOwner;
    int m_frontendPending = 0;
    int m_otherPending = 0;
    int m_frontendAuthorizations = 0;
    int m_otherAuthorizations = 0;
    quint64 m_frontendGeneration = 0;
    quint64 m_initialOwnerGeneration = 0;
    bool m_ownerReady = false;
    bool m_claimed = false;
};
