#include "ChooserService.h"
#include "ChooserDBusTypes.h"
#include "ChooserOptions.h"
#include "FileChooserDialog.h"

#include <QApplication>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusPendingCallWatcher>
#include <QDBusVariant>
#include <QDialogButtonBox>
#include <QEventLoop>
#include <QFile>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

namespace {
const QString serviceName = QStringLiteral("org.win7explorer.FileChooser1");
const QString servicePath = QStringLiteral("/org/win7explorer/FileChooser1");
const QString requestInterface = QStringLiteral("org.win7explorer.FileChooser1.Request");

QDBusMessage call(const QDBusConnection &connection, const QString &path,
                  const QString &interface, const QString &method, const QList<QVariant> &arguments = {})
{
    QDBusMessage message = QDBusMessage::createMethodCall(serviceName, path, interface, method);
    message.setArguments(arguments);
    QDBusPendingCallWatcher watcher(connection.asyncCall(message, 5000));
    QEventLoop loop;
    QObject::connect(&watcher, &QDBusPendingCallWatcher::finished, &loop, &QEventLoop::quit);
    if (!watcher.isFinished())
        loop.exec();
    return watcher.reply();
}

QVariantMap result(const QDBusConnection &connection, const QString &path)
{
    const QDBusMessage reply = call(connection, path, requestInterface, QStringLiteral("GetResult"));
    return reply.type() == QDBusMessage::ReplyMessage && reply.arguments().size() == 1
        ? qdbus_cast<QVariantMap>(reply.arguments().first()) : QVariantMap();
}

QString request(const QDBusConnection &connection, const QVariantMap &options = {}, bool save = false)
{
    const QDBusMessage reply = call(connection, servicePath, serviceName,
        save ? QStringLiteral("SaveFile") : QStringLiteral("OpenFile"), {options});
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().size() != 1)
        return {};
    return qdbus_cast<QDBusObjectPath>(reply.arguments().first()).path();
}
} // namespace

class ChooserSignalRecorder : public QObject {
    Q_OBJECT
public:
    QList<uint> statuses;
    QList<QVariantMap> results;
public Q_SLOTS:
    void finished(uint status, const QVariantMap &value)
    {
        statuses.append(status);
        results.append(value);
    }
};

class ChooserServiceTest : public QObject {
    Q_OBJECT
private:
    QDBusConnection m_bus{QStringLiteral("invalid")};
    QDBusConnection m_owner{QStringLiteral("invalid")};
    QDBusConnection m_other{QStringLiteral("invalid")};
    ChooserService *m_service = nullptr;
    int m_serial = 0;
    QString m_suffix;

    void subscribe(QDBusConnection bus, const QString &path, ChooserSignalRecorder *recorder)
    {
        QVERIFY(bus.connect(serviceName, path, requestInterface, QStringLiteral("Finished"),
                            recorder, SLOT(finished(uint,QVariantMap))));
    }
    void close(const QDBusConnection &bus, const QString &path)
    {
        QCOMPARE(call(bus, path, requestInterface, QStringLiteral("Close")).type(), QDBusMessage::ReplyMessage);
    }

private Q_SLOTS:
    void initTestCase()
    {
        QApplication::setQuitOnLastWindowClosed(false);
        QApplication::setDesktopFileName(QStringLiteral("linux-explorer-win7-dialog"));
        registerChooserDBusTypes();
    }
    void init()
    {
        m_suffix = QString::number(++m_serial);
        m_bus = QDBusConnection::connectToBus(QDBusConnection::SessionBus, QStringLiteral("chooser-service-") + m_suffix);
        m_owner = QDBusConnection::connectToBus(QDBusConnection::SessionBus, QStringLiteral("chooser-owner-") + m_suffix);
        m_other = QDBusConnection::connectToBus(QDBusConnection::SessionBus, QStringLiteral("chooser-other-") + m_suffix);
        QVERIFY(m_bus.isConnected());
        QVERIFY(m_owner.isConnected());
        QVERIFY(m_other.isConnected());
        QVERIFY(m_owner.baseService() != m_other.baseService());
        m_service = new ChooserService(m_bus);
        QVERIFY(m_service->claim());
    }
    void cleanup()
    {
        delete m_service;
        m_service = nullptr;
        m_bus = QDBusConnection(QStringLiteral("invalid"));
        m_owner = QDBusConnection(QStringLiteral("invalid"));
        m_other = QDBusConnection(QStringLiteral("invalid"));
        for (const QString &prefix : {QStringLiteral("chooser-service-"), QStringLiteral("chooser-owner-"), QStringLiteral("chooser-other-")})
            QDBusConnection::disconnectFromBus(prefix + m_suffix);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    void versionAndIntrospection()
    {
        // The service's static target must retain resource-only UI objects too.
        QVERIFY(QFile::exists(QStringLiteral(":/win7/commandbar.png")));
        QCOMPARE(QByteArray(QDBusMetaType::typeToSignature(QMetaType::fromType<ChooserWireFilters>())), QByteArray("a(ssasas)"));
        const QDBusMessage reply = call(m_owner, servicePath, QStringLiteral("org.freedesktop.DBus.Properties"),
                                        QStringLiteral("Get"), {serviceName, QStringLiteral("Version")});
        QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
        const QVariant version = qvariant_cast<QDBusVariant>(reply.arguments().first()).variant();
        QCOMPARE(version.metaType(), QMetaType::fromType<uint>());
        QCOMPARE(version.toUInt(), 1u);
        const QString path = request(m_owner);
        QVERIFY(!path.isEmpty());
        const auto introspection = call(m_owner, path, QStringLiteral("org.freedesktop.DBus.Introspectable"), QStringLiteral("Introspect"));
        const QString xml = introspection.arguments().first().toString();
        QVERIFY(xml.contains(QStringLiteral("name=\"Finished\"")));
        QVERIFY(!xml.contains(QStringLiteral("name=\"deleteLater\"")));
        QVERIFY(!xml.contains(QStringLiteral("name=\"start\"")));
    }
    void failedRequestCanBeRecoveredAfterMissedSignal()
    {
        const QString path = request(m_owner, {{QStringLiteral("local_only"), false}});
        QVERIFY(path.startsWith(servicePath + QStringLiteral("/requests/r")));
        QTRY_COMPARE(result(m_owner, path).value(QStringLiteral("state")).toString(), QStringLiteral("failed"));
        ChooserSignalRecorder late;
        subscribe(m_owner, path, &late);
        const QVariantMap snapshot = result(m_owner, path);
        QCOMPARE(snapshot.value(QStringLiteral("status")).metaType(), QMetaType::fromType<uint>());
        QCOMPARE(snapshot.value(QStringLiteral("status")).toUInt(), 2u);
        QVERIFY(!snapshot.value(QStringLiteral("error_name")).toString().isEmpty());
        QVERIFY(!snapshot.value(QStringLiteral("error_message")).toString().isEmpty());
        QVERIFY(!m_service->dialogForRequest(path));
        QTest::qWait(20);
        QCOMPARE(late.statuses.size(), 0);
        close(m_owner, path);
        QCOMPARE(m_service->requestCount(), 0);
    }
    void invalidOptions_data()
    {
        QTest::addColumn<QVariantMap>("options");
        QTest::addColumn<bool>("save");
        QTest::newRow("unknown") << QVariantMap{{QStringLiteral("unknown"), true}} << false;
        QTest::newRow("wrong-bool") << QVariantMap{{QStringLiteral("multiple"), 1}} << false;
        QTest::newRow("wrong-string") << QVariantMap{{QStringLiteral("title"), true}} << false;
        QTest::newRow("remote-file-host") << QVariantMap{{QStringLiteral("starting_location"), QStringLiteral("file://remote/tmp/a")}} << false;
        QTest::newRow("remote-scheme") << QVariantMap{{QStringLiteral("starting_location"), QStringLiteral("smb://host/share")}} << false;
        QTest::newRow("relative-path") << QVariantMap{{QStringLiteral("starting_location"), QStringLiteral("tmp/a")}} << false;
        QTest::newRow("unencoded-space") << QVariantMap{{QStringLiteral("starting_location"), QStringLiteral("file:///tmp/a b")}} << false;
        QTest::newRow("invalid-escape") << QVariantMap{{QStringLiteral("starting_location"), QStringLiteral("file:///tmp/%zz")}} << false;
        QTest::newRow("query") << QVariantMap{{QStringLiteral("starting_location"), QStringLiteral("file:///tmp?a")}} << false;
        QTest::newRow("no-parent-modal") << QVariantMap{{QStringLiteral("modal"), true}} << false;
        QTest::newRow("wrong-parent") << QVariantMap{{QStringLiteral("parent_window"), QStringLiteral("invalid:123")}} << false;
        QTest::newRow("zero-x11-parent") << QVariantMap{{QStringLiteral("parent_window"), QStringLiteral("x11:0")}} << false;
        QTest::newRow("empty-wayland-parent") << QVariantMap{{QStringLiteral("parent_window"), QStringLiteral("wayland:")}} << false;
        QTest::newRow("open-multiple") << QVariantMap{{QStringLiteral("multiple"), true}} << false;
        QTest::newRow("open-directory") << QVariantMap{{QStringLiteral("directory"), true}} << false;
        QTest::newRow("save-multiple") << QVariantMap{{QStringLiteral("multiple"), true}} << true;
        QTest::newRow("save-directory") << QVariantMap{{QStringLiteral("directory"), true}} << true;
        QTest::newRow("leading-dot-extension") << QVariantMap{{QStringLiteral("default_extension"), QStringLiteral(".txt")}} << true;
        QTest::newRow("non-leaf") << QVariantMap{{QStringLiteral("suggested_name"), QStringLiteral("../a")}} << true;
        QTest::newRow("wrong-filter-wire-type") << QVariantMap{{QStringLiteral("filters"), QStringList{QStringLiteral("*.txt")}}} << false;
        const ChooserWireFilter text{QStringLiteral("text"), QStringLiteral("Text"), {QStringLiteral("*.txt")}, {QStringLiteral("text/plain")}};
        QTest::newRow("duplicate-filter-id") << QVariantMap{{QStringLiteral("filters"), QVariant::fromValue(ChooserWireFilters{text, text})}} << false;
        QTest::newRow("unknown-filter-id") << QVariantMap{{QStringLiteral("selected_filter"), QStringLiteral("missing")}} << false;
        QTest::newRow("unknown-mime") << QVariantMap{{QStringLiteral("filters"), QVariant::fromValue(ChooserWireFilters{{QStringLiteral("a"), QStringLiteral("A"), {}, {QStringLiteral("not/a-real-mime-type")}}})}} << false;
        QTest::newRow("too-many-filters") << QVariantMap{{QStringLiteral("filters"), QVariant::fromValue(ChooserWireFilters(65, text))}} << false;
        QTest::newRow("oversized-title") << QVariantMap{{QStringLiteral("title"), QString(4097, QLatin1Char('x'))}} << false;
    }
    void invalidOptions()
    {
        QFETCH(QVariantMap, options);
        QFETCH(bool, save);
        const QString path = request(m_owner, options, save);
        QVERIFY(!path.isEmpty());
        QTRY_COMPARE(result(m_owner, path).value(QStringLiteral("status")).toUInt(), 2u);
        QVERIFY(!m_service->dialogForRequest(path));
    }
    void backendMismatch()
    {
        // The suite's offscreen backend has no native foreign-parent protocol.
        if (QGuiApplication::platformName() != QLatin1String("offscreen"))
            QSKIP("Offscreen-only backend compatibility check");
        for (const QString &parent : {QStringLiteral("x11:123"), QStringLiteral("wayland:exported-token")}) {
            const QString path = request(m_owner, {{QStringLiteral("parent_window"), parent}});
            QTRY_COMPARE(result(m_owner, path).value(QStringLiteral("status")).toUInt(), 2u);
            QVERIFY(!m_service->dialogForRequest(path));
        }
    }
    void cancelIsOnceOnlyDirectedAndRetained()
    {
        const QString path = request(m_owner);
        QVERIFY(!path.isEmpty());
        QTRY_VERIFY(m_service->dialogForRequest(path));
        QCOMPARE(m_service->dialogForRequest(path)->windowModality(), Qt::NonModal);
        ChooserSignalRecorder owner, other;
        subscribe(m_owner, path, &owner);
        subscribe(m_other, path, &other);
        QCOMPARE(call(m_owner, path, requestInterface, QStringLiteral("Cancel")).type(), QDBusMessage::ReplyMessage);
        QTRY_COMPARE(owner.statuses.size(), 1);
        QCOMPARE(owner.statuses.first(), 1u);
        QCOMPARE(owner.results.first().value(QStringLiteral("state")).toString(), QStringLiteral("canceled"));
        QCOMPARE(call(m_owner, path, requestInterface, QStringLiteral("Cancel")).type(), QDBusMessage::ReplyMessage);
        QTest::qWait(20);
        QCOMPARE(owner.statuses.size(), 1);
        QCOMPARE(other.statuses.size(), 0);
        QCOMPARE(result(m_owner, path).value(QStringLiteral("status")).toUInt(), 1u);
        QCOMPARE(m_service->requestCount(), 1);
        close(m_owner, path);
        QCOMPARE(m_service->requestCount(), 0);
    }
    void unauthorizedMethodsCannotChangeOrReadRequest()
    {
        const QString path = request(m_owner);
        QTRY_VERIFY(m_service->dialogForRequest(path));
        for (const QString &method : {QStringLiteral("Cancel"), QStringLiteral("Close"), QStringLiteral("GetResult")}) {
            const auto reply = call(m_other, path, requestInterface, method);
            QCOMPARE(reply.type(), QDBusMessage::ErrorMessage);
            QCOMPARE(reply.errorName(), QStringLiteral("org.freedesktop.DBus.Error.AccessDenied"));
        }
        QCOMPARE(result(m_owner, path).value(QStringLiteral("state")).toString(), QStringLiteral("pending"));
        QVERIFY(m_service->dialogForRequest(path));
    }
    void concurrentOwnersAndDisconnect()
    {
        const QString first = request(m_owner, {{QStringLiteral("title"), QStringLiteral("First owner")}});
        const QString second = request(m_other, {{QStringLiteral("title"), QStringLiteral("Other owner")}});
        const QString retainedFailure = request(m_owner, {{QStringLiteral("local_only"), false}});
        QTRY_COMPARE(result(m_owner, retainedFailure).value(QStringLiteral("status")).toUInt(), 2u);
        QVERIFY(first != second);
        QTRY_VERIFY(m_service->dialogForRequest(first));
        QTRY_VERIFY(m_service->dialogForRequest(second));
        QVERIFY(m_service->dialogForRequest(first) != m_service->dialogForRequest(second));
        QPointer<FileChooserDialog> firstDialog = m_service->dialogForRequest(first);
        QPointer<FileChooserDialog> secondDialog = m_service->dialogForRequest(second);
        m_owner = QDBusConnection(QStringLiteral("invalid"));
        QDBusConnection::disconnectFromBus(QStringLiteral("chooser-owner-") + m_suffix);
        QTRY_COMPARE(m_service->requestCount(), 1);
        QTRY_VERIFY(firstDialog.isNull());
        QVERIFY(secondDialog);
        QCOMPARE(result(m_other, second).value(QStringLiteral("state")).toString(), QStringLiteral("pending"));
        close(m_other, second);
        QTRY_VERIFY(secondDialog.isNull());
        QCOMPARE(m_service->requestCount(), 0);
    }
    void pendingCloseCancelsAndCleansUp()
    {
        const QString path = request(m_owner);
        QTRY_VERIFY(m_service->dialogForRequest(path));
        QPointer<FileChooserDialog> dialog = m_service->dialogForRequest(path);
        ChooserSignalRecorder recorder;
        subscribe(m_owner, path, &recorder);
        close(m_owner, path);
        QTRY_COMPARE(recorder.statuses.size(), 1);
        QCOMPARE(recorder.statuses.first(), 1u);
        QTRY_VERIFY(dialog.isNull());
        QCOMPARE(m_service->requestCount(), 0);
        QCOMPARE(call(m_owner, path, requestInterface, QStringLiteral("GetResult")).type(), QDBusMessage::ErrorMessage);
    }
    void ownerRequestLimitIsRecoverableByClose()
    {
        QStringList paths;
        for (int i = 0; i < 8; ++i) {
            const QString path = request(m_owner, {{QStringLiteral("local_only"), false}});
            QVERIFY(!path.isEmpty());
            paths.append(path);
        }
        const auto reply = call(m_owner, servicePath, serviceName, QStringLiteral("OpenFile"), {QVariantMap()});
        QCOMPARE(reply.errorName(), QStringLiteral("org.freedesktop.DBus.Error.LimitsExceeded"));
        close(m_owner, paths.takeFirst());
        QVERIFY(!request(m_owner, {{QStringLiteral("local_only"), false}}).isEmpty());
        // Another owner has its own allocation even when the first is full.
        QVERIFY(!request(m_other).isEmpty());
    }
    void cancelRejectsNestedConfirmation()
    {
        const QString path = request(m_owner);
        QTRY_VERIFY(m_service->dialogForRequest(path));
        QPointer<FileChooserDialog> dialog = m_service->dialogForRequest(path);
        auto *confirmation = new QMessageBox(QMessageBox::Warning, QStringLiteral("Overwrite"), QStringLiteral("Replace?"),
                                              QMessageBox::Yes | QMessageBox::No, dialog);
        QPointer<QMessageBox> guardedConfirmation = confirmation;
        ChooserSignalRecorder recorder;
        subscribe(m_owner, path, &recorder);
        QTimer::singleShot(0, this, [this, path] {
            auto message = QDBusMessage::createMethodCall(serviceName, path, requestInterface, QStringLiteral("Cancel"));
            m_owner.asyncCall(message);
        });
        QCOMPARE(confirmation->exec(), int(QDialog::Rejected));
        QTRY_COMPARE(recorder.statuses.size(), 1);
        QCOMPARE(recorder.statuses.first(), 1u);
        QTRY_VERIFY(dialog.isNull());
        QVERIFY(guardedConfirmation.isNull());
        QCOMPARE(result(m_owner, path).value(QStringLiteral("state")).toString(), QStringLiteral("canceled"));
    }
    void cancelActualSaveOverwriteDoesNotWrite()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QFile file(directory.filePath(QStringLiteral("existing.txt")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("keep this content");
        file.close();
        const QString path = request(m_owner, {
            {QStringLiteral("starting_location"), QUrl::fromLocalFile(directory.path()).toString(QUrl::FullyEncoded)},
            {QStringLiteral("suggested_name"), QStringLiteral("existing.txt")}}, true);
        QTRY_VERIFY(m_service->dialogForRequest(path));
        QPointer<FileChooserDialog> dialog = m_service->dialogForRequest(path);
        auto *buttons = dialog->findChild<QDialogButtonBox *>();
        QVERIFY(buttons);
        QAbstractButton *accept = nullptr;
        for (QAbstractButton *button : buttons->buttons()) {
            if (buttons->buttonRole(button) == QDialogButtonBox::AcceptRole)
                accept = button;
        }
        QVERIFY(accept);
        bool sawConfirmation = false;
        QTimer poll;
        connect(&poll, &QTimer::timeout, this, [&] {
            if (!dialog)
                return;
            for (QDialog *child : dialog->findChildren<QDialog *>()) {
                if (child->isVisible()) {
                    sawConfirmation = true;
                    poll.stop();
                    // Do not add another nested loop to the application under
                    // test: the real external caller also cancels asynchronously.
                    auto message = QDBusMessage::createMethodCall(serviceName, path, requestInterface, QStringLiteral("Cancel"));
                    m_owner.asyncCall(message);
                    break;
                }
            }
        });
        poll.start(10);
        QTimer failsafe;
        failsafe.setSingleShot(true);
        connect(&failsafe, &QTimer::timeout, this, [&] {
            auto message = QDBusMessage::createMethodCall(serviceName, path, requestInterface, QStringLiteral("Cancel"));
            m_owner.asyncCall(message);
        });
        failsafe.start(5000);
        QTest::mouseClick(accept, Qt::LeftButton);
        QTRY_COMPARE(result(m_owner, path).value(QStringLiteral("state")).toString(), QStringLiteral("canceled"));
        QVERIFY2(sawConfirmation, "Actual SaveFile must ask before overwriting an existing file");
        QTRY_VERIFY(dialog.isNull());
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("keep this content"));
    }
    void acceptedSelectionIsEncodedAndIndependent()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString filename = QStringLiteral("a space #% é.txt");
        const QString filePath = directory.filePath(filename);
        QFile file(filePath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("original");
        file.close();
        const ChooserWireFilters filters{{QStringLiteral("text"), QStringLiteral("Text"), {QStringLiteral("*.txt")}, {QStringLiteral("text/plain")}}};
        const QString path = request(m_owner, {
            {QStringLiteral("starting_location"), QUrl::fromLocalFile(directory.path()).toString(QUrl::FullyEncoded)},
            {QStringLiteral("suggested_name"), filename},
            {QStringLiteral("filters"), QVariant::fromValue(filters)},
            {QStringLiteral("selected_filter"), QStringLiteral("text")}});
        const QString otherPath = request(m_other);
        QTRY_VERIFY(m_service->dialogForRequest(path));
        QPointer<FileChooserDialog> dialog = m_service->dialogForRequest(path);
        ChooserSignalRecorder recorder;
        subscribe(m_owner, path, &recorder);
        // Enter the leaf in the actual chooser, then click its real accept button.
        QLineEdit *name = nullptr;
        for (QLineEdit *edit : dialog->findChildren<QLineEdit *>()) {
            if (edit->text() == filename)
                name = edit;
        }
        QVERIFY2(name, "Chooser must display suggested_name in its file-name editor");
        auto *buttons = dialog->findChild<QDialogButtonBox *>();
        QVERIFY(buttons);
        QPushButton *accept = nullptr;
        for (QAbstractButton *button : buttons->buttons()) {
            if (buttons->buttonRole(button) == QDialogButtonBox::AcceptRole)
                accept = qobject_cast<QPushButton *>(button);
        }
        QVERIFY(accept);
        QTest::mouseClick(accept, Qt::LeftButton);
        QTRY_COMPARE(recorder.statuses.size(), 1);
        QCOMPARE(recorder.statuses.first(), 0u);
        const auto snapshot = result(m_owner, path);
        QCOMPARE(snapshot.value(QStringLiteral("uris")).toStringList(), QStringList{QUrl::fromLocalFile(filePath).toString(QUrl::FullyEncoded)});
        QCOMPARE(snapshot.value(QStringLiteral("selected_filter")).toString(), QStringLiteral("text"));
        QCOMPARE(result(m_other, otherPath).value(QStringLiteral("state")).toString(), QStringLiteral("pending"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("original"));
    }
};

QTEST_MAIN(ChooserServiceTest)
#include "ChooserServiceTest.moc"
