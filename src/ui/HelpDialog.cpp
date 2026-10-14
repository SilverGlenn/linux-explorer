#include "HelpDialog.h"
#include "aero/text.h"

#include <QDialogButtonBox>
#include <QTextBrowser>
#include <QVBoxLayout>

HelpDialog::HelpDialog(QWidget *parent) : QDialog(parent)
{
    setObjectName(QStringLiteral("explorerHelp"));
    setWindowTitle(tr("File Explorer Help"));
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
    setAttribute(Qt::WA_DeleteOnClose);
    setMinimumSize(420, 320);
    resize(560, 440);
    Aero::setPointSize(this, 9);

    auto *layout = new QVBoxLayout(this);
    auto *content = new QTextBrowser(this);
    content->setObjectName(QStringLiteral("explorerHelpContent"));
    content->setAccessibleName(tr("File Explorer help"));
    content->setHtml(tr(R"(
        <h1 style="color:#006600; font-size:16pt; font-weight:normal">Browse files and folders</h1>
        <p>Double-click a file or folder to open it. To open a library, select
        Documents, Music, Pictures, or Videos in the navigation pane.
        These libraries use your Linux user folders; they do not combine
        multiple locations.</p>
        <p>Use Back and Forward to revisit folders. Click a name in the Address
        bar to go to that location, or its arrow to choose a subfolder.
        Press Alt+D to type a path.</p>
        <h2 style="font-size:11pt">Change how files are shown</h2>
        <p>Click the view button to cycle through Large icons, List, Details,
        Tiles, and Content. Click its arrow to choose any view directly.
        In Details view, click a column heading to sort and drag its edge
        to change the width.</p>
        <h2 style="font-size:11pt">Copy, move, or rename a file</h2>
        <p>Inside a folder, select the items and use Organize or the right-click
        menu. Use Copy and Paste to make copies, or Cut and Paste to move them.
        Rename selects the name without its extension. Escape cancels a rename.
        Delete normally moves items to the Recycle Bin; Shift+Delete removes
        them permanently. Undo reverses supported file operations.</p>
        <h2 style="font-size:11pt">Preview or find a file</h2>
        <p>Press Alt+P to show or hide the preview pane, then select one file.
        Text is read-only and limited to the first 256 KiB. Other previews
        depend on installed KDE thumbnail providers.</p>
        <p>Type in Search to filter the current folder. Press Enter to search
        subfolders if enabled in Folder and search options. Recursive search
        requires kio-extras. Search is not available on the synthetic Libraries
        and Computer pages.</p>
        <h2 style="font-size:11pt">Use the keyboard</h2>
        <table cellpadding="4">
          <tr><td>Alt+Left / Alt+Right</td><td>Back / forward</td></tr>
          <tr><td>Alt+Up</td><td>Up one level</td></tr>
          <tr><td>Alt+D</td><td>Edit the Address bar</td></tr>
          <tr><td>Ctrl+E</td><td>Focus Search</td></tr>
          <tr><td>F6 / Shift+F6</td><td>Cycle visible panes</td></tr>
          <tr><td>F2 / F5</td><td>Rename / refresh</td></tr>
          <tr><td>Alt+P</td><td>Show or hide preview</td></tr>
          <tr><td>Ctrl+Shift+1…8</td><td>Choose a view</td></tr>
          <tr><td>Alt</td><td>Show or hide the classic menu bar</td></tr>
        </table>
    )"));
    layout->addWidget(content);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
    layout->addWidget(buttons);
}
