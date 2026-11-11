# exFAT Image Builder — native C++ architecture

The native rebuild of the PS5 exFAT Image Builder. C++20, Qt 6 (Widgets for
the UI, Core/Network/Concurrent for the backend), CMake + Ninja, MSVC 2022 on
Windows (release), GCC/Clang on Linux (development and CI tests). It replaced
the Python application of versions 1 to 4, which is no longer in the
repository.

## Decisions

| Decision | Choice | Why |
|---|---|---|
| GUI framework | **Qt 6 Widgets** + QSS + custom-painted widgets | Native text rendering, high-DPI, accessibility, tables/dialogs/file pickers, mature on Windows; QSS + `QStyle` painting reproduce the design system exactly. Dear ImGui was rejected: immediate-mode, no accessibility, reads as a developer tool. Qt Quick was rejected for now: heavier deployment and the design system has no motion to justify it. |
| Backend dependency | Qt Core only (no Widgets) | Clean UI/backend split; QProcess, JSON, file APIs and threads are shared by every engine and testable headless. |
| Error handling | `fw::Result<T>` / `fw::Status`, no exceptions across modules | Explicit failure paths; user message + technical detail separated. The queue catches stray exceptions as a last resort. |
| Concurrency | One worker thread per running job (dedicated `QThreadPool`); engines may use internal worker pools sized by `performance.workerThreads` | Disk-bound jobs run one at a time by default; CPU-bound stages (compression, hashing) scale with cores. |
| Cancellation | Cooperative `CancellationToken` with pause; process trees killed via Windows job objects (+ `taskkill /T`) / POSIX process groups | Audit H3/H4: nothing keeps running after cancel or close. |
| Outputs | Always `<final>.part` then rename (`PartFile`); an existing output is set aside and restored on failure | Audit H6, M2. |
| Settings | Schema declared by the consuming module; Settings page generated from the schema; immutable per-job snapshots with validated per-job overrides | "Every setting must be functional": no control without a consumer. |
| Tests | Qt Test executables, one per `tests/**/tst_*.cpp`, registered with CTest | Ships with Qt, no extra dependency. |
| Licence | Proprietary from 5.0.1 (Copyright © 2026 Deckerr97, all rights reserved; `packaging/LICENCE.txt`) | Third-party components keep their own licences (`packaging/THIRD_PARTY_NOTICES-native.md`). Owner's note: the PFS engine and the AMPR index (Lazy_MkPFS / MkPFS) and the fake-SELF reader (make_fself) are ports of GPL-3.0 code, and the zstd pack writer follows ps5-dump-forge (GPL-3.0-or-later); their authors' terms still apply to those parts. Other programs' behaviour is reimplemented, not copied. The published build bundles no Sony tools. |

## Layout

```
native/
  CMakeLists.txt            project, options, Qt/zlib discovery
  cmake/FwWarnings.cmake    fw_configure_target(), fw_add_module()
  cmake/AmprRuntime.cmake   the bundled ampr-emu-ng runtime (pinned by SHA-256)
  cmake/BackportEngine.cmake  the bundled PS5 Backport engine (pinned by SHA-256)
  src/
    core/                   fw_core     — Result, Cancellation, Progress, Log, Settings, Paths,
                                          FileUtil, PathSafety, ProcessRunner, Dependencies
    jobs/                   fw_jobs     — Job, JobFactory, JobQueue, JobHistory, ModuleContext
    engines/<name>/         fw_engine_<name> — one static library per feature module
      general/              app-wide settings (General, Performance, Storage, Verification, Logging, Advanced)
      gameinfo/             dump detection, param.json/param.sfo, metadata, covers, library scan, dump rename
      exfat/                native exFAT image writer/reader/verify/extract/edit
      pfs/                  PFS / FFPFSC compressed image builder/reader/verify (GPL port), AMPR index
      ufs2/                 FFPKG (UFS2) images (port of UFS2Tool newfs), reader/verify/extract/edit
      fpkg/                 FPKG packages through the user's Publishing Tools
      backport/             read-only helpers for the Backport page's game list (SELF reader, SDK table,
                                          metadata); backports are made by the bundled PS5 Backport engine
      ampr/                 zstd asset packs (write, read, unpack), LZ4 pack reader, the bundled runtime
      dlc/                  DLC packages through the user's fpkg-cli, dlc_emu INIs, key lookup
      tools/                Language Stripper, DLC-emu editor, problem reports
      integration/          cross-engine workflows: image.convert, image.compare (ImageFiles: read-only
                                          file access to any image or folder), FPKG image stagers, post-build upload
    connect/ps5/            fw_connect_ps5 — FTP, payload sender, kernel log, DPI/Y2JB, mount configs
    connect/update/         fw_connect_update — release check and verified download
    connect/notify/         fw_connect_notify — Discord webhook message when the queue finishes
    registration/           calls every module's registerModule()
    app/                    the Qt Widgets application (theme, widgets, pages, main.cpp)
      backport/             host of the bundled PS5 Backport engine (worker process, settings, ledger)
    onefile/                the single-file launcher (plain Win32) and its packer
  tests/
    support/                fw_test_support, fw_test_helper process
    core/                   core + queue tests
    engines/<name>/         module tests (tst_*.cpp)
    connect/<name>/         connection module tests, with the fake PS5
    app/                    UI tests (offscreen)
    onefile/                launcher and packer tests
  docs/                     architecture, UI, design, engine documents; records of the v5 rewrite
```

Every `src/engines/*/CMakeLists.txt` is a single `fw_add_module(<target> DEPENDS ...)`
line; sources are globbed, and the module adds itself to the app and the tests.
Nobody edits a shared source list. The only shared file is
`src/registration/Registration.cpp`, which already calls every planned module.

## Module contract

Each module exposes `void fw::<name>::registerModule(fw::ModuleContext&)` in
`Module.h` and registers:

1. **Settings** (`ctx.settings.add(SettingDef{...})`): key `<module>.<camelCase>`,
   the section it appears in, sentence-case label, a description that says what
   it does and when to change it, type/default/range/choices,
   `perJob` if a job may override it, `enabledWhenKey` for dependent settings,
   `dependencyId` if it needs a tool. Legacy keys map with `addLegacyKey()`.
   The module must read every setting it declares.
2. **Dependency probes** (`ctx.dependencies.add(id, name, purpose, probe)`):
   a probe never downloads anything and returns status, version, path, detail
   and a user-facing `howToFix`.
3. **Job kinds** (`ctx.jobs.add(kind, displayName, creator)`): `creator` builds a
   job from JSON parameters (the same JSON `Job::parameters()` returns), so the
   queue can persist, restore and retry it.

## Job rules

* `validate()` is cheap (no tree walks) and runs on the UI thread.
* `run()` runs on a worker thread: declare `phases()`, call
  `ctx.progress->beginPhase(i, totalBytes, totalFiles)`, `advance()` from any
  thread, `ctx.token.checkpoint()` at least every 4 MiB / 100 files, log through
  `ctx.log` (`info` for milestones, `debug` for command lines and timings).
* Check free space (`ensureFreeSpace`) with the `storage.freeSpaceMarginMiB`
  reserve before writing.
* Write outputs through `PartFile`; never delete or overwrite user data that the
  job did not create; never delete a source unless verification passed.
* Verification result goes in `JobOutcome::verification`; `Failed` makes the job
  fail. "Done" is only shown after verification (audit M1).
* Use `ctx.tempDir` (private, removed by the queue) for intermediate files.

## UI rules

* The UI never blocks: anything that touches the disk or a tool goes through the
  queue or `QtConcurrent::run` with results posted back via queued signals.
* Pages read services from `AppContext` (settings store, queue, history,
  dependencies, log buffer).
* All look-and-feel comes from the design system (`docs/DESIGN_V5.md`):
  tokens in one place, QSS generated from tokens, custom widgets for the rest.

## Dependencies by module

| Target | Qt modules | Why |
|---|---|---|
| core, jobs, most engines | Core, Concurrent (+ zlib) | — |
| gameinfo | + Gui (QImage only) | cover normalisation; no widgets, no GUI application object |
| connect/ps5 | + Network, Sql | sockets; SQLite edits of the console's app.db/appinfo.db (ships `sqldrivers/qsqlite.dll`) |
| connect/update | + Network | HTTPS to GitHub |
| connect/notify | + Network | HTTPS POST to a Discord webhook |
| app | + Gui, Widgets, Svg, Network | the interface |

Qt sockets only work on threads Qt started (the queue's `QThreadPool`,
`QThread`, `QtConcurrent`); never use them from `std::thread`.

## Archives as build sources

`core/ArchiveSource.h`: a build job's `source` may be a `.zip`, `.7z` or `.rar` of a dump (the first bytes decide
the reader, not the name). `validate()` lists the archive (nothing is unpacked) and checks it holds exactly one
game; `run()` unpacks only the game into `<tempDir>/zip-source` in one pass, after a free-space check from the listed
sizes, with every checksum verified and file times kept, then builds from that folder. ZIP uses the app's own reader
(`core/Zip.*`); 7z and RAR use libarchive 3.8.11 with liblzma 5.8.4 (`core/ArchiveReader.*`,
`cmake/ArchiveLibs.cmake`: only the 7z, RAR and RAR5 readers are compiled, with the hand-written configuration in
`cmake/libarchive/`, so the app's configure runs no libarchive platform checks). A 7z or RAR list is kept while the
file is unchanged; `param.json` is read from it only within a budget (256 MiB, 5 s), else the game is shown without
it. Refused, each with one sentence: password-protected archives, solid RAR 4, RAR 5 dictionaries over 64 MiB,
unsupported methods (named), a later part of a multi-part RAR (the first part is named) or a missing part, damaged or
cut-short archives (the entry is named), unsafe or Windows-unstorable names, names that differ only in case, more than
2,000,000 entries, a 7z list of files over 64 MiB, a file larger than listed. Links are counted and never created.
Tests: `tests/core/tst_archive_source.cpp` (7z made with libarchive's writer, linked into that test only; fixtures in
`tests/data/archives` made by hand with 7-Zip, see `make_archive_fixtures.py`; RAR from libarchive's own samples).

## Testing switches

`FW_DATA_DIR` isolates the app's own data (tests, CI). The Backport page also
uses the files of the bundled PS5 Backport engine, which `D97BETA_CONFIG_DIR`
and `D97BETA_DATA_DIR` move; CTest points both into the build folder, and a
test that shows the page sets them to its own temporary folders. `FW_OFFLINE=1` stops the
silent update check and the Discord webhook (set for every test and CI run). Module tests link only
their own module (`tests/engines/<m>/`, `tests/connect/<m>/`); helper
executables come from a module's `tests/.../extra.cmake`.

## Data locations

`Paths`: settings, history and queue in `%APPDATA%\kerrdec97\exFAT Image Builder Farewell`;
logs, temp and caches in `%LOCALAPPDATA%\...`; portable mode with
`portable.txt` next to the exe (`<exe>\data`); `FW_DATA_DIR` overrides both
(tests, CI). Stale `fw-*` temp folders older than a day are removed at start;
in a temporary folder the user chose, only job folders that carry the app's
marker are removed.

## Command line

`exFAT Image Builder.exe [--self-test] [--screenshot-pages <dir>] [--quit-after <ms>]`
— `--self-test` prints a JSON dependency/settings report; `--screenshot-pages`
renders every page to PNG (CI evidence); `--quit-after` for launch smoke tests.
