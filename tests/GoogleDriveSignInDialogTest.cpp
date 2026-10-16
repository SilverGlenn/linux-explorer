#include "../src/data/GoogleDriveConnection.h"
#include "../src/model/Locations.h"
#include "../src/ui/GoogleDriveSignInDialog.h"

#include <KFilePlacesModel>

#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QIcon>
#include <QLabel>
#include <QPushButton>
#include <QTest>

namespace {

class FakeGoogleDriveConnection : public GoogleDriveConnection {
public:
    using GoogleDriveConnection::GoogleDriveConnection;

    bool available = true;
    bool connectedState = false;
    int connectCalls = 0;
    int retryCalls = 0;
    int cancelCalls = 0;
    int disconnectCalls = 0;
    int signOutCalls = 0;
    QString reason;

    bool isAvailable() const override { return available; }
    QString unavailableReason() const override { return reason; }
    bool isConnected() const override { return connectedState; }
    void connectDrive() override { ++connectCalls; }
    void cancel() override { ++cancelCalls; }
    void disconnectDrive() override { ++disconnectCalls; }
    void signOut() override { ++signOutCalls; }
    void signInAgain() override { ++retryCalls; }
};

QPushButton *button(GoogleDriveSignInDialog &dialog, const char *name)
{
    return dialog.findChild<QPushButton *>(QLatin1String(name));
}

QLabel *status(GoogleDriveSignInDialog &dialog)
{
    return dialog.findChild<QLabel *>(QStringLiteral("googleDriveSignInStatus"));
}

QCheckBox *networkCheck(GoogleDriveSignInDialog &dialog)
{
    return dialog.findChild<QCheckBox *>(QStringLiteral("googleDriveShowInNetwork"));
}

int placeCount(KFilePlacesModel &places, const QUrl &url)
{
    int count = 0;
    for (int row = 0; row < places.rowCount(); ++row)
        count += places.url(places.index(row, 0)).matches(url, QUrl::StripTrailingSlash);
    return count;
}

void waitForPlacesInitialized(KFilePlacesModel &places)
{
    QTRY_VERIFY(places.rowCount() >= 5);
    // KIO may still add ordinary locations asynchronously. Assertions below
    // count only the Drive URL, so their arrival cannot affect the result.
    QCoreApplication::processEvents();
}

void showDialog(GoogleDriveSignInDialog &dialog)
{
    dialog.show();
    dialog.activateWindow();
    QCoreApplication::processEvents();
}

} // namespace

class GoogleDriveSignInDialogTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void startsIdleAndUsesWindowModalAccessibility()
    {
        QWidget parent;
        FakeGoogleDriveConnection connection;
        GoogleDriveSignInDialog dialog(nullptr, &parent, &connection);
        showDialog(dialog);

        QCOMPARE(connection.connectCalls, 0);
        QCOMPARE(connection.retryCalls, 0);
        QCOMPARE(connection.cancelCalls, 0);
        QCOMPARE(connection.signOutCalls, 0);
        QCOMPARE(dialog.windowModality(), Qt::WindowModal);
        QVERIFY(button(dialog, "googleDriveSignIn")->isVisible());
        QVERIFY(button(dialog, "googleDriveSignIn")->isEnabled());
        QVERIFY(button(dialog, "googleDriveSignInCancel")->isVisible());
        QVERIFY(button(dialog, "googleDriveSignOut")->isVisible());
        QVERIFY(button(dialog, "googleDriveSignOut")->isEnabled());
        QVERIFY(networkCheck(dialog)->isChecked());
        QCOMPARE(button(dialog, "googleDriveSignIn")->accessibleName(),
                 QStringLiteral("Sign in with Google"));
        QVERIFY(!status(dialog)->accessibleDescription().isEmpty());
    }

    void officialButtonArtworkAndAltGShortcut()
    {
        FakeGoogleDriveConnection connection;
        GoogleDriveSignInDialog dialog(nullptr, nullptr, &connection);
        showDialog(dialog);
        QPushButton *signIn = button(dialog, "googleDriveSignIn");

        QVERIFY(!signIn->icon().isNull());
        QVERIFY(!signIn->icon().pixmap(QSize(180, 40)).isNull());
        QCOMPARE(signIn->iconSize(), QSize(180, 40));
        QCOMPARE(signIn->size(), QSize(184, 44));
        QCOMPARE(signIn->text(), QString());
        QVERIFY(signIn->isDefault());
        QCOMPARE(signIn->shortcut(), QKeySequence(Qt::ALT | Qt::Key_G));
        QCOMPARE(signIn->accessibleName(), QStringLiteral("Sign in with Google"));

        QVERIFY(QTest::qWaitForWindowActive(&dialog));
        signIn->setFocus(Qt::ShortcutFocusReason);
        QTRY_VERIFY(signIn->hasFocus());
        QTest::keyClick(signIn, Qt::Key_G, Qt::AltModifier);
        QTRY_COMPARE(connection.connectCalls, 1);
        QVERIFY(!signIn->isEnabled());
        dialog.reject();
    }

    void unavailableBuildExplainsAndDoesNotStartWork()
    {
        FakeGoogleDriveConnection connection;
        connection.available = false;
        connection.reason = QStringLiteral(
            "Google Drive sign-in is unavailable in this build. Contact the application distributor.");
        GoogleDriveSignInDialog dialog(nullptr, nullptr, &connection);
        showDialog(dialog);

        QVERIFY(!button(dialog, "googleDriveSignIn")->isEnabled());
        QVERIFY(button(dialog, "googleDriveSignInCancel")->isEnabled());
        QCOMPARE(status(dialog)->text(), connection.reason);
        QCOMPARE(connection.connectCalls + connection.retryCalls, 0);
    }

    void explicitSignInConnectsAndOpenReturnsVerifiedMount()
    {
        FakeGoogleDriveConnection connection;
        GoogleDriveSignInDialog dialog(nullptr, nullptr, &connection);
        showDialog(dialog);

        button(dialog, "googleDriveSignIn")->click();
        QCOMPARE(connection.connectCalls, 1);
        QVERIFY(!button(dialog, "googleDriveSignIn")->isEnabled());
        QCOMPARE(status(dialog)->text(), QStringLiteral("Connecting to Google Drive…"));

        const QUrl mount = Locations::googleDriveMount();
        QVERIFY(mount.isLocalFile());
        Q_EMIT connection.connected(mount);
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
        QCOMPARE(dialog.mappedUrl(), mount);
    }

    void checkedBookmarkIsOnlyAttemptedAfterVerifiedMount()
    {
        KFilePlacesModel places;
        waitForPlacesInitialized(places);
        FakeGoogleDriveConnection connection;
        GoogleDriveSignInDialog dialog(&places, nullptr, &connection);
        showDialog(dialog);
        button(dialog, "googleDriveSignIn")->click();

        // The helper checks the live read-only mount metadata, so a fabricated
        // signal URL cannot create a Network shortcut in an offline test.
        Q_EMIT connection.connected(Locations::googleDriveMount());
        QCOMPARE(dialog.result(), 0);
        QVERIFY(status(dialog)->text().contains(QStringLiteral("could not be added to Network")));
        QVERIFY(button(dialog, "googleDriveOpen")->isEnabled());
        QCOMPARE(placeCount(places, Locations::googleDriveMount()), 0);
    }

    void uncheckedBookmarkLeavesOpenDriveAvailable()
    {
        FakeGoogleDriveConnection connection;
        GoogleDriveSignInDialog dialog(nullptr, nullptr, &connection);
        showDialog(dialog);
        networkCheck(dialog)->setChecked(false);
        button(dialog, "googleDriveSignIn")->click();
        const QUrl mount = Locations::googleDriveMount();
        Q_EMIT connection.connected(mount);

        QCOMPARE(dialog.result(), int(QDialog::Accepted));
        QCOMPARE(dialog.mappedUrl(), mount);
        QCOMPARE(connection.cancelCalls, 0);
    }

    void retryAndConnectionActionsCallTheirOwnOperations()
    {
        FakeGoogleDriveConnection connection;
        GoogleDriveSignInDialog dialog(nullptr, nullptr, &connection);
        showDialog(dialog);
        button(dialog, "googleDriveSignIn")->click();
        Q_EMIT connection.failed(QStringLiteral("Access was revoked."));
        QCOMPARE(status(dialog)->text(), QStringLiteral("Access was revoked."));
        QVERIFY(button(dialog, "googleDriveSignInAgain")->isVisible());
        button(dialog, "googleDriveSignInAgain")->click();
        QCOMPARE(connection.retryCalls, 1);
        QVERIFY(!button(dialog, "googleDriveSignInAgain")->isEnabled());
    }

    void connectedActionsStayBusyUntilTheirResults()
    {
        FakeGoogleDriveConnection connection;
        connection.connectedState = true;
        GoogleDriveSignInDialog dialog(nullptr, nullptr, &connection);
        showDialog(dialog);

        QVERIFY(button(dialog, "googleDriveOpen")->isVisible());
        button(dialog, "googleDriveDisconnect")->click();
        QCOMPARE(connection.disconnectCalls, 1);
        QVERIFY(!button(dialog, "googleDriveOpen")->isEnabled());
        QVERIFY(!button(dialog, "googleDriveDisconnect")->isEnabled());
        QVERIFY(!button(dialog, "googleDriveSignOut")->isEnabled());
        Q_EMIT connection.disconnected();
        QVERIFY(button(dialog, "googleDriveSignIn")->isEnabled());

        FakeGoogleDriveConnection signingOut;
        signingOut.connectedState = true;
        GoogleDriveSignInDialog signOutDialog(nullptr, nullptr, &signingOut);
        showDialog(signOutDialog);
        button(signOutDialog, "googleDriveSignOut")->click();
        QCOMPARE(signingOut.signOutCalls, 1);
        QVERIFY(!button(signOutDialog, "googleDriveOpen")->isEnabled());
        Q_EMIT signingOut.disconnected();
        Q_EMIT signingOut.signedOut();
        Q_EMIT signingOut.failed(QStringLiteral("Google account revocation could not be confirmed."));
        QCOMPARE(status(signOutDialog)->text(),
                 QStringLiteral("Google account revocation could not be confirmed."));
        signOutDialog.reject();
        QCOMPARE(signingOut.cancelCalls, 0);

        FakeGoogleDriveConnection closingWhileSigningOut;
        closingWhileSigningOut.connectedState = true;
        GoogleDriveSignInDialog closingDialog(nullptr, nullptr, &closingWhileSigningOut);
        showDialog(closingDialog);
        button(closingDialog, "googleDriveSignOut")->click();
        closingDialog.reject();
        QCOMPARE(closingWhileSigningOut.cancelCalls, 0);
    }

    void offlineDisconnectedCanBeSignedOut()
    {
        FakeGoogleDriveConnection connection;
        GoogleDriveSignInDialog dialog(nullptr, nullptr, &connection);
        showDialog(dialog);

        QVERIFY(button(dialog, "googleDriveSignOut")->isVisible());
        QVERIFY(button(dialog, "googleDriveSignOut")->isEnabled());
        QVERIFY(!button(dialog, "googleDriveOpen")->isVisible());
        QVERIFY(!button(dialog, "googleDriveDisconnect")->isVisible());

        Q_EMIT connection.failed(QStringLiteral("The Drive mount is unavailable."));
        QVERIFY(button(dialog, "googleDriveSignOut")->isVisible());
        QVERIFY(button(dialog, "googleDriveSignInAgain")->isVisible());

        button(dialog, "googleDriveSignOut")->click();
        QCOMPARE(connection.signOutCalls, 1);
        QVERIFY(!button(dialog, "googleDriveSignOut")->isEnabled());
        QVERIFY(!button(dialog, "googleDriveSignInAgain")->isEnabled());
        Q_EMIT connection.signedOut();
        QVERIFY(button(dialog, "googleDriveSignOut")->isVisible());
        QVERIFY(button(dialog, "googleDriveSignOut")->isEnabled());
        QVERIFY(button(dialog, "googleDriveSignIn")->isEnabled());

        FakeGoogleDriveConnection fresh;
        GoogleDriveSignInDialog newDialog(nullptr, nullptr, &fresh);
        showDialog(newDialog);
        QVERIFY(button(newDialog, "googleDriveSignOut")->isVisible());
        QVERIFY(button(newDialog, "googleDriveSignOut")->isEnabled());
        QCOMPARE(fresh.connectCalls + fresh.retryCalls + fresh.signOutCalls
                     + fresh.cancelCalls,
                 0);
    }

    void cancelOwnsAttemptAndIgnoresLateSuccess()
    {
        KFilePlacesModel places;
        waitForPlacesInitialized(places);
        FakeGoogleDriveConnection connection;
        GoogleDriveSignInDialog dialog(&places, nullptr, &connection);
        showDialog(dialog);
        button(dialog, "googleDriveSignIn")->click();
        dialog.reject();
        QCOMPARE(connection.cancelCalls, 1);
        Q_EMIT connection.connected(Locations::googleDriveMount());
        QCOMPARE(dialog.result(), int(QDialog::Rejected));
        QVERIFY(dialog.mappedUrl().isEmpty());
        QCOMPARE(placeCount(places, Locations::googleDriveMount()), 0);
    }

    void longPlainTextErrorsWrapAtCommonScaleFactors()
    {
        FakeGoogleDriveConnection connection;
        GoogleDriveSignInDialog dialog(nullptr, nullptr, &connection);
        showDialog(dialog);
        const QString message = QStringLiteral("<b>Google Drive access failed</b>. ")
            + QStringLiteral("This detailed error explains that the saved account grant was revoked. ").repeated(8);

        for (const qreal scale : {1.0, 1.25, 1.5}) {
            QFont font = status(dialog)->font();
            font.setPointSizeF(9.0 * scale);
            status(dialog)->setFont(font);
            Q_EMIT connection.failed(message);
            QCoreApplication::processEvents();
            QCOMPARE(status(dialog)->textFormat(), Qt::PlainText);
            QVERIFY(status(dialog)->wordWrap());
            QCOMPARE(status(dialog)->text(), message);
            QVERIFY(status(dialog)->height() >= status(dialog)->heightForWidth(status(dialog)->width()));
            QVERIFY(status(dialog)->parentWidget()->rect().contains(status(dialog)->geometry()));
            QVERIFY(dialog.height() < 1000);
        }
    }

    void snapshotsCommonStatesWhenRequested()
    {
        const QString output = qEnvironmentVariable("EXPLORER_SNAPSHOTS");
        if (output.isEmpty())
            QSKIP("Set EXPLORER_SNAPSHOTS for Google Drive sign-in snapshots");
        const QString folder = QDir(output).filePath(QStringLiteral("google-drive-sign-in"));
        QVERIFY(QDir().mkpath(folder));

        auto save = [&folder](GoogleDriveSignInDialog &dialog, const QString &name) {
            return dialog.grab().save(QDir(folder).filePath(name + QStringLiteral(".png")));
        };

        FakeGoogleDriveConnection initialConnection;
        GoogleDriveSignInDialog initial(nullptr, nullptr, &initialConnection);
        showDialog(initial);
        QVERIFY(save(initial, QStringLiteral("initial")));
        const int scalePercent = qRound(initial.devicePixelRatioF() * 100.0);
        QVERIFY(save(initial, QStringLiteral("sign-in-button-%1").arg(scalePercent)));
        button(initial, "googleDriveSignIn")->click();
        QVERIFY(save(initial, QStringLiteral("busy")));
        Q_EMIT initialConnection.failed(QStringLiteral("Google Drive access was denied. Try signing in again."));
        QVERIFY(save(initial, QStringLiteral("error")));

        FakeGoogleDriveConnection connectedConnection;
        connectedConnection.connectedState = true;
        GoogleDriveSignInDialog connectedDialog(nullptr, nullptr, &connectedConnection);
        showDialog(connectedDialog);
        QVERIFY(save(connectedDialog, QStringLiteral("connected")));

        FakeGoogleDriveConnection unavailableConnection;
        unavailableConnection.available = false;
        unavailableConnection.reason = QStringLiteral(
            "Google Drive sign-in is unavailable in this build. Contact the application distributor.");
        GoogleDriveSignInDialog unavailable(nullptr, nullptr, &unavailableConnection);
        showDialog(unavailable);
        QVERIFY(save(unavailable, QStringLiteral("missing")));
    }
};

QTEST_MAIN(GoogleDriveSignInDialogTest)
#include "GoogleDriveSignInDialogTest.moc"
