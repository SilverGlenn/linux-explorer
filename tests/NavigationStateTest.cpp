#include "../src/data/Settings.h"
#include "../src/ui/NavigationPane.h"
#include <AeroQt/stylesheet.h>

#include <QApplication>
#include <QDir>
#include <QSettings>
#include <QScrollBar>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QTest>
#include <QImage>
#include <QtMath>

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

} // namespace

class NavigationStateTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        if (qEnvironmentVariableIsSet("NAVIGATION_USE_AEROQT"))
            Aero::registerStylesheet(qobject_cast<QApplication *>(QCoreApplication::instance()));
    }

    void init()
    {
        QSettings().remove(QStringLiteral("Navigation/Groups"));
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
};

QTEST_MAIN(NavigationStateTest)
#include "NavigationStateTest.moc"
