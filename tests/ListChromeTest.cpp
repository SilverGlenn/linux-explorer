#include "../src/aero/listview.h"
#include "../src/aero/splitter.h"
#include "../src/ui/NavigationItemDelegate.h"
#include <AeroQt/stylesheet.h>

#include <QApplication>
#include <QDir>
#include <QHeaderView>
#include <QMouseEvent>
#include <QProxyStyle>
#include <QSignalSpy>
#include <QStandardItemModel>
#include <QStyleOptionViewItem>
#include <QTest>
#include <QTreeView>

class VisibleSplitterGripStyle : public QProxyStyle {
public:
    void drawControl(ControlElement element, const QStyleOption *option,
                     QPainter *painter, const QWidget *widget) const override
    {
        if (element == CE_Splitter) {
            painter->fillRect(option->rect, Qt::magenta);
            return;
        }
        QProxyStyle::drawControl(element, option, painter, widget);
    }
};

class ListChromeTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void paneDividerIsThinAndDraggable_data()
    {
        QTest::addColumn<Qt::Orientation>("orientation");
        QTest::newRow("horizontal") << Qt::Horizontal;
        QTest::newRow("vertical") << Qt::Vertical;
    }

    void paneDividerIsThinAndDraggable()
    {
        QFETCH(Qt::Orientation, orientation);
        VisibleSplitterGripStyle gripStyle;
        Aero::PaneSplitter splitter(orientation);
        splitter.setStyle(&gripStyle);
        splitter.setChildrenCollapsible(false);
        splitter.addWidget(new QWidget);
        splitter.addWidget(new QWidget);
        splitter.resize(420, 300);
        splitter.setSizes({160, 260});
        splitter.show();
        QCoreApplication::processEvents();

        QSplitterHandle *handle = splitter.handle(1);
        const bool horizontal = orientation == Qt::Horizontal;
        QCOMPARE(splitter.handleWidth(), 1);
        QCOMPARE(horizontal ? handle->contentsRect().width() : handle->contentsRect().height(), 1);
        QVERIFY((horizontal ? handle->width() : handle->height()) >= 5);

        // A style that deliberately paints a loud grip must never be called.
        // The divider remains a uniform single rule along the whole pane.
        const QImage image = handle->grab().toImage();
        const qreal dpr = image.devicePixelRatio();
        const QColor rule = Aero::Palette::rgb(Aero::Palette::PaneRule);
        for (qreal fraction : {0.25, 0.5, 0.75}) {
            const QPoint sample = horizontal
                ? QPoint(qRound(handle->contentsRect().center().x() * dpr), qRound(handle->height() * fraction * dpr))
                : QPoint(qRound(handle->width() * fraction * dpr), qRound(handle->contentsRect().center().y() * dpr));
            QCOMPARE(image.pixelColor(sample), rule);
        }
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x)
                QVERIFY(image.pixelColor(x, y) != QColor(Qt::magenta));
        }

        // Press in the transparent expanded hit margin, outside the line.
        const QList<int> before = splitter.sizes();
        const QPoint press = horizontal ? QPoint(0, handle->height() / 2) : QPoint(handle->width() / 2, 0);
        QVERIFY(!handle->contentsRect().contains(press));
        const QPoint delta = horizontal ? QPoint(36, 0) : QPoint(0, 36);
        QTest::mousePress(handle, Qt::LeftButton, Qt::NoModifier, press);
        const QPoint target = press + delta;
        QMouseEvent move(QEvent::MouseMove, target, handle->mapToGlobal(target),
                         Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(handle, &move);
        QTest::mouseRelease(handle, Qt::LeftButton, Qt::NoModifier, press);
        const int moved = splitter.sizes().first() - before.first();
        QVERIFY(moved > 0);
        // Native dragging normalizes a press in the overlapping grab margin
        // to the layout gap. The pane should follow the pointer to within that
        // small hit area, rather than requiring a pixel-exact margin offset.
        const int hitWidth = horizontal ? handle->width() : handle->height();
        QVERIFY(qAbs(moved - 36) <= hitWidth);
    }

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
        QCOMPARE(row.right(), tree.viewport()->rect().right() - NavigationItemDelegate::rightGutter);
        QCOMPARE(row.height(), option.rect.height());

        // Simulate a row geometry wider than a temporarily narrowed pane. The
        // background still fills all available pane width, but never escapes it.
        option.rect.setWidth(tree.viewport()->width() + 80);
        const QRect clipped = NavigationItemDelegate::boundedRowRect(option);
        QRect viewportBounds = tree.viewport()->rect();
        viewportBounds.adjust(0, 0, -NavigationItemDelegate::rightGutter, 0);
        QCOMPARE(clipped, option.rect.intersected(viewportBounds));
        QCOMPARE(clipped.right(), tree.viewport()->rect().right() - NavigationItemDelegate::rightGutter);
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

    void headerHoverClearsWhenPointerEntersTrailingBlankSpace()
    {
        QTreeView tree;
        QStandardItemModel model(1, 2);
        model.setHorizontalHeaderLabels({QStringLiteral("Name"), QStringLiteral("Type")});
        tree.setModel(&model);
        Aero::configureListTree(&tree);
        QHeaderView *header = tree.header();
        header->setStretchLastSection(false);
        header->setSectionsMovable(false);
        header->resizeSection(0, 100);
        header->resizeSection(1, 100);
        tree.resize(420, 100);
        tree.show();
        QCoreApplication::processEvents();

        QSignalSpy entered(header, &QHeaderView::sectionEntered);
        const QPoint blank(header->length() + 30, header->viewport()->height() / 2);
        const QPoint lastSection(header->sectionViewportPosition(1) + header->sectionSize(1) / 2,
                                 blank.y());
        QVERIFY(header->viewport()->rect().contains(blank));
        QCOMPARE(header->logicalIndexAt(blank), -1);
        QCOMPARE(header->logicalIndexAt(lastSection), 1);
        const auto movePointer = [&](const QPoint &position) {
            // QPA injection avoids the offscreen cursor conversion at fractional DPR.
            QTest::mouseMove(tree.windowHandle(), header->viewport()->mapTo(&tree, position));
            QCoreApplication::processEvents();
        };
        // Establish a clean state through a real leave before the in-header
        // transition; the offscreen cursor may initially rest over a section.
        movePointer(lastSection);
        movePointer(QPoint(lastSection.x(), header->height() + 20));
        movePointer(blank);
        QVERIFY(header->viewport()->underMouse());
        const QImage normal = header->grab().toImage();
        // Qt's sectionEntered path is reached during pressed movement. Use
        // real input to establish that state, then release before moving blank.
        QTest::mousePress(tree.windowHandle(), Qt::LeftButton, Qt::NoModifier,
                          header->viewport()->mapTo(&tree, QPoint(header->sectionSize(0) / 2, blank.y())));
        movePointer(lastSection);
        QTest::mouseRelease(tree.windowHandle(), Qt::LeftButton, Qt::NoModifier,
                            header->viewport()->mapTo(&tree, lastSection));
        QVERIFY(!entered.isEmpty());
        QCOMPARE(entered.last().first().toInt(), 1);
        header->setSortIndicator(0, Qt::AscendingOrder);
        const QImage hovered = header->grab().toImage();
        QVERIFY(hovered != normal);
        movePointer(blank);
        QVERIFY(header->viewport()->underMouse()); // No leaveEvent can clear the state.
        const QImage returnedToBlank = header->grab().toImage();
        QVERIFY(returnedToBlank == normal);
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
