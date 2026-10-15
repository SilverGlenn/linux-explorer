#include "ChooserDBusTypes.h"

#include <QDBusMetaType>

QDBusArgument &operator<<(QDBusArgument &argument, const ChooserWireFilter &filter)
{
    argument.beginStructure();
    argument << filter.id << filter.label << filter.globs << filter.mimeTypes;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, ChooserWireFilter &filter)
{
    argument.beginStructure();
    argument >> filter.id >> filter.label >> filter.globs >> filter.mimeTypes;
    argument.endStructure();
    return argument;
}

void registerChooserDBusTypes()
{
    qDBusRegisterMetaType<ChooserWireFilter>();
    qDBusRegisterMetaType<ChooserWireFilters>();
}
