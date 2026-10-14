#pragma once

#include <QLineEdit>

// Native QLineEdit editing, selection, placeholder colour and clear behaviour,
// with Win7's 9 pt text retained across focus/stylesheet transitions.
class SearchBox : public QLineEdit {
    Q_OBJECT
public:
    explicit SearchBox(QWidget *parent = nullptr);

protected:
    void paintEvent(QPaintEvent *event) override;
};
