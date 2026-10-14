#include "listview.h"

#include "text.h"

#include <QAbstractItemView>
#include <QFontMetrics>
#include <QFrame>
#include <QHeaderView>
#include <QTreeView>

namespace Aero {

void configureListTree(QTreeView *tree)
{
    tree->setRootIsDecorated(false);
    tree->setItemsExpandable(false);
    tree->setIndentation(0);
    tree->setUniformRowHeights(true);
    tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    tree->setSelectionBehavior(QAbstractItemView::SelectRows);
    // Qt's default would turn a double click into an inline rename instead of
    // opening the item, and renaming is left to the application on F2
    tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tree->setAlternatingRowColors(false);
    tree->setFrameShape(QFrame::NoFrame);
    // Before sorting is enabled, which immediately re sorts by the indicator,
    // and the indicator starts out descending
    tree->header()->setSortIndicator(0, Qt::AscendingOrder);
    tree->setSortingEnabled(true);
    tree->header()->setDefaultAlignment(Qt::AlignLeft);
    tree->header()->setStretchLastSection(true);
    setPointSize(tree->header(), 9);
    // Keep the platform header's stateful paint (hover, press, sort arrow,
    // and resize handle) instead of flattening every section to one color.
    // Pinned rather than left to the size hint, which is cached before the app
    // wide stylesheet polishes the widget and comes out twice as tall
    const QFontMetrics metrics(tree->header()->font());
    const int padding = 4;
    tree->header()->setFixedHeight(metrics.height() + 2 * padding + 1);
}

} // namespace Aero
