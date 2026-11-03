# 🎮 exFAT Image Builder — Farewell Edition

**A Windows toolkit for building, converting, verifying, editing and
backporting PS5 game images from your own game dumps.**

![Platform](https://img.shields.io/badge/Platform-Windows%2010%20%2F%2011-blue)
![Python](https://img.shields.io/badge/Python-3.11-yellow)
![Release](https://img.shields.io/github/v/release/kerrdec97/ps5-exfat-builder)
![Downloads](https://img.shields.io/github/downloads/kerrdec97/ps5-exfat-builder/total)

> ### 👋 This is the final release
> **v4.1.0 — Farewell Edition** is the last version of exFAT Image Builder.
> It restores the complete v4.0.2 source (which had never been published),
> fixes the problems reported since — most importantly finished images being
> deleted on non-English Windows — and gives the app a consistent, modern
> look. Thank you to everyone who used it, reported bugs, translated it and
> supported it. See [CHANGELOG.md](CHANGELOG.md) for everything that changed.

> ⚠️ **Homebrew & personal backup tool.** Use it only with games and content
> **you legally own and dumped yourself**. It does not download games, decrypt
> retail packages, bypass DRM or provide copyrighted content.

------------------------------------------------------------------------

## ✨ Features

**Build & convert**
- Build **exFAT** images, **FFPKG** (UFS2) images and compressed **FFPFSC** /
  uncompressed **FFPFS** PFS images from a game dump folder.
- Build installable **FPKG** (`.pkg`) packages with your own Sony Publishing
  Tools (`prospero-pub-cmd.exe`, not included): project generation, PlayGo
  layout, progress, header checks and optional `img_verify`. See
  [docs/FPKG.md](docs/FPKG.md).
- Convert existing `.exfat` / `.ffpkg` images to each other or to `.ffpfsc`.
- Mixed-format **build queue**, editable while it runs (queued jobs can be
  added, removed and reordered).
- Automatic game detection (title, PPSA, version, firmware) and output naming
  presets, including a custom template (`%TITLE% %PPSA% %VERSION% %FIRMWARE%`).
- Temp/staging folder support, free-space checks and automatic **AMPR index**
  generation when `fakelib/libSceAmpr.sprx` is present.
- Two PFS backends: bundled **Lazy_MkPFS** (default) and stock **mkpfs**.

**Verify, extract & edit**
- Post-build verification (mount + content comparison, or native MkPFS
  verify). Finished images are **never deleted** by a status check — doubtful
  results are kept and flagged for you to inspect.
- Extract `.exfat`, `.ffpkg`, `.ffpfs` and `.ffpfsc` back to folders.
- Edit images: add, replace and delete files; FFPKG editor.

**Backports**
- Auto Backport, SDK detection and SDK fixer, batch SDK retag, fakelib
  support, language stripper, and restore of original files from managed
  backups.

**PS5**
- FTP browser and transfer queue, connection diagnostics and speed test,
  remote content manager, payload sender, live kernel log, ShadowMount+ /
  MicroMount configuration with Fakelib Manager, Y2JB tools, DLC-emu editor.

**Library & more**
- Game library with cover art, metadata, duplicate detection, bulk rename
  (Dump Rename), history, diagnostics report, optional shutdown when finished.
- **17 interface languages** (English + 16 community translations).

------------------------------------------------------------------------

## 📦 Supported formats

| Format | Build | Convert | Extract | Notes |
|--------|:-----:|:-------:|:-------:|-------|
| `.exfat` | ✅ | ✅ | ✅ | Needs OSFMount |
| `.ffpkg` (UFS2) | ✅ | ✅ | ✅ | Needs .NET 8; mount-based extract needs Dokan v2 |
| `.ffpfsc` (compressed PFS) | ✅ | ✅ | ✅ | Bundled backends |
| `.ffpfs` (uncompressed PFS) | ✅ | — | ✅ | Bundled backends |

------------------------------------------------------------------------

## 🧰 Requirements

| Requirement | Needed for |
|-------------|------------|
| Windows 10 / 11, 64-bit | Everything |
| Administrator rights | Mounting images (the app asks for elevation) |
| **[OSFMount](https://www.osforensics.com/tools/mount-disk-images.html)** (free) | Building, verifying and extracting `.exfat` images |
| **Microsoft .NET 8 Runtime (x64)** | `.ffpkg` creation / conversion (UFS2Tool) |
| **Dokan v2** | Mount-based `.ffpkg` extraction (v1 is not compatible) |
| Bundled — nothing to install | Lazy_MkPFS, mkpfs 0.0.9, UFS2Tool, robocopy (part of Windows) |

------------------------------------------------------------------------

## ⚡ Quick start

1. Install OSFMount (and .NET 8 if you want `.ffpkg`).
2. Download `exFAT Image Builder.exe` from the
   [Releases page](https://github.com/kerrdec97/ps5-exfat-builder/releases)
   and verify its SHA-256 (see below).
3. Open **Build**, pick your game dump folder (the folder containing
   `eboot.bin`), choose **exFAT**, **ffpkg** or **PFS**, and pick an output
   folder.
4. Click **Add to queue**, then **Build all**.
5. Watch the progress card; the result appears in the output folder.

------------------------------------------------------------------------

## 🔐 Verifying your download (SHA-256)

Every release ships `exFAT Image Builder.exe.sha256`. Compare it with:

```powershell
Get-FileHash ".\exFAT Image Builder.exe" -Algorithm SHA256
```

```bat
certutil -hashfile "exFAT Image Builder.exe" SHA256
```

The hash must match exactly (case does not matter). If it doesn't, delete the
file and download it again from the official Releases page.

------------------------------------------------------------------------

## 🛡 Antivirus & false positives

Some antivirus engines flag the `.exe` with generic, machine-learning
detections (for example `Wacatac.B!ml`). These are guesses, not matches
against known malware. They happen because:

- the app is a **PyInstaller** single-file executable that unpacks itself at
  start-up — a pattern heuristics distrust;
- it legitimately does system-level work: mounting images, assigning drive
  letters, running OSFMount / robocopy / format, opening FTP connections to
  your PS5, and requesting Administrator rights;
- it is **not code-signed**.

The Farewell build is made in a clean, pinned environment, does not use UPX
compression and carries Windows version information. That reduces some
triggers but **cannot guarantee** a clean scan — only code signing would. To
satisfy yourself the build is safe: read the source in this repository, build
it yourself with `build.bat`, check the SHA-256, or scan it on VirusTotal.

------------------------------------------------------------------------

## 🔨 Building from source

Requirements: Windows 10/11 and 64-bit **Python 3.11** available as `py -3.11`.

```bat
build.bat
```

`build.bat` creates an isolated virtual environment (`.build-venv`) with the
pinned packages from `requirements-build.txt`, runs the release checks
(`tools/release_check.py`: compile, bundled files, version consistency and the
unit tests), builds `dist\exFAT Image Builder.exe` with PyInstaller and writes
the `.sha256` file. `build.bat clean` recreates the environment.

Running from source:

```bat
py -3.11 -m pip install -r requirements-build.txt
py -3.11 exfat_builder.py
```

Running the tests:

```bat
py -3.11 -m unittest discover -s tests -t .
```

Set `EXFAT_GUI_SMOKE=1` to also run the GUI smoke test (it opens the main
window and visits every page).

------------------------------------------------------------------------

## 🩺 Troubleshooting

**"Windows protected your PC" (SmartScreen)** — the exe is unsigned. If the
SHA-256 matches, click **More info → Run anyway**.

**OSFMount not found** — install OSFMount from PassMark. If you set a custom
path in Settings, point it at the *installed* `osfmount.com` /
`OSFMount.exe`, not at the downloaded installer.

**Stuck at "Creating & mounting image"** — the OSFMount driver can get stuck on
a mount left over from an earlier run. The build now times out with a message;
use **Force Dismount**, and restart Windows if that does not help.

**Verification says "inconclusive"** — the image mounted but Windows did not
make the drive readable in time (often antivirus scanning a new volume). The
image is kept; mount it yourself to check.

**"Not enough space in the image" when adding files** — exFAT images have a
fixed size. Rebuild with a larger custom size (Build → exFAT image size →
Custom).

**`.ffpkg` builds fail immediately** — install the Microsoft .NET 8 Runtime
(x64); the log says so when that is the cause.

**Something else** — use **Report Issue** in the app to generate a diagnostic
report (version, system information, recent log, redacted settings).

------------------------------------------------------------------------

## ⌨️ Keyboard shortcuts

| Shortcut | Action |
|----------|--------|
| `Ctrl + B` | Start the build queue (Build tab) |
| `Esc` | Cancel the running build queue (asks first) |
| `F5` | Refresh the current list (PS5, Library, My Images) |
| `Ctrl + Shift + T` | Test the PS5 FTP connection |
| `Ctrl + Q` | Quit |

------------------------------------------------------------------------

## 🧪 What was and wasn't tested for the Farewell Edition

**On Windows (GitHub Actions, Windows Server 2025, every push to the
development branch):** the release exe is built with `build.bat`; 118
automated tests run, including a real `.ffpkg` build with the bundled
UFS2Tool on .NET 10 and a PowerShell parse of the embedded build script; the
app opens every page in English, German and Japanese with no errors and
screenshots each one; the built exe is launched and must still be running
after 40 seconds. The exe and screenshots are kept as build artifacts.

**On Linux:** the same tests plus GUI tests under Xvfb, including an FPKG
job driven through the real Build tab against a stand-in Publishing Tools.

**Not tested:** image builds that need OSFMount (not available on the CI
runner), robocopy copying a real game, the FPKG builder against real Sony
Publishing Tools, and anything on a PS5. Please report anything that
misbehaves, with the log.

------------------------------------------------------------------------

## ⚠️ Known limitations

- Files larger than 4 GB inside an `.ffpkg` cannot be extracted with the
  bundled UFS2Tool.
- Compressed archives (zip / 7z / rar) are not read directly — extract them
  first.
- Wine / Linux is not supported (OSFMount needs real Windows).
- FPKG needs your own Publishing Tools build that makes plaintext debug
  packages; a stock install's output is rejected. It has not yet been tested
  with real tools or on a console. Paths must be ASCII; no DLC packages yet.
- The PS5 Backport GUI's engine improvements are not merged into the
  Backports page yet.

------------------------------------------------------------------------

## ❤️ Credits

Created and maintained by **DecKerr97** ([kerrdec97](https://github.com/kerrdec97)).
Inspired by **NookieAI** • **stonemodder (Porkfolio)**.

Thanks to everyone in the PS5 homebrew community, especially:

-   Nazky — Auto-Backport pipeline and Lazy_MkPFS
-   BestPig
-   SvenGDK — UFS2Tool
-   drakmor — ShadowMount+ / MicroMount, DLC-emu and AMPR research
-   PSBrew — MkPFS and PFS research
-   john-tornblom — PS5 payload SDK
-   ps5-payload-dev
-   idlesauce
-   NookieAI
-   stonemodder
-   alik70952 — UI modernisation proposal (pull request #57)

…and every translator, tester, issue reporter and supporter. See
**[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)** for full licensing
information and `LICENSES/` for the licence texts.

------------------------------------------------------------------------

## ⚖️ Disclaimer

This software is intended only for content you legally own. It is **not
affiliated with or endorsed by Sony Interactive Entertainment**. PlayStation
and PS5 are trademarks of Sony Interactive Entertainment. Use at your own
risk and back up anything you care about.
