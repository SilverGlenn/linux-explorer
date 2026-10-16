# Google Drive connection

Explorer uses an application-owned Google Desktop OAuth client. Users choose
**Tools → Connect Google Drive... → Sign in with Google**, authorize in their
normal system browser, and return to **Computer → Hard Disk Drives → Google Drive (G:)**. They do not
create Cloud projects, import JSON, use a terminal, or choose an encryption password.
The command is also on Computer and Network's context menu.

## Current release status

The sign-in, secure storage, and mount integration are implemented. The registered
application client is supplied through external build configuration and is never
included in source. An unconfigured development build disables sign-in and
explains that the distributor must provide support.
It never substitutes rclone's shared client, KDE's client, another application's
identity, or an end-user project wizard. A real Google account test requires a
maintainer's registered Desktop client. Google permits development tests using
the External audience in Testing status, with the test account explicitly added
under Audience. Production release still requires Google's applicable verification
process. Unit tests and synthetic local mounts do not establish that Google has
approved the application.

## User behavior

- Sign-in begins only after an explicit button press. Google credentials are
  entered in the system browser; Explorer never asks for the Google password.
- The desktop credential wallet remembers the account. It may request its normal
  unlock/permission confirmation. There is no separate Drive encryption password.
- A successful read-only mount appears as **Google Drive (G:)** under Computer's
  Hard Disk Drives and in the Computer sidebar, using the existing drive artwork
  with Google's official Drive logo in the corner. The entry tracks verified mount
  state without reading the remote filesystem or opening the credential wallet.
  Remote capacity stays unknown rather than displaying host-disk capacity.
  **Show in Network** is an optional additional shortcut; a saved shortcut is reused
  when reconnecting. Old `gdrive:/` and account-root Network entry points open the
  current managed Drive. Provider descendant URLs and opaque file IDs are never
  guessed or translated into local paths. KDE accounts/bookmarks are left intact.
  Drive address checks and breadcrumb folder menus use cancellable asynchronous
  KIO jobs with a 15-second deadline; a stalled FUSE mount cannot block the UI
  through these paths. Drive never inherits an administrator worker identity.
  The G: label is Explorer's presentation of its Linux FUSE mount, not a system-wide
  Windows drive-letter assignment.
- **Disconnect** normally unmounts the filesystem and retains the saved account.
  A busy mount is not force-unmounted. **Sign out** first disconnects, then attempts
  Google revocation and removes the application's local credentials. If Google
  cannot confirm revocation, the UI says so and directs the user to Google's app
  permissions. Sign out remains available while disconnected, including after a
  cancelled attempt saved credentials. Removing the bookmark only removes the shortcut.
- Cancel ends the current connection attempt. Consent already granted in Google's
  browser may survive cancellation; an in-flight secure wallet write is allowed
  to finish without mounting or bookmarking. Other Explorer windows remain usable.

The initial integration requests only `drive.readonly` and mounts read-only.
Browsing, downloading, and copying to a local folder are supported. Upload,
rename, deletion, and saving into Drive are disabled. Google-native documents use
rclone export formats and need separate live compatibility checks; some applications
can see their size as zero. There is no offline synchronization or login autostart.

## Maintainer registration and release

This is one registration for the application, owned and maintained by its
publisher. It is not an onboarding step for every user. Follow Google's
[desktop OAuth setup](https://developers.google.com/identity/protocols/oauth2/native-app)
and [OAuth policies](https://developers.google.com/identity/protocols/oauth2/policies).
Register a **Desktop app** client in the application's project, enable Drive API,
use accurate Explorer branding and contact information, and configure only
`https://www.googleapis.com/auth/drive.readonly` for this implementation.

That scope reads the complete existing Drive and is classified as **restricted**.
A public release needs the applicable Google verification; a narrower `drive.file`
grant cannot browse an arbitrary existing Drive as a network filesystem. Keep
personal/development testing and production registrations separate. Server-side
handling of restricted data has additional assessment requirements; this design
keeps OAuth and file access on the user's desktop. See Google's
[Drive scope requirements](https://developers.google.com/workspace/drive/api/guides/api-specific-auth).
Do not instruct users to bypass a blocked application or borrow another app's client.

Keep the maintainer's downloaded Desktop client JSON outside the repository and
provide it at build time:

```bash
cmake -S . -B build \
  -DCMAKE_PREFIX_PATH="$HOME/.local" \
  -DEXPLORER_GOOGLE_DRIVE_CLIENT_JSON='/private/release/google-desktop-client.json' \
  -DEXPLORER_REQUIRE_GOOGLE_DRIVE_CLIENT=ON
cmake --build build -j1
```

The required-client flag rejects missing/invalid clients for a Drive-enabled
release. It validates configuration, not Google's approval status. Unconfigured
development builds remain buildable with sign-in unavailable. No OAuth credentials
are invented, checked into source, or taken from environment-variable overrides.
When updating an already configured local installation, preserve both client
settings in every build directory that can supply the installed browser. An
unconfigured binary disables sign-in even if another build was configured earlier.
Coordinate installation between concurrent project chats so an unrelated UI
update cannot replace the registered build with an unconfigured one. A local,
git-ignored `CMakeUserPresets.json` can retain the external client path and
required-client flag; it must contain no credential contents.
Native desktop client values are extractable from a distributed binary; Google
explicitly notes that native apps cannot keep a shared client secret confidential.
The code uses S256 PKCE rather than treating that value as an authentication secret.

The release workflow requires the repository secret
`GOOGLE_DRIVE_DESKTOP_CLIENT_JSON` and passes it as a private, read-only file outside
the source tree. Ordinary CI builds remain unconfigured. The release artifact
includes the connection helper beside the browser executable. Configuring this
secret or publishing a release is not performed by the local implementation task.

Dependencies are Qt 6.8+ NetworkAuth, Qt6Keychain, Python 3, rclone, and FUSE3.
Ubuntu development packages are `qt6-networkauth-dev` and `qtkeychain-qt6-dev`;
runtime packages include `rclone` and `fuse3`. Ship the helper beside Explorer.
QtKeychain uses the desktop's secure store with insecure fallback disabled.

## Implementation and credential boundaries

`GoogleDriveSignInDialog` uses the existing Aero task-dialog layout and window
modality. `GoogleDriveConnection` coordinates the shared application session.
An operating-system lock in the user's private desktop runtime directory spans
wallet changes and mounting, so a second Explorer process cannot replace the
encryption key during another connection. Cancellation retains that lock until
an in-flight wallet write finishes.
`GoogleDriveAuth` uses Qt NetworkAuth for authorization code exchange with S256
PKCE, a cryptographically random state, and an IPv4 loopback listener on a random
port/path. The callback validates method, Host, path, state, duplicate fields,
size, and single use. Token exchange and revocation use fixed Google HTTPS URLs,
normal certificate validation, bounded requests, and no redirect following.
Denied or incomplete scope grants fail before credentials are used.

QtKeychain holds a versioned, client-bound account record and a random encryption
key. Reauthorization preserves the existing encryption key. The encrypted rclone
config is the canonical owner of the latest refreshed token; reconnect never
replaces it with a stale wallet copy. Sign-out exports that current token through
a private pipe for revocation.

The helper's `app-connect` and `app-export-token` receive a bounded versioned
credential envelope only through stdin. Credentials never enter command arguments,
environment variables, diagnostic logs, or plaintext configuration files. Official
rclone encryption reads a sealed Linux memfd and writes only encrypted data. The
app mount runs rclone in the foreground as a detached process, with its unlock key
provided over a one-shot pipe, avoiding rclone's daemon key tempfile. Content VFS
caching is disabled. This protects persisted credentials; an unlocked session is
still accessible to processes running with the user's identity.

The application config is
`$XDG_CONFIG_HOME/linux-explorer-win7/google-drive/app-rclone.conf` (default
`~/.config/linux-explorer-win7/google-drive/app-rclone.conf`), mode `0600` in a
private directory. The reserved mount is
`$XDG_DATA_HOME/linux-explorer-win7/mounts/google-drive` (default
`~/.local/share/linux-explorer-win7/mounts/google-drive`). The helper refuses unsafe
paths, foreign/writable/stacked mounts, nonempty backing folders, and concurrent
operations. The disconnected backing directory is protected with mode `000`;
Explorer also refuses offline navigation into the reserved root or descendants.
FUSE requires temporary directory access during mount startup. An uncatchable
process/OS crash in that startup interval can leave mode `700` on the empty local
backing folder; the lexical Explorer navigation guard still rejects it while
disconnected, and the next normal disconnect restores mode `000`.
No automatic package installation, administrator mount, or force unmount occurs.

Legacy personal `rclone.conf` and the user's default rclone config are separate
and never silently imported. The old `setup --client-json` helper command remains
a developer/manual compatibility tool; the product sign-in path never invokes it.
The previously installed KDE KIO worker can still handle explicit `gdrive:` URLs,
but it is not the default sign-in backend. Its packaged provider failed live Drive
authorization because its Drive scope was removed; see
[KDE's explanation](https://bugs.kde.org/show_bug.cgi?id=480779#c46).

## Validation

Tests use fake Google responses, in-memory credential stores, isolated process
pipes, temporary homes, and dummy local FUSE data. They cover PKCE/state/callback
validation, rejected scopes, credential binding and storage failures, safe
cancellation, canonical-token revocation, UI states and scaling, envelope limits,
encryption gates, mount identity, and disconnected-folder protection. None of
these tests reads a real wallet or authorizes an account.

Before release, use the maintainer's development client/test account to verify
browser consent, returning-account reconnect, browsing and downloading a chosen
plain test file, one Network shortcut, cancellation, revoked permissions, offline
failure, normal disconnect, and sign-out. Confirm uploads/deletes remain disabled
and repeat after an idle Explorer restart. Test Google-native export separately.
Then complete the production verification and packaging process. Do not call an
unconfigured build or a synthetic mount a working Google account connection.
