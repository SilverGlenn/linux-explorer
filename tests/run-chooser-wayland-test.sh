#!/usr/bin/env bash
# Optional real Wayland protocol test; no connection to the desktop compositor.
set -euo pipefail
if [[ "${EXPLORER_CHOOSER_WAYLAND_INSIDE:-}" == 1 ]]; then
    export QT_QPA_PLATFORM=wayland
    export WAYLAND_DISPLAY=explorer-chooser-test
    unset DISPLAY XDG_ACTIVATION_TOKEN DESKTOP_STARTUP_ID
    result=0
    "$EXPLORER_CHOOSER_WAYLAND_TEST" || result=$?
    printf '%s\n' "$result" > "$EXPLORER_CHOOSER_WAYLAND_STATUS"
    exit "$result"
fi
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
build="$(cd -- "${1:-$root/build}" && pwd)"
kwin="${2:-$(command -v kwin_wayland)}"
export EXPLORER_CHOOSER_BINARY="$build/explorer-win7-chooser"
export EXPLORER_PORTAL_BINARY="$build/explorer-win7-portal"
export EXPLORER_CHOOSER_WAYLAND_TEST="$build/chooser_wayland_tests"
export EXPLORER_CHOOSER_WAYLAND_TRACE="$build/chooser-wayland-client.log"
export EXPLORER_CHOOSER_WAYLAND_STATUS
EXPLORER_CHOOSER_WAYLAND_STATUS="$(mktemp -t explorer-chooser-wayland.XXXXXXXX)"
trap 'rm -f -- "$EXPLORER_CHOOSER_WAYLAND_STATUS"' EXIT
export EXPLORER_CHOOSER_WAYLAND_INSIDE=1
export QT_QPA_PLATFORM=offscreen QT_STYLE_OVERRIDE=Fusion
export LIBGL_ALWAYS_SOFTWARE=1 KWIN_COMPOSE=Q
unset DISPLAY WAYLAND_DISPLAY XDG_ACTIVATION_TOKEN DESKTOP_STARTUP_ID
"$root/tests/run-ui-tests.sh" /usr/bin/timeout 60 "$kwin" \
    --virtual --width 800 --height 600 --socket explorer-chooser-test \
    --no-lockscreen --no-global-shortcuts --no-kactivities \
    --exit-with-session "$root/tests/run-chooser-wayland-test.sh"
# Some compositor versions exit successfully even when their child failed.
read -r result < "$EXPLORER_CHOOSER_WAYLAND_STATUS" || {
    echo "The private Wayland test session did not produce a result." >&2
    exit 1
}
[[ "$result" == 0 ]]
