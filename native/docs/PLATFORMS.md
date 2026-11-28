# Windows, Linux and macOS

exFAT Image Builder is one code base for three systems. Windows is where it
started and where most of its users are; the Linux and macOS port began on
28 November 2026 (branch `dev/linux-mac-port`). This page
says what differs per system, how each is built and packaged, and what is
proven where.

## What works where

| Feature | Windows | Linux | macOS | Notes |
|---|---|---|---|---|
| exFAT, FFPKG/UFS2, PFS (FFPFSC/FFPFS), zstd packs: build, verify, extract, convert, edit | yes | yes | yes | Byte-identical output: the format tests compare with the same fixtures on every system |
| Game metadata, Library, dump check, tools | yes | yes | yes | |
| PS5 Connect (FTP, payloads, kernel log, ShadowMount+, Y2JB) | yes | yes | yes | macOS asks once for "local network" access |
| Queue, history, settings, 42 languages | yes | yes | yes | |
| After the queue: sleep, hibernate, shut down, restart; keep awake | yes | yes (systemd: `systemctl`, `systemd-inhibit`) | yes, no hibernate (`pmset`, System Events, `caffeinate`) | `core/Power.cpp` |
| Backport tab | yes | yes | yes | Each system bundles that system's build of the PS5 Backport engine (from 0.3.5-test5) |
| FPKG packages (Sony Publishing Tools) | yes | through Wine | through Wine (Rosetta 2 on Apple Silicon) | The user's own tools; PSVIETHOA FPKG Builder's Linux/macOS downloads show the same approach |
| DLC packages (fpkg-cli) | yes | yes | yes | PSVIETHOA FPKG Builder ships a native fpkg-cli for each system |
| Update check | yes | yes | yes | Each system is offered only its own file (`connect/update`) |
| Portable mode | `portable.txt` next to the exe | next to the AppImage, or in the folder edition | no (the app bundle keeps its files in Application Support) | |
| Show in folder | selects the file | selects it where the file manager supports `org.freedesktop.FileManager1`, else opens the folder | selects it in Finder | `ui/Widgets.cpp` |

## Where the app keeps its files

| | Settings, queue, history | Logs, cache, temporary files |
|---|---|---|
| Windows | `%APPDATA%\kerrdec97\exFAT Image Builder Farewell` | `%LOCALAPPDATA%\kerrdec97\exFAT Image Builder Farewell` |
| Linux | `~/.config/exfat-image-builder` | `~/.local/share/exfat-image-builder` (never `/tmp`, which is often memory) |
| macOS | `~/Library/Application Support/exFAT Image Builder` | the same folder |

`FW_DATA_DIR` overrides all of them, as before (tests, CI). The Windows names
keep "Farewell" so settings carry over from every earlier version; the new
systems had no earlier version, so they use the plain name. The Backport tab
uses the engine's own folders on each system (`app/backport/BackportHost.cpp`).

Files shipped with the program (`engines/`, `tools/`, `LICENSES/`, the
notices) are found through `Paths::resourceDir()`: the program's folder on
Windows and Linux, `Contents/Resources` inside the macOS app bundle (code
signing allows no data files next to the executable).

## Platform rules in the code

* Conditionals are `Q_OS_WIN`, `Q_OS_MACOS`, `Q_OS_LINUX`; "not Windows" means
  POSIX code that must also compile on macOS.
* Names inside a game follow the console, not the host: the AMPR runtime's
  own names (`ampr_emu.index`, `fakelib/libSceAmpr.sprx`) are found whatever
  their letter case on every system, and an archive whose game folder comes in
  two spellings unpacks into one folder named as on Windows.
* "Names this computer cannot store" (Windows device names, trailing dots,
  300-byte names) are each system's own limit (`core/PathSafety`): Linux and
  macOS store `CON` or `name.`; ext4 and APFS refuse names over 255 bytes.
* Paths compare without regard to case on Windows and macOS (NTFS, APFS as
  macOS formats it), with case on Linux (`isSameOrInside`).
* Flushing to disk: `FlushFileBuffers` (Windows), `F_FULLFSYNC` (macOS; plain
  `fsync` only hands data to the drive there), `fsync` (Linux).
* Temporary folders record the owner's process id and start time on every
  system (Linux `/proc/<pid>/stat`, macOS `sysctl`), so a reused id does not
  keep a stale folder.

## Building

Windows: `AGENTS.md`. Linux (Ubuntu 22.04 or newer, GCC 12+, Qt 6.10.3
`gcc_64` from the Qt installer or `aqtinstall`):

```sh
cmake -S native -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.3/gcc_64 -DFW_FETCH_BACKPORT_ENGINE=ON
cmake --build build
FW_OFFLINE=1 QT_QPA_PLATFORM=offscreen ctest --test-dir build --timeout 900
```

Run the tests as a normal user, not root: root ignores read-only files and
permissions, and several tests check exactly that. Install `fonts-inter`
(the interface font) and `exfatprogs` (`fsck.exfat` for the exFAT tests).

macOS (13.3 or later, Xcode command-line tools, Qt 6.10.3 `macos`): the same,
with `-DCMAKE_OSX_ARCHITECTURES=arm64` (or `x86_64`) and
`-DCMAKE_OSX_DEPLOYMENT_TARGET=13.3` (`std::to_chars` for numbers needs 13.3). The build makes `bin/exFAT Image
Builder.app`.

On the owner's PC, Linux is built and tested in WSL (Ubuntu 22.04, tools in
`/root/fwtools`, Qt in `/root/Qt`); macOS only on GitHub's runners.

## Packaging

`native/packaging/make-release-unix.sh --build-dir build` (on Linux or a Mac):

* Linux: `exFAT-Image-Builder-linux-<arch>.AppImage` (linuxdeploy with its Qt
  plug-in, appimagetool and the AppImage runtime, all pinned by SHA-256 in the
  script; X11 and Wayland) and `exFAT-Image-Builder-linux-<arch>.tar.gz` (the
  same files as a folder, for systems without FUSE and for portable use).
  Built on Ubuntu 22.04, so they run on systems with glibc 2.35 or newer.
* macOS: `exFAT-Image-Builder-macos-<arch>.dmg` with the app bundle,
  `macdeployqt`, ad-hoc signed (not notarised: users allow it once under
  System Settings > Privacy & Security).
* Both: LICENCE.txt, the public-edition notices, LICENSES/, the bundled
  engines checked against their pins, the private-file guard, a `--self-test`
  of the packaged program with its own data folders, and
  `SHA256SUMS-<platform>-<arch>.txt`, whose lines go into the release's one
  `SHA256SUMS.txt` (the update check reads that file).

Private builds (Sony tools, fpkg-cli, YouTube packages) are Windows-only and
made with `make-release.ps1`, as before.

## CI

`.github/workflows/native-unix.yml`: Ubuntu 22.04 (x86_64), macOS 15 on
Apple Silicon and on Intel. Each builds with Qt 6.10.3, runs every test,
packages with `make-release-unix.sh`, runs the package's self-test and
screenshots every page; everything is uploaded as artefacts. Build errors and
failing test lines are repeated as annotations, which the public API serves
without signing in. It runs on pushes to `dev/linux-mac-port`, for pull
requests and by hand. `native-linux.yml` (Ubuntu 24.04, sanitizers, distro Qt)
stays as it was.

## What only a person can check

Marked **HARDWARE REQUIRED** in reports: images built on Linux or a Mac on a
real PS5; the file dialogs and drag and drop of each desktop; high-DPI and
Wayland scaling; external and network drives; Gatekeeper and the privacy
prompts on a Mac; Sony's tools under a real Wine; the Backport engine's
Linux and macOS builds with real games.
