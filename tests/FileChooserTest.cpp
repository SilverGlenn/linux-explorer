#include "FileChooserDialog.h"
#include "BrowserNavigation.h"
#include "DirectoryModel.h"
#include "FileView.h"
#include "Libraries.h"
#include "Locations.h"
#include "NavigationPane.h"
#include "PreviewPane.h"
#include "SearchBox.h"
#include "ViewModeMenu.h"

#include <AeroQt/stylesheet.h>
#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QCheckBox>
#include <QScrollArea>
#include <QScrollBar>
#include <QAccessible>
#include <QItemSelectionModel>
#include <QCompleter>
#include <QStringListModel>
#include <QContextMenuEvent>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScreen>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyle>
#include <QStyleOptionComboBox>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QtMath>

class FileChooserTest : public QObject {
    Q_OBJECT
private:
    static void writeFile(const QString &path, const QByteArray &contents = "example\n")
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(contents), contents.size());
    }

    static ChooserOptions optionsFor(const QString &path, ChooserMode mode = ChooserMode::OpenFile)
    {
        ChooserOptions options;
        options.startingLocation = QUrl::fromLocalFile(path);
        options.mode = mode;
        return options;
    }

    static QLineEdit *name(FileChooserDialog &dialog)
    {
        return dialog.findChild<QLineEdit *>(QStringLiteral("chooserFileName"));
    }

    static void show(FileChooserDialog &dialog)
    {
        dialog.show();
        dialog.activateWindow();
        QTest::qWait(30);
    }

    static auto watchForLaunch(const QString &script, const QString &marker)
    {
        const QByteArray previousMarker = qgetenv("EXPLORER_CHOOSER_LAUNCH_MARKER");
        qputenv("EXPLORER_CHOOSER_LAUNCH_MARKER", QFile::encodeName(marker));
        // Fixed script; even special characters in the marker path remain data.
        writeFile(script, "#!/bin/sh\n: > \"${EXPLORER_CHOOSER_LAUNCH_MARKER:?}\"\n");
        return qScopeGuard([previousMarker] {
            if (previousMarker.isNull()) qunsetenv("EXPLORER_CHOOSER_LAUNCH_MARKER");
            else qputenv("EXPLORER_CHOOSER_LAUNCH_MARKER", previousMarker);
        });
    }

private slots:
    void initTestCase()
    {
        Aero::registerStylesheet(qApp);
    }

    void activationSelectsWithoutLaunchingAndPoliciesAreOptIn()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString script = dir.filePath(QStringLiteral("run me.sh"));
        const QString marker = dir.filePath(QStringLiteral("launched"));
        const auto restoreMarker = watchForLaunch(script, marker);
        QVERIFY(QFile::setPermissions(script, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        FileChooserDialog dialog(optionsFor(dir.path()));
        QSignalSpy accepted(&dialog, &QDialog::accepted);
        show(dialog);
        auto *model = dialog.findChild<DirectoryModel *>();
        auto *files = dialog.findChild<FileView *>();
        QTRY_VERIFY(model->indexForUrl(QUrl::fromLocalFile(script)).isValid());
        QCOMPARE(files->currentView()->selectionMode(), QAbstractItemView::SingleSelection);
        QVERIFY(!files->currentView()->dragEnabled());
        QVERIFY(!files->currentView()->viewport()->acceptDrops());
        QSignalSpy renamed(files, &FileView::renameRequested);
        files->renameItem(QUrl::fromLocalFile(script));
        QCOMPARE(renamed.count(), 0);
        auto *places = dialog.findChild<NavigationPane *>();
        auto *tree = places->findChild<QTreeWidget *>();
        QVERIFY(tree);
        QVERIFY(!tree->dragEnabled());
        QVERIFY(!tree->viewport()->acceptDrops());
        QCOMPARE(tree->contextMenuPolicy(), Qt::PreventContextMenu);
        QContextMenuEvent context(QContextMenuEvent::Mouse, QPoint(10, 10), QPoint(10, 10));
        QApplication::sendEvent(tree->viewport(), &context);
        QVERIFY(!QApplication::activePopupWidget());
        QVERIFY(files->selectUrl(QUrl::fromLocalFile(script)));
        files->focusView();
        QTest::keyClick(files->currentView(), Qt::Key_Return);
        QTRY_COMPARE(accepted.count(), 1);
        QCOMPARE(dialog.selectedUrls(), QList<QUrl>{QUrl::fromLocalFile(script)});
        QTest::qWait(60);
        QVERIFY(!QFileInfo::exists(marker));
        DirectoryModel normalModel;
        FileView normal(&normalModel);
        QCOMPARE(normal.currentView()->selectionMode(), QAbstractItemView::ExtendedSelection);
        QVERIFY(normal.currentView()->dragEnabled());
    }

    void filterGlobsOrMimeKeepsFoldersAndSearchIndependent()
    {
        QTemporaryDir dir;
        QVERIFY(QDir(dir.path()).mkdir(QStringLiteral("folder.bin")));
        writeFile(dir.filePath(QStringLiteral("one.TxT")));
        writeFile(dir.filePath(QStringLiteral("picture.png")), QByteArray::fromHex(
            "89504e470d0a1a0a0000000d49484452000000010000000108060000001f15c489"));
        writeFile(dir.filePath(QStringLiteral("hidden.bin")), QByteArray("\0\1\2\3", 4));
        auto options = optionsFor(dir.path());
        options.filters = {{QStringLiteral("mixed"), QStringLiteral("Text or PNG"), {QStringLiteral("*.txt")}, {QStringLiteral("image/png")}},
                           {QStringLiteral("binary"), QStringLiteral("Binary"), {QStringLiteral("*.bin")}, {}}};
        options.selectedFilter = QStringLiteral("mixed");
        FileChooserDialog dialog(options);
        show(dialog);
        auto *model = dialog.findChild<DirectoryModel *>();
        QTRY_COMPARE(model->rowCount(), 3);
        QCOMPARE(dialog.selectedFilter(), QStringLiteral("mixed"));
        QVERIFY(model->indexForUrl(QUrl::fromLocalFile(dir.filePath(QStringLiteral("folder.bin")))).isValid());
        QVERIFY(!model->indexForUrl(QUrl::fromLocalFile(dir.filePath(QStringLiteral("hidden.bin")))).isValid());
        auto *search = dialog.findChild<SearchBox *>();
        search->setText(QStringLiteral("one"));
        QTRY_COMPARE(model->rowCount(), 1);
        dialog.findChild<QComboBox *>()->setCurrentIndex(1);
        QTRY_COMPARE(model->rowCount(), 0);
        search->clear();
        QTRY_COMPARE(model->rowCount(), 2);
        dialog.findChild<QComboBox *>()->setCurrentIndex(0);
        QTRY_COMPARE(model->rowCount(), 3);
        auto *files = dialog.findChild<FileView *>();
        const QUrl folder = QUrl::fromLocalFile(dir.filePath(QStringLiteral("folder.bin")));
        QVERIFY(files->selectUrl(folder));
        files->focusView();
        QTest::keyClick(files->currentView(), Qt::Key_Return);
        QTRY_COMPARE(dialog.currentUrl(), folder);
        QVERIFY(dialog.selectedUrls().isEmpty());
    }

    void selectedFolderButtonNavigatesAndPreservesSuggestedName_data()
    {
        QTest::addColumn<bool>("save");
        QTest::newRow("open") << false;
        QTest::newRow("save") << true;
    }

    void selectedFolderButtonNavigatesAndPreservesSuggestedName()
    {
        QFETCH(bool, save);
        QTemporaryDir dir;
        QVERIFY(QDir(dir.path()).mkdir(QStringLiteral("folder.bin")));
        writeFile(dir.filePath(QStringLiteral("stale.txt")));
        auto options = optionsFor(dir.path(), save ? ChooserMode::SaveFile : ChooserMode::OpenFile);
        options.suggestedName = QStringLiteral("stale.txt");
        options.filters = {{"text", "Text", {"*.txt"}, {}}};
        FileChooserDialog dialog(options);
        show(dialog);
        QSignalSpy accepted(&dialog, &QDialog::accepted);
        auto *model = dialog.findChild<DirectoryModel *>();
        auto *files = dialog.findChild<FileView *>();
        const QUrl folder = QUrl::fromLocalFile(dir.filePath(QStringLiteral("folder.bin")));
        QTRY_VERIFY(model->indexForUrl(folder).isValid());
        QVERIFY(files->selectUrl(folder));
        auto *buttons = dialog.findChild<QDialogButtonBox *>();
        for (auto *button : buttons->buttons())
            if (buttons->buttonRole(button) == QDialogButtonBox::AcceptRole) button->click();
        QCOMPARE(dialog.currentUrl(), folder);
        QCOMPARE(accepted.count(), 0);
        QCOMPARE(name(dialog)->text(), QStringLiteral("stale.txt"));
        QVERIFY(dialog.isVisible());
        dialog.navigateTo(QUrl::fromLocalFile(dir.path()));
        QTRY_VERIFY(model->indexForUrl(folder).isValid());
        QVERIFY(files->selectUrl(folder));
        name(dialog)->setFocus();
        name(dialog)->selectAll();
        QTest::keyClicks(name(dialog), "stale.txt");
        dialog.accept();
        if (save) {
            auto *confirm = dialog.findChild<QMessageBox *>();
            QVERIFY(confirm);
            confirm->button(QMessageBox::No)->click();
        } else {
            QCOMPARE(accepted.count(), 1);
            QCOMPARE(dialog.selectedUrls().first(), QUrl::fromLocalFile(dir.filePath(QStringLiteral("stale.txt"))));
        }
    }

    void overlongSaveNamesAndPathsStayOpen()
    {
        QTemporaryDir dir;
        FileChooserDialog dialog(optionsFor(dir.path(), ChooserMode::SaveFile));
        show(dialog);
        QSignalSpy accepted(&dialog, &QDialog::accepted);
        for (const QString &text : {QString(300, QLatin1Char('a')), QString(100, QChar(0x65e5)),
                                    QStringLiteral("/") + QString(4200, QLatin1Char('x'))}) {
            name(dialog)->setText(text);
            dialog.accept();
            QCOMPARE(accepted.count(), 0);
            QVERIFY(dialog.isVisible());
            QVERIFY(!dialog.errorMessage().isEmpty());
            QVERIFY(dialog.selectedUrls().isEmpty());
        }
        QVERIFY(QDir(dir.path()).entryList(QDir::Files | QDir::Hidden).isEmpty());
    }

    void typedAndEncodedNamesRoundTrip_data()
    {
        QTest::addColumn<QString>("leaf");
        QTest::addColumn<bool>("encoded");
        QTest::addColumn<bool>("save");
        for (const bool save : {false, true}) {
            const char *mode = save ? "save" : "open";
            QTest::addRow("%s-literal", mode) << QStringLiteral("sp ace # 100% 日本語.txt") << false << save;
            QTest::addRow("%s-uri", mode) << QStringLiteral("sp ace # 100% 日本語.txt") << true << save;
            QTest::addRow("%s-literal-percent-escape", mode) << QStringLiteral("a%20b.txt") << false << save;
            QTest::addRow("%s-leading-spaces", mode) << QStringLiteral("  real name  ") << false << save;
            QTest::addRow("%s-literal-colon", mode) << QStringLiteral("report:2026.txt") << false << save;
            QTest::addRow("%s-uri-colon", mode) << QStringLiteral("report:2026.txt") << true << save;
        }
    }

    void typedAndEncodedNamesRoundTrip()
    {
        QFETCH(QString, leaf);
        QFETCH(bool, encoded);
        QFETCH(bool, save);
        QTemporaryDir dir;
        const QString path = dir.filePath(leaf);
        if (!save) writeFile(path);
        auto options = optionsFor(dir.path(), save ? ChooserMode::SaveFile : ChooserMode::OpenFile);
        options.suggestedName = encoded ? QString::fromUtf8(QUrl::fromLocalFile(path).toEncoded()) : leaf;
        FileChooserDialog dialog(options);
        show(dialog);
        QSignalSpy accepted(&dialog, &QDialog::accepted);
        dialog.accept();
        QCOMPARE(accepted.count(), 1);
        QCOMPARE(dialog.selectedUrls().first().toLocalFile(), path);
        const QByteArray wire = dialog.selectedUrls().first().toEncoded(QUrl::FullyEncoded);
        QCOMPARE(QUrl::fromEncoded(wire, QUrl::StrictMode).toLocalFile(), path);
        if (leaf.contains('#')) QVERIFY(wire.contains("%23"));
        if (leaf.contains('%')) QVERIFY(wire.contains("%25"));
        if (save) QVERIFY(!QFileInfo::exists(path));
    }

    void saveExtensionsAndNoWrites_data()
    {
        QTest::addColumn<QString>("typed");
        QTest::addColumn<QString>("extension");
        QTest::addColumn<QString>("expected");
        QTest::newRow("infer") << "draft" << "" << "draft.txt";
        QTest::newRow("explicit-default") << "draft" << "md" << "draft.md";
        QTest::newRow("keep-suffix") << "draft.bin" << "txt" << "draft.bin";
        QTest::newRow("literal-colon") << "report:2026.txt" << "txt" << "report:2026.txt";
        QTest::newRow("colon-with-default-extension") << "report:2026" << "txt" << "report:2026.txt";
        QTest::newRow("trailing-dot") << "draft." << "txt" << "draft.";
        QTest::newRow("hidden-name") << ".draft" << "txt" << ".draft.txt";
        QTest::newRow("space-and-unicode-extension") << "draft" << "a 日本語" << "draft.a 日本語";
        QTest::newRow("consecutive-dot-extension") << "draft" << "a..b" << "draft.a..b";
        QTest::newRow("literal-special") << "a # %.日本語" << "txt" << "a # %.日本語";
    }

    void saveExtensionsAndNoWrites()
    {
        QFETCH(QString, typed);
        QFETCH(QString, extension);
        QFETCH(QString, expected);
        QTemporaryDir dir;
        auto options = optionsFor(dir.path(), ChooserMode::SaveFile);
        options.filters = {{"text", "Text", {"*.txt"}, {}}};
        options.defaultExtension = extension;
        options.suggestedName = typed;
        FileChooserDialog dialog(options);
        show(dialog);
        QSignalSpy accepted(&dialog, &QDialog::accepted);
        dialog.accept();
        QCOMPARE(accepted.count(), 1);
        QCOMPARE(dialog.selectedUrls().first().toLocalFile(), dir.filePath(expected));
        QVERIFY(QDir(dir.path()).entryList(QDir::Files | QDir::Hidden).isEmpty());
    }

    void saveInfersLiteralUnicodeExtensionFromSelectedFilter()
    {
        QTemporaryDir dir;
        auto options = optionsFor(dir.path(), ChooserMode::SaveFile);
        options.suggestedName = QStringLiteral("draft");
        options.filters = {{"text", "Text", {"*.txt"}, {}},
                           {"unicode", "Unicode", {"*.[ch]", QString::fromUtf8("*.日本語")}, {}}};
        options.selectedFilter = QStringLiteral("unicode");
        FileChooserDialog dialog(options);
        show(dialog);
        dialog.accept();
        QCOMPARE(dialog.selectedFilter(), QStringLiteral("unicode"));
        QCOMPARE(dialog.selectedUrls(), QList<QUrl>{QUrl::fromLocalFile(dir.filePath(QStringLiteral("draft.日本語")))});
        QVERIFY(QDir(dir.path()).entryList(QDir::Files | QDir::Hidden).isEmpty());
    }

    void overwriteDeclineApproveAndCancellation()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("existing.txt"));
        writeFile(path, "must not change\n");
        auto options = optionsFor(dir.path(), ChooserMode::SaveFile);
        options.suggestedName = QStringLiteral("existing.txt");
        FileChooserDialog dialog(options);
        QSignalSpy accepted(&dialog, &QDialog::accepted);
        show(dialog);
        dialog.accept();
        auto *confirm = dialog.findChild<QMessageBox *>();
        QVERIFY(confirm);
        QCOMPARE(confirm->defaultButton(), confirm->button(QMessageBox::No));
        confirm->button(QMessageBox::No)->click();
        QTRY_VERIFY(!confirm->isVisible());
        QVERIFY(dialog.isVisible());
        QCOMPARE(accepted.count(), 0);
        QVERIFY(dialog.selectedUrls().isEmpty());
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        dialog.accept();
        confirm = dialog.findChild<QMessageBox *>();
        QVERIFY(confirm);
        confirm->button(QMessageBox::Yes)->click();
        QTRY_COMPARE(accepted.count(), 1);
        QCOMPARE(dialog.selectedUrls().first().toLocalFile(), path);
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("must not change\n"));
        file.close();

        FileChooserDialog canceled(options);
        show(canceled);
        QSignalSpy canceledAccepted(&canceled, &QDialog::accepted);
        QSignalSpy rejected(&canceled, &QDialog::rejected);
        canceled.accept();
        QPointer<QMessageBox> pending = canceled.findChild<QMessageBox *>();
        QVERIFY(pending && pending->isVisible());
        canceled.reject();
        QCOMPARE(rejected.count(), 1);
        QCOMPARE(canceledAccepted.count(), 0);
        QVERIFY(canceled.selectedUrls().isEmpty());
        QVERIFY(!pending || !pending->isVisible());
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(!pending);
    }

    void invalidAndLocalOnlyLocations()
    {
        QTemporaryDir dir;
        auto options = optionsFor(dir.filePath(QStringLiteral("missing")));
        FileChooserDialog dialog(options);
        show(dialog);
        QVERIFY(!dialog.errorMessage().isEmpty());
        QVERIFY(dialog.findChild<QLabel *>(QStringLiteral("chooserError"))->isVisible());
        QSignalSpy accepted(&dialog, &QDialog::accepted);
        dialog.navigateTo(QUrl::fromLocalFile(dir.path()));
        QCOMPARE(dialog.currentUrl(), QUrl::fromLocalFile(dir.path()));
        QVERIFY(dialog.errorMessage().isEmpty());
        for (const QUrl &url : {QUrl("https://example.invalid/"), QUrl("admin:///tmp"),
                               QUrl("file://remote/tmp"), QUrl("trash:///"), QUrl("libraries:///bad")}) {
            dialog.navigateTo(url);
            QCOMPARE(dialog.currentUrl(), QUrl::fromLocalFile(dir.path()));
            QVERIFY(!dialog.errorMessage().isEmpty());
        }
        for (const QString &text : {QString(), QStringLiteral("missing"), QStringLiteral("file:///tmp/a%xx"),
                                    QStringLiteral("file:///tmp/a#fragment"), QStringLiteral("https://example.invalid/file")}) {
            name(dialog)->setText(text);
            dialog.accept();
            QCOMPARE(accepted.count(), 0);
            QVERIFY(!dialog.errorMessage().isEmpty());
            QVERIFY(dialog.isVisible());
        }
        dialog.navigateTo(Locations::libraries());
        QCOMPARE(dialog.currentUrl(), Locations::libraries());
        name(dialog)->setText(QStringLiteral("relative.txt"));
        dialog.accept();
        QVERIFY(!dialog.errorMessage().isEmpty());
        for (const auto &entry : Libraries::entries()) {
            QVERIFY(QDir().mkpath(entry.url.toLocalFile()));
            dialog.navigateTo(entry.url);
            QCOMPARE(dialog.currentUrl(), entry.url);
        }
        dialog.navigateTo(Locations::computer());
        QCOMPARE(dialog.currentUrl(), Locations::computer());
        QCOMPARE(accepted.count(), 0);
    }

    void saveRejectsRemoteUriText()
    {
        QTemporaryDir dir;
        // If URLs were misread as relative paths, these would be writable parents.
        for (const QString &folder : {QStringLiteral("admin:"), QStringLiteral("trash:"),
                                      QStringLiteral("sftp:/host")})
            QVERIFY(QDir(dir.path()).mkpath(folder));
        FileChooserDialog dialog(optionsFor(dir.path(), ChooserMode::SaveFile));
        show(dialog);
        QSignalSpy accepted(&dialog, &QDialog::accepted);
        for (const QString &text : {QStringLiteral("sftp://host/output.log"),
                                    QStringLiteral("admin:/output.log"),
                                    QStringLiteral("trash:/output.log"),
                                    QStringLiteral("file://remote/tmp/output.log")}) {
            name(dialog)->setText(text);
            dialog.accept();
            QCOMPARE(accepted.count(), 0);
            QVERIFY(dialog.isVisible());
            QVERIFY(!dialog.errorMessage().isEmpty());
            QVERIFY(dialog.selectedUrls().isEmpty());
        }
    }

    void unreadablePermissionsAndSymlinks()
    {
        QTemporaryDir dir;
        const QString locked = dir.filePath(QStringLiteral("locked"));
        QVERIFY(QDir().mkdir(locked));
        QVERIFY(QFile::setPermissions(locked, QFile::WriteOwner));
        FileChooserDialog dialog(optionsFor(dir.path()));
        show(dialog);
        dialog.navigateTo(QUrl::fromLocalFile(locked));
        QCOMPARE(dialog.currentUrl(), QUrl::fromLocalFile(dir.path()));
        QVERIFY(!dialog.errorMessage().isEmpty());
        QVERIFY(QFile::setPermissions(locked, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        const QString file = dir.filePath(QStringLiteral("unreadable"));
        writeFile(file);
        QVERIFY(QFile::setPermissions(file, QFile::WriteOwner));
        name(dialog)->setText(QStringLiteral("unreadable"));
        dialog.accept();
        QVERIFY(dialog.selectedUrls().isEmpty());
        QVERIFY(!dialog.errorMessage().isEmpty());
        QVERIFY(QFile::setPermissions(file, QFile::ReadOwner | QFile::WriteOwner));
        const QString broken = dir.filePath(QStringLiteral("broken"));
        QVERIFY(QFile::link(dir.filePath(QStringLiteral("nonexistent")), broken));
        auto saveOptions = optionsFor(dir.path(), ChooserMode::SaveFile);
        saveOptions.suggestedName = QStringLiteral("broken");
        FileChooserDialog save(saveOptions);
        show(save);
        save.accept();
        QVERIFY(save.selectedUrls().isEmpty());
        QVERIFY(!save.errorMessage().isEmpty());
        QVERIFY(!QFileInfo::exists(dir.filePath(QStringLiteral("nonexistent"))));
        const QString link = dir.filePath(QStringLiteral("link"));
        QVERIFY(QFile::link(file, link));
        name(dialog)->setText(QStringLiteral("link"));
        dialog.accept();
        QCOMPARE(dialog.selectedUrls(), QList<QUrl>{QUrl::fromLocalFile(link)});

        QVERIFY(QFile::setPermissions(locked, QFile::ReadOwner | QFile::ExeOwner));
        auto unwritableOptions = optionsFor(locked, ChooserMode::SaveFile);
        unwritableOptions.suggestedName = QStringLiteral("new.txt");
        FileChooserDialog unwritable(unwritableOptions);
        show(unwritable);
        unwritable.accept();
        QVERIFY(unwritable.selectedUrls().isEmpty());
        QVERIFY(!unwritable.errorMessage().isEmpty());
        QVERIFY(QFile::setPermissions(locked, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    }

    void keyboardAddressEscapeAndPaneCycle()
    {
        QTemporaryDir dir;
        QVERIFY(QDir(dir.path()).mkdir(QStringLiteral("sub")));
        FileChooserDialog dialog(optionsFor(dir.path()));
        show(dialog);
        auto *nav = dialog.findChild<BrowserNavigation *>();
        QPointer<QLineEdit> editor;
        auto *files = dialog.findChild<FileView *>();
        for (const auto &key : {QKeySequence(Qt::CTRL | Qt::Key_L), QKeySequence(Qt::ALT | Qt::Key_D)}) {
            QTest::keySequence(&dialog, key);
            editor = nav->pathEditor();
            QVERIFY(editor);
            QTRY_VERIFY(editor->isVisible() && editor->hasFocus());
            QTest::keyClick(editor, Qt::Key_Escape);
            QVERIFY(!nav->pathEditor());
            QTRY_VERIFY(!editor || !editor->isVisible());
            QVERIFY(dialog.isVisible());
        }
        QTest::keyClick(&dialog, Qt::Key_F4);
        editor = nav->pathEditor();
        QVERIFY(editor);
        QTRY_VERIFY(editor->isVisible());
        if (auto *popup = QApplication::activePopupWidget()) popup->close();
        QTest::keyClick(editor, Qt::Key_Escape);
        const QUrl sub = QUrl::fromLocalFile(dir.filePath(QStringLiteral("sub")));
        writeFile(dir.filePath(QStringLiteral("existing.txt")));
        name(dialog)->setText(QStringLiteral("existing.txt"));
        QSignalSpy accepted(&dialog, &QDialog::accepted);
        auto *search = dialog.findChild<SearchBox *>();
        for (const auto &key : {QKeySequence(Qt::CTRL | Qt::Key_E), QKeySequence(Qt::CTRL | Qt::Key_F),
                                QKeySequence(Qt::Key_F3)}) {
            files->focusView();
            QTest::keySequence(&dialog, key);
            QTRY_VERIFY(search->hasFocus());
            QTest::keyClick(search, Qt::Key_Return);
            QCOMPARE(accepted.count(), 0);
            QVERIFY(dialog.isVisible());
        }
        nav->focusPathEdit();
        nav->pathEditor()->setText(sub.toLocalFile());
        QTest::keyClick(nav->pathEditor(), Qt::Key_Return);
        QTRY_VERIFY(dialog.currentUrl().matches(sub, QUrl::StripTrailingSlash));
        QCOMPARE(accepted.count(), 0);
        QVERIFY(dialog.isVisible());
        files->focusView();
        QTest::keyClick(&dialog, Qt::Key_Left, Qt::AltModifier);
        QTRY_VERIFY(dialog.currentUrl().matches(QUrl::fromLocalFile(dir.path()), QUrl::StripTrailingSlash));
        QTest::keyClick(&dialog, Qt::Key_Right, Qt::AltModifier);
        QTRY_COMPARE(dialog.currentUrl(), sub);
        QTest::keyClick(&dialog, Qt::Key_Up, Qt::AltModifier);
        QTRY_VERIFY(dialog.currentUrl().matches(QUrl::fromLocalFile(dir.path()), QUrl::StripTrailingSlash));
        name(dialog)->setFocus();
        QTest::keyClick(name(dialog), Qt::Key_F6);
        QTRY_VERIFY(dialog.findChild<QComboBox *>()->hasFocus());
        QTest::keyClick(dialog.findChild<QComboBox *>(), Qt::Key_F6, Qt::ShiftModifier);
        QTRY_VERIFY(name(dialog)->hasFocus());
        QTest::keyClick(name(dialog), Qt::Key_F6, Qt::ShiftModifier);
        QTRY_VERIFY(files->currentView()->hasFocus());
        QTest::keyClick(files->currentView(), Qt::Key_F6, Qt::ShiftModifier);
        auto *places = dialog.findChild<NavigationPane *>();
        QTRY_VERIFY(places->isAncestorOf(QApplication::focusWidget()));
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_F6, Qt::ShiftModifier);
        QTRY_VERIFY(search->hasFocus());
        QTest::keyClick(search, Qt::Key_F6, Qt::ShiftModifier);
        QTRY_VERIFY(nav->pathEditor() && nav->pathEditor()->hasFocus());
        QTest::keyClick(nav->pathEditor(), Qt::Key_Escape);
        QVERIFY(dialog.isVisible());
        files->focusView();
        QTest::keyClick(files->currentView(), Qt::Key_Escape);
        QTRY_VERIFY(!dialog.isVisible());
    }

    void addressCompletionPopupKeepsNativeReturnHandling()
    {
        QTemporaryDir dir;
        auto options = optionsFor(dir.path());
        options.suggestedName = QStringLiteral("existing.txt");
        writeFile(dir.filePath(options.suggestedName));
        FileChooserDialog dialog(options);
        show(dialog);
        auto *navigation = dialog.findChild<BrowserNavigation *>();
        navigation->focusPathEdit();
        auto *editor = navigation->pathEditor();
        // A deterministic native completer popup avoids depending on filesystem
        // watcher timing while exercising the real QLineEdit/QCompleter routing.
        auto *completion = new QCompleter(QStringList{QStringLiteral("candidate")}, editor);
        editor->setCompleter(completion);
        editor->setText(QStringLiteral("cand"));
        completion->setCompletionPrefix(QStringLiteral("cand"));
        completion->complete();
        QTRY_VERIFY(completion->popup()->isVisible());
        completion->popup()->setCurrentIndex(completion->completionModel()->index(0, 0));
        QSignalSpy accepted(&dialog, &QDialog::accepted);
        QSignalSpy rejected(&dialog, &QDialog::rejected);
        QTest::keyClick(completion->popup(), Qt::Key_Return);
        QTRY_VERIFY(!completion->popup()->isVisible());
        QCOMPARE(editor->text(), QStringLiteral("candidate"));
        QCOMPARE(accepted.count(), 0);
        QCOMPARE(rejected.count(), 0);
        QVERIFY(dialog.isVisible());
        QCOMPARE(dialog.currentUrl(), options.startingLocation);
    }

    void cancelButtonEnterCancels_data()
    {
        QTest::addColumn<bool>("save");
        QTest::addColumn<int>("key");
        QTest::newRow("open-return") << false << int(Qt::Key_Return);
        QTest::newRow("open-enter") << false << int(Qt::Key_Enter);
        QTest::newRow("save-return") << true << int(Qt::Key_Return);
        QTest::newRow("save-enter") << true << int(Qt::Key_Enter);
    }

    void cancelButtonEnterCancels()
    {
        QFETCH(bool, save);
        QFETCH(int, key);
        QTemporaryDir dir;
        auto options = optionsFor(dir.path(), save ? ChooserMode::SaveFile : ChooserMode::OpenFile);
        options.suggestedName = QStringLiteral("valid.txt");
        if (!save) writeFile(dir.filePath(options.suggestedName));
        FileChooserDialog dialog(options);
        show(dialog);
        QSignalSpy accepted(&dialog, &QDialog::accepted);
        QSignalSpy rejected(&dialog, &QDialog::rejected);
        auto *cancel = dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Cancel);
        QVERIFY(cancel);
        cancel->setFocus();
        QTRY_VERIFY(cancel->hasFocus());
        QTest::keyClick(cancel, Qt::Key(key));
        QCOMPARE(rejected.count(), 1);
        QCOMPARE(accepted.count(), 0);
        QVERIFY(dialog.selectedUrls().isEmpty());
    }

    void addressTypedFileAcceptsExactlyOnceWithoutLaunching()
    {
        QTemporaryDir dir;
        const QString script = dir.filePath(QStringLiteral("file # 100% 日本語.sh"));
        const QString marker = dir.filePath(QStringLiteral("launched"));
        const auto restoreMarker = watchForLaunch(script, marker);
        QVERIFY(QFile::setPermissions(script, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        auto options = optionsFor(dir.path());
        options.suggestedName = QStringLiteral("stale-and-missing");
        FileChooserDialog dialog(options);
        show(dialog);
        auto *buttons = dialog.findChild<QDialogButtonBox *>();
        for (auto *button : buttons->buttons()) {
            auto *push = qobject_cast<QPushButton *>(button);
            QVERIFY(push);
            QCOMPARE(push->isDefault(), buttons->buttonRole(button) == QDialogButtonBox::AcceptRole);
        }
        auto *navigation = dialog.findChild<BrowserNavigation *>();
        QSignalSpy accepted(&dialog, &QDialog::accepted);
        QSignalSpy rejected(&dialog, &QDialog::rejected);
        navigation->focusPathEdit();
        navigation->pathEditor()->setText(QString::fromUtf8(QUrl::fromLocalFile(script).toEncoded()));
        QTest::keyClick(navigation->pathEditor(), Qt::Key_Return);
        QCOMPARE(accepted.count(), 1);
        QCOMPARE(rejected.count(), 0);
        QCOMPARE(dialog.selectedUrls(), QList<QUrl>{QUrl::fromLocalFile(script)});
        QTest::qWait(30);
        QVERIFY(!QFileInfo::exists(marker));
    }

    void unsupportedCppOptionsStayOpen_data()
    {
        QTest::addColumn<int>("kind");
        QTest::newRow("remote") << 0;
        QTest::newRow("multiple") << 1;
        QTest::newRow("directories") << 2;
        QTest::newRow("filter-id") << 3;
        QTest::newRow("leading-dot-extension") << 4;
        QTest::newRow("separator-extension") << 5;
        QTest::newRow("control-extension") << 6;
    }

    void unsupportedCppOptionsStayOpen()
    {
        QFETCH(int, kind);
        QTemporaryDir dir;
        writeFile(dir.filePath(QStringLiteral("existing.txt")));
        auto options = optionsFor(dir.path());
        options.suggestedName = QStringLiteral("existing.txt");
        if (kind == 0) options.localOnly = false;
        if (kind == 1) { options.mode = ChooserMode::SaveFile; options.multiple = true; }
        if (kind == 2) { options.mode = ChooserMode::SaveFile; options.directory = true; }
        if (kind == 3) options.selectedFilter = QStringLiteral("missing-id");
        if (kind == 4) options.defaultExtension = QStringLiteral(".txt");
        if (kind == 5) options.defaultExtension = QStringLiteral("a/b");
        if (kind == 6) options.defaultExtension = QStringLiteral("a\nb");
        FileChooserDialog dialog(options);
        show(dialog);
        QSignalSpy accepted(&dialog, &QDialog::accepted);
        dialog.accept();
        QCOMPARE(accepted.count(), 0);
        QVERIFY(dialog.isVisible());
        QVERIFY(!dialog.errorMessage().isEmpty());
        QVERIFY(dialog.selectedUrls().isEmpty());
    }

    void fileTypeArrowUsesAeroGlyphAndNativePopup()
    {
        QTemporaryDir dir;
        writeFile(dir.filePath(QStringLiteral("selected.deb")));
        auto options = optionsFor(dir.path());
        options.suggestedName = QStringLiteral("selected.deb");
        options.filters = {{"deb", "Debian packages (*.deb)", {"*.deb"}, {}},
                           {"all", "All files (*)", {"*"}, {}}};
        FileChooserDialog dialog(options);
        show(dialog);
        QSignalSpy accepted(&dialog, &QDialog::accepted);
        auto *combo = dialog.findChild<QComboBox *>(QStringLiteral("chooserFileType"));
        QVERIFY(combo);
        const QImage glyph(QStringLiteral(":/AeroQt/ribbon/arrowDown.svg.png"));
        QVERIFY(!glyph.isNull());
        QVERIFY(dialog.styleSheet().contains(QStringLiteral("url(:/AeroQt/ribbon/arrowDown.svg.png)")));
        QStyleOptionComboBox option;
        option.initFrom(combo);
        const QRect arrow = combo->style()->subControlRect(QStyle::CC_ComboBox, &option,
                                                           QStyle::SC_ComboBoxArrow, combo);
        QVERIFY(arrow.width() >= 18);
        QTest::mouseMove(&dialog, QPoint(1, 1));
        for (int state = 0; state < 3; ++state) {
            if (state == 1) QTest::mouseMove(combo, arrow.center());
            if (state == 2) {
                QTest::mouseClick(combo, Qt::LeftButton, Qt::NoModifier, arrow.center());
                QTRY_VERIFY(combo->view()->isVisible());
            }
            QTest::qWait(10);
            const QImage image = combo->grab().toImage();
            const QString snapshots = qEnvironmentVariable("EXPLORER_SNAPSHOTS");
            if (!snapshots.isEmpty()) {
                QVERIFY(QDir().mkpath(snapshots));
                QVERIFY(image.save(snapshots + QStringLiteral("/chooser-file-type-%1.png").arg(state)));
            }
            const qreal dpr = image.devicePixelRatio();
            const QRect inside = arrow.adjusted(3, 2, -3, -2);
            const QRect pixels = QRect(qRound(inside.x() * dpr), qRound(inside.y() * dpr),
                qRound(inside.width() * dpr), qRound(inside.height() * dpr)).intersected(image.rect());
            QRect ink;
            int darkPixels = 0;
            for (int y = pixels.top(); y <= pixels.bottom(); ++y) {
                for (int x = pixels.left(); x <= pixels.right(); ++x) {
                    const QColor color = image.pixelColor(x, y);
                    if (qMax(color.red(), qMax(color.green(), color.blue())) >= 128) continue;
                    // The old platform arrow was navy. Fractional-DPI blending
                    // of black over the pressed #D6E4F0 surface can differ by 14
                    // between red/blue while all channels remain below 128.
                    QVERIFY2(qAbs(color.red() - color.blue()) <= 14,
                        qPrintable(QStringLiteral("state %1 pixel %2").arg(state).arg(color.name())));
                    ++darkPixels;
                    ink = ink.united(QRect(x, y, 1, 1));
                }
            }
            QVERIFY(darkPixels >= qRound(4 * dpr * dpr));
            QVERIFY(ink.width() <= qCeil(7 * dpr));
            QVERIFY(ink.height() <= qCeil(5 * dpr));
            const QPointF center = QRectF(ink).center() / dpr;
            QVERIFY(qAbs(center.x() - QRectF(arrow).center().x()) <= 2);
            QVERIFY(qAbs(center.y() - QRectF(arrow).center().y()) <= 2);
        }
        QTest::keyClick(combo->view(), Qt::Key_Escape);
        QTRY_VERIFY(!combo->view()->isVisible());
        combo->setFocus();
        QTest::keyClick(combo, Qt::Key_Down, Qt::AltModifier);
        QTRY_VERIFY(combo->view()->isVisible());
        QTest::keyClick(combo->view(), Qt::Key_Down);
        QTest::keyClick(combo->view(), Qt::Key_Return);
        QTRY_COMPARE(dialog.selectedFilter(), QStringLiteral("all"));
        QCOMPARE(accepted.count(), 0);
        QVERIFY(dialog.isVisible());
        QVERIFY(dialog.errorMessage().isEmpty());
    }

    void searchCaptionFollowsFriendlyLocationAndHistory()
    {
        QTemporaryDir dir;
        QVERIFY(QDir(dir.path()).mkdir(QStringLiteral("Folder with spaces")));
        FileChooserDialog dialog(optionsFor(dir.path()));
        show(dialog);
        auto *search = dialog.findChild<SearchBox *>();
        auto *navigation = dialog.findChild<BrowserNavigation *>();
        QVERIFY(search && navigation);
        QCOMPARE(search->placeholderText(), QStringLiteral("Search %1").arg(QFileInfo(dir.path()).fileName()));
        QCOMPARE(search->accessibleName(), search->placeholderText());
        const QUrl folder = QUrl::fromLocalFile(dir.filePath(QStringLiteral("Folder with spaces")));
        dialog.navigateTo(folder);
        QCOMPARE(search->placeholderText(), QStringLiteral("Search Folder with spaces"));
        search->setText(QStringLiteral("nothing"));
        QTest::keyClick(&dialog, Qt::Key_Left, Qt::AltModifier);
        QTRY_COMPARE(dialog.currentUrl(), QUrl::fromLocalFile(dir.path()));
        QVERIFY(search->text().isEmpty());
        QCOMPARE(search->accessibleName(), search->placeholderText());
        QTest::keyClick(&dialog, Qt::Key_Right, Qt::AltModifier);
        QTRY_COMPARE(dialog.currentUrl(), folder);
        QCOMPARE(search->placeholderText(), QStringLiteral("Search Folder with spaces"));
        for (const auto &location : {Locations::computer(), Locations::libraries(), QUrl::fromLocalFile(QStringLiteral("/"))}) {
            dialog.navigateTo(location);
            const auto crumbs = navigation->crumbsFor(location);
            QVERIFY(!crumbs.isEmpty());
            QCOMPARE(search->placeholderText(), QStringLiteral("Search %1").arg(crumbs.last().label));
            QCOMPARE(search->accessibleName(), search->placeholderText());
            QVERIFY(!search->placeholderText().contains(QStringLiteral(":///")));
            QCOMPARE(search->isEnabled(), location.isLocalFile());
        }
    }

    void viewButtonCyclesAndArrowOpensSharedFlyout()
    {
        QTemporaryDir dir;
        FileChooserDialog dialog(optionsFor(dir.path()));
        show(dialog);
        auto *button = dialog.findChild<QToolButton *>(QStringLiteral("chooserViewButton"));
        auto *files = dialog.findChild<FileView *>();
        QVERIFY(button && files);
        QCOMPARE(button->popupMode(), QToolButton::MenuButtonPopup);
        QCOMPARE(button->size(), QSize(44, 24));
        QVERIFY(qobject_cast<ViewModeMenu *>(button->menu()));
        QTest::mouseMove(&dialog, QPoint(1, 1));
        QTest::qWait(10);
        const QImage rendered = button->grab().toImage();
        const qreal dpr = rendered.devicePixelRatio();
        const QRect glyphArea(qRound(31 * dpr), qRound(8 * dpr), qRound(10 * dpr), qRound(8 * dpr));
        int arrowPixels = 0;
        for (int y = glyphArea.top(); y <= glyphArea.bottom(); ++y)
            for (int x = glyphArea.left(); x <= glyphArea.right(); ++x) {
                const QColor color = rendered.pixelColor(x, y);
                if (qMax(color.red(), qMax(color.green(), color.blue())) < 100) ++arrowPixels;
            }
        QVERIFY(arrowPixels >= qRound(5 * dpr * dpr));
        const QString snapshots = qEnvironmentVariable("EXPLORER_SNAPSHOTS");
        if (!snapshots.isEmpty()) {
            QVERIFY(QDir().mkpath(snapshots));
            QVERIFY(rendered.save(snapshots + QStringLiteral("/chooser-view-button-idle.png")));
        }
        QTest::mouseClick(button, Qt::LeftButton, Qt::NoModifier, QPoint(10, 12));
        QCOMPARE(files->viewMode(), Settings::ViewMode::Tiles);
        QVERIFY(!button->menu()->isVisible());
        bool popupVisible = false;
        QTimer::singleShot(0, button->menu(), [&] {
            popupVisible = button->menu()->isVisible();
            auto *choice = button->menu()->findChild<QToolButton *>(QStringLiteral("viewMode7"));
            if (choice) choice->click();
            else button->menu()->close();
        });
        QTest::mouseClick(button, Qt::LeftButton, Qt::NoModifier, QPoint(38, 12));
        QTRY_COMPARE(files->viewMode(), Settings::ViewMode::Content);
        QVERIFY(popupVisible);
        QVERIFY(!button->menu()->isVisible());
        QVERIFY(button->menu()->findChild<QToolButton *>(QStringLiteral("viewMode7"))->isChecked());
        QCOMPARE(button->icon().cacheKey(), ViewModeMenu::iconForMode(Settings::ViewMode::Content).cacheKey());
    }

    void previewDefaultsAndToggleRemainUsableAtMinimumWidth_data()
    {
        QTest::addColumn<int>("mode");
        QTest::addColumn<bool>("directory");
        QTest::newRow("open") << int(ChooserMode::OpenFile) << false;
        QTest::newRow("save") << int(ChooserMode::SaveFile) << false;
        QTest::newRow("folder") << int(ChooserMode::OpenFile) << true;
        QTest::newRow("batch-save") << int(ChooserMode::SaveFiles) << false;
    }

    void previewDefaultsAndToggleRemainUsableAtMinimumWidth()
    {
        QFETCH(int, mode);
        QFETCH(bool, directory);
        QTemporaryDir dir;
        auto options = optionsFor(dir.path(), ChooserMode(mode));
        options.directory = directory;
        if (options.mode == ChooserMode::SaveFiles) options.saveNames = {QStringLiteral("new.txt")};
        FileChooserDialog dialog(options);
        show(dialog);
        auto *preview = dialog.findChild<PreviewPane *>();
        auto *button = dialog.findChild<QToolButton *>(QStringLiteral("chooserPreviewButton"));
        auto *splitter = dialog.findChild<QSplitter *>(QStringLiteral("chooserSplitter"));
        auto *files = dialog.findChild<FileView *>();
        auto *buttons = dialog.findChild<QDialogButtonBox *>();
        QVERIFY(preview && button && splitter && files && buttons);
        QVERIFY(preview->isVisible());
        QVERIFY(button->isChecked());
        QVERIFY(button->toolTip().startsWith(QStringLiteral("Hide")));
        QVERIFY(!dialog.findChild<QToolButton *>(QStringLiteral("helpButton")));
        QCOMPARE(splitter->count(), 3);
        dialog.resize(480, 340);
        QTest::qWait(20);
        QCOMPARE(dialog.width(), 480);
        QVERIFY(files->width() >= 160);
        QVERIFY(preview->width() >= 160);
        QVERIFY(dialog.rect().contains(QRect(buttons->mapTo(&dialog, QPoint()), buttons->size())));
        QVERIFY(name(dialog)->width() >= 100);
        QTest::mouseClick(button, Qt::LeftButton);
        QVERIFY(!preview->isVisible());
        QVERIFY(!button->isChecked());
        QVERIFY(button->toolTip().startsWith(QStringLiteral("Show")));
        QTest::keyClick(&dialog, Qt::Key_P, Qt::AltModifier);
        QTRY_VERIFY(preview->isVisible());
        QVERIFY(button->isChecked());
        QVERIFY(button->toolTip().startsWith(QStringLiteral("Hide")));
    }

    void previewTracksSelectedTextImagesAndClearsOnNavigation()
    {
        QTemporaryDir dir;
        QVERIFY(QDir(dir.path()).mkdir(QStringLiteral("sub")));
        const QUrl first = QUrl::fromLocalFile(dir.filePath(QStringLiteral("first.txt")));
        const QUrl second = QUrl::fromLocalFile(dir.filePath(QStringLiteral("second.txt")));
        const QUrl picture = QUrl::fromLocalFile(dir.filePath(QStringLiteral("picture.png")));
        writeFile(first.toLocalFile(), "First selected contents\n");
        writeFile(second.toLocalFile(), "Second selected contents\n");
        QImage fixture(120, 80, QImage::Format_RGB32);
        fixture.fill(Qt::green);
        QVERIFY(fixture.save(picture.toLocalFile()));
        FileChooserDialog dialog(optionsFor(dir.path()));
        show(dialog);
        auto *model = dialog.findChild<DirectoryModel *>();
        auto *files = dialog.findChild<FileView *>();
        auto *preview = dialog.findChild<PreviewPane *>();
        auto *text = preview->findChild<QPlainTextEdit *>(QStringLiteral("textPreview"));
        auto *image = preview->findChild<QLabel *>(QStringLiteral("previewImage"));
        auto *placeholder = preview->findChild<QLabel *>(QStringLiteral("previewPlaceholder"));
        auto *stack = preview->findChild<QStackedWidget *>();
        QTRY_COMPARE(model->rowCount(), 4);
        auto *view = files->currentView();
        const QModelIndex firstIndex = model->indexForUrl(first);
        view->scrollTo(firstIndex);
        QTRY_VERIFY(!view->visualRect(firstIndex).isEmpty());
        QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, view->visualRect(firstIndex).center());
        QTRY_COMPARE(text->toPlainText(), QStringLiteral("First selected contents\n"));
        QCOMPARE(stack->currentWidget(), text);
        QVERIFY(text->isReadOnly());
        QVERIFY(files->selectUrl(picture));
        QVERIFY(files->selectUrl(second));
        QTRY_COMPARE(text->toPlainText(), QStringLiteral("Second selected contents\n"));
        QTest::qWait(100);
        QCOMPARE(stack->currentWidget(), text);
        QVERIFY(image->pixmap().isNull());
        QVERIFY(files->selectUrl(picture));
        QTRY_COMPARE(stack->currentWidget(), image);
        QVERIFY(!image->pixmap().isNull());
        const QImage actual = image->pixmap().toImage();
        const QColor center = actual.pixelColor(actual.width() / 2, actual.height() / 2);
        QVERIFY(center.green() > 200 && center.red() < 80 && center.blue() < 80);
        const QString snapshots = qEnvironmentVariable("EXPLORER_SNAPSHOTS");
        if (!snapshots.isEmpty()) {
            QVERIFY(QDir().mkpath(snapshots));
            QVERIFY(dialog.grab().save(snapshots + QStringLiteral("/chooser-preview-image.png")));
        }
        files->selectionModel()->clearSelection();
        QCOMPARE(stack->currentWidget(), placeholder);
        QVERIFY(text->toPlainText().isEmpty());
        QVERIFY(image->pixmap().isNull());
        QVERIFY(files->selectUrl(first));
        dialog.navigateTo(QUrl::fromLocalFile(dir.filePath(QStringLiteral("sub"))));
        QTest::qWait(100);
        QCOMPARE(stack->currentWidget(), placeholder);
        QVERIFY(text->toPlainText().isEmpty());
        QVERIFY(image->pixmap().isNull());
        for (const auto &location : {Locations::libraries(), Locations::computer()}) {
            dialog.navigateTo(location);
            QCOMPARE(stack->currentWidget(), placeholder);
        }
    }

    void previewHideCloseCancelPendingContentAndShowResumesSelection()
    {
        QTemporaryDir dir;
        const QUrl first = QUrl::fromLocalFile(dir.filePath(QStringLiteral("first.txt")));
        const QUrl second = QUrl::fromLocalFile(dir.filePath(QStringLiteral("second.txt")));
        writeFile(first.toLocalFile(), "Old text\n");
        writeFile(second.toLocalFile(), "Current text\n");
        FileChooserDialog dialog(optionsFor(dir.path()));
        show(dialog);
        auto *files = dialog.findChild<FileView *>();
        auto *preview = dialog.findChild<PreviewPane *>();
        auto *text = preview->findChild<QPlainTextEdit *>(QStringLiteral("textPreview"));
        auto *placeholder = preview->findChild<QLabel *>(QStringLiteral("previewPlaceholder"));
        auto *stack = preview->findChild<QStackedWidget *>();
        QTRY_COMPARE(dialog.findChild<DirectoryModel *>()->rowCount(), 2);
        QVERIFY(files->selectUrl(first));
        dialog.hide();
        QTest::qWait(100);
        QCOMPARE(stack->currentWidget(), placeholder);
        QVERIFY(text->toPlainText().isEmpty());
        QVERIFY(files->selectUrl(second));
        QTest::qWait(50);
        QVERIFY(text->toPlainText().isEmpty());
        show(dialog);
        QTRY_COMPARE(text->toPlainText(), QStringLiteral("Current text\n"));
        auto *button = dialog.findChild<QToolButton *>(QStringLiteral("chooserPreviewButton"));
        QTest::mouseClick(button, Qt::LeftButton);
        QVERIFY(!preview->isVisible());
        QVERIFY(files->selectUrl(first));
        QTest::qWait(50);
        QVERIFY(text->toPlainText().isEmpty());
        QTest::keyClick(&dialog, Qt::Key_P, Qt::AltModifier);
        QTRY_COMPARE(text->toPlainText(), QStringLiteral("Old text\n"));
        QVERIFY(files->selectUrl(second));
        dialog.close();
        QTRY_VERIFY(!dialog.isVisible());
        QTest::qWait(100);
        QCOMPARE(stack->currentWidget(), placeholder);
        QVERIFY(text->toPlainText().isEmpty());
        QVERIFY(dialog.selectedUrls().isEmpty());
    }

    void focusCycleIncludesVisibleReadOnlyPreviewAndSkipsHiddenPane()
    {
        QTemporaryDir dir;
        const QUrl item = QUrl::fromLocalFile(dir.filePath(QStringLiteral("note.txt")));
        writeFile(item.toLocalFile(), "Preview keyboard contents\n");
        FileChooserDialog dialog(optionsFor(dir.path()));
        show(dialog);
        auto *files = dialog.findChild<FileView *>();
        auto *text = dialog.findChild<QPlainTextEdit *>(QStringLiteral("textPreview"));
        QTRY_COMPARE(dialog.findChild<DirectoryModel *>()->rowCount(), 1);
        QVERIFY(files->selectUrl(item));
        QTRY_VERIFY(text->isVisible());
        files->focusView();
        QTest::keyClick(files->currentView(), Qt::Key_F6);
        QTRY_VERIFY(text->hasFocus());
        QSignalSpy accepted(&dialog, &QDialog::accepted);
        QTest::keyClick(text, Qt::Key_Return);
        QCOMPARE(accepted.count(), 0);
        QVERIFY(dialog.isVisible());
        QTest::keyClick(text, Qt::Key_F6);
        QTRY_VERIFY(name(dialog)->hasFocus());
        QTest::keyClick(name(dialog), Qt::Key_F6, Qt::ShiftModifier);
        QTRY_VERIFY(text->hasFocus());
        QTest::keyClick(text, Qt::Key_P, Qt::AltModifier);
        QTRY_VERIFY(files->currentView()->hasFocus());
        name(dialog)->setFocus();
        QTest::keyClick(name(dialog), Qt::Key_F6, Qt::ShiftModifier);
        QTRY_VERIFY(files->currentView()->hasFocus());
        QTest::keyClick(&dialog, Qt::Key_P, Qt::AltModifier);
        QTRY_VERIFY(text->isVisible());
        files->focusView();
        QTest::keyClick(files->currentView(), Qt::Key_F6);
        QTRY_VERIFY(text->hasFocus());
    }

    void multipleFileSelectionNeverParsesWhitespaceAndTypedIntentWins()
    {
        QTemporaryDir dir;
        const QList<QUrl> urls = {QUrl::fromLocalFile(dir.filePath("a space # %.txt")),
                                 QUrl::fromLocalFile(dir.filePath(QString::fromUtf8("日本語.txt")))};
        for (const auto &url : urls) writeFile(url.toLocalFile());
        auto options = optionsFor(dir.path());
        options.multiple = true;
        FileChooserDialog dialog(options);
        show(dialog);
        auto *model = dialog.findChild<DirectoryModel *>();
        auto *files = dialog.findChild<FileView *>();
        QTRY_COMPARE(model->rowCount(), 2);
        QCOMPARE(files->currentView()->selectionMode(), QAbstractItemView::ExtendedSelection);
        QVERIFY(files->selectUrls(urls).isEmpty());
        QCOMPARE(model->itemsForIndexes(files->selectedIndexes()).size(), 2);
        files->setViewMode(Settings::ViewMode::LargeIcons);
        QCOMPARE(files->currentView()->selectionMode(), QAbstractItemView::ExtendedSelection);
        QCOMPARE(model->itemsForIndexes(files->selectedIndexes()).size(), 2);
        files->focusView();
        QTest::keyClick(files->currentView(), Qt::Key_Return);
        QCOMPARE(dialog.selectedUrls().size(), 2);
        for (const auto &url : urls) QVERIFY(dialog.selectedUrls().contains(url));

        FileChooserDialog typed(options);
        show(typed);
        auto *typedFiles = typed.findChild<FileView *>();
        QTRY_COMPARE(typed.findChild<DirectoryModel *>()->rowCount(), 2);
        QVERIFY(typedFiles->selectUrls(urls).isEmpty());
        name(typed)->setText(QString::fromUtf8(urls.first().toEncoded()));
        typed.accept();
        QCOMPARE(typed.selectedUrls(), QList<QUrl>{urls.first()});
    }

    void mixedFileFolderSelectionAndStaleNameStayOpen()
    {
        QTemporaryDir dir;
        QVERIFY(QDir(dir.path()).mkdir("folder"));
        writeFile(dir.filePath("file.txt"));
        auto options = optionsFor(dir.path());
        options.multiple = true;
        options.suggestedName = "file.txt";
        FileChooserDialog dialog(options);
        show(dialog);
        auto *files = dialog.findChild<FileView *>();
        QTRY_COMPARE(dialog.findChild<DirectoryModel *>()->rowCount(), 2);
        const QList<QUrl> urls = {QUrl::fromLocalFile(dir.filePath("file.txt")),
                                 QUrl::fromLocalFile(dir.filePath("folder"))};
        QVERIFY(files->selectUrls(urls).isEmpty());
        dialog.accept();
        QVERIFY(dialog.selectedUrls().isEmpty());
        QVERIFY(!dialog.errorMessage().isEmpty());
        QVERIFY(dialog.isVisible());
        QVERIFY(files->selectUrl(urls.first()));
        QCOMPARE(name(dialog)->text(), QStringLiteral("file.txt"));
        files->selectionModel()->clearSelection();
        QVERIFY(name(dialog)->text().isEmpty());
        dialog.accept();
        QVERIFY(dialog.selectedUrls().isEmpty());
        QVERIFY(dialog.isVisible());
    }

    void folderSelectionAndNavigation_data()
    {
        QTest::addColumn<bool>("multiple");
        QTest::newRow("single") << false;
        QTest::newRow("multiple") << true;
    }

    void folderSelectionAndNavigation()
    {
        QFETCH(bool, multiple);
        QTemporaryDir dir;
        QVERIFY(QDir(dir.path()).mkdir("a space # %"));
        QVERIFY(QDir(dir.path()).mkdir(QString::fromUtf8("日本語")));
        writeFile(dir.filePath("not-a-folder.txt"));
        auto options = optionsFor(dir.path());
        options.directory = true;
        options.multiple = multiple;
        options.filters = {{"text", "Text", {"*.txt"}, {}}};
        FileChooserDialog dialog(options);
        show(dialog);
        auto *model = dialog.findChild<DirectoryModel *>();
        auto *files = dialog.findChild<FileView *>();
        QTRY_COMPARE(model->rowCount(), 2);
        const QUrl first = QUrl::fromLocalFile(dir.filePath("a space # %"));
        const QUrl second = QUrl::fromLocalFile(dir.filePath(QString::fromUtf8("日本語")));
        QVERIFY(files->selectUrl(first));
        auto *view = files->currentView();
        const QModelIndex index = model->indexForUrl(first);
        QTRY_VERIFY(!view->visualRect(index).isEmpty());
        QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, view->visualRect(index).center());
        QTest::mouseDClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, view->visualRect(index).center());
        QTRY_COMPARE(dialog.currentUrl(), first);
        QVERIFY(dialog.selectedUrls().isEmpty());
        dialog.navigateTo(options.startingLocation);
        QTRY_COMPARE(model->rowCount(), 2);
        QVERIFY(files->selectUrl(first));
        files->focusView();
        QTest::keyClick(files->currentView(), Qt::Key_Return);
        QCOMPARE(dialog.currentUrl(), first);
        QVERIFY(dialog.selectedUrls().isEmpty());
        dialog.navigateTo(options.startingLocation);
        QTRY_COMPARE(model->rowCount(), 2);
        const QList<QUrl> selected = multiple ? QList<QUrl>{first, second} : QList<QUrl>{first};
        QVERIFY(files->selectUrls(selected).isEmpty());
        dialog.accept();
        QCOMPARE(dialog.selectedUrls().size(), selected.size());
        for (const auto &url : selected) QVERIFY(dialog.selectedUrls().contains(url));

        FileChooserDialog current(options);
        show(current);
        current.accept();
        QCOMPARE(current.selectedUrls(), QList<QUrl>{options.startingLocation});

        FileChooserDialog typed(options);
        show(typed);
        name(typed)->setText(QString::fromUtf8(first.toEncoded()));
        typed.accept();
        QCOMPARE(typed.selectedUrls(), QList<QUrl>{first});
    }

    void folderModeVirtualLocationsRequireRealBackingFolders()
    {
        QTemporaryDir dir;
        auto options = optionsFor(dir.path());
        options.directory = true;
        FileChooserDialog dialog(options);
        show(dialog);
        for (const auto &location : {Locations::libraries(), Locations::computer()}) {
            dialog.navigateTo(location);
            dialog.accept();
            QCOMPARE(dialog.currentUrl(), location);
            QVERIFY(dialog.selectedUrls().isEmpty());
            QVERIFY(!dialog.errorMessage().isEmpty());
        }
        for (const auto &entry : Libraries::entries()) {
            QVERIFY(QDir().mkpath(entry.url.toLocalFile()));
            dialog.navigateTo(entry.url);
            QCOMPARE(dialog.currentUrl(), entry.url);
        }
        const auto last = dialog.currentUrl();
        dialog.accept();
        QCOMPARE(dialog.selectedUrls(), QList<QUrl>{last});
    }

    void choicesAreNativeAccessibleAndFooterIsBounded()
    {
        QTemporaryDir dir;
        auto options = optionsFor(dir.path());
        options.acceptLabel = "&Choose && keep";
        options.choices = {{"check", "&Reencode", {}, ""},
                           {"encoding", "&Encoding:", {{"utf8", "Unicode"}, {"latin", "Western"}}, ""},
                           {"enabled", "Enabled", {}, "true"},
                           {"selected", "Selected:", {{"a", "A"}, {"b", "B"}}, "b"}};
        for (int i = 0; i < 15; ++i)
            options.choices.append({QString::number(i), QStringLiteral("Extra %1").arg(i), {}, ""});
        FileChooserDialog dialog(options);
        show(dialog);
        QCOMPARE(dialog.selectedChoices().value("check"), QStringLiteral("false"));
        QCOMPARE(dialog.selectedChoices().value("encoding"), QStringLiteral("utf8"));
        QCOMPARE(dialog.selectedChoices().value("enabled"), QStringLiteral("true"));
        QCOMPARE(dialog.selectedChoices().value("selected"), QStringLiteral("b"));
        auto *scroll = dialog.findChild<QScrollArea *>("chooserChoices");
        QVERIFY(scroll);
        QVERIFY(scroll->height() <= 110);
        QVERIFY(scroll->verticalScrollBar()->maximum() > 0);
        QComboBox *encoding = nullptr;
        for (auto *combo : scroll->findChildren<QComboBox *>())
            if (combo->accessibleName() == QStringLiteral("&Encoding:")) encoding = combo;
        QVERIFY(encoding);
        QVERIFY(encoding->property("chooserAeroCombo").toBool());
        QVERIFY(encoding->focusPolicy() != Qt::NoFocus);
        bool buddy = false;
        for (auto *label : scroll->findChildren<QLabel *>())
            if (label->buddy() == encoding) buddy = true;
        QVERIFY(buddy);
        auto *iface = QAccessible::queryAccessibleInterface(encoding);
        QVERIFY(iface);
        QVERIFY(!iface->text(QAccessible::Name).isEmpty());
        scroll->ensureWidgetVisible(encoding);
        encoding->setFocus();
        QTest::keyClick(encoding, Qt::Key_Down, Qt::AltModifier);
        QTRY_VERIFY(encoding->view()->isVisible());
        QTest::keyClick(encoding->view(), Qt::Key_Down);
        QTest::keyClick(encoding->view(), Qt::Key_Return);
        QTRY_COMPARE(dialog.selectedChoices().value("encoding"), QStringLiteral("latin"));
        auto *check = scroll->findChild<QCheckBox *>();
        QVERIFY(check);
        QCOMPARE(check->palette().color(QPalette::WindowText), QColor(Qt::black));
        scroll->ensureWidgetVisible(check);
        check->setFocus();
        QTest::keyClick(check, Qt::Key_Space);
        QCOMPARE(dialog.selectedChoices().value("check"), QStringLiteral("true"));
        auto *buttons = dialog.findChild<QDialogButtonBox *>();
        bool acceptLabel = false;
        for (auto *button : buttons->buttons())
            if (buttons->buttonRole(button) == QDialogButtonBox::AcceptRole)
                acceptLabel = button->text() == options.acceptLabel;
        QVERIFY(acceptLabel);
        for (const QSize size : {QSize(760, 510), QSize(540, 400), QSize(480, 340)}) {
            dialog.resize(size);
            QTest::qWait(20);
            QVERIFY(dialog.rect().contains(QRect(buttons->mapTo(&dialog, QPoint()), buttons->size())));
            QVERIFY(name(dialog)->width() >= 100);
            QVERIFY(encoding->width() >= 100);
            QVERIFY(scroll->height() <= 110);
        }
    }

    void saveFilesPreservesOrderUniquifiesAndNeverWrites()
    {
        QTemporaryDir dir;
        writeFile(dir.filePath("same.txt"), "unchanged");
        QVERIFY(QDir(dir.path()).mkdir("same (1).txt"));
        QVERIFY(QFile::link(dir.filePath("missing"), dir.filePath("same (2).txt")));
        auto options = optionsFor(dir.path(), ChooserMode::SaveFiles);
        options.saveNames = {"same.txt", "same.txt", "same (3).txt", "space # %.txt", ".hidden", ".hidden"};
        FileChooserDialog dialog(options);
        show(dialog);
        dialog.accept();
        const QStringList expected = {"same (3).txt", "same (4).txt", "same (3) (1).txt", "space # %.txt", ".hidden", ".hidden (1)"};
        QCOMPARE(dialog.selectedUrls().size(), expected.size());
        for (int i = 0; i < expected.size(); ++i) {
            QCOMPARE(dialog.selectedUrls()[i], QUrl::fromLocalFile(dir.filePath(expected[i])));
            QVERIFY(!QFileInfo::exists(dir.filePath(expected[i])));
        }
        QFile original(dir.filePath("same.txt"));
        QVERIFY(original.open(QIODevice::ReadOnly));
        QCOMPARE(original.readAll(), QByteArray("unchanged"));
        QCOMPARE(QDir(dir.path()).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot).size(), 3);
    }

    void saveFilesRejectsInvalidNamesAndPermissions_data()
    {
        QTest::addColumn<QString>("leaf");
        QTest::newRow("empty") << QString();
        QTest::newRow("dot") << QStringLiteral(".");
        QTest::newRow("dotdot") << QStringLiteral("..");
        QTest::newRow("path") << QStringLiteral("a/b");
        QTest::newRow("nul") << QString(QChar::Null);
        QTest::newRow("long") << QString(300, QLatin1Char('a'));
        QTest::newRow("unicode-byte-limit") << QString(100, QChar(0x65e5));
    }

    void saveFilesRejectsInvalidNamesAndPermissions()
    {
        QFETCH(QString, leaf);
        QTemporaryDir dir;
        auto options = optionsFor(dir.path(), ChooserMode::SaveFiles);
        options.saveNames = {"valid.txt", leaf};
        FileChooserDialog dialog(options);
        show(dialog);
        dialog.accept();
        QVERIFY(dialog.selectedUrls().isEmpty());
        QVERIFY(dialog.isVisible());
        QVERIFY(!dialog.errorMessage().isEmpty());
        QVERIFY(QDir(dir.path()).entryList(QDir::Files | QDir::Hidden).isEmpty());
    }

    void saveFilesSelectedFolderAndUnwritableParent()
    {
        QTemporaryDir dir;
        QVERIFY(QDir(dir.path()).mkdir("dest"));
        const QString dest = dir.filePath("dest");
        auto options = optionsFor(dir.path(), ChooserMode::SaveFiles);
        options.saveNames = {"exact", "literal\\name"};
        FileChooserDialog dialog(options);
        show(dialog);
        auto *model = dialog.findChild<DirectoryModel *>();
        const QUrl folder = QUrl::fromLocalFile(dest);
        QTRY_VERIFY(model->indexForUrl(folder).isValid());
        QVERIFY(dialog.findChild<FileView *>()->selectUrl(folder));
        QVERIFY(QFile::setPermissions(dest, QFile::ReadOwner | QFile::ExeOwner));
        dialog.accept();
        QVERIFY(dialog.selectedUrls().isEmpty());
        QVERIFY(!dialog.errorMessage().isEmpty());
        QVERIFY(QFile::setPermissions(dest, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        dialog.accept();
        QCOMPARE(dialog.selectedUrls(), (QList<QUrl>{QUrl::fromLocalFile(dest + "/exact"),
                                                    QUrl::fromLocalFile(dest + "/literal\\name")}));
        QVERIFY(QDir(dest).entryList(QDir::Files | QDir::Hidden).isEmpty());
    }

    void saveFilesCollisionSearchIsBoundedAndCannotExceedByteLimit()
    {
        QTemporaryDir dir;
        auto options = optionsFor(dir.path(), ChooserMode::SaveFiles);
        const QString longName(255, QLatin1Char('a'));
        writeFile(dir.filePath(longName));
        options.saveNames = {longName};
        FileChooserDialog tooLong(options);
        show(tooLong);
        tooLong.accept();
        QVERIFY(tooLong.selectedUrls().isEmpty());
        QVERIFY(!tooLong.errorMessage().isEmpty());

        writeFile(dir.filePath("occupied.txt"));
        for (int i = 1; i <= 10000; ++i) {
            QFile file(dir.filePath(QStringLiteral("occupied (%1).txt").arg(i)));
            QVERIFY(file.open(QIODevice::WriteOnly));
        }
        options.saveNames = {"occupied.txt"};
        FileChooserDialog bounded(options);
        show(bounded);
        bounded.accept();
        QVERIFY(bounded.selectedUrls().isEmpty());
        QVERIFY(!bounded.errorMessage().isEmpty());
        QVERIFY(!QFileInfo::exists(dir.filePath("occupied (10001).txt")));
    }

    void modeFootersFitNarrowWindow_data()
    {
        QTest::addColumn<int>("mode");
        QTest::newRow("multi-file") << 0;
        QTest::newRow("multi-folder") << 1;
        QTest::newRow("save-files") << 2;
    }

    void modeFootersFitNarrowWindow()
    {
        QFETCH(int, mode);
        QTemporaryDir dir;
        auto options = optionsFor(dir.path(), mode == 2 ? ChooserMode::SaveFiles : ChooserMode::OpenFile);
        options.multiple = mode < 2;
        options.directory = mode == 1;
        options.saveNames = {"exact.txt"};
        options.choices = {{"check", "&Enable", {}, ""},
                           {"format", "&Format:", {{"one", "First"}, {"two", "Second"}}, ""}};
        FileChooserDialog dialog(options);
        show(dialog);
        for (const QSize size : {QSize(760, 510), QSize(540, 400), QSize(480, 340)}) {
            dialog.resize(size);
            QTest::qWait(20);
            auto *buttons = dialog.findChild<QDialogButtonBox *>();
            QVERIFY(dialog.rect().contains(QRect(buttons->mapTo(&dialog, QPoint()), buttons->size())));
            QVERIFY(name(dialog)->width() >= 100);
            const QString snapshots = qEnvironmentVariable("EXPLORER_SNAPSHOTS");
            if (!snapshots.isEmpty()) {
                QVERIFY(QDir().mkpath(snapshots));
                QVERIFY(dialog.grab().save(snapshots + QStringLiteral("/chooser-mode-%1-%2x%3.png")
                    .arg(mode).arg(size.width()).arg(size.height())));
            }
        }
    }

    void portalSensitiveGlobsAndMimeFamilyAreOptIn()
    {
        QTemporaryDir dir;
        writeFile(dir.filePath("lower.txt"));
        writeFile(dir.filePath("upper.TxT"));
        writeFile(dir.filePath("sound.wav"), QByteArray::fromHex("524946462400000057415645666d74201000000001000100401f0000803e0000020010006461746100000000"));
        QVERIFY(QDir(dir.path()).mkdir("folder"));
        auto options = optionsFor(dir.path());
        options.filterCaseSensitivity = Qt::CaseSensitive;
        options.filters = {{"portal", "Text or Audio", {"*.txt"}, {"audio/*"}}};
        FileChooserDialog dialog(options);
        show(dialog);
        auto *model = dialog.findChild<DirectoryModel *>();
        QTRY_COMPARE(model->rowCount(), 3);
        QVERIFY(model->indexForUrl(QUrl::fromLocalFile(dir.filePath("lower.txt"))).isValid());
        QVERIFY(!model->indexForUrl(QUrl::fromLocalFile(dir.filePath("upper.TxT"))).isValid());
        QVERIFY(model->indexForUrl(QUrl::fromLocalFile(dir.filePath("sound.wav"))).isValid());
        options.filterCaseSensitivity = Qt::CaseInsensitive;
        FileChooserDialog insensitive(options);
        show(insensitive);
        QTRY_COMPARE(insensitive.findChild<DirectoryModel *>()->rowCount(), 4);
    }

    void geometryAndNativeDialogIdentity()
    {
        QTemporaryDir dir;
        const QString original = QGuiApplication::desktopFileName();
        QGuiApplication::setDesktopFileName(QStringLiteral("linux-explorer-win7-dialog"));
        FileChooserDialog dialog(optionsFor(dir.path()));
        show(dialog);
        QVERIFY(!(dialog.windowFlags() & Qt::FramelessWindowHint));
        QVERIFY(!(dialog.windowFlags() & Qt::WindowStaysOnTopHint));
        auto *placesView = dialog.findChild<NavigationPane *>()->findChild<QAbstractItemView *>();
        QCOMPARE(placesView->palette().color(QPalette::Text), QColor(Qt::black));
        QCOMPARE(dialog.findChild<SearchBox *>()->objectName(), QStringLiteral("searchBox"));
        QCOMPARE(QGuiApplication::desktopFileName(), QStringLiteral("linux-explorer-win7-dialog"));
        const auto expectedScale = qEnvironmentVariable("EXPLORER_EXPECT_SCALE");
        if (!expectedScale.isEmpty())
            QVERIFY(qAbs(dialog.devicePixelRatioF() - expectedScale.toDouble()) < 0.02);
        const QString expectedDisplay = qEnvironmentVariable("EXPLORER_EXPECT_DISPLAY");
        if (!expectedDisplay.isEmpty()) {
            const QStringList dimensions = expectedDisplay.split('x');
            const QScreen *screen = QGuiApplication::primaryScreen();
            QCOMPARE(qRound(screen->geometry().width() * screen->devicePixelRatio()), dimensions[0].toInt());
            QCOMPARE(qRound(screen->geometry().height() * screen->devicePixelRatio()), dimensions[1].toInt());
        }
        for (const QSize size : {QSize(760, 510), QSize(540, 400), QSize(480, 340)}) {
            dialog.resize(size);
            QTest::qWait(20);
            auto *editor = name(dialog);
            auto *filters = dialog.findChild<QComboBox *>();
            auto *buttons = dialog.findChild<QDialogButtonBox *>();
            QVERIFY(editor->width() >= 100);
            QVERIFY(filters->width() >= 100);
            QVERIFY(editor->height() >= editor->fontMetrics().height());
            QVERIFY(dialog.rect().contains(QRect(editor->mapTo(&dialog, QPoint()), editor->size())));
            QVERIFY(dialog.rect().contains(QRect(buttons->mapTo(&dialog, QPoint()), buttons->size())));
            QVERIFY(editor->mapTo(&dialog, editor->rect().bottomLeft()).y()
                < filters->mapTo(&dialog, QPoint()).y());
            auto *nav = dialog.findChild<BrowserNavigation *>();
            nav->focusPathEdit();
            auto *path = nav->pathEditor();
            QVERIFY(path->width() >= 100);
            QVERIFY(path->geometry().height() >= path->fontMetrics().height());
            nav->endPathEdit();
            const QString snapshots = qEnvironmentVariable("EXPLORER_SNAPSHOTS");
            if (!snapshots.isEmpty()) {
                QVERIFY(QDir().mkpath(snapshots));
                QTest::qWait(10);
                QVERIFY(dialog.grab().save(snapshots + QStringLiteral("/chooser-%1x%2.png")
                    .arg(size.width()).arg(size.height())));
            }
        }
        QGuiApplication::setDesktopFileName(original);
    }
};

QTEST_MAIN(FileChooserTest)
#include "FileChooserTest.moc"
