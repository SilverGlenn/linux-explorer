#include "../src/data/GoogleDriveSyncController.h"

#include <QFile>
#include <QJsonDocument>
#include <QDir>
#include <QProcess>
#include <QLockFile>
#include <QStandardPaths>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

namespace {
class TestController : public GoogleDriveSyncController
{
public:
    explicit TestController(QString helper) : m_helper(std::move(helper)) {}
protected:
    QString helperPath() const override { return m_helper; }
private:
    QString m_helper;
};

QString helper(QTemporaryDir &directory, const QByteArray &body)
{
    const QString path = directory.filePath(QStringLiteral("helper.py"));
    QFile script(path);
    if (!script.open(QIODevice::WriteOnly))
        return {};
    script.write("#!/usr/bin/env python3\nimport json, os, sys, time\n");
    script.write(body);
    script.close();
    script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    return path;
}

const QByteArray statusBody =
    "print(json.dumps({'version':1,'ok':True,'available':True,'worker_running':False,"
    "'sync_configured':True,'pairs':[], 'message':'Ready',"
    "'ambient_removed':not any(k.startswith('RCLONE_') for k in os.environ)}))\n";
}

class GoogleDriveSyncControllerTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void refreshIsAsynchronousAndRemovesAmbientRcloneOverrides()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        TestController controller(helper(directory, "time.sleep(.05)\n" + statusBody));
        qputenv("RCLONE_CONFIG_PASS", "synthetic-ambient-secret");
        QSignalSpy changed(&controller, &GoogleDriveSyncController::changed);
        controller.refresh();
        qunsetenv("RCLONE_CONFIG_PASS");
        QVERIFY(controller.isBusy());
        QVERIFY(!controller.isAvailable());
        QTRY_VERIFY(!controller.isBusy());
        QVERIFY(controller.isAvailable());
        QVERIFY(controller.hasSavedPermission());
        QVERIFY(!controller.hasWritePermission());
        QVERIFY(!controller.workerRunning());
        QVERIFY(controller.m_status.value(QStringLiteral("ambient_removed")).toBool());
        QCOMPARE(controller.statusMessage(), QStringLiteral("Ready"));
        QVERIFY(changed.count() >= 2);
        controller.m_poll->stop();
    }

    void invalidAndOversizedResponsesCannotBecomeAReadyWorker_data()
    {
        QTest::addColumn<QByteArray>("body");
        QTest::newRow("version") << QByteArray("print(json.dumps({'version':2,'ok':True,'available':True,'worker_running':True,'pairs':[]}))\n");
        QTest::newRow("trailing-content") << QByteArray("print('{} {}')\n");
        QTest::newRow("too-large") << QByteArray("print('x' * (1024 * 1024 + 1))\n");
    }

    void invalidAndOversizedResponsesCannotBecomeAReadyWorker()
    {
        QFETCH(QByteArray, body);
        QTemporaryDir directory;
        TestController controller(helper(directory, body));
        controller.refresh();
        QTRY_VERIFY(!controller.isBusy());
        QVERIFY(!controller.isAvailable());
        QVERIFY(!controller.workerRunning());
        controller.m_poll->stop();
    }

    void workerErrorsPreserveActionableMessagesEvenWithNonzeroExit()
    {
        QTemporaryDir directory;
        const auto body = QByteArray(
            "request=json.load(sys.stdin)\n"
            "print(json.dumps({'version':1,'ok':False,'available':True,'worker_running':True,"
            "'sync_configured':True,'pairs':[], 'message':'The access sentinel is missing.'}))\n"
            "sys.exit(1)\n");
        TestController controller(helper(directory, body));
        controller.m_workerRunning = true;
        QSignalSpy failures(&controller, &GoogleDriveSyncController::failed);
        controller.syncNow(QStringLiteral("synthetic-pair"));
        QTRY_COMPARE(failures.count(), 1);
        QCOMPARE(controller.statusMessage(), QStringLiteral("The access sentinel is missing."));
        QVERIFY(controller.hasSavedPermission());
        helper(directory, statusBody);
        controller.refresh();
        QTRY_VERIFY(!controller.isBusy());
        QCOMPARE(controller.statusMessage(), QStringLiteral("The access sentinel is missing."));
        controller.m_poll->stop();
    }

    void failedStatusPollCannotInventAStoppedOrUnsupportedWorker()
    {
        QTemporaryDir directory;
        TestController controller(helper(directory, "print('invalid')\n"));
        controller.m_workerRunning = true;
        controller.m_available = true;
        controller.m_status = {{QStringLiteral("sync_configured"), true}};
        controller.m_message = QStringLiteral("The selected folder contains an unsupported document.");
        controller.m_preserveError = true;
        controller.refresh();
        QTRY_VERIFY(!controller.isBusy());
        QVERIFY(controller.workerRunning());
        QVERIFY(controller.isAvailable());
        QVERIFY(controller.hasSavedPermission());
        QCOMPARE(controller.statusMessage(), QStringLiteral("The selected folder contains an unsupported document."));
        controller.m_poll->stop();
    }

    void metadataCommandsWaitBeyondTheWorkerRpcBudget_data()
    {
        QTest::addColumn<QString>("action");
        for (const QString &action : {QStringLiteral("preview"), QStringLiteral("list-folders"),
                                      QStringLiteral("review"), QStringLiteral("resume-reviewed")})
            QTest::newRow(qPrintable(action)) << action;
    }

    void metadataCommandsWaitBeyondTheWorkerRpcBudget()
    {
        QFETCH(QString, action);
        QTemporaryDir directory;
        TestController controller(helper(directory, "json.load(sys.stdin)\ntime.sleep(.05)\n" + statusBody));
        controller.m_workerRunning = true;
        bool completed = false;
        controller.request({{QStringLiteral("action"), action}},
                           [&](bool ok, QJsonObject) { completed = ok; });
        QVERIFY(controller.m_process);
        const auto *deadline = controller.m_process->findChild<QTimer *>();
        QVERIFY(deadline);
        QVERIFY2(deadline->interval() > 180000, "Client must not time out before the worker's 180-second RPC response budget");
        QTRY_VERIFY(completed);
        QVERIFY(!controller.isBusy());
    }

    void backingOutCancelsOnlyTheReadOnlySetupClientAndIgnoresLateResults()
    {
        QTemporaryDir directory;
        TestController controller(helper(directory, "json.load(sys.stdin)\ntime.sleep(10)\n" + statusBody));
        controller.m_workerRunning = true;
        controller.m_available = true;
        QSignalSpy failures(&controller, &GoogleDriveSyncController::failed);
        QSignalSpy previews(&controller, &GoogleDriveSyncController::previewReady);
        controller.previewPair(QStringLiteral("synthetic-folder"), directory.path());
        QVERIFY(controller.isBusy());
        controller.cancelSetup();
        QTRY_VERIFY(!controller.isBusy());
        QCOMPARE(failures.count(), 0);
        QCOMPARE(previews.count(), 0);
        QVERIFY(controller.workerRunning());
        QVERIFY(controller.isAvailable());
        QCOMPARE(controller.statusMessage(), QStringLiteral("Synchronization setup cancelled."));
    }

    void privateEnvelopeUsesStdinAndHelperTimeoutCompletesOnce()
    {
        QTemporaryDir directory;
        TestController controller(helper(directory,
            "value=json.load(sys.stdin)\n"
            "assert len(sys.argv)==2\n"
            "assert value['config_password']=='synthetic-private-password'\n"
            "print(json.dumps({'version':1,'ok':True}))\n"));
        int completions = 0;
        bool success = false;
        controller.runHelper(QStringLiteral("start"),
            {{QStringLiteral("config_password"), QStringLiteral("synthetic-private-password")}},
            [&](bool ok, QJsonObject result) { ++completions; success = ok && result.value(QStringLiteral("ok")).toBool(); });
        QTRY_COMPARE(completions, 1);
        QVERIFY(success);

        TestController slow(helper(directory, "time.sleep(10)\n"));
        completions = 0;
        slow.runHelper(QStringLiteral("status"), {},
            [&](bool ok, QJsonObject) { ++completions; success = ok; }, 30);
        QTRY_COMPARE(completions, 1);
        QVERIFY(!success);
        QTest::qWait(50);
        QCOMPARE(completions, 1);
        QVERIFY(!slow.isBusy());
    }

    void disconnectWaitsForStopAndVerifiedWorkerExit()
    {
        QTemporaryDir directory;
        const QString marker = directory.filePath(QStringLiteral("stopped"));
        auto body = QByteArray("marker=") + QJsonDocument(QJsonArray{marker}).toJson(QJsonDocument::Compact)
            + "[0]\n"
              "if sys.argv[1]=='request':\n"
              "    request=json.load(sys.stdin)\n"
              "    assert request['action']=='stop'\n"
              "    time.sleep(.05)\n"
              "    open(marker,'w').close()\n"
              "print(json.dumps({'version':1,'ok':True,'available':True,'worker_running':not os.path.exists(marker),"
              "'sync_configured':True,'pairs':[]}))\n";
        TestController controller(helper(directory, body));
        bool completed = false, success = false;
        controller.prepareDisconnect(false, [&](bool ok, QString) { completed = true; success = ok; });
        QVERIFY(!completed);
        QTRY_VERIFY(completed);
        QVERIFY(success);
        QVERIFY(QFile::exists(marker));
        QVERIFY(!controller.workerRunning());
        QVERIFY(!controller.isBusy());
    }

    void disconnectCannotReportSuccessWhileWorkerStillHoldsSession()
    {
        QTemporaryDir directory;
        auto body = QByteArray(
            "if sys.argv[1]=='request': json.load(sys.stdin)\n"
            "print(json.dumps({'version':1,'ok':True,'available':True,'worker_running':True,'pairs':[]}))\n");
        TestController controller(helper(directory, body));
        bool completed = false, success = true;
        controller.prepareDisconnect(false, [&](bool ok, QString) { completed = true; success = ok; });
        QTRY_VERIFY(completed);
        QVERIFY(!success);
        QVERIFY(controller.workerRunning());
    }

    void backgroundStatusPollingCannotRaceExplicitDisconnect()
    {
        QTemporaryDir directory;
        TestController controller(helper(directory, "time.sleep(.1)\n" + statusBody));
        controller.refresh();
        QVERIFY(controller.isBusy());
        bool completed = false, success = false;
        controller.prepareDisconnect(false, [&](bool ok, QString) { completed = true; success = ok; });
        QTRY_VERIFY(completed);
        QVERIFY(success);
        QVERIFY(!controller.m_poll->isActive());
        QVERIFY(!controller.isBusy());
    }

    void disconnectRetiresAnOwnedReadOnlyPreviewBeforeStoppingTheWorker()
    {
        QTemporaryDir directory;
        TestController controller(helper(directory,
            "if sys.argv[1]=='request':\n"
            "    request=json.load(sys.stdin)\n"
            "    if request['action']=='preview': time.sleep(10)\n" + statusBody));
        controller.m_workerRunning = true;
        controller.previewPair(QStringLiteral("synthetic-folder"), directory.path());
        QVERIFY(controller.isBusy());
        bool completed = false, success = false;
        controller.prepareDisconnect(true, [&](bool ok, QString) { completed = true; success = ok; });
        QTRY_VERIFY(completed);
        QVERIFY(success);
        QVERIFY(!controller.workerRunning());
        QVERIFY(!controller.isBusy());
        QVERIFY(!controller.m_poll->isActive());
    }

    void queuedWorkerRequestRetainsItsCompletionAfterStarting()
    {
        QTemporaryDir directory;
        TestController controller(helper(directory,
            "request=json.load(sys.stdin)\n"
            "result={'version':1,'ok':True,'available':True,'worker_running':True,'sync_configured':True,'pairs':[]}\n"
            "if sys.argv[1]=='request': result['review']={'token':'reviewed-fixture'}\n"
            "print(json.dumps(result))\n"));
        controller.m_pendingRequest = {{QStringLiteral("action"), QStringLiteral("review")},
                                       {QStringLiteral("pair_id"), QStringLiteral("fixture")}};
        int calls = 0;
        controller.m_pendingCompletion = [&](bool success, QJsonObject result) {
            QVERIFY(success);
            QCOMPARE(result.value(QStringLiteral("review")).toObject().value(QStringLiteral("token")).toString(),
                     QStringLiteral("reviewed-fixture"));
            ++calls;
        };
        controller.m_browsePassword = QStringLiteral("synthetic-browse-password");
        controller.startWorker({}, QStringLiteral("synthetic-sync-password"));
        QTRY_COMPARE(calls, 1);
        QVERIFY(controller.m_pendingRequest.isEmpty());
        QVERIFY(!controller.m_pendingCompletion);
        controller.m_poll->stop();
    }

    void timedOutHelperCannotClaimSuccessFromAnEarlyResponse()
    {
        QTemporaryDir directory;
        TestController controller(helper(directory, statusBody + "sys.stdout.flush()\ntime.sleep(10)\n"));
        int calls = 0;
        bool successful = true;
        controller.runHelper(QStringLiteral("status"), {},
            [&](bool ok, QJsonObject) { successful = ok; ++calls; }, 100);
        QTRY_COMPARE(calls, 1);
        QVERIFY(!successful);
    }

    void failedStartupStatusCanBeRetriedWithoutOpeningAuthentication()
    {
        QTemporaryDir directory;
        const QString path = helper(directory, "print('invalid')\n");
        TestController controller(path);
        controller.restoreWorker();
        QTRY_VERIFY(!controller.isBusy());
        QVERIFY(!controller.m_browseAuth);
        helper(directory, statusBody);
        controller.restoreWorker();
        QTRY_VERIFY(!controller.isBusy());
        QVERIFY(controller.isAvailable());
        QVERIFY(!controller.m_browseAuth); // no active pairs, so no wallet read
    }

    void syncAuthorizationCannotStartDuringAnotherAccountOperation()
    {
        QTemporaryDir directory;
        TestController controller(helper(directory, statusBody));
        const QString runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
        QVERIFY(!runtime.isEmpty());
        QLockFile accountLock(QDir(runtime).filePath(QStringLiteral("explorer-win7-google-drive-auth.lock")));
        accountLock.setStaleLockTime(0);
        QVERIFY(accountLock.tryLock());
        QSignalSpy failures(&controller, &GoogleDriveSyncController::failed);
        controller.requestWritePermission();
        QCOMPARE(failures.count(), 1);
        QVERIFY(!controller.m_browseAuth);
        QVERIFY(!controller.m_syncAuth);
        QVERIFY(!controller.m_process);
        QVERIFY(!controller.m_authorizationLock);
        QVERIFY(!controller.isBusy());
    }
};

QTEST_MAIN(GoogleDriveSyncControllerTest)
#include "GoogleDriveSyncControllerTest.moc"
