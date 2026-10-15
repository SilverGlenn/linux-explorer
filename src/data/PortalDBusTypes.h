#pragma once

#include <QByteArray>
#include <QDBusArgument>
#include <QList>
#include <QString>

// Standard portal wire types only; this target has no widget/UI dependency.
struct PortalFilterRule { uint type; QString value; };
struct PortalFilter { QString name; QList<PortalFilterRule> rules; };
using PortalFilters = QList<PortalFilter>;
struct PortalChoiceOption { QString id; QString label; };
struct PortalChoice { QString id; QString label; QList<PortalChoiceOption> options; QString selected; };
using PortalChoices = QList<PortalChoice>;
struct PortalChoiceResult { QString id; QString selected; };
using PortalChoiceResults = QList<PortalChoiceResult>;
using PortalFileNames = QList<QByteArray>;

Q_DECLARE_METATYPE(PortalFilterRule)
Q_DECLARE_METATYPE(PortalFilter)
Q_DECLARE_METATYPE(PortalFilters)
Q_DECLARE_METATYPE(PortalChoiceOption)
Q_DECLARE_METATYPE(PortalChoice)
Q_DECLARE_METATYPE(PortalChoices)
Q_DECLARE_METATYPE(PortalChoiceResult)
Q_DECLARE_METATYPE(PortalChoiceResults)
Q_DECLARE_METATYPE(PortalFileNames)

QDBusArgument &operator<<(QDBusArgument &, const PortalFilterRule &);
const QDBusArgument &operator>>(const QDBusArgument &, PortalFilterRule &);
QDBusArgument &operator<<(QDBusArgument &, const PortalFilter &);
const QDBusArgument &operator>>(const QDBusArgument &, PortalFilter &);
QDBusArgument &operator<<(QDBusArgument &, const PortalChoiceOption &);
const QDBusArgument &operator>>(const QDBusArgument &, PortalChoiceOption &);
QDBusArgument &operator<<(QDBusArgument &, const PortalChoice &);
const QDBusArgument &operator>>(const QDBusArgument &, PortalChoice &);
QDBusArgument &operator<<(QDBusArgument &, const PortalChoiceResult &);
const QDBusArgument &operator>>(const QDBusArgument &, PortalChoiceResult &);
void registerPortalDBusTypes();
