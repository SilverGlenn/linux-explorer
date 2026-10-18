#include "BreadcrumbBar.h"
#include "ComputerModel.h"
#include "ComputerView.h"
#include "DetailsPane.h"
#include "DirectoryModel.h"
#include "DriveLabel.h"
#include "FileManagerService.h"
#include "FileView.h"
#include "FolderDragData.h"
#include "Libraries.h"
#include "LibrariesView.h"
#include "Locations.h"
#include "MainWindow.h"
#include "ApplicationAppearance.h"
#include "NavigationPane.h"
#include "PreviewPane.h"
#include "Settings.h"
#include "SearchBox.h"
#include "ViewModeMenu.h"
#include "aero/contextmenu.h"
#include "aero/fonts.h"
#include <AeroQt/stylesheet.h>
#include <AeroQt/navbuttons.h>
#include <KFilePlacesModel>

#include <QActionGroup>
#include <QAbstractButton>
#include <QApplication>
#include <QAccessible>
#include <QContextMenuEvent>
#include <QClipboard>
#include <QDialog>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDir>
#include <QDropEvent>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDrag>
#include <QFile>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QListView>
#include <QMenuBar>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPainter>
#include <QPdfWriter>
#include <QScopeGuard>
#include <QPointer>
#include <QPushButton>
#include <QProcess>
#include <QScreen>
#include <QSettings>
#include <QSignalSpy>
#include <QSlider>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QTimer>
#include <QTest>
#include <QToolButton>
#include <QTreeView>
#include <QTreeWidget>
#include <QWidgetAction>
#include <QWindow>
#include <memory>

namespace {
// Stable drives let the grouping tests run without depending on host devices.
class TestComputerModel : public ComputerModel {
public:
    explicit TestComputerModel(KFilePlacesModel *places) : ComputerModel(places) {}
    int rowCount(const QModelIndex &parent = {}) const override
    {
        return parent.isValid() ? 0 : (fixedPresent ? 2 : 1);
    }
    QVariant data(const QModelIndex &index, int role) const override
    {
        const bool removable = !fixedPresent || index.row() == 1;
        if (role == RemovableRole) return removable;
        if (role == UrlRole)
            return QUrl::fromLocalFile(removable ? QStringLiteral("/media/test") : QStringLiteral("/"));
        if (role == Qt::DisplayRole && index.column() == Name)
            return removable ? QStringLiteral("USB Disk") : QStringLiteral("Local Disk (C:)");
        return {};
    }
    void resetDrives(bool present = true)
    {
        beginResetModel();
        fixedPresent = present;
        endResetModel();
    }
private:
    bool fixedPresent = true;
};

QAction *actionNamed(QWidget &window, const QString &name)
{
    for (QAction *action : window.findChildren<QAction *>()) {
        if (action->text().remove(QLatin1Char('&')) == name) return action;
    }
    return nullptr;
}

QTreeWidgetItem *heading(QTreeWidget *tree, const QUrl &url)
{
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = tree->topLevelItem(i);
        if (item->data(0, Qt::UserRole).toUrl() == url) return item;
    }
    return nullptr;
}
} // namespace

class ExplorerUiTest : public QObject {
    Q_OBJECT
private:
    QTemporaryDir m_files;
    QUrl m_folder;
    QUrl m_notes;
    QUrl m_readme;

    bool writeFile(const QString &name, const QByteArray &data)
    {
        QFile file(m_folder.toLocalFile() + QLatin1Char('/') + name);
        return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
    }
    std::unique_ptr<MainWindow> window(const QUrl &url)
    {
        auto result = std::make_unique<MainWindow>(url);
        result->setAttribute(Qt::WA_DeleteOnClose, false);
        result->show();
        result->activateWindow();
        QTest::qWait(100);
        return result;
    }
    void navigate(MainWindow &window, const QUrl &url)
    {
        window.findChild<NavigationPane *>()->urlActivated(url);
    }

private Q_SLOTS:
    void initTestCase()
    {
        qRegisterMetaType<QDropEvent *>();
        const QString expectedDisplay = qEnvironmentVariable("EXPLORER_EXPECT_DISPLAY");
        if (!expectedDisplay.isEmpty()) {
            const QStringList dimensions = expectedDisplay.split(QLatin1Char('x'));
            QCOMPARE(dimensions.size(), 2);
            QScreen *screen = QApplication::primaryScreen();
            const qreal scale = qEnvironmentVariable("EXPLORER_EXPECT_SCALE").toDouble();
            QCOMPARE(screen->devicePixelRatio(), scale);
            QCOMPARE(qRound(screen->geometry().width() * scale), dimensions.at(0).toInt());
            QCOMPARE(qRound(screen->geometry().height() * scale), dimensions.at(1).toInt());
        }
        QVERIFY(m_files.isValid());
        const QString folder = m_files.path() + QStringLiteral("/Explorer Reference");
        QVERIFY(QDir().mkpath(folder + QStringLiteral("/Photos")));
        QVERIFY(QDir().mkpath(folder + QStringLiteral("/Project notes")));
        m_folder = QUrl::fromLocalFile(folder);
        m_notes = QUrl::fromLocalFile(folder + QStringLiteral("/Notes.txt"));
        m_readme = QUrl::fromLocalFile(folder + QStringLiteral("/Readme.txt"));
        QVERIFY(writeFile(QStringLiteral("Notes.txt"), "Windows 7 layout\n"));
        QVERIFY(writeFile(QStringLiteral("Readme.txt"), "Explorer reference\n"));
        for (const auto &entry : Libraries::entries())
            QVERIFY(QDir().mkpath(entry.url.toLocalFile()));
        Settings::setNotifyUnmountedDrives(false);
        Settings::setSingleClickToOpen(false);
        Settings::setHideKnownExtensions(false);
        Settings::setDefaultViewMode(Settings::ViewMode::Details);
    }

    void viewButtonUsesNativeFiveModeCycle()
    {
        using Mode = Settings::ViewMode;
        QCOMPARE(ViewModeMenu::nextMode(Mode::LargeIcons), Mode::List);
        QCOMPARE(ViewModeMenu::nextMode(Mode::List), Mode::Details);
        QCOMPARE(ViewModeMenu::nextMode(Mode::Details), Mode::Tiles);
        QCOMPARE(ViewModeMenu::nextMode(Mode::Tiles), Mode::Content);
        QCOMPARE(ViewModeMenu::nextMode(Mode::Content), Mode::LargeIcons);
        QCOMPARE(ViewModeMenu::nextMode(Mode::MediumIcons), Mode::List);
    }

    void viewSliderAndSharedActionsStayInSync()
    {
        QActionGroup group(this);
        for (int i = 0; i <= int(Settings::ViewMode::Content); ++i) {
            auto *action = new QAction(QStringLiteral("Mode %1").arg(i), &group);
            action->setData(i);
            action->setCheckable(true);
            group.addAction(action);
        }
        group.actions().at(5)->setChecked(true);
        ViewModeMenu menu(&group);
        menu.popup(QPoint(30, 30));
        auto *slider = menu.findChild<QSlider *>(QStringLiteral("viewModeSlider"));
        QVERIFY(slider);
        QCOMPARE(slider->value(), 2);
        QSignalSpy triggered(&group, &QActionGroup::triggered);
        slider->setValue(6);
        QCOMPARE(group.checkedAction()->data().toInt(), 1);
        QCOMPARE(triggered.count(), 1);
        for (QAction *action : group.actions()) {
            action->setChecked(true);
            QCOMPARE(slider->value(), 7 - action->data().toInt());
        }
        auto *choice = menu.findChild<QToolButton *>(QStringLiteral("viewMode3"));
        QVERIFY(choice);
        choice->click();
        QCOMPARE(group.checkedAction()->data().toInt(), 3);
        QVERIFY(menu.isHidden());
    }

    void breadcrumbOverflowKeepsCurrentFolderAndDropdown()
    {
        BreadcrumbBar bar;
        const QUrl current(QStringLiteral("file:///tmp/a/b/c"));
        bar.setTrail(QIcon(), {
            {QStringLiteral("Computer"), Locations::computer()},
            {QStringLiteral("Local Disk"), QUrl(QStringLiteral("file:///"))},
            {QStringLiteral("A very long ancestor folder"), QUrl(QStringLiteral("file:///tmp/a"))},
            {QStringLiteral("Current folder"), current}
        });
        bar.resize(150, 22);
        bar.show();
        QTest::qWait(30);
        QVERIFY(bar.hiddenAncestorCount() > 0);
        QList<QToolButton *> visible;
        for (auto *arrow : bar.findChildren<QToolButton *>(QStringLiteral("crumbDropdown")))
            if (arrow->isVisible()) visible.append(arrow);
        QVERIFY(!visible.isEmpty());
        QSignalSpy menu(&bar, &BreadcrumbBar::menuRequested);
        visible.last()->click();
        QCOMPARE(menu.count(), 1);
        QCOMPARE(menu.takeFirst().at(0).toUrl(), current);
        auto *leading = bar.findChild<QToolButton *>(QStringLiteral("breadcrumbLocations"));
        QVERIFY(leading->property("overflow").toBool());
        leading->click();
        QVERIFY(!menu.takeFirst().at(0).toUrl().isValid());
        bar.resize(1000, 22);
        QTRY_COMPARE(bar.hiddenAncestorCount(), 0);
    }

    void librariesUseRealXdgFoldersAndDoubleClick()
    {
        LibrariesView view;
        view.resize(600, 300);
        view.show();
        auto *list = view.findChild<QListView *>(QStringLiteral("libraryList"));
        QVERIFY(list);
        QCOMPARE(view.libraryCount(), Libraries::entries().size());
        QVERIFY(view.libraryCount() > 0);
        QCOMPARE(list->itemDelegate()->sizeHint(QStyleOptionViewItem(), list->model()->index(0, 0)), QSize(256, 64));
        QCOMPARE(list->model()->index(0, 0).data(Qt::UserRole).toUrl(), Libraries::entries().first().url);
        QTest::qWait(30);
        const QPoint pos = list->visualRect(list->model()->index(0, 0)).center();
        QSignalSpy opened(&view, &LibrariesView::urlActivated);
        QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, pos);
        QCOMPARE(opened.count(), 0);
        QTest::mouseDClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, pos);
        QCOMPARE(opened.count(), 1);
        QCOMPARE(opened.first().at(0).toUrl(), Libraries::entries().first().url);
        QTest::keyClick(list, Qt::Key_Return);
        QCOMPARE(opened.count(), 2);
    }

    void libraryTilesKeepTwoColumnsAtTheGuestWindowWidth()
    {
        LibrariesView view;
        view.resize(528, 300);
        view.show();
        auto *list = view.findChild<QListView *>(QStringLiteral("libraryList"));
        QTest::qWait(50);
        const QRect first = list->visualRect(list->model()->index(0, 0));
        const QRect second = list->visualRect(list->model()->index(1, 0));
        const QRect third = list->visualRect(list->model()->index(2, 0));
        QCOMPARE(second.top(), first.top());
        QCOMPARE(third.top() - first.top(), 64);
        QVERIFY(list->viewport()->rect().contains(second));
    }

    void libraryTilesKeepTwoColumnsInTheReferenceSizedWindow()
    {
        auto win = window(Locations::libraries());
        win->resize(686, 440);
        QTest::qWait(150);
        auto *list = win->findChild<QListView *>(QStringLiteral("libraryList"));
        const QRect first = list->visualRect(list->model()->index(0, 0));
        const QRect second = list->visualRect(list->model()->index(1, 0));
        QCOMPARE(first.top(), second.top());
    }

    void freshWindowFitsAvailableScreen()
    {
        Settings::setWindowGeometry({});
        auto win = window(Locations::libraries());
        const QRect available = win->screen()->availableGeometry();
        QVERIFY(win->frameGeometry().width() <= available.width());
        QVERIFY(win->frameGeometry().height() <= available.height());
    }

    void librariesDetailsKeepsSelectionAndHonestMetadata()
    {
        LibrariesView view;
        view.resize(640, 350);
        view.show();
        auto *list = view.findChild<QListView *>(QStringLiteral("libraryList"));
        auto *tree = view.findChild<QTreeView *>(QStringLiteral("libraryDetails"));
        QVERIFY(list && tree);
        const QUrl first = list->model()->index(0, 0).data(Qt::UserRole).toUrl();
        list->setCurrentIndex(list->model()->index(0, 0));
        view.setViewMode(Settings::ViewMode::Details);
        QVERIFY(tree->isVisible());
        QCOMPARE(tree->selectionModel(), list->selectionModel());
        QVERIFY(tree->selectionModel()->isRowSelected(0, QModelIndex()));
        QCOMPARE(view.selectedUrl(), first);
        const QStringList columns{QStringLiteral("Name"), QStringLiteral("Date modified"),
                                  QStringLiteral("Type"), QStringLiteral("Size")};
        for (int i = 0; i < columns.size(); ++i)
            QCOMPARE(tree->model()->headerData(i, Qt::Horizontal).toString(), columns.at(i));
        QCOMPARE(tree->model()->index(0, 1).data().toString(), QString());
        QCOMPARE(tree->model()->index(0, 2).data().toString(), QStringLiteral("Library"));
        QCOMPARE(tree->model()->index(0, 3).data().toString(), QString());
        QVERIFY(!tree->model()->index(0, 0).flags().testFlag(Qt::ItemIsEditable));
        tree->setCurrentIndex(tree->model()->index(0, 2));
        QCOMPARE(view.selectedUrl(), first);
        QSignalSpy opened(&view, &LibrariesView::urlActivated);
        QTest::keyClick(tree, Qt::Key_Return);
        QCOMPARE(opened.first().at(0).toUrl(), first);
        view.refresh();
        QCOMPARE(view.selectedUrl(), first);
        view.setViewMode(Settings::ViewMode::Tiles);
        QVERIFY(list->isVisible());
        QCOMPARE(view.selectedUrl(), first);
    }

    void librariesRefreshUpdatesFooterAndPreservesSelection()
    {
        auto win = window(Locations::libraries());
        auto *view = win->findChild<LibrariesView *>();
        auto *list = view->findChild<QListView *>(QStringLiteral("libraryList"));
        auto *primary = win->findChild<QLabel *>(QStringLiteral("detailsPrimary"));
        QVERIFY(list && primary);
        list->setCurrentIndex(list->model()->index(0, 0));
        const QUrl selected = view->selectedUrl();
        const QString name = view->selectedName();
        QTest::keyClick(win.get(), Qt::Key_F5);
        QCOMPARE(view->selectedUrl(), selected);
        QCOMPARE(primary->toolTip(), name);
        list->clearSelection();
        QTest::keyClick(win.get(), Qt::Key_F5);
        QCOMPARE(primary->toolTip(), QStringLiteral("%1 items").arg(Libraries::entries().size()));
    }

    void librariesKeyboardContextMenuTargetsTheCurrentRow()
    {
        LibrariesView view;
        view.resize(640, 350);
        view.setViewMode(Settings::ViewMode::Details);
        view.show();
        auto *tree = view.findChild<QTreeView *>(QStringLiteral("libraryDetails"));
        QVERIFY(tree);
        tree->setCurrentIndex(tree->model()->index(0, 2));
        const QUrl selected = view.selectedUrl();
        QSignalSpy opened(&view, &LibrariesView::urlActivated);
        bool hasMenu = false;
        QTimer::singleShot(0, &view, [&] {
            auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            hasMenu = menu && menu->defaultAction();
            if (menu) {
                menu->defaultAction()->trigger();
                menu->close();
            }
        });
        QContextMenuEvent event(QContextMenuEvent::Keyboard, QPoint(-1, -1),
                                 tree->mapToGlobal(QPoint(-1, -1)));
        QApplication::sendEvent(tree, &event);
        QVERIFY(hasMenu);
        QCOMPARE(opened.size(), 1);
        QCOMPARE(opened.first().at(0).toUrl(), selected);
    }

    void classicMenuBarUsesNativeFiveMenus()
    {
        auto win = window(Locations::libraries());
        QStringList names;
        for (QAction *action : win->menuBar()->actions())
            names.append(action->text().remove(QLatin1Char('&')));
        QCOMPARE(names, QStringList({"File", "Edit", "View", "Tools", "Help"}));
        QVERIFY(actionNamed(*win, QStringLiteral("View help")));
        QVERIFY(actionNamed(*win, QStringLiteral("About File Explorer")));
    }

    void addFoldersToFavoritesFromOutsideSidebar_data()
    {
        QTest::addColumn<QString>("source");
        QTest::newRow("folder-menu") << QStringLiteral("folder");
        QTest::newRow("multiple-folders") << QStringLiteral("multiple");
        QTest::newRow("folder-background") << QStringLiteral("background");
        QTest::newRow("favorites-heading") << QStringLiteral("heading");
        QTest::newRow("file-menu") << QStringLiteral("file");
        QTest::newRow("mixed-selection") << QStringLiteral("mixed");
    }

    void addFoldersToFavoritesFromOutsideSidebar()
    {
        QFETCH(QString, source);
        auto win = window(m_folder);
        auto *pane = win->findChild<NavigationPane *>();
        auto *view = win->findChild<FileView *>();
        auto *tree = pane->findChild<QTreeWidget *>(QStringLiteral("win7NavPane"));
        KFilePlacesModel *places = pane->placesModel();
        const QUrl photos = QUrl::fromLocalFile(m_folder.toLocalFile() + QStringLiteral("/Photos"));
        const QUrl notes = QUrl::fromLocalFile(m_folder.toLocalFile() + QStringLiteral("/Project notes"));
        QList<QUrl> targets;
        if (source == QLatin1String("folder"))
            targets = {photos};
        else if (source == QLatin1String("multiple"))
            targets = {photos, notes};
        else if (source == QLatin1String("file"))
            targets = {m_notes};
        else if (source == QLatin1String("mixed"))
            targets = {photos, m_notes};
        else
            targets = {m_folder};
        const auto cleanup = qScopeGuard([&] {
            for (int row = places->rowCount() - 1; row >= 0; --row) {
                const QModelIndex index = places->index(row, 0);
                if (targets.contains(places->url(index)))
                    places->removePlace(index);
            }
        });
        const auto favoriteCount = [&](const QUrl &url) {
            int count = 0;
            for (int row = 0; row < tree->topLevelItemCount(); ++row) {
                QTreeWidgetItem *group = tree->topLevelItem(row);
                if (group->text(0) != QLatin1String("Favorites")) continue;
                for (int i = 0; i < group->childCount(); ++i)
                    if (group->child(i)->data(0, Qt::UserRole).toUrl().matches(url, QUrl::StripTrailingSlash))
                        ++count;
            }
            return count;
        };
        for (const QUrl &target : targets)
            QCOMPARE(favoriteCount(target), 0);

        const bool onItem = source != QLatin1String("background") && source != QLatin1String("heading");
        if (onItem)
            QTRY_VERIFY(view->selectUrls(targets).isEmpty());

        bool sawMenu = false;
        bool sawAdd = false;
        const QString command = source == QLatin1String("heading")
            ? QStringLiteral("Add current location to Favorites") : QStringLiteral("Add to Favorites");
        QTimer::singleShot(0, win.get(), [&] {
            auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            if (!menu) return;
            sawMenu = true;
            for (QAction *action : menu->actions()) {
                if (action->text() != command) continue;
                sawAdd = action->isEnabled();
                if (sawAdd) action->trigger();
                break;
            }
            menu->close();
        });
        if (source == QLatin1String("heading")) {
            QTreeWidgetItem *favorites = nullptr;
            for (int row = 0; row < tree->topLevelItemCount(); ++row)
                if (tree->topLevelItem(row)->text(0) == QLatin1String("Favorites"))
                    favorites = tree->topLevelItem(row);
            QVERIFY(favorites);
            tree->customContextMenuRequested(tree->visualItemRect(favorites).center());
        } else {
            view->contextMenuRequested(view->mapToGlobal(QPoint(50, 50)), onItem);
        }
        QVERIFY(sawMenu);
        const bool shouldAdd = source != QLatin1String("file") && source != QLatin1String("mixed");
        QCOMPARE(sawAdd, shouldAdd);
        for (const QUrl &target : targets)
            QTRY_COMPARE(favoriteCount(target), shouldAdd ? 1 : 0);
    }

    void dragMainViewFoldersIntoFavorites_data()
    {
        QTest::addColumn<QString>("scenario");
        QTest::newRow("heading-details") << QStringLiteral("details");
        QTest::newRow("heading-icons") << QStringLiteral("icons");
        QTest::newRow("heading-grouped-details") << QStringLiteral("grouped");
        QTest::newRow("insert-multiple") << QStringLiteral("multiple");
        QTest::newRow("existing-favorite") << QStringLiteral("duplicate");
        QTest::newRow("collapsed-heading") << QStringLiteral("collapsed");
        QTest::newRow("file-rejected") << QStringLiteral("file");
        QTest::newRow("mixed-rejected") << QStringLiteral("mixed");
    }

    void dragMainViewFoldersIntoFavorites()
    {
        QFETCH(QString, scenario);
        const QStringList oldOrder = Settings::favoritesOrder();
        Settings::setFavoritesOrder({});
        const auto restoreOrder = qScopeGuard([&] { Settings::setFavoritesOrder(oldOrder); });
        auto win = window(m_folder);
        auto *pane = win->findChild<NavigationPane *>();
        auto *view = win->findChild<FileView *>();
        auto *tree = pane->findChild<QTreeWidget *>();
        auto *places = pane->placesModel();
        const QUrl photos = QUrl::fromLocalFile(m_folder.toLocalFile() + QStringLiteral("/Photos"));
        const QUrl notes = QUrl::fromLocalFile(m_folder.toLocalFile() + QStringLiteral("/Project notes"));
        const auto cleanup = qScopeGuard([&] {
            for (int row = places->rowCount() - 1; row >= 0; --row) {
                const QModelIndex index = places->index(row, 0);
                if (places->url(index) == photos || places->url(index) == notes)
                    places->removePlace(index);
            }
        });
        if (scenario == QLatin1String("icons")) view->setViewMode(Settings::ViewMode::LargeIcons);
        else view->setViewMode(Settings::ViewMode::Details);
        if (scenario == QLatin1String("grouped")) view->setGroupColumn(DirectoryModel::Type);
        if (scenario == QLatin1String("duplicate")) pane->addToFavorites(photos);
        const auto favoriteGroup = [&] {
            for (int row = 0; row < tree->topLevelItemCount(); ++row)
                if (tree->topLevelItem(row)->text(0) == QLatin1String("Favorites"))
                    return tree->topLevelItem(row);
            return static_cast<QTreeWidgetItem *>(nullptr);
        };
        const auto favoriteUrls = [&] {
            QList<QUrl> urls;
            auto *group = favoriteGroup();
            if (group)
                for (int i = 0; i < group->childCount(); ++i)
                    urls.append(group->child(i)->data(0, Qt::UserRole).toUrl());
            return urls;
        };
        QList<QUrl> selection{photos};
        if (scenario == QLatin1String("multiple")) selection.append(notes);
        if (scenario == QLatin1String("file")) selection = {m_notes};
        if (scenario == QLatin1String("mixed")) selection.append(m_notes);
        QTRY_VERIFY(view->selectUrls(selection).isEmpty());
        auto *source = view->currentView();
        std::unique_ptr<QMimeData> mime(source->model()->mimeData(source->selectionModel()->selectedIndexes()));
        QVERIFY(mime && mime->hasUrls());
        const bool shouldAdd = scenario != QLatin1String("file") && scenario != QLatin1String("mixed");
        QCOMPARE(mime->hasFormat(FolderDragData::mimeType), shouldAdd);
        const QList<QUrl> before = favoriteUrls();
        QVERIFY(!before.isEmpty());
        if (scenario == QLatin1String("collapsed")) favoriteGroup()->setExpanded(false);
        const QPoint pos = scenario == QLatin1String("multiple")
            ? tree->visualItemRect(favoriteGroup()->child(0)).topLeft() + QPoint(40, 1)
            : tree->visualItemRect(favoriteGroup()).center();
        QSignalSpy filesystemDrop(pane, &NavigationPane::dropped);
        QSignalSpy navigated(pane, &NavigationPane::urlActivated);
        const Qt::DropActions actions = Qt::MoveAction | Qt::CopyAction | Qt::LinkAction;
        QDragEnterEvent enter(pos, actions, mime.get(), Qt::LeftButton, Qt::ShiftModifier);
        QApplication::sendEvent(tree->viewport(), &enter);
        QDragMoveEvent move(pos, actions, mime.get(), Qt::LeftButton, Qt::ShiftModifier);
        QApplication::sendEvent(tree->viewport(), &move);
        QCOMPARE(move.isAccepted(), shouldAdd);
        if (shouldAdd) {
            QCOMPARE(move.dropAction(), Qt::LinkAction);
            auto *hint = tree->findChild<QWidget *>(QStringLiteral("favoriteDragHint"));
            QVERIFY(hint && hint->isVisible());
            QCOMPARE(hint->accessibleName(), QStringLiteral("Create link in Favorites"));
            const QString captureDir = qEnvironmentVariable("EXPLORER_FAVORITES_CAPTURE_DIR");
            if (!captureDir.isEmpty()) {
                QDir().mkpath(captureDir);
                QPixmap capture = win->grab();
                QPainter painter(&capture);
                painter.drawPixmap(win->mapFromGlobal(hint->pos()), hint->grab());
                painter.end();
                capture.save(captureDir + QLatin1Char('/') + scenario + QStringLiteral(".png"));
            }
        }
        QDropEvent drop(QPointF(pos), actions, mime.get(), Qt::LeftButton, Qt::ShiftModifier);
        QApplication::sendEvent(tree->viewport(), &drop);
        QCOMPARE(drop.isAccepted(), shouldAdd);
        if (shouldAdd) QCOMPARE(drop.dropAction(), Qt::LinkAction);
        QCOMPARE(filesystemDrop.count(), 0);
        QCOMPARE(navigated.count(), 0);
        QList<QUrl> expected = before;
        if (shouldAdd) {
            int at = scenario == QLatin1String("multiple") ? 0 : expected.size();
            for (const QUrl &url : mime->urls()) {
                const int previous = expected.indexOf(url);
                if (previous >= 0) {
                    expected.removeAt(previous);
                    if (previous < at) --at;
                }
                expected.insert(at++, url);
            }
        }
        QCOMPARE(favoriteUrls(), expected);
        pane->refresh();
        QCOMPARE(favoriteUrls(), expected);
        QVERIFY(QFileInfo::exists(photos.toLocalFile()));
        QVERIFY(QFileInfo::exists(notes.toLocalFile()));
    }

    void nativeMouseFavoriteDrag_data()
    {
        QTest::addColumn<bool>("fromMainView");
        QTest::newRow("main-view-to-heading") << true;
        QTest::newRow("reorder-shortcut") << false;
    }

    void nativeMouseFavoriteDrag()
    {
        if (!qEnvironmentVariableIsSet("EXPLORER_NATIVE_FAVORITES_DRAG")
            || QGuiApplication::platformName() != QLatin1String("xcb"))
            QSKIP("Optional physical mouse drag check; run in an isolated Xvfb session.");
        QFETCH(bool, fromMainView);
        Settings::setFavoritesOrder({});
        auto win = window(m_folder);
        win->resize(800, 600);
        win->move(200, 75);
        auto *pane = win->findChild<NavigationPane *>();
        auto *view = win->findChild<FileView *>();
        view->setViewMode(Settings::ViewMode::Details);
        auto *tree = pane->findChild<QTreeWidget *>();
        auto *places = pane->placesModel();
        const QUrl photos = QUrl::fromLocalFile(m_folder.toLocalFile() + QStringLiteral("/Photos"));
        const auto cleanup = qScopeGuard([&] {
            for (int row = places->rowCount() - 1; row >= 0; --row)
                if (places->url(places->index(row, 0)) == photos)
                    places->removePlace(places->index(row, 0));
            Settings::setFavoritesOrder({});
        });
        if (!fromMainView) pane->addToFavorites(photos);
        QTRY_VERIFY(view->selectUrl(photos));
        QCoreApplication::processEvents();
        QTreeWidgetItem *favorites = nullptr;
        for (int row = 0; row < tree->topLevelItemCount(); ++row)
            if (tree->topLevelItem(row)->text(0) == QLatin1String("Favorites"))
                favorites = tree->topLevelItem(row);
        QVERIFY(favorites);
        QAbstractItemView *sourceView = fromMainView ? view->currentView() : tree;
        const QModelIndex sourceIndex = fromMainView ? view->currentView()->selectionModel()->selectedRows().first()
            : tree->indexFromItem(favorites->child(favorites->childCount() - 1));
        const QPoint source = sourceView->viewport()->mapToGlobal(sourceView->visualRect(sourceIndex).center());
        const QPoint target = tree->viewport()->mapToGlobal(fromMainView
            ? tree->visualItemRect(favorites).center()
            : tree->visualItemRect(favorites->child(0)).topLeft() + QPoint(40, 1));
        const QString captures = qEnvironmentVariable("EXPLORER_FAVORITES_CAPTURE_DIR");
        bool captured = false;
        QTimer::singleShot(800, win.get(), [&] {
            auto *hint = tree->findChild<QWidget *>(QStringLiteral("favoriteDragHint"));
            captured = hint && hint->isVisible();
            if (!captures.isEmpty()) {
                QDir().mkpath(captures);
                const QString name = fromMainView ? QStringLiteral("native-add") : QStringLiteral("native-reorder");
                win->screen()->grabWindow(0).save(captures + QLatin1Char('/') + name + QStringLiteral(".png"));
                if (auto *drag = sourceView->findChild<QDrag *>())
                    drag->pixmap().save(captures + QLatin1Char('/') + name + QStringLiteral("-drag-image.png"));
            }
        });
        QTimer::singleShot(4500, win.get(), [] { QDrag::cancel(); });
        QSignalSpy filesystemDrop(pane, &NavigationPane::dropped);
        QSignalSpy navigated(pane, &NavigationPane::urlActivated);
        QProcess mouse;
        mouse.start(QStringLiteral("xdotool"), {QStringLiteral("mousemove"), QString::number(source.x()), QString::number(source.y()),
            QStringLiteral("mousedown"), QStringLiteral("1"), QStringLiteral("sleep"), QStringLiteral("0.15"),
            QStringLiteral("mousemove"), QString::number(source.x() + 15), QString::number(source.y()),
            QStringLiteral("sleep"), QStringLiteral("0.15"), QStringLiteral("mousemove"), QString::number(target.x()),
            QString::number(target.y()), QStringLiteral("sleep"), QStringLiteral("1.2"), QStringLiteral("mouseup"), QStringLiteral("1")});
        QVERIFY(mouse.waitForStarted());
        QTRY_VERIFY_WITH_TIMEOUT(captured, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(mouse.state() == QProcess::NotRunning, 3000);
        QCOMPARE(mouse.exitCode(), 0);
        QCOMPARE(filesystemDrop.count(), 0);
        QCOMPARE(navigated.count(), 0);
        favorites = nullptr;
        for (int row = 0; row < tree->topLevelItemCount(); ++row)
            if (tree->topLevelItem(row)->text(0) == QLatin1String("Favorites"))
                favorites = tree->topLevelItem(row);
        QVERIFY(favorites);
        QCOMPARE(favorites->child(fromMainView ? favorites->childCount() - 1 : 0)->data(0, Qt::UserRole).toUrl(), photos);
        QVERIFY(QFileInfo::exists(photos.toLocalFile()));
    }

    void itemContextMenuUsesCompactWindowsCommandGroups_data()
    {
        QTest::addColumn<bool>("folder");
        QTest::addColumn<bool>("extended");
        QTest::newRow("folder") << true << false;
        QTest::newRow("folder-shift") << true << true;
        QTest::newRow("file") << false << false;
    }

    void itemContextMenuUsesCompactWindowsCommandGroups()
    {
        QFETCH(bool, folder);
        QFETCH(bool, extended);
        auto win = window(m_folder);
        auto *model = win->findChild<DirectoryModel *>();
        auto *view = win->findChild<FileView *>();
        const QUrl target = folder
            ? QUrl::fromLocalFile(m_folder.toLocalFile() + QStringLiteral("/Project notes")) : m_notes;
        QTRY_VERIFY(model->indexForUrl(target).isValid());
        QVERIFY(view->selectUrl(target));
        if (extended) {
            QTest::keyPress(win->windowHandle(), Qt::Key_Shift, Qt::ShiftModifier);
            QCoreApplication::processEvents();
        }
        const auto releaseShift = qScopeGuard([&] {
            if (extended) QTest::keyRelease(win->windowHandle(), Qt::Key_Shift);
        });
        QStringList commands;
        bool sawMenu = false;
        bool hasWidgetRows = false;
        bool hasShortcutCaptions = false;
        QString defaultCommand;
        QTimer::singleShot(0, win.get(), [&] {
            auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            if (!menu) return;
            const auto close = qScopeGuard([menu] { menu->close(); });
            sawMenu = true;
            if (menu->defaultAction()) {
                defaultCommand = menu->defaultAction()->text();
                QVERIFY(menu->defaultAction()->font().bold());
            }
            for (QAction *action : menu->actions()) {
                if (action->isSeparator()) continue;
                // Shell popups reserve the icon gutter even when no action has
                // an icon, plus generous space to the right of their labels.
                QVERIFY(menu->width() >= QFontMetrics(menu->font()).horizontalAdvance(
                    action->text().remove(QLatin1Char('&'))) + 84);
                QVERIFY(menu->actionGeometry(action).height() >= 22);
                commands.append(action->text().remove(QLatin1Char('&')));
                hasWidgetRows |= qobject_cast<QWidgetAction *>(action) != nullptr;
                hasShortcutCaptions |= !action->shortcut().isEmpty();
            }
            const QString snapshots = qEnvironmentVariable("EXPLORER_SNAPSHOTS");
            if (!snapshots.isEmpty()) {
                QVERIFY(QDir().mkpath(snapshots));
                QVERIFY(menu->grab().save(snapshots + QStringLiteral("/context-")
                    + QLatin1String(QTest::currentDataTag()) + QStringLiteral(".png")));
            }
            for (QAction *action : menu->actions())
                if (action->text() == QLatin1String("Copy")) action->trigger();
        });
        view->contextMenuRequested(view->mapToGlobal(QPoint(50, 50)), true);
        QVERIFY(sawMenu);
        QCOMPARE(defaultCommand, QStringLiteral("Open"));
        QCOMPARE(commands.first(), QStringLiteral("Open"));
        QCOMPARE(commands.last(), QStringLiteral("Properties"));
        QVERIFY(commands.indexOf(QStringLiteral("Send to")) < commands.indexOf(QStringLiteral("Cut")));
        QVERIFY(commands.indexOf(QStringLiteral("Copy")) < commands.indexOf(QStringLiteral("Create shortcut")));
        QVERIFY(commands.indexOf(QStringLiteral("Delete")) < commands.indexOf(QStringLiteral("Rename")));
        QCOMPARE(commands.contains(QStringLiteral("Copy as path")), extended);
        QCOMPARE(commands.contains(QStringLiteral("Open with")), !folder);
        QVERIFY(!hasWidgetRows);
        QVERIFY(!hasShortcutCaptions);
        QVERIFY(!commands.contains(QStringLiteral("Assign Tags")));
        QVERIFY(!commands.contains(QStringLiteral("Activities")));
        QVERIFY(QApplication::clipboard()->mimeData()->urls().contains(target));
        QApplication::clipboard()->clear();
        // The context copy has no caption; the actual keyboard action remains bound.
        QCOMPARE(actionNamed(*win, QStringLiteral("Copy"))->shortcut(), QKeySequence(QKeySequence::Copy));
    }

    void backgroundContextMenuKeepsSecondaryCommandsInMoreOptions()
    {
        auto win = window(m_folder);
        auto *model = win->findChild<DirectoryModel *>();
        auto *view = win->findChild<FileView *>();
        QTRY_VERIFY(model->indexForUrl(m_notes).isValid());
        QStringList commands;
        QStringList secondary;
        QTimer::singleShot(0, win.get(), [&] {
            auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            if (!menu) return;
            const auto close = qScopeGuard([menu] { menu->close(); });
            for (QAction *action : menu->actions()) {
                if (!action->isSeparator()) commands.append(action->text().remove(QLatin1Char('&')));
                if (action->text() == QLatin1String("More options") && action->menu())
                    for (QAction *extra : action->menu()->actions())
                        if (!extra->isSeparator()) secondary.append(extra->text());
            }
        });
        view->contextMenuRequested(view->mapToGlobal(QPoint(50, 50)), false);
        QCOMPARE(commands.first(), QStringLiteral("View"));
        QCOMPARE(commands.last(), QStringLiteral("Properties"));
        QVERIFY(!commands.contains(QStringLiteral("Select all")));
        QVERIFY(!commands.contains(QStringLiteral("Invert selection")));
        QVERIFY(secondary.contains(QStringLiteral("Select all")));
        QVERIFY(secondary.contains(QStringLiteral("Invert selection")));
    }

    void contextMenuMatchesMeasuredWindowsGeometry()
    {
        QMenu menu;
        QFont font = Aero::shellFont();
        menu.setFont(font);
        auto *open = menu.addAction(QStringLiteral("Open"));
        auto *next = menu.addAction(QStringLiteral("Open in new window"));
        auto *separator = menu.addSeparator();
        auto *more = menu.addMenu(QStringLiteral("Send to"));
        auto *child = more->addAction(QStringLiteral("Documents"));
        Aero::styleContextMenu(&menu);
        menu.popup(QPoint(100, 100));
        const auto close = qScopeGuard([&] { more->close(); menu.close(); });
        QTRY_VERIFY(menu.isVisible());
        const QRect first = menu.actionGeometry(open);
        const QRect second = menu.actionGeometry(next);
        QCOMPARE(first.topLeft(), QPoint(3, 3));
        QCOMPARE(first.height(), 22);
        QCOMPARE(second.top(), first.bottom() + 1);
        QCOMPARE(second.height(), 22);
        QCOMPARE(menu.actionGeometry(separator).height(), 8);
        // Different labels determine the width; Windows has no fixed minimum.
        const int labelWidth = QFontMetrics(font).horizontalAdvance(next->text());
        QVERIFY(qAbs(menu.width() - labelWidth - 90) <= 1);
        const QImage image = menu.grab().toImage();
        const qreal dpr = image.devicePixelRatio();
        QCOMPARE(image.pixelColor(qFloor(29 * dpr), qFloor(10 * dpr)), QColor(0xe2e3e3));
        QCOMPARE(image.pixelColor(qFloor(30 * dpr), qFloor(10 * dpr)), QColor(Qt::white));
        const QRect submenuRow = menu.actionGeometry(more->menuAction());
        const QPoint arrowTop(submenuRow.right() - 12, submenuRow.center().y() - 3);
        for (int column = 0; column < 4; ++column)
            for (int row = 0; row < 7; ++row) {
                const QPoint sample = arrowTop + QPoint(column, row);
                const QColor pixel = image.pixelColor(qFloor((sample.x() + 0.5) * dpr),
                                                      qFloor((sample.y() + 0.5) * dpr));
                QCOMPARE(pixel == QColor(Qt::black), row >= column && row < 7 - column);
            }
        menu.setActiveAction(more->menuAction());
        QTest::keyClick(&menu, Qt::Key_Right);
        QTRY_VERIFY(more->isVisible());
        QCOMPARE(more->actionGeometry(child).height(), 22);
        QVERIFY(qAbs(more->width() - QFontMetrics(font).horizontalAdvance(child->text()) - 75) <= 1);
        more->close();
        menu.close();

        // Long translations and larger text expand the menu rather than clip.
        next->setText(QString(12, QLatin1Char('W')));
        font.setPixelSize(24);
        menu.setFont(font);
        menu.popup(QPoint(100, 100));
        QTRY_VERIFY(menu.isVisible());
        QVERIFY(menu.width() > QFontMetrics(font).horizontalAdvance(next->text()) + 90);
        QVERIFY(menu.actionGeometry(next).height() > QFontMetrics(font).height());
    }

    void f6SkipsHiddenAndDisabledPanes()
    {
        auto win = window(Locations::libraries());
        auto *places = win->findChild<NavigationPane *>();
        auto *view = win->findChild<LibrariesView *>();
        places->hide();
        view->setFocus(Qt::TabFocusReason);
        QVERIFY(view->isAncestorOf(QApplication::focusWidget()));
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_F6);
        auto *edit = win->findChild<QLineEdit *>(QStringLiteral("pathEditor"));
        QVERIFY(edit);
        QCOMPARE(QApplication::focusWidget(), edit);
        QTest::keyClick(edit, Qt::Key_F6);
        QTRY_VERIFY(view->isAncestorOf(QApplication::focusWidget()));
        QTRY_VERIFY(!win->findChild<QLineEdit *>(QStringLiteral("pathEditor")));
    }

    void helpIsModelessAndIconOnlyControlsHaveAccessibleNames()
    {
        auto win = window(Locations::libraries());
        auto *button = win->findChild<QToolButton *>(QStringLiteral("helpButton"));
        QVERIFY(button);
        QCOMPARE(button->defaultAction(), actionNamed(*win, QStringLiteral("View help")));
        for (const auto &name : {"helpButton", "previewPaneButton", "changeViewButton",
                                 "backButton", "forwardButton"}) {
            QWidget *control = win->findChild<QWidget *>(QLatin1String(name));
            QVERIFY2(control, name);
            QAccessibleInterface *accessible = QAccessible::queryAccessibleInterface(control);
            QVERIFY(accessible);
            QVERIFY2(!accessible->text(QAccessible::Name).isEmpty(), name);
        }
        QTest::keyClick(win.get(), Qt::Key_F1);
        auto *help = win->findChild<QDialog *>(QStringLiteral("explorerHelp"));
        QVERIFY(help && help->isVisible());
        QVERIFY(!help->isModal());
        auto *content = help->findChild<QTextBrowser *>(QStringLiteral("explorerHelpContent"));
        QVERIFY(content->toPlainText().contains(QStringLiteral("Alt+P")));
        button->click();
        QCOMPARE(win->findChildren<QDialog *>(QStringLiteral("explorerHelp")).size(), 1);
        help->close();
    }

    void smallWindowsKeepFixedToolbarControlsOnscreen()
    {
        auto win = window(Locations::computer());
        win->resize(500, 400);
        QTRY_COMPARE(win->width(), 500);
        auto *bar = win->findChild<QWidget *>(QStringLiteral("commandBar"));
        auto *overflow = win->findChild<QToolButton *>(QStringLiteral("commandOverflow"));
        QTRY_VERIFY(overflow->isVisible());
        for (const auto &name : {"changeViewButton", "previewPaneButton", "helpButton"}) {
            auto *button = win->findChild<QToolButton *>(QLatin1String(name));
            QVERIFY(button && button->isVisible());
            const QRect bounds(button->mapTo(bar, QPoint()), button->size());
            QVERIFY2(bar->rect().contains(bounds), name);
        }
        auto *crumbs = win->findChild<BreadcrumbBar *>();
        QVERIFY(crumbs && crumbs->width() > 0);
    }

    void navigationLibrariesAreSelectableButNotDropTargets()
    {
        NavigationPane pane;
        pane.resize(158, 500);
        pane.show();
        auto *tree = pane.findChild<QTreeWidget *>();
        QVERIFY(tree);
        QTRY_VERIFY(heading(tree, Locations::libraries()));
        QStringList favorites;
        QTreeWidgetItem *favoriteGroup = nullptr;
        for (int i = 0; i < tree->topLevelItemCount(); ++i)
            if (tree->topLevelItem(i)->text(0) == QLatin1String("Favorites"))
                favoriteGroup = tree->topLevelItem(i);
        QVERIFY(favoriteGroup);
        for (int i = 0; i < favoriteGroup->childCount(); ++i)
            favorites.append(favoriteGroup->child(i)->text(0));
        QCOMPARE(favorites, QStringList({"Desktop", "Downloads", "Recent Places"}));
        auto *libraries = heading(tree, Locations::libraries());
        QCOMPARE(libraries->childCount(), Libraries::entries().size());
        QVERIFY(libraries->isExpanded());
        auto *computer = heading(tree, Locations::computer());
        QVERIFY(computer);
        QVERIFY(!computer->isExpanded());
        tree->setCurrentItem(libraries);
        QSignalSpy activated(&pane, &NavigationPane::urlActivated);
        QTest::keyClick(tree, Qt::Key_Return);
        QCOMPARE(activated.count(), 1);
        QCOMPARE(activated.first().at(0).toUrl(), Locations::libraries());
        QSignalSpy dropped(&pane, &NavigationPane::dropped);
        QMimeData mime;
        mime.setUrls({m_notes});
        const QPoint center = tree->visualItemRect(libraries).center();
        QDropEvent drop(QPointF(center), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(tree->viewport(), &drop);
        QCOMPARE(dropped.count(), 0);
        QVERIFY(!drop.isAccepted());
    }

    void shellRootNeverEnablesDestructiveFileOperations()
    {
        auto win = window(Locations::libraries());
        QVERIFY(win->findChild<LibrariesView *>()->isVisible());
        QCOMPARE(win->windowTitle(), QStringLiteral("Libraries"));
        for (const auto &name : {"Cut", "Copy", "Paste", "Delete", "Rename", "Properties"}) {
            QAction *action = actionNamed(*win, QLatin1String(name));
            QVERIFY2(action, name);
            QVERIFY2(!action->isEnabled(), name);
        }
        auto *search = win->findChild<QLineEdit *>(QStringLiteral("searchBox"));
        QVERIFY(search);
        QVERIFY(!search->isEnabled());
        QVERIFY(win->findChild<QPushButton *>(QStringLiteral("backButton"))->isEnabled() == false);
    }

    void navigationHistoryAndLibraryUpWork()
    {
        auto win = window(Locations::libraries());
        navigate(*win, m_folder);
        auto *model = win->findChild<DirectoryModel *>();
        QTRY_COMPARE(model->rowCount(), 4);
        QCOMPARE(win->windowTitle(), QStringLiteral("Explorer Reference"));
        auto *back = win->findChild<QPushButton *>(QStringLiteral("backButton"));
        auto *forward = win->findChild<QPushButton *>(QStringLiteral("forwardButton"));
        QVERIFY(back->isEnabled());
        back->click();
        QCOMPARE(win->windowTitle(), QStringLiteral("Libraries"));
        QVERIFY(forward->isEnabled());
        forward->click();
        QCOMPARE(win->windowTitle(), QStringLiteral("Explorer Reference"));
        navigate(*win, Libraries::entries().first().url);
        QTRY_VERIFY(win->findChild<QWidget *>(QStringLiteral("libraryFolderHeader"))->isVisible());
        QAction *up = actionNamed(*win, QStringLiteral("Up one level"));
        QVERIFY(up);
        up->trigger();
        QCOMPARE(win->windowTitle(), QStringLiteral("Libraries"));
        QVERIFY(!win->findChild<LibrariesView *>()->isHidden());
    }

    void addressEditingCancelsAndAcceptsVirtualLocations()
    {
        auto win = window(m_folder);
        win->activateWindow();
        QTRY_VERIFY(win->isActiveWindow());
        QTest::keyClick(win.get(), Qt::Key_D, Qt::AltModifier);
        auto *edit = win->findChild<QLineEdit *>(QStringLiteral("pathEditor"));
        QVERIFY(edit);
        QCOMPARE(edit->selectedText(), m_folder.toLocalFile());
        QTest::keyClicks(edit, QStringLiteral("/not/a/committed/path"));
        QTest::keyClick(edit, Qt::Key_Escape);
        QTRY_VERIFY(!win->findChild<QLineEdit *>(QStringLiteral("pathEditor")));
        QCOMPARE(win->windowTitle(), QStringLiteral("Explorer Reference"));
        QTest::keyClick(win.get(), Qt::Key_L, Qt::ControlModifier);
        edit = win->findChild<QLineEdit *>(QStringLiteral("pathEditor"));
        QVERIFY(edit);
        QTest::keyClicks(edit, QStringLiteral("libraries:///"));
        QTest::keyClick(edit, Qt::Key_Return);
        QTRY_COMPARE(win->windowTitle(), QStringLiteral("Libraries"));
    }

    void addressEditFillsItsWholeTypingSurface()
    {
        auto win = window(m_folder);
        auto *box = win->findChild<QWidget *>(QStringLiteral("addressBox"));
        QVERIFY(box);
        const int widths[] = {820, 500};
        const QString output = qEnvironmentVariable("EXPLORER_SNAPSHOTS");
        for (const int width : widths) {
            win->resize(width, 440);
            win->activateWindow();
            win->findChild<FileView *>()->focusView();
            QTRY_VERIFY(win->isActiveWindow());
            QTest::keyClick(win.get(), Qt::Key_D, Qt::AltModifier);
            auto *edit = win->findChild<QLineEdit *>(QStringLiteral("pathEditor"));
            QVERIFY(edit);
            QTRY_VERIFY(edit->hasFocus());
            QCoreApplication::processEvents();
            auto *surface = edit->parentWidget();
            QCOMPARE(edit->geometry(), surface->rect());
            QCOMPARE(edit->textMargins(), QMargins(4, 0, 4, 0));
            auto *refresh = box->findChild<QToolButton *>();
            QVERIFY(refresh);
            QVERIFY(edit->mapTo(box, edit->rect().topRight()).x() < refresh->x());
            edit->clear();
            const QImage composite = win->grab().toImage();
            const qreal dpr = composite.devicePixelRatio();
            // Scan above and below the empty editor's insertion caret.
            // The whole typing surface, including its former outer margins,
            // must be white; the address frame and refresh remain outside it.
            for (const int y : {0, surface->height() - 1}) {
                for (int x = 0; x < surface->width(); ++x) {
                    const QPoint point = surface->mapTo(win.get(), QPoint(x, y));
                    QCOMPARE(composite.pixelColor(qFloor((point.x() + 0.5) * dpr),
                                                   qFloor((point.y() + 0.5) * dpr)), QColor(Qt::white));
                }
            }
            QTest::mouseClick(win->windowHandle(), Qt::LeftButton, Qt::NoModifier,
                              surface->mapTo(win.get(), QPoint(surface->width() - 1, surface->height() / 2)));
            QVERIFY(edit->hasFocus());
            QTest::keyClicks(edit, QStringLiteral("/a path still being typed"));
            QCOMPARE(edit->text(), QStringLiteral("/a path still being typed"));
            if (!output.isEmpty()) {
                QVERIFY(QDir().mkpath(output));
                QVERIFY(win->grab().save(output + QStringLiteral("/address-edit-%1.png").arg(width)));
            }
            QTest::keyClick(edit, Qt::Key_Escape);
            QTRY_VERIFY(!win->findChild<QLineEdit *>(QStringLiteral("pathEditor")));
            auto *bar = win->findChild<BreadcrumbBar *>();
            QVERIFY(bar);
            QCOMPARE(bar->parentWidget()->layout()->contentsMargins(), QMargins(4, 0, 4, 0));
            QCOMPARE(win->windowTitle(), QStringLiteral("Explorer Reference"));
        }
    }

    void searchBoxKeepsNinePointTextAcrossFocusAndNavigationRestore()
    {
        auto win = window(m_folder);
        auto *search = win->findChild<SearchBox *>();
        auto *view = win->findChild<FileView *>();
        QVERIFY(search && view);
        view->focusView();
        QTRY_VERIFY(!search->hasFocus());
        const QString cue = QStringLiteral("Search Explorer Reference");
        QCOMPARE(search->placeholderText(), cue);
        QVERIFY(search->palette().color(QPalette::PlaceholderText).alpha() > 0);
        search->grab();
        QCOMPARE(search->font().pointSizeF(), 9.0);
        QVERIFY(search->font().italic());
        const int textHeight = search->fontMetrics().height();

        QTest::keyClick(win.get(), Qt::Key_E, Qt::ControlModifier);
        QTRY_VERIFY(search->hasFocus());
        QCOMPARE(search->palette().color(QPalette::PlaceholderText).alpha(), 0);
        QCOMPARE(search->placeholderText(), cue);
        QCOMPARE(search->accessibleName(), cue);
        QTest::keyClicks(search, QStringLiteral("Notes"));
        QCOMPARE(search->text(), QStringLiteral("Notes"));
        search->grab();
        QCOMPARE(search->font().pointSizeF(), 9.0);
        QVERIFY(!search->font().italic());
        QCOMPARE(search->fontMetrics().height(), textHeight);
        view->focusView();
        QTRY_VERIFY(!search->hasFocus());
        QVERIFY(search->palette().color(QPalette::PlaceholderText).alpha() > 0);
        QCOMPARE(search->font().pointSizeF(), 9.0);
        QVERIFY(!search->font().italic());

        // Exercise MainWindow's real QSignalBlocker restore, not a standalone
        // SearchBox mutation. No recursive search is started by typing here.
        navigate(*win, Locations::libraries());
        navigate(*win, m_folder);
        QCOMPARE(search->text(), QString());
        search->grab();
        QVERIFY(search->font().italic());
        QSignalSpy changed(search, &QLineEdit::textChanged);
        navigate(*win, Locations::search(m_folder, QStringLiteral("Readme")));
        QCOMPARE(search->text(), QStringLiteral("Readme"));
        QCOMPARE(changed.count(), 0);
        search->grab();
        QCOMPARE(search->font().pointSizeF(), 9.0);
        QVERIFY(!search->font().italic());
        QCOMPARE(search->fontMetrics().height(), textHeight);
        navigate(*win, m_folder);
        QCOMPARE(search->text(), QString());
        search->grab();
        QCOMPARE(search->font().pointSizeF(), 9.0);
        QVERIFY(search->font().italic());
    }

    void searchEditorAndMagnifierRespondToActualMouseClicks()
    {
        auto win = window(m_folder);
        auto *search = win->findChild<SearchBox *>();
        auto *view = win->findChild<FileView *>();
        QVERIFY(search && view);
        QVERIFY(search->isEnabled());
        view->focusView();
        QTRY_VERIFY(!search->hasFocus());
        const QPoint fieldPoint = search->mapTo(win.get(), QPoint(search->width() / 3, search->height() / 2));
        QCOMPARE(win->childAt(fieldPoint), search);
        QTest::mouseClick(win->windowHandle(), Qt::LeftButton, Qt::NoModifier, fieldPoint);
        QTRY_VERIFY(search->hasFocus());
        QTest::keyClicks(search, QStringLiteral("Notes"));
        QCOMPARE(search->text(), QStringLiteral("Notes"));
        search->clear();

        auto buttons = search->findChildren<QAbstractButton *>();
        QVERIFY(!buttons.isEmpty());
        QAbstractButton *magnifier = nullptr;
        for (auto *button : buttons)
            if (button->isVisible()) magnifier = button;
        QVERIFY(magnifier);
        view->focusView();
        QTRY_VERIFY(!search->hasFocus());
        const QPoint iconPoint = magnifier->mapTo(win.get(), magnifier->rect().center());
        QTest::mouseClick(win->windowHandle(), Qt::LeftButton, Qt::NoModifier, iconPoint);
        QTRY_VERIFY(search->hasFocus());
        QTest::keyClicks(search, QStringLiteral("Readme"));
        QCOMPARE(search->text(), QStringLiteral("Readme"));
    }

    void focusedSearchKeepsItsFrameInTheActualNavigationBar()
    {
        auto win = window(m_folder);
        auto *search = win->findChild<SearchBox *>();
        QVERIFY(search);
        search->setFocus();
        QTRY_VERIFY(search->hasFocus());
        const QImage focused = search->grab().toImage();
        const auto checkFrame = [search, &focused] {
            const QImage image = search->grab().toImage();
            const qreal dpr = image.devicePixelRatio();
            const int frame = qFloor(4 * dpr);
            const int center = image.width() / 2;
            // The Aero entry has a dark inset below its translucent outer
            // highlight, not a solid blue one-pixel custom border. It must
            // retain the same texture when the editor paints text/selection.
            return search->hasFrame()
                && image.pixelColor(center, qRound(dpr)).lightness()
                    < image.pixelColor(center, qRound(5 * dpr)).lightness()
                && image.copy(0, 0, image.width(), frame)
                    == focused.copy(0, 0, focused.width(), frame)
                && image.copy(0, image.height() - frame, image.width(), frame)
                    == focused.copy(0, focused.height() - frame, focused.width(), frame);
        };
        QVERIFY(checkFrame());
        QTest::keyClicks(search, QStringLiteral("Notes"));
        QVERIFY(checkFrame());
        QCOMPARE(search->font().pointSizeF(), 9.0);
        search->selectAll();
        QVERIFY(checkFrame());
        search->clear();
        QVERIFY(checkFrame());
    }

    void collapsedNavigationGroupsSurviveMainWindowReconstruction()
    {
        const bool oldFavorites = Settings::navigationGroupExpanded(QStringLiteral("favorites")).value_or(true);
        const bool oldLibraries = Settings::navigationGroupExpanded(QStringLiteral("libraries")).value_or(true);
        const auto restore = qScopeGuard([oldFavorites, oldLibraries] {
            Settings::setNavigationGroupExpanded(QStringLiteral("favorites"), oldFavorites);
            Settings::setNavigationGroupExpanded(QStringLiteral("libraries"), oldLibraries);
        });
        Settings::setNavigationGroupExpanded(QStringLiteral("favorites"), true);
        Settings::setNavigationGroupExpanded(QStringLiteral("libraries"), true);
        {
            auto win = window(Locations::libraries());
            auto *tree = win->findChild<NavigationPane *>()->findChild<QTreeWidget *>();
            QVERIFY(tree);
            auto *libraries = heading(tree, Locations::libraries());
            QVERIFY(libraries);
            libraries->setExpanded(false);
            for (int row = 0; row < tree->topLevelItemCount(); ++row)
                if (tree->topLevelItem(row)->text(0) == QStringLiteral("Favorites"))
                    tree->topLevelItem(row)->setExpanded(false);
            win->close();
        }
        auto win = window(Locations::libraries());
        auto *pane = win->findChild<NavigationPane *>();
        auto *tree = pane->findChild<QTreeWidget *>();
        QVERIFY(tree);
        QVERIFY(!heading(tree, Locations::libraries())->isExpanded());
        pane->refresh();
        QVERIFY(!heading(tree, Locations::libraries())->isExpanded());
        QCOMPARE(Settings::navigationGroupExpanded(QStringLiteral("favorites")).value_or(true), false);
        QCOMPARE(Settings::navigationGroupExpanded(QStringLiteral("libraries")).value_or(true), false);
    }

    void localPdfPreviewUsesTheRealPreviewPaneAndClearsForText()
    {
        if (!QFile::exists(QStringLiteral("/usr/bin/pdftoppm")))
            QSKIP("Optional Poppler PDF renderer is not installed");
        QTemporaryDir fixture;
        QVERIFY(fixture.isValid());
        const QString pdf = fixture.filePath(QStringLiteral("Preview reference.pdf"));
        {
            QPdfWriter writer(pdf);
            writer.setResolution(72);
            QPainter painter(&writer);
            QVERIFY(painter.isActive());
            painter.fillRect(QRect(0, 0, writer.width(), writer.height()), Qt::white);
            QFont title = painter.font();
            title.setPointSize(24);
            painter.setFont(title);
            painter.setPen(QColor(QStringLiteral("#195080")));
            painter.drawText(QRect(40, 60, writer.width() - 80, 100),
                             Qt::AlignCenter, QStringLiteral("Lightweight PDF preview"));
            painter.setPen(Qt::black);
            painter.drawText(QRect(40, 180, writer.width() - 80, 100),
                             Qt::AlignCenter, QStringLiteral("A controlled first-page reference."));
        }
        const QString textPath = fixture.filePath(QStringLiteral("Notes.txt"));
        {
            QFile file(textPath);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QVERIFY(file.write("The text preview still works.") > 0);
        }
        auto win = window(QUrl::fromLocalFile(fixture.path()));
        auto *model = win->findChild<DirectoryModel *>();
        QTRY_COMPARE(model->rowCount(), 2);
        auto *view = win->findChild<FileView *>();
        auto *button = win->findChild<QToolButton *>(QStringLiteral("previewPaneButton"));
        auto *pane = win->findChild<PreviewPane *>();
        QVERIFY(view && button && pane);
        QVERIFY(view->selectUrl(QUrl::fromLocalFile(pdf)));
        button->click();
        QTRY_VERIFY(pane->isVisible());
        auto *image = pane->findChild<QLabel *>(QStringLiteral("previewImage"));
        auto *caption = pane->findChild<QLabel *>(QStringLiteral("pdfPreviewCaption"));
        QVERIFY(image && caption);
        QTRY_VERIFY(!image->pixmap().isNull());
        QVERIFY(caption->isVisible());
        QCOMPARE(caption->text(), QStringLiteral("PDF — first page only"));
        win->hide();
        QCoreApplication::processEvents();
        win->show();
        win->activateWindow();
        QTRY_VERIFY(!image->pixmap().isNull());
        QVERIFY(caption->isVisible());
        QVERIFY(button->isChecked());
        const QString output = qEnvironmentVariable("EXPLORER_SNAPSHOTS");
        if (!output.isEmpty()) {
            QVERIFY(QDir().mkpath(output));
            QVERIFY(win->grab().save(output + QStringLiteral("/pdf-preview-window.png")));
        }
        QVERIFY(view->selectUrl(QUrl::fromLocalFile(textPath)));
        auto *text = pane->findChild<QPlainTextEdit *>(QStringLiteral("textPreview"));
        QTRY_COMPARE(text->toPlainText(), QStringLiteral("The text preview still works."));
        QVERIFY(caption->isHidden());
        QVERIFY(image->pixmap().isNull());
        button->click();
        QVERIFY(pane->isHidden());
    }

    void breadcrumbPopupLifetime_data()
    {
        QTest::addColumn<QString>("closeBy");
        QTest::newRow("dismiss") << QStringLiteral("dismiss");
        QTest::newRow("folder-action") << QStringLiteral("action");
        QTest::newRow("external-navigation") << QStringLiteral("navigate");
    }

    void breadcrumbPopupLifetime()
    {
        QFETCH(QString, closeBy);
        auto win = window(m_folder);
        const QPointer<BreadcrumbBar> bar(win->findChild<BreadcrumbBar *>());
        QVERIFY(bar);
        const QPointer<QToolButton> arrow(
            bar->findChildren<QToolButton *>(QStringLiteral("crumbDropdown")).last());
        QVERIFY(arrow->isVisible());
        QSignalSpy requested(bar, &BreadcrumbBar::menuRequested);
        QPointer<QMenu> popup;
        bool sawOpen = false;
        bool closedOnNavigation = false;
        bool foundFolderAction = false;
        QTimer::singleShot(0, win.get(), [&] {
            popup = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            if (!popup) return;
            sawOpen = popup->isVisible() && arrow->property("popupOpen").toBool();
            if (closeBy == QLatin1String("navigate")) {
                navigate(*win, Locations::libraries());
                closedOnNavigation = !popup->isVisible();
                popup->close(); // Bound the test even if navigation fails to close it.
            } else if (closeBy == QLatin1String("action")) {
                for (QAction *action : popup->actions()) {
                    if (action->text() != QLatin1String("Photos")) continue;
                    foundFolderAction = true;
                    QTest::mouseClick(popup, Qt::LeftButton, Qt::NoModifier,
                                      popup->actionGeometry(action).center());
                    break;
                }
                if (popup && popup->isVisible()) popup->close();
            } else {
                QTest::keyClick(popup, Qt::Key_Escape);
            }
        });
        QTimer::singleShot(1000, win.get(), [] {
            if (auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget()))
                menu->close();
        });
        QTest::mousePress(arrow, Qt::LeftButton);
        QTest::mouseRelease(win.get(), Qt::LeftButton, Qt::NoModifier, QPoint(0, 0));
        QCOMPARE(requested.count(), 1);
        QVERIFY(sawOpen);
        QVERIFY(popup.isNull()); // MainWindow's stack QMenu dies after exec().
        if (closeBy == QLatin1String("dismiss")) {
            QVERIFY(bar);
            QVERIFY(!arrow->property("popupOpen").toBool());
            QCOMPARE(win->windowTitle(), QStringLiteral("Explorer Reference"));
        } else {
            QTRY_VERIFY(bar.isNull()); // Deferred deletion of the replaced trail.
            if (closeBy == QLatin1String("action")) {
                QVERIFY(foundFolderAction);
                QCOMPARE(win->windowTitle(), QStringLiteral("Photos"));
            } else {
                QVERIFY(closedOnNavigation);
                QCOMPARE(win->windowTitle(), QStringLiteral("Libraries"));
            }
            auto *replacement = win->findChild<BreadcrumbBar *>();
            QVERIFY(replacement);
            for (auto *button : replacement->findChildren<QToolButton *>())
                QVERIFY(!button->property("popupOpen").toBool());
        }
    }

    void navigationSizeHintMatchesInsetReservation()
    {
        auto win = window(m_folder);
        auto *search = win->findChild<SearchBox *>();
        QVERIFY(search);
        auto *navigation = search->parentWidget();
        auto *buttons = navigation->findChild<Aero::NavButtons *>();
        QVERIFY(buttons && navigation->layout());
        const QMargins margins = navigation->layout()->contentsMargins();
        const int expectedHint = buttons->height() + margins.top() + margins.bottom();
        for (const QSize size : {QSize(820, 560), QSize(500, 400)}) {
            win->resize(size);
            QTRY_COMPARE(navigation->height(), navigation->sizeHint().height());
            const int hint = navigation->sizeHint().height();
            QCOMPARE(hint, expectedHint);
            QCOMPARE(navigation->geometry(), QRect(0, 0, win->width(), hint));
            QCOMPARE(win->contentsMargins().top(), hint + 2);
            QCOMPARE(win->centralWidget()->geometry().top(), hint + 2);
        }
    }

    void selectedDirectoryFooterKeepsMetadataAtNormalAndNarrowWidths()
    {
        auto win = window(m_folder);
        auto *model = win->findChild<DirectoryModel *>();
        auto *view = win->findChild<FileView *>();
        auto *footer = win->findChild<DetailsPane *>();
        QTRY_COMPARE(model->rowCount(), 4);
        QVERIFY(view->selectUrl(QUrl::fromLocalFile(m_folder.toLocalFile() + QStringLiteral("/Project notes"))));
        auto *name = footer->findChild<QLabel *>(QStringLiteral("detailsPrimary"));
        auto *type = footer->findChild<QLabel *>(QStringLiteral("detailsSecondary"));
        auto *modified = footer->findChild<QLabel *>(QStringLiteral("detailsTopMetadata"));
        auto *size = footer->findChild<QLabel *>(QStringLiteral("detailsBottomMetadata"));
        auto *created = footer->findChild<QLabel *>(QStringLiteral("detailsCreatedMetadata"));
        QVERIFY(name && type && modified && size && created);
        for (const int width : {820, 500}) {
            win->resize(width, 560);
            QTRY_COMPARE(footer->width(), win->centralWidget()->width());
            QTRY_COMPARE(name->toolTip(), QStringLiteral("Project notes"));
            QCOMPARE(type->toolTip(), QStringLiteral("File folder"));
            QVERIFY(modified->toolTip().startsWith(QStringLiteral("Date modified:")));
            QCOMPARE(size->toolTip(), QString());
            QCOMPARE(created->toolTip(), QString());
            QVERIFY(created->isHidden());
            QCOMPARE(footer->height(), 54);
            for (auto *label : {name, type, modified}) {
                QCOMPARE(label->accessibleName(), label->toolTip());
                QVERIFY(label->isVisible());
                QVERIFY(footer->rect().contains(label->geometry()));
            }
            if (width == 820) {
                QCOMPARE(name->text(), name->toolTip());
                QCOMPARE(modified->text(), modified->toolTip());
            }
        }
        navigate(*win, Locations::libraries());
        QCOMPARE(modified->toolTip(), QString());
        QCOMPARE(type->toolTip(), QString());
    }

    void fileViewsPreserveSelectionAndRenameBaseName()
    {
        auto win = window(m_folder);
        auto *model = win->findChild<DirectoryModel *>();
        auto *view = win->findChild<FileView *>();
        QTRY_COMPARE(model->rowCount(), 4);
        QVERIFY(view->selectUrl(m_notes));
        for (int i = 0; i <= int(Settings::ViewMode::Content); ++i) {
            view->setViewMode(Settings::ViewMode(i));
            QCOMPARE(view->viewMode(), Settings::ViewMode(i));
            const auto items = model->itemsForIndexes(view->selectedIndexes());
            QCOMPARE(items.size(), 1);
            QCOMPARE(items.first().url(), m_notes);
        }
        view->setViewMode(Settings::ViewMode::SmallIcons);
        auto *icons = qobject_cast<QListView *>(view->currentView());
        QVERIFY(icons);
        QCOMPARE(icons->viewMode(), QListView::ListMode);
        QCOMPARE(icons->flow(), QListView::LeftToRight);
        view->setViewMode(Settings::ViewMode::Details);
        auto *tree = qobject_cast<QTreeView *>(view->currentView());
        QVERIFY(tree);
        QCOMPARE(tree->header()->visualIndex(model->viewColumnFor(DirectoryModel::Name)), 0);
        QCOMPARE(tree->header()->visualIndex(model->viewColumnFor(DirectoryModel::ModifiedTime)), 1);
        QCOMPARE(tree->header()->visualIndex(model->viewColumnFor(DirectoryModel::Type)), 2);
        QCOMPARE(tree->header()->visualIndex(model->viewColumnFor(DirectoryModel::Size)), 3);
        QSignalSpy renamed(view, &FileView::renameRequested);
        view->renameItem(m_notes);
        QTRY_VERIFY(view->findChild<QLineEdit *>());
        auto *edit = view->findChild<QLineEdit *>();
        QCOMPARE(edit->text(), QStringLiteral("Notes.txt"));
        QTRY_COMPARE(edit->selectedText(), QStringLiteral("Notes"));
        QTest::keyClick(edit, Qt::Key_Escape);
        QCOMPARE(renamed.count(), 0);
        QVERIFY(QFile::exists(m_notes.toLocalFile()));
    }

    void folderFirstAndNaturalNameSortingWorkInBothDirections()
    {
        QTemporaryDir fixture;
        QVERIFY(fixture.isValid());
        QVERIFY(QDir().mkdir(fixture.path() + QStringLiteral("/Folder")));
        for (const auto &name : {"item_2.txt", "item_10.txt"}) {
            QFile file(fixture.path() + QLatin1Char('/') + QLatin1String(name));
            QVERIFY(file.open(QIODevice::WriteOnly));
        }
        DirectoryModel model;
        model.setUrl(QUrl::fromLocalFile(fixture.path()));
        QTRY_COMPARE(model.rowCount(), 3);
        model.sort(DirectoryModel::Name, Qt::AscendingOrder);
        QVERIFY(model.itemForIndex(model.model()->index(0, 0)).isDir());
        QCOMPARE(model.itemForIndex(model.model()->index(1, 0)).name(), QStringLiteral("item_2.txt"));
        QCOMPARE(model.itemForIndex(model.model()->index(2, 0)).name(), QStringLiteral("item_10.txt"));
        model.sort(DirectoryModel::Name, Qt::DescendingOrder);
        QVERIFY(model.itemForIndex(model.model()->index(0, 0)).isDir());
        QCOMPARE(model.itemForIndex(model.model()->index(1, 0)).name(), QStringLiteral("item_10.txt"));
        QCOMPARE(model.model()->headerData(model.viewColumnFor(DirectoryModel::ModifiedTime),
                                          Qt::Horizontal).toString(), QStringLiteral("Date modified"));
    }

    void realRenameCopyMoveAndUndoUseKio()
    {
        QTemporaryDir fixture;
        QVERIFY(fixture.isValid());
        const QString source = fixture.path() + QStringLiteral("/Source");
        const QString copy = fixture.path() + QStringLiteral("/Copy");
        const QString move = fixture.path() + QStringLiteral("/Move");
        for (const auto &path : {source, copy, move}) QVERIFY(QDir().mkdir(path));
        const QString oldName = source + QStringLiteral("/Original.txt");
        const QString newName = source + QStringLiteral("/Renamed.txt");
        QFile file(oldName);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("File operation reference\n"), qint64(25));
        file.close();

        auto win = window(QUrl::fromLocalFile(source));
        auto *model = win->findChild<DirectoryModel *>();
        auto *view = win->findChild<FileView *>();
        QTRY_COMPARE(model->rowCount(), 1);
        QVERIFY(view->selectUrl(QUrl::fromLocalFile(oldName)));
        QTimer watchdog;
        watchdog.setInterval(1000);
        connect(&watchdog, &QTimer::timeout, this, [] {
            if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) {
                QStringList text;
                for (auto *label : dialog->findChildren<QLabel *>()) text.append(label->text());
                qWarning() << "Unexpected file-operation dialog" << dialog->windowTitle() << text;
                dialog->reject();
            }
        });
        watchdog.start();
        actionNamed(*win, QStringLiteral("Rename"))->trigger();
        QTRY_VERIFY(view->findChild<QLineEdit *>());
        auto *editor = view->findChild<QLineEdit *>();
        editor->setText(QStringLiteral("Renamed.txt"));
        QTest::keyClick(editor, Qt::Key_Return);
        QTRY_VERIFY(QFile::exists(newName));
        QVERIFY(!QFile::exists(oldName));
        QTRY_VERIFY(model->indexForUrl(QUrl::fromLocalFile(newName)).isValid());
        QVERIFY(view->selectUrl(QUrl::fromLocalFile(newName)));
        actionNamed(*win, QStringLiteral("Copy"))->trigger();
        navigate(*win, QUrl::fromLocalFile(copy));
        QTRY_COMPARE(model->url(), QUrl::fromLocalFile(copy));
        QTRY_COMPARE(model->rowCount(), 0);
        auto *paste = actionNamed(*win, QStringLiteral("Paste"));
        QTRY_VERIFY(paste->isEnabled());
        paste->trigger();
        const QString copied = copy + QStringLiteral("/Renamed.txt");
        QTRY_VERIFY(QFile::exists(copied));
        QVERIFY(QFile::exists(newName));
        QFile contents(copied);
        QVERIFY(contents.open(QIODevice::ReadOnly));
        QCOMPARE(contents.readAll(), QByteArray("File operation reference\n"));
        contents.close();

        QTRY_VERIFY(model->indexForUrl(QUrl::fromLocalFile(copied)).isValid());
        QVERIFY(view->selectUrl(QUrl::fromLocalFile(copied)));
        actionNamed(*win, QStringLiteral("Cut"))->trigger();
        navigate(*win, QUrl::fromLocalFile(move));
        QTRY_COMPARE(model->url(), QUrl::fromLocalFile(move));
        QTRY_VERIFY(paste->isEnabled());
        paste->trigger();
        const QString moved = move + QStringLiteral("/Renamed.txt");
        QTRY_VERIFY(QFile::exists(moved));
        QTRY_VERIFY(!QFile::exists(copied));
        // The label changes (Undo Move/Copy/Rename), so find by its shortcut.
        QAction *undo = nullptr;
        for (QAction *action : win->findChildren<QAction *>())
            if (action->shortcut() == QKeySequence::Undo) undo = action;
        QVERIFY(undo);
        QTRY_VERIFY(undo->isEnabled());
        undo->trigger();
        QTRY_VERIFY(QFile::exists(copied) && !QFile::exists(moved));
        QTRY_VERIFY(undo->isEnabled());
        undo->trigger();
        QTRY_VERIFY(!QFile::exists(copied));
        QTRY_VERIFY(undo->isEnabled());
        undo->trigger();
        QTRY_VERIFY(QFile::exists(oldName) && !QFile::exists(newName));
        QApplication::clipboard()->clear();
    }

    void commandOverflowPreservesCommandsAtNarrowWidths()
    {
        auto win = window(Locations::computer());
        win->resize(700, 500);
        auto *overflow = win->findChild<QToolButton *>(QStringLiteral("commandOverflow"));
        auto *bar = win->findChild<QWidget *>(QStringLiteral("commandBar"));
        auto *view = win->findChild<QToolButton *>(QStringLiteral("changeViewButton"));
        QVERIFY(overflow && bar && view);
        QTRY_VERIFY(overflow->isVisible());
        QVERIFY(!overflow->menu()->actions().isEmpty());
        QVERIFY(view->geometry().right() < bar->width());
        win->resize(1600, 700);
        QTRY_VERIFY(!overflow->isVisible());
    }

    void openWithMenuStaysAPopupWhenCommandsAreRebuilt()
    {
        auto win = window(m_folder);
        auto *model = win->findChild<DirectoryModel *>();
        auto *view = win->findChild<FileView *>();
        QTRY_VERIFY(model->indexForUrl(m_notes).isValid());
        QVERIFY(view->selectUrl(m_notes));
        QToolButton *openWith = nullptr;
        // The command bar follows a selection after a short coalescing delay
        const auto findOpenWith = [&] {
            for (auto *button : win->findChildren<QToolButton *>())
                if (button->text() == QLatin1String("Open with")) openWith = button;
            return openWith != nullptr;
        };
        QTRY_VERIFY(findOpenWith());
        QPointer<QMenu> menu = openWith->menu();
        QVERIFY(menu);
        QCOMPARE(menu->parentWidget(), openWith);
        QCOMPARE(menu->windowType(), Qt::Popup);
        QVERIFY(menu->isWindow());
        QCoreApplication::processEvents();
        QVERIFY(!menu->isVisible());
        const QString snapshots = qEnvironmentVariable("EXPLORER_SNAPSHOTS");
        if (!snapshots.isEmpty()) {
            QDir().mkpath(snapshots);
            QVERIFY(win->grab().save(snapshots + QStringLiteral("/command-bar-file-selected.png")));
        }
        QVERIFY(view->selectUrl(m_readme));
        QTRY_VERIFY(menu.isNull());
        for (auto *button : win->findChildren<QToolButton *>()) {
            if (button->text() == QLatin1String("Open with")) {
                QCOMPARE(button->menu()->windowType(), Qt::Popup);
                QVERIFY(!button->menu()->isVisible());
            }
        }
    }

    void systemDriveNameIsOnlyADisplayAlias()
    {
        KFilePlacesModel places;
        const QUrl root = QUrl::fromLocalFile(QStringLiteral("/"));
        places.addPlace(QStringLiteral("Original system label"), root);
        places.addPlace(QStringLiteral("Other volume"), QUrl::fromLocalFile(QStringLiteral("/mnt/other")));
        places.addPlace(QStringLiteral("Remote root"), QUrl(QStringLiteral("sftp://example.test/")));
        int checked = 0;
        for (int row = 0; row < places.rowCount(); ++row) {
            const QModelIndex index = places.index(row, 0);
            const QString original = places.text(index);
            if (original == QLatin1String("Original system label")) {
                QCOMPARE(DriveLabel::forPlace(&places, index), QStringLiteral("Local Disk (C:)"));
                QCOMPARE(places.text(index), original);
                QCOMPARE(places.url(index), root);
                ++checked;
            } else if (original == QLatin1String("Other volume") || original == QLatin1String("Remote root")) {
                QCOMPARE(DriveLabel::forPlace(&places, index), original);
                ++checked;
            }
        }
        QCOMPARE(checked, 3);
        QVERIFY(DriveLabel::forPlace(nullptr, {}).isEmpty());
    }

    void driveGroupsCollapseIndependentlyAndSurviveRefresh()
    {
        KFilePlacesModel places;
        TestComputerModel model(&places);
        ComputerView view(&model);
        view.resize(700, 420);
        view.show();
        auto *fixed = view.findChild<QToolButton *>(QStringLiteral("fixedDriveHeading"));
        auto *removable = view.findChild<QToolButton *>(QStringLiteral("removableDriveHeading"));
        auto *fixedList = view.findChild<QListView *>(QStringLiteral("fixedDriveList"));
        auto *removableList = view.findChild<QListView *>(QStringLiteral("removableDriveList"));
        QVERIFY(fixed && removable && fixedList && removableList);
        QTRY_VERIFY(fixedList->isVisible() && removableList->isVisible());
        QCOMPARE(fixed->text(), QStringLiteral("Hard Disk Drives (1)"));
        const QString snapshots = qEnvironmentVariable("EXPLORER_SNAPSHOTS");
        if (!snapshots.isEmpty()) {
            QDir().mkpath(snapshots);
            QVERIFY(view.grab().save(snapshots + QStringLiteral("/drive-groups-expanded.png")));
        }
        const int removableTop = removable->mapTo(&view, QPoint()).y();
        fixedList->setCurrentIndex(fixedList->model()->index(0, 0));
        QVERIFY(view.selectedIndex().isValid());
        QTest::mouseClick(fixed, Qt::LeftButton);
        QVERIFY(!fixed->isChecked());
        QVERIFY(!fixedList->isVisible());
        QVERIFY(removableList->isVisible());
        QVERIFY(!view.selectedIndex().isValid());
        QTRY_VERIFY(removable->mapTo(&view, QPoint()).y() < removableTop);
        if (!snapshots.isEmpty())
            QVERIFY(view.grab().save(snapshots + QStringLiteral("/drive-groups-collapsed.png")));
        QTest::keyClick(fixed, Qt::Key_Space);
        QVERIFY(fixedList->isVisible());
        QTest::keyClick(fixed, Qt::Key_Left);
        QVERIFY(!fixedList->isVisible());
        QTest::keyClick(fixed, Qt::Key_Right);
        QVERIFY(fixedList->isVisible());
        QTest::mouseClick(fixed, Qt::LeftButton);
        model.resetDrives();
        QVERIFY(!fixedList->isVisible());
        for (int mode = 0; mode <= int(Settings::ViewMode::Content); ++mode) {
            view.setViewMode(Settings::ViewMode(mode));
            if (Settings::ViewMode(mode) != Settings::ViewMode::Details) {
                QVERIFY(!fixedList->isVisible());
                QVERIFY(removableList->isVisible());
            }
        }
        model.resetDrives(false);
        QVERIFY(!fixed->isVisible());
        model.resetDrives(true);
        QVERIFY(fixed->isVisible());
        QVERIFY(!fixedList->isVisible());
        removable->click();
        view.focusView();
        QVERIFY(fixed->hasFocus());
        fixed->click();
        QVERIFY(fixedList->isVisible());
        QVERIFY(!removableList->isVisible());
    }

    void previewToolbarSharesTheAltPAction()
    {
        auto win = window(m_folder);
        auto *preview = win->findChild<PreviewPane *>();
        auto *button = win->findChild<QToolButton *>(QStringLiteral("previewPaneButton"));
        QVERIFY(preview && button);
        QVERIFY(!preview->isVisible());
        const auto iconKey = button->icon().cacheKey();
        QVERIFY(!button->icon().isNull());
        button->click();
        QCOMPARE(button->icon().cacheKey(), iconKey);
        QVERIFY(preview->isVisible());
        QVERIFY(button->isChecked());
        QVERIFY(button->toolTip().startsWith(QStringLiteral("Hide")));
        button->defaultAction()->trigger();
        QVERIFY(!preview->isVisible());
        QVERIFY(!button->isChecked());
        QVERIFY(button->toolTip().startsWith(QStringLiteral("Show")));
    }

    void textPreviewIsReadOnlyBoundedAndDiscardsStaleResults()
    {
        PreviewPane pane;
        pane.resize(250, 300);
        pane.show();
        auto *text = pane.findChild<QPlainTextEdit *>(QStringLiteral("textPreview"));
        auto *placeholder = pane.findChild<QLabel *>(QStringLiteral("previewPlaceholder"));
        auto *stack = pane.findChild<QStackedWidget *>();
        QVERIFY(text && placeholder && stack);
        QCOMPARE(placeholder->alignment(), Qt::AlignCenter);
        pane.setItems({KFileItem(m_notes)});
        QTRY_COMPARE(text->toPlainText(), QStringLiteral("Windows 7 layout\n"));
        QVERIFY(text->isReadOnly());
        QTest::keyClicks(text, QStringLiteral("cannot edit"));
        QCOMPARE(text->toPlainText(), QStringLiteral("Windows 7 layout\n"));
        pane.setItems({KFileItem(m_notes)});
        pane.setItems({});
        QTest::qWait(200);
        QCOMPARE(stack->currentWidget(), placeholder);
        QCOMPARE(placeholder->text(), QStringLiteral("Select a file to preview."));
        pane.setItems({KFileItem(m_notes), KFileItem(m_readme)});
        QCOMPARE(placeholder->text(), QStringLiteral("2 items selected."));
        QVERIFY(writeFile(QStringLiteral("Large.txt"), QByteArray(300 * 1024, 'x')));
        pane.setItems({KFileItem(QUrl::fromLocalFile(m_folder.toLocalFile() + QStringLiteral("/Large.txt")))});
        // Bounded at 256 KiB of content; very long lines are broken for layout
        QTRY_COMPARE(text->toPlainText().remove(QLatin1Char('\n')).size(), 256 * 1024);
        QVERIFY(!text->toolTip().isEmpty());
        QVERIFY(QFile::remove(m_folder.toLocalFile() + QStringLiteral("/Large.txt")));
    }

    void utf16TextPreviewHonorsTheBom()
    {
        QVERIFY(writeFile(QStringLiteral("Unicode.txt"), QByteArray::fromHex("fffe480065006c006c006f00")));
        PreviewPane pane;
        pane.show();
        pane.setItems({KFileItem(QUrl::fromLocalFile(m_folder.toLocalFile() + QStringLiteral("/Unicode.txt")))});
        auto *text = pane.findChild<QPlainTextEdit *>(QStringLiteral("textPreview"));
        QTRY_COMPARE(text->toPlainText(), QStringLiteral("Hello"));
        QVERIFY(QFile::remove(m_folder.toLocalFile() + QStringLiteral("/Unicode.txt")));
    }

    void detailsPaneClearsMetadataForFolderSummaries()
    {
        DetailsPane pane;
        pane.resize(700, 54);
        pane.show();
        pane.showSelection({KFileItem(m_notes)});
        QCOMPARE(pane.findChild<QLabel *>(QStringLiteral("detailsPrimary"))->toolTip(), QStringLiteral("Notes.txt"));
        QVERIFY(pane.findChild<QLabel *>(QStringLiteral("detailsBottomMetadata"))->toolTip().startsWith(QStringLiteral("Size:")));
        pane.showFolderSummary(4);
        QCOMPARE(pane.findChild<QLabel *>(QStringLiteral("detailsPrimary"))->toolTip(), QStringLiteral("4 items"));
        QCOMPARE(pane.findChild<QLabel *>(QStringLiteral("detailsBottomMetadata"))->toolTip(), QString());
        QCOMPARE(pane.height(), 54);
        QTest::qWait(10);
        QCOMPARE(pane.grab().toImage().pixelColor(690, 30), QColor(QStringLiteral("#F1F5FB")));
    }

    void fileManagerRegistrationIsOptIn()
    {
        if (!QDBusConnection::sessionBus().isConnected()) QSKIP("No session bus");
        FileManagerService service;
        QVERIFY(service.claim());
        QVERIFY(!service.ownsSharedName());
        QVERIFY(QDBusConnection::sessionBus().interface()->isServiceRegistered(
            QStringLiteral("io.github.LinuxExplorer.Win7")));
    }

    void renderSnapshots()
    {
        const QString path = qEnvironmentVariable("EXPLORER_SNAPSHOTS");
        if (path.isEmpty()) QSKIP("Set EXPLORER_SNAPSHOTS to render visual review images");
        QVERIFY(QDir().mkpath(path));
        auto win = window(Locations::libraries());
        win->resize(820, 560);
        QTest::qWait(100);
        QVERIFY(win->grab().save(path + QStringLiteral("/libraries.png")));
        win->resize(686, 440);
        QTest::qWait(50);
        QVERIFY(win->grab().save(path + QStringLiteral("/reference-size-libraries.png")));
        win->resize(820, 560);
        auto *library = win->findChild<LibrariesView *>();
        auto *libraryList = library->findChild<QListView *>(QStringLiteral("libraryList"));
        libraryList->setCurrentIndex(libraryList->model()->index(0, 0));
        QTest::qWait(20);
        QVERIFY(win->grab().save(path + QStringLiteral("/libraries-selected.png")));
        library->setViewMode(Settings::ViewMode::Details);
        QTest::qWait(20);
        QVERIFY(win->grab().save(path + QStringLiteral("/libraries-details.png")));
        library->setViewMode(Settings::ViewMode::Tiles);
        navigate(*win, m_folder);
        auto *model = win->findChild<DirectoryModel *>();
        QTRY_COMPARE(model->rowCount(), 4);
        auto *view = win->findChild<FileView *>();
        QTest::qWait(100);
        QVERIFY(win->grab().save(path + QStringLiteral("/details.png")));
        QVERIFY(view->selectUrl(QUrl::fromLocalFile(m_folder.toLocalFile() + QStringLiteral("/Project notes"))));
        QTest::qWait(30);
        QVERIFY(win->grab().save(path + QStringLiteral("/selected-folder.png")));
        win->resize(500, 400);
        QTest::qWait(30);
        QVERIFY(win->grab().save(path + QStringLiteral("/narrow-selected-folder.png")));
        win->resize(820, 560);
        auto *search = win->findChild<SearchBox *>();
        QVERIFY(search);
        search->setFocus();
        QTRY_VERIFY(search->hasFocus());
        QVERIFY(win->grab().save(path + QStringLiteral("/search-focused-empty.png")));
        QTest::keyClicks(search, QStringLiteral("Notes"));
        QVERIFY(win->grab().save(path + QStringLiteral("/search-focused-input.png")));
        search->clear();
        view->focusView();
        QVERIFY(view->selectUrl(m_notes));
        QAction *content = actionNamed(*win, QStringLiteral("Content"));
        QVERIFY(content);
        content->trigger();
        QTest::qWait(100);
        QVERIFY(win->grab().save(path + QStringLiteral("/content.png")));
        auto *preview = win->findChild<QToolButton *>(QStringLiteral("previewPaneButton"));
        preview->click();
        auto *text = win->findChild<QPlainTextEdit *>(QStringLiteral("textPreview"));
        QTRY_VERIFY(!text->toPlainText().isEmpty());
        QVERIFY(win->grab().save(path + QStringLiteral("/preview.png")));
        ViewModeMenu *menu = win->findChild<ViewModeMenu *>();
        menu->popup(QPoint(100, 100));
        QTest::qWait(100);
        QVERIFY(menu->grab().save(path + QStringLiteral("/view-selector.png")));
        menu->close();
        preview->click();
        navigate(*win, Locations::computer());
        win->resize(700, 500);
        QTest::qWait(200);
        QVERIFY(win->grab().save(path + QStringLiteral("/computer.png")));
        auto *computer = win->findChild<ComputerView *>();
        QVERIFY(computer);
        const auto computerMode = computer->viewMode();
        computer->setViewMode(Settings::ViewMode::Content);
        QTest::qWait(100);
        QVERIFY(win->grab().save(path + QStringLiteral("/computer-content.png")));
        computer->setViewMode(computerMode);
        win->resize(500, 400);
        QTest::qWait(100);
        QVERIFY(win->grab().save(path + QStringLiteral("/narrow-computer.png")));
    }
};

int main(int argc, char **argv)
{
    // Real widgets and KIO, but no writes to the user's folders or preferences.
    QTemporaryDir environment;
    if (!environment.isValid()) return 1;
    const bool nativeTheme = qEnvironmentVariableIsSet("EXPLORER_NATIVE_THEME");
    const QString originalConfig = qEnvironmentVariable("EXPLORER_NATIVE_CONFIG_DIR",
        qEnvironmentVariable("XDG_CONFIG_HOME", QDir::homePath() + QStringLiteral("/.config")));
    const QString referenceFonts = qEnvironmentVariable("EXPLORER_NATIVE_REFERENCE_FONTS");
    const QString root = environment.path();
    for (const auto &name : {"home", "config", "cache", "data", "runtime"})
        QDir().mkpath(root + QLatin1Char('/') + QLatin1String(name));
    QFile::setPermissions(root + QStringLiteral("/runtime"),
                         QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    qputenv("HOME", (root + QStringLiteral("/home")).toLocal8Bit());
    qputenv("XDG_CONFIG_HOME", (root + QStringLiteral("/config")).toLocal8Bit());
    qputenv("XDG_CACHE_HOME", (root + QStringLiteral("/cache")).toLocal8Bit());
    qputenv("XDG_DATA_HOME", (root + QStringLiteral("/data")).toLocal8Bit());
    qputenv("XDG_RUNTIME_DIR", (root + QStringLiteral("/runtime")).toLocal8Bit());
    if (nativeTheme) {
        // Visual review only: copy theme preferences, never user data or auth.
        QDir().mkpath(root + QStringLiteral("/config/Kvantum"));
        QFile::copy(originalConfig + QStringLiteral("/kdeglobals"),
                    root + QStringLiteral("/config/kdeglobals"));
        QFile::copy(originalConfig + QStringLiteral("/Kvantum/kvantum.kvconfig"),
                    root + QStringLiteral("/config/Kvantum/kvantum.kvconfig"));
        const QString fonts = root + QStringLiteral("/data/linux-explorer-win7/reference-fonts");
        QDir().mkpath(fonts);
        if (!referenceFonts.isEmpty())
            for (const char *name : {"segoeui.ttf", "segoeuib.ttf", "segoeuii.ttf", "segoeuiz.ttf",
                                     "micross.ttf", "tahoma.ttf", "lucon.ttf"})
                QFile::copy(referenceFonts + QLatin1Char('/') + QLatin1String(name),
                            fonts + QLatin1Char('/') + QLatin1String(name));
    }
    QApplication app(argc, argv);
    if (nativeTheme) Explorer::configureApplicationAppearance(&app);
    else Aero::configureFonts(&app);
    app.setOrganizationName(QStringLiteral("linux-explorer-win7-tests"));
    app.setApplicationName(QStringLiteral("explorer-ui-tests"));
    if (!nativeTheme || qEnvironmentVariableIsSet("EXPLORER_ICON_THEME"))
        QIcon::setThemeName(qEnvironmentVariable("EXPLORER_ICON_THEME", QStringLiteral("breeze")));
    ExplorerUiTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "ExplorerUiTest.moc"
