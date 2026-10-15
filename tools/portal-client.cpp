// Example client of the STANDARD frontend portal, not the private chooser API.
#include "PortalDBusTypes.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <QTimer>
#include <QUuid>
#include <QUrl>

namespace {
constexpr auto service = "org.freedesktop.portal.Desktop";
constexpr auto rootPath = "/org/freedesktop/portal/desktop";
constexpr auto chooserInterface = "org.freedesktop.portal.FileChooser";
constexpr auto requestInterface = "org.freedesktop.portal.Request";

QString localPath(const QString &text)
{
    if (text.isEmpty()) return {};
    if (text.startsWith(QLatin1String("file:"), Qt::CaseInsensitive)
        || text.contains(QLatin1String("://"))) {
        QUrl url(text, QUrl::StrictMode);
        if (!url.isValid() || !url.isLocalFile()
            || (!url.host().isEmpty() && url.host() != QLatin1String("localhost"))
            || url.hasQuery() || url.hasFragment() || !url.userInfo().isEmpty()) return {};
        url.setHost(QString());
        return QDir::isAbsolutePath(url.toLocalFile()) ? url.toLocalFile() : QString();
    }
    QString path = text;
    if (path == QLatin1String("~")) path = QDir::homePath();
    else if (path.startsWith(QLatin1String("~/"))) path.replace(0, 1, QDir::homePath());
    return QFileInfo(path).absoluteFilePath();
}

QByteArray pathBytes(const QString &path)
{
    QByteArray bytes = QFile::encodeName(path);
    bytes.append('\0');
    return bytes;
}

bool stringList(const QJsonValue &value, QStringList &strings)
{
    if (value.isUndefined()) return true;
    if (!value.isArray()) return false;
    for (const auto &entry : value.toArray()) {
        if (!entry.isString()) return false;
        strings.append(entry.toString());
    }
    return true;
}

QJsonObject jsonFilter(const PortalFilter &filter)
{
    QJsonArray rules;
    for (const auto &rule : filter.rules)
        rules.append(QJsonObject{{"type", int(rule.type)}, {"value", rule.value}});
    return {{"name", filter.name}, {"rules", rules}};
}
} // namespace

class PortalClient final : public QObject {
    Q_OBJECT
public:
    explicit PortalClient(QObject *parent = nullptr) : QObject(parent)
    {
        auto *watcher = new QDBusServiceWatcher(service, m_bus,
            QDBusServiceWatcher::WatchForUnregistration, this);
        connect(watcher, &QDBusServiceWatcher::serviceUnregistered, this, [this] {
            fail(QStringLiteral("ServiceDisconnected"), QStringLiteral("The portal frontend disconnected."));
        });
        m_bus.connect(QString(), QStringLiteral("/org/freedesktop/DBus/Local"),
            QStringLiteral("org.freedesktop.DBus.Local"), QStringLiteral("Disconnected"),
            this, SLOT(busLost()));
    }

    void start(const QString &method, const QString &parentWindow, const QString &title,
               QVariantMap options, int cancelAfter)
    {
        if (!m_bus.isConnected()) {
            fail(QStringLiteral("NoSessionBus"), m_bus.lastError().message());
            return;
        }
        const QString token = QStringLiteral("explorer_")
            + QUuid::createUuid().toString(QUuid::Id128);
        QString sender = m_bus.baseService().mid(1);
        sender.replace(QLatin1Char('.'), QLatin1Char('_'));
        m_handle = QString::fromLatin1(rootPath) + QStringLiteral("/request/") + sender
            + QLatin1Char('/') + token;
        options.insert(QStringLiteral("handle_token"), token);
        // Standard portals have NO GetResult. Subscribe before OpenFile using
        // the specified handle_token path so an early Response cannot be lost.
        if (!m_bus.connect(service, m_handle, requestInterface, "Response", this,
                           SLOT(response(uint,QVariantMap)))) {
            fail(QStringLiteral("SignalSubscriptionFailed"), m_bus.lastError().message());
            return;
        }
        auto call = QDBusMessage::createMethodCall(service, rootPath, chooserInterface, method);
        call.setArguments({parentWindow, title, options});
        auto *watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(call), this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, cancelAfter] {
            const QDBusPendingReply<QDBusObjectPath> reply = *watcher;
            watcher->deleteLater();
            if (m_done) return;
            if (reply.isError()) {
                fail(reply.error().name(), reply.error().message());
                return;
            }
            if (reply.value().path() != m_handle) {
                fail(QStringLiteral("UnexpectedRequestHandle"),
                     QStringLiteral("The portal did not honor the handle_token request path."));
                return;
            }
            QTextStream(stderr) << "Request: " << m_handle << Qt::endl;
            if (cancelAfter >= 0) QTimer::singleShot(cancelAfter, this, &PortalClient::cancel);
        });
    }

public Q_SLOTS:
    void response(uint status, const QVariantMap &results)
    {
        if (m_done) return;
        m_done = true;
        QJsonObject output{
            {"status", int(status)},
            {"state", status == 0 ? "accepted" : status == 1 ? "canceled" : "failed"},
            {"request_handle", m_handle},
            {"uris", QJsonArray::fromStringList(qdbus_cast<QStringList>(results.value(QStringLiteral("uris"))))}
        };
        if (results.contains(QStringLiteral("current_filter")))
            output.insert(QStringLiteral("current_filter"), jsonFilter(qdbus_cast<PortalFilter>(
                results.value(QStringLiteral("current_filter")))));
        if (results.contains(QStringLiteral("choices"))) {
            QJsonArray choices;
            for (const auto &choice : qdbus_cast<PortalChoiceResults>(results.value(QStringLiteral("choices"))))
                choices.append(QJsonObject{{"id", choice.id}, {"selected", choice.selected}});
            output.insert(QStringLiteral("choices"), choices);
        }
        print(output);
        QCoreApplication::exit(status <= 2 ? int(status) : 3);
    }

    void busLost()
    {
        fail(QStringLiteral("BusDisconnected"), QStringLiteral("The session bus disconnected."));
    }

private:
    void cancel()
    {
        if (m_done) return;
        auto call = QDBusMessage::createMethodCall(service, m_handle, requestInterface, "Close");
        auto *watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(call), this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher] {
            const QDBusPendingReply<> reply = *watcher;
            watcher->deleteLater();
            if (m_done) return;
            if (reply.isError()) { fail(reply.error().name(), reply.error().message()); return; }
            // The standard Request.Close contract deliberately emits no
            // Response afterward. The client knows its own explicit cancel.
            m_done = true;
            print({{"status", 1}, {"state", "canceled"}, {"canceled_by_client", true},
                   {"request_handle", m_handle}, {"uris", QJsonArray()}});
            QCoreApplication::exit(1);
        });
    }

    void fail(const QString &name, const QString &message)
    {
        if (m_done) return;
        m_done = true;
        print({{"state", "failed"}, {"error_name", name}, {"error_message", message}});
        QCoreApplication::exit(3);
    }

    static void print(const QJsonObject &output)
    {
        QTextStream(stdout) << QJsonDocument(output).toJson(QJsonDocument::Compact) << Qt::endl;
    }

    QDBusConnection m_bus = QDBusConnection::sessionBus();
    QString m_handle;
    bool m_done = false;
};

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("explorer-win7-portal-client"));
    registerPortalDBusTypes();
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        "Standard FileChooser portal client. Prints JSON; never reads or writes the chosen destinations."));
    parser.addHelpOption();
    parser.addPositionalArgument(QStringLiteral("mode"), QStringLiteral("open, save, or save-many"));
    parser.addOptions({
        {"title", "Dialog title.", "text"},
        {"start", "Starting local folder (path or encoded file URI).", "folder", QDir::currentPath()},
        {"name", "Suggested SaveFile name.", "filename"},
        {"current-file", "Existing SaveFile path/URI to preselect.", "file"},
        {"file", "Repeatable SaveFiles leaf filename (save-many only).", "filename"},
        {"multiple", "Allow multiple files/folders in open mode."},
        {"directory", "Choose folders in open mode."},
        {"deb", "Use a case-sensitive *.deb filter."},
        {"log", "Use a case-sensitive *.log filter."},
        {"filter", "Repeatable JSON: {\"name\":\"Text\",\"globs\":[\"*.txt\"],\"mime_types\":[\"text/plain\"]}.", "json"},
        {"filter-index", "Initially selected filter, zero-based.", "index"},
        {"choice", "Repeatable JSON: {\"id\":\"x\",\"label\":\"X\",\"options\":[{\"id\":\"a\",\"label\":\"A\"}],\"selected\":\"a\"}; empty options means checkbox.", "json"},
        {"accept-label", "Portal accept label (underscore mnemonic syntax).", "label"},
        {"parent-window", "wayland:<exported-handle> or x11:<hex-XID>.", "handle"},
        {"modal", "Request modality; a real parent is needed to enforce it."},
        {"cancel-after", "Explicitly Close the request after N milliseconds.", "milliseconds"}
    });
    parser.process(app);
    const auto arguments = parser.positionalArguments();
    if (arguments.size() != 1 || (arguments[0] != QLatin1String("open")
        && arguments[0] != QLatin1String("save") && arguments[0] != QLatin1String("save-many")))
        parser.showHelp(3);
    const QString mode = arguments[0];
    auto usageError = [](const QString &message) { QTextStream(stderr) << message << Qt::endl; return 3; };
    if (mode != QLatin1String("open") && (parser.isSet("multiple") || parser.isSet("directory")))
        return usageError(QStringLiteral("--multiple/--directory are OpenFile options only."));
    if (mode != QLatin1String("save-many") && parser.isSet("file"))
        return usageError(QStringLiteral("--file is a save-many option only."));
    if (mode == QLatin1String("save-many") && parser.values("file").isEmpty())
        return usageError(QStringLiteral("save-many requires at least one --file filename."));
    const QString start = localPath(parser.value("start"));
    if (start.isEmpty()) return usageError(QStringLiteral("--start must be a local folder path or encoded file URI."));
    QVariantMap options{{QStringLiteral("current_folder"), pathBytes(start)},
                        {QStringLiteral("modal"), parser.isSet("modal")}};
    if (mode == QLatin1String("open")) {
        options.insert(QStringLiteral("multiple"), parser.isSet("multiple"));
        options.insert(QStringLiteral("directory"), parser.isSet("directory"));
    }
    if (parser.isSet("name")) options.insert(QStringLiteral("current_name"), parser.value("name"));
    if (parser.isSet("current-file")) {
        const QString path = localPath(parser.value("current-file"));
        if (path.isEmpty()) return usageError(QStringLiteral("--current-file must be local."));
        options.insert(QStringLiteral("current_file"), pathBytes(path));
    }
    if (parser.isSet("accept-label")) options.insert(QStringLiteral("accept_label"), parser.value("accept-label"));
    if (mode == QLatin1String("save-many")) {
        PortalFileNames files;
        for (const QString &name : parser.values("file")) files.append(pathBytes(name));
        options.insert(QStringLiteral("files"), QVariant::fromValue(files));
    }
    PortalFilters filters;
    if (parser.isSet("deb")) filters.append({QStringLiteral("Debian packages (*.deb)"), {{0, QStringLiteral("*.deb")}}});
    if (parser.isSet("log")) filters.append({QStringLiteral("Text logs (*.log)"), {{0, QStringLiteral("*.log")}}});
    for (const QString &json : parser.values("filter")) {
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(json.toUtf8(), &error);
        const auto object = document.object();
        QStringList globs, mimeTypes;
        if (error.error != QJsonParseError::NoError || !document.isObject() || !object.value("name").isString()
            || !stringList(object.value("globs"), globs) || !stringList(object.value("mime_types"), mimeTypes))
            return usageError(QStringLiteral("Invalid --filter JSON."));
        PortalFilter filter{object.value("name").toString(), {}};
        for (const auto &glob : globs) filter.rules.append({0, glob});
        for (const auto &mime : mimeTypes) filter.rules.append({1, mime});
        filters.append(filter);
    }
    if (!filters.isEmpty()) options.insert(QStringLiteral("filters"), QVariant::fromValue(filters));
    if (parser.isSet("filter-index")) {
        bool ok = false;
        const int index = parser.value("filter-index").toInt(&ok);
        if (!ok || index < 0 || index >= filters.size()) return usageError(QStringLiteral("Invalid --filter-index."));
        options.insert(QStringLiteral("current_filter"), QVariant::fromValue(filters[index]));
    }
    PortalChoices choices;
    for (const QString &json : parser.values("choice")) {
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(json.toUtf8(), &error);
        const auto object = document.object();
        if (error.error != QJsonParseError::NoError || !document.isObject()
            || !object.value("id").isString() || !object.value("label").isString()
            || (!object.value("selected").isUndefined() && !object.value("selected").isString())
            || (!object.value("options").isUndefined() && !object.value("options").isArray()))
            return usageError(QStringLiteral("Invalid --choice JSON."));
        PortalChoice choice{object.value("id").toString(), object.value("label").toString(), {}, object.value("selected").toString()};
        for (const auto &entry : object.value("options").toArray()) {
            const auto option = entry.toObject();
            if (!entry.isObject() || !option.value("id").isString() || !option.value("label").isString())
                return usageError(QStringLiteral("Invalid --choice options."));
            choice.options.append({option.value("id").toString(), option.value("label").toString()});
        }
        choices.append(choice);
    }
    if (!choices.isEmpty()) options.insert(QStringLiteral("choices"), QVariant::fromValue(choices));
    int cancelAfter = -1;
    if (parser.isSet("cancel-after")) {
        bool ok = false;
        cancelAfter = parser.value("cancel-after").toInt(&ok);
        if (!ok || cancelAfter < 0) return usageError(QStringLiteral("Invalid --cancel-after."));
    }
    const QString method = mode == QLatin1String("open") ? QStringLiteral("OpenFile")
        : mode == QLatin1String("save") ? QStringLiteral("SaveFile") : QStringLiteral("SaveFiles");
    const QString title = parser.isSet("title") ? parser.value("title")
        : mode == QLatin1String("open") ? QStringLiteral("Explorer portal — Open")
        : QStringLiteral("Explorer portal — Save");
    PortalClient client;
    QTimer::singleShot(0, &client, [&] {
        client.start(method, parser.value("parent-window"), title, options, cancelAfter);
    });
    return app.exec();
}

#include "portal-client.moc"
