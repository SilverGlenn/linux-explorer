#include "NavigationPane.h"
#include "GoogleDriveConnection.h"
#include "Locations.h"
#include "Settings.h"
#include "aero/icons.h"

#include <KFilePlacesModel>

#include <QApplication>
#include <QImage>
#include <QMenu>
#include <QScopeGuard>
#include <QSettings>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>
#include <QTreeWidget>

namespace {

// Only mount-state presentation is under test. No account, browser, helper,
// wallet, or real remote filesystem is involved.
class FixtureConnection final : public GoogleDriveConnection {
public:
    explicit FixtureConnection(bool mounted = false) : m_mounted(mounted) {}
    bool isConnected() const override { return m_mounted; }
    void connectDrive() override { ++accountRequests; }
    void signInAgain() override { ++accountRequests; }
    void disconnectDrive() override { ++accountRequests; }
    void signOut() override { ++accountRequests; }
    void setMounted(bool mounted, bool notify = true)
    {
        m_mounted = mounted;
        if (notify)
            Q_EMIT mountStateChanged(mounted);
    }
    int accountRequests = 0;
private:
    bool m_mounted;
};

QTreeWidget *treeFor(NavigationPane &pane)
{
    return pane.findChild<QTreeWidget *>(QStringLiteral("win7NavPane"));
}

QTreeWidgetItem *group(NavigationPane &pane, const QString &name)
{
    QTreeWidget *tree = treeFor(pane);
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        auto *item = tree->topLevelItem(i);
        if (item->text(0) == name)
            return item;
    }
    return nullptr;
}

QList<QTreeWidgetItem *> driveRows(NavigationPane &pane, const QString &groupName)
{
    QList<QTreeWidgetItem *> rows;
    auto *parent = group(pane, groupName);
    if (!parent)
        return rows;
    for (int i = 0; i < parent->childCount(); ++i) {
        auto *item = parent->child(i);
        if (item->data(0, Qt::UserRole).toUrl().matches(
                Locations::googleDriveMount(), QUrl::StripTrailingSlash))
            rows.append(item);
    }
    return rows;
}

void removeFixtureBookmarks(KFilePlacesModel *places)
{
    for (int row = places->rowCount() - 1; row >= 0; --row) {
        const QModelIndex index = places->index(row, 0);
        if (places->text(index).startsWith(QLatin1String("Drive navigation fixture")))
            places->removePlace(index);
    }
}

} // namespace

class GoogleDriveNavigationTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void init()
    {
        QSettings().remove(QStringLiteral("Navigation/Groups"));
        QSettings().remove(QStringLiteral("Navigation/FavoritesOrder"));
    }

    void mountStateCreatesOneComputerEntryWithoutBookmark()
    {
        FixtureConnection connection;
        NavigationPane pane(&connection, nullptr);
        QVERIFY(treeFor(pane));
        QCOMPARE(driveRows(pane, QStringLiteral("Computer")).size(), 0);

        connection.setMounted(true);
        QTRY_COMPARE(driveRows(pane, QStringLiteral("Computer")).size(), 1);
        auto *drive = driveRows(pane, QStringLiteral("Computer")).first();
        QCOMPARE(drive->text(0), QStringLiteral("Google Drive (G:)"));
        QCOMPARE(drive->data(0, Qt::UserRole).toUrl(), Locations::googleDriveMount());
        QCOMPARE(drive->parent()->text(0), QStringLiteral("Computer"));
        QCOMPARE(drive->data(0, Qt::UserRole + 2).toInt(), -1);
        QVERIFY(!drive->icon(0).isNull());
        QCOMPARE(drive->icon(0).pixmap(16, 16).toImage(),
                 Aero::googleDriveIcon().pixmap(16, 16).toImage());
        QCOMPARE(driveRows(pane, QStringLiteral("Network")).size(), 0);

        // No placeholder means expanding the synthetic drive cannot schedule
        // an eager filesystem scan or register a watcher on the FUSE root.
        QCOMPARE(drive->childCount(), 0);
        drive->setExpanded(true);
        QCOMPARE(drive->childCount(), 0);
        pane.refresh();
        connection.setMounted(true);
        QCOMPARE(driveRows(pane, QStringLiteral("Computer")).size(), 1);
        connection.setMounted(false);
        QCOMPARE(driveRows(pane, QStringLiteral("Computer")).size(), 0);
        connection.setMounted(true);
        QCOMPARE(driveRows(pane, QStringLiteral("Computer")).size(), 1);
        QCOMPARE(connection.accountRequests, 0);
    }

    void refreshObservesMountStateWithoutPlacesChange()
    {
        FixtureConnection connection;
        NavigationPane pane(&connection, nullptr);
        connection.setMounted(true, false);
        pane.refresh();
        QCOMPARE(driveRows(pane, QStringLiteral("Computer")).size(), 1);
        connection.setMounted(false, false);
        pane.refresh();
        QCOMPARE(driveRows(pane, QStringLiteral("Computer")).size(), 0);
        QCOMPARE(connection.accountRequests, 0);
    }

    void optionalNetworkBookmarksDoNotControlOrDuplicateComputerDrive()
    {
        FixtureConnection connection(true);
        NavigationPane pane(&connection, nullptr);
        KFilePlacesModel *places = pane.placesModel();
        const auto cleanup = qScopeGuard([&] { removeFixtureBookmarks(places); });
        const QUrl mount = Locations::googleDriveMount();
        places->addPlace(QStringLiteral("Drive navigation fixture one"), mount,
                         QStringLiteral("folder-network"));
        places->addPlace(QStringLiteral("Drive navigation fixture duplicate"),
                         QUrl::fromLocalFile(mount.toLocalFile() + QLatin1Char('/')),
                         QStringLiteral("folder-network"));
        QTRY_COMPARE(driveRows(pane, QStringLiteral("Network")).size(), 1);
        QCOMPARE(driveRows(pane, QStringLiteral("Computer")).size(), 1);
        const int bookmarkCount = places->rowCount();

        // Both aliases are visible, but root and descendant selection should
        // resolve to the canonical Computer entry at the same depth.
        group(pane, QStringLiteral("Computer"))->setExpanded(true);
        group(pane, QStringLiteral("Network"))->setExpanded(true);
        pane.setCurrentUrl(mount);
        QCOMPARE(treeFor(pane)->currentItem(),
                 driveRows(pane, QStringLiteral("Computer")).first());
        pane.setCurrentUrl(QUrl::fromLocalFile(mount.toLocalFile() + QStringLiteral("/Documents")));
        QCOMPARE(treeFor(pane)->currentItem(),
                 driveRows(pane, QStringLiteral("Computer")).first());
        pane.refresh();
        QCOMPARE(places->rowCount(), bookmarkCount);
        connection.setMounted(false);
        QCOMPARE(driveRows(pane, QStringLiteral("Computer")).size(), 0);
        QCOMPARE(driveRows(pane, QStringLiteral("Network")).size(), 1);
        QCOMPARE(places->rowCount(), bookmarkCount);
        connection.setMounted(true);
        QCOMPARE(driveRows(pane, QStringLiteral("Computer")).size(), 1);

        for (int row = 0; row < places->rowCount(); ++row) {
            const QModelIndex index = places->index(row, 0);
            if (places->text(index).startsWith(QLatin1String("Drive navigation fixture")))
                places->setPlaceHidden(index, true);
        }
        QTRY_COMPARE(driveRows(pane, QStringLiteral("Network")).size(), 0);
        QCOMPARE(driveRows(pane, QStringLiteral("Computer")).size(), 1);
        QCOMPARE(places->rowCount(), bookmarkCount);
        connection.setMounted(false);
        QCOMPARE(driveRows(pane, QStringLiteral("Computer")).size(), 0);
        connection.setMounted(true);
        QCOMPARE(driveRows(pane, QStringLiteral("Computer")).size(), 1);
        QCOMPARE(connection.accountRequests, 0);
    }

    void virtualDriveMenuOnlyOffersSafeNavigation()
    {
        FixtureConnection connection(true);
        NavigationPane pane(&connection, nullptr);
        pane.resize(270, 600);
        pane.show();
        QCoreApplication::processEvents();
        group(pane, QStringLiteral("Computer"))->setExpanded(true);
        QTreeWidget *tree = treeFor(pane);
        auto *drive = driveRows(pane, QStringLiteral("Computer")).first();
        QStringList actions;
        QTimer::singleShot(0, &pane, [&] {
            auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            if (!menu)
                return;
            for (QAction *action : menu->actions())
                actions.append(action->text());
            menu->close();
        });
        tree->customContextMenuRequested(tree->visualItemRect(drive).center());
        QVERIFY(actions.contains(QStringLiteral("Open")));
        QVERIFY(actions.contains(QStringLiteral("Open in new window")));
        for (const QString &action : actions) {
            QVERIFY(!action.contains(QStringLiteral("Rename"), Qt::CaseInsensitive));
            QVERIFY(!action.contains(QStringLiteral("Unmount"), Qt::CaseInsensitive));
            QVERIFY(!action.contains(QStringLiteral("Eject"), Qt::CaseInsensitive));
            QVERIFY(!action.contains(QStringLiteral("Permissions"), Qt::CaseInsensitive));
        }
        QCOMPARE(connection.accountRequests, 0);
    }

    void openingVirtualDriveUsesFixedUrl()
    {
        FixtureConnection connection(true);
        NavigationPane pane(&connection, nullptr);
        pane.resize(270, 600);
        pane.show();
        QCoreApplication::processEvents();
        group(pane, QStringLiteral("Computer"))->setExpanded(true);
        QTreeWidget *tree = treeFor(pane);
        auto *drive = driveRows(pane, QStringLiteral("Computer")).first();
        QSignalSpy activated(&pane, &NavigationPane::urlActivated);
        QSignalSpy newWindow(&pane, &NavigationPane::newWindowRequested);
        QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier,
                          tree->visualItemRect(drive).center());
        QCOMPARE(activated.size(), 1);
        QCOMPARE(activated.first().first().toUrl(), Locations::googleDriveMount());
        QTest::mouseClick(tree->viewport(), Qt::MiddleButton, Qt::NoModifier,
                          tree->visualItemRect(drive).center());
        QCOMPARE(newWindow.size(), 1);
        QCOMPARE(newWindow.first().first().toUrl(), Locations::googleDriveMount());
        QCOMPARE(connection.accountRequests, 0);
    }
};

QTEST_MAIN(GoogleDriveNavigationTest)
#include "GoogleDriveNavigationTest.moc"
