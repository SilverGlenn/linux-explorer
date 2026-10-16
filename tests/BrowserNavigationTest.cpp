#include "BrowserNavigation.h"
#include "BreadcrumbBar.h"
#include "ComputerModel.h"
#include "GoogleDriveConnection.h"
#include "GoogleDriveService.h"
#include "Libraries.h"
#include "Locations.h"
#include "Settings.h"

#include <KFilePlacesModel>
#include <QApplication>
#include <QFile>
#include <QDir>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QMenu>
#include <QProgressBar>
#include <QPushButton>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolButton>

namespace {
class FixtureDriveConnection final : public GoogleDriveConnection {
public:
    bool mounted = false;
    bool isConnected() const override { return mounted; }
};
}

class BrowserNavigationTest : public QObject {
    Q_OBJECT
private:
    struct Browser {
        KFilePlacesModel places;
        FixtureDriveConnection driveConnection;
        ComputerModel computer{&places, &driveConnection, nullptr};
        QWidget host;
        QHBoxLayout *layout = new QHBoxLayout(&host);
        BrowserNavigation navigation{&places, &computer, layout, &driveConnection, nullptr};

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
    void managedDriveNeverInheritsAdministratorIdentity()
    {
        Browser browser;
        browser.driveConnection.mounted = true;
        auto &nav = browser.navigation;
        QVERIFY(nav.initialize(QUrl(QStringLiteral("admin:///tmp"))));
        nav.focusPathEdit();
        nav.pathEditor()->setText(QStringLiteral("gdrive:/google2"));
        nav.submitPathEdit();
        QCOMPARE(nav.currentUrl(), Locations::googleDriveMount());
        const QUrl folder = QUrl::fromLocalFile(
            Locations::googleDriveMount().toLocalFile() + QStringLiteral("/Folder"));
        QUrl elevated = folder;
        elevated.setScheme(QStringLiteral("admin"));
        QVERIFY(nav.navigateTo(elevated));
        QCOMPARE(nav.currentUrl(), folder);
        browser.driveConnection.mounted = false;
        QVERIFY(nav.navigateTo(Locations::computer()));
        QSignalSpy errors(&nav, &BrowserNavigation::errorOccurred);
        QVERIFY(!nav.navigateTo(elevated));
        QCOMPARE(errors.size(), 1);
        QCOMPARE(nav.currentUrl(), Locations::computer());
    }

    void typedManagedDriveDescendantsResolveAsynchronouslyAndCancelStaleResults()
    {
        const QString root = Locations::googleDriveMount().toLocalFile();
        if (!root.startsWith(QDir::tempPath() + QStringLiteral("/explorer-tests.")))
            QSKIP("This filesystem fixture requires run-ui-tests.sh isolation.");
        QVERIFY(QDir().mkpath(root + QStringLiteral("/Folder")));
        const auto cleanup = qScopeGuard([&] { QDir(root).removeRecursively(); });
        QFile fixture(root + QStringLiteral("/file.txt"));
        QVERIFY(fixture.open(QIODevice::WriteOnly));
        fixture.close();
        Browser browser;
        browser.driveConnection.mounted = true;
        auto &nav = browser.navigation;
        const QUrl initial = Locations::computer();
        const QUrl folder = QUrl::fromLocalFile(root + QStringLiteral("/Folder"));
        QVERIFY(nav.initialize(initial));
        QSignalSpy files(&nav, &BrowserNavigation::typedFileActivated);
        nav.focusPathEdit();
        nav.pathEditor()->setText(folder.toLocalFile());
        nav.submitPathEdit();
        QCOMPARE(nav.currentUrl(), initial); // worker has not returned yet
        QTRY_COMPARE(nav.currentUrl(), folder);
        nav.focusPathEdit();
        nav.pathEditor()->setText(fixture.fileName());
        nav.submitPathEdit();
        QCOMPARE(files.size(), 0);
        QTRY_COMPARE(files.size(), 1);
        QCOMPARE(files.first().first().toUrl(), QUrl::fromLocalFile(fixture.fileName()));
        QCOMPARE(nav.currentUrl(), folder);

        nav.focusPathEdit();
        nav.pathEditor()->setText(folder.toLocalFile());
        nav.submitPathEdit();
        QVERIFY(nav.navigateTo(initial));
        QTest::qWait(50);
        QCOMPARE(nav.currentUrl(), initial);
        QCOMPARE(files.size(), 1);
    }

    void managedDriveBreadcrumbMenuListsFoldersAsynchronously()
    {
        const QString root = Locations::googleDriveMount().toLocalFile();
        if (!root.startsWith(QDir::tempPath() + QStringLiteral("/explorer-tests.")))
            QSKIP("This filesystem fixture requires run-ui-tests.sh isolation.");
        QVERIFY(QDir().mkpath(root + QStringLiteral("/Folder")));
        QVERIFY(QDir().mkpath(root + QStringLiteral("/.Hidden")));
        const auto cleanup = qScopeGuard([&] { QDir(root).removeRecursively(); });
        QFile fixture(root + QStringLiteral("/file.txt"));
        QVERIFY(fixture.open(QIODevice::WriteOnly));
        fixture.close();
        Browser browser;
        browser.driveConnection.mounted = true;
        QVERIFY(browser.navigation.initialize(Locations::googleDriveMount()));
        auto *bar = browser.host.findChild<BreadcrumbBar *>();
        QVERIFY(bar);
        QStringList labels;
        QTimer poll;
        poll.setInterval(10);
        connect(&poll, &QTimer::timeout, &browser.host, [&] {
            auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            if (!menu) return;
            labels.clear();
            for (QAction *action : menu->actions())
                if (action->isVisible()) labels.append(action->text());
            if (labels.contains(QStringLiteral("Folder"))) menu->close();
        });
        QTimer::singleShot(3000, &browser.host, [] {
            if (auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget())) menu->close();
        });
        poll.start();
        Q_EMIT bar->menuRequested(Locations::googleDriveMount(), QPoint(20, 20));
        poll.stop();
        QVERIFY(labels.contains(QStringLiteral("Folder")));
        QVERIFY(!labels.contains(QStringLiteral("file.txt")));
        QVERIFY(!labels.contains(QStringLiteral(".Hidden")));
        QCOMPARE(browser.navigation.currentUrl(), Locations::googleDriveMount());
    }

    void legacyGoogleEntryPointsUseManagedDriveAndHistory()
    {
        Browser browser;
        auto &nav = browser.navigation;
        browser.driveConnection.mounted = true;
        const QUrl mount = Locations::googleDriveMount();
        const QUrl legacy(QStringLiteral("gdrive:/google2"));
        QVERIFY(nav.initialize(Locations::computer()));
        QSignalSpy locations(&nav, &BrowserNavigation::locationChanged);
        QVERIFY(nav.navigateTo(legacy));
        QCOMPARE(nav.currentUrl(), mount);
        QCOMPARE(locations.last().first().toUrl(), mount);
        QVERIFY(!nav.navigateTo(QUrl(QStringLiteral("gdrive:/"))));
        nav.goUp();
        QCOMPARE(nav.currentUrl(), Locations::computer());
        nav.goBack();
        QCOMPARE(nav.currentUrl(), mount);
        browser.driveConnection.mounted = false;
        nav.goBack();
        QCOMPARE(nav.currentUrl(), Locations::computer());
        QSignalSpy errors(&nav, &BrowserNavigation::errorOccurred);
        nav.goForward();
        QCOMPARE(errors.size(), 1);
        QCOMPARE(nav.currentUrl(), Locations::computer());
        QVERIFY(!nav.navigateTo(legacy));
        QCOMPARE(errors.size(), 2);
    }

    void destroyingNavigationClosesPendingDriveMenu()
    {
        Browser browser;
        browser.driveConnection.mounted = true;
        auto *nav = new BrowserNavigation(&browser.places, &browser.computer,
            browser.layout, &browser.driveConnection, &browser.host);
        QVERIFY(nav->initialize(Locations::googleDriveMount()));
        auto *bar = browser.layout->itemAt(browser.layout->count() - 1)->widget()
            ->findChild<BreadcrumbBar *>();
        // BreadcrumbBar itself can be the last layout widget.
        if (!bar)
            bar = qobject_cast<BreadcrumbBar *>(browser.layout->itemAt(browser.layout->count() - 1)->widget());
        QVERIFY(bar);
        QTimer::singleShot(0, &browser.host, [nav] { delete nav; });
        Q_EMIT bar->menuRequested(Locations::googleDriveMount(), QPoint(20, 20));
        QTest::qWait(20);
        QVERIFY(!QApplication::activePopupWidget());
    }

    void legacyGoogleEntryPointsRespectDestinationPolicyAndOfflineGuard()
    {
        Browser browser;
        auto &nav = browser.navigation;
        const QUrl initial = Locations::computer();
        const QUrl legacy(QStringLiteral("gdrive:/google2"));
        QVERIFY(nav.initialize(initial));
        QSignalSpy errors(&nav, &BrowserNavigation::errorOccurred);
        QSignalSpy files(&nav, &BrowserNavigation::typedFileActivated);
        nav.focusPathEdit();
        nav.pathEditor()->setText(legacy.toString());
        nav.submitPathEdit();
        QCOMPARE(errors.size(), 1);
        QCOMPARE(files.size(), 0);
        QCOMPARE(nav.currentUrl(), initial);
        browser.driveConnection.mounted = true;
        nav.setUrlPolicy([](const QUrl &url) {
            return Locations::isGoogleDriveMount(url) ? QStringLiteral("Drive blocked") : QString();
        });
        QVERIFY(!nav.initialize(legacy));
        QCOMPARE(errors.last().first().toString(), QStringLiteral("Drive blocked"));
        QCOMPARE(nav.currentUrl(), initial);
        nav.setUrlPolicy({});
        QVERIFY(nav.navigateTo(QUrl(QStringLiteral("gdrive:/new-account"))));
        QVERIFY(nav.redirectTo(QUrl(QStringLiteral("gdrive:/new-account")), legacy));
        QCOMPARE(nav.currentUrl(), Locations::googleDriveMount());
        nav.goBack();
        QCOMPARE(nav.currentUrl(), initial);
    }

    void googleDriveTrailPreservesProviderIds()
    {
        Browser browser;
        const QUrl target(QStringLiteral("gdrive:/person+test@example.com/Reports/Q4?id=opaque%2Fid"));
        const auto trail = browser.navigation.crumbsFor(target);
        QCOMPARE(trail.size(), 5);
        QCOMPARE(trail.at(0).label, QStringLiteral("Network"));
        QCOMPARE(trail.at(0).target, QUrl(QStringLiteral("remote:/")));
        QCOMPARE(trail.at(1).label, QStringLiteral("Google Drive"));
        QCOMPARE(trail.at(1).target, QUrl(QStringLiteral("gdrive:/")));
        QCOMPARE(trail.at(2).label, QStringLiteral("person+test@example.com"));
        QCOMPARE(trail.at(2).target, QUrl(QStringLiteral("gdrive:/person+test@example.com")));
        QCOMPARE(trail.at(3).target, QUrl(QStringLiteral("gdrive:/person+test@example.com/Reports")));
        QCOMPARE(trail.last().target, target);
    }

    void googleDriveMountRecognitionUsesAPathBoundary()
    {
        const QUrl mount = Locations::googleDriveMount();
        QVERIFY(mount.isLocalFile());
        QVERIFY(Locations::isGoogleDriveMount(mount));
        QVERIFY(Locations::isGoogleDriveMount(
            QUrl::fromLocalFile(mount.toLocalFile() + QStringLiteral("/Projects"))));
        QVERIFY(!Locations::isGoogleDriveMount(
            QUrl::fromLocalFile(mount.toLocalFile() + QStringLiteral("-backup"))));
        QVERIFY(!Locations::isGoogleDriveMount(
            QUrl::fromLocalFile(mount.toLocalFile() + QStringLiteral("/../google-drive-old"))));
        QVERIFY(!Locations::isGoogleDriveMount(QUrl(QStringLiteral("gdrive:/account"))));
    }

    void mountInfoEscapesDecodeKernelPathBytes()
    {
        QCOMPARE(Locations::detail::decodeMountInfoField(
                     QByteArrayLiteral("space\\040tab\\011line\\012slash\\134")),
                 QByteArray("space tab\tline\nslash\\"));
        QCOMPARE(Locations::detail::decodeMountInfoField(QByteArrayLiteral("unknown\\777")),
                 QByteArrayLiteral("unknown\\777"));
    }

    void googleDriveMountedParserRequiresExactReadOnlyRcloneRecord_data()
    {
        QTest::addColumn<QByteArray>("record");
        QTest::addColumn<bool>("mounted");
        const QByteArray mountPath = Locations::googleDriveMount().toLocalFile().toUtf8();
        const auto escape = [](const QByteArray &path) {
            QByteArray escaped;
            for (char ch : path) {
                switch (ch) {
                case ' ': escaped += QByteArrayLiteral("\\040"); break;
                case '\t': escaped += QByteArrayLiteral("\\011"); break;
                case '\n': escaped += QByteArrayLiteral("\\012"); break;
                case '\\': escaped += QByteArrayLiteral("\\134"); break;
                default: escaped.append(ch); break;
                }
            }
            return escaped;
        };
        const QByteArray encodedMountPath = escape(mountPath);
        const auto record = [](const QByteArray &mountPoint,
                               const QByteArray &options,
                               const QByteArray &fsType,
                               const QByteArray &source) {
            return QByteArrayLiteral("41 1 0:42 / ") + mountPoint + ' '
                + options + QByteArrayLiteral(" - ") + fsType + ' '
                + source + QByteArrayLiteral(" rw,user_id=1000\n");
        };

        QTest::newRow("valid")
            << record(encodedMountPath, QByteArrayLiteral("ro,nosuid,nodev"), QByteArrayLiteral("fuse.rclone"),
                      QByteArrayLiteral("explorer-google-drive:")) << true;
        QTest::newRow("foreign-source")
            << record(encodedMountPath, QByteArrayLiteral("ro,nosuid"), QByteArrayLiteral("fuse.rclone"),
                      QByteArrayLiteral("other-rclone:")) << false;
        QTest::newRow("read-write")
            << record(encodedMountPath, QByteArrayLiteral("rw,nosuid"), QByteArrayLiteral("fuse.rclone"),
                      QByteArrayLiteral("explorer-google-drive:")) << false;
        QTest::newRow("wrong-filesystem")
            << record(encodedMountPath, QByteArrayLiteral("ro,nosuid"), QByteArrayLiteral("fuse.sshfs"),
                      QByteArrayLiteral("explorer-google-drive:")) << false;
        QTest::newRow("sibling-path")
            << record(escape(mountPath + "-backup"), QByteArrayLiteral("ro,nosuid"),
                      QByteArrayLiteral("fuse.rclone"), QByteArrayLiteral("explorer-google-drive:"))
            << false;
        QTest::newRow("stacked-mount-records")
            << (record(encodedMountPath, QByteArrayLiteral("ro,nosuid"),
                       QByteArrayLiteral("fuse.rclone"), QByteArrayLiteral("explorer-google-drive:"))
                + record(encodedMountPath, QByteArrayLiteral("rw,nosuid"),
                         QByteArrayLiteral("fuse.rclone"), QByteArrayLiteral("other-rclone:")))
            << false;
    }

    void googleDriveMountedParserRequiresExactReadOnlyRcloneRecord()
    {
        QFETCH(QByteArray, record);
        QFETCH(bool, mounted);
        QCOMPARE(Locations::googleDriveMountedIn(record), mounted);
        QCOMPARE(Locations::googleDriveMountIdIn(record),
                 mounted ? QStringLiteral("41") : QString());
    }

    void disconnectedPersonalMountBreadcrumbsHideBackingPath()
    {
        Browser browser;
        const QUrl mount = Locations::googleDriveMount();
        const auto root = browser.navigation.crumbsFor(mount);
        QCOMPARE(root.size(), 2);
        QCOMPARE(root.at(0).label, QStringLiteral("Computer"));
        QCOMPARE(root.at(0).target, Locations::computer());
        QCOMPARE(root.at(1).label, QStringLiteral("Google Drive (G:)"));
        QCOMPARE(root.at(1).target, mount);

        const QUrl descendant = QUrl::fromLocalFile(
            mount.toLocalFile() + QStringLiteral("/Projects/Reports"));
        const auto trail = browser.navigation.crumbsFor(descendant);
        QCOMPARE(trail.size(), 4);
        QCOMPARE(trail.at(0).label, QStringLiteral("Computer"));
        QCOMPARE(trail.at(1).label, QStringLiteral("Google Drive (G:)"));
        QCOMPARE(trail.at(2).label, QStringLiteral("Projects"));
        QCOMPARE(trail.at(2).target,
                 QUrl::fromLocalFile(mount.toLocalFile() + QStringLiteral("/Projects")));
        QCOMPARE(trail.at(3).label, QStringLiteral("Reports"));
        QCOMPARE(trail.at(3).target, descendant);
        for (const auto &crumb : trail)
            QVERIFY(!crumb.label.contains(QStringLiteral(".local/share")));
    }

    void offlineGoogleDrivePathIsRejectedBeforeHistoryOrFileDispatch()
    {
        Browser browser;
        auto &nav = browser.navigation;
        const QUrl initial = QUrl::fromLocalFile(QStringLiteral("/tmp"));
        QVERIFY(nav.initialize(initial));
        QSignalSpy errors(&nav, &BrowserNavigation::errorOccurred);
        QSignalSpy locations(&nav, &BrowserNavigation::locationChanged);
        QSignalSpy files(&nav, &BrowserNavigation::typedFileActivated);
        const QString mount = Locations::googleDriveMount().toLocalFile();

        QVERIFY(!nav.navigateTo(QUrl::fromLocalFile(mount + QStringLiteral("/Projects"))));
        QCOMPARE(nav.currentUrl(), initial);
        QCOMPARE(errors.count(), 1);
        QCOMPARE(errors.last().first().toString(),
                 QStringLiteral("Google Drive is not connected. Connect it before opening this folder."));
        QCOMPARE(locations.count(), 0);

        const QString recentBefore = Settings::recentPaths().join(QLatin1Char('\n'));
        nav.focusPathEdit();
        nav.pathEditor()->setText(mount + QStringLiteral("/missing.txt"));
        nav.submitPathEdit();
        QCOMPARE(files.count(), 0);
        QCOMPARE(nav.currentUrl(), initial);
        QCOMPARE(errors.count(), 2);
        QCOMPARE(locations.count(), 0);
        QCOMPARE(Settings::recentPaths().join(QLatin1Char('\n')), recentBefore);
    }

    void workerRedirectReplacesHistoryAndIgnoresStaleResults()
    {
        Browser browser;
        auto &nav = browser.navigation;
        const QUrl start = Locations::computer();
        const QUrl setup(QStringLiteral("gdrive:/new-account"));
        const QUrl account(QStringLiteral("gdrive:/person@example.com/Reports?id=fixture"));
        QVERIFY(nav.initialize(start));
        QVERIFY(nav.navigateTo(setup));
        QSignalSpy locations(&nav, &BrowserNavigation::locationChanged);
        QSignalSpy sounds(&nav, &BrowserNavigation::navigated);
        QVERIFY(nav.redirectTo(setup, account));
        QCOMPARE(nav.currentUrl(), account);
        QCOMPARE(locations.count(), 1);
        QCOMPARE(sounds.count(), 0);
        QVERIFY(!nav.redirectTo(setup, QUrl(QStringLiteral("gdrive:/different@example.com"))));
        nav.goBack();
        QCOMPARE(nav.currentUrl(), start);
        nav.goForward();
        QCOMPARE(nav.currentUrl(), account);
        nav.setUrlPolicy([](const QUrl &url) {
            return url.scheme() == QLatin1String("gdrive") ? QString() : QStringLiteral("Rejected");
        });
        QVERIFY(!nav.redirectTo(account, QUrl(QStringLiteral("sftp://example.invalid/"))));
        QCOMPARE(nav.currentUrl(), account);
    }

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

    void explicitAddressSubmissionUsesSharedParserAndPolicy()
    {
        Browser browser;
        auto &nav = browser.navigation;
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QVERIFY(QDir(directory.path()).mkdir(QStringLiteral("sub")));
        const QString filePath = directory.filePath(QStringLiteral("a # %.txt"));
        QFile file(filePath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        QVERIFY(nav.initialize(QUrl::fromLocalFile(directory.path())));
        QSignalSpy files(&nav, &BrowserNavigation::typedFileActivated);
        QSignalSpy errors(&nav, &BrowserNavigation::errorOccurred);
        nav.submitPathEdit(); // No active editor is a no-op.
        nav.focusPathEdit();
        nav.pathEditor()->setText(directory.filePath(QStringLiteral("sub")));
        nav.submitPathEdit();
        QCOMPARE(nav.currentUrl(), QUrl::fromLocalFile(directory.filePath(QStringLiteral("sub"))));
        QVERIFY(!nav.pathEditor());
        nav.focusPathEdit();
        nav.pathEditor()->setText(QString::fromUtf8(QUrl::fromLocalFile(filePath).toEncoded()));
        nav.submitPathEdit();
        QCOMPARE(files.count(), 1);
        QCOMPARE(files.first().first().toUrl(), QUrl::fromLocalFile(filePath));
        QVERIFY(!nav.pathEditor());
        nav.setUrlPolicy([](const QUrl &) { return QStringLiteral("denied"); });
        nav.focusPathEdit();
        nav.pathEditor()->setText(directory.path());
        nav.submitPathEdit();
        QCOMPARE(errors.count(), 1);
        QCOMPARE(files.count(), 1);
        QVERIFY(!nav.pathEditor());
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
