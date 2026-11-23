# Repository map

Which folder owns what, and which document describes it. The layout and the
module contract are in [ARCHITECTURE.md](ARCHITECTURE.md); this page is the
index for finding the code. Paths are relative to the repository root.

## Top level

| Path | What it is |
|---|---|
| `README.md` | The user-facing page (download, features, credits, licence) |
| `AGENTS.md` | Start here as a developer or coding agent |
| `CONTRIBUTING.md` | Branches, commits, tests, what never goes into the repository |
| `CHANGELOG.md` | Every version, newest first (versions 1 to 4 were the Python app) |
| `THIRD_PARTY_NOTICES.md` | Points to `native/packaging/THIRD_PARTY_NOTICES-native.md` |
| `LICENSES/` | Licence texts copied into every build (`GPL-3.0.txt`, `Lucide-ISC.txt`, `UFS2Tool-BSD-2-Clause.txt`) |
| `docs/FPKG.md` | User guide for FPKG packages |
| `docs/screenshots/` | Pictures used by `README.md` |
| `docs/history/` | README and changelog of the Python app (3.6.4), kept as history |
| `.github/workflows/` | `native-windows.yml` (build, test, package; by hand or called), `native-linux.yml` (GCC with ASan/UBSan; by hand), `native-release.yml` (on a `v5.*` tag: pre-release unless the release exists) |
| `native/` | The application, its tests, packaging and documents |

## `native/`

| Path | What it is |
|---|---|
| `CMakeLists.txt` | Project `FarewellEdition`, the version (`project(... VERSION x.y.z)`, read by every packaging script), options `FW_BUILD_TESTS`, `FW_BUILD_APP`, `FW_WARNINGS_AS_ERRORS`; zlib, zstd and LZ4 sources |
| `cmake/FwWarnings.cmake` | `fw_configure_target()` (warnings, defines, `/utf-8`), `fw_add_module()`, `fw_strip_pdb_path()` |
| `cmake/ArchiveLibs.cmake`, `cmake/libarchive/` | libarchive (7z, RAR, RAR5 readers only) and liblzma, with a hand-written configuration header |
| `cmake/AmprRuntime.cmake` | The bundled ampr-emu-ng runtime, pinned by version and SHA-256 |
| `cmake/BackportEngine.cmake` | The bundled PS5 Backport engine, pinned by version and SHA-256 (included from `src/app/CMakeLists.txt`) |
| `README.md` | Install, data locations, command line |
| `RELEASE_NOTES.md` | Notes of the current release (the release workflow publishes this file) |
| `packaging/` | See [Packaging](#packaging) |
| `docs/` | See [Documents](#documents) |

## Source modules (`native/src/`)

Every module is a static library. Engines and connection modules are added by
`src/CMakeLists.txt` from every sub-folder that has a `CMakeLists.txt`; each
of those is one `fw_add_module(<target> DEPENDS ...)` line, and the sources are
globbed. `src/registration/Registration.cpp` calls each module's
`registerModule()`.

| Folder | Target | Owns | Depends on | Document |
|---|---|---|---|---|
| `core/` | `fw_core` | `Result`, `Cancellation`, `Progress`, `Log`, `Settings` (schema and store), `Paths`, `FileUtil` (`PartFile`, free space, renames), `PathSafety`, `FsLinks`, `FolderAccess`, `ProcessRunner`, `Dependencies`, `Zip`, `ArchiveReader`/`ArchiveSource` (7z/RAR/ZIP sources), `OrderedJson`, `RandomSource`, `Power`, `CrashHandler`, `Tr` (i18n), `BuildInfo.h.in` | Qt Core, Concurrent, zlib, libarchive (private) | ARCHITECTURE.md, I18N.md |
| `jobs/` | `fw_jobs` | `Job`, `JobFactory`, `JobQueue`, `JobHistory`, `QueueCompletionWatcher`, `Module.h` (`ModuleContext`) | core | ARCHITECTURE.md, UI.md section 6 |
| `engines/general/` | `fw_engine_general` | App-wide settings: General, Performance, Storage, Verification, Logging, Advanced | — | — |
| `engines/gameinfo/` | `fw_engine_gameinfo` | Dump detection, `param.json` / `param.sfo`, versions, naming, covers, library scan, dump rename | Qt Gui (QImage) | engines/GAMEINFO.md |
| `engines/exfat/` | `fw_engine_exfat` | exFAT format, source scan, writer, reader, verify, extract, edit, jobs | gameinfo, general, pfs (AMPR index) | engines/EXFAT.md |
| `engines/ufs2/` | `fw_engine_ufs2` | FFPKG (UFS2) format, tree, writer, reader, check, verify, edit, jobs | gameinfo, general, pfs | engines/UFS2.md |
| `engines/pfs/` | `fw_engine_pfs` | PFS / FFPFSC builder, layout, codec, reader, verify, AMPR index jobs (GPL-3.0 port) | gameinfo, general | engines/PFS.md |
| `engines/fpkg/` | `fw_engine_fpkg` | FPKG builds through the user's Publishing Tools: toolchain discovery, GP5, `param.json`, PlayGo, source scanner and stagers, package header and inspection, backport update packages | general | engines/FPKG.md, PPR_COMPANION.md, `docs/FPKG.md` |
| `engines/ampr/` | `fw_engine_ampr` | zstd pack writer and reader, LZ4 pack reader, pack rules, packed games, the bundled runtime | pfs, general, zstd, LZ4 | ZSTD_PACKS.md |
| `engines/dlc/` | `fw_engine_dlc` | DLC INIs, DLC conversion through fpkg-cli, region lookup | fpkg, Qt Network | PPR_COMPANION.md |
| `engines/backport/` | `fw_engine_backport` | Read-only helpers for the Backport page's list: SELF/ELF readers (GPL-3.0 port), SDK table, metadata | — | engines/BACKPORT.md |
| `engines/tools/` | `fw_engine_tools` | Language Stripper, dlc_emu editor, dump check, problem reports | gameinfo, backport, exfat | engines/TOOLS.md |
| `engines/integration/` | `fw_engine_integration` | Cross-engine jobs: `image.convert`, `image.compare`, `image.build.zstd`, `game.extract`, FPKG image stagers, `ImageFiles` (read any image or folder), AMPR over FTP | exfat, ufs2, pfs, fpkg, general, ampr, connect/ps5 | ARCHITECTURE.md, ZSTD_PACKS.md |
| `connect/ps5/` | `fw_connect_ps5` | FTP client and transfers, payloads, kernel log, discovery, DPI install, Y2JB, ShadowMount+ / MicroMount configs, Fakelib, autoloader, crash dumps, console games | Qt Network, Sql, gameinfo | engines/PS5_CONNECT.md |
| `connect/update/` | `fw_connect_update` | Release check and verified download from GitHub | Qt Network | — |
| `connect/notify/` | `fw_connect_notify` | Discord webhook message when the queue finishes | Qt Network | — |
| `registration/` | `fw_registration` | `registerAllModules()`, `registeredModuleNames()` | every module | ARCHITECTURE.md |
| `app/` | `fw_app_lib`, `fw_app` | The Qt Widgets application; `fw_app_lib` is everything but `main.cpp`, so tests can drive real pages | registration, Qt Widgets, Svg, Network | UI.md, DESIGN_V5.md |
| `onefile/` | `fw_onefile` (`fw_onefile_stub.exe`), `fw_onefile_pack` | The single-file launcher (plain Win32, static runtime) and the packer that appends the deployed folder to it; Windows only | bcrypt, cabinet | — |

### Inside `src/app/`

| Path | Owns |
|---|---|
| `main.cpp` | Command line, paths, language, single-instance lock, crash handler, `AppContext`, `MainWindow` |
| `AppContext.*` | Settings store, schema, dependency registry, job factory, queue, history, log buffer |
| `MainWindow.*` | The shell: pages (`addPage`), sidebar, header chips, status bar, toasts, after-queue countdown, update banner, `screenshotAllPages` |
| `pages/` | One class per destination: Home, Build image, Image tools, Backport, DLC, Queue, PS5 connect (`pages/ps5/` tabs), Library, Logs, Settings, About; sheets (compare result, hex preview, dump check) |
| `ui/` | The design system: `Theme` (all tokens and the generated style sheet), `Style`, `Widgets`, `Components`, `Shell`, `Sheet`, `SettingRow`, `DependencyRow`, `JobRows`, `JobMonitor`, `LogView`, `LibraryGrid`, `Icons`, `HomeKit`, `Motion`, `Accessible` |
| `services/` | `JobSubmit` (the only path from pages to the queue; kinds and parameters listed in the header), `ConsoleSession`, `GameInfo`, `ImageInfo`, `ImageTree`, `HexPreview`, `AppStores`, `SetupChecklist`, `Language`, `LauncherIdentity`, `PortableMode` |
| `models/` | `JobQueueModel`, `LogModel` / `LogFilterModel` |
| `backport/` | `BackportHost` (finds and runs the engine, its folders and environment), `BackportCore` (the window-side logic of the original program) |
| `resources/icons/` | Lucide SVG icons (`icons.qrc`) |
| `resources/lang/` | Language catalogues (`<code>.json`, `lang.qrc`) and the generated `_source.json` |
| `resources/app/`, `windows/` | App icons, `app.rc.in`, `app.manifest` |

## Tests (`native/tests/`)

| Path | What it is |
|---|---|
| `CMakeLists.txt` | One Qt Test executable per `tst_*.cpp` (names unique across the tree), CTest registration with isolated folders, resource locks, `extra.cmake` includes |
| `support/` | `fw_test_support_core` / `fw_test_support` (`isolateAppData`, files, processes), `fw_test_helper` process, `I18nScan.h`, `ProgressAudit.h`, `SparseFile.h`, `ClosedPort.h` |
| `core/`, `app/`, `app/buildpage/`, `onefile/` | Tests that link every module (core and queue, UI offscreen, the launcher) |
| `engines/<name>/`, `connect/<name>/` | Tests that link only that module |
| `engines/fpkg/fake_pubcmd.cpp` | The stand-in for `prospero-pub-cmd.exe` (target `fw_fake_pubcmd`) |
| `connect/ps5/FakePs5.h` | In-process fake console services (`fake::FakeFtpServer` and others) |
| `engines/BenchSettings.h` | Performance settings for benchmarks from `FW_BENCH_*` |
| `data/` | Fixtures and the scripts that made them (`*.py`, `make_*`); byte for byte (`-text` in `.gitattributes`) |
| `lsan.supp` | LeakSanitizer suppressions for the Linux workflow |

Details: [TESTING.md](TESTING.md).

## Packaging

| File | What it does |
|---|---|
| `packaging/make-release.ps1` | Release build on a Windows PC: configure, build, CTest, deploy, pin check, one-file exe, smoke test, optional folder zip, `SHA256SUMS.txt` |
| `packaging/make-onefile.ps1` | Packs a deployed folder into the one-file exe and verifies it |
| `packaging/finish-deploy.ps1` | Puts the VC++ runtime DLLs next to the program and removes Qt plugins the app never loads |
| `packaging/private-files.ps1` | `Find-PrivateFiles`: what only a private build may hold |
| `packaging/pc-acceptance.ps1` | Full PC acceptance run into an evidence folder (no console, no Sony tools) |
| `packaging/installer.iss` | Inno Setup script (CI artefact only; not part of a release) |
| `packaging/LICENCE.txt`, `THIRD_PARTY_NOTICES-native.md`, `licenses/`, `README-portable.txt` | Shipped next to the program |

Release steps: [RELEASE_CHECKLIST.md](RELEASE_CHECKLIST.md).

## Documents

| Document | Kept current | Subject |
|---|---|---|
| ARCHITECTURE.md | yes | Decisions, layout, module contract, job and UI rules, test switches, data locations |
| UI.md, DESIGN_V5.md | yes | Pages, components, screenshot mode, job states |
| I18N.md | yes | Languages, `_source.json`, right-to-left |
| PERFORMANCE.md | yes | Measurements and how to reproduce them |
| ZSTD_PACKS.md, PPR_COMPANION.md | yes | zstd packs; backport updates and DLC |
| engines/*.md | yes | One per engine |
| REPOSITORY_MAP.md, TESTING.md, TROUBLESHOOTING.md, RELEASE_CHECKLIST.md, DEPENDENCIES.md | yes | This set |
| LEGACY_INVENTORY.md, LEGACY_UI_INVENTORY.md, MIGRATION_MATRIX.md, BACKPORT_PARITY.md, FPKG_PARITY.md, DESIGN_PROPOSAL.md | no | Records of the v5 rewrite; they describe the removed Python app or plans of that time |
