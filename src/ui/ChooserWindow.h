#pragma once

#include <QString>

class QDialog;

namespace Explorer {
struct ChooserWindowOptions {
    QString parentWindow;
    QString activationToken;
    bool modal = false;
};

// Empty on success. Modal private requests require a compatible foreign parent.
QString validateChooserWindowOptions(const ChooserWindowOptions &options);
// Call only with validated options, before the dialog has a native window.
void showChooserWindow(QDialog *dialog, const ChooserWindowOptions &options);
}
