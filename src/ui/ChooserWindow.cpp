#include "ChooserWindow.h"

#include <AeroQt/insetwindow.h>

#include <KStartupInfo>
#include <KWindowSystem>
#include <QDialog>
#include <QGuiApplication>
#include <QRegularExpression>
#include <QWindow>

namespace {
bool hasControl(const QString &value)
{
    for (QChar ch : value) {
        if (ch.category() == QChar::Other_Control)
            return true;
    }
    return false;
}
} // namespace

void Explorer::configureChooserGlass(QDialog *dialog, QWidget *contents, QWidget *navigation)
{
    // Use the same frame inset as the browser. AeroQt owns the top geometry,
    // contents margins, transparency and blur lifecycle; the QDialog continues
    // to own its native decoration and presentation metadata.
    dialog->setAttribute(Qt::WA_TranslucentBackground, true);
    dialog->setAttribute(Qt::WA_NoSystemBackground, true);
    Aero::makeInsetWindow(dialog, contents, navigation, nullptr);
}

QString Explorer::validateChooserWindowOptions(const ChooserWindowOptions &options)
{
    const QString parent = options.parentWindow;
    if (parent.size() > 4096 || options.activationToken.size() > 4096)
        return QStringLiteral("Chooser window metadata exceeds string limits.");
    if (options.modal && parent.isEmpty())
        return QStringLiteral("modal requires parent_window.");
    if (!parent.isEmpty()) {
        static const QRegularExpression x11(QStringLiteral("^x11:([0-9a-fA-F]{1,16})$"));
        const auto match = x11.match(parent);
        if (match.hasMatch()) {
            bool ok = false;
            const qulonglong id = match.captured(1).toULongLong(&ok, 16);
            if (!ok || id == 0 || id > 0xffffffffULL || !KWindowSystem::isPlatformX11())
                return QStringLiteral("X11 parent is invalid or incompatible with this backend.");
        } else if (parent.startsWith(QLatin1String("wayland:"))) {
            const QString handle = parent.mid(8);
            if (handle.isEmpty() || hasControl(handle) || handle.contains(QLatin1Char(' '))
                || !KWindowSystem::isPlatformWayland())
                return QStringLiteral("Wayland parent is invalid or incompatible with this backend.");
        } else {
            return QStringLiteral("parent_window must be x11:<hexid> or wayland:<exported-handle>.");
        }
    }
    if (hasControl(options.activationToken))
        return QStringLiteral("activation_token contains control characters.");
    return {};
}

void Explorer::showChooserWindow(QDialog *dialog, const ChooserWindowOptions &options)
{
    dialog->setWindowModality(options.modal ? Qt::WindowModal : Qt::NonModal);
    const QString previous = QGuiApplication::desktopFileName();
    QGuiApplication::setDesktopFileName(QStringLiteral("linux-explorer-win7-dialog"));
    dialog->winId();
    QWindow *window = dialog->windowHandle();
    const ChooserWindowOptions &presentation = options;
    if (presentation.parentWindow.startsWith(QLatin1String("x11:"))) {
        KWindowSystem::setMainWindow(window, WId(presentation.parentWindow.mid(4).toULongLong(nullptr, 16)));
    } else if (presentation.parentWindow.startsWith(QLatin1String("wayland:"))) {
        KWindowSystem::setMainWindow(window, presentation.parentWindow.mid(8));
    }
    if (!presentation.activationToken.isEmpty() && KWindowSystem::isPlatformX11())
        KStartupInfo::setNewStartupId(window, presentation.activationToken.toUtf8());
    dialog->show();
    QGuiApplication::setDesktopFileName(previous);
    if (!presentation.activationToken.isEmpty()) {
        if (KWindowSystem::isPlatformWayland())
            KWindowSystem::setCurrentXdgActivationToken(presentation.activationToken);
        KWindowSystem::activateWindow(window);
    }
}
