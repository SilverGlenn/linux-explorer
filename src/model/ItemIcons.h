#pragma once

#include <QIcon>
#include <QMimeDatabase>
#include <QString>

#include <KFileItem>

// A .desktop or .directory file names its own icon, and an absolute path there
// is opened and decoded on the GUI thread, where a FIFO or /dev/ptmx blocks
// forever, so only theme names are taken from a file's contents
namespace ItemIcons {

inline QString safeIconName(const KFileItem &item)
{
    const QString name = item.iconName();
    if (!name.contains(QLatin1Char('/')))
        return name;
    const QMimeType mime = QMimeDatabase().mimeTypeForName(item.mimetype());
    return mime.isValid() ? mime.iconName() : QStringLiteral("unknown");
}

inline QIcon safeIcon(const KFileItem &item, const QIcon &fallback = QIcon())
{
    return QIcon::fromTheme(safeIconName(item), fallback);
}

} // namespace ItemIcons
