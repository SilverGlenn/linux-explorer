#include "GoogleDriveSecretStore.h"

#include <QDBusConnection>
#include <QDBusContext>
#include <QDBusObjectPath>
#include <QDBusMetaType>
#include <QDBusArgument>
#include <QDBusError>
#include <QDBusVariant>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTest>

using Attributes = QMap<QString, QString>;
using Paths = QList<QDBusObjectPath>;
Q_DECLARE_METATYPE(Attributes)
struct FixtureSecret {
    QDBusObjectPath session{QStringLiteral("/")};
    QByteArray parameters;
    QByteArray value;
    QString contentType{QStringLiteral("text/plain; charset=utf8")};
};
Q_DECLARE_METATYPE(FixtureSecret)
QDBusArgument &operator<<(QDBusArgument &argument, const FixtureSecret &secret)
{
    argument.beginStructure();
    argument << secret.session << secret.parameters << secret.value << secret.contentType;
    argument.endStructure();
    return argument;
}
const QDBusArgument &operator>>(const QDBusArgument &argument, FixtureSecret &secret)
{
    argument.beginStructure();
    argument >> secret.session >> secret.parameters >> secret.value >> secret.contentType;
    argument.endStructure();
    return argument;
}
using SecretMap = QMap<QDBusObjectPath, FixtureSecret>;
Q_DECLARE_METATYPE(SecretMap)

class LockedItem final : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Secret.Item")
    Q_PROPERTY(bool Locked READ locked)
    Q_PROPERTY(Attributes Attributes READ attributes)
    Q_PROPERTY(QString Label READ label)
    Q_PROPERTY(qulonglong Created READ created)
    Q_PROPERTY(qulonglong Modified READ created)
public:
    bool isLocked = true;
    QString user = QStringLiteral("fixture-only-account");
    int loads = 0;
    QByteArray record = "isolated account fixture";
    bool locked() const { return isLocked; }
    Attributes attributes() const { return {{QStringLiteral("user"), user},
        {QStringLiteral("server"), QStringLiteral("explorer-win7-google-drive")}, {QStringLiteral("type"), QStringLiteral("base64")}}; }
    QString label() const { return QStringLiteral("Isolated fixture account"); }
    qulonglong created() const { return 1; }
public Q_SLOTS:
    FixtureSecret GetSecret(const QDBusObjectPath &session)
    {
        ++loads;
        if (isLocked)
            sendErrorReply(QStringLiteral("org.freedesktop.Secret.Error.IsLocked"), QStringLiteral("Fixture is locked"));
        return {session, {}, record.toBase64(), QStringLiteral("text/plain; charset=utf8")};
    }
};

class SecretServiceFixture final : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Secret.Service")
    Q_PROPERTY(Paths Collections READ collections)
public:
    int searches = 0;
    int unlocks = 0;
    int sessions = 0;
    int secretLoads = 0;
    QByteArray record = "isolated account fixture";
    Attributes requested;
    bool legacyOnly = false;
    Paths collections() const { return {}; }
public Q_SLOTS:
    Paths SearchItems(const Attributes &attributes, Paths &locked)
    {
        ++searches;
        requested = attributes;
        if (legacyOnly) {
            if (attributes.value(QStringLiteral("user")) == QStringLiteral("desktop-account-v1"))
                return {QDBusObjectPath(QStringLiteral("/org/freedesktop/secrets/collection/fixture/ready"))};
            return {};
        }
        if (attributes.value(QStringLiteral("user")) == QStringLiteral("fixture-unlocked-account"))
            return {QDBusObjectPath(QStringLiteral("/org/freedesktop/secrets/collection/fixture/ready"))};
        locked = {QDBusObjectPath(QStringLiteral("/org/freedesktop/secrets/collection/fixture/locked"))};
        return {};
    }
    Paths Unlock(const Paths &, QDBusObjectPath &prompt)
    {
        ++unlocks;
        prompt = QDBusObjectPath(QStringLiteral("/org/freedesktop/secrets/prompt/forbidden"));
        return {};
    }
    QDBusVariant OpenSession(const QString &algorithm, const QDBusVariant &, QDBusObjectPath &session)
    {
        if (algorithm != QStringLiteral("plain")) {
            sendErrorReply(QDBusError::NotSupported, QStringLiteral("Isolated fixture supports the plain test session only"));
            return QDBusVariant(QString());
        }
        ++sessions;
        session = QDBusObjectPath(QStringLiteral("/org/freedesktop/secrets/session/fixture"));
        return QDBusVariant(QString());
    }
    SecretMap GetSecrets(const Paths &items, const QDBusObjectPath &session)
    {
        ++secretLoads;
        SecretMap result;
        for (const auto &path : items)
            result.insert(path, {session, {}, record.toBase64(), QStringLiteral("text/plain; charset=utf8")});
        return result;
    }
};

class GoogleDriveSecretStoreTest final : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void lockedWalletIsReadWithoutCallingUnlockOrPrompt()
    {
        // run-ui-tests.sh supplies an isolated bus and empty home. Refuse to
        // replace any service even there: this test must never use a real wallet.
        qDBusRegisterMetaType<Attributes>();
        qDBusRegisterMetaType<Paths>();
        qDBusRegisterMetaType<FixtureSecret>();
        qDBusRegisterMetaType<SecretMap>();
        qRegisterMetaType<Attributes>("Attributes");
        qRegisterMetaType<Paths>("Paths");
        qRegisterMetaType<FixtureSecret>("FixtureSecret");
        qRegisterMetaType<SecretMap>("SecretMap");
        auto bus = QDBusConnection::sessionBus();
        SecretServiceFixture service;
        LockedItem item;
        LockedItem ready;
        ready.isLocked = false;
        ready.user = QStringLiteral("fixture-unlocked-account");
        QVERIFY(bus.registerService(QStringLiteral("org.freedesktop.secrets")));
        QVERIFY(bus.registerObject(QStringLiteral("/org/freedesktop/secrets"), &service,
            QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllProperties));
        QVERIFY(bus.registerObject(QStringLiteral("/org/freedesktop/secrets/collection/fixture/locked"), &item,
            QDBusConnection::ExportAllProperties));
        QVERIFY(bus.registerObject(QStringLiteral("/org/freedesktop/secrets/collection/fixture/ready"), &ready,
            QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllProperties));
        bool finished = false;
        auto result = GoogleDriveCredentialStore::Result::Failed;
        GoogleDriveSecretStore::run(this, QStringLiteral("fixture-only-account"),
            GoogleDriveSecretStore::Action::Read, true, {},
            [&finished, &result](auto value, const QByteArray &record) {
                result = value;
                QVERIFY(record.isEmpty());
                finished = true;
            });
        QTRY_VERIFY_WITH_TIMEOUT(finished, 10000);
        QCOMPARE(result, GoogleDriveCredentialStore::Result::InteractionRequired);
        QCOMPARE(service.searches, 1);
        QCOMPARE(service.unlocks, 0);
        QCOMPARE(service.requested, item.attributes());
        finished = false;
        QByteArray loaded;
        GoogleDriveSecretStore::run(this, ready.user, GoogleDriveSecretStore::Action::Read, true, {},
            [&finished, &result, &loaded](auto value, const QByteArray &record) {
                result = value;
                loaded = record;
                finished = true;
            });
        QTRY_VERIFY_WITH_TIMEOUT(finished, 10000);
        QCOMPARE(result, GoogleDriveCredentialStore::Result::Success);
        QCOMPARE(loaded, ready.record);
        QCOMPARE(service.unlocks, 0);
        QCOMPARE(service.searches, 2);
        QVERIFY(service.sessions >= 1);
        QVERIFY(service.secretLoads + ready.loads >= 1);
        // An upgrade must also reuse an already-unlocked legacy Secret Service
        // record without migrating, prompting, or reopening Google sign-in.
        service.legacyOnly = true;
        ready.user = QStringLiteral("desktop-account-v1");
        GoogleDriveAuth::ClientConfig client{QStringLiteral("123-fixture.apps.googleusercontent.com"),
                                            QStringLiteral("fixture-secret")};
        ready.record = QJsonDocument(QJsonObject{
            {QStringLiteral("version"), 1}, {QStringLiteral("client_id"), client.clientId},
            {QStringLiteral("scope"), GoogleDriveAuth::requiredScope()},
            {QStringLiteral("config_password"), QString(43, QLatin1Char('x'))},
            {QStringLiteral("token"), QJsonObject{
                {QStringLiteral("access_token"), QStringLiteral("synthetic-access")},
                {QStringLiteral("refresh_token"), QStringLiteral("synthetic-refresh")},
                {QStringLiteral("token_type"), QStringLiteral("Bearer")},
                {QStringLiteral("expiry"), QStringLiteral("2099-01-01T00:00:00Z")}}}
        }).toJson(QJsonDocument::Compact);
        service.record = ready.record;
        GoogleDriveAuth auth(client);
        QSignalSpy authorized(&auth, &GoogleDriveAuth::authorized);
        QSignalSpy unavailable(&auth, &GoogleDriveAuth::savedAccountUnavailable);
        auth.connectSavedAccount();
        QTRY_COMPARE_WITH_TIMEOUT(authorized.count(), 1, 10000);
        QCOMPARE(unavailable.count(), 0);
        QVERIFY(authorized.first().at(0).toByteArray().isEmpty());
        QCOMPARE(service.unlocks, 0);
        bus.unregisterObject(QStringLiteral("/org/freedesktop/secrets/collection/fixture/ready"));
        bus.unregisterObject(QStringLiteral("/org/freedesktop/secrets/collection/fixture/locked"));
        bus.unregisterObject(QStringLiteral("/org/freedesktop/secrets"));
        bus.unregisterService(QStringLiteral("org.freedesktop.secrets"));
    }
};

QTEST_GUILESS_MAIN(GoogleDriveSecretStoreTest)
#include "GoogleDriveSecretStoreTest.moc"
