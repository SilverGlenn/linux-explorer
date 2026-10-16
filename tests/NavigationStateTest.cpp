#include "../src/data/Settings.h"
#include "../src/ui/NavigationPane.h"
#include "../src/model/Locations.h"
#include "../src/model/FolderDragData.h"
#include <AeroQt/stylesheet.h>
#include <KBookmark>
#include <KFilePlacesModel>

#include <QApplication>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QMenu>
#include <QMimeData>
#include <QSettings>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QTest>
#include <QTemporaryDir>
#include <QTimer>
#include <QImage>
#include <QtMath>
#include <memory>

namespace {

QTreeWidgetItem *group(QTreeWidget *tree, const QString &name)
{
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = tree->topLevelItem(i);
        if (item->text(0) == name)
            return item;
    }
    return nullptr;
}

QTreeWidget *treeFor(NavigationPane &pane)
{
    return pane.findChild<QTreeWidget *>(QStringLiteral("win7NavPane"));
}

QTreeWidgetItem *favorite(NavigationPane &pane, const QUrl &url)
{
    QTreeWidgetItem *favorites = group(treeFor(pane), QStringLiteral("Favorites"));
    if (favorites) {
        for (int i = 0; i < favorites->childCount(); ++i) {
            QTreeWidgetItem *item = favorites->child(i);
            if (item->data(0, Qt::UserRole).toUrl().matches(url, QUrl::StripTrailingSlash))
                return item;
        }
    }
    return nullptr;
}

QModelIndex placeFor(KFilePlacesModel *places, const QUrl &url)
{
    for (int row = 0; row < places->rowCount(); ++row) {
        const QModelIndex index = places->index(row, 0);
        if (!places->isDevice(index) && places->url(index).matches(url, QUrl::StripTrailingSlash))
            return index;
    }
    return {};
}

QList<QUrl> favoriteUrls(NavigationPane &pane)
{
    QList<QUrl> urls;
    auto *favorites = group(treeFor(pane), QStringLiteral("Favorites"));
    if (favorites)
        for (int i = 0; i < favorites->childCount(); ++i)
            urls.append(favorites->child(i)->data(0, Qt::UserRole).toUrl());
    return urls;
}

} // namespace

class NavigationStateTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        qRegisterMetaType<QDropEvent *>();
        if (qEnvironmentVariableIsSet("NAVIGATION_USE_AEROQT"))
            Aero::registerStylesheet(qobject_cast<QApplication *>(QCoreApplication::instance()));
    }

    void init()
    {
        QSettings().remove(QStringLiteral("Navigation/Groups"));
        QSettings().remove(QStringLiteral("Navigation/FavoritesOrder"));
    }

    void topGroupChoicesSurviveRebuildAndNewPane()
    {
        {
            NavigationPane pane;
            QTRY_VERIFY(treeFor(pane));
            QTreeWidget *tree = treeFor(pane);
            QTRY_VERIFY(group(tree, QStringLiteral("Favorites")));
            QTRY_VERIFY(group(tree, QStringLiteral("Libraries")));
            QTreeWidgetItem *favorites = group(tree, QStringLiteral("Favorites"));
            QTreeWidgetItem *libraries = group(tree, QStringLiteral("Libraries"));
            QTreeWidgetItem *computer = group(tree, QStringLiteral("Computer"));
            QVERIFY(favorites);
            QVERIFY(libraries);
            QVERIFY(computer);
            QVERIFY(favorites->isExpanded());
            QVERIFY(libraries->isExpanded());
            favorites->setExpanded(false);
            libraries->setExpanded(false);
            computer->setExpanded(true);
            // With no devices in the isolated test HOME, the heading has no
            // user-toggleable branch; seed its explicit persisted choice.
            Settings::setNavigationGroupExpanded(QStringLiteral("computer"), true);
            QTRY_VERIFY(!Settings::navigationGroupExpanded(QStringLiteral("favorites")).value());
            QTRY_VERIFY(!Settings::navigationGroupExpanded(QStringLiteral("libraries")).value());
            QTRY_VERIFY(Settings::navigationGroupExpanded(QStringLiteral("computer")).value());

            pane.refresh();
            tree = treeFor(pane);
            QTRY_VERIFY(group(tree, QStringLiteral("Favorites")));
            QVERIFY(!group(tree, QStringLiteral("Favorites"))->isExpanded());
            QVERIFY(!group(tree, QStringLiteral("Libraries"))->isExpanded());
            QVERIFY(group(tree, QStringLiteral("Computer"))->isExpanded());
        }

        QSettings reopened;
        QVERIFY(!reopened.value(QStringLiteral("Navigation/Groups/favorites")).toBool());
        QVERIFY(!reopened.value(QStringLiteral("Navigation/Groups/libraries")).toBool());
        QVERIFY(reopened.value(QStringLiteral("Navigation/Groups/computer")).toBool());

        NavigationPane nextPane;
        QTreeWidget *tree = treeFor(nextPane);
        QTRY_VERIFY(group(tree, QStringLiteral("Favorites")));
        QVERIFY(!group(tree, QStringLiteral("Favorites"))->isExpanded());
        QVERIFY(!group(tree, QStringLiteral("Libraries"))->isExpanded());
        QVERIFY(group(tree, QStringLiteral("Computer"))->isExpanded());

        // Highlight synchronization is read-only: it must not expand groups.
        nextPane.setCurrentUrl(QUrl::fromLocalFile(QDir::homePath()));
        QVERIFY(!group(tree, QStringLiteral("Favorites"))->isExpanded());
    }

    void lastPaneGroupActionWinsOnOtherPaneRefresh()
    {
        NavigationPane paneA;
        NavigationPane paneB;
        QTreeWidget *treeA = treeFor(paneA);
        QTreeWidget *treeB = treeFor(paneB);
        QTRY_VERIFY(group(treeA, QStringLiteral("Libraries")));
        QTRY_VERIFY(group(treeB, QStringLiteral("Libraries")));
        QVERIFY(group(treeA, QStringLiteral("Libraries"))->isExpanded());
        QVERIFY(group(treeB, QStringLiteral("Libraries"))->isExpanded());

        group(treeB, QStringLiteral("Libraries"))->setExpanded(false);
        QVERIFY(!Settings::navigationGroupExpanded(QStringLiteral("libraries")).value());
        paneA.refresh();
        QTRY_VERIFY(group(treeA, QStringLiteral("Libraries")));
        QVERIFY(!group(treeA, QStringLiteral("Libraries"))->isExpanded());
    }

    void realTreeSelectionLeavesVisibleRightGutter()
    {
        NavigationPane pane;
        pane.resize(110, 120);
        pane.show();
        QTreeWidget *tree = treeFor(pane);
        QTRY_VERIFY(group(tree, QStringLiteral("Favorites")));
        QTreeWidgetItem *favorites = group(tree, QStringLiteral("Favorites"));
        QTRY_VERIFY(favorites->childCount() > 0);
        QTreeWidgetItem *row = favorites->child(0);
        favorites->setExpanded(true);
        tree->setMouseTracking(true);
        tree->viewport()->setMouseTracking(true);
        tree->scrollToItem(row);
        tree->setCurrentItem(row);
        QVERIFY(tree->currentItem() == row);
        QVERIFY(tree->selectionModel()->isSelected(tree->indexFromItem(row)));
        tree->setFocus();
        QCoreApplication::processEvents();

        // The places model may asynchronously rebuild the tree while the test
        // pumps events, so reacquire its row before using tree geometry.
        favorites = group(tree, QStringLiteral("Favorites"));
        QTRY_VERIFY(favorites && favorites->childCount() > 0);
        row = favorites->child(0);
        favorites->setExpanded(true);
        tree->scrollToItem(row);
        tree->setCurrentItem(row);
        const QRect rect = tree->visualItemRect(row);
        QVERIFY(rect.isValid());
        QVERIFY(tree->verticalScrollBar()->maximum() > 0);
        const QString captureDir = qEnvironmentVariable("NAVIGATION_CAPTURE_DIR");
        const QImage normal = tree->viewport()->grab().toImage();
        QTest::mouseMove(tree->viewport(), QPoint(1, tree->viewport()->height() - 1));
        QTest::mouseMove(tree->viewport(), rect.center());
        QCoreApplication::processEvents();
        const QImage hover = tree->viewport()->grab().toImage();
        QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, rect.center());
        QCoreApplication::processEvents();
        const QImage active = tree->viewport()->grab().toImage();
        const qreal dpr = active.devicePixelRatio();
        QVERIFY(dpr > 0.0);
        const int logicalWidth = qRound(active.width() / dpr);
        const int selectedLogicalX = qMin(qMax(rect.left(), rect.right() - 5), logicalWidth - 4);
        const int y = rect.center().y();
        QVERIFY(selectedLogicalX >= rect.left());
        QVERIFY(selectedLogicalX < logicalWidth - 3);
        const int selectedX = qFloor((selectedLogicalX + 0.5) * dpr);
        const int gutterX = qFloor((logicalWidth - 1.5) * dpr);
        const int pixelY = qFloor((y + 0.5) * dpr);
        if (!captureDir.isEmpty()) {
            QDir().mkpath(captureDir);
            normal.save(captureDir + QStringLiteral("/navigation-normal.png"));
            hover.save(captureDir + QStringLiteral("/navigation-hover.png"));
            active.save(captureDir + QStringLiteral("/navigation-active-selected.png"));
        }
        QVERIFY(active.pixelColor(selectedX, pixelY) != active.pixelColor(gutterX, pixelY));

        tree->clearFocus();
        QCoreApplication::processEvents();
        const QImage inactive = tree->viewport()->grab().toImage();
        QVERIFY(active != inactive);

        if (!captureDir.isEmpty())
            inactive.save(captureDir + QStringLiteral("/navigation-inactive-selected.png"));
    }

    void defaultExpansionAppliesWithoutStoredChoice()
    {
        NavigationPane pane;
        QTreeWidget *tree = treeFor(pane);
        QTRY_VERIFY(group(tree, QStringLiteral("Favorites")));
        QVERIFY(group(tree, QStringLiteral("Favorites"))->isExpanded());
        QVERIFY(group(tree, QStringLiteral("Libraries"))->isExpanded());
        QVERIFY(!group(tree, QStringLiteral("Computer"))->isExpanded());
    }

    void arbitraryFolderFavoritePersistsAndDoesNotDuplicate()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("Projects/Client assets"));
        QVERIFY(QDir().mkpath(path));
        const QUrl url = QUrl::fromLocalFile(path);
        NavigationPane pane;
        KFilePlacesModel *places = pane.placesModel();
        const auto cleanup = qScopeGuard([&] { places->removePlace(placeFor(places, url)); });
        QVERIFY(!favorite(pane, url));
        group(treeFor(pane), QStringLiteral("Favorites"))->setExpanded(false);
        pane.addToFavorites(url);
        QTRY_VERIFY(favorite(pane, url));
        QCOMPARE(favorite(pane, url)->text(0), QStringLiteral("Client assets"));
        QVERIFY(group(treeFor(pane), QStringLiteral("Favorites"))->isExpanded());

        const int rows = places->rowCount();
        pane.addToFavorites(QUrl::fromLocalFile(path + QLatin1Char('/')));
        QCOMPARE(places->rowCount(), rows);
        places->setPlaceHidden(placeFor(places, url), true);
        QTRY_VERIFY(!favorite(pane, url));
        pane.addToFavorites(url);
        QTRY_VERIFY(favorite(pane, url));
        QCOMPARE(places->rowCount(), rows);

        NavigationPane reopened;
        QTRY_VERIFY(favorite(reopened, url));
        QCOMPARE(favorite(reopened, url)->text(0), QStringLiteral("Client assets"));
    }

    void specialFoldersCanBeExplicitFavorites_data()
    {
        QTest::addColumn<QUrl>("url");
        QTest::newRow("home") << QUrl::fromLocalFile(QDir::homePath());
        QTest::newRow("library") << QUrl::fromLocalFile(
            QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation));
        QTest::newRow("root") << QUrl::fromLocalFile(QStringLiteral("/"));
        QTest::newRow("network-folder") << QUrl(QStringLiteral("smb://server/share/Team folders"));
    }

    void specialFoldersCanBeExplicitFavorites()
    {
        QFETCH(QUrl, url);
        if (url.isLocalFile())
            QVERIFY(QDir().mkpath(url.toLocalFile()));
        NavigationPane pane;
        KFilePlacesModel *places = pane.placesModel();
        const QModelIndex original = placeFor(places, url);
        const bool existed = original.isValid();
        const QString oldMetadata = places->bookmarkForIndex(original)
            .metaDataItem(QStringLiteral("explorerWin7Favorite"));
        const auto cleanup = qScopeGuard([&] {
            const QModelIndex index = placeFor(places, url);
            if (!existed) {
                places->removePlace(index);
            } else {
                KBookmark bookmark = places->bookmarkForIndex(index);
                bookmark.setMetaDataItem(QStringLiteral("explorerWin7Favorite"), oldMetadata);
                places->refresh();
            }
        });
        NavigationPane otherWindow;
        pane.addToFavorites(url);
        QTRY_VERIFY(favorite(pane, url));
        QTRY_VERIFY(favorite(otherWindow, url));
        NavigationPane reopened;
        QTRY_VERIFY(favorite(reopened, url));
        int matchingPlaces = 0;
        for (int row = 0; row < places->rowCount(); ++row) {
            const QModelIndex index = places->index(row, 0);
            if (!places->isDevice(index) && places->url(index).matches(url, QUrl::StripTrailingSlash))
                ++matchingPlaces;
        }
        QCOMPARE(matchingPlaces, 1);
    }

    void virtualLocationsCannotBeAddedAsFolders()
    {
        NavigationPane pane;
        const int rows = pane.placesModel()->rowCount();
        const QList<QUrl> urls = {{}, Locations::computer(), Locations::libraries(),
            Locations::search(QUrl::fromLocalFile(QDir::homePath()), QStringLiteral("test")),
            QUrl(QStringLiteral("trash:/")), QUrl(QStringLiteral("remote:/")),
            QUrl(QStringLiteral("recentlyused:/locations"))};
        for (const QUrl &url : urls) {
            QVERIFY(!NavigationPane::canAddToFavorites(url));
            pane.addToFavorites(url);
        }
        QCOMPARE(pane.placesModel()->rowCount(), rows);
    }

    void bookmarkedNetworkFolderCanBeAddedFromSidebar()
    {
        NavigationPane pane;
        pane.resize(250, 600);
        pane.show();
        const QUrl url(QStringLiteral("smb://server/share/Sidebar folder"));
        KFilePlacesModel *places = pane.placesModel();
        const auto cleanup = qScopeGuard([&] { places->removePlace(placeFor(places, url)); });
        places->addPlace(QStringLiteral("Sidebar folder"), url, QStringLiteral("folder-network"));
        QTreeWidget *tree = treeFor(pane);
        QTRY_VERIFY(group(tree, QStringLiteral("Network")));
        QTreeWidgetItem *network = group(tree, QStringLiteral("Network"));
        network->setExpanded(true);
        QTreeWidgetItem *row = nullptr;
        for (int i = 0; i < network->childCount(); ++i)
            if (network->child(i)->data(0, Qt::UserRole).toUrl() == url)
                row = network->child(i);
        QVERIFY(row);
        bool added = false;
        QTimer::singleShot(0, &pane, [&] {
            auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            if (!menu) return;
            for (QAction *action : menu->actions()) {
                if (action->text() != QLatin1String("Add to Favorites")) continue;
                action->trigger();
                added = true;
                break;
            }
            menu->close();
        });
        tree->customContextMenuRequested(tree->visualItemRect(row).center());
        QVERIFY(added);
        QTRY_VERIFY(favorite(pane, url));
    }

    void dragFavorites_data()
    {
        QTest::addColumn<QString>("destination");
        QTest::newRow("custom-to-top") << QStringLiteral("top");
        QTest::newRow("default-to-end") << QStringLiteral("end");
        QTest::newRow("above-default") << QStringLiteral("above");
        QTest::newRow("favorites-heading") << QStringLiteral("heading");
        QTest::newRow("outside-favorites") << QStringLiteral("outside");
        QTest::newRow("same-position") << QStringLiteral("same");
        QTest::newRow("expanded-folder") << QStringLiteral("expanded");
    }

    void dragFavorites()
    {
        QFETCH(QString, destination);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QUrl first = QUrl::fromLocalFile(directory.filePath(QStringLiteral("First")));
        const QUrl second = QUrl::fromLocalFile(directory.filePath(QStringLiteral("Second")));
        QVERIFY(QDir().mkpath(first.toLocalFile() + QStringLiteral("/Nested")));
        QVERIFY(QDir().mkpath(second.toLocalFile()));
        NavigationPane pane;
        pane.resize(270, 700);
        pane.show();
        auto *tree = treeFor(pane);
        KFilePlacesModel *places = pane.placesModel();
        const auto cleanup = qScopeGuard([&] {
            places->removePlace(placeFor(places, first));
            places->removePlace(placeFor(places, second));
        });
        pane.addToFavorites(first);
        pane.addToFavorites(second);
        NavigationPane otherWindow;
        QTRY_COMPARE(favoriteUrls(otherWindow), favoriteUrls(pane));
        const QList<QUrl> before = favoriteUrls(pane);
        QVERIFY(before.size() >= 4);
        QVERIFY(tree->dragEnabled());

        QUrl source = second;
        QTreeWidgetItem *target = favorite(pane, before.first());
        QPoint position = tree->visualItemRect(target).topLeft() + QPoint(40, 2);
        int insertionIndex = 0;
        bool accepted = true;
        if (destination == QLatin1String("end")) {
            source = before.first();
            position = tree->visualItemRect(favorite(pane, second)).bottomLeft() + QPoint(40, -1);
            insertionIndex = before.size();
        } else if (destination == QLatin1String("above")) {
            source = before.at(1);
        } else if (destination == QLatin1String("heading")) {
            position = tree->visualItemRect(group(tree, QStringLiteral("Favorites"))).center();
            insertionIndex = before.size();
        } else if (destination == QLatin1String("outside")) {
            position = tree->visualItemRect(group(tree, QStringLiteral("Libraries"))).center();
            accepted = false;
        } else if (destination == QLatin1String("same")) {
            position = tree->visualItemRect(favorite(pane, source)).bottomLeft() + QPoint(40, -1);
            insertionIndex = before.indexOf(source) + 1;
        } else if (destination == QLatin1String("expanded")) {
            source = before.first();
            favorite(pane, first)->setExpanded(true);
            QTRY_VERIFY(favorite(pane, first)->childCount() > 0
                && favorite(pane, first)->child(0)->data(0, Qt::UserRole).toUrl().isValid());
            position = tree->visualItemRect(favorite(pane, first)->child(0)).bottomLeft() + QPoint(40, -1);
            insertionIndex = before.indexOf(first) + 1;
        }
        QTreeWidgetItem *sourceItem = favorite(pane, source);
        tree->setCurrentItem(sourceItem);
        const QPersistentModelIndex sourceIndex(tree->indexFromItem(sourceItem));
        std::unique_ptr<QMimeData> mime(tree->model()->mimeData({sourceIndex}));
        QVERIFY(mime);
        QVERIFY(mime->hasFormat(QStringLiteral("application/x-explorer-win7-favorite")));
        QVERIFY(!mime->hasUrls());
        QSignalSpy filesystemDrop(&pane, &NavigationPane::dropped);
        QSignalSpy navigation(&pane, &NavigationPane::urlActivated);
        QDragEnterEvent enter(position, Qt::MoveAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(tree->viewport(), &enter);
        QVERIFY(enter.isAccepted());
        QDragMoveEvent move(position, Qt::MoveAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(tree->viewport(), &move);
        QCOMPARE(move.isAccepted(), accepted);
        QDropEvent drop(QPointF(position), Qt::MoveAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(tree->viewport(), &drop);
        QCOMPARE(drop.isAccepted(), accepted);
        QCOMPARE(filesystemDrop.count(), 0);
        QCOMPARE(navigation.count(), 0);
        QList<QUrl> expected = before;
        if (accepted) {
            const int old = expected.indexOf(source);
            expected.removeAt(old);
            expected.insert(insertionIndex - (old < insertionIndex ? 1 : 0), source);
        }
        QCOMPARE(favoriteUrls(pane), expected);
        QVERIFY(sourceIndex.isValid());
        QCOMPARE(favorite(pane, source), sourceItem);
        QCOMPARE(tree->currentItem(), sourceItem);
        if (destination == QLatin1String("expanded"))
            QVERIFY(favorite(pane, first)->isExpanded());
        QTRY_COMPARE(favoriteUrls(otherWindow), expected);
        pane.refresh();
        QCOMPARE(favoriteUrls(pane), expected);
        NavigationPane reopened;
        QTRY_COMPARE(favoriteUrls(reopened), expected);
        QVERIFY(QFileInfo::exists(first.toLocalFile() + QStringLiteral("/Nested")));
        QVERIFY(QFileInfo::exists(second.toLocalFile()));
    }

    void externalFileDropStillUsesFileOperations()
    {
        NavigationPane pane;
        pane.resize(270, 700);
        pane.show();
        QTreeWidget *tree = treeFor(pane);
        auto *favorites = group(tree, QStringLiteral("Favorites"));
        QVERIFY(favorites && favorites->childCount() > 0);
        const QList<QUrl> before = favoriteUrls(pane);
        const QUrl target = before.first();
        const QPoint pos = tree->visualItemRect(favorites->child(0)).center();
        QMimeData mime;
        mime.setUrls({QUrl::fromLocalFile(QStringLiteral("/tmp/regular-file"))});
        QSignalSpy filesystemDrop(&pane, &NavigationPane::dropped);
        QDragEnterEvent enter(pos, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(tree->viewport(), &enter);
        QDropEvent drop(QPointF(pos), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(tree->viewport(), &drop);
        QVERIFY(drop.isAccepted());
        QCOMPARE(filesystemDrop.count(), 1);
        QCOMPARE(filesystemDrop.first().at(1).toUrl(), target);
        QCOMPARE(favoriteUrls(pane), before);
        QVERIFY(Settings::favoritesOrder().isEmpty());
    }

    void draggingNearTheEdgeScrollsLongFavorites()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        NavigationPane pane;
        pane.resize(260, 220);
        pane.show();
        auto *places = pane.placesModel();
        QList<QUrl> added;
        const auto cleanup = qScopeGuard([&] {
            for (const QUrl &url : added)
                places->removePlace(placeFor(places, url));
        });
        for (int i = 0; i < 20; ++i) {
            const QString path = directory.filePath(QString::number(i));
            QVERIFY(QDir().mkpath(path));
            added.append(QUrl::fromLocalFile(path));
            pane.addToFavorites(added.last());
        }
        QTreeWidget *tree = treeFor(pane);
        QCoreApplication::processEvents();
        tree->verticalScrollBar()->setValue(0);
        auto *source = group(tree, QStringLiteral("Favorites"))->child(0);
        std::unique_ptr<QMimeData> mime(tree->model()->mimeData({tree->indexFromItem(source)}));
        QVERIFY(mime);
        const QPoint pos(50, tree->viewport()->height() - 4);
        QDragEnterEvent enter(pos, Qt::MoveAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(tree->viewport(), &enter);
        QDragMoveEvent move(pos, Qt::MoveAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(tree->viewport(), &move);
        QTRY_VERIFY(tree->verticalScrollBar()->value() > 0);
        QDragLeaveEvent leave;
        QApplication::sendEvent(tree->viewport(), &leave);
        const int stopped = tree->verticalScrollBar()->value();
        QTest::qWait(150);
        QCOMPARE(tree->verticalScrollBar()->value(), stopped);
    }

    void favoriteInsertFeedbackMatchesTheReference()
    {
        NavigationPane pane;
        pane.resize(270, 600);
        pane.show();
        QTreeWidget *tree = treeFor(pane);
        QCoreApplication::processEvents();
        auto *favorites = group(tree, QStringLiteral("Favorites"));
        QVERIFY(favorites && favorites->childCount() >= 2);
        const QPoint pos = tree->visualItemRect(favorites->child(0)).topLeft() + QPoint(40, 1);
        std::unique_ptr<QMimeData> mime(tree->model()->mimeData(
            {tree->indexFromItem(favorites->child(favorites->childCount() - 1))}));
        const QImage normal = tree->viewport()->grab().toImage();
        QDragEnterEvent enter(pos, Qt::MoveAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(tree->viewport(), &enter);
        QDragMoveEvent move(pos, Qt::MoveAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(tree->viewport(), &move);
        QVERIFY(move.isAccepted());
        auto *hint = tree->findChild<QWidget *>(QStringLiteral("favoriteDragHint"));
        QVERIFY(hint && hint->isVisible());
        QCOMPARE(hint->accessibleName(), QStringLiteral("Move here"));
        const QImage during = tree->viewport()->grab().toImage();
        const qreal dpr = during.devicePixelRatio();
        const int y = qRound(tree->visualItemRect(favorites->child(0)).top() * dpr);
        bool blackMarker = false;
        for (int dy = -1; dy <= 1; ++dy) {
            const QColor color = during.pixelColor(qRound(100 * dpr), y + dy);
            blackMarker |= color.red() < 180 && qAbs(color.red() - color.green()) < 5
                && qAbs(color.green() - color.blue()) < 5;
        }
        QVERIFY(blackMarker);
        QCOMPARE(during.pixelColor(qRound(12 * dpr), y), normal.pixelColor(qRound(12 * dpr), y));
        const QString captures = qEnvironmentVariable("NAVIGATION_CAPTURE_DIR");
        if (!captures.isEmpty()) {
            QDir().mkpath(captures);
            during.save(captures + QStringLiteral("/favorites-insert-marker.png"));
            hint->grab().save(captures + QStringLiteral("/favorites-move-hint.png"));
        }
        // A folder's center is not an insertion target for a saved shortcut.
        QDragMoveEvent center(tree->visualItemRect(favorites->child(0)).center(), Qt::MoveAction,
                               mime.get(), Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(tree->viewport(), &center);
        QVERIFY(!center.isAccepted());
        QVERIFY(!hint->isVisible());
        QDragLeaveEvent leave;
        QApplication::sendEvent(tree->viewport(), &leave);
    }

    void remoteFolderDragUsesListingMetadata()
    {
        NavigationPane pane;
        pane.resize(270, 600);
        pane.show();
        auto *places = pane.placesModel();
        const QUrl remote(QStringLiteral("smb://server/share/Remote folder"));
        const auto cleanup = qScopeGuard([&] { places->removePlace(placeFor(places, remote)); });
        QMimeData mime;
        mime.setUrls({remote});
        FolderDragData::setFolders(&mime, {remote});
        QTreeWidget *tree = treeFor(pane);
        const QPoint pos = tree->visualItemRect(group(tree, QStringLiteral("Favorites"))).center();
        QSignalSpy filesystemDrop(&pane, &NavigationPane::dropped);
        QDragEnterEvent enter(pos, Qt::LinkAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(tree->viewport(), &enter);
        QDropEvent drop(QPointF(pos), Qt::LinkAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(tree->viewport(), &drop);
        QVERIFY(drop.isAccepted());
        QCOMPARE(drop.dropAction(), Qt::LinkAction);
        QCOMPARE(filesystemDrop.count(), 0);
        QTRY_VERIFY(favorite(pane, remote));
    }
};

QTEST_MAIN(NavigationStateTest)
#include "NavigationStateTest.moc"
