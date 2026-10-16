# Windows 7 Explorer fidelity and UX Guide checklist

## Priority

1. Reproduce the observed Windows 7 Explorer client layout and interaction.
2. Use Microsoft's **Windows 7-era** UX Guide to check behavior, discoverability,
   keyboard access, feedback, and scaling.
3. Adapt operating-system services honestly to Linux/KDE. Do not add dead Windows
   commands, fabricate metadata, import Fluent/ribbon UI, or turn the application
   into a generic modern file manager.

The guide itself states that it was created for Windows 7. Its general rules
are guardrails, not a replacement for the Explorer-specific reference.
For example, the guest's blue, read-only Libraries instruction is preserved;
it is not made into a fake hyperlink just to reconcile a general text rule.

## Sources consulted

- [Design basics](https://learn.microsoft.com/en-us/windows/win32/uxguide/)
- [Power and simplicity](https://learn.microsoft.com/en-us/windows/win32/uxguide/how-to-design-desktop-ux)
- [UX checklist](https://learn.microsoft.com/en-us/windows/win32/uxguide/top-violations)
- [Keyboard interaction](https://learn.microsoft.com/en-us/windows/win32/uxguide/inter-keyboard)
- [Toolbars](https://learn.microsoft.com/en-us/windows/win32/uxguide/cmd-toolbars)
- [Layout](https://learn.microsoft.com/en-us/windows/win32/uxguide/vis-layout)

Primary visual evidence remains [the VM reference pass](windows7-reference.md).
Downloaded guide/Qt implementation references are development-cache files, not
application resources or material shipped in the fork.

## Applied checks

| Area | Explorer-specific implementation and validation |
| --- | --- |
| Probable defaults | Libraries opens first. Favorites starts with Desktop, Downloads, Recent Places; Libraries is expanded, Computer/Network collapsed. KDE's automatic Trash/Recent Files bookmarks are not inserted as default Windows Favorites. Explicit custom bookmarks remain. |
| Recognizable controls | Round navigation buttons; split view button; original blue-question-mark Help and two-pane Preview glyphs. The new glyphs are drawn locally at multiple resolutions, not extracted from the ISO. |
| Toolbar vs. comprehensive menus | Contextual commands stay on the command strip; hidden classic menu bar retains File, Edit, View, Tools, Help. Linux destinations are under View/Go to, not an extra top-level menu. |
| Progressive disclosure | Current-folder breadcrumb dropdown; hidden ancestors behind the leading chevron; command overflow behind a chevron. These menus use actual navigation/actions, not visual placeholders. |
| View consistency | Verified five-mode main-button cycle; eight-choice flyout. Libraries now has a real Details table with Name, Date modified, Type, Size, shared selection, full-row highlighting, and no invented date/size values. |
| Readable layout | Nominal Libraries tiles are 256 × 64; ordinary file tiles remain 256 × 56. Libraries reflows into two columns at the guest's client width, including Kvantum's scrollbar reservation behavior. A subtle rule separates its instruction from its tiles. |
| Keyboard | Alt+D and F6 actually focus the address editor; queued widget visibility no longer leaves typing on the file list. F6 skips hidden/disabled panes. Library items support Return and keyboard context menus. Buttons expose accessible names. |
| Predictable state | F5 restores library selection and reports the completed count, rather than leaving 0 items in the footer. Preview's tooltip changes between Show and Hide, while sharing Alt+P's checked action. |
| Real tasks | Actual KIO rename/copy/cut-paste/move and Undo are tested on temporary files. Rename updates the cached selected KFileItem so Copy cannot continue using the old filename. Basename-only rename selection and Escape cancellation are also covered. |
| Feedback without invented data | Asynchronous, bounded, read-only text preview; stale results discarded. Creation time is displayed only when KIO supplies it. Selected metadata and unselected counts remain separate. |
| Screen space and scaling | Fresh windows fit the available screen; the minimum allows useful 500 px-wide review windows. Fixed right-hand toolbar controls remain onscreen while excess commands overflow. Automated 800×600/100%, 1024×768/125%, and 1200×900/150% checks include startup geometry, interaction, and client images. |
| Help semantics | F1, the Help icon, and View help open a modeless local quick-help window, not About. This is a Linux fallback, not a fake Windows Help and Support viewer. |
| Safe integration | Separate executable/config/desktop/private bus identity; default-manager registration is opt-in. Tests isolate both the Qt process and D-Bus environment, and disable unneeded desktop-service activation. |
| Google Drive | The task-named command in Tools, Computer, and Network opens an Aero dialog that blocks only its parent. Google's unchanged pre-approved sign-in button sits within that layout, preserving keyboard/default/focus/accessibility behavior. Explicit sign-in uses the system browser and secure desktop wallet; no user project, client JSON, terminal, or encryption password appears in onboarding. A verified read-only mount/listing appears as Google Drive (G:) under Computer → Hard Disk Drives, with the existing drive artwork and Google’s unchanged logo badge; Network bookmarking is optional. Mount observation uses bounded kernel metadata, and unknown remote quota never becomes host-disk capacity. Cancel, retry, disconnect, and sign-out have visible task states and plain text errors; status grows to fit scaling. Missing app registration disables sign-in with distributor guidance. See [the integration notes](google-drive.md). |

## Reproduce the checks

```bash
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build -j2
EXPLORER_SNAPSHOTS="$PWD/build/screenshots" ctest --test-dir build --output-on-failure
tests/run-display-matrix.sh build
```

The display matrix requires Qt's configurable offscreen platform backend (tested
with Qt 6.10.2). It verifies the requested screen geometry and scale rather than
silently accepting the platform's default 800×800 screen.

For installed Kvantum/libAeroQt client styling without touching desktop focus:

```bash
QT_QPA_PLATFORM=offscreen QT_STYLE_OVERRIDE=kvantum EXPLORER_NATIVE_THEME=1 \
  EXPLORER_SNAPSHOTS="$PWD/build/screenshots-themed" \
  tests/run-ui-tests.sh "$PWD/build/explorer_ui_tests" renderSnapshots
```

These are client captures. They do not establish compositor-frame/glass parity,
screen-reader certification, complete high-contrast coverage, or every locale's
layout. Those require further targeted validation.

## Deliberate limits

- XDG-backed one-location shortcuts, not Windows multi-location libraries.
- Discrete view-slider stops, not continuous icon sizing.
- KIO previews/services instead of Windows preview/ACL/HomeGroup components.
- Known extensions remain visible by default for safety; Folder Options can
  change that.
- Exact folder/sidebar artwork and font metrics depend on installed themes.
- The Basic-frame VM cannot validate Aero Glass or compositor shadows.
