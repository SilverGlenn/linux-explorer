#include "DetailsPane.h"
#include "ComputerModel.h"
#include "aero/artwork.h"
#include "aero/icons.h"
#include "aero/text.h"

#include <KIO/Global>
#include <QDateTime>
#include <QFile>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QSysInfo>
#ifdef Q_OS_LINUX
#include <sys/sysinfo.h>
#endif

namespace {
class ElidingLabel : public QLabel {
public:
    using QLabel::QLabel;
    void setFullText(const QString &text)
    {
        m_full = text;
        setToolTip(text);
        setAccessibleName(text);
        elide();
    }
protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QLabel::resizeEvent(event);
        elide();
    }
private:
    void elide() { QLabel::setText(fontMetrics().elidedText(m_full, Qt::ElideRight, width())); }
    QString m_full;
};

QLabel *detailLabel(const char *name, const char *color, QWidget *parent)
{
    auto *label = new ElidingLabel(parent);
    label->setObjectName(QLatin1String(name));
    Aero::setPointSize(label, 9);
    label->setStyleSheet(QStringLiteral("color: %1; background: transparent;").arg(QLatin1String(color)));
    label->setMinimumWidth(0);
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    label->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
    return label;
}

QString cpuName()
{
    QFile file(QStringLiteral("/proc/cpuinfo"));
    if (file.open(QIODevice::ReadOnly)) {
        for (const QByteArray &line : file.readAll().split('\n')) {
            if (line.startsWith("model name"))
                return QString::fromLocal8Bit(line.mid(line.indexOf(':') + 1).trimmed());
        }
    }
    return QSysInfo::currentCpuArchitecture();
}
} // namespace

DetailsPane::DetailsPane(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("detailsPane"));
    setAttribute(Qt::WA_StyledBackground, true);
    setFixedHeight(54);
    setStyleSheet(Aero::panelSheet(objectName(), Aero::Palette::StripSurface,
                                  Qt::TopEdge, Aero::Palette::StripRule));
    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(16, 2, 16, 2);
    row->setSpacing(12);
    m_icon = new QLabel(this);
    m_icon->setFixedSize(48, 48);
    row->addWidget(m_icon);
    auto *text = new QGridLayout;
    text->setContentsMargins(0, 4, 0, 4);
    text->setHorizontalSpacing(18);
    text->setVerticalSpacing(0);
    m_primary = detailLabel("detailsPrimary", Aero::Palette::Text, this);
    m_secondary = detailLabel("detailsSecondary", Aero::Palette::MutedText, this);
    m_topDetail = detailLabel("detailsTopMetadata", Aero::Palette::LinkText, this);
    m_bottomDetail = detailLabel("detailsBottomMetadata", Aero::Palette::MutedText, this);
    m_createdDetail = detailLabel("detailsCreatedMetadata", Aero::Palette::LinkText, this);
    text->addWidget(m_primary, 0, 0);
    text->addWidget(m_secondary, 1, 0);
    text->addWidget(m_topDetail, 0, 1);
    text->addWidget(m_bottomDetail, 1, 1, 1, 2);
    text->addWidget(m_createdDetail, 0, 2);
    // Keep the name/type column flexible: a fixed 100 px first column made
    // selected folder names disappear even when the pane had ample room.
    text->setColumnStretch(0, 2);
    text->setColumnStretch(1, 1);
    text->setColumnStretch(2, 1);
    row->addLayout(text, 1);
}

void DetailsPane::setContent(const QIcon &icon, const QString &primary,
                             const QString &secondary, const QString &topDetail,
                             const QString &bottomDetail, const QString &createdDetail)
{
    m_icon->setPixmap(icon.pixmap(48, 48));
    m_icon->setAccessibleName(primary);
    static_cast<ElidingLabel *>(m_primary)->setFullText(primary);
    static_cast<ElidingLabel *>(m_secondary)->setFullText(secondary);
    static_cast<ElidingLabel *>(m_topDetail)->setFullText(topDetail);
    static_cast<ElidingLabel *>(m_bottomDetail)->setFullText(bottomDetail);
    static_cast<ElidingLabel *>(m_createdDetail)->setFullText(createdDetail);
    m_createdDetail->setVisible(!createdDetail.isEmpty());
}

void DetailsPane::showFolderSummary(int count, const QIcon &icon)
{
    setContent(icon.isNull() ? Aero::themeIcon({"folder"}) : icon,
               count == 1 ? tr("1 item") : tr("%1 items").arg(count), QString());
}

void DetailsPane::showLibrary(const QIcon &icon, const QString &name)
{
    setContent(icon, name, tr("Library"));
}

void DetailsPane::showComputerSummary()
{
    QString memory;
#ifdef Q_OS_LINUX
    struct sysinfo info {};
    if (sysinfo(&info) == 0) {
        const double gib = double(info.totalram) * info.mem_unit / (1024.0 * 1024.0 * 1024.0);
        memory = tr("Memory: %1 GB").arg(QLocale().toString(gib, 'f', 2));
    }
#endif
    setContent(Aero::themeIcon({"computer"}), QSysInfo::machineHostName(),
               QString(), memory, tr("Processor: %1").arg(cpuName()));
}

void DetailsPane::showDrive(const QModelIndex &index)
{
    if (!index.isValid()) return;
    QString figures;
    if (index.data(ComputerModel::SizeKnownRole).toBool()) {
        figures = tr("%1 free of %2").arg(
            KIO::convertSize(index.data(ComputerModel::AvailableSizeRole).toULongLong()),
            KIO::convertSize(index.data(ComputerModel::TotalSizeRole).toULongLong()));
    }
    setContent(qvariant_cast<QIcon>(index.data(Qt::DecorationRole)),
               index.data(Qt::DisplayRole).toString(),
               index.data(ComputerModel::RemovableRole).toBool() ? tr("Removable Disk") : tr("Local Disk"),
               figures, index.data(ComputerModel::FileSystemTypeRole).toString());
}

void DetailsPane::showMessage(const QString &message)
{
    setContent(Aero::themeIcon({"dialog-warning", "dialog-error"}), message, QString());
}

void DetailsPane::showSelection(const QList<KFileItem> &items)
{
    if (items.isEmpty()) return;
    if (items.size() == 1) {
        const KFileItem &item = items.first();
        const QDateTime modified = item.time(KFileItem::ModificationTime);
        const QString date = modified.isValid()
            ? tr("Date modified: %1").arg(QLocale().toString(modified, QLocale::ShortFormat)) : QString();
        // Use KIO's supplied birth time; statting a mounted path here could
        // block selection changes on a slow device.
        const QDateTime created = item.time(KFileItem::CreationTime);
        const QString createdText = created.isValid()
            ? tr("Date created: %1").arg(QLocale().toString(created, QLocale::ShortFormat)) : QString();
        setContent(QIcon::fromTheme(item.iconName(), Aero::themeIcon({"folder"})),
                   item.text(), item.isDir() ? tr("File folder") : item.mimeComment(), date,
                   item.isDir() ? QString() : tr("Size: %1").arg(KIO::convertSize(item.size())), createdText);
        return;
    }
    KIO::filesize_t total = 0;
    bool anyDirs = false;
    for (const KFileItem &item : items) {
        if (item.isDir()) anyDirs = true;
        else total += item.size();
    }
    setContent(Aero::themeIcon({"edit-select-all", "folder"}),
               tr("%1 items selected").arg(items.size()), QString(), QString(),
               anyDirs ? tr("Size: %1 (files only)").arg(KIO::convertSize(total))
                       : tr("Size: %1").arg(KIO::convertSize(total)));
}
