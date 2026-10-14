#include "ViewModeMenu.h"
#include "aero/icons.h"
#include "aero/text.h"

#include <QActionGroup>
#include <QHBoxLayout>
#include <QSignalBlocker>
#include <QSlider>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidgetAction>

QIcon ViewModeMenu::iconForMode(Settings::ViewMode mode)
{
    switch (mode) {
    case Settings::ViewMode::ExtraLargeIcons:
    case Settings::ViewMode::LargeIcons:
        return Aero::themeIcon({"view-preview", "view-list-icons"});
    case Settings::ViewMode::MediumIcons:
    case Settings::ViewMode::SmallIcons:
        return Aero::themeIcon({"view-list-icons", "view-list-details"});
    case Settings::ViewMode::List:
        return Aero::themeIcon({"view-list-text", "view-list-details"});
    case Settings::ViewMode::Tiles:
        return Aero::themeIcon({"view-list-tiles", "view-list-icons"});
    case Settings::ViewMode::Content:
        return Aero::themeIcon({"view-list-content", "view-list-details"});
    default:
        return Aero::themeIcon({"view-list-details", "view-list-text"});
    }
}

Settings::ViewMode ViewModeMenu::nextMode(Settings::ViewMode mode)
{
    // Verified in the VM: the main button skips the other icon sizes.
    switch (mode) {
    case Settings::ViewMode::List:    return Settings::ViewMode::Details;
    case Settings::ViewMode::Details: return Settings::ViewMode::Tiles;
    case Settings::ViewMode::Tiles:   return Settings::ViewMode::Content;
    case Settings::ViewMode::Content: return Settings::ViewMode::LargeIcons;
    default:                         return Settings::ViewMode::List;
    }
}

ViewModeMenu::ViewModeMenu(QActionGroup *modes, QWidget *parent)
    : QMenu(parent), m_modes(modes)
{
    setObjectName(QStringLiteral("viewModeMenu"));
    auto *panel = new QWidget;
    auto *row = new QHBoxLayout(panel);
    row->setContentsMargins(8, 6, 4, 6);
    row->setSpacing(8);
    m_slider = new QSlider(Qt::Vertical, panel);
    m_slider->setObjectName(QStringLiteral("viewModeSlider"));
    m_slider->setRange(0, int(Settings::ViewMode::Content));
    m_slider->setPageStep(1);
    m_slider->setTickPosition(QSlider::TicksRight);
    m_slider->setTickInterval(1);
    m_slider->setFixedWidth(26);
    m_slider->setAccessibleName(tr("View size and layout"));
    m_slider->setToolTip(tr("Drag to change your view"));
    row->addWidget(m_slider);

    auto *choices = new QVBoxLayout;
    choices->setContentsMargins(0, 0, 0, 0);
    choices->setSpacing(0);
    for (QAction *action : modes->actions()) {
        const auto mode = Settings::ViewMode(action->data().toInt());
        auto *button = new QToolButton(panel);
        button->setObjectName(QStringLiteral("viewMode%1").arg(int(mode)));
        button->setText(action->text().remove(QLatin1Char('&')));
        button->setIcon(iconForMode(mode));
        button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        button->setCheckable(true);
        button->setFocusPolicy(Qt::StrongFocus);
        button->setAutoRaise(true);
        button->setFixedHeight(30);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        button->setStyleSheet(QStringLiteral(
            "QToolButton { text-align: left; padding: 3px 6px; border: 1px solid transparent; }"
            "QToolButton:hover, QToolButton:checked, QToolButton:focus { border: 1px solid #9DBAD9;"
            " border-radius: 2px; background: qlineargradient(x1:0,y1:0,x2:0,y2:1,"
            " stop:0 #F5FAFF, stop:1 #DCEBFC); color: black; }"));
        Aero::setPointSize(button, 9);
        button->setChecked(action->isChecked());
        connect(action, &QAction::changed, this, &ViewModeMenu::sync);
        connect(action, &QAction::changed, button, [action, button] {
            button->setChecked(action->isChecked());
            button->setEnabled(action->isEnabled());
        });
        connect(button, &QToolButton::clicked, this, [this, action] {
            action->trigger();
            close();
        });
        choices->addWidget(button);
    }
    row->addLayout(choices);
    auto *widgetAction = new QWidgetAction(this);
    widgetAction->setDefaultWidget(panel);
    addAction(widgetAction);
    connect(m_slider, &QSlider::valueChanged, this, [this](int value) {
        const int mode = int(Settings::ViewMode::Content) - value;
        for (QAction *action : m_modes->actions()) {
            if (action->data().toInt() == mode && action->isEnabled()) {
                action->trigger();
                break;
            }
        }
    });
    connect(modes, &QActionGroup::triggered, this, [this] { sync(); });
    connect(this, &QMenu::aboutToShow, this, [this] {
        sync();
        m_slider->setFocus();
    });
    sync();
}

void ViewModeMenu::sync()
{
    const QAction *selected = m_modes->checkedAction();
    if (!selected) return;
    QSignalBlocker blocker(m_slider);
    m_slider->setValue(int(Settings::ViewMode::Content) - selected->data().toInt());
}
