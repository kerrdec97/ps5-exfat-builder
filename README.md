# 🎮 exFAT Image Builder

> Build PS5 exFAT and ffpkg game images — scripts bundled, game name auto-detected

**by DecKerr97** · [Releases](https://github.com/kerrdec97/ps5-exfat-builder/releases) · [Issues](https://github.com/kerrdec97/ps5-exfat-builder/issues)

![Version](https://img.shields.io/badge/version-v1.6.0-4a9eff?style=flat-square)
![Platform](https://img.shields.io/badge/platform-Windows-lightgrey?style=flat-square)
![License](https://img.shields.io/badge/license-MIT-green?style=flat-square)

---

## What is it?

A Windows GUI tool that converts PS5 game folders into `.exfat` or `.ffpkg` disk images for use with PS5 homebrew on jailbroken consoles. It handles the entire process automatically — no command line required.

---

## Requirements

| Requirement | Notes |
|---|---|
| **Windows 10 / 11** (64-bit) | Required |
| **OSFMount** | Free — [download here](https://www.osforensics.com/tools/mount-disk-images.html) — required for exFAT builds |
| **.NET 8 Runtime** | Required for ffpkg builds — use the Check .NET 8 button in the ffpkg tab |
| **PS5 game dump** | Must contain `eboot.bin` in the root folder |

> ⚠️ The app requires **Administrator** privileges. It will prompt automatically on launch.

---

## Installation

1. Download `exFAT Image Builder.exe` from the [Releases page](https://github.com/kerrdec97/ps5-exfat-builder/releases)
2. Install [OSFMount](https://www.osforensics.com/tools/mount-disk-images.html) for exFAT builds
3. Run the exe — no install needed, just double-click
4. If OSFMount is not detected, go to **⚙ Settings → OSFMount → Browse**

---

## Features

### 🔨 exFAT Tab
- Queue-based workflow — add multiple games and build them all in sequence
- Game name, PPSA ID and version auto-detected from `param.sfo` / `pfs-version.dat`
- Real-time progress — file count, GB written, MB/s rolling average, ETA
- Estimated total build time shown before starting
- Pre-build checklist — OSFMount, eboot.bin, drive space, write permissions, drive root detection
- Image verified after build
- Build log auto-saved to dedicated logs folder
- Queue save / load as `.json`
- Add multiple game folders to queue at once
- Force Dismount button — uses Windows shell eject method
- Auto-retry failed builds

### 📦 ffpkg Tab (NEW)
- Full UFS2 `.ffpkg` image builder using UFS2Tool
- UFS2Tool bundled inside the exe — no separate download needed
- Sector size locked at 512 bytes — fixes Windows broken image bug
- Requires .NET 8 Runtime — Check .NET 8 button with download link
- Full queue system matching the exFAT tab
- Two-stage progress bar — Write Structure → Copy Files → Finalise
- File count and ETA shown live
- Auto-upload to PS5 after build

### ⚙️ Advanced Tab (NEW)
- **exFAT parameters** — Cluster size, Sector size, Concurrent copy threads (default 1 — avoids fragmentation)
- **ffpkg parameters** — Block size, Fragment size, Min free %, Bytes per inode, Sector size (locked 512)
- Setup recommendations panel — thread count guidance by drive type
- Active parameters summary box
- Save and Reset to Defaults buttons

### 🌐 Multi-Language Support (NEW — 17 languages)
English, Chinese, German, French, Spanish, Portuguese, Japanese, Korean, Russian, Arabic, Italian, Dutch, Polish, Turkish, Thai, Vietnamese, Indonesian — switch instantly in Settings, no restart needed

### 📚 Library Tab
- Grid / list view with cover art
- Scan progress bar — shows % and games found live
- Right-click → Add to exFAT Queue or Add to ffpkg Queue
- Build All — adds every game to queue at once

### 💾 My Images Tab
- Scans folders for `.exfat` files
- Multi-select batch upload to PS5

### 🎮 PS5 Manager Tab
- Unified view of local vs PS5 images
- PS5 storage bar with colour coding
- Version mismatch warning

### 📡 FTP Upload / PS5 Browser Tabs
- Upload with live progress, cancel, auto-upload after build
- Full FTP browser — navigate, rename, move, delete, download, upload

### 🗂 File Manager Tab
- Mount `.exfat` images read-write
- Add, replace, delete files without rebuilding

### 📤 Extract Tab
- Extract `.exfat` back to a folder

### 🧩 Backports Tab
- Apply backport patches to game folders or mounted exFAT images
- Drag and drop, conflict preview, auto backup

### 📦 Payload Manager Tab
- Save and send `.elf` / `.bin` payloads to PS5 via TCP

### 📋 Klog Monitor Tab
- Stream PS5 kernel logs live, filter, pause/resume, export

### ⚙ Settings Tab
- OSFMount path, temp folder, logs folder, PS5 FTP, theme, language

### ❓ Help Tab
- Built-in FAQ, live changelog from GitHub, tutorial guide button

### General
- Auto-update — downloads and installs new versions automatically
- Crash reporter — catches errors, saves log, copies to clipboard
- DPI awareness — no clipping on high-DPI laptops

---

## Building from source

```bash
pip install pyinstaller pillow
build.bat
```

---

## Troubleshooting

| Problem | Fix |
|---|---|
| OSFMount not detected | Settings → OSFMount → Browse for `osfmount.com` |
| ffpkg build fails | Install .NET 8 Runtime — use the Check .NET 8 button |
| False "Build failed" dialog | Fixed in v1.5.1+ — update |
| App freezes on USB drive browse | Fixed in v1.5.0+ — update |
| Output not found after build | Check antivirus isn't quarantining the output file. Don't use drive root (C:\) as output |
| FTP won't connect | Make sure homebrew FTP server is running on PS5 |
| Crash on startup | Log saved to `~/exfat_builder_logs/` — share on GitHub Issues |

---

## Credits

- Inspired by **NookieAI** and **stonemodder** (Porkfolio)
- ffpkg support via **UFS2Tool** by SvenGDK — [github.com/SvenGDK/UFS2Tool](https://github.com/SvenGDK/UFS2Tool)
- exFAT image creation via **ShadowMountPlus** by drakmor — [github.com/drakmor/ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus/releases)
- PS5 homebrew community

---

## License

MIT — see [LICENSE](LICENSE)
