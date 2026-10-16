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

## Refinement pass 2

The fresh offline Basic-frame evidence is in
`~/VMs/windows7/reference-pass2/25-folder-baseline.png` through
`40-picture-folder.png`. The VM remains shut down. In particular:

- `26-crumb-current-hover.png`, `27-crumb-arrow-hover.png`, and
  `28-crumb-pressed-popup.png`: one shared split-segment highlight and a retained
  popup-open state, without link underlines.
- `29-selected-directory-footer.png`: name, File folder, and Date modified;
  no creation date or recursive directory size.
- `30-details-header-normal.png`, `33-header-hover.png`, and
  `34-header-pressed.png`: Details header state feedback.
- `35-search-focused-empty.png` and `36-search-typed.png`: focused-empty cue
  hidden, with stable small search text. Windows' search-filter suggestion flyout
  is not implemented by this Linux folder filter.
- `37-sidebar-documents.png` and `38-sidebar-expanded.png`: selection spans the
  navigation pane, not just the text label, but stops at the pane boundary.
- `39-picture-thumbnails.png` and `40-picture-folder.png`: real cold-folder image
  thumbnails rather than generic picture icons.

The completed components use native line editing with 9 pt search typography,
shared breadcrumb split chrome, bounded sidebar/header painting, asynchronous
KIO thumbnails with cache/stale-result checks, and flexible footer metadata.
Integration regressions verify these hooks in a real MainWindow, including
QSignalBlocker search-URL restore and dismissal, action navigation, and external
navigation during the actual breadcrumb QMenu's lifetime. External navigation
now closes the old menu before replacing the trail.

Navigation height is owned by AeroQt's sizeHint/inset contract, not a conflicting
fixed-height clamp. The earlier source-image estimate was 35 px plus a 2 px inset;
the **loaded runtime on this host measures 34 px and 36 px**, respectively, in
both Fusion and AeroQt/Kvantum. Tests derive the row expectation from the loaded
NavButtons height plus layout margins, require geometry == sizeHint, and require
the inset/central top == sizeHint + 2. No installed AeroQt code was patched to
force the estimate.

Validation on Qt 6.10.2: all five Fusion suites pass; all five suites pass at each
of 800×600/100%, 1024×768/125%, and 1200×900/150%; and all five native-widget-theme
runs pass. Full UI/address explicitly register AeroQt over Windows7Aero Kvantum;
the remaining three suites use Kvantum only. A fractional-DPR test correction
maps logical border coordinates into grabbed image pixels and routes the arrow
hover through QPA, rather than sampling transparent padding or relying on Qt's
offscreen cursor conversion. Assertions of shared-border feedback are retained.

Local whole-client captures are under `build/screenshots-refinement/` and
`build/screenshots-refinement-themed/`, including `selected-folder.png`,
`narrow-selected-folder.png`, `search-focused-empty.png`, and
`search-focused-input.png`. Matrix images/logs are in `build/screenshots-96dpi/`,
`build/screenshots-120dpi/`, `build/screenshots-144dpi/` and `build/test-*.log`.
These are QWidget grabs of the complete client, **not native framed/glass visual
acceptance**. Narrow footer fields intentionally elide; their full accessible
names and tooltips remain available.

## Properties reference pass

The Windows 7 VM was started in temporary QEMU snapshot mode for this pass and
cleanly shut down afterward. The ten captures are local development evidence in
`build/references/windows7-properties/`; the VM disk and configuration were not
changed persistently. The guest used Basic frames at normal text size.

The folder General sheet has an approximately 361 × 449 px client, the file
sheet 361 × 477 px, and drive sheet 361 × 467 px. General content is white,
with a 32 px icon beside the name, left-aligned labels and values about 73 px
apart, horizontal separators, and OK/Cancel/Apply buttons at the bottom right.
Folder metadata rows have about 26 px pitch. Drive General uses blue/magenta
used/free-space keys, byte and human-readable capacity values, and an oval pie.

The Linux sheet exposes supported General, Security, and Details information.
KIO still performs changes, directory-size queries, and full permission editing.
The UI-only system-drive caption does not change the volume or mount name.

### Properties fonts

A follow-up queried `WM_GETFONT`/`LOGFONT` on the native General, Security and
Details pages, their text fields, lists, headers, tabs and footer buttons. All
request `MS Shell Dlg`, height **−11 logical pixels**, weight **400**, nonitalic.
At 96 DPI this is the rounded **8 pt** classic dialog font. The guest's
`FontSubstitutes` registry maps `MS Shell Dlg` to **Microsoft Sans Serif**;
`MS Shell Dlg 2` maps to Tahoma and is not the font these controls request.
Text-only evidence is in
`build/references/windows7-properties-fonts/native-control-fonts.txt`.

The sheet previously inherited Explorer's Segoe UI 9 pt. It now requests
Microsoft Sans Serif 8 pt, with Arial/Liberation Sans fallbacks. Its tabs, fields,
labels, lists and buttons receive explicit fonts because Aero and desktop style
class defaults can survive reparenting independently of the dialog's font.
The caption remains system-managed. The footer uses Windows
button order even when the desktop platform theme supplies another order.

For this user's local installation, the reference font comes from their own
VM and is installed in their user font directory. It is not a repository asset
and is not bundled with application packages. The snapshot VM was shut down
cleanly and its base disk was unchanged.

### Whole-application font audit

The subsequent VM pass queried system menu/message fonts and native control
fonts. DirectUI headings were compared with native GDI text widths where no
font handle was exposed. Full evidence, including those inference limits, is in
`build/references/windows7-app-fonts/README.md`.

| Surface | Windows 7 font |
| --- | --- |
| Menus, submenus, sidebar, address/search, command bar, lists/headers, status | Segoe UI 9 pt regular |
| Default menu command | Segoe UI 9 pt bold |
| Computer/file group headings and Content file/drive names | Segoe UI 11 pt regular |
| Green Libraries and individual-library headings | Segoe UI 14 pt regular |
| Map Network Drive main instruction | Segoe UI 12 pt regular |
| Folder Options, all three tabs | Microsoft Sans Serif 8 pt regular |
| About body and button | Tahoma 8 pt regular |
| Plain-text preview | Lucida Console 9.75 pt regular (195 twips, 13 px at 96 DPI) |

Search placeholders are italic; entered text is regular. Libraries tile labels
remain 9 pt; their green headings now use the measured 14 pt size. Modern Linux-specific task dialogs retain the
shell's Segoe UI roles. The official Google sign-in artwork is unchanged.

The installed ReactXP Segoe UI was version 5.55; the guest's regular face is
5.01. Private user-supplied regular, bold, italic, Microsoft Sans Serif, Tahoma,
and Lucida Console files live in
`$XDG_DATA_HOME/linux-explorer-win7/reference-fonts`. Qt loads them for this app
before selecting shell fonts, so existing desktop font choices and other apps
are unaffected. No Microsoft fonts are repository or package assets. Hosts
without these optional files use the installed family or declared fallbacks.

The app pins shell class fonts after installing the Aero style, and context
menus set their own fonts recursively, including submenus added on demand.
Tests inspect rendered menu style options, every classic-dialog tab, preview
character formatting, and the raw font name tables of selected faces; a
matching family name alone does not establish that the reference face loaded.

## Context menu follow-up

Ordinary and Shift item-menu captures were taken in an offline snapshot VM,
then Windows was shut down cleanly. Local screenshots are in
`build/references/windows7-context/`. The ordinary folder menu begins with bold
Open and Open in new window. Send to precedes Cut/Copy; Create shortcut,
Delete, and Rename form the next group; Properties is last. Copy as path is
present in the Shift menu. File application choices use an Open with submenu.

The Linux menu retains these command groups. Desktop service extensions stay
inside More options, rather than appending application commands, tagging
controls, or widget rows to the main menu. Unsupported shell commands are not
displayed as placeholders. Context captions omit shortcut text; the original
actions retain their keyboard bindings.

### Context menu geometry at 100% scaling

A fresh offline snapshot pass on 2026-10-16 verified **Smaller – 100% (default)**
in the Windows Display control panel. The guest display was 1280 × 768 with
Basic frames. Full captures, border-exact crops and machine-readable measurements
are in `build/references/windows7-context-sizing/`; popup sizes below exclude
their drop shadows.

| Ordinary Explorer menu | Width × height (px) | Capture |
| --- | --- | --- |
| Folder | 250 × 329 | `06-folder-menu.png` |
| Text file | 223 × 332 | `05-file-menu.png` |
| Folder background | 250 × 285 | `04-background-menu.png` |
| Same folder through Documents library, background | 165 × 266 | `20-library-background-menu.png` |
| Send to child | 222 × 138 | `16-send-submenu.png` |
| Include in library child | 171 × 124 | `13-folder-send-hover.png` |
| Share with child | 225 × 102 | `11-folder-sync-hover.png` |

Normal commands allocate exactly **22 px vertically**. Separators allocate
**8 px**, with a grey/white two-pixel rule at offsets 4 and 5. The installed
Shared Folder Synchronization shell extension uses a shorter 19 px item; it is
not the standard command-row height. Hover captures establish these bounds,
rather than estimating height from glyph baselines.

The menu has a **3 px frame/content inset**. The icon gutter fills horizontal
offsets 3–28 from the left edge, followed by a grey divider at 29 and a white
edge at 30. Text starts at **35 px**. Hover spans from left+3 to right−3 and
includes the gutter. Separator rules start at left+30. Submenu arrows occupy
roughly four pixels horizontally, with their rightmost ink 12 px before the
right border.

Width follows content: the root captures leave approximately **55 px after
the longest label**, in addition to the 35 px left inset. Longest label advance
plus approximately **90 px** reproduces their widths; a universal 224/250 px
minimum does not. The library background is narrower because it omits the long
Shared Folder Synchronization command. Simple Send to/Include in library child
menus leave approximately 40 px after their longest label (about 75 px total
overhead); shell-provided child-menu sizing can vary.

## Native decoration dependency

The app's transparent navigation row does not own the compositor's caption or
inner-border artwork. SMOD's default caption/client boundary remains a separate
integration dependency. Removing the navigation height clamp cannot remove it.
Do not compensate with frameless window-control clones or remove real native
controls.

For a deliberately approved SMOD configuration change only:

1. Back up the user's `smodrc` and inspect the existing exception order before
   editing. Preserve every unrelated rule and setting. SMOD uses the first
   matching enabled exception, so check for an earlier broader match.
2. Add an enabled **window-class** exception matching the exact whitespace-delimited
   browser token with `(^|\s)linux-explorer-win7($|\s)`. Do not use an unanchored
   substring: `linux-explorer-win7-dialog` must not match. This also accommodates
   a composite resource-name/class string.
3. For this browser exception only set `HideInnerBorder=true`,
   `HideCaption=true`, `HideIcon=true`, and **`HideTitleBar=false`**. The real
   titlebar controls, move/resize, and system menu must remain usable. Follow
   SMOD's configuration UI/version-specific exception format; these are not
   global Windeco defaults.
4. After user approval and deliberate reload/relaunch, validate a positively
   identified preview browser in Libraries or a synthetic non-personal folder,
   restored/maximized and active/inactive, with menus open/closed; confirm the
   dialog class remains unaffected. Only capture that identified window.

The application does not apply this rule automatically. It was subsequently
applied manually on the development desktop after explicit user approval, and the
user accepted the resulting appearance. This is a host-specific SMOD integration,
not proof of Windows Aero Glass parity. The identified application captures are
`build/native-frame-before.png` and `build/native-frame-after.png`; transparent
compositor pixels can appear black in a black-backed viewer. No Microsoft frame
resources were imported and no installed theme code was patched.

The file picker uses the separate desktop identity
`linux-explorer-win7-chooser-dialog`. To let its transparent navigation row meet
the native titlebar without a second inner frame, its approved SMOD exception
matches `(^|\s)linux-explorer-win7-chooser-dialog($|\s)` and sets
`HideInnerBorder=true`, `HideCaption=false`, `HideIcon=false`, and
`HideTitleBar=false` (`ExceptionType=0`, `Enabled=true`, `Mask=0`). This keeps the
picker's title, icon, and native window controls. Back up `smodrc`, preserve the
existing exception order, and reload the decoration deliberately as above; the
application and installers do not change SMOD settings. Ordinary Explorer
dialogs retain `linux-explorer-win7-dialog`, so the picker rule does not alter
their frame or inherit the browser's hidden caption. Native before/after picker
captures are `build/picker-border-before.png` and `build/picker-border-after.png`.

### Sharp background text through an XWayland picker's address bar

The Wayland build of Aero Glass Blur installed on the development desktop has
its X11 blur-property handling disabled. Its native decoration still blurs, but
an XWayland picker's transparent navigation row shows sharp background content.
The chooser already requests blur through `KWindowEffects`; changing breadcrumb
painting or making the strip opaque does not fix this compositor integration.
Native Wayland pickers send the supported `org_kde_kwin_blur_manager` protocol.

Pickers for X11 parents must stay on X11 to retain native transient parenting and
modality. For this Aero effect version, the scoped workaround is to add the exact
`linux-explorer-win7-chooser-dialog` token to the effect's **force-blur window
classes**, with matching-window blur enabled. In `kwinrc`, these are
`WindowClasses` (newline-separated) and `BlurMatching=true` under
`[Effect-aeroglassblur]`. Preserve existing classes and other effect settings,
then reconfigure only `aeroglassblur`. The opaque file list and footer remain
opaque; the compositor blurs the backdrop visible through the navigation row.
This setting is separate from the SMOD inner-border exception. The application
and installers do not apply it automatically.

The workaround was verified with the installed picker and Aero effect in a
private OpenGL KWin session with an X11 parent and a synthetic text backdrop.
`build/picker-aero-check/before.png` reproduces sharp text across the navigation
row; `after.png` shows the row blurred after reconfiguring only the effect. The
same exact-class rule was then applied to the development desktop, with its
previous `kwinrc` saved as `build/picker-aero-check/kwinrc.before-live`.

## Follow-up: controls, navigation, and PDF

Further direct user feedback identified problems not exposed by the old
standalone control fixtures:

- Only breadcrumb arrow ink (including locations/overflow) moves down two logical
  pixels; labels, icons, hit targets and split-menu keyboard behavior stay put.
  Raster tests compare visible arrow/label bounds at the real 20 px breadcrumb
  height and at 100%, 125%, and 150%.
- AeroQt's glass-ancestor QSS supplies a line-edit border-image. An intermediate
  flat-border workaround made SearchBox opaque white; direct user feedback
  rejected that appearance. SearchBox now keeps the native QLineEdit frame and
  explicitly uses the address-entry normal/hover/disabled resources, with a
  transparent base even while focused and typing. Composed fixtures compare the
  frame and interior exactly against an address field on blue and warm parents
  at 100/125/150%, retaining 9 pt text and native editing/clear/action behavior.
  QWindow mouse-click tests pass in offscreen and Wayland-created MainWindows.
  These injected events do not establish compositor delivery of physical clicks
  in an already-running desktop window; a reported native click failure remains
  subject to live validation rather than speculative event overrides.
- Sidebar selection ends three logical pixels before the content divider. Both
  Qt's row/branch background and the item delegate are bounded; geometry-only
  delegate checks are insufficient. This small gutter follows the user's stated
  preference while retaining full-row rather than text-hug selection.
- Favorites, Libraries, Computer and Network have stable-ID boolean preferences.
  Missing keys use the original defaults; explicit false remains collapsed.
  Rebuilds and current-folder highlighting must not overwrite the user's choice.
- Local PDFs use the installed Poppler `/usr/bin/pdftoppm` in a subprocess, first
  page only, scaled to at most 1024 px per edge. PPM output and errors are bounded,
  the process has a five-second wall deadline and Linux memory/CPU limits, and
  decode work stays off the GUI thread. Selection/visibility changes cancel
  obsolete jobs; generations prevent stale images. No renderer tempfiles or
  browser engine are used. Encrypted/unsupported documents may fall back, and
  remote PDFs are not downloaded automatically. No page-navigation or PDF text
  search is claimed.

The seven test targets are registered in CTest and the three-scale display
matrix. New controlled QPdfWriter fixtures validate first-page output and real
MainWindow PDF-to-text transitions without reading user documents. PDF tests
also exercise unavailable renderers, malformed/raster-size limits, timeout,
resource limits, latest-selection cancellation and asynchronous child reaping.
The optional provider's runtime package is `poppler-utils` on Debian/Ubuntu or
`poppler` on Arch. Private Python helpers are test fixtures, not an application
runtime dependency.

Whole-client evidence is under `build/screenshots-followup-themed/`, including
`pdf-preview-window.png`, `search-focused-input.png` and navigation images.
These are not compositor-frame captures. Existing user default-manager,
Meta+E and approved SMOD configuration must be preserved when installing updates.

## Favorites drag reference pass

An offline Windows 7 SP1 snapshot was inspected on 2026-10-16 at 1280 × 768,
100% scaling, with Basic frames. The original disk was not modified and this
temporary guest was shut down cleanly; a separately running VM was untouched.
Captures are in `build/references/windows7-favorites/`.

- Dragging a main-view folder onto the Favorites heading highlights the heading,
  says **Create link in Favorites**, and appends a shortcut. The original folder
  stays in place (`10-added-folder.png`, `11-folder-over-heading-active.png`).
- The narrow gaps between shortcuts show a **black, one-pixel insertion line**
  starting at the shortcut icon indentation, ending a few pixels before the
  pane boundary, with short vertical endcaps. The whole row is not an insertion
  zone (`08-folder-between-favorites.png`, `13-custom-favorite-between.png`).
- Reordering an existing user-added shortcut says **Move here**, including when
  hovering the Favorites heading (`13-custom-favorite-between.png`,
  `14-custom-favorite-heading.png`). Heading drops append; they do not insert first.
- Hovering a folder's center highlights that folder and changes the operation
  label to **Move to Desktop** in this example (`09-folder-on-favorite.png`).
- The drag image is a large, translucent icon above the pointer, approximately
  96 logical pixels, with a small white action label and blue text just below
  the pointer. Multiple-folder drags stack icons and put a blue count badge near
  the center (`15-multiple-folder-drag.png`).

The Linux client now uses those insertion zones, marker geometry, action labels,
heading behavior and drag images. Main-view directories carry their known folder
type with the normal URI list, including remote directories and grouped Details
views. Header/gap drops create bookmarks and return a Link action, rather than
moving folders. Existing-folder center drops still use KIO file operations.
Saved Favorites are bookmarks rather than physical Windows `.lnk` files, so
dragging a saved shortcut itself is restricted to reordering; its center-drop
cannot move the original folder.
Reordering uses a layout change on the existing shortcut rows, preserving their
selection, expansion state and scroll position instead of rebuilding the sidebar.

Physical mouse drags were also exercised in an isolated Xvfb display, using the
actual main view and navigation widgets. Local captures and drag-image PNGs are
in `build/references/linux-favorites/`. Those headless captures validate client
feedback and input delivery; they do not establish compositor-frame or Aero
Glass parity, and drag-image alpha is rendered by the live desktop compositor.

## Remaining fidelity work

- Multi-location libraries and a real library-management UI.
- Continuous view-slider sizing and more exact stop spacing.
- More native preview formats and editable file metadata.
- Exact font/icon fidelity depends on the user's installed theme and fonts.
- Aero Glass frame/shadows must be compared on a graphics-capable Windows VM
  separately; this Basic-frame reference cannot establish them.
