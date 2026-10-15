#pragma once

#include "ChooserOptions.h"
#include "ChooserWindow.h"
#include "PortalDBusTypes.h"
#include <QVariantMap>

struct PortalChooserOptions {
    ChooserOptions chooser;
    Explorer::ChooserWindowOptions window;
    // Each internal filter id is its index here, never a reconstructed tuple.
    PortalFilters filters;
    QString error;
};

PortalChooserOptions portalChooserOptions(ChooserMode mode, const QString &parentWindow,
                                         const QString &title, const QVariantMap &options);
