# exFAT Image Builder 5

**Turn your PS5 game dumps into images that mount — and backport, convert,
verify and send them to your console — from one app, in one download.**

![Platform](https://img.shields.io/badge/Windows-10%20%2F%2011%20x64-3d7bff)
![Release](https://img.shields.io/github/v/release/kerrdec97/ps5-exfat-builder?include_prereleases&color=8b5cf6)
![Downloads](https://img.shields.io/github/downloads/kerrdec97/ps5-exfat-builder/total?color=3d7bff)
![Licence](https://img.shields.io/badge/licence-GPL--3.0-8b5cf6)

![Home](docs/screenshots/home.png)

> **Version 5 is a complete rewrite.** The app is now native C++/Qt: it
> writes exFAT, UFS2 and PFS images itself, so it needs **no administrator
> rights, no OSFMount, no .NET and no Python**, and it ships as **one .exe**.

> ⚠️ For games and content **you legally own and dumped yourself**. The app
> does not download games, decrypt retail packages or bypass DRM, and it is
> not affiliated with Sony Interactive Entertainment.

---

## ✨ What it does

| | |
|---|---|
| **Build image** | Drop a dump folder (or a zipped dump), pick a format, done. **exFAT**, **FFPKG** (UFS2), **PFS** (compressed `.ffpfsc` or plain `.ffpfs`) and installable **FPKG** `.pkg` (with your own Sony Publishing Tools). Each format shows only its own options; experts get the rest under *Advanced options*. Every image is read back and verified before it gets its final name. |
| **Backport** | Make games run on older firmware with the built-in **PS5 Backport engine 0.3.4 by Deckerr97** — the same engine as the standalone PS5 Backport GUI, so results are identical. Target SDK 4–13, PS5 Pro fix, PS5 Pro build, AMPR / PlayGo switch, and optional apply to the game folder with a verified originals backup and one-click restore. |
| **Image tools** | Open an image to **extract** it, **verify** it (or compare it with a folder), **convert** between exFAT, FFPKG, FFPFSC and FFPFS, or **edit** it (add, replace, rename, delete files). |
| **PS5 connect** | A real FTP file manager with a transfer queue, payload sender, kernel log and crash-dump capture, **ShadowMount+ 1.7** configuration (new format, safe-to-unplug), Fakelib manager, autoloader, AMPR index, Y2JB tools and package install. |
| **Library** | Your dumps and images in a cover-art grid: formats at a glance, bulk build, backport, rename. |
| **Queue** | Everything runs in a queue you can pause, reorder, cancel and retry while you keep working. |

<p align="center">
  <img src="docs/screenshots/build.png" width="49%" alt="Build image">
  <img src="docs/screenshots/backport.png" width="49%" alt="Backport">
  <img src="docs/screenshots/imagetools.png" width="49%" alt="Image tools">
  <img src="docs/screenshots/library.png" width="49%" alt="Library">
</p>

## ⚡ Quick start

1. Download **`exFAT Image Builder.exe`** from the
   [Releases page](https://github.com/kerrdec97/ps5-exfat-builder/releases)
   and check its SHA-256 (below).
2. Run it. The first start unpacks the app once (a second or two); later
   starts are instant.
3. **Build image** → drop your game folder (the one with `eboot.bin`) →
   pick a format → **Build image**.

Portable use: put an empty `portable.txt` next to the exe and the app keeps
its settings, logs and unpacked files in that folder.

## 🔐 Verify your download

Every release has `SHA256SUMS.txt`. In PowerShell:

```powershell
Get-FileHash ".\exFAT.Image.Builder.exe" -Algorithm SHA256
```

GitHub replaces the spaces in the file name with dots; the hash is the same.

## 🛡 Antivirus warnings

The exe is not code-signed and it is a self-extracting program (it unpacks
the app and the backport engine to your user folder on first start), which
some antivirus engines flag by heuristics (`Wacatac`, `!ml`). These are
guesses, not matches. You can check the hash, scan it on VirusTotal, read the
source, or build it yourself (below).

## 🧰 Requirements

* Windows 10 1809+ or Windows 11, 64-bit.
* Optional: your own **Sony Publishing Tools** (`prospero-pub-cmd.exe`) for
  FPKG packages. Nothing from Sony is included or downloaded.
* For console features: a jailbroken PS5 running an FTP server (ftpsrv or
  etaHEN), and ShadowMount+ to mount images.

## 🔨 Building from source

See [native/README.md](native/README.md). In short (Visual Studio 2022 Build
Tools, Qt 6.8+, CMake and Ninja):

```bash
cmake -S native -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DFW_FETCH_BACKPORT_ENGINE=ON
```

```bash
cmake --build build --parallel
```

```bash
powershell -NoProfile -ExecutionPolicy Bypass -File native\packaging\make-release.ps1
```

`make-release.ps1` builds, runs the test suite and writes
`dist\exFAT Image Builder.exe` plus `SHA256SUMS.txt`.

## 🙏 Credits

* **PS5 Backport engine** — Deckerr97
  ([PS5 Backport GUI](https://github.com/kerrdec97/PS5-Backport-GUI)), bundled
  unchanged as a separate program.
* PFS engine ported from **Lazy_MkPFS / PSBrew MkPFS** (GPL-3.0); UFS2 layout
  after **UFS2Tool** by SvenGDK (BSD-2-Clause); fake-SELF reader after
  **make_fself** (john-tornblom, Nazky); PS5 package facts from
  **LibProsperoPKG** (SvenGDK); **ShadowMount+**, **ftpsrv**, **etaHEN**,
  **ampr_emu** (drakmor) and the PS5 homebrew community; **Qt** (LGPL-3.0);
  **Lucide** icons (ISC).
* Everyone who reported bugs, translated and tested earlier versions.

Full notices: [native/packaging/THIRD_PARTY_NOTICES-native.md](native/packaging/THIRD_PARTY_NOTICES-native.md).

## 📜 Licence

GPL-3.0-or-later (see [LICENSES](LICENSES)). The bundled PS5 Backport engine
is a separate program under its own terms. Earlier Python versions (v1–v4)
remain in the git history and the v4.x tags.
