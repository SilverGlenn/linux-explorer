#include "BrowserNavigation.h"
#include "Branding.h"
#include "ComputerModel.h"
#include "DriveLabel.h"
#include "Archives.h"
#include "BreadcrumbBar.h"
#include "Libraries.h"
#include "Locations.h"
#include "Settings.h"
#include "aero/icons.h"
#include "aero/text.h"

#include <AeroQt/navbuttons.h>
#include <KFilePlacesModel>
#include <KIO/Global>
#include <QAction>
#include <QCompleter>
#include <QDir>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QProgressBar>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QTimer>
#include <QToolButton>

namespace {
// The address bar's fill runs on a timer rather than on the listing, most
// directories reporting no progress at all and the rest reporting it late, so
// it creeps to a ceiling, waits there, then runs out once the folder lands
constexpr int kProgressCeiling = 80;
constexpr int kProgressTickInterval = 200;
constexpr int kProgressTickStep = 1;          // so the ceiling is sixteen seconds off

// Two pixels is the glass texture's own frame
constexpr int kPathProgressInset = 2;

// Faded rather than solid, the crumbs having to stay readable across it
constexpr qreal kPathProgressOpacity = 0.5;

constexpr int kProgressFinishDuration = 180;

// Or a directory with thousands of subfolders builds a menu off the screen
constexpr int kMaxCrumbMenuEntries = 60;

constexpr int kMaxRecentEntries = 15;

} // namespace

BrowserNavigation::BrowserNavigation(KFilePlacesModel *places, ComputerModel *computer,
                                     QHBoxLayout *layout, QObject *parent)
    : QObject(parent ? parent : layout->parentWidget())
    , m_places(places)
    , m_computerModel(computer)
    , m_host(layout->parentWidget())
{
    // No background at all, the inset call marking this bar transparent so the
    // theme paints its children against the window's glass
    auto *navBtns = new Aero::NavButtons(m_host);
    m_backBtn = navBtns->back();
    m_forwardBtn = navBtns->forward();
    m_backBtn->setObjectName(QStringLiteral("backButton"));
    m_forwardBtn->setObjectName(QStringLiteral("forwardButton"));
    m_backBtn->setAccessibleName(tr("Back"));
    m_forwardBtn->setAccessibleName(tr("Forward"));
    m_backBtn->setEnabled(false);
    m_forwardBtn->setEnabled(false);
    connect(m_backBtn, &QPushButton::clicked, this, &BrowserNavigation::goBack);
    connect(m_forwardBtn, &QPushButton::clicked, this, &BrowserNavigation::goForward);
    layout->addWidget(navBtns);

    // The chrome reserves the slot and paints the arrow, and giving it a menu
    // is what makes the button appear, it being hidden until then
    auto *recentMenu = new QMenu(navBtns);
    connect(recentMenu, &QMenu::aboutToShow, this, [this, recentMenu] {
        recentMenu->clear();

        // Newest first, walking the history backwards
        const int oldestShown = qMax(0, int(m_history.size()) - kMaxRecentEntries);
        for (int i = m_history.size() - 1; i >= oldestShown; --i) {
            const QUrl url = m_history.at(i);
            const QList<Crumb> crumbs = crumbsFor(url);
            QString label = url.toString();
            if (!crumbs.isEmpty())
                label = crumbs.last().label;

            QAction *action = recentMenu->addAction(locationIcon(url), label);
            action->setCheckable(true);
            action->setChecked(i == m_historyIndex);
            connect(action, &QAction::triggered, this, [this, i] {
                // Moves within the history rather than pushing an entry, so
                // forward can still return
                if (i < 0 || i >= m_history.size())
                    return;
                goToHistoryIndex(i);
            });
        }
    });
    // Taken by reference, so the menu has to be a named variable
    navBtns->setMenu(recentMenu);
    navBtns->menuButton()->setToolTip(tr("Recent locations"));
    navBtns->menuButton()->setAccessibleName(tr("Recent locations"));
    layout->addSpacing(4);

    // On the theme's glass texture so the box matches the search field, and
    // scoped by name so the child labels are unaffected
    m_pathBox = new QWidget;
    m_pathBox->installEventFilter(this);
    m_pathBox->setObjectName("addressBox");
    m_pathBox->setStyleSheet(
        "#addressBox { border-image: url(:/AeroQt/transpbg/entry/normal.png) 4 4 4 4 repeat; border-width: 4px; }"
        "#addressBox:hover { border-image: url(:/AeroQt/transpbg/entry/hover.png) 4 4 4 4 repeat; }"
    );
    m_pathBox->setFixedHeight(24);
    // Clicking the bar's empty space starts editing the path
    m_pathBox->setCursor(Qt::IBeamCursor);
    auto *pathBoxLayout = new QHBoxLayout(m_pathBox);
    pathBoxLayout->setContentsMargins(0, 2, 2, 2);
    pathBoxLayout->setSpacing(0);

    auto *crumbArea = new QWidget;
    m_pathLayout = new QHBoxLayout(crumbArea);
    m_pathLayout->setContentsMargins(4, 0, 4, 0);
    m_pathLayout->setSpacing(0);
    pathBoxLayout->addWidget(crumbArea, 1);

    // Inside the box rather than in the layout, positioned by hand and lowered
    // behind the crumbs, which have no background and read straight over it
    m_pathProgress = new QProgressBar(m_pathBox);
    m_pathProgress->setRange(0, 100);
    m_pathProgress->setTextVisible(false);
    // A child taking the click would leave a dead strip across the bar
    m_pathProgress->setAttribute(Qt::WA_TransparentForMouseEvents);
    // An effect rather than colours of its own, so the bar stays the real
    // style's rather than an imitation of it
    auto *fade = new QGraphicsOpacityEffect(m_pathProgress);
    fade->setOpacity(kPathProgressOpacity);
    m_pathProgress->setGraphicsEffect(fade);
    m_pathProgress->hide();
    m_pathProgress->lower();

    m_pathProgressTimer = new QTimer(this);
    m_pathProgressTimer->setInterval(kProgressTickInterval);
    connect(m_pathProgressTimer, &QTimer::timeout, this, [this] {
        const int value = m_pathProgress->value();
        if (value < kProgressCeiling) {
            const int next = qMin(value + kProgressTickStep, kProgressCeiling);
            m_pathProgress->setValue(next);
        }
    });

    // One animation rather than one per finish, so a folder abandoned partway
    // can stop the previous run instead of it writing into the next bar
    m_pathProgressFill = new QPropertyAnimation(m_pathProgress, "value", this);
    m_pathProgressFill->setDuration(kProgressFinishDuration);
    m_pathProgressFill->setEndValue(100);
    connect(m_pathProgressFill, &QPropertyAnimation::finished,
            this, &BrowserNavigation::stopPathProgress);

    layout->addWidget(m_pathBox, 1);
    auto *refreshButton = new QToolButton;
    refreshButton->setIcon(Aero::themeIcon({"view-refresh", "reload"}));
    refreshButton->setStyleSheet(
        QStringLiteral("QToolButton { border: none;"
                       " border-left: 1px solid rgba(121, 121, 121, 173);"
                       " background: transparent; border-image: none; }"));
    refreshButton->setToolTip(tr("Refresh (F5)"));
    refreshButton->setAccessibleName(tr("Refresh"));
    refreshButton->setFixedWidth(24);
    refreshButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    refreshButton->setCursor(Qt::ArrowCursor);
    pathBoxLayout->addWidget(refreshButton);
    connect(refreshButton, &QToolButton::clicked, this, &BrowserNavigation::refreshRequested);

    const auto refresh = [this] { refreshTrail(); };
    connect(places, &QAbstractItemModel::modelReset, this, refresh);
    connect(places, &QAbstractItemModel::rowsInserted, this, refresh);
    connect(places, &QAbstractItemModel::rowsRemoved, this, refresh);
}

BrowserNavigation::Crumb BrowserNavigation::driveFor(const QString &path) const
{
    KFilePlacesModel *places = m_places;
    QString label;
    QUrl mountUrl;
    int bestLength = -1;

    for (int row = 0; row < places->rowCount(); ++row) {
        const QModelIndex index = places->index(row, 0);
        const KFilePlacesModel::GroupType group = places->groupType(index);
        if (group != KFilePlacesModel::DevicesType
            && group != KFilePlacesModel::RemovableDevicesType) {
            continue;
        }

        const QUrl deviceUrl = places->url(index);
        if (!deviceUrl.isLocalFile())
            continue;

        // Only a mount the path is on, so /media/a must not claim /media/ab
        const QString mount = QDir::cleanPath(deviceUrl.toLocalFile());
        const bool isRoot = (mount == QLatin1String("/"));
        QString mountPrefix = mount;
        if (!isRoot)
            mountPrefix += QLatin1Char('/');
        if (path != mount && !path.startsWith(mountPrefix))
            continue;

        // The longest match wins, the nearer mount being the one it is on
        if (mount.length() > bestLength) {
            bestLength = mount.length();
            // Never substituted, friendly mode renaming directories and not
            // drives
            label = DriveLabel::forPlace(places, index);
            mountUrl = deviceUrl;
        }
    }

    if (bestLength < 0) {
        // Nothing in the places model claims this path, which happens before
        // the model has finished populating
        return {tr("Local Disk"), QUrl::fromLocalFile(QStringLiteral("/"))};
    }
    return {label, mountUrl};
}

QList<BrowserNavigation::Crumb> BrowserNavigation::crumbsFor(const QUrl &url) const
{
    QList<Crumb> crumbs;
    if (Locations::isLibraries(url))
        return {{tr("Libraries"), Locations::libraries()}};
    crumbs.append({tr("Computer"), Locations::computer()});

    if (Locations::isComputer(url))
        return crumbs;

    // Search results hang off the folder that was searched
    if (Locations::isSearch(url)) {
        const QUrl folder = Locations::searchFolder(url);
        crumbs = crumbsFor(folder);
        QString where;
        if (!crumbs.isEmpty())
            where = crumbs.last().label;
        crumbs.append({tr("Search Results in %1").arg(where), url});
        return crumbs;
    }

    // An archive is browsed at the path of the archive file itself, so the
    // trail runs through the real folders and carries on into it
    if (Archives::isInsideArchive(url)) {
        const QUrl archiveFile = Archives::archiveFileFor(url);
        if (archiveFile.isValid()) {
            crumbs = crumbsFor(KIO::upUrl(archiveFile));
            QUrl walk = archiveFile;
            walk.setScheme(url.scheme());
            crumbs.append({archiveFile.fileName(), walk});

            // Whatever is left is the path inside the archive
            const QString inside = url.path().mid(archiveFile.path().length());
            const QStringList segments =
                inside.split(QLatin1Char('/'), Qt::SkipEmptyParts);
            for (const QString &segment : segments) {
                walk.setPath(walk.path() + QLatin1Char('/') + segment);
                crumbs.append({segment, walk});
            }
            return crumbs;
        }
    }

    // An elevated view is an ordinary path served by a different worker, so it
    // gets the ordinary trail, and the targets stay elevated since a crumb
    // dropping back would take the window's privileges away halfway along
    if (url.scheme() == QLatin1String("admin")) {
        QUrl local = url;
        local.setScheme(QStringLiteral("file"));
        crumbs = crumbsFor(local);
        for (Crumb &crumb : crumbs) {
            if (crumb.target.isLocalFile())
                crumb.target.setScheme(QStringLiteral("admin"));
        }
        return crumbs;
    }

    if (!url.isLocalFile()) {
        // Remote and virtual locations hang straight off Computer
        QString label = url.scheme();
        if (url.scheme() == QLatin1String("trash"))
            label = tr("Recycle Bin");
        crumbs.append({label, url});
        return crumbs;
    }

    const QString path = QDir::cleanPath(url.toLocalFile());

    Libraries::Entry library;
    int longest = -1;
    for (const auto &entry : Libraries::entries()) {
        const QString root = QDir::cleanPath(entry.url.toLocalFile());
        if ((path == root || path.startsWith(root + QLatin1Char('/')))
            && root.size() > longest) {
            library = entry;
            longest = root.size();
        }
    }
    if (library.url.isValid()) {
        crumbs = {{tr("Libraries"), Locations::libraries()}, {library.name, library.url}};
        QString walk = QDir::cleanPath(library.url.toLocalFile());
        for (const QString &segment : path.mid(walk.size()).split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
            walk += QLatin1Char('/') + segment;
            crumbs.append({Branding::displayName(QUrl::fromLocalFile(walk), segment),
                           QUrl::fromLocalFile(walk)});
        }
        return crumbs;
    }

    // Computer, then a drive, then folders, and home is walked through like
    // any other path rather than collapsed to a root of its own
    const Crumb drive = driveFor(path);
    crumbs.append(drive);

    // The rest of the path below the mount point, one folder at a time
    const QString mount = QDir::cleanPath(drive.target.toLocalFile());
    const bool mountIsRoot = (mount == QLatin1String("/"));
    QString walk;
    QString belowMount = path;
    if (!mountIsRoot) {
        walk = mount;
        belowMount = path.mid(mount.length());
    }

    const QStringList segments = belowMount.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString &segment : segments) {
        walk += QLatin1Char('/') + segment;
        // The label may be rewritten for friendly naming, the target never
        QString label = Branding::folderName(walk);
        if (label.isEmpty())
            label = segment;
        crumbs.append({label, QUrl::fromLocalFile(walk)});
    }
    return crumbs;
}

void BrowserNavigation::focusPathEdit(bool dropHistory)
{
    // Already editing, where Win7 selects again rather than doing nothing
    if (m_pathEdit) {
        m_pathEdit->setFocus();
        m_pathEdit->selectAll();
    } else {
        beginPathEdit();
    }

    if (dropHistory)
        showPathHistoryMenu();
}

void BrowserNavigation::showPathHistoryMenu()
{
    const QStringList paths = Settings::recentPaths();
    if (paths.isEmpty() || !m_pathBox)
        return;

    QMenu menu(m_host);
    for (const QString &path : paths) {
        QAction *action = menu.addAction(path);
        connect(action, &QAction::triggered, this, [this, path] {
            const QUrl target = QUrl::fromUserInput(path, QDir::homePath(),
                                                    QUrl::AssumeLocalFile);
            if (target.isValid())
                navigateTo(target);
        });
    }

    // Under the address box and the same width, the way Win7 drops it
    menu.setMinimumWidth(m_pathBox->width());
    // The editor keeps the keyboard, a menu taking focus for a reason the
    // event filter does not treat as clicking away
    menu.exec(m_pathBox->mapToGlobal(QPoint(0, m_pathBox->height())));
}

void BrowserNavigation::finishPathProgress()
{
    // Never shown, the folder having listed inside the delay
    if (!m_pathProgress->isVisible()) {
        stopPathProgress();
        return;
    }
    m_pathProgressTimer->stop();
    m_pathProgressFill->setStartValue(m_pathProgress->value());
    m_pathProgressFill->start();
}

void BrowserNavigation::stopPathProgress()
{
    m_pathProgressFill->stop();
    m_pathProgressTimer->stop();
    m_pathProgress->hide();
    // Emptied on the way out, or the next folder shows the last one's fill
    m_pathProgress->setValue(0);
}

void BrowserNavigation::layoutPathProgress()
{
    if (!m_pathProgress)
        return;
    m_pathProgress->setGeometry(
        m_pathBox->rect().adjusted(kPathProgressInset, kPathProgressInset,
                                   -kPathProgressInset, -kPathProgressInset));
    m_pathProgress->lower();
}

QIcon BrowserNavigation::locationIcon(const QUrl &url) const
{
    if (Locations::isComputer(url))
        return Aero::themeIcon({"computer", "computer-laptop"});
    if (Locations::isSearch(url))
        return Aero::themeIcon({"system-search", "folder-saved-search"});
    if (Locations::isLibraries(url))
        return Aero::themeIcon({"folder-library", "folder-documents", "folder"});

    // Prefer the places entry so the glyphs match the pane's, and the lookup
    // matches ancestors too, hence checking the location itself
    KFilePlacesModel *places = m_places;
    const QModelIndex index = places->closestItem(url);
    if (index.isValid() && places->url(index).matches(url, QUrl::StripTrailingSlash))
        return places->icon(index);

    return QIcon::fromTheme(KIO::iconNameForUrl(url), Aero::themeIcon({"folder"}));
}

void BrowserNavigation::clearPathLayout()
{
    // A location change (including a D-Bus reveal) must not leave a menu for
    // the old trail visible. Close it while its breadcrumb still exists.
    if (m_crumbMenu)
        m_crumbMenu->close();
    while (QLayoutItem *item = m_pathLayout->takeAt(0)) {
        if (QWidget *widget = item->widget()) {
            widget->deleteLater();
        }
        delete item;
    }
    // Restore breadcrumb padding; the editable field instead fills this
    // whole surface and keeps its text inset inside the editor itself.
    m_pathLayout->setContentsMargins(4, 0, 4, 0);
    // Cleared before the deferred deletes run, so anything reacting to the
    // editor losing focus sees edit mode as already finished
    m_pathEdit = nullptr;
}

void BrowserNavigation::beginPathEdit()
{
    if (m_pathEdit)
        return;

    const QUrl url = currentUrl();
    clearPathLayout();

    // An elevated location edits as the plain path it is, and what the user
    // types goes back through the same worker below
    const bool elevated = url.scheme() == QLatin1String("admin");
    QString editText = url.toString();
    if (url.isLocalFile() || elevated)
        editText = displayPath(url);
    m_pathEdit = new QLineEdit(editText);
    m_pathEdit->installEventFilter(this);
    m_pathEdit->setObjectName(QStringLiteral("pathEditor"));
    m_pathEdit->setAccessibleName(tr("Address bar"));
    m_pathEdit->setFrame(false);
    m_pathEdit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    Aero::setPointSize(m_pathEdit, 9);
    // Editing is one opaque surface up to the refresh separator. Keep the
    // text padding inside QLineEdit, not as unpainted layout margins, and
    // suppress the inherited glass border-image rather than nesting frames.
    m_pathLayout->setContentsMargins(0, 0, 0, 0);
    m_pathEdit->setTextMargins(4, 0, 4, 0);
    m_pathEdit->setStyleSheet(
        QStringLiteral("#pathEditor { background: white; border: none;"
                       " border-image: none; padding: 0; color: %1; }")
            .arg(QLatin1String(Aero::Palette::Text)));
    m_pathLayout->addWidget(m_pathEdit, 1);

    // A filesystem model rather than a completion object, the completer needing
    // a model to pop up from and this one reading a directory only when the
    // typed text reaches it
    auto *fsModel = new QFileSystemModel(m_pathEdit);
    fsModel->setRootPath(QStringLiteral("/"));
    fsModel->setFilter(QDir::Dirs | QDir::Drives | QDir::NoDotAndDotDot);
    auto *completer = new QCompleter(fsModel, m_pathEdit);
    completer->setCompletionMode(QCompleter::PopupCompletion);
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    m_pathEdit->setCompleter(completer);

    connect(m_pathEdit, &QLineEdit::returnPressed, this, [this] {
        QString text = m_pathEdit->text().trimmed();
        if (text.isEmpty()) {
            endPathEdit();
            return;
        }
        if (text.startsWith(QLatin1Char('~')))
            text.replace(0, 1, QDir::homePath());

        QUrl target = QUrl::fromUserInput(text, QDir::homePath(),
                                          QUrl::AssumeLocalFile);

        if (!accepts(target)) {
            endPathEdit();
            return;
        }

        // Or a nonexistent path pushes a history entry and shows nothing at all
        if (target.isLocalFile()) {
            const QFileInfo info(target.toLocalFile());
            if (info.isFile()) {
                // A file rather than a folder, which Win7 opens and stays put
                endPathEdit();
                emit typedFileActivated(target);
                return;
            }
            if (!info.isDir()) {
                emit errorOccurred(tr("Windows can't find '%1'. Check the "
                                          "spelling and try again.").arg(text));
                endPathEdit();
                return;
            }
        }

        // A path typed into an elevated window stays elevated, as its crumbs do.
        if (target.isLocalFile() && currentUrl().scheme() == QLatin1String("admin"))
            target.setScheme(QStringLiteral("admin"));
        if (!accepts(target)) {
            endPathEdit();
            return;
        }
        Settings::addRecentPath(text);

        // Navigating rebuilds the trail and destroys the editor, so when it
        // declines to move the trail has to be put back by hand
        const bool movesSomewhere = target.isValid()
            && !target.matches(currentUrl(), QUrl::StripTrailingSlash);
        if (!movesSomewhere || !navigateTo(target))
            endPathEdit();
    });

    // Layout insertion queues show() for later. Show now so F6/Alt+D
    // actually transfer the keyboard instead of leaving it on the file list.
    m_pathEdit->show();
    m_pathEdit->setFocus();
    m_pathEdit->selectAll();
}

void BrowserNavigation::endPathEdit()
{
    if (!m_pathEdit)
        return;
    setCrumbTrail(currentUrl());
}

void BrowserNavigation::setCrumbTrail(const QUrl &url)
{
    clearPathLayout();

    auto *bar = new BreadcrumbBar;
    QList<BreadcrumbBar::Segment> segments;
    for (const Crumb &crumb : crumbsFor(url))
        segments.append({crumb.label, crumb.target});
    bar->setTrail(locationIcon(url), segments);
    connect(bar, &BreadcrumbBar::urlActivated, this,
            [this](const QUrl &target) { navigateTo(target); });
    connect(bar, &BreadcrumbBar::menuRequested, this, &BrowserNavigation::showCrumbMenu);
    m_pathLayout->addWidget(bar, 1);
}

void BrowserNavigation::showCrumbMenu(const QUrl &parent, const QPoint &globalPos)
{
    QMenu menu(m_host);
    const QPointer<BreadcrumbBar> bar(qobject_cast<BreadcrumbBar *>(sender()));
    connect(&menu, &QMenu::aboutToShow, &menu, [bar, parent] {
        if (bar) bar->setPopupOpen(parent, true);
    });
    connect(&menu, &QMenu::aboutToHide, &menu, [bar, parent] {
        if (bar) bar->setPopupOpen(parent, false);
    });

    if (!parent.isValid()) {
        // Hidden ancestors remain reachable in the leading dropdown.
        for (const Crumb &crumb : crumbsFor(currentUrl())) {
            menu.addAction(locationIcon(crumb.target), crumb.label, this,
                           [this, target = crumb.target] { navigateTo(target); });
        }
        menu.addSeparator();
        menu.addAction(locationIcon(Locations::libraries()), tr("Libraries"),
                       this, [this] { navigateTo(Locations::libraries()); });
        menu.addAction(Aero::themeIcon({"computer", "computer-laptop"}), tr("Computer"),
                       this, [this] { navigateTo(Locations::computer()); });
        menu.addSeparator();
        KFilePlacesModel *places = m_places;
        for (int row = 0; row < places->rowCount(); ++row) {
            const QModelIndex index = places->index(row, 0);
            if (places->isHidden(index) || places->setupNeeded(index))
                continue;
            const QUrl url = places->url(index);
            menu.addAction(places->icon(index), DriveLabel::forPlace(places, index),
                           this, [this, url] { navigateTo(url); });
        }
    } else if (Locations::isLibraries(parent)) {
        for (const auto &entry : Libraries::entries()) {
            menu.addAction(QIcon::fromTheme(entry.iconName), entry.name, this,
                           [this, url = entry.url] { navigateTo(url); });
        }
    } else if (Locations::isComputer(parent)) {
        for (int row = 0; row < m_computerModel->rowCount(); ++row) {
            const QModelIndex index = m_computerModel->index(row, ComputerModel::Name);
            const QUrl url = index.data(ComputerModel::UrlRole).toUrl();
            if (!url.isValid())
                continue;
            menu.addAction(qvariant_cast<QIcon>(index.data(Qt::DecorationRole)),
                           index.data(Qt::DisplayRole).toString(), this,
                           [this, url] { navigateTo(url); });
        }
    } else if (parent.isLocalFile()) {
        const bool showHidden = (m_showHiddenFiles ? m_showHiddenFiles() : false);
        QDir::Filters filters = QDir::Dirs | QDir::NoDotAndDotDot;
        if (showHidden)
            filters |= QDir::Hidden;

        QDir dir(parent.toLocalFile());
        dir.setFilter(filters);
        dir.setSorting(QDir::Name | QDir::LocaleAware | QDir::IgnoreCase);

        const QIcon folderIcon = Aero::themeIcon({"folder"});
        const QFileInfoList entries = dir.entryInfoList();
        const QString here = QDir::cleanPath(currentUrl().toLocalFile());
        int looked = 0;
        for (const QFileInfo &entry : entries) {
            if (looked >= kMaxCrumbMenuEntries)
                break;
            ++looked;

            const QString path = entry.absoluteFilePath();
            if (Branding::isSystemFolder(path) && !showHidden)
                continue;

            const QUrl url = QUrl::fromLocalFile(path);
            const QString label = Branding::displayName(url, entry.fileName());
            QAction *action = menu.addAction(folderIcon, label, this,
                                             [this, url] { navigateTo(url); });

            // Ticked when this is the folder on show or one of its ancestors,
            // a whole segment at a time, since a bare prefix would tick a
            // folder whenever you were anywhere under a longer name
            const QString candidate = QDir::cleanPath(path);
            const bool onCurrentPath = here == candidate
                || here.startsWith(candidate + QLatin1Char('/'));
            action->setCheckable(true);
            action->setChecked(onCurrentPath);
        }
    }

    if (menu.isEmpty())
        return;
    if (m_crumbMenu)
        m_crumbMenu->close();
    m_crumbMenu = &menu; // QPointer clears when this stack menu is destroyed.
    menu.exec(globalPos);
}

void BrowserNavigation::goUp()
{
    const QUrl current = currentUrl();
    if (Locations::isComputer(current) || Locations::isLibraries(current))
        return;
    if (Libraries::forUrl(current).url.isValid()) {
        navigateTo(Locations::libraries());
        return;
    }

    // The folder that was searched is the only sensible parent for results
    if (Locations::isSearch(current)) {
        navigateTo(Locations::searchFolder(current));
        return;
    }

    // Up out of an archive's root lands in the folder the archive sits in, the
    // ordinary walk carrying on and producing a location nothing can list
    if (Archives::isInsideArchive(current)) {
        const QUrl archiveFile = Archives::archiveFileFor(current);
        if (archiveFile.isValid()
            && QDir::cleanPath(current.path())
                   == QDir::cleanPath(archiveFile.path())) {
            navigateTo(KIO::upUrl(archiveFile));
            return;
        }
    }

    // The ordinary walk stops at the filesystem root, where Win7 goes on to
    // Computer instead
    if (current.isLocalFile()
        && QDir::cleanPath(current.toLocalFile()) == QLatin1String("/")) {
        navigateTo(Locations::computer());
        return;
    }

    const QUrl parent = KIO::upUrl(current);
    if (parent.isValid() && !parent.matches(current, QUrl::StripTrailingSlash))
        navigateTo(parent);
}

void BrowserNavigation::pushHistory(const QUrl &url)
{
    // Navigating to the current location again must not grow the history
    if (m_historyIndex >= 0 && m_history.at(m_historyIndex) == url)
        return;
    // Drop any forward entries before branching to a new location
    while (m_history.size() > m_historyIndex + 1)
        m_history.removeLast();
    m_history.append(url);
    m_historyIndex = m_history.size() - 1;
    updateNavButtons();
}

void BrowserNavigation::updateNavButtons()
{
    if (m_backBtn)
        m_backBtn->setEnabled(m_historyIndex > 0);
    if (m_forwardBtn)
        m_forwardBtn->setEnabled(m_historyIndex < m_history.size() - 1);
}

QString BrowserNavigation::displayPath(const QUrl &url)
{
    // The worker's own location is plumbing, and nothing to put in a message
    QUrl local = url;
    if (local.scheme() == QLatin1String("admin"))
        local.setScheme(QStringLiteral("file"));
    return local.toDisplayString(QUrl::PreferLocalFile);
}

void BrowserNavigation::installShortcuts(QWidget *host, bool includeUpAndRefresh)
{
    const auto add = [this, host](const QList<QKeySequence> &keys, auto slot) {
        auto *action = new QAction(this);
        action->setShortcuts(keys);
        connect(action, &QAction::triggered, this, slot);
        host->addAction(action);
    };
    // Text editors claim Backspace through ShortcutOverride before this action.
    add({QKeySequence(QKeySequence::Back), QKeySequence(Qt::Key_Backspace)},
        [this] { goBack(); });
    add({QKeySequence(QKeySequence::Forward)}, [this] { goForward(); });
    add({QKeySequence(Qt::CTRL | Qt::Key_L), QKeySequence(Qt::ALT | Qt::Key_D)},
        [this] { focusPathEdit(false); });
    add({QKeySequence(Qt::Key_F4)}, [this] { focusPathEdit(true); });
    if (includeUpAndRefresh) {
        add({QKeySequence(Qt::ALT | Qt::Key_Up)}, [this] { goUp(); });
        add({QKeySequence(Qt::Key_F5)}, [this] { emit refreshRequested(); });
    }
}

bool BrowserNavigation::accepts(const QUrl &url)
{
    if (!url.isValid())
        return false;
    const QString error = m_urlPolicy ? m_urlPolicy(url) : QString();
    if (error.isEmpty())
        return true;
    emit errorOccurred(error);
    return false;
}

bool BrowserNavigation::initialize(const QUrl &url)
{
    if (!accepts(url))
        return false;
    m_history.clear();
    m_historyIndex = -1;
    pushHistory(url);
    setCrumbTrail(url);
    emit locationChanged(url);
    return true;
}

QUrl BrowserNavigation::currentUrl() const
{
    return m_historyIndex < 0 ? QUrl() : m_history.at(m_historyIndex);
}

bool BrowserNavigation::navigateTo(const QUrl &url, bool announceNavigation)
{
    if (url.matches(currentUrl(), QUrl::StripTrailingSlash) || !accepts(url))
        return false;
    pushHistory(url);
    setCrumbTrail(url);
    if (announceNavigation)
        emit navigated();
    emit locationChanged(url);
    return true;
}

void BrowserNavigation::goToHistoryIndex(int index)
{
    if (index < 0 || index >= m_history.size() || !accepts(m_history.at(index)))
        return;
    m_historyIndex = index;
    updateNavButtons();
    setCrumbTrail(currentUrl());
    emit navigated();
    emit locationChanged(currentUrl());
}

void BrowserNavigation::goBack()
{
    goToHistoryIndex(m_historyIndex - 1);
}

void BrowserNavigation::goForward()
{
    goToHistoryIndex(m_historyIndex + 1);
}

void BrowserNavigation::refreshTrail()
{
    if (!m_pathEdit)
        setCrumbTrail(currentUrl());
}

void BrowserNavigation::startPathProgress()
{
    m_pathProgressFill->stop();
    m_pathProgress->hide();
    m_pathProgress->setValue(0);
    m_pathProgressTimer->start();
}

void BrowserNavigation::showPathProgress()
{
    layoutPathProgress();
    m_pathProgress->show();
}

void BrowserNavigation::setPathProgress(int percent)
{
    m_pathProgress->setValue(qMax(m_pathProgress->value(), percent));
}

bool BrowserNavigation::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_pathBox && event->type() == QEvent::MouseButtonPress) {
        beginPathEdit();
        return true;
    }
    if (watched == m_pathBox && event->type() == QEvent::Resize)
        layoutPathProgress();
    if (m_pathEdit && watched == m_pathEdit) {
        if (event->type() == QEvent::KeyPress
            && static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape) {
            endPathEdit();
            return true;
        }
        // Completion/history popups do not cancel the editor; clicking away does.
        if (event->type() == QEvent::FocusOut
            && static_cast<QFocusEvent *>(event)->reason() != Qt::PopupFocusReason) {
            endPathEdit();
            return true;
        }
    }
    return QObject::eventFilter(watched, event);
}
