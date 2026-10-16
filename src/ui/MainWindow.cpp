#include "MainWindow.h"
#include "BrowserNavigation.h"
#include "Assets.h"
#include "AccessDialogs.h"
#include "Branding.h"
#include "ComputerModel.h"
#include "ComputerView.h"
#include "DetailsPane.h"
#include "DirectoryModel.h"
#include "AboutDialog.h"
#include "HelpDialog.h"
#include "Archives.h"
#include "SearchBox.h"
#include "Libraries.h"
#include "LibrariesView.h"
#include "ViewModeMenu.h"
#include "FileOps.h"
#include "FileView.h"
#include "aero/icons.h"
#include "Locations.h"
#include "MapDriveDialog.h"
#include "MountDialog.h"
#include "OptionsDialog.h"
#include "NavigationPane.h"
#include "PreviewPane.h"
#include "Settings.h"
#include "aero/arrows.h"
#include "aero/buttons.h"
#include "aero/splitter.h"
#include "aero/strips.h"
#include "aero/text.h"
#include "aero/artwork.h"

#include <KDirModel>
#include <KFileItemActions>
#include <KFileItemListProperties>
#include <KFilePlacesModel>
#include <KIO/FileUndoManager>
#include <KIO/Global>
#include <KNewFileMenu>
#include <KProtocolInfo>
#include <KWindowSystem>
#include <KMessageWidget>

#include <AeroQt/insetwindow.h>

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QCursor>
#include <QDir>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMouseEvent>
#include <QPushButton>
#include <QScreen>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWindow>

#include <algorithm>

namespace {

// The gap Win7 leaves between the navigation pane and the file list
constexpr int kListLeftMargin = 8;

// Long enough that ordinary local folders never show the placeholder at all
constexpr int kLoadingPlaceholderDelay = 400;

constexpr int kNotificationDelay = 1000;
constexpr int kNotificationSlideDuration = 180;

// In creation order, and owning nothing, windows deleting themselves on close
QList<MainWindow *> g_windows;

} // namespace

int MainWindow::openWindowCount()
{
    return g_windows.size();
}

MainWindow *MainWindow::openWindow(const QUrl &folder, const QList<QUrl> &selection,
                                   const QString &startupId)
{
    QUrl target = folder;
    if (!target.isValid())
        target = Locations::libraries();

    // Two reveal requests for the same folder should end up as one window with
    // both files showing
    MainWindow *window = nullptr;
    for (MainWindow *candidate : std::as_const(g_windows)) {
        if (candidate->currentUrl().matches(target, QUrl::StripTrailingSlash)) {
            window = candidate;
            break;
        }
    }
    if (!window) {
        window = new MainWindow(target);
        window->show();
    }

    if (!selection.isEmpty())
        window->selectOnArrival(selection);

    // Without the caller's token the window is stacked on top but left
    // unfocused, covering what the user was doing and still refusing their keys
    if (!startupId.isEmpty())
        KWindowSystem::setCurrentXdgActivationToken(startupId);
    window->show();
    window->raise();
    if (QWindow *handle = window->windowHandle())
        KWindowSystem::activateWindow(handle);

    return window;
}

MainWindow::MainWindow(const QUrl &startUrl, QWidget *parent)
    : QMainWindow(parent)
    , m_model(new DirectoryModel(this))
    , m_places(new NavigationPane(this))
{
    // From the navigation pane's places model, so the two agree about which
    // drives exist
    m_computerModel = new ComputerModel(m_places->placesModel(), this);

    g_windows.append(this);

    setAttribute(Qt::WA_DeleteOnClose);
    setMinimumSize(480, 360);
    QSize initialSize(940, 620);
    if (QScreen *screen = this->screen())
        initialSize = initialSize.boundedTo(screen->availableGeometry().size() - QSize(24, 48));
    resize(initialSize.expandedTo(minimumSize()));

    m_navSound.setSource(QUrl::fromLocalFile(
        "/usr/share/sounds/Windows 7/og/Windows Navigation Start.wav"));
    m_navSound.setVolume(1.0f);

    buildNavigationBar();

    auto *central = new QWidget;
    // ID scoped, a declaration only sheet matching everything and dragging
    // every descendant, scroll bars included, into the stylesheet engine
    central->setObjectName("explorerCentral");
    central->setStyleSheet(Aero::panelSheet(central->objectName(), Aero::Palette::Surface));

    auto *root = new QVBoxLayout(central);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // Before the actions and the command bar, which both refer to the file view
    QWidget *body = buildBody();
    buildActions();

    root->addWidget(buildCommandBar());
    root->addWidget(buildSearchAgainBar());
    root->addWidget(buildNotificationBar());
    root->addWidget(body, 1);

    m_details = new DetailsPane;
    root->addWidget(m_details);

    setCentralWidget(central);
    statusBar()->hide();

    // Hidden until Alt is pressed, unless the setting says otherwise
    buildMenuBar();
    menuBar()->setVisible(Settings::alwaysShowMenus());

    // After the central widget is set, and the bar must not also be in that
    // layout, the inset call placing it in the frame's glass area itself
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    Aero::makeInsetWindow(this, central, m_navBar, nullptr);

    setupShortcuts();

    // The whole application, so the side buttons navigate wherever the pointer
    // is, the file list and navigation pane consuming presses themselves
    qApp->installEventFilter(this);

    connect(m_places, &NavigationPane::urlActivated, this, &MainWindow::navigateTo);
    connect(m_places, &NavigationPane::newWindowRequested,
            this, &MainWindow::openNewWindow);
    connect(m_places, &NavigationPane::emptyTrashRequested, this,
            [this] { FileOps::emptyTrash(this); });
    connect(m_places, &NavigationPane::connectDrivesRequested,
            this, &MainWindow::showMountDialog);
    connect(m_places, &NavigationPane::mapDriveRequested,
            this, &MainWindow::showMapDriveDialog);
    connect(m_places, &NavigationPane::dropped, this, &MainWindow::handleDrop);

    m_loadingTimer = new QTimer(this);
    m_loadingTimer->setSingleShot(true);
    m_loadingTimer->setInterval(kLoadingPlaceholderDelay);
    connect(m_loadingTimer, &QTimer::timeout, this, [this] {
        // The listing may have finished while the timer was running
        if (!m_loading)
            return;
        if (m_stack->currentWidget() == m_loadingPage)
            m_loadingLabel->show();
        // Same delay as the placeholder, and for the same reason
        m_navigation->showPathProgress();
    });

    connect(m_model, &DirectoryModel::loadingStarted, this, [this] {
        m_loading = true;
        m_lastLoadFailed = false;
        invalidateSelection();

        // The page swaps in at once and only the label waits, since leaving the
        // list up means watching the batches trickle in and only then being
        // replaced by a placeholder
        m_loadingLabel->hide();
        if (!isComputerView() && !isLibrariesView())
            m_stack->setCurrentWidget(m_loadingPage);
        m_loadingTimer->start();

        // Already creeping before the bar is shown, so it arrives part filled
        m_navigation->startPathProgress();
        updateListMessage();
    });
    connect(m_model, &DirectoryModel::loadingProgress, this, [this](int percent) {
        // Only forwards, the timer possibly having carried the bar past what
        // the lister reports
        m_navigation->setPathProgress(percent);
    });
    connect(m_model, &DirectoryModel::loadingFinished, this, [this] {
        m_loading = false;
        m_loadingTimer->stop();
        invalidateSelection();
        // Runs out to full before hiding rather than cutting off partway
        m_navigation->finishPathProgress();
        if (m_stack->currentWidget() == m_loadingPage)
            m_stack->setCurrentWidget(m_listPage);
        // Last chance for a reveal naming something not in this folder
        applyPendingSelection(true);
        updateDetailsPane();
        updateListMessage();
    });

    // A folder created through the new menu goes into rename mode once listed,
    // creating one returning before the lister has seen it
    connect(m_model, &DirectoryModel::itemsAdded, this, [this] {
        invalidateSelection();
        applyPendingSelection(false);
    });

    // A rename can update an existing selected row without a selectionChanged
    // signal. Cached KFileItems must not leave Copy/Properties on the old URL.
    connect(m_model->model(), &QAbstractItemModel::dataChanged, this,
            [this](const QModelIndex &first, const QModelIndex &last) {
        invalidateSelection();
        if (isComputerView() || isLibrariesView()) return;
        const QModelIndexList selected = m_fileView->selectedIndexes();
        const bool selectionChanged = std::any_of(selected.cbegin(), selected.cend(),
            [&first, &last](const QModelIndex &index) {
                return index.parent() == first.parent()
                    && index.row() >= first.row() && index.row() <= last.row();
            });
        if (!selectionChanged) return; // avoid rebuilding chrome for unrelated thumbnails
        updateDetailsPane();
        updateActionStates();
        rebuildContextualCommands();
    });

    // The places model populates asynchronously, so on a cold start the drives
    // land after Computer was summarised and the pane would report none
    connect(m_computerModel, &QAbstractItemModel::modelReset, this, [this] {
        if (isComputerView())
            updateDetailsPane();
        // Connecting a drive removes it from the hidden tally
        updateNotification();
    });

    connect(m_model, &DirectoryModel::errorOccurred, this,
            [this](int error, const QString &message, const QUrl &url) {
        if (!url.matches(currentUrl(), QUrl::StripTrailingSlash))
            return;
        // An unreadable directory never completes, so the placeholder has to
        // be torn down here too
        m_loading = false;
        m_lastLoadFailed = true;
        m_loadingTimer->stop();
        m_navigation->stopPathProgress();
        if (m_stack->currentWidget() == m_loadingPage)
            m_stack->setCurrentWidget(m_listPage);
        m_details->showMessage(message);
        updateListMessage();
        reportListingFailure(error, url);
    });

    // Named after whatever it will reverse
    auto *undoManager = KIO::FileUndoManager::self();
    connect(undoManager, &KIO::FileUndoManager::undoAvailable, this,
            [this](bool available) { m_actUndo->setEnabled(available); });
    connect(undoManager, &KIO::FileUndoManager::undoTextChanged, this,
            [this](const QString &text) {
        if (text.isEmpty())
            m_actUndo->setText(tr("Undo"));
        else
            m_actUndo->setText(text);
    });

    const QByteArray geometry = Settings::windowGeometry();
    if (!geometry.isEmpty())
        restoreGeometry(geometry);
    const QByteArray splitter = Settings::splitterState();
    if (!splitter.isEmpty())
        m_splitter->restoreState(splitter);
    m_fileView->setHeaderState(Settings::headerState());

    // Options the views hold rather than read, so a fresh window agrees with
    // the dialog
    m_fileView->setSingleClickToOpen(Settings::singleClickToOpen());

    // Not a navigation, opening a window not counting as one
    m_navigation->initialize(startUrl);
}

// The window class a decoration theme matches on is the app id, which Qt reads
// from the desktop file name as the toplevel is made, and that happens inside
// this call rather than at construction, so the name is swapped around it and
// put back for the dialogs
void MainWindow::setVisible(bool visible)
{
    if (!visible) {
        QMainWindow::setVisible(false);
        return;
    }

    const QString dialogs = QGuiApplication::desktopFileName();
    QGuiApplication::setDesktopFileName(QStringLiteral("linux-explorer-win7"));
    QMainWindow::setVisible(true);
    QGuiApplication::setDesktopFileName(dialogs);
}

MainWindow::~MainWindow()
{
    g_windows.removeOne(this);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    Settings::setWindowGeometry(saveGeometry());
    Settings::setSplitterState(m_splitter->saveState());
    Settings::setHeaderState(m_fileView->headerState());
    QMainWindow::closeEvent(event);
}

void MainWindow::buildActions()
{
    // Scoped to the file view rather than the window, a window wide Delete
    // firing with the navigation pane focused and trashing the list's selection
    const auto fileAction = [this](const QString &text, auto slot,
                                   const QKeySequence &key = {}) {
        auto *action = new QAction(text, m_fileView);
        if (!key.isEmpty()) {
            action->setShortcut(key);
            action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
            // Parenting only gives ownership, a shortcut being live only once
            // the action is in some widget's own list
            m_fileView->addAction(action);
        }
        connect(action, &QAction::triggered, this, slot);
        return action;
    };

    // About the window rather than the selection, so they work whatever has
    // the focus
    const auto windowAction = [this](const QString &text, auto slot,
                                     const QKeySequence &key = {}) {
        auto *action = new QAction(text, this);
        if (!key.isEmpty()) {
            action->setShortcut(key);
            addAction(action);
        }
        connect(action, &QAction::triggered, this, slot);
        return action;
    };

    m_actOpen = fileAction(tr("Open"), [this] {
        if (isLibrariesView()) {
            const QUrl url = m_librariesView->selectedUrl();
            if (url.isValid()) navigateTo(url);
        } else {
            openItems(selectedItems());
        }
    });
    m_actOpenNewWindow = fileAction(tr("Open in new window"), [this] {
        if (isLibrariesView()) {
            const QUrl url = m_librariesView->selectedUrl();
            if (url.isValid()) openNewWindow(url);
        } else {
            openInNewWindow(selectedItems());
        }
    });
    m_actOpenWith = fileAction(tr("Open with..."), [this] {
        FileOps::openWith(selectedItems(), this);
    });
    m_actCut = fileAction(tr("Cut"), [this] {
        FileOps::cutToClipboard(selectedItems());
    }, QKeySequence::Cut);
    m_actCopy = fileAction(tr("Copy"), [this] {
        FileOps::copyToClipboard(selectedItems());
    }, QKeySequence::Copy);
    m_actPaste = fileAction(tr("Paste"), [this] {
        if (!isComputerView() && !isLibrariesView())
            FileOps::pasteFromClipboard(operationFolder(), this);
    }, QKeySequence::Paste);
    m_actCopyPath = fileAction(tr("Copy as path"), [this] {
        FileOps::copyPathToClipboard(selectedItems());
    }, QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C));
    m_actCreateLink = fileAction(tr("Create shortcut"), [this] {
        FileOps::createLink(selectedUrls(), operationFolder(), this);
    });
    m_actDelete = fileAction(tr("Delete"), [this] {
        FileOps::moveToTrash(selectedUrls(), this);
    }, QKeySequence(Qt::Key_Delete));
    m_actDeleteForever = fileAction(tr("Delete permanently"), [this] {
        FileOps::deletePermanently(selectedUrls(), this);
    }, QKeySequence(Qt::SHIFT | Qt::Key_Delete));
    m_actRename = fileAction(tr("Rename"), &MainWindow::renameSelection,
                             QKeySequence(Qt::Key_F2));
    m_actProperties = fileAction(tr("Properties"), [this] {
        const QList<KFileItem> selection = selectedItems();
        // With nothing selected Win7 reports on the current folder
        if (selection.isEmpty()) {
            if (!isComputerView() && !isLibrariesView())
                FileOps::showProperties({KFileItem(operationFolder())}, this);
        } else {
            FileOps::showProperties(selection, this);
        }
    }, QKeySequence(Qt::ALT | Qt::Key_Return));
    m_actRestore = fileAction(tr("Restore"), [this] {
        FileOps::restoreFromTrash(selectedUrls(), this);
    });
    m_actRestoreAll = windowAction(tr("Restore all items"), [this] {
        QList<QUrl> urls;
        QAbstractItemModel *listing = m_model->model();
        urls.reserve(listing->rowCount());
        for (int row = 0; row < listing->rowCount(); ++row) {
            const KFileItem item = m_model->itemForIndex(listing->index(row, 0));
            if (!item.isNull())
                urls.append(item.url());
        }
        FileOps::restoreFromTrash(urls, this);
    });
    m_actSelectAll = fileAction(tr("Select all"), [this] {
        m_fileView->selectAll();
    }, QKeySequence::SelectAll);
    m_actInvertSelection = fileAction(tr("Invert selection"), [this] {
        m_fileView->invertSelection();
    });

    m_actUndo = windowAction(tr("Undo"), [] { FileOps::undo(); }, QKeySequence::Undo);
    m_actUndo->setEnabled(FileOps::isUndoAvailable());

    m_actRefresh = windowAction(tr("Refresh"), &MainWindow::refresh,
                                QKeySequence(Qt::Key_F5));
    m_actEmptyTrash = windowAction(tr("Empty Recycle Bin"), [this] {
        FileOps::emptyTrash(this);
    });
    m_actOpenTerminal = windowAction(tr("Open in Terminal"), [this] {
        FileOps::openTerminalAt(operationFolder(), this);
    }, QKeySequence(Qt::SHIFT | Qt::Key_F4));
    m_actNewWindow = windowAction(tr("New window"), [this] {
        openNewWindow(currentUrl());
    }, QKeySequence::New);
    m_actClose = windowAction(tr("Close"), [this] { close(); }, QKeySequence::Close);
    m_actUp = windowAction(tr("Up one level"), &MainWindow::goUp,
                           QKeySequence(Qt::ALT | Qt::Key_Up));
    m_actConnectDrives = windowAction(tr("Connect drives..."),
                                      &MainWindow::showMountDialog);
    m_actMapDrive = windowAction(tr("Map network drive..."),
                                 &MainWindow::showMapDriveDialog);
    m_actExtract = windowAction(tr("Extract All..."), &MainWindow::extractSelection);
    m_actOptions = windowAction(tr("Folder and search options"),
                                &MainWindow::showOptionsDialog);
    m_actOpenAsAdmin = windowAction(tr("Run as Administrator"),
                                    &MainWindow::openAsAdministrator);

    m_actHelp = windowAction(tr("View help"), &MainWindow::showHelp,
                              QKeySequence(Qt::Key_F1));
    m_actHelp->setIcon(Aero::helpIcon());
    m_actHelp->setToolTip(tr("Get help (F1)"));

    m_actPreviewPane = windowAction(tr("Preview pane"), [this](bool on) {
        m_actPreviewPane->setToolTip(on ? tr("Hide the preview pane (Alt+P)")
                                      : tr("Show the preview pane (Alt+P)"));
        m_previewPane->setVisible(on);
        if (on)
            m_previewPane->setItems(selectedItems());
    }, QKeySequence(Qt::ALT | Qt::Key_P));
    m_actPreviewPane->setCheckable(true);
    m_actPreviewPane->setIcon(Aero::previewPaneIcon());
    m_actPreviewPane->setToolTip(tr("Show the preview pane (Alt+P)"));

    m_actShowHidden = windowAction(tr("Show hidden files"), [this](bool on) {
        m_model->setShowHiddenFiles(on);
    }, QKeySequence(Qt::CTRL | Qt::Key_H));
    m_actShowHidden->setCheckable(true);
    m_actShowHidden->setChecked(Settings::showHiddenFiles());

    m_actHideExtensions = windowAction(tr("Hide extensions for known file types"),
                                       [this](bool on) {
        Settings::setHideKnownExtensions(on);
        m_model->refreshDisplayNames();
    });
    m_actHideExtensions->setCheckable(true);
    m_actHideExtensions->setChecked(Settings::hideKnownExtensions());

    m_actUseCheckBoxes = windowAction(tr("Use check boxes to select items"),
                                      [this](bool on) {
        Settings::setUseCheckBoxes(on);
        m_fileView->setCheckBoxesVisible(on);
    });
    m_actUseCheckBoxes->setCheckable(true);
    m_actUseCheckBoxes->setChecked(Settings::useCheckBoxes());

    // In Win7's own order, on Win7's own shortcuts
    m_viewModeGroup = new QActionGroup(this);
    const std::pair<const char *, Settings::ViewMode> modes[] = {
        {QT_TR_NOOP("Extra large icons"), Settings::ViewMode::ExtraLargeIcons},
        {QT_TR_NOOP("Large icons"),       Settings::ViewMode::LargeIcons},
        {QT_TR_NOOP("Medium icons"),      Settings::ViewMode::MediumIcons},
        {QT_TR_NOOP("Small icons"),       Settings::ViewMode::SmallIcons},
        {QT_TR_NOOP("List"),              Settings::ViewMode::List},
        {QT_TR_NOOP("Details"),           Settings::ViewMode::Details},
        {QT_TR_NOOP("Tiles"),             Settings::ViewMode::Tiles},
        {QT_TR_NOOP("Content"),           Settings::ViewMode::Content},
    };
    int index = 0;
    for (const auto &[label, mode] : modes) {
        // Ctrl+Shift+1 for the first mode, Ctrl+Shift+2 for the next, and so on
        const Qt::Key digit = Qt::Key(Qt::Key_1 + index);

        auto *action = new QAction(tr(label), this);
        action->setCheckable(true);
        action->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | digit));
        action->setData(int(mode));
        connect(action, &QAction::triggered, this, [this, mode] { setViewMode(mode); });
        m_viewModeGroup->addAction(action);
        addAction(action);
        ++index;
    }

    m_sortGroup = new QActionGroup(this);
    const std::pair<const char *, int> sorts[] = {
        {QT_TR_NOOP("Name"),          DirectoryModel::Name},
        {QT_TR_NOOP("Date modified"), DirectoryModel::ModifiedTime},
        {QT_TR_NOOP("Type"),          DirectoryModel::Type},
        {QT_TR_NOOP("Size"),          DirectoryModel::Size},
    };
    for (const auto &[label, column] : sorts) {
        auto *action = new QAction(tr(label), this);
        action->setCheckable(true);
        action->setData(column);
        connect(action, &QAction::triggered, this, [this, column] {
            m_fileView->sortBy(column, m_model->sortOrder());
        });
        m_sortGroup->addAction(action);
    }

    // Separate from the sort actions, a listing being able to group by type
    // and sort by date at once
    m_groupGroup = new QActionGroup(this);
    const std::pair<const char *, int> groups[] = {
        {QT_TR_NOOP("(None)"),        -1},
        {QT_TR_NOOP("Name"),          DirectoryModel::Name},
        {QT_TR_NOOP("Date modified"), DirectoryModel::ModifiedTime},
        {QT_TR_NOOP("Type"),          DirectoryModel::Type},
        {QT_TR_NOOP("Size"),          DirectoryModel::Size},
    };
    for (const auto &[label, column] : groups) {
        auto *action = new QAction(tr(label), this);
        action->setCheckable(true);
        action->setChecked(column == -1);
        action->setData(column);
        connect(action, &QAction::triggered, this, [this, column] {
            m_fileView->setGroupColumn(column);
        });
        m_groupGroup->addAction(action);
    }

    auto *orderGroup = new QActionGroup(this);
    m_sortAscending = new QAction(tr("Ascending"), this);
    m_sortDescending = new QAction(tr("Descending"), this);
    for (QAction *action : {m_sortAscending, m_sortDescending}) {
        action->setCheckable(true);
        orderGroup->addAction(action);
    }
    connect(m_sortAscending, &QAction::triggered, this, [this] {
        m_fileView->sortBy(m_model->sortColumn(), Qt::AscendingOrder);
    });
    connect(m_sortDescending, &QAction::triggered, this, [this] {
        m_fileView->sortBy(m_model->sortColumn(), Qt::DescendingOrder);
    });

    // The desktop's own menu supplies the folder command, the templates and
    // the naming dialog that avoids collisions
    m_newFileMenu = new KNewFileMenu(this);
    m_newFileMenu->setParentWidget(this);
    m_newFileMenu->setText(tr("New"));
    const auto rememberCreated = [this](const QUrl &url) {
        // Selected and renamed once the lister sees it
        m_pendingSelection = {url};
        m_pendingRename = true;
    };
    connect(m_newFileMenu, &KNewFileMenu::directoryCreated, this, rememberCreated);
    connect(m_newFileMenu, &KNewFileMenu::fileCreated, this, rememberCreated);

    m_itemActions = new KFileItemActions(this);
    m_itemActions->setParentWidget(this);
}

void MainWindow::buildMenuBar()
{
    QMenuBar *bar = menuBar();

    QMenu *file = bar->addMenu(tr("&File"));
    file->addAction(m_newFileMenu);
    file->addSeparator();
    file->addAction(m_actOpen);
    file->addAction(m_actOpenNewWindow);
    file->addAction(m_actOpenWith);
    file->addSeparator();
    file->addAction(m_actCreateLink);
    file->addAction(m_actRename);
    file->addAction(m_actDelete);
    file->addSeparator();
    file->addAction(m_actProperties);
    file->addSeparator();
    file->addAction(m_actNewWindow);
    file->addAction(m_actClose);

    QMenu *edit = bar->addMenu(tr("&Edit"));
    edit->addAction(m_actUndo);
    edit->addSeparator();
    edit->addAction(m_actCut);
    edit->addAction(m_actCopy);
    edit->addAction(m_actPaste);
    edit->addAction(m_actCopyPath);
    edit->addSeparator();
    edit->addAction(m_actSelectAll);
    edit->addAction(m_actInvertSelection);

    QMenu *view = bar->addMenu(tr("&View"));
    for (QAction *action : m_viewModeGroup->actions())
        view->addAction(action);
    view->addSeparator();
    QMenu *sort = view->addMenu(tr("Sort by"));
    for (QAction *action : m_sortGroup->actions())
        sort->addAction(action);
    sort->addSeparator();
    sort->addAction(m_sortAscending);
    sort->addAction(m_sortDescending);

    QMenu *group = view->addMenu(tr("Group by"));
    for (QAction *action : m_groupGroup->actions())
        group->addAction(action);
    view->addSeparator();
    view->addAction(m_actShowHidden);
    view->addAction(m_actHideExtensions);
    view->addAction(m_actUseCheckBoxes);
    view->addSeparator();
    view->addAction(m_actRefresh);

    QMenu *tools = bar->addMenu(tr("&Tools"));
    tools->addAction(m_actConnectDrives);
    tools->addAction(m_actMapDrive);
    tools->addSeparator();
    tools->addAction(m_actOpenTerminal);
    tools->addAction(m_actOpenAsAdmin);
    tools->addSeparator();
    tools->addAction(m_actEmptyTrash);

    // Keep the five native top-level menus; Linux destinations do not need
    // an extra top-level Go menu.
    QMenu *go = view->addMenu(tr("Go to"));
    go->addAction(m_actUp);
    go->addSeparator();
    go->addAction(tr("Computer"), this,
                  [this] { navigateTo(Locations::computer()); });
    go->addAction(tr("Home"), this, [this] {
        navigateTo(QUrl::fromLocalFile(QDir::homePath()));
    });
    go->addAction(tr("Recycle Bin"), this, [this] {
        navigateTo(QUrl(QStringLiteral("trash:/")));
    });

    QMenu *help = bar->addMenu(tr("&Help"));
    help->addAction(m_actHelp);
    help->addSeparator();
    help->addAction(tr("About File Explorer"), this,
                    &MainWindow::showAboutDialog);
}

void MainWindow::setupShortcuts()
{
    // Shortcuts with no menu entry of their own, all of them window wide since
    // they are about where the keyboard goes rather than the selection
    const auto add = [this](const QList<QKeySequence> &keys, auto slot) {
        auto *action = new QAction(this);
        action->setShortcuts(keys);
        connect(action, &QAction::triggered, this, slot);
        addAction(action);
        return action;
    };

    m_navigation->installShortcuts(this, false); // Up/Refresh already have menu actions.
    add({QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_N)},
        &MainWindow::createNewFolder);

    add({QKeySequence(Qt::CTRL | Qt::Key_E), QKeySequence(Qt::CTRL | Qt::Key_F),
         QKeySequence(Qt::Key_F3)},
        &MainWindow::focusSearchBox);

    add({QKeySequence(Qt::Key_F6)}, [this] { cyclePanes(true); });
    add({QKeySequence(Qt::SHIFT | Qt::Key_F6)}, [this] { cyclePanes(false); });

    add({QKeySequence(Qt::Key_F11)}, [this] {
        // Going full screen takes the frame and its glass area with it, and Qt
        // reparents the inset content back so nothing has to be undone here
        if (isFullScreen())
            showNormal();
        else
            showFullScreen();
    });
}

void MainWindow::focusSearchBox()
{
    if (!m_searchBox->isEnabled())
        return;   // Computer has no folder to search
    m_searchBox->setFocus();
    m_searchBox->selectAll();
}

void MainWindow::cyclePanes(bool forward)
{
    // In Win7's order, and the address bar is entered by starting an edit, the
    // only way it can hold the keyboard
    enum Pane { NavigationPaneStop, AddressStop, ListStop, SearchStop, StopCount };

    QWidget *focus = qApp->focusWidget();
    int current = ListStop;
    if (m_navigation->pathEditor() && (focus == m_navigation->pathEditor()
        || m_navigation->pathEditor()->isAncestorOf(focus)))
        current = AddressStop;
    else if (focus == m_searchBox)
        current = SearchStop;
    else if (focus && m_places->isAncestorOf(focus))
        current = NavigationPaneStop;

    // Skips stops that cannot take the keyboard, so on the Computer page this
    // steps past the search box rather than appearing to do nothing
    for (int step = 1; step < StopCount; ++step) {
        // Going backwards by one is the same as going forwards by all but one,
        // which keeps the result from going negative before the wrap around
        const int offset = forward ? step : StopCount - step;
        const int next = (current + offset) % StopCount;
        switch (next) {
        case NavigationPaneStop:
            if (!m_places->isVisible()) continue;
            m_places->setFocus(Qt::TabFocusReason);
            return;
        case AddressStop:
            m_navigation->focusPathEdit(false);
            return;
        case ListStop:
            m_navigation->endPathEdit();
            if (isComputerView())
                m_computerView->focusView();
            else if (isLibrariesView())
                m_librariesView->setFocus(Qt::TabFocusReason);
            else
                m_fileView->focusView();
            return;
        case SearchStop:
            if (!m_searchBox->isVisible() || !m_searchBox->isEnabled())
                continue;
            m_navigation->endPathEdit();
            focusSearchBox();
            return;
        }
    }
}

void MainWindow::buildNavigationBar()
{
    m_navBar = new QWidget;
    // AeroQt reserves top->sizeHint().height(). Do not clamp that below the
    // loaded navigation artwork plus its layout margins.
    auto *layout = new QHBoxLayout(m_navBar);
    layout->setContentsMargins(4, 3, 6, 3);
    layout->setSpacing(2);

    m_navigation = new BrowserNavigation(m_places->placesModel(), m_computerModel,
                                         layout, this);
    m_navigation->setShowHiddenFilesProvider([this] { return m_model->showHiddenFiles(); });
    connect(m_navigation, &BrowserNavigation::locationChanged, this, &MainWindow::showLocation);
    connect(m_navigation, &BrowserNavigation::navigated, &m_navSound, &QSoundEffect::play);
    connect(m_navigation, &BrowserNavigation::refreshRequested, this, &MainWindow::refresh);
    connect(m_navigation, &BrowserNavigation::typedFileActivated, this, [this](const QUrl &url) {
        FileOps::openItem(KFileItem(url), this);
    });
    connect(m_navigation, &BrowserNavigation::errorOccurred, this, [this](const QString &message) {
        m_details->showMessage(message);
    });
    layout->addSpacing(6);

    // SearchBox keeps the shared Aero entry artwork even while focused;
    // address editing uses a separate full-width opaque typing surface.
    m_searchBox = new SearchBox;
    // The Win7 glyph comes first, with the standard name as the fallback
    m_searchBox->addAction(Aero::themeIcon({"gtk-search", "system-search"}),
                           QLineEdit::TrailingPosition);
    m_searchBox->setToolTip(tr("Filters this folder as you type. Press Enter to "
                               "search subfolders as well."));

    connect(m_searchBox, &QLineEdit::textChanged, this, [this](const QString &text) {
        m_model->setNameFilter(text);
        updateDetailsPane();
        updateListMessage();
    });
    connect(m_searchBox, &QLineEdit::returnPressed, this, &MainWindow::startSearch);

    layout->addWidget(m_searchBox);
}

QWidget *MainWindow::buildCommandBar()
{
    Aero::Strip *bar = Aero::commandBar(Explorer::Art::CommandBar);
    m_commandBar = bar;
    bar->setObjectName(QStringLiteral("commandBar"));
    bar->setArt(Explorer::Art::CommandBar);
    m_commandLayout = bar->row();
    m_commandLayout->setSpacing(4);
    m_commandLayout->setSizeConstraint(QLayout::SetNoConstraint);
    bar->setMinimumWidth(0);

    auto *organize = new Aero::MenuButton(tr("Organize"));
    auto *menu = new QMenu(organize);
    menu->addAction(m_actCut);
    menu->addAction(m_actCopy);
    menu->addAction(m_actPaste);
    menu->addSeparator();
    menu->addAction(m_actUndo);
    menu->addSeparator();
    menu->addAction(m_actSelectAll);
    menu->addSeparator();

    QMenu *layout = menu->addMenu(tr("Layout"));
    auto *menuBarAction = layout->addAction(tr("Menu bar"));
    menuBarAction->setCheckable(true);
    connect(menuBarAction, &QAction::toggled, this,
            [this](bool on) { menuBar()->setVisible(on); });
    layout->addAction(m_actPreviewPane);
    auto *navPaneAction = layout->addAction(tr("Navigation pane"));
    navPaneAction->setCheckable(true);
    navPaneAction->setChecked(true);
    connect(navPaneAction, &QAction::toggled, this,
            [this](bool on) { m_places->setVisible(on); });
    auto *detailsPaneAction = layout->addAction(tr("Details pane"));
    detailsPaneAction->setCheckable(true);
    detailsPaneAction->setChecked(true);
    connect(detailsPaneAction, &QAction::toggled, this,
            [this](bool on) { m_details->setVisible(on); });
    connect(layout, &QMenu::aboutToShow, this,
            [this, menuBarAction, navPaneAction, detailsPaneAction] {
        menuBarAction->setChecked(menuBar()->isVisible());
        navPaneAction->setChecked(m_places->isVisible());
        detailsPaneAction->setChecked(m_details->isVisible());
    });

    // A command rather than a submenu, some settings being radio choices, two
    // depending on each other, and none taking effect until the user says so
    menu->addAction(m_actOptions);

    menu->addSeparator();
    menu->addAction(m_actDelete);
    menu->addAction(m_actRename);
    menu->addAction(m_actProperties);
    menu->addSeparator();
    menu->addAction(m_actClose);
    organize->setMenu(menu);
    m_commandLayout->addWidget(organize);

    // Win7 puts this on the bar itself rather than only inside the New menu
    auto *newFolder = new Aero::MenuButton(tr("New folder"));
    newFolder->setShowArrow(false);
    connect(newFolder, &QToolButton::clicked, this, &MainWindow::createNewFolder);
    m_commandLayout->addWidget(newFolder);
    m_newFolderButton = newFolder;

    m_commandOverflow = new QToolButton(bar);
    m_commandOverflow->setObjectName(QStringLiteral("commandOverflow"));
    m_commandOverflow->setText(QStringLiteral("\u00bb"));
    m_commandOverflow->setToolTip(tr("More commands"));
    m_commandOverflow->setAccessibleName(tr("More commands"));
    m_commandOverflow->setAutoRaise(true);
    m_commandOverflow->setFixedSize(24, 24);
    m_commandOverflow->setPopupMode(QToolButton::InstantPopup);
    m_commandOverflow->setMenu(new QMenu(m_commandOverflow));
    m_commandOverflow->setStyleSheet("QToolButton::menu-indicator { image: none; }");
    m_commandOverflow->hide();
    m_commandLayout->addWidget(m_commandOverflow);
    m_commandLayout->addStretch(1);

    m_viewButton = new QToolButton(bar);
    m_viewButton->setObjectName(QStringLiteral("changeViewButton"));
    m_viewButton->setPopupMode(QToolButton::MenuButtonPopup);
    m_viewButton->setAutoRaise(true);
    m_viewButton->setFixedSize(44, 24);
    m_viewButton->setIcon(ViewModeMenu::iconForMode(Settings::ViewMode::Details));
    m_viewButton->setToolTip(tr("Change your view"));
    m_viewButton->setAccessibleName(tr("Change your view"));
    m_viewButton->setMenu(new ViewModeMenu(m_viewModeGroup, m_viewButton));
    connect(m_viewButton, &QToolButton::clicked, this, [this] {
        setViewMode(ViewModeMenu::nextMode(currentViewMode()));
    });
    m_commandLayout->addWidget(m_viewButton);

    auto *previewButton = new QToolButton(bar);
    previewButton->setObjectName(QStringLiteral("previewPaneButton"));
    previewButton->setDefaultAction(m_actPreviewPane);
    previewButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
    previewButton->setAutoRaise(true);
    previewButton->setFixedSize(24, 24);
    previewButton->setToolTip(tr("Show the preview pane (Alt+P)"));
    previewButton->setAccessibleName(tr("Preview pane"));
    m_commandLayout->addWidget(previewButton);

    auto *helpButton = new QToolButton;
    helpButton->setObjectName(QStringLiteral("helpButton"));
    helpButton->setFixedSize(24, 24);
    helpButton->setCursor(Qt::ArrowCursor);
    helpButton->setStyleSheet("QToolButton { border: none; background: transparent; }");
    helpButton->setObjectName(QStringLiteral("helpButton"));
    helpButton->setDefaultAction(m_actHelp);
    helpButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
    helpButton->setAccessibleName(tr("Help"));
    m_commandLayout->addWidget(helpButton);

    return bar;
}

void MainWindow::rebuildContextualCommands()
{
    // Rebuilt rather than shown and hidden, which buttons belong here depending
    // on what is selected
    for (QWidget *widget : std::as_const(m_contextualCommands)) {
        // Removed at once, a deferred delete leaving the widget in the layout
        // so the insert position below would count buttons on their way out
        m_commandLayout->removeWidget(widget);
        widget->hide();
        widget->deleteLater();
    }
    m_contextualCommands.clear();

    if (!m_commandLayout)
        return;

    // After Organize, before New folder, as in the native command strip.
    const int insertAt = 1;
    QTimer::singleShot(0, this, &MainWindow::layoutCommandBar);
    const auto addButton = [this, insertAt](const QString &text, QMenu *menu,
                                            auto slot) {
        auto *button = new Aero::MenuButton(text);
        button->setShowArrow(menu != nullptr);
        if (menu) {
            // These menus are rebuilt with the contextual buttons. Give the
            // button ownership so selection changes do not accumulate menus.
            menu->setParent(button);
            button->setMenu(menu);
        } else {
            button->setPopupMode(QToolButton::DelayedPopup);
            connect(button, &QToolButton::clicked, this, slot);
        }
        m_commandLayout->insertWidget(insertAt + m_contextualCommands.size(), button);
        m_contextualCommands.append(button);
    };

    if (isComputerView()) {
        // Win7's own row, each opening the KDE app that does the same job
        addButton(tr("System properties"), nullptr, [this] {
            openSystemApplication({QStringLiteral("org.kde.kinfocenter")});
        });

        // Whichever software manager is installed, Discover first as the one
        // that ships with Plasma
        addButton(tr("Uninstall or change a program"), nullptr, [this] {
            openSystemApplication({QStringLiteral("org.kde.discover"),
                                   QStringLiteral("org.manjaro.pamac.manager"),
                                   QStringLiteral("octopi")});
        });

        addButton(tr("Map network drive"), nullptr,
                  [this] { m_actMapDrive->trigger(); });
        addButton(tr("Mount drives"), nullptr,
                  [this] { m_actConnectDrives->trigger(); });

        addButton(tr("Open Control Panel"), nullptr, [this] {
            openSystemApplication({QStringLiteral("systemsettings")});
        });
        return;
    }

    if (isLibrariesView()) {
        if (m_librariesView->selectedUrl().isValid())
            addButton(tr("Open"), nullptr, [this] { m_actOpen->trigger(); });
        return;
    }

    const QList<KFileItem> selection = selectedItems();
    if (isTrashView()) {
        if (!selection.isEmpty()) {
            QString restoreText = tr("Restore the selected items");
            if (selection.size() == 1)
                restoreText = tr("Restore this item");
            addButton(restoreText, nullptr, [this] { m_actRestore->trigger(); });
        } else if (m_model->rowCount() > 0) {
            addButton(tr("Restore all items"), nullptr,
                      [this] { m_actRestoreAll->trigger(); });
        }
        addButton(tr("Empty the Recycle Bin"), nullptr,
                  [this] { m_actEmptyTrash->trigger(); });
        return;
    }

    if (selection.isEmpty())
        return;

    addButton(tr("Open"), nullptr, [this] { m_actOpen->trigger(); });

    // Win7 puts the extract command right here when an archive is picked
    if (selection.size() == 1 && Archives::isBrowsable(selection.first())) {
        addButton(tr("Extract all files"), nullptr,
                  [this] { m_actExtract->trigger(); });
    }

    // Files only, a folder having exactly one thing that opens it
    const bool anyFiles = std::any_of(selection.cbegin(), selection.cend(),
                                      [](const KFileItem &item) { return !item.isDir(); });
    if (anyFiles) {
        auto *openWithMenu = new QMenu(this);
        connect(openWithMenu, &QMenu::aboutToShow, this, [this, openWithMenu] {
            openWithMenu->clear();
            const QList<KFileItem> items = selectedItems();
            if (items.isEmpty())
                return;
            m_itemActions->setItemListProperties(
                KFileItemListProperties(KFileItemList(items)));
            m_itemActions->insertOpenWithActionsTo(nullptr, openWithMenu, {});
        });
        addButton(tr("Open with"), openWithMenu, [] {});
    }

    addButton(tr("Share with"), nullptr, [this] {
        // The desktop's share plugins live in the item's service menu
        const QList<KFileItem> items = selectedItems();
        if (items.isEmpty())
            return;
        QMenu menu(this);
        m_itemActions->setItemListProperties(
            KFileItemListProperties(KFileItemList(items)));
        m_itemActions->addActionsTo(&menu);
        if (menu.isEmpty())
            m_details->showMessage(tr("No sharing services are installed."));
        else
            menu.exec(QCursor::pos());
    });
}

void MainWindow::layoutCommandBar()
{
    if (!m_commandLayout || !m_commandOverflow) return;
    const bool wantNewFolder = !isComputerView() && !isLibrariesView();
    m_newFolderButton->setVisible(wantNewFolder);
    for (QWidget *widget : std::as_const(m_contextualCommands)) widget->show();
    m_commandOverflow->hide();
    QMenu *overflow = m_commandOverflow->menu();
    overflow->clear();

    const auto requiredWidth = [this] {
        int width = m_commandLayout->contentsMargins().left()
                  + m_commandLayout->contentsMargins().right();
        int count = 0;
        for (int i = 0; i < m_commandLayout->count(); ++i) {
            QWidget *widget = m_commandLayout->itemAt(i)->widget();
            if (widget && !widget->isHidden()) {
                width += qMax(widget->minimumWidth(), widget->sizeHint().width());
                ++count;
            }
        }
        return width + count * m_commandLayout->spacing();
    };
    int hiddenFrom = m_contextualCommands.size();
    while (requiredWidth() > m_commandBar->width() && hiddenFrom > 0) {
        m_contextualCommands.at(--hiddenFrom)->hide();
        m_commandOverflow->show();
    }
    if (requiredWidth() > m_commandBar->width() && wantNewFolder) {
        m_newFolderButton->hide();
        m_commandOverflow->show();
    }
    const auto addHidden = [overflow](QWidget *widget) {
        auto *button = qobject_cast<QToolButton *>(widget);
        if (!button) return;
        if (button->menu()) {
            overflow->addMenu(button->menu())->setText(button->text());
        } else {
            QAction *action = overflow->addAction(button->icon(), button->text());
            action->setEnabled(button->isEnabled());
            QObject::connect(action, &QAction::triggered, button, &QToolButton::click);
        }
    };
    for (int i = hiddenFrom; i < m_contextualCommands.size(); ++i)
        addHidden(m_contextualCommands.at(i));
    if (wantNewFolder && m_newFolderButton->isHidden()) addHidden(m_newFolderButton);
}

QWidget *MainWindow::buildSearchAgainBar()
{
    // The ways to widen a search, and without them a content search cannot be
    // reached at all, the search box having nowhere to express a scope
    m_searchAgainBar = new QWidget;
    m_searchAgainBar->setObjectName(QStringLiteral("searchAgainBar"));
    m_searchAgainBar->setStyleSheet(
        Aero::panelSheet(m_searchAgainBar->objectName(), Aero::Palette::StripSurface,
                         Qt::BottomEdge, Aero::Palette::StripRule));
    m_searchAgainBar->hide();

    auto *row = new QHBoxLayout(m_searchAgainBar);
    row->setContentsMargins(10, 4, 10, 4);
    row->setSpacing(12);

    row->addWidget(Aero::bodyLabel(tr("Search again in:")));

    const auto link = [this, row](const QString &text, bool contents,
                                  bool wholeMachine) {
        auto *label = qobject_cast<Aero::LinkLabel *>(Aero::bodyLabel(text, true));
        connect(label, &Aero::LinkLabel::clicked, this,
                [this, contents, wholeMachine] { searchAgain(contents, wholeMachine); });
        row->addWidget(label);
        return label;
    };

    link(tr("Computer"), false, true);
    link(tr("File Contents"), true, false);
    row->addStretch(1);

    return m_searchAgainBar;
}

void MainWindow::updateSearchAgainBar()
{
    const QUrl url = currentUrl();
    // Only the search worker can widen the scope
    m_searchAgainBar->setVisible(
        Locations::isSearch(url)
        && KProtocolInfo::isKnownProtocol(QStringLiteral("filenamesearch")));
}

void MainWindow::searchAgain(bool contents, bool wholeMachine)
{
    const QUrl url = currentUrl();
    if (!Locations::isSearch(url))
        return;

    const QString term = Locations::searchTerm(url);
    if (term.isEmpty())
        return;

    // Win7's computer scope is the whole machine, so the filesystem root
    const QUrl folder = wholeMachine ? QUrl::fromLocalFile(QStringLiteral("/"))
                                     : Locations::searchFolder(url);
    navigateTo(Locations::search(folder, term, contents));
}

QWidget *MainWindow::buildNotificationBar()
{
    m_notification = new KMessageWidget;
    m_notification->setPosition(KMessageWidget::Header);
    m_notification->hide();

    // The strip carries no policy, only reporting the click, and which dialog
    // that means is decided here
    connect(m_notification, &KMessageWidget::linkActivated, this,
            [this](const QString &link) {
        if (link == QLatin1String("admin-warning"))
            AccessDialogs::showAdministratorWarning(this);
        else if (link == QLatin1String("mount-dlg"))
            showMountDialog();
    });
    // Dismissing hides it for the rest of the session
    connect(m_notification, &KMessageWidget::hideAnimationFinished, this, [this] {
        m_notificationDismissed = true;
    });

    return m_notification;
}

MainWindow::Notice MainWindow::pendingNotice() const
{
    // Only one strip, and the administrator warning outranks the drives notice
    if (currentUrl().scheme() == QLatin1String("admin"))
        return Notice::Administrator;

    if (isComputerView() && !m_notificationDismissed
        && Settings::notifyUnmountedDrives()
        && m_computerModel->unmountedCount() > 0) {
        return Notice::UnmountedDrives;
    }

    return Notice::None;
}

void MainWindow::updateNotification()
{
    if (!m_notification)
        return;

    m_notice = pendingNotice();
    if (m_notice == Notice::None) {
        m_notification->clearActions();
        return;
    }

    // The drives notice can be waved away, where the administrator one stays
    // for as long as it is true
    if (m_notice == Notice::Administrator) {
        m_notification->setText(
            tr("You're navigating as an administrator, be careful. "
               "<a href=\"admin-warning\">Click for information...</a>"));
        m_notification->setCloseButtonVisible(false);
    } else {
        m_notification->setText(unmountedDrivesNotice(m_computerModel->unmountedCount()));
        m_notification->setCloseButtonVisible(true);
    }
    m_notification->animatedShow();
}

// Windows counts the drives it found rather than saying some
QString MainWindow::unmountedDrivesNotice(int hidden)
{
    if (hidden == 1) {
        return tr("A drive is connected to your computer but is not mounted. "
                  "<a href=\"mount-dlg\">Click to change...</a>");
    }
    return tr("%1 drives are connected to your computer but are not mounted. "
              "<a href=\"mount-dlg\">Click to change...</a>").arg(hidden);
}

QWidget *MainWindow::buildBody()
{
    m_splitter = new Aero::PaneSplitter(Qt::Horizontal);
    m_splitter->setChildrenCollapsible(false);

    m_splitter->addWidget(m_places);

    m_fileView = new FileView(m_model);
    connect(m_fileView, &FileView::activated, this, &MainWindow::activateIndex);
    connect(m_fileView, &FileView::middleClicked, this, [this](const QModelIndex &index) {
        const KFileItem item = m_model->itemForIndex(index);
        if (!item.isNull())
            openInNewWindow({item});
    });
    connect(m_fileView, &FileView::contextMenuRequested,
            this, &MainWindow::showContextMenu);
    connect(m_fileView, &FileView::selectionChanged, this, [this] {
        invalidateSelection();
        updateDetailsPane();
        updateActionStates();
        rebuildContextualCommands();
        if (m_previewPane->isVisible())
            m_previewPane->setItems(selectedItems());
    });
    connect(m_fileView, &FileView::renameRequested, this, &MainWindow::applyRename);
    connect(m_fileView, &FileView::dropped, this, &MainWindow::handleDrop);
    // Silent, the drag still being in progress and the navigation sound
    // belonging to something the user has finished doing
    connect(m_fileView, &FileView::springLoaded, this, [this](const QUrl &folder) {
        m_navigation->navigateTo(folder, false);
    });
    connect(m_fileView, &FileView::viewModeChanged, this,
            [this](Settings::ViewMode mode) {
        for (QAction *action : m_viewModeGroup->actions())
            action->setChecked(action->data().toInt() == int(mode));
    });
    connect(m_fileView, &FileView::groupColumnChanged, this, [this](int column) {
        // Also fires when picking an icon mode dropped the grouping, hence the
        // menu following the view rather than the other way round
        for (QAction *action : m_groupGroup->actions())
            action->setChecked(action->data().toInt() == column);
    });
    connect(&FileOps::GLOBAL_RENAME_CONTEXT, &FileOps::GlobalRenameContext::finishRename,
            this, [this](const QUrl &newUrl) {
        m_fileView->selectUrl(newUrl);
    });

    m_computerView = new ComputerView(m_computerModel);
    connect(m_computerView, &ComputerView::urlActivated, this, &MainWindow::navigateTo);
    connect(m_computerView, &ComputerView::selectionChanged,
            this, &MainWindow::updateDetailsPane);
    connect(m_computerView, &ComputerView::contextMenuRequested,
            this, &MainWindow::showComputerContextMenu);
    connect(m_computerView, &ComputerView::viewModeChanged, this,
            [this](Settings::ViewMode mode) {
        for (QAction *action : m_viewModeGroup->actions())
            action->setChecked(action->data().toInt() == int(mode));
    });

    m_listPage = new QWidget;
    m_listPage->setObjectName(QStringLiteral("fileListPage"));
    m_listPage->setStyleSheet(Aero::panelSheet(m_listPage->objectName(), Aero::Palette::Surface));
    auto *listLayout = new QVBoxLayout(m_listPage);
    listLayout->setContentsMargins(kListLeftMargin, 0, 0, 0);
    listLayout->setSpacing(0);
    m_libraryHeader = new QWidget;
    m_libraryHeader->setObjectName(QStringLiteral("libraryFolderHeader"));
    auto *libraryLayout = new QVBoxLayout(m_libraryHeader);
    libraryLayout->setContentsMargins(10, 8, 8, 8);
    libraryLayout->setSpacing(0);
    m_libraryHeading = Aero::label(QString(), 12, "#006600");
    libraryLayout->addWidget(m_libraryHeading);
    libraryLayout->addWidget(Aero::label(tr("Includes: 1 location"), 9, Aero::Palette::LinkText));
    m_libraryHeader->hide();
    listLayout->addWidget(m_libraryHeader);
    listLayout->addWidget(m_fileView, 1);

    m_loadingPage = new QWidget;
    m_loadingPage->setObjectName(QStringLiteral("loadingPage"));
    m_loadingPage->setStyleSheet(Aero::panelSheet(m_loadingPage->objectName(), Aero::Palette::Surface));
    auto *loadingLayout = new QVBoxLayout(m_loadingPage);
    loadingLayout->setContentsMargins(0, 0, 0, 0);
    loadingLayout->addSpacing(20);
    m_loadingLabel = Aero::label(tr("Working on it..."), 9);
    loadingLayout->addWidget(m_loadingLabel, 0, Qt::AlignHCenter);
    loadingLayout->addStretch(1);

    m_stack = new QStackedWidget;
    m_stack->addWidget(m_listPage);
    m_stack->addWidget(m_computerView);
    m_stack->addWidget(m_loadingPage);
    m_librariesView = new LibrariesView;
    m_stack->addWidget(m_librariesView);
    connect(m_librariesView, &LibrariesView::urlActivated, this, &MainWindow::navigateTo);
    connect(m_librariesView, &LibrariesView::newWindowRequested, this, &MainWindow::openNewWindow);
    connect(m_librariesView, &LibrariesView::selectionChanged, this, [this] {
        invalidateSelection();
        updateDetailsPane();
        updateActionStates();
        rebuildContextualCommands();
    });
    connect(m_librariesView, &LibrariesView::viewModeChanged, this,
            [this] { updateActionStates(); });
    m_splitter->addWidget(m_stack);

    // In the splitter rather than floating over the list, so its edge resizes
    // the file list as the navigation pane's does
    m_previewPane = new PreviewPane;
    // The splitter supplies the pane rule; avoid doubling it with the
    // preview's standalone left border.
    m_previewPane->setStyleSheet(Aero::panelSheet(m_previewPane->objectName(), Aero::Palette::Surface));
    m_previewPane->hide();
    m_splitter->addWidget(m_previewPane);

    m_splitter->setStretchFactor(0, 0);
    m_splitter->setStretchFactor(1, 1);
    m_splitter->setStretchFactor(2, 0);
    m_splitter->setSizes({158, 750, 250});

    return m_splitter;
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    // Every window installs this filter on the application, so without the
    // check on which is active the first one opened answers for all of them
    if (event->type() == QEvent::MouseButtonPress && isActiveWindow()) {
        auto *mouse = static_cast<QMouseEvent *>(event);
        // The back and forward buttons on the side of a mouse
        if (mouse->button() == Qt::XButton1) {
            goBack();
            return true;
        }
        if (mouse->button() == Qt::XButton2) {
            goForward();
            return true;
        }
    }

    // Here rather than in the views, both answering it and neither owning what
    // changing mode means
    if (event->type() == QEvent::Wheel && isActiveWindow()) {
        auto *wheel = static_cast<QWheelEvent *>(event);
        auto *widget = qobject_cast<QWidget *>(watched);

        QWidget *zoomable = m_fileView;
        if (isComputerView())
            zoomable = m_computerView;
        else if (isLibrariesView())
            zoomable = m_librariesView;

        const bool ctrlHeld = wheel->modifiers().testFlag(Qt::ControlModifier);
        const bool overView = widget && zoomable
            && (widget == zoomable || zoomable->isAncestorOf(widget));
        if (ctrlHeld && overView) {
            zoomViewMode(wheel->angleDelta().y());
            return true;
        }
    }

    // Any other key pressed while it is held cancels this, so the combinations
    // are unaffected
    if (event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        m_altAlone = (key->key() == Qt::Key_Alt);
    } else if (event->type() == QEvent::KeyRelease) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Alt && m_altAlone && isActiveWindow()) {
            m_altAlone = false;
            menuBar()->setVisible(!menuBar()->isVisible());
            return true;
        }
        m_altAlone = false;
    }

    if (watched == m_commandBar && event->type() == QEvent::Resize)
        QTimer::singleShot(0, this, &MainWindow::layoutCommandBar);

    // Plain labels for the same reason the crumbs are, Win7's blue task link
    // look not being something a real button can be talked into
    return QMainWindow::eventFilter(watched, event);
}

bool MainWindow::isComputerView() const
{
    return m_stack && m_stack->currentWidget() == m_computerView;
}

bool MainWindow::isLibrariesView() const
{
    return Locations::isLibraries(currentUrl());
}

bool MainWindow::isTrashView() const
{
    return currentUrl().scheme() == QLatin1String("trash");
}

QUrl MainWindow::operationFolder() const
{
    const QUrl current = currentUrl();
    if (Locations::isLibraries(current) || Locations::isComputer(current))
        return {};
    // Nothing can be created in a search result, so those commands act on the
    // folder that was searched
    if (Locations::isSearch(current))
        return Locations::searchFolder(current);
    return current;
}

void MainWindow::showLocation(const QUrl &url)
{
    invalidateSelection();

    if (Locations::isLibraries(url)) {
        m_loading = false;
        m_loadingTimer->stop();
        m_navigation->stopPathProgress();
        m_stack->setCurrentWidget(m_librariesView);
        m_librariesView->setViewMode(Settings::hasViewModeFor(url)
            ? Settings::viewModeFor(url) : Settings::ViewMode::Tiles);
    } else if (Locations::isComputer(url)) {
        // Never handed to KIO, there being no worker, and this page is built
        // in process and always ready so a pending placeholder is cancelled
        m_loading = false;
        m_loadingTimer->stop();
        m_navigation->stopPathProgress();
        m_stack->setCurrentWidget(m_computerView);

        // Win7 opens this page in tiles, so it falls back to its own mode
        // rather than the global default
        Settings::ViewMode mode = Settings::ViewMode::Tiles;
        if (Settings::hasViewModeFor(url))
            mode = Settings::viewModeFor(url);
        m_computerView->setViewMode(mode);
    } else {
        m_stack->setCurrentWidget(m_listPage);
        m_model->setUrl(url);
        const auto library = Libraries::forUrl(url);
        m_libraryHeader->setVisible(library.url.isValid());
        m_libraryHeading->setText(tr("%1 library").arg(library.name));

        // Not through the path a user takes, which would write this folder's
        // mode back out as the default for every folder
        m_fileView->setViewMode(Settings::viewModeFor(url));
    }

    m_places->setCurrentUrl(url);

    m_fileView->setDestination(operationFolder());

    // A stale filter would narrow the new folder with no visible cause, and
    // search results keep their term, that being what produced them
    if (Locations::isSearch(url)) {
        QSignalBlocker blocker(m_searchBox);
        m_searchBox->setText(Locations::searchTerm(url));
        m_model->setNameFilter(QString());
    } else {
        m_searchBox->clear();
    }
    m_searchBox->setEnabled(!Locations::isComputer(url) && !Locations::isLibraries(url));

    updateWindowTitle();
    updateDetailsPane();
    updateListMessage();
    updateNotification();
    updateSearchAgainBar();
    updateActionStates();
    rebuildContextualCommands();
}

void MainWindow::navigateTo(const QUrl &url)
{
    m_navigation->navigateTo(url);
}

void MainWindow::goBack()
{
    m_navigation->goBack();
}

void MainWindow::goForward()
{
    m_navigation->goForward();
}

void MainWindow::goUp()
{
    m_navigation->goUp();
}

void MainWindow::refresh()
{
    if (isComputerView())
        m_computerModel->refresh();
    else if (isLibrariesView())
        m_librariesView->refresh();
    else
        m_model->refresh();
    m_places->refresh();
}

void MainWindow::updateWindowTitle()
{
    // The last crumb, so the title agrees with the address bar
    const auto crumbs = m_navigation->crumbsFor(currentUrl());
    QString label = tr("Computer");
    if (!crumbs.isEmpty())
        label = crumbs.last().label;
    setWindowTitle(label);
    // Win7's search box names the folder it will search
    m_searchBox->setPlaceholderText(tr("Search %1").arg(label));
    m_searchBox->setAccessibleName(tr("Search %1").arg(label));
}

void MainWindow::startSearch()
{
    const QString term = m_searchBox->text().trimmed();
    const QUrl folder = operationFolder();
    if (term.isEmpty() || isComputerView() || !folder.isValid())
        return;

    // With subfolder search off, Enter does nothing beyond what typing did
    if (!Settings::searchSubfolders())
        return;

    // Without the search worker the navigation fails with a message that says
    // nothing about what to install, and the filter typing applied is still in
    // place so declining leaves the folder narrowed rather than empty
    if (!KProtocolInfo::isKnownProtocol(QStringLiteral("filenamesearch"))) {
        m_details->showMessage(tr("Searching subfolders needs kio-extras. "
                                  "Showing matches in this folder only."));
        return;
    }

    navigateTo(Locations::search(folder, term, Settings::searchFileContents()));
}

void MainWindow::openNewWindow(const QUrl &url)
{
    QUrl target = url;
    if (!target.isValid())
        target = Locations::computer();

    // The delete on close flag set in the constructor owns this
    auto *window = new MainWindow(target);
    window->show();
}

void MainWindow::showMountDialog()
{
    MountDialog dialog(m_places->placesModel(), this);
    dialog.exec();
}

void MainWindow::openSystemApplication(const QStringList &desktopNames)
{
    if (!FileOps::openApplication(desktopNames, this))
        m_details->showMessage(tr("The program for this is not installed."));
}

void MainWindow::showHelp()
{
    if (!m_helpWindow) m_helpWindow = new HelpDialog(this);
    m_helpWindow->show();
    m_helpWindow->raise();
    m_helpWindow->activateWindow();
}

void MainWindow::showAboutDialog()
{
    AboutDialog dialog(this);
    dialog.exec();
}

void MainWindow::showOptionsDialog()
{
    OptionsDialog dialog(this);

    // Every window, these being application settings
    connect(&dialog, &OptionsDialog::applied, this, [] {
        for (MainWindow *window : std::as_const(g_windows))
            window->applyOptions();
    });

    connect(&dialog, &OptionsDialog::applyViewToAllFolders, this, [this] {
        // This folder's view becomes the default, and clearing the remembered
        // ones is what extends it to folders that already had a preference
        const Settings::ViewMode mode = currentViewMode();
        Settings::clearRememberedViewModes();
        Settings::setDefaultViewMode(mode);
        Settings::setViewModeFor(currentUrl(), mode);
    });

    connect(&dialog, &OptionsDialog::resetAllFolders, this, [this] {
        Settings::clearRememberedViewModes();
        for (MainWindow *window : std::as_const(g_windows))
            window->setViewMode(Settings::defaultViewMode());
    });

    dialog.exec();
}

void MainWindow::applyOptions()
{
    m_model->setShowHiddenFiles(Settings::showHiddenFiles());
    m_model->refreshDisplayNames();
    m_fileView->setCheckBoxesVisible(Settings::useCheckBoxes());
    m_fileView->setSingleClickToOpen(Settings::singleClickToOpen());

    // Only ever forces the bar on, hiding it snatching it from a user who had
    // just opened it themselves
    if (Settings::alwaysShowMenus())
        menuBar()->show();

    // The menu actions are a second view of the same settings
    m_actShowHidden->setChecked(Settings::showHiddenFiles());
    m_actHideExtensions->setChecked(Settings::hideKnownExtensions());
    m_actUseCheckBoxes->setChecked(Settings::useCheckBoxes());

    refreshBranding();
    updateActionStates();
    updateNotification();
}

void MainWindow::showMapDriveDialog()
{
    MapDriveDialog dialog(m_places->placesModel(), this);
    if (dialog.exec() != QDialog::Accepted)
        return;

    // Also the only way to find out whether the share is reachable, nothing
    // having been contacted yet and the prompt coming with the first listing
    m_places->refresh();
    navigateTo(dialog.mappedUrl());
}

void MainWindow::setViewMode(Settings::ViewMode mode)
{
    if (isComputerView()) {
        // For this page alone, a mode picked for drives not following the user
        // into folders
        m_computerView->setViewMode(mode);
        Settings::setViewModeFor(currentUrl(), mode);
        updateActionStates();
        return;
    }
    if (isLibrariesView()) {
        m_librariesView->setViewMode(mode);
        Settings::setViewModeFor(currentUrl(), mode);
        updateActionStates();
        return;
    }

    m_fileView->setViewMode(mode);
    // Also the default for folders with no preference of their own
    Settings::setViewModeFor(currentUrl(), mode);
    Settings::setDefaultViewMode(mode);
    updateActionStates();
}

void MainWindow::zoomViewMode(int angleDelta)
{
    // One notch of an ordinary mouse wheel, as Qt reports it
    const int notch = 120;

    // Anything finer than a notch is carried over, so a touchpad still gets
    // there eventually
    m_zoomRemainder += angleDelta;
    const int steps = m_zoomRemainder / notch;
    if (steps == 0)
        return;
    m_zoomRemainder -= steps * notch;

    // The modes run largest first, so scrolling up walks the list backwards
    const Settings::ViewMode current = currentViewMode();
    const int first = int(Settings::ViewMode::ExtraLargeIcons);
    const int last = int(Settings::ViewMode::Content);
    const int next = qBound(first, int(current) - steps, last);
    if (next == int(current))
        return;

    setViewMode(Settings::ViewMode(next));
    // The menu ticks its own action group, where from the wheel there is none
    updateActionStates();
}

void MainWindow::refreshBranding()
{
    m_model->refreshDisplayNames();
    m_navigation->refreshTrail();
    m_places->refresh();
    updateWindowTitle();
}

void MainWindow::updateListMessage()
{
    // Nothing to say while the loading page is up, while the drives are
    // showing, or when the folder failed to open
    if (isComputerView() || isLibrariesView() || m_loading || m_lastLoadFailed || m_model->rowCount() > 0) {
        m_fileView->setStatusMessage(QString());
        return;
    }

    // An empty search result says something different from an empty folder
    const bool searching = Locations::isSearch(currentUrl())
                        || !m_searchBox->text().isEmpty();
    if (searching)
        m_fileView->setStatusMessage(tr("No items match your search."));
    else
        m_fileView->setStatusMessage(tr("This folder is empty."));
}

void MainWindow::updateDetailsPane()
{
    if (isComputerView()) {
        const QModelIndex selected = m_computerView->selectedIndex();
        if (selected.isValid())
            m_details->showDrive(selected);
        else
            m_details->showComputerSummary();
        return;
    }
    if (isLibrariesView()) {
        const QUrl selected = m_librariesView->selectedUrl();
        if (selected.isValid())
            m_details->showLibrary(m_navigation->locationIcon(selected), m_librariesView->selectedName());
        else
            m_details->showFolderSummary(m_librariesView->libraryCount(), m_navigation->locationIcon(currentUrl()));
        return;
    }

    const QList<KFileItem> selection = selectedItems();
    if (!selection.isEmpty()) {
        m_details->showSelection(selection);
        return;
    }

    m_details->showFolderSummary(m_model->rowCount(), m_navigation->locationIcon(currentUrl()));
}

bool MainWindow::isFolderWritable(const QUrl &folder) const
{
    if (!folder.isLocalFile())
        return true;   // not our question to answer, KIO reports what it finds

    // Cached per folder, this being called on every selection change, which
    // during a rubber band drag is every mouse move, and the check can stall
    if (folder != m_writableUrl) {
        m_writableUrl = folder;
        m_writable = QFileInfo(folder.toLocalFile()).isWritable();
    }
    return m_writable;
}

void MainWindow::updateActionStates()
{
    const QList<KFileItem> selection = selectedItems();
    const bool any = !selection.isEmpty();
    const bool one = selection.size() == 1;
    const bool computer = isComputerView();
    const bool libraries = isLibrariesView();
    const bool shellRoot = computer || libraries;
    const bool librarySelected = libraries && m_librariesView->selectedUrl().isValid();
    const bool trash = isTrashView();
    const QUrl folder = operationFolder();

    // The archive workers are read only, so everything that would write has to
    // stand down
    const bool readOnly = Archives::isInsideArchive(folder);

    // Only what can be browsed, a file having no window of its own to open in
    const bool anyBrowsable = std::any_of(selection.cbegin(), selection.cend(),
                                          [](const KFileItem &item) {
        return item.isDir() || Archives::urlFor(item).isValid();
    });

    m_actOpen->setEnabled(any || librarySelected);
    m_actOpenNewWindow->setEnabled((anyBrowsable && !trash) || librarySelected);
    m_actOpenWith->setEnabled(any && !trash);
    m_actCut->setEnabled(any && !trash);
    m_actCopy->setEnabled(any && !trash);
    m_actCopyPath->setEnabled(any);
    m_actCreateLink->setEnabled(any && !trash && folder.isLocalFile());
    m_actPaste->setEnabled(!shellRoot && !trash && !readOnly && FileOps::canPaste());
    m_actDelete->setEnabled(any && !trash && !readOnly);
    m_actDeleteForever->setEnabled(any && !readOnly);
    // Win7 renames a whole selection at once, so this is not a single item
    m_actRename->setEnabled(any && !trash && !readOnly);
    m_newFileMenu->setEnabled(!shellRoot && !trash && !readOnly);
    if (m_newFolderButton) {
        m_newFolderButton->setVisible(!shellRoot);
        m_newFolderButton->setEnabled(!trash && !readOnly);
    }
    m_actProperties->setEnabled(!shellRoot);
    m_actRestore->setEnabled(any && trash);
    m_actRestore->setVisible(trash);
    m_actRestoreAll->setEnabled(trash && m_model->rowCount() > 0);
    m_actRestoreAll->setVisible(trash);
    m_actEmptyTrash->setEnabled(trash || !shellRoot);
    m_actSelectAll->setEnabled(!shellRoot);
    m_actInvertSelection->setEnabled(!shellRoot);
    m_actOpenTerminal->setEnabled(folder.isLocalFile());
    m_actUp->setEnabled(!shellRoot);
    m_actUndo->setEnabled(FileOps::isUndoAvailable());

    const bool archiveSelected = one && Archives::isBrowsable(selection.first());
    m_actExtract->setEnabled(archiveSelected || Archives::isInsideArchive(currentUrl()));

    // Only where the user cannot already write, or elevation becomes something
    // people click by reflex, and the admin worker is local only
    const bool alreadyAdmin = currentUrl().scheme() == QLatin1String("admin");
    m_actOpenAsAdmin->setEnabled(folder.isLocalFile() && !alreadyAdmin
                                 && !isFolderWritable(folder));

    // The modes apply to the drives page too, but not the ordering, this menu
    // being over the file list's columns while drives sort from their own
    const Settings::ViewMode mode = currentViewMode();
    for (QAction *action : m_viewModeGroup->actions()) {
        action->setEnabled(true);
        action->setChecked(action->data().toInt() == int(mode));
    }
    if (m_viewButton)
        m_viewButton->setIcon(ViewModeMenu::iconForMode(mode));
    for (QAction *action : m_sortGroup->actions()) {
        action->setEnabled(!shellRoot);
        action->setChecked(action->data().toInt() == m_model->sortColumn());
    }
    m_sortAscending->setChecked(m_model->sortOrder() == Qt::AscendingOrder);
    m_sortDescending->setChecked(m_model->sortOrder() == Qt::DescendingOrder);
}

void MainWindow::activateIndex(const QModelIndex &index)
{
    const KFileItem item = m_model->itemForIndex(index);
    if (item.isNull())
        return;
    openItems({item});
}

void MainWindow::openInNewWindow(const QList<KFileItem> &items)
{
    // Only the things this application can browse, a file having no window of
    // its own to open in
    for (const KFileItem &item : items) {
        if (item.isDir()) {
            openNewWindow(item.url());
            continue;
        }
        const QUrl archive = Archives::urlFor(item);
        if (archive.isValid())
            openNewWindow(archive);
    }
}

void MainWindow::openItems(const QList<KFileItem> &items)
{
    // The setting that opens each folder in its own window, folders only
    const bool separateWindows = Settings::browseInNewWindow();

    for (const KFileItem &item : items) {
        if (item.isDir()) {
            if (separateWindows)
                openNewWindow(item.url());
            else
                navigateTo(item.url());
            continue;
        }

        // Anything with no archive worker goes to the desktop's handler
        const QUrl archive = Archives::urlFor(item);
        if (archive.isValid()) {
            if (separateWindows)
                openNewWindow(archive);
            else
                navigateTo(archive);
            continue;
        }

        FileOps::openItem(item, this);
    }
}

void MainWindow::extractSelection()
{
    // Either an archive picked in the list, or the one being browsed
    const QList<KFileItem> selection = selectedItems();
    QUrl archive;
    if (selection.size() == 1 && Archives::isBrowsable(selection.first()))
        archive = Archives::urlFor(selection.first());
    else if (Archives::isInsideArchive(currentUrl()))
        archive = currentUrl();

    if (!archive.isValid())
        return;

    const QUrl archiveFile = Archives::archiveFileFor(archive);
    if (!archiveFile.isValid())
        return;

    // Windows suggests a folder beside the archive, named after it
    QString folder = archiveFile.fileName();
    const int dot = folder.lastIndexOf(QLatin1Char('.'));
    if (dot > 0)
        folder.truncate(dot);   // drop the extension
    const QString besideArchive = KIO::upUrl(archiveFile).toLocalFile();
    const QString suggested = besideArchive + QLatin1Char('/') + folder;

    bool ok = false;
    const QString destination = QInputDialog::getText(
        this, tr("Extract Compressed (Zipped) Folders"),
        tr("Files will be extracted to this folder:"), QLineEdit::Normal,
        suggested, &ok);
    if (!ok || destination.isEmpty())
        return;

    FileOps::extractArchive(archive, QUrl::fromLocalFile(destination), this);
}

void MainWindow::openAsAdministrator()
{
    const QString failure = openFolderAsAdministrator(operationFolder());
    if (!failure.isEmpty())
        m_details->showMessage(failure);
}

QString MainWindow::openFolderAsAdministrator(const QUrl &folder)
{
    if (!folder.isLocalFile())
        return tr("Only folders on this computer can be opened as "
                  "administrator.");

    // The admin worker is a separate package, and without this check its
    // absence surfaces as an unsupported protocol
    if (!KProtocolInfo::isKnownProtocol(QStringLiteral("admin")))
        return tr("Opening a folder as administrator needs kio-admin.");

    // A new window, since mixing an elevated view into the same history makes
    // it far too easy to lose track of which one is in front
    QUrl admin = folder;
    admin.setScheme(QStringLiteral("admin"));
    openNewWindow(admin);
    return {};
}

void MainWindow::reportListingFailure(int error, const QUrl &url)
{
    // Anything else is already in the details pane in KIO's own words
    if (!AccessDialogs::isPermissionError(error))
        return;

    const QUrl folder = url.isValid() ? url : currentUrl();
    const QString path = displayPath(folder);

    // Only where elevation could change the answer, an elevated view having
    // nothing above it and a server refusing on its own account, so both of
    // those get Windows' flat refusal instead
    const bool alreadyAdmin = folder.scheme() == QLatin1String("admin");
    if (alreadyAdmin || !folder.isLocalFile()) {
        AccessDialogs::showLocationUnavailable(this, path);
        return;
    }

    // Windows titles this one with the name of the folder that refused
    QString name = folder.fileName();
    if (name.isEmpty())
        name = path;
    // Windows ends the sequence with the same box whether the rights were
    // declined or simply not granted
    if (!AccessDialogs::askForAdminAccess(this, name)) {
        AccessDialogs::showLocationUnavailable(this, path);
        return;
    }

    // Aimed at the folder that refused rather than whatever this window ended
    // up showing
    const QString failure = openFolderAsAdministrator(folder);
    if (!failure.isEmpty())
        AccessDialogs::showLocationUnavailable(this, path, failure);
}

QString MainWindow::displayPath(const QUrl &url)
{
    return BrowserNavigation::displayPath(url);
}

void MainWindow::handleDrop(QDropEvent *event, const QUrl &destination)
{
    if (!destination.isValid() || Locations::isComputer(destination))
        return;

    // The job puts up the copy, move and link menu, performs the transfer and
    // records it with the undo manager
    FileOps::dropOn(event, destination, this);
}

void MainWindow::showContextMenu(const QPoint &globalPos, bool onItem)
{
    const QList<KFileItem> selection = selectedItems();
    updateActionStates();

    QMenu menu(this);

    // The views clear the selection when a click misses, so an empty selection
    // means the same as a click on the background
    if (onItem && !selection.isEmpty())
        buildItemContextMenu(menu, selection);
    else
        buildFolderContextMenu(menu);

    menu.exec(globalPos);
}

void MainWindow::buildItemContextMenu(QMenu &menu,
                                      const QList<KFileItem> &selection)
{
    menu.addAction(m_actOpen);
    if (m_actOpenNewWindow->isEnabled() && !isTrashView())
        menu.addAction(m_actOpenNewWindow);

    const bool anyFiles = std::any_of(selection.cbegin(), selection.cend(),
                                      [](const KFileItem &item) { return !item.isDir(); });
    if (anyFiles) {
        // Also where installed service menus come from
        m_itemActions->setItemListProperties(
            KFileItemListProperties(KFileItemList(selection)));
        m_itemActions->insertOpenWithActionsTo(nullptr, &menu, {});
    }
    menu.addSeparator();

    if (isTrashView()) {
        menu.addAction(m_actRestore);
        menu.addAction(m_actDeleteForever);
        menu.addSeparator();
        menu.addAction(m_actProperties);
        return;
    }

    // Right above cut, where Win7's archive menu puts it
    if (selection.size() == 1 && Archives::isBrowsable(selection.first())) {
        menu.addAction(m_actExtract);
        menu.addSeparator();
    }

    menu.addAction(m_actCut);
    menu.addAction(m_actCopy);
    menu.addAction(m_actCopyPath);
    menu.addAction(m_actCreateLink);
    buildSendToMenu(menu.addMenu(tr("Send to")));

    menu.addSeparator();
    menu.addAction(m_actRename);
    menu.addAction(m_actDelete);

    menu.addSeparator();
    menu.addAction(m_actProperties);

    // Service menus attach to the selection, which a folder menu has none of
    m_itemActions->setItemListProperties(
        KFileItemListProperties(KFileItemList(selection)));
    m_itemActions->addActionsTo(&menu);
}

void MainWindow::buildFolderContextMenu(QMenu &menu)
{
    // Win7's order, arrangement, then clipboard, then what can be created
    QMenu *view = menu.addMenu(tr("View"));
    for (QAction *action : m_viewModeGroup->actions())
        view->addAction(action);

    QMenu *sort = menu.addMenu(tr("Sort by"));
    for (QAction *action : m_sortGroup->actions())
        sort->addAction(action);
    sort->addSeparator();
    sort->addAction(m_sortAscending);
    sort->addAction(m_sortDescending);

    QMenu *group = menu.addMenu(tr("Group by"));
    for (QAction *action : m_groupGroup->actions())
        group->addAction(action);

    menu.addAction(m_actRefresh);

    menu.addSeparator();
    menu.addAction(m_actPaste);

    menu.addSeparator();
    m_newFileMenu->setWorkingDirectory(operationFolder());
    m_newFileMenu->checkUpToDate();
    menu.addAction(m_newFileMenu);

    menu.addSeparator();
    menu.addAction(m_actSelectAll);
    menu.addAction(m_actInvertSelection);

    // Extracting the archive being browsed, from inside it
    if (Archives::isInsideArchive(currentUrl())) {
        menu.addSeparator();
        menu.addAction(m_actExtract);
    }

    if (operationFolder().isLocalFile()) {
        menu.addSeparator();
        menu.addAction(m_actOpenTerminal);
        // Only where it would help, offering it on a writable folder teaching
        // the user to reach for elevation by reflex
        if (m_actOpenAsAdmin->isEnabled())
            menu.addAction(m_actOpenAsAdmin);
    }

    if (isTrashView()) {
        menu.addSeparator();
        menu.addAction(m_actRestoreAll);
        menu.addAction(m_actEmptyTrash);
    }

    menu.addSeparator();
    menu.addAction(m_actProperties);
}

void MainWindow::showComputerContextMenu(const QUrl &url, const QPoint &globalPos)
{
    QMenu menu(this);

    const bool mounted = url.isValid();
    if (mounted) {
        menu.addAction(tr("Open"), this, [this, url] { navigateTo(url); });
        menu.addAction(tr("Open in new window"), this,
                       [this, url] { openNewWindow(url); });
        menu.addSeparator();
    }

    // The same route the desktop's device notifier takes, the unmount, the
    // prompt and the reporting all belonging to the system beneath
    KFilePlacesModel *places = m_places->placesModel();
    const QModelIndex placeIndex = m_computerView->placeIndexFor(url);
    if (placeIndex.isValid()) {
        if (places->setupNeeded(placeIndex)) {
            menu.addAction(tr("Connect"), this,
                           [places, placeIndex] { places->requestSetup(placeIndex); });
        } else if (places->isDevice(placeIndex)) {
            // Never the drive the running system is on
            const QString mount = places->url(placeIndex).toLocalFile();
            if (QDir::cleanPath(mount) != QLatin1String("/")) {
                menu.addAction(tr("Eject"), this,
                               [places, placeIndex] { places->requestEject(placeIndex); });
                menu.addAction(tr("Disconnect"), this,
                               [places, placeIndex] { places->requestTeardown(placeIndex); });
            }
        }
        menu.addAction(tr("Rename..."), this, [this, places, placeIndex] {
            // The places model's label, the volume's own belonging to a
            // partition tool
            bool ok = false;
            const QString name = QInputDialog::getText(
                this, tr("Rename"), tr("Drive name:"), QLineEdit::Normal,
                places->text(placeIndex), &ok);
            if (ok && !name.isEmpty()) {
                places->editPlace(placeIndex, name, places->url(placeIndex),
                                  places->icon(placeIndex).name());
            }
        });
        menu.addSeparator();
    }

    menu.addAction(m_actConnectDrives);
    menu.addAction(m_actRefresh);

    QMenu *sort = menu.addMenu(tr("Sort by"));
    const std::pair<const char *, ComputerModel::SortKey> keys[] = {
        {QT_TR_NOOP("Name"),       ComputerModel::SortByName},
        {QT_TR_NOOP("Total size"), ComputerModel::SortBySize},
        {QT_TR_NOOP("Free space"), ComputerModel::SortByFree},
    };
    for (const auto &[label, key] : keys) {
        QAction *action = sort->addAction(tr(label));
        action->setCheckable(true);
        action->setChecked(m_computerModel->sortKey() == key);
        connect(action, &QAction::triggered, this,
                [this, key] { m_computerModel->setSortKey(key); });
    }

    if (mounted) {
        menu.addSeparator();
        menu.addAction(tr("Properties"), this, [this, url] {
            FileOps::showProperties({KFileItem(url)}, this);
        });
    }

    menu.exec(globalPos);
}

void MainWindow::buildSendToMenu(QMenu *menu)
{
    const QList<QUrl> urls = selectedUrls();
    if (urls.isEmpty()) {
        menu->setEnabled(false);
        return;
    }

    // Local files only, the archiver working on paths rather than locations
    const bool allLocal = std::all_of(urls.cbegin(), urls.cend(),
                                      [](const QUrl &url) { return url.isLocalFile(); });
    if (allLocal && FileOps::canCompress()) {
        menu->addAction(Aero::themeIcon({"application-zip", "package-x-generic"}),
                        tr("Compressed (zipped) folder"), this, [this, urls] {
            if (!FileOps::compress(urls, this))
                m_details->showMessage(tr("Creating an archive needs Ark."));
        });
        menu->addSeparator();
    }

    const QString desktop = QStandardPaths::writableLocation(
        QStandardPaths::DesktopLocation);
    if (!desktop.isEmpty()) {
        menu->addAction(Aero::themeIcon({"user-desktop"}), tr("Desktop (create shortcut)"),
                        this, [this, urls, desktop] {
            FileOps::createLink(urls, QUrl::fromLocalFile(desktop), this);
        });
    }

    const std::pair<QStandardPaths::StandardLocation, const char *> folders[] = {
        {QStandardPaths::DocumentsLocation, "folder-documents"},
        {QStandardPaths::PicturesLocation,  "folder-pictures"},
        {QStandardPaths::MusicLocation,     "folder-music"},
    };
    for (const auto &[location, iconName] : folders) {
        const QString path = QStandardPaths::writableLocation(location);
        if (path.isEmpty())
            continue;
        const QUrl target = QUrl::fromLocalFile(path);
        menu->addAction(Aero::themeIcon({iconName, "folder"}), QDir(path).dirName(),
                        this, [this, urls, target] {
            FileOps::copy(urls, target, this);
        });
    }

    // Removable drives only
    bool separatorAdded = false;
    for (int row = 0; row < m_computerModel->rowCount(); ++row) {
        const QModelIndex index = m_computerModel->index(row, ComputerModel::Name);
        if (!index.data(ComputerModel::RemovableRole).toBool())
            continue;
        const QUrl target = index.data(ComputerModel::UrlRole).toUrl();
        if (!target.isValid())
            continue;
        if (!separatorAdded) {
            menu->addSeparator();
            separatorAdded = true;
        }
        menu->addAction(qvariant_cast<QIcon>(index.data(Qt::DecorationRole)),
                        index.data(Qt::DisplayRole).toString(), this,
                        [this, urls, target] { FileOps::copy(urls, target, this); });
    }
}

void MainWindow::selectOnArrival(const QList<QUrl> &urls)
{
    m_pendingSelection = urls;
    m_pendingRename = false;
    // Tried straight away too, a reveal for the folder already on screen having
    // nothing to wait for
    applyPendingSelection(!m_loading);
}

void MainWindow::applyPendingSelection(bool listingFinished)
{
    if (m_pendingSelection.isEmpty() || isComputerView() || isLibrariesView())
        return;

    if (m_pendingRename) {
        // The new menu's case, straight into the rename editor once listed
        const QUrl target = m_pendingSelection.constFirst();
        if (!m_fileView->selectUrl(target, true)) {
            if (listingFinished)
                m_pendingSelection.clear();
            return;
        }
        m_pendingSelection.clear();
        m_pendingRename = false;
        return;
    }

    m_pendingSelection = m_fileView->selectUrls(m_pendingSelection);

    // Dropped once the listing ends, found or not, or a request left pending
    // hijacks the user's next selection
    if (listingFinished)
        m_pendingSelection.clear();
}

void MainWindow::renameSelection()
{
    const QList<KFileItem> selection = selectedItems();
    if (selection.isEmpty())
        return;

    // A rename over several files opens one editor and applies what it commits
    // to all of them, and they are remembered now since opening the editor
    // moves the current index
    m_batchRename.clear();
    if (selection.size() > 1)
        m_batchRename = selection;

    // The editor reports the new name rather than writing to the model, so the
    // rename goes through KIO and lands in the undo history
    m_fileView->renameItem(selection.first().url());
}

void MainWindow::applyRename(const QUrl &url, const QString &newName)
{
    const QList<KFileItem> batch = m_batchRename;
    m_batchRename.clear();

    // Checked against what was actually committed, so a batch left over from an
    // abandoned rename cannot capture the next single one
    const bool sameBatch = std::any_of(batch.cbegin(), batch.cend(),
                                       [&url](const KFileItem &item) {
        return item.url() == url;
    });

    bool renamed = false;
    if (batch.size() > 1 && sameBatch)
        renamed = FileOps::renameBatch(batch, newName, this);
    else
        renamed = FileOps::rename(url, newName, this);

    if (!renamed)
        m_fileView->selectUrl(url);
}

void MainWindow::createNewFolder()
{
    if (isComputerView() || isLibrariesView())
        return;   // shell roots must never reach a writable KIO operation

    m_newFileMenu->setWorkingDirectory(operationFolder());
    m_newFileMenu->createDirectory();
}

QList<KFileItem> MainWindow::selectedItems() const
{
    if (m_selectionValid)
        return m_selection;

    // The drives page has devices for rows, and its indices bear no relation to
    // the directory model's
    m_selection.clear();
    if (m_fileView && !isComputerView() && !isLibrariesView())
        m_selection = m_model->itemsForIndexes(m_fileView->selectedIndexes());
    m_selectionValid = true;
    return m_selection;
}

void MainWindow::invalidateSelection()
{
    m_selectionValid = false;
    m_selection.clear();
}

QList<QUrl> MainWindow::selectedUrls() const
{
    QList<QUrl> urls;
    const QList<KFileItem> items = selectedItems();
    urls.reserve(items.size());
    for (const KFileItem &item : items)
        urls.append(item.url());
    return urls;
}

QUrl MainWindow::currentUrl() const
{
    return m_navigation->currentUrl();
}

Settings::ViewMode MainWindow::currentViewMode() const
{
    if (isComputerView())
        return m_computerView->viewMode();
    if (isLibrariesView())
        return m_librariesView->viewMode();
    return m_fileView->viewMode();
}
