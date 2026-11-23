# Dependencies

Every third-party component of exFAT Image Builder 5: what it is used for,
the version, the licence, and how it gets into the build. The licence notices
that ship with the program are `native/packaging/THIRD_PARTY_NOTICES-native.md`
(copied next to the program as `THIRD_PARTY_NOTICES.md`) and the texts in
`LICENSES/` and `native/packaging/licenses/`. When this page and the notices
differ, the notices and the CMake files are right; fix this page.

## Licence of the app, and the GPL-3.0 caveat

The app is proprietary from 5.0.1 (`native/packaging/LICENCE.txt`).
Third-party components keep their own licences. The owner's note, recorded in
the notices and in `ARCHITECTURE.md`: the PFS engine and the AMPR index are
ports of Lazy_MkPFS / MkPFS (GPL-3.0), the fake-SELF reader is a port of
make_fself (GPL-3.0), and the zstd pack writer follows ps5-dump-forge
(GPL-3.0-or-later); their authors' terms still apply to those parts. Other
programs' behaviour is reimplemented, not copied. Do not add more GPL or
other copyleft code to the app: describe the idea and the licence, and leave
the decision to the owner.

## Compiled into the app

| Component | Version | Used for | Licence | How it is obtained |
|---|---|---|---|---|
| Qt 6: Core, Concurrent, Network, Gui, Widgets, Svg, Sql (SQLite driver); Test for the tests | 6.10.3 for releases built on the owner's PC (msvc2022_64); the manual Windows workflow uses 6.8.3; CMake accepts 6.4 or later | Framework, UI, sockets, SQLite edits of the console's `app.db` | LGPL-3.0, dynamically linked | Installed Qt kit (`CMAKE_PREFIX_PATH`); deployed with `windeployqt`, unused plugins removed by `finish-deploy.ps1`. The code of other projects inside the Qt files is listed in the notices and `licenses/Qt-third-party-code.txt` |
| zlib | 1.3.1 | PFS/FFPFSC compression, ZIP | zlib | `find_package(ZLIB)`; if none is found, FetchContent from the madler/zlib release, pinned by SHA-256 (`native/CMakeLists.txt`) |
| zstd | 1.5.7 | zstd asset packs (write and read) | BSD-3-Clause | FetchContent, pinned by SHA-256; only `lib/common`, `lib/compress`, `lib/decompress` C sources (`ZSTD_DISABLE_ASM`, no legacy formats) |
| LZ4 | 1.10.0, `lib/lz4.c` only | Reading LZ4 (AMPRPAK4) packs; never written | BSD-2-Clause | FetchContent, pinned by SHA-256 |
| libarchive | 3.8.11, the 7z, RAR and RAR5 readers | 7z and RAR archives as build sources | BSD-2-Clause | FetchContent, pinned (`cmake/ArchiveLibs.cmake`); compiled with `cmake/libarchive/fw_libarchive_config.h`, not with its own CMake project. `fw_libarchive_write` (7z writer) is linked into `tst_archive_source` only |
| liblzma (XZ Utils) | 5.8.4, library only | LZMA/LZMA2 for 7z | 0BSD | FetchContent, pinned; XZ Utils' CMake project with every tool off |
| Lucide icons | vendored SVGs | Interface icons | ISC (`LICENSES/Lucide-ISC.txt`) | In the repository: `native/src/app/resources/icons/` (`icons.qrc`) |
| Microsoft Visual C++ runtime | from the MSVC used for the build | `vcruntime140*.dll`, `msvcp140*.dll` next to the program | Microsoft distributable code | Copied from the MSVC redist folder by `finish-deploy.ps1` (no `vc_redist.x64.exe` in the release). The one-file launcher uses the static runtime |

## Code ported or formats followed

| Source | What in the app | Licence |
|---|---|---|
| Lazy_MkPFS (Nazky), MkPFS (PSBrew) | PFS / FFPFSC engine and AMPR index (`src/engines/pfs`), byte-identical to Lazy_MkPFS; `engines/PFS.md` names the upstream commit | GPL-3.0 (port) |
| make_fself (john-tornblom, modified by Nazky) | Fake-SELF reader (`src/engines/backport/SelfFormat.h`, `SelfReader.*`) | GPL-3.0 (port) |
| ps5-dump-forge 0.5.4 (quer3q) | zstd pack writer and reader, packing rules, LZ4 reader, image-wrapped `.ffpfsc` (`src/engines/ampr`, `integration/ImageFiles.cpp`) | GPL-3.0-or-later |
| ampr-emu-ng (quer3q) | Format of the zstd packs | GPL-3.0-or-later |
| UFS2Tool 4.0.0 (SvenGDK) | FFPKG layout (`src/engines/ufs2`), byte-identical to `newfs -D` | BSD-2-Clause (`LICENSES/UFS2Tool-BSD-2-Clause.txt`) |
| LibProsperoPKG (SvenGDK) | Facts used by the FPKG engine: PS5 flat-path hash, UCP layout | GPL-3.0-or-later |
| Microsoft exFAT specification | exFAT engine (`engines/EXFAT.md`) | Public specification |
| PSVIETHOA FPKG Builder 3.1 | FPKG behaviour, reimplemented from `FPKG_PARITY.md`; no code copied | — |
| ppr-pkg Companion 1.5.1 (the owner's) | Backport update packages and the DLC page (`PPR_COMPANION.md`) | the owner's |

## Bundled programs (separate files, pinned)

| File in a build | Version | Licence | Pin and switches |
|---|---|---|---|
| `engines/ampr-emu-ng/libSceAmpr.sprx` | ampr-emu-ng 0.5.0 release build (runs on the console; copied unchanged to `fakelib/` of a packed game) | GPL-3.0-or-later; source is the tagged release, named in the notices | `cmake/AmprRuntime.cmake`: `FW_AMPR_RUNTIME_SHA256`. `FW_AMPR_RUNTIME_SPRX=<file>`, or `FW_FETCH_AMPR_RUNTIME=ON` (default) downloads it once into `<build>/_deps`. Without it the app builds and the zstd option says the runtime is missing |
| `engines/backport/PS5BackportGUI.exe` | PS5 Backport GUI 0.3.4 (the owner's, closed source), run as a child process, no code linked | Its own terms | `cmake/BackportEngine.cmake`: `FW_BACKPORT_ENGINE_SHA256`. `FW_BACKPORT_ENGINE_EXE=<file>` (preferred), or `FW_FETCH_BACKPORT_ENGINE=ON` (default OFF; the Windows workflow and `make-release.ps1` without `-BackportEngine` turn it on). Without it the Backport tab shows "Engine missing" |

`make-release.ps1` checks both files in the build tree against these pins
before it deploys. The pins, the URLs and the files are changed only by the
owner.

## Supplied by the user (never in the repository or a public build)

| Tool | Used by | Located through |
|---|---|---|
| Sony Publishing Tools (`prospero-pub-cmd.exe`, `prospero-dds2png.exe`; SDK Fix toolchain for backport updates) | FPKG builds, verify, extract | Settings › Tools, or `SCE_PROSPERO_PUBTOOLS` |
| fpkg-cli (PSVIETHOA FPKG Builder, front end of drakmor's LibProsperoPkg) | DLC page | Settings › Tools › DLC engine, or `FW_FPKG_CLI` |
| YouTube app packages | PS5 connect › Y2JB | A folder the user chooses |

Only the owner's private builds carry copies of these
(`make-release.ps1 -PrivateToolkit`, `-PrivateDlcEngine`, `-PrivateYoutube`,
output in `dist-private\`); `private-files.ps1` keeps them out of a public
build.

## Build and development tools

| Tool | Version | Notes |
|---|---|---|
| Visual Studio 2022 C++ x64 tools (MSVC), Windows SDK | 2022 | Releases. Installing it needs a UAC prompt: the owner's |
| CMake | 3.24 or later | `cmake_minimum_required(VERSION 3.24)` |
| Ninja | any recent | The generator used everywhere |
| Python 3 | any recent | Only for the fixture scripts in `native/tests/data` and `tests/core/make_archive_fixtures.py`; the app does not use Python |
| Inno Setup 6 | 6 | `packaging/installer.iss`, Windows workflow artefact only |
| GCC, Ubuntu 24.04 packages | as installed by `native-linux.yml` | Linux tests with ASan/UBSan |
| GitHub Actions | `actions/checkout@v4`, `jurplel/install-qt-action@v4`, `ilammy/msvc-dev-cmd@v1`, `actions/upload-artifact@v4`, `actions/download-artifact@v4` | The three workflows |

## Building without the network

Every downloaded source can come from a local copy of the same pinned
archive (for example the `_deps` folder of an earlier build):

```powershell
cmake -S native -B <build> -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_PREFIX_PATH=<Qt>" `
  -DFETCHCONTENT_FULLY_DISCONNECTED=ON `
  "-DFETCHCONTENT_SOURCE_DIR_ZLIB=<deps>\zlib-src" `
  "-DFETCHCONTENT_SOURCE_DIR_FW_ZSTD_SRC=<deps>\fw_zstd_src-src" `
  "-DFETCHCONTENT_SOURCE_DIR_FW_LZ4_SRC=<deps>\fw_lz4_src-src" `
  "-DFETCHCONTENT_SOURCE_DIR_FW_XZ_SRC=<deps>\fw_xz_src-src" `
  "-DFETCHCONTENT_SOURCE_DIR_FW_LIBARCHIVE_SRC=<deps>\fw_libarchive_src-src" `
  "-DFW_AMPR_RUNTIME_SPRX=<deps>\ampr-emu-ng\libSceAmpr-0.5.0.sprx" -DFW_FETCH_AMPR_RUNTIME=OFF `
  "-DFW_BACKPORT_ENGINE_EXE=<folder>\PS5BackportGUI-0.3.4.exe" -DFW_FETCH_BACKPORT_ENGINE=OFF
```

The SHA-256 checks of the two bundled files still run.

## Changing a dependency

* Third-party sources: pin the release archive by `URL_HASH SHA256=...`,
  compile library code only, keep its warnings out (`/W0`, `-w`), add or
  update its row in `THIRD_PARTY_NOTICES-native.md` and its licence text in
  `native/packaging/licenses/`, and update this page and `README.md` credits.
* A new Qt module: add it to the notices' Qt row and check `finish-deploy.ps1`
  keeps the plugins it needs.
* Never fetch or run unofficial binaries; use published releases and source.
