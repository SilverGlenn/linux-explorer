#pragma once

#include "Settings.h"
#include <QUrl>
#include <QWidget>

class QAbstractItemView;
class QListView;
class QStackedWidget;
class QStandardItemModel;
class QTreeView;

// The shell's Libraries landing page, not a KIO directory or a writable folder.
class LibrariesView : public QWidget {
    Q_OBJECT
public:
    explicit LibrariesView(QWidget *parent = nullptr);
    int libraryCount() const;
    QUrl selectedUrl() const;
    QString selectedName() const;
    void refresh();
    void setViewMode(Settings::ViewMode mode);
    Settings::ViewMode viewMode() const { return m_mode; }

Q_SIGNALS:
    void urlActivated(const QUrl &url);
    void newWindowRequested(const QUrl &url);
    void selectionChanged();
    void viewModeChanged(Settings::ViewMode mode);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void activateCurrent();
    void applyMode();
    void updateGrid();
    void showItemContextMenu(QAbstractItemView *view, const QPoint &pos);
    QStackedWidget *m_views = nullptr;
    QTreeView *m_details = nullptr;
    QListView *m_list = nullptr;
    QStandardItemModel *m_model = nullptr;
    Settings::ViewMode m_mode = Settings::ViewMode::Tiles;
};
