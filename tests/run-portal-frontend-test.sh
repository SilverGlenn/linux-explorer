#!/usr/bin/env bash
# Real frontend, private bus and curated metadata; no desktop service discovery.
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ "${1:-}" != --inside ]]; then
    exec "$root/tests/run-ui-tests.sh" bash "$0" --inside "$@"
fi
shift
export XDG_DATA_DIRS="$HOME/empty-data"
export XDG_CONFIG_DIRS="$HOME/empty-config"
export XDG_CURRENT_DESKTOP=ExplorerTest
export GSETTINGS_SCHEMA_DIR=/usr/share/glib-2.0/schemas
export GSETTINGS_BACKEND=memory
export XDG_DESKTOP_PORTAL_DIR="$XDG_DATA_HOME/xdg-desktop-portal/portals"
unset XDG_DESKTOP_PORTAL_TEST_APP_INFO_KIND XDG_DESKTOP_PORTAL_TEST_HOST_APPID
unset XDG_DESKTOP_PORTAL_TEST_FLATPAK_METADATA XDG_DESKTOP_PORTAL_TEST_SNAP_METADATA
unset XDG_DESKTOP_PORTAL_TEST_LINYAPS_METADATA
mkdir -p "$XDG_DESKTOP_PORTAL_DIR" "$XDG_CONFIG_HOME/xdg-desktop-portal" "$XDG_DATA_DIRS" "$XDG_CONFIG_DIRS"
cp "$root/portal/explorer-win7.portal" "$XDG_DESKTOP_PORTAL_DIR/"
printf '[preferred]\ndefault=none\norg.freedesktop.impl.portal.FileChooser=explorer-win7\n' > "$XDG_DESKTOP_PORTAL_DIR/portals.conf"
exec "$@"
