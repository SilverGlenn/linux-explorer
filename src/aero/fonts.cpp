#include "fonts.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>
#include <QSet>
#include <QStandardPaths>
#include <QToolTip>
#include <QWidget>

namespace {
QFont font(const QStringList &families, qreal pointSize)
{
    QFont result;
    result.setFamilies(families);
    result.setPointSizeF(pointSize);
    result.setWeight(QFont::Normal);
    result.setItalic(false);
    return result;
}
} // namespace

QFont Aero::shellFont(qreal pointSize)
{
    return font({QStringLiteral("Segoe UI"), QStringLiteral("Noto Sans"),
                 QStringLiteral("DejaVu Sans")}, pointSize);
}

QFont Aero::classicDialogFont(ClassicFont family)
{
    return font({family == ClassicFont::Tahoma ? QStringLiteral("Tahoma")
                                             : QStringLiteral("Microsoft Sans Serif"),
                 QStringLiteral("Arial"), QStringLiteral("Liberation Sans")}, 8);
}

QFont Aero::textPreviewFont()
{
    // The native text preview reports CHARFORMAT yHeight=195 twips: 9.75pt.
    QFont result = font({QStringLiteral("Lucida Console"), QStringLiteral("Courier New"),
                         QStringLiteral("DejaVu Sans Mono")}, 9.75);
    result.setStyleHint(QFont::Monospace);
    return result;
}

QString Aero::referenceFontDirectory()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
        + QStringLiteral("/linux-explorer-win7/reference-fonts");
}

int Aero::loadReferenceFonts(const QString &directory)
{
    const QDir folder(directory.isEmpty() ? referenceFontDirectory() : directory);
    static QSet<QString> loaded;
    int count = 0;
    for (const char *name : {"segoeui.ttf", "segoeuib.ttf", "segoeuii.ttf", "segoeuiz.ttf",
                             "micross.ttf", "tahoma.ttf", "lucon.ttf"}) {
        const QString path = QFileInfo(folder.filePath(QLatin1String(name))).canonicalFilePath();
        if (path.isEmpty() || loaded.contains(path)) continue;
        if (QFontDatabase::addApplicationFont(path) >= 0) {
            loaded.insert(path);
            ++count;
        }
    }
    return count;
}

void Aero::configureFonts(QApplication *app)
{
    const QFont shell = shellFont();
    app->setFont(shell);
    // Aero and desktop platform themes set class fonts independently of the
    // application font. Pin the shell roles after the native style is installed.
    for (const char *name : {"QWidget", "QDialog", "QLabel", "QMenu", "QMenuBar",
                             "QAbstractButton", "QPushButton", "QToolButton", "QCheckBox",
                             "QRadioButton", "QGroupBox", "QTabWidget", "QTabBar", "QComboBox",
                             "QLineEdit", "QAbstractItemView", "QTreeView", "QListView",
                             "QTableView", "QHeaderView", "QStatusBar", "QTextEdit",
                             "QPlainTextEdit", "QTextBrowser", "QDialogButtonBox"})
        app->setFont(shell, name);
    QToolTip::setFont(shell);
}

void Aero::applyClassicDialogFont(QWidget *dialog, ClassicFont family)
{
    const QFont classic = classicDialogFont(family);
    dialog->setFont(classic);
    // Controls are frequently constructed without a parent; their desktop class
    // font can survive reparenting. Set every sheet control explicitly as well.
    for (QWidget *control : dialog->findChildren<QWidget *>())
        if (control->window() == dialog) control->setFont(classic);
}
