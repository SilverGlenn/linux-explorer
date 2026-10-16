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

## Remaining fidelity work

- Multi-location libraries and a real library-management UI.
- Continuous view-slider sizing and more exact stop spacing.
- More native preview formats and editable file metadata.
- Exact font/icon fidelity depends on the user's installed theme and fonts.
- Aero Glass frame/shadows must be compared on a graphics-capable Windows VM
  separately; this Basic-frame reference cannot establish them.
