#pragma once

#include <QDBusContext>
#include <QObject>
#include <QString>
#include <QStringList>

// How the rest of the desktop asks a file manager to reveal a file, and the
// single instance lock that comes with owning a bus name
//
// The fork has its own instance name. The shared desktop service is opt-in so
// trying this build does not take over the user's existing file manager.
class FileManagerService : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.FileManager1")

public:
    explicit FileManagerService(QObject *parent = nullptr);

    // False when another Explorer already holds the private name, in which case
    // nothing was registered and the caller should forward rather than open
    bool claim(bool registerSharedService = false);

    bool ownsSharedName() const { return m_ownsSharedName; }

    // Hands the locations to the Explorer already running
    static bool forward(const QStringList &uris, bool reveal);

    // The activation token this process was launched with, so a window opened
    // for another application may take the focus
    static QString startupId();

    // The most locations one call may carry, anything past it being ignored
    static constexpr int maxUris = 16;

    // Overrides which executable counts as this application when judging a bus
    // caller, for tests that host the service in a different binary. An empty
    // path restores QCoreApplication::applicationFilePath().
    static void setTrustedExecutable(const QString &path);

public Q_SLOTS:
    // CamelCase because these are wire names fixed by the specification, and
    // scriptable since only these are exported on the bus
    Q_SCRIPTABLE void ShowFolders(const QStringList &uriList, const QString &startupId);
    Q_SCRIPTABLE void ShowItems(const QStringList &uriList, const QString &startupId);
    Q_SCRIPTABLE void ShowItemProperties(const QStringList &uriList, const QString &startupId);

private:
    // False for a bus call from some other program. Direct calls from this
    // process count as trusted.
    bool callerIsTrusted() const;

    bool m_ownsSharedName = false;
};
