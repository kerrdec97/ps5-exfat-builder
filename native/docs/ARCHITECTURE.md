# Farewell Edition — native C++ architecture

The native rebuild of the PS5 exFAT Image Builder. C++20, Qt 6 (Widgets for
the UI, Core/Network/Concurrent for the backend), CMake + Ninja, MSVC 2022 on
Windows (release), GCC/Clang on Linux (development and CI tests). The legacy
Python application at the repository root is untouched and remains the
reference until the migration matrix says otherwise.

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
| Licence | GPL-3.0-or-later for the native app | The legacy app already distributes GPL-3.0 components (Lazy_MkPFS, MkPFS, ps5-payload-sdk). The PFS engine is a port of GPL-3.0 code with attribution. Code with no licence (the PSVIETHOA FPKG Builder, the Backport GUI checkout) is never copied — behaviour is reimplemented. Sony tools are never bundled. |

## Layout

```
native/
  CMakeLists.txt            project, options, Qt/zlib discovery
  cmake/FwWarnings.cmake    fw_configure_target(), fw_add_module()
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
      backport/             Backport engine (scan, planner, patcher, ZIPs, journaled apply, restore)
      tools/                Language Stripper, DLC-emu editor, problem reports
      integration/          cross-engine workflows: image.convert, FPKG image stagers, post-build upload
    connect/ps5/            fw_connect_ps5 — FTP, payload sender, kernel log, DPI/Y2JB, mount configs
    connect/update/         fw_connect_update — release check and verified download
    connect/notify/         fw_connect_notify — Discord webhook message when the queue finishes
    registration/           calls every module's registerModule()
    app/                    the Qt Widgets application (theme, widgets, pages, main.cpp)
  tests/
    support/                fw_test_support, fw_test_helper process
    core/                   core + queue tests
    engines/<name>/         module tests (tst_*.cpp)
  docs/                     architecture, parity matrices, inventories, design proposal
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
* All look-and-feel comes from the design system (`docs/DESIGN_PROPOSAL.md`):
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

## Testing switches

`FW_DATA_DIR` isolates all app data (tests, CI). `FW_OFFLINE=1` stops the
silent update check and the Discord webhook (set for every test and CI run). Module tests link only
their own module (`tests/engines/<m>/`, `tests/connect/<m>/`); helper
executables come from a module's `tests/.../extra.cmake`.

## Data locations

`Paths`: settings, history and queue in `%APPDATA%\kerrdec97\exFAT Image Builder Farewell`;
logs, temp and caches in `%LOCALAPPDATA%\...`; portable mode with
`portable.txt` next to the exe (`<exe>\data`); `FW_DATA_DIR` overrides both
(tests, CI). Stale `fw-*` temp folders older than a day are removed at start.

## Command line

`exFAT Image Builder.exe [--self-test] [--screenshot-pages <dir>] [--quit-after <ms>]`
— `--self-test` prints a JSON dependency/settings report; `--screenshot-pages`
renders every page to PNG (CI evidence); `--quit-after` for launch smoke tests.
