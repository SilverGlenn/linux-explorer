#pragma once

#include <QDBusArgument>
#include <QList>
#include <QString>
#include <QStringList>

// Wire-only types: keep caller metadata and D-Bus serialization out of the UI.
struct ChooserWireFilter {
    QString id;
    QString label;
    QStringList globs;
    QStringList mimeTypes;
};
using ChooserWireFilters = QList<ChooserWireFilter>;
Q_DECLARE_METATYPE(ChooserWireFilter)
Q_DECLARE_METATYPE(ChooserWireFilters)

QDBusArgument &operator<<(QDBusArgument &argument, const ChooserWireFilter &filter);
const QDBusArgument &operator>>(const QDBusArgument &argument, ChooserWireFilter &filter);
void registerChooserDBusTypes();
