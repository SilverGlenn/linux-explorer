#include "icons.h"

#include <QFont>
#include <QLinearGradient>
#include <QPainter>
#include <QPixmap>
#include <functional>

namespace Aero {

namespace {

QStringList &fallbacks()
{
    static QStringList names;
    return names;
}

QIcon toolbarGlyph(const std::function<void(QPainter &)> &paint)
{
    QIcon icon;
    for (int side : {16, 20, 24, 32, 48}) {
        QPixmap pixmap(side, side);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.scale(side / 16.0, side / 16.0);
        paint(painter);
        painter.end();
        icon.addPixmap(pixmap);
    }
    return icon;
}
} // namespace

QIcon helpIcon()
{
    static const QIcon icon = toolbarGlyph([](QPainter &painter) {
        QLinearGradient fill(0, 1, 0, 15);
        fill.setColorAt(0, QColor(QStringLiteral("#B5DEFA")));
        fill.setColorAt(0.45, QColor(QStringLiteral("#4799D9")));
        fill.setColorAt(1, QColor(QStringLiteral("#24529A")));
        painter.setBrush(fill);
        painter.setPen(QPen(QColor(QStringLiteral("#517AA7")), 1));
        painter.drawEllipse(QRectF(1, 1, 14, 14));
        QFont font(QStringLiteral("Sans Serif"));
        font.setPixelSize(12);
        font.setBold(true);
        painter.setFont(font);
        painter.setPen(Qt::white);
        painter.drawText(QRectF(1, 0, 14, 16), Qt::AlignCenter, QStringLiteral("?"));
    });
    return icon;
}

QIcon previewPaneIcon()
{
    static const QIcon icon = toolbarGlyph([](QPainter &painter) {
        painter.setBrush(Qt::white);
        painter.setPen(QPen(QColor(QStringLiteral("#77818D")), 1));
        painter.drawRect(QRectF(1.5, 2.5, 13, 11));
        QLinearGradient pane(0, 5, 0, 13);
        pane.setColorAt(0, QColor(QStringLiteral("#D4EBFF")));
        pane.setColorAt(1, QColor(QStringLiteral("#75B6E4")));
        painter.fillRect(QRectF(9, 5, 5, 8), pane);
        painter.fillRect(QRectF(2, 3, 12, 2), QColor(QStringLiteral("#E2EAF3")));
        painter.drawLine(QPointF(2, 5.5), QPointF(14, 5.5));
        painter.drawLine(QPointF(8.5, 5), QPointF(8.5, 13));
    });
    return icon;
}

QIcon googleDriveIcon()
{
    // Match the supplied Windows 7 silver drive reference independently of
    // the platform theme used by previews, choosers, or another desktop.
    QIcon base;
    for (int side : {16, 24, 32, 48, 256})
        base.addFile(QStringLiteral(":/win7/google-drive-base/drive-%1.png").arg(side),
                     QSize(side, side));
    const QIcon badge(QStringLiteral(":/google/drive-logo.png"));
    QIcon icon;
    for (int side : {16, 20, 24, 32, 48, 64, 96, 128, 256}) {
        QPixmap pixmap(side, side);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        base.paint(&painter, QRect(0, 0, side, side));
        // Match the Windows system-drive badge on the upper-left of the drive,
        // keeping Google's original image unaltered apart from resizing.
        // Match the Microsoft badge's visible size in the 16px sidebar icon,
        // allowing for the Google asset's transparent margins.
        const int extent = side == 16 ? 12 : qMax(7, qRound(side / 3.0));
        // The compact navigation artwork fills the canvas vertically, unlike
        // the larger drive artwork with its transparent space above the disk.
        const int badgeTop = side == 16 ? 0 : qRound(side / 3.0);
        badge.paint(&painter, QRect(0, badgeTop, extent, extent));
        painter.end();
        icon.addPixmap(pixmap);
    }
    return icon;
}

QIcon tryIconName(const QString &name)
{
    const QIcon icon = QIcon::fromTheme(name);
    if (!icon.isNull())
        return icon;

    return QIcon::fromTheme(name + QStringLiteral("-symbolic"));
}

QIcon themeIcon(std::initializer_list<const char *> names)
{
    for (const char *name : names) {
        const QIcon icon = tryIconName(QString::fromLatin1(name));
        if (!icon.isNull())
            return icon;
    }

    for (const QString &name : std::as_const(fallbacks())) {
        const QIcon icon = QIcon::fromTheme(name);
        if (!icon.isNull())
            return icon;
    }

    return QIcon();
}

void setIconFallbacks(const QStringList &names)
{
    fallbacks() = names;
}

} // namespace Aero
