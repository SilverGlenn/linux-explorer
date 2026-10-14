#pragma once

#include <QFontMetrics>
#include <QPainter>
#include <QStyledItemDelegate>

// Keeps the navigation selection/hover treatment around the row's content,
// rather than extending it across the unused width of the navigation pane.
class NavigationItemDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    static QRect contentRect(const QStyleOptionViewItem &option,
                             const QModelIndex &index)
    {
        int contentWidth = QFontMetrics(option.font).horizontalAdvance(option.text);
        if (option.features.testFlag(QStyleOptionViewItem::HasDecoration))
            contentWidth += option.decorationSize.width() + 4;
        // A few pixels after the label keep the selection from touching glyphs.
        contentWidth += 6;
        const int right = qMin(option.rect.right(), option.rect.left() + contentWidth - 1);
        return QRect(option.rect.left(), option.rect.top(),
                     qMax(1, right - option.rect.left() + 1), option.rect.height());
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        QStyleOptionViewItem contentOption(option);
        contentOption.rect = contentRect(option, index);
        QStyledItemDelegate::paint(painter, contentOption, index);
    }
};
