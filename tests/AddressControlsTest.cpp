#include "BreadcrumbBar.h"
#include "SearchBox.h"
#include <AeroQt/stylesheet.h>

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QHBoxLayout>
#include <QLabel>
#include <QSignalBlocker>
#include <QSignalSpy>
#include <QTest>
#include <QToolButton>

class AddressControlsTest : public QObject {
    Q_OBJECT
private:
    static QList<BreadcrumbBar::Segment> trail()
    {
        return {{QStringLiteral("Computer"), QUrl(QStringLiteral("computer:/"))},
                {QStringLiteral("My Documents"), QUrl::fromLocalFile(QStringLiteral("/documents"))},
                {QStringLiteral("Explorer Reference"), QUrl::fromLocalFile(QStringLiteral("/documents/reference"))}};
    }
    static void expose(QWidget &widget)
    {
        widget.show();
        widget.activateWindow();
        QTest::qWait(20);
    }

private Q_SLOTS:
    void breadcrumbActivationAndKeyboard()
    {
        BreadcrumbBar bar;
        bar.resize(500, 22);
        bar.setTrail(QIcon(), trail());
        expose(bar);
        QSignalSpy navigation(&bar, &BreadcrumbBar::urlActivated);
        QSignalSpy menus(&bar, &BreadcrumbBar::menuRequested);
        const auto names = bar.findChildren<QToolButton *>(QStringLiteral("crumbName"));
        const auto arrows = bar.findChildren<QToolButton *>(QStringLiteral("crumbDropdown"));
        QCOMPARE(names.size(), 3);
        QCOMPARE(arrows.size(), 3);
        for (int i = 0; i < names.size(); ++i) {
            QTest::mouseClick(names.at(i), Qt::LeftButton);
            QCOMPARE(navigation.last().first().toUrl(), trail().at(i).url);
            const int navigations = navigation.size();
            QTest::mouseClick(arrows.at(i), Qt::LeftButton);
            QCOMPARE(navigation.size(), navigations);
            QCOMPARE(menus.last().first().toUrl(), trail().at(i).url);
            QVERIFY(!names.at(i)->font().underline());
        }
        names.last()->setFocus(Qt::TabFocusReason);
        QVERIFY(names.last()->hasFocus());
        QTest::keyClick(names.last(), Qt::Key_Space);
        QCOMPARE(navigation.size(), 4);
        QTest::keyClick(names.last(), Qt::Key_Tab);
        QVERIFY(arrows.last()->hasFocus());
        QTest::keyClick(arrows.last(), Qt::Key_Space);
        QCOMPARE(menus.size(), 4);
        auto *leading = bar.findChild<QToolButton *>(QStringLiteral("breadcrumbLocations"));
        QTest::mouseClick(leading, Qt::LeftButton);
        QVERIFY(!menus.last().first().toUrl().isValid());
        bar.setEnabled(false);
        QTest::mouseClick(names.last(), Qt::LeftButton);
        QTest::mouseClick(arrows.last(), Qt::LeftButton);
        QCOMPARE(navigation.size(), 4);
        QCOMPARE(menus.size(), 5);
    }

    void breadcrumbPopupAndSharedChrome()
    {
        BreadcrumbBar bar;
        bar.resize(500, 22);
        bar.setTrail(QIcon(), trail());
        expose(bar);
        const auto names = bar.findChildren<QToolButton *>(QStringLiteral("crumbName"));
        const auto arrows = bar.findChildren<QToolButton *>(QStringLiteral("crumbDropdown"));
        auto *leading = bar.findChild<QToolButton *>(QStringLiteral("breadcrumbLocations"));
        if (bar.focusWidget()) bar.focusWidget()->clearFocus();
        QTest::mouseMove(&bar, QPoint(490, 10));
        const QImage normal = names.last()->parentWidget()->grab().toImage();
        QTest::mouseMove(names.last(), names.last()->rect().center());
        const QImage nameHover = names.last()->parentWidget()->grab().toImage();
        QVERIFY(nameHover.pixelColor(nameHover.width() - 1, 5)
                != normal.pixelColor(normal.width() - 1, 5));
        QTest::mousePress(names.last(), Qt::LeftButton);
        const QImage namePressed = names.last()->parentWidget()->grab().toImage();
        QVERIFY(namePressed != nameHover);
        QSignalSpy cancelledNavigation(&bar, &BreadcrumbBar::urlActivated);
        QTest::mouseRelease(names.last(), Qt::LeftButton, Qt::NoModifier, QPoint(-4, -4));
        QCOMPARE(cancelledNavigation.size(), 0);
        QTest::mouseMove(arrows.last(), arrows.last()->rect().center());
        const QImage arrowHover = names.last()->parentWidget()->grab().toImage();
        QVERIFY(arrowHover.pixelColor(0, 5) != normal.pixelColor(0, 5));
        QTest::mouseMove(&bar, QPoint(490, 10));
        bar.setPopupOpen(trail().last().url, true);
        QVERIFY(arrows.last()->property("popupOpen").toBool());
        const QImage popup = names.last()->parentWidget()->grab().toImage();
        QVERIFY(popup != normal);
        // The name's outer edge changes too, not just the independent arrow.
        QVERIFY(popup.pixelColor(0, 5) != normal.pixelColor(0, 5));
        bar.setPopupOpen(trail().first().url, true);
        QVERIFY(!arrows.last()->property("popupOpen").toBool());
        QVERIFY(arrows.first()->property("popupOpen").toBool());
        bar.setPopupOpen(trail().first().url, false);
        QVERIFY(!arrows.first()->property("popupOpen").toBool());
        bar.setPopupOpen(QUrl(), true);
        QVERIFY(leading->property("popupOpen").toBool());
        bar.setTrail(QIcon(), trail());
        QVERIFY(!leading->property("popupOpen").toBool());
    }

    void breadcrumbOverflowAndElision()
    {
        BreadcrumbBar bar;
        bar.setTrail(QIcon(), trail());
        bar.resize(150, 22);
        expose(bar);
        QCOMPARE(bar.hiddenAncestorCount(), 2);
        const auto names = bar.findChildren<QToolButton *>(QStringLiteral("crumbName"));
        const auto arrows = bar.findChildren<QToolButton *>(QStringLiteral("crumbDropdown"));
        QVERIFY(!names.first()->isVisible());
        QVERIFY(names.last()->isVisible());
        QVERIFY(arrows.last()->isVisible());
        QVERIFY(names.last()->text() != trail().last().label);
        QCOMPARE(names.last()->accessibleName(), trail().last().label);
        QVERIFY(arrows.last()->parentWidget()->geometry().right() < bar.width());
        auto *leading = bar.findChild<QToolButton *>(QStringLiteral("breadcrumbLocations"));
        QVERIFY(leading->property("overflow").toBool());
        QCOMPARE(leading->text(), QStringLiteral("\u00ab"));
        bar.resize(500, 22);
        QCoreApplication::processEvents();
        QCOMPARE(bar.hiddenAncestorCount(), 0);
        QCOMPARE(names.last()->text(), trail().last().label);
        QVERIFY(!leading->property("overflow").toBool());
    }

    void searchFontAndEditing()
    {
        QWidget window;
        QFont inherited = window.font();
        inherited.setPointSize(14);
        window.setFont(inherited);
        auto *layout = new QHBoxLayout(&window);
        auto *search = new SearchBox;
        auto *other = new QLineEdit;
        layout->addWidget(search);
        layout->addWidget(other);
        search->setPlaceholderText(QStringLiteral("Search Explorer Reference"));
        expose(window);
        const auto checkFont = [search](bool italic) {
            search->grab();
            QCOMPARE(search->font().pointSizeF(), 9.0);
            QCOMPARE(search->font().italic(), italic);
        };
        checkFont(true);
        const int height = search->fontMetrics().height();
        search->setFocus(Qt::TabFocusReason);
        checkFont(true);
        QSignalSpy entered(search, &QLineEdit::returnPressed);
        QTest::keyClicks(search, "notes");
        QCOMPARE(search->text(), QStringLiteral("notes"));
        checkFont(false);
        QCOMPARE(search->fontMetrics().height(), height);
        QTest::keyClick(search, Qt::Key_Return);
        QCOMPARE(entered.size(), 1);
        QTest::keyClick(search, Qt::Key_A, Qt::ControlModifier);
        QCOMPARE(search->selectedText(), QStringLiteral("notes"));
        other->setFocus();
        checkFont(false);
        search->clear();
        checkFont(true);
        // MainWindow restores search URLs under QSignalBlocker.
        {
            QSignalBlocker blocker(search);
            search->setText(QStringLiteral("restored"));
        }
        checkFont(false);
        {
            QSignalBlocker blocker(search);
            search->clear();
        }
        checkFont(true);
        search->setEnabled(false);
        checkFont(true);
        QCOMPARE(search->placeholderText(), QStringLiteral("Search Explorer Reference"));
    }

    void renderStateSnapshots()
    {
        const QString output = qEnvironmentVariable("EXPLORER_SNAPSHOTS");
        if (output.isEmpty()) QSKIP("Set EXPLORER_SNAPSHOTS for control-state captures");
        QVERIFY(QDir().mkpath(output));
        BreadcrumbBar bar;
        bar.resize(430, 22);
        bar.setTrail(QIcon(), trail());
        expose(bar);
        const auto names = bar.findChildren<QToolButton *>(QStringLiteral("crumbName"));
        const auto arrows = bar.findChildren<QToolButton *>(QStringLiteral("crumbDropdown"));
        const auto save = [&output](QWidget &widget, const QString &name) {
            QVERIFY(widget.grab().save(output + QLatin1Char('/') + name + QStringLiteral(".png")));
        };
        if (bar.focusWidget()) bar.focusWidget()->clearFocus();
        QTest::mouseMove(&bar, QPoint(420, 10));
        save(bar, QStringLiteral("breadcrumb-normal"));
        QTest::mouseMove(names.last(), names.last()->rect().center());
        save(bar, QStringLiteral("breadcrumb-name-hover"));
        QTest::mousePress(names.last(), Qt::LeftButton);
        save(bar, QStringLiteral("breadcrumb-name-pressed"));
        QTest::mouseRelease(names.last(), Qt::LeftButton);
        QTest::mouseMove(arrows.last(), arrows.last()->rect().center());
        save(bar, QStringLiteral("breadcrumb-arrow-hover"));
        QTest::mousePress(arrows.last(), Qt::LeftButton);
        save(bar, QStringLiteral("breadcrumb-arrow-pressed"));
        QTest::mouseRelease(arrows.last(), Qt::LeftButton);
        QTest::mouseMove(&bar, QPoint(420, 10));
        names.last()->setFocus(Qt::TabFocusReason);
        save(bar, QStringLiteral("breadcrumb-keyboard-focus"));
        names.last()->clearFocus();
        bar.setPopupOpen(trail().last().url, true);
        save(bar, QStringLiteral("breadcrumb-popup-open"));
        bar.setPopupOpen(trail().last().url, false);
        QWidget peer;
        expose(peer);
        QVERIFY(!bar.isActiveWindow());
        save(bar, QStringLiteral("breadcrumb-inactive"));
        peer.hide();
        bar.activateWindow();
        bar.setEnabled(false);
        save(bar, QStringLiteral("breadcrumb-disabled"));
        bar.setEnabled(true);
        bar.resize(150, 22);
        save(bar, QStringLiteral("breadcrumb-overflow-elision"));
        QWidget window;
        auto *layout = new QHBoxLayout(&window);
        auto *search = new SearchBox;
        auto *other = new QLineEdit;
        layout->addWidget(search);
        layout->addWidget(other);
        search->setPlaceholderText(QStringLiteral("Search Explorer Reference"));
        expose(window);
        other->setFocus();
        save(*search, QStringLiteral("search-placeholder"));
        search->setFocus();
        save(*search, QStringLiteral("search-focused-placeholder"));
        QTest::keyClicks(search, "notes");
        save(*search, QStringLiteral("search-input"));
        other->setFocus();
        save(*search, QStringLiteral("search-blurred-input"));
        search->clear();
        save(*search, QStringLiteral("search-cleared"));
    }
};

int main(int argc, char **argv)
{
    // run-ui-tests.sh supplies a private HOME/config/bus. Copy only the theme
    // preferences for opt-in native captures; never change desktop settings.
    const bool nativeTheme = qEnvironmentVariableIsSet("EXPLORER_NATIVE_THEME");
    if (nativeTheme) {
        const QString original = qEnvironmentVariable("EXPLORER_NATIVE_CONFIG_DIR");
        const QString config = qEnvironmentVariable("XDG_CONFIG_HOME");
        QDir().mkpath(config + QStringLiteral("/Kvantum"));
        QFile::copy(original + QStringLiteral("/kdeglobals"), config + QStringLiteral("/kdeglobals"));
        QFile::copy(original + QStringLiteral("/Kvantum/kvantum.kvconfig"),
                    config + QStringLiteral("/Kvantum/kvantum.kvconfig"));
    }
    QApplication app(argc, argv);
    if (nativeTheme) Aero::registerStylesheet(&app);
    AddressControlsTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "AddressControlsTest.moc"
