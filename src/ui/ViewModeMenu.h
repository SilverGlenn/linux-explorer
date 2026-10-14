#pragma once

#include "Settings.h"
#include <QIcon>
#include <QMenu>

class QActionGroup;
class QSlider;

// Win7's view flyout has a vertical slider, not eight radio-menu entries.
// The actions are shared with View, keyboard shortcuts and context menus.
class ViewModeMenu : public QMenu {
    Q_OBJECT
public:
    explicit ViewModeMenu(QActionGroup *modes, QWidget *parent = nullptr);
    static QIcon iconForMode(Settings::ViewMode mode);
    static Settings::ViewMode nextMode(Settings::ViewMode mode);

private:
    void sync();
    QActionGroup *m_modes;
    QSlider *m_slider;
};
