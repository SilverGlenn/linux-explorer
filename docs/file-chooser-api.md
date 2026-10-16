# Explorer Open/Save chooser — private API v1

This is an Explorer-owned dialog service, **not** `org.freedesktop.FileManager1`,
`--select`, an installed C++ widget SDK, or the standard portal interface. GDebi7
is intentionally unchanged. A separate [FileChooser portal backend](file-chooser-portal.md)
reuses the same UI. Applications keep their own file reading/writing, error
handling, and parent-window input inhibition.

Strict type, string-length, and filter-input limits below are enforced at the
untrusted D-Bus boundary. Internal `ChooserOptions` is trusted C++ configuration,
not an installed SDK, and does not duplicate the wire parser's bounds. It retains
the same local-only policy, selected-filter/default-extension semantics, and
filesystem acceptance checks. Protocol adapters validate their own wire formats
before constructing trusted options; the portal additionally uses folder/multiple,
choice and batch-save capabilities of that same UI.

The chooser keeps its native titled dialog frame and uses Explorer's Aero
navigation strip. Search names the current folder. The split view button cycles
views or opens the view menu; the preview pane starts visible and can be toggled
with its toolbar button or Alt+P. Preview uses the existing read-only text, PDF,
and thumbnail handlers. Pane dividers remain draggable without a visible grip.

## Build, install, activate

Use the same dependencies and user-local prefix as Explorer:

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH="$HOME/.local" \
  -DCMAKE_INSTALL_PREFIX="$HOME/.local" -DBUILD_TESTING=ON
cmake --build build -j2
scripts/install-chooser-local.sh build
```

The `Chooser` install component is independent of the browser installation:

```bash
cmake --install build --prefix "$HOME/.local" --component Chooser
```

Installed paths with that prefix:

- `~/.local/bin/explorer-win7-chooser` — persistent, independently activated service.
- `~/.local/bin/explorer-win7-chooser-client` — small asynchronous CLI/wire example.
- `~/.local/share/dbus-1/services/org.win7explorer.FileChooser1.service` — session
  bus activation entry with an absolute, quoted executable path.
- `~/.local/share/applications/linux-explorer-win7-chooser-dialog.desktop` — picker
  identity; the generic `linux-explorer-win7-dialog.desktop` is also retained.

The first method call activates the service. The installer reloads D-Bus's
activation-file discovery if a session bus is available; it does not launch the
chooser, stop Explorer, terminate file operations, change associations/Meta+E,
modify Qt/theme settings, install decoration rules, or change any portal service.
For a nonstandard prefix, its `share` directory must already be discoverable by
the login session's D-Bus daemon. An install-time `--prefix` override is supported;
`DESTDIR` is packaging-only and is never embedded in the activation command.

The service can also be started directly for development:

```bash
./build/explorer-win7-chooser
```

Do not start a second copy to replace an active service. Installing an update
does not update an already running process. If the installer reports an existing
chooser owner, finish/cancel its requests and restart **only that chooser service
when idle**, or use a new login. No browser restart is required for the separate
chooser service; already-running browser windows retain their previous code
until the user closes/reopens them. The installer never kills either process.

## Identity and wire contract

All communication is on the **session bus**.

| Item | Value |
| --- | --- |
| Well-known bus name | `org.win7explorer.FileChooser1` |
| Service object | `/org/win7explorer/FileChooser1` |
| Service interface | `org.win7explorer.FileChooser1` |
| Read-only property | `Version`, type `u`, value **1** |
| Request interface | `org.win7explorer.FileChooser1.Request` |
| Request object pattern | `/org/win7explorer/FileChooser1/requests/r<unique-id>` |

Methods:

```text
org.win7explorer.FileChooser1.OpenFile(in a{sv} options, out o request)
org.win7explorer.FileChooser1.SaveFile(in a{sv} options, out o request)

org.win7explorer.FileChooser1.Request.Cancel()
org.win7explorer.FileChooser1.Request.Close()
org.win7explorer.FileChooser1.Request.GetResult(out a{sv} result)

org.win7explorer.FileChooser1.Request.Finished(u status, a{sv} result)  [signal]
```

`OpenFile`/`SaveFile` return a handle promptly; they never block waiting for user
interaction. Each request has its own chooser, options, selected filter, owner,
and terminal state. Invalid or unsupported configuration is delivered as a
**failed request**, not a normal Explorer/reveal window. Failures that prevent
allocating a request at all (for example, a service resource limit) can instead
be ordinary D-Bus method errors. A D-Bus disconnect/service crash is a transport
failure, not a fabricated successful selection.

V1 bounds retained requests to **64 per service / 8 per owner**, including terminal
objects. Call `Close` to release them promptly. Exceeding either limit returns
`org.freedesktop.DBus.Error.LimitsExceeded`. Filter input is bounded to 64 filters
and 256 combined glob/MIME entries (256 characters per pattern). IDs are limited
to 256 characters, filter labels to 1024, other option strings to 4096, and
`starting_location` to 16384 encoded ASCII characters. Suggested filenames and
default extensions are additionally limited and validated as filesystem names;
control characters and path separators are not accepted there.

Failed-request `error_name` values use the interface prefix plus
`.Error.InvalidOptions`, `.Error.UnsupportedOptions`, `.Error.PresentationFailed`,
or `.Error.InvalidResult`. Clients should handle an unknown failure name as a
failure too, rather than assuming this list can never grow.

### Options (`a{sv}`)

Each value is a D-Bus variant containing **exactly** the indicated type. Unknown
keys, wrong types, invalid filter IDs, and unsupported policies are rejected.
Underscores in option names are significant.

| Key | Contained type | Default / meaning |
| --- | --- | --- |
| `title` | `s` | Mode-appropriate title if omitted. |
| `starting_location` | `s` | Absolute, properly encoded local `file:///…` directory URI; HOME if omitted. Do not pass a friendly caption or a raw filesystem path here. |
| `suggested_name` | `s` | Optional filename to populate File name. This is literal filename text, not a URI. |
| `filters` | `a(ssasas)` | Array of `(id, label, globs, mimeTypes)`; omitted/empty means all files. See below. |
| `selected_filter` | `s` | Initially selected filter ID; first filter if omitted. |
| `default_extension` | `s` | Save extension without a leading dot, e.g. `log`; otherwise inferred from selected filter if possible. |
| `local_only` | `b` | `true`; `false` is explicitly unsupported in v1. |
| `multiple` | `b` | `false`; OpenFile can select multiple files/folders. SaveFile rejects `true`. |
| `directory` | `b` | `false`; OpenFile chooses existing folders instead of files. SaveFile rejects `true`. |
| `parent_window` | `s` | Empty, `x11:<hex-XID>`, or `wayland:<exported-xdg-foreign-handle>`. Must match the service's window-system backend. |
| `activation_token` | `s` | Fresh user-activation token; see Wayland requirements below. |
| `modal` | `b` | `false`; `true` requires a valid nonempty `parent_window`. |

Folder and multiple selection are additive capabilities of this v1 request/result
model; ordinary single-file calls are unchanged. Both flags may be true for multiple
folders. Remote browsing remains unsupported. Choices and SaveFiles are available
through the standard portal, not new keys/methods on this private interface.

### Filters

Each filter has four fields, in order:

1. `s id` — nonempty, unique, stable caller ID; returned as `selected_filter`.
2. `s label` — human-readable combo-box label.
3. `as globs` — filename glob patterns, e.g. `['*.deb']` or `['*.log', '*.txt']`.
4. `as mimeTypes` — MIME type names, e.g. `['text/plain']`.

A file is shown if it matches **any** supplied glob or MIME type in the selected
filter. Directories are not removed by the file-type filter and remain navigable.
The independent search field is a fixed-text folder search, not another file-type
filter implementation. Use `'*'` to request an all-files filter explicitly.

For GDebi's `.deb` requirement, use a glob-only filter: adding a broad MIME type
would intentionally broaden the allowed matches. A filter is presentation, not a
security boundary; callers must still validate file contents themselves.

### Results, races, ownership, cancellation

Exactly one terminal state transition occurs per allocated request:

| `status` (`u`) | `state` (`s`) | Meaning |
| --- | --- | --- |
| 0 | `accepted` | User selected validated files/folders, or one Save destination. |
| 1 | `canceled` | Cancel, Escape, window close, explicit cancellation, or owner disconnect. |
| 2 | `failed` | Invalid/unsupported request or service-side failure. |

`Finished` is **unicast to the original unique-name owner**, not broadcast to
other chooser clients. Only that owner can call request methods; other clients
receive `org.freedesktop.DBus.Error.AccessDenied`. A well-known application name
is not used as request ownership and cannot take over an old request.

Result dictionary fields:

| Field | Type | Meaning |
| --- | --- | --- |
| `state` | `s` | `pending`, `accepted`, `canceled`, or `failed`. |
| `status` | `u` | Terminal status above; absent while pending. |
| `uris` | `as` | Fully encoded absolute local file URIs. One result unless OpenFile `multiple=true`; folder URIs when `directory=true`. No selection on cancellation/failure. |
| `selected_filter` | `s` | Caller filter ID where applicable (empty for no filters). |
| `error_name` | `s` | Machine-readable failure classification on failure. |
| `error_message` | `s` | Human-readable failure detail on failure. |

**Race-free client pattern:** keep one session-bus connection alive; call
OpenFile/SaveFile; subscribe to `Finished` at the returned object path; then call
`GetResult`. A result can have completed before signal subscription. Treat the
first terminal observation as final, and deduplicate the signal/query responses.
`GetResult` is a snapshot and does not emit another terminal signal.

`Cancel` is idempotent: a pending request becomes canceled, and a terminal result
never changes. It also dismisses a nested overwrite prompt. `Close` acknowledges
and releases a terminal request; on a pending request it cancels once and releases
it. Objects are retained for up to **60 seconds after completion** if not closed.
Once released, calls get the normal unknown-object error. Keep the originating
connection alive until completion: a one-shot `qdbus`/`dbus-send` process that exits
after receiving the handle immediately cancels its request.

When a client disconnects, the service dismisses all of its dialogs (including
confirmation dialogs), releases its request objects, and leaves all other clients
untouched. A signal to a disconnected caller is not deliverable; the internal
terminal state still transitions only once. If the service disappears, clients
must treat outstanding requests as transport failures and release parent input
inhibition; they must not wait indefinitely for `Finished`.

## Open and Save semantics

- Activating a folder navigates; activating a file validates/selects it and
  accepts. No associated application is launched. The address editor's typed-file
  handling follows chooser selection rather than Explorer's ordinary launch rule.
- Open requires an existing readable regular local file. Invalid/missing/unreadable
  locations produce a visible error, never an elevation/authentication offer.
- File name accepts relative filenames against the current directory, absolute
  local paths, and explicitly encoded file URIs. Literal paths preserve spaces,
  Unicode, `#`, `%` and ordinary colons (`report:2026.log`); results are always
  `QUrl::FullyEncoded` URIs. Only `file:` or hierarchical `scheme:/…` text is
  interpreted as an explicit URI, with nonlocal schemes rejected. Use a `./`
  prefix for a literal filename beginning with the reserved `file:` syntax.
- Save returns a filename only. It does **not** create, open, truncate, lock, or
  reserve the destination. The caller performs the actual write afterwards.
- If a typed leaf has no suffix and does not end with a dot, Save appends
  `default_extension` when specified; otherwise it uses the **first** simple
  `*.ext` glob in the selected filter, in caller-supplied order. Patterns whose
  extension still contains wildcard syntax are skipped; no simple pattern means
  no inferred extension. A leading-dot-only name such as `.draft` is treated as
  suffixless (`.draft.txt` with extension `txt`); `.draft.ext` keeps its suffix.
  An explicit suffix or trailing dot is retained, not replaced.
- Extension handling happens **before** existence/overwrite checks. Existing
  destinations require overwrite confirmation. Declining leaves the chooser
  active and the destination untouched. Directory destinations are navigation,
  not acceptance; nonregular or invalid destinations are refused.
- This is not a sandbox/permission-grant API and returns no file descriptor. POSIX
  permissions can change between selection and client access. Overwrite approval
  does not eliminate a filesystem TOCTOU race; use appropriate safe file-writing
  semantics in the consumer.
- Local means a filesystem location addressed with a local file URI, not a promise
  that storage is physically local. Kernel-mounted network filesystems are still
  local paths. Remote KIO URLs, `file://remote-host/…`, admin URLs, friendly display
  names, and virtual library roots are not returned as selected files. XDG library
  shortcuts resolve to their real backing folders.

## Working client examples

These run without GDebi7 changes and invoke the real service-owned chooser:

```bash
# Browse: select one local .deb.
~/.local/bin/explorer-win7-chooser-client open --deb \
  --title 'Open Debian package' --start "$HOME/Downloads"

# Save Log: choose a local output filename; the client itself writes nothing.
~/.local/bin/explorer-win7-chooser-client save --log \
  --title 'Save installation log' --start "$HOME/Documents" \
  --name 'gdebi-install.log'

# Multiple globs in one caller-defined filter (JSON is only the CLI input form).
~/.local/bin/explorer-win7-chooser-client save --start "$HOME" --name 'install' \
  --filter '{"id":"logs","label":"Logs (*.log, *.txt)","globs":["*.log","*.txt"],"mime_types":[]}' \
  --selected-filter logs --default-extension log

# Exercise explicit cancellation; exit status 1 is expected.
~/.local/bin/explorer-win7-chooser-client open --deb --start "$HOME" --cancel-after 1000
```

The CLI prints one JSON object on stdout and the request handle on stderr. Exit
codes: **0** accepted, **1** canceled, **2** failed request, **3** transport/usage
failure. It subscribes then queries the result and acknowledges with `Close`.
Never parse whitespace-delimited filenames, invoke a shell with the returned
path, or manually percent-decode a URI with string replacement. Use your toolkit's
URI-to-local-path API, e.g. `QUrl(uri, QUrl::StrictMode).toLocalFile()`.

An accepted response has this shape:

```json
{"status":0,"state":"accepted","uris":["file:///home/user/Downloads/a%20%23%2050%25%20%CE%A9.deb"],"selected_filter":"deb","request_handle":"/org/win7explorer/FileChooser1/requests/r..."}
```

The underlying D-Bus Open options for Browse are:

```text
{
  "title": variant(s, "Open Debian package"),
  "starting_location": variant(s, "file:///home/user/Downloads"),
  "filters": variant(a(ssasas), [("deb", "Debian packages (*.deb)", ["*.deb"], [])]),
  "local_only": variant(b, true),
  "multiple": variant(b, false),
  "directory": variant(b, false)
}
```

For Save Log call `SaveFile` with filter `("log", "Text logs (*.log)", ["*.log"], [])`,
`suggested_name = variant(s, "gdebi-install.log")` and
`default_extension = variant(s, "log")`.

## Wayland parent-window and focus requirements

The CLI examples above are deliberately **standalone**, nonmodal dialogs. A real
application integration should send a parent and fresh activation information:

1. Export the caller's **actual top-level surface** with xdg-foreign (for Qt/KF6,
   use the toolkit/KWaylandExtras export API; GTK has its Wayland window-export
   API). Wait for the asynchronous export to finish. `winId()` or a `wl_surface*`
   is **not** an exported Wayland handle.
2. Pass `parent_window = 'wayland:' + exported_handle`. Keep the export alive until
   the chooser terminates and then unexport it. Parent and service must be on the
   same compositor/backend; mismatched X11/Wayland parent types fail explicitly.
3. Obtain a fresh **xdg-activation token from the user action which opened the
   chooser**, targeted at `linux-explorer-win7-chooser-dialog` where the toolkit asks for an
   app ID. Pass it as `activation_token`. Do not recycle another request's token
   or rely on a token inherited at login. The compositor ultimately decides focus;
   without a valid token, the service does not force focus or bypass policy.
4. Send `modal = true` for an application-modal operation and inhibit interaction
   with the appropriate caller window(s) while waiting. Cross-process Qt modality
   cannot enforce your application's own input state. Release inhibition on all
   accepted/canceled/failed/transport-error paths.
5. If the parent closes, explicitly cancel; dropping the originating D-Bus
   connection also performs cleanup. Keep the owner connection alive during the
   complete request, not just the initial method call.

On X11 use `x11:<hex-XID>` of the real parent and an appropriate fresh startup ID
in `activation_token`. On Wayland the service uses KF6's foreign-parent support,
not an always-on-top flag or borderless pseudo-dialog. Native dialog titlebars
and decorations remain, under `linux-explorer-win7-chooser-dialog`; the browser-only
SMOD caption-removal exception must not match this app ID. A separate opt-in
inner-border exception can join the picker's Aero navigation to its native frame
while retaining its title and controls.

## Validation and limits

```bash
ctest --test-dir build --output-on-failure
# All widget suites, including chooser, at 100% / 125% / 150%:
tests/run-display-matrix.sh build
# Installation + actual automatic service activation on a private bus:
tests/run-chooser-activation-test.sh build
# Optional real Wayland export/import and modality/decorations smoke check:
# requires kwin_wayland and Qt's Wayland platform plugin.
tests/run-chooser-wayland-test.sh build
```

When `kwin_wayland` is available at configure time, CTest also registers the
native Wayland smoke check. It launches a **virtual KWin compositor** with its own
temporary settings/runtime and private bus, not the desktop compositor. The test
exports a real parent surface and observes the separate service's xdg-foreign
import/parent, dialog app ID, decoration and modality protocol requests. The
service protocol trace is retained at `build/chooser-wayland-client.log`.

D-Bus/widget tests use temporary HOME/config/data/runtime directories and an
isolated session bus, never the user's active Explorer/chooser service. The
activation test installs into a temporary prefix with spaces, `%`, `#` and Unicode,
activates the real executable, runs Open/Save/cancel/concurrent requests, and
cleans up only that private test service. A separate daemon-loss check verifies
that its pending client exits with transport status 3 and the service shuts down,
without using or terminating the desktop bus.

Implemented v1 is local Open (single/multiple files or folders) and single-file
Save, glob/MIME filters, typed paths, validation, safe filename selection/overwrite
confirmation, keyboard access, owner isolation/cancellation and user-local activation.
This private interface does not provide remote selection, a public installed widget
SDK, descriptors/permission grants, or GDebi7 integration. The separate standard
portal backend adds choices/batch-save and leaves sandbox grants to the frontend.

The automated display matrix checks Qt client layout and behavior, not native
compositor focus delivery, physical keyboard/mouse events, screen-reader
certification, or exact Windows frame artwork. The optional virtual-Wayland test
checks real parent/decorations/modality protocol delivery, not physical input or
focus. Activation-token focus policy, client-side input inhibition, and decorated
active/inactive dialog appearance still require a live compositor integration
check with the consumer's exported parent. No claim of that manual acceptance is
made by these automated tests.
