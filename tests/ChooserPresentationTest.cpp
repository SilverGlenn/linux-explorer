#include "ApplicationAppearance.h"
#include "ChooserWindow.h"

#include <QApplication>
#include <QDialog>
#include <QEvent>
#include <QTest>
#include <QWindow>

class NativeCreationDialog : public QDialog {
public:
    QString nativeCreationAppId;
protected:
    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::WinIdChange && windowHandle())
            nativeCreationAppId = QGuiApplication::desktopFileName();
        return QDialog::event(event);
    }
};

class ChooserPresentationTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void validateChooserWindowOptions_data()
    {
        QTest::addColumn<QString>("parent");
        QTest::addColumn<QString>("token");
        QTest::addColumn<bool>("modal");
        QTest::addColumn<bool>("valid");
        QTest::newRow("standalone") << QString() << QString() << false << true;
        QTest::newRow("standalone-token") << QString() << QStringLiteral("startup-token") << false << true;
        QTest::newRow("modal-needs-parent") << QString() << QString() << true << false;
        QTest::newRow("unknown-parent") << QStringLiteral("invalid:123") << QString() << false << false;
        QTest::newRow("zero-xid") << QStringLiteral("x11:0") << QString() << false << false;
        QTest::newRow("oversized-xid") << QStringLiteral("x11:100000000") << QString() << false << false;
        QTest::newRow("empty-wayland") << QStringLiteral("wayland:") << QString() << false << false;
        QTest::newRow("control-token") << QString() << QStringLiteral("token\n") << false << false;
        QTest::newRow("nul-token") << QString() << QString(QChar::Null) << false << false;
        QTest::newRow("oversized-token") << QString() << QString(4097, QLatin1Char('a')) << false << false;
        QTest::newRow("oversized-parent") << QString(4097, QLatin1Char('a')) << QString() << false << false;
    }
    void validateChooserWindowOptions()
    {
        QFETCH(QString, parent);
        QFETCH(QString, token);
        QFETCH(bool, modal);
        QFETCH(bool, valid);
        QCOMPARE(Explorer::validateChooserWindowOptions({parent, token, modal}).isEmpty(), valid);
    }
    void foreignParentsRequireCompatibleBackend()
    {
        if (QGuiApplication::platformName() != QLatin1String("offscreen"))
            QSKIP("Requires offscreen backend");
        QVERIFY(!Explorer::validateChooserWindowOptions({QStringLiteral("x11:123"), {}, true}).isEmpty());
        QVERIFY(!Explorer::validateChooserWindowOptions({QStringLiteral("wayland:exported-token"), {}, true}).isEmpty());
    }
    void showChooserWindowScopesNativeAppIdAndRetainsDecorations()
    {
        const QString previous = QGuiApplication::desktopFileName();
        QGuiApplication::setDesktopFileName(QStringLiteral("test-browser-id"));
        NativeCreationDialog dialog;
        dialog.setWindowTitle(QStringLiteral("Choose a file"));
        const Qt::WindowFlags flags = dialog.windowFlags();
        Explorer::showChooserWindow(&dialog, {});
        QVERIFY(dialog.isVisible());
        QVERIFY(dialog.windowHandle());
        QCOMPARE(dialog.nativeCreationAppId, QStringLiteral("linux-explorer-win7-dialog"));
        QCOMPARE(QGuiApplication::desktopFileName(), QStringLiteral("test-browser-id"));
        QCOMPARE(dialog.windowModality(), Qt::NonModal);
        QCOMPARE(dialog.windowFlags(), flags);
        QVERIFY(!dialog.windowFlags().testFlag(Qt::FramelessWindowHint));
        QVERIFY(!dialog.windowFlags().testFlag(Qt::WindowStaysOnTopHint));
        QCOMPARE(dialog.windowTitle(), QStringLiteral("Choose a file"));
        QGuiApplication::setDesktopFileName(previous);
    }
    void configureApplicationAppearancePreservesLifecycleAndFiltersScrollBars()
    {
        auto *app = qobject_cast<QApplication *>(QCoreApplication::instance());
        const bool quitOnLastWindowClosed = app->quitOnLastWindowClosed();
        const bool quitLockEnabled = app->isQuitLockEnabled();
        const QString appId = app->desktopFileName();
        Explorer::configureApplicationAppearance(app);
        QCOMPARE(app->quitOnLastWindowClosed(), quitOnLastWindowClosed);
        QCOMPARE(app->isQuitLockEnabled(), quitLockEnabled);
        QCOMPARE(app->desktopFileName(), appId);
        QVERIFY(!app->styleSheet().contains(QLatin1String("QScrollBar")));

        // Simulate a desktop theme reapplying mixed rules after setup.
        app->setStyleSheet(QStringLiteral("/* { comment } */ QScrollBar, QLabel { color: red; } QPushButton { color: blue; }"));
        QEvent styleChange(QEvent::StyleChange);
        QCoreApplication::sendEvent(app, &styleChange);
        QTRY_VERIFY(!app->styleSheet().contains(QLatin1String("QScrollBar")));
        QVERIFY(app->styleSheet().contains(QStringLiteral("QLabel { color: red; }")));
        QVERIFY(app->styleSheet().contains(QStringLiteral("QPushButton { color: blue; }")));
    }
};

QTEST_MAIN(ChooserPresentationTest)
#include "ChooserPresentationTest.moc"
