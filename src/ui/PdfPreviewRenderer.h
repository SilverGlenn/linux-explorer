#pragma once

#include <QImage>
#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QFutureWatcher>

// Local first-page raster only. No files are created and no network URLs are
// accepted. shutdown() reaps the process asynchronously before deleting this
// object; the pane must not parent/delete it while a renderer is running.
class PdfPreviewRenderer : public QObject {
    Q_OBJECT
public:
    enum class Failure { Unavailable, Invalid, TimedOut, OutputLimit };
    Q_ENUM(Failure)
    static constexpr int MaximumDimension = 1024;
    static constexpr qint64 MaximumOutputBytes = MaximumDimension * MaximumDimension * 3 + 1024;
    static constexpr qint64 MaximumErrorBytes = 16 * 1024;
    static constexpr int TimeoutMs = 5000;

    // Program/deadline injection is limited to component tests. Production uses
    // the installed, absolute provider path and never a shell or PATH lookup.
    explicit PdfPreviewRenderer(const QString &program = QStringLiteral("/usr/bin/pdftoppm"),
                                int timeoutMs = TimeoutMs);
    void render(const QString &localPath, quint64 generation);
    void cancel();
    void shutdown();

signals:
    void ready(quint64 generation, const QImage &image);
    void failed(quint64 generation, PdfPreviewRenderer::Failure failure);

private:
    void startPending();
    void collectOutput();
    void collectErrors();
    void abort(Failure failure);
    void processFinished(int exitCode, QProcess::ExitStatus status);
    void settled();

    QProcess m_process;
    QTimer m_timer;
    QFutureWatcher<QImage> m_decoder;
    QString m_program;
    int m_timeoutMs;
    QString m_pendingPath;
    quint64 m_pendingGeneration = 0;
    quint64 m_generation = 0;
    QByteArray m_output;
    qint64 m_errorBytes = 0;
    Failure m_failure = Failure::Invalid;
    bool m_aborted = false;
    bool m_cancelled = false;
    bool m_decoding = false;
    bool m_stopping = false;
};
