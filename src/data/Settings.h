#pragma once

#include <QByteArray>
#include <QList>
#include <QtGlobal>
#include <QString>
#include <QStringList>
#include <optional>

class QUrl;

// Window defaults and per-folder layouts. Continuous layout changes are
// coalesced by the window, with pending changes flushed on navigation/close.
namespace Settings {

// In View menu order
enum class ViewMode {
    ExtraLargeIcons = 0,
    LargeIcons,
    MediumIcons,
    SmallIcons,
    List,
    Details,
    Tiles,
    Content,
};

int iconSizeFor(ViewMode mode);

QByteArray windowGeometry();
void setWindowGeometry(const QByteArray &state);

QByteArray splitterState();
void setSplitterState(const QByteArray &state);

// Saved as one blob, since restoring order and widths separately would apply a
// width to whichever section sat at that index first
QByteArray headerState();
void setHeaderState(const QByteArray &state);

bool showHiddenFiles();
void setShowHiddenFiles(bool show);

// Win7's hide extensions for known file types, off here by default
bool hideKnownExtensions();
void setHideKnownExtensions(bool hide);

ViewMode defaultViewMode();
void setDefaultViewMode(ViewMode mode);

// Per folder, falling back to the default mode
ViewMode viewModeFor(const QUrl &url);
void setViewModeFor(const QUrl &url, ViewMode mode);

struct FolderViewState {
    ViewMode mode = ViewMode::Details;
    QByteArray header;
    QList<int> visibleColumns;
    int sortColumn = 0;
    Qt::SortOrder sortOrder = Qt::AscendingOrder;
    int groupColumn = -1;
    bool previewVisible = false;
    int previewWidth = 250;
    bool navigationVisible = true;
    bool detailsVisible = true;
};

std::optional<FolderViewState> folderViewStateFor(const QUrl &url);
void setFolderViewStateFor(const QUrl &url, const FolderViewState &state);

// Typed into the address bar, most recent first, and what F4 drops under it
QStringList recentPaths();
void addRecentPath(const QString &path);

// Win7's use check boxes to select items
bool useCheckBoxes();
void setUseCheckBoxes(bool use);

// Folder Options, View, always show menus
bool alwaysShowMenus();
void setAlwaysShowMenus(bool show);

// Folder Options, General, open each folder in its own window
bool browseInNewWindow();
void setBrowseInNewWindow(bool separate);

// Folder Options, General, single click to open an item, applied by the file
// view directly so the desktop's own setting cannot override it
bool singleClickToOpen();
void setSingleClickToOpen(bool single);

// Folder Options, Search, whether Enter looks inside files as well as at their
// names, off by default since without an index it reads every file it passes
bool searchFileContents();
void setSearchFileContents(bool contents);

// Folder Options, Search, include subfolders in search results, and with it
// off Enter filters this folder rather than starting a recursive search
bool searchSubfolders();
void setSearchSubfolders(bool recursive);

// Folder Options, View, warn about drives that are connected but not mounted,
// on by default so the notice is not missed the first time it matters
bool notifyUnmountedDrives();
void setNotifyUnmountedDrives(bool notify);

// Folder Options, reset folders (including the per-folder pane/column layouts).
void clearRememberedViewModes();

// Stable navigation group IDs, independent of translated headings.
std::optional<bool> navigationGroupExpanded(const QString &groupId);
void setNavigationGroupExpanded(const QString &groupId, bool expanded);

QStringList favoritesOrder();
void setFavoritesOrder(const QStringList &urls);

// Computer needs the difference, its own fallback being Tiles, and the mode
// alone cannot say whether the default applied
bool hasViewModeFor(const QUrl &url);

// The file picker's last folder and arrangement, shared by every application
// and by both chooser backends, so it lives in its own file rather than in
// whichever process happened to show the dialog
QUrl chooserFolder();
void setChooserFolder(const QUrl &folder);
// Source column and order, the column -1 until one is chosen (the dialog
// then sorts by Date modified, newest first)
int chooserSortColumn();
Qt::SortOrder chooserSortOrder();
void setChooserSort(int sourceColumn, Qt::SortOrder order);
ViewMode chooserViewMode();
void setChooserViewMode(ViewMode mode);
void clearChooserMemory();

} // namespace Settings
