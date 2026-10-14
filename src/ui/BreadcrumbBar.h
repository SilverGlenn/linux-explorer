#pragma once

#include <QIcon>
#include <QList>
#include <QUrl>
#include <QWidget>

class QLabel;
class QToolButton;

// Every segment, including the current folder, has a child-folder dropdown.
// When space runs out, hide ancestors from the left rather than widening the
// window, leaving the current location readable and all ancestors reachable.
class BreadcrumbBar : public QWidget {
    Q_OBJECT
public:
    struct Segment { QString label; QUrl url; };
    explicit BreadcrumbBar(QWidget *parent = nullptr);
    void setTrail(const QIcon &icon, const QList<Segment> &segments);
    // Call when the actual menu opens/closes. An invalid URL is the leading
    // locations/hidden-ancestors dropdown, as in menuRequested.
    void setPopupOpen(const QUrl &url, bool open);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
    int hiddenAncestorCount() const { return m_hidden; }

Q_SIGNALS:
    void urlActivated(const QUrl &url);
    void menuRequested(const QUrl &parent, const QPoint &globalPos);

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    struct Part {
        Segment segment;
        QWidget *group;
        QToolButton *name;
        QToolButton *arrow;
    };
    void layoutParts();
    QLabel *m_icon;
    QToolButton *m_leading;
    QList<Part> m_parts;
    int m_hidden = 0;
};
