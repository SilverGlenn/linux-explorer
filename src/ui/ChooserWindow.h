#pragma once

#include <QString>

class QDialog;
class QWidget;

namespace Explorer {
struct ChooserWindowOptions {
    QString parentWindow;
    QString activationToken;
    bool modal = false;
};

// Empty on success. A portal may dispatch to a matching native presenter;
// private requests require a parent compatible with this process.
QString validateChooserWindowOptions(const ChooserWindowOptions &options,
                                    bool allowDifferentBackend = false);
// Call after building the contents, before native window creation. Navigation
// belongs to the glass inset, not to the opaque contents' layout.
void configureChooserGlass(QDialog *dialog, QWidget *contents, QWidget *navigation);
// Call only with validated options, before the dialog has a native window.
void showChooserWindow(QDialog *dialog, const ChooserWindowOptions &options);
}
