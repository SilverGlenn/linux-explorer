#pragma once

// The Windows 7 column list, a flat borderless tree used as a table

class QTreeView;
class QModelIndex;
class QStyleOptionViewItem;

namespace Aero {

// Styling is scoped to the header, so the body keeps the real Qt style
void configureListTree(QTreeView *tree);

// Qt can supply logical rather than visual column edges after sections move.
void correctRowSelectionPosition(QStyleOptionViewItem &option, const QModelIndex &index);

} // namespace Aero
