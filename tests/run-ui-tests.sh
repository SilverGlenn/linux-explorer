#!/usr/bin/env bash
# Isolate the bus daemon's activation environment as well as the test process.
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
binary="${1:-$root/build/explorer_ui_tests}"
if (($#)); then shift; fi
if [[ -n "${EXPLORER_NATIVE_THEME:-}" ]]; then
    export EXPLORER_NATIVE_CONFIG_DIR="${XDG_CONFIG_HOME:-$HOME/.config}"
fi
sandbox="$(mktemp -d -t explorer-tests.XXXXXXXX)"
trap 'rm -rf -- "$sandbox"' EXIT
export HOME="$sandbox/home"
export XDG_CONFIG_HOME="$sandbox/config"
export XDG_DATA_HOME="$sandbox/data"
export XDG_CACHE_HOME="$sandbox/cache"
export XDG_RUNTIME_DIR="$sandbox/runtime"
mkdir -p "$HOME" "$XDG_CONFIG_HOME" "$XDG_DATA_HOME" "$XDG_CACHE_HOME" "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"
dbus-run-session --config-file="$root/tests/session-bus.conf" -- "$binary" "$@"
