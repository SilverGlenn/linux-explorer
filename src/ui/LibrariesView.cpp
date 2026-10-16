#include "LibrariesView.h"
#include "Libraries.h"
#include "aero/icons.h"
#include "aero/contextmenu.h"
#include "aero/listview.h"
#include "aero/palette.h"
#include "aero/text.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QFrame>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QLabel>
#include <QListView>
#include <QMenu>
#include <QPainter>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QTimer>
#include <QTreeView>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

namespace {
constexpr int kTileWidth = 256;
// The Libraries shell tiles have more row spacing than ordinary file tiles.
constexpr int kTileHeight = 64;

class LibraryDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    bool tiles = true;
    bool content = false;
    int rowWidth = 256;
    int tileWidth = kTileWidth;

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        if (tiles) return QSize(tileWidth, kTileHeight);
        if (content) return QSize(rowWidth, 50);
        return QStyledItemDelegate::sizeHint(option, index);
    }

    void paint(QPainter *p, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        if (!tiles && !content) {
            QStyledItemDelegate::paint(p, option, index);
            return;
        }
        QStyleOptionViewItem background(option);
        initStyleOption(&background, index);
        background.text.clear();
        background.icon = {};
        background.features &= ~QStyleOptionViewItem::HasDecoration;
        const QWidget *widget = option.widget;
        QStyle *style = widget ? widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &background, p, widget);

        const int iconSize = content ? 32 : 48;
        const QRect iconRect(option.rect.left() + 4,
                             option.rect.center().y() - iconSize / 2,
                             iconSize, iconSize);
        qvariant_cast<QIcon>(index.data(Qt::DecorationRole)).paint(p, iconRect);
        const QRect textRect = option.rect.adjusted(iconSize + 12, 0, -4, 0);
        const int line = option.fontMetrics.height();
        p->save();
        p->setFont(option.font);
        const bool selected = option.state & QStyle::State_Selected;
        p->setPen(selected ? option.palette.color(QPalette::HighlightedText)
                           : Aero::Palette::rgb(Aero::Palette::Text));
        p->drawText(QRect(textRect.left(), textRect.center().y() - line,
                          textRect.width(), line), Qt::AlignVCenter,
                    option.fontMetrics.elidedText(index.data().toString(), Qt::ElideRight,
                                                  textRect.width()));
        p->setPen(selected ? option.palette.color(QPalette::HighlightedText)
                           : Aero::Palette::rgb(Aero::Palette::MutedText));
        p->drawText(QRect(textRect.left(), textRect.center().y(), textRect.width(), line),
                    Qt::AlignVCenter, QObject::tr("Library"));
        p->restore();
    }
};
} // namespace

LibrariesView::LibrariesView(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("librariesPage"));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(18, 8, 8, 0);
    layout->setSpacing(0);
    auto *title = Aero::label(tr("Libraries"), 14, "#006600");
    title->setObjectName(QStringLiteral("librariesHeading"));
    layout->addWidget(title);
    auto *description = Aero::label(
        tr("Open a library to see your files and arrange them by folder, date, and other properties."),
        9, Aero::Palette::LinkText);
    description->setMinimumWidth(0);
    description->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    description->setToolTip(description->text());
    layout->addWidget(description);
    layout->addSpacing(8);
    auto *rule = new QFrame;
    rule->setObjectName(QStringLiteral("librariesRule"));
    rule->setFixedHeight(1);
    rule->setStyleSheet(QStringLiteral("background: #ECF0F5; border: none;"));
    layout->addWidget(rule);
    layout->addSpacing(4);

    m_model = new QStandardItemModel(this);
    m_model->setHorizontalHeaderLabels({tr("Name"), tr("Date modified"), tr("Type"), tr("Size")});
    m_list = new QListView(this);
    m_list->setObjectName(QStringLiteral("libraryList"));
    m_list->setModel(m_model);
    m_list->setItemDelegate(new LibraryDelegate(m_list));
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setMovement(QListView::Static);
    m_list->setResizeMode(QListView::Adjust);
    Aero::setPointSize(m_list, 9);
    m_list->setAccessibleName(tr("Libraries"));

    m_details = new QTreeView(this);
    m_details->setObjectName(QStringLiteral("libraryDetails"));
    m_details->setAccessibleName(tr("Libraries, details"));
    m_details->setModel(m_model);
    Aero::setPointSize(m_details, 9);
    Aero::configureListTree(m_details);
    m_details->setSelectionMode(QAbstractItemView::SingleSelection);
    m_details->setSelectionModel(m_list->selectionModel());
    m_details->header()->setSectionsMovable(true);
    m_details->header()->setStretchLastSection(false);
    m_details->header()->setMinimumSectionSize(32);
    m_details->setColumnWidth(0, 240);
    m_details->setColumnWidth(1, 140);
    m_details->setColumnWidth(2, 120);
    m_details->setColumnWidth(3, 80);

    const QList<QAbstractItemView *> views{m_list, m_details};
    for (QAbstractItemView *view : views) {
        view->installEventFilter(this);
        view->viewport()->installEventFilter(this);
        view->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(view, &QAbstractItemView::doubleClicked, this, [this] {
            if (!Settings::singleClickToOpen()) activateCurrent();
        });
        connect(view, &QAbstractItemView::clicked, this, [this] {
            if (Settings::singleClickToOpen()) activateCurrent();
        });
        connect(view, &QWidget::customContextMenuRequested, this,
                [this, view](const QPoint &pos) { showItemContextMenu(view, pos); });
    }
    connect(m_list->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, &LibrariesView::selectionChanged);
    setFocusPolicy(Qt::StrongFocus);
    setFocusProxy(m_list);
    m_views = new QStackedWidget(this);
    m_views->addWidget(m_list);
    m_views->addWidget(m_details);
    layout->addWidget(m_views, 1);
    refresh();
    applyMode();
}

void LibrariesView::refresh()
{
    const QUrl selected = selectedUrl();
    QSignalBlocker blocker(m_list->selectionModel());
    // Keep the header state and announce the completed refresh only once.
    // Otherwise a selection reset makes the window's footer stick at 0 items.
    m_model->setRowCount(0);
    for (const auto &entry : Libraries::entries()) {
        auto *item = new QStandardItem(QIcon::fromTheme(entry.iconName,
                                                       Aero::themeIcon({"folder"})), entry.name);
        item->setData(entry.url, Qt::UserRole);
        item->setData(tr("Library for %1").arg(entry.url.toLocalFile()), Qt::AccessibleDescriptionRole);
        item->setToolTip(entry.url.toDisplayString(QUrl::PreferLocalFile));
        // These describe shell shortcuts, not the backing folders' timestamps
        // or sizes. Do not fabricate metadata for the synthetic objects.
        QList<QStandardItem *> row{item, new QStandardItem,
                                  new QStandardItem(tr("Library")), new QStandardItem};
        for (QStandardItem *cell : row)
            cell->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
        m_model->appendRow(row);
    }
    m_model->sort(m_details->header()->sortIndicatorSection(),
                  m_details->header()->sortIndicatorOrder());
    for (int row = 0; selected.isValid() && row < m_model->rowCount(); ++row) {
        const QModelIndex index = m_model->index(row, 0);
        if (index.data(Qt::UserRole).toUrl() == selected) {
            m_list->selectionModel()->setCurrentIndex(index,
                QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
            break;
        }
    }
    updateGrid();
    Q_EMIT selectionChanged();
}

int LibrariesView::libraryCount() const { return m_model->rowCount(); }
QUrl LibrariesView::selectedUrl() const
{
    return m_list->selectionModel()->hasSelection()
        ? m_list->currentIndex().siblingAtColumn(0).data(Qt::UserRole).toUrl() : QUrl();
}
QString LibrariesView::selectedName() const
{
    return selectedUrl().isValid()
        ? m_list->currentIndex().siblingAtColumn(0).data().toString() : QString();
}
void LibrariesView::activateCurrent()
{
    const QUrl url = selectedUrl();
    if (url.isValid()) Q_EMIT urlActivated(url);
}

void LibrariesView::showItemContextMenu(QAbstractItemView *view, const QPoint &pos)
{
    const QModelIndex index = view->indexAt(pos);
    if (!index.isValid()) return;
    view->setCurrentIndex(index);
    const QUrl target = selectedUrl();
    QMenu menu(this);
    menu.setDefaultAction(menu.addAction(tr("&Open"), this,
                          [this, target] { Q_EMIT urlActivated(target); }));
    menu.addAction(tr("Open in &new window"), this,
                   [this, target] { Q_EMIT newWindowRequested(target); });
    Aero::setPointSize(&menu, 9);
    Aero::styleContextMenu(&menu);
    menu.exec(view->viewport()->mapToGlobal(pos));
}

bool LibrariesView::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_list->viewport() && event->type() == QEvent::Resize)
        updateGrid();
    if (event->type() == QEvent::ContextMenu) {
        const auto *context = static_cast<QContextMenuEvent *>(event);
        if (context->reason() == QContextMenuEvent::Keyboard) {
            QAbstractItemView *view = m_views->currentWidget() == m_details
                ? static_cast<QAbstractItemView *>(m_details) : m_list;
            if (watched == view || watched == view->viewport()) {
                const QModelIndex index = view->currentIndex().siblingAtColumn(0);
                if (index.isValid())
                    showItemContextMenu(view, view->visualRect(index).center());
                return true;
            }
        }
    }
    if ((watched == m_list || watched == m_details) && event->type() == QEvent::KeyPress) {
        const auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            activateCurrent();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void LibrariesView::setViewMode(Settings::ViewMode mode)
{
    if (mode == m_mode) return;
    m_mode = mode;
    applyMode();
    Q_EMIT viewModeChanged(mode);
}

void LibrariesView::applyMode()
{
    auto *delegate = static_cast<LibraryDelegate *>(m_list->itemDelegate());
    delegate->tiles = m_mode == Settings::ViewMode::Tiles;
    delegate->content = m_mode == Settings::ViewMode::Content;
    delegate->rowWidth = m_list->viewport()->width();
    if (m_mode == Settings::ViewMode::Details) {
        if (m_list->selectionModel()->hasSelection())
            m_list->selectionModel()->select(m_list->currentIndex().siblingAtColumn(0),
                QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        m_views->setCurrentWidget(m_details);
        setFocusProxy(m_details);
        return;
    }
    m_views->setCurrentWidget(m_list);
    setFocusProxy(m_list);
    if (m_mode != Settings::ViewMode::Tiles)
        m_list->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    const int icon = Settings::iconSizeFor(m_mode);
    m_list->setIconSize(QSize(icon, icon));
    m_list->setSpacing(0);
    if (delegate->tiles) {
        m_list->setViewMode(QListView::IconMode);
        m_list->setFlow(QListView::LeftToRight);
        m_list->setWrapping(true);
        m_list->setGridSize(QSize(kTileWidth, kTileHeight));
    } else if (delegate->content) {
        m_list->setViewMode(QListView::ListMode);
        m_list->setFlow(QListView::TopToBottom);
        m_list->setWrapping(false);
        m_list->setGridSize(QSize(m_list->viewport()->width(), delegate->content ? 50 : 22));
    } else if (m_mode == Settings::ViewMode::List || m_mode == Settings::ViewMode::SmallIcons) {
        m_list->setViewMode(QListView::ListMode);
        m_list->setFlow(m_mode == Settings::ViewMode::List
                       ? QListView::TopToBottom : QListView::LeftToRight);
        m_list->setWrapping(true);
        m_list->setGridSize(QSize(160, 22));
    } else {
        m_list->setViewMode(QListView::IconMode);
        m_list->setFlow(QListView::LeftToRight);
        m_list->setWrapping(true);
        m_list->setGridSize(QSize(icon + 32, icon + 40));
    }
    m_list->setWordWrap(!delegate->tiles && m_list->viewMode() == QListView::IconMode);
    updateGrid();
    m_list->doItemsLayout();
    m_list->viewport()->update();
}

void LibrariesView::updateGrid()
{
    auto *delegate = static_cast<LibraryDelegate *>(m_list->itemDelegate());
    const int width = qMax(1, m_list->viewport()->width());
    delegate->rowWidth = width;
    if (m_mode == Settings::ViewMode::Tiles) {
        // At the captured guest's width, a strict 256 px grid incorrectly
        // wraps Music below Documents. Allow slightly narrower cells, as the
        // shell does, while preserving the nominal spacing on larger windows.
        int layoutWidth = qMax(1, m_list->maximumViewportSize().width());
        int columns = qMax(1, qRound(layoutWidth / double(kTileWidth)));
        const int rows = (libraryCount() + columns - 1) / columns;
        const bool fits = rows * kTileHeight <= m_list->maximumViewportSize().height();
        // QListView otherwise reserves a scrollbar column even when no bar is
        // needed. That makes the guest-sized page wrap incorrectly in Kvantum.
        m_list->setVerticalScrollBarPolicy(fits ? Qt::ScrollBarAlwaysOff : Qt::ScrollBarAsNeeded);
        if (!fits) {
            const QStyle *style = m_list->style();
            if (!style->pixelMetric(QStyle::PM_ScrollView_ScrollBarOverlap))
                layoutWidth -= style->pixelMetric(QStyle::PM_ScrollBarExtent)
                    + (style->styleHint(QStyle::SH_ScrollView_FrameOnlyAroundContents)
                       ? 2 * style->pixelMetric(QStyle::PM_DefaultFrameWidth) : 0);
            layoutWidth = qMax(1, layoutWidth);
            columns = qMax(1, qRound(layoutWidth / double(kTileWidth)));
        }
        delegate->tileWidth = qMin(kTileWidth, qMax(1, (layoutWidth - 2) / columns));
        const QSize grid(delegate->tileWidth, kTileHeight);
        if (m_list->gridSize() != grid) {
            m_list->setGridSize(grid);
            // Relayout after the viewport's resize handler finishes; doing
            // it inside that handler leaves scrollbar extents stale.
            QTimer::singleShot(0, m_list, [this] { m_list->doItemsLayout(); });
        }
    } else if (m_mode == Settings::ViewMode::Content) {
        m_list->setGridSize(QSize(width, 50));
    }
}
