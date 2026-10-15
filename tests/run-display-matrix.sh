#!/usr/bin/env bash
# Qt 6's configurable offscreen backend is required (tested with Qt 6.10).
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
build="${1:-$root/build}"
build="$(cd -- "$build" && pwd)"
for entry in '96dpi 1 800x600' '120dpi 1.25 1024x768' '144dpi 1.5 1200x900'; do
    read -r name scale display <<< "$entry"
    echo "Testing $display at $name ($scale scaling)"
    log="$build/test-$name.log"
    : > "$log"
    for suite in address_controls_tests thumbnail_loading_tests details_pane_tests list_chrome_tests navigation_state_tests browser_navigation_tests file_chooser_tests chooser_service_tests pdf_preview_tests explorer_ui_tests; do
        echo "  $suite" | tee -a "$log"
        QT_QPA_PLATFORM="offscreen:configfile=$root/tests/screens/$name.json" \
          QT_STYLE_OVERRIDE=Fusion QT_SCALE_FACTOR="$scale" \
          EXPLORER_EXPECT_DISPLAY="$display" EXPLORER_EXPECT_SCALE="$scale" \
          EXPLORER_SNAPSHOTS="$build/screenshots-$name" \
          "$root/tests/run-ui-tests.sh" "$build/$suite" >> "$log" 2>&1 || {
            tail -n 80 "$log"
            exit 1
          }
    done
    grep 'Totals:' "$log"
done
