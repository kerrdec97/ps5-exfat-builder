# exFAT Image Builder

**A complete Windows toolkit for preparing, converting, and deploying PS5 game images for homebrew use.**

exFAT Image Builder turns a raw PS5 game dump into a mountable image — `.exfat`, `.ffpkg`, or compressed `.ffpfsc` PFS — and handles everything around that: batch builds, format conversion, extraction, a scannable game library, dump renaming, backporting, and direct communication with a jailbroken PS5 over FTP (payload deployment, kernel-log streaming, ShadowMount+ / MicroMount config, Y2JB, and game management).

It's a single self-contained desktop application: one `.exe`, no installer, with the heavy lifting tools bundled inside.

---

## Badges

![Platform](https://img.shields.io/badge/platform-Windows-blue)
![Version](https://img.shields.io/badge/version-3.6.x-brightgreen)
![Python](https://img.shields.io/badge/python-3.11-blue)
![UI](https://img.shields.io/badge/UI-Tkinter-orange)
![License](https://img.shields.io/badge/license-See%20repository-lightgrey)
![Status](https://img.shields.io/badge/PFS%20support-active-success)

---

## Overview

PS5 homebrew tools like **ShadowMount+** and **MicroMount** mount game images directly from external storage, but getting a raw dump into a clean, correctly-named, mountable image is fiddly and error-prone. exFAT Image Builder consolidates that whole pipeline into one place:

- **Build** a dump folder into a mountable image in your chosen format.
- **Convert** between formats without rebuilding from scratch.
- **Extract** any supported image back to a folder for editing.
- **Organize** your collection with a scannable, cover-art library.
- **Deploy** straight to a jailbroken PS5 over FTP, with payload, kernel-log, and mount-tool management built in.

Everything runs through a tabbed desktop UI with a live build status card, a batch queue, and an automatic diagnostic-report generator for filing issues.

---

## Screenshots

> Replace these placeholders with real captures from your build.

### Build
`![Build tab](docs/screenshots/build.png)`

### Library
`![Library tab](docs/screenshots/library.png)`

### PS5
`![PS5 tab](docs/screenshots/ps5.png)`

### Backports
`![Backports tab](docs/screenshots/backports.png)`

---

## Installation

This is a standalone Windows application — no Python install required to run the released `.exe`.

1. Download the latest `exFAT Image Builder.exe` from the [Releases page](https://github.com/kerrdec97/ps5-exfat-builder/releases).
2. (Optional but recommended) verify the `.sha256` checksum published alongside it.
3. Run the executable. There is no installer and nothing is written to the registry.

### Running from source

```bash
py -3.11 -m pip install --upgrade pillow tkinterdnd2 psutil mkpfs==0.0.7
py -3.11 exfat_builder.py
```

### Building the executable

The repository ships a `build.bat` that installs the build dependencies and produces a one-file PyInstaller build:

```bat
build.bat
```

Output lands in `dist\exFAT Image Builder.exe`.

---

## System Requirements

| Requirement | Detail |
|-------------|--------|
| Operating system | Windows 10 / 11 (64-bit) |
| Python (source only) | 3.11 |
| **OSFMount** | Required for `.exfat` create/mount and `.exfat` extraction. Install separately (free). |
| **Dokan v2** | Required for `.ffpkg` *mount-as-drive* and the mount-based `.ffpkg` extraction path. Install separately (free). v1 is not compatible. |
| mkpfs | **Bundled** (0.0.7) — used for all PFS pack/unpack. Nothing to install. |
| UFS2Tool | **Bundled** — used for `.ffpkg` creation and extraction. |
| Free disk space | PFS compression needs scratch space; budget roughly the size of the source image on the temp drive during a build. |

---

## Supported Formats

| Format | Build | Convert | Extract | Mount on PS5 |
|--------|:-----:|:-------:|:-------:|:------------:|
| `.exfat` | ✅ | ✅ | ✅ | ShadowMount+ / MicroMount |
| `.ffpkg` | ✅ | ✅ | ✅ | (UFS package) |
| `.ffpfsc` (compressed PFS) | ✅ | ✅ | ✅ | ShadowMount+ / MicroMount |
| `.ffpfs` (uncompressed PFS) | ✅ | — | ✅ | ShadowMount+ / MicroMount |

---

## Core Features

- **Three output formats** from a single Build tab: `.exfat`, `.ffpkg`, and PFS (`.ffpfsc` / `.ffpfs`).
- **Batch queue** — mix formats in one queue; jobs build sequentially in order.
- **Live build status card** — 5-phase tracker plus per-file, speed, ETA, compression, temp-space, CPU and RAM metrics.
- **Format conversion** between `.exfat` and `.ffpkg`, and straight into `.ffpfsc`.
- **Extraction** of every supported format back to a folder.
- **Game library** with multi-format scanning, cover art, search, and metadata.
- **Dump Rename** with PPSA detection, confidence scoring, and batch rename.
- **Backports** scanning, queueing, and automated processing.
- **Full PS5 toolkit** — FTP browser, payload deployment, kernel-log streaming, ShadowMount+ and MicroMount config editors, Y2JB, and a game manager.
- **Diagnostics** — automatic timestamped diagnostic report with one-click sharing.
- **16 interface languages** plus English, and **dark / light themes**.
- **Optional shutdown-when-finished** for long overnight batches.
- **Boot-time update check** against the GitHub releases (toggleable).

---

## Build Workflows

The unified **Build** tab takes a PS5 game dump folder and produces a mountable image.

### exFAT creation
Builds a RAW `.exfat` image, mounts it via OSFMount, formats it as exFAT, and copies the dump in with robocopy. Image size can be auto-computed from the source or set manually. Large payloads automatically switch to unbuffered I/O (`/J`) so the Windows cache doesn't balloon on multi-gigabyte files.

### FFPKG creation
Packs the dump into a `.ffpkg` UFS package via the bundled UFS2Tool.

### PFS / FFPFSC creation
Produces a PFS image for ShadowMount+ / MicroMount:
- `.ffpfsc` — compressed container (smaller on disk; decompressed on the console).
- `.ffpfs` — uncompressed PFS (larger, full read speed).

PFS images are produced via the official ShadowMount+ two-step method using bundled mkpfs. The Build tab also offers a route to pack an existing `.exfat` / `.ffpkg` directly into a `.ffpfsc`.

### Queue system
Add multiple games — even of different output formats — to a single queue and build them in sequence. Queue status (current / next / after) is shown live.

### Output naming presets
Names are derived from the dump's `param.json`, with three presets:
- **PPSA only** — `PPSA01234`
- **PPSA + Title** — `PPSA01234 Spider-Man`
- **PPSA + Title + Version** — `PPSA01234 Spider-Man (01.005.000)`

The generated name remains editable per job, and a global lowercase-names option is available.

### Compression settings
For PFS output, a selectable compression level (1 Fastest / 3 Fast / 6 Balanced / 9 Maximum) trades CPU time against size. PS5 game data is largely incompressible, so lower levels are usually the better speed/size balance. Worker counts scale to the level to keep compression from outrunning the disk writer.

### CPU core selection
Worker / thread counts are configurable for the copy and compression stages.

### Temporary folder support
A dedicated temp/scratch folder can be set (Advanced settings) and is used for PFS staging and compression spool, with a free-space guard.

### Progress reporting
The status card shows the active phase (Scan → Create exFAT → Format → Copy → Compress/Finalize) with live current-file, progress, speed, ETA, compression ratio, temp usage, and CPU/RAM, plus a timestamped output log.

---

## Conversion Workflows

The **Convert** tab handles format-to-format conversion without a full rebuild:

| From | To | Method |
|------|----|--------|
| `.exfat` | `.ffpkg` | Repackage via UFS2Tool |
| `.ffpkg` | `.exfat` | Extract via UFS2Tool, then build an exFAT image |
| `.exfat` / `.ffpkg` | `.ffpfsc` | Single-step mkpfs pack (PFS tab "Build from existing") |

Cross-drive conversions stage on the source drive so the instant hard link works, writing only the final (smaller) output to the chosen destination. The original source file is never modified.

---

## Extraction Workflows

The **Extract** tab unpacks any supported image back to a folder:

| Source | Method |
|--------|--------|
| `.exfat` | OSFMount mount + file mirror |
| `.ffpkg` | UFS2Tool whole-image extract, or a Dokan mount + robocopy stream (no in-memory size limit) |
| `.ffpfs` / `.ffpfsc` | mkpfs unpack (a `.ffpfsc` unpacks to its nested image) |

The `.ffpkg` mount-based path requires Dokan v2 and always unmounts the drive when finished, including on cancel or failure.

---

## Library Management

The **Library** tab scans one or more folders and presents your collection as a cover-art grid.

- **Scanning** — multi-format: detects `.exfat`, `.ffpkg`, and `.ffpfsc` images across all configured scan folders, with a live scan progress bar and per-folder chips you can add or remove.
- **Cover art** — covers are normalized to a uniform 2:3 portrait format (consistent fill/crop) and cached, so mixed source artwork renders as a clean, uniform grid even with 100+ games.
- **Search** — live text filter across the scanned set.
- **Metadata** — title, PPSA ID, and version surfaced per card from each image's metadata.
- **Images sub-tab** — a dedicated artwork view, grouped under Library.

---

## Dump Rename

The **Dump Rename** tab cleans up messy dump folder names into a consistent scheme.

- **PPSA detection** — reads `param.json` / dump metadata to recover the real PPSA ID, title, and version.
- **Rename preview** — each dump shows its proposed new name before anything is changed.
- **Confidence scoring** — a colour pill per dump:
  - 🟢 **Ready** — PPSA + title + version all resolved.
  - 🟡 **Needs review** — partial (e.g. PPSA only, or PPSA + title).
  - 🔴 **Failed** — no PPSA detected; the original name is kept.
- **Batch rename** — select any subset and rename them all at once. Renames can be done in place, or copied/renamed into a separate destination folder.

Generic folder names (e.g. `downloads`, `games`, `backup`) are rejected as titles, and the three naming presets match the Build tab.

---

## Backports

The **Backports** tab (Games / Auto / Results sub-tabs) helps prepare games for a target firmware.

- **Scanning** — scans configured backport folders and presents games as cards in a grid (auto-scans on open if folders are saved).
- **Queueing** — queue any number of games; a "Process queue (N)" button is always in view.
- **Processing** — the Auto Backport workflow detects each game's SDK, lets you set a target SDK and fakelib option, and processes the queue sequentially, with results collected in the Results sub-tab.

---

## PS5 Tools

The **PS5** tab groups every console-communication feature. All of it works over FTP to a jailbroken PS5 on your network.

### FTP
A two-pane FTP client with a **browser** (remote file tree) and **Quick Upload** sub-tab — browse the PS5 filesystem, transfer images, and send files to game directories.

### Payload deployment
A **Payload Manager** that lists saved payloads (e.g. etaHEN) with descriptions and target firmware, and sends them to the PS5 over its listening port. Custom payloads can be added.

### Klog
A live **kernel-log streamer** — connect to the PS5's klog port (default 3232), stream output in real time, filter and colour lines, and export the log.

### ShadowMount+
A config editor and control panel for ShadowMount+: edit its mount profile, send the ELF payload, and a **Safe to unplug** flow that signals ShadowMount+ to release all mounts and *confirms cleanup via the kernel log* before telling you it's safe to remove the drive.

### MicroMount
A parallel config editor for MicroMount: edit `config.ini` (target dir, scan paths, depth/interval, debug, LVD/PFS mount profile), load the live config over FTP, push edits back, send the `micromount.elf` payload, and fetch `debug.log`.

### Y2JB
A manager for the YouTube-to-Jailbreak (Y2JB) PKG workflow, with its own connection/port settings.

### Console management
A **PS5 Game Manager** that reconciles local builds against what's on the console — showing what's on the PS5, what's built locally but not uploaded, and what isn't built yet — with search, status filters, and per-game actions.

---

## Pegasus Export

> **Not applicable.** This project does not include Pegasus playlist/export functionality. (That belongs to a separate tool.) No Pegasus export exists here.

---

## PFS Support

PFS is fully supported and active.

- **Build** a dump into `.ffpfsc` (compressed) or `.ffpfs` (uncompressed).
- **Convert** an existing `.exfat` / `.ffpkg` straight into a `.ffpfsc`.
- **Extract** a `.ffpfs` / `.ffpfsc` back to a folder.

All PFS routes use bundled **mkpfs 0.0.7** and follow the official ShadowMount+ packing method. Selectable compression level, a build queue, temp-folder redirection, and live progress all apply to PFS builds.

> Note: hardware boot validation of PFS images is an ongoing community effort — see the changelog and release notes for the current testing status.

---

## Queue System

The Build tab's queue accepts multiple jobs of any output format and runs them in order:

- Add games one at a time, each with its own format, naming preset, and (for PFS) compression level.
- The queue shows **current / next / after** so you always know what's running.
- Combined with **shutdown-when-finished**, the queue is designed for unattended overnight batches.

---

## Diagnostics

- **Logging** — every build streams a timestamped output log; recent lines are also kept in a ring buffer for failure analysis.
- **Validation** — the build pipeline verifies inputs (e.g. `eboot.bin` presence), checks that copied data actually landed before finishing, and sanity-checks compressed output so an implausibly small `.ffpfsc` is treated as a failed pack (your source is kept, never silently deleted).
- **Issue reporting** — a **Report Issue** action automatically generates a timestamped diagnostic `.txt` (build summary, diagnostics, system info) and opens a card offering one-click **GitHub / Discord / Telegram / open-folder** sharing.

---

## Reporting Issues

Use the in-app **Report Issue** button (it attaches a ready-made diagnostic file), or file directly on the [GitHub Issues page](https://github.com/kerrdec97/ps5-exfat-builder/issues). When reporting a build problem, attaching the generated diagnostic `.txt` makes triage far faster.

---

## Downloads

Grab the latest build from the **[Releases page](https://github.com/kerrdec97/ps5-exfat-builder/releases)**. Each release ships the `exFAT Image Builder.exe` and a `.sha256` checksum.

---

## Contributing

Contributions, bug reports, and community game reports are welcome:

1. Open an issue describing the bug or request (attach a diagnostic report for build issues).
2. For code changes, fork the repo, make your change, and open a pull request.
3. Keep the codebase's existing structure: the entry point is `exfat_builder.py`, with tab modules under `ui/` and design tokens in `tkinter_theme.py`.

---

## Credits

- **Author / maintainer:** [Deckerr97](https://github.com/kerrdec97) (GitHub: `kerrdec97`)
- **Bundled tools:** mkpfs, UFS2Tool, DokanNet
- **External tools:** OSFMount, Dokan
- **Community contributors and donors** are credited in the in-app **Credits** tab.

---

## License

See the repository for license terms.

---

## Disclaimer

This tool is intended for managing **your own legally-owned** PS5 game backups on consoles you own and have modified yourself. It does not bypass copy protection, does not include or distribute any game content, and is not affiliated with, endorsed by, or associated with Sony Interactive Entertainment. PlayStation and PS5 are trademarks of Sony Interactive Entertainment. Use it responsibly and in accordance with the laws of your jurisdiction. The author accepts no liability for misuse or for any damage to hardware or data — back up anything you care about before use.
