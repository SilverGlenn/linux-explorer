#include "ChooserPresenter.h"
#include "FileChooserDialog.h"

#include <KWindowSystem>
#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSocketNotifier>
#include <QTimer>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <utility>

namespace {
// Include the worst JSON escaping of 4096 legal leaf names plus choices.
constexpr qsizetype maxRequestBytes = 16 * 1024 * 1024;
// 4096 selections can each have a long, percent-encoded local URI.
constexpr qsizetype maxReplyBytes = 64 * 1024 * 1024;
constexpr int startupTimeoutMs = 10000;

QJsonArray strings(const QStringList &values)
{
    QJsonArray result;
    for (const QString &value : values)
        result.append(value);
    return result;
}

QJsonObject encodeRequest(const ChooserOptions &chooser, const Explorer::ChooserWindowOptions &window)
{
    QJsonArray filters;
    for (const auto &filter : chooser.filters) {
        filters.append(QJsonObject{{QStringLiteral("id"), filter.id}, {QStringLiteral("label"), filter.label},
            {QStringLiteral("globs"), strings(filter.globs)}, {QStringLiteral("mimes"), strings(filter.mimeTypes)}});
    }
    QJsonArray choices;
    for (const auto &choice : chooser.choices) {
        QJsonArray options;
        for (const auto &option : choice.options)
            options.append(QJsonObject{{QStringLiteral("id"), option.id}, {QStringLiteral("label"), option.label}});
        choices.append(QJsonObject{{QStringLiteral("id"), choice.id}, {QStringLiteral("label"), choice.label},
            {QStringLiteral("options"), options}, {QStringLiteral("selected"), choice.selected}});
    }
    return {{QStringLiteral("version"), 1}, {QStringLiteral("mode"), int(chooser.mode)},
        {QStringLiteral("title"), chooser.title},
        {QStringLiteral("location"), chooser.startingLocation.toString(QUrl::FullyEncoded)},
        {QStringLiteral("name"), chooser.suggestedName}, {QStringLiteral("filters"), filters},
        {QStringLiteral("selected_filter"), chooser.selectedFilter},
        {QStringLiteral("extension"), chooser.defaultExtension}, {QStringLiteral("local"), chooser.localOnly},
        {QStringLiteral("multiple"), chooser.multiple}, {QStringLiteral("directory"), chooser.directory},
        {QStringLiteral("accept_label"), chooser.acceptLabel},
        {QStringLiteral("case_sensitive"), chooser.filterCaseSensitivity == Qt::CaseSensitive},
        {QStringLiteral("choices"), choices}, {QStringLiteral("save_names"), strings(chooser.saveNames)},
        {QStringLiteral("parent"), window.parentWindow}, {QStringLiteral("token"), window.activationToken},
        {QStringLiteral("modal"), window.modal}};
}

bool readString(const QJsonObject &object, const QString &key, QString *value, int limit = 4096)
{
    const auto json = object.value(key);
    if (!json.isString())
        return false;
    *value = json.toString();
    return value->size() <= limit && !value->contains(QChar::Null);
}

bool readBool(const QJsonObject &object, const QString &key, bool *value)
{
    const auto json = object.value(key);
    if (!json.isBool())
        return false;
    *value = json.toBool();
    return true;
}

bool readStrings(const QJsonObject &object, const QString &key, QStringList *values, int count, int length)
{
    const auto json = object.value(key);
    if (!json.isArray() || json.toArray().size() > count)
        return false;
    for (const auto &item : json.toArray()) {
        if (!item.isString())
            return false;
        const QString value = item.toString();
        if (value.size() > length || value.contains(QChar::Null))
            return false;
        values->append(value);
    }
    return true;
}

bool decodeRequest(const QJsonObject &object, ChooserOptions *chooser, Explorer::ChooserWindowOptions *window)
{
    if (object.value(QStringLiteral("version")) != QJsonValue(1))
        return false;
    const auto mode = object.value(QStringLiteral("mode"));
    if (mode != QJsonValue(int(ChooserMode::OpenFile)) && mode != QJsonValue(int(ChooserMode::SaveFile))
        && mode != QJsonValue(int(ChooserMode::SaveFiles)))
        return false;
    chooser->mode = ChooserMode(mode.toInt());
    QString location;
    bool caseSensitive = false;
    if (!readString(object, QStringLiteral("title"), &chooser->title)
        || !readString(object, QStringLiteral("location"), &location, 65536)
        || !readString(object, QStringLiteral("name"), &chooser->suggestedName)
        || !readString(object, QStringLiteral("selected_filter"), &chooser->selectedFilter)
        || !readString(object, QStringLiteral("extension"), &chooser->defaultExtension)
        || !readString(object, QStringLiteral("accept_label"), &chooser->acceptLabel, 8192)
        || !readString(object, QStringLiteral("parent"), &window->parentWindow)
        || !readString(object, QStringLiteral("token"), &window->activationToken)
        || !readBool(object, QStringLiteral("local"), &chooser->localOnly)
        || !readBool(object, QStringLiteral("multiple"), &chooser->multiple)
        || !readBool(object, QStringLiteral("directory"), &chooser->directory)
        || !readBool(object, QStringLiteral("modal"), &window->modal)
        || !readBool(object, QStringLiteral("case_sensitive"), &caseSensitive)
        || !readStrings(object, QStringLiteral("save_names"), &chooser->saveNames, 4096, 255))
        return false;
    chooser->startingLocation = QUrl(location, QUrl::StrictMode);
    if ((!location.isEmpty() && !chooser->startingLocation.isValid()) || !chooser->localOnly
        || !Explorer::validateChooserWindowOptions(*window).isEmpty())
        return false;
    chooser->filterCaseSensitivity = caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive;
    const auto filters = object.value(QStringLiteral("filters"));
    if (!filters.isArray() || filters.toArray().size() > 64)
        return false;
    int patterns = 0;
    for (const auto &item : filters.toArray()) {
        if (!item.isObject())
            return false;
        ChooserFilter filter;
        const auto json = item.toObject();
        if (!readString(json, QStringLiteral("id"), &filter.id, 256)
            || !readString(json, QStringLiteral("label"), &filter.label, 1024)
            || !readStrings(json, QStringLiteral("globs"), &filter.globs, 256, 256)
            || !readStrings(json, QStringLiteral("mimes"), &filter.mimeTypes, 256, 256))
            return false;
        patterns += filter.globs.size() + filter.mimeTypes.size();
        if (patterns > 256)
            return false;
        chooser->filters.append(std::move(filter));
    }
    const auto choices = object.value(QStringLiteral("choices"));
    if (!choices.isArray() || choices.toArray().size() > 64)
        return false;
    int options = 0;
    for (const auto &item : choices.toArray()) {
        if (!item.isObject())
            return false;
        ChooserChoice choice;
        const auto json = item.toObject();
        if (!readString(json, QStringLiteral("id"), &choice.id, 256)
            || !readString(json, QStringLiteral("label"), &choice.label, 2048)
            || !readString(json, QStringLiteral("selected"), &choice.selected, 256))
            return false;
        const auto values = json.value(QStringLiteral("options"));
        if (!values.isArray())
            return false;
        for (const auto &value : values.toArray()) {
            if (++options > 256 || !value.isObject())
                return false;
            ChooserChoiceOption option;
            if (!readString(value.toObject(), QStringLiteral("id"), &option.id, 256)
                || !readString(value.toObject(), QStringLiteral("label"), &option.label, 1024))
                return false;
            choice.options.append(std::move(option));
        }
        chooser->choices.append(std::move(choice));
    }
    return true;
}

QJsonObject encodeSelection(uint response, const ChooserSelection &selection)
{
    QJsonArray urls;
    for (const auto &url : selection.urls)
        urls.append(url.toString(QUrl::FullyEncoded));
    QJsonObject choices;
    for (auto it = selection.choices.cbegin(); it != selection.choices.cend(); ++it)
        choices.insert(it.key(), it.value());
    return {{QStringLiteral("event"), QStringLiteral("finished")}, {QStringLiteral("response"), int(response)},
        {QStringLiteral("uris"), urls}, {QStringLiteral("filter"), selection.filter},
        {QStringLiteral("choices"), choices}};
}

bool decodeSelection(const QJsonObject &object, uint *response, ChooserSelection *selection)
{
    const auto status = object.value(QStringLiteral("response"));
    if (status != QJsonValue(0) && status != QJsonValue(1) && status != QJsonValue(2))
        return false;
    *response = uint(status.toInt());
    QStringList urls;
    if (!readStrings(object, QStringLiteral("uris"), &urls, 4096, 65536)
        || !readString(object, QStringLiteral("filter"), &selection->filter, 256))
        return false;
    for (const auto &url : urls)
        selection->urls.append(QUrl(url, QUrl::StrictMode));
    const auto choices = object.value(QStringLiteral("choices"));
    if (!choices.isObject() || choices.toObject().size() > 64)
        return false;
    const auto values = choices.toObject();
    for (auto it = values.begin(); it != values.end(); ++it) {
        if (it.key().size() > 256 || it.key().contains(QChar::Null) || !it.value().isString()
            || it.value().toString().size() > 256 || it.value().toString().contains(QChar::Null))
            return false;
        selection->choices.insert(it.key(), it.value().toString());
    }
    return true;
}

bool writeOutput(const QJsonObject &object)
{
    const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
    if (bytes.size() > maxReplyBytes)
        return false;
    QFile output;
    if (!output.open(STDOUT_FILENO, QIODevice::WriteOnly))
        return false;
    return output.write(bytes) == bytes.size() && output.flush();
}

class PresenterChild : public QObject {
public:
    explicit PresenterChild(QObject *parent = nullptr) : QObject(parent)
    {
        const int flags = fcntl(STDIN_FILENO, F_GETFL);
        if (flags < 0 || fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK) < 0) {
            QTimer::singleShot(0, this, [] { QCoreApplication::exit(1); });
            return;
        }
        m_input = new QSocketNotifier(STDIN_FILENO, QSocketNotifier::Read, this);
        connect(m_input, &QSocketNotifier::activated, this, [this] { readInput(); });
    }

    ~PresenterChild() override { dismiss(); }

private:
    void readInput()
    {
        char buffer[16384];
        for (;;) {
            const ssize_t size = ::read(STDIN_FILENO, buffer, sizeof(buffer));
            if (size == 0) {
                finish(1);
                return;
            }
            if (size < 0) {
                if (errno == EINTR)
                    continue;
                if (errno != EAGAIN && errno != EWOULDBLOCK)
                    finish(2);
                return;
            }
            m_bytes.append(buffer, size);
            if (m_bytes.size() > maxRequestBytes) {
                finish(2);
                return;
            }
            for (;;) {
                const qsizetype end = m_bytes.indexOf('\n');
                if (end < 0)
                    break;
                const QByteArray line = m_bytes.first(end);
                m_bytes.remove(0, end + 1);
                if (m_started) {
                    finish(line == "cancel" ? 1 : 2);
                    return;
                }
                QJsonParseError error;
                const auto document = QJsonDocument::fromJson(line, &error);
                ChooserOptions chooser;
                Explorer::ChooserWindowOptions window;
                if (error.error != QJsonParseError::NoError || !document.isObject()
                    || !decodeRequest(document.object(), &chooser, &window)) {
                    finish(2);
                    return;
                }
                m_started = true;
                m_dialog = new FileChooserDialog(chooser);
                m_dialog->setAttribute(Qt::WA_DeleteOnClose, false);
                connect(m_dialog, &QDialog::finished, this, [this](int status) {
                    ChooserSelection selection;
                    if (status == QDialog::Accepted && m_dialog) {
                        selection = {m_dialog->selectedUrls(), m_dialog->selectedFilter(), m_dialog->selectedChoices()};
                    }
                    finish(status == QDialog::Accepted ? 0 : 1, selection);
                });
                connect(m_dialog, &QObject::destroyed, this, [this] {
                    m_dialog = nullptr;
                    if (!m_terminal)
                        finish(2);
                });
                Explorer::showChooserWindow(m_dialog, window);
                if (!writeOutput({{QStringLiteral("event"), QStringLiteral("ready")}})) {
                    finish(2);
                    return;
                }
            }
        }
    }

    void finish(uint response, const ChooserSelection &selection = {})
    {
        if (m_terminal)
            return;
        m_terminal = true;
        if (m_input)
            m_input->setEnabled(false);
        dismiss();
        const bool written = writeOutput(encodeSelection(response, selection));
        QCoreApplication::exit(written ? 0 : 1);
    }

    void dismiss()
    {
        QPointer<FileChooserDialog> dialog = m_dialog;
        m_dialog = nullptr;
        if (!dialog)
            return;
        QList<QPointer<QDialog>> children;
        for (auto *child : dialog->findChildren<QDialog *>())
            children.prepend(child);
        for (const auto &child : children)
            if (child)
                child->reject();
        if (dialog) {
            dialog->reject();
            dialog->deleteLater();
        }
    }

    QSocketNotifier *m_input = nullptr;
    QByteArray m_bytes;
    QPointer<FileChooserDialog> m_dialog;
    bool m_started = false;
    bool m_terminal = false;
};
} // namespace

ChooserPresenter::ChooserPresenter(QObject *parent) : QObject(parent), m_startupTimer(new QTimer(this))
{
    m_startupTimer->setSingleShot(true);
    connect(m_startupTimer, &QTimer::timeout, this, [this] { complete(2); });
}

ChooserPresenter::~ChooserPresenter()
{
    cancel();
}

QString ChooserPresenter::requiredPlatform(const Explorer::ChooserWindowOptions &options)
{
    if (options.parentWindow.startsWith(QLatin1String("x11:")) && !KWindowSystem::isPlatformX11())
        return QStringLiteral("xcb");
    if (options.parentWindow.startsWith(QLatin1String("wayland:")) && !KWindowSystem::isPlatformWayland())
        return QStringLiteral("wayland");
    return {};
}

int ChooserPresenter::runChild()
{
    PresenterChild child;
    return QCoreApplication::exec();
}

void ChooserPresenter::start(const ChooserOptions &chooser, const Explorer::ChooserWindowOptions &window,
                             Completion completion)
{
    if (m_process || m_done)
        return;
    m_completion = std::move(completion);
    const QByteArray request = QJsonDocument(encodeRequest(chooser, window)).toJson(QJsonDocument::Compact) + '\n';
    const QString platform = requiredPlatform(window);
    if (request.size() > maxRequestBytes || platform.isEmpty()) {
        QTimer::singleShot(0, this, [this] { complete(2); });
        return;
    }
    auto *process = new QProcess(this);
    m_process = process;
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("QT_QPA_PLATFORM"), platform);
    process->setProcessEnvironment(environment);
    process->setProcessChannelMode(QProcess::ForwardedErrorChannel);
    process->setProgram(QCoreApplication::applicationFilePath());
    QStringList arguments{QStringLiteral("--present-chooser")};
    // Qt's xcb plugin consumes -name before our command-line parser. Keep the
    // chooser-specific resource name used by picker decoration rules.
    if (platform == QLatin1String("xcb"))
        arguments.append({QStringLiteral("-name"), QStringLiteral("linux-explorer-win7-chooser-dialog")});
    process->setArguments(arguments);
    connect(process, &QProcess::started, this, [this, request] {
        if (m_process && m_process->write(request) != request.size())
            complete(2);
    });
    connect(process, &QProcess::readyReadStandardOutput, this, [this] { readOutput(); });
    connect(process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart || error == QProcess::ReadError || error == QProcess::WriteError)
            complete(2);
    });
    connect(process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        readOutput();
        if (!m_done)
            complete(status == QProcess::NormalExit && code == 0 && m_received && m_output.isEmpty() ? m_response : 2,
                     m_selection);
    });
    m_startupTimer->start(startupTimeoutMs);
    process->start();
}

void ChooserPresenter::readOutput()
{
    if (m_done || !m_process)
        return;
    m_output.append(m_process->read(maxReplyBytes - m_output.size() + 1));
    if (m_output.size() > maxReplyBytes) {
        complete(2);
        return;
    }
    for (;;) {
        const qsizetype end = m_output.indexOf('\n');
        if (end < 0)
            return;
        const QByteArray line = m_output.first(end);
        m_output.remove(0, end + 1);
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(line, &error);
        if (error.error != QJsonParseError::NoError || !document.isObject() || m_received) {
            complete(2);
            return;
        }
        const auto object = document.object();
        const auto event = object.value(QStringLiteral("event"));
        if (event == QJsonValue(QStringLiteral("ready")) && !m_ready && object.size() == 1) {
            m_ready = true;
            m_startupTimer->stop();
        } else if (event == QJsonValue(QStringLiteral("finished"))
                   && decodeSelection(object, &m_response, &m_selection) && (m_ready || m_response == 2)) {
            m_received = true;
            m_startupTimer->start(2000);
        } else {
            complete(2);
            return;
        }
    }
}

void ChooserPresenter::complete(uint response, const ChooserSelection &selection)
{
    if (m_done)
        return;
    m_done = true;
    m_startupTimer->stop();
    releaseProcess(response == 2);
    auto completion = std::move(m_completion);
    if (completion)
        completion(response, selection);
}

void ChooserPresenter::cancel()
{
    m_done = true;
    m_completion = {};
    m_startupTimer->stop();
    releaseProcess(false);
}

void ChooserPresenter::releaseProcess(bool force)
{
    QProcess *process = m_process;
    m_process = nullptr;
    if (!process)
        return;
    disconnect(process, nullptr, this, nullptr);
    // Request deletion must not synchronously wait for a child to unwind an
    // overwrite dialog. The pipe EOF also closes it if the portal process dies.
    process->setParent(QCoreApplication::instance());
    if (process->state() == QProcess::NotRunning) {
        process->deleteLater();
        return;
    }
    connect(process, &QProcess::finished, process, &QObject::deleteLater);
    if (force) {
        process->kill();
    } else {
        process->write("cancel\n");
        process->closeWriteChannel();
        QTimer::singleShot(1000, process, [process] {
            if (process->state() != QProcess::NotRunning)
                process->kill();
        });
    }
}
