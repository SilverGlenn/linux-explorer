#!/usr/bin/env bash
# Isolate the bus daemon's activation environment as well as the test process.
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
binary="${1:-$root/build/explorer_ui_tests}"
if (($#)); then shift; fi
if [[ -n "${EXPLORER_NATIVE_THEME:-}" ]]; then
    export EXPLORER_NATIVE_CONFIG_DIR="${XDG_CONFIG_HOME:-$HOME/.config}"
    export EXPLORER_NATIVE_REFERENCE_FONTS="${XDG_DATA_HOME:-$HOME/.local/share}/linux-explorer-win7/reference-fonts"
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
if [[ -n "${EXPLORER_NATIVE_THEME:-}" ]]; then
    # QTEST_MAIN component suites also need the selected Kvantum theme. Only
    # preferences are copied; the live config and desktop bus remain untouched.
    mkdir -p "$XDG_CONFIG_HOME/Kvantum"
    for preference in kdeglobals Kvantum/kvantum.kvconfig; do
        if [[ -f "$EXPLORER_NATIVE_CONFIG_DIR/$preference" ]]; then
            cp -- "$EXPLORER_NATIVE_CONFIG_DIR/$preference" "$XDG_CONFIG_HOME/$preference"
        fi
    done
    # User-supplied fonts are visual fixtures, never package/repository assets.
    task_fonts="$XDG_DATA_HOME/linux-explorer-win7/reference-fonts"
    mkdir -p "$task_fonts"
    for task_font in segoeui.ttf segoeuib.ttf segoeuii.ttf segoeuiz.ttf micross.ttf tahoma.ttf lucon.ttf; do
        if [[ -f "$EXPLORER_NATIVE_REFERENCE_FONTS/$task_font" ]]; then
            cp -- "$EXPLORER_NATIVE_REFERENCE_FONTS/$task_font" "$task_fonts/$task_font"
        fi
    done
fi
dbus-run-session --config-file="$root/tests/session-bus.conf" -- "$binary" "$@"
