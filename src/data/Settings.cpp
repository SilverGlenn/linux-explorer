#include "Settings.h"

#include <atomic>

#include <QSettings>
#include <QStringList>
#include <QUrl>

namespace Settings {

namespace {

// About Win7's own list length
constexpr int kMaxRecentPaths = 25;

QString key(const char *name)
{
    return QStringLiteral("View/") + QLatin1String(name);
}

// Constructing a QSettings costs about 22us, and these are read from the sort
// comparator and from every painted cell, so the value is held in memory and
// the setter is the only thing that can move it
struct BoolSetting {
    const char *name;
    bool fallback;
    std::atomic<signed char> cached{-1};

    bool get()
    {
        signed char value = cached.load(std::memory_order_relaxed);
        if (value < 0) {
            value = QSettings().value(key(name), fallback).toBool() ? 1 : 0;
            cached.store(value, std::memory_order_relaxed);
        }
        return value == 1;
    }

    void set(bool on)
    {
        QSettings().setValue(key(name), on);
        cached.store(on ? 1 : 0, std::memory_order_relaxed);
    }
};

BoolSetting s_showHiddenFiles{"ShowHiddenFiles", false};
BoolSetting s_hideKnownExtensions{"HideKnownExtensions", false};
BoolSetting s_useCheckBoxes{"UseCheckBoxes", false};
BoolSetting s_alwaysShowMenus{"AlwaysShowMenus", false};
BoolSetting s_browseInNewWindow{"BrowseInNewWindow", false};
BoolSetting s_singleClickToOpen{"SingleClickToOpen", false};
BoolSetting s_searchFileContents{"SearchFileContents", false};
BoolSetting s_searchSubfolders{"SearchSubfolders", true};
BoolSetting s_notifyUnmountedDrives{"NotifyUnmountedDrives", true};

// QSettings treats a slash as a group separator, so paths are percent encoded
QString folderKey(const QUrl &url)
{
    const QUrl normalized = url.adjusted(QUrl::NormalizePathSegments | QUrl::StripTrailingSlash
                                        | QUrl::RemovePassword);
    return QString::fromLatin1(normalized.toEncoded().toPercentEncoding());
}

QVariant legacyViewMode(QSettings &settings, const QUrl &url)
{
    const QString prefix = QStringLiteral("FolderViews/");
    QVariant value = settings.value(prefix + folderKey(url));
    if (!value.isValid())
        value = settings.value(prefix + QString::fromLatin1(url.toEncoded().toPercentEncoding()));
    if (!value.isValid()) {
        QUrl withSlash = url.adjusted(QUrl::NormalizePathSegments);
        if (!withSlash.path().endsWith(QLatin1Char('/')))
            withSlash.setPath(withSlash.path() + QLatin1Char('/'));
        value = settings.value(prefix + QString::fromLatin1(withSlash.toEncoded().toPercentEncoding()));
    }
    return value;
}

int clampMode(int raw)
{
    if (raw < int(ViewMode::ExtraLargeIcons) || raw > int(ViewMode::Content))
        return int(ViewMode::Details);
    return raw;
}

} // namespace

int iconSizeFor(ViewMode mode)
{
    switch (mode) {
    case ViewMode::ExtraLargeIcons: return 256;
    case ViewMode::LargeIcons:      return 96;
    case ViewMode::MediumIcons:     return 48;
    case ViewMode::SmallIcons:      return 16;
    case ViewMode::Tiles:           return 48;
    case ViewMode::Content:         return 32;
    case ViewMode::List:
    case ViewMode::Details:
    default:                        return 16;
    }
}

QByteArray windowGeometry()
{
    return QSettings().value(key("WindowGeometry")).toByteArray();
}

void setWindowGeometry(const QByteArray &state)
{
    QSettings().setValue(key("WindowGeometry"), state);
}

QByteArray splitterState()
{
    return QSettings().value(key("SplitterState")).toByteArray();
}

void setSplitterState(const QByteArray &state)
{
    QSettings().setValue(key("SplitterState"), state);
}

QByteArray headerState()
{
    return QSettings().value(key("HeaderState")).toByteArray();
}

void setHeaderState(const QByteArray &state)
{
    QSettings().setValue(key("HeaderState"), state);
}

bool showHiddenFiles()
{
    return s_showHiddenFiles.get();
}

void setShowHiddenFiles(bool show)
{
    s_showHiddenFiles.set(show);
}

bool hideKnownExtensions()
{
    return s_hideKnownExtensions.get();
}

void setHideKnownExtensions(bool hide)
{
    s_hideKnownExtensions.set(hide);
}

bool useCheckBoxes()
{
    return s_useCheckBoxes.get();
}

void setUseCheckBoxes(bool use)
{
    s_useCheckBoxes.set(use);
}

bool alwaysShowMenus()
{
    return s_alwaysShowMenus.get();
}

void setAlwaysShowMenus(bool show)
{
    s_alwaysShowMenus.set(show);
}

bool browseInNewWindow()
{
    return s_browseInNewWindow.get();
}

void setBrowseInNewWindow(bool separate)
{
    s_browseInNewWindow.set(separate);
}

bool singleClickToOpen()
{
    return s_singleClickToOpen.get();
}

void setSingleClickToOpen(bool single)
{
    s_singleClickToOpen.set(single);
}

bool searchFileContents()
{
    return s_searchFileContents.get();
}

void setSearchFileContents(bool contents)
{
    s_searchFileContents.set(contents);
}

bool searchSubfolders()
{
    return s_searchSubfolders.get();
}

void setSearchSubfolders(bool recursive)
{
    s_searchSubfolders.set(recursive);
}

bool notifyUnmountedDrives()
{
    return s_notifyUnmountedDrives.get();
}

void setNotifyUnmountedDrives(bool notify)
{
    s_notifyUnmountedDrives.set(notify);
}

void clearRememberedViewModes()
{
    QSettings s;
    // The whole group, since removing the keys alone would leave it behind
    s.remove(QStringLiteral("FolderViews"));
    s.remove(QStringLiteral("FolderStates"));
}

std::optional<bool> navigationGroupExpanded(const QString &groupId)
{
    QSettings s;
    s.beginGroup(QStringLiteral("Navigation/Groups"));
    const QString key = groupId;
    if (!s.contains(key))
        return std::nullopt;
    return s.value(key).toBool();
}

void setNavigationGroupExpanded(const QString &groupId, bool expanded)
{
    QSettings s;
    s.setValue(QStringLiteral("Navigation/Groups/") + groupId, expanded);
}

QStringList favoritesOrder()
{
    return QSettings().value(QStringLiteral("Navigation/FavoritesOrder")).toStringList();
}

void setFavoritesOrder(const QStringList &urls)
{
    QSettings settings;
    settings.setValue(QStringLiteral("Navigation/FavoritesOrder"), urls);
    settings.sync();
}

QStringList recentPaths()
{
    return QSettings().value(key("RecentPaths")).toStringList();
}

void addRecentPath(const QString &path)
{
    if (path.isEmpty())
        return;

    QStringList paths = recentPaths();
    // Retyping a path moves it to the top rather than adding a duplicate
    paths.removeAll(path);
    paths.prepend(path);
    while (paths.size() > kMaxRecentPaths)
        paths.removeLast();
    QSettings().setValue(key("RecentPaths"), paths);
}

ViewMode defaultViewMode()
{
    const int stored = QSettings().value(key("DefaultMode"), int(ViewMode::Details)).toInt();
    return ViewMode(clampMode(stored));
}

void setDefaultViewMode(ViewMode mode)
{
    QSettings().setValue(key("DefaultMode"), int(mode));
}

ViewMode viewModeFor(const QUrl &url)
{
    if (!url.isValid() || url.isEmpty())
        return defaultViewMode();
    if (const auto state = folderViewStateFor(url))
        return state->mode;

    QSettings s;
    const QVariant stored = legacyViewMode(s, url);

    if (!stored.isValid())
        return defaultViewMode();
    return ViewMode(clampMode(stored.toInt()));
}

bool hasViewModeFor(const QUrl &url)
{
    if (!url.isValid() || url.isEmpty())
        return false;
    if (folderViewStateFor(url))
        return true;
    QSettings s;
    return legacyViewMode(s, url).isValid();
}

void setViewModeFor(const QUrl &url, ViewMode mode)
{
    if (!url.isValid() || url.isEmpty())
        return;

    if (auto state = folderViewStateFor(url)) {
        state->mode = mode;
        setFolderViewStateFor(url, *state);
    }
    QSettings s;
    s.beginGroup(QStringLiteral("FolderViews"));

    // Keep legacy choices too; visiting more folders must not silently discard
    // a preference that has not yet been migrated to a complete layout.
    s.setValue(folderKey(url), int(mode));
    s.endGroup();
}

std::optional<FolderViewState> folderViewStateFor(const QUrl &url)
{
    if (!url.isValid() || url.isEmpty())
        return std::nullopt;
    const QVariantMap values = QSettings().value(QStringLiteral("FolderStates/") + folderKey(url)).toMap();
    if (values.value(QStringLiteral("version")).toInt() != 1)
        return std::nullopt;
    const auto integer = [&values](const QString &name, int fallback) {
        bool ok = false;
        const int value = values.value(name).toInt(&ok);
        return ok ? value : fallback;
    };
    FolderViewState state;
    state.mode = ViewMode(clampMode(integer(QStringLiteral("mode"), int(defaultViewMode()))));
    state.header = values.value(QStringLiteral("header")).toByteArray();
    if (state.header.size() > 65536)
        state.header.clear();
    for (const QVariant &column : values.value(QStringLiteral("columns")).toList()) {
        bool ok = false;
        const int source = column.toInt(&ok);
        if (ok) state.visibleColumns.append(source);
    }
    state.sortColumn = integer(QStringLiteral("sortColumn"), 0);
    state.sortOrder = integer(QStringLiteral("sortOrder"), 0) == int(Qt::DescendingOrder)
        ? Qt::DescendingOrder : Qt::AscendingOrder;
    state.groupColumn = integer(QStringLiteral("groupColumn"), -1);
    state.previewVisible = values.value(QStringLiteral("previewVisible"), false).toBool();
    state.previewWidth = qBound(120, integer(QStringLiteral("previewWidth"), 250), 2000);
    state.navigationVisible = values.value(QStringLiteral("navigationVisible"), true).toBool();
    state.detailsVisible = values.value(QStringLiteral("detailsVisible"), true).toBool();
    return state;
}

void setFolderViewStateFor(const QUrl &url, const FolderViewState &state)
{
    if (!url.isValid() || url.isEmpty())
        return;
    QVariantList columns;
    for (int source : state.visibleColumns)
        columns.append(source);
    const QVariantMap values {
        {QStringLiteral("version"), 1},
        {QStringLiteral("mode"), int(state.mode)},
        {QStringLiteral("header"), state.header},
        {QStringLiteral("columns"), columns},
        {QStringLiteral("sortColumn"), state.sortColumn},
        {QStringLiteral("sortOrder"), int(state.sortOrder)},
        {QStringLiteral("groupColumn"), state.groupColumn},
        {QStringLiteral("previewVisible"), state.previewVisible},
        {QStringLiteral("previewWidth"), state.previewWidth},
        {QStringLiteral("navigationVisible"), state.navigationVisible},
        {QStringLiteral("detailsVisible"), state.detailsVisible}
    };
    QSettings().setValue(QStringLiteral("FolderStates/") + folderKey(url), values);
}

namespace {
struct ChooserSettings : QSettings {
    ChooserSettings()
        : QSettings(QSettings::NativeFormat, QSettings::UserScope,
                    QStringLiteral("linux-explorer-win7"), QStringLiteral("file-picker"))
    {
    }
};
} // namespace

QUrl chooserFolder()
{
    return QUrl(ChooserSettings().value(QStringLiteral("Picker/Folder")).toString(), QUrl::StrictMode);
}

void setChooserFolder(const QUrl &folder)
{
    ChooserSettings().setValue(QStringLiteral("Picker/Folder"), folder.toString(QUrl::FullyEncoded));
}

int chooserSortColumn()
{
    bool ok = false;
    const int column = ChooserSettings().value(QStringLiteral("Picker/SortColumn")).toInt(&ok);
    return ok && column >= 0 ? column : -1;
}

Qt::SortOrder chooserSortOrder()
{
    return ChooserSettings().value(QStringLiteral("Picker/SortOrder"), int(Qt::DescendingOrder)).toInt()
        == int(Qt::AscendingOrder) ? Qt::AscendingOrder : Qt::DescendingOrder;
}

void setChooserSort(int sourceColumn, Qt::SortOrder order)
{
    ChooserSettings settings;
    settings.setValue(QStringLiteral("Picker/SortColumn"), sourceColumn);
    settings.setValue(QStringLiteral("Picker/SortOrder"), int(order));
}

ViewMode chooserViewMode()
{
    return ViewMode(clampMode(ChooserSettings().value(QStringLiteral("Picker/ViewMode"),
                                                      int(ViewMode::Details)).toInt()));
}

void setChooserViewMode(ViewMode mode)
{
    ChooserSettings().setValue(QStringLiteral("Picker/ViewMode"), int(mode));
}

void clearChooserMemory()
{
    ChooserSettings().remove(QStringLiteral("Picker"));
}

} // namespace Settings
