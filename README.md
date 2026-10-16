# linux-explorer — Windows 7 fidelity fork

A Linux file manager maintained by [Glenn Silver (SilverGlenn)](https://github.com/SilverGlenn),
focused on the appearance and behavior of Windows 7 Explorer. This version builds
on [actuallyaridan/linux-explorer](https://github.com/actuallyaridan/linux-explorer)
and retains its Qt 6, KDE Frameworks, KIO, and libAeroQt foundation.

Development happens on [`windows7-fidelity`](https://github.com/SilverGlenn/linux-explorer/tree/windows7-fidelity).
The application installs as `explorer-win7`, with a **File Explorer** launcher and
its own settings and application identity.

**This is a development version.** Explorer parity is incomplete, and Google
Drive sign-in requires a maintainer-configured application client. See
[Windows 7 reference notes](docs/windows7-reference.md) and the
[UX checklist](docs/uxguide-checklist.md) for observed behavior and remaining work.

## Features in this version

- A Libraries landing page backed by the real XDG Documents, Music, Pictures,
  and Videos folders, plus Computer and Network destinations.
- Windows 7-style breadcrumb navigation, search, command bar, classic menus,
  eight view modes, details pane, and persistent navigation groups.
- Real file operations through KIO, including copy, move, rename, trash,
  conflict handling, and undo. Natural sorting keeps folders first.
- A compact Properties dialog for local files, folders, and drives, with
  General, Security, and Details pages and asynchronous Size on disk calculation.
- Asynchronous thumbnails, bounded text previews, and optional first-page PDF
  previews using Poppler.
- A separate Open/Save chooser, with a private D-Bus API and an optional standard
  FileChooser portal backend.
- Google Drive browser sign-in, secure desktop credential storage, a read-only
  **Google Drive (G:)** mount, remote capacity reporting, and saved-account reconnect.
- Optional two-way synchronization between selected Google Drive folders and
  local folders, with separate write authorization, initial-copy preview,
  pause/resume controls, conflict preservation, and deletion review.

Linux filesystem paths, device permissions, and desktop services remain native.
Multi-location libraries, continuous view-slider sizing, and Windows preview
handlers are not implemented. Aero frame appearance depends on the installed
theme and window decoration; see the
[decoration integration notes](docs/windows7-reference.md#native-decoration-dependency).

## Build and run

Required development dependencies:

- CMake 3.19+ and a C++17 compiler.
- Qt 6.8+ with Widgets, Concurrent, Multimedia, DBus, UiPlugin, and NetworkAuth;
  Qt Test for the regression suite.
- Qt6Keychain and KDE Frameworks 6: KIO, CoreAddons, Config, WidgetsAddons,
  WindowSystem, Solid, and FrameworkIntegration.
- [libAeroQt](https://gitgud.io/atmk/libaero-qt), built and installed separately.

For Arch-based systems:

```bash
sudo pacman -S cmake ninja extra-cmake-modules qt6-base qt6-tools qt6-multimedia \
  qt6-networkauth qtkeychain-qt6 kio kcoreaddons kconfig kwidgetsaddons \
  kwindowsystem solid frameworkintegration
```

Clone this fork and build its development branch:

```bash
git clone --branch windows7-fidelity https://github.com/SilverGlenn/linux-explorer.git
cd linux-explorer
cmake -S . -B build -DCMAKE_PREFIX_PATH="$HOME/.local" \
  -DCMAKE_INSTALL_PREFIX="$HOME/.local" -DBUILD_TESTING=ON
cmake --build build -j2
./build/explorer-win7
```

`CMAKE_PREFIX_PATH` helps CMake find libAeroQt when it is installed under
`~/.local`. A system installation is discovered normally.

```bash
./build/explorer-win7 libraries:///     # default landing page
./build/explorer-win7 computer:///      # drives
./build/explorer-win7 ~/Documents      # open a folder
./build/explorer-win7 --select ~/a.txt  # reveal a file
```

Use [AeroThemePlasma](https://github.com/aeroshell-desktop/aerothemeplasma) and
appropriate installed icons and fonts for the intended desktop appearance.

### Optional runtime dependencies

| Dependency | Feature |
| --- | --- |
| `kio-extras` | Search providers and archive browsing |
| `kio-admin` | Administrator file operations |
| `ark` | Compressed-folder creation |
| `ffmpegthumbs`, `kdegraphics-thumbnailers` | Additional thumbnail providers |
| `kdenetwork-filesharing` | Network sharing integration |
| `pdftoppm` from Poppler | Local first-page PDF preview |
| Python 3, rclone, FUSE3 | Google Drive mounting and synchronization |

Two-way synchronization requires a stable rclone **1.71.0 or newer**. See
[Google Drive setup and release notes](docs/google-drive.md) for supported folder
types, authorization, runtime discovery, and recovery behavior.

## Installation and desktop integration

```bash
cmake --install build
```

The fork installs alongside existing file managers. Installation keeps the
desktop's default file manager, directory association, and shortcuts intact.
Its browser application ID is `linux-explorer-win7`; dialog and picker IDs are
`linux-explorer-win7-dialog` and `linux-explorer-win7-chooser-dialog`.

To deliberately let this instance answer desktop **Open Containing Folder**
requests, close any existing fork instance and launch:

```bash
explorer-win7 --register-file-manager
```

This requests the session's `org.freedesktop.FileManager1` name if available.
Another file manager already holding the name keeps it.

### Open/Save chooser

The independent chooser service uses `org.win7explorer.FileChooser1` and shares
the browser's navigation and file-view components. It supports file and folder
selection, multiple-file Open, and single-file Save. It returns the selected
destination; the requesting application performs the open or save operation.

Install just the chooser and try its sample client:

```bash
scripts/install-chooser-local.sh build
~/.local/bin/explorer-win7-chooser-client open --deb --start "$HOME/Downloads"
~/.local/bin/explorer-win7-chooser-client save --log --start "$HOME/Documents" \
  --name 'example.log'
```

See the [private chooser API and integration guide](docs/file-chooser-api.md)
for activation, filters, cancellation, Wayland parenting, and limitations.

### Optional FileChooser portal

Portal-aware applications can use the same chooser through an opt-in backend:

```bash
scripts/install-portal-local.sh build "$HOME/.local"
~/.local/bin/explorer-win7-portal-setup enable
~/.local/bin/explorer-win7-portal-client open --multiple
```

Only FileChooser is selected; other desktop portal functions keep their existing
providers. Apply portal changes at the next login or through a deliberate idle
restart. See the [portal installation and rollback guide](docs/file-chooser-portal.md).

## Google Drive

Choose **Tools → Connect Google Drive... → Sign in with Google**. Consent happens
in the system browser, and the desktop credential store remembers the account.
The connected **Google Drive (G:)** entry supports browsing and copying files to
local folders. Its streamed filesystem remains read-only.

Choose **Tools → Google Drive synchronization...** to authorize writes separately
and pair selected cloud folders with initially empty local folders. Review the
initial copy before starting. Synchronized folders remain available offline;
an independent worker continues synchronization after Explorer windows close.

Development builds without a registered application client explain that sign-in
is unavailable. Users of a configured build do not need to create Google Cloud
projects or import credentials. Production distribution requires the applicable
Google verification. Configuration, supported files, backups, conflicts,
disconnect, and sign-out are documented in [docs/google-drive.md](docs/google-drive.md).

## Testing

```bash
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

The suite exercises Qt widgets, KIO operations, navigation, previews, Properties,
Drive connection and synchronization, chooser services, and portal integration.
Tests use temporary homes and isolated D-Bus sessions. Google account tests use
fake responses and credential stores; live authorization still needs separate
maintainer validation. Optional Wayland checks need a test compositor.

For the display/scaling matrix:

```bash
tests/run-display-matrix.sh build
```

## Keyboard shortcuts

| Key | Action |
| --- | --- |
| Alt+D, Ctrl+L, F4 | Edit the address bar; F4 also opens typed-path history |
| Ctrl+E, Ctrl+F, F3 | Focus search |
| Alt+Left / Alt+Right | Back / forward |
| Backspace / Alt+Up | Back / up |
| F2 / F5 / F11 | Rename / refresh / full screen |
| Alt | Toggle the classic menu bar |
| Alt+P | Toggle the preview pane |
| F6 / Shift+F6 | Cycle panes |
| Ctrl+Shift+1…8 | Select a view mode |
| Ctrl+wheel | Step through view modes |
| Alt+Enter | Properties |
| F1 | File Explorer Help |

## Credits and license

**Fork maintainer and contributor:** [Glenn Silver (SilverGlenn)](https://github.com/SilverGlenn).

**Original project:** [actuallyaridan/linux-explorer](https://github.com/actuallyaridan/linux-explorer),
created by [actuallyaridan](https://github.com/actuallyaridan). Full credit goes to
the upstream author and contributors for the original implementation, design,
and foundation extended by this fork. Upstream attribution and history are retained.

The project uses [Qt](https://www.qt.io/),
[KDE Frameworks](https://develop.kde.org/products/frameworks/), and
[KIO](https://invent.kde.org/frameworks/kio) for its application and file-operation
foundation, [libAeroQt](https://gitgud.io/atmk/libaero-qt) for Aero chrome, and
[AeroThemePlasma](https://github.com/aeroshell-desktop/aerothemeplasma) for desktop
theme integration. Google Drive integration uses [rclone](https://rclone.org/)
and QtKeychain; bundled artwork attribution is retained with its assets.

The original project is part of actuallyaridan's **WSL (Windows-alike Software
for Linux)** series. Related upstream projects:

- [Linux Device Manager](https://github.com/actuallyaridan/linux-devmgmt)
- [Linux Control Panel](https://github.com/actuallyaridan/linux-control)
- [Linux Minesweeper](https://github.com/actuallyaridan/linux-minesweeper)

This fork retains the upstream **GPL-3.0** license; see [LICENSE](LICENSE).
Windows and Windows Explorer are Microsoft product names used as visual and
behavioral references. No affiliation or endorsement is implied.
