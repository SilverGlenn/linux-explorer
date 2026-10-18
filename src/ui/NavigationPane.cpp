#include "NavigationPane.h"
#include "NavigationItemDelegate.h"
#include "../data/Settings.h"
#include "../data/GoogleDriveConnection.h"
#include "Branding.h"
#include "DriveLabel.h"
#include "FolderDragData.h"
#include "aero/icons.h"
#include "aero/drag.h"
#include "aero/contextmenu.h"
#include "Locations.h"
#include "Libraries.h"
#include "aero/text.h"
#include "aero/artwork.h"

#include <KDirWatch>
#include <KFilePlacesModel>
#include <KBookmark>
#include <KProtocolManager>

#include <QApplication>
#include <QDir>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QEnterEvent>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QInputDialog>
#include <QKeyEvent>
#include <QMenu>
#include <QMetaMethod>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QStyleOptionViewItem>
#include <QScrollBar>
#include <QStandardPaths>
#include <QTimer>
#include <QTreeWidget>
#include <QVariantAnimation>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <algorithm>
#include <functional>

namespace NavigationPaneOrdering {

// Kept as a pure item operation so ordering can be tested without simulating
// Solid devices or modifying the shared Places model.
void pinSystemDrive(QTreeWidgetItem *computer)
{
    const QUrl root = QUrl::fromLocalFile(QStringLiteral("/"));
    for (int i = 1; i < computer->childCount(); ++i) {
        if (computer->child(i)->data(0, Qt::UserRole).toUrl() == root) {
            computer->insertChild(0, computer->takeChild(i));
            return;
        }
    }
}

} // namespace NavigationPaneOrdering

namespace {

constexpr int kSelectionGutter = NavigationItemDelegate::rightGutter;

// How long the expander arrows take to fade in or out, in milliseconds
constexpr int kArrowFadeDuration = 500;

// Only direct children of Favorites can be reordered. The private MIME type
// keeps a shortcut drag out of the filesystem copy/move path.
constexpr int kGroupIdRole = Qt::UserRole + 3;
constexpr int kFavoriteRankRole = Qt::UserRole + 4;
constexpr int kGoogleDriveRole = Qt::UserRole + 5;
const QString kFavoriteMime = QStringLiteral("application/x-explorer-win7-favorite");

bool isFavorite(QTreeWidgetItem *item)
{
    return item && item->parent()
        && item->parent()->data(0, kGroupIdRole).toString() == QLatin1String("favorites");
}

QString favoriteKey(const QUrl &url)
{
    return url.adjusted(QUrl::NormalizePathSegments | QUrl::StripTrailingSlash
                         | QUrl::RemovePassword).toString(QUrl::FullyEncoded);
}

class NavigationEntry final : public QTreeWidgetItem {
public:
    using QTreeWidgetItem::QTreeWidgetItem;

    bool operator<(const QTreeWidgetItem &other) const override
    {
        const QVariant rank = data(0, kFavoriteRankRole);
        const QVariant otherRank = other.data(0, kFavoriteRankRole);
        if (rank.isValid() && otherRank.isValid())
            return rank.toInt() < otherRank.toInt();
        return QTreeWidgetItem::operator<(other);
    }
};

class FavoriteDragHint final : public QWidget {
public:
    explicit FavoriteDragHint(QWidget *parent) : QWidget(parent, Qt::ToolTip)
    {
        setObjectName(QStringLiteral("favoriteDragHint"));
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_ShowWithoutActivating);
        Aero::setPointSize(this, 9);
        hide();
    }

    void setCaption(const QString &caption, bool link)
    {
        m_caption = caption;
        m_link = link;
        setAccessibleName(caption);
        resize(fontMetrics().horizontalAdvance(caption) + 25, fontMetrics().height() + 6);
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), QColor(QStringLiteral("#ffffff")));
        painter.setPen(QColor(QStringLiteral("#767676")));
        painter.drawRect(rect().adjusted(0, 0, -1, -1));
        const int y = height() / 2;
        painter.setPen(QPen(QColor(QStringLiteral("#147bc1")), 2));
        if (m_link) {
            painter.drawLine(5, y + 4, 5, y - 3);
            painter.drawLine(5, y - 3, 13, y - 3);
            painter.drawLine(10, y - 6, 13, y - 3);
            painter.drawLine(10, y, 13, y - 3);
        } else {
            painter.drawLine(4, y, 14, y);
            painter.drawLine(10, y - 4, 14, y);
            painter.drawLine(10, y + 4, 14, y);
        }
        painter.setPen(QColor(QStringLiteral("#0000ff")));
        painter.drawText(rect().adjusted(20, 0, -4, 0), Qt::AlignLeft | Qt::AlignVCenter, m_caption);
    }

private:
    QString m_caption;
    bool m_link = false;
};

class NavigationTree : public QTreeWidget {
    public:
    explicit NavigationTree(QWidget *parent = nullptr)
    : QTreeWidget(parent)
    , m_fade(new QVariantAnimation(this))
    , m_scroll(new QTimer(this))
    , m_hint(new FavoriteDragHint(this))
    {
        m_fade->setDuration(kArrowFadeDuration);
        connect(m_fade, &QVariantAnimation::valueChanged, this,
                [this](const QVariant &value) {
            m_arrowOpacity = value.toReal();
            viewport()->update();
        });
        m_scroll->setInterval(70);
        connect(m_scroll, &QTimer::timeout, this, [this] {
            const int direction = m_dragPos.y() < autoScrollMargin() ? -1
                : m_dragPos.y() >= viewport()->height() - autoScrollMargin() ? 1 : 0;
            QScrollBar *bar = verticalScrollBar();
            const int previous = bar->value();
            bar->setValue(previous + direction * qMax(1, bar->singleStep()));
            if (bar->value() != previous && favoriteDragScrolled)
                favoriteDragScrolled(m_dragPos);
        });
    }

    std::function<void(const QPoint &)> favoriteDragScrolled;

    void scrollFavoriteDrag(const QPoint &pos)
    {
        m_dragPos = pos;
        if (pos.y() < autoScrollMargin() || pos.y() >= viewport()->height() - autoScrollMargin())
            m_scroll->start();
        else
            m_scroll->stop();
    }

    void setFavoriteDropLine(int y)
    {
        if (y < 0) m_scroll->stop();
        if (m_dropLine == y) return;
        m_dropLine = y;
        viewport()->update();
    }

    void showDragFeedback(const QPoint &pos, const QString &caption, bool link,
                          QTreeWidgetItem *target = nullptr)
    {
        m_dropTarget = indexFromItem(target);
        viewport()->update();
        m_hint->setCaption(caption, link);
        m_hint->move(viewport()->mapToGlobal(pos) + QPoint(12, 14));
        m_hint->show();
    }

    void clearDragFeedback()
    {
        setFavoriteDropLine(-1);
        m_dropTarget = QModelIndex();
        m_hint->hide();
        viewport()->update();
    }

    protected:
    QMimeData *mimeData(const QList<QTreeWidgetItem *> &items) const override
    {
        if (items.size() != 1 || !isFavorite(items.first()))
            return nullptr;
        auto *mime = new QMimeData;
        mime->setData(kFavoriteMime, items.first()->data(0, Qt::UserRole).toUrl().toEncoded());
        return mime;
    }

    void startDrag(Qt::DropActions) override
    {
        QTreeWidgetItem *item = currentItem();
        if (!isFavorite(item)) return;
        QDrag drag(this);
        drag.setMimeData(mimeData({item}));
        drag.setPixmap(Aero::shellDragPixmap(item->icon(0), devicePixelRatioF()));
        drag.setHotSpot(Aero::shellDragHotSpot());
        drag.exec(Qt::MoveAction);
        clearDragFeedback();
    }

    void paintEvent(QPaintEvent *event) override
    {
        QTreeWidget::paintEvent(event);
        if (m_dropLine < 0) return;
        QPainter painter(viewport());
        painter.setPen(QPen(Qt::black, 1));
        const int left = 2 * indentation() + 1;
        const int right = viewport()->width() - 8;
        painter.drawLine(left, m_dropLine, right, m_dropLine);
        painter.drawLine(left, m_dropLine - 2, left, m_dropLine + 2);
        painter.drawLine(right, m_dropLine - 2, right, m_dropLine + 2);
    }

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
        QStyleOptionViewItem rowOption(option);
        if (index == m_dropTarget)
            rowOption.state |= QStyle::State_Selected | QStyle::State_Active;
        QTreeWidget::drawRow(painter, rowOption, index);
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
    QTimer *m_scroll = nullptr;
    FavoriteDragHint *m_hint = nullptr;
    QPersistentModelIndex m_dropTarget;
    QPoint m_dragPos;
    qreal m_arrowOpacity = 0.0;
    int m_dropLine = -1;
};

// The stand in child that gives a collapsed folder its expander
constexpr int kPlaceholderRole = Qt::UserRole + 1;

// Absent for the headings and for subfolders found by walking the filesystem
constexpr int kPlaceRowRole = Qt::UserRole + 2;

const QString kFavoriteMetadata = QStringLiteral("explorerWin7Favorite");

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

bool isGoogleDriveMountRoot(const QUrl &url)
{
    if (!url.isLocalFile())
        return false;
    const QUrl mount = Locations::googleDriveMount();
    return mount.isLocalFile()
        && QDir::cleanPath(url.toLocalFile()) == QDir::cleanPath(mount.toLocalFile());
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
    : NavigationPane(GoogleDriveConnection::sharedConnection(), parent)
{
}

NavigationPane::NavigationPane(GoogleDriveConnection *driveConnection, QWidget *parent)
    : QWidget(parent)
    , m_places(new KFilePlacesModel(this))
    , m_driveConnection(driveConnection)
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
    m_tree->setDragEnabled(true);
    m_tree->viewport()->setAcceptDrops(true);
    m_tree->setDropIndicatorShown(true);
    m_tree->viewport()->installEventFilter(this);
    static_cast<NavigationTree *>(m_tree)->favoriteDragScrolled = [this](const QPoint &pos) {
        if (!m_dragMime) {
            static_cast<NavigationTree *>(m_tree)->clearDragFeedback();
            return;
        }
        QDragMoveEvent move(pos, Qt::CopyAction | Qt::MoveAction | Qt::LinkAction,
                            m_dragMime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(m_tree->viewport(), &move);
    };

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
    if (m_driveConnection) {
        connect(m_driveConnection, &GoogleDriveConnection::mountStateChanged,
                this, &NavigationPane::rebuild);
        connect(m_driveConnection, &QObject::destroyed, this, &NavigationPane::refresh);
    }

    root->addWidget(m_tree);
    // Keep open Explorer windows in sync without changing other places groups.
    for (QWidget *widget : QApplication::allWidgets()) {
        auto *pane = qobject_cast<NavigationPane *>(widget);
        if (!pane || pane == this) continue;
        connect(this, &NavigationPane::favoritesOrderChanged, pane, &NavigationPane::applyFavoritesOrder);
        connect(pane, &NavigationPane::favoritesOrderChanged, this, &NavigationPane::applyFavoritesOrder);
    }
    connect(this, &NavigationPane::favoritesOrderChanged, this, &NavigationPane::applyFavoritesOrder);
    rebuild();
}

KFilePlacesModel *NavigationPane::placesModel() const
{
    return m_places;
}

bool NavigationPane::canAddToFavorites(const QUrl &url, bool knownDirectory)
{
    if (!url.isValid() || url.isEmpty() || Locations::isComputer(url)
        || Locations::isLibraries(url) || Locations::isSearch(url)
        || url.scheme() == QLatin1String("trash")
        || url.scheme() == QLatin1String("remote")
        || url.scheme() == QLatin1String("recentlyused"))
        return false;
    // This predicate also sees selected files. Mount membership cannot prove
    // a folder, and a synchronous stat can stall on a dead FUSE mount.
    if (Locations::isGoogleDriveMount(url))
        return false;
    if (url.isLocalFile())
        return knownDirectory || QFileInfo(url.toLocalFile()).isDir();
    return KProtocolManager::supportsListing(url);
}

void NavigationPane::addToFavorites(const QUrl &url, const QString &name)
{
    if (!canAddToFavorites(url))
        return;

    const QUrl folder = url.adjusted(QUrl::NormalizePathSegments | QUrl::StripTrailingSlash
                                     | QUrl::RemovePassword);
    const auto findPlace = [this, &folder] {
        for (int row = 0; row < m_places->rowCount(); ++row) {
            const QModelIndex index = m_places->index(row, 0);
            if (!m_places->isDevice(index)
                && m_places->url(index).adjusted(QUrl::NormalizePathSegments | QUrl::RemovePassword)
                    .matches(folder, QUrl::StripTrailingSlash))
                return index;
        }
        return QModelIndex();
    };
    QModelIndex index = findPlace();
    if (!index.isValid()) {
        QString label = name.isEmpty() ? folder.fileName() : name;
        if (label.isEmpty())
            label = folder.toDisplayString(QUrl::PreferLocalFile);
        m_places->addPlace(label, folder, QStringLiteral("folder"));
        index = findPlace();
    }
    if (!index.isValid())
        return;

    // Explicit favorites can include Home, libraries and network folders,
    // whose default places otherwise belong in other groups or stay hidden.
    KBookmark bookmark = m_places->bookmarkForIndex(index);
    bookmark.setMetaDataItem(kFavoriteMetadata, QStringLiteral("true"));
    m_places->setGroupHidden(m_places->groupType(index), false);
    index = findPlace();
    m_places->setPlaceHidden(index, false);
    m_places->refresh(); // saves bookmark metadata and notifies other windows
    Settings::setNavigationGroupExpanded(QStringLiteral("favorites"), true);
    refresh();
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
    signature << QStringLiteral("favoritesOrder") << Settings::favoritesOrder();
    signature << QStringLiteral("googleDriveMounted")
              << QString::number(m_driveConnection && m_driveConnection->isConnected());
    signature.reserve(m_places->rowCount());
    for (int row = 0; row < m_places->rowCount(); ++row) {
        const QModelIndex index = m_places->index(row, 0);
        // The model's own label rather than the drive one, which asks Solid and
        // is far too expensive to run on every data change
        signature << QStringLiteral("%1|%2|%3|%4|%5|%6")
                         .arg(m_places->url(index).toString(),
                              m_places->text(index),
                              QString::number(int(m_places->groupType(index))),
                              QString::number(m_places->isHidden(index)),
                              QString::number(m_places->setupNeeded(index)),
                              m_places->bookmarkForIndex(index).metaDataItem(kFavoriteMetadata));
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
    QTreeWidgetItem *match = nullptr;
    for (QTreeWidgetItemIterator it(m_tree); *it; ++it) {
        if (!urlForItem(*it).matches(url, QUrl::StripTrailingSlash))
            continue;
        if ((*it)->data(0, kGoogleDriveRole).toBool())
            return *it;
        if (!match)
            match = *it;
    }
    return match;
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

    auto *item = new NavigationEntry(group);
    // One call covers every kind of place
    item->setText(0, Branding::displayName(url, DriveLabel::forPlace(m_places, placeIndex)));
    item->setIcon(0, m_places->icon(placeIndex));
    item->setData(0, Qt::UserRole, url);
    item->setData(0, kPlaceRowRole, placeIndex.row());
    item->setSizeHint(0, QSize(0, 21));
    // A mounted device is a directory by definition, and stat'ing a slow or
    // stalled one here would block the GUI thread for the whole pane
    addPlaceholderIfExpandable(item, url, m_places->isDevice(placeIndex));
}

void NavigationPane::addPlaceholderIfExpandable(QTreeWidgetItem *item, const QUrl &url,
                                                bool knownDirectory)
{
    // Local directories only, anything remote meaning a job per node
    if (!url.isLocalFile() || Locations::isGoogleDriveMount(url))
        return;

    if (!knownDirectory) {
        const QFileInfo info(url.toLocalFile());
        if (!info.isDir() || !info.isReadable())
            return;
    }

    addPlaceholder(item);
}

void NavigationPane::populateChildren(QTreeWidgetItem *item)
{
    // Nothing to do unless the only child is the stand in
    if (!hasOnlyPlaceholder(item))
        return;

    const QUrl url = urlForItem(item);
    if (!url.isLocalFile() || Locations::isGoogleDriveMount(url)
        || m_scanning.contains(url))
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
        // The scan listed only directories, so no stat per child
        addPlaceholderIfExpandable(child, childUrl, true);

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
    static_cast<NavigationTree *>(m_tree)->clearDragFeedback();

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

    // This is an app-owned drive, independent of the optional shared Places
    // bookmark. Its fixed identity never needs a filesystem or Solid query.
    if (m_driveConnection && m_driveConnection->isConnected()) {
        auto *drive = new QTreeWidgetItem(computer);
        drive->setText(0, tr("Google Drive (G:)"));
        drive->setIcon(0, Aero::googleDriveIcon());
        drive->setData(0, Qt::UserRole, Locations::googleDriveMount());
        drive->setData(0, kPlaceRowRole, -1);
        drive->setData(0, kGoogleDriveRole, true);
        drive->setSizeHint(0, QSize(0, 21));
    }

    bool networkDriveAdded = false;

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
        // A saved mount-root bookmark remains an optional Network alias. The
        // virtual Computer entry above must never depend on that shared file.
        if (m_places->bookmarkForIndex(index).metaDataItem(kFavoriteMetadata) == QLatin1String("true")) {
            group = favorites;
        } else if (isGoogleDriveMountRoot(placeUrl)) {
            if (networkDriveAdded)
                continue;
            networkDriveAdded = true;
            group = network;
        } else {
            switch (m_places->groupType(index)) {
            case KFilePlacesModel::PlacesType: {
                // Keep saved Drive accounts under Network even if their optional
                // worker was removed since the bookmark was created.
                if (placeUrl.scheme() == QLatin1String("gdrive")) {
                    group = network;
                    break;
                }
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
        }
        addEntry(group, index);
    }

    // Reorder only this view's constructed children. Shared Places ordering,
    // Favorites ranks, bookmark metadata, and the other drives stay intact.
    NavigationPaneOrdering::pinSystemDrive(computer);

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
            item = new NavigationEntry;
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

    // Native defaults lead until the user arranges Favorites. Unlisted/new
    // favorites follow the saved ones in their existing order.
    const QStringList order = Settings::favoritesOrder();
    if (!order.isEmpty()) {
        QHash<QString, int> positions;
        for (int i = 0; i < order.size(); ++i)
            positions.insert(order.at(i), i);
        QList<QTreeWidgetItem *> items = favorites->takeChildren();
        std::stable_sort(items.begin(), items.end(), [&](auto *left, auto *right) {
            return positions.value(favoriteKey(urlForItem(left)), order.size())
                < positions.value(favoriteKey(urlForItem(right)), order.size());
        });
        favorites->addChildren(items);
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
        if (group == favorites || group->childCount() > 0 || group->data(0, Qt::UserRole).toUrl().isValid()) {
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

    // When both groups are open, the managed drive is the canonical root;
    // choosing its optional Network bookmark must not steal the highlight.
    QTreeWidgetItem *drive = itemForUrl(Locations::googleDriveMount());
    if (drive && drive->data(0, kGoogleDriveRole).toBool()
        && isItemVisible(drive)
        && urlForItem(drive).matches(m_currentUrl, QUrl::StripTrailingSlash))
        return drive;

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

        if (url.scheme() == QLatin1String("gdrive") && url.isParentOf(m_currentUrl)
            && url.path().size() > bestLength) {
            bestLength = url.path().size();
            best = *it;
        }

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

int NavigationPane::favoriteDropIndex(const QPoint &pos, int *lineY) const
{
    QTreeWidgetItem *favorites = nullptr;
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = m_tree->topLevelItem(i);
        if (item->data(0, kGroupIdRole).toString() == QLatin1String("favorites")) {
            favorites = item;
            break;
        }
    }
    if (!favorites) return -1;
    QTreeWidgetItem *target = m_tree->itemAt(pos);
    if (target == favorites) {
        // Windows highlights the heading and appends, without an insert line.
        return favorites->childCount();
    }
    // The spacer immediately below Favorites is an append target, too.
    const bool append = target && target->flags() == Qt::NoItemFlags && favorites->isExpanded()
        && m_tree->indexOfTopLevelItem(target) == m_tree->indexOfTopLevelItem(favorites) + 1
        && favorites->childCount() > 0;
    if (append)
        target = favorites->child(favorites->childCount() - 1);
    QTreeWidgetItem *root = target;
    while (root && root->parent() && root->parent() != favorites)
        root = root->parent();
    if (!root || root->parent() != favorites) return -1;

    const QRect rect = m_tree->visualItemRect(root);
    constexpr int insertZone = 4;
    QTreeWidgetItem *last = root;
    while (last->isExpanded() && last->childCount() > 0)
        last = last->child(last->childCount() - 1);
    const QRect lastRect = m_tree->visualItemRect(last);
    const bool before = target == root && pos.y() < rect.top() + insertZone;
    const bool after = append || (target == last && pos.y() >= lastRect.bottom() - insertZone + 1);
    if (!before && !after) return -1;
    int y = rect.top();
    if (after) {
        y = lastRect.bottom() + 1;
    }
    if (lineY) *lineY = y;
    return favorites->indexOfChild(root) + (after ? 1 : 0);
}

void NavigationPane::rememberDrag(const QMimeData *mime)
{
    if (m_dragMime == mime) return;
    m_dragMime = mime;
    m_dragFolders.clear();
    const QList<QUrl> urls = mime->urls();
    if (urls.isEmpty()) return;
    const QList<QUrl> knownFolders = FolderDragData::folders(mime);
    for (const QUrl &url : urls) {
        const bool folder = knownFolders.contains(url)
            || (url.isLocalFile() && !Locations::isGoogleDriveMount(url)
                && QFileInfo(url.toLocalFile()).isDir());
        if (!folder || !canAddToFavorites(url, folder)) {
            m_dragFolders.clear();
            return;
        }
        if (!m_dragFolders.contains(url)) m_dragFolders.append(url);
    }
}

bool NavigationPane::addDroppedFavorites(QList<QUrl> urls, int insertionIndex)
{
    if (urls.isEmpty() || insertionIndex < 0) return false;
    QStringList order;
    for (QTreeWidgetItemIterator it(m_tree); *it; ++it)
        if (isFavorite(*it)) order.append(favoriteKey(urlForItem(*it)));
    int position = qMin(insertionIndex, int(order.size()));
    for (const QUrl &url : urls) {
        const QString key = favoriteKey(url);
        const int previous = order.indexOf(key);
        if (previous >= 0) {
            order.removeAt(previous);
            if (previous < position) --position;
        }
        order.insert(position++, key);
        addToFavorites(url);
    }
    for (const QString &key : Settings::favoritesOrder())
        if (!order.contains(key)) order.append(key);
    Settings::setFavoritesOrder(order);
    Q_EMIT favoritesOrderChanged();
    return true;
}

bool NavigationPane::moveFavorite(const QUrl &url, int insertionIndex)
{
    QTreeWidgetItem *item = nullptr;
    for (QTreeWidgetItemIterator it(m_tree); *it; ++it) {
        if (isFavorite(*it) && favoriteKey(urlForItem(*it)) == favoriteKey(url)) {
            item = *it;
            break;
        }
    }
    if (!item || insertionIndex < 0) return false;
    QTreeWidgetItem *favorites = item->parent();
    const int old = favorites->indexOfChild(item);
    int destination = qMin(insertionIndex, favorites->childCount());
    if (old < destination) --destination;
    if (old == destination) return true;
    QStringList order;
    for (int i = 0; i < favorites->childCount(); ++i)
        order.append(favoriteKey(urlForItem(favorites->child(i))));
    order.move(old, destination);
    // Keep hidden favorites in the saved list as well.
    for (const QString &key : Settings::favoritesOrder())
        if (!order.contains(key)) order.append(key);
    Settings::setFavoritesOrder(order);
    Q_EMIT favoritesOrderChanged();
    return true;
}

void NavigationPane::applyFavoritesOrder()
{
    const QStringList order = Settings::favoritesOrder();
    QHash<QString, int> positions;
    for (int i = 0; i < order.size(); ++i)
        positions.insert(order.at(i), i);
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        auto *favorites = m_tree->topLevelItem(i);
        if (favorites->data(0, kGroupIdRole).toString() != QLatin1String("favorites")) continue;
        for (int row = 0; row < favorites->childCount(); ++row) {
            QTreeWidgetItem *item = favorites->child(row);
            item->setData(0, kFavoriteRankRole,
                positions.value(favoriteKey(urlForItem(item)), order.size() + row));
        }
        // Sorting emits a layout change instead of removing/recreating rows,
        // retaining selection, expanded branches and in-flight directory scans.
        const int scroll = m_tree->verticalScrollBar()->value();
        favorites->sortChildren(0, Qt::AscendingOrder);
        m_tree->verticalScrollBar()->setValue(scroll);
        break;
    }
    m_placesSignature = placesSignature();
}

void NavigationPane::showContextMenu(const QPoint &pos)
{
    QTreeWidgetItem *item = m_tree->itemAt(pos);
    if (!item || item->flags() == Qt::NoItemFlags)
        return;

    const QUrl url = urlForItem(item);
    const bool favoriteRow = isFavorite(item);
    const int placeRow = item->data(0, kPlaceRowRole).toInt();
    QModelIndex placeIndex;
    if (placeRow >= 0)
        placeIndex = m_places->index(placeRow, 0);

    QMenu menu(this);
    // The local-only chooser shares this pane but handles none of the window
    // commands. Offer each only in a host that can perform it.
    const auto handled = [this](auto signal) {
        return isSignalConnected(QMetaMethod::fromSignal(signal));
    };

    if (item->data(0, kGroupIdRole).toString() == QLatin1String("favorites")) {
        const QUrl current = m_currentUrl;
        QAction *add = menu.addAction(tr("Add current location to Favorites"), this,
                                     [this, current] { addToFavorites(current); });
        add->setEnabled(canAddToFavorites(current));
        menu.addSeparator();
    }

    if (url.isValid()) {
        menu.addAction(tr("Open"), this, [this, url] { Q_EMIT urlActivated(url); });
        if (item->data(0, kGoogleDriveRole).toBool() && handled(&NavigationPane::newWindowRequested))
            menu.addAction(tr("Open in new window"), this,
                           [this, url] { Q_EMIT newWindowRequested(url); });
        menu.addSeparator();
    }

    if (url.scheme() == QLatin1String("trash") && handled(&NavigationPane::emptyTrashRequested)) {
        menu.addAction(tr("Empty Recycle Bin"), this,
                       [this] { Q_EMIT emptyTrashRequested(); });
        menu.addSeparator();
    }

    if (Locations::isComputer(url) && handled(&NavigationPane::connectDrivesRequested)) {
        menu.addAction(tr("Connect drives..."), this,
                       [this] { Q_EMIT connectDrivesRequested(); });
        menu.addSeparator();
    }

    if (url.scheme() == QLatin1String("remote")
        && (handled(&NavigationPane::mapDriveRequested) || handled(&NavigationPane::googleDriveRequested))) {
        if (handled(&NavigationPane::mapDriveRequested))
            menu.addAction(tr("Map network drive..."), this,
                           [this] { Q_EMIT mapDriveRequested(); });
        if (handled(&NavigationPane::googleDriveRequested))
            menu.addAction(tr("Connect Google Drive..."), this,
                           [this] { Q_EMIT googleDriveRequested(); });
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

        if (!favoriteRow) {
            menu.addSeparator();
            QAction *up = menu.addAction(tr("Move up"));
            up->setEnabled(placeRow > 0);
            connect(up, &QAction::triggered, this,
                    [this, placeRow] { m_places->movePlace(placeRow, placeRow - 1); });
            QAction *down = menu.addAction(tr("Move down"));
            down->setEnabled(placeRow < m_places->rowCount() - 1);
            connect(down, &QAction::triggered, this,
                    [this, placeRow] { m_places->movePlace(placeRow, placeRow + 2); });
        }
        menu.addSeparator();
    }
    if (favoriteRow) {
        const int row = item->parent()->indexOfChild(item);
        QAction *up = menu.addAction(tr("Move up"), this,
                                     [this, url, row] { moveFavorite(url, row - 1); });
        up->setEnabled(row > 0);
        QAction *down = menu.addAction(tr("Move down"), this,
                                       [this, url, row] { moveFavorite(url, row + 2); });
        down->setEnabled(row < item->parent()->childCount() - 1);
        menu.addSeparator();
    }
    if (canAddToFavorites(url) && (!placeIndex.isValid() || !favoriteRow)) {
        const QString name = item->text(0);
        menu.addAction(tr("Add to Favorites"), this, [this, url, name] {
            addToFavorites(url, name);
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

    Aero::setPointSize(&menu, 9);
    Aero::styleContextMenu(&menu);
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
        rememberDrag(drag->mimeData());
        if (drag->mimeData()->hasFormat(kFavoriteMime)) {
            drag->setDropAction(Qt::MoveAction);
            drag->accept();
            return true;
        }
        if (drag->mimeData()->hasUrls()) {
            drag->acceptProposedAction();
            return true;
        }
        break;
    }
    case QEvent::DragMove: {
        auto *drag = static_cast<QDragMoveEvent *>(event);
        const QPoint pos = drag->position().toPoint();
        rememberDrag(drag->mimeData());
        auto *tree = static_cast<NavigationTree *>(m_tree);
        int lineY = -1;
        const int index = favoriteDropIndex(pos, &lineY);
        QTreeWidgetItem *under = m_tree->itemAt(pos);
        if (drag->mimeData()->hasFormat(kFavoriteMime)) {
            if (index >= 0) {
                drag->setDropAction(Qt::MoveAction);
                drag->accept();
                tree->setFavoriteDropLine(lineY);
                tree->showDragFeedback(pos, tr("Move here"), false, lineY < 0 ? under : nullptr);
                tree->scrollFavoriteDrag(pos);
            } else {
                drag->ignore();
                tree->clearDragFeedback();
                if (isFavorite(under)) tree->scrollFavoriteDrag(pos);
            }
            return true;
        }
        if (index >= 0) {
            // Inserting/heading drops create bookmarks. Returning Link (or
            // Copy to an older source) never authorizes deleting source files.
            const Qt::DropAction action = drag->possibleActions().testFlag(Qt::LinkAction)
                ? Qt::LinkAction : drag->possibleActions().testFlag(Qt::CopyAction)
                    ? Qt::CopyAction : Qt::IgnoreAction;
            if (!m_dragFolders.isEmpty() && action != Qt::IgnoreAction) {
                drag->setDropAction(action);
                drag->accept();
                tree->setFavoriteDropLine(lineY);
                tree->showDragFeedback(pos, tr("Create link in Favorites"), true, lineY < 0 ? under : nullptr);
                tree->scrollFavoriteDrag(pos);
            } else {
                drag->ignore();
                tree->clearDragFeedback();
            }
            return true;
        }
        tree->setFavoriteDropLine(-1);
        const QUrl target = targetAt(pos);
        if (drag->mimeData()->hasUrls() && target.isValid()) {
            drag->acceptProposedAction();
            const QString text = drag->dropAction() == Qt::CopyAction ? tr("Copy to %1")
                : drag->dropAction() == Qt::LinkAction ? tr("Create link in %1") : tr("Move to %1");
            tree->showDragFeedback(pos, text.arg(under->text(0)), drag->dropAction() == Qt::LinkAction, under);
        } else {
            drag->ignore();
            tree->clearDragFeedback();
        }
        return true;
    }
    case QEvent::Drop: {
        auto *drop = static_cast<QDropEvent *>(event);
        static_cast<NavigationTree *>(m_tree)->clearDragFeedback();
        rememberDrag(drop->mimeData());
        const int index = favoriteDropIndex(drop->position().toPoint());
        if (drop->mimeData()->hasFormat(kFavoriteMime)) {
            const QUrl url = QUrl::fromEncoded(drop->mimeData()->data(kFavoriteMime));
            if (moveFavorite(url, index)) {
                drop->setDropAction(Qt::MoveAction);
                drop->accept();
            } else {
                drop->ignore();
            }
            m_dragMime.clear();
            m_dragFolders.clear();
            return true;
        }
        if (index >= 0) {
            const Qt::DropAction action = drop->possibleActions().testFlag(Qt::LinkAction)
                ? Qt::LinkAction : drop->possibleActions().testFlag(Qt::CopyAction)
                    ? Qt::CopyAction : Qt::IgnoreAction;
            if (action != Qt::IgnoreAction && addDroppedFavorites(m_dragFolders, index)) {
                drop->setDropAction(action);
                drop->accept();
            } else {
                drop->ignore();
            }
            m_dragMime.clear();
            m_dragFolders.clear();
            return true;
        }
        const QUrl target = targetAt(drop->position().toPoint());
        if (drop->mimeData()->hasUrls() && target.isValid()) {
            Q_EMIT dropped(drop, target);
            drop->acceptProposedAction();
        }
        m_dragMime.clear();
        m_dragFolders.clear();
        // Consumed either way, or the tree makes rows out of the dropped files
        return true;
    }
    case QEvent::DragLeave:
        static_cast<NavigationTree *>(m_tree)->clearDragFeedback();
        m_dragMime.clear();
        m_dragFolders.clear();
        break;
    default:
        break;
    }

    return QWidget::eventFilter(watched, event);
}
