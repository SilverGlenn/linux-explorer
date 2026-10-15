#!/usr/bin/env bash
# Opt-in ONLY to FileChooser; never stop portal/KWin/app processes automatically.
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
build="${1:-$root/build}"
prefix="${2:-$HOME/.local}"
enable="${3:-}"
if [[ "$(id -u)" == 0 ]]; then
    echo "Install as your desktop user, not root." >&2
    exit 1
fi
if [[ "$prefix" != /* || ( -n "$enable" && "$enable" != --enable ) ]]; then
    echo "Usage: $0 [build-directory] [absolute-prefix] [--enable]" >&2
    exit 1
fi
for binary in explorer-win7-portal explorer-win7-portal-client; do
    if [[ ! -x "$build/$binary" ]]; then
        echo "Missing $build/$binary; configure and build the portal targets first." >&2
        exit 1
    fi
done
cmake --install "$build" --prefix "$prefix" --component Portal
if command -v dbus-send >/dev/null && [[ -n "${DBUS_SESSION_BUS_ADDRESS:-}" ]]; then
    dbus-send --session --type=method_call --print-reply \
        --dest=org.freedesktop.DBus /org/freedesktop/DBus \
        org.freedesktop.DBus.ReloadConfig >/dev/null || \
        echo "Could not refresh activation files; a new login may be needed." >&2
fi
if [[ "$enable" == --enable ]]; then
    "$prefix/bin/explorer-win7-portal-setup" enable --prefix "$prefix"
else
    echo "Installed only. No portal preferences were changed."
    echo "Explicit FileChooser-only opt-in: $prefix/bin/explorer-win7-portal-setup enable --prefix \"$prefix\""
fi
printf 'Backend: %s/bin/explorer-win7-portal\nClient: %s/bin/explorer-win7-portal-client\nMetadata: %s/share/xdg-desktop-portal/portals/explorer-win7.portal\n' \
    "$prefix" "$prefix" "$prefix"
echo "No application, chooser, KDE backend, or portal frontend was stopped."
echo "A running portal frontend/backend keeps its old configuration/code."
echo "Apply with an idle frontend/backend restart, or a new login; do not interrupt active portal requests or sessions."
