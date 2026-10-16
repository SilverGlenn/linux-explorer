# Explorer FileChooser portal integration

This backend reuses the real Explorer chooser, navigation, models and file views.
It does not contain another browser, replace KDE's other portal services, or make
all applications use portals. GDebi7 source is unchanged.

The shared chooser retains its titled native frame, Aero navigation, folder-named
search and split view control. Its preview pane is on by default; the preview
button or Alt+P toggles it. It has no adjacent Help button.

## Build and user-local installation

The normal Explorer dependencies still apply. The frontend runtime is
`xdg-desktop-portal`; Python 3 is used by the optional preference/rollback tool.

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH="$HOME/.local" \
  -DCMAKE_INSTALL_PREFIX="$HOME/.local" -DBUILD_TESTING=ON
cmake --build build -j2
# Install only; no preference changes and no process restarts:
scripts/install-portal-local.sh build "$HOME/.local"
```

The independent `Portal` install component provides:

- `~/.local/bin/explorer-win7-portal` — backend executable.
- `~/.local/bin/explorer-win7-portal-client` — standard frontend client/demo.
- `~/.local/bin/explorer-win7-portal-setup` — guarded preference/rollback tool.
- `~/.local/share/dbus-1/services/org.freedesktop.impl.portal.desktop.explorerwin7.service`.
- `~/.local/share/xdg-desktop-portal/portals/explorer-win7.portal` — advertises
  **only** `org.freedesktop.impl.portal.FileChooser`.
- The picker-specific `linux-explorer-win7-chooser-dialog` desktop identity,
  the existing generic dialog identity, and this document under
  `~/.local/share/doc/linux-explorer-win7/`.

No public widget SDK is installed. The private chooser service remains separate;
its client/API is documented in [file-chooser-api.md](file-chooser-api.md).

## Opt in without replacing KDE's other portals

```bash
~/.local/bin/explorer-win7-portal-setup status
~/.local/bin/explorer-win7-portal-setup enable
```

Or explicitly install and configure in one step:

```bash
scripts/install-portal-local.sh build "$HOME/.local" --enable
```

The tool finds the current desktop's existing portal profile. If a user profile
already exists, it changes only its FileChooser preference. Otherwise it copies
the existing profile to a desktop-specific user profile and adds that one override.
It preserves the existing default and explicit Settings, Secret, ScreenCast,
RemoteDesktop, Screenshot and other interface choices. It does not invent an
unrelated default if no baseline is found. The resulting preference starts with:

```ini
[preferred]
# Existing default/other preferences remain as they were.
org.freedesktop.impl.portal.FileChooser=explorer-win7;kde;
```

The fallback list reflects the prior preference/default, rather than always
forcing `kde` on another desktop. Explicit enable omits the `none` sentinel from
that FileChooser list, because the frontend treats it anywhere in a list as
disabling the whole interface. The prior value remains backed up for rollback;
`default=none` and all other interface preferences remain untouched.
A backend list is a discovery preference, **not**
a promise to retry KDE after an in-progress Explorer request fails.

The tool checks that the installed metadata is discoverable in the current XDG
data search path and declares only FileChooser. It refuses ambiguous/malformed
or symlink-managed user profiles instead of replacing them. A custom install
prefix needs its `share` directory in the login session's `XDG_DATA_DIRS`.
`XDG_DESKTOP_PORTAL_DIR` is a test/override mechanism that supersedes ordinary
configuration; the tool refuses normal opt-in while that override is set.

### Applying configuration safely

Installation reloads D-Bus activation-file discovery, but it never restarts an
application, KWin, KDE backend, or portal frontend. A running frontend can retain
its old backend selection, and a running backend retains its old executable.

Use a new login, or restart **only an idle portal frontend/backend** deliberately.
Do not interrupt file dialogs, screen sharing, remote-desktop sessions or other
portal operations. Do not restart the KDE backend merely to change FileChooser.
The installer reports the restart requirement rather than terminating sessions.

### Rollback

Enable prints a backup directory under
`$XDG_STATE_HOME/linux-explorer-win7/portal-selection/` (normally `~/.local/state`).

```bash
~/.local/bin/explorer-win7-portal-setup restore
# Or select the exact transaction printed by enable:
~/.local/bin/explorer-win7-portal-setup restore --backup /path/to/transaction
```

An unchanged newly created user profile is removed; an unchanged existing profile
is restored byte-for-byte. If the user made unrelated edits afterward, only the
FileChooser key is restored and those edits remain. If FileChooser itself was
changed, rollback refuses to overwrite the new choice. An idle frontend restart
or new login applies rollback. The backend files can remain installed but unused.

## Public application API versus backend API

Applications must call the **standard frontend**, not the backend directly:

```text
Session bus: org.freedesktop.portal.Desktop
Object:      /org/freedesktop/portal/desktop
Interface:   org.freedesktop.portal.FileChooser

OpenFile(s parent_window, s title, a{sv} options) -> o request
SaveFile(s parent_window, s title, a{sv} options) -> o request
SaveFiles(s parent_window, s title, a{sv} options) -> o request

Request interface: org.freedesktop.portal.Request
Response(u response, a{sv} results) [signal]
Close()                            [method]
```

The standard frontend FileChooser v4 contract includes folder selection (v3),
multiple files/folders, filters, choices and batch-save destinations. Its response
codes are **0 accepted, 1 canceled, 2 other failure**. The backend supports the
corresponding operations but does not replace the frontend's app identity,
sandbox, document export or permission handling.

This implementation's backend identity is:

```text
Bus:       org.freedesktop.impl.portal.desktop.explorerwin7
Object:    /org/freedesktop/portal/desktop
Interface: org.freedesktop.impl.portal.FileChooser

OpenFile(o handle, s app_id, s parent_window, s title, a{sv} options) -> (u, a{sv})
SaveFile(o handle, s app_id, s parent_window, s title, a{sv} options) -> (u, a{sv})
SaveFiles(o handle, s app_id, s parent_window, s title, a{sv} options) -> (u, a{sv})
```

Backend replies are delayed until the interaction terminates. The backend exports
`org.freedesktop.impl.portal.Request.Close()` at each supplied handle. Only the
current unique owner of `org.freedesktop.portal.Desktop` can create or close backend
requests. Requests do not mix app results; frontend disconnect/bus loss dismisses
its interactions. No chooser request becomes an Explorer/reveal window.

## Standard options and results

Values in `a{sv}` contain these wire types:

| Option | Type | Meaning |
| --- | --- | --- |
| `handle_token` | `s` | Frontend request-path token, used to subscribe before making the call. |
| `accept_label` | `s` | Custom action label with portal underscore mnemonic syntax. |
| `modal` | `b` | Defaults true; a supplied real parent is needed to enforce cross-process modality. |
| `multiple` | `b` | OpenFile: select multiple files/folders. |
| `directory` | `b` | OpenFile: choose folders rather than files. |
| `filters` | `a(sa(us))` | Label plus rules `(0, glob)` or `(1, MIME type)`. |
| `current_filter` | `(sa(us))` | Initial filter; may apply alone when the filter list is empty. |
| `choices` | `a(ssa(ss)s)` | Choice ID, label, `(option ID, label)` list, initial selection. Empty options means boolean. |
| `current_folder` | `ay` | NUL-terminated local filesystem path bytes. |
| `current_name` | `s` | SaveFile suggested filename. |
| `current_file` | `ay` | SaveFile current local path, NUL-terminated. |
| `files` | `aay` | SaveFiles leaf names, each NUL-terminated. |

Portal globs are **case-sensitive**, as specified by the portal API; the private
Windows-7-style chooser API keeps its case-insensitive default. MIME subtype
wildcards such as `audio/*` are supported. Directories remain navigable under file
filters. Filters aid selection; consumers must still validate file contents.

Accepted results contain `uris: as` of fully encoded **local `file://` URIs**,
`choices: a(ss)` where applicable, and the exact `current_filter: (sa(us))` where
applicable. The frontend remains responsible for exposing sandbox-accessible
locations. Spaces, Unicode, `#`, `%` and literal colon-containing names are not
split or shell-interpolated. Filesystem-byte paths which cannot round-trip through
Qt's filename encoding are rejected rather than silently selecting another path.

Folder acceptance returns real existing folders, not a virtual Libraries/Computer
caption. Folder activation still navigates. Multiple selection uses the existing
shared selection model, not parsing a filename string.

Single Save retains documented suffix inference/overwrite confirmation. SaveFiles
selects a folder and constructs outputs in the requested order. Occupied names,
broken links and duplicates are uniquified with ` (N)` before the final suffix
(or after a suffixless/dotfile name), with a bounded search. Neither operation
creates, truncates or reserves output files. Consumers must safely perform later
I/O and handle races; this backend returns no descriptor and grants no privilege.

### Request lifetime and subscription race

Keep one originating D-Bus connection alive. Unlike the private chooser API,
the standard portal has **no GetResult**. Use `handle_token` and subscribe to the
predicted `/org/freedesktop/portal/desktop/request/<sender>/<token>` path before
calling the frontend. `<sender>` is the caller unique name without `:`, with dots
changed to underscores. The included client demonstrates this pattern.

User Cancel/window close produces response 1. A client that explicitly invokes
`Request.Close` ends its own operation; the frontend intentionally emits no
`Response` afterward. The CLI therefore prints a local canceled result after the
Close acknowledgement. A disappearing frontend/bus is a transport failure, not a
fabricated accepted selection.

## Client examples

```bash
# One Debian package through the real standard portal frontend:
explorer-win7-portal-client open --deb --start "$HOME/Downloads"
# Multiple packages:
explorer-win7-portal-client open --deb --multiple --start "$HOME/Downloads"
# A directory, or multiple directories with --multiple:
explorer-win7-portal-client open --directory --start "$HOME"
# Save Log (no file is written by this operation):
explorer-win7-portal-client save --log --name 'gdebi-install.log' --start "$HOME/Documents"
# Batch save destinations (names are separate arguments, not split output):
explorer-win7-portal-client save-many --start "$HOME/Documents" \
  --file 'first log.txt' --file 'second # 50% 日本語.txt'
# Additional native controls:
explorer-win7-portal-client open --start "$HOME" \
  --choice '{"id":"encoding","label":"Encoding","options":[{"id":"utf8","label":"UTF-8"},{"id":"latin1","label":"Latin-1"}],"selected":"utf8"}' \
  --choice '{"id":"readonly","label":"Read only","options":[],"selected":"false"}'
```

The CLI prints one JSON object; exit codes are 0 accepted, 1 canceled, 2 portal
failure, 3 transport/usage failure. `--cancel-after 1000` explicitly exercises Close.
It keeps the connection alive, pre-subscribes with a unique token, and never opens
or writes selected files. A one-shot qdbus/dbus-send process which immediately exits
is not a working interactive chooser client.

## Parenting and focus

Real consumers should export their top-level Wayland surface and pass
`wayland:<xdg-foreign handle>`, keeping it exported until completion. On X11 use
`x11:<hex-XID>` (including Chromium's `x11:0x...` form). A pointer or Wayland
`winId()` is not an exported handle. Caller and picker must use the same desktop
session. When their native display backends differ, the portal starts a presenter
on the caller's backend so the real parent and modality are retained. Its private
pipe closes with the request or backend; it does not claim another portal service
or expose an application API. Native dialog decorations are
retained under the `linux-explorer-win7-chooser-dialog` identity; no
topmost/borderless workaround is used. This separate identity allows an opt-in
SMOD inner-border exception while preserving the picker title and other dialogs'
frames. Empty-parent requests remain usable as standalone nonmodal dialogs.

Activation-token forwarding depends on frontend/toolkit version. A valid forwarded
fresh token is honored, but it is not required to make a standard request. The
compositor controls focus; real clients must manage their own input inhibition
and release it on success, cancellation or transport failure. Protocol tests do
not certify physical-input/focus behavior on the user's desktop.

## Coverage and rollout limits

- Portal-aware apps, many sandboxed apps, GTK FileChooserNative/FileDialog and
  suitably configured Qt/browser integrations can use this backend unchanged.
- Native KDE/Qt dialogs do not all automatically use portals. Toolkit/build or
  per-app opt-ins may be needed; this project does not change global Qt settings.
- Apps with custom pickers, some Java/Wine applications and games need their own
  integration. There is no safe universal interception switch.
- Remote KIO/virtual result URLs are not supported; only actual local filesystem
  URIs are returned. Kernel-mounted network storage is still a local path.
- No authentication, root execution, new Microsoft artwork or GDebi7 source change.

Tests use isolated buses and temporary settings, including real frontend/backend
and optional virtual-compositor checks. Mixed-display tests use a private KWin
and Xwayland to check native X11 parenting, modality and request cleanup. See
CTest/test logs for the exact executed matrix. True sandbox/document-grant tests
and physical focus must be reported separately from backend protocol/widget
evidence; do not infer their success from a host-client frontend test.

Official contracts: [FileChooser frontend](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.FileChooser.html),
[backend](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.impl.portal.FileChooser.html),
and [backend Request](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.impl.portal.Request.html).
