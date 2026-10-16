#pragma once

#include <QIcon>
#include <QPainter>
#include <QPixmap>

namespace Aero {

// Observed in the offline Windows 7 reference VM: a large translucent icon
// above the pointer, rather than Qt's dragged selection rectangle.
inline QPixmap shellDragPixmap(const QList<QIcon> &icons, qreal dpr = 1.0)
{
    constexpr int size = 96;
    QPixmap image(qRound(size * dpr), qRound(size * dpr));
    image.setDevicePixelRatio(dpr);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    QFont font = painter.font();
    font.setPointSize(9);
    painter.setFont(font);
    painter.setOpacity(0.25);
    painter.setPen(QColor(QStringLiteral("#7d98b5")));
    painter.setBrush(Qt::white);
    painter.drawRect(QRect(1, 1, size - 3, size - 3));
    painter.setOpacity(0.55);
    const int count = icons.size();
    for (int i = 0; i < qMin(count, 3); ++i) {
        const int offset = count > 1 ? i * 6 : 0;
        const int extent = count > 1 ? 84 : size;
        icons.at(i).paint(&painter, QRect(offset, offset, extent, extent));
    }
    if (count > 1) {
        painter.setOpacity(1);
        painter.setPen(Qt::white);
        QLinearGradient blue(0, 40, 0, 58);
        blue.setColorAt(0, QColor(QStringLiteral("#75b7ff")));
        blue.setColorAt(1, QColor(QStringLiteral("#0759d8")));
        painter.setBrush(blue);
        const QString text = QString::number(count);
        const int width = qMax(18, painter.fontMetrics().horizontalAdvance(text) + 8);
        const QRect badge((size - width) / 2, 40, width, 18);
        painter.drawRoundedRect(badge, 2, 2);
        painter.drawText(badge, Qt::AlignCenter, text);
    }
    return image;
}

inline QPixmap shellDragPixmap(const QIcon &icon, qreal dpr = 1.0)
{
    return shellDragPixmap(QList<QIcon>{icon}, dpr);
}

inline QPoint shellDragHotSpot() { return QPoint(48, 88); }

} // namespace Aero
