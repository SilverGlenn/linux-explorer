// A deliberately small wire client, not another file browser or a public C++ SDK.
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <QTimer>
#include <QUrl>

namespace {
constexpr auto service = "org.win7explorer.FileChooser1";
constexpr auto path = "/org/win7explorer/FileChooser1";
constexpr auto requestInterface = "org.win7explorer.FileChooser1.Request";

struct Filter {
    QString id;
    QString label;
    QStringList globs;
    QStringList mimeTypes;
};
using Filters = QList<Filter>;

QDBusArgument &operator<<(QDBusArgument &argument, const Filter &filter)
{
    argument.beginStructure();
    argument << filter.id << filter.label << filter.globs << filter.mimeTypes;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, Filter &filter)
{
    argument.beginStructure();
    argument >> filter.id >> filter.label >> filter.globs >> filter.mimeTypes;
    argument.endStructure();
    return argument;
}

bool stringArray(const QJsonValue &value, QStringList &strings)
{
    if (value.isUndefined()) return true;
    if (!value.isArray()) return false;
    for (const QJsonValue &entry : value.toArray()) {
        if (!entry.isString()) return false;
        strings.append(entry.toString());
    }
    return true;
}

QString startingUri(const QString &argument)
{
    // Shell filenames remain literal, including # and %. Explicit URIs are
    // already encoded by their caller; never run paths through a shell.
    if (argument.startsWith(QLatin1String("file:")) || argument.contains(QLatin1String("://")))
        return argument;
    return QUrl::fromLocalFile(QFileInfo(argument).absoluteFilePath()).toString(QUrl::FullyEncoded);
}
} // namespace

Q_DECLARE_METATYPE(Filter)
Q_DECLARE_METATYPE(Filters)

class Client final : public QObject {
    Q_OBJECT
public:
    explicit Client(QObject *parent = nullptr) : QObject(parent)
    {
        auto *owner = new QDBusServiceWatcher(service, m_bus,
            QDBusServiceWatcher::WatchForUnregistration, this);
        connect(owner, &QDBusServiceWatcher::serviceUnregistered, this, [this] {
            transportError(QStringLiteral("ServiceDisconnected"),
                           QStringLiteral("The chooser service disconnected before completion."));
        });
    }

    void start(const QString &method, const QVariantMap &options, int cancelAfter)
    {
        auto call = QDBusMessage::createMethodCall(service, path, service, method);
        call.setArguments({options});
        auto *watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(call), this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this,
                [this, watcher, cancelAfter] {
            const QDBusPendingReply<QDBusObjectPath> reply = *watcher;
            watcher->deleteLater();
            if (reply.isError()) {
                transportError(reply.error().name(), reply.error().message());
                return;
            }
            m_request = reply.value().path();
            QTextStream(stderr) << "Request: " << m_request << Qt::endl;
            if (!m_bus.connect(service, m_request, requestInterface, "Finished", this,
                               SLOT(finished(uint,QVariantMap)))) {
                transportError("SignalSubscriptionFailed", m_bus.lastError().message());
                return;
            }
            // Subscribe first, then query: this closes the method-reply/signal
            // race, including failures which completed before subscription.
            auto query = QDBusMessage::createMethodCall(service, m_request,
                                                        requestInterface, "GetResult");
            auto *resultWatcher = new QDBusPendingCallWatcher(m_bus.asyncCall(query), this);
            connect(resultWatcher, &QDBusPendingCallWatcher::finished, this,
                    [this, resultWatcher] {
                const QDBusPendingReply<QVariantMap> result = *resultWatcher;
                resultWatcher->deleteLater();
                if (m_done) return;
                if (result.isError()) {
                    transportError(result.error().name(), result.error().message());
                    return;
                }
                const QVariantMap values = result.value();
                if (values.value(QStringLiteral("state")).toString() != QLatin1String("pending"))
                    finished(values.value(QStringLiteral("status")).toUInt(), values);
            });
            if (cancelAfter >= 0) {
                QTimer::singleShot(cancelAfter, this, [this] {
                    if (m_done) return;
                    const auto cancel = QDBusMessage::createMethodCall(service, m_request,
                                                                       requestInterface, "Cancel");
                    auto *cancelWatcher = new QDBusPendingCallWatcher(m_bus.asyncCall(cancel), this);
                    connect(cancelWatcher, &QDBusPendingCallWatcher::finished, this,
                            [this, cancelWatcher] {
                        const QDBusPendingReply<> reply = *cancelWatcher;
                        cancelWatcher->deleteLater();
                        if (!m_done && reply.isError())
                            transportError(reply.error().name(), reply.error().message());
                    });
                });
            }
        });
    }

public Q_SLOTS:
    void finished(uint status, const QVariantMap &results)
    {
        if (m_done) return; // GetResult and Finished describe the same result.
        m_done = true;
        QVariantMap values = results;
        if (values.contains(QStringLiteral("uris")))
            values[QStringLiteral("uris")] = qdbus_cast<QStringList>(values.value(QStringLiteral("uris")));
        values.insert(QStringLiteral("status"), status);
        values.insert(QStringLiteral("request_handle"), m_request);
        QTextStream(stdout) << QJsonDocument(QJsonObject::fromVariantMap(values))
                                   .toJson(QJsonDocument::Compact) << Qt::endl;
        // Synchronous acknowledgement is quick and ensures the service can
        // release this terminal object before the CLI disconnects.
        auto close = QDBusMessage::createMethodCall(service, m_request, requestInterface, "Close");
        m_bus.call(close);
        QCoreApplication::exit(status <= 2 ? int(status) : 3);
    }

private:
    void transportError(const QString &name, const QString &message)
    {
        if (m_done) return;
        m_done = true;
        const QJsonObject error{{"state", "failed"}, {"error_name", name},
                                {"error_message", message}};
        QTextStream(stdout) << QJsonDocument(error).toJson(QJsonDocument::Compact) << Qt::endl;
        QCoreApplication::exit(3);
    }

    QDBusConnection m_bus = QDBusConnection::sessionBus();
    QString m_request;
    bool m_done = false;
};

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("explorer-win7-chooser-client"));
    qDBusRegisterMetaType<Filter>();
    qDBusRegisterMetaType<Filters>();

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        "Asynchronous Explorer chooser client. Prints one JSON result; never opens or writes the selected file."));
    parser.addHelpOption();
    parser.addPositionalArgument(QStringLiteral("mode"), QStringLiteral("open or save"));
    parser.addOptions({
        {"title", "Dialog title.", "text"},
        {"start", "Starting folder (local path or encoded file URI).", "folder", QDir::currentPath()},
        {"name", "Suggested filename.", "filename"},
        {"deb", "Use a Debian packages (*.deb) filter."},
        {"log", "Use a text logs (*.log) filter and .log default extension."},
        {"filter", "Repeatable JSON filter: {\"id\":\"x\",\"label\":\"X\",\"globs\":[\"*.x\"],\"mime_types\":[]}.", "json"},
        {"selected-filter", "Initially selected filter ID.", "id"},
        {"default-extension", "Save extension without a leading dot.", "extension"},
        {"parent-window", "x11:<hexid> or wayland:<exported-handle> (not a wl_surface pointer).", "handle"},
        {"activation-token", "Fresh Wayland xdg-activation token or X11 startup ID.", "token"},
        {"modal", "Request modality; requires --parent-window. Client must also inhibit its parent."},
        {"cancel-after", "Explicitly cancel after N milliseconds (test aid).", "milliseconds"}
    });
    parser.process(app);
    const auto arguments = parser.positionalArguments();
    if (arguments.size() != 1 || (arguments[0] != QLatin1String("open") && arguments[0] != QLatin1String("save")))
        parser.showHelp(3);

    Filters filters;
    if (parser.isSet(QStringLiteral("deb")))
        filters.append({QStringLiteral("deb"), QStringLiteral("Debian packages (*.deb)"),
                         {QStringLiteral("*.deb")}, {}});
    if (parser.isSet(QStringLiteral("log")))
        filters.append({QStringLiteral("log"), QStringLiteral("Text logs (*.log)"),
                         {QStringLiteral("*.log")}, {}});
    for (const QString &json : parser.values(QStringLiteral("filter"))) {
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(json.toUtf8(), &error);
        const auto object = document.object();
        Filter filter{object.value("id").toString(), object.value("label").toString(), {}, {}};
        if (error.error != QJsonParseError::NoError || !document.isObject()
            || !object.value("id").isString() || !object.value("label").isString()
            || !stringArray(object.value("globs"), filter.globs)
            || !stringArray(object.value("mime_types"), filter.mimeTypes)) {
            QTextStream(stderr) << "Invalid --filter JSON: " << json << Qt::endl;
            return 3;
        }
        filters.append(filter);
    }
    QVariantMap options{
        {QStringLiteral("starting_location"), startingUri(parser.value(QStringLiteral("start")))},
        {QStringLiteral("local_only"), true},
        {QStringLiteral("modal"), parser.isSet(QStringLiteral("modal"))}
    };
    const QList<QPair<QString, QString>> fields{
        {QStringLiteral("title"), QStringLiteral("title")},
        {QStringLiteral("name"), QStringLiteral("suggested_name")},
        {QStringLiteral("selected-filter"), QStringLiteral("selected_filter")},
        {QStringLiteral("default-extension"), QStringLiteral("default_extension")},
        {QStringLiteral("parent-window"), QStringLiteral("parent_window")},
        {QStringLiteral("activation-token"), QStringLiteral("activation_token")}
    };
    for (const auto &field : fields) {
        if (parser.isSet(field.first)) options.insert(field.second, parser.value(field.first));
    }
    if (!filters.isEmpty()) options.insert(QStringLiteral("filters"), QVariant::fromValue(filters));
    if (parser.isSet(QStringLiteral("log")) && !parser.isSet(QStringLiteral("default-extension")))
        options.insert(QStringLiteral("default_extension"), QStringLiteral("log"));
    int cancelAfter = -1;
    if (parser.isSet(QStringLiteral("cancel-after"))) {
        bool ok = false;
        cancelAfter = parser.value(QStringLiteral("cancel-after")).toInt(&ok);
        if (!ok || cancelAfter < 0) {
            QTextStream(stderr) << "--cancel-after requires a nonnegative integer." << Qt::endl;
            return 3;
        }
    }
    Client client;
    client.start(arguments[0] == QLatin1String("open") ? QStringLiteral("OpenFile") : QStringLiteral("SaveFile"),
                 options, cancelAfter);
    return app.exec();
}

#include "chooser-client.moc"
