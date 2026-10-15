#include "ChooserService.h"
#include "ChooserDBusTypes.h"
#include "ChooserOptions.h"
#include "FileChooserDialog.h"
#include "ChooserWindow.h"

#include <QDBusConnectionInterface>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusServiceWatcher>
#include <QDir>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QPointer>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>
#include <utility>

namespace {
const QString serviceName = QStringLiteral("org.win7explorer.FileChooser1");
const QString servicePath = QStringLiteral("/org/win7explorer/FileChooser1");
const QString requestInterface = QStringLiteral("org.win7explorer.FileChooser1.Request");
constexpr int maxRequests = 64;
constexpr int maxOwnerRequests = 8;

struct Configuration {
    ChooserOptions chooser;
    Explorer::ChooserWindowOptions presentation;
    QString errorName;
    QString errorMessage;
};

bool hasControl(const QString &value)
{
    for (QChar ch : value) {
        if (ch.category() == QChar::Other_Control)
            return true;
    }
    return false;
}

bool localUri(const QString &text, QUrl *result)
{
    // StrictMode alone permits unencoded Unicode. The wire accepts encoded URI
    // text, not shell paths, and QUrl::isLocalFile alone accepts remote hosts.
    if (text.isEmpty() || text.size() > 16384)
        return false;
    for (QChar ch : text) {
        if (ch.unicode() <= 0x20 || ch.unicode() >= 0x7f)
            return false;
    }
    QUrl url(text, QUrl::StrictMode);
    if (!url.isValid() || url.scheme() != QLatin1String("file")
        || (!url.host().isEmpty() && url.host() != QLatin1String("localhost"))
        || !url.userInfo().isEmpty() || url.port() != -1
        || url.hasQuery() || url.hasFragment())
        return false;
    url.setHost(QString()); // localhost must not become a UNC path on Unix.
    if (!QDir::isAbsolutePath(url.toLocalFile()) || url.path().contains(QChar::Null))
        return false;
    *result = url;
    return true;
}

Configuration configuration(const QVariantMap &wire, bool save)
{
    Configuration config;
    config.chooser.mode = save ? ChooserMode::SaveFile : ChooserMode::OpenFile;
    auto invalid = [&config](const QString &message, bool unsupported = false) {
        config.errorName = serviceName + (unsupported ? QStringLiteral(".Error.UnsupportedOptions")
                                                      : QStringLiteral(".Error.InvalidOptions"));
        config.errorMessage = message;
        return config;
    };
    static const QSet<QString> strings = {
        QStringLiteral("title"), QStringLiteral("starting_location"),
        QStringLiteral("suggested_name"), QStringLiteral("selected_filter"),
        QStringLiteral("default_extension"), QStringLiteral("parent_window"),
        QStringLiteral("activation_token")};
    static const QSet<QString> booleans = {
        QStringLiteral("local_only"), QStringLiteral("multiple"),
        QStringLiteral("directory"), QStringLiteral("modal")};
    for (auto it = wire.cbegin(); it != wire.cend(); ++it) {
        if (strings.contains(it.key())) {
            if (it.value().metaType() != QMetaType::fromType<QString>())
                return invalid(QStringLiteral("Option %1 must be a string.").arg(it.key()));
            const QString value = it.value().toString();
            const int limit = it.key() == QLatin1String("starting_location") ? 16384 : 4096;
            if (value.size() > limit || value.contains(QChar::Null))
                return invalid(QStringLiteral("Option %1 exceeds string limits.").arg(it.key()));
        } else if (booleans.contains(it.key())) {
            if (it.value().metaType() != QMetaType::fromType<bool>())
                return invalid(QStringLiteral("Option %1 must be a boolean.").arg(it.key()));
        } else if (it.key() != QLatin1String("filters")) {
            return invalid(QStringLiteral("Unknown chooser option: %1.").arg(it.key().left(128)));
        }
    }
    auto string = [&wire](const char *key) { return wire.value(QLatin1String(key)).toString(); };
    auto boolean = [&wire](const char *key, bool fallback = false) {
        return wire.value(QLatin1String(key), fallback).toBool();
    };
    config.chooser.title = string("title");
    config.chooser.suggestedName = string("suggested_name");
    config.chooser.defaultExtension = string("default_extension");
    config.chooser.selectedFilter = string("selected_filter");
    config.chooser.localOnly = boolean("local_only", true);
    config.chooser.multiple = boolean("multiple");
    config.chooser.directory = boolean("directory");
    config.presentation = {string("parent_window"), string("activation_token"), boolean("modal")};
    if (!config.chooser.localOnly)
        return invalid(QStringLiteral("Only local files are supported."), true);
    if (save && (config.chooser.multiple || config.chooser.directory))
        return invalid(QStringLiteral("SaveFile does not support multiple or directory selection."), true);
    if (wire.contains(QStringLiteral("starting_location"))
        && !localUri(string("starting_location"), &config.chooser.startingLocation))
        return invalid(QStringLiteral("starting_location must be an encoded absolute local file URI."));
    const QString leaf = config.chooser.suggestedName;
    if (leaf.size() > 255 || leaf == QLatin1String(".") || leaf == QLatin1String("..")
        || leaf.contains(QLatin1Char('/')) || hasControl(leaf))
        return invalid(QStringLiteral("suggested_name must be a single file name."));
    const QString extension = config.chooser.defaultExtension;
    if (extension.size() > 255 || extension.startsWith(QLatin1Char('.'))
        || extension.contains(QLatin1Char('/')) || extension.contains(QLatin1Char('\\'))
        || extension.contains(QLatin1Char('*')) || extension.contains(QLatin1Char('?'))
        || hasControl(extension))
        return invalid(QStringLiteral("default_extension must be a literal extension without a leading dot."));

    ChooserWireFilters filters;
    if (wire.contains(QStringLiteral("filters"))) {
        const QVariant value = wire.value(QStringLiteral("filters"));
        if (value.metaType() == QMetaType::fromType<QDBusArgument>()) {
            const QDBusArgument argument = value.value<QDBusArgument>();
            if (argument.currentSignature() != QLatin1String("a(ssasas)"))
                return invalid(QStringLiteral("filters must have signature a(ssasas)."));
            // Bound nested arrays while decoding, rather than first building
            // unbounded lists from an untrusted message.
            int decodedPatterns = 0;
            auto patterns = [&decodedPatterns, &argument](QStringList *values) {
                argument.beginArray();
                while (!argument.atEnd()) {
                    if (++decodedPatterns > 256)
                        return false;
                    QString value;
                    argument >> value;
                    if (value.size() > 256)
                        return false;
                    values->append(value);
                }
                argument.endArray();
                return true;
            };
            argument.beginArray();
            while (!argument.atEnd()) {
                if (filters.size() >= 64)
                    return invalid(QStringLiteral("Too many filters (maximum 64)."));
                ChooserWireFilter filter;
                argument.beginStructure();
                argument >> filter.id >> filter.label;
                if (!patterns(&filter.globs) || !patterns(&filter.mimeTypes))
                    return invalid(QStringLiteral("Filter pattern limits exceeded."));
                argument.endStructure();
                filters.append(std::move(filter));
            }
            argument.endArray();
        } else if (value.metaType() == QMetaType::fromType<ChooserWireFilters>()) {
            filters = value.value<ChooserWireFilters>();
        } else {
            return invalid(QStringLiteral("filters must have signature a(ssasas)."));
        }
    }
    if (filters.size() > 64)
        return invalid(QStringLiteral("Too many filters (maximum 64)."));
    QSet<QString> ids;
    QMimeDatabase mimeDatabase;
    int patternCount = 0;
    for (const ChooserWireFilter &filter : filters) {
        if (filter.id.isEmpty() || filter.id.size() > 256 || hasControl(filter.id)
            || ids.contains(filter.id) || filter.label.isEmpty()
            || filter.label.size() > 1024 || hasControl(filter.label))
            return invalid(QStringLiteral("Filters require unique nonempty ids and labels within limits."));
        ids.insert(filter.id);
        patternCount += filter.globs.size() + filter.mimeTypes.size();
        if (patternCount > 256 || (filter.globs.isEmpty() && filter.mimeTypes.isEmpty()))
            return invalid(QStringLiteral("Filters require patterns (maximum 256 total)."));
        for (const QString &glob : filter.globs) {
            if (glob.isEmpty() || glob.size() > 256 || glob.contains(QLatin1Char('/')) || hasControl(glob)
                || !QRegularExpression(QRegularExpression::wildcardToRegularExpression(glob)).isValid())
                return invalid(QStringLiteral("Invalid or oversized filter glob."));
        }
        for (const QString &mime : filter.mimeTypes) {
            if (mime.size() > 256 || !mimeDatabase.mimeTypeForName(mime).isValid())
                return invalid(QStringLiteral("Unknown filter MIME type."));
        }
        config.chooser.filters.append({filter.id, filter.label, filter.globs, filter.mimeTypes});
    }
    if (!config.chooser.selectedFilter.isEmpty() && !ids.contains(config.chooser.selectedFilter))
        return invalid(QStringLiteral("selected_filter must name a supplied filter id."));

    const QString presentationError = Explorer::validateChooserWindowOptions(config.presentation);
    if (!presentationError.isEmpty())
        return invalid(presentationError);
    return config;
}
} // namespace

class ChooserRequest : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.win7explorer.FileChooser1.Request")
public:
    ChooserRequest(ChooserService *service, QString path, QString owner, Configuration config)
        : QObject(service), m_service(service), m_path(std::move(path)),
          m_owner(std::move(owner)), m_config(std::move(config))
    {
        m_results.insert(QStringLiteral("state"), QStringLiteral("pending"));
    }

    QString owner() const { return m_owner; }
    FileChooserDialog *dialog() const { return m_dialog; }

    void start()
    {
        if (m_terminal)
            return;
        if (!m_config.errorName.isEmpty()) {
            fail(m_config.errorName, m_config.errorMessage);
            return;
        }
        m_dialog = m_service->m_factory(m_config.chooser);
        if (!m_dialog) {
            fail(serviceName + QStringLiteral(".Error.PresentationFailed"), QStringLiteral("Could not create chooser."));
            return;
        }
        m_dialog->setAttribute(Qt::WA_DeleteOnClose, false);
        connect(m_dialog, &QDialog::finished, this, [this](int result) {
            if (m_terminal)
                return;
            if (result != QDialog::Accepted) {
                finish(1, {});
                return;
            }
            const QList<QUrl> urls = m_dialog->selectedUrls();
            if (urls.isEmpty() || urls.size() > 4096
                || ((m_config.chooser.mode == ChooserMode::SaveFile || !m_config.chooser.multiple)
                    && urls.size() != 1)) {
                fail(serviceName + QStringLiteral(".Error.InvalidResult"), QStringLiteral("Chooser returned an invalid selection count."));
                return;
            }
            QStringList uris;
            for (const QUrl &url : urls) {
                QUrl local;
                if (!localUri(url.toString(QUrl::FullyEncoded), &local)) {
                    fail(serviceName + QStringLiteral(".Error.InvalidResult"), QStringLiteral("Chooser returned a nonlocal URL."));
                    return;
                }
                const QFileInfo info(local.toLocalFile());
                const bool open = m_config.chooser.mode == ChooserMode::OpenFile;
                if ((open && (m_config.chooser.directory ? !info.isDir() : !info.isFile()))
                    || (!open && info.isDir())) {
                    fail(serviceName + QStringLiteral(".Error.InvalidResult"), QStringLiteral("Chooser returned an invalid file or directory selection."));
                    return;
                }
                uris.append(local.toString(QUrl::FullyEncoded));
            }
            const QString selected = m_dialog->selectedFilter();
            bool knownFilter = selected.isEmpty() && m_config.chooser.filters.isEmpty();
            for (const ChooserFilter &filter : m_config.chooser.filters)
                knownFilter = knownFilter || filter.id == selected;
            if (!knownFilter) {
                fail(serviceName + QStringLiteral(".Error.InvalidResult"), QStringLiteral("Chooser returned an invalid selection."));
                return;
            }
            finish(0, {{QStringLiteral("uris"), uris}, {QStringLiteral("selected_filter"), selected}});
        });
        connect(m_dialog, &QObject::destroyed, this, [this] {
            m_dialog = nullptr;
            if (!m_terminal)
                fail(serviceName + QStringLiteral(".Error.PresentationFailed"), QStringLiteral("Chooser was destroyed."));
        });
        Explorer::showChooserWindow(m_dialog, m_config.presentation);
    }

    void cancel()
    {
        if (!m_terminal)
            finish(1, {});
        dismissDialog();
    }

    void dismissDialog()
    {
        if (!m_dialog)
            return;
        // The chooser can be inside a nested overwrite exec(). Mark terminal
        // before rejecting children, then defer destruction until it unwinds.
        QPointer<FileChooserDialog> dialog = m_dialog;
        QList<QPointer<QDialog>> children;
        for (QDialog *child : dialog->findChildren<QDialog *>())
            children.prepend(child);
        for (const QPointer<QDialog> &child : children) {
            if (child)
                child->reject();
        }
        if (dialog) {
            dialog->reject();
            dialog->deleteLater();
        }
        m_dialog = nullptr;
    }

public Q_SLOTS:
    void Cancel()
    {
        if (authorize())
            cancel();
    }
    void Close()
    {
        if (!authorize())
            return;
        cancel();
        m_service->removeRequest(m_path);
    }
    QVariantMap GetResult()
    {
        return authorize() ? m_results : QVariantMap();
    }

Q_SIGNALS:
    // Introspection only: never emit a broadcast signal. finish() sends a
    // destination-addressed D-Bus message to the originating unique name.
    void Finished(uint status, const QVariantMap &results);

private:
    friend class ChooserService;
    bool authorize()
    {
        if (calledFromDBus() && message().service() == m_owner)
            return true;
        if (calledFromDBus())
            sendErrorReply(QDBusError::AccessDenied, QStringLiteral("This request belongs to another connection."));
        return false;
    }
    void fail(const QString &name, const QString &message)
    {
        finish(2, {{QStringLiteral("error_name"), name}, {QStringLiteral("error_message"), message}});
    }
    void finish(uint status, QVariantMap results)
    {
        if (m_terminal)
            return;
        m_terminal = true;
        if (!results.contains(QStringLiteral("uris")))
            results.insert(QStringLiteral("uris"), QStringList());
        if (!results.contains(QStringLiteral("selected_filter")))
            results.insert(QStringLiteral("selected_filter"), QString());
        results.insert(QStringLiteral("state"), status == 0 ? QStringLiteral("accepted")
            : status == 1 ? QStringLiteral("canceled") : QStringLiteral("failed"));
        results.insert(QStringLiteral("status"), status);
        m_results = std::move(results);
        QDBusMessage signal = QDBusMessage::createTargetedSignal(m_owner, m_path, requestInterface, QStringLiteral("Finished"));
        signal.setArguments({status, m_results});
        m_service->m_connection.send(signal);
        if (m_dialog) {
            m_dialog->hide();
            m_dialog->deleteLater();
        }
        QTimer::singleShot(60000, this, [this] { m_service->removeRequest(m_path); });
    }

    ChooserService *m_service;
    QString m_path;
    QString m_owner;
    Configuration m_config;
    QVariantMap m_results;
    QPointer<FileChooserDialog> m_dialog;
    bool m_terminal = false;
};

ChooserService::ChooserService(const QDBusConnection &connection, QObject *parent)
    : QObject(parent), m_connection(connection),
      m_watcher(new QDBusServiceWatcher(this)),
      m_factory([](const ChooserOptions &options) { return new FileChooserDialog(options); })
{
    registerChooserDBusTypes();
    m_watcher->setConnection(m_connection);
    m_watcher->setWatchMode(QDBusServiceWatcher::WatchForUnregistration);
    connect(m_watcher, &QDBusServiceWatcher::serviceUnregistered,
            this, &ChooserService::ownerDisconnected);
}

ChooserService::~ChooserService()
{
    const auto requests = m_requests;
    for (ChooserRequest *request : requests) {
        m_connection.unregisterObject(request->m_path);
        request->cancel();
        delete request;
    }
    if (m_claimed) {
        m_connection.unregisterObject(servicePath);
        m_connection.unregisterService(serviceName);
    }
}

bool ChooserService::claim()
{
    if (m_claimed)
        return true;
    if (!m_connection.isConnected() || !m_connection.interface())
        return false;
    const auto reply = m_connection.interface()->registerService(serviceName,
        QDBusConnectionInterface::DontQueueService, QDBusConnectionInterface::DontAllowReplacement);
    if (!reply.isValid() || reply.value() != QDBusConnectionInterface::ServiceRegistered)
        return false;
    if (!m_connection.registerObject(servicePath, this,
            QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllProperties)) {
        m_connection.unregisterService(serviceName);
        return false;
    }
    m_claimed = true;
    return true;
}

void ChooserService::setDialogFactory(DialogFactory factory)
{
    if (m_requests.isEmpty() && factory)
        m_factory = std::move(factory);
}

FileChooserDialog *ChooserService::dialogForRequest(const QString &path) const
{
    const auto request = m_requests.value(path);
    return request ? request->dialog() : nullptr;
}

QDBusObjectPath ChooserService::OpenFile(const QVariantMap &options)
{
    return createRequest(options, false);
}

QDBusObjectPath ChooserService::SaveFile(const QVariantMap &options)
{
    return createRequest(options, true);
}

QDBusObjectPath ChooserService::createRequest(const QVariantMap &options, bool save)
{
    if (!calledFromDBus() || !message().service().startsWith(QLatin1Char(':'))) {
        if (calledFromDBus())
            sendErrorReply(QDBusError::AccessDenied, QStringLiteral("A unique caller connection is required."));
        return QDBusObjectPath(QStringLiteral("/"));
    }
    const QString owner = message().service();
    int ownerCount = 0;
    for (ChooserRequest *request : m_requests)
        ownerCount += request->owner() == owner;
    if (m_requests.size() >= maxRequests || ownerCount >= maxOwnerRequests) {
        sendErrorReply(QDBusError::LimitsExceeded, QStringLiteral("Too many retained chooser requests; Close unused requests."));
        return QDBusObjectPath(QStringLiteral("/"));
    }
    const QString path = servicePath + QStringLiteral("/requests/r%1").arg(++m_serial);
    auto *request = new ChooserRequest(this, path, owner, configuration(options, save));
    if (!m_connection.registerObject(path, request,
            QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals)) {
        delete request;
        sendErrorReply(QDBusError::InternalError, QStringLiteral("Could not register chooser request."));
        return QDBusObjectPath(QStringLiteral("/"));
    }
    m_requests.insert(path, request);
    m_watcher->addWatchedService(owner);
    // The method's automatic reply is sent before this event runs. Clients can
    // still miss Finished; the retained GetResult snapshot closes that race.
    QTimer::singleShot(0, request, [this, request, owner] {
        // Also cover a caller disappearing before the watch's match rule was
        // installed. No dialog should be orphaned by that registration race.
        const auto alive = m_connection.interface()->isServiceRegistered(owner);
        if (!alive.isValid() || !alive.value()) {
            ownerDisconnected(owner);
            return;
        }
        request->start();
    });
    return QDBusObjectPath(path);
}

void ChooserService::removeRequest(const QString &path)
{
    ChooserRequest *request = m_requests.take(path);
    if (!request)
        return;
    const QString owner = request->owner();
    m_connection.unregisterObject(path);
    request->dismissDialog();
    request->deleteLater();
    for (ChooserRequest *other : m_requests) {
        if (other->owner() == owner)
            return;
    }
    m_watcher->removeWatchedService(owner);
}

void ChooserService::ownerDisconnected(const QString &owner)
{
    const auto requests = m_requests;
    for (auto it = requests.cbegin(); it != requests.cend(); ++it) {
        if (it.value()->owner() == owner) {
            it.value()->cancel();
            removeRequest(it.key());
        }
    }
}

#include "ChooserService.moc"
