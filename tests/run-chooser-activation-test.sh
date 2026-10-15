#!/usr/bin/env bash
# End-to-end installation/activation and wire-client check on a PRIVATE bus.
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ "${1:-}" == --inside ]]; then
    prefix="$2"
    work="$3"
    service_pid=""
    cleanup_service() {
        # This PID was obtained from THIS private bus, never the desktop bus.
        if [[ -n "$service_pid" ]]; then kill "$service_pid" 2>/dev/null || true; fi
    }
    trap cleanup_service EXIT
    client="$prefix/bin/explorer-win7-chooser-client"
    expect_client() {
        local expected="$1" output="$2" result=0
        shift 2
        timeout 20 "$client" "$@" > "$output" || result=$?
        if [[ "$result" != "$expected" ]]; then
            echo "Client returned $result, expected $expected; result in $output" >&2
            return 1
        fi
    }
    mkdir -p "$work/files # % Ω"
    expect_client 1 "$work/open.json" open --deb --title 'Select a package' \
        --start "$work/files # % Ω" --cancel-after 25
    read -r kind service_pid < <(busctl --address="$DBUS_SESSION_BUS_ADDRESS" call \
        org.freedesktop.DBus /org/freedesktop/DBus org.freedesktop.DBus \
        GetConnectionUnixProcessID s org.win7explorer.FileChooser1)
    [[ "$kind" == u && "$service_pid" =~ ^[0-9]+$ ]]
    [[ "$(readlink "/proc/$service_pid/exe")" == "$prefix/bin/explorer-win7-chooser" ]]
    busctl --address="$DBUS_SESSION_BUS_ADDRESS" get-property \
        org.win7explorer.FileChooser1 /org/win7explorer/FileChooser1 \
        org.win7explorer.FileChooser1 Version | grep -qx 'u 1'
    expect_client 1 "$work/save.json" save --log --name 'GDebi # 50% Ω' \
        --start "$work/files # % Ω" --cancel-after 25
    expect_client 2 "$work/failed.json" open --deb --modal --start "$work"
    expect_client 1 "$work/concurrent-a.json" open --deb --start "$work" --cancel-after 80 &
    a=$!
    expect_client 1 "$work/concurrent-b.json" save --log --start "$work" --cancel-after 40 &
    b=$!
    wait "$a"
    wait "$b"
    python3 - "$work" <<'PY'
import json
import pathlib
import sys
work = pathlib.Path(sys.argv[1])
handles = set()
for name in ('open', 'save', 'concurrent-a', 'concurrent-b', 'failed'):
    result = json.loads((work / (name + '.json')).read_text())
    expected = 2 if name == 'failed' else 1
    assert result['status'] == expected, result
    assert result['state'] == ('failed' if expected == 2 else 'canceled'), result
    assert result.get('uris', []) == [], result
    assert result['request_handle'] not in handles, result
    handles.add(result['request_handle'])
assert list((work / 'files # % Ω').iterdir()) == [], 'Save must not write files'
PY
    for name in io.github.LinuxExplorer.Win7 org.freedesktop.FileManager1; do
        busctl --address="$DBUS_SESSION_BUS_ADDRESS" call org.freedesktop.DBus \
            /org/freedesktop/DBus org.freedesktop.DBus NameHasOwner s "$name" | grep -qx 'b false'
    done
    echo 'Private-bus chooser install, activation, Open/Save, cancellation, failure and concurrency: PASS'
    exit 0
fi
build="$(cd -- "${1:-$root/build}" && pwd)"
sandbox="$(mktemp -d -t explorer-chooser-activation.XXXXXXXX)"
trap 'rm -rf -- "$sandbox"' EXIT
export HOME="$sandbox/home"
export XDG_CONFIG_HOME="$sandbox/config"
export XDG_DATA_HOME="$sandbox/data"
export XDG_CACHE_HOME="$sandbox/cache"
export XDG_RUNTIME_DIR="$sandbox/runtime"
# Do not accidentally discover services from the user's data directories.
export XDG_DATA_DIRS="$sandbox/empty-data"
mkdir -p "$HOME" "$XDG_CONFIG_HOME" "$XDG_DATA_HOME" "$XDG_CACHE_HOME" "$XDG_RUNTIME_DIR" "$XDG_DATA_DIRS"
chmod 700 "$XDG_RUNTIME_DIR"
prefix="$sandbox/install # % Ω"
cmake --install "$build" --prefix "$prefix" --component Chooser > "$sandbox/install.log"
# Deliberately only this activation directory; no KDE services or desktop bus.
config="$sandbox/session-bus.conf"
printf '%s\n' '<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN" "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">' \
    '<busconfig><type>session</type><listen>unix:tmpdir=/tmp</listen><auth>EXTERNAL</auth>' \
    '<policy context="default"><allow own="*"/><allow send_destination="*"/><allow receive_sender="*"/></policy>' \
    "<servicedir>$prefix/share/dbus-1/services</servicedir></busconfig>" > "$config"
export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-offscreen}"
export QT_STYLE_OVERRIDE="${QT_STYLE_OVERRIDE:-Fusion}"
dbus-run-session --config-file="$config" -- "$0" --inside "$prefix" "$sandbox"
