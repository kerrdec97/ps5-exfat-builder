# exFAT Image Builder — UI Refactor Build (Stages 1–3 applied)

This is your complete project with the UI refactor work from Stages 1, 2,
and 3 applied. It is ready to run or build as-is.

## What's included

A full copy of your original project, with these changes layered in:

- **Stage 1** — `tkinter_theme.py` extended with a design-token system
  (SPACING, RADIUS, ELEVATION, STATUS_COLORS, etc.) + safe helpers.
  Purely additive; nothing old was changed or removed.
- **Stage 2** — 7 new reusable UI components under `ui/shared/`:
  `badges.py`, `sidebar.py`, `header.py`, `dashboard.py`, `queue.py`,
  `toolbar.py`, `empty_state.py`. Standalone, not wired into tabs.
- **Stage 3** — `exfat_builder.py` shell refactor:
  - `_switch_tab()` now uses a single tab registry instead of three
    fragile parallel lists. Same public method name; unknown/old keys
    now safely fall back to the default tab instead of crashing.
  - A secondary **sidebar** was added (additive — the top tab strip is
    unchanged and remains primary navigation).
  - The legacy `last_tab` migration map is byte-for-byte unchanged.

## What has NOT changed

- No backend build / extract / convert / FTP / Klog logic was touched.
- All 15 `ui/tab_*.py` modules are byte-for-byte identical to your
  original upload.
- The Windows admin-elevation guard is fully intact.
- Stage 4 (the real Overview/Dashboard tab) is NOT in this build yet.

## How to run

    py -3.11 exfat_builder.py

## How to build the .exe

Same as before — `build.bat` is unchanged and still builds from the
top-level `exfat_builder.py`:

    build.bat

It installs pyinstaller / pillow / tkinterdnd2, then produces
`dist\exFAT Image Builder.exe`.

## Notes

- `ui/exfat_builder.py`, `ui/tab_ftp.zip`, and `ui/ui.zip` are stale
  leftovers from your original archive. They are NOT used by `build.bat`
  (which builds the top-level `exfat_builder.py`). They were left in
  place because removing them was not approved — they are harmless but
  can be deleted safely if you wish.
- The sidebar adds ~210px of left width. On a narrow window the content
  area is tighter than before. If that feels cramped, a collapsible
  sidebar can be added in a later stage.
- All Python files in this package compile cleanly.
