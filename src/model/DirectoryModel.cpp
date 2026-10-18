#include "DirectoryModel.h"
#include "Branding.h"
#include "Settings.h"
#include "FolderDragData.h"
#include "ItemIcons.h"
#include "aero/palette.h"

#include <KDirLister>
#include <KDirModel>
#include <KDirSortFilterProxyModel>
#include <KIO/Global>
#include <KIO/Job>
#include <KIO/Paste>
#include <KIO/SimpleJob>
#include <KUrlMimeData>

#include <QApplication>
#include <QBrush>
#include <QClipboard>
#include <QColor>
#include <QCollator>
#include <QDateTime>
#include <QFileInfo>
#include <QDir>
#include <QRegularExpression>
#include <QIcon>
#include <QLocale>
#include <QMimeData>
#include <QLocale>
#include <QMimeDatabase>
#include <QMimeType>
#include <QPainter>
#include <QPixmap>
#include <QSet>


namespace {

// Windows ghosts hidden entries rather than greying them
constexpr qreal kHiddenIconOpacity = 0.45;

// Fainter still, so a cut hidden file stays distinguishable
constexpr qreal kCutIconOpacity = 0.30;

// Comfortably more than a screenful in any view mode
constexpr int kMaxFadedIcons = 512;

// A glob this long is no real filter, and compiling it per dialog is a cost a
// hostile caller could set
constexpr int kMaxGlobLength = 256;

// Names cached for the sort, dropped wholesale at the bound as the icons are
constexpr int kMaxCachedNames = 50000;

// What the details view starts with, the rest coming from the header menu
const QList<int> &defaultColumns()
{
    static const QList<int> columns = {
        DirectoryModel::Name,
        DirectoryModel::Size,
        DirectoryModel::ModifiedTime,
        DirectoryModel::Type,
    };
    return columns;
}

// KIO's own date column prefers a relative wording, which is useless in a
// column where the point is comparing rows, and the system locale is used
// rather than the default since the two disagree over how a date is written
QString formatTimestamp(const QDateTime &when)
{
    if (!when.isValid())
        return {};
    const QLocale locale = QLocale::system();
    return locale.toString(when.date(), QLocale::ShortFormat)
         + QLatin1Char(' ')
         + locale.toString(when.time(), QLocale::ShortFormat);
}

// Bidi overrides and isolates and control characters let a name read as
// something else ("invoice_fdp.exe" shown reversed), so they are shown as the
// replacement character, the real name staying untouched for rename
QString neutralizeSpoofing(QString name)
{
    const auto spoofing = [](QChar ch) {
        const ushort c = ch.unicode();
        return (c >= 0x202A && c <= 0x202E) || (c >= 0x2066 && c <= 0x2069)
            || c == 0x200E || c == 0x200F || c == 0x061C
            || ch.category() == QChar::Other_Control;
    };
    int i = 0;
    while (i < name.size() && !spoofing(name.at(i)))
        ++i;
    for (; i < name.size(); ++i) {
        if (spoofing(name.at(i)))
            name[i] = QChar::ReplacementCharacter;
    }
    return name;
}

// By name alone unless KIO already knows, since sniffing the contents on the
// GUI thread downloads the file from a network or FUSE mount
QList<QMimeType> mimesFor(const KFileItem &item)
{
    QMimeDatabase db;
    if (item.isMimeTypeKnown()) {
        const QMimeType known = db.mimeTypeForName(item.mimetype());
        if (known.isValid())
            return {known};
    }
    return db.mimeTypesForFileName(item.name());
}

// What a double click would run rather than open, whose extension is the one
// clue to that and so is never hidden. Judged by type, not the exec bit, which
// every file has on FAT and NTFS drives and which does not make a document run.
bool isLaunchable(const KFileItem &item, const QList<QMimeType> &mimes)
{
    if (item.name().endsWith(QLatin1String(".desktop"), Qt::CaseInsensitive)
        || item.name().endsWith(QLatin1String(".directory"), Qt::CaseInsensitive))
        return true;
    static const QStringList launchable = {
        QStringLiteral("application/x-desktop"),
        QStringLiteral("application/x-executable"),
        QStringLiteral("application/x-pie-executable"),
        QStringLiteral("application/x-sharedlib"),
        QStringLiteral("application/x-shellscript"),
        QStringLiteral("application/x-appimage"),
        QStringLiteral("application/vnd.appimage"),
        QStringLiteral("application/x-ms-dos-executable"),
        QStringLiteral("application/vnd.microsoft.portable-executable"),
        QStringLiteral("application/x-msdownload"),
        QStringLiteral("application/x-msdos-program"),
        QStringLiteral("application/x-msi"),
        QStringLiteral("application/x-ms-shortcut"),
        QStringLiteral("application/x-java-archive"),
        QStringLiteral("text/x-python"),
        QStringLiteral("text/x-perl"),
        QStringLiteral("text/x-ruby"),
    };
    for (const QMimeType &mime : mimes) {
        for (const QString &type : launchable) {
            if (mime.inherits(type))
                return true;
        }
    }
    return false;
}

} // namespace

// Win7 prints the name in black and every other column in grey, done on the
// model rather than a delegate, which would owe the whole item's painting in
// order to change one colour
class Win7ColumnProxy : public KDirSortFilterProxyModel {
public:
    explicit Win7ColumnProxy(QObject *parent = nullptr)
        : KDirSortFilterProxyModel(parent)
        , m_columns(defaultColumns())
    {
        // From the clipboard rather than our own cut, since another file
        // manager's cut has to ghost these items too
        connect(QApplication::clipboard(), &QClipboard::dataChanged,
                this, &Win7ColumnProxy::refreshCutItems);
        refreshCutItems();
        // Qt's C-locale collator cannot do numeric sorting. Use a numeric-
        // capable locale there too, including minimal CI/container sessions.
        if (m_nameCollator.locale().language() == QLocale::C)
            m_nameCollator.setLocale(QLocale(QLocale::English, QLocale::UnitedStates));
        m_nameCollator.setCaseSensitivity(Qt::CaseInsensitive);
        m_nameCollator.setNumericMode(true);
    }

    QMimeData *mimeData(const QModelIndexList &indexes) const override
    {
        QMimeData *mime = KDirSortFilterProxyModel::mimeData(indexes);
        if (!mime) return nullptr;
        QList<QUrl> folders;
        for (const QModelIndex &index : indexes) {
            const KFileItem item = fileItem(mapToSource(index));
            if (item.isNull()) continue;
            if (!item.isDir()) return mime;
            if (!folders.contains(item.url())) folders.append(item.url());
        }
        if (!folders.isEmpty()) FolderDragData::setFolders(mime, folders);
        return mime;
    }

    QStringList mimeTypes() const override
    {
        QStringList types = KDirSortFilterProxyModel::mimeTypes();
        types.append(FolderDragData::mimeType);
        return types;
    }

    QList<int> columns() const { return m_columns; }

    void setColumns(const QList<int> &sourceColumns)
    {
        QList<int> wanted = sourceColumns;
        if (!wanted.contains(DirectoryModel::Name))
            wanted.prepend(DirectoryModel::Name);
        std::sort(wanted.begin(), wanted.end());
        if (wanted == m_columns)
            return;

        // Structural, since without the surrounding pair the views go on
        // addressing columns that no longer exist
        beginResetModel();
        m_columns = wanted;
        endResetModel();
    }

    // Renumbered in source order, which the column lookup relies on
    bool filterAcceptsColumn(int sourceColumn, const QModelIndex &) const override
    {
        return m_columns.contains(sourceColumn);
    }

    void setChooserFilter(const QStringList &globs, const QStringList &mimeTypes,
                          Qt::CaseSensitivity sensitivity, bool foldersOnly)
    {
        m_chooserSensitivity = sensitivity;
        m_foldersOnly = foldersOnly;
        m_chooserMimeTypes = mimeTypes;
        m_chooserActive = !globs.isEmpty() || !mimeTypes.isEmpty();

        // Compiled once here rather than per row, and an over-long glob is
        // dropped, which leaves it matching nothing rather than everything
        m_chooserGlobs.clear();
        for (const QString &glob : globs) {
            if (glob.size() > kMaxGlobLength)
                continue;
            const QRegularExpression expression(QRegularExpression::wildcardToRegularExpression(
                glob, QRegularExpression::NonPathWildcardConversion),
                sensitivity == Qt::CaseInsensitive
                    ? QRegularExpression::CaseInsensitiveOption : QRegularExpression::NoPatternOption);
            if (expression.isValid())
                m_chooserGlobs.append(expression);
        }
        invalidate();
    }

    // For the source changing what a name is built from, and called before the
    // proxy's own handling of it so the re-sort never reads a stale name
    void clearNameCache()
    {
        m_nameCache.clear();
    }

    // Drops the directories friendly mode hides rather than renames, and
    // showing hidden files brings them back
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override
    {
        if (isHiddenSystemFolder(sourceRow, sourceParent))
            return false;
        const KFileItem item = fileItem(sourceModel()->index(sourceRow, KDirModel::Name, sourceParent));
        if (m_foldersOnly && !item.isNull() && !item.isDir())
            return false;
        if (!item.isNull() && !item.isDir() && m_chooserActive) {
            bool matches = false;
            for (const QRegularExpression &expression : m_chooserGlobs) {
                if (expression.match(item.name()).hasMatch()) {
                    matches = true;
                    break;
                }
            }
            if (!matches && !m_chooserMimeTypes.isEmpty()) {
                const QList<QMimeType> mimes = mimesFor(item);
                for (const QMimeType &mime : mimes) {
                    for (const QString &type : m_chooserMimeTypes) {
                        if (mime.name() == type || mime.inherits(type)
                            || (type.endsWith(QLatin1String("/*"))
                                && mime.name().startsWith(type.chopped(1)))) {
                            matches = true;
                            break;
                        }
                    }
                    if (matches)
                        break;
                }
            }
            if (!matches)
                return false;
        }
        return KDirSortFilterProxyModel::filterAcceptsRow(sourceRow, sourceParent);
    }

    // Sorts on the displayed name, or the rows come out alphabetical by their
    // real ones and nonsense on screen, and folders first still comes from the
    // base class, applied before it delegates here
    bool subSortLessThan(const QModelIndex &left, const QModelIndex &right) const override
    {
        if (left.column() != KDirModel::Name)
            return KDirSortFilterProxyModel::subSortLessThan(left, right);

        const KFileItem leftItem = fileItem(left);
        const KFileItem rightItem = fileItem(right);
        if (leftItem.isNull() || rightItem.isNull())
            return KDirSortFilterProxyModel::subSortLessThan(left, right);

        // The base class applies directories/hidden-files ordering inside
        // subSortLessThan(), not before it. Do not bypass that ordering merely
        // because we have friendly captions to compare.
        if ((sortFoldersFirst() && leftItem.isDir() != rightItem.isDir())
            || (sortHiddenFilesLast() && leftItem.isHidden() != rightItem.isHidden()))
            return KDirSortFilterProxyModel::subSortLessThan(left, right);
        const QString leftName = displayName(leftItem);
        const QString rightName = displayName(rightItem);
        const int order = m_nameCollator.compare(leftName, rightName);
        if (order != 0)
            return order < 0;

        return KDirSortFilterProxyModel::subSortLessThan(left, right);
    }

    QVariant headerData(int section, Qt::Orientation orientation, int role) const override
    {
        if (orientation == Qt::Horizontal && role == Qt::DisplayRole
            && section >= 0 && section < m_columns.size()
            && m_columns.at(section) == DirectoryModel::ModifiedTime)
            return QObject::tr("Date modified");
        return KDirSortFilterProxyModel::headerData(section, orientation, role);
    }

    QVariant data(const QModelIndex &index, int role) const override
    {
        if (!index.isValid())
            return KDirSortFilterProxyModel::data(index, role);

        // Source numbering, which matches the view's only while every column
        // is switched on
        const QModelIndex sourceIndex = mapToSource(index);
        const int column = sourceIndex.column();
        const KFileItem item = fileItem(sourceIndex);

        if (role == Qt::ForegroundRole && column != DirectoryModel::Name)
            return QBrush(Aero::Palette::rgb(Aero::Palette::MutedText));

        // Hidden entries and entries cut to the clipboard are ghosted, and
        // only the icon fades
        if (role == Qt::DecorationRole && column == DirectoryModel::Name
            && !item.isNull()) {
            // The source would load an absolute icon path on this thread, so
            // those items never ask it
            const bool pathIcon = item.iconName().contains(QLatin1Char('/'));
            const QIcon safe = pathIcon
                ? ItemIcons::safeIcon(item, QIcon::fromTheme(QStringLiteral("unknown")))
                : QIcon();
            const bool cut = isCut(item);
            const bool ghosted = item.isHidden()
                || Branding::isSystemFolder(item.url().toLocalFile());
            if (cut || ghosted) {
                const QVariant base = pathIcon
                    ? QVariant::fromValue(safe)
                    : KDirSortFilterProxyModel::data(index, role);
                const QIcon icon = qvariant_cast<QIcon>(base);
                const qreal opacity = cut ? kCutIconOpacity : kHiddenIconOpacity;
                if (!icon.isNull())
                    return fadedIcon(icon, opacity, cut);
            }
            if (pathIcon)
                return safe;
        }

        if (role == Qt::DisplayRole && !item.isNull()) {
            switch (column) {
            case DirectoryModel::ModifiedTime:
                return formatTimestamp(item.time(KFileItem::ModificationTime));

            // Blank for folders, as in Win7, since the model reports only the
            // directory's own size and the real figure needs a tree walk
            case DirectoryModel::Size:
                if (item.isDir())
                    return QString();
                break;

            // Only the displayed string changes, the item's location being
            // left untouched
            case DirectoryModel::Name:
                return displayName(item);

            default:
                break;
            }
        }

        // Win7's infotip, which is what the icon views have instead of columns
        if (role == Qt::ToolTipRole && !item.isNull())
            return infoTip(item);

        return KDirSortFilterProxyModel::data(index, role);
    }

private:
    KFileItem fileItem(const QModelIndex &sourceIndex) const
    {
        auto *dirModel = qobject_cast<KDirModel *>(sourceModel());
        if (!dirModel || !sourceIndex.isValid())
            return KFileItem();
        return dirModel->itemForIndex(sourceIndex);
    }

    bool isHiddenSystemFolder(int sourceRow, const QModelIndex &sourceParent) const
    {
        if (!Branding::windowsFriendlyMode())
            return false;

        auto *dirModel = qobject_cast<KDirModel *>(sourceModel());
        if (!dirModel || dirModel->dirLister()->showHiddenFiles())
            return false;

        const QModelIndex sourceIndex =
            dirModel->index(sourceRow, KDirModel::Name, sourceParent);
        const KFileItem item = dirModel->itemForIndex(sourceIndex);
        if (item.isNull())
            return false;

        return Branding::isSystemFolder(item.url().toLocalFile());
    }

    // Cached for the sort, which asks twice per comparison, and keyed on the
    // location since a rename or a changed type reaches here as a source
    // signal that clears it, and a setting is checked on every lookup
    QString displayName(const KFileItem &item) const
    {
        const bool hide = Settings::hideKnownExtensions();
        const bool friendly = Branding::windowsFriendlyMode();
        if (hide != m_cachedHide || friendly != m_cachedFriendly) {
            m_nameCache.clear();
            m_cachedHide = hide;
            m_cachedFriendly = friendly;
        }

        const QUrl url = item.url();
        const auto cached = m_nameCache.constFind(url);
        if (cached != m_nameCache.constEnd())
            return cached.value();

        const QString name = computeDisplayName(item);
        if (m_nameCache.size() >= kMaxCachedNames)
            m_nameCache.clear();
        m_nameCache.insert(url, name);
        return name;
    }

    QString computeDisplayName(const KFileItem &item) const
    {
        QString name = neutralizeSpoofing(item.text());

        if (Branding::windowsFriendlyMode()) {
            const QString mapped = Branding::folderName(item.url().toLocalFile());
            if (!mapped.isEmpty())
                return mapped;   // a renamed system folder has no extension to hide
        }

        if (Settings::hideKnownExtensions() && !item.isDir()) {
            // Known means the desktop can name the type, and stripping an
            // unrecognised suffix would hide the only clue to what it is
            // Only the last suffix goes, "Invoice.pdf.desktop" must not read as
            // "Invoice", and a dot at either end of the name does not start one
            const int dot = name.lastIndexOf(QLatin1Char('.'));
            const bool hasExtension = dot > 0 && dot < name.length() - 1;
            if (hasExtension) {
                static const QString unknown =
                    QMimeDatabase().mimeTypeForName(QStringLiteral("application/octet-stream")).name();
                const QList<QMimeType> mimes = mimesFor(item);
                const bool known = !mimes.isEmpty() && mimes.first().name() != unknown;
                if (known && !isLaunchable(item, mimes))
                    name.truncate(dot);
            }
        }
        return name;
    }

    QString infoTip(const KFileItem &item) const
    {
        QStringList lines;
        lines << displayName(item);
        if (!item.mimeComment().isEmpty())
            lines << QStringLiteral("Type: %1").arg(item.mimeComment());
        const QString when = formatTimestamp(item.time(KFileItem::ModificationTime));
        if (!when.isEmpty())
            lines << QStringLiteral("Date modified: %1").arg(when);
        if (!item.isDir())
            lines << QStringLiteral("Size: %1").arg(KIO::convertSize(item.size()));

        // Rich text by declaration, with every line escaped, or a file named
        // "<img src=x>" would be rendered rather than shown
        for (QString &line : lines)
            line = line.toHtmlEscaped();
        return QStringLiteral("<qt>%1</qt>").arg(lines.join(QStringLiteral("<br>")));
    }

    bool isCut(const KFileItem &item) const
    {
        return !m_cutUrls.isEmpty() && m_cutUrls.contains(item.url());
    }

    void refreshCutItems()
    {
        QSet<QUrl> urls;
        const QMimeData *mimeData = QApplication::clipboard()->mimeData();
        if (mimeData && KIO::isClipboardDataCut(mimeData)) {
            const QList<QUrl> cut = KUrlMimeData::urlsFromMimeData(mimeData);
            urls = QSet<QUrl>(cut.begin(), cut.end());
        }
        if (urls == m_cutUrls)
            return;
        m_cutUrls = urls;

        // The whole row, working out which changed costing more than a repaint
        if (rowCount() > 0) {
            const QModelIndex topLeft = index(0, 0);
            const QModelIndex bottomRight = index(rowCount() - 1, columnCount() - 1);
            Q_EMIT dataChanged(topLeft, bottomRight, {Qt::DecorationRole});
        }
    }

    // The cache is a requirement rather than an optimisation, this running for
    // every visible cell on every repaint, and it is keyed on the icon's own
    // contents rather than its name, which is shared by every file of a MIME
    // type and with previews on gave every ghosted photo the same picture
    QIcon fadedIcon(const QIcon &source, qreal opacity, bool cut) const
    {
        const QString prefix = cut ? QStringLiteral("cut:") : QStringLiteral("hidden:");
        const QString key = prefix + QString::number(source.cacheKey());
        const auto cached = m_fadedIcons.constFind(key);
        if (cached != m_fadedIcons.constEnd())
            return cached.value();

        QIcon faded;
        // The available sizes come back empty for scalable themes
        for (int size : {16, 22, 24, 32, 48, 64, 96, 128, 256}) {
            const QPixmap original = source.pixmap(size, size);
            if (original.isNull())
                continue;

            QPixmap ghost(original.size());
            ghost.setDevicePixelRatio(original.devicePixelRatio());
            ghost.fill(Qt::transparent);

            QPainter painter(&ghost);
            painter.setOpacity(opacity);
            painter.drawPixmap(0, 0, original);
            painter.end();

            faded.addPixmap(ghost);
        }

        // A key is one icon rather than one MIME type, so a folder of
        // thumbnails would otherwise keep an entry per file, and it is dropped
        // wholesale since what is on screen is rebuilt on the next repaint
        if (m_fadedIcons.size() >= kMaxFadedIcons)
            m_fadedIcons.clear();

        m_fadedIcons.insert(key, faded);
        return faded;
    }

    Qt::CaseSensitivity m_chooserSensitivity = Qt::CaseInsensitive;
    bool m_foldersOnly = false;
    bool m_chooserActive = false;
    QList<QRegularExpression> m_chooserGlobs;
    QStringList m_chooserMimeTypes;
    QList<int> m_columns;
    QCollator m_nameCollator;
    QSet<QUrl> m_cutUrls;
    mutable QHash<QString, QIcon> m_fadedIcons;
    mutable QHash<QUrl, QString> m_nameCache;
    mutable bool m_cachedHide = false;
    mutable bool m_cachedFriendly = false;
};

DirectoryModel::DirectoryModel(QObject *parent)
    : QObject(parent)
    , m_dirModel(new KDirModel(this))
    , m_proxy(new Win7ColumnProxy(this))
{
    // Before the proxy connects its own handlers, which re-sort on these and
    // so need the names already dropped. A preview only changes the icon, and a
    // new row has a location no name was cached under
    auto clearNames = [this] { m_proxy->clearNameCache(); };
    connect(m_dirModel, &QAbstractItemModel::modelReset, m_proxy, clearNames);
    connect(m_dirModel, &QAbstractItemModel::layoutChanged, m_proxy, clearNames);
    connect(m_dirModel, &QAbstractItemModel::rowsRemoved, m_proxy, clearNames);
    connect(m_dirModel, &QAbstractItemModel::dataChanged, m_proxy,
            [this](const QModelIndex &, const QModelIndex &, const QList<int> &roles) {
        if (roles.isEmpty() || roles.contains(Qt::DisplayRole) || roles.contains(Qt::EditRole))
            m_proxy->clearNameCache();
    });

    m_proxy->setSourceModel(m_dirModel);
    m_proxy->setSortFoldersFirst(true);
    // Win7 ignores case when sorting
    m_proxy->setSortCaseSensitivity(Qt::CaseInsensitive);
    m_proxy->sort(KDirModel::Name, Qt::AscendingOrder);

    // Name column only, the default matching sizes and dates as well
    m_proxy->setFilterKeyColumn(KDirModel::Name);
    m_proxy->setFilterCaseSensitivity(Qt::CaseInsensitive);

    // The model disables drops by default, and this only advertises the rows
    // as targets, the drop itself belonging to the view
    m_dirModel->setDropsAllowed(KDirModel::DropOnDirectory);

    KDirLister *lister = m_dirModel->dirLister();
    lister->setShowHiddenFiles(Settings::showHiddenFiles());
    lister->setAutoErrorHandlingEnabled(false);   // we surface errors ourselves

    connect(lister, &KCoreDirLister::started, this, &DirectoryModel::loadingStarted);
    connect(lister, &KCoreDirLister::completed, this, &DirectoryModel::loadingFinished);
    connect(lister, &KCoreDirLister::percent, this, &DirectoryModel::loadingProgress);
    connect(lister, &KCoreDirLister::redirection, this, &DirectoryModel::urlRedirected);
    connect(lister, &KCoreDirLister::jobError, this, [this](KIO::Job *job) {
        // The failing job names the directory it was reading, which is not
        // always the lister's own
        QUrl url;
        if (auto *simple = qobject_cast<KIO::SimpleJob *>(job))
            url = simple->url();
        if (!url.isValid())
            url = m_dirModel->dirLister()->url();
        Q_EMIT errorOccurred(job->error(), job->errorString(), url);
    });
    connect(lister, &KCoreDirLister::itemsAdded, this,
            [this](const QUrl &, const KFileItemList &items) {
        QList<QUrl> urls;
        urls.reserve(items.size());
        for (const KFileItem &item : items)
            urls.append(item.url());
        Q_EMIT itemsAdded(urls);
    });
}

QAbstractItemModel *DirectoryModel::model() const
{
    return m_proxy;
}

void DirectoryModel::setUrl(const QUrl &url)
{
    if (url.isValid()) {
        m_proxy->clearNameCache();
        m_dirModel->openUrl(url);
    }
}

QUrl DirectoryModel::url() const
{
    return m_dirModel->dirLister()->url();
}

void DirectoryModel::refresh()
{
    const QUrl current = url();
    if (current.isValid())
        m_dirModel->openUrl(current, KDirModel::Reload);
}

void DirectoryModel::setShowHiddenFiles(bool show)
{
    KDirLister *lister = m_dirModel->dirLister();
    if (lister->showHiddenFiles() == show)
        return;
    lister->setShowHiddenFiles(show);
    // The lister does not filter what it has already cached
    lister->emitChanges();
    // The proxy's filter reads this setting too
    m_proxy->invalidate();
    Settings::setShowHiddenFiles(show);
}

bool DirectoryModel::showHiddenFiles() const
{
    return m_dirModel->dirLister()->showHiddenFiles();
}

QList<int> DirectoryModel::visibleColumns() const
{
    return m_proxy->columns();
}

void DirectoryModel::setVisibleColumns(const QList<int> &sourceColumns)
{
    m_proxy->setColumns(sourceColumns);
}

void DirectoryModel::setColumnVisible(int sourceColumn, bool visible)
{
    QList<int> columns = m_proxy->columns();
    if (visible) {
        if (!columns.contains(sourceColumn))
            columns.append(sourceColumn);
    } else {
        columns.removeAll(sourceColumn);
    }
    m_proxy->setColumns(columns);
}

bool DirectoryModel::isColumnVisible(int sourceColumn) const
{
    return m_proxy->columns().contains(sourceColumn);
}

int DirectoryModel::viewColumnFor(int sourceColumn) const
{
    // The survivors stay in source order, so a column's view index is how many
    // enabled columns sort before it
    return m_proxy->columns().indexOf(sourceColumn);
}

int DirectoryModel::sourceColumnFor(int viewColumn) const
{
    const QList<int> columns = m_proxy->columns();
    if (viewColumn < 0 || viewColumn >= columns.size())
        return -1;
    return columns.at(viewColumn);
}

int DirectoryModel::sortColumn() const
{
    return sourceColumnFor(m_proxy->sortColumn());
}

Qt::SortOrder DirectoryModel::sortOrder() const
{
    return m_proxy->sortOrder();
}

void DirectoryModel::sort(int sourceColumn, Qt::SortOrder order)
{
    const int viewColumn = viewColumnFor(sourceColumn);
    if (viewColumn < 0)
        return;   // sorting by a column that is not on show has nothing to click
    m_proxy->sort(viewColumn, order);
}

void DirectoryModel::refreshDisplayNames()
{
    // Not a repaint, the names feeding the sort, so the rows have to be
    // reordered rather than merely redrawn
    m_proxy->clearNameCache();
    m_proxy->invalidate();
}

void DirectoryModel::setNameFilter(const QString &pattern)
{
    m_proxy->setFilterFixedString(pattern);
}

KFileItem DirectoryModel::itemForIndex(const QModelIndex &proxyIndex) const
{
    if (!proxyIndex.isValid())
        return KFileItem();
    return m_dirModel->itemForIndex(m_proxy->mapToSource(proxyIndex));
}

QList<KFileItem> DirectoryModel::itemsForIndexes(const QModelIndexList &proxyIndexes) const
{
    QList<KFileItem> items;
    items.reserve(proxyIndexes.size());
    for (const QModelIndex &index : proxyIndexes) {
        // A row selection reports one index per column
        if (index.column() != 0)
            continue;
        const KFileItem item = itemForIndex(index);
        if (!item.isNull())
            items.append(item);
    }
    return items;
}

QModelIndex DirectoryModel::indexForUrl(const QUrl &url) const
{
    const QModelIndex sourceIndex = m_dirModel->indexForUrl(url);
    if (!sourceIndex.isValid())
        return {};
    return m_proxy->mapFromSource(sourceIndex);
}

int DirectoryModel::rowCount() const
{
    return m_proxy->rowCount();
}

void DirectoryModel::setChooserFilter(const QStringList &globs, const QStringList &mimeTypes,
                                      Qt::CaseSensitivity sensitivity, bool foldersOnly)
{
    m_proxy->setChooserFilter(globs, mimeTypes, sensitivity, foldersOnly);
}
