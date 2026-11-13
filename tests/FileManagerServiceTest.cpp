#include "DirectoryModel.h"
#include "FileManagerService.h"
#include "FileView.h"
#include "MainWindow.h"

#include <QApplication>
#include <QDBusConnectionInterface>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>
#include <memory>

namespace {
constexpr auto appName = "io.github.LinuxExplorer.Win7";
constexpr auto sharedName = "org.freedesktop.FileManager1";
constexpr auto objectPath = "/org/freedesktop/FileManager1";
}

class FileManagerServiceTest : public QObject {
    Q_OBJECT
    std::unique_ptr<FileManagerService> m_service;

private Q_SLOTS:
    void init()
    {
        QApplication::setQuitOnLastWindowClosed(false);
        QVERIFY(QDBusConnection::sessionBus().isConnected());
        // The launcher is the Explorer binary, not this test, so it is the
        // executable allowed to register the shared name, and calls made from
        // this process are those of an outside peer
        FileManagerService::setTrustedExecutable(QStringLiteral(EXPLORER_BINARY));
        m_service = std::make_unique<FileManagerService>();
        QVERIFY(m_service->claim());
        QVERIFY(!m_service->ownsSharedName());
    }

    void cleanup()
    {
        QList<MainWindow *> windows;
        for (QWidget *widget : QApplication::topLevelWidgets()) {
            if (auto *window = qobject_cast<MainWindow *>(widget))
                windows.append(window);
        }
        qDeleteAll(windows);
        auto bus = QDBusConnection::sessionBus();
        bus.unregisterService(sharedName);
        bus.unregisterService(appName);
        bus.unregisterObject(objectPath);
        m_service.reset();
        FileManagerService::setTrustedExecutable({});
    }

    void outsidePeerCannotRegisterSharedName()
    {
        auto message = QDBusMessage::createMethodCall(appName, objectPath, appName, "RegisterFileManager");
        // A connection of its own, as an outside app has: Qt delivers a call to
        // a name this same connection owns locally, never through the bus
        auto peer = QDBusConnection::connectToBus(QDBusConnection::SessionBus,
                                                  QStringLiteral("outsidePeer"));
        const auto disconnect = qScopeGuard([] {
            QDBusConnection::disconnectFromBus(QStringLiteral("outsidePeer"));
        });
        QVERIFY(peer.isConnected());
        QDBusPendingCallWatcher reply(peer.asyncCall(message));
        QTRY_VERIFY(reply.isFinished());
        QCOMPARE(reply.reply().type(), QDBusMessage::ErrorMessage);
        QCOMPARE(reply.reply().errorName(), QDBusError::errorString(QDBusError::AccessDenied));
        QVERIFY(!m_service->ownsSharedName());
    }

    void outsidePeerCannotReachUnspecifiedSlots()
    {
        auto message = QDBusMessage::createMethodCall(appName, objectPath, QString(), "deleteLater");
        QDBusPendingCallWatcher reply(QDBusConnection::sessionBus().asyncCall(message));
        QTRY_VERIFY(reply.isFinished());
        QCOMPARE(reply.reply().type(), QDBusMessage::ErrorMessage);
        QVERIFY(m_service);
    }

    void outsidePeerCannotOpenRemoteSchemes()
    {
        QTemporaryDir files;
        QVERIFY(files.isValid());
        for (const QString &uri : {QStringLiteral("smb://host/share"), QStringLiteral("sftp://host/home"),
                                   QStringLiteral("fish://host/"), QStringLiteral("admin:///etc"),
                                   QStringLiteral("file://remote-host/etc")}) {
            auto message = QDBusMessage::createMethodCall(appName, objectPath, sharedName, "ShowFolders");
            message.setArguments({QStringList{uri}, QString()});
            QDBusPendingCallWatcher reply(QDBusConnection::sessionBus().asyncCall(message));
            QTRY_VERIFY(reply.isFinished());
            QCOMPARE(reply.reply().type(), QDBusMessage::ReplyMessage);
            QCOMPARE(MainWindow::openWindowCount(), 0);
        }

        // A local folder from the same peer is still honoured
        auto message = QDBusMessage::createMethodCall(appName, objectPath, sharedName, "ShowFolders");
        message.setArguments({QStringList{QUrl::fromLocalFile(files.path()).toString(QUrl::FullyEncoded)},
                              QString(QStringLiteral("bad token\n"))});
        QDBusPendingCallWatcher reply(QDBusConnection::sessionBus().asyncCall(message));
        QTRY_VERIFY(reply.isFinished());
        QCOMPARE(reply.reply().type(), QDBusMessage::ReplyMessage);
        QCOMPARE(MainWindow::openWindowCount(), 1);
    }

    void listsAreCapped()
    {
        QTemporaryDir files;
        QVERIFY(files.isValid());
        QStringList uris;
        for (int i = 0; i < FileManagerService::maxUris + 4; ++i) {
            const QString folder = files.filePath(QString::number(i));
            QVERIFY(QDir().mkpath(folder));
            uris.append(QUrl::fromLocalFile(folder).toString(QUrl::FullyEncoded));
        }
        auto message = QDBusMessage::createMethodCall(appName, objectPath, sharedName, "ShowFolders");
        message.setArguments({uris, QString()});
        QDBusPendingCallWatcher reply(QDBusConnection::sessionBus().asyncCall(message));
        QTRY_VERIFY(reply.isFinished());
        QCOMPARE(reply.reply().type(), QDBusMessage::ReplyMessage);
        QCOMPARE(MainWindow::openWindowCount(), FileManagerService::maxUris);
    }

    void activationLaunchRegistersExistingInstanceAndRevealsFiles()
    {
        // This is the executable and argv used by the desktop activation entry.
        // The primary instance must acquire the shared name without opening
        // Libraries for the launch's empty argument list.
        QProcess activation;
        auto environment = QProcessEnvironment::systemEnvironment();
        // Forwarding must work without loading any GUI platform or theme.
        environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("explorer-test-no-gui"));
        activation.setProcessEnvironment(environment);
        activation.start(QStringLiteral(EXPLORER_BINARY), {"--register-file-manager"});
        QVERIFY(activation.waitForStarted());
        QTRY_COMPARE_WITH_TIMEOUT(activation.state(), QProcess::NotRunning, 10000);
        QCOMPARE(activation.exitCode(), 0);
        QVERIFY(m_service->ownsSharedName());
        QCOMPARE(QDBusConnection::sessionBus().interface()->serviceOwner(sharedName).value(),
                 QDBusConnection::sessionBus().baseService());
        QCOMPARE(MainWindow::openWindowCount(), 0);

        QTemporaryDir files;
        QVERIFY(files.isValid());
        const QString folder = files.filePath(QStringLiteral("Downloads # 50% Ω"));
        QVERIFY(QDir().mkpath(folder));
        QList<QUrl> urls;
        QStringList uris;
        for (const QString &name : {QStringLiteral("Report # 50%.txt"), QStringLiteral("Notes Ω.txt")}) {
            QFile file(folder + '/' + name);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.close();
            urls.append(QUrl::fromLocalFile(file.fileName()));
            uris.append(urls.last().toString(QUrl::FullyEncoded));
        }

        auto message = QDBusMessage::createMethodCall(sharedName, objectPath, sharedName, "ShowItems");
        message.setArguments({uris, QString()});
        QDBusPendingCallWatcher reply(QDBusConnection::sessionBus().asyncCall(message));
        QTRY_VERIFY(reply.isFinished());
        QCOMPARE(reply.reply().type(), QDBusMessage::ReplyMessage);
        QCOMPARE(MainWindow::openWindowCount(), 1);
        MainWindow *window = nullptr;
        for (QWidget *widget : QApplication::topLevelWidgets()) {
            if (auto *candidate = qobject_cast<MainWindow *>(widget))
                window = candidate;
        }
        QVERIFY(window);
        auto *model = window->findChild<DirectoryModel *>();
        auto *view = window->findChild<FileView *>();
        QVERIFY(model && view);
        QTRY_COMPARE(model->rowCount(), 2);
        QTRY_COMPARE(model->itemsForIndexes(view->selectedIndexes()).size(), 2);
        for (const auto &item : model->itemsForIndexes(view->selectedIndexes()))
            QVERIFY(urls.contains(item.url()));

        QProcess reveal;
        reveal.setProcessEnvironment(environment);
        reveal.start(QStringLiteral(EXPLORER_BINARY), {"--select", urls.first().toLocalFile()});
        QVERIFY(reveal.waitForStarted());
        QTRY_COMPARE_WITH_TIMEOUT(reveal.state(), QProcess::NotRunning, 10000);
        QCOMPARE(reveal.exitCode(), 0);
        QCOMPARE(MainWindow::openWindowCount(), 1);
        QTRY_COMPARE(model->itemsForIndexes(view->selectedIndexes()).size(), 1);
        QCOMPARE(model->itemsForIndexes(view->selectedIndexes()).first().url(), urls.first());

        // A repeated reveal reuses that folder instead of opening another.
        QDBusPendingCallWatcher repeated(QDBusConnection::sessionBus().asyncCall(message));
        QTRY_VERIFY(repeated.isFinished());
        QCOMPARE(repeated.reply().type(), QDBusMessage::ReplyMessage);
        QCOMPARE(MainWindow::openWindowCount(), 1);
    }

    void registrationPreservesAnotherFileManager()
    {
        auto other = QDBusConnection::connectToBus(QDBusConnection::SessionBus, "other-file-manager");
        QVERIFY(other.registerService(sharedName));
        QProcess launch;
        launch.start(QStringLiteral(EXPLORER_BINARY), {"--register-file-manager"});
        QVERIFY(launch.waitForStarted());
        QTRY_COMPARE_WITH_TIMEOUT(launch.state(), QProcess::NotRunning, 10000);
        QCOMPARE(launch.exitCode(), 0);
        QVERIFY(!m_service->ownsSharedName());
        QCOMPARE(QDBusConnection::sessionBus().interface()->serviceOwner(sharedName).value(),
                 other.baseService());
        QCOMPARE(MainWindow::openWindowCount(), 0);
        other.unregisterService(sharedName);
        QDBusConnection::disconnectFromBus("other-file-manager");
    }

    void coldLaunchStartsGui_data()
    {
        QTest::addColumn<bool>("activation");
        QTest::newRow("command-line") << false;
        QTest::newRow("bus-activation") << true;
    }

    void coldLaunchStartsGui()
    {
        QFETCH(bool, activation);
        auto bus = QDBusConnection::sessionBus();
        bus.unregisterService(appName);
        bus.unregisterObject(objectPath);
        QTemporaryDir files;
        QVERIFY(files.isValid());
        QFile file(files.filePath(QStringLiteral("Report # 50% Ω.txt")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();

        QProcess browser;
        const auto stop = qScopeGuard([&browser] {
            if (browser.state() != QProcess::NotRunning) {
                browser.terminate();
                if (!browser.waitForFinished(5000)) {
                    browser.kill();
                    browser.waitForFinished();
                }
            }
        });
        auto environment = QProcessEnvironment::systemEnvironment();
        QStringList arguments{QStringLiteral("--register-file-manager")};
        if (activation)
            environment.insert(QStringLiteral("DBUS_STARTER_BUS_TYPE"), QStringLiteral("session"));
        else
            arguments << QStringLiteral("--select") << file.fileName();
        browser.setProcessEnvironment(environment);
        browser.start(QStringLiteral(EXPLORER_BINARY), arguments);
        QVERIFY(browser.waitForStarted());
        QTRY_VERIFY_WITH_TIMEOUT(bus.interface()->isServiceRegistered(sharedName).value(), 10000);
        QVERIFY(bus.interface()->serviceOwner(sharedName).value() != bus.baseService());

        auto message = QDBusMessage::createMethodCall(sharedName, objectPath, sharedName, "ShowItems");
        message.setArguments({QStringList{QUrl::fromLocalFile(file.fileName()).toString(QUrl::FullyEncoded)},
                              QString()});
        QDBusPendingCallWatcher reply(bus.asyncCall(message));
        QTRY_VERIFY_WITH_TIMEOUT(reply.isFinished(), 10000);
        QCOMPARE(reply.reply().type(), QDBusMessage::ReplyMessage);
        QCOMPARE(browser.state(), QProcess::Running);
    }
};

QTEST_MAIN(FileManagerServiceTest)
#include "FileManagerServiceTest.moc"
