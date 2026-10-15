#include "FileChooserDialog.h"
#include "BrowserNavigation.h"
#include "ComputerModel.h"
#include "ComputerView.h"
#include "DirectoryModel.h"
#include "FileView.h"
#include "Libraries.h"
#include "LibrariesView.h"
#include "Locations.h"
#include "NavigationPane.h"
#include "SearchBox.h"
#include "ViewModeMenu.h"
#include "Assets.h"
#include "aero/icons.h"
#include "aero/artwork.h"
#include "aero/palette.h"
#include "aero/text.h"
#include "aero/strips.h"

#include <QAbstractItemView>
#include <QActionGroup>
#include <QApplication>
#include <QComboBox>
#include <QCompleter>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>
#include <QRegularExpression>
#include <QShortcut>
#include <QSplitter>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <climits>
#include <unistd.h>

namespace {
bool readableDirectory(const QFileInfo &info)
{
    const auto permissions = info.permissions();
    return info.isDir() && info.isReadable() && info.isExecutable()
        && (permissions & (QFile::ReadOwner | QFile::ReadGroup | QFile::ReadOther))
        && (permissions & (QFile::ExeOwner | QFile::ExeGroup | QFile::ExeOther));
}

bool readableFile(const QFileInfo &info)
{
    return info.isReadable()
        && (info.permissions() & (QFile::ReadOwner | QFile::ReadGroup | QFile::ReadOther));
}

bool writable(const QFileInfo &info)
{
    return info.isWritable()
        && (info.permissions() & (QFile::WriteOwner | QFile::WriteGroup | QFile::WriteOther));
}

bool hasControl(const QString &text)
{
    for (const QChar character : text)
        if (character.category() == QChar::Other_Control) return true;
    return false;
}

bool localFileUrl(const QUrl &url)
{
    return url.isValid() && url.isLocalFile() && url.host().isEmpty()
        && !url.hasQuery() && !url.hasFragment()
        && QDir::isAbsolutePath(url.toLocalFile()) && !url.toLocalFile().contains(QChar::Null);
}
}

FileChooserDialog::FileChooserDialog(const ChooserOptions &options, QWidget *parent)
    : QDialog(parent), m_options(options)
{
    setObjectName(QStringLiteral("fileChooserDialog"));
    QPalette colors = palette();
    colors.setColor(QPalette::Window, Aero::Palette::rgb(Aero::Palette::FooterSurface));
    colors.setColor(QPalette::Base, Qt::white);
    colors.setColor(QPalette::AlternateBase, QColor(QStringLiteral("#F7F7F7")));
    colors.setColor(QPalette::Text, Qt::black);
    colors.setColor(QPalette::WindowText, Qt::black);
    colors.setColor(QPalette::Button, Aero::Palette::rgb(Aero::Palette::FooterSurface));
    colors.setColor(QPalette::ButtonText, Qt::black);
    colors.setColor(QPalette::PlaceholderText, Aero::Palette::rgb(Aero::Palette::MutedText));
    setPalette(colors);
    Aero::setPointSize(this, 9);
    // Shared panes already paint white surfaces. Keep their text readable even
    // when the application's installed widget theme supplies dark text roles.
    setStyleSheet(QStringLiteral(
        "QDialog#fileChooserDialog { background: #F0F0F0; color: black; }"
        "QDialog#fileChooserDialog QLabel, QDialog#fileChooserDialog QToolButton { color: black; }"
        "QDialog#fileChooserDialog QAbstractItemView { background: white; color: black;"
        " selection-background-color: #CCE8FF; selection-color: black; }"
        "QDialog#fileChooserDialog QHeaderView::section { background: #F7F7F7; color: black; }"
        "QDialog#fileChooserDialog QLineEdit#chooserFileName,"
        "QDialog#fileChooserDialog QComboBox#chooserFileType { background: white; color: black;"
        " border-image: none; border: 1px solid #7F9DB9; padding: 2px; }"));
    setWindowTitle(options.title.isEmpty()
        ? (options.mode == ChooserMode::SaveFile ? tr("Save As") : tr("Open")) : options.title);
    setSizeGripEnabled(true);
    resize(760, 510);
    setMinimumSize(480, 340);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    m_places = new NavigationPane;
    m_model = new DirectoryModel(this);
    m_computerModel = new ComputerModel(m_places->placesModel(), this);
    m_files = new FileView(m_model);
    m_files->setObjectName(QStringLiteral("chooserFiles"));
    m_files->setChooserMode(true);
    m_files->setSingleClickToOpen(false);
    m_libraries = new LibrariesView;
    m_computer = new ComputerView(m_computerModel);
    m_libraries->setViewMode(Settings::ViewMode::Details);
    m_computer->setViewMode(Settings::ViewMode::Details);

    auto *navBar = new QWidget;
    navBar->setObjectName(QStringLiteral("chooserNavigation"));
    navBar->setStyleSheet(Aero::panelSheet(navBar->objectName(), Aero::Palette::StripSurface));
    auto *navRow = new QHBoxLayout(navBar);
    navRow->setContentsMargins(4, 3, 6, 3);
    navRow->setSpacing(2);
    m_navigation = new BrowserNavigation(m_places->placesModel(), m_computerModel, navRow, this);
    m_navigation->setUrlPolicy([this](const QUrl &url) { return locationError(url); });
    m_navigation->setShowHiddenFilesProvider([this] { return m_model->showHiddenFiles(); });
    m_navigation->installShortcuts(this);
    navRow->addSpacing(6);
    m_search = new SearchBox;
    m_search->setAccessibleName(tr("Search current folder"));
    m_search->setPlaceholderText(tr("Search"));
    m_search->addAction(Aero::themeIcon({"gtk-search", "system-search"}), QLineEdit::TrailingPosition);
    m_search->setMinimumWidth(90);
    m_search->setMaximumWidth(220);
    navRow->addWidget(m_search);
    layout->addWidget(navBar);

    auto *commandBar = Aero::commandBar(Explorer::Art::CommandBar);
    commandBar->row()->addWidget(Aero::bodyLabel(tr("Select a file")));
    commandBar->row()->addStretch();
    auto *modes = new QActionGroup(this);
    const QList<QPair<Settings::ViewMode, QString>> viewModes = {
        {Settings::ViewMode::ExtraLargeIcons, tr("Extra large icons")},
        {Settings::ViewMode::LargeIcons, tr("Large icons")},
        {Settings::ViewMode::MediumIcons, tr("Medium icons")},
        {Settings::ViewMode::SmallIcons, tr("Small icons")},
        {Settings::ViewMode::List, tr("List")},
        {Settings::ViewMode::Details, tr("Details")},
        {Settings::ViewMode::Tiles, tr("Tiles")},
        {Settings::ViewMode::Content, tr("Content")}
    };
    for (const auto &mode : viewModes) {
        auto *action = new QAction(ViewModeMenu::iconForMode(mode.first), mode.second, modes);
        action->setCheckable(true);
        action->setData(static_cast<int>(mode.first));
        action->setChecked(mode.first == Settings::ViewMode::Details);
        connect(action, &QAction::triggered, this, [this, mode] {
            m_files->setViewMode(mode.first);
            m_libraries->setViewMode(mode.first);
            m_computer->setViewMode(mode.first);
            restrictViews(m_libraries);
            restrictViews(m_computer);
        });
    }
    auto *viewButton = new QToolButton;
    viewButton->setObjectName(QStringLiteral("chooserViewButton"));
    viewButton->setText(tr("Change your view"));
    viewButton->setAccessibleName(viewButton->text());
    viewButton->setIcon(ViewModeMenu::iconForMode(Settings::ViewMode::Details));
    viewButton->setPopupMode(QToolButton::InstantPopup);
    viewButton->setMenu(new ViewModeMenu(modes, viewButton));
    connect(m_files, &FileView::viewModeChanged, viewButton, [viewButton](Settings::ViewMode mode) {
        viewButton->setIcon(ViewModeMenu::iconForMode(mode));
    });
    commandBar->row()->addWidget(viewButton);
    layout->addWidget(commandBar);

    auto *splitter = new QSplitter;
    splitter->setChildrenCollapsible(false);
    splitter->addWidget(m_places);
    m_stack = new QStackedWidget;
    m_stack->addWidget(m_files);
    m_stack->addWidget(m_libraries);
    m_stack->addWidget(m_computer);
    splitter->addWidget(m_stack);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({170, 590});
    layout->addWidget(splitter, 1);

    auto *bottom = new QWidget;
    bottom->setObjectName(QStringLiteral("chooserFooter"));
    bottom->setStyleSheet(Aero::panelSheet(bottom->objectName(), Aero::Palette::FooterSurface));
    auto *grid = new QGridLayout(bottom);
    grid->setContentsMargins(12, 10, 12, 12);
    grid->setColumnStretch(1, 1);
    m_name = new QLineEdit(options.suggestedName);
    m_name->setObjectName(QStringLiteral("chooserFileName"));
    auto *nameLabel = new QLabel(tr("File &name:"));
    nameLabel->setBuddy(m_name);
    m_name->setAccessibleName(tr("File name"));
    m_filters = new QComboBox;
    m_filters->setObjectName(QStringLiteral("chooserFileType"));
    auto *typeLabel = new QLabel(tr("Files of &type:"));
    typeLabel->setBuddy(m_filters);
    m_filters->setAccessibleName(tr("Files of type"));
    if (options.filters.isEmpty())
        m_filters->addItem(tr("All files (*.*)"), QString());
    for (const auto &filter : options.filters)
        m_filters->addItem(filter.label, filter.id);
    const int selected = m_filters->findData(options.selectedFilter);
    if (selected >= 0)
        m_filters->setCurrentIndex(selected);
    m_buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    m_buttons->setObjectName(QStringLiteral("chooserButtons"));
    m_buttons->setOrientation(Qt::Vertical);
    auto *choose = m_buttons->addButton(options.mode == ChooserMode::SaveFile ? tr("&Save") : tr("&Open"),
                                        QDialogButtonBox::AcceptRole);
    choose->setDefault(true);
    grid->addWidget(nameLabel, 0, 0);
    grid->addWidget(m_name, 0, 1);
    grid->addWidget(typeLabel, 1, 0);
    grid->addWidget(m_filters, 1, 1);
    grid->addWidget(m_buttons, 0, 2, 2, 1);
    m_error = new QLabel;
    m_error->setObjectName(QStringLiteral("chooserError"));
    m_error->setAccessibleName(tr("File selection error"));
    m_error->setWordWrap(true);
    m_error->setTextFormat(Qt::PlainText);
    m_error->setStyleSheet(QStringLiteral("color: #9c1919;"));
    m_error->hide();
    grid->addWidget(m_error, 2, 0, 1, 3);
    layout->addWidget(bottom);

    m_loadingDelay = new QTimer(this);
    m_loadingDelay->setSingleShot(true);
    m_loadingDelay->setInterval(250);
    connect(m_loadingDelay, &QTimer::timeout, m_navigation, &BrowserNavigation::showPathProgress);
    connect(m_model, &DirectoryModel::loadingStarted, this, [this] {
        m_navigation->startPathProgress();
        m_loadingDelay->start();
    });
    connect(m_model, &DirectoryModel::loadingProgress, m_navigation, &BrowserNavigation::setPathProgress);
    connect(m_model, &DirectoryModel::loadingFinished, this, [this] {
        m_loadingDelay->stop();
        m_navigation->finishPathProgress();
    });
    connect(m_model, &DirectoryModel::errorOccurred, this, [this](int, const QString &message, const QUrl &) {
        m_loadingDelay->stop();
        m_navigation->stopPathProgress();
        showError(message);
    });
    connect(m_navigation, &BrowserNavigation::locationChanged, this, &FileChooserDialog::showLocation);
    connect(m_navigation, &BrowserNavigation::errorOccurred, this, &FileChooserDialog::showError);
    connect(m_navigation, &BrowserNavigation::typedFileActivated, this, &FileChooserDialog::activateUrl);
    connect(m_navigation, &BrowserNavigation::refreshRequested, this, [this] {
        const QString error = locationError(currentUrl());
        if (!error.isEmpty()) { showError(error); return; }
        showError({});
        if (currentUrl() == Locations::libraries()) m_libraries->refresh();
        else if (currentUrl() == Locations::computer()) m_computerModel->refresh();
        else m_model->refresh();
    });
    connect(m_places, &NavigationPane::urlActivated, this, &FileChooserDialog::navigateTo);
    connect(m_libraries, &LibrariesView::urlActivated, this, &FileChooserDialog::navigateTo);
    connect(m_computer, &ComputerView::urlActivated, this, &FileChooserDialog::navigateTo);
    connect(m_files, &FileView::activated, this, [this](const QModelIndex &index) {
        activateUrl(m_model->itemForIndex(index).url());
    });
    connect(m_files, &FileView::selectionChanged, this, [this] {
        const auto items = m_model->itemsForIndexes(m_files->selectedIndexes());
        m_selectedDirectory = QUrl();
        if (items.size() == 1) {
            if (items.first().isDir()) {
                m_selectedDirectory = items.first().url();
            } else {
                // Real leaf name, never the friendly display text or hidden extension.
                m_name->setText(QFileInfo(items.first().url().toLocalFile()).fileName());
            }
        }
    });
    connect(m_name, &QLineEdit::textChanged, this, [this] { m_selectedDirectory = QUrl(); });
    connect(m_search, &QLineEdit::textChanged, m_model, &DirectoryModel::setNameFilter);
    connect(m_filters, &QComboBox::currentIndexChanged, this, &FileChooserDialog::applyFilter);
    connect(m_buttons, &QDialogButtonBox::accepted, this, &FileChooserDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &FileChooserDialog::reject);

    for (const auto key : {QKeySequence(Qt::CTRL | Qt::Key_E),
                           QKeySequence(Qt::CTRL | Qt::Key_F), QKeySequence(Qt::Key_F3)}) {
        auto *shortcut = new QShortcut(key, this);
        connect(shortcut, &QShortcut::activated, this, [this] {
            if (!m_search->isEnabled()) return;
            m_navigation->endPathEdit();
            m_search->setFocus(Qt::ShortcutFocusReason);
            m_search->selectAll();
        });
    }
    for (const auto key : {QKeySequence(Qt::Key_F6), QKeySequence(Qt::SHIFT | Qt::Key_F6)}) {
        auto *shortcut = new QShortcut(key, this);
        connect(shortcut, &QShortcut::activated, this, [this, key] {
            cycleFocus(key == QKeySequence(Qt::SHIFT | Qt::Key_F6));
        });
    }
    // Also covers the shared address editor, which is created lazily.
    qApp->installEventFilter(this);
    restrictViews(m_places);
    restrictViews(m_libraries);
    restrictViews(m_computer);
    applyFilter();

    m_optionsSupported = options.localOnly && !options.multiple && !options.directory;
    if (!options.selectedFilter.isEmpty() && selected < 0)
        m_optionsSupported = false;
    const QString extension = options.defaultExtension;
    if (extension.size() > 255 || extension.startsWith(QLatin1Char('.'))
        || extension.contains(QLatin1Char('/')) || extension.contains(QLatin1Char('\\'))
        || extension.contains(QLatin1Char('*')) || extension.contains(QLatin1Char('?'))
        || hasControl(extension))
        m_optionsSupported = false;
    choose->setEnabled(m_optionsSupported);
    const QUrl start = options.startingLocation.isEmpty()
        ? QUrl::fromLocalFile(QDir::homePath()) : options.startingLocation;
    if (!m_navigation->initialize(start)) {
        const QString error = errorMessage();
        m_navigation->initialize(QUrl::fromLocalFile(QDir::homePath()));
        showError(error);
    }
    if (!m_optionsSupported)
        showError(tr("These file chooser options are not supported."));
    for (auto *button : findChildren<QPushButton *>()) {
        button->setAutoDefault(button == choose);
        button->setDefault(button == choose);
    }
    m_name->selectAll();
    m_name->setFocus();
}

FileChooserDialog::~FileChooserDialog()
{
    qApp->removeEventFilter(this);
    dismissOverwrite();
}

void FileChooserDialog::setVisible(bool visible)
{
    if (!visible) { QDialog::setVisible(false); return; }
    // Scope the id to native window creation; never match the browser-only caption exception.
    const QString previous = QGuiApplication::desktopFileName();
    QGuiApplication::setDesktopFileName(QStringLiteral("linux-explorer-win7-dialog"));
    QDialog::setVisible(true);
    QGuiApplication::setDesktopFileName(previous);
}

QString FileChooserDialog::selectedFilter() const
{
    return m_filters->currentData().toString();
}

QUrl FileChooserDialog::currentUrl() const { return m_navigation->currentUrl(); }
QString FileChooserDialog::errorMessage() const { return m_error->text(); }

void FileChooserDialog::showError(const QString &message)
{
    m_error->setText(message);
    m_error->setVisible(!message.isEmpty());
}

QString FileChooserDialog::locationError(const QUrl &url) const
{
    if (url == Locations::libraries() || url == Locations::computer())
        return {};
    if (!localFileUrl(url))
        return tr("Only local files and folders can be selected.");
    const QFileInfo info(url.toLocalFile());
    if (!info.exists())
        return tr("The location does not exist: %1").arg(url.toLocalFile());
    if (info.isDir() && !readableDirectory(info))
        return tr("The folder cannot be read or searched: %1").arg(info.filePath());
    if (!info.isDir() && (!info.isFile() || !readableFile(info)))
        return tr("The file cannot be read: %1").arg(info.filePath());
    return {};
}

void FileChooserDialog::navigateTo(const QUrl &url)
{
    // Library entries already carry their actual XDG folder URL.
    const auto library = Libraries::forUrl(url);
    const QUrl target = library.url.isValid() ? library.url : url;
    if (target.isLocalFile() && QFileInfo(target.toLocalFile()).isFile()) {
        activateUrl(target);
        return;
    }
    m_navigation->navigateTo(target);
}

void FileChooserDialog::showLocation(const QUrl &url)
{
    showError({});
    m_selectedDirectory = QUrl();
    m_places->setCurrentUrl(url);
    m_search->clear();
    if (url == Locations::libraries() || url == Locations::computer()) {
        m_loadingDelay->stop();
        m_navigation->stopPathProgress();
        m_search->setEnabled(false);
        m_stack->setCurrentWidget(url == Locations::libraries()
            ? static_cast<QWidget *>(m_libraries) : static_cast<QWidget *>(m_computer));
    } else {
        m_search->setEnabled(true);
        m_stack->setCurrentWidget(m_files);
        m_files->setStatusMessage({});
        m_model->setUrl(url);
    }
}

void FileChooserDialog::applyFilter()
{
    const int index = m_filters->currentIndex();
    if (index >= 0 && index < m_options.filters.size()) {
        const auto &filter = m_options.filters.at(index);
        m_model->setChooserFilter(filter.globs, filter.mimeTypes);
    } else {
        m_model->setChooserFilter({}, {});
    }
}

QString FileChooserDialog::defaultExtension() const
{
    if (!m_options.defaultExtension.isEmpty())
        return m_options.defaultExtension;
    const int index = m_filters->currentIndex();
    if (index >= 0 && index < m_options.filters.size()) {
        for (const auto &glob : m_options.filters.at(index).globs) {
            if (!glob.startsWith(QLatin1String("*."))) continue;
            const QString extension = glob.mid(2);
            if (!extension.isEmpty() && !extension.startsWith(QLatin1Char('.'))
                && !extension.contains(QLatin1Char('/')) && !extension.contains(QLatin1Char('\\'))
                && !extension.contains(QLatin1Char('*')) && !extension.contains(QLatin1Char('?'))
                && !extension.contains(QLatin1Char('[')) && !extension.contains(QLatin1Char(']'))
                && !hasControl(extension))
                return extension;
        }
    }
    return {};
}

QUrl FileChooserDialog::typedUrl() const
{
    const QString text = m_name->text();
    if (text.isEmpty() || text.contains(QChar::Null))
        return {};
    // Only explicitly typed URIs are decoded. Relative filename %, # and spaces stay literal.
    if (QRegularExpression(QStringLiteral("^[A-Za-z][A-Za-z0-9+.-]*:")).match(text).hasMatch())
        return QUrl::fromEncoded(text.toUtf8(), QUrl::StrictMode);
    if (QDir::isAbsolutePath(text))
        return QUrl::fromLocalFile(text);
    if (!localFileUrl(currentUrl()))
        return {};
    return QUrl::fromLocalFile(QDir(currentUrl().toLocalFile()).absoluteFilePath(text));
}

void FileChooserDialog::activateUrl(const QUrl &url)
{
    if (localFileUrl(url) && QFileInfo(url.toLocalFile()).isDir()) {
        navigateTo(url);
        return;
    }
    validateSelection(url);
}

void FileChooserDialog::accept()
{
    if (m_overwrite)
        return;
    if (m_navigation->pathEditor() && m_navigation->pathEditor()->isVisible()) {
        // Address Enter is handled by the shared editor, never by the default button.
        return;
    }
    if (m_selectedDirectory.isValid()) {
        navigateTo(m_selectedDirectory);
        return;
    }
    activateUrl(typedUrl());
}

void FileChooserDialog::validateSelection(const QUrl &url, bool overwriteApproved)
{
    if (!m_optionsSupported) { showError(tr("These file chooser options are not supported.")); return; }
    if (!localFileUrl(url)) { showError(tr("Enter an absolute local file URI or a file name in a local folder.")); return; }
    QString path = url.toLocalFile();
    const QString leaf = QFileInfo(path).fileName();
    if (leaf.isEmpty() || leaf == QLatin1String(".") || leaf == QLatin1String("..")) {
        showError(tr("Enter a valid file name.")); return;
    }
    if (m_options.mode == ChooserMode::SaveFile
        && (QFileInfo(path).suffix().isEmpty()
            || (leaf.startsWith(QLatin1Char('.')) && leaf.indexOf(QLatin1Char('.'), 1) < 0))
        && !leaf.endsWith(QLatin1Char('.')) && !defaultExtension().isEmpty())
        path += QLatin1Char('.') + defaultExtension();
    path = QDir::cleanPath(path);
    const QFileInfo info(path);
    const QFileInfo parent(info.absolutePath());
    const QByteArray parentPath = QFile::encodeName(parent.filePath());
    const long nameLimit = ::pathconf(parentPath.constData(), _PC_NAME_MAX);
    const long pathLimit = ::pathconf(parentPath.constData(), _PC_PATH_MAX);
    if (QFile::encodeName(info.fileName()).size() > (nameLimit > 0 ? nameLimit : NAME_MAX)
        || QFile::encodeName(path).size() >= (pathLimit > 0 ? pathLimit : PATH_MAX)) {
        showError(tr("The file name or path is too long for the destination filesystem.")); return;
    }
    if (!readableDirectory(parent)) { showError(tr("The containing folder cannot be read or searched.")); return; }
    if (info.isSymbolicLink() && !info.exists()) { showError(tr("The file is a broken symbolic link.")); return; }
    if (m_options.mode == ChooserMode::OpenFile) {
        if (!info.exists() || !info.isFile() || !readableFile(info)) {
            showError(tr("Select an existing readable file.")); return;
        }
    } else {
        if (!writable(parent)) { showError(tr("The destination folder is not writable.")); return; }
        if (info.exists() && (!info.isFile() || !writable(info))) {
            showError(tr("The destination is not a writable regular file.")); return;
        }
        if (info.exists() && !overwriteApproved) {
            auto *confirmation = new QMessageBox(QMessageBox::Warning, tr("Confirm Save As"),
                tr("%1 already exists.\nDo you want to replace it?").arg(info.fileName()),
                QMessageBox::Yes | QMessageBox::No, this);
            confirmation->setObjectName(QStringLiteral("chooserOverwrite"));
            confirmation->setDefaultButton(QMessageBox::No);
            confirmation->setWindowModality(Qt::WindowModal);
            confirmation->setAttribute(Qt::WA_DeleteOnClose);
            m_overwrite = confirmation;
            const QUrl destination = QUrl::fromLocalFile(path);
            connect(confirmation, &QDialog::finished, this, [this, confirmation, destination](int result) {
                if (m_overwrite != confirmation) return;
                m_overwrite.clear();
                if (result == QMessageBox::Yes)
                    validateSelection(destination, true);
            });
            confirmation->open();
            return;
        }
    }
    showError({});
    m_selectedUrls = {QUrl::fromLocalFile(path)};
    m_validated = true;
    done(QDialog::Accepted);
}

void FileChooserDialog::dismissOverwrite()
{
    if (!m_overwrite) return;
    auto *confirmation = m_overwrite.data();
    m_overwrite.clear();
    disconnect(confirmation, nullptr, this, nullptr);
    confirmation->reject();
}

void FileChooserDialog::reject() { done(QDialog::Rejected); }

void FileChooserDialog::done(int result)
{
    if (result == QDialog::Accepted && !m_validated) { accept(); return; }
    m_validated = false;
    dismissOverwrite();
    if (result != QDialog::Accepted) m_selectedUrls.clear();
    QDialog::done(result);
}

void FileChooserDialog::restrictViews(QWidget *container)
{
    for (auto *view : container->findChildren<QAbstractItemView *>()) {
        view->setContextMenuPolicy(Qt::PreventContextMenu);
        view->setDragDropMode(QAbstractItemView::NoDragDrop);
        view->setDragEnabled(false);
        view->setAcceptDrops(false);
        view->viewport()->setAcceptDrops(false);
        view->setEditTriggers(QAbstractItemView::NoEditTriggers);
        view->installEventFilter(this);
        view->viewport()->installEventFilter(this);
    }
}

bool FileChooserDialog::eventFilter(QObject *watched, QEvent *event)
{
    const auto *widget = qobject_cast<QWidget *>(watched);
    if (!widget || (widget != this && !isAncestorOf(widget)))
        return QDialog::eventFilter(watched, event);
    const bool browserPane = widget && (m_places->isAncestorOf(widget)
        || m_libraries->isAncestorOf(widget) || m_computer->isAncestorOf(widget));
    if (browserPane && event->type() == QEvent::MouseButtonPress
        && static_cast<QMouseEvent *>(event)->button() == Qt::MiddleButton)
        return true;
    if (browserPane && (event->type() == QEvent::ContextMenu || event->type() == QEvent::DragEnter
        || event->type() == QEvent::DragMove || event->type() == QEvent::Drop)) {
        event->ignore();
        return true;
    }
    if (event->type() == QEvent::KeyPress) {
        const auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            if (watched == m_search) return true;
            auto *editor = m_navigation->pathEditor();
            if (editor && watched == editor) {
                // A live completer must handle candidate selection itself.
                if (editor->completer() && editor->completer()->popup()->isVisible())
                    return QDialog::eventFilter(watched, event);
                m_navigation->submitPathEdit();
                return true;
            }
        }
        if (key->key() == Qt::Key_Escape && m_navigation->pathEditor()
            && m_navigation->pathEditor()->isVisible()) {
            m_navigation->endPathEdit();
            m_files->focusView();
            return true;
        }
    }
    return QDialog::eventFilter(watched, event);
}

void FileChooserDialog::cycleFocus(bool backwards)
{
    QWidget *focused = QApplication::focusWidget();
    const QList<QWidget *> stops = {m_navigation->pathEditor(), m_search,
        m_places->findChild<QAbstractItemView *>(), m_stack->currentWidget(), m_name, m_filters, m_buttons};
    int current = -1;
    for (int i = 0; i < stops.size(); ++i)
        if (stops[i] && (focused == stops[i] || stops[i]->isAncestorOf(focused))) current = i;
    for (int step = 1; step <= stops.size(); ++step) {
        const int next = (current + (backwards ? -step : step) + stops.size()) % stops.size();
        QWidget *target = stops[next];
        if (next != 0 && (!target || !target->isEnabled() || !target->isVisible())) continue;
        m_navigation->endPathEdit();
        if (next == 0) m_navigation->focusPathEdit();
        else if (target == m_files) m_files->focusView();
        else if (target == m_computer) m_computer->focusView();
        else if (target == m_libraries) {
            for (auto *view : m_libraries->findChildren<QAbstractItemView *>())
                if (view->isVisible()) { view->setFocus(Qt::TabFocusReason); break; }
        } else if (target == m_buttons) m_buttons->buttons().first()->setFocus(Qt::TabFocusReason);
        else target->setFocus(Qt::TabFocusReason);
        return;
    }
}
