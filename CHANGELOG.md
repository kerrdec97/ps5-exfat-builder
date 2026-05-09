# Changelog

## v2.5.0 — Major UI overhaul + stability release

### 🎨 Full UI rebuild
- All 18 tabs redesigned against pixel-accurate mocks
- New purple-tinted dark theme replacing electric blue
- Single design system in `tkinter_theme.py` shared across every tab
- Inter / JetBrains Mono fonts where available, with system fallbacks
- Dark text fields throughout (was hard-to-read white-on-dark in many places)
- Scrollable canvas on every long tab — no more cut-off bottom buttons

### 🪪 Dump Rename — three-pane inspector
- Left rail: Treeview grouped by Ready / Needs review / Duplicate / Failed, with size + confidence chips
- Centre: per-game inspector with SFO metadata (PPSA, title, version, SDK), full path, three name candidates
- Right rail: naming presets (PPSA / + title / + version), case (upper/lower/keep), sanitize options, move destination, bulk action buttons
- Smart container detection — folders without PPSA/version patterns (`Keep`, `Backups`, `MyGames`) are recursed into rather than renamed
- Determinate progress bar with byte-level ETA on cross-folder moves: `[3/8] PPSA03352 The Callisto Protocol · 47.2 GB / 285.6 GB · 412.3 MB/s · ETA 9m 43s`
- UI stays responsive — pre-scan moved to a worker thread
- Windows case-only renames work properly (`ppsa12345 game` → `PPSA12345 GAME`)
- Two-step temp rename fallback for stubborn filesystems
- Retry with backoff on transient locks (Windows Explorer, antivirus, search indexer)
- Parent/child path dedupe — selecting both a container and a child no longer breaks the child's rename
- Cross-folder "Move to" actually MOVES (copy + remove source) instead of just copying
- Error dialog at end with up to 6 errors + pointer to OUTPUT LOG

### 🎨 Cover art
- Frames are strict 180×180 squares (previously stretched horizontally, leaving covers letterboxed in tall rectangles)
- Smart trim with 25% safety cap — catches padded logos (Park Beyond, Tevi) without eating real artwork (Astro Bot's blue, The Last of Us's dark)
- Library auto-dedupe by PPSA — backport residue and partial decrypts no longer show as duplicate cards
- Shared `_load_cover_image` helper used by Library grid + list, PS5 Mgr, Backports cards, Backports auto banner, Images tab

### 💜 Backports overhaul
- Sub-bar with Auto / Results, badge strip
- 380px game rail with cover thumbnails
- Filter pills converted to segmented control: All / FW 4 / FW 5-7 / FW 9+
- "Backport All Games" purple primary button
- Auto-panel selected-game banner with cover art

### 🐛 Library scan tightening
- Subfolders without `eboot.bin` no longer register as games
- Require eboot.bin within 3 levels (not 4) for cleaner top-level scans
- SDK threshold raised — warn at >=18, backport at >=16 (matching current HEN coverage)
- Removed stale PPSA-name-only fallback that flagged junk folders

### 🐛 Other bug fixes
- exFAT and ffpkg output filenames render visibly against dark backgrounds
- FTP got a Cancel button
- Output log no longer auto-pops on every log line
- PS5 Mgr cover lookup hyphen-normalises both sides (PPSA-12345 vs PPSA12345)
- Settings + Files readonly entries display content properly
- Language selection persists when dialog closes
- Naming preset radios + case + sanitize options for Dump Rename
- Tree row text uses `orig_name` (stable), not live proposed name
- exFAT bottom action buttons no longer cut off
- ffpkg same — extracted action buttons pinned to bottom

---

## v2.0.1 — Initial v2.x baseline

- Deduplicated `lowercase_names` checkbox
- `_get_threads_arg` defaults to '8' when missing
- Removed dead `skip_verify` codepath and PowerShell `[switch]$SkipVerify`
- PS1 `$out = & $osf -a` corrected
- All `subprocess.Popen('explorer "..."')` switched to `os.startfile()`
- PyInstaller .bat: dropped `--uac-admin`, added `--clean --noconfirm`, errorlevel checks, `py -3.11` pin
- 17 i18n languages baseline
