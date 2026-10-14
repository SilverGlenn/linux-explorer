#include "../src/aero/listview.h"
#include "../src/ui/NavigationItemDelegate.h"
#include <AeroQt/stylesheet.h>

#include <QApplication>
#include <QDir>
#include <QHeaderView>
#include <QStandardItemModel>
#include <QStyleOptionViewItem>
#include <QTest>
#include <QTreeView>

class ListChromeTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        if (qEnvironmentVariableIsSet("LIST_CHROME_USE_AEROQT"))
            Aero::registerStylesheet(qobject_cast<QApplication *>(QCoreApplication::instance()));
    }

    void navigationHighlightFillsRowButCannotOverflowViewport()
    {
        QTreeView tree;
        QStandardItemModel model(1, 1);
        model.setData(model.index(0, 0), QStringLiteral("Documents"));
        tree.setHeaderHidden(true);
        tree.setModel(&model);
        tree.resize(180, 100);
        tree.show();
        QCoreApplication::processEvents();

        QStyleOptionViewItem option;
        option.initFrom(tree.viewport());
        option.widget = tree.viewport();
        option.rect = tree.visualRect(model.index(0, 0));
        const QRect row = NavigationItemDelegate::boundedRowRect(option);
        QVERIFY(!row.isEmpty());
        QCOMPARE(row.left(), option.rect.left());
        QCOMPARE(row.right(), tree.viewport()->rect().right());
        QCOMPARE(row.height(), option.rect.height());

        // Simulate a row geometry wider than a temporarily narrowed pane. The
        // background still fills all available pane width, but never escapes it.
        option.rect.setWidth(tree.viewport()->width() + 80);
        const QRect clipped = NavigationItemDelegate::boundedRowRect(option);
        QCOMPARE(clipped, option.rect.intersected(tree.viewport()->rect()));
        QCOMPARE(clipped.right(), tree.viewport()->rect().right());
    }

    void headerVisualStatesRenderForReferenceReview()
    {
        QTreeView tree;
        QStandardItemModel model(1, 2);
        model.setHorizontalHeaderLabels({QStringLiteral("Name"), QStringLiteral("Date modified")});
        tree.setModel(&model);
        Aero::configureListTree(&tree);
        tree.resize(420, 100);
        tree.show();
        QCoreApplication::processEvents();

        QHeaderView *header = tree.header();
        const auto saveCapture = [header](const QString &state) {
            const QImage image = header->grab().toImage();
            const QString directory = qEnvironmentVariable("LIST_CHROME_CAPTURE_DIR");
            if (!directory.isEmpty()) {
                QDir().mkpath(directory);
                image.save(directory + QLatin1Char('/') + state + ".png");
            }
            return image;
        };

        const QImage normal = saveCapture(QStringLiteral("header-normal"));
        QVERIFY(QMetaObject::invokeMethod(header, "sectionEntered",
                                          Qt::DirectConnection, Q_ARG(int, 0)));
        QCoreApplication::processEvents();
        const QImage hover = saveCapture(QStringLiteral("header-hover"));
        QVERIFY(QMetaObject::invokeMethod(header, "sectionPressed",
                                          Qt::DirectConnection, Q_ARG(int, 0)));
        QCoreApplication::processEvents();
        const QImage pressed = saveCapture(QStringLiteral("header-pressed"));
        QVERIFY(QMetaObject::invokeMethod(header, "sectionClicked",
                                          Qt::DirectConnection, Q_ARG(int, 0)));
        header->setSortIndicator(1, Qt::DescendingOrder);
        QCoreApplication::processEvents();
        const QImage sorted = saveCapture(QStringLiteral("header-sorted"));
        const int initialWidth = header->sectionSize(0);
        header->resizeSection(0, initialWidth + 24);
        QCoreApplication::processEvents();
        const QImage resized = saveCapture(QStringLiteral("header-resized"));

        QVERIFY(!normal.isNull());
        QVERIFY(!hover.isNull());
        QVERIFY(!pressed.isNull());
        QVERIFY(!sorted.isNull());
        QVERIFY(!resized.isNull());
        QVERIFY(normal != hover);
        QVERIFY(hover != pressed);
        QCOMPARE(header->sectionSize(0), initialWidth + 24);
    }

    void sharedHeaderRetainsNativeStatefulPaintingAndResizing()
    {
        QTreeView tree;
        QStandardItemModel model(1, 2);
        model.setHorizontalHeaderLabels({QStringLiteral("Name"), QStringLiteral("Type")});
        tree.setModel(&model);
        Aero::configureListTree(&tree);

        QVERIFY(tree.header()->sectionsClickable());
        QVERIFY(tree.header()->styleSheet().isEmpty());
        QVERIFY(tree.header()->sectionResizeMode(0) != QHeaderView::Fixed);
        QCOMPARE(tree.header()->sortIndicatorSection(), 0);
        QCOMPARE(tree.header()->sortIndicatorOrder(), Qt::AscendingOrder);
    }
};

QTEST_MAIN(ListChromeTest)
#include "ListChromeTest.moc"
