#include "BreadcrumbBar.h"
#include "aero/arrows.h"
#include "aero/palette.h"
#include "aero/text.h"

#include <utility>
#include <QLabel>
#include <QResizeEvent>
#include <QToolButton>

namespace {
constexpr int kLeadingWidth = 16;
constexpr int kArrowWidth = 12;
constexpr int kIconWidth = 18;
constexpr int kTextPadding = 8;

QToolButton *makeButton(QWidget *parent)
{
    auto *button = new QToolButton(parent);
    Aero::setPointSize(button, 9);
    button->setAutoRaise(true);
    button->setStyleSheet(QStringLiteral(
        "QToolButton { border: 1px solid transparent; padding: 0; background: transparent; color: black; }"
        "QToolButton:hover, QToolButton:focus { border-color: #A9C9E8; background: #E8F3FC; }"
        "QToolButton:pressed { border-color: #7DA2CE; background: #D5E6F7; }"));
    return button;
}

QIcon arrowIcon()
{
    return QIcon(Aero::arrowPixmap(Qt::RightArrow,
                                  Aero::Palette::rgb(Aero::Palette::CrumbArrow), 6));
}
} // namespace

BreadcrumbBar::BreadcrumbBar(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("breadcrumbBar"));
    setMinimumWidth(0);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_icon = new QLabel(this);
    m_icon->setFixedSize(16, 16);
    m_leading = makeButton(this);
    m_leading->setObjectName(QStringLiteral("breadcrumbLocations"));
    m_leading->setIcon(arrowIcon());
    m_leading->setAccessibleName(tr("Locations and hidden ancestors"));
    m_leading->setToolTip(tr("Locations and hidden ancestors"));
    connect(m_leading, &QToolButton::clicked, this, [this] {
        Q_EMIT menuRequested(QUrl(), m_leading->mapToGlobal(QPoint(0, height())));
    });
}

void BreadcrumbBar::setTrail(const QIcon &icon, const QList<Segment> &segments)
{
    for (const Part &part : std::as_const(m_parts)) delete part.group;
    m_parts.clear();
    m_icon->setPixmap(icon.pixmap(16, 16));
    for (const Segment &segment : segments) {
        auto *group = new QWidget(this);
        auto *name = makeButton(group);
        name->setText(segment.label);
        name->setToolTip(segment.label);
        name->setAccessibleName(segment.label);
        connect(name, &QToolButton::clicked, this,
                [this, url = segment.url] { Q_EMIT urlActivated(url); });
        auto *arrow = makeButton(group);
        arrow->setIcon(arrowIcon());
        arrow->setObjectName(QStringLiteral("crumbDropdown"));
        arrow->setAccessibleName(tr("Folders in %1").arg(segment.label));
        arrow->setToolTip(tr("Folders in %1").arg(segment.label));
        connect(arrow, &QToolButton::clicked, this, [this, arrow, url = segment.url] {
            Q_EMIT menuRequested(url, arrow->mapToGlobal(QPoint(0, arrow->height())));
        });
        m_parts.append({segment, group, name, arrow});
        group->show();
    }
    layoutParts();
    updateGeometry();
}

QSize BreadcrumbBar::sizeHint() const
{
    int width = kIconWidth + kLeadingWidth;
    for (const Part &part : m_parts)
        width += part.name->fontMetrics().horizontalAdvance(part.segment.label)
                 + kTextPadding + kArrowWidth;
    return QSize(qMin(width, 400), 20);
}
QSize BreadcrumbBar::minimumSizeHint() const { return QSize(40, 20); }

void BreadcrumbBar::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    layoutParts();
}

void BreadcrumbBar::layoutParts()
{
    m_icon->move(0, (height() - 16) / 2);
    m_leading->setGeometry(kIconWidth, 0, kLeadingWidth, height());
    QList<int> widths;
    int total = 0;
    for (const Part &part : std::as_const(m_parts)) {
        const int desired = part.name->fontMetrics().horizontalAdvance(part.segment.label)
                            + kTextPadding + kArrowWidth;
        widths.append(desired);
        total += desired;
    }
    const int available = qMax(0, width() - kIconWidth - kLeadingWidth);
    m_hidden = 0;
    while (total > available && m_hidden + 1 < m_parts.size())
        total -= widths.at(m_hidden++);
    m_leading->setIcon(m_hidden ? QIcon() : arrowIcon());
    m_leading->setText(m_hidden ? QStringLiteral("\u00ab") : QString());
    m_leading->setProperty("overflow", m_hidden > 0);
    int x = kIconWidth + kLeadingWidth;
    for (int i = 0; i < m_parts.size(); ++i) {
        const Part &part = m_parts.at(i);
        part.group->setVisible(i >= m_hidden);
        if (i < m_hidden) continue;
        const int cell = qMin(widths.at(i), qMax(0, width() - x));
        const int arrowWidth = qMin(kArrowWidth, cell);
        const int textWidth = cell - arrowWidth;
        part.group->setGeometry(x, 0, cell, height());
        part.name->setGeometry(0, 0, textWidth, height());
        part.name->setText(part.name->fontMetrics().elidedText(
            part.segment.label, Qt::ElideRight, qMax(0, textWidth - kTextPadding)));
        part.arrow->setGeometry(textWidth, 0, arrowWidth, height());
        x += cell;
    }
}
