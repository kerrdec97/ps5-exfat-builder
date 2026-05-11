# v2.6.0 — Tab Consolidation, Build Pipeline Rewrite, UI Refresh

> ⚠️ To **play backported games** on your PS5 you need BestPig's BackPork
> payload: <https://github.com/BestPig/BackPork>

This release rolls up everything from the internal v2.5.0 → v2.5.8 series
plus the polish work since. The headline changes are a complete rewrite
of the build pipeline (which fixes a six-layer bug chain that caused
silent build failures on real-world game paths), a major tab
consolidation that drops the visible top-bar count from 15 to 9, and a
cohesive UI refresh.

---

## 🗂️ Tab consolidation — fewer top-level tabs, sub-tabs underneath

The top tab bar previously held 15 tabs. Some were heavily used, others
were one-off panels that pushed the heavy hitters out of view on
narrower screens. v2.6.0 consolidates related tabs into sub-tab groups,
dropping the visible top-bar count to 9 without losing any
functionality.

- **exFAT** (top-level) → **Build / Extract / Edit exFAT** as sub-tabs
- **Library** (top-level) → **Source Dumps / Built Images** as sub-tabs
- **PS5** (top-level) → **Manager / FTP / Klog / Payloads / Y2JB** as sub-tabs
- **Settings** (top-level) → **General / Language / Help** as sub-tabs
- New top-level tabs: **ffpkg**, **Backports**, **Convert**, **Dump Rename**, **Credits**

Saved tab keys from prior versions are remapped automatically so your
last-active tab still reopens on launch.

---

## 🔨 Build pipeline — six-layer bug chain fixed

Real-world PS5 dumps with version-number parentheses in folder names
(e.g. `D:\keep\PPSA19639 Minecraft Preview (01.024.000)\PPSA19639`)
were silently failing to build. Six compounding bugs were fixed, each
masking the next:

1. **BAT swallowed PowerShell exit codes** — `if %RC% GTR 7` treated any
   exit code 1–7 as a robocopy warning, so PowerShell exceptions were
   reported as success. Builds claimed `[OK] Done` while producing no
   output file.
2. **BAT used `powershell.exe` for path checks** — nested quoting
   mangled paths with spaces or special characters, producing
   false-negative "eboot.bin not found" errors.
3. **BAT used `if (...)` blocks for error handling** — cmd's parser sees
   `)` inside paths like `(01.024.000)` as the block terminator and
   misparses every following line. Every `if (...)` block is now `if
   ... goto :label` with a handler at the bottom of the script. Safe
   for any path content.
4. **Drag-drop didn't auto-descend** — users routinely drop the outer
   wrapper folder. The Library scanner already descended; drag-drop,
   Browse, and multi-folder browse did not. A new `_resolve_game_root`
   helper now walks up to two levels deep looking for `eboot.bin` and
   is wired into every queue-add path.
5. **PowerShell `ErrorActionPreference = "Stop"` killed the script on
   stderr writes** — OSFMount, robocopy and format.com all write
   informational chatter to stderr even on a successful run. The
   script terminated before `$LASTEXITCODE` could be read. Every
   native-command call is now wrapped in a `Stop → Continue → Stop`
   sandwich; the exit code is the source of truth.
6. **`ss,4096` was a fictional OSFMount option** — the PS1 was passing
   `-o rw,rem,ss,4096` to OSFMount, which has no such option. OSFMount
   responded by dumping its full help text and exiting -1. The exFAT
   logical sector size is set by `format.com`, not by OSFMount, so
   this option was never needed. Dropped.

Result: builds with parenthesised version numbers, deeply nested
folders, OSFMount banner output, and non-default sector sizes now all
work cleanly.

---

## 🔍 Build failure diagnostics

When a build does fail, the "Item failed" dialog now shows the actual
PowerShell error reason instead of just the output path. A per-build
ring buffer captures the last 80 stdout lines and the dialog picks out
the relevant `[ERROR]` / `throw` / `Exception` lines automatically. The
OUTPUT LOG drawer auto-opens on failure so the full log is right there.

Verify failures are now non-blocking during a queue run — the failure
is logged and the queue dot turns amber, but the next item continues.
Single-item builds still show a modal dialog.

---

## 📊 Live build progress — real "files copied so far"

The status line now shows a real, live `X / Y files (%)` count that
ticks upward as the copy progresses. A background thread walks the
mounted destination drive every 5 seconds (cheap because metadata-only)
and the result is shown alongside the existing GB written / MB/s / ETA.

Numbers are comma-formatted (`22,364`) for readability. When the source
count thread is still warming up the display falls back gracefully.

Previously the count was sourced from robocopy's final summary line —
which only fires once per build, at the end — meaning during a 10-minute
copy the count display either stayed at 0 or showed the previous game's
total (the "22,364 / 48 files" bug).

---

## 🎨 UI refresh

### Cohesive button palette

Bright mint green was retired for action buttons app-wide. The new
palette:

- **Purple (brand accent)** — primary actions: Add to Queue, Scan, Apply
- **Muted teal** — secondary forward actions: Build All, Convert, Extract,
  Connect, Library Build All
- **Amber** — Pause (unchanged)
- **Ghost outline** — Force Dismount, tertiary actions (unchanged)
- **Red** — destructive actions (unchanged)
- Bright green is reserved for status dots only — where green
  semantically means "all good / connected / done"

### Build tab layout

- Compact drop zone: ~70px tall instead of ~200px, one-row layout with
  glyph + "Drop a game folder here, or click to browse"
- Redundant "Add to queue" card header removed — the drop zone already
  explained itself
- Auto-scroll wrapper: scrollbar appears only when content overflows
  (e.g. when the OUTPUT LOG drawer is open), hidden otherwise

### Library tab

- Folder chip strip now paints immediately on tab open (previously it
  stayed empty until you added/removed a folder, even with three
  saved folders)
- "Scanning folders:" label with hint "(click ✕ to stop scanning a
  folder)" added above the strip
- ✕ button on each chip is now 13pt bold with an always-visible dim
  pill background — readable as a button at rest, not just a glyph

---

## 🆕 Convert tab

New top-level tab for exFAT ↔ ffpkg conversions in either direction.
Uses the same UFS2Tool pipeline as the ffpkg builder. Spacious
Build-tab style with the same drop-zone and queue affordances.

---

## 🆕 Extract tab redesign

Extract (now a sub-tab of exFAT) was rewritten to match the spacious
Build-tab style. Drop zone, extract-to-folder picker, progress, and
output log integration all consistent with the rest of the app.

---

## 🆕 Edit exFAT tab

Edit exFAT (also a sub-tab of exFAT) lets you mount an existing
exFAT image and add/remove files via a built-in browser, then dismount
cleanly. No need to manually mount with OSFMount.

---

## 🆕 Credits tab

New top-level tab consolidating donor recognition (with a bulletproof
load chain — falls back through three tiers if the donors.json is
missing or corrupted) and GitHub contributor credits for the homebrew
ecosystem this tool depends on: **Nazky**, **BestPig**, **drakmor**,
**SvenGDK**, **ps5-payload-dev**, **NookieAI**, **stonemodder**.

---

## 🛠️ OSFMount detection

- Registry-based lookup added — finds OSFMount even when installed to a
  non-default location
- Accepts both `osfmount.com` and `osfmount.exe` paths
- Auto-heal: if a stored path is stale, the next launch silently
  searches for the current install
- Path is forwarded through the entire pipeline (Settings → Python →
  BAT → PS1) — no more "osfmount.com not found" errors during the build

---

## 🛠️ Robust dismount

A new `_dismount_drive_robust` helper replaces the old retry loop:

1. Stops our drive-poll loop first (releases our own Win32 handles
   that would block FSCTL_LOCK_VOLUME)
2. `osfmount -D` (force) × 3
3. `osfmount -d` × 3
4. `mountvol /D`
5. `FSCTL_LOCK_VOLUME` + `FSCTL_DISMOUNT_VOLUME`

Total budget ~30 seconds — long enough for Defender's post-copy scan
to settle. The legacy retry-osfmount-d loop is gone.

A `Force Dismount` button is wired up for the rare cases where Windows
leaves something hung. Defaults to virtual disks only; system drive
protection is on by default.

---

## 🛠️ Large game support

PS1 enumeration is now streaming — files are counted as they're
discovered instead of building a list in memory first. Verified
working on PS5 dumps with 200,000+ files (e.g. Demon's Souls).
robocopy `/J` (unbuffered I/O) and adaptive `/MT` thread counts based
on detected source media type.

NVMe drive detection now has multiple fallbacks — when `Get-PhysicalDisk`
returns nothing (common on some Windows builds with NVMe), the PS1
falls back to `MSFT_PhysicalDisk` via CIM, then to
`Win32_DiskDrive.InterfaceType`. Source media reported as `ssd` enables
`/MT:32` for fast multi-threaded copy.

---

## 🛠️ Other fixes

- Sanitized filenames so special characters in game titles don't break
  output paths
- Hidden cmd windows on every subprocess call (`creationflags=_NO_WIN_FLAGS`)
- Single global OUTPUT LOG at the bottom of every tab (no more duplicate
  log widgets)
- Pre-flight source scan catches missing eboot.bin, extreme path
  lengths and other common failure modes up front
- Donors load chain is bulletproof — `donors.json` → bundled fallback →
  hardcoded minimum, never shows "Could not load"
- `has_anonymous: true` flag for "Anonymous supporters" (no count)
- About → Credits relabel
- Tab-switch resilience: saved tab keys from older versions are
  remapped to the new sub-tab structure on launch

---

## Credits

Thanks to **Nazky**, **BestPig**, **drakmor**, **SvenGDK**,
**ps5-payload-dev**, **NookieAI** and **stonemodder** — and to
everyone who reported issues and tested fixes.

⚠️ To **play backported games** on your PS5 you need BestPig's BackPork
payload: <https://github.com/BestPig/BackPork>
