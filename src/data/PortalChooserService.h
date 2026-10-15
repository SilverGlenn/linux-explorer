#pragma once

#include "ChooserOptions.h"
#include <QDBusConnection>
#include <QDBusContext>
#include <QDBusObjectPath>
#include <QHash>
#include <QVariantMap>
#include <functional>

class FileChooserDialog;
class PortalChooserRequest;
class QDBusMessage;
class QDBusServiceWatcher;

// FileChooser backend only. Calls/results belong to the portal frontend, not
// arbitrary applications. No browser routing, grants or document-store policy.
class PortalChooserService : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.impl.portal.FileChooser")
    Q_PROPERTY(uint version READ version CONSTANT)
public:
    explicit PortalChooserService(const QDBusConnection &connection, QObject *parent = nullptr);
    ~PortalChooserService() override;
    bool claim();
    uint version() const { return 4; }
    FileChooserDialog *dialogForRequest(const QString &path) const;
    int requestCount() const { return m_requests.size(); }

public Q_SLOTS:
    uint OpenFile(const QDBusObjectPath &handle, const QString &appId, const QString &parentWindow,
                  const QString &title, const QVariantMap &options, QVariantMap &results);
    uint SaveFile(const QDBusObjectPath &handle, const QString &appId, const QString &parentWindow,
                  const QString &title, const QVariantMap &options, QVariantMap &results);
    uint SaveFiles(const QDBusObjectPath &handle, const QString &appId, const QString &parentWindow,
                   const QString &title, const QVariantMap &options, QVariantMap &results);

private Q_SLOTS:
    void busDisconnected();

private:
    friend class PortalChooserRequest;
    uint createRequest(ChooserMode mode, const QDBusObjectPath &handle, const QString &appId,
                       const QString &parentWindow, const QString &title, const QVariantMap &options);
    void authorize(const QDBusMessage &call, std::function<void()> accepted);
    void removeRequest(const QString &path, PortalChooserRequest *request);
    void cancelRequests();
    void reply(const QDBusMessage &call, uint response, const QVariantMap &results = {});

    QDBusConnection m_connection;
    QDBusServiceWatcher *m_watcher;
    QHash<QString, PortalChooserRequest *> m_requests;
    int m_authorizations = 0;
    quint64 m_frontendGeneration = 0;
    bool m_claimed = false;
};
