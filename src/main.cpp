#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDir>
#include <QStringList>
#include <QUrl>
#include "FileManagerService.h"
#include "MainWindow.h"
#include "ApplicationAppearance.h"
#include "aero/icons.h"
#include "Locations.h"

// Bare paths are accepted alongside locations, an argument from a shell being
// far more likely to be a relative path
static QList<QUrl> urlsFrom(const QStringList &arguments)
{
    QList<QUrl> urls;
    urls.reserve(arguments.size());
    for (const QString &argument : arguments) {
        // A quoted home shorthand arrives unexpanded and would otherwise be
        // read as a relative path of that name
        QString path = argument;
        if (path == QLatin1String("~"))
            path = QDir::homePath();
        else if (path.startsWith(QLatin1String("~/")))
            path.replace(0, 1, QDir::homePath());

        const QUrl url = QUrl::fromUserInput(path, QDir::currentPath(),
                                             QUrl::AssumeLocalFile);
        if (url.isValid())
            urls.append(url);
    }
    return urls;
}

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    app.setOrganizationName("linux-explorer-win7");
    app.setApplicationName("explorer-win7");
    app.setApplicationVersion(QStringLiteral("1.2-win7.1"));

    // A decoration theme's exception list matches on the window class, which on
    // Wayland is the app id and is read from this as each window is created.
    // The browser windows claim the plain name for themselves, so everything
    // else lands here, dialogs raised by KIO and the desktop included, and an
    // exception written to strip a browser window's caption leaves them alone
    app.setDesktopFileName(QStringLiteral("linux-explorer-win7-dialog"));
    // No display name is set, Qt appending one to every title where Explorer's
    // is just the folder name
    app.setWindowIcon(Aero::themeIcon({"system-file-manager", "folder"}));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("The Windows 7 Explorer, on Linux."));
    parser.addHelpOption();
    parser.addVersionOption();
    // Windows spells this select
    QCommandLineOption selectOption(
        QStringList{QStringLiteral("s"), QStringLiteral("select")},
        QStringLiteral("Open each argument's parent folder with the argument "
                       "selected, instead of opening it."));
    parser.addOption(selectOption);
    QCommandLineOption registerOption(QStringLiteral("register-file-manager"),
        QStringLiteral("Also claim org.freedesktop.FileManager1 for this session. "
                       "By default this preview leaves your existing file manager alone."));
    parser.addOption(registerOption);
    parser.addPositionalArgument(
        QStringLiteral("url"),
        QStringLiteral("Folders to open, or files to reveal with --select."),
        QStringLiteral("[url...]"));
    parser.process(app);

    const bool reveal = parser.isSet(selectOption);
    const QList<QUrl> urls = urlsFrom(parser.positionalArguments());

    // Normalised before anything else sees them, the handoff and the bus
    // interface both speaking locations, and this process's working directory
    // being the only place a relative path can correctly resolve
    //
    // FullyEncoded, since the receiving end re-parses these in StrictMode: a
    // plain toString() leaves spaces and other reserved characters literal,
    // which StrictMode then rejects, silently dropping the location
    QStringList uris;
    uris.reserve(urls.size());
    for (const QUrl &url : urls)
        uris.append(url.toString(QUrl::FullyEncoded));

    // One Explorer per session, so a second launch hands its arguments to the
    // one holding the bus name and gets out of the way
    auto *service = new FileManagerService(&app);
    if (!service->claim(parser.isSet(registerOption))) {
        if (FileManagerService::forward(uris, reveal))
            return 0;
        // The running instance did not answer, so open a window here
    }

    Explorer::configureApplicationAppearance(&app);

    const QString startupId = FileManagerService::startupId();
    if (reveal)
        service->ShowItems(uris, startupId);
    else
        service->ShowFolders(uris, startupId);

    // A reveal naming only files that no longer exist can get here with no
    // window, which would be a process the user cannot see or quit
    if (MainWindow::openWindowCount() == 0)
        MainWindow::openWindow(Locations::libraries(), {}, startupId);

    return app.exec();
}
