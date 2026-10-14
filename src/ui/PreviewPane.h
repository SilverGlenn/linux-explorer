#pragma once

#include <QList>
#include <QPixmap>
#include <QPointer>
#include <QWidget>
#include <KFileItem>

class QLabel;
class QPlainTextEdit;
class QStackedWidget;
namespace KIO { class PreviewJob; }

// A preview is content, not another metadata card. Plain text is rendered
// read-only; other formats use KIO's installed thumbnail providers.
class PreviewPane : public QWidget {
    Q_OBJECT
public:
    explicit PreviewPane(QWidget *parent = nullptr);
    ~PreviewPane() override;
    void setItems(const QList<KFileItem> &items);

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    void showPlaceholder(const QString &text);
    void cancelPreview();
    void updateImage();
    QStackedWidget *m_stack = nullptr;
    QLabel *m_placeholder = nullptr;
    QLabel *m_image = nullptr;
    QPlainTextEdit *m_text = nullptr;
    QPixmap m_pixmap;
    quint64 m_generation = 0;
    QPointer<KIO::PreviewJob> m_job;
};
