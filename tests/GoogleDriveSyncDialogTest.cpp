#include "../src/data/GoogleDriveSyncController.h"
#include "../src/ui/GoogleDriveSyncDialog.h"
#include "../src/ui/ApplicationAppearance.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QDir>
#include <QFile>
#include <QApplication>
#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QTreeWidget>

namespace {

class FakeGoogleDriveSyncController : public GoogleDriveSyncController
{
public:
    using GoogleDriveSyncController::GoogleDriveSyncController;

    bool available = true;
    bool busy = false;
    bool writePermission = false;
    bool savedPermission = false;
    int refreshCalls = 0;
    int permissionCalls = 0;
    int signInAgainCalls = 0;
    int listCalls = 0;
    int previewCalls = 0;
    int addCalls = 0;
    int pauseCalls = 0;
    int resumeCalls = 0;
    int syncNowCalls = 0;
    int removeCalls = 0;
    int reviewCalls = 0;
    int resumeReviewedCalls = 0;
    int cancelSetupCalls = 0;
    QString lastParentId;
    QString lastRemoteId;
    QString lastLocalPath;
    QString lastPairId;
    QString lastReviewToken;
    QString message;
    QJsonArray pairList;
    QJsonObject reviewDocument;

    bool isAvailable() const override { return available; }
    bool isBusy() const override { return busy; }
    bool workerRunning() const override { return writePermission; }
    bool hasWritePermission() const override { return writePermission; }
    bool hasSavedPermission() const override { return savedPermission || writePermission; }
    QString statusMessage() const override { return message; }
    QJsonArray pairs() const override { return pairList; }

    void refresh() override { ++refreshCalls; }
    void requestWritePermission() override { ++permissionCalls; }
    void signInAgain() override { ++signInAgainCalls; }
    void listFolders(const QString &parentId = QStringLiteral("root")) override
    {
        ++listCalls;
        lastParentId = parentId;
    }
    void previewPair(const QString &remoteFolderId, const QString &localPath) override
    {
        ++previewCalls;
        lastRemoteId = remoteFolderId;
        lastLocalPath = localPath;
    }
    void addPair(const QString &remoteFolderId, const QString &localPath) override
    {
        ++addCalls;
        lastRemoteId = remoteFolderId;
        lastLocalPath = localPath;
        pairList.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("pair-1")},
                                   {QStringLiteral("remote_folder_id"), remoteFolderId},
                                   {QStringLiteral("remote_name"), QStringLiteral("Project")},
                                   {QStringLiteral("local_path"), localPath},
                                   {QStringLiteral("state"), QStringLiteral("initializing")}});
        Q_EMIT changed();
    }
    void pausePair(const QString &pairId) override
    { ++pauseCalls; lastPairId = pairId; Q_EMIT changed(); }
    void resumePair(const QString &pairId) override
    { ++resumeCalls; lastPairId = pairId; Q_EMIT changed(); }
    void syncNow(const QString &pairId) override
    { ++syncNowCalls; lastPairId = pairId; Q_EMIT changed(); }
    void removePair(const QString &pairId) override
    { ++removeCalls; lastPairId = pairId; Q_EMIT changed(); }
    void reviewPair(const QString &pairId) override
    { ++reviewCalls; lastPairId = pairId; Q_EMIT reviewReady(reviewDocument); }
    void resumeReviewed(const QString &pairId, const QString &reviewToken) override
    { ++resumeReviewedCalls; lastPairId = pairId; lastReviewToken = reviewToken; Q_EMIT changed(); }
    void cancelSetup() override { ++cancelSetupCalls; busy = false; }
};

QPushButton *button(GoogleDriveSyncDialog &dialog, const char *name)
{
    return dialog.findChild<QPushButton *>(QLatin1String(name));
}

QTreeWidget *tree(GoogleDriveSyncDialog &dialog, const char *name)
{
    return dialog.findChild<QTreeWidget *>(QLatin1String(name));
}

void showDialog(GoogleDriveSyncDialog &dialog)
{
    dialog.show();
    dialog.activateWindow();
    QCoreApplication::processEvents();
}

} // namespace

class GoogleDriveSyncDialogTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase()
    {
        Explorer::configureApplicationAppearance(qApp);
    }

    void savedPermissionCanResumeWithoutNewConsent()
    {
        FakeGoogleDriveSyncController controller;
        controller.savedPermission = true;
        GoogleDriveSyncDialog dialog(nullptr, &controller);
        showDialog(dialog);

        button(dialog, "googleDriveSyncAdd")->click();
        QCOMPARE(controller.listCalls, 0);
        auto *permission = button(dialog, "googleDriveSyncPermission");
        QVERIFY(permission->isVisible());
        QVERIFY(permission->isEnabled());
        QCOMPARE(permission->text(), QStringLiteral("Resume synchronization"));
        permission->click();
        QCOMPARE(controller.permissionCalls, 1);
        QCOMPARE(controller.listCalls, 0); // Resume will trigger loading when access is active.
    }

    void expiredSavedPermissionHasExplicitReauthorizationAction()
    {
        FakeGoogleDriveSyncController controller;
        controller.savedPermission = true;
        GoogleDriveSyncDialog dialog(nullptr, &controller);
        showDialog(dialog);

        button(dialog, "googleDriveSyncAdd")->click();
        auto *reauthorize = button(dialog, "googleDriveSyncSignInAgain");
        QVERIFY(reauthorize->isVisible());
        QVERIFY(reauthorize->isEnabled());
        QVERIFY(reauthorize->accessibleDescription().contains(
            QStringLiteral("same Google account as the Drive mount")));
        reauthorize->click();
        QCOMPARE(controller.signInAgainCalls, 1);
        QCOMPARE(controller.permissionCalls, 0);
        QVERIFY(!reauthorize->isEnabled());
    }

    void closingSetupCancelsOwnedAuthorizationButNotBackgroundPairs()
    {
        FakeGoogleDriveSyncController controller;
        auto *setupDialog = new GoogleDriveSyncDialog(nullptr, &controller);
        showDialog(*setupDialog);
        button(*setupDialog, "googleDriveSyncAdd")->click();
        button(*setupDialog, "googleDriveSyncPermission")->click();
        QCOMPARE(controller.permissionCalls, 1);
        delete setupDialog;
        QCOMPARE(controller.cancelSetupCalls, 1);

        QTemporaryDir localFolder;
        QVERIFY(localFolder.isValid());
        controller.pairList = QJsonArray{QJsonObject{
            {QStringLiteral("id"), QStringLiteral("pair-background")},
            {QStringLiteral("remote_name"), QStringLiteral("Background pair")},
            {QStringLiteral("local_path"), localFolder.path()},
            {QStringLiteral("state"), QStringLiteral("syncing")}}};
        auto *managerDialog = new GoogleDriveSyncDialog(nullptr, &controller);
        showDialog(*managerDialog);
        delete managerDialog;
        QCOMPARE(controller.cancelSetupCalls, 1);
    }

    void setupRequiresExplicitWritePermissionAndReviewedInitialCopy()
    {
        FakeGoogleDriveSyncController controller;
        GoogleDriveSyncDialog dialog(nullptr, &controller);
        showDialog(dialog);
        QCOMPARE(controller.refreshCalls, 1);

        button(dialog, "googleDriveSyncAdd")->click();
        QCOMPARE(controller.permissionCalls, 0);
        QCOMPARE(controller.listCalls, 0);
        QVERIFY(!button(dialog, "googleDriveSyncStart")->isEnabled());

        button(dialog, "googleDriveSyncPermission")->click();
        QCOMPARE(controller.permissionCalls, 1);
        controller.writePermission = true;
        Q_EMIT controller.changed();
        QCOMPARE(controller.listCalls, 1);
        QCOMPARE(controller.lastParentId, QStringLiteral("root"));

        Q_EMIT controller.foldersListed(
            QStringLiteral("root"),
            QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("folder-1")},
                                   {QStringLiteral("name"), QStringLiteral("Project")}}});
        QTemporaryDir localFolder;
        QVERIFY(localFolder.isValid());
        auto *remoteTree = tree(dialog, "googleDriveSyncRemoteFolders");
        QCOMPARE(remoteTree->topLevelItemCount(), 1);
        remoteTree->setCurrentItem(remoteTree->topLevelItem(0));
        QFile existing(localFolder.filePath(QStringLiteral("existing.txt")));
        QVERIFY(existing.open(QIODevice::WriteOnly));
        existing.write("keep");
        existing.close();
        dialog.findChild<QLineEdit *>(QStringLiteral("googleDriveSyncLocalFolder"))
            ->setText(localFolder.path());

        QPushButton *preview = button(dialog, "googleDriveSyncPreview");
        QVERIFY(!preview->isEnabled());
        QVERIFY(QFile::remove(existing.fileName()));
        auto *localPath = dialog.findChild<QLineEdit *>(QStringLiteral("googleDriveSyncLocalFolder"));
        localPath->clear();
        localPath->setText(localFolder.path());
        QVERIFY(preview->isEnabled());
        preview->click();
        QCOMPARE(controller.previewCalls, 1);
        QCOMPARE(controller.lastRemoteId, QStringLiteral("folder-1"));
        QCOMPARE(controller.lastLocalPath, QDir::cleanPath(localFolder.path()));
        QVERIFY(!button(dialog, "googleDriveSyncStart")->isEnabled());

        Q_EMIT controller.previewReady(QJsonObject{
            {QStringLiteral("folder_id"), QStringLiteral("folder-1")},
            {QStringLiteral("name"), QStringLiteral("Project")},
            {QStringLiteral("file_count"), 3},
            {QStringLiteral("bytes"), 2048}});
        QLabel *summary = dialog.findChild<QLabel *>(QStringLiteral("googleDriveSyncPreviewSummary"));
        QVERIFY(summary->text().contains(QStringLiteral("3 files")));
        QVERIFY(summary->text().contains(QStringLiteral("from Google Drive")));
        QVERIFY(button(dialog, "googleDriveSyncStart")->isEnabled());

        button(dialog, "googleDriveSyncStart")->click();
        QCOMPARE(controller.addCalls, 1);
        QCOMPARE(controller.lastRemoteId, QStringLiteral("folder-1"));
        QCOMPARE(controller.lastLocalPath, QDir::cleanPath(localFolder.path()));
        QVERIFY(button(dialog, "googleDriveSyncAdd")->isVisible());
        QCOMPARE(tree(dialog, "googleDriveSyncPairs")->topLevelItemCount(), 1);
        QVERIFY(dialog.findChild<QLabel *>(QStringLiteral("googleDriveSyncStatus"))
                    ->text().contains(QStringLiteral("Synchronization is starting")));
    }

    void backgroundStatusDoesNotReplacePendingPreviewProgress()
    {
        FakeGoogleDriveSyncController controller;
        controller.writePermission = true;
        controller.savedPermission = true;
        GoogleDriveSyncDialog dialog(nullptr, &controller);
        showDialog(dialog);
        button(dialog, "googleDriveSyncAdd")->click();
        Q_EMIT controller.foldersListed(QStringLiteral("root"), QJsonArray{
            QJsonObject{{QStringLiteral("id"), QStringLiteral("folder-1")},
                        {QStringLiteral("name"), QStringLiteral("Project")}}});
        tree(dialog, "googleDriveSyncRemoteFolders")
            ->setCurrentItem(tree(dialog, "googleDriveSyncRemoteFolders")->topLevelItem(0));
        QTemporaryDir localFolder;
        QVERIFY(localFolder.isValid());
        dialog.findChild<QLineEdit *>(QStringLiteral("googleDriveSyncLocalFolder"))
            ->setText(localFolder.path());
        button(dialog, "googleDriveSyncPreview")->click();

        controller.busy = true;
        controller.message = QStringLiteral(
            "Google Drive synchronization support is unavailable. Try refreshing.");
        Q_EMIT controller.changed();
        QCOMPARE(dialog.findChild<QLabel *>(QStringLiteral("googleDriveSyncPreviewSummary"))->text(),
                 QStringLiteral("Checking the initial copy…"));
        QCOMPARE(dialog.findChild<QLabel *>(QStringLiteral("googleDriveSyncStatus"))->text(),
                 QStringLiteral("Reviewing the selected Drive folder…"));
        QVERIFY(!button(dialog, "googleDriveSyncPreview")->isEnabled());
        QVERIFY(!button(dialog, "googleDriveSyncStart")->isEnabled());

        controller.busy = false;
        controller.message.clear();
        Q_EMIT controller.previewReady(QJsonObject{
            {QStringLiteral("folder_id"), QStringLiteral("folder-1")},
            {QStringLiteral("name"), QStringLiteral("Project")},
            {QStringLiteral("file_count"), 2},
            {QStringLiteral("bytes"), 1024}});
        QVERIFY(dialog.findChild<QLabel *>(QStringLiteral("googleDriveSyncPreviewSummary"))
                    ->text().contains(QStringLiteral("2 files")));
        QVERIFY(button(dialog, "googleDriveSyncStart")->isEnabled());
    }

    void previewFailureClearsProgressAndAllowsRetry()
    {
        FakeGoogleDriveSyncController controller;
        controller.writePermission = true;
        controller.savedPermission = true;
        GoogleDriveSyncDialog dialog(nullptr, &controller);
        showDialog(dialog);
        button(dialog, "googleDriveSyncAdd")->click();
        Q_EMIT controller.foldersListed(QStringLiteral("root"), QJsonArray{
            QJsonObject{{QStringLiteral("id"), QStringLiteral("folder-1")},
                        {QStringLiteral("name"), QStringLiteral("Project")}}});
        auto *remote = tree(dialog, "googleDriveSyncRemoteFolders");
        remote->setCurrentItem(remote->topLevelItem(0));
        QTemporaryDir localFolder;
        QVERIFY(localFolder.isValid());
        dialog.findChild<QLineEdit *>(QStringLiteral("googleDriveSyncLocalFolder"))
            ->setText(localFolder.path());
        button(dialog, "googleDriveSyncPreview")->click();
        QVERIFY(button(dialog, "googleDriveSyncPreview")->isEnabled() == false);

        Q_EMIT controller.failed(QStringLiteral("Could not check this Drive folder. Try again."));
        auto *summary = dialog.findChild<QLabel *>(QStringLiteral("googleDriveSyncPreviewSummary"));
        QVERIFY(summary->text().isEmpty());
        QCOMPARE(dialog.findChild<QLabel *>(QStringLiteral("googleDriveSyncStatus"))->text(),
                 QStringLiteral("Could not check this Drive folder. Try again."));
        QVERIFY(button(dialog, "googleDriveSyncPreview")->isEnabled());
        QVERIFY(!button(dialog, "googleDriveSyncStart")->isEnabled());

        button(dialog, "googleDriveSyncPreview")->click();
        QCOMPARE(controller.previewCalls, 2);
        QCOMPARE(summary->text(), QStringLiteral("Checking the initial copy…"));
    }

    void backingOutCancelsPendingPreviewAndIgnoresLateResults()
    {
        FakeGoogleDriveSyncController controller;
        controller.writePermission = true;
        controller.savedPermission = true;
        GoogleDriveSyncDialog dialog(nullptr, &controller);
        showDialog(dialog);
        button(dialog, "googleDriveSyncAdd")->click();
        Q_EMIT controller.foldersListed(QStringLiteral("root"), QJsonArray{
            QJsonObject{{QStringLiteral("id"), QStringLiteral("folder-1")},
                        {QStringLiteral("name"), QStringLiteral("Project")}}});
        auto *remote = tree(dialog, "googleDriveSyncRemoteFolders");
        remote->setCurrentItem(remote->topLevelItem(0));
        QTemporaryDir localFolder;
        QVERIFY(localFolder.isValid());
        dialog.findChild<QLineEdit *>(QStringLiteral("googleDriveSyncLocalFolder"))
            ->setText(localFolder.path());
        button(dialog, "googleDriveSyncPreview")->click();
        QCOMPARE(dialog.findChild<QLabel *>(QStringLiteral("googleDriveSyncStatus"))->text(),
                 QStringLiteral("Reviewing the selected Drive folder…"));

        button(dialog, "googleDriveSyncBack")->click();
        QCOMPARE(controller.cancelSetupCalls, 1);
        QVERIFY(button(dialog, "googleDriveSyncAdd")->isVisible());
        QVERIFY(!button(dialog, "googleDriveSyncStart")->isVisible());
        QVERIFY(dialog.findChild<QLabel *>(QStringLiteral("googleDriveSyncPreviewSummary"))
                    ->text().isEmpty());
        QVERIFY(dialog.findChild<QLabel *>(QStringLiteral("googleDriveSyncStatus"))->text().isEmpty());

        Q_EMIT controller.previewReady(QJsonObject{
            {QStringLiteral("folder_id"), QStringLiteral("folder-1")},
            {QStringLiteral("name"), QStringLiteral("Late result")},
            {QStringLiteral("file_count"), 2},
            {QStringLiteral("bytes"), 1024}});
        Q_EMIT controller.failed(QStringLiteral("Late preview failure"));
        QVERIFY(button(dialog, "googleDriveSyncAdd")->isVisible());
        QVERIFY(dialog.findChild<QLabel *>(QStringLiteral("googleDriveSyncPreviewSummary"))
                    ->text().isEmpty());
        QVERIFY(dialog.findChild<QLabel *>(QStringLiteral("googleDriveSyncStatus"))->text().isEmpty());
    }

    void existingPairActionsTargetSelectedPairAndKeepLocalFilesOnRemoval()
    {
        FakeGoogleDriveSyncController controller;
        QTemporaryDir localFolder;
        QVERIFY(localFolder.isValid());
        controller.pairList = QJsonArray{QJsonObject{
            {QStringLiteral("id"), QStringLiteral("pair-7")},
            {QStringLiteral("remote_folder_id"), QStringLiteral("folder-7")},
            {QStringLiteral("remote_name"), QStringLiteral("Photos")},
            {QStringLiteral("local_path"), localFolder.path()},
            {QStringLiteral("state"), QStringLiteral("paused")},
            {QStringLiteral("can_resume"), true}}};
        GoogleDriveSyncDialog dialog(nullptr, &controller);
        showDialog(dialog);
        auto *pairs = tree(dialog, "googleDriveSyncPairs");
        QCOMPARE(pairs->topLevelItemCount(), 1);
        pairs->setCurrentItem(pairs->topLevelItem(0));

        QCOMPARE(button(dialog, "googleDriveSyncPauseResume")->text(), QStringLiteral("Resume"));
        button(dialog, "googleDriveSyncPauseResume")->click();
        QCOMPARE(controller.resumeCalls, 1);
        QCOMPARE(controller.lastPairId, QStringLiteral("pair-7"));
        button(dialog, "googleDriveSyncNow")->click();
        QCOMPARE(controller.syncNowCalls, 1);
        QVERIFY(button(dialog, "googleDriveSyncOpenLocal")->isEnabled());
        button(dialog, "googleDriveSyncRemove")->click();
        QCOMPARE(controller.removeCalls, 1);
        QCOMPARE(controller.lastPairId, QStringLiteral("pair-7"));
        QVERIFY(button(dialog, "googleDriveSyncRemove")->toolTip().contains(
            QStringLiteral("keeps the local files")));

        controller.pairList[0] = QJsonObject{
            {QStringLiteral("id"), QStringLiteral("pair-7")},
            {QStringLiteral("remote_folder_id"), QStringLiteral("folder-7")},
            {QStringLiteral("remote_name"), QStringLiteral("Photos")},
            {QStringLiteral("local_path"), localFolder.path()},
            {QStringLiteral("state"), QStringLiteral("needs-review")},
            {QStringLiteral("can_resume"), true},
            {QStringLiteral("error"), QStringLiteral("Review the existing files before retrying.")}};
        Q_EMIT controller.changed();
        QCOMPARE(pairs->topLevelItem(0)->text(2), QStringLiteral("Needs attention"));
        QVERIFY(pairs->topLevelItem(0)->toolTip(2).contains(
            QStringLiteral("Review the existing files before retrying.")));
    }

    void needsReviewRequiresExplicitReviewAndConfirmation()
    {
        FakeGoogleDriveSyncController controller;
        QTemporaryDir localFolder;
        QVERIFY(localFolder.isValid());
        controller.pairList = QJsonArray{QJsonObject{
            {QStringLiteral("id"), QStringLiteral("pair-review")},
            {QStringLiteral("remote_name"), QStringLiteral("Project")},
            {QStringLiteral("local_path"), localFolder.path()},
            {QStringLiteral("state"), QStringLiteral("needs-review")},
            {QStringLiteral("can_resume"), true},
            {QStringLiteral("error"), QStringLiteral("Review the file removals.")}}};
        controller.reviewDocument = QJsonObject{
            {QStringLiteral("pair_id"), QStringLiteral("pair-review")},
            {QStringLiteral("token"), QStringLiteral("review-token-1")},
            {QStringLiteral("message"), QStringLiteral("Review the changes before continuing. Backups are retained.")},
            {QStringLiteral("local_deleted"), QJsonArray{QStringLiteral("draft.txt")}},
            {QStringLiteral("remote_deleted"), QJsonArray{QStringLiteral("archive/old.txt")}}};
        GoogleDriveSyncDialog dialog(nullptr, &controller);
        showDialog(dialog);
        auto *pairs = tree(dialog, "googleDriveSyncPairs");
        pairs->setCurrentItem(pairs->topLevelItem(0));

        auto *reviewButton = button(dialog, "googleDriveSyncPauseResume");
        QCOMPARE(reviewButton->text(), QStringLiteral("Review…"));
        QVERIFY(reviewButton->isEnabled());
        QVERIFY(!button(dialog, "googleDriveSyncNow")->isEnabled());
        QTimer::singleShot(0, [] {
            auto *review = QApplication::activeModalWidget();
            QVERIFY(review);
            QCOMPARE(review->windowModality(), Qt::WindowModal);
            auto *localList = review->findChild<QPlainTextEdit *>(
                QStringLiteral("googleDriveSyncLocalRemovals"));
            auto *driveList = review->findChild<QPlainTextEdit *>(
                QStringLiteral("googleDriveSyncDriveRemovals"));
            QVERIFY(localList);
            QVERIFY(driveList);
            QCOMPARE(localList->toPlainText(), QStringLiteral("draft.txt"));
            QCOMPARE(driveList->toPlainText(), QStringLiteral("archive/old.txt"));
            review->findChild<QPushButton *>(QStringLiteral("googleDriveSyncReviewCancel"))->click();
        });
        reviewButton->click();
        QCOMPARE(controller.reviewCalls, 1);
        QCOMPARE(controller.resumeReviewedCalls, 0);

        QTimer::singleShot(0, [] {
            auto *review = QApplication::activeModalWidget();
            QVERIFY(review);
            review->findChild<QPushButton *>(QStringLiteral("googleDriveSyncReviewResume"))->click();
        });
        reviewButton->click();
        QCOMPARE(controller.reviewCalls, 2);
        QCOMPARE(controller.resumeReviewedCalls, 1);
        QCOMPARE(controller.lastPairId, QStringLiteral("pair-review"));
        QCOMPARE(controller.lastReviewToken, QStringLiteral("review-token-1"));
    }

    void unsupportedBuildExplainsAvailability()
    {
        FakeGoogleDriveSyncController controller;
        controller.available = false;
        controller.message = QStringLiteral("Google Drive sync is unavailable.");
        GoogleDriveSyncDialog dialog(nullptr, &controller);
        showDialog(dialog);
        QVERIFY(button(dialog, "googleDriveSyncAdd")->isVisible());
        QVERIFY(!button(dialog, "googleDriveSyncAdd")->isEnabled());
        QCOMPARE(dialog.findChild<QLabel *>(QStringLiteral("googleDriveSyncStatus"))->text(),
                 controller.message);
    }

    void snapshotsCommonStatesWhenRequested()
    {
        const QString output = qEnvironmentVariable("EXPLORER_SNAPSHOTS");
        if (output.isEmpty())
            QSKIP("Set EXPLORER_SNAPSHOTS for Google Drive synchronization snapshots");

        FakeGoogleDriveSyncController controller;
        controller.writePermission = true;
        controller.savedPermission = true;
        QTemporaryDir localFolder;
        QVERIFY(localFolder.isValid());
        QTemporaryDir setupFolder;
        QVERIFY(setupFolder.isValid());
        controller.pairList = QJsonArray{
            QJsonObject{{QStringLiteral("id"), QStringLiteral("pair-review")},
                        {QStringLiteral("remote_name"), QStringLiteral("Needs attention")},
                        {QStringLiteral("local_path"), localFolder.path()},
                        {QStringLiteral("state"), QStringLiteral("needs-review")},
                        {QStringLiteral("error"), QStringLiteral("Review existing files before retrying.")}},
            QJsonObject{{QStringLiteral("id"), QStringLiteral("pair-paused")},
                        {QStringLiteral("remote_name"), QStringLiteral("Archive")},
                        {QStringLiteral("local_path"), localFolder.path()},
                        {QStringLiteral("state"), QStringLiteral("paused")}},
            QJsonObject{{QStringLiteral("id"), QStringLiteral("pair-syncing")},
                        {QStringLiteral("remote_name"), QStringLiteral("Current work")},
                        {QStringLiteral("local_path"), localFolder.path()},
                        {QStringLiteral("state"), QStringLiteral("syncing")}}};
        GoogleDriveSyncDialog dialog(nullptr, &controller);
        showDialog(dialog);

        const QString scale = QString::number(qRound(dialog.devicePixelRatioF() * 100));
        const QString folder = QDir(output).filePath(QStringLiteral("google-drive-sync-%1").arg(scale));
        QVERIFY(QDir().mkpath(folder));
        auto save = [&dialog, &folder](const QString &name) {
            QApplication::processEvents();
            return dialog.grab().save(QDir(folder).filePath(name + QStringLiteral(".png")));
        };

        QVERIFY(save(QStringLiteral("manager-statuses")));
        button(dialog, "googleDriveSyncAdd")->click();
        Q_EMIT controller.foldersListed(QStringLiteral("root"), QJsonArray{
            QJsonObject{{QStringLiteral("id"), QStringLiteral("folder-project")},
                        {QStringLiteral("name"), QStringLiteral("Project files")}}});
        auto *remote = tree(dialog, "googleDriveSyncRemoteFolders");
        remote->setCurrentItem(remote->topLevelItem(0));
        dialog.findChild<QLineEdit *>(QStringLiteral("googleDriveSyncLocalFolder"))
            ->setText(setupFolder.path());
        QVERIFY(save(QStringLiteral("setup")));

        button(dialog, "googleDriveSyncPreview")->click();
        Q_EMIT controller.previewReady(QJsonObject{
            {QStringLiteral("folder_id"), QStringLiteral("folder-project")},
            {QStringLiteral("name"), QStringLiteral("Project files")},
            {QStringLiteral("file_count"), 12},
            {QStringLiteral("bytes"), 1024 * 1024 * 4}});
        QApplication::processEvents();
        auto *summary = dialog.findChild<QLabel *>(QStringLiteral("googleDriveSyncPreviewSummary"));
        QVERIFY(summary);
        QVERIFY(summary->text().contains(QStringLiteral("If both sides change a file")));
        const int requiredSummaryHeight = summary->fontMetrics().boundingRect(
            summary->rect(), Qt::TextWordWrap, summary->text()).height();
        QVERIFY2(summary->height() >= requiredSummaryHeight,
                 "the full initial-copy summary must fit inside its label before capture");
        QVERIFY(save(QStringLiteral("initial-copy-review")));
    }
};

QTEST_MAIN(GoogleDriveSyncDialogTest)
#include "GoogleDriveSyncDialogTest.moc"
