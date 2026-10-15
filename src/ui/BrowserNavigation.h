#pragma once

#include <QIcon>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QUrl>
#include <functional>
#include <utility>

class ComputerModel;
class KFilePlacesModel;
class QHBoxLayout;
class QLineEdit;
class QMenu;
class QProgressBar;
class QPropertyAnimation;
class QPushButton;
class QTimer;
class QWidget;

// The shared Explorer navigation/address surface and its single history.
// Populates the supplied navigation-row layout; does not create a window, file
// view, search box or listing model, and never opens files or invokes FileOps.
// The host/layout and shared places/computer models must outlive this object.
class BrowserNavigation : public QObject {
    Q_OBJECT
public:
    struct Crumb {
        QString label;
        QUrl target;
    };

    BrowserNavigation(KFilePlacesModel *places, ComputerModel *computer,
                      QHBoxLayout *layout, QObject *parent = nullptr);

    // Call once per host. MainWindow already has Up/Refresh menu actions.
    void installShortcuts(QWidget *host, bool includeUpAndRefresh = true);

    // Empty means allowed; otherwise the message is emitted as errorOccurred.
    // Applied before every navigation/history change and typed file activation.
    // Install before initialize() to restrict remote/admin/virtual locations.
    using UrlPolicy = std::function<QString(const QUrl &)>;
    void setUrlPolicy(UrlPolicy policy) { m_urlPolicy = std::move(policy); }
    void setShowHiddenFilesProvider(std::function<bool()> provider)
    { m_showHiddenFiles = std::move(provider); }

    // Resets the history and emits locationChanged without navigated (no sound).
    bool initialize(const QUrl &url);
    QUrl currentUrl() const;
    // Silent spring-loaded navigation still records history and changes location.
    bool navigateTo(const QUrl &url, bool announceNavigation = true);
    void goBack();
    void goForward();
    void goUp();

    void focusPathEdit(bool dropHistory = false);
    void endPathEdit();
    // Commit the current address using the shared parser/policy. Hosts with a
    // default dialog button can consume Return and call this to avoid bubbling.
    void submitPathEdit();
    QLineEdit *pathEditor() const { return m_pathEdit; }
    void refreshTrail(); // Does not destroy an active edit.
    QList<Crumb> crumbsFor(const QUrl &url) const;
    QIcon locationIcon(const QUrl &url) const;
    static QString displayPath(const QUrl &url);

    // Host drives these from the existing DirectoryModel listing lifecycle;
    // showPathProgress is delayed by the host's loading-placeholder timer.
    void startPathProgress();
    void showPathProgress();
    void setPathProgress(int percent);
    void finishPathProgress();
    void stopPathProgress();

signals:
    // History/currentUrl and address trail are already updated on delivery.
    void locationChanged(const QUrl &url);
    void navigated();
    // A real local file typed into the address bar. The host decides whether
    // to select/accept it (chooser) or open it (file manager); history is intact.
    void typedFileActivated(const QUrl &url);
    void errorOccurred(const QString &message);
    void refreshRequested();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    bool accepts(const QUrl &url);
    void goToHistoryIndex(int index);
    void pushHistory(const QUrl &url);
    void updateNavButtons();
    Crumb driveFor(const QString &path) const;
    void beginPathEdit();
    void showPathHistoryMenu();
    void setCrumbTrail(const QUrl &url);
    void clearPathLayout();
    void showCrumbMenu(const QUrl &parent, const QPoint &globalPos);
    void layoutPathProgress();

    KFilePlacesModel *m_places;
    ComputerModel *m_computerModel;
    QWidget *m_host;
    UrlPolicy m_urlPolicy;
    std::function<bool()> m_showHiddenFiles;
    QList<QUrl> m_history;
    int m_historyIndex = -1;
    QWidget *m_pathBox = nullptr;
    QHBoxLayout *m_pathLayout = nullptr;
    QPointer<QMenu> m_crumbMenu;
    QLineEdit *m_pathEdit = nullptr;
    QPushButton *m_backBtn = nullptr;
    QPushButton *m_forwardBtn = nullptr;
    QProgressBar *m_pathProgress = nullptr;
    QTimer *m_pathProgressTimer = nullptr;
    QPropertyAnimation *m_pathProgressFill = nullptr;
};
