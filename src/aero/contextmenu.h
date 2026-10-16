#pragma once

class QMenu;

namespace Aero {
// Apply Explorer's popup geometry to this menu and its submenus. QAction and
// QMenu still handle activation, keyboard navigation, and desktop integrations.
void styleContextMenu(QMenu *menu, bool submenu = false);
}
