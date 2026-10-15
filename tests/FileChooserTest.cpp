#include "FileChooserDialog.h"
#include "BrowserNavigation.h"
#include "DirectoryModel.h"
#include "FileView.h"
#include "Libraries.h"
#include "Locations.h"
#include "NavigationPane.h"
#include "SearchBox.h"

#include <AeroQt/stylesheet.h>
#include <QAbstractItemView>
#include <QApplication>
#include <QComboBox>
#include <QCompleter>
#include <QStringListModel>
#include <QContextMenuEvent>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>

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
        writeFile(script, QStringLiteral("#!/bin/sh\ntouch '%1'\n").arg(marker).toUtf8());
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
        QTest::newRow("literal") << QStringLiteral("sp ace # 100% 日本語.txt") << false;
        QTest::newRow("uri") << QStringLiteral("sp ace # 100% 日本語.txt") << true;
        QTest::newRow("literal-percent-escape") << QStringLiteral("a%20b.txt") << false;
        QTest::newRow("leading-spaces") << QStringLiteral("  real name  ") << false;
    }

    void typedAndEncodedNamesRoundTrip()
    {
        QFETCH(QString, leaf);
        QFETCH(bool, encoded);
        QTemporaryDir dir;
        const QString path = dir.filePath(leaf);
        writeFile(path);
        auto options = optionsFor(dir.path());
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
    }

    void saveExtensionsAndNoWrites_data()
    {
        QTest::addColumn<QString>("typed");
        QTest::addColumn<QString>("extension");
        QTest::addColumn<QString>("expected");
        QTest::newRow("infer") << "draft" << "" << "draft.txt";
        QTest::newRow("explicit-default") << "draft" << "md" << "draft.md";
        QTest::newRow("keep-suffix") << "draft.bin" << "txt" << "draft.bin";
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

    void addressTypedFileAcceptsExactlyOnceWithoutLaunching()
    {
        QTemporaryDir dir;
        const QString script = dir.filePath(QStringLiteral("file # 100% 日本語.sh"));
        const QString marker = dir.filePath(QStringLiteral("launched"));
        writeFile(script, QStringLiteral("#!/bin/sh\ntouch '%1'\n").arg(marker).toUtf8());
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
        if (kind == 1) options.multiple = true;
        if (kind == 2) options.directory = true;
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
