#pragma once

#include <QPainter>
#include <QStyledItemDelegate>

// Keeps tree selection and hover backgrounds full-row, clipped to the
// navigation viewport rather than extending past its pane boundary.
class NavigationItemDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    static QRect boundedRowRect(const QStyleOptionViewItem &option)
    {
        return option.widget ? option.rect.intersected(option.widget->rect())
                             : option.rect;
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        QStyleOptionViewItem rowOption(option);
        rowOption.rect = boundedRowRect(option);
        QStyledItemDelegate::paint(painter, rowOption, index);
    }
};
