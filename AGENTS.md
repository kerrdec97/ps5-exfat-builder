# AGENTS.md: working on exFAT Image Builder 5

Read this first, then `native/docs/ARCHITECTURE.md`. Everything here describes
the code as it is in this repository; the deeper documents are linked.

## What the project is

A native C++20 / Qt 6 Widgets Windows application (one exe) for PS5 game dumps
the user owns. It builds, verifies, extracts, converts and edits game images
(exFAT, FFPKG/UFS2, FFPFSC and FFPFS/PFS), writes "packed folders" (game files
in zstd packs), builds installable FPKG packages and backport update packages
with the user's own Sony Publishing Tools, builds DLC packages with the user's
own fpkg-cli, backports games with the bundled PS5 Backport engine (a separate
program), and talks to a PS5 over the network (FTP, payloads, kernel log,
ShadowMount+, Y2JB). The Python application of versions 1 to 4 is no longer in
the repository (its tags remain; old documents are in `docs/history/`).

The app is closed source (`native/packaging/LICENCE.txt`). Some parts are
ports of GPL-3.0 code (PFS engine, AMPR index, fake-SELF reader, zstd pack
writer); see `native/docs/DEPENDENCIES.md`. Never copy GPL or other copyleft
code into the app: describe the idea and its licence instead.

## Repository map

Full map: `native/docs/REPOSITORY_MAP.md`. In short:

| Path | Owns |
|---|---|
| `native/CMakeLists.txt`, `native/cmake/` | Project, options, pinned third-party sources, bundled engines (`AmprRuntime.cmake`, `BackportEngine.cmake`, `ArchiveLibs.cmake`) |
| `native/src/core/` (`fw_core`) | `Result`/`Status`, cancellation, progress, log, settings, paths, file utilities (`PartFile`), process runner, archives, i18n (`Tr.h`), power actions |
| `native/src/jobs/` (`fw_jobs`) | `Job`, `JobFactory`, `JobQueue`, `JobHistory`, `ModuleContext` |
| `native/src/engines/<name>/` | One static library per feature: `general`, `gameinfo`, `exfat`, `ufs2`, `pfs`, `fpkg`, `ampr`, `dlc`, `backport` (read-only helpers), `tools`, `integration` (cross-engine jobs) |
| `native/src/connect/{ps5,update,notify}/` | PS5 console services, release check/download, Discord webhook |
| `native/src/registration/` | `registerAllModules()`: the one place that calls every module |
| `native/src/app/` | The Qt Widgets app: `main.cpp`, `MainWindow`, `AppContext`, `pages/`, `ui/` (design system), `services/`, `backport/` (engine host), `resources/` (icons, languages) |
| `native/src/onefile/` | The single-file launcher stub and its packer (plain Win32) |
| `native/tests/` | One Qt Test program per `tst_*.cpp`, fakes, fixtures (`tests/data/**`) |
| `native/packaging/` | Release scripts, notices, licence, private-file guard |
| `native/docs/` | Architecture, UI, i18n, engines; records of the v5 rewrite |
| `.github/workflows/` | Windows, Linux (sanitizers) and release workflows, started by hand or by a `v5.*` tag |

## How the app starts

1. A release is `exFAT Image Builder.exe`, the one-file launcher
   (`src/onefile/stub_main.cpp`): it unpacks the deployed program once to
   `%LOCALAPPDATA%\kerrdec97\exFAT Image Builder\runtime\` (beside the exe in
   portable mode) and starts it with the same command line, setting
   `FW_LAUNCHER_DIR` / `FW_LAUNCHER_EXE`. The folder edition skips this step.
2. `src/app/main.cpp`: command line (`--self-test`, `--screenshot-pages
   <dir>`, `--quit-after <ms>`, `--version`), `Paths::initialize()`
   (`FW_DATA_DIR`, `portable.txt`), language (`FW_LANG` or the stored
   `general.language`), the single-instance lock `app.lock` in the data
   folder, crash handler, theme.
3. `AppContext::initialize()` builds the settings schema, the dependency
   registry and the job factory by calling `fw::registerAllModules()`, loads
   settings, starts logging and restores the queue (`--self-test` uses a
   read-only mode, prints JSON and exits).
4. `MainWindow` creates the pages (`addPage(...)` in `MainWindow.cpp`).

## How the formats work (one paragraph each)

* **exFAT** (`src/engines/exfat`, `native/docs/engines/EXFAT.md`): written from
  the public Microsoft exFAT specification; a bare volume (no partition
  table), 64 KiB clusters by default for ShadowMount+, planned then written in
  one sequential pass, read back and verified before it gets its name.
* **FFPKG / UFS2** (`src/engines/ufs2`, `engines/UFS2.md`): a port of the
  UFS2Tool `newfs` layout (BSD-2-Clause), byte-identical to it, with a
  read-only fsck-style check, verify, extract and edit.
* **PFS / FFPFSC / FFPFS** (`src/engines/pfs`, `engines/PFS.md`): a C++ port
  of Lazy_MkPFS / MkPFS (GPL-3.0), byte-identical to Lazy_MkPFS, streaming
  and multithreaded; `.ffpfsc` is the compressed container, `.ffpfs` plain.
  Also writes the AMPR index (`ampr_emu.index`).
* **zstd packs and packed folders** (`src/engines/ampr`,
  `native/docs/ZSTD_PACKS.md`): the game's files go into zstd packs read on the
  console by the bundled ampr-emu-ng runtime (`fakelib/libSceAmpr.sprx`); LZ4
  packs are read only. `integration` builds images from packed games.
* **FPKG packages** (`src/engines/fpkg`, `engines/FPKG.md`, user guide
  `docs/FPKG.md`): the app prepares the GP5 project, `param.json` and PlayGo
  data and drives the user's own `prospero-pub-cmd.exe` (`img_create`,
  `img_verify`, `img_extract`). Backport update packages: `BackportUpdate.*`
  (`native/docs/PPR_COMPANION.md`). Nothing from Sony is in the repository.
* **DLC** (`src/engines/dlc`): DLC INIs and DLC `.pkg` files become PS5 DLC
  packages through the user's own fpkg-cli (`PPR_COMPANION.md`).
* **Backport** (`src/app/backport`, `engines/BACKPORT.md`): the Backport page
  runs the bundled PS5 Backport engine 0.3.4 as a child process
  (`--worker job.json`, JSON over its standard streams). The app has no
  backport engine of its own.
* **Game metadata** (`engines/GAMEINFO.md`), **PS5 connect**
  (`engines/PS5_CONNECT.md`), **dump check and tools** (`engines/TOOLS.md`).

## Build, run and test on Windows

Toolchain used on this project: Visual Studio 2022 C++ x64 tools (MSVC),
Qt 6.10.3 `msvc2022_64`, CMake 3.24 or later with Ninja, CTest. The manual CI
workflow uses Qt 6.8.3. Commands (PowerShell, from the repository root,
inside a VS 2022 x64 developer environment, `VsDevCmd.bat -arch=amd64`):

```powershell
$Qt = '<Qt folder>\6.10.3\msvc2022_64'
$env:PATH = "$Qt\bin;$env:PATH"            # the test programs and the app need the Qt DLLs
cmake -S native -B <build> -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_PREFIX_PATH=$Qt" -DFW_FETCH_BACKPORT_ENGINE=ON
cmake --build <build> --parallel
$env:FW_OFFLINE = '1'
ctest --test-dir <build> --output-on-failure --timeout 900
```

Put `<build>` outside the repository or under `native/build*/` (git-ignored).
A first build takes about ten minutes. One test: `ctest --test-dir <build> -R
"^tst_exfat_format$" --output-on-failure`. Run a test program yourself with
`<build>\tests\tst_x.exe -o out.txt,txt`: Qt Test prints nothing to a
redirected stdout. Matrix and switches: `native/docs/TESTING.md`. Building
without the network (`FETCHCONTENT_SOURCE_DIR_*`, local engine copies):
`native/docs/DEPENDENCIES.md`.

**Environment switches.** CTest sets these for every test; any run of the app
or of a test program outside CTest must set them too:

| Variable | Effect |
|---|---|
| `FW_DATA_DIR=<tmp>` | The app's settings, queue, history, logs and temp go to `<tmp>\data` and `<tmp>\local` instead of the user's folders |
| `D97BETA_CONFIG_DIR`, `D97BETA_DATA_DIR`, `D97BETA_ENGINE_HOME` | Move the PS5 Backport engine's own files (shared with the user's standalone copy of that program) |
| `FW_OFFLINE=1` | No update check, no Discord message, no online DLC lookup |
| `FW_NO_POWER_ACTIONS=1` | Sleep and shut down after the queue are refused (also refused in any `tst_*` program) |
| `QT_QPA_PLATFORM=offscreen` | UI tests without a window (CTest sets it) |
| `FW_LANG=<code>` / `qps` | Interface language; `qps` shows every translated text in brackets |
| `FW_I18N_UPDATE=1` | `tst_i18n_source` rewrites `src/app/resources/lang/_source.json` |

Screenshot mode: `"exFAT Image Builder.exe" --screenshot-pages <dir>` renders
every page and demo state to PNG with specimen data and touches no user files
(`native/docs/UI.md` section 5). Self-test: `--self-test` prints a JSON
report of dependencies, settings count and job kinds.

## Never modify

* The bundled engine pins in `native/cmake/BackportEngine.cmake` and
  `native/cmake/AmprRuntime.cmake` (version, URL, SHA-256), and the bundled
  files themselves. Changing a pin is the owner's decision.
* The PS5 Backport GUI project and its worktrees, game dumps, working
  backports and crash-dump folders outside this repository: read-only
  reference material. Never run git commands that write there.
* Users' data folders: `%APPDATA%` and `%LOCALAPPDATA%\kerrdec97\...`,
  `%APPDATA%` and `%LOCALAPPDATA%\Deckerr97BackportGuiBeta`, the engine home
  (`D97B`), the one-file cache. Use the switches above instead.
* `native/tests/data/**` fixtures (compared byte for byte; regenerate them
  only with their own scripts), the generated `_source.json` (never by hand),
  the `<!-- public-edition -->` markers in
  `native/packaging/THIRD_PARTY_NOTICES-native.md` (make-release needs them).
* Never add Sony tools, fpkg-cli, YouTube packages, commercial game data or
  private build output (`dist-private/`, `tools/fpkg-toolkit`,
  `tools/fpkg-cli`, `tools/youtube`) to the repository or a public build.

## How to add things

* **A format or engine**: a folder `native/src/engines/<name>/` with a
  one-line `CMakeLists.txt` (`fw_add_module(fw_engine_<name> DEPENDS ...)`;
  sources are globbed), a `Module.h` declaring
  `void fw::<name>::registerModule(fw::ModuleContext&)`, a call in
  `src/registration/Registration.cpp` (and its name in
  `registeredModuleNames()`), tests in `native/tests/engines/<name>/`, a
  document in `native/docs/engines/`.
* **A job kind**: a `Job` subclass (`jobs/Job.h`) with `kind()`, `title()`,
  `parameters()` (enough JSON to re-create it), cheap `validate()`, `run()`
  on a worker thread with `phases()` / `phaseWeights()`; register it with
  `ctx.jobs.add(kind, displayName, creator)`; the page submits it through
  `app/services/JobSubmit.h` (kinds and parameters listed there). Follow the
  job rules in `ARCHITECTURE.md` (summary under "Regression rules").
* **A setting**: `ctx.settings.add(SettingDef{...})` in the owning module, key
  `<module>.<camelCase>`, a section from `SettingsSchema::sectionOrder()`,
  sentence-case label and a description; the module must read it (no control
  without a consumer). The Settings page and job options are generated.
* **A dependency probe**: `ctx.dependencies.add(id, name, purpose, probe)`;
  a probe never downloads and returns a `howToFix`.
* **A page**: a `pages::Page` (or `JobFormPage`) subclass in
  `src/app/pages/`, added in `MainWindow` with `addPage(...)`, styled only
  through `ui/Theme` tokens and `ui/Widgets`; give it demo states for
  `--screenshot-pages`; tests in `native/tests/app/` (`native/docs/UI.md`).
* **A text or translation**: user-visible text goes through `FW_TR("...")`
  (British spelling, "…" as one character). Then run `tst_i18n_source` with
  `FW_I18N_UPDATE=1` and commit `_source.json`. A language: add
  `src/app/resources/lang/<code>.json`, list it in `lang.qrc`
  (`native/docs/I18N.md`).

## Diagnosing failures

`native/docs/TROUBLESHOOTING.md` has the list. First steps: the job's log
(Logs page, or `<FW_DATA_DIR>\local\logs\*.log` / `*.jsonl`),
`--self-test`, the failing test run alone with `-o file,txt`, crash reports
in `<logs>\crashes`, `FW_ONEFILE_DEBUG=1` for the launcher.

## Mock versus console evidence

Most tests run against stand-ins: the fake publisher
(`tests/engines/fpkg/fake_pubcmd.cpp`), the fake PS5 services
(`tests/connect/ps5/FakePs5.h`, `fake::FakeFtpServer`), synthetic dumps. A
result from them is **MOCK** and is never evidence of console behaviour.
Anything that only a real PS5, real Sony Publishing Tools or a real game
shows is **HARDWARE REQUIRED**; say so in reports and leave it to the owner.
Never contact a PS5. Use synthetic fixtures for anything that writes.

## Regression rules

* Every behavioural fix comes with a test that fails before the fix.
* Code style as the neighbours: `fw::Result<T>` / `fw::Status` and `FW_TRY`,
  no exceptions across modules, `FW_TR` for user text.
* Job rules (`ARCHITECTURE.md` "Job rules"): `validate()` cheap and on the UI
  thread; `run()` checks `ctx.token.checkpoint()` at least every 4 MiB / 100
  files; outputs through `PartFile` (`<final>.fw.part`, then one-step
  replace), `ensureFreeSpace` with the reserve, never delete or overwrite
  user data the job did not create, never delete a source unless verification
  passed; "Done" only after verification; `renameWithoutCopying()`, never
  `QFile::rename()`; intermediate files in `ctx.tempDir`; a failure that kept
  work under another name is not an `IoError`.
* The UI never blocks: disk and tool work goes through the queue or
  `QtConcurrent::run`.

## Git and release operations

An agent may: work on a feature branch or its own worktree, make small
descriptive commits, run builds and tests, prepare release notes and
checklists. An agent may not, without the owner: push (above all to `main`),
merge into `main`, create tags or GitHub releases (a `v5.*` tag starts the
release workflow), publish a build, change pins, or run `git stash` on a
shared checkout. Private builds (`make-release.ps1 -PrivateToolkit`,
`-PrivateDlcEngine`, `-PrivateYoutube`) and their files never go into the
repository or a public download. No UAC prompts, installers, system settings
or Defender exclusions. Release steps: `native/docs/RELEASE_CHECKLIST.md`;
contribution rules: `CONTRIBUTING.md`.
