#include "PdfPreviewRenderer.h"

#include <QBuffer>
#include <QFileInfo>
#include <QImageReader>
#include <QtConcurrent>
#include <algorithm>
#include <utility>
#include <sys/resource.h>
#include <unistd.h>

namespace {
QImage decodePage(const QByteArray &bytes)
{
    QBuffer buffer;
    buffer.setData(bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer, "ppm");
    const QSize size = reader.size();
    // Check before allocating/decoding, including output from a broken provider.
    if (size.isEmpty() || size.width() > PdfPreviewRenderer::MaximumDimension
        || size.height() > PdfPreviewRenderer::MaximumDimension)
        return {};
    return reader.read();
}
} // namespace

PdfPreviewRenderer::PdfPreviewRenderer(const QString &program, int timeoutMs)
    : m_program(program), m_timeoutMs(std::clamp(timeoutMs, 1, TimeoutMs))
{
    m_process.setProcessChannelMode(QProcess::SeparateChannels);
    // Poppler parses untrusted documents out of process. Pixel/output limits do
    // not bound parser allocations, so cap address space and CPU in the child.
    m_process.setChildProcessModifier([] {
        const rlimit memory{512 * 1024 * 1024, 512 * 1024 * 1024};
        const rlimit cpu{5, 5};
        const rlimit core{0, 0};
        if (setrlimit(RLIMIT_AS, &memory) || setrlimit(RLIMIT_CPU, &cpu)
            || setrlimit(RLIMIT_CORE, &core))
            _exit(126);
    });
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, [this] { abort(Failure::TimedOut); });
    connect(&m_process, &QProcess::readyReadStandardOutput, this, &PdfPreviewRenderer::collectOutput);
    connect(&m_process, &QProcess::readyReadStandardError, this, &PdfPreviewRenderer::collectErrors);
    connect(&m_process, &QProcess::finished, this, &PdfPreviewRenderer::processFinished);
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) return;
        m_timer.stop();
        if (!m_cancelled) emit failed(m_generation, Failure::Unavailable);
        settled();
    });
    connect(&m_decoder, &QFutureWatcher<QImage>::finished, this, [this] {
        const QImage image = m_decoder.result();
        m_decoding = false;
        if (!m_cancelled) {
            if (image.isNull()) emit failed(m_generation, Failure::Invalid);
            else emit ready(m_generation, image);
        }
        settled();
    });
}

void PdfPreviewRenderer::render(const QString &localPath, quint64 generation)
{
    cancel();
    if (m_stopping) return;
    // Absolute paths cannot be mistaken for options, even for "-name.pdf".
    m_pendingPath = QFileInfo(localPath).absoluteFilePath();
    m_pendingGeneration = generation;
    startPending();
}

void PdfPreviewRenderer::cancel()
{
    m_pendingPath.clear();
    m_cancelled = true;
    m_timer.stop();
    if (m_process.state() != QProcess::NotRunning) m_process.kill();
}

void PdfPreviewRenderer::shutdown()
{
    m_stopping = true;
    cancel();
    settled();
}

void PdfPreviewRenderer::startPending()
{
    if (m_stopping || m_pendingPath.isEmpty() || m_decoding
        || m_process.state() != QProcess::NotRunning)
        return;
    const QString path = m_pendingPath;
    m_pendingPath.clear();
    m_generation = m_pendingGeneration;
    m_cancelled = false;
    m_aborted = false;
    m_output.clear();
    m_errorBytes = 0;
    m_process.setProgram(m_program);
    m_process.setArguments({QStringLiteral("-f"), QStringLiteral("1"),
                            QStringLiteral("-l"), QStringLiteral("1"),
                            QStringLiteral("-singlefile"), QStringLiteral("-scale-to"),
                            QString::number(MaximumDimension), path});
    m_process.start(QIODevice::ReadOnly);
    m_process.closeWriteChannel();
    m_timer.start(m_timeoutMs);
}

void PdfPreviewRenderer::collectOutput()
{
    if (m_cancelled || m_aborted) return;
    const qint64 remaining = MaximumOutputBytes - m_output.size();
    const QByteArray bytes = m_process.read(remaining + 1);
    if (bytes.size() > remaining) {
        abort(Failure::OutputLimit);
        return;
    }
    m_output.append(bytes);
}

void PdfPreviewRenderer::collectErrors()
{
    if (m_cancelled || m_aborted) return;
    m_process.setReadChannel(QProcess::StandardError);
    m_errorBytes += m_process.read(MaximumErrorBytes - m_errorBytes + 1).size();
    m_process.setReadChannel(QProcess::StandardOutput);
    if (m_errorBytes > MaximumErrorBytes) abort(Failure::OutputLimit);
}

void PdfPreviewRenderer::abort(Failure failure)
{
    if (m_cancelled || m_aborted) return;
    m_aborted = true;
    m_failure = failure;
    m_process.kill();
}

void PdfPreviewRenderer::processFinished(int exitCode, QProcess::ExitStatus status)
{
    m_timer.stop();
    collectOutput();
    collectErrors();
    if (m_cancelled) {
        settled();
        return;
    }
    if (m_aborted || status != QProcess::NormalExit || exitCode != 0) {
        emit failed(m_generation, m_aborted ? m_failure : Failure::Invalid);
        settled();
        return;
    }
    m_decoding = true;
    m_decoder.setFuture(QtConcurrent::run(decodePage, std::exchange(m_output, {})));
}

void PdfPreviewRenderer::settled()
{
    if (m_decoding || m_process.state() != QProcess::NotRunning) return;
    m_output.clear();
    if (m_stopping) deleteLater();
    else startPending();
}
