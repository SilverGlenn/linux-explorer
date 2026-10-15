#include "PortalChooserService.h"
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
    app.setApplicationName(QStringLiteral("explorer-win7-portal"));
    app.setApplicationVersion(QStringLiteral("1"));
    app.setDesktopFileName(QStringLiteral("linux-explorer-win7-dialog"));
    app.setWindowIcon(Aero::themeIcon({"system-file-manager", "folder"}));
    app.setQuitOnLastWindowClosed(false);
    app.setQuitLockEnabled(false);

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Explorer FileChooser portal backend."));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.process(app);
    if (!parser.positionalArguments().isEmpty())
        parser.showHelp(1);

    auto bus = QDBusConnection::sessionBus();
    bus.connect(QString(), QStringLiteral("/org/freedesktop/DBus/Local"),
                QStringLiteral("org.freedesktop.DBus.Local"), QStringLiteral("Disconnected"), &app, SLOT(quit()));
    PortalChooserService service(bus);
    if (!service.claim()) {
        qCritical() << "Could not claim org.freedesktop.impl.portal.desktop.explorerwin7";
        return 1;
    }
    Explorer::configureApplicationAppearance(&app);
    return app.exec();
}
