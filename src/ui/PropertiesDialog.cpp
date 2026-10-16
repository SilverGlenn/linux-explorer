#include "PropertiesDialog.h"

#include "aero/icons.h"
#include "aero/fonts.h"

#include <KApplicationTrader>
#include <KIO/ChmodJob>
#include <KIO/CopyJob>
#include <KIO/FileUndoManager>
#include <KIO/Global>
#include <KIO/ListJob>
#include <KJob>
#include <KOpenWithDialog>
#include <KPageWidget>
#include <KPageWidgetModel>
#include <KPropertiesDialog>

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPainter>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTabBar>
#include <QTabWidget>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtConcurrentRun>

#include <algorithm>
#include <sys/stat.h>
#include <utility>

namespace {

class DesktopPropertiesDialog : public KPropertiesDialog {
public:
    using KPropertiesDialog::KPropertiesDialog;

    void selectPermissionsPage()
    {
        auto *model = qobject_cast<KPageWidgetModel *>(pageWidget()->model());
        if (model && model->rowCount() > 1)
            setCurrentPage(model->item(model->index(1, 0)));
    }
};

QString unavailable()
{
    return PropertiesDialog::tr("Not available");
}

QString dateText(const QDateTime &date)
{
    return date.isValid() ? QLocale().toString(date, QLocale::ShortFormat) : unavailable();
}

QString byteText(quint64 bytes)
{
    const QLocale locale;
    return PropertiesDialog::tr("%1 (%2 bytes)")
        .arg(locale.formattedDataSize(qint64(bytes), 2, QLocale::DataSizeTraditionalFormat),
             locale.toString(bytes));
}

QLabel *valueLabel(const QString &text = {}, const char *name = nullptr)
{
    auto *label = new QLabel(text);
    if (name)
        label->setObjectName(QLatin1String(name));
    label->setTextFormat(Qt::PlainText);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    label->setMinimumWidth(0);
    label->setToolTip(text);
    return label;
}

void setValue(QLabel *label, const QString &text)
{
    if (!label)
        return;
    label->setText(text);
    label->setToolTip(text);
}

QGridLayout *rows(QVBoxLayout *layout, int labelWidth)
{
    auto *grid = new QGridLayout;
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(8);
    grid->setVerticalSpacing(0);
    grid->setColumnMinimumWidth(0, labelWidth);
    grid->setColumnStretch(1, 1);
    layout->addLayout(grid);
    return grid;
}

void row(QGridLayout *grid, int index, const QString &title, QWidget *value)
{
    auto *label = new QLabel(title);
    label->setTextFormat(Qt::PlainText);
    label->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
    grid->addWidget(label, index, 0, Qt::AlignLeft | Qt::AlignVCenter);
    grid->addWidget(value, index, 1);
    grid->setRowMinimumHeight(index, 26);
}

void separator(QVBoxLayout *layout)
{
    layout->addSpacing(7);
    auto *line = new QFrame;
    line->setObjectName(QStringLiteral("propertiesSeparator"));
    line->setFrameShape(QFrame::NoFrame);
    line->setFixedHeight(1);
    line->setStyleSheet(QStringLiteral(
        "QFrame#propertiesSeparator { background-color: #a0a0a0; border: none; }"));
    layout->addWidget(line);
    layout->addSpacing(7);
}

QWidget *pageWidget()
{
    auto *page = new QWidget;
    page->setObjectName(QStringLiteral("propertiesPage"));
    page->setAutoFillBackground(true);
    QPalette palette = page->palette();
    palette.setColor(QPalette::Window, Qt::white);
    palette.setColor(QPalette::WindowText, Qt::black);
    page->setPalette(palette);
    return page;
}

// The small, flattened capacity pie used on Windows' drive General page.
class CapacityPie : public QWidget {
public:
    CapacityPie(quint64 used, quint64 total, QWidget *parent = nullptr)
        : QWidget(parent), m_used(used), m_total(total)
    {
        setFixedSize(128, 72);
        setAccessibleName(PropertiesDialog::tr("Drive space"));
        setToolTip(PropertiesDialog::tr("Used space: %1\nCapacity: %2")
                       .arg(byteText(used), byteText(total)));
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF top(2, 3, width() - 4, height() - 15);
        const QRectF bottom = top.translated(0, 8);
        const int usedAngle = m_total
            ? qBound(0, int((static_cast<long double>(m_used) / m_total) * 5760), 5760) : 0;
        painter.setPen(QPen(Qt::black, 1));
        painter.setBrush(QColor(125, 0, 125));
        painter.drawEllipse(bottom);
        painter.setBrush(QColor(0, 0, 150));
        if (usedAngle)
            painter.drawPie(bottom, 0, usedAngle);
        painter.setBrush(QColor(255, 0, 255));
        painter.drawEllipse(top);
        painter.setBrush(QColor(0, 0, 255));
        if (usedAngle)
            painter.drawPie(top, 0, usedAngle);
    }

private:
    quint64 m_used;
    quint64 m_total;
};

} // namespace

PropertiesDialog::PropertiesDialog(const KFileItemList &items, QWidget *parent)
    : QDialog(parent), m_items(items), m_data(PropertiesData::fromItems(items))
{
    setObjectName(QStringLiteral("propertiesDialog"));
    setWindowTitle(tr("%1 Properties").arg(m_data.displayName));
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    // Windows 7's property sheet requests MS Shell Dlg, mapped to Microsoft
    // Sans Serif, at 8pt (-11 logical pixels at 96 DPI). Explorer itself uses
    // Segoe UI 9pt. Keep a compact fallback where the legacy font is absent.
    setFont(Aero::classicDialogFont());
    QPalette sheetPalette = palette();
    sheetPalette.setColor(QPalette::Window, QColor(QStringLiteral("#f0f0f0")));
    sheetPalette.setColor(QPalette::WindowText, Qt::black);
    setPalette(sheetPalette);
    setAutoFillBackground(true);
    // Leave tabs and controls to the native style; only the page and footer
    // surfaces are defined by this property sheet.
    setStyleSheet(QStringLiteral(
        "QDialog#propertiesDialog { background-color: #f0f0f0; }"
        "QDialogButtonBox#propertiesButtons { button-layout: 0; }"
        "QWidget#propertiesPage { background-color: white; color: black; }"
        "QLabel#propertiesError { color: #a80000; }"));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);
    m_tabs = new QTabWidget;
    m_tabs->setObjectName(QStringLiteral("propertiesTabs"));
    m_tabs->setDocumentMode(false);
    m_tabs->tabBar()->setExpanding(false);
    m_tabs->setUsesScrollButtons(true);
    m_tabs->addTab(buildGeneralPage(), tr("General"));
    m_tabs->addTab(buildSecurityPage(), tr("Security"));
    m_tabs->addTab(buildDetailsPage(), tr("Details"));
    layout->addWidget(m_tabs, 1);

    m_error = new QLabel;
    m_error->setObjectName(QStringLiteral("propertiesError"));
    m_error->setWordWrap(true);
    m_error->setTextFormat(Qt::PlainText);
    m_error->hide();
    layout->addWidget(m_error);
    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel
                                     | QDialogButtonBox::Apply);
    m_buttons->setObjectName(QStringLiteral("propertiesButtons"));
    m_buttons->setCenterButtons(false);
    for (auto *button : m_buttons->buttons())
        button->setMinimumSize(75, 23);
    for (auto *button : findChildren<QPushButton *>())
        button->setAutoDefault(false);
    m_buttons->button(QDialogButtonBox::Ok)->setAutoDefault(true);
    m_buttons->button(QDialogButtonBox::Ok)->setDefault(true);
    m_buttons->button(QDialogButtonBox::Apply)->setEnabled(false);
    connect(m_buttons, &QDialogButtonBox::accepted, this, &PropertiesDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &PropertiesDialog::reject);
    connect(this, &QDialog::finished, this, [this] { stopSizeCalculation(); });
    connect(m_buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked,
            this, [this] { applyChanges(false); });
    layout->addWidget(m_buttons);

    refresh();
    connect(m_name, &QLineEdit::textChanged, this, [this](const QString &name) {
        if (m_hidden) {
            const QSignalBlocker blocker(m_hidden);
            m_hidden->setChecked(name.startsWith(QLatin1Char('.')));
        }
        updateDirtyState();
    });
    if (m_hidden) {
        connect(m_hidden, &QCheckBox::toggled, this, [this](bool hidden) {
            QString name = m_name->text();
            if (hidden && !name.startsWith(QLatin1Char('.')))
                name.prepend(QLatin1Char('.'));
            else if (!hidden && name.startsWith(QLatin1Char('.')))
                name.remove(0, 1);
            m_name->setText(name);
            updateDirtyState();
        });
    }
    if (m_readOnly)
        connect(m_readOnly, &QCheckBox::toggled, this, &PropertiesDialog::updateDirtyState);

    Aero::applyClassicDialogFont(this);
    const int height = m_data.kind == PropertiesData::Kind::File ? 477 : 449;
    setMinimumSize(qMax(361, minimumSizeHint().width()),
                   qMax(height, minimumSizeHint().height()));
    resize(minimumSize());
    m_name->setFocus(Qt::OtherFocusReason);
}

PropertiesDialog::~PropertiesDialog()
{
    stopSizeCalculation();
    if (m_changeJob) {
        disconnect(m_changeJob, nullptr, this, nullptr);
        m_changeJob->kill(KJob::Quietly);
    }
}

QUrl PropertiesDialog::url() const
{
    return m_data.singleUrl;
}

int PropertiesDialog::labelWidth() const
{
    int width = 65;
    for (const QString &label : {tr("Type of file:"), tr("Size on disk:"), tr("Opens with:")})
        width = qMax(width, fontMetrics().horizontalAdvance(label));
    return width;
}

QWidget *PropertiesDialog::buildGeneralPage()
{
    auto *page = pageWidget();
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(12, 14, 12, 12);
    layout->setSpacing(0);
    auto *advanced = new QPushButton(tr("Advanced..."));
    advanced->setObjectName(QStringLiteral("propertiesAdvanced"));
    advanced->setMinimumSize(75, 23);
    advanced->setToolTip(tr("Open all properties provided by the desktop."));
    connect(advanced, &QPushButton::clicked, this, [this] { openDesktopProperties(false); });

    auto *heading = rows(layout, labelWidth());
    auto *icon = new QLabel;
    QIcon itemIcon;
    if (m_data.kind == PropertiesData::Kind::Drive)
        itemIcon = Aero::themeIcon({"drive-harddisk", "drive-removable-media"});
    else if (m_items.size() == 1)
        itemIcon = QIcon::fromTheme(m_items.first().iconName());
    else
        itemIcon = Aero::themeIcon({"text-x-generic", "folder"});
    icon->setPixmap(itemIcon.pixmap(32, 32));
    icon->setFixedSize(32, 32);
    heading->addWidget(icon, 0, 0, Qt::AlignLeft | Qt::AlignVCenter);
    m_name = new QLineEdit;
    m_name->setObjectName(QStringLiteral("propertiesName"));
    m_name->setReadOnly(!m_data.canRename);
    m_name->setMinimumHeight(23);
    heading->addWidget(m_name, 0, 1);
    heading->setRowMinimumHeight(0, 34);
    separator(layout);

    if (m_data.kind == PropertiesData::Kind::Drive) {
        auto *metadata = rows(layout, labelWidth());
        m_type = valueLabel(m_data.typeName, "propertiesType");
        row(metadata, 0, tr("Type:"), m_type);
        row(metadata, 1, tr("File system:"), valueLabel(m_data.fileSystem));
        separator(layout);

        auto *space = new QGridLayout;
        space->setContentsMargins(0, 0, 0, 0);
        space->setHorizontalSpacing(8);
        space->setVerticalSpacing(0);
        space->setColumnStretch(2, 1);
        for (int index = 0; index != 3; ++index) {
            if (index < 2) {
                auto *key = new QLabel;
                key->setFixedSize(15, 15);
                key->setStyleSheet(index == 0
                    ? QStringLiteral("background: #0000ff; border: 1px solid black;")
                    : QStringLiteral("background: #ff00ff; border: 1px solid black;"));
                space->addWidget(key, index, 0);
            }
            const QString title = index == 0 ? tr("Used space:")
                : index == 1 ? tr("Free space:") : tr("Capacity:");
            const quint64 amount = index == 0 ? m_data.usedSize
                : index == 1 ? m_data.availableSize : m_data.totalSize;
            space->addWidget(new QLabel(title), index, 1);
            auto *bytes = valueLabel(tr("%1 bytes").arg(QLocale().toString(amount)));
            bytes->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            space->addWidget(bytes, index, 2);
            auto *human = new QLabel(QLocale().formattedDataSize(qint64(amount), 2,
                                                    QLocale::DataSizeTraditionalFormat));
            human->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            space->addWidget(human, index, 3);
            space->setRowMinimumHeight(index, 26);
            if (index == 1)
                space->setRowMinimumHeight(index, 34);
        }
        layout->addLayout(space);
        layout->addWidget(new CapacityPie(m_data.usedSize, m_data.totalSize), 0, Qt::AlignHCenter);
        auto *driveCaption = new QLabel(m_data.displayName);
        driveCaption->setTextFormat(Qt::PlainText);
        driveCaption->setAlignment(Qt::AlignCenter);
        layout->addWidget(driveCaption);
        auto *advancedRow = new QHBoxLayout;
        advancedRow->addStretch(1);
        advancedRow->addWidget(advanced);
        layout->addLayout(advancedRow);
        layout->addStretch(1);
    } else {
        const bool file = m_data.kind == PropertiesData::Kind::File;
        m_type = valueLabel(m_data.typeName, "propertiesType");
        if (file) {
            auto *type = rows(layout, labelWidth());
            row(type, 0, tr("Type of file:"), m_type);
            if (!m_data.mimeType.isEmpty()) {
                auto *openRow = new QWidget;
                auto *openLayout = new QHBoxLayout(openRow);
                openLayout->setContentsMargins(0, 0, 0, 0);
                openLayout->setSpacing(8);
                m_opensWith = valueLabel({}, "propertiesOpensWith");
                openLayout->addWidget(m_opensWith, 1);
                auto *change = new QPushButton(tr("Change..."));
                change->setObjectName(QStringLiteral("propertiesChangeApplication"));
                change->setMinimumSize(75, 23);
                openLayout->addWidget(change);
                connect(change, &QPushButton::clicked, this, &PropertiesDialog::chooseApplication);
                row(type, 1, tr("Opens with:"), openRow);
            }
            separator(layout);
        }
        auto *sizes = rows(layout, labelWidth());
        int sizeRow = 0;
        if (!file)
            row(sizes, sizeRow++, tr("Type:"), m_type);
        m_location = valueLabel(m_data.location, "propertiesLocation");
        row(sizes, sizeRow++, tr("Location:"), m_location);
        m_size = valueLabel({}, "propertiesSize");
        row(sizes, sizeRow++, tr("Size:"), m_size);
        m_sizeOnDisk = valueLabel({}, "propertiesSizeOnDisk");
        row(sizes, sizeRow++, tr("Size on disk:"), m_sizeOnDisk);
        if (m_data.kind == PropertiesData::Kind::Folder || m_data.kind == PropertiesData::Kind::Multiple) {
            m_contains = valueLabel(tr("Calculating..."), "propertiesContains");
            row(sizes, sizeRow++, tr("Contains:"), m_contains);
        }
        if (m_data.kind != PropertiesData::Kind::Multiple) {
            separator(layout);
            auto *times = rows(layout, labelWidth());
            m_created = valueLabel(dateText(m_data.created), "propertiesCreated");
            row(times, 0, tr("Created:"), m_created);
            if (m_data.kind == PropertiesData::Kind::File) {
                m_modified = valueLabel(dateText(m_data.modified), "propertiesModified");
                m_accessed = valueLabel(dateText(m_data.accessed), "propertiesAccessed");
                row(times, 1, tr("Modified:"), m_modified);
                row(times, 2, tr("Accessed:"), m_accessed);
            }
            separator(layout);
            auto *attributes = rows(layout, labelWidth());
            auto *checks = new QWidget;
            auto *checkLayout = new QHBoxLayout(checks);
            checkLayout->setContentsMargins(0, 0, 0, 0);
            checkLayout->setSpacing(12);
            m_readOnly = new QCheckBox(tr("Read-only"));
            m_readOnly->setObjectName(QStringLiteral("propertiesReadOnly"));
            m_readOnly->setEnabled(m_data.kind == PropertiesData::Kind::File && m_data.canChangePermissions);
            m_readOnly->setToolTip(tr("Controls the owner's write permission. Other permissions are preserved."));
            m_hidden = new QCheckBox(tr("Hidden"));
            m_hidden->setObjectName(QStringLiteral("propertiesHidden"));
            m_hidden->setEnabled(m_data.canRename);
            m_hidden->setToolTip(tr("Hidden items have a name beginning with a dot."));
            checkLayout->addWidget(m_readOnly);
            if (file) {
                checkLayout->addWidget(m_hidden);
                checkLayout->addStretch(1);
                checkLayout->addWidget(advanced);
            }
            row(attributes, 0, tr("Attributes:"), checks);
            if (!file) {
                auto *hiddenRow = new QWidget;
                auto *hiddenLayout = new QHBoxLayout(hiddenRow);
                hiddenLayout->setContentsMargins(0, 0, 0, 0);
                hiddenLayout->addWidget(m_hidden);
                hiddenLayout->addStretch(1);
                hiddenLayout->addWidget(advanced);
                row(attributes, 1, {}, hiddenRow);
            }
        } else {
            auto *advancedRow = new QHBoxLayout;
            advancedRow->addStretch(1);
            advancedRow->addWidget(advanced);
            layout->addLayout(advancedRow);
        }
        layout->addStretch(1);
    }
    return page;
}

QWidget *PropertiesDialog::buildSecurityPage()
{
    auto *page = pageWidget();
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(12, 14, 12, 12);
    layout->setSpacing(10);
    layout->addWidget(new QLabel(tr("Object name:")));
    m_securityObject = valueLabel();
    m_securityObject->setWordWrap(true);
    m_securityObject->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    layout->addWidget(m_securityObject);
    layout->addWidget(new QLabel(tr("Permissions:")));
    m_security = new QTreeWidget;
    m_security->setObjectName(QStringLiteral("propertiesPermissions"));
    m_security->setColumnCount(4);
    m_security->setHeaderLabels({tr("Name"), tr("Read"), tr("Write"), tr("Execute")});
    m_security->setRootIsDecorated(false);
    m_security->setUniformRowHeights(true);
    m_security->setSelectionMode(QAbstractItemView::NoSelection);
    m_security->setFocusPolicy(Qt::NoFocus);
    m_security->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int column = 1; column < 4; ++column)
        m_security->header()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    layout->addWidget(m_security, 1);
    auto *editRow = new QHBoxLayout;
    editRow->addStretch(1);
    auto *edit = new QPushButton(tr("Edit..."));
    edit->setObjectName(QStringLiteral("propertiesEditPermissions"));
    edit->setMinimumSize(75, 23);
    editRow->addWidget(edit);
    connect(edit, &QPushButton::clicked, this, [this] { openDesktopProperties(true); });
    layout->addLayout(editRow);
    return page;
}

QWidget *PropertiesDialog::buildDetailsPage()
{
    auto *page = pageWidget();
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(12, 14, 12, 12);
    m_details = new QTreeWidget;
    m_details->setObjectName(QStringLiteral("propertiesDetails"));
    m_details->setColumnCount(2);
    m_details->setHeaderLabels({tr("Property"), tr("Value")});
    m_details->setRootIsDecorated(false);
    m_details->setUniformRowHeights(true);
    m_details->setAlternatingRowColors(false);
    m_details->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_details->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    layout->addWidget(m_details);
    return page;
}

void PropertiesDialog::refresh()
{
    for (KFileItem &item : m_items)
        item.refresh();
    m_data = PropertiesData::fromItems(m_items);
    setWindowTitle(tr("%1 Properties").arg(m_data.displayName));
    {
        const QSignalBlocker blocker(m_name);
        m_name->setText(m_data.canRename ? m_data.realName : m_data.displayName);
    }
    if (m_readOnly) {
        const QSignalBlocker blocker(m_readOnly);
        m_readOnly->setChecked(m_data.readOnly);
    }
    if (m_hidden) {
        const QSignalBlocker blocker(m_hidden);
        m_hidden->setChecked(m_data.hidden);
    }
    setValue(m_type, m_data.typeName);
    setValue(m_location, m_data.location);
    setValue(m_created, dateText(m_data.created));
    setValue(m_modified, dateText(m_data.modified));
    setValue(m_accessed, dateText(m_data.accessed));
    refreshAssociation();
    updateSizeLabels();
    refreshDetails();
    refreshSecurity();
    if (m_buttons)
        updateDirtyState();
    startSizeCalculation();
}

void PropertiesDialog::refreshAssociation()
{
    if (!m_opensWith)
        return;
    const KService::Ptr service = m_pendingService ? m_pendingService
        : KApplicationTrader::preferredService(m_data.mimeType);
    setValue(m_opensWith, service ? service->name() : tr("Choose an application"));
}

void PropertiesDialog::refreshSecurity()
{
    if (!m_security)
        return;
    m_security->clear();
    setValue(m_securityObject, m_items.size() == 1
                 ? url().toDisplayString(QUrl::PreferLocalFile) : m_data.displayName);
    if (m_items.size() != 1) {
        auto *item = new QTreeWidgetItem(m_security, {tr("Multiple items")});
        item->setToolTip(0, tr("Use Edit to inspect or change the selected items' permissions."));
        return;
    }
    const QStringList names{tr("Owner (%1)").arg(m_data.owner),
                            tr("Group (%1)").arg(m_data.group), tr("Others")};
    const uint masks[3][3]{{S_IRUSR, S_IWUSR, S_IXUSR},
                           {S_IRGRP, S_IWGRP, S_IXGRP},
                           {S_IROTH, S_IWOTH, S_IXOTH}};
    for (int index = 0; index < 3; ++index) {
        auto *item = new QTreeWidgetItem(m_security, {names.at(index)});
        for (int column = 0; column < 3; ++column) {
            item->setText(column + 1, m_data.permissions & masks[index][column] ? tr("Allow") : QStringLiteral("—"));
            item->setTextAlignment(column + 1, Qt::AlignCenter);
        }
        item->setToolTip(0, names.at(index));
    }
}

void PropertiesDialog::refreshDetails()
{
    if (!m_details)
        return;
    m_details->clear();
    const auto detail = [this](const QString &name, const QString &value) {
        auto *item = new QTreeWidgetItem(m_details, {name, value.isEmpty() ? unavailable() : value});
        item->setToolTip(1, item->text(1));
    };
    detail(tr("Name"), m_data.displayName);
    detail(tr("Type"), m_data.typeName);
    if (m_items.size() == 1) {
        detail(tr("Location"), url().toDisplayString(QUrl::PreferLocalFile));
        detail(tr("Owner"), m_data.owner);
        detail(tr("Group"), m_data.group);
        detail(tr("Permissions"), QString::number(m_data.permissions & 07777, 8).rightJustified(4, QLatin1Char('0')));
    } else {
        detail(tr("Items"), QLocale().toString(m_items.size()));
    }
    if (m_data.kind == PropertiesData::Kind::Drive) {
        detail(tr("File system"), m_data.fileSystem);
        detail(tr("Mount point"), m_data.mountPoint);
        detail(tr("Device"), m_data.device);
        detail(tr("Used space"), byteText(m_data.usedSize));
        detail(tr("Free space"), byteText(m_data.availableSize));
        detail(tr("Capacity"), byteText(m_data.totalSize));
    } else {
        detail(tr("MIME type"), m_data.mimeType);
        detail(tr("Size"), m_size ? m_size->text() : byteText(m_data.size));
        detail(tr("Size on disk"), m_sizeOnDisk ? m_sizeOnDisk->text() : unavailable());
        if (m_contains)
            detail(tr("Contains"), m_contains->text());
        if (m_items.size() == 1) {
            detail(tr("Created"), dateText(m_data.created));
            detail(tr("Modified"), dateText(m_data.modified));
            detail(tr("Accessed"), dateText(m_data.accessed));
            detail(tr("Hidden"), m_data.hidden ? tr("Yes") : tr("No"));
        }
    }
}

void PropertiesDialog::stopSizeCalculation()
{
    ++m_allocationGeneration;
    if (m_allocationCancel)
        m_allocationCancel->store(true, std::memory_order_relaxed);
    m_allocationCancel.reset();
    if (m_allocationWatcher) {
        disconnect(m_allocationWatcher, nullptr, this, nullptr);
        // QFutureWatcher does not wait for the worker when destroyed. The
        // worker owns only its path snapshot and cancellation token.
        m_allocationWatcher->deleteLater();
        m_allocationWatcher.clear();
    }
    for (const QPointer<KJob> &job : std::as_const(m_sizeJobs)) {
        if (job) {
            disconnect(job, nullptr, this, nullptr);
            job->kill(KJob::Quietly);
        }
    }
    m_sizeJobs.clear();
    m_sizesRemaining = 0;
}

void PropertiesDialog::startSizeCalculation()
{
    stopSizeCalculation();
    if (!m_contains)
        return;
    m_totalBytes = m_totalFiles = m_totalFolders = 0;
    m_sizeIncomplete = false;
    m_allocationResult = {};
    QStringList paths;
    m_allocationAvailable = std::all_of(m_items.cbegin(), m_items.cend(),
                                      [](const KFileItem &item) { return item.url().isLocalFile(); });
    if (m_allocationAvailable) {
        for (const KFileItem &item : std::as_const(m_items))
            paths.append(item.url().toLocalFile());
        m_allocationCancel = std::make_shared<std::atomic_bool>(false);
        auto *watcher = new QFutureWatcher<FolderAllocation::Result>(this);
        m_allocationWatcher = watcher;
        const quint64 generation = m_allocationGeneration;
        connect(watcher, &QFutureWatcher<FolderAllocation::Result>::finished, this,
                [this, watcher, generation] {
            if (generation != m_allocationGeneration || m_allocationWatcher != watcher)
                return;
            m_allocationResult = watcher->result();
            m_allocationWatcher.clear();
            m_allocationCancel.reset();
            watcher->deleteLater();
            updateSizeLabels();
            refreshDetails();
        });
        const auto cancel = m_allocationCancel;
        watcher->setFuture(QtConcurrent::run([paths, cancel] {
            return FolderAllocation::calculate(paths, cancel);
        }));
    }
    for (const KFileItem &item : std::as_const(m_items)) {
        if (!item.isDir() || item.isLink()) {
            ++m_totalFiles;
            m_totalBytes += quint64(item.size());
            continue;
        }
        // DirectorySizeJob includes directory inode sizes. Windows' Size is
        // the sum of file lengths, so accumulate KIO's recursive listing.
        auto *job = KIO::listRecursive(item.url(), KIO::HideProgressInfo);
        job->setUiDelegate(nullptr);
        m_sizeJobs.append(job);
        ++m_sizesRemaining;
        connect(job, &KIO::ListJob::entries, this,
                [this](KIO::Job *, const KIO::UDSEntryList &entries) {
            for (const KIO::UDSEntry &entry : entries) {
                const QString name = entry.stringValue(KIO::UDSEntry::UDS_NAME);
                if (name == QLatin1String(".") || name == QLatin1String(".."))
                    continue;
                if (entry.isDir())
                    ++m_totalFolders;
                else {
                    ++m_totalFiles;
                    m_totalBytes += quint64(qMax<qint64>(0, entry.numberValue(KIO::UDSEntry::UDS_SIZE, 0)));
                }
            }
            updateSizeLabels();
        });
        connect(job, &KIO::ListJob::subError, this, [this](KIO::ListJob *, KIO::ListJob *) {
            m_sizeIncomplete = true;
        });
        connect(job, &KJob::result, this, [this](KJob *finished) {
            m_sizeIncomplete |= finished->error() != 0;
            --m_sizesRemaining;
            updateSizeLabels();
            refreshDetails();
        });
    }
    updateSizeLabels();
    refreshDetails();
}

void PropertiesDialog::updateSizeLabels()
{
    if (!m_size)
        return;
    if (m_contains) {
        const QString suffix = m_sizesRemaining > 0 ? tr(" (calculating...)")
            : m_sizeIncomplete ? tr(" (partial)") : QString();
        setValue(m_size, byteText(m_totalBytes) + suffix);
        setValue(m_contains, tr("%1 Files, %2 Folders")
                     .arg(QLocale().toString(m_totalFiles), QLocale().toString(m_totalFolders)) + suffix);
        setValue(m_sizeOnDisk, m_allocationWatcher ? tr("Calculating...")
                     : m_allocationAvailable ? byteText(m_allocationResult.bytes)
                         + (m_allocationResult.incomplete ? tr(" (partial)") : QString())
                     : unavailable());
    } else {
        setValue(m_size, byteText(m_data.size));
        setValue(m_sizeOnDisk, m_data.allocatedSizeKnown ? byteText(m_data.sizeOnDisk) : unavailable());
    }
}

bool PropertiesDialog::isDirty() const
{
    return (m_data.canRename && m_name->text() != m_data.realName)
        || (m_readOnly && m_readOnly->isEnabled() && m_readOnly->isChecked() != m_data.readOnly)
        || bool(m_pendingService);
}

void PropertiesDialog::updateDirtyState()
{
    if (m_buttons)
        m_buttons->button(QDialogButtonBox::Apply)->setEnabled(!m_busy && isDirty());
}

void PropertiesDialog::setBusy(bool busy)
{
    m_busy = busy;
    m_tabs->setEnabled(!busy);
    m_buttons->button(QDialogButtonBox::Ok)->setEnabled(!busy);
    m_buttons->button(QDialogButtonBox::Cancel)->setEnabled(!busy);
    updateDirtyState();
}

void PropertiesDialog::showError(const QString &message)
{
    m_error->setText(message);
    m_error->setVisible(!message.isEmpty());
}

void PropertiesDialog::accept()
{
    if (!m_busy)
        applyChanges(true);
}

void PropertiesDialog::reject()
{
    if (!m_busy)
        QDialog::reject();
}

void PropertiesDialog::applyChanges(bool closeAfter)
{
    if (m_busy)
        return;
    showError({});
    if (!isDirty()) {
        if (closeAfter)
            QDialog::accept();
        return;
    }
    m_closeAfterApply = closeAfter;
    m_applyReadOnly = m_readOnly && m_readOnly->isEnabled()
        && m_readOnly->isChecked() != m_data.readOnly;
    m_requestedReadOnly = m_readOnly && m_readOnly->isChecked();
    if (m_data.canRename && m_name->text() != m_data.realName) {
        const QString name = m_name->text();
        if (name.isEmpty() || name == QLatin1String(".") || name == QLatin1String("..")
            || name.contains(QLatin1Char('/')) || name.contains(QChar::Null)) {
            showError(tr("Enter a file name without a slash."));
            m_name->setFocus();
            return;
        }
        const QUrl oldUrl = url();
        QUrl destination = KIO::upUrl(oldUrl);
        QString destinationPath = destination.path();
        if (!destinationPath.endsWith(QLatin1Char('/')))
            destinationPath.append(QLatin1Char('/'));
        destination.setPath(destinationPath + name);
        const QFileInfo destinationInfo(destination.toLocalFile());
        if (destinationInfo.exists() || destinationInfo.isSymLink()) {
            showError(tr("An item named “%1” already exists. Choose another name.").arg(name));
            m_name->setFocus();
            return;
        }
        setBusy(true);
        stopSizeCalculation();
        auto *job = KIO::moveAs(oldUrl, destination, KIO::HideProgressInfo);
        // A property name edit must never replace another item. Without a UI
        // delegate a collision that races the preflight fails instead of
        // offering an overwrite dialog.
        job->setUiDelegate(nullptr);
        m_changeJob = job;
        KIO::FileUndoManager::self()->recordCopyJob(job);
        connect(job, &KJob::result, this, [this, oldUrl, destination](KJob *finished) {
            m_changeJob.clear();
            if (finished->error()) {
                setBusy(false);
                showError(finished->errorString());
                startSizeCalculation();
                return;
            }
            m_items.first().setUrl(destination);
            m_items.first().refresh();
            m_data = PropertiesData::fromItems(m_items);
            Q_EMIT renamed(oldUrl, destination);
            applyPermissions();
        });
    } else {
        setBusy(true);
        applyPermissions();
    }
}

void PropertiesDialog::applyPermissions()
{
    if (!m_applyReadOnly) {
        finishApply();
        return;
    }
    const int requested = m_requestedReadOnly ? 0 : S_IWUSR;
    // The mask leaves group/other, execute, special and all other bits alone.
    auto *job = KIO::chmod(m_items, requested, S_IWUSR, {}, {}, false, KIO::HideProgressInfo);
    job->setUiDelegate(nullptr);
    m_changeJob = job;
    connect(job, &KJob::result, this, [this](KJob *finished) {
        m_changeJob.clear();
        if (finished->error()) {
            setBusy(false);
            showError(finished->errorString());
            m_applyReadOnly = false;
            startSizeCalculation();
            return;
        }
        finishApply();
    });
}

void PropertiesDialog::finishApply()
{
    if (m_pendingService) {
        KApplicationTrader::setPreferredService(m_data.mimeType, m_pendingService);
        m_pendingService.reset();
    }
    m_applyReadOnly = false;
    refresh();
    setBusy(false);
    Q_EMIT applied(url());
    if (m_closeAfterApply)
        QDialog::accept();
}

void PropertiesDialog::chooseApplication()
{
    // The MIME-type constructor can save an association on its own. The
    // generic picker lets the property sheet commit it only on OK or Apply.
    KOpenWithDialog chooser(this);
    chooser.setWindowTitle(tr("Open with"));
    chooser.setSaveNewApplications(false);
    if (chooser.exec() != QDialog::Accepted)
        return;
    if (!chooser.service()) {
        showError(tr("Choose an installed application to use for this file type."));
        return;
    }
    const KService::Ptr current = KApplicationTrader::preferredService(m_data.mimeType);
    m_pendingService = current && current->storageId() == chooser.service()->storageId()
        ? KService::Ptr() : chooser.service();
    refreshAssociation();
    updateDirtyState();
}

void PropertiesDialog::openDesktopProperties(bool permissionsPage)
{
    auto *dialog = new DesktopPropertiesDialog(m_items, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowModality(Qt::WindowModal);
    if (permissionsPage)
        dialog->selectPermissionsPage();
    connect(dialog, &KPropertiesDialog::applied, this, [this, dialog] {
        const QUrl oldUrl = url();
        const QString draftName = m_name->text();
        const bool nameEdited = m_data.canRename && draftName != m_data.realName;
        const bool oldReadOnly = m_data.readOnly;
        const bool draftReadOnly = m_readOnly && m_readOnly->isChecked();
        const bool readOnlyEdited = m_readOnly && m_readOnly->isEnabled()
            && draftReadOnly != oldReadOnly;
        if (m_items.size() == 1 && dialog->url().isValid())
            m_items.first().setUrl(dialog->url());
        // Refresh committed metadata without discarding unrelated draft edits.
        // A rename or owner-write change committed in the full editor wins
        // over an earlier draft of that same property.
        refresh();
        if (nameEdited && oldUrl.matches(url(), QUrl::StripTrailingSlash))
            m_name->setText(draftName);
        if (readOnlyEdited && m_data.readOnly == oldReadOnly)
            m_readOnly->setChecked(draftReadOnly);
        updateDirtyState();
        if (oldUrl.isValid() && oldUrl != url())
            Q_EMIT renamed(oldUrl, url());
        Q_EMIT applied(url());
    });
    dialog->show();
}
