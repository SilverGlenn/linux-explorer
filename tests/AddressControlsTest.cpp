#include "BreadcrumbBar.h"
#include "SearchBox.h"
#include <AeroQt/stylesheet.h>

#include <QApplication>
#include <QAbstractButton>
#include <QAccessible>
#include <QDir>
#include <QFile>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPointer>
#include <QSignalBlocker>
#include <QSignalSpy>
#include <QStyle>
#include <QTest>
#include <QTimer>
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
    static QRectF inkBounds(QWidget *widget)
    {
        const QImage image = widget->grab().toImage();
        QRect bounds;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                const QColor color = image.pixelColor(x, y);
                if (color.alpha() > 200 && qMax(color.red(), qMax(color.green(), color.blue())) < 180)
                    bounds |= QRect(x, y, 1, 1);
            }
        }
        const qreal dpr = image.devicePixelRatio();
        return QRectF(bounds.x() / dpr, bounds.y() / dpr,
                      bounds.width() / dpr, bounds.height() / dpr);
    }
    static void applyGlassTheme(QWidget &widget)
    {
        widget.setProperty("_Aero_transpbg", true);
        if (qEnvironmentVariableIsSet("EXPLORER_NATIVE_THEME")) {
            // Use the installed AeroQt rules inherited by MainWindow's inset
            // navigation bar, not an approximation of its competing QSS.
            QFile stylesheet(QStringLiteral(":/AeroQt/style.css"));
            QVERIFY(stylesheet.open(QIODevice::ReadOnly));
            widget.setStyleSheet(QString::fromUtf8(stylesheet.readAll()));
        }
    }
    static void snapshot(QWidget &widget, const QString &name)
    {
        const QString output = qEnvironmentVariable("EXPLORER_SNAPSHOTS");
        if (output.isEmpty()) return;
        QVERIFY(QDir().mkpath(output));
        QVERIFY(widget.grab().save(output + QLatin1Char('/') + name + QStringLiteral(".png")));
    }

private Q_SLOTS:
    void breadcrumbVisibleArrowAlignment_data()
    {
        QTest::addColumn<int>("height");
        QTest::addColumn<int>("width");
        QTest::newRow("integrated-locations") << 20 << 500;
        QTest::newRow("integrated-overflow") << 20 << 150;
        QTest::newRow("standalone-locations") << 22 << 500;
        QTest::newRow("standalone-overflow") << 22 << 150;
    }

    void breadcrumbVisibleArrowAlignment()
    {
        QFETCH(int, height);
        QFETCH(int, width);
        BreadcrumbBar bar;
        bar.resize(width, height);
        QPixmap icon(16, 16);
        icon.fill(Qt::cyan);
        bar.setTrail(QIcon(icon), trail());
        expose(bar);
        const auto names = bar.findChildren<QToolButton *>(QStringLiteral("crumbName"));
        const auto arrows = bar.findChildren<QToolButton *>(QStringLiteral("crumbDropdown"));
        auto *leading = bar.findChild<QToolButton *>(QStringLiteral("breadcrumbLocations"));
        const QStringList states = {QStringLiteral("normal"), QStringLiteral("name-hover"),
            QStringLiteral("arrow-hover"), QStringLiteral("leading-hover"),
            QStringLiteral("name-pressed"), QStringLiteral("arrow-pressed"),
            QStringLiteral("menu-open")};
        for (const QString &state : states) {
            for (auto *button : bar.findChildren<QToolButton *>()) {
                button->clearFocus();
                button->setDown(false);
            }
            bar.setPopupOpen(QUrl(), false);
            QTest::mouseMove(bar.windowHandle(), QPoint(8, height / 2));
            if (state == QLatin1String("name-hover"))
                QTest::mouseMove(bar.windowHandle(), names.last()->mapTo(&bar, names.last()->rect().center()));
            if (state == QLatin1String("arrow-hover"))
                QTest::mouseMove(bar.windowHandle(), arrows.last()->mapTo(&bar, arrows.last()->rect().center()));
            if (state == QLatin1String("leading-hover")) {
                QTest::mouseMove(bar.windowHandle(), leading->mapTo(&bar, leading->rect().center()));
                QTRY_VERIFY(leading->underMouse());
            }
            if (state == QLatin1String("name-pressed")) names.last()->setDown(true);
            if (state == QLatin1String("arrow-pressed")) {
                for (auto *arrow : arrows) arrow->setDown(true);
                leading->setDown(true);
            }
            // Measure the real raster ink, excluding the light split chrome.
            // In MainWindow a 24px address frame leaves 20px for the bar; the
            // original standalone captures use 22px. Neither may move globally.
            for (int i = 0; i < arrows.size(); ++i) {
                if (!arrows.at(i)->isVisible()) continue;
                if (state == QLatin1String("menu-open")) bar.setPopupOpen(trail().at(i).url, true);
                const QRectF label = inkBounds(names.at(i));
                const QRectF arrow = inkBounds(arrows.at(i));
                QVERIFY(!label.isEmpty());
                QVERIFY(!arrow.isEmpty());
                const qreal offset = arrow.center().y() - label.center().y();
                qInfo() << QTest::currentDataTag() << state << i << "arrow" << arrow
                        << "label" << label << "vertical offset" << offset;
                QVERIFY2(qAbs(offset) <= 1.6, qPrintable(QStringLiteral("%1: visible arrow-label offset %2")
                    .arg(state).arg(offset)));
                const auto *labelIcon = bar.findChild<QLabel *>();
                QCOMPARE(labelIcon->geometry(), QRect(0, (height - 16) / 2, 16, 16));
                QVERIFY(qAbs(arrow.center().y() - (labelIcon->y() + 8.0)) <= 2.0);
            }
            if (state == QLatin1String("menu-open")) bar.setPopupOpen(QUrl(), true);
            const QRectF leadingInk = inkBounds(leading);
            const QRectF lastLabel = inkBounds(names.last());
            QVERIFY(!leadingInk.isEmpty());
            qInfo() << QTest::currentDataTag() << state << "leading" << leadingInk
                    << "vertical offset" << leadingInk.center().y() - lastLabel.center().y();
            QVERIFY2(qAbs(leadingInk.center().y() - lastLabel.center().y()) <= 1.6,
                     qPrintable(QStringLiteral("%1: leading ink %2 vs label %3")
                         .arg(state).arg(leadingInk.center().y()).arg(lastLabel.center().y())));
            snapshot(bar, QStringLiteral("geometry-%1-%2").arg(QLatin1String(QTest::currentDataTag()), state));
        }
    }

    void integratedAddressAndSearchEdges()
    {
        QWidget window;
        applyGlassTheme(window);
        auto *layout = new QHBoxLayout(&window);
        layout->setContentsMargins(4, 3, 6, 3);
        layout->setSpacing(6);
        auto *address = new QWidget;
        address->setObjectName(QStringLiteral("addressBox"));
        address->setFixedHeight(24);
        address->setStyleSheet(QStringLiteral(
            "#addressBox { border-image: url(:/AeroQt/transpbg/entry/normal.png) 4 4 4 4 repeat; border-width: 4px; }"));
        auto *addressLayout = new QHBoxLayout(address);
        addressLayout->setContentsMargins(0, 2, 2, 2);
        addressLayout->setSpacing(0);
        auto *area = new QWidget;
        auto *areaLayout = new QHBoxLayout(area);
        areaLayout->setContentsMargins(4, 0, 4, 0);
        areaLayout->setSpacing(0);
        auto *bar = new BreadcrumbBar;
        QPixmap icon(16, 16);
        icon.fill(QColor("#329BC8"));
        bar->setTrail(QIcon(icon), trail());
        areaLayout->addWidget(bar, 1);
        addressLayout->addWidget(area, 1);
        auto *refresh = new QToolButton;
        refresh->setText(QStringLiteral("R"));
        refresh->setFixedWidth(24);
        addressLayout->addWidget(refresh);
        layout->addWidget(address, 1);
        auto *search = new SearchBox;
        search->setPlaceholderText(QStringLiteral("Search Explorer Reference"));
        search->setAccessibleName(search->placeholderText());
        search->addAction(window.style()->standardIcon(QStyle::SP_FileDialogContentsView), QLineEdit::TrailingPosition);
        const auto iconButtons = search->findChildren<QAbstractButton *>();
        search->setClearButtonEnabled(true);
        QAbstractButton *clearButton = nullptr;
        for (auto *button : search->findChildren<QAbstractButton *>())
            if (!iconButtons.contains(button)) clearButton = button;
        QVERIFY(clearButton);
        layout->addWidget(search);
        window.resize(760, 30);
        expose(window);
        QCOMPARE(bar->height(), 20);
        QImage focusedFrame;
        const auto checkEdges = [search, &focusedFrame](const QString &state) {
            const QImage image = search->grab().toImage();
            const int frame = qFloor(4 * image.devicePixelRatio());
            // Editing, selection and clear actions must not erase or replace
            // the Aero frame. Its texture/blending is checked against the
            // address entry below, rather than hard-coded solid border colors.
            if (search->hasFocus()) {
                if (focusedFrame.isNull()) focusedFrame = image;
                QCOMPARE(image.copy(0, 0, image.width(), frame),
                         focusedFrame.copy(0, 0, image.width(), frame));
                QCOMPARE(image.copy(0, image.height() - frame, image.width(), frame),
                         focusedFrame.copy(0, image.height() - frame, image.width(), frame));
            }
            QVERIFY(search->hasFrame());
            snapshot(*search, QStringLiteral("integrated-search-%1").arg(state));
            QCOMPARE(search->font().pointSizeF(), 9.0);
        };
        refresh->setFocus();
        checkEdges(QStringLiteral("blurred-empty"));
        snapshot(window, QStringLiteral("integrated-address-normal"));
        search->setFocus();
        checkEdges(QStringLiteral("focused-empty"));
        const QImage focusedCue = search->grab().toImage();
        search->setPlaceholderText(QString());
        QCOMPARE(search->grab().toImage(), focusedCue);
        search->setPlaceholderText(QStringLiteral("Search Explorer Reference"));
        QTest::keyClicks(search, "notes");
        checkEdges(QStringLiteral("focused-typed"));
        snapshot(window, QStringLiteral("integrated-address-search-typed"));
        QTest::keyClick(search, Qt::Key_A, Qt::ControlModifier);
        QCOMPARE(search->selectedText(), QStringLiteral("notes"));
        checkEdges(QStringLiteral("focused-selection"));
        QTest::keyClick(search, Qt::Key_Backspace);
        checkEdges(QStringLiteral("focused-cleared"));
        {
            QSignalBlocker blocker(search);
            search->setText(QStringLiteral("restored"));
        }
        checkEdges(QStringLiteral("focused-restored"));
        QVERIFY(!search->font().italic());
        // QLineEdit still owns its clear button and extra trailing icon; we
        // have only replaced the frame, not the editor or its actions.
        QTRY_VERIFY(clearButton->isVisible());
        QTest::mouseClick(clearButton, Qt::LeftButton);
        QCOMPARE(search->text(), QString());
        checkEdges(QStringLiteral("focused-clear-action"));
        refresh->setFocus();
        checkEdges(QStringLiteral("blurred-cleared"));
        search->setText(QStringLiteral("notes"));
        checkEdges(QStringLiteral("blurred-typed"));
    }

    void searchGlassMatchesAddressEntry_data()
    {
        QTest::addColumn<QColor>("background");
        QTest::newRow("blue-glass") << QColor("#386991");
        QTest::newRow("warm-glass") << QColor("#916438");
    }

    void searchGlassMatchesAddressEntry()
    {
        QFETCH(QColor, background);
        QWidget window;
        applyGlassTheme(window);
        window.setObjectName(QStringLiteral("glassTestWindow"));
        window.setStyleSheet(window.styleSheet() + QStringLiteral(
            "\n#glassTestWindow { background-color: %1; }").arg(background.name()));
        auto *layout = new QHBoxLayout(&window);
        // Align both entries to the same physical-pixel phase at 100/125/150%.
        layout->setContentsMargins(12, 12, 12, 12);
        layout->setSpacing(8);
        // A blank addressBox uses the same entry as MainWindow, without text
        // obscuring the pixels we compare. Both controls share the glass parent.
        auto *address = new QWidget;
        address->setObjectName(QStringLiteral("addressBox"));
        address->setFixedSize(240, 24);
        auto *search = new SearchBox;
        search->setFixedWidth(240);
        auto *other = new QToolButton;
        other->setText(QStringLiteral("Focus elsewhere"));
        layout->addWidget(address);
        layout->addWidget(search);
        layout->addWidget(other);
        expose(window);
        const auto checkEntry = [&](const QString &asset, const QString &state) {
            address->setStyleSheet(QStringLiteral(
                "#addressBox { border-image: url(:/AeroQt/transpbg/entry/%1.png) 4 4 4 4 repeat; border-width: 4px; }")
                .arg(asset));
            // Capture the common parent: grabbing an individual transparent
            // QLineEdit returns uncomposited alpha on some Qt styles.
            const QImage composed = window.grab().toImage();
            QCOMPARE(composed.pixelColor(0, 0), background);
            const qreal scale = composed.devicePixelRatio();
            const auto crop = [&](QWidget *control) {
                const QPoint origin = control->mapTo(&window, QPoint());
                return composed.copy(qRound(origin.x() * scale), qRound(origin.y() * scale),
                                     qRound(control->width() * scale), qRound(control->height() * scale));
            };
            const QImage expected = crop(address);
            const QImage actual = crop(search);
            snapshot(window, QStringLiteral("glass-%1-%2").arg(QLatin1String(QTest::currentDataTag()), state));
            QCOMPARE(actual.size(), expected.size());
            const qreal dpr = actual.devicePixelRatio();
            const int frame = qFloor(4 * dpr);
            // All four textured frame strips, including corners, must match
            // the real address entry, not a flat substitute or a second frame.
            QCOMPARE(actual.copy(0, 0, actual.width(), frame),
                     expected.copy(0, 0, expected.width(), frame));
            QCOMPARE(actual.copy(0, actual.height() - frame, actual.width(), frame),
                     expected.copy(0, expected.height() - frame, expected.width(), frame));
            QCOMPARE(actual.copy(0, 0, frame, actual.height()),
                     expected.copy(0, 0, frame, expected.height()));
            QCOMPARE(actual.copy(actual.width() - frame, 0, frame, actual.height()),
                     expected.copy(expected.width() - frame, 0, frame, expected.height()));
            // This patch is clear of cue/query/selection and trailing actions.
            // Equality on two different colored parents proves the same alpha
            // blend as addressBox, including focused/typed and disabled states.
            const QRect interior(qRound(120 * dpr), qRound(6 * dpr),
                                 qRound(8 * dpr), qRound(10 * dpr));
            QCOMPARE(actual.copy(interior), expected.copy(interior));
            const QColor blended = actual.pixelColor(interior.center());
            QVERIFY(blended != QColor(Qt::white));
            QVERIFY(blended != background);
        };
        other->setFocus();
        QTest::mouseMove(window.windowHandle(), other->mapTo(&window, other->rect().center()));
        QTRY_VERIFY(!search->underMouse());
        checkEntry(QStringLiteral("normal"), QStringLiteral("normal"));
        QTest::mouseMove(window.windowHandle(), search->mapTo(&window, search->rect().center()));
        QTRY_VERIFY(search->underMouse());
        checkEntry(QStringLiteral("hover"), QStringLiteral("hover"));
        QTest::mouseClick(window.windowHandle(), Qt::LeftButton, Qt::NoModifier,
                          search->mapTo(&window, search->rect().center()));
        QTRY_VERIFY(search->hasFocus());
        QTest::mouseMove(window.windowHandle(), other->mapTo(&window, other->rect().center()));
        QTRY_VERIFY(!search->underMouse());
        checkEntry(QStringLiteral("hover"), QStringLiteral("focused-empty"));
        QTest::keyClicks(search, "notes");
        checkEntry(QStringLiteral("hover"), QStringLiteral("focused-typed"));
        search->selectAll();
        checkEntry(QStringLiteral("hover"), QStringLiteral("focused-selection"));
        search->clear();
        checkEntry(QStringLiteral("hover"), QStringLiteral("focused-cleared"));
        search->setEnabled(false);
        checkEntry(QStringLiteral("disabled"), QStringLiteral("disabled"));
    }

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

    void breadcrumbMenusOpenOnPressWithoutDuplicateActivation()
    {
        BreadcrumbBar bar;
        bar.resize(500, 22);
        bar.setTrail(QIcon(), trail());
        expose(bar);
        QSignalSpy menus(&bar, &BreadcrumbBar::menuRequested);
        QSignalSpy navigation(&bar, &BreadcrumbBar::urlActivated);
        bool sawOpenState = false;
        connect(&bar, &BreadcrumbBar::menuRequested, &bar,
                [&bar, &sawOpenState](const QUrl &url, const QPoint &position) {
            QMenu menu;
            menu.addAction(QStringLiteral("Child folder"));
            connect(&menu, &QMenu::aboutToShow, &menu, [&bar, url, &sawOpenState] {
                bar.setPopupOpen(url, true);
                auto *arrow = url.isValid()
                    ? bar.findChildren<QToolButton *>(QStringLiteral("crumbDropdown")).last()
                    : bar.findChild<QToolButton *>(QStringLiteral("breadcrumbLocations"));
                sawOpenState = arrow->property("popupOpen").toBool();
            });
            connect(&menu, &QMenu::aboutToHide, &menu,
                    [&bar, url] { bar.setPopupOpen(url, false); });
            QTimer::singleShot(0, &menu, &QMenu::close);
            menu.exec(position);
        });
        auto *arrow = bar.findChildren<QToolButton *>(QStringLiteral("crumbDropdown")).last();
        QTest::mousePress(arrow, Qt::LeftButton);
        QCOMPARE(menus.size(), 1);
        QVERIFY(sawOpenState);
        QVERIFY(!arrow->isDown());
        QVERIFY(!arrow->property("popupOpen").toBool());
        QTest::mouseMove(arrow, QPoint(-2, -2));
        QTest::mouseMove(arrow, arrow->rect().center());
        QCOMPARE(menus.size(), 1);
        QTest::mouseRelease(arrow, Qt::LeftButton);
        QCOMPARE(menus.size(), 1);
        arrow->setFocus(Qt::TabFocusReason);
        QTest::keyPress(arrow, Qt::Key_Space);
        QCOMPARE(menus.size(), 2);
        QTest::keyRelease(arrow, Qt::Key_Space);
        QCOMPARE(menus.size(), 2);
        auto *leading = bar.findChild<QToolButton *>(QStringLiteral("breadcrumbLocations"));
        QTest::mousePress(leading, Qt::LeftButton);
        QCOMPARE(menus.size(), 3);
        QVERIFY(!menus.last().first().toUrl().isValid());
        QVERIFY(!leading->isDown());
        QTest::mouseRelease(leading, Qt::LeftButton);
        QCOMPARE(menus.size(), 3);
        QCOMPARE(navigation.size(), 0);
    }

    void breadcrumbPressAllowsTrailReplacement()
    {
        BreadcrumbBar bar;
        bar.resize(500, 22);
        bar.setTrail(QIcon(), trail());
        expose(bar);
        const QPointer<QToolButton> arrow(
            bar.findChildren<QToolButton *>(QStringLiteral("crumbDropdown")).last());
        QSignalSpy menus(&bar, &BreadcrumbBar::menuRequested);
        connect(&bar, &BreadcrumbBar::menuRequested, &bar,
                [&bar] { bar.setTrail(QIcon(), trail()); });
        QTest::mousePress(arrow, Qt::LeftButton);
        QTest::mouseRelease(&bar, Qt::LeftButton, Qt::NoModifier, QPoint(490, 10));
        QCOMPARE(menus.size(), 1);
        QVERIFY(arrow.isNull());
        QCOMPARE(bar.findChildren<QToolButton *>(QStringLiteral("crumbDropdown")).size(), 3);
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
        auto *segment = names.last()->parentWidget();
        const auto borderColor = [](const QImage &image, const QPoint &logicalPosition) {
            const qreal dpr = image.devicePixelRatio();
            const int x = qBound(0, qFloor(logicalPosition.x() * dpr), image.width() - 1);
            const int y = qBound(0, qFloor(logicalPosition.y() * dpr), image.height() - 1);
            return image.pixelColor(x, y);
        };
        const QPoint rightBorder(segment->width() - 1, 5);
        const QPoint leftBorder(0, 5);
        const QImage normal = segment->grab().toImage();
        QTest::mouseMove(names.last(), names.last()->rect().center());
        const QImage nameHover = segment->grab().toImage();
        // QImage coordinates are physical pixels. Fractional DPR can leave
        // padding after the last logical border pixel in the grabbed image.
        QVERIFY(borderColor(nameHover, rightBorder) != borderColor(normal, rightBorder));
        QTest::mousePress(names.last(), Qt::LeftButton);
        const QImage namePressed = names.last()->parentWidget()->grab().toImage();
        QVERIFY(namePressed != nameHover);
        QSignalSpy cancelledNavigation(&bar, &BreadcrumbBar::urlActivated);
        QTest::mouseRelease(names.last(), Qt::LeftButton, Qt::NoModifier, QPoint(-4, -4));
        QCOMPARE(cancelledNavigation.size(), 0);
        // QWidget mouseMove uses QCursor::setPos; the offscreen cursor path
        // misroutes fractional-DPR positions. Inject through QPA instead.
        QTest::mouseMove(bar.windowHandle(), arrows.last()->mapTo(&bar, arrows.last()->rect().center()));
        QTRY_VERIFY(arrows.last()->underMouse());
        const QImage arrowHover = names.last()->parentWidget()->grab().toImage();
        QVERIFY(borderColor(arrowHover, leftBorder) != borderColor(normal, leftBorder));
        QTest::mouseMove(&bar, QPoint(490, 10));
        bar.setPopupOpen(trail().last().url, true);
        QVERIFY(arrows.last()->property("popupOpen").toBool());
        const QImage popup = names.last()->parentWidget()->grab().toImage();
        QVERIFY(popup != normal);
        // The name's outer edge changes too, not just the independent arrow.
        QVERIFY(borderColor(popup, leftBorder) != borderColor(normal, leftBorder));
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

    void searchHidesFocusedPlaceholderAndRestoresLocationOnBlur()
    {
        QWidget window;
        auto *layout = new QHBoxLayout(&window);
        auto *search = new SearchBox;
        auto *other = new QLineEdit;
        layout->addWidget(search);
        layout->addWidget(other);
        const QString location = QStringLiteral("Search Explorer Reference");
        search->setPlaceholderText(location);
        search->setAccessibleName(location);
        expose(window);
        QSignalSpy changes(search, &QLineEdit::textChanged);
        search->setFocus(Qt::TabFocusReason);
        QVERIFY(search->hasFocus());
        const QImage focusedCue = search->grab().toImage();
        QCOMPARE(search->placeholderText(), location);
        QCOMPARE(search->font().pointSizeF(), 9.0);
        auto *accessible = QAccessible::queryAccessibleInterface(search);
        QVERIFY(accessible);
        QCOMPARE(accessible->text(QAccessible::Name), location);
        // A focused field with a location cue must render exactly as an empty
        // cue: no text drawn, without clearing the actual placeholder property.
        search->setPlaceholderText(QString());
        const QImage focusedBlank = search->grab().toImage();
        QCOMPARE(focusedCue, focusedBlank);
        const QString nextLocation = QStringLiteral("Search Pictures");
        search->setPlaceholderText(nextLocation);
        QCOMPARE(search->grab().toImage(), focusedBlank);
        {
            QSignalBlocker blocker(search);
            search->setText(QStringLiteral("restored"));
        }
        search->grab();
        QCOMPARE(search->font().pointSizeF(), 9.0);
        QVERIFY(!search->font().italic());
        {
            QSignalBlocker blocker(search);
            search->clear();
        }
        QCOMPARE(search->grab().toImage(), focusedBlank);
        QCOMPARE(search->placeholderText(), nextLocation);
        other->setFocus(Qt::TabFocusReason);
        QVERIFY(!search->hasFocus());
        const QImage blurredCue = search->grab().toImage();
        QCOMPARE(search->placeholderText(), nextLocation);
        search->setPlaceholderText(QString());
        QVERIFY(search->grab().toImage() != blurredCue);
        search->setPlaceholderText(nextLocation);
        QCOMPARE(search->grab().toImage(), blurredCue);
        QCOMPARE(changes.size(), 0);
        QCOMPARE(search->text(), QString());
        QCOMPARE(search->font().pointSizeF(), 9.0);
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
        const auto popupStateConnection = connect(&bar, &BreadcrumbBar::menuRequested, &bar,
            [&bar](const QUrl &url) { bar.setPopupOpen(url, true); });
        QTest::mousePress(arrows.last(), Qt::LeftButton);
        save(bar, QStringLiteral("breadcrumb-arrow-pressed"));
        QTest::mouseRelease(arrows.last(), Qt::LeftButton);
        bar.setPopupOpen(trail().last().url, false);
        disconnect(popupStateConnection);
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
