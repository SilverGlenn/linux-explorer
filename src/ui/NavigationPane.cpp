#include "NavigationPane.h"
#include "NavigationItemDelegate.h"
#include "../data/Settings.h"
#include "Branding.h"
#include "DriveLabel.h"
#include "aero/icons.h"
#include "Locations.h"
#include "Libraries.h"
#include "aero/text.h"
#include "aero/artwork.h"

#include <KDirWatch>
#include <KFilePlacesModel>
#include <KBookmark>

#include <QDir>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QEnterEvent>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QInputDialog>
#include <QKeyEvent>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QStandardPaths>
#include <QTreeWidget>
#include <QVariantAnimation>
#include <QVBoxLayout>
#include <QtConcurrent>

namespace {

constexpr int kSelectionGutter = NavigationItemDelegate::rightGutter;

// How long the expander arrows take to fade in or out, in milliseconds
constexpr int kArrowFadeDuration = 500;

class NavigationTree : public QTreeWidget {
    public:
    explicit NavigationTree(QWidget *parent = nullptr)
    : QTreeWidget(parent)
    , m_fade(new QVariantAnimation(this))
    {
        m_fade->setDuration(kArrowFadeDuration);
        connect(m_fade, &QVariantAnimation::valueChanged, this,
                [this](const QVariant &value) {
            m_arrowOpacity = value.toReal();
            viewport()->update();
        });
    }

    protected:
    void enterEvent(QEnterEvent *event) override
    {
        fadeArrowsTo(1.0);
        QTreeWidget::enterEvent(event);
    }

    void leaveEvent(QEvent *event) override
    {
        fadeArrowsTo(0.0);
        QTreeWidget::leaveEvent(event);
    }

    void drawRow(QPainter *painter, const QStyleOptionViewItem &option,
                 const QModelIndex &index) const override
    {
        painter->save();
        const QRect viewportRect = viewport()->rect();
        painter->setClipRect(QRect(viewportRect.left(), viewportRect.top(),
                                   qMax(0, viewportRect.width() - kSelectionGutter),
                                   viewportRect.height()), Qt::IntersectClip);
        QTreeWidget::drawRow(painter, option, index);
        painter->restore();
    }

    void drawBranches(QPainter *painter, const QRect &rect,
                      const QModelIndex &index) const override
    {
        painter->save();
        painter->setOpacity(m_arrowOpacity);
        const QRect viewportRect = viewport()->rect();
        painter->setClipRect(QRect(viewportRect.left(), viewportRect.top(),
                                   qMax(0, viewportRect.width() - kSelectionGutter),
                                   viewportRect.height()), Qt::IntersectClip);
        QTreeWidget::drawBranches(painter, rect, index);
        painter->restore();
    }

    private:
        void fadeArrowsTo(qreal target)
        {
            m_fade->stop();
            m_fade->setStartValue(m_arrowOpacity);
            m_fade->setEndValue(target);
            m_fade->start();
        }

    QVariantAnimation *m_fade = nullptr;
    qreal m_arrowOpacity = 0.0;
};

// The stand in child that gives a collapsed folder its expander
constexpr int kPlaceholderRole = Qt::UserRole + 1;

// Absent for the headings and for subfolders found by walking the filesystem
constexpr int kPlaceRowRole = Qt::UserRole + 2;
constexpr int kGroupIdRole = Qt::UserRole + 3;

constexpr int kGroupSpacing = 20;

// A spacer row rather than extra height on the headings, since padding a
// heading moves its text but not its expander triangle
QTreeWidgetItem *makeSpacer(int height = kGroupSpacing)
{
    auto *spacer = new QTreeWidgetItem;
    spacer->setFlags(Qt::NoItemFlags);   // not selectable, not clickable
    spacer->setSizeHint(0, QSize(0, height));
    return spacer;
}

// The places model files these alongside the desktop and downloads with no flag
// to tell them apart, so Win7's libraries split is matched by path
bool isLibraryPlace(const QUrl &url)
{
    if (!url.isLocalFile())
        return false;

    static const QStandardPaths::StandardLocation locations[] = {
        QStandardPaths::DocumentsLocation,
        QStandardPaths::MusicLocation,
        QStandardPaths::PicturesLocation,
        QStandardPaths::MoviesLocation,
    };

    const QString path = QDir::cleanPath(url.toLocalFile());
    for (const auto location : locations) {
        const QString dir = QStandardPaths::writableLocation(location);
        if (!dir.isEmpty() && QDir::cleanPath(dir) == path)
            return true;
    }
    return false;
}

// On a worker thread, since a folder on an unresponsive mount can take seconds
// to list and on the main thread that freezes the whole window
QList<QUrl> scanSubdirectories(const QUrl &url)
{
    QList<QUrl> children;
    if (!url.isLocalFile())
        return children;

    QDir dir(url.toLocalFile());
    dir.setFilter(QDir::Dirs | QDir::NoDotAndDotDot);
    dir.setSorting(QDir::Name | QDir::LocaleAware | QDir::IgnoreCase);

    const QFileInfoList entries = dir.entryInfoList();
    children.reserve(entries.size());
    for (const QFileInfo &entry : entries) {
        // Agreeing with the file list about what friendly mode hides
        if (Branding::isSystemFolder(entry.absoluteFilePath()))
            continue;
        children.append(QUrl::fromLocalFile(entry.absoluteFilePath()));
    }
    return children;
}

// A child whose only job is to make its parent show an expander triangle
void addPlaceholder(QTreeWidgetItem *parent)
{
    auto *placeholder = new QTreeWidgetItem(parent);
    placeholder->setData(0, kPlaceholderRole, true);
}

// True until the item's real subfolders have been read in
bool hasOnlyPlaceholder(QTreeWidgetItem *item)
{
    return item->childCount() == 1
        && item->child(0)->data(0, kPlaceholderRole).toBool();
}

// A row under a collapsed parent is still in the tree, and highlighting it
// would put the selection somewhere the user cannot see
bool isItemVisible(QTreeWidgetItem *item)
{
    for (QTreeWidgetItem *parent = item->parent(); parent; parent = parent->parent()) {
        if (!parent->isExpanded())
            return false;
    }
    return true;
}

} // namespace

NavigationPane::NavigationPane(QWidget *parent)
    : QWidget(parent)
    , m_places(new KFilePlacesModel(this))
    , m_tree(new NavigationTree(this))
    , m_watch(new KDirWatch(this))
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    m_tree->setColumnCount(1);
    m_tree->setHeaderHidden(true);
    m_tree->setRootIsDecorated(true);
    m_tree->setIndentation(14);
    m_tree->setIconSize(QSize(16, 16));
    m_tree->installEventFilter(this);
    // Uniform heights would ignore the size hints the spacer rows depend on
    m_tree->setUniformRowHeights(false);
    m_tree->setFrameShape(QFrame::NoFrame);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tree->setItemDelegate(new NavigationItemDelegate(m_tree));
    m_tree->setExpandsOnDoubleClick(false);
    Aero::setPointSize(m_tree, 9);

    // Without the proxy the focus keys land on this container, which cannot
    // take the keyboard, and appear to skip the pane entirely
    setFocusProxy(m_tree);

    // On the viewport, or the tree offers the drop to its own item model
    m_tree->setAcceptDrops(true);
    m_tree->viewport()->setAcceptDrops(true);
    m_tree->setDropIndicatorShown(true);
    m_tree->viewport()->installEventFilter(this);

    // Spring loaded folders, where resting a drag on a collapsed branch opens it
    m_tree->setAutoExpandDelay(700);

    // ID scoped, a declaration only sheet matching everything and pulling every
    // descendant, scroll bars included, into the stylesheet engine
    m_tree->setObjectName("win7NavPane");
    m_tree->setStyleSheet(
        Aero::panelSheet(QStringLiteral("win7NavPane"), Aero::Palette::Surface,
                         Qt::RightEdge, Aero::Palette::PaneRule) +
        // No connector lines, just the expander triangle, and the childless
        // qualifier matters on all three or the expander itself goes blank
        "#win7NavPane::branch:!has-children:has-siblings:!adjoins-item,"
        "#win7NavPane::branch:!has-children:has-siblings:adjoins-item,"
        "#win7NavPane::branch:!has-children:!has-siblings:adjoins-item"
        " { border-image: none; image: none; }"
        // Win7's expander glyphs in place of the style's own indicator, taken
        // from a resource since the stylesheet engine loads images by name
        "#win7NavPane::branch:has-children:!has-siblings:closed,"
        "#win7NavPane::branch:has-children:has-siblings:closed"
        " { border-image: none; image: url(:/win7/branch-closed.png); }"
        "#win7NavPane::branch:open:has-children:!has-siblings,"
        "#win7NavPane::branch:open:has-children:has-siblings"
        " { border-image: none; image: url(:/win7/branch-open.png); }");

    // Single click activates here, unlike the file list
    connect(m_tree, &QTreeWidget::itemClicked, this,
            [this](QTreeWidgetItem *item, int) {
        if (item->flags() == Qt::NoItemFlags)
            return;   // a spacer row between groups

        const QUrl url = urlForItem(item);
        if (url.isValid())
            Q_EMIT urlActivated(url);
        else
            item->setExpanded(!item->isExpanded());
    });

    // The highlight sits on the deepest row on show leading to the current
    // folder, so opening or closing a branch moves it
    connect(m_tree, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem *item) {
        const QUrl url = urlForItem(item);
        const QString groupId = item->data(0, kGroupIdRole).toString();
        if (!m_restoringTree && !groupId.isEmpty())
            Settings::setNavigationGroupExpanded(groupId, true);
        if (url.isValid())
            m_expanded.insert(url);
        populateChildren(item);
        syncHighlight();
    });
    connect(m_tree, &QTreeWidget::itemCollapsed, this, [this](QTreeWidgetItem *item) {
        const QUrl url = urlForItem(item);
        const QString groupId = item->data(0, kGroupIdRole).toString();
        if (!m_restoringTree && !groupId.isEmpty())
            Settings::setNavigationGroupExpanded(groupId, false);
        if (url.isValid())
            m_expanded.remove(url);
        syncHighlight();
    });

    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_tree, &QTreeWidget::customContextMenuRequested,
            this, &NavigationPane::showContextMenu);

    // A folder added or removed elsewhere repopulates the item watching it
    connect(m_watch, &KDirWatch::dirty, this, [this](const QString &path) {
        QTreeWidgetItem *item = itemForUrl(QUrl::fromLocalFile(path));
        if (!item || !item->isExpanded())
            return;
        // Children are only read for an item whose sole child is the
        // placeholder, so the marker goes back first
        while (item->childCount() > 0)
            delete item->takeChild(0);
        addPlaceholder(item);
        populateChildren(item);
    });

    // The model repopulates asynchronously and on every plug or unplug, so the
    // tree is rebuilt rather than patched
    connect(m_places, &QAbstractItemModel::modelReset, this, &NavigationPane::rebuild);
    connect(m_places, &QAbstractItemModel::rowsInserted, this, &NavigationPane::rebuild);
    connect(m_places, &QAbstractItemModel::rowsRemoved, this, &NavigationPane::rebuild);
    connect(m_places, &QAbstractItemModel::dataChanged, this, &NavigationPane::rebuild);

    root->addWidget(m_tree);
    rebuild();
}

KFilePlacesModel *NavigationPane::placesModel() const
{
    return m_places;
}

void NavigationPane::refresh()
{
    // Forced, since the labels can change without any of the model moving
    m_placesSignature.clear();
    rebuild();
}

QStringList NavigationPane::placesSignature() const
{
    QStringList signature;
    signature.reserve(m_places->rowCount());
    for (int row = 0; row < m_places->rowCount(); ++row) {
        const QModelIndex index = m_places->index(row, 0);
        // The model's own label rather than the drive one, which asks Solid and
        // is far too expensive to run on every data change
        signature << QStringLiteral("%1|%2|%3|%4|%5")
                         .arg(m_places->url(index).toString(),
                              m_places->text(index),
                              QString::number(int(m_places->groupType(index))),
                              QString::number(m_places->isHidden(index)),
                              QString::number(m_places->setupNeeded(index)));
    }
    return signature;
}

QUrl NavigationPane::urlForItem(QTreeWidgetItem *item) const
{
    if (!item)
        return {};
    return item->data(0, Qt::UserRole).toUrl();
}

QTreeWidgetItem *NavigationPane::itemForUrl(const QUrl &url) const
{
    if (!url.isValid())
        return nullptr;
    for (QTreeWidgetItemIterator it(m_tree); *it; ++it) {
        if (urlForItem(*it).matches(url, QUrl::StripTrailingSlash))
            return *it;
    }
    return nullptr;
}

QTreeWidgetItem *NavigationPane::addGroup(const QString &groupId, const QString &title,
                                          std::initializer_list<const char *> iconNames,
                                          const QUrl &url)
{
    auto *item = new QTreeWidgetItem(m_tree);
    item->setText(0, title);
    item->setData(0, kGroupIdRole, groupId);
    item->setSizeHint(0, QSize(0, 21));
    item->setIcon(0, Aero::themeIcon(iconNames));
    item->setData(0, kPlaceRowRole, -1);
    if (url.isValid()) {
        item->setData(0, Qt::UserRole, url);
    } else {
        // Enabled so clicks reach the toggle handler, but never selectable
        item->setFlags(Qt::ItemIsEnabled);
    }
    return item;
}

void NavigationPane::addEntry(QTreeWidgetItem *group, const QModelIndex &placeIndex)
{
    const QUrl url = m_places->url(placeIndex);

    // A heading that is itself a destination would otherwise list a child
    // pointing at the same place, where Win7 shows one row
    const QUrl groupUrl = group->data(0, Qt::UserRole).toUrl();
    if (groupUrl.isValid() && groupUrl.matches(url, QUrl::StripTrailingSlash))
        return;

    auto *item = new QTreeWidgetItem(group);
    // One call covers every kind of place
    item->setText(0, Branding::displayName(url, DriveLabel::forPlace(m_places, placeIndex)));
    item->setIcon(0, m_places->icon(placeIndex));
    item->setData(0, Qt::UserRole, url);
    item->setData(0, kPlaceRowRole, placeIndex.row());
    item->setSizeHint(0, QSize(0, 21));
    addPlaceholderIfExpandable(item, url);
}

void NavigationPane::addPlaceholderIfExpandable(QTreeWidgetItem *item, const QUrl &url)
{
    // Local directories only, anything remote meaning a job per node
    if (!url.isLocalFile())
        return;

    const QFileInfo info(url.toLocalFile());
    if (!info.isDir() || !info.isReadable())
        return;

    addPlaceholder(item);
}

void NavigationPane::populateChildren(QTreeWidgetItem *item)
{
    // Nothing to do unless the only child is the stand in
    if (!hasOnlyPlaceholder(item))
        return;

    const QUrl url = urlForItem(item);
    if (!url.isLocalFile() || m_scanning.contains(url))
        return;

    m_scanning.insert(url);

    // The item is not captured, a rebuild during the scan leaving it dangling,
    // so the result is matched back up by location
    auto *watcher = new QFutureWatcher<QList<QUrl>>(this);
    connect(watcher, &QFutureWatcher<QList<QUrl>>::finished, this,
            [this, watcher, url] {
        const QList<QUrl> children = watcher->result();
        watcher->deleteLater();
        m_scanning.remove(url);
        applyChildren(url, children);
    });
    watcher->setFuture(QtConcurrent::run(scanSubdirectories, url));
}

void NavigationPane::applyChildren(const QUrl &url, const QList<QUrl> &children)
{
    QTreeWidgetItem *item = itemForUrl(url);
    if (!item)
        return;   // the tree was rebuilt while the scan was running

    // Anything but the lone placeholder means the item was repopulated since
    if (!hasOnlyPlaceholder(item))
        return;

    delete item->takeChild(0);

    const QIcon folderIcon = Aero::themeIcon({"folder"});
    for (const QUrl &childUrl : children) {
        auto *child = new QTreeWidgetItem(item);
        child->setText(0, Branding::displayName(childUrl, childUrl.fileName()));
        child->setIcon(0, folderIcon);
        child->setData(0, Qt::UserRole, childUrl);
        child->setData(0, kPlaceRowRole, -1);
        child->setSizeHint(0, QSize(0, 21));
        addPlaceholderIfExpandable(child, childUrl);

        // Expanding schedules that child's own scan, one level per round
        if (m_expanded.contains(childUrl))
            child->setExpanded(true);
    }

    if (url.isLocalFile() && !m_watched.contains(url.toLocalFile())) {
        m_watched.insert(url.toLocalFile());
        m_watch->addDir(url.toLocalFile());
    }

    // The new rows may include the next step towards the current folder
    syncHighlight();
}

void NavigationPane::rebuild()
{
    const QStringList signature = placesSignature();
    if (signature == m_placesSignature && m_tree->topLevelItemCount() > 0)
        return;
    m_placesSignature = signature;

    // The tree about to be discarded is what these were taken out for, and
    // leaving them behind accumulates an inotify watch per folder ever opened
    for (const QString &path : std::as_const(m_watched))
        m_watch->removeDir(path);
    m_watched.clear();

    m_restoringTree = true;
    m_tree->clear();

    QTreeWidgetItem *favorites =
        addGroup(QStringLiteral("favorites"), tr("Favorites"), {"favorites", "bookmarks", "starred"});
    QTreeWidgetItem *libraries =
        addGroup(QStringLiteral("libraries"), tr("Libraries"), {"folder-library", "folder-documents", "folder"}, Locations::libraries());
    QTreeWidgetItem *computer =
        addGroup(QStringLiteral("computer"), tr("Computer"), {"computer", "computer-laptop"}, Locations::computer());
    QTreeWidgetItem *network =
        addGroup(QStringLiteral("network"), tr("Network"), {"folder-network", "network-workgroup", "network-server"},
                 QUrl(QStringLiteral("remote:/")));

    for (int row = 0; row < m_places->rowCount(); ++row) {
        const QModelIndex index = m_places->index(row, 0);
        if (m_places->isHidden(index))
            continue;

        const QUrl placeUrl = m_places->url(index);
        const bool systemItem = m_places->bookmarkForIndex(index)
            .metaDataItem(QStringLiteral("isSystemItem")) == QLatin1String("true");
        // KDE's built-in Trash and Recent Files are not Win7 Favorites. Keep
        // explicitly added bookmarks, and never edit the shared places file.
        if (systemItem && (placeUrl.scheme() == QLatin1String("trash")
            || (placeUrl.scheme() == QLatin1String("recentlyused")
                && placeUrl.path() == QLatin1String("/files"))))
            continue;

        QTreeWidgetItem *group = nullptr;
        switch (m_places->groupType(index)) {
        case KFilePlacesModel::PlacesType: {
            // Win7 keeps no home entry here, it being a third route to a place
            // the other two groups already reach
            const bool isHome = placeUrl.isLocalFile()
                && QDir::cleanPath(placeUrl.toLocalFile()) == QDir::homePath();
            if (isHome)
                continue;

            if (isLibraryPlace(placeUrl))
                continue; // canonical XDG library shortcuts are added below
            group = favorites;
            break;
        }
        case KFilePlacesModel::RecentlySavedType:
        case KFilePlacesModel::SearchForType:
            group = favorites;
            break;
        case KFilePlacesModel::DevicesType:
        case KFilePlacesModel::RemovableDevicesType:
            // Left out here as on the Computer page, so the two agree
            if (m_places->setupNeeded(index))
                continue;
            group = computer;
            break;
        case KFilePlacesModel::RemoteType:
            group = network;
            break;
        default:
            continue;   // tags and anything new upstream adds
        }
        addEntry(group, index);
    }

    // KDE does not always bookmark Desktop/Downloads. Supply the native
    // defaults without modifying user-places.xbel, and honor hidden bookmarks.
    int favoritePosition = 0;
    const struct { QStandardPaths::StandardLocation location; const char *name; const char *icon; } defaults[] = {
        {QStandardPaths::DesktopLocation, QT_TR_NOOP("Desktop"), "user-desktop"},
        {QStandardPaths::DownloadLocation, QT_TR_NOOP("Downloads"), "folder-download"},
    };
    for (const auto &entry : defaults) {
        const QString path = QStandardPaths::writableLocation(entry.location);
        if (path.isEmpty() || QDir::cleanPath(path) == QDir::homePath()) continue;
        const QUrl url = QUrl::fromLocalFile(path);
        QTreeWidgetItem *item = nullptr;
        for (int i = 0; i < favorites->childCount(); ++i)
            if (urlForItem(favorites->child(i)).matches(url, QUrl::StripTrailingSlash))
                item = favorites->child(i);
        if (!item) {
            bool bookmarked = false;
            for (int row = 0; row < m_places->rowCount(); ++row)
                if (m_places->url(m_places->index(row, 0)).matches(url, QUrl::StripTrailingSlash))
                    bookmarked = true;
            if (bookmarked) continue;
            item = new QTreeWidgetItem;
            item->setText(0, tr(entry.name));
            item->setIcon(0, Aero::themeIcon({entry.icon, "folder"}));
            item->setData(0, Qt::UserRole, url);
            item->setData(0, kPlaceRowRole, -1);
            item->setSizeHint(0, QSize(0, 21));
            addPlaceholderIfExpandable(item, url);
        } else {
            favorites->takeChild(favorites->indexOfChild(item));
        }
        favorites->insertChild(favoritePosition++, item);
    }
    for (int i = favoritePosition; i < favorites->childCount(); ++i) {
        QTreeWidgetItem *item = favorites->child(i);
        const QUrl url = urlForItem(item);
        if (url.scheme() == QLatin1String("recentlyused") && url.path() == QLatin1String("/locations")) {
            item->setText(0, tr("Recent Places"));
            favorites->takeChild(i);
            favorites->insertChild(favoritePosition, item);
            break;
        }
    }

    for (const auto &entry : Libraries::entries()) {
        auto *item = new QTreeWidgetItem(libraries);
        item->setText(0, entry.name);
        item->setIcon(0, QIcon::fromTheme(entry.iconName, Aero::themeIcon({"folder"})));
        item->setData(0, Qt::UserRole, entry.url);
        item->setData(0, kPlaceRowRole, -1);
        item->setSizeHint(0, QSize(0, 21));
        addPlaceholderIfExpandable(item, entry.url);
    }

    // Defaults apply only until the user has explicitly chosen a group state.
    const QHash<QString, bool> groupDefaults = {
        {QStringLiteral("favorites"), true}, {QStringLiteral("libraries"), true},
        {QStringLiteral("computer"), false}, {QStringLiteral("network"), false},
    };
    for (QTreeWidgetItem *group : {favorites, libraries, computer, network}) {
        if (group->childCount() > 0 || group->data(0, Qt::UserRole).toUrl().isValid()) {
            const QString id = group->data(0, kGroupIdRole).toString();
            group->setExpanded(Settings::navigationGroupExpanded(id).value_or(groupDefaults.value(id)));
        } else
            delete group;   // a heading with neither children nor a destination
    }

    // Each expand starts a scan whose children reopen in turn as results land
    for (QTreeWidgetItemIterator it(m_tree); *it; ++it) {
        const QUrl url = urlForItem(*it);
        if (url.isValid() && !(*it)->data(0, kGroupIdRole).isValid()
            && m_expanded.contains(url))
            (*it)->setExpanded(true);
    }

    // After the empty headings are gone, so a dropped group leaves no double
    // gap, and backwards so the indices ahead stay valid as rows are inserted
    for (int i = m_tree->topLevelItemCount() - 1; i >= 0; --i)
        m_tree->insertTopLevelItem(i, makeSpacer(i == 0 ? 10 : kGroupSpacing));

    setCurrentUrl(m_currentUrl);
    m_restoringTree = false;
}

void NavigationPane::setCurrentUrl(const QUrl &url)
{
    m_currentUrl = url;
    syncHighlight();
}

QTreeWidgetItem *NavigationPane::highlightTarget() const
{
    if (!m_currentUrl.isValid())
        return nullptr;

    const bool local = m_currentUrl.isLocalFile();
    QString target;
    if (local)
        target = QDir::cleanPath(m_currentUrl.toLocalFile());

    // Every row rather than the places model's closest entry, the tree also
    // carrying headings that are destinations and subfolders found on disk
    QTreeWidgetItem *best = nullptr;
    int bestLength = -1;
    for (QTreeWidgetItemIterator it(m_tree); *it; ++it) {
        const QUrl url = urlForItem(*it);
        if (!url.isValid() || !isItemVisible(*it))
            continue;

        // The folder itself beats any ancestor, wherever it turns up
        if (url.matches(m_currentUrl, QUrl::StripTrailingSlash))
            return *it;

        if (!local || !url.isLocalFile())
            continue;

        // Only a folder the current one is inside of, so /home/a must not
        // count as containing /home/ab
        const QString path = QDir::cleanPath(url.toLocalFile());
        QString prefix = path;
        if (!prefix.endsWith(QLatin1Char('/')))
            prefix += QLatin1Char('/');
        if (!target.startsWith(prefix))
            continue;
        if (path.length() > bestLength) {
            bestLength = path.length();
            best = *it;
        }
    }
    return best;
}

void NavigationPane::syncHighlight()
{
    // Nothing is expanded from here, the pane marking where the folder lies
    // rather than opening itself up to it
    QTreeWidgetItem *item = highlightTarget();
    if (m_tree->currentItem() == item)
        return;

    QSignalBlocker blocker(m_tree);
    m_tree->clearSelection();
    m_tree->setCurrentItem(item);
    if (item)
        m_tree->scrollToItem(item);
}

void NavigationPane::showContextMenu(const QPoint &pos)
{
    QTreeWidgetItem *item = m_tree->itemAt(pos);
    if (!item || item->flags() == Qt::NoItemFlags)
        return;

    const QUrl url = urlForItem(item);
    const int placeRow = item->data(0, kPlaceRowRole).toInt();
    QModelIndex placeIndex;
    if (placeRow >= 0)
        placeIndex = m_places->index(placeRow, 0);

    QMenu menu(this);

    if (url.isValid()) {
        menu.addAction(tr("Open"), this, [this, url] { Q_EMIT urlActivated(url); });
        menu.addSeparator();
    }

    if (url.scheme() == QLatin1String("trash")) {
        menu.addAction(tr("Empty Recycle Bin"), this,
                       [this] { Q_EMIT emptyTrashRequested(); });
        menu.addSeparator();
    }

    if (Locations::isComputer(url)) {
        menu.addAction(tr("Connect drives..."), this,
                       [this] { Q_EMIT connectDrivesRequested(); });
        menu.addSeparator();
    }

    if (url.scheme() == QLatin1String("remote")) {
        menu.addAction(tr("Map network drive..."), this,
                       [this] { Q_EMIT mapDriveRequested(); });
        menu.addSeparator();
    }

    // A subfolder found on the filesystem has no bookmark behind it to edit
    if (placeIndex.isValid()) {
        const bool device = m_places->isDevice(placeIndex);

        menu.addAction(tr("Rename..."), this, [this, placeIndex] {
            bool ok = false;
            const QString name = QInputDialog::getText(
                this, tr("Rename"), tr("Name:"), QLineEdit::Normal,
                m_places->text(placeIndex), &ok);
            if (ok && !name.isEmpty()) {
                m_places->editPlace(placeIndex, name, m_places->url(placeIndex),
                                    m_places->icon(placeIndex).name());
            }
        });

        if (!device) {
            // Devices come from the system, so removing one could only mean
            // hiding it, which is the entry below
            menu.addAction(tr("Remove"), this,
                           [this, placeIndex] { m_places->removePlace(placeIndex); });
        }
        menu.addAction(tr("Hide"), this, [this, placeIndex] {
            m_places->setPlaceHidden(placeIndex, true);
        });

        menu.addSeparator();
        QAction *up = menu.addAction(tr("Move up"));
        up->setEnabled(placeRow > 0);
        connect(up, &QAction::triggered, this,
                [this, placeRow] { m_places->movePlace(placeRow, placeRow - 1); });
        QAction *down = menu.addAction(tr("Move down"));
        down->setEnabled(placeRow < m_places->rowCount() - 1);
        connect(down, &QAction::triggered, this,
                [this, placeRow] { m_places->movePlace(placeRow, placeRow + 2); });
        menu.addSeparator();
    } else if (url.isLocalFile()) {
        menu.addAction(tr("Add to Favorites"), this, [this, url, item] {
            m_places->addPlace(item->text(0), url,
                               QStringLiteral("folder"), QString());
        });
        menu.addSeparator();
    }

    QAction *showHidden = menu.addAction(tr("Show hidden places"));
    showHidden->setCheckable(true);
    showHidden->setChecked(false);
    connect(showHidden, &QAction::triggered, this, [this] {
        // Unhides everything at once, as Win7's own restore does
        for (int row = 0; row < m_places->rowCount(); ++row) {
            const QModelIndex index = m_places->index(row, 0);
            if (m_places->isHidden(index))
                m_places->setPlaceHidden(index, false);
        }
    });

    menu.exec(m_tree->viewport()->mapToGlobal(pos));
}

bool NavigationPane::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_tree && event->type() == QEvent::KeyPress) {
        const auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            const QUrl url = urlForItem(m_tree->currentItem());
            if (url.isValid()) Q_EMIT urlActivated(url);
            return true;
        }
    }
    if (watched != m_tree->viewport())
        return QWidget::eventFilter(watched, event);

    const auto targetAt = [this](const QPoint &pos) {
        QTreeWidgetItem *item = m_tree->itemAt(pos);
        const QUrl url = urlForItem(item);
        // Computer is not a directory and nothing can be copied into it
        if (Locations::isComputer(url) || Locations::isLibraries(url))
            return QUrl();
        return url;
    };

    switch (event->type()) {
    case QEvent::MouseButtonPress: {
        auto *mouse = static_cast<QMouseEvent *>(event);
        if (mouse->button() != Qt::MiddleButton)
            break;
        const QUrl url = urlForItem(m_tree->itemAt(mouse->position().toPoint()));
        if (url.isValid() && !Locations::isComputer(url))
            Q_EMIT newWindowRequested(url);
        return true;
    }
    case QEvent::DragEnter: {
        auto *drag = static_cast<QDragEnterEvent *>(event);
        if (drag->mimeData()->hasUrls()) {
            drag->acceptProposedAction();
            return true;
        }
        break;
    }
    case QEvent::DragMove: {
        auto *drag = static_cast<QDragMoveEvent *>(event);
        const QPoint pos = drag->position().toPoint();
        const QUrl target = targetAt(pos);
        if (drag->mimeData()->hasUrls() && target.isValid()) {
            drag->acceptProposedAction();
            m_tree->setCurrentItem(m_tree->itemAt(pos));
        } else {
            drag->ignore();
        }
        return true;
    }
    case QEvent::Drop: {
        auto *drop = static_cast<QDropEvent *>(event);
        const QUrl target = targetAt(drop->position().toPoint());
        if (target.isValid()) {
            Q_EMIT dropped(drop, target);
            drop->acceptProposedAction();
        }
        // Consumed either way, or the tree makes rows out of the dropped files
        return true;
    }
    default:
        break;
    }

    return QWidget::eventFilter(watched, event);
}
