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
    // Use the address bar's translucent Aero entry, including while editing.
    // Explicit local states keep native focus painting from replacing it with
    // an opaque base or a second frame. QLineEdit still owns all input/actions.
    setStyleSheet(QStringLiteral(
        "#searchBox { font-size: 9pt; background: transparent; color: %1;"
        " border-image: url(:/AeroQt/transpbg/entry/normal.png) 4 4 4 4 repeat;"
        " border-width: 4px; padding: 0; }"
        "#searchBox:hover, #searchBox:focus {"
        " border-image: url(:/AeroQt/transpbg/entry/hover.png) 4 4 4 4 repeat; }"
        "#searchBox:disabled {"
        " border-image: url(:/AeroQt/transpbg/entry/disabled.png) 4 4 4 4 repeat; }")
        .arg(QLatin1String(Aero::Palette::Text)));
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

void SearchBox::focusInEvent(QFocusEvent *event)
{
    QLineEdit::focusInEvent(event);
    m_blurredPalette = palette();
    QPalette focusedPalette = m_blurredPalette;
    focusedPalette.setColor(QPalette::All, QPalette::PlaceholderText, Qt::transparent);
    setPalette(focusedPalette);
}

void SearchBox::focusOutEvent(QFocusEvent *event)
{
    QLineEdit::focusOutEvent(event);
    QPalette blurredPalette = palette();
    for (QPalette::ColorGroup group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled})
        blurredPalette.setBrush(group, QPalette::PlaceholderText,
                                m_blurredPalette.brush(group, QPalette::PlaceholderText));
    setPalette(blurredPalette);
}

void SearchBox::paintEvent(QPaintEvent *event)
{
    // Navigation restores search terms with QSignalBlocker. Derive the visual
    // state from the actual text too, so that blocked updates cannot leave an
    // italic query or a roman placeholder. Only this widget's font is adjusted;
    // QLineEdit paints the native placeholder with our focus-only palette rule.
    QFont textFont = font();
    if (textFont.pointSizeF() != 9 || textFont.italic() != text().isEmpty()) {
        textFont.setPointSize(9);
        textFont.setItalic(text().isEmpty());
        setFont(textFont);
    }
    QLineEdit::paintEvent(event);
}
