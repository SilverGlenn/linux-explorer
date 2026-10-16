#include "ChooserService.h"
#include "ApplicationAppearance.h"
#include "aero/icons.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDBusConnection>
#include <QDebug>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("linux-explorer-win7"));
    app.setApplicationName(QStringLiteral("explorer-win7-chooser"));
    app.setApplicationVersion(QStringLiteral("1"));
    app.setDesktopFileName(QStringLiteral("linux-explorer-win7-chooser-dialog"));
    app.setWindowIcon(Aero::themeIcon({"system-file-manager", "folder"}));
    // Activation is long-lived, independent of both browser and dialog count.
    app.setQuitOnLastWindowClosed(false);
    // KIO jobs use QEventLoopLocker; completing the last listing must not quit
    // this long-lived service even though it has no visible dialog left.
    app.setQuitLockEnabled(false);

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Private Windows 7 Explorer file chooser service."));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.process(app);
    if (!parser.positionalArguments().isEmpty())
        parser.showHelp(1);

    auto bus = QDBusConnection::sessionBus();
    bus.connect(QString(), QStringLiteral("/org/freedesktop/DBus/Local"),
                QStringLiteral("org.freedesktop.DBus.Local"), QStringLiteral("Disconnected"),
                &app, SLOT(quit()));
    ChooserService service(bus);
    if (!service.claim()) {
        qCritical() << "Could not claim org.win7explorer.FileChooser1";
        return 1;
    }
    Explorer::configureApplicationAppearance(&app);
    return app.exec();
}
