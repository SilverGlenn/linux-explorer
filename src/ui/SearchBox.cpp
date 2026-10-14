#include "SearchBox.h"
#include "aero/palette.h"
#include "aero/text.h"

SearchBox::SearchBox(QWidget *parent) : QLineEdit(parent)
{
    setObjectName(QStringLiteral("searchBox"));
    Aero::setPointSize(this, 9);
    setPlaceholderText(tr("Search"));
    setMinimumWidth(140);
    setMaximumWidth(240);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    setFixedHeight(24);
    setClearButtonEnabled(false);
    // Keep the native unfocused field. Do not clear/reapply a focus stylesheet:
    // that can replace the widget font with the application's inherited size.
    setStyleSheet(QStringLiteral(
        "QLineEdit { font-size: 9pt; }"
        "QLineEdit:focus { border: 1px solid %1; border-radius: 2px;"
        " background: %2; color: %3; }")
        .arg(QLatin1String(Aero::Palette::FocusBorder),
             QLatin1String(Aero::Palette::Surface), QLatin1String(Aero::Palette::Text)));
    connect(this, &QLineEdit::textChanged, this, [this](const QString &text) {
        QFont textFont = font();
        textFont.setPointSize(9);
        textFont.setItalic(text.isEmpty());
        setFont(textFont);
    });
    QFont placeholderFont = font();
    placeholderFont.setItalic(true);
    setFont(placeholderFont);
}

void SearchBox::paintEvent(QPaintEvent *event)
{
    // Navigation restores search terms with QSignalBlocker. Derive the visual
    // state from the actual text too, so that blocked updates cannot leave an
    // italic query or a roman placeholder. QLineEdit still paints the native
    // placeholder (including its palette colour and focus visibility policy).
    QFont textFont = font();
    if (textFont.pointSizeF() != 9 || textFont.italic() != text().isEmpty()) {
        textFont.setPointSize(9);
        textFont.setItalic(text().isEmpty());
        setFont(textFont);
    }
    QLineEdit::paintEvent(event);
}
