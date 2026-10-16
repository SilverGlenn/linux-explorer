#include "contextmenu.h"
#include "fonts.h"

#include <QMenu>
#include <QPainter>
#include <QProxyStyle>
#include <QStyleOptionMenuItem>
#include <QtMath>

namespace {

// Measured in the Windows 7 VM at 96 DPI: 3px panel inset, 22px rows,
// text at x35, and 55px after the longest label. Width follows the contents.
class ContextMenuStyle final : public QProxyStyle {
public:
    explicit ContextMenuStyle(bool submenu)
        : QProxyStyle(QStringLiteral("Fusion")), m_labelPadding(submenu ? 69 : 84) {}

    static int scaled(int value, const QWidget *widget)
    {
        // Qt already scales logical coordinates for the screen's device ratio.
        // Also accommodate larger text without clipping or crowding the gutter.
        const QFont font = widget ? widget->font() : QFont();
        const qreal factor = qMax(1.0, font.pointSizeF() > 0
            ? font.pointSizeF() / 9.0 : font.pixelSize() / 12.0);
        return qRound(value * factor);
    }

    int pixelMetric(PixelMetric metric, const QStyleOption *option,
                    const QWidget *widget) const override
    {
        switch (metric) {
        case PM_MenuPanelWidth: return scaled(3, widget);
        case PM_MenuHMargin:
        case PM_MenuVMargin:
        case PM_MenuDesktopFrameWidth: return 0;
        case PM_SmallIconSize: return scaled(16, widget);
        case PM_SubMenuOverlap: return scaled(-3, widget);
        default: return QProxyStyle::pixelMetric(metric, option, widget);
        }
    }

    QSize sizeFromContents(ContentsType type, const QStyleOption *option,
                           const QSize &contents, const QWidget *widget) const override
    {
        const auto *item = qstyleoption_cast<const QStyleOptionMenuItem *>(option);
        if (type != CT_MenuItem || !item)
            return QProxyStyle::sizeFromContents(type, option, contents, widget);
        if (item->menuItemType == QStyleOptionMenuItem::Separator)
            return QSize(scaled(84, widget), scaled(8, widget));

        const QString label = item->text.section(QLatin1Char('\t'), 0, 0);
        QFont font = item->font;
        if (item->menuItemType == QStyleOptionMenuItem::DefaultItem) font.setBold(true);
        const int labelWidth = QFontMetrics(font).size(Qt::TextSingleLine | Qt::TextShowMnemonic,
                                                     label).width();
        // QMenu adds the 3px frame on both sides. Keep the shortcut column too
        // for third-party submenus, although the shell commands omit captions.
        return QSize(labelWidth + scaled(m_labelPadding, widget) + item->reservedShortcutWidth,
                     qMax(scaled(22, widget), contents.height() + scaled(5, widget)));
    }

    void drawPrimitive(PrimitiveElement element, const QStyleOption *option,
                       QPainter *painter, const QWidget *widget) const override
    {
        if (element != PE_PanelMenu) {
            QProxyStyle::drawPrimitive(element, option, painter, widget);
            return;
        }
        painter->save();
        painter->fillRect(option->rect, QColor(0xf0f0f0));
        painter->setPen(QColor(0x979797));
        painter->drawRect(option->rect.adjusted(0, 0, -1, -1));
        painter->setPen(QColor(0xf5f5f5));
        painter->drawRect(option->rect.adjusted(1, 1, -2, -2));
        painter->drawRect(option->rect.adjusted(2, 2, -3, -3));
        const QRect gutterRect = visualRect(option->direction, option->rect,
            QRect(option->rect.left() + scaled(3, widget), option->rect.top() + scaled(3, widget),
                  scaled(26, widget), option->rect.height() - scaled(6, widget)));
        painter->fillRect(gutterRect, QColor(0xf1f1f1));
        const int gutter = scaled(29, widget);
        const int x = option->direction == Qt::RightToLeft
            ? option->rect.right() - gutter : option->rect.left() + gutter;
        painter->setPen(QColor(0xe2e3e3));
        painter->drawLine(x, option->rect.top() + 3, x, option->rect.bottom() - 3);
        painter->setPen(Qt::white);
        const int next = option->direction == Qt::RightToLeft ? x - 1 : x + 1;
        painter->drawLine(next, option->rect.top() + 3, next, option->rect.bottom() - 3);
        painter->restore();
    }

    void drawControl(ControlElement element, const QStyleOption *option,
                     QPainter *painter, const QWidget *widget) const override
    {
        const auto *item = qstyleoption_cast<const QStyleOptionMenuItem *>(option);
        if (element != CE_MenuItem || !item) {
            QProxyStyle::drawControl(element, option, painter, widget);
            return;
        }
        painter->save();
        const QRect rect = item->rect;
        const auto visual = [&](const QRect &r) { return visualRect(item->direction, rect, r); };
        const auto px = [&](int n) { return scaled(n, widget); };
        if (item->menuItemType == QStyleOptionMenuItem::Separator) {
            const QRect line = visual(QRect(rect.left() + px(27), rect.top() + px(4),
                                           rect.width() - px(27), 1));
            painter->fillRect(line, QColor(0xe0e0e0));
            painter->fillRect(line.translated(0, 1), Qt::white);
            painter->restore();
            return;
        }
        const bool enabled = item->state.testFlag(State_Enabled);
        if (item->state.testFlag(State_Selected) && enabled) {
            QLinearGradient gradient(rect.topLeft(), rect.bottomLeft());
            gradient.setColorAt(0, QColor(0xeff6fd));
            gradient.setColorAt(1, QColor(0xdcecfb));
            painter->setPen(QColor(0x9ac6ed));
            painter->setBrush(gradient);
            painter->drawRoundedRect(rect.adjusted(0, 0, -1, -1), 2, 2);
        }
        const QColor textColor = enabled ? QColor(Qt::black) : QColor(0x6d6d6d);
        const int iconSize = px(16);
        const QRect iconRect = visual(QRect(rect.left() + px(4),
            rect.center().y() - iconSize / 2 + 1, iconSize, iconSize));
        if (!item->icon.isNull()) {
            item->icon.paint(painter, iconRect, Qt::AlignCenter,
                             enabled ? QIcon::Normal : QIcon::Disabled,
                             item->checked ? QIcon::On : QIcon::Off);
        } else if (item->checked) {
            painter->setPen(QPen(textColor, px(2)));
            if (item->checkType == QStyleOptionMenuItem::Exclusive) {
                painter->setBrush(textColor);
                painter->drawEllipse(iconRect.center(), px(2), px(2));
            } else {
                const QPoint c = iconRect.center();
                QPolygon tick;
                tick << QPoint(c.x() - px(4), c.y()) << QPoint(c.x() - px(1), c.y() + px(3))
                     << QPoint(c.x() + px(5), c.y() - px(4));
                painter->drawPolyline(tick);
            }
        }

        QFont font = item->font;
        if (item->menuItemType == QStyleOptionMenuItem::DefaultItem) font.setBold(true);
        painter->setFont(font);
        painter->setPen(textColor);
        int flags = Qt::AlignVCenter | Qt::TextSingleLine | Qt::TextShowMnemonic;
        if (!styleHint(SH_UnderlineShortcut, option, widget)) flags |= Qt::TextHideMnemonic;
        flags |= item->direction == Qt::RightToLeft ? Qt::AlignRight : Qt::AlignLeft;
        QRect textRect(rect.left() + px(32), rect.top(), rect.width() - px(52), rect.height());
        painter->drawText(visual(textRect), flags, item->text.section(QLatin1Char('\t'), 0, 0));
        if (item->text.contains(QLatin1Char('\t'))) {
            const QRect shortcutRect = visual(QRect(rect.right() - px(20) - item->reservedShortcutWidth,
                rect.top(), item->reservedShortcutWidth, rect.height()));
            painter->drawText(shortcutRect, Qt::AlignVCenter | Qt::AlignRight | Qt::TextSingleLine,
                              item->text.section(QLatin1Char('\t'), 1));
        }
        if (item->menuItemType == QStyleOptionMenuItem::SubMenu) {
            const QRect arrowRect = visual(QRect(rect.right() - px(12),
                                                rect.center().y() - px(3), px(4), px(7)));
            // Explicit pixel columns retain the reference's 4 by 7 arrow;
            // filling a triangle polygon drops its final row and column.
            for (int column = 0; column < arrowRect.width(); ++column) {
                const int inset = qRound(column * (arrowRect.height() - 1.0)
                                         / (2 * (arrowRect.width() - 1)));
                const int x = item->direction == Qt::RightToLeft
                    ? arrowRect.right() - column : arrowRect.left() + column;
                painter->fillRect(QRect(x, arrowRect.top() + inset, 1,
                                        arrowRect.height() - 2 * inset), textColor);
            }
        }
        painter->restore();
    }

private:
    const int m_labelPadding;
};

} // namespace

void Aero::styleContextMenu(QMenu *menu, bool submenu)
{
    // Popup windows do not reliably inherit the parent menu's font.
    menu->setFont(shellFont());
    for (QAction *action : menu->actions()) {
        QFont font = shellFont();
        font.setBold(action == menu->defaultAction());
        action->setFont(font);
    }
    if (!menu->property("shellContextMenuStyled").toBool()) {
        menu->setProperty("shellContextMenuStyled", true);
        auto *style = new ContextMenuStyle(submenu);
        style->setParent(menu);
        menu->setStyle(style);
        QObject::connect(menu, &QMenu::aboutToShow, menu, [menu] {
            // KIO and KNewFileMenu can populate further submenus on demand.
            for (QAction *action : menu->actions()) {
                QFont font = menu->font();
                font.setBold(action == menu->defaultAction());
                action->setFont(font);
            }
            for (QAction *action : menu->actions())
                if (action->menu()) Aero::styleContextMenu(action->menu(), true);
        });
    }
    for (QAction *action : menu->actions())
        if (action->menu()) Aero::styleContextMenu(action->menu(), true);
}
