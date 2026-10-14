#include "BreadcrumbBar.h"
#include "aero/text.h"

#include <utility>
#include <QLabel>
#include <QPainter>
#include <QResizeEvent>
#include <QStyle>
#include <QStyleOptionFocusRect>
#include <QToolButton>

namespace {
constexpr int kLeadingWidth = 16;
constexpr int kArrowWidth = 12;
constexpr int kIconWidth = 18;
constexpr int kTextPadding = 8;

// Keep two real buttons for independent activation and keyboard access, but
// paint their chrome once, as a split breadcrumb rather than adjacent buttons.
class CrumbButton : public QToolButton {
public:
    explicit CrumbButton(QWidget *parent) : QToolButton(parent)
    {
        Aero::setPointSize(this, 9);
        setAutoRaise(true);
        setAttribute(Qt::WA_Hover);
        connect(this, &QToolButton::pressed, parent, qOverload<>(&QWidget::update));
        connect(this, &QToolButton::released, parent, qOverload<>(&QWidget::update));
    }

protected:
    bool event(QEvent *event) override
    {
        const bool result = QToolButton::event(event);
        switch (event->type()) {
        case QEvent::Enter:
        case QEvent::Leave:
        case QEvent::FocusIn:
        case QEvent::FocusOut:
        case QEvent::EnabledChange:
            parentWidget()->update();
            break;
        default: break;
        }
        return result;
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        const QColor color = palette().color(isEnabled() ? QPalette::Active : QPalette::Disabled,
                                              QPalette::ButtonText);
        painter.setPen(color);
        if (property("arrow").toBool()) {
            const bool down = property("popupOpen").toBool();
            const QPoint c = rect().center();
            const QPolygon triangle = down
                ? QPolygon{c + QPoint(-3, -1), c + QPoint(3, -1), c + QPoint(0, 2)}
                : QPolygon{c + QPoint(-1, -3), c + QPoint(-1, 3), c + QPoint(2, 0)};
            painter.setBrush(color);
            painter.setPen(Qt::NoPen);
            painter.drawPolygon(triangle);
        } else {
            painter.drawText(rect().adjusted(kTextPadding / 2, 0, -kTextPadding / 2, 0),
                             Qt::AlignVCenter | Qt::AlignLeft | Qt::TextSingleLine, text());
        }
        if (hasFocus() && isEnabled()) {
            QStyleOptionFocusRect focus;
            focus.initFrom(this);
            focus.rect = rect().adjusted(2, 2, -2, -2);
            focus.backgroundColor = palette().color(QPalette::Base);
            style()->drawPrimitive(QStyle::PE_FrameFocusRect, &focus, &painter, this);
        }
    }
};

class CrumbSegment : public QWidget {
public:
    explicit CrumbSegment(QWidget *parent) : QWidget(parent)
    {
        name = new CrumbButton(this);
        arrow = new CrumbButton(this);
        arrow->setProperty("arrow", true);
    }
    CrumbButton *name;
    CrumbButton *arrow;

protected:
    void paintEvent(QPaintEvent *) override
    {
        const bool popup = arrow->property("popupOpen").toBool();
        const bool active = isEnabled() && (name->underMouse() || arrow->underMouse()
            || name->hasFocus() || arrow->hasFocus() || name->isDown() || arrow->isDown() || popup);
        if (!active) return;
        QPainter painter(this);
        const QRect frame = rect().adjusted(0, 0, -1, -1);
        const bool pressed = name->isDown() || arrow->isDown() || popup;
        QLinearGradient gradient(0, 0, 0, height());
        gradient.setColorAt(0, QColor(pressed ? "#DCEBFA" : "#F5FAFE"));
        gradient.setColorAt(1, QColor(pressed ? "#C1DCF5" : "#DDEDFC"));
        painter.fillRect(frame, gradient);
        // The hot half is stronger, but the outer border encloses both halves.
        if (arrow->underMouse() || arrow->isDown() || popup)
            painter.fillRect(arrow->geometry().adjusted(0, 1, -1, -1),
                             QColor(arrow->isDown() || popup ? "#B8D6F1" : "#C9E2F8"));
        else if (name->underMouse() || name->isDown())
            painter.fillRect(name->geometry().adjusted(1, 1, -1, -1),
                             QColor(name->isDown() ? "#C1DCF5" : "#DFEFFC"));
        painter.setPen(QColor(pressed ? "#7DA2CE" : "#A9C9E8"));
        painter.drawRect(frame);
        painter.drawLine(arrow->x(), 1, arrow->x(), height() - 2);
    }
};
} // namespace

BreadcrumbBar::BreadcrumbBar(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("breadcrumbBar"));
    setMinimumWidth(0);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_icon = new QLabel(this);
    m_icon->setFixedSize(16, 16);
    // The leading dropdown uses the same shared chrome, with an empty name half.
    auto *leading = new CrumbSegment(this);
    leading->setObjectName(QStringLiteral("breadcrumbLeading"));
    leading->name->hide();
    m_leading = leading->arrow;
    m_leading->setObjectName(QStringLiteral("breadcrumbLocations"));
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
    setPopupOpen(QUrl(), false);
    m_icon->setPixmap(icon.pixmap(16, 16));
    for (const Segment &segment : segments) {
        auto *group = new CrumbSegment(this);
        auto *name = group->name;
        name->setObjectName(QStringLiteral("crumbName"));
        name->setText(segment.label);
        name->setToolTip(segment.label);
        name->setAccessibleName(segment.label);
        connect(name, &QToolButton::clicked, this,
                [this, url = segment.url] { Q_EMIT urlActivated(url); });
        auto *arrow = group->arrow;
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

void BreadcrumbBar::setPopupOpen(const QUrl &url, bool open)
{
    const auto apply = [](QToolButton *arrow, bool on) {
        arrow->setProperty("popupOpen", on);
        arrow->update();
        arrow->parentWidget()->update();
    };
    // Only one breadcrumb popup may be open. An invalid URL names the leading
    // locations/overflow menu, matching menuRequested's existing convention.
    if (open || !url.isValid()) apply(m_leading, open && !url.isValid());
    for (const Part &part : std::as_const(m_parts))
        if (open || part.segment.url == url) apply(part.arrow, open && part.segment.url == url);
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
    m_leading->parentWidget()->setGeometry(kIconWidth, 0, kLeadingWidth, height());
    m_leading->setGeometry(0, 0, kLeadingWidth, height());
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
    m_leading->setProperty("arrow", m_hidden == 0);
    m_leading->setText(m_hidden ? QStringLiteral("\u00ab") : QString());
    m_leading->setProperty("overflow", m_hidden > 0);
    m_leading->update();
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
