#include "GoogleDriveDialog.h"
#include "GoogleDriveSignInDialog.h"
#include "GoogleDriveService.h"
#include "Locations.h"
#include "MainWindow.h"
#include "NavigationPane.h"
#include "Settings.h"

#include <KFilePlacesModel>
#include <KIO/Global>
#include <QAccessible>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QPointer>
#include <QPushButton>
#include <QProcess>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QUuid>
#include <sys/stat.h>

namespace {
class FakeDriveService : public GoogleDriveService {
public:
    bool available = true;
    int loads = 0;
    int additions = 0;
    int verifications = 0;
    int cancellations = 0;
    QUrl verified;

    bool isAvailable() const override { return available; }
    void loadAccounts() override { ++loads; }
    void addAccount() override { ++additions; }
    void verifyAccount(const QUrl &url) override { ++verifications; verified = url; }
    void cancel() override { ++cancellations; }
};

// Uses the real job ownership code, but deletion occurs before the event loop
// can dispatch a worker. This is a lifecycle check, not a live Drive test.
class AvailableDriveService : public GoogleDriveService {
public:
    bool isAvailable() const override { return true; }
};

QUrl uniqueAccount()
{
    return GoogleDriveService::accountUrl(
        QStringLiteral("test-") + QUuid::createUuid().toString(QUuid::WithoutBraces));
}

KIO::UDSEntry entry(const QString &name, mode_t type = S_IFDIR,
                    const QString &foreignUrl = {})
{
    KIO::UDSEntry value;
    value.fastInsert(KIO::UDSEntry::UDS_NAME, name);
    value.fastInsert(KIO::UDSEntry::UDS_FILE_TYPE, type);
    value.fastInsert(KIO::UDSEntry::UDS_DISPLAY_NAME, QStringLiteral("Untrusted display label"));
    if (!foreignUrl.isEmpty())
        value.fastInsert(KIO::UDSEntry::UDS_URL, foreignUrl);
    return value;
}

QModelIndex placeFor(KFilePlacesModel &places, const QUrl &url)
{
    for (int row = 0; row < places.rowCount(); ++row) {
        const QModelIndex index = places.index(row, 0);
        if (places.url(index).matches(url, QUrl::StripTrailingSlash))
            return index;
    }
    return {};
}

int placeCount(KFilePlacesModel &places, const QUrl &url)
{
    int count = 0;
    for (int row = 0; row < places.rowCount(); ++row)
        count += places.url(places.index(row, 0)).matches(url, QUrl::StripTrailingSlash);
    return count;
}

QPushButton *button(GoogleDriveDialog &dialog, const char *name)
{
    return dialog.findChild<QPushButton *>(QLatin1String(name));
}

QComboBox *picker(GoogleDriveDialog &dialog)
{
    return dialog.findChild<QComboBox *>();
}

void showDialog(GoogleDriveDialog &dialog)
{
    dialog.show();
    dialog.activateWindow();
    QCoreApplication::processEvents();
}

void discover(GoogleDriveDialog &dialog, FakeDriveService &service,
              const QList<GoogleDriveAccount> &accounts)
{
    button(dialog, "googleDriveConnect")->click();
    Q_EMIT service.accountsReady(accounts, {});
}
}

class GoogleDriveTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void managedDriveEntryPointsDoNotTranslateProviderFileIds()
    {
        for (const QString &path : {QStringLiteral("gdrive:/"),
                 QStringLiteral("gdrive:"), QStringLiteral("gdrive:/google2"),
                 QStringLiteral("gdrive:/person@example.com/")}) {
            QCOMPARE(GoogleDriveService::managedDriveLocation(QUrl(path)),
                     Locations::googleDriveMount());
        }
        for (const QString &path : {QStringLiteral("gdrive:/new-account"),
                 QStringLiteral("gdrive:/google2/Reports"),
                 QStringLiteral("gdrive:/google2?id=opaque"),
                 QStringLiteral("gdrive:/google2#fragment"),
                 QStringLiteral("gdrive://foreign/google2"),
                 QStringLiteral("gdrive:/google2/../other"),
                 QStringLiteral("gdrive:/google2%2Ffolder"),
                 QStringLiteral("sftp://example.invalid/google2"),
                 QStringLiteral("file:///tmp/google2")}) {
            const QUrl url(path);
            QCOMPARE(GoogleDriveService::managedDriveLocation(url), url);
        }
    }

    void initTestCase()
    {
        qRegisterMetaType<QList<GoogleDriveAccount>>();
        Settings::setNotifyUnmountedDrives(false);
    }

    void accountUrlsRejectUnsafeLocations_data()
    {
        QTest::addColumn<QString>("location");
        for (const QString &location : {
                 QString(), QStringLiteral("gdrive:/"), QStringLiteral("gdrive:"),
                 QStringLiteral("file:/alice"), QStringLiteral("https://drive.google.com/alice"),
                 QStringLiteral("gdrive://host/alice"), QStringLiteral("gdrive://user:secret@host/alice"),
                 QStringLiteral("gdrive://user@/alice"), QStringLiteral("gdrive:/alice?token=secret"),
                 QStringLiteral("gdrive:/alice?"), QStringLiteral("gdrive:/alice#fragment"),
                 QStringLiteral("gdrive:/alice#"), QStringLiteral("gdrive:/new-account"),
                 QStringLiteral("gdrive:/new-account/"), QStringLiteral("gdrive:/%6Eew-account"),
                 QStringLiteral("gdrive:/alice/folder"), QStringLiteral("gdrive:alice"),
                 QStringLiteral("gdrive:/./"), QStringLiteral("gdrive:/.."),
                 QStringLiteral("gdrive:/alice/../bob"), QStringLiteral("gdrive:/%2E%2E"),
                 QStringLiteral("gdrive:/alice%2Fbob"), QStringLiteral("gdrive:/alice%5Cbob"),
                 QStringLiteral("gdrive:/alice%00"), QStringLiteral("gdrive:/alice%0A"),
                 QStringLiteral("gdrive:/alice%7F")})
            QTest::newRow(location.toUtf8().constData()) << location;
    }

    void accountUrlsRejectUnsafeLocations()
    {
        QFETCH(QString, location);
        QVERIFY(!GoogleDriveService::isAccountUrl(QUrl(location, QUrl::StrictMode)));
    }

    void accountNamesAndSpecialCharacters()
    {
        for (const QString &name : {QString(), QStringLiteral("."), QStringLiteral(".."),
                                   QStringLiteral("new-account"), QStringLiteral("a/b"),
                                   QStringLiteral("a\\b"), QStringLiteral("a\n"),
                                   QString(QChar::Null), QString(257, QLatin1Char('a'))})
            QVERIFY2(!GoogleDriveService::accountUrl(name).isValid(), qPrintable(name));

        const QString name = QString::fromUtf8("Zoë 日本 + % ? # @ : account");
        const QUrl url = GoogleDriveService::accountUrl(name);
        QVERIFY(GoogleDriveService::isAccountUrl(url));
        QCOMPARE(url.path(), QLatin1Char('/') + name);
        QVERIFY(url.authority().isEmpty());
        QVERIFY(!url.hasQuery());
        QVERIFY(!url.hasFragment());
        QCOMPARE(QUrl::fromEncoded(url.toEncoded()).path(), url.path());
        QVERIFY(url.toEncoded().contains("%3F"));
        QVERIFY(url.toEncoded().contains("%23"));
        QUrl trailing = url;
        trailing.setPath(url.path() + QLatin1Char('/'));
        QVERIFY(GoogleDriveService::isAccountUrl(trailing));
        QVERIFY(GoogleDriveService::isAccountUrl(GoogleDriveService::accountUrl(
            QString(256, QLatin1Char('a')))));
    }

    void listingFiltersSyntheticEntriesAndNeverTrustsForeignUrls()
    {
        const KIO::UDSEntryList entries = {
            entry(QStringLiteral(".")), entry(QStringLiteral("..")),
            entry(QStringLiteral("new-account")), entry(QStringLiteral("alice")),
            entry(QStringLiteral("alice")), entry(QStringLiteral("ordinary-file"), S_IFREG),
            entry(QStringLiteral("bob"), S_IFDIR, QStringLiteral("https://attacker.invalid/")),
            entry(QStringLiteral("a/b")), entry(QStringLiteral("bad\nname"))};
        const auto accounts = GoogleDriveService::accountsForListing(QUrl(QStringLiteral("gdrive:/")), entries);
        QCOMPARE(accounts.size(), 2);
        QCOMPARE(accounts.at(0).name, QStringLiteral("alice"));
        QCOMPARE(accounts.at(0).url, GoogleDriveService::accountUrl(QStringLiteral("alice")));
        QCOMPARE(accounts.at(1).name, QStringLiteral("bob"));
        QCOMPARE(accounts.at(1).url, GoogleDriveService::accountUrl(QStringLiteral("bob")));
        QVERIFY(GoogleDriveService::accountsForListing(QUrl(QStringLiteral("https://host/")), entries).isEmpty());
        QVERIFY(GoogleDriveService::accountsForListing(QUrl(QStringLiteral("gdrive://host/")), entries).isEmpty());
        QVERIFY(GoogleDriveService::accountsForListing(QUrl(QStringLiteral("gdrive:/new-account")), entries).isEmpty());
        QVERIFY(GoogleDriveService::accountsForListing(QUrl(QStringLiteral("gdrive:/alice/folder")), entries).isEmpty());
    }

    void redirectedListingRepresentsTheAccountRatherThanItsFiles()
    {
        const auto accounts = GoogleDriveService::accountsForListing(
            QUrl(QStringLiteral("gdrive:/alice/")),
            {entry(QStringLiteral("Photos")), entry(QStringLiteral("bob")),
             entry(QStringLiteral("notes.txt"), S_IFREG)});
        QCOMPARE(accounts.size(), 1);
        QCOMPARE(accounts.first().name, QStringLiteral("alice"));
        QCOMPARE(accounts.first().url, GoogleDriveService::accountUrl(QStringLiteral("alice")));
    }

    void rememberAccountDeduplicatesUnhidesAndPreservesUserLabel()
    {
        KFilePlacesModel places;
        const QUrl url = uniqueAccount();
        places.addPlace(QStringLiteral("My work storage"), url, QStringLiteral("folder-blue"));
        QTRY_VERIFY(placeFor(places, url).isValid());
        const QString originalIcon = places.icon(placeFor(places, url)).name();
        places.setPlaceHidden(placeFor(places, url), true);
        QTRY_VERIFY(places.isHidden(placeFor(places, url)));
        QUrl trailing = url;
        trailing.setPath(url.path() + QLatin1Char('/'));
        QVERIFY(GoogleDriveService::rememberAccount(&places, trailing, QStringLiteral("Replacement")));
        QTRY_VERIFY(!places.isHidden(placeFor(places, url)));
        QCOMPARE(placeCount(places, url), 1);
        QCOMPARE(places.text(placeFor(places, url)), QStringLiteral("My work storage"));
        QCOMPARE(places.icon(placeFor(places, url)).name(), originalIcon);

        const QUrl second = uniqueAccount();
        QVERIFY(GoogleDriveService::rememberAccount(&places, second, QStringLiteral("Personal")));
        QTRY_COMPARE(placeCount(places, second), 1);
        QVERIFY(GoogleDriveService::rememberAccount(&places, second, QStringLiteral("Changed")));
        QCOMPARE(placeCount(places, second), 1);
        QCOMPARE(places.text(placeFor(places, second)), QStringLiteral("Google Drive (Personal)"));
        const int rows = places.rowCount();
        QVERIFY(!GoogleDriveService::rememberAccount(&places, QUrl(QStringLiteral("gdrive:/new-account")), {}));
        QVERIFY(!GoogleDriveService::rememberAccount(&places, QUrl(QStringLiteral("gdrive://user:secret@host/a")), {}));
        QVERIFY(!GoogleDriveService::rememberAccount(nullptr, second, {}));
        QCOMPARE(places.rowCount(), rows);
    }

    void dialogStartsWithoutJobsAndUnavailableSupportIsExplained()
    {
        FakeDriveService service;
        GoogleDriveDialog dialog(nullptr, nullptr, &service);
        showDialog(dialog);
        QCOMPARE(service.loads + service.additions + service.verifications, 0);
        QVERIFY(button(dialog, "googleDriveConnect")->isEnabled());
        QVERIFY(!picker(dialog)->isEnabled());
        QVERIFY(button(dialog, "googleDriveAddAccount")->isVisible());
        QVERIFY(button(dialog, "googleDriveAddAccount")->isEnabled());
        QCOMPARE(dialog.findChild<QCheckBox *>()->isChecked(), true);
        dialog.reject();
        QCOMPARE(service.cancellations, 1);

        FakeDriveService missing;
        missing.available = false;
        GoogleDriveDialog unavailable(nullptr, nullptr, &missing);
        showDialog(unavailable);
        QVERIFY(!button(unavailable, "googleDriveConnect")->isEnabled());
        QVERIFY(!picker(unavailable)->isEnabled());
        QVERIFY(unavailable.findChild<QLabel *>(QStringLiteral("googleDriveStatus"))->text().contains(QStringLiteral("kio-gdrive")));
        QVERIFY(!button(unavailable, "googleDriveAddAccount")->isVisible());
        QCOMPARE(missing.loads + missing.additions + missing.verifications, 0);
        QVERIFY(button(unavailable, "googleDriveCancel")->isEnabled());
    }

    void authorizationErrorsExplainMissingPermissionWithoutSaving()
    {
        const QString message = GoogleDriveService::connectionError(
            KIO::ERR_ACCESS_DENIED, QStringLiteral("Access denied to ."));
        QVERIFY(message.contains(QStringLiteral("do not request Drive permission")));
        QVERIFY(!message.contains(QStringLiteral("Access denied to .")));
        QCOMPARE(GoogleDriveService::connectionError(KIO::ERR_CANNOT_CONNECT,
            QStringLiteral("Network unavailable")), QStringLiteral("Network unavailable"));

        KFilePlacesModel places;
        const QUrl url = uniqueAccount();
        FakeDriveService service;
        GoogleDriveDialog dialog(&places, nullptr, &service);
        QSignalSpy accepted(&dialog, &QDialog::accepted);
        showDialog(dialog);
        discover(dialog, service, {{QStringLiteral("Work"), url}});
        button(dialog, "googleDriveConnect")->click();
        Q_EMIT service.failed(message);
        QCOMPARE(accepted.count(), 0);
        QCOMPARE(placeCount(places, url), 0);
        QCOMPARE(dialog.findChild<QLabel *>(QStringLiteral("googleDriveStatus"))->text(), message);
        QVERIFY(button(dialog, "googleDriveCancel")->isEnabled());
    }

    void personalMountCannotBeMappedWhileDisconnected()
    {
        QVERIFY(!Locations::googleDriveMounted());
        KFilePlacesModel places;
        const QUrl mount = Locations::googleDriveMount();
        QVERIFY(!GoogleDriveService::rememberMountedDrive(&places));
        QCOMPARE(placeCount(places, mount), 0);

        QProcess client;
        client.start(QCoreApplication::applicationDirPath() + QStringLiteral("/explorer-win7"),
                     {QStringLiteral("--map-google-drive-mount")});
        QVERIFY(client.waitForFinished(10000));
        QCOMPARE(client.exitStatus(), QProcess::NormalExit);
        QCOMPARE(client.exitCode(), 1);
        QVERIFY(client.readAllStandardError().contains("No shortcut was added"));
        QCOMPARE(placeCount(places, mount), 0);
    }

    void addAccountIsAvailableImmediatelyWithoutDiscovery()
    {
        FakeDriveService service;
        GoogleDriveDialog dialog(nullptr, nullptr, &service);
        showDialog(dialog);
        QCOMPARE(service.loads + service.additions + service.verifications, 0);
        QVERIFY(button(dialog, "googleDriveAddAccount")->isVisible());
        button(dialog, "googleDriveAddAccount")->click();
        QCOMPARE(service.additions, 1);
        QCOMPARE(service.loads, 0);
        QVERIFY(!button(dialog, "googleDriveAddAccount")->isEnabled());

        const QUrl account = uniqueAccount();
        Q_EMIT service.accountsReady({{QStringLiteral("New account"), account}}, account);
        QCOMPARE(picker(dialog)->count(), 1);
        QCOMPARE(picker(dialog)->currentData().toUrl(), account);
        QVERIFY(button(dialog, "googleDriveConnect")->isEnabled());
        QCOMPARE(service.verifications, 0);
    }

    void dialogIsWindowModalAndLeavesOtherTopLevelUsable()
    {
        QWidget owner;
        QWidget other;
        QPushButton ownerButton(QStringLiteral("Owner"), &owner);
        QPushButton otherButton(QStringLiteral("Other"), &other);
        int ownerClicks = 0;
        int otherClicks = 0;
        connect(&ownerButton, &QPushButton::clicked, &owner, [&] { ++ownerClicks; });
        connect(&otherButton, &QPushButton::clicked, &other, [&] { ++otherClicks; });
        owner.resize(240, 120);
        other.resize(240, 120);
        ownerButton.setGeometry(20, 20, 100, 32);
        otherButton.setGeometry(20, 20, 100, 32);
        owner.show();
        other.show();
        QCoreApplication::processEvents();

        FakeDriveService service;
        GoogleDriveDialog dialog(nullptr, &owner, &service);
        QCOMPARE(dialog.parentWidget(), &owner);
        QCOMPARE(dialog.windowModality(), Qt::WindowModal);
        QTimer::singleShot(0, &dialog, [&] {
            QCOMPARE(QApplication::activeModalWidget(),
                     static_cast<QWidget *>(&dialog));
            QCOMPARE(dialog.windowModality(), Qt::WindowModal);
            QTest::mouseClick(&otherButton, Qt::LeftButton);
            QCOMPARE(otherClicks, 1);
            dialog.reject();
        });
        QCOMPARE(dialog.exec(), int(QDialog::Rejected));
        QCOMPARE(ownerClicks, 0);
        QCOMPARE(otherClicks, 1);
    }

    void connectionRequiresVerificationBeforeBookmarkAndAcceptance()
    {
        KFilePlacesModel places;
        const QUrl url = uniqueAccount();
        FakeDriveService service;
        GoogleDriveDialog dialog(&places, nullptr, &service);
        QSignalSpy accepted(&dialog, &QDialog::accepted);
        showDialog(dialog);
        discover(dialog, service, {{QStringLiteral("Work"), url}});
        QCOMPARE(service.loads, 1);
        QCOMPARE(QApplication::focusWidget(), picker(dialog));
        QVERIFY(button(dialog, "googleDriveConnect")->isDefault());
        QCOMPARE(picker(dialog)->count(), 1);
        QCOMPARE(service.verifications, 0);
        QCOMPARE(placeCount(places, url), 0);
        QCOMPARE(accepted.count(), 0);
        Q_EMIT service.connected(url); // An unsolicited result cannot accept.
        QCOMPARE(accepted.count(), 0);

        QTest::keyClick(picker(dialog), Qt::Key_Return);
        QCOMPARE(service.verifications, 1);
        QCOMPARE(service.verified, url);
        QVERIFY(!button(dialog, "googleDriveConnect")->isEnabled());
        QVERIFY(button(dialog, "googleDriveCancel")->isEnabled());
        QCOMPARE(placeCount(places, url), 0);
        Q_EMIT service.connected(url);
        QCOMPARE(accepted.count(), 1);
        QCOMPARE(dialog.mappedUrl(), url);
        QTRY_COMPARE(placeCount(places, url), 1);
        Q_EMIT service.connected(url);
        QCOMPARE(accepted.count(), 1);
        QCOMPARE(placeCount(places, url), 1);
    }

    void failedVerificationCanRetryWithoutSavingAndRejectsDifferentAccount()
    {
        KFilePlacesModel places;
        const QUrl url = uniqueAccount();
        FakeDriveService service;
        GoogleDriveDialog dialog(&places, nullptr, &service);
        QSignalSpy accepted(&dialog, &QDialog::accepted);
        showDialog(dialog);
        discover(dialog, service, {{QStringLiteral("Work"), url}});
        button(dialog, "googleDriveConnect")->click();
        Q_EMIT service.failed(QStringLiteral("Credentials were revoked."));
        QCOMPARE(accepted.count(), 0);
        QCOMPARE(placeCount(places, url), 0);
        QVERIFY(dialog.mappedUrl().isEmpty());
        QVERIFY(button(dialog, "googleDriveConnect")->isEnabled());
        QVERIFY(dialog.findChild<QLabel *>(QStringLiteral("googleDriveStatus"))->text().contains(QStringLiteral("revoked")));
        button(dialog, "googleDriveConnect")->click();
        Q_EMIT service.connected(uniqueAccount());
        QCOMPARE(accepted.count(), 0);
        QCOMPARE(placeCount(places, url), 0);
        QVERIFY(button(dialog, "googleDriveConnect")->isEnabled());
        button(dialog, "googleDriveConnect")->click();
        QCOMPARE(service.verifications, 3);
        Q_EMIT service.connected(url);
        QCOMPARE(accepted.count(), 1);
        QTRY_COMPARE(placeCount(places, url), 1);
    }

    void uncheckedBookmarkOpensWithoutPersisting()
    {
        KFilePlacesModel places;
        const QUrl url = uniqueAccount();
        FakeDriveService service;
        GoogleDriveDialog dialog(&places, nullptr, &service);
        QSignalSpy accepted(&dialog, &QDialog::accepted);
        showDialog(dialog);
        discover(dialog, service, {{QStringLiteral("Temporary"), url}});
        dialog.findChild<QCheckBox *>()->setChecked(false);
        button(dialog, "googleDriveConnect")->click();
        Q_EMIT service.connected(url);
        QCOMPARE(accepted.count(), 1);
        QCOMPARE(dialog.mappedUrl(), url);
        QCOMPARE(placeCount(places, url), 0);
    }

    void addAccountMergesAndSelectsSuggestedAccount()
    {
        const QUrl first = uniqueAccount();
        const QUrl second = uniqueAccount();
        FakeDriveService service;
        GoogleDriveDialog dialog(nullptr, nullptr, &service);
        showDialog(dialog);
        discover(dialog, service, {{QStringLiteral("Work"), first}});
        button(dialog, "googleDriveAddAccount")->click();
        QCOMPARE(service.additions, 1);
        QVERIFY(!picker(dialog)->isEnabled());
        Q_EMIT service.accountsReady({{QStringLiteral("Personal"), second}}, second);
        QCOMPARE(picker(dialog)->count(), 2);
        QCOMPARE(picker(dialog)->currentData().toUrl(), second);
        QCOMPARE(service.verifications, 0);
        button(dialog, "googleDriveAddAccount")->click();
        Q_EMIT service.accountsReady({{QStringLiteral("Personal updated"), second}}, second);
        QCOMPARE(picker(dialog)->count(), 2);
        QCOMPARE(picker(dialog)->currentText(), QStringLiteral("Personal updated"));
        button(dialog, "googleDriveReloadAccounts")->click();
        QCOMPARE(service.loads, 2);
        Q_EMIT service.accountsReady({{QStringLiteral("Personal"), second},
                                     {QStringLiteral("Unsafe"), QUrl(QStringLiteral("https://host/"))}}, {});
        QCOMPARE(picker(dialog)->count(), 1);
        QCOMPARE(picker(dialog)->currentData().toUrl(), second);
    }

    void emptyDiscoveryStillOffersAccountSetup()
    {
        FakeDriveService service;
        GoogleDriveDialog dialog(nullptr, nullptr, &service);
        showDialog(dialog);
        discover(dialog, service, {});
        QCOMPARE(service.loads, 1);
        QCOMPARE(picker(dialog)->count(), 0);
        QVERIFY(!button(dialog, "googleDriveConnect")->isEnabled());
        QVERIFY(button(dialog, "googleDriveAddAccount")->isVisible());
        QVERIFY(button(dialog, "googleDriveAddAccount")->isEnabled());
        QVERIFY(button(dialog, "googleDriveReloadAccounts")->isEnabled());
        button(dialog, "googleDriveAddAccount")->click();
        QCOMPARE(service.additions, 1);
        const QUrl account = uniqueAccount();
        Q_EMIT service.accountsReady({{QStringLiteral("New account"), account}}, account);
        QCOMPARE(picker(dialog)->currentData().toUrl(), account);
        QVERIFY(button(dialog, "googleDriveConnect")->isEnabled());
    }

    void cancellationIgnoresLateResults_data()
    {
        QTest::addColumn<bool>("verifying");
        QTest::newRow("discovery") << false;
        QTest::newRow("verification") << true;
    }

    void cancellationIgnoresLateResults()
    {
        QFETCH(bool, verifying);
        KFilePlacesModel places;
        const QUrl url = uniqueAccount();
        FakeDriveService service;
        GoogleDriveDialog dialog(&places, nullptr, &service);
        QSignalSpy accepted(&dialog, &QDialog::accepted);
        QSignalSpy rejected(&dialog, &QDialog::rejected);
        showDialog(dialog);
        button(dialog, "googleDriveConnect")->click();
        if (verifying) {
            Q_EMIT service.accountsReady({{QStringLiteral("Work"), url}}, {});
            button(dialog, "googleDriveConnect")->click();
        }
        QTest::keyClick(&dialog, Qt::Key_Escape);
        QCOMPARE(rejected.count(), 1);
        QCOMPARE(service.cancellations, 1);
        const QString previousStatus = dialog.findChild<QLabel *>(QStringLiteral("googleDriveStatus"))->text();
        Q_EMIT service.accountsReady({{QStringLiteral("Late"), url}}, url);
        Q_EMIT service.connected(url);
        Q_EMIT service.failed(QStringLiteral("Late failure"));
        QCOMPARE(accepted.count(), 0);
        QVERIFY(dialog.mappedUrl().isEmpty());
        QCOMPARE(placeCount(places, url), 0);
        QCOMPARE(dialog.findChild<QLabel *>(QStringLiteral("googleDriveStatus"))->text(), previousStatus);
    }

    void destructionCancelsPendingWork()
    {
        FakeDriveService fake;
        {
            GoogleDriveDialog dialog(nullptr, nullptr, &fake);
            button(dialog, "googleDriveConnect")->click();
            QCOMPARE(fake.loads, 1);
        }
        QCOMPARE(fake.cancellations, 1);

        auto *service = new AvailableDriveService;
        QSignalSpy ready(service, &GoogleDriveService::accountsReady);
        QSignalSpy connected(service, &GoogleDriveService::connected);
        QSignalSpy failed(service, &GoogleDriveService::failed);
        QPointer<GoogleDriveService> guard(service);
        service->loadAccounts();
        delete service;
        QVERIFY(guard.isNull());
        QCoreApplication::processEvents();
        QCOMPARE(ready.count() + connected.count() + failed.count(), 0);
    }

    void keyboardAccessibilityAndSizing()
    {
        FakeDriveService service;
        GoogleDriveDialog dialog(nullptr, nullptr, &service);
        showDialog(dialog);
        QVERIFY(!dialog.accessibleName().isEmpty());
        QVERIFY(button(dialog, "googleDriveConnect")->isDefault());
        QVERIFY(!picker(dialog)->accessibleName().isEmpty());
        auto *accessible = QAccessible::queryAccessibleInterface(button(dialog, "googleDriveConnect"));
        QVERIFY(accessible);
        QVERIFY(!accessible->text(QAccessible::Name).isEmpty());
        QTest::keyClick(button(dialog, "googleDriveConnect"), Qt::Key_Return);
        QCOMPARE(service.loads, 1);
        Q_EMIT service.accountsReady({{QStringLiteral("A long account name for layout checks"), uniqueAccount()}}, {});
        dialog.adjustSize();
        QCoreApplication::processEvents();
        for (auto *control : dialog.findChildren<QPushButton *>()) {
            if (!control->isVisible()) continue;
            QVERIFY(!control->accessibleName().isEmpty());
            const QRect bounds(control->mapTo(&dialog, QPoint()), control->size());
            QVERIFY2(dialog.rect().contains(bounds), qPrintable(control->objectName()));
        }
        picker(dialog)->setFocus();
        QTest::keyClick(picker(dialog), Qt::Key_Tab);
        QCOMPARE(QApplication::focusWidget(), button(dialog, "googleDriveConnect"));
    }

    void mainWindowActionsOpenCancelableDialogWithoutAuthentication()
    {
        MainWindow window(Locations::computer());
        window.setAttribute(Qt::WA_DeleteOnClose, false);
        window.show();
        QCoreApplication::processEvents();
        QAction *connectAction = nullptr;
        bool listedInTools = false;
        for (QAction *top : window.menuBar()->actions()) {
            QMenu *menu = top->menu();
            if (!menu || top->text().remove(QLatin1Char('&')) != QLatin1String("Tools")) continue;
            for (QAction *action : menu->actions()) {
                if (action->text().remove(QLatin1Char('&')) == QLatin1String("Connect Google Drive...")) {
                    connectAction = action;
                    listedInTools = true;
                }
            }
        }
        QVERIFY(listedInTools && connectAction && connectAction->isEnabled());
        QToolButton *computerButton = nullptr;
        for (auto *candidate : window.findChildren<QToolButton *>())
            if (candidate->text().remove(QLatin1Char('&')) == QLatin1String("Connect Google Drive"))
                computerButton = candidate;
        QVERIFY(computerButton && computerButton->isEnabled());
        bool sawDialog = false;
        bool sawInitialState = false;
        QTimer::singleShot(0, &window, [&] {
            auto *dialog = qobject_cast<GoogleDriveSignInDialog *>(QApplication::activeModalWidget());
            sawDialog = dialog;
            if (dialog) {
                sawInitialState = dialog->findChild<QPushButton *>(QStringLiteral("googleDriveSignIn"))
                    && !dialog->findChild<QComboBox *>();
                dialog->reject();
            }
        });
        connectAction->trigger();
        QVERIFY(sawDialog);
        QVERIFY(sawInitialState);
        QVERIFY(!window.findChild<GoogleDriveSignInDialog *>());
    }

    void networkContextMenuOffersGoogleDriveAction()
    {
        NavigationPane pane;
        pane.resize(250, 500);
        pane.show();
        auto *tree = pane.findChild<QTreeWidget *>();
        QVERIFY(tree);
        QCoreApplication::processEvents();
        QTreeWidgetItem *network = nullptr;
        for (int row = 0; row < tree->topLevelItemCount(); ++row) {
            auto *item = tree->topLevelItem(row);
            if (item->data(0, Qt::UserRole).toUrl() == QUrl(QStringLiteral("remote:/")))
                network = item;
        }
        QVERIFY(network);
        tree->scrollToItem(network);
        QSignalSpy requested(&pane, &NavigationPane::googleDriveRequested);
        bool found = false;
        QTimer::singleShot(0, &pane, [&] {
            for (auto *menu : pane.findChildren<QMenu *>()) {
                if (!menu->isVisible()) continue;
                for (QAction *action : menu->actions()) {
                    if (action->text() == QLatin1String("Connect Google Drive...")) {
                        found = action->isEnabled();
                        action->trigger();
                    }
                }
                menu->close();
            }
        });
        tree->customContextMenuRequested(tree->visualItemRect(network).center());
        QVERIFY(found);
        QCOMPARE(requested.count(), 1);
    }

    void renderSnapshots()
    {
        const QString output = qEnvironmentVariable("EXPLORER_SNAPSHOTS");
        if (output.isEmpty()) QSKIP("Set EXPLORER_SNAPSHOTS for Google Drive dialog captures");
        QVERIFY(QDir().mkpath(output));
        FakeDriveService service;
        GoogleDriveDialog dialog(nullptr, nullptr, &service);
        showDialog(dialog);
        QVERIFY(dialog.grab().save(output + QStringLiteral("/google-drive-initial.png")));
        const QUrl url = GoogleDriveService::accountUrl(QStringLiteral("account@example.com"));
        discover(dialog, service, {{QStringLiteral("account@example.com"), url}});
        dialog.adjustSize();
        QCoreApplication::processEvents();
        QVERIFY(dialog.grab().save(output + QStringLiteral("/google-drive-ready.png")));
        button(dialog, "googleDriveConnect")->click();
        QVERIFY(dialog.grab().save(output + QStringLiteral("/google-drive-busy.png")));
        Q_EMIT service.failed(QStringLiteral("Could not connect to Google Drive. Check your connection and try again."));
        auto *status = dialog.findChild<QLabel *>(QStringLiteral("googleDriveStatus"));
        QVERIFY(status);
        QVERIFY(status->height() >= status->heightForWidth(status->width()));
        QVERIFY(status->parentWidget()->rect().contains(status->geometry()));
        QVERIFY(dialog.grab().save(output + QStringLiteral("/google-drive-error.png")));
        Q_EMIT service.failed(GoogleDriveService::connectionError(KIO::ERR_ACCESS_DENIED, {}));
        QVERIFY(status->height() >= status->heightForWidth(status->width()));
        QVERIFY(status->parentWidget()->rect().contains(status->geometry()));
        QVERIFY(dialog.grab().save(output + QStringLiteral("/google-drive-access-denied.png")));
        dialog.reject();
        FakeDriveService missing;
        missing.available = false;
        GoogleDriveDialog unavailable(nullptr, nullptr, &missing);
        showDialog(unavailable);
        QVERIFY(unavailable.grab().save(output + QStringLiteral("/google-drive-missing.png")));
    }
};

QTEST_MAIN(GoogleDriveTest)
#include "GoogleDriveTest.moc"
