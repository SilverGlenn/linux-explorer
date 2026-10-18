#pragma once

#include <QHash>
#include <QPointer>
#include <QSet>
#include <QStringList>
#include <QUrl>
#include <QWidget>

class KDirWatch;
class KFilePlacesModel;
class GoogleDriveConnection;
class QDropEvent;
class QModelIndex;
class QMimeData;
class QTreeWidget;
class QTreeWidgetItem;

// The left hand pane, grouped as Win7 groups it into favorites, libraries,
// computer and network
//
// The entries come from the places model, whose own grouping supplies the
// headings, and favorites against libraries is the one split it cannot express,
// so the standard user directories are matched by path
class NavigationPane : public QWidget {
    Q_OBJECT

public:
    explicit NavigationPane(QWidget *parent = nullptr);

    // Supplies verified mount state without starting sign-in or probing Drive.
    NavigationPane(GoogleDriveConnection *driveConnection, QWidget *parent);

    // Shared with the address bar, which resolves its icon from these entries
    KFilePlacesModel *placesModel() const;

    // Explicit favorites use persistent places bookmarks, reusing an existing
    // bookmark (and unhiding it) when the same folder is added again.
    // knownDirectory skips the stat for a caller that already has the answer
    static bool canAddToFavorites(const QUrl &url, bool knownDirectory = false);
    void addToFavorites(const QUrl &url, const QString &name = QString());

    // Follows navigation that started elsewhere without reporting it back, and
    // the tree is never opened up to it, the highlight landing on the deepest
    // row already on show that leads to the folder
    void setCurrentUrl(const QUrl &url);

    // Keeps expanded folders open, the model repopulating on every plug
    void refresh();

Q_SIGNALS:
    void urlActivated(const QUrl &url);
    void newWindowRequested(const QUrl &url);

    // From the recycle bin's context menu, the window owning the confirmation
    void emptyTrashRequested();

    // From the computer heading's context menu
    void connectDrivesRequested();

    // From the network heading's context menu
    void mapDriveRequested();
    void googleDriveRequested();

    void favoritesOrderChanged();

    // The event is still live and must be used before returning
    void dropped(QDropEvent *event, const QUrl &destination);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void rebuild();

    // What the tree is built from, so a data change that moved none of it can
    // be ignored rather than clearing the tree and rescanning every open branch
    QStringList placesSignature() const;

    QTreeWidgetItem *addGroup(const QString &groupId, const QString &title,
                              std::initializer_list<const char *> iconNames,
                              const QUrl &url = QUrl());
    void addEntry(QTreeWidgetItem *group, const QModelIndex &placeIndex);

    // A placeholder child makes the triangle appear without reading the
    // subfolders, since scanning up front would stall on an unresponsive mount
    // knownDirectory skips the stat for a caller that already knows the url is
    // a directory, such as a child the background scan just listed
    void addPlaceholderIfExpandable(QTreeWidgetItem *item, const QUrl &url,
                                    bool knownDirectory = false);
    void populateChildren(QTreeWidgetItem *item);

    // Scans run off the main thread, so the item may be gone by the time one
    // lands
    void applyChildren(const QUrl &url, const QList<QUrl> &children);

    // Called again whenever what is on show can change
    void syncHighlight();

    // The folder's own row if on show, otherwise the deepest visible one
    // containing it, and nothing at all if none leads to it
    QTreeWidgetItem *highlightTarget() const;

    void showContextMenu(const QPoint &pos);
    bool moveFavorite(const QUrl &url, int insertionIndex);
    void applyFavoritesOrder();
    int favoriteDropIndex(const QPoint &pos, int *lineY = nullptr) const;
    bool addDroppedFavorites(QList<QUrl> urls, int insertionIndex);
    void rememberDrag(const QMimeData *mime);
    QTreeWidgetItem *itemForUrl(const QUrl &url) const;
    QUrl urlForItem(QTreeWidgetItem *item) const;

    KFilePlacesModel *m_places = nullptr;
    QPointer<GoogleDriveConnection> m_driveConnection;
    QTreeWidget      *m_tree = nullptr;

    // Watches every expanded folder, so changes made elsewhere show up here,
    // tracked so a rebuild can drop the ones its tree no longer has rows for
    KDirWatch *m_watch = nullptr;
    QSet<QString> m_watched;

    QStringList m_placesSignature;

    // So the highlight survives a rebuild
    QUrl m_currentUrl;

    // Reopened after a rebuild
    QSet<QUrl> m_expanded;
    bool m_restoringTree = false;

    QPointer<const QMimeData> m_dragMime;
    QList<QUrl> m_dragFolders;

    // Scans in flight, so reexpanding a folder starts no duplicate
    QSet<QUrl> m_scanning;
};
