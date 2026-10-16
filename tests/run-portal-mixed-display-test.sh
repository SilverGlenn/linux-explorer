#!/usr/bin/env bash
# Native Wayland backend plus Xwayland caller, isolated from the user's session.
set -euo pipefail
if [[ "${EXPLORER_PORTAL_MIXED_INSIDE:-}" == 1 ]]; then
    export QT_QPA_PLATFORM=xcb
    unset XDG_ACTIVATION_TOKEN DESKTOP_STARTUP_ID
    result=0
    if [[ -z "${DISPLAY:-}" || -z "${WAYLAND_DISPLAY:-}" ]]; then
        echo "Private KWin session did not provide both Xwayland and Wayland displays." >&2
        result=1
    else
        "$EXPLORER_PORTAL_MIXED_TEST" || result=$?
    fi
    printf '%s\n' "$result" > "$EXPLORER_PORTAL_MIXED_STATUS"
    exit "$result"
fi
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
build="$(cd -- "${1:-$root/build}" && pwd)"
kwin="${2:-$(command -v kwin_wayland)}"
export EXPLORER_PORTAL_BINARY="$build/explorer-win7-portal"
export EXPLORER_PORTAL_MIXED_TEST="$build/portal_mixed_display_tests"
export EXPLORER_PORTAL_MIXED_TRACE="$build/portal-mixed-display-client"
export EXPLORER_PORTAL_MIXED_STATUS
EXPLORER_PORTAL_MIXED_STATUS="$(mktemp -t explorer-portal-mixed.XXXXXXXX)"
trap 'rm -f -- "$EXPLORER_PORTAL_MIXED_STATUS"' EXIT
export EXPLORER_PORTAL_MIXED_INSIDE=1
export QT_QPA_PLATFORM=offscreen QT_STYLE_OVERRIDE=Fusion
export LIBGL_ALWAYS_SOFTWARE=1 KWIN_COMPOSE=Q
unset DISPLAY WAYLAND_DISPLAY XDG_ACTIVATION_TOKEN DESKTOP_STARTUP_ID
# run-ui-tests creates a fresh home/runtime and a bus with no desktop activators.
# KWin exports its own two display endpoints to the exit-with-session process.
"$root/tests/run-ui-tests.sh" /usr/bin/timeout 100 "$kwin" \
    --virtual --width 1024 --height 768 --socket explorer-portal-mixed-test \
    --xwayland --no-lockscreen --no-global-shortcuts --no-kactivities \
    --exit-with-session "$root/tests/run-portal-mixed-display-test.sh"
# Some KWin versions return success even when the session child failed.
read -r result < "$EXPLORER_PORTAL_MIXED_STATUS" || {
    echo "The private mixed-display test session did not produce a result." >&2
    exit 1
}
[[ "$result" == 0 ]]
