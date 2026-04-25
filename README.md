# 🎮 exFAT Image Builder

> Build PS5 exFAT game images — scripts bundled, game name auto-detected

**by DecKerr97** · [Releases](https://github.com/kerrdec97/ps5-exfat-builder/releases) · [Issues](https://github.com/kerrdec97/ps5-exfat-builder/issues)

![Version](https://img.shields.io/badge/version-v1.5.0-4a9eff?style=flat-square)
![Platform](https://img.shields.io/badge/platform-Windows-lightgrey?style=flat-square)
![License](https://img.shields.io/badge/license-MIT-green?style=flat-square)

---

## What is it?

A Windows GUI tool that converts PS5 game folders into `.exfat` disk images for use with PS5 homebrew on jailbroken consoles. It handles the entire process — sizing, mounting, formatting and copying — automatically, with no command line required.

---

## Requirements

| Requirement | Notes |
|---|---|
| **Windows 10 / 11** (64-bit) | Required |
| **OSFMount** | Free — [download here](https://www.osforensics.com/tools/mount-disk-images.html) |
| **PS5 game dump** | Must contain `eboot.bin` in the root folder |

> ⚠️ The app requires **Administrator** privileges. It will prompt automatically on launch.

---

## Installation

1. Download `exFAT Image Builder.exe` from the [Releases page](https://github.com/kerrdec97/ps5-exfat-builder/releases)
2. Install [OSFMount](https://www.osforensics.com/tools/mount-disk-images.html)
3. Run the exe — no install needed, just double-click
4. If OSFMount is not detected automatically, go to **⚙ Settings → OSFMount → Browse** to locate `osfmount.com`

---

## Features

### 🔨 Build Tab
- Queue-based workflow — add multiple games and build them all in sequence
- Game name, PPSA ID and version auto-detected from `param.sfo` / `pfs-version.dat`
- Real-time progress — file count (`342 / 1847 files`), GB written, MB/s speed, ETA
- Estimated total build time shown before starting
- Pre-build checklist — checks OSFMount, `eboot.bin`, drive space
- Image verified after build — confirms `eboot.bin` and file count match source
- Corrupt image detection — flags suspiciously small output files
- Build log auto-saved to dedicated logs folder
- Queue save / load as `.json`
- Recently used folders dropdown (last 10)
- Auto-retry failed builds (Off / 1x / 2x / 3x / 5x)

### 📚 Library Tab
- Scans multiple folders for PS5 game dumps
- Grid view with cover art or compact list view
- Search / filter by game name or PPSA ID
- Right-click menu — add to queue, view details, open folder
- **Build All** — adds every scanned game to the queue at once

### 💾 My Images Tab
- Scans folders for `.exfat` files
- Multi-select with Ctrl+Click for batch operations
- Batch upload selected images to PS5

### 🎮 PS5 Manager Tab
- Unified view of local images vs what is on your PS5
- 🟢 In sync · 🟡 Not uploaded · ⚫ PS5 only
- Version mismatch warning when local and PS5 versions differ
- Upload button per row
- PS5 storage bar — used / free / total with colour coding

### 📡 FTP Upload Tab
- Upload any `.exfat` file or folder to PS5
- Live progress — GB sent, MB/s, ETA
- Cancel upload at any time
- Auto-upload after build option

### 📡 PS5 Browser Tab
- Full FTP file browser
- Navigate, rename, cut/paste, move to any path, delete recursively, download, upload
- Keyboard shortcuts — Delete, F2, F5, Backspace

### 🗂 File Manager Tab
- Mount `.exfat` images read-write via OSFMount
- Add files, add folders, replace, delete, new folder
- Dismount with retry — handles stubborn unmounts on some systems

### 📤 Extract Tab
- Extract `.exfat` back to a folder with live progress

### 🧩 Backports Tab
- Apply backport patches to game folders or mounted exFAT images
- Full folder structure preserved (fakelib, sce_module, sce_sys etc)
- Drag and drop backport files and folders
- Conflict preview — shows exactly what will be overwritten before applying
- Auto backup of originals — named `GameName - Backport Backup (date)`
- File list auto-clears when a new game target is selected

### 📦 Payload Manager Tab
- Save `.elf` and `.bin` payloads permanently
- Send to PS5 via TCP with live progress and MB/s speed
- Auto-name from filename when browsing
- Colour-coded ELF / BIN badges

### 📋 Klog Monitor Tab
- Connect to PS5 and stream kernel logs live via TCP (default port 3232)
- Colour-coded output — errors, warnings, debug, info
- Timestamps on every line
- Pause / Resume — freezes display while still receiving in background
- Real-time keyword filter with highlighting
- Export log to `.txt`

### ⚙ Settings Tab
- OSFMount path — Browse or Auto-detect
- Temp folder — usage display, change location, clear
- Logs folder — dedicated location, Open Logs button, clear all
- PS5 FTP — IP, port, auto-detect, test connection, ping
- Auto-upload after build
- Sound notifications
- Auto-retry count
- Dark / Light mode

### ❓ Help Tab
- Built-in FAQ
- Live changelog fetched from GitHub

### General
- Auto-update — downloads and installs new versions automatically
- Crash reporter — catches unhandled errors, saves log, offers clipboard copy
- DPI awareness — no clipping on high-DPI or dual-GPU laptops
- Window size and position remembered between launches
- End-of-tab indicator on every scrollable tab

---

## Lite Version

`exFAT Image Builder Lite.exe` contains only the **Build** and **Library** tabs — ideal for users who just want to build images quickly without the extra features.

---

## Building from source

```bash
pip install pyinstaller pillow
build.bat
```

Produces both exes in `dist\`.

---

## Troubleshooting

| Problem | Fix |
|---|---|
| OSFMount not detected | Settings → OSFMount → Browse for `osfmount.com` |
| App freezes when browsing USB drive | Fixed in v1.5.0 — update |
| Image not dismounting after build | Fixed in v1.5.0 — dismount now retries automatically |
| eboot.bin not found | Must be in the root of the game folder |
| Output drive low on space | Output drive needs free space equal to the game size |
| FTP will not connect | Make sure homebrew FTP server is running on PS5 |
| Crash on startup | Log saved to `~/exfat_builder_logs/` — share on GitHub Issues |
| Game name not detected | Game may be missing `param.sfo` — falls back to folder name |

---

## Credits

- Inspired by **NookieAI** and **stonemodder** (Porkfolio)
- PS5 homebrew community

---

## License

MIT — see [LICENSE](LICENSE)
