#pragma once

#include <QCoreApplication>
#include <QDir>
#include <QList>
#include <QObject>
#include <QStandardPaths>
#include <QUrl>

// Linux's XDG user folders are the targets behind the four library shortcuts.
// This is deliberately not a claim to implement Windows' multi-folder libraries.
namespace Libraries {

struct Entry {
    QString name;
    QString iconName;
    QUrl url;
};

inline QList<Entry> entries()
{
    const struct {
        QStandardPaths::StandardLocation location;
        const char *name;
        const char *icon;
    } definitions[] = {
        {QStandardPaths::DocumentsLocation, QT_TRANSLATE_NOOP("Libraries", "Documents"), "folder-documents"},
        {QStandardPaths::MusicLocation, QT_TRANSLATE_NOOP("Libraries", "Music"), "folder-music"},
        {QStandardPaths::PicturesLocation, QT_TRANSLATE_NOOP("Libraries", "Pictures"), "folder-pictures"},
        {QStandardPaths::MoviesLocation, QT_TRANSLATE_NOOP("Libraries", "Videos"), "folder-videos"},
    };
    QList<Entry> result;
    for (const auto &definition : definitions) {
        const QString path = QStandardPaths::writableLocation(definition.location);
        // XDG permits a user to disable a folder by pointing it at $HOME.
        if (path.isEmpty() || QDir::cleanPath(path) == QDir::homePath())
            continue;
        result.append({QCoreApplication::translate("Libraries", definition.name),
                       QLatin1String(definition.icon), QUrl::fromLocalFile(path)});
    }
    return result;
}

inline Entry forUrl(const QUrl &url)
{
    for (const auto &entry : entries()) {
        if (entry.url.matches(url, QUrl::StripTrailingSlash))
            return entry;
    }
    return {};
}

} // namespace Libraries
