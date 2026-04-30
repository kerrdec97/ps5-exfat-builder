# 🎮 exFAT Image Builder

> Build PS5 exFAT and ffpkg game images — backport, patch, manage, convert

**by DecKerr97** · [Releases](https://github.com/kerrdec97/ps5-exfat-builder/releases) · [Issues](https://github.com/kerrdec97/ps5-exfat-builder/issues)

![Version](https://img.shields.io/badge/version-v1.8.0-4a9eff?style=flat-square)
![Platform](https://img.shields.io/badge/platform-Windows-lightgrey?style=flat-square)
![License](https://img.shields.io/badge/license-MIT-green?style=flat-square)

---

## Requirements

| Requirement | Notes |
|---|---|
| **Windows 10 / 11** (64-bit) | Required |
| **OSFMount** | [Download](https://www.osforensics.com/tools/mount-disk-images.html) — required for exFAT builds |
| **.NET 8 Runtime** | Required for ffpkg builds only |
| **PS5 game dump** | Must contain `eboot.bin` in the root folder |

> ⚠️ Requires **Administrator** privileges. Prompts automatically on launch.

> ⚠️ To **play backported games** on your PS5 you need BestPig's BackPork payload running: [github.com/BestPig/BackPork](https://github.com/BestPig/BackPork)

---

## Features

### 🔨 exFAT Tab
- Queue multiple games, build in sequence
- Auto-detected game name, PPSA ID, version
- Real-time progress — files copied, GB written, MB/s, ETA
- ⏸ Pause / ▶ Resume queue
- Per-game elapsed time in status and log
- Pre-build checklist — blocks output=source folder, checks space
- Force Dismount, post-build verification

### 📦 ffpkg Tab
- UFS2 ffpkg builder via UFS2Tool (bundled)
- Sector size locked at 512 — fixes Windows broken image bug
- Requires .NET 8

### ⚡ Auto Backport Tab
- Full decrypt/re-sign pipeline via Backport.py by Nazky & BestPig
- SDK auto-detected from param.sfo on game folder browse
- Yellow banner shows detected SDK and recommended target
- Fakelib support — applied to `game/fakelib/` subfolder
- Originals snapshot before fakelib applied
- Patched files applied back to game folder in correct paths
- Zip backups: `PPSA12345 Game Title (01.000.000) original files.zip`
- PPSA ID read from param.sfo when not in folder name
- ↩ Restore originals from zip
- Compatibility notes field
- `decrypted/` excluded from all operations

### 📚 Library Tab
- Grid / list view with cover art
- Right-click → Add to exFAT Queue, Add to ffpkg Queue, ⚡ Auto Backport
- ⚡ Backport All — batch backport all library games in sequence

### 💾 My Images Tab
- Browse and manage `.exfat` image files
- Upload to PS5 via FTP

### 📡 FTP Tab (merged with PS5 Browser)
- ↑ Upload sub-tab with live progress bar
- 📂 PS5 Browser sub-tab — navigate, upload, download, rename, delete
- Any "Upload to PS5" button auto-switches to this tab

### 🧪 Language Stripper (Advanced tab) — BETA
> ⚠️ **Beta feature — not fully tested. May not work on all games. Backs up removed files as zips before deleting.**

- Scans game folders for language-specific files and folders
- Finds Unreal Engine language PAK files (`pakchunk_lang_en.pak`)
- Finds PSARC language audio files (`audio_en.psarc`)
- Finds loose locale folders (`en-US/`, `de-DE/` etc)
- Single game or scan all games in a folder at once
- Results grouped by game with size per entry
- Quick actions: Keep All, Keep None, Keep en-US Only
- Removes and backs up to zip before deleting

### ⚙ Advanced Tab
- exFAT: cluster size, sector size, copy threads
- ffpkg: block size, fragment size, inode count
- Post-Build: delete source folder after successful build
- Language Stripper (beta)

### ⏻ Shutdown after task (Settings tab)
- Shut down, Restart or Sleep PC after task completes
- Configurable delay (30s–600s) with cancellable countdown dialog
- Per-trigger: exFAT queue, ffpkg queue, backport, FTP upload

### General
- 14-tab interface, all fitting on 1366px+ screens
- 17 languages
- Global output log visible on all tabs
- Auto-update with retry logic and working directory fix
- Crash reporter

---

## Building from source

```bash
pip install pyinstaller pillow
build.bat
```

---

## Credits

| Contribution | Credit |
|---|---|
| PS5 Auto Backport pipeline (Backport.py, src/) | **Nazky** — [github.com/Nazky](https://github.com/Nazky) |
| PS5 backport research & backport enabler payload | **BestPig** — [github.com/BestPig/BackPork](https://github.com/BestPig/BackPork) |
| exFAT image creation (ShadowMountPlus) | **drakmor** — [github.com/drakmor/ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus/releases) |
| ffpkg builder (UFS2Tool) | **SvenGDK** — [github.com/SvenGDK/UFS2Tool](https://github.com/SvenGDK/UFS2Tool) |
| Klog server | **ps5-payload-dev** — [github.com/ps5-payload-dev/klogsrv](https://github.com/ps5-payload-dev/klogsrv) |
| Inspiration | **NookieAI** and **stonemodder** (Porkfolio) |

---

## License

MIT — see [LICENSE](LICENSE)
