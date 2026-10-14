# Windows 7 Explorer reference notes

## Reference environment

Upstream base: `613320c5237d708f539840f699de9a34990108ee`.
Development branch: `windows7-fidelity`.

Inspected Windows 7 Ultimate SP1 x64 from the user's disc image, installed in an
offline QEMU/KVM VM. Guest: 1 CPU, 1536 MB RAM, 800 × 600 display, standard VGA,
normal-size text. The VM was shut down through Windows after the reference pass.

**Graphics caveat:** this VM uses the Basic frame, not accelerated Aero Glass.
The client layout, menus, view switching, selection, address bar, and preview
behavior were observed directly. Aero transparency, animation timing, and window
shadows are not claimed to have been verified here.

The local reference captures are in `~/VMs/windows7/reference/`. They are not
bundled with the application. No icons, DLL resources, fonts, or other Microsoft
assets were extracted from the ISO for this fork.

## Window anatomy and spacing

- No ribbon. Navigation/address/search row, command strip, white body, and a
  full-width pale-blue details pane.
- Back and Forward are round buttons; a small history dropdown follows them.
  No permanent Up button. Up remains a keyboard command.
- Address and search fields are about 24 logical pixels tall. The search field
  names the current location and uses an italic placeholder.
- The command strip is roughly 30 px tall. Organize is first; contextual commands
  precede New folder. View split button, preview toggle, and Help are at the right.
- The navigation pane in this guest window is about 158 px wide. Icons are 16 px;
  rows are about 21 px high. The first heading has about 10 px above it, and
  groups are separated by a larger, roughly 20 px blank row.
- The bottom details pane is about 54 px tall and spans the navigation pane too.
  It has a 48 px icon. It is distinct from the optional classic status bar.

## Libraries and navigation pane

Default Explorer opens to Libraries, not Computer. The landing page has a green
Libraries heading, an explanatory blue line, and four 48 px tiles:
Documents, Music, Pictures, Videos. Each tile has a second line, Library. The Libraries gallery has approximately
256 px column spacing and 64 px rows, separate from ordinary folder tiles;
a faint horizontal rule sits below the instruction.

The left tree starts with Favorites (Desktop, Downloads, Recent Places), then
Libraries and its four children, Computer, and Network. Favorites and Libraries
start expanded; Computer and Network start collapsed. Expanders fade into view
when the pointer enters the navigation pane. A heading such as Libraries is also
a selectable destination, not merely an expand/collapse button.

A real library root shows a green Documents library heading and an Includes
locations line above the file list. In the Windows guest, Documents includes
the personal and Public documents locations. Libraries are shell objects, not
ordinary writable directories.

**Linux adaptation:** this version's four entries are shortcuts to the actual
XDG user folders. The landing page is synthetic and never reaches a KIO write
operation. It does not aggregate multiple directories or implement .library-ms
files. Disabled XDG folders pointing at HOME are omitted. The library root
header reports one location, honestly.

## Breadcrumb/address behavior

Each named segment is an independently clickable location with a separate
child-folder dropdown. The last/current folder also has a dropdown. There is no
web-style underline on hover. Clicking a segment's arrow does not navigate.

When space is tight, ancestors disappear from the left and a leading double
chevron exposes them. The last segment stays visible instead of forcing the
window wider. An address-edit shortcut selects the path; Escape cancels.
Alt+D and F4 were verified in this guest. Ctrl+L is retained as a Linux-friendly
convenience, not claimed as observed Windows 7 behavior.

Reference: 13-crumb-menu.png; 04-folder-details.png.

## Views: button vs. flyout

The split button has two distinct behaviors:

- **Main half:** verified cycle Large icons → List → Details → Tiles → Content →
  Large icons. It does not cycle through all eight modes.
- **Arrow half:** opens a tall, narrow flyout with a vertical slider on the left
  and icon/text choices on the right. All eight choices are available:
  Extra large icons, Large icons, Medium icons, Small icons, List, Details,
  Tiles, Content.

In Windows the slider has intermediate icon-size positions and uneven spacing
between the named stops. The Linux flyout currently uses eight discrete stops;
continuous icon sizing remains a fidelity gap.

Selection persists when changing views. List fills downward and then into
columns; Small icons puts icon and text side by side rather than underneath.
Large icons have much less vertical padding than the upstream implementation.

Details column order: Name, Date modified, Type, Size. Folders sort before files.
Folder sizes are blank. Headers can be resized/reordered. Tiles use a 48 px icon
with name/type/size to its right. Content uses a 32 px icon, a slightly larger
name at the left, date and size at the right, with thin horizontal row rules.

Reference: 05-view-popup.png; 19-view-cycle-1.png through 23-view-cycle-5.png.

## Selection, rename, and details

- Single click selects; double click opens. Navigation-tree destinations open on
  single click. Return also opens the selected item.
- Active selection is a subtle blue highlight with dark text. Inactive selection
  is grey. Details selects a full row, not individual cells.
- F2 enters inline rename and preselects the base name. Escape does not rename.
- With no selection, the bottom pane reports only the item count; it does not
  permanently show the current volume's free space.
- A selected folder shows its name/type and modification date.
- A selected text file shows name/type, modification date, creation date, and
  size in separate aligned fields. Linux creation time is shown only when KIO
  actually supplies it; a missing birth time is not fabricated.
- Computer's unselected footer describes the machine, not a count of drives.
  Drive selection shows its type and capacity information.

The Windows guest hides known extensions by default. This fork intentionally
keeps upstream's safer Linux default of showing extensions; Folder Options can
change it. File operation identities always use real URLs/names, not captions.

Reference: 16-text-file-selected.png; 17-text-file-rename.png.

## Preview pane

The toolbar preview button and Alt+P toggle the same state. The pane is a
resizable part of the body; the details pane below it still spans the whole
window. With nothing selected, Select a file to preview. is centered in the
empty pane.

A text preview is actual read-only content at the top-left, not a large file
icon with a duplicate filename/type/size card. Those details remain below.
The Linux implementation reads bounded text off the UI thread, handles BOMs,
and ignores stale asynchronous results after selection changes. Other formats
still depend on installed KIO thumbnail providers, rather than Windows preview
handlers. Native document/PDF/audio preview-handler parity is not implemented.

Reference: 02-preview.png; 18-text-preview.png.

## Menus and command strip

Organize begins Cut, Copy, Paste; Undo; Select all; Layout; Folder and search
options; Delete, Rename, Properties; Close. Layout has Menu bar, Details pane,
Preview pane, Navigation pane. Disabled commands remain visibly disabled.
Do not add fake functional commands just to fill out the Windows menu.

Alt reveals the classic File, Edit, View, Tools, Help menu above the command
strip. The background context menu is different from a selected-item menu:
View, Sort by, Group by, Refresh, customization/sharing, New, Properties.
Computer has different contextual commands (System properties, uninstall/change
a program, network drive mapping, Control Panel). At a narrow width, commands
move into a chevron overflow rather than pushing the fixed right-side controls
offscreen.

Sharing, mounting, permissions, and file operations in this fork remain KDE/KIO
operations. Windows ACLs, HomeGroup, indexed shell search, and library management
are not simulated with dead buttons. The five classic top-level menus are
retained; Linux-specific destinations are grouped under View/Go to. F1 uses a
modeless local quick-help fallback, not the Windows Help and Support runtime.

Reference: 03-computer.png; 09-organize.png; 14-background-menu.png;
24-classic-menu.png.

## UX Guide follow-up

See [the separate checklist](uxguide-checklist.md) for how the official Win7-era
recommendations support this reference. They do not replace the observed UI
with a modern redesign. The follow-up includes guest-width tile reflow,
full-row library Details selection, default Favorites parity, fresh-window
screen fitting, keyboard focus fixes, display scaling, and real KIO file-operation
regression checks.

## Remaining fidelity work

- Multi-location libraries and a real library-management UI.
- Continuous view-slider sizing and more exact stop spacing.
- More native preview formats and editable file metadata.
- Exact font/icon fidelity depends on the user's installed theme and fonts.
- Aero Glass frame/shadows must be compared on a graphics-capable Windows VM
  separately; this Basic-frame reference cannot establish them.
