#include "PdfPreviewRenderer.h"
#include "PreviewPane.h"

#include <KFileItem>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QPainter>
#include <QPdfWriter>
#include <QPlainTextEdit>
#include <QPointer>
#include <QSignalSpy>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QtTest>
#include <cerrno>
#include <csignal>

namespace {
bool writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

bool writePdf(const QString &path)
{
    QPdfWriter writer(path);
    writer.setResolution(72);
    writer.setPageSize(QPageSize(QPageSize::A4));
    QPainter painter(&writer);
    if (!painter.isActive()) return false;
    painter.fillRect(writer.pageLayout().paintRectPixels(72), QColor(235, 80, 65));
    painter.setPen(Qt::black);
    painter.setFont(QFont(QStringLiteral("Sans Serif"), 24));
    painter.drawText(QRect(30, 60, 450, 100), Qt::AlignCenter, QStringLiteral("FIRST PAGE\nControlled PDF fixture"));
    if (!writer.newPage()) return false;
    painter.fillRect(writer.pageLayout().paintRectPixels(72), Qt::blue);
    painter.drawText(QRect(30, 60, 450, 100), Qt::AlignCenter, QStringLiteral("SECOND PAGE — not previewed"));
    return painter.end();
}

// Minimal private password-required fixture; no real document is embedded.
QByteArray encryptedPdf()
{
    const QList<QByteArray> objects{
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] >>",
        "<< /Filter /Standard /V 1 /R 2 /Length 40 /P -4 "
        "/O <0000000000000000000000000000000000000000000000000000000000000000> "
        "/U <0000000000000000000000000000000000000000000000000000000000000000> >>"};
    QByteArray pdf("%PDF-1.4\n");
    QList<int> offsets;
    for (int i = 0; i < objects.size(); ++i) {
        offsets.append(pdf.size());
        pdf += QByteArray::number(i + 1) + " 0 obj\n" + objects.at(i) + "\nendobj\n";
    }
    const int xref = pdf.size();
    pdf += "xref\n0 5\n0000000000 65535 f \n";
    for (int offset : offsets)
        pdf += QByteArray::number(offset).rightJustified(10, '0') + " 00000 n \n";
    pdf += "trailer\n<< /Size 5 /Root 1 0 R /Encrypt 4 0 R "
           "/ID [<00112233445566778899aabbccddeeff><00112233445566778899aabbccddeeff>] >>\n"
           "startxref\n" + QByteArray::number(xref) + "\n%%EOF\n";
    return pdf;
}

KFileItem pdfItem(const QString &path)
{
    return KFileItem(QUrl::fromLocalFile(path), QStringLiteral("application/pdf"), 0);
}
} // namespace

class PdfPreviewTest : public QObject {
    Q_OBJECT
    QTemporaryDir m_files;
    QList<QPointer<PdfPreviewRenderer>> m_renderers;

    PdfPreviewRenderer *renderer(const QString &program = QStringLiteral("/usr/bin/pdftoppm"),
                                 int timeout = PdfPreviewRenderer::TimeoutMs)
    {
        auto *result = new PdfPreviewRenderer(program, timeout);
        m_renderers.append(result);
        return result;
    }

    QString provider(const QByteArray &body)
    {
        const QString path = m_files.filePath(QStringLiteral("fake renderer.py"));
        if (!writeFile(path, "#!/usr/bin/python3\nimport os, time, sys, resource\n" + body)) return {};
        if (!QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner)) return {};
        return path;
    }

    void snapshot(PreviewPane &pane, const QString &name)
    {
        const QString directory = qEnvironmentVariable("EXPLORER_TEST_SCREENSHOT_DIR");
        if (directory.isEmpty()) return;
        QVERIFY(QDir().mkpath(directory));
        QVERIFY(pane.grab().save(directory + QLatin1Char('/') + name + QStringLiteral(".png")));
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_files.isValid());
        QVERIFY(QFile::exists(QStringLiteral("/usr/bin/pdftoppm")));
        QVERIFY(writePdf(m_files.filePath(QStringLiteral("-first page.pdf"))));
        QVERIFY(writeFile(m_files.filePath(QStringLiteral("note.txt")), "Current text selection"));
    }

    void cleanup()
    {
        for (auto &job : m_renderers) if (job) job->shutdown();
        for (auto &job : m_renderers) QTRY_VERIFY(job.isNull());
        m_renderers.clear();
    }

    void realFirstPageIsBoundedAndNotSecondPage()
    {
        auto *job = renderer();
        QSignalSpy ready(job, &PdfPreviewRenderer::ready);
        QSignalSpy failed(job, &PdfPreviewRenderer::failed);
        job->render(m_files.filePath(QStringLiteral("-first page.pdf")), 17);
        QTRY_COMPARE(ready.size(), 1);
        QCOMPARE(failed.size(), 0);
        QCOMPARE(ready.first().at(0).toULongLong(), quint64(17));
        const QImage image = qvariant_cast<QImage>(ready.first().at(1));
        QVERIFY(!image.isNull());
        QCOMPARE(qMax(image.width(), image.height()), PdfPreviewRenderer::MaximumDimension);
        QVERIFY(image.width() * image.height() <= PdfPreviewRenderer::MaximumDimension * PdfPreviewRenderer::MaximumDimension);
        const QColor center = image.pixelColor(image.width() / 2, image.height() / 2);
        QVERIFY(center.red() > 200 && center.blue() < 100);
    }

    void paneShowsHonestCaptionAndResizesWithoutDistortion()
    {
        PreviewPane pane;
        pane.resize(360, 520);
        pane.show();
        auto *image = pane.findChild<QLabel *>(QStringLiteral("previewImage"));
        auto *caption = pane.findChild<QLabel *>(QStringLiteral("pdfPreviewCaption"));
        auto *stack = pane.findChild<QStackedWidget *>();
        QVERIFY(image && caption && stack);
        pane.setItems({pdfItem(m_files.filePath(QStringLiteral("-first page.pdf")))});
        QTRY_COMPARE(stack->currentWidget(), image);
        QVERIFY(caption->isVisible());
        QCOMPARE(caption->text(), QStringLiteral("PDF — first page only"));
        const QSize initial = image->pixmap().size();
        QVERIFY(initial.width() > 100);
        snapshot(pane, QStringLiteral("pdf-first-page"));
        pane.resize(190, 250);
        QCoreApplication::processEvents();
        const QSize small = image->pixmap().size();
        QVERIFY(small.width() < initial.width());
        QVERIFY(small.width() <= image->width() && small.height() <= image->height());
        QVERIFY(qAbs(double(initial.width()) / initial.height() - double(small.width()) / small.height()) < 0.02);
    }

    void rapidPdfToTextClearAndHideCannotPublishStaleImages()
    {
        PreviewPane pane;
        pane.resize(300, 400);
        pane.show();
        auto *text = pane.findChild<QPlainTextEdit *>(QStringLiteral("textPreview"));
        auto *placeholder = pane.findChild<QLabel *>(QStringLiteral("previewPlaceholder"));
        auto *caption = pane.findChild<QLabel *>(QStringLiteral("pdfPreviewCaption"));
        auto *stack = pane.findChild<QStackedWidget *>();
        QVERIFY(text && placeholder && caption && stack);
        const KFileItem pdf = pdfItem(m_files.filePath(QStringLiteral("-first page.pdf")));
        for (int i = 0; i < 15; ++i) {
            pane.setItems({pdf});
            pane.setItems({KFileItem(QUrl::fromLocalFile(m_files.filePath(QStringLiteral("note.txt"))))});
        }
        QTRY_COMPARE(text->toPlainText(), QStringLiteral("Current text selection"));
        QTest::qWait(200);
        QCOMPARE(stack->currentWidget(), text);
        QVERIFY(!caption->isVisible());
        pane.setItems({pdf});
        pane.setItems({});
        QTest::qWait(200);
        QCOMPARE(stack->currentWidget(), placeholder);
        QCOMPARE(placeholder->text(), QStringLiteral("Select a file to preview."));
        pane.setItems({pdf});
        pane.hide();
        QTest::qWait(200);
        pane.show();
        QCOMPARE(stack->currentWidget(), placeholder);
        QVERIFY(!caption->isVisible());
        auto *shortLived = new PreviewPane;
        shortLived->show();
        shortLived->setItems({pdf});
        delete shortLived;
        QTest::qWait(200); // QProcess must be reaped without destructor waits/warnings.
    }

    void currentKioImageSurvivesCancelledPdf()
    {
        const QString path = m_files.filePath(QStringLiteral("current image.png"));
        QImage fixture(120, 80, QImage::Format_RGB32);
        fixture.fill(Qt::green);
        QVERIFY(fixture.save(path));
        PreviewPane pane;
        pane.resize(300, 400);
        pane.show();
        auto *image = pane.findChild<QLabel *>(QStringLiteral("previewImage"));
        auto *stack = pane.findChild<QStackedWidget *>();
        pane.setItems({pdfItem(m_files.filePath(QStringLiteral("-first page.pdf")))});
        pane.setItems({KFileItem(QUrl::fromLocalFile(path))});
        QTRY_COMPARE(stack->currentWidget(), image);
        QTest::qWait(200);
        const QImage result = image->pixmap().toImage();
        QVERIFY(!result.isNull());
        QCOMPARE(result.pixelColor(result.width() / 2, result.height() / 2), QColor(Qt::green));
        QVERIFY(!pane.findChild<QLabel *>(QStringLiteral("pdfPreviewCaption"))->isVisible());
    }

    void invalidTruncatedAndPasswordRequiredFailReadably_data()
    {
        QTest::addColumn<QByteArray>("bytes");
        QTest::newRow("invalid") << QByteArray("This is not a PDF");
        QTest::newRow("truncated") << QByteArray("%PDF-1.7\n1 0 obj\n<< /Type /Catalog");
        QTest::newRow("password-required") << encryptedPdf();
    }

    void invalidTruncatedAndPasswordRequiredFailReadably()
    {
        QFETCH(QByteArray, bytes);
        const QString path = m_files.filePath(QStringLiteral("bad.pdf"));
        QVERIFY(writeFile(path, bytes));
        PreviewPane pane;
        pane.resize(300, 400);
        pane.show();
        auto *placeholder = pane.findChild<QLabel *>(QStringLiteral("previewPlaceholder"));
        auto *stack = pane.findChild<QStackedWidget *>();
        pane.setItems({pdfItem(path)});
        QTRY_VERIFY(placeholder->text().startsWith(QStringLiteral("Cannot preview this PDF.")));
        QCOMPARE(stack->currentWidget(), placeholder);
        QVERIFY(!pane.findChild<QLabel *>(QStringLiteral("pdfPreviewCaption"))->isVisible());
        snapshot(pane, QStringLiteral("pdf-failure-") + QString::fromLatin1(QTest::currentDataTag()));
    }

    void remotePdfHasExplicitLocalOnlyFallback()
    {
        PreviewPane pane;
        pane.setItems({KFileItem(QUrl(QStringLiteral("https://invalid.example/private.pdf")),
                                QStringLiteral("application/pdf"), 0)});
        QCOMPARE(pane.findChild<QLabel *>(QStringLiteral("previewPlaceholder"))->text(),
                 QStringLiteral("PDF preview is available for local files only."));
    }

    void missingRendererIsReported()
    {
        auto *job = renderer(m_files.filePath(QStringLiteral("no-such-renderer")));
        QSignalSpy failed(job, &PdfPreviewRenderer::failed);
        job->render(m_files.filePath(QStringLiteral("-first page.pdf")), 9);
        QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(qvariant_cast<PdfPreviewRenderer::Failure>(failed.first().at(1)), PdfPreviewRenderer::Failure::Unavailable);
    }

    void timeoutKillsAndReapsRenderer()
    {
        const QString pidPath = m_files.filePath(QStringLiteral("timeout.pid"));
        const QString program = provider("open('" + pidPath.toUtf8() + "', 'w').write(str(os.getpid()))\ntime.sleep(30)\n");
        QVERIFY(!program.isEmpty());
        auto *job = renderer(program, 150);
        QSignalSpy failed(job, &PdfPreviewRenderer::failed);
        job->render(QStringLiteral("/unused.pdf"), 1);
        QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(qvariant_cast<PdfPreviewRenderer::Failure>(failed.first().at(1)), PdfPreviewRenderer::Failure::TimedOut);
        QFile pidFile(pidPath);
        QVERIFY(pidFile.open(QIODevice::ReadOnly));
        const int pid = pidFile.readAll().toInt();
        QVERIFY(pid > 0);
        QCOMPARE(::kill(pid, 0), -1);
        QCOMPARE(errno, ESRCH);
    }

    void cancelRestartAndShutdownKeepOnlyLatestGeneration()
    {
        const QString pidPath = m_files.filePath(QStringLiteral("cancel.pid"));
        const QString program = provider("open('" + pidPath.toUtf8() + "', 'a').write(str(os.getpid()) + '\\n')\n"
                                         "if sys.argv[-1].endswith('slow.pdf'): time.sleep(30)\n"
                                         "os.write(1, b'P6\\n1 1\\n255\\n\\xff\\x00\\x00')\n");
        QVERIFY(!program.isEmpty());
        auto *job = renderer(program);
        QSignalSpy ready(job, &PdfPreviewRenderer::ready);
        QSignalSpy failed(job, &PdfPreviewRenderer::failed);
        job->render(QStringLiteral("/slow.pdf"), 1);
        QTRY_VERIFY(QFile::exists(pidPath));
        job->render(QStringLiteral("/obsolete.pdf"), 2);
        job->render(QStringLiteral("/latest.pdf"), 3);
        QTRY_COMPARE(ready.size(), 1);
        QCOMPARE(ready.first().at(0).toULongLong(), quint64(3));
        QCOMPARE(failed.size(), 0);
        job->render(QStringLiteral("/slow.pdf"), 4);
        QTest::qWait(100);
        QPointer<PdfPreviewRenderer> guard(job);
        job->shutdown();
        QTRY_VERIFY(guard.isNull());
        QFile pidFile(pidPath);
        QVERIFY(pidFile.open(QIODevice::ReadOnly));
        for (const auto &line : pidFile.readAll().split('\n')) {
            if (line.isEmpty()) continue;
            const int pid = line.toInt();
            QVERIFY(pid > 0);
            QCOMPARE(::kill(pid, 0), -1);
            QCOMPARE(errno, ESRCH);
        }
        QCOMPARE(ready.size(), 1);
        QCOMPARE(failed.size(), 0);
    }

    void outputAndImageBounds_data()
    {
        QTest::addColumn<QByteArray>("body");
        QTest::addColumn<PdfPreviewRenderer::Failure>("failure");
        QTest::newRow("stdout") << QByteArray("os.write(1, b'x' * (4 * 1024 * 1024))\n")
                                << PdfPreviewRenderer::Failure::OutputLimit;
        QTest::newRow("stderr") << QByteArray("os.write(2, b'x' * (64 * 1024))\n")
                                << PdfPreviewRenderer::Failure::OutputLimit;
        QTest::newRow("oversize-image") << QByteArray("os.write(1, b'P6\\n999999 999999\\n255\\n')\n")
                                        << PdfPreviewRenderer::Failure::Invalid;
        QTest::newRow("truncated-image") << QByteArray("os.write(1, b'P6\\n100 100\\n255\\nabc')\n")
                                         << PdfPreviewRenderer::Failure::Invalid;
    }

    void childResourceLimitsAreApplied()
    {
        const QString program = provider(
            "assert resource.getrlimit(resource.RLIMIT_AS)[0] == 512 * 1024 * 1024\n"
            "assert resource.getrlimit(resource.RLIMIT_CPU)[0] == 5\n"
            "assert resource.getrlimit(resource.RLIMIT_CORE)[0] == 0\n"
            "os.write(1, b'P6\\n1 1\\n255\\n\\xff\\x00\\x00')\n");
        QVERIFY(!program.isEmpty());
        auto *job = renderer(program);
        QSignalSpy ready(job, &PdfPreviewRenderer::ready);
        QSignalSpy failed(job, &PdfPreviewRenderer::failed);
        job->render(QStringLiteral("/unused.pdf"), 1);
        QTRY_COMPARE(ready.size(), 1);
        QCOMPARE(failed.size(), 0);
    }

    void outputAndImageBounds()
    {
        QFETCH(QByteArray, body);
        QFETCH(PdfPreviewRenderer::Failure, failure);
        const QString program = provider(body);
        QVERIFY(!program.isEmpty());
        auto *job = renderer(program);
        QSignalSpy failed(job, &PdfPreviewRenderer::failed);
        QSignalSpy ready(job, &PdfPreviewRenderer::ready);
        job->render(QStringLiteral("/unused.pdf"), 1);
        QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(qvariant_cast<PdfPreviewRenderer::Failure>(failed.first().at(1)), failure);
        QCOMPARE(ready.size(), 0);
    }
};

QTEST_MAIN(PdfPreviewTest)
#include "PdfPreviewTest.moc"
