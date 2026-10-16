#pragma once

#include "palette.h"

#include <QPainter>
#include <QSplitter>

namespace Aero {

// A one-logical-pixel pane rule, with QSplitter's expanded hit area when the
// handle width is one. Keep its native input handling, but skip style painting
// so themes cannot replace this divider with a raised grip.
class PaneSplitter : public QSplitter {
public:
    explicit PaneSplitter(Qt::Orientation orientation = Qt::Horizontal, QWidget *parent = nullptr)
        : QSplitter(orientation, parent)
    {
        setHandleWidth(1);
    }

protected:
    QSplitterHandle *createHandle() override
    {
        return new Handle(orientation(), this);
    }

private:
    class Handle : public QSplitterHandle {
    public:
        Handle(Qt::Orientation orientation, QSplitter *parent)
            : QSplitterHandle(orientation, parent)
        {
            setAutoFillBackground(false);
            setAttribute(Qt::WA_NoSystemBackground);
        }

    protected:
        void paintEvent(QPaintEvent *) override
        {
            QPainter painter(this);
            // QSplitterHandle reserves overlapping mouse margins in its
            // contentsRect; only the central layout gap should be painted.
            painter.fillRect(contentsRect(), Palette::rgb(Palette::PaneRule));
        }
    };
};

} // namespace Aero
