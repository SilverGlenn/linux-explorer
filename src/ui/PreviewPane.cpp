#include "PreviewPane.h"
#include "PdfPreviewRenderer.h"
#include "aero/text.h"
#include "aero/fonts.h"
#include "aero/artwork.h"

#include <KIO/PreviewJob>
#include <QFile>
#include <QFutureWatcher>
#include <QHideEvent>
#include <QLabel>
#include <QPlainTextEdit>
#include <QShowEvent>
#include <QStackedWidget>
#include <QStringConverter>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent>

namespace {
constexpr qint64 kTextPreviewLimit = 256 * 1024;
struct TextPreview {
    QString text;
    bool readable = false;
    bool truncated = false;
};

TextPreview readText(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QByteArray bytes = file.read(kTextPreviewLimit + 1);
    const bool truncated = bytes.size() > kTextPreviewLimit;
    bytes.truncate(kTextPreviewLimit);
    const auto encoding = QStringConverter::encodingForData(bytes);
    // Unknown text with embedded NULs is usually binary, not something to dump
    // into a text widget. UTF-16/32 with a BOM is handled by the decoder below.
    if (!encoding && bytes.contains('\0')) return {};
    QStringDecoder decoder(encoding.value_or(QStringConverter::Utf8));
    return {decoder(bytes), true, truncated};
}
} // namespace

PreviewPane::PreviewPane(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("win7PreviewPane"));
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(Aero::panelSheet(objectName(), Aero::Palette::Surface,
                                   Qt::LeftEdge, Aero::Palette::PaneRule));
    setMinimumWidth(160);
    resize(250, height());
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(1, 0, 0, 0);
    root->setSpacing(0);
    m_stack = new QStackedWidget(this);
    root->addWidget(m_stack);
    m_placeholder = Aero::label(QString(), 9, Aero::Palette::HeadingText);
    m_placeholder->setObjectName(QStringLiteral("previewPlaceholder"));
    m_placeholder->setAlignment(Qt::AlignCenter);
    m_placeholder->setWordWrap(true);
    m_stack->addWidget(m_placeholder);
    m_image = new QLabel;
    m_image->setObjectName(QStringLiteral("previewImage"));
    m_image->setAlignment(Qt::AlignCenter);
    m_image->setMinimumSize(0, 0);
    m_image->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    m_stack->addWidget(m_image);
    m_text = new QPlainTextEdit;
    m_text->setObjectName(QStringLiteral("textPreview"));
    m_text->setReadOnly(true);
    m_text->setFrameShape(QFrame::NoFrame);
    m_text->setFont(Aero::textPreviewFont());
    m_text->setStyleSheet(QStringLiteral("QPlainTextEdit { background: white; color: black; }"));
    m_stack->addWidget(m_text);
    m_pdfCaption = Aero::label(tr("PDF — first page only"), 9, Aero::Palette::HeadingText);
    m_pdfCaption->setObjectName(QStringLiteral("pdfPreviewCaption"));
    m_pdfCaption->setAlignment(Qt::AlignCenter);
    m_pdfCaption->setWordWrap(true);
    root->addWidget(m_pdfCaption);
    m_pdf = new PdfPreviewRenderer;
    connect(m_pdf, &PdfPreviewRenderer::ready, this, [this](quint64 generation, const QImage &image) {
        if (generation != m_generation) return;
        m_pixmap = QPixmap::fromImage(image);
        m_pdfCaption->show();
        m_stack->setCurrentWidget(m_image);
        updateImage();
    });
    connect(m_pdf, &PdfPreviewRenderer::failed, this,
            [this](quint64 generation, PdfPreviewRenderer::Failure failure) {
        if (generation != m_generation) return;
        switch (failure) {
        case PdfPreviewRenderer::Failure::Unavailable:
            showPlaceholder(tr("PDF renderer is not available."));
            break;
        case PdfPreviewRenderer::Failure::TimedOut:
            showPlaceholder(tr("PDF preview timed out."));
            break;
        case PdfPreviewRenderer::Failure::OutputLimit:
            showPlaceholder(tr("PDF preview exceeded its safety limits."));
            break;
        case PdfPreviewRenderer::Failure::Invalid:
            showPlaceholder(tr("Cannot preview this PDF. It may be invalid, encrypted, or unsupported."));
            break;
        }
    });
    showPlaceholder(tr("Select a file to preview."));
}

PreviewPane::~PreviewPane()
{
    cancelPreview();
    disconnect(m_pdf, nullptr, this, nullptr);
    m_pdf->shutdown();
}

void PreviewPane::cancelPreview()
{
    ++m_generation;
    if (m_job) m_job->kill();
    m_job = nullptr;
    if (m_pdf) m_pdf->cancel();
}

void PreviewPane::showPlaceholder(const QString &text)
{
    m_pdfCaption->hide();
    m_placeholder->setText(text);
    m_stack->setCurrentWidget(m_placeholder);
}

void PreviewPane::setItems(const QList<KFileItem> &items)
{
    m_items = items;
    m_resumeOnShow = false;
    cancelPreview();
    m_pixmap = {};
    m_image->clear();
    m_text->clear();
    if (items.size() != 1) {
        showPlaceholder(items.isEmpty() ? tr("Select a file to preview.")
                                       : tr("%1 items selected.").arg(items.size()));
        return;
    }
    const KFileItem item = items.first();
    if (item.isDir()) {
        showPlaceholder(tr("No preview available."));
        return;
    }
    const quint64 generation = m_generation;
    showPlaceholder(tr("Loading preview..."));
    const QString mime = item.mimetype();
    if (mime == QLatin1String("application/pdf")) {
        if (!item.url().isLocalFile()) {
            showPlaceholder(tr("PDF preview is available for local files only."));
            return;
        }
        showPlaceholder(tr("Loading PDF first page..."));
        m_pdf->render(item.url().toLocalFile(), generation);
        return;
    }
    if (item.url().isLocalFile()
        && (mime.startsWith(QLatin1String("text/")) || mime == QLatin1String("application/json"))) {
        // Even a local path can be on a slow mount. Bound the read and keep it
        // off the GUI thread; a generation token discards obsolete results.
        auto *watcher = new QFutureWatcher<TextPreview>(this);
        connect(watcher, &QFutureWatcher<TextPreview>::finished, this, [this, watcher, generation] {
            const TextPreview preview = watcher->result();
            watcher->deleteLater();
            if (generation != m_generation) return;
            if (!preview.readable) {
                showPlaceholder(tr("No preview available."));
                return;
            }
            m_text->setPlainText(preview.text);
            m_text->setToolTip(preview.truncated ? tr("Preview limited to the first 256 KiB.") : QString());
            m_stack->setCurrentWidget(m_text);
        });
        watcher->setFuture(QtConcurrent::run(readText, item.url().toLocalFile()));
        return;
    }

    auto *job = new KIO::PreviewJob(KFileItemList({item}), QSize(512, 512));
    m_job = job;
    job->setIgnoreMaximumSize(false);
    job->setScaleType(KIO::PreviewJob::ScaledAndCached);
    connect(job, &KIO::PreviewJob::gotPreview, this,
            [this, generation](const KFileItem &, const QPixmap &pixmap) {
        if (generation != m_generation) return;
        m_pixmap = pixmap;
        updateImage();
        m_stack->setCurrentWidget(m_image);
    });
    connect(job, &KJob::result, this, [this, generation] {
        if (generation == m_generation && m_pixmap.isNull())
            showPlaceholder(tr("No preview available."));
    });
    job->start();
}

void PreviewPane::updateImage()
{
    if (m_pixmap.isNull()) return;
    const QSize room = m_stack->size() - QSize(16, 16);
    if (room.width() > 0 && room.height() > 0)
        m_image->setPixmap(m_pixmap.scaled(room.boundedTo(m_pixmap.size()),
                                         Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

void PreviewPane::hideEvent(QHideEvent *event)
{
    m_resumeOnShow = true;
    cancelPreview();
    m_pixmap = {};
    m_image->clear();
    showPlaceholder(tr("Select a file to preview."));
    QWidget::hideEvent(event);
}

void PreviewPane::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    if (!m_resumeOnShow) return;
    const quint64 generation = m_generation;
    // MainWindow may synchronously set a new selection after showing the pane.
    // Resume only if that did not supersede the preview canceled on hide.
    QTimer::singleShot(0, this, [this, generation] {
        if (isVisible() && m_resumeOnShow && generation == m_generation)
            setItems(m_items);
    });
}

void PreviewPane::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    updateImage();
}
