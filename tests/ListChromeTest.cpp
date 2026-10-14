#include "../src/aero/listview.h"
#include "../src/ui/NavigationItemDelegate.h"

#include <QApplication>
#include <QHeaderView>
#include <QStandardItemModel>
#include <QStyleOptionViewItem>
#include <QTest>
#include <QTreeView>

class ListChromeTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void navigationHighlightHugsContent()
    {
        QTreeView tree;
        QStandardItemModel model(1, 1);
        model.setData(model.index(0, 0), QStringLiteral("Documents"));
        tree.setModel(&model);
        tree.resize(320, 100);
        tree.show();
        QCoreApplication::processEvents();

        QStyleOptionViewItem option;
        option.initFrom(&tree);
        option.rect = tree.visualRect(model.index(0, 0)); // includes live tree indentation
        option.features |= QStyleOptionViewItem::HasDisplay
                         | QStyleOptionViewItem::HasDecoration;
        option.decorationSize = QSize(16, 16);
        option.text = QStringLiteral("Documents");
        option.font = tree.font();
        const QRect content = NavigationItemDelegate::contentRect(
            option, model.index(0, 0));

        QVERIFY(content.left() >= option.rect.left());
        QVERIFY(content.right() < option.rect.right());
        QCOMPARE(content.top(), option.rect.top());
        QCOMPARE(content.height(), option.rect.height());
    }

    void sharedHeaderRetainsNativeStatefulPaintingAndResizing()
    {
        QTreeView tree;
        QStandardItemModel model(1, 2);
        model.setHorizontalHeaderLabels({QStringLiteral("Name"), QStringLiteral("Type")});
        tree.setModel(&model);
        Aero::configureListTree(&tree);

        QVERIFY(tree.header()->styleSheet().isEmpty());
        QVERIFY(tree.header()->sectionResizeMode(0) != QHeaderView::Fixed);
        QCOMPARE(tree.header()->sortIndicatorSection(), 0);
        QCOMPARE(tree.header()->sortIndicatorOrder(), Qt::AscendingOrder);
    }
};

QTEST_MAIN(ListChromeTest)
#include "ListChromeTest.moc"
