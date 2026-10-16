#include "ApplicationAppearance.h"
#include "AboutDialog.h"
#include "OptionsDialog.h"
#include "PreviewPane.h"
#include "aero/contextmenu.h"
#include "aero/fonts.h"

#include <QApplication>
#include <QDir>
#include <QFontDatabase>
#include <QFontInfo>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QRawFont>
#include <QScopeGuard>
#include <QStyleOptionMenuItem>
#include <QTabWidget>
#include <QTest>
#include <QToolButton>
#include <QTreeView>

namespace {
class MenuInspector : public QMenu {
public:
    static QStyleOptionMenuItem item(QMenu *menu, QAction *action)
    {
        QStyleOptionMenuItem result;
        const auto initialize = &MenuInspector::initStyleOption;
        (menu->*initialize)(&result, action);
        return result;
    }
};

void verifyFont(const QFont &font, const QString &family, qreal points, bool bold = false)
{
    QCOMPARE(font.pointSizeF(), points);
    QCOMPARE(font.bold(), bold);
    QVERIFY(!font.italic());
    if (QFontDatabase::families().contains(family, Qt::CaseInsensitive))
        QCOMPARE(QFontInfo(font).family(), family);
}
} // namespace

class FontTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void initTestCase()
    {
        Explorer::configureApplicationAppearance(qobject_cast<QApplication *>(qApp));
    }

    void shellDefaultsOverrideDesktopAndStyleClassFonts()
    {
        const QFont desktop(QStringLiteral("DejaVu Sans"), 13, QFont::Bold, true);
        qApp->setFont(desktop);
        for (const char *name : {"QMenu", "QLabel", "QToolButton", "QLineEdit", "QTreeView"})
            qApp->setFont(desktop, name);
        Aero::configureFonts(qobject_cast<QApplication *>(qApp));
        QLineEdit address;
        QTreeView files;
        QToolButton command;
        for (QWidget *control : QList<QWidget *>{&address, &files, &command}) {
            control->ensurePolished();
            verifyFont(control->font(), QStringLiteral("Segoe UI"), 9);
        }
    }

    void menusUseShellFontIncludingDynamicSubmenus()
    {
        const auto restore = qScopeGuard([] { Aero::configureFonts(qobject_cast<QApplication *>(qApp)); });
        qApp->setFont(QFont(QStringLiteral("DejaVu Sans"), 13));
        QMenu root;
        root.setFont(QFont(QStringLiteral("DejaVu Sans"), 17, QFont::Bold, true));
        auto *open = root.addAction(QStringLiteral("Open"));
        root.setDefaultAction(open);
        root.addAction(QStringLiteral("Properties"))->setFont(QFont(QStringLiteral("DejaVu Sans"), 22));
        QMenu *child = root.addMenu(QStringLiteral("Send to"));
        QMenu *dynamic = nullptr;
        connect(child, &QMenu::aboutToShow, child, [&] {
            if (dynamic) return;
            dynamic = child->addMenu(QStringLiteral("Added on demand"));
            dynamic->setFont(QFont(QStringLiteral("DejaVu Sans"), 25));
            dynamic->addAction(QStringLiteral("Documents"))->setFont(
                QFont(QStringLiteral("DejaVu Sans"), 25, QFont::Bold, true));
        });
        Aero::styleContextMenu(&root);
        root.popup(QPoint(10, 10));
        const auto close = qScopeGuard([&] { if (dynamic) dynamic->close(); child->close(); root.close(); });
        QTRY_VERIFY(root.isVisible());
        child->popup(QPoint(200, 10));
        QTRY_VERIFY(dynamic);
        dynamic->popup(QPoint(400, 10));
        QTRY_VERIFY(dynamic->isVisible());
        for (QMenu *menu : {&root, child, dynamic}) {
            verifyFont(menu->font(), QStringLiteral("Segoe UI"), 9);
            for (QAction *action : menu->actions()) {
                const auto option = MenuInspector::item(menu, action);
                verifyFont(option.font, QStringLiteral("Segoe UI"), 9,
                           action == menu->defaultAction());
            }
        }
    }

    void classicDialogsUseTheirNativeFamiliesOnEveryTab()
    {
        QWidget parent;
        parent.setFont(Aero::shellFont());
        OptionsDialog options(&parent);
        options.show();
        auto *tabs = options.findChild<QTabWidget *>();
        QVERIFY(tabs);
        for (int page = 0; page < tabs->count(); ++page) {
            tabs->setCurrentIndex(page);
            QCoreApplication::processEvents();
            for (QWidget *control : options.findChildren<QWidget *>())
                if (control->isVisible())
                    verifyFont(control->font(), QStringLiteral("Microsoft Sans Serif"), 8);
        }
        AboutDialog about(&parent);
        about.show();
        QCoreApplication::processEvents();
        for (QWidget *control : about.findChildren<QWidget *>())
            if (control->isVisible()) verifyFont(control->font(), QStringLiteral("Tahoma"), 8);
        verifyFont(parent.font(), QStringLiteral("Segoe UI"), 9);
    }

    void textPreviewUsesNativeCharacterFormat()
    {
        PreviewPane preview;
        auto *text = preview.findChild<QPlainTextEdit *>(QStringLiteral("textPreview"));
        QVERIFY(text);
        verifyFont(text->font(), QStringLiteral("Lucida Console"), 9.75);
    }

    void privateReferenceFacesAreActuallySelected()
    {
        const QDir directory(Aero::referenceFontDirectory());
        if (!directory.exists(QStringLiteral("segoeui.ttf")))
            QSKIP("Private VM reference fonts are optional and are not bundled.");
        const auto check = [&](const QString &file, QFont font) {
            if (!directory.exists(file)) return;
            QRawFont reference(directory.filePath(file), 12);
            QVERIFY(reference.isValid());
            const QRawFont rendered = QRawFont::fromFont(font);
            QCOMPARE(rendered.fontTable("name"), reference.fontTable("name"));
        };
        QFont shell = Aero::shellFont();
        check(QStringLiteral("segoeui.ttf"), shell);
        shell.setBold(true);
        check(QStringLiteral("segoeuib.ttf"), shell);
        shell = Aero::shellFont();
        shell.setItalic(true);
        check(QStringLiteral("segoeuii.ttf"), shell);
        check(QStringLiteral("tahoma.ttf"), Aero::classicDialogFont(Aero::ClassicFont::Tahoma));
        check(QStringLiteral("micross.ttf"), Aero::classicDialogFont());
        check(QStringLiteral("lucon.ttf"), Aero::textPreviewFont());
    }
};

QTEST_MAIN(FontTest)
#include "FontTest.moc"
