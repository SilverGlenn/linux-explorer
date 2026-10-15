#include "PortalDBusTypes.h"
#include <QDBusMetaType>

QDBusArgument &operator<<(QDBusArgument &arg, const PortalFilterRule &value)
{
    arg.beginStructure(); arg << value.type << value.value; arg.endStructure(); return arg;
}
const QDBusArgument &operator>>(const QDBusArgument &arg, PortalFilterRule &value)
{
    arg.beginStructure(); arg >> value.type >> value.value; arg.endStructure(); return arg;
}
QDBusArgument &operator<<(QDBusArgument &arg, const PortalFilter &value)
{
    arg.beginStructure(); arg << value.name << value.rules; arg.endStructure(); return arg;
}
const QDBusArgument &operator>>(const QDBusArgument &arg, PortalFilter &value)
{
    arg.beginStructure(); arg >> value.name >> value.rules; arg.endStructure(); return arg;
}
QDBusArgument &operator<<(QDBusArgument &arg, const PortalChoiceOption &value)
{
    arg.beginStructure(); arg << value.id << value.label; arg.endStructure(); return arg;
}
const QDBusArgument &operator>>(const QDBusArgument &arg, PortalChoiceOption &value)
{
    arg.beginStructure(); arg >> value.id >> value.label; arg.endStructure(); return arg;
}
QDBusArgument &operator<<(QDBusArgument &arg, const PortalChoice &value)
{
    arg.beginStructure(); arg << value.id << value.label << value.options << value.selected; arg.endStructure(); return arg;
}
const QDBusArgument &operator>>(const QDBusArgument &arg, PortalChoice &value)
{
    arg.beginStructure(); arg >> value.id >> value.label >> value.options >> value.selected; arg.endStructure(); return arg;
}
QDBusArgument &operator<<(QDBusArgument &arg, const PortalChoiceResult &value)
{
    arg.beginStructure(); arg << value.id << value.selected; arg.endStructure(); return arg;
}
const QDBusArgument &operator>>(const QDBusArgument &arg, PortalChoiceResult &value)
{
    arg.beginStructure(); arg >> value.id >> value.selected; arg.endStructure(); return arg;
}
void registerPortalDBusTypes()
{
    qDBusRegisterMetaType<PortalFilterRule>();
    qDBusRegisterMetaType<PortalFilter>();
    qDBusRegisterMetaType<PortalFilters>();
    qDBusRegisterMetaType<PortalChoiceOption>();
    qDBusRegisterMetaType<PortalChoice>();
    qDBusRegisterMetaType<PortalChoices>();
    qDBusRegisterMetaType<PortalChoiceResult>();
    qDBusRegisterMetaType<PortalChoiceResults>();
    qDBusRegisterMetaType<PortalFileNames>();
}
