# 🚀 v2.7.0 — ShadowMountPlus integration & SDK detection overhaul

Released 2026-05-13.

**Repo:** https://github.com/kerrdec97/ps5-exfat-builder

## ✨ Highlights

- ⚡ New **ShadowMount+** sub-tab under PS5 — full `config.ini` editor, payload sender, and "Safe to unplug" workflow.
- 🔧 PS5 SDK detection rewritten to correctly read BCD-encoded `sdkVersion` / `requiredSystemSoftwareVersion` fields. Outer Worlds 2 (SDK 10, FW 12.7), Among Us (SDK 2), RESIDENT EVIL (SDK 17), and similar games now detect correctly instead of silently failing.
- 💬 Friendlier error messages across all PS5-facing actions — common cases like "FTP not running" are now self-explanatory popups instead of raw `WinError 10061`.

## ➕ Added

### ⚡ ShadowMount+ sub-tab (PS5 → ⚡ ShadowMount+)

Full GUI editor for every key documented in the ShadowMountPlus `config.ini`:

- ⏱ **Kstuff pause behaviour** — image and direct launch delays, auto-toggle, crash detection, per-title no-pause and delay overrides
- 💽 **Mount options** — read-only, force-mount, exFAT/UFS backend selection
- 🔍 **Scanning** — scan depth, interval, stability wait, custom scan paths
- 🧩 **Fakelib overlays** — sandbox watcher, global path, priority, per-title excludes
- 📌 **Per-image overrides** — read-only/read-write/sector size by filename
- ⚙ **Advanced** — debug logging, quiet mode, all five sector-size defaults

Plus the action toolbar:

- ⬇ **Load from PS5** — fetches the current config via FTP and populates the form
- ⬆ **Push to PS5** — validates ranges (0–3600 for delays, `TITLEID:SECONDS` syntax for per-title rules) then writes `/data/shadowmount/config.ini` with section comments and clean grouping
- 💾 **Save locally** — persists the form into the app's settings file so it round-trips across launches
- ↺ **Reset to defaults** — restores documented defaults without touching the PS5
- ⏏ **Safe to unplug** — drops the `/data/shadowmount/STOP` sentinel via FTP so ShadowMountPlus releases all mounts cleanly, letting you unplug a USB / nvme enclosure without yanking active mount points. ShadowMountPlus auto-clears the stale STOP flag at next payload start.
- 🚀 **Send Payload (ELF)** — streams `shadowmountplus.elf` to TCP 9021 with live progress in the status line. Path is remembered per-user in settings.
- 📝 **Fetch debug.log** — retrieves `/data/shadowmount/debug.log` and opens it in a scrollable viewer with "Save as..." option.

### 🔧 SDK detection

- 🧠 New `ExFATBuilder._parse_ps5_version_field(raw)` — central BCD-aware parser used by all three SDK readers (`_read_sfo_info`, `_read_sdk_from_folder`, `_abp_read_required_fw`). Handles the printed-digit-pairs-as-hex encoding PS5 actually uses (`0x1000000000000000` = SDK 10, `0x1270000000000000` = FW 12.7) and falls back to numeric hex if a–f characters appear in the field.
- 🖥 FW display in the Auto Backport status row now formats correctly:
  - `0x1270…` → `FW 12.7`
  - `0x1207…` → `FW 12.07`
  - `0x1200…` → `FW 12.0`
- 🥇 SDK priority order: `sdkVersion` first, then `requiredSystemSoftwareVersion` as fallback.
- 🎯 Auto-suggested target SDK is now clamped to `[1, 10]` (the SDKVersionPatcher supported range) with a floor of 4 when detected SDK ≥ 5, so high-SDK games can't silently push the dropdown into an unsupported value.

### 💬 Error handling

All ShadowMount+ FTP/TCP errors now translate common failure modes into actionable messages:

- 🚫 `WinError 10061` / refused → "PS5 FTP server not running" with instructions for etaHEN / itemzflow / payload-loader startup
- ⏰ `WinError 10060` / timed out → "Check PS5 is awake and IP is right"
- 📡 `WinError 10065` / unreachable → "No network route, check IP and same-network"
- 📄 `550` on Load → "No config on PS5 yet, push to create it" (legitimate first-run state)
- 📄 `550` on Fetch debug.log → "No log yet, enable debug logging and push"
- ⚡ Send Payload `10061` → "Payload listener (etaHEN / GoldHEN / PLK) must be running on port 9021"

## 🔄 Changed

- ✅ **Auto Backport:** target-SDK dropdown is validated before launching the pipeline. Out-of-range values trigger a clear popup and clamp instead of the cryptic mid-pipeline `Pair number must be between 1 and 10` error that left the game folder in a half-patched state.
- 📦 **Bulk "Backport All" runner:** same SDK validation pre-flight.
- 🏷 **Backports tab badges:** SDK badge now appears for any detected SDK ≥ 1 (was ≥ 5); SDK 16+ rendered with a warning amber colour to flag games beyond standard backport range.
- 📐 **Status label** in ShadowMount+ action bar now wraps long error text inside its container instead of bleeding off-screen to the right.

## 🐛 Fixed

- 🔢 **BCD parser fallback was wrong for short hex inputs.** `_parse_ps5_version_field('0xFF')` returned `(0, 0)` instead of `(255, 0)` because the fallback hardcoded a 64-bit shift. Now uses length-aware shift (`nbits - 8`), so 32-bit `param.sfo` SYSTEM_VER fields parse correctly even when they fall through to the numeric-hex path.
- 🪲 `App._read_sdk_from_folder` calls (8 occurrences) corrected to `ExFATBuilder._read_sdk_from_folder`. The wrong class name was raising `NameError` inside `try/except` blocks that swallowed silently, leaving SDK detection apparently broken with no diagnostic.
- 🔎 Deep-fallback SDK probe in `_lib_scan` / `_bp_scan_games`: replaced silent `except Exception: pass` blocks with `self._log('[SCAN] Deep SDK probe failed: %r')` so future failures of this path are visible in OUTPUT LOG instead of producing badge-less cards with no explanation.
- 🎨 **ShadowMount+ form:** pause-delay row labels were overflowing into the entry widgets (32-char label column too narrow for "Pause delay for image-backed launches"). Bumped to 42 chars; everything aligns cleanly.
- 🎨 **ShadowMount+ form:** "Title IDs that should NOT pause kstuff" textarea was shifted right by a hardcoded 258px hack trying to align under a separate label. Replaced with the standard label-above-textarea layout used in Scanning and Fakelib sections.

## ⚠ Known issues

- 🔌 The status-bar "PS5 Online" indicator reflects network reachability, not FTP availability. If FTP isn't running on the PS5, actions will fail at connect time — the friendly error popup now explains this.
- 🛑 Per-game unmount isn't supported; "Safe to unplug" stops the entire ShadowMountPlus payload because the upstream ELF doesn't expose a per-game release API. Resume by re-sending the payload.
- 📦 ELF payload (`shadowmountplus.elf`) is not bundled — pick the path once via the Browse button and it's remembered in settings.

## 🙏 Credits

- **drakmor** — [ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus)
- **@NazkyYT** & **@bestpig** — Backport.py pipeline
- **PS5 R&D community** — for figuring all this stuff out
