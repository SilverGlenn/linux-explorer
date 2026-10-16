#pragma once

#include <QMimeData>
#include <QUrl>

namespace FolderDragData {

inline const QString mimeType = QStringLiteral("application/x-explorer-win7-folders");

// The directory listing already knows which remote items are folders. Carry
// that information with the normal URI list instead of statting a server on
// every drag movement. Other applications still receive the ordinary URLs.
inline void setFolders(QMimeData *mime, const QList<QUrl> &folders)
{
    QMimeData data;
    data.setUrls(folders);
    mime->setData(mimeType, data.data(QStringLiteral("text/uri-list")));
}

inline QList<QUrl> folders(const QMimeData *mime)
{
    QMimeData data;
    data.setData(QStringLiteral("text/uri-list"), mime->data(mimeType));
    return data.urls();
}

} // namespace FolderDragData
