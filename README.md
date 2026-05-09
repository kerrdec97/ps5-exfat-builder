# 🎮 exFAT Image Builder

> Build PS5 exFAT and ffpkg game images — backport, patch, manage, convert, rename

**by DecKerr97** · [Releases](https://github.com/kerrdec97/ps5-exfat-builder/releases) · [Issues](https://github.com/kerrdec97/ps5-exfat-builder/issues)

![Version](https://img.shields.io/badge/version-v2.5.0-b07ad6?style=flat-square)
![Platform](https://img.shields.io/badge/platform-Windows-lightgrey?style=flat-square)
![License](https://img.shields.io/badge/license-MIT-green?style=flat-square)

---

## What is it?

A Windows GUI tool for PS5 homebrew — build exFAT and ffpkg game images, auto-backport games to older firmware, rename and organize your dumps, manage your library, send payloads, monitor klogs, and more. No command line required.

**v2.5 is a major UI overhaul** — every tab redesigned, a new purple-tinted dark theme, a dedicated three-pane Dump Rename inspector, and dozens of bug fixes covering scanning, renaming, cover art rendering, and progress reporting.

---

## Requirements

| Requirement | Notes |
| --- | --- |
| **Windows 10 / 11** (64-bit) | Required |
| **OSFMount** | Free — [download here](https://www.osforensics.com/tools/mount-disk-images.html) — required for exFAT builds |
| **.NET 8 Runtime** | Required for ffpkg builds only |
| **PS5 game dump** | Must contain `eboot.bin` somewhere in the folder |

> ⚠️ The app requires **Administrator** privileges. It will prompt automatically on launch.

---

## What's new in v2.5

### 🎨 Full UI rebuild

Every one of the 18 tabs has been redesigned from scratch against pixel-accurate HTML mocks. The chrome is consistent, fonts are readable, scrolling no longer jitters, and everything respects a single design system (`tkinter_theme.py` with a shared `COLORS` and `FONTS` palette).

### 💜 Purple theme

The whole app moved off the original electric-blue palette to a purple-tinted dark theme. Dark fields, light text, purple accents, klog phosphor green preserved.

### 🪪 Dump Rename — three-pane inspector

The old single-list rename screen is gone. The new layout:

- **Left rail** — scrollable Treeview grouped by Ready / Needs review / Duplicate / Failed, with size and confidence chips
- **Centre pane** — game inspector showing FROM / TO names, detected SFO metadata (PPSA, title, version, SDK), full path, and three name candidates you can click to apply
- **Right rail** — naming preset radios (PPSA only, PPSA + title, PPSA + title + version), case (upper/lower/keep), special-character options, move destination field, bulk action buttons

### 🐛 Major bug fixes

- **Cover art** — covers now render at consistent 180×180 squares; padded logo icons (Park Beyond, Tevi) auto-trim their borders; full-bleed art (Last of Us, House of the Dead) renders edge-to-edge without distortion
- **Library dedupe** — multiple folders sharing a PPSA (real game + backport residue) now collapse to one entry, keeping the largest copy
- **Library scan** — folders without `eboot.bin` no longer register as games; subfolders inside a top-level scan target only count if they have eboot.bin within 3 levels
- **Dump Rename failures** — Windows case-only renames work properly; folder access conflicts (Explorer locks, AV scans) get retried with backoff; container folders like `Keep` are recognised and recursed into rather than renamed
- **Dump Rename progress** — determinate progress bar with byte-level ETA for cross-folder moves; status text shows current item, MB/s, time remaining
- **Field visibility** — every text input is dark-on-light → now dark-on-dark across all 18 tabs; readonly fields display content properly
- **Build progress** — exFAT and ffpkg output filenames render visibly; bottom-pinned action buttons no longer cut off
- **Backports** — sub-bar with Auto / Results, filter pills (FW 4 / FW 5-7 / FW 9+), 380px game rail with cover thumbnails

---

## Features

### 🔨 exFAT Tab
- Queue-based workflow — add multiple games at once or one at a time
- Game name, PPSA ID and version auto-detected
- Real-time progress — file count, GB written, MB/s, ETA
- Estimated build time before starting
- Pre-build checklist — drive root detection, write permissions, eboot.bin, space
- Force Dismount button (Windows shell eject)
- Build log auto-saved, queue save/load

### 📦 ffpkg Tab
- Full UFS2 ffpkg builder via UFS2Tool (bundled)
- Sector size locked at 512 — fixes Windows broken image bug
- Requires .NET 8

### 🧩 Backports Tab — Manual
- Apply backport files to game folder or mounted exFAT image
- Drag and drop, conflict preview, auto backup named after game + date
- File list auto-clears on new target

### ⚡ Backports Tab — Auto
- Automatically patches PS5 ELF executables using the full decrypt/re-sign pipeline
- Powered by Backport.py by Nazky & BestPig — bundled, no install needed
- Fakelib path saved automatically for future use
- Patched files applied back to game folder in correct paths
- Backup zips — originals and backported files saved as separate `.zip` files
- Filter pills: All / FW 4 / FW 5-7 / FW 9+
- Accessible from Library tab — right-click any game → ⚡ Auto Backport

### 📚 Library Tab
- Scan progress bar — live folder count and games found
- Grid / list view with cover art (covers normalized to 180px square)
- Auto-dedupe by PPSA (keeps largest copy)
- SDK threshold colour-coding — backport-needed games highlighted
- Right-click → Add to exFAT Queue, Add to ffpkg Queue, ⚡ Auto Backport

### 💾 Images Tab
- Scan for `.exfat` files
- 🔄 Convert to ffpkg — mounts exFAT, builds ffpkg, live progress bar
- Batch upload to PS5

### 🎮 PS5 Manager Tab
- Unified local vs PS5 view
- PS5 storage bar with colour coding
- Game thumbnails

### 📡 FTP Upload / PS5 Browser Tab
- Dual-pane local ↔ PS5 view
- Upload with live progress, **Cancel button**, auto-upload after build
- Full FTP browser — navigate, rename, move, delete, download, upload

### 🪪 Dump Rename Tab
- Three-pane inspector (list rail · detail · controls)
- Ready / Review / Duplicate / Failed grouping
- Naming presets, case, special-character options
- In-place rename or cross-folder move with byte-level ETA
- Container folder detection — `Keep`, `Backups`, etc. recursed into rather than renamed
- Windows case-only rename support (lowercase → UPPERCASE works on NTFS)
- Retry with backoff for transient access denied / file in use errors

### 🗂 Files Tab
- Mount `.exfat` images read-write
- Add, replace, delete files without rebuilding

### 📤 Extract Tab
- Extract `.exfat` back to a folder

### 📦 Payload Manager Tab
- Save and send `.elf` / `.bin` payloads to PS5 via TCP

### 📋 Klog Monitor Tab
- Stream PS5 kernel logs live via TCP
- Phosphor green CRT styling
- Timestamps, pause/resume, keyword filter, export

### 📜 History Tab
- Past builds and operations
- Quick re-run, re-export, file open

### ⚙ Advanced Tab
- exFAT: cluster size, sector size, copy threads
- ffpkg: block size, fragment size, min free %, bytes per inode

### ⚙ Settings Tab
- OSFMount custom path, temp folder, logs folder
- PS5 FTP — IP, port, auto-detect
- Library scan threshold (>= SDK 18 warn, >= SDK 16 backport)
- Language, theme, notifications

### 🌐 17 Languages
English, Chinese, German, French, Spanish, Portuguese, Japanese, Korean, Russian, Arabic, Italian, Dutch, Polish, Turkish, Thai, Vietnamese, Indonesian

### General
- Auto-update with retry logic and working directory fix
- Crash reporter — saves log, copies to clipboard
- End-of-tab indicator on every scrollable tab
- Single design system across all tabs

---

## Building from source

```
pip install pyinstaller pillow tkinterdnd2
build.bat
```

Build with `--clean --noconfirm` flags is included. The build will fail if a previous `dist\exFAT Image Builder.exe` is still running or held by antivirus — close the app and pause AV scanning if needed.

Python 3.11.x is the tested version.

---

## Troubleshooting

| Problem | Fix |
| --- | --- |
| OSFMount not detected | Settings → OSFMount → Browse for `osfmount.com` |
| Auto-update crashes on restart | Fixed in v1.6.3+ — update manually this time |
| ffpkg build fails | Install .NET 8 Runtime |
| FTP won't connect | Make sure homebrew FTP is running on PS5 |
| Crash on startup | Log at `~/exfat_builder_logs/` — share on GitHub Issues |
| Build "Access denied" on .exe | Close any running instance + pause AV; rebuild |
| Dump Rename "destination already exists" on case rename | Fixed in v2.5.0+ |
| Dump Rename freezes on Apply | Fixed in v2.5.0+ — pre-scan moved to worker thread |
| `Keep` folder got renamed to a game | Fixed in v2.5.0+ — manually rename it back, then re-scan |
| Cover art looks wrong sizes | Fixed in v2.5.0+ — re-scan Library after updating |
| White text fields hard to read | Fixed in v2.5.0+ — fields now dark-on-light text |

---

## Credits

| Contribution | Credit |
| --- | --- |
| PS5 Auto Backport pipeline (Backport.py, src/) | **Nazky** — [github.com/Nazky](https://github.com/Nazky) |
| PS5 Backport research & tools | **BestPig** — [github.com/BestPig](https://github.com/BestPig) |
| exFAT image creation (ShadowMountPlus) | **drakmor** — [github.com/drakmor/ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus/releases) |
| ffpkg builder (UFS2Tool) | **SvenGDK** — [github.com/SvenGDK/UFS2Tool](https://github.com/SvenGDK/UFS2Tool) |
| Klog server | **ps5-payload-dev** — [github.com/ps5-payload-dev/klogsrv](https://github.com/ps5-payload-dev/klogsrv) |
| Inspiration | **NookieAI** and **stonemodder** (Porkfolio) |
| PS5 homebrew community | Everyone contributing to the scene |

---

## License

MIT — see [LICENSE](LICENSE)
