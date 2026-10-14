#include "listview.h"

#include "palette.h"
#include "text.h"

#include <QAbstractItemView>
#include <QFontMetrics>
#include <QFrame>
#include <QHeaderView>
#include <QPainter>
#include <QPen>
#include <QTreeView>

namespace {

// QHeaderView does not consistently provide per-section hover/pressed style
// state across Qt styles. Supply those states so its normal native section,
// sort indicator, and resize-handle rendering remain otherwise untouched.
class StatefulListHeader final : public QHeaderView {
public:
    explicit StatefulListHeader(QWidget *parent)
        : QHeaderView(Qt::Horizontal, parent)
    {
        setMouseTracking(true);
        connect(this, &QHeaderView::sectionEntered, this, [this](int section) {
            if (m_hovered != section) {
                const int previous = m_hovered;
                m_hovered = section;
                if (previous >= 0) updateSection(previous);
                updateSection(m_hovered);
            }
        });
        connect(this, &QHeaderView::sectionPressed, this, [this](int section) {
            if (m_pressed >= 0) updateSection(m_pressed);
            m_pressed = section;
            updateSection(section);
        });
        connect(this, &QHeaderView::sectionClicked, this, [this](int section) {
            if (m_pressed == section) {
                m_pressed = -1;
                updateSection(section);
            }
        });
    }

protected:
    void leaveEvent(QEvent *event) override
    {
        const int previous = m_hovered;
        m_hovered = -1;
        if (previous >= 0) updateSection(previous);
        QHeaderView::leaveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        QHeaderView::mouseReleaseEvent(event);
        if (m_pressed >= 0) {
            const int previous = m_pressed;
            m_pressed = -1;
            updateSection(previous);
        }
    }

    void initStyleOptionForIndex(QStyleOptionHeader *option,
                                 int logicalIndex) const override
    {
        QHeaderView::initStyleOptionForIndex(option, logicalIndex);
        if (logicalIndex == m_hovered)
            option->state |= QStyle::State_MouseOver;
        if (logicalIndex == m_pressed)
            option->state |= QStyle::State_Sunken;
    }

    void paintSection(QPainter *painter, const QRect &rect,
                      int logicalIndex) const override
    {
        QHeaderView::paintSection(painter, rect, logicalIndex);
        if (logicalIndex != m_hovered && logicalIndex != m_pressed)
            return;

        const bool pressed = logicalIndex == m_pressed;
        QColor tint(QLatin1String(pressed ? Aero::Palette::GlassPressed
                                          : Aero::Palette::GlassHover));
        QColor edge(QLatin1String(pressed ? Aero::Palette::FocusBorder
                                          : Aero::Palette::GlassBorder));
        tint.setAlpha(pressed ? 96 : 64);
        edge.setAlpha(pressed ? 150 : 105);
        painter->save();
        painter->setPen(QPen(edge, 1));
        painter->setBrush(tint);
        painter->drawRect(rect.adjusted(0, 0, -1, -1));
        painter->restore();
    }

private:
    int m_hovered = -1;
    int m_pressed = -1;
};

} // namespace

namespace Aero {

void configureListTree(QTreeView *tree)
{
    tree->setHeader(new StatefulListHeader(tree));
    tree->setRootIsDecorated(false);
    tree->setItemsExpandable(false);
    tree->setIndentation(0);
    tree->setUniformRowHeights(true);
    tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    tree->setSelectionBehavior(QAbstractItemView::SelectRows);
    // Qt's default would turn a double click into an inline rename instead of
    // opening the item, and renaming is left to the application on F2
    tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tree->setAlternatingRowColors(false);
    tree->setFrameShape(QFrame::NoFrame);
    // Before sorting is enabled, which immediately re sorts by the indicator,
    // and the indicator starts out descending
    tree->header()->setSortIndicator(0, Qt::AscendingOrder);
    tree->setSortingEnabled(true);
    tree->header()->setDefaultAlignment(Qt::AlignLeft);
    tree->header()->setSectionsClickable(true);
    tree->header()->setMouseTracking(true);
    tree->header()->setAttribute(Qt::WA_Hover, true);
    tree->header()->setStretchLastSection(true);
    setPointSize(tree->header(), 9);
    // The shared header supplies Win7 hover/press feedback but leaves the
    // normal/sorted section and resize handle to the active style.
    // Pinned rather than left to the size hint, which is cached before the app
    // wide stylesheet polishes the widget and comes out twice as tall
    const QFontMetrics metrics(tree->header()->font());
    const int padding = 4;
    tree->header()->setFixedHeight(metrics.height() + 2 * padding + 1);
}

} // namespace Aero
