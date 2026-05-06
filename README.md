# 🎮 exFAT Image Builder

> Build PS5 exFAT and ffpkg game images — backport, patch, manage, rename dumps, convert

**by DecKerr97** · [Releases](https://github.com/kerrdec97/ps5-exfat-builder/releases) · [Issues](https://github.com/kerrdec97/ps5-exfat-builder/issues)

![Version](https://img.shields.io/badge/version-v1.9.0-4a9eff?style=flat-square)
![Platform](https://img.shields.io/badge/platform-Windows-lightgrey?style=flat-square)
![License](https://img.shields.io/badge/license-MIT-green?style=flat-square)

---

## What is it?

A Windows GUI tool for PS5 homebrew — build exFAT and ffpkg game images, auto-backport games to older firmware, rename and organise your dump collection, manage your game library, send payloads, monitor klogs, and much more. No command line required.

---

## Requirements

| Requirement | Notes |
|---|---|
| **Windows 10 / 11** (64-bit) | Required |
| **OSFMount** | Free — [download here](https://www.osforensics.com/tools/mount-disk-images.html) — required for exFAT builds |
| **.NET 8 Runtime** | Required for ffpkg builds only |
| **PS5 game dump** | Must contain `eboot.bin` in the root folder |
| **Python 3.10+** | Only needed if running from source |

> ⚠️ The app requires **Administrator** privileges. It will prompt automatically on launch.

---

## Features

### 🔨 exFAT Tab
- Queue-based workflow — add multiple games at once
- Game name, PPSA ID and version auto-detected from `param.sfo` / `param.json`
- Output always named `PPSA##### Game Title (version).exfat`
- Real-time progress — file count, GB written, MB/s, ETA
- Write speed benchmark — test output drive before building for accurate ETA
- Resume interrupted builds — detects partial images and offers to skip or rebuild
- Pre-build checklist — drive root detection, write permissions, eboot.bin, space
- Post-build verify, Force Dismount, build log auto-saved

### 📦 ffpkg Tab
- Full UFS2 ffpkg builder via UFS2Tool (bundled)
- Sector size locked at 512 — fixes Windows broken image bug
- Requires .NET 8

### 🗂️ Dump Rename Manager Tab *(New in v1.9.0)*
- Scan a folder of PS5 game dumps and auto-rename to `PPSA##### Game Title (version)` format
- Reads `param.sfo` and `param.json` — searches up to 5 folder levels deep
- **Confidence indicator** — 🟢 clean read / 🟡 name guessed from folder / 🔴 no PPSA found
- Cover art from `sce_sys/icon0.png` shown on each card
- SDK version and folder date shown per game
- Editable new name field — tweak before applying
- Move to destination folder or rename in place
- Right-click menu — Open in Explorer, Add to exFAT Queue, Delete dump
- Session memory — remembers last scanned folder

### ⚡ Backports Tab — Auto Backport
- Automatically patches PS5 ELF executables using the full decrypt/re-sign pipeline
- Powered by Backport.py by Nazky & BestPig — bundled, no install needed
- Select fakelib folder — applied to `game/fakelib/` with originals backed up
- `param.json` now included in originals backup zip
- `fakelib/` excluded from originals zip — backup contains only real pre-patch game files
- Target SDK selector (4–11) with firmware version labels
- SDK auto-detected from `param.sfo` / `param.json` — searches up to 5 levels deep
- Backup zips named `PPSA##### Game Title (version) original files.zip`
- Accessible from Library tab — click any game → ⚡ Auto Backport

### 🧩 Backports Tab — Manual
- Apply backport files to game folder or mounted exFAT image
- Drag and drop, conflict preview, auto backup

### 📚 Library Tab
- Scan progress bar — live folder count and games found
- Grid / list view with cover art
- Right-click → Add to exFAT Queue, Add to ffpkg Queue, ⚡ Auto Backport

### 💾 My Images Tab
- Scan for `.exfat` files
- 🔄 Convert to ffpkg — mounts exFAT, builds ffpkg, live progress bar
- Batch upload to PS5

### 🌐 PS5 WebUI Tab
- Quick-launch buttons for etaHEN WebUI, VoidShell WebUI, PS5 Store JSON
- Custom IP/port with path field — opens in your system browser
- IP saved between sessions

### 🎮 PS5 Manager Tab
- Unified local vs PS5 view
- PS5 storage bar with colour coding

### 📡 FTP / PS5 Browser Tabs
- Upload with live progress, cancel, auto-upload after build
- Full FTP browser — navigate, rename, move, delete, download, upload

### 🗂 File Manager Tab
- Mount `.exfat` images read-write
- Add, replace, delete files without rebuilding

### 📤 Extract Tab
- Extract `.exfat` back to a folder

### 📦 Payload Manager Tab
- Save and send `.elf` / `.bin` payloads to PS5 via TCP

### 📋 Klog Monitor Tab
- Stream PS5 kernel logs live via TCP
- Timestamps, pause/resume, keyword filter, export

### ⚙️ Advanced Tab
- exFAT: cluster size, sector size, copy threads
- Write speed benchmark for accurate ETA
- Skip verify, exclude hidden files, custom image size override
- Post-build: shutdown / restart / sleep countdown

### 🌍 17 Languages
English, Chinese, German, French, Spanish, Portuguese, Japanese, Korean, Russian, Arabic, Italian, Dutch, Polish, Turkish, Thai, Vietnamese, Indonesian

---

## Building from source

```bash
pip install pyinstaller pillow
build.bat
```

---

## Changelog

### v1.9.0
- **New: Dump Rename Manager tab** — scan, auto-rename, cover art, confidence indicator, right-click menu
- **SDK detection improved** — reads `param.sfo` and `param.json`, searches up to 5 levels deep, detailed error if not found
- **`param.json` included in originals backup zip** — both single backport and Backport All paths
- **`fakelib/` excluded from originals zip** — backup now contains only pre-patch game files
- **Naming consistency** — all outputs (exFAT, ffpkg, backport zips) always include `PPSA ID + title + version`
- **Benchmark mode** — test write speed to output directory before building (Advanced tab)
- **Resume interrupted builds** — detects partial `.exfat` images on queue start
- **Shutdown fix** — countdown no longer blocked by the build complete messagebox
- **Dump rename path fix** — mixed separator Access Denied error on Windows rename resolved
- **WebUI** — opens PS5 WebUI (etaHEN, VoidShell etc.) in system browser; IP saved between sessions
- Removed: 7-Zip / Batch ZIP Extract feature
- Removed: Stream-optimised copy order
- Various crash fixes and stability improvements

### v1.8.0
- Backport All — queue all library games for auto backport in one click
- Klog monitor improvements
- FTP browser enhancements

### v1.7.0
- Auto Backport pipeline — full ELF decrypt/re-sign via Backport.py
- Fakelib support
- SDK auto-detection

[Full release history →](https://github.com/kerrdec97/ps5-exfat-builder/releases)

---

## Troubleshooting

| Problem | Fix |
|---|---|
| OSFMount not detected | Settings → OSFMount → Browse for `osfmount.com` |
| SDK not detected | Check `sce_sys/param.json` exists and has `requiredSystemSoftwareVersion` |
| ffpkg build fails | Install .NET 8 Runtime |
| Image runs out of space | Fixed in v1.6.2+ — size margins increased |
| Output not found after build | Check antivirus isn't quarantining `.exfat` files |
| FTP won't connect | Make sure homebrew FTP is running on PS5 |
| Dump rename Access Denied | Run as Administrator (app should prompt automatically) |
| Crash on startup | Log saved to `~/exfat_builder_logs/` — share on GitHub Issues |

---

## Credits

| Contribution | Credit |
|---|---|
| PS5 Auto Backport pipeline (Backport.py, src/) | **Nazky** — [github.com/Nazky](https://github.com/Nazky) |
| PS5 Backport research & tools | **BestPig** — [github.com/BestPig](https://github.com/BestPig) |
| exFAT image creation (ShadowMountPlus) | **drakmor** — [github.com/drakmor/ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus/releases) |
| ffpkg builder (UFS2Tool) | **SvenGDK** — [github.com/SvenGDK/UFS2Tool](https://github.com/SvenGDK/UFS2Tool) |
| Klog server | **ps5-payload-dev** — [github.com/ps5-payload-dev/klogsrv](https://github.com/ps5-payload-dev/klogsrv) |
| Inspiration | **NookieAI** and **stonemodder** (Porkfolio) |
| PS5 homebrew community | Everyone contributing to the scene |

---

## License

MIT — see [LICENSE](https://github.com/kerrdec97/ps5-exfat-builder/blob/main/LICENSE)
