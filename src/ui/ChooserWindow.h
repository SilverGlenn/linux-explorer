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

// Empty on success. Modal private requests require a compatible foreign parent.
QString validateChooserWindowOptions(const ChooserWindowOptions &options);
// Call after building the contents, before native window creation. Navigation
// belongs to the glass inset, not to the opaque contents' layout.
void configureChooserGlass(QDialog *dialog, QWidget *contents, QWidget *navigation);
// Call only with validated options, before the dialog has a native window.
void showChooserWindow(QDialog *dialog, const ChooserWindowOptions &options);
}
