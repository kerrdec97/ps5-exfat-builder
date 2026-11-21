# Troubleshooting (developers)

What a failure looks like and where to look first. The messages quoted are
the ones in the code and scripts. Users' problems with packages are covered
in `docs/FPKG.md`; this page is about building, testing and diagnosing.

## Where to look

| What | Where |
|---|---|
| A job's log | Logs page (filter by job), or the log files: `<local>\logs\*.log` (text) and `*.jsonl` (JSON lines). `<local>` is `%LOCALAPPDATA%\kerrdec97\exFAT Image Builder Farewell`, `<exe>\data` in portable mode, `<FW_DATA_DIR>\local` with the override; Settings › Logging can move the folder |
| A job's result details | Queue page › the job; `JobOutcome::details` (for example the `image.compare` result sheet) |
| Settings, queue, history | `<data>\settings.json`, `queue.json`, `history.jsonl` (`<data>` = `%APPDATA%\kerrdec97\exFAT Image Builder Farewell`, or `<FW_DATA_DIR>\data`) |
| Crashes | `<logs>\crashes`: a minidump and a short text report (Windows); the next start offers them for a problem report |
| Dependencies | `"exFAT Image Builder.exe" --self-test` prints JSON: version, every probe (status, version, path, how to fix), settings count, job kinds. Settings › Tools shows the same |
| The single-file launcher | `FW_ONEFILE_DEBUG=1` prints `fw-onefile: ...` trace lines on stderr; `FW_ONEFILE_NO_UI=1` turns message boxes into stderr text |
| The PS5 Backport engine | Its own files, in the folders `D97BETA_CONFIG_DIR` / `D97BETA_DATA_DIR` name (by default `%APPDATA%` and `%LOCALAPPDATA%\Deckerr97BackportGuiBeta`; logs under `logs\runs`) |

Always reproduce with the isolation variables set (`FW_DATA_DIR`,
`D97BETA_*`, `FW_OFFLINE=1`, `FW_NO_POWER_ACTIONS=1`) so the user's own
folders stay untouched.

## Configure and build

| Symptom | Cause and fix |
|---|---|
| `Two test files are called tst_x.cpp ...` | Test file names are target names and must be unique under `native/tests`. Rename one |
| `The PS5 Backport engine does not match the pinned SHA-256 (...)` | `FW_BACKPORT_ENGINE_EXE` is not the pinned release (or a download was damaged; it is removed and fetched again by the next configure). Use the release file; never change the pin |
| `PS5 Backport engine: not bundled (...)` | Neither `FW_BACKPORT_ENGINE_EXE` nor `FW_FETCH_BACKPORT_ENGINE=ON` (off by default). The app builds; the Backport tab shows "Engine missing" |
| `Could not download the ampr-emu-ng runtime (...). zstd packs will be unavailable.` | No network or a changed release file. Give `FW_AMPR_RUNTIME_SPRX=<file>` (the pinned 0.3.3 release) or configure again online |
| `The ampr-emu-ng runtime does not match the pinned SHA-256` | `FW_AMPR_RUNTIME_SPRX` names another file |
| FetchContent download fails | Configure offline from local sources: `-DFETCHCONTENT_FULLY_DISCONNECTED=ON` and `FETCHCONTENT_SOURCE_DIR_<NAME>` (`DEPENDENCIES.md`) |
| `Qt6` not found | Pass `-DCMAKE_PREFIX_PATH=<Qt>\6.10.3\msvc2022_64` |
| `cl` or the Windows SDK not found | Run in a VS 2022 x64 developer environment (`VsDevCmd.bat -arch=amd64 -host_arch=amd64`). Installing Build Tools needs a UAC prompt and a licence acceptance: the owner does that |
| A new source file is not compiled | Sources are globbed with `CONFIGURE_DEPENDS`; build again (CMake re-globs). A module folder needs its own `CMakeLists.txt` |

## Tests

| Symptom | Cause and fix |
|---|---|
| A test program prints nothing | Qt Test writes nothing to a redirected stdout on Windows. Run it with `-o <file>,txt` and read the file |
| A test program exits at once with `-1073741515` (`0xC0000135`) | A DLL was not found: put `<Qt>\bin` on `PATH` |
| `tst_i18n_source`: `_source.json is not what the code shows: n new, n gone ...` | A user-visible text changed. Run that test once with `FW_I18N_UPDATE=1` and commit `src/app/resources/lang/_source.json`; never edit it by hand. A stale key in a catalogue must be removed |
| A shipped catalogue below 98 % | `tst_i18n_source` lists the missing keys; the translation is the integrator's job |
| `tst_exfat_windows` is skipped | It needs `FW_WINDOWS_MOUNT_TEST=1` and administrator rights; it runs in the Windows workflow |
| A Backport test skips with "A program named PS5BackportGUI.exe is already running" | Close the standalone program; the three Backport tests share a resource lock for the same reason |
| Timeout on a heavy test | 900 s per test. Run heavy tests alone (`ctest -L heavy`); benchmarks with large sizes need `QTEST_FUNCTION_TIMEOUT` |
| Linux: LeakSanitizer reports inside Qt or system libraries | Suppressions are in `native/tests/lsan.supp` |
| A new fixture is missing in a fresh clone | `.gitignore` ignores `*.exe`, `*.pkg`, `*.exfat`, `*.log` ... everywhere: `git add -f` it |

## Running the app

| Symptom | Cause and fix |
|---|---|
| "exFAT Image Builder is already running ..." | Another copy holds `<data>\app.lock`. A lock left by a crashed copy is taken over. Use a different `FW_DATA_DIR` for a second test copy |
| "The data folder cannot be written ..." | The data folder (portable `data\`, or `FW_DATA_DIR`) is read-only or missing |
| The Backport tab says "Engine missing" | `engines\backport\PS5BackportGUI.exe` is not next to the exe (or up to four folders above it). `FW_BACKPORT_ENGINE=<exe>` names another copy (tests) |
| zstd option says the runtime is missing | `engines\ampr-emu-ng\libSceAmpr.sprx` was not bundled (see Configure); `FW_AMPR_RUNTIME` names another file |
| FPKG: Publishing Tools not found | Settings › Tools, or `SCE_PROSPERO_PUBTOOLS`; the tools also need the VC++ 2015-2022 x64 runtime. Tests use `fw_fake_pubcmd` instead |
| DLC: fpkg-cli not found | Settings › Tools › DLC engine, or `FW_FPKG_CLI` |
| A `<name>.fw.part` file is left | An interrupted run; it is the app's own and is removed when the output is written again. A file called `<name>.part` is never touched |
| No update check or Discord message | `FW_OFFLINE` is set (as in every test) |
| The computer did not sleep or shut down after the queue | `FW_NO_POWER_ACTIONS` is set, or the program is a test |

## Single-file launcher exit codes

| Code | Meaning (`src/onefile/stub_main.cpp`) |
|---|---|
| 9101 | The exe is damaged, carries no program, or its payload could not be read: download it again |
| 9102 | Unpacking failed (disk full, folder not writable) |
| 9103 | The unpacked program could not be started |
| 9104 | Unpacking was cancelled |

Any other code is the program's own.

## Release scripts

| Message | Cause |
|---|---|
| `Qt was not found: pass -QtDir <...\msvc2022_64>, set QT_ROOT_DIR, or put windeployqt on PATH.` | `make-release.ps1` has no Qt |
| `No Visual Studio / Build Tools (vswhere.exe missing).` | No MSVC; the owner installs it |
| `Close the running app first: ... is in use.` | A running copy holds the output exe |
| `The PS5 Backport engine is missing from ...` / `... is not the pinned release ...` | The build tree lacks the bundled engines or holds other files; configure and build again without `-SkipBuild` |
| `A public build must not hold private files: ...` | Private tools found in the deploy folder of a public build. Remove them; never publish that folder |
| `CTest failed (n)` | Fix the failing test; `-SkipTests` is not for releases |
| `Self-test (...) failed` / `did not finish within 120 s` | The packed or deployed exe does not start cleanly; run it by hand with `FW_ONEFILE_DEBUG=1` |

## Mock or console?

When a failure only shows on a PS5 or with real Sony tools, say so: a
passing test against `FakePs5.h` or `fw_fake_pubcmd` is MOCK evidence. Report
the console-only part as HARDWARE REQUIRED and leave it to the owner.
