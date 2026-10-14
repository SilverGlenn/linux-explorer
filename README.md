# linux-explorer — Windows 7 fidelity fork

An experimental fork of [actuallyaridan/linux-explorer](https://github.com/actuallyaridan/linux-explorer),
based on direct inspection of Windows 7 Ultimate SP1 Explorer in an offline VM.
The upstream Qt6/KDE/KIO architecture is retained; this is a usable Linux file
manager, not a mock desktop or a Windows emulator.

**First fidelity pass, not complete parity.** Multi-location libraries, continuous
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
- Sidebar row/group spacing is closer to the Windows guest; Computer and Network
  start collapsed.
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

This installs `explorer-win7` and a separate **File Explorer (Windows 7 preview)**
launcher. It does not replace `explorer`, Dolphin, your Meta+E shortcut, your
directory MIME association, or your existing file-manager D-Bus service.
Preferences and the single-instance bus name are separate from upstream.

The browser app ID is `linux-explorer-win7`. If your decoration theme has an
application-specific inset/no-caption exception for upstream Explorer, add this
ID to the same exception manually; this fork does not alter your KWin settings.

For deliberate desktop integration, close the preview's existing instance and
launch it with:

```bash
explorer-win7 --register-file-manager
```

That opts into claiming `org.freedesktop.FileManager1` for the session, if
available. It does not forcefully replace another manager holding the name.
No system-wide D-Bus activation file is installed.

The original `aur/`, desktop, and D-Bus files are retained as upstream references;
they are not this fork's installation instructions or publishing targets.

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

Generate review images of Libraries, Details, Content, Preview, Computer, and the
view flyout:

```bash
EXPLORER_SNAPSHOTS="$PWD/build/screenshots" \
  ctest --test-dir build --output-on-failure
```

CTest uses Qt's offscreen/Fusion renderer for portable checks. These images verify
the client layout; they are not a screenshot of your compositor's Aero frame.
Headless multimedia/window-system warnings are expected, not test failures.

For a second client-layout pass using libAeroQt and your installed Kvantum theme:

```bash
QT_QPA_PLATFORM=offscreen QT_STYLE_OVERRIDE=kvantum EXPLORER_NATIVE_THEME=1 \
  EXPLORER_SNAPSHOTS="$PWD/build/screenshots-themed" \
  tests/run-ui-tests.sh "$PWD/build/explorer_ui_tests" renderSnapshots
```

This copies only KDE/Kvantum theme preferences into the temporary config. It still
excludes the compositor frame and does not require changing focus on your desktop.

The Guide's display/scaling matrix is reproducible with Qt's configurable
offscreen backend (tested on Qt 6.10):

```bash
tests/run-display-matrix.sh build
```

This verifies 800×600 at 100%, 1024×768 at 125%, and 1200×900 at 150%, including
the actual requested screen geometry and scale. Logs/images stay under `build/`.

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

The local development branch is `windows7-fidelity`. A local branch/checkout is
not a GitHub-hosted fork; publication requires authentication to the user's
GitHub account. Do not push these changes to the upstream remote. The prepared
local commit uses the automation author `Pi <pi@localhost>` rather than
inventing a user identity; amend it with your identity if desired before publishing.

After authorizing GitHub CLI and creating your own fork, point `origin` at that
fork and push the branch:

```bash
gh auth login
gh repo fork actuallyaridan/linux-explorer --clone=false --remote=false
git remote add origin https://github.com/YOUR_USERNAME/linux-explorer.git
git push -u origin windows7-fidelity
```

## Credits and license

Upstream design and implementation: **actuallyaridan**. Chrome support:
**libAeroQt** and **AeroThemePlasma**. File operations: **KIO/KDE Frameworks**.

This fork retains the upstream GPL-3.0 license; see [LICENSE](LICENSE).
Windows and Windows Explorer are Microsoft product names used as visual and
behavioral references. No affiliation or endorsement is implied.
