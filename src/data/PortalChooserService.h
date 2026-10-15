#pragma once

#include "ChooserOptions.h"
#include <QDBusConnection>
#include <QDBusObjectPath>
#include <QHash>
#include <QVariantMap>
#include <functional>

class FileChooserDialog;
class PortalChooserRequest;
class PortalChooserRouter;
class QDBusMessage;
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
    void removeRequest(const QString &path, PortalChooserRequest *request);
    void cancelRequests();
    void reply(const QDBusMessage &call, uint response, const QVariantMap &results = {});

    QDBusConnection m_connection;
    QDBusServiceWatcher *m_watcher;
    PortalChooserRouter *m_router;
    QHash<QString, PortalChooserRequest *> m_requests;
    int m_authorizations = 0;
    quint64 m_frontendGeneration = 0;
    bool m_claimed = false;
};
