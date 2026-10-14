#pragma once

#include <QIcon>
#include <QList>
#include <QModelIndex>
#include <QWidget>
#include <KFileItem>

class QLabel;

// Win7's full-width, 54px details pane. A summary is just a count; a selection
// adds type, date and size. Disk capacity belongs to Computer, not every folder.
class DetailsPane : public QWidget {
    Q_OBJECT
public:
    explicit DetailsPane(QWidget *parent = nullptr);
    void showFolderSummary(int itemCount, const QIcon &icon = QIcon());
    void showLibrary(const QIcon &icon, const QString &name);
    void showComputerSummary();
    void showSelection(const QList<KFileItem> &items);
    void showDrive(const QModelIndex &index);
    void showMessage(const QString &message);

private:
    void setContent(const QIcon &icon, const QString &primary,
                    const QString &secondary, const QString &topDetail = QString(),
                    const QString &bottomDetail = QString(),
                    const QString &createdDetail = QString());
    QLabel *m_icon = nullptr;
    QLabel *m_primary = nullptr;
    QLabel *m_secondary = nullptr;
    QLabel *m_topDetail = nullptr;
    QLabel *m_bottomDetail = nullptr;
    QLabel *m_createdDetail = nullptr;
};
