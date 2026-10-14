#pragma once

#include <QDialog>

// A Linux quick-help fallback, not a simulation of Windows Help and Support.
class HelpDialog : public QDialog {
    Q_OBJECT
public:
    explicit HelpDialog(QWidget *parent = nullptr);
};
