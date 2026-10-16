#pragma once

#include "ChooserOptions.h"
#include <QDialog>
#include <QMap>
#include <QPointer>

class BrowserNavigation;
class ComputerModel;
class ComputerView;
class DirectoryModel;
class FileView;
class LibrariesView;
class NavigationPane;
class PreviewPane;
class SearchBox;
class QAction;
class QComboBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QMessageBox;
class QStackedWidget;
class QTimer;

// A selection-only common file dialog. No file operations or application launches.
class FileChooserDialog : public QDialog {
    Q_OBJECT
public:
    explicit FileChooserDialog(const ChooserOptions &options, QWidget *parent = nullptr);
    ~FileChooserDialog() override;
    QList<QUrl> selectedUrls() const { return m_selectedUrls; }
    QString selectedFilter() const;
    QMap<QString, QString> selectedChoices() const;
    QUrl currentUrl() const;
    QString errorMessage() const;
    void setVisible(bool visible) override;

public slots:
    void navigateTo(const QUrl &url);
    void accept() override;
    void reject() override;
    void done(int result) override;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    QString locationError(const QUrl &url) const;
    void showLocation(const QUrl &url);
    void showError(const QString &message);
    void activateUrl(const QUrl &url);
    QUrl typedUrl() const;
    void validateSelection(const QUrl &url, bool overwriteApproved = false);
    QString defaultExtension() const;
    void applyFilter();
    bool folderMode() const;
    void acceptItems(const QList<QUrl> &urls, bool folders);
    void acceptSaveFiles(const QUrl &folder);
    void finishSelection(const QList<QUrl> &urls);
    void cycleFocus(bool backwards);
    void dismissOverwrite();
    void restrictViews(QWidget *container);
    void updatePreview();
    void setPreviewVisible(bool visible);

    ChooserOptions m_options;
    QList<QUrl> m_selectedUrls;
    bool m_selectionIntent = false;
    QMap<QString, QWidget *> m_choiceControls;
    NavigationPane *m_places;
    DirectoryModel *m_model;
    ComputerModel *m_computerModel;
    BrowserNavigation *m_navigation;
    FileView *m_files;
    LibrariesView *m_libraries;
    ComputerView *m_computer;
    QStackedWidget *m_stack;
    PreviewPane *m_preview;
    QAction *m_previewAction;
    SearchBox *m_search;
    QLineEdit *m_name;
    QComboBox *m_filters;
    QLabel *m_error;
    QDialogButtonBox *m_buttons;
    QTimer *m_loadingDelay;
    QPointer<QMessageBox> m_overwrite;
    bool m_optionsSupported = true;
    bool m_validated = false;
};
