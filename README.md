# linux-explorer — Windows 7 fidelity fork

An experimental fork of [actuallyaridan/linux-explorer](https://github.com/actuallyaridan/linux-explorer),
based on direct inspection of Windows 7 Ultimate SP1 Explorer in an offline VM.
The upstream Qt6/KDE/KIO architecture is retained; this is a usable Linux file
manager, not a mock desktop or a Windows emulator.

**VM-driven fidelity work, not complete parity.** Multi-location libraries, continuous
view-slider sizing, and native Windows preview handlers are still missing.
The reference VM used Basic graphics, so its Aero Glass frame was not a valid
reference for transparency or shadows.

See [the reference notes](docs/windows7-reference.md) for observed behavior,
measurements, intentional Linux adaptations, and remaining work. The
[UX Guide checklist](docs/uxguide-checklist.md) explains how Microsoft's
Windows 7-era principles are used without overriding Explorer fidelity.

## What changed

- Libraries is the default landing page, with a green heading and four tiles
  backed by real XDG Documents/Music/Pictures/Videos folders.
- Libraries is a selectable sidebar destination. Canonical library entries and
  Desktop/Downloads defaults no longer depend on KDE having bookmarked them.
- Sidebar row/group spacing is closer to the Windows guest. Favorites, Libraries,
  Computer, and Network remember their expanded/collapsed state; the first-run
  defaults keep Computer and Network collapsed. Selection leaves a three-pixel
  logical gap before the navigation/content divider.
- View is a split button: its main half uses the verified five-mode cycle; its
  arrow opens an eight-choice flyout with a vertical slider.
- A preview-pane toolbar button shares the Alt+P action.
- Breadcrumbs are button-like rather than underlined links, include a current-
  folder dropdown, and collapse ancestors when the window is narrow.
- Contextual commands precede New folder, and excess commands move into a
  functional chevron overflow.
- The pale-blue, full-width details pane has a 48 px icon, aligned metadata,
  and a count-only unselected folder summary.
- Text preview shows read-only content instead of a duplicate metadata card.
  Reads are asynchronous, bounded to 256 KiB, BOM-aware, and protected against
  stale results after selection changes.
- Local PDFs have a lightweight, asynchronous first-page preview using Poppler.
  Selection changes and hiding the pane cancel obsolete work; document processing
  has pixel, output, CPU, memory, and time limits. No embedded browser is used.
- Content view puts date/size at the right with row rules. Small icons uses
  side-by-side icon/text. Icon views have less vertical padding.
- Natural name sorting preserves folders-first ordering in both directions.
  Inline rename preserves the extension when selecting the base name.
- Libraries has a proper Details table and preserves selection/counts on refresh.
  The classic menu bar has the five native menus; F6 skips hidden panes, and
  the address editor really takes focus. F1 opens local help rather than About.
- Rename invalidates cached selected-item URLs, so Copy/Properties uses the
  new filename. Real KIO rename/copy/cut-paste/move and Undo have regression coverage.

File operations still go through KIO. Linux paths, device identities, mount
permissions, and installed sharing services remain real; unsupported Windows
features are not represented by pretend controls. Known extensions stay visible
by default, as in upstream, rather than copying Windows' hidden-extension default.

## Build and run

Requires CMake, a C++17 compiler, Qt6 Widgets/Concurrent/Multimedia/DBus/UiPlugin,
KDE Frameworks 6 (KIO, CoreAddons, Config, WidgetsAddons, WindowSystem, Solid,
FrameworkIntegration), and [libAeroQt](https://gitgud.io/atmk/libaero-qt).

PDF preview optionally uses `/usr/bin/pdftoppm` (`poppler-utils` on Debian/Ubuntu,
`poppler` on Arch). Without it, the pane explains that the renderer is unavailable.
This is a local-file, first-page raster preview, not a full PDF viewer: no page
navigation, PDF text search, password entry, or automatic remote download.

With dependencies installed:

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH="$HOME/.local" -DCMAKE_INSTALL_PREFIX="$HOME/.local"
cmake --build build -j2
./build/explorer-win7
```

On Arch, the Qt/KDE development dependencies are:

```bash
sudo pacman -S cmake ninja qt6-base qt6-tools qt6-multimedia kio kcoreaddons \
  kconfig kwidgetsaddons kwindowsystem solid frameworkintegration
```

Build/install libAeroQt separately. The prefix argument above is useful when it
is installed under `~/.local`; a system installation is discovered normally.

```bash
./build/explorer-win7 libraries:///     # the default landing page
./build/explorer-win7 computer:///      # drives
./build/explorer-win7 ~/Documents      # a real folder
./build/explorer-win7 --select ~/a.txt  # reveal a file in its parent
```

For the intended chrome, use AeroThemePlasma/libAeroQt and an appropriate icon
theme/font already installed on your desktop. The repository does not bundle
new Microsoft resources extracted from the Windows disc.

## Install alongside your existing file manager

```bash
cmake --install build
```

This installs `explorer-win7` and a separate **File Explorer**
launcher. It does not replace `explorer`, Dolphin, your Meta+E shortcut, your
directory MIME association, or your existing file-manager D-Bus service.
Preferences and the single-instance bus name are separate from upstream.

The browser app ID is `linux-explorer-win7`; dialogs use
`linux-explorer-win7-dialog`. Continuous caption/navigation glass depends on an
**opt-in browser-only SMOD decoration exception**, not just the client layout.
This fork does not alter KWin/SMOD settings. See the [decoration integration
notes](docs/windows7-reference.md#native-decoration-dependency) for the exact
match and retained native controls. Apply live decoration changes only with
explicit user approval; the application never installs this rule automatically.

For deliberate desktop integration, close the preview's existing instance and
launch it with:

```bash
explorer-win7 --register-file-manager
```

That opts into claiming `org.freedesktop.FileManager1` for the session, if
available. It does not forcefully replace another manager holding the name.
No `org.freedesktop.FileManager1` activation file is installed by this fork.
The private chooser has its own independent activation entry, described below.

The original `aur/`, desktop, and D-Bus files are retained as upstream references;
they are not this fork's installation instructions or publishing targets.

## Explorer-owned Open/Save chooser

Other applications can request a Windows 7–style **Open or Save** dialog over
the private, asynchronous `org.win7explorer.FileChooser1` session-bus
API. The chooser reuses Explorer's real navigation, locations and file-view
components, but is a separate service and a common-file-dialog layout—not a
reveal window. It never launches the chosen file or writes the Save destination.
GDebi7 is not modified by this implementation.

After building, install just the chooser without replacing or stopping Explorer:

```bash
scripts/install-chooser-local.sh build
~/.local/bin/explorer-win7-chooser-client open --deb --start "$HOME/Downloads"
~/.local/bin/explorer-win7-chooser-client save --log --start "$HOME/Documents" \
  --name 'gdebi-install.log'
```

D-Bus activates the independent service on the next client request. The installer
reports if an already-running chooser needs an idle restart; it never terminates
Explorer windows or file operations. No default-manager, shortcut, theme or portal
settings are changed by that chooser-only installer.

See **[the v1 interface and integration guide](docs/file-chooser-api.md)** for exact
wire types, filtering/extensions/overwrite behavior, URI encoding, cancellation
and disconnect handling, activation paths, client examples, Wayland exported-parent
and activation-token requirements, tests and remaining limitations. Open also
supports multiple files and folder selection; Save remains single-file. Remote
selection is unsupported.

## Optional standard FileChooser portal

An opt-in backend lets portal-aware applications use this same Explorer chooser,
including folders, multiple files, extra checkbox/combo choices, and batch-save
destinations. Only FileChooser is selected; KDE's screen sharing, screenshots,
settings, secrets and other portal functions retain their existing providers.

```bash
scripts/install-portal-local.sh build "$HOME/.local"       # install only
~/.local/bin/explorer-win7-portal-setup enable             # explicit opt-in
~/.local/bin/explorer-win7-portal-client open --deb --multiple
~/.local/bin/explorer-win7-portal-client open --directory
```

The installer never restarts portal/app processes. Apply a selection/code update
with a new login or deliberate idle frontend/backend restart, not while dialogs
or portal sessions are active. The setup tool backs up the prior profile and
supports guarded rollback. It does not force applications with custom/nonportal
pickers to use portals or change global Qt theme settings.

See **[portal installation, API, coverage and rollback](docs/file-chooser-portal.md)**.

## Tests and visual review

The regression suite uses real Qt widgets and KIO in temporary HOME/config/data
directories, not your personal folders. The test runner also isolates the
D-Bus daemon's environment and disables unneeded desktop-service activation,
so the test process and its helpers do not use your normal configuration:

```bash
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

Generate review images of Libraries, Details, selected folders (normal/narrow),
focused-empty/typed search, Content, Preview, Computer, and the view flyout:

```bash
EXPLORER_SNAPSHOTS="$PWD/build/screenshots" \
  ctest --test-dir build --output-on-failure
```

Widget regressions use Qt's offscreen/Fusion renderer for portable checks. These images verify
the client layout; they are not a screenshot of your compositor's Aero frame.
Headless multimedia/window-system warnings are expected, not test failures.

For a second client-layout pass using libAeroQt and your installed Kvantum theme:

```bash
QT_QPA_PLATFORM=offscreen QT_STYLE_OVERRIDE=kvantum EXPLORER_NATIVE_THEME=1 \
  EXPLORER_SNAPSHOTS="$PWD/build/screenshots-themed" \
  tests/run-ui-tests.sh "$PWD/build/explorer_ui_tests"
```

For the original seven browser suites with the native widget theme, use this runner loop
(CTest intentionally pins Fusion):

```bash
for suite in address_controls_tests thumbnail_loading_tests details_pane_tests list_chrome_tests navigation_state_tests pdf_preview_tests explorer_ui_tests; do
  QT_QPA_PLATFORM=offscreen QT_STYLE_OVERRIDE=kvantum EXPLORER_NATIVE_THEME=1 \
    NAVIGATION_USE_AEROQT=1 LIST_CHROME_USE_AEROQT=1 \
    tests/run-ui-tests.sh "$PWD/build/$suite" || exit 1
done
```

Only `kdeglobals` and `Kvantum/kvantum.kvconfig` are copied into the temporary
config. Full UI/address tests explicitly register AeroQt over Kvantum; the flags
above also enable it for the navigation/list suites. The remaining component
suites exercise Kvantum only. These runs exclude the compositor frame and do not
change focus or settings on your desktop.

The Guide's display/scaling matrix is reproducible with Qt's configurable
offscreen backend (tested on Qt 6.10):

```bash
tests/run-display-matrix.sh build
```

This runs all thirteen widget/service/frontend suites at 800×600/100%, 1024×768/125%, and
1200×900/150%, including chooser keyboard behavior and full-UI assertions of actual
screen geometry and scale. Each suite retains its temporary HOME and private bus.
Logs/images stay under `build/`. CTest additionally checks installed private and
portal chooser activation, FileChooser-only preference/rollback fixtures, and,
when `kwin_wayland` is available, private and portal parent/decorations/modality
protocols on a separate virtual compositor. The real system `xdg-desktop-portal`
frontend is required for frontend tests; it runs with curated portal/service
metadata and temporary settings, not the desktop service. Host-client tests do
not require a document-portal FUSE mount or prove sandbox document grants.

End-to-end checks cover stable 9 pt search and its visible focused frame, blocked
navigation restore, real breadcrumb popup lifetime, navigation/inset agreement,
selected-folder footer metadata, persisted sidebar groups across MainWindow
reconstruction, and real first-page PDF-to-text preview transitions. Component
suites additionally check actual arrow ink alignment, the sidebar divider gutter,
asynchronous thumbnail identity/cache behavior, and PDF safety/cancellation
limits. Logs and images remain under `build/`; see the
[reference notes](docs/windows7-reference.md#follow-up-controls-navigation-and-pdf)
for the bounded PDF feature and native-frame validation limits.

The GitHub build workflow is configured to build and run the suite in Arch. Release
publishing is limited to this repository's own artifacts; the upstream AUR
publishing job has been removed.

## Shortcuts

| Key | Action |
| --- | --- |
| Alt+D, Ctrl+L, F4 | Edit the address bar; F4 also opens typed-path history |
| Ctrl+E, Ctrl+F, F3 | Focus search |
| Alt+Left / Alt+Right | Back / forward |
| Backspace / Alt+Up | Back / up |
| F2 / F5 / F11 | Rename / refresh / full screen |
| Alt | Toggle classic menu bar |
| Alt+P | Toggle preview pane |
| F6 / Shift+F6 | Cycle panes |
| Ctrl+Shift+1…8 | Select a view mode |
| Ctrl+wheel | Step through view modes |
| Alt+Enter | Properties |
| F1 | File Explorer Help |

Search, archives, thumbnails and sharing depend on installed KIO providers.
Useful optional packages remain `kio-extras`, `kio-admin`, `ark`,
`ffmpegthumbs`, `kdegraphics-thumbnailers`, and `kdenetwork-filesharing`.

## Fork status and publishing

The development branch is `windows7-fidelity`; this checkout's `origin` is the
user's fork, `https://github.com/SilverGlenn/linux-explorer.git`, not upstream.
Refinement pass 2 remains **local and unpublished** pending review of the exact
final commit. No installation or push is part of these validation runs. Commits
use the per-command automation identity `Pi <pi@localhost>` without changing the
user's Git identity. After approval, publish only the reviewed branch with a
normal, non-force push; do not change the default branch or create a release.

## Credits and license

Upstream design and implementation: **actuallyaridan**. Chrome support:
**libAeroQt** and **AeroThemePlasma**. File operations: **KIO/KDE Frameworks**.

This fork retains the upstream GPL-3.0 license; see [LICENSE](LICENSE).
Windows and Windows Explorer are Microsoft product names used as visual and
behavioral references. No affiliation or endorsement is implied.
