#pragma once

#include <QPainter>
#include <QStyledItemDelegate>

// Keeps row backgrounds clipped to a small gutter before the navigation
// divider; NavigationTree also clips style painting outside the delegate.
class NavigationItemDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    static constexpr int rightGutter = 3;

    static QRect boundedRowRect(const QStyleOptionViewItem &option)
    {
        if (!option.widget)
            return option.rect;
        QRect bounds = option.widget->rect();
        bounds.adjust(0, 0, -rightGutter, 0);
        return option.rect.intersected(bounds);
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        QStyleOptionViewItem rowOption(option);
        rowOption.rect = boundedRowRect(option);
        QStyledItemDelegate::paint(painter, rowOption, index);
    }
};
