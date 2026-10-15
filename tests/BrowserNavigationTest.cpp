#include "BrowserNavigation.h"
#include "BreadcrumbBar.h"
#include "ComputerModel.h"
#include "Libraries.h"
#include "Locations.h"
#include "Settings.h"

#include <KFilePlacesModel>
#include <QApplication>
#include <QFile>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QMenu>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolButton>

class BrowserNavigationTest : public QObject {
    Q_OBJECT
private:
    struct Browser {
        KFilePlacesModel places;
        ComputerModel computer{&places};
        QWidget host;
        QHBoxLayout *layout = new QHBoxLayout(&host);
        BrowserNavigation navigation{&places, &computer, layout};

        Browser()
        {
            host.resize(640, 48);
            host.show();
            host.activateWindow();
            navigation.installShortcuts(&host);
            QTest::qWait(10);
        }
    };

private slots:
    void historyBranchesAndSilentNavigation()
    {
        Browser browser;
        auto &nav = browser.navigation;
        const QUrl root = QUrl::fromLocalFile(QStringLiteral("/"));
        const QUrl first = QUrl::fromLocalFile(QStringLiteral("/tmp"));
        const QUrl branch = QUrl::fromLocalFile(QStringLiteral("/usr"));
        QSignalSpy locations(&nav, &BrowserNavigation::locationChanged);
        QSignalSpy navigated(&nav, &BrowserNavigation::navigated);
        auto *back = browser.host.findChild<QPushButton *>(QStringLiteral("backButton"));
        auto *forward = browser.host.findChild<QPushButton *>(QStringLiteral("forwardButton"));
        QVERIFY(back);
        QVERIFY(forward);
        QVERIFY(nav.initialize(root));
        QCOMPARE(locations.count(), 1);
        QCOMPARE(navigated.count(), 0);
        QVERIFY(!back->isEnabled());
        QVERIFY(!forward->isEnabled());
        QVERIFY(nav.navigateTo(first));
        QVERIFY(!nav.navigateTo(QUrl::fromLocalFile(QStringLiteral("/tmp/"))));
        QCOMPARE(locations.count(), 2);
        QCOMPARE(navigated.count(), 1);
        QVERIFY(back->isEnabled());
        back->click();
        QCOMPARE(nav.currentUrl(), root);
        QVERIFY(forward->isEnabled());
        forward->click();
        QCOMPARE(nav.currentUrl(), first);
        nav.goBack();
        const int soundCount = navigated.count();
        QVERIFY(nav.navigateTo(branch, false));
        QCOMPARE(navigated.count(), soundCount);
        QCOMPARE(nav.currentUrl(), branch);
        QVERIFY(!forward->isEnabled());
        nav.goForward();
        QCOMPARE(nav.currentUrl(), branch);
        nav.goBack();
        QCOMPARE(nav.currentUrl(), root);
    }

    void policyRejectsBeforeHistoryAndTypedFileDispatch()
    {
        Browser browser;
        auto &nav = browser.navigation;
        const QUrl root = QUrl::fromLocalFile(QStringLiteral("/"));
        const QUrl remote(QStringLiteral("sftp://example.invalid/folder"));
        QVERIFY(nav.initialize(root));
        QVERIFY(nav.navigateTo(remote));
        nav.goBack();
        nav.setUrlPolicy([](const QUrl &url) {
            return url.isLocalFile() ? QString() : QStringLiteral("Local locations only");
        });
        QSignalSpy errors(&nav, &BrowserNavigation::errorOccurred);
        QSignalSpy locations(&nav, &BrowserNavigation::locationChanged);
        QVERIFY(!nav.navigateTo(QUrl(QStringLiteral("admin:///tmp"))));
        nav.goForward(); // Recheck policy even for an existing history entry.
        QCOMPARE(nav.currentUrl(), root);
        QCOMPARE(errors.count(), 2);
        QCOMPARE(locations.count(), 0);
        QVERIFY(!nav.initialize(remote));
        QCOMPARE(nav.currentUrl(), root);

        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString path = temp.filePath(QStringLiteral("typed.txt"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        const QUrl target = QUrl::fromLocalFile(path);
        nav.setUrlPolicy([target](const QUrl &url) {
            return url == target ? QStringLiteral("File rejected") : QString();
        });
        QSignalSpy files(&nav, &BrowserNavigation::typedFileActivated);
        nav.focusPathEdit();
        QVERIFY(nav.pathEditor());
        nav.pathEditor()->setText(path);
        QTest::keyClick(nav.pathEditor(), Qt::Key_Return);
        QCOMPARE(files.count(), 0);
        QCOMPARE(errors.last().at(0).toString(), QStringLiteral("File rejected"));
        QCOMPARE(nav.currentUrl(), root);
        QVERIFY(!nav.pathEditor());
    }

    void typedFilesAndMissingPathsDoNotNavigate()
    {
        Browser browser;
        auto &nav = browser.navigation;
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QUrl folder = QUrl::fromLocalFile(temp.path());
        QVERIFY(nav.initialize(folder));
        const QString path = temp.filePath(QStringLiteral("file.txt"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("Do not launch me");
        file.close();
        QSignalSpy files(&nav, &BrowserNavigation::typedFileActivated);
        QSignalSpy errors(&nav, &BrowserNavigation::errorOccurred);
        QSignalSpy locations(&nav, &BrowserNavigation::locationChanged);
        nav.focusPathEdit();
        nav.pathEditor()->setText(path);
        QTest::keyClick(nav.pathEditor(), Qt::Key_Return);
        QCOMPARE(files.count(), 1);
        QCOMPARE(files.first().at(0).toUrl(), QUrl::fromLocalFile(path));
        QCOMPARE(nav.currentUrl(), folder);
        QCOMPARE(locations.count(), 0);
        QVERIFY(!nav.pathEditor());
        nav.focusPathEdit();
        nav.pathEditor()->setText(temp.filePath(QStringLiteral("missing")));
        QTest::keyClick(nav.pathEditor(), Qt::Key_Return);
        QCOMPARE(errors.count(), 1);
        QCOMPARE(locations.count(), 0);
        QCOMPARE(nav.currentUrl(), folder);
        QVERIFY(!nav.pathEditor());
        nav.goBack();
        QCOMPARE(nav.currentUrl(), folder);
    }

    void sharedAddressKeysSurfaceAndRefresh()
    {
        Browser browser;
        auto &nav = browser.navigation;
        QVERIFY(nav.initialize(QUrl::fromLocalFile(QStringLiteral("/tmp"))));
        QTest::keyClick(&browser.host, Qt::Key_L, Qt::ControlModifier);
        QTRY_VERIFY(nav.pathEditor());
        auto *editor = nav.pathEditor();
        QVERIFY(editor->hasFocus());
        QCOMPARE(editor->objectName(), QStringLiteral("pathEditor"));
        QCOMPARE(editor->accessibleName(), QStringLiteral("Address bar"));
        QCOMPARE(editor->selectedText(), QStringLiteral("/tmp"));
        auto *box = browser.host.findChild<QWidget *>(QStringLiteral("addressBox"));
        QVERIFY(box);
        auto *refresh = box->findChild<QToolButton *>();
        QVERIFY(refresh);
        const QRect editorRect(editor->mapTo(box, QPoint(0, 0)), editor->size());
        QCOMPARE(editorRect.left(), 0);
        QCOMPARE(editorRect.right() + 1, refresh->x());
        QVERIFY(editor->styleSheet().contains(QStringLiteral("background: white")));
        nav.refreshTrail();
        QCOMPARE(nav.pathEditor(), editor); // Async places updates must not discard edits.
        QTest::keyClick(editor, Qt::Key_Escape);
        QVERIFY(!nav.pathEditor());
        QTest::keyClick(&browser.host, Qt::Key_D, Qt::AltModifier);
        QTRY_VERIFY(nav.pathEditor());
        nav.pathEditor()->setText(QStringLiteral("/usr"));
        QTest::keyClick(nav.pathEditor(), Qt::Key_Return);
        QCOMPARE(nav.currentUrl(), QUrl::fromLocalFile(QStringLiteral("/usr")));
        QVERIFY(Settings::recentPaths().contains(QStringLiteral("/usr")));
        QSignalSpy refreshes(&nav, &BrowserNavigation::refreshRequested);
        refresh->click();
        QCOMPARE(refreshes.count(), 1);
        QTest::keyClick(&browser.host, Qt::Key_F5);
        QCOMPARE(refreshes.count(), 2);
    }

    void shellAndElevatedTrailsAndParents()
    {
        Browser browser;
        auto &nav = browser.navigation;
        QVERIFY(nav.initialize(Locations::libraries()));
        QCOMPARE(nav.crumbsFor(nav.currentUrl()).last().label, QStringLiteral("Libraries"));
        nav.goUp();
        QCOMPARE(nav.currentUrl(), Locations::libraries());
        QVERIFY(nav.navigateTo(QUrl::fromLocalFile(QStringLiteral("/"))));
        nav.goUp();
        QCOMPARE(nav.currentUrl(), Locations::computer());
        nav.goUp();
        QCOMPARE(nav.currentUrl(), Locations::computer());
        const QUrl admin(QStringLiteral("admin:///tmp/subfolder"));
        QVERIFY(nav.navigateTo(admin)); // Address-only: no admin worker is invoked.
        const auto crumbs = nav.crumbsFor(admin);
        QCOMPARE(crumbs.last().target, admin);
        for (const auto &crumb : crumbs)
            QVERIFY(!crumb.target.isLocalFile());
        QCOMPARE(BrowserNavigation::displayPath(admin), QStringLiteral("/tmp/subfolder"));
        nav.goUp();
        QCOMPARE(nav.currentUrl(), QUrl(QStringLiteral("admin:///tmp/")));
        for (const auto &entry : Libraries::entries()) {
            QVERIFY(nav.navigateTo(entry.url));
            nav.goUp();
            QCOMPARE(nav.currentUrl(), Locations::libraries());
        }
    }

    void progressUsesSharedAddressSurface()
    {
        Browser browser;
        auto &nav = browser.navigation;
        QVERIFY(nav.initialize(QUrl::fromLocalFile(QStringLiteral("/tmp"))));
        auto *progress = browser.host.findChild<QProgressBar *>();
        QVERIFY(progress);
        nav.startPathProgress();
        QVERIFY(!progress->isVisible());
        nav.setPathProgress(40);
        nav.setPathProgress(20);
        QCOMPARE(progress->value(), 40);
        nav.showPathProgress();
        QVERIFY(progress->isVisible());
        nav.finishPathProgress();
        QTRY_VERIFY(!progress->isVisible());
        QCOMPARE(progress->value(), 0);
        nav.startPathProgress();
        nav.showPathProgress();
        nav.stopPathProgress();
        QVERIFY(!progress->isVisible());
    }
};

QTEST_MAIN(BrowserNavigationTest)
#include "BrowserNavigationTest.moc"
