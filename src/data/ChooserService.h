#pragma once

#include <QDBusConnection>
#include <QDBusContext>
#include <QDBusObjectPath>
#include <QHash>
#include <QObject>
#include <QVariantMap>
#include <functional>

class ChooserRequest;
struct ChooserOptions;
class FileChooserDialog;
class QDBusServiceWatcher;

// Private, asynchronous chooser service. No browser or FileManager1 routing.
// Standalone nonmodal requests need no parent; focus is not guaranteed without
// a valid platform activation token. Foreign-parent metadata never enters UI options.
class ChooserService : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.win7explorer.FileChooser1")
    Q_PROPERTY(uint Version READ version CONSTANT)

public:
    using DialogFactory = std::function<FileChooserDialog *(const ChooserOptions &)>;
    explicit ChooserService(const QDBusConnection &connection, QObject *parent = nullptr);
    ~ChooserService() override;

    bool claim();
    uint version() const { return 1; }

    // C++-only hooks; none are exported over D-Bus. Set the factory before use.
    void setDialogFactory(DialogFactory factory);
    FileChooserDialog *dialogForRequest(const QString &path) const;
    int requestCount() const { return m_requests.size(); }

public Q_SLOTS:
    QDBusObjectPath OpenFile(const QVariantMap &options);
    QDBusObjectPath SaveFile(const QVariantMap &options);

private:
    friend class ChooserRequest;
    QDBusObjectPath createRequest(const QVariantMap &options, bool save);
    void removeRequest(const QString &path);
    void ownerDisconnected(const QString &owner);

    QDBusConnection m_connection;
    QDBusServiceWatcher *m_watcher;
    QHash<QString, ChooserRequest *> m_requests;
    DialogFactory m_factory;
    quint64 m_serial = 0;
    bool m_claimed = false;
};
