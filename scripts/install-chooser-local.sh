#!/usr/bin/env bash
# Install only the chooser, never replace/stop a running file manager.
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
build="${1:-$root/build}"
prefix="${2:-$HOME/.local}"
if [[ "$(id -u)" == 0 ]]; then
    echo "Install the chooser as your normal desktop user, not root." >&2
    exit 1
fi
if [[ "$prefix" != /* ]]; then
    echo "The installation prefix must be absolute." >&2
    exit 1
fi
for binary in explorer-win7-chooser explorer-win7-chooser-client; do
    if [[ ! -x "$build/$binary" ]]; then
        echo "Missing $build/$binary; configure and build the project first." >&2
        exit 1
    fi
done
# CMake replaces these files without signaling any running process.
cmake --install "$build" --prefix "$prefix" --component Chooser
if command -v dbus-send >/dev/null 2>&1 && [[ -n "${DBUS_SESSION_BUS_ADDRESS:-}" ]]; then
    # Refresh only activation-file discovery. Do not change activation's GUI
    # environment, which belongs to the login session/compositor.
    dbus-send --session --type=method_call --print-reply \
        --dest=org.freedesktop.DBus /org/freedesktop/DBus \
        org.freedesktop.DBus.ReloadConfig >/dev/null || \
        echo "Could not reload D-Bus activation files; log out/in if activation fails." >&2
    owner="$(dbus-send --session --type=method_call --print-reply \
        --dest=org.freedesktop.DBus /org/freedesktop/DBus \
        org.freedesktop.DBus.NameHasOwner string:org.win7explorer.FileChooser1 2>/dev/null || true)"
    if [[ "$owner" == *"boolean true"* ]]; then
        echo "The chooser service is already running with the old code."
        echo "Restart that chooser service only after its clients finish/cancel their requests."
        echo "No Explorer window or operation was stopped. A new login also loads the update."
    fi
fi
printf 'Chooser: %s/bin/explorer-win7-chooser\nClient: %s/bin/explorer-win7-chooser-client\nActivation: %s/share/dbus-1/services/org.win7explorer.FileChooser1.service\n' \
    "$prefix" "$prefix" "$prefix"
echo "The service starts on the next client request; nothing was launched by this installer."
if [[ "$prefix" != "$HOME/.local" ]]; then
    echo "For a custom prefix, its share directory must already be in your session's XDG_DATA_DIRS."
fi
