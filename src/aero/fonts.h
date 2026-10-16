#pragma once

#include <QFont>
#include <QString>

class QApplication;
class QWidget;

namespace Aero {

enum class ClassicFont { MicrosoftSansSerif, Tahoma };

QFont shellFont(qreal pointSize = 9);
QFont classicDialogFont(ClassicFont family = ClassicFont::MicrosoftSansSerif);
QFont textPreviewFont();

// Optional, user-supplied reference fonts remain outside application packages.
QString referenceFontDirectory();
int loadReferenceFonts(const QString &directory = {});
void configureFonts(QApplication *app);
void applyClassicDialogFont(QWidget *dialog,
                            ClassicFont family = ClassicFont::MicrosoftSansSerif);

} // namespace Aero
