#!/usr/bin/env bash
# Real installed backend activation and standard client on a disposable bus.
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ "${1:-}" == --inside ]]; then
    prefix="$2"; work="$3"; build="$4"
    frontend_pid=""; backend_pid=""
    cleanup() {
        for pid in "$backend_pid" "$frontend_pid"; do
            if [[ -n "$pid" ]]; then kill "$pid" 2>/dev/null || true; fi
        done
        wait 2>/dev/null || true
    }
    trap cleanup EXIT
    # This is the shipped installer, on this private bus/temp config only.
    "$root/scripts/install-portal-local.sh" "$build" "$prefix" --enable > "$build/portal-install.log" 2>&1
    setup="$prefix/bin/explorer-win7-portal-setup"
    "$setup" status > "$work/status.json"
    python3 - "$work/status.json" <<'PY'
import json, sys
s = json.load(open(sys.argv[1]))
assert s['configured'], s
assert s['other_preferences'] == {'default': 'none', 'org.freedesktop.impl.portal.Settings': 'kde;', 'org.freedesktop.impl.portal.ScreenCast': 'kde;'}, s
PY
    # The frontend also scans compiled-in system metadata dirs. Override only
    # its child environment, AFTER exercising the helper's ordinary discovery.
    metadata="$prefix/share/xdg-desktop-portal/portals"
    cp "$XDG_CONFIG_HOME/xdg-desktop-portal/kde-portals.conf" "$metadata/portals.conf"
    XDG_DESKTOP_PORTAL_DIR="$metadata" /usr/libexec/xdg-desktop-portal --verbose > "$build/portal-activation-frontend.log" 2>&1 &
    frontend_pid=$!
    ready=0
    for ((attempt=0; attempt<200; ++attempt)); do
        kill -0 "$frontend_pid"
        if busctl --address="$DBUS_SESSION_BUS_ADDRESS" call org.freedesktop.DBus /org/freedesktop/DBus \
            org.freedesktop.DBus NameHasOwner s org.freedesktop.portal.Desktop | grep -qx 'b true'; then ready=1; break; fi
        sleep 0.05
    done
    [[ "$ready" == 1 ]]
    client="$prefix/bin/explorer-win7-portal-client"
    expect_cancel() {
        local result=0 output="$1"; shift
        timeout 20 "$client" "$@" > "$output" 2>> "$build/portal-activation-client.log" || result=$?
        if [[ "$result" != 1 ]]; then
            echo "Portal client returned $result, expected cancellation (1)" >&2
            cat "$output" >&2
            return 1
        fi
    }
    mkdir -p "$work/files # % Ω"
    expect_cancel "$work/open.json" open --start "$work/files # % Ω" --multiple --cancel-after 25
    read -r kind backend_pid < <(busctl --address="$DBUS_SESSION_BUS_ADDRESS" call org.freedesktop.DBus \
        /org/freedesktop/DBus org.freedesktop.DBus GetConnectionUnixProcessID s org.freedesktop.impl.portal.desktop.explorerwin7)
    [[ "$kind" == u && "$backend_pid" =~ ^[0-9]+$ ]]
    [[ "$(readlink "/proc/$backend_pid/exe")" == "$prefix/bin/explorer-win7-portal" ]]
    expect_cancel "$work/save.json" save --log --name 'never # % Ω' --start "$work/files # % Ω" --cancel-after 25
    expect_cancel "$work/batch.json" save-many --file 'one # %.txt' --file 'two Ω' --start "$work/files # % Ω" --cancel-after 25
    expect_cancel "$work/a.json" open --directory --start "$work/files # % Ω" --cancel-after 80 & a=$!
    expect_cancel "$work/b.json" save --start "$work/files # % Ω" --cancel-after 40 & b=$!
    wait "$a"; wait "$b"
    python3 - "$work" <<'PY'
import json, pathlib, sys
w = pathlib.Path(sys.argv[1]); handles = set()
for name in ('open', 'save', 'batch', 'a', 'b'):
    r = json.loads((w / (name + '.json')).read_text())
    assert r['status'] == 1 and r['state'] == 'canceled' and r['canceled_by_client'], r
    assert r['uris'] == [] and r['request_handle'] not in handles, r
    handles.add(r['request_handle'])
assert not list((w / 'files # % Ω').iterdir())
PY
    for name in org.freedesktop.FileManager1 io.github.LinuxExplorer.Win7; do
        busctl --address="$DBUS_SESSION_BUS_ADDRESS" call org.freedesktop.DBus \
            /org/freedesktop/DBus org.freedesktop.DBus NameHasOwner s "$name" | grep -qx 'b false'
    done
    # The backend must persist after all requests are closed.
    kill -0 "$backend_pid"
    "$setup" restore > "$work/restore.json"
    cmp "$work/before.conf" "$XDG_CONFIG_HOME/xdg-desktop-portal/kde-portals.conf"
    echo 'Installed special-prefix portal: real frontend, backend activation, CLI Open/Save/SaveFiles/concurrency/Close, no writes/names, FileChooser-only opt-in and rollback: PASS'
    exit 0
fi
build="$(cd -- "${1:-$root/build}" && pwd)"
sandbox="$(mktemp -d -t explorer-portal-activation.XXXXXXXX)"
trap 'rm -rf -- "$sandbox"' EXIT
export HOME="$sandbox/home" XDG_CONFIG_HOME="$sandbox/config" XDG_DATA_HOME="$sandbox/data"
export XDG_CACHE_HOME="$sandbox/cache" XDG_RUNTIME_DIR="$sandbox/runtime" XDG_STATE_HOME="$sandbox/state"
export XDG_CONFIG_DIRS="$sandbox/empty-config" XDG_CURRENT_DESKTOP=KDE
prefix="$sandbox/install # % Ω"
export XDG_DATA_DIRS="$prefix/share"
export GSETTINGS_SCHEMA_DIR=/usr/share/glib-2.0/schemas GSETTINGS_BACKEND=memory
unset XDG_DESKTOP_PORTAL_DIR XDG_DESKTOP_PORTAL_TEST_APP_INFO_KIND XDG_DESKTOP_PORTAL_TEST_HOST_APPID
unset XDG_DESKTOP_PORTAL_TEST_FLATPAK_METADATA XDG_DESKTOP_PORTAL_TEST_SNAP_METADATA XDG_DESKTOP_PORTAL_TEST_LINYAPS_METADATA
mkdir -p "$HOME" "$XDG_CONFIG_HOME/xdg-desktop-portal" "$XDG_DATA_HOME" "$XDG_CACHE_HOME" "$XDG_RUNTIME_DIR" "$XDG_CONFIG_DIRS"
chmod 700 "$XDG_RUNTIME_DIR"
printf '[preferred]\ndefault=none\norg.freedesktop.impl.portal.Settings=kde;\norg.freedesktop.impl.portal.ScreenCast=kde;\n' > "$sandbox/before.conf"
cp "$sandbox/before.conf" "$XDG_CONFIG_HOME/xdg-desktop-portal/kde-portals.conf"
# Activation is restricted to exactly the installed FileChooser backend.
config="$sandbox/bus.conf"
printf '%s\n' '<busconfig><type>session</type><listen>unix:tmpdir=/tmp</listen><auth>EXTERNAL</auth>' \
    '<policy context="default"><allow own="*"/><allow send_destination="*"/><allow receive_sender="*"/></policy>' \
    "<servicedir>$prefix/share/dbus-1/services</servicedir></busconfig>" > "$config"
export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-offscreen}" QT_STYLE_OVERRIDE="${QT_STYLE_OVERRIDE:-Fusion}"
: > "$build/portal-activation-client.log"
dbus-run-session --config-file="$config" -- bash "$0" --inside "$prefix" "$sandbox" "$build"
python3 - "$prefix" <<'PY'
import pathlib, sys
p = pathlib.Path(sys.argv[1])
service = (p / 'share/dbus-1/services/org.freedesktop.impl.portal.desktop.explorerwin7.service').read_text()
assert 'Exec="' + str(p) + '/bin/explorer-win7-portal"\n' in service
for identity in ('linux-explorer-win7-dialog', 'linux-explorer-win7-chooser-dialog'):
    text = (p / ('share/applications/' + identity + '.desktop')).read_text()
    assert 'Exec="' + str(p).replace('%', '%%') + '/bin/explorer-win7-portal"\n' in text
    assert 'NoDisplay=true\n' in text
    assert 'StartupWMClass=' + identity + '\n' in text
PY
if command -v desktop-file-validate >/dev/null; then
    desktop-file-validate "$prefix/share/applications/linux-explorer-win7-dialog.desktop" \
        "$prefix/share/applications/linux-explorer-win7-chooser-dialog.desktop"
fi
