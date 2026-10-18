#include "AccessDialogs.h"

#include "aero/icons.h"
#include "aero/sounds.h"
#include "aero/taskdialog.h"
#include "aero/text.h"

#include <KIO/Global>

#include <QCoreApplication>
#include <QDialog>
#include <QLabel>
#include <QPushButton>
#include <QStorageInfo>

namespace {

QString translate(const char *text)
{
    return QCoreApplication::translate("AccessDialogs", text);
}

} // namespace

namespace AccessDialogs {

bool isPermissionError(int error)
{
    switch (error) {
    case KIO::ERR_ACCESS_DENIED:
    case KIO::ERR_WRITE_ACCESS_DENIED:
    case KIO::ERR_CANNOT_ENTER_DIRECTORY:
    case KIO::ERR_CANNOT_OPEN_FOR_READING:
    case KIO::ERR_CANNOT_OPEN_FOR_WRITING:
        return true;
    default:
        return false;
    }
}

void showFailure(QWidget *parent, const QString &title, const QString &primary,
                 const QString &secondary)
{
    const QIcon errorIcon =
        Aero::themeIcon({"dialog-error", "messagebox_critical", "emblem-error"});
    Aero::TaskDialog dialog(parent, title, errorIcon);

    QString reason = secondary;
    if (reason.isEmpty())
        reason = translate("Access is denied.");

    dialog.addLine(Aero::bodyLabel(primary));
    dialog.addLine(Aero::bodyLabel(reason));

    QPushButton *ok = dialog.addButton(translate("OK"));
    ok->setDefault(true);
    QObject::connect(ok, &QPushButton::clicked, &dialog, &QDialog::accept);

    dialog.lockSize(360);
    Aero::playSound(QStringLiteral("Windows Critical Stop"));
    dialog.exec();
}

void showLocationUnavailable(QWidget *parent, const QString &path,
                             const QString &reason)
{
    showFailure(parent, translate("Location is not available"),
                translate("%1 is not accessible.").arg(path), reason);
}

bool canOfferElevation(const QString &localPath)
{
    // Removable media and foreign mounts are somebody else's to administer,
    // and the admin worker would only widen what a stick or share can do here
    static const char *const kForeignRoots[] = {"/media", "/run/media", "/mnt"};
    for (const char *root : kForeignRoots) {
        const QString prefix = QString::fromLatin1(root);
        if (localPath == prefix || localPath.startsWith(prefix + QLatin1Char('/')))
            return false;
    }

    const QStorageInfo storage(localPath);
    if (!storage.isValid())
        return true;
    const QByteArray type = storage.fileSystemType();
    return !(type.startsWith("fuse") || type.startsWith("nfs") || type == "cifs"
             || type.startsWith("smb") || type == "9p" || type == "vfat"
             || type == "exfat" || type.startsWith("ntfs"));
}

bool askForAdminAccess(QWidget *parent, const QString &folderName, const QString &path)
{
    // The folder's own name is the title, as in Windows
    const QIcon warningIcon = Aero::themeIcon({"dialog-warning", "messagebox_warning"});
    Aero::TaskDialog dialog(parent, folderName, warningIcon);

    // Win7's main instruction, bigger and in its own blue
    QLabel *primary = Aero::label(
        translate("You don't currently have permission to access this folder."),
        12, Aero::Palette::CrumbHover);
    dialog.addLine(primary);
    // Not Windows' wording, since Continue rewrites no permissions here and
    // opens the folder through the admin worker instead
    dialog.addLine(Aero::bodyLabel(
        translate("Click Continue to open this folder as an administrator.")));
    // The title is only the last component, and elevating the wrong folder
    // is the mistake worth preventing. bodyLabel is plain text, so the path
    // cannot inject markup.
    if (!path.isEmpty())
        dialog.addLine(Aero::bodyLabel(path));

    QPushButton *proceed = dialog.addButton(translate("Continue"));
    proceed->setDefault(true);
    QPushButton *cancel = dialog.addButton(translate("Cancel"));

    QObject::connect(proceed, &QPushButton::clicked, &dialog, &QDialog::accept);
    QObject::connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);

    dialog.lockSize(400);
    Aero::playSound(QStringLiteral("Windows Exclamation"));
    return dialog.exec() == QDialog::Accepted;
}

void showAdministratorWarning(QWidget *parent)
{
    const QIcon warningIcon = Aero::themeIcon({"dialog-warning", "messagebox_warning"});
    Aero::TaskDialog dialog(parent, translate("Administrator"), warningIcon);
    dialog.addLine(Aero::bodyLabel(translate(
        "You are using File Explorer as an administrator. You can perform "
        "actions that are normally restricted, which means that you have the "
        "ability to render your computer unusable if you don't know what "
        "you're doing. Be careful!")));

    QPushButton *ok = dialog.addButton(translate("OK"));
    ok->setDefault(true);
    QObject::connect(ok, &QPushButton::clicked, &dialog, &QDialog::accept);

    dialog.lockSize(420);
    Aero::playSound(QStringLiteral("Windows Exclamation"));
    dialog.exec();
}

} // namespace AccessDialogs
