#include "SearchBox.h"
#include "aero/palette.h"
#include "aero/text.h"

#include <QPainter>

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
    // AeroQt's glass ancestor supplies a border-image, even when a local focus
    // rule sets a border. Suppress both style frames in every state and paint
    // exactly one frame after the native editor. Keep QLineEdit responsible for
    // editing, selection, actions and accessibility, with a stable text inset.
    setFrame(false);
    setStyleSheet(QStringLiteral(
        "#searchBox { font-size: 9pt; border-image: none; border: none;"
        " border-radius: 0; padding: 2px; background: %1; color: %2; }")
        .arg(QLatin1String(Aero::Palette::Surface), QLatin1String(Aero::Palette::Text)));
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

    // Filled edge strips remain continuous at fractional DPR too, without a
    // second native/QSS frame or a half-clipped pen at the widget boundary.
    QPainter painter(this);
    const QColor border = !isEnabled() ? palette().color(QPalette::Disabled, QPalette::Mid)
        : hasFocus() ? QColor(QLatin1String(Aero::Palette::FocusBorder)) : QColor("#7F9DB9");
    painter.fillRect(QRect(0, 0, width(), 1), border);
    painter.fillRect(QRect(0, height() - 1, width(), 1), border);
    painter.fillRect(QRect(0, 1, 1, height() - 2), border);
    painter.fillRect(QRect(width() - 1, 1, 1, height() - 2), border);
}
