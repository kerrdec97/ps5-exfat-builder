# exFAT Image Builder 5 (native C++)

exFAT Image Builder 5 is a native C++20 / Qt 6 Windows application for PS5
game dumps. It builds, verifies, extracts, converts and edits PS5 game images
(exFAT, FFPKG/UFS2, FFPFSC and FFPFS/PFS), builds FPKG packages with your own
Sony Publishing Tools, backports games to older firmware with the bundled PS5
Backport engine, and talks to a PS5 over the network (FTP, payloads, kernel
log, ShadowMount+). It replaces the Python application of v1–v4.

> For games and content you legally own and dumped yourself. It does not
> download games, decrypt retail packages or bypass DRM, and it is not
> affiliated with or endorsed by Sony Interactive Entertainment.

## Requirements

* Windows 10 1809 or later, 64-bit (Windows 11 recommended).
* No administrator rights, OSFMount, .NET or Python: image formats are written
  and read natively.
* Optional, user-supplied: Sony Publishing Tools (`prospero-pub-cmd.exe`) for
  FPKG packages. Nothing from Sony is included or downloaded.

## Install

* **Single file (recommended)**: download `exFAT Image Builder.exe` and run
  it; nothing to unzip or install, no admin prompt, no VC++ Redistributable
  needed. The first start of each version unpacks the program once (about
  60 MB, a second or less) to
  `%LOCALAPPDATA%\kerrdec97\exFAT Image Builder\runtime\` (or `%TEMP%` if that
  is not writable); later starts run it from there at once, and older versions
  are removed. A damaged download refuses to start. With an empty
  `portable.txt` next to the exe, the unpacked copy goes to
  `exFAT Image Builder.data\` and settings and logs to `data\` beside it.
  To remove the app, delete the exe and that `runtime` folder.
* **Installer**: `exFAT-Image-Builder-Farewell-<version>-win64-setup.exe`
  installs per user (no admin prompt) or for all users.
* **Portable**: unzip `exFAT-Image-Builder-Farewell-<version>-win64-portable.zip`
  anywhere. Create an empty `portable.txt` next to the exe to keep settings and
  logs in a `data` folder beside it.
* Verify downloads with `SHA256SUMS.txt` from the same release.
* Release build on a PC: `native\packaging\make-release.ps1 [-Zip]` writes
  `dist\exFAT Image Builder.exe` and `dist\SHA256SUMS.txt`;
  `native\packaging\make-onefile.ps1` packs any deployed folder (every file
  under it, e.g. `engines\backport\...`, or `-Extra '<source>=<path inside>'`).

## Where things are stored

| What | Default location | Portable mode |
|---|---|---|
| Settings, queue, history | `%APPDATA%\kerrdec97\exFAT Image Builder Farewell` | `<app>\data` |
| Logs (text + JSON lines), crash reports | `%LOCALAPPDATA%\kerrdec97\exFAT Image Builder Farewell\logs` (or Settings › Logging › Log folder) | `<app>\data\logs` |
| Temporary files | `%LOCALAPPDATA%\...\temp` (or Settings › Storage › Temporary folder); folders older than a day are removed at start | `<app>\data\temp` |

Settings from the legacy app (`~/.exfat_builder_settings.json`) can be
imported from Settings › Advanced.

## Command line

```
"exFAT Image Builder.exe" [--self-test] [--screenshot-pages <dir>] [--quit-after <ms>]
```

* `--self-test` prints a JSON report of every dependency and the registered
  settings and job types, then exits. The single-file exe passes the command
  line, working folder and exit code through, and shows the output in the
  console it was started from (or wherever it is redirected).
* `--screenshot-pages <dir>` renders every page to PNG files (used by CI).

## Building from source

Requirements: CMake 3.24+, Ninja, a C++20 compiler (MSVC 2022, GCC 13 or
Clang 16), Qt 6.4 or later (Core, Gui, Widgets, Svg, Network, Concurrent,
Test). zlib is used from the system or fetched automatically.

Windows (Developer PowerShell for VS 2022, Qt 6.8 MSVC kit on `PATH`):

```bash
cmake -S native -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DFW_FETCH_BACKPORT_ENGINE=ON
```

The Backport tab needs the PS5 Backport engine next to the app
(`engines/backport/PS5BackportGUI.exe`). `-DFW_FETCH_BACKPORT_ENGINE=ON`
downloads the pinned release once and checks its SHA-256;
`-DFW_BACKPORT_ENGINE_EXE=<path>` uses a local copy instead. Without either the
app builds and the Backport tab says the engine is missing
(`cmake/BackportEngine.cmake`).

```bash
cmake --build build --parallel
```

```bash
ctest --test-dir build --output-on-failure
```

```bash
windeployqt --release --no-translations "build\bin\exFAT Image Builder.exe"
```

Linux (development and CI; the app is a Windows product but the engines and
tests are portable):

```bash
sudo apt-get install ninja-build qt6-base-dev qt6-base-dev-tools libqt6svg6-dev zlib1g-dev exfatprogs
```

```bash
cmake -S native -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build build && QT_QPA_PLATFORM=offscreen ctest --test-dir build
```

Continuous integration: `.github/workflows/native-windows.yml` (MSVC + Qt 6.8:
build, tests, deploy, launch the real exe, portable zip + installer) and
`.github/workflows/native-linux.yml` (GCC with AddressSanitizer and UBSan).

## Project layout

See `docs/ARCHITECTURE.md`. In short: `src/core` (errors, cancellation,
progress, logging, settings, files, processes, ZIP, JSON), `src/jobs` (queue,
history), `src/engines/*` and `src/connect/*` (one module per feature, each
registering its settings, dependency checks and job types), `src/app` (the Qt
Widgets interface built on the design system in `docs/DESIGN_PROPOSAL.md`),
`tests/` (Qt Test suites, one executable per file).

## Licence

GPL-3.0-or-later. The PFS engine is a port of Lazy_MkPFS / PSBrew MkPFS
(GPL-3.0); the UFS2 engine follows UFS2Tool's layout (BSD-2-Clause); Qt is
used under the LGPL-3.0; icons are Lucide (ISC). See
`packaging/THIRD_PARTY_NOTICES-native.md` and the `LICENSES` folder.
