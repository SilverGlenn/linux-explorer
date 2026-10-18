#include "PortalChooserOptions.h"

#include <algorithm>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>

namespace {
bool text(const QString &value, int limit, bool nonempty = false)
{
    return (!nonempty || !value.isEmpty()) && value.size() <= limit && !value.contains(QChar::Null);
}

// Control characters and the bidi controls (U+202A-202E, U+2066-2069,
// U+200E/F, U+061C) can disguise a name or caption. Other format characters,
// the zero-width joiners above all, are ordinary in Persian, Indic and emoji
// names and stay. The private service rejects control text too.
bool disguising(QChar ch)
{
    const ushort c = ch.unicode();
    return ch.category() == QChar::Other_Control
        || (c >= 0x202A && c <= 0x202E) || (c >= 0x2066 && c <= 0x2069)
        || c == 0x200E || c == 0x200F || c == 0x061C;
}

bool hasHiddenFormatting(const QString &value)
{
    return std::any_of(value.cbegin(), value.cend(), disguising);
}

// Captions are display-only: strip what could disguise them, then truncate.
QString caption(const QString &value, qsizetype limit)
{
    QString result;
    for (const QChar ch : value) {
        if (!disguising(ch))
            result += ch;
    }
    if (result.size() > limit) {
        result.truncate(limit);
        if (result.back().isHighSurrogate())
            result.chop(1);
    }
    return result;
}

QString mnemonic(const QString &value)
{
    QString result;
    for (qsizetype i = 0; i < value.size(); ++i) {
        const QChar ch = value.at(i);
        if (ch == QLatin1Char('&')) {
            result += QStringLiteral("&&");
        } else if (ch == QLatin1Char('_') && i + 1 < value.size()) {
            if (value.at(i + 1) == QLatin1Char('_')) {
                result += QLatin1Char('_');
                ++i;
            } else {
                result += QLatin1Char('&');
            }
        } else {
            result += ch;
        }
    }
    return result;
}

bool fileBytes(const QByteArray &value, QString *result, bool leaf)
{
    if (value.isEmpty() || value.size() > 16384 || !value.endsWith('\0'))
        return false;
    const QByteArray bytes = value.first(value.size() - 1);
    if (bytes.contains('\0'))
        return false;
    const QString decoded = QFile::decodeName(bytes);
    if (QFile::encodeName(decoded) != bytes)
        return false; // Qt cannot represent these filesystem bytes losslessly.
    if (leaf) {
        if (bytes.isEmpty() || bytes.size() > 255 || decoded == QLatin1String(".")
            || decoded == QLatin1String("..") || decoded.contains(QLatin1Char('/')))
            return false;
    } else if (!decoded.isEmpty() && !QDir::isAbsolutePath(decoded)) {
        return false;
    }
    *result = decoded;
    return true;
}

bool readFilter(const QDBusArgument &arg, PortalFilter *filter, int *rules)
{
    arg.beginStructure();
    arg >> filter->name;
    if (!text(filter->name, 1024, true))
        return false;
    arg.beginArray();
    while (!arg.atEnd()) {
        if (++*rules > 256)
            return false;
        PortalFilterRule rule;
        arg >> rule;
        if (!text(rule.value, 256, true))
            return false;
        filter->rules.append(rule);
    }
    arg.endArray();
    arg.endStructure();
    return true;
}

bool readFilters(const QVariant &value, PortalFilters *filters, bool single)
{
    if (value.metaType() != QMetaType::fromType<QDBusArgument>())
        return false;
    const QDBusArgument arg = value.value<QDBusArgument>();
    if (arg.currentSignature() != (single ? QLatin1String("(sa(us))") : QLatin1String("a(sa(us))")))
        return false;
    int rules = 0;
    if (single) {
        PortalFilter filter;
        if (!readFilter(arg, &filter, &rules))
            return false;
        filters->append(filter);
    } else {
        arg.beginArray();
        while (!arg.atEnd()) {
            if (filters->size() >= 64)
                return false;
            PortalFilter filter;
            if (!readFilter(arg, &filter, &rules))
                return false;
            filters->append(filter);
        }
        arg.endArray();
    }
    return true;
}

bool readChoices(const QVariant &value, PortalChoices *choices)
{
    if (value.metaType() != QMetaType::fromType<QDBusArgument>())
        return false;
    const QDBusArgument arg = value.value<QDBusArgument>();
    if (arg.currentSignature() != QLatin1String("a(ssa(ss)s)"))
        return false;
    int options = 0;
    arg.beginArray();
    while (!arg.atEnd()) {
        if (choices->size() >= 64)
            return false;
        PortalChoice choice;
        arg.beginStructure();
        arg >> choice.id >> choice.label;
        if (!text(choice.id, 256, true) || !text(choice.label, 1024, true))
            return false;
        arg.beginArray();
        while (!arg.atEnd()) {
            if (++options > 256)
                return false;
            PortalChoiceOption option;
            arg >> option;
            if (!text(option.id, 256, true) || !text(option.label, 1024, true))
                return false;
            choice.options.append(option);
        }
        arg.endArray();
        arg >> choice.selected;
        if (!text(choice.selected, 256))
            return false;
        arg.endStructure();
        choices->append(choice);
    }
    arg.endArray();
    return true;
}

bool sameFilter(const PortalFilter &a, const PortalFilter &b)
{
    if (a.name != b.name || a.rules.size() != b.rules.size())
        return false;
    for (qsizetype i = 0; i < a.rules.size(); ++i) {
        if (a.rules.at(i).type != b.rules.at(i).type || a.rules.at(i).value != b.rules.at(i).value)
            return false;
    }
    return true;
}
} // namespace

PortalChooserOptions portalChooserOptions(ChooserMode mode, const QString &parentWindow,
                                         const QString &title, const QVariantMap &wire)
{
    PortalChooserOptions config;
    config.chooser.mode = mode;
    config.chooser.title = caption(title, 256);
    config.chooser.filterCaseSensitivity = Qt::CaseSensitive;
    auto invalid = [&config](const QString &error) {
        config.error = error;
        return config;
    };
    if (!text(title, 4096) || !text(parentWindow, 4096) || wire.size() > 128)
        return invalid(QStringLiteral("Request metadata exceeds limits."));
    const QSet<QString> strings{QStringLiteral("accept_label"), QStringLiteral("current_name"), QStringLiteral("activation_token")};
    const QSet<QString> booleans{QStringLiteral("modal"), QStringLiteral("multiple"), QStringLiteral("directory")};
    const QSet<QString> bytes{QStringLiteral("current_folder"), QStringLiteral("current_file")};
    for (auto it = wire.cbegin(); it != wire.cend(); ++it) {
        if (strings.contains(it.key())) {
            if (it.value().metaType() != QMetaType::fromType<QString>() || !text(it.value().toString(), 4096))
                return invalid(QStringLiteral("Invalid string option: %1").arg(it.key()));
        } else if (booleans.contains(it.key())) {
            if (it.value().metaType() != QMetaType::fromType<bool>())
                return invalid(QStringLiteral("Invalid boolean option: %1").arg(it.key()));
        } else if (bytes.contains(it.key()) && it.value().metaType() != QMetaType::fromType<QByteArray>()) {
            return invalid(QStringLiteral("Invalid byte path option: %1").arg(it.key()));
        }
        // Unknown backend options are intentionally ignored for forward compatibility.
    }
    config.chooser.acceptLabel = mnemonic(caption(wire.value(QStringLiteral("accept_label")).toString(), 64));
    config.chooser.multiple = wire.value(QStringLiteral("multiple"), false).toBool();
    config.chooser.directory = wire.value(QStringLiteral("directory"), false).toBool();
    if (mode != ChooserMode::OpenFile && (config.chooser.multiple || config.chooser.directory))
        return invalid(QStringLiteral("Save methods do not accept multiple/directory flags."));
    config.window = {parentWindow, wire.value(QStringLiteral("activation_token")).toString(),
                     wire.value(QStringLiteral("modal"), true).toBool() && !parentWindow.isEmpty()};
    // A different native display backend is handled by a presenter process;
    // malformed parents remain errors rather than silently losing parenting.
    config.error = Explorer::validateChooserWindowOptions(config.window, true);
    if (!config.error.isEmpty())
        return config;

    QString folder, file;
    if (wire.contains(QStringLiteral("current_folder"))
        && !fileBytes(wire.value(QStringLiteral("current_folder")).toByteArray(), &folder, false))
        return invalid(QStringLiteral("current_folder must be a terminated, lossless absolute filesystem path."));
    if (wire.contains(QStringLiteral("current_file"))
        && !fileBytes(wire.value(QStringLiteral("current_file")).toByteArray(), &file, false))
        return invalid(QStringLiteral("current_file must be a terminated, lossless absolute filesystem path."));
    const QString name = wire.value(QStringLiteral("current_name")).toString();
    if (!name.isEmpty() && (QFile::encodeName(name).size() > 255 || name == QLatin1String(".")
        || name == QLatin1String("..") || name.contains(QLatin1Char('/')) || hasHiddenFormatting(name)
        || QFile::decodeName(QFile::encodeName(name)) != name))
        return invalid(QStringLiteral("current_name must be a lossless leaf filename without control or bidi formatting characters."));
    if (mode == ChooserMode::SaveFile && !file.isEmpty()) {
        const QFileInfo info(file);
        QString leaf;
        if (info.isDir() || !fileBytes(QFile::encodeName(info.fileName()) + '\0', &leaf, true))
            return invalid(QStringLiteral("current_file must name a valid file, not a folder."));
        // Existing-file context wins over current_folder; current_name, when
        // nonempty, overrides only its basename. Empty suggestions are absent.
        folder = info.absolutePath();
        config.chooser.suggestedName = info.fileName();
    }
    if (mode == ChooserMode::SaveFile && !name.isEmpty())
        config.chooser.suggestedName = name;
    if (!folder.isEmpty())
        config.chooser.startingLocation = QUrl::fromLocalFile(folder);

    PortalFilters filters, current;
    if (wire.contains(QStringLiteral("filters")) && !readFilters(wire.value(QStringLiteral("filters")), &filters, false))
        return invalid(QStringLiteral("Invalid or oversized filters; expected a(sa(us))."));
    if (wire.contains(QStringLiteral("current_filter")) && !readFilters(wire.value(QStringLiteral("current_filter")), &current, true))
        return invalid(QStringLiteral("Invalid current_filter; expected (sa(us))."));
    int selected = -1;
    if (!current.isEmpty()) {
        if (filters.isEmpty())
            filters = current;
        for (qsizetype i = 0; i < filters.size(); ++i) {
            if (sameFilter(filters.at(i), current.first())) {
                selected = i;
                break;
            }
        }
        if (selected < 0)
            return invalid(QStringLiteral("current_filter must match a supplied filter exactly."));
    }
    int rules = 0;
    static const QRegularExpression mime(QStringLiteral("^[A-Za-z0-9!#$&^_.+\\-]+/(?:[A-Za-z0-9!#$&^_.+\\-]+|\\*)$"));
    for (qsizetype i = 0; i < filters.size(); ++i) {
        const PortalFilter &filter = filters.at(i);
        if (filter.rules.isEmpty())
            return invalid(QStringLiteral("Each filter must have at least one rule."));
        ChooserFilter internal{QString::number(i), filter.name, {}, {}};
        for (const PortalFilterRule &rule : filter.rules) {
            if (++rules > 256 || (rule.type != 0 && rule.type != 1))
                return invalid(QStringLiteral("Invalid filter rule type or count."));
            if (rule.type == 0) {
                if (!QRegularExpression(QRegularExpression::wildcardToRegularExpression(
                        rule.value, QRegularExpression::NonPathWildcardConversion)).isValid())
                    return invalid(QStringLiteral("Invalid glob pattern."));
                internal.globs.append(rule.value);
            } else {
                if (!mime.match(rule.value).hasMatch())
                    return invalid(QStringLiteral("Invalid MIME filter pattern."));
                internal.mimeTypes.append(rule.value);
            }
        }
        config.chooser.filters.append(internal);
    }
    config.filters = filters;
    if (selected >= 0)
        config.chooser.selectedFilter = QString::number(selected);

    PortalChoices choices;
    if (wire.contains(QStringLiteral("choices")) && !readChoices(wire.value(QStringLiteral("choices")), &choices))
        return invalid(QStringLiteral("Invalid or oversized choices; expected a(ssa(ss)s)."));
    QSet<QString> ids;
    for (const PortalChoice &choice : choices) {
        if (ids.contains(choice.id))
            return invalid(QStringLiteral("Choice ids must be unique."));
        ids.insert(choice.id);
        ChooserChoice internal{choice.id, QString(choice.label).replace(QLatin1Char('&'), QStringLiteral("&&")), {}, choice.selected};
        QSet<QString> optionIds;
        for (const PortalChoiceOption &option : choice.options) {
            if (optionIds.contains(option.id))
                return invalid(QStringLiteral("Choice option ids must be unique."));
            optionIds.insert(option.id);
            internal.options.append({option.id, option.label});
        }
        if (choice.options.isEmpty()) {
            if (!choice.selected.isEmpty() && choice.selected != QLatin1String("true") && choice.selected != QLatin1String("false"))
                return invalid(QStringLiteral("Boolean choices require true/false selection."));
        } else if (!choice.selected.isEmpty() && !optionIds.contains(choice.selected)) {
            return invalid(QStringLiteral("Unknown selected choice option."));
        }
        config.chooser.choices.append(internal);
    }

    if (wire.contains(QStringLiteral("files"))) {
        const QVariant value = wire.value(QStringLiteral("files"));
        if (value.metaType() != QMetaType::fromType<QDBusArgument>()
            || value.value<QDBusArgument>().currentSignature() != QLatin1String("aay"))
            return invalid(QStringLiteral("files must have signature aay."));
        const QDBusArgument arg = value.value<QDBusArgument>();
        arg.beginArray();
        while (!arg.atEnd()) {
            if (config.chooser.saveNames.size() >= 4096)
                return invalid(QStringLiteral("Too many filenames (maximum 4096)."));
            QByteArray bytes;
            arg >> bytes;
            QString leaf;
            if (!fileBytes(bytes, &leaf, true))
                return invalid(QStringLiteral("files must contain terminated, lossless leaf filenames."));
            config.chooser.saveNames.append(leaf);
        }
        arg.endArray();
    }
    if (mode == ChooserMode::SaveFiles && config.chooser.saveNames.isEmpty())
        return invalid(QStringLiteral("SaveFiles requires at least one filename."));
    return config;
}
