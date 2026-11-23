# Testing

Every `native/tests/**/tst_*.cpp` is one Qt Test program, registered with
CTest under its file name (`native/tests/CMakeLists.txt`). There were 78
programs when this page was written; `ctest --test-dir <build> -N` lists the
current set. Tests are built with the default `FW_BUILD_TESTS=ON`.

## What CTest sets for every test

Each test gets its own folders under `<build>/tests/isolated/<test>/` and this
environment (`set_tests_properties ... ENVIRONMENT` in `tests/CMakeLists.txt`):

| Variable | Value |
|---|---|
| `QT_QPA_PLATFORM` | `offscreen` |
| `QT_FORCE_STDERR_LOGGING` | `1` (otherwise Qt Test on Windows may send its report to the debugger channel and CTest shows nothing) |
| `FW_OFFLINE`, `FW_NO_POWER_ACTIONS` | `1` |
| `FW_AMPR_RUNTIME` | the bundled `libSceAmpr.sprx` of this build (empty when it is not bundled) |
| `FW_DATA_DIR` | `<isolated>/appdata` |
| `D97BETA_CONFIG_DIR`, `D97BETA_DATA_DIR`, `D97BETA_ENGINE_HOME` | `<isolated>/backport-config`, `backport-data`, `backport-engine-home` |

The timeout is 900 s per test. `fwtest::isolateAppData(QTemporaryDir&)`
(`tests/support/TestSupport.cpp`) is the first line of defence inside a test:
it points `Paths` and the three `D97BETA_*` folders at the test's own
temporary folder and sets `FW_OFFLINE` and `FW_NO_POWER_ACTIONS`. Use it in
every test that touches settings, the queue or the Backport page.
`power::perform` never acts in a `tst_*` program; tests install
`power::setPerformHook` to see what would have happened.

## Running

```powershell
# all tests (Qt bin folder on PATH, inside the build's developer environment)
$env:FW_OFFLINE = '1'
ctest --test-dir <build> --output-on-failure --timeout 900

# one test
ctest --test-dir <build> -R "^tst_ufs2_format$" --output-on-failure

# the heavy ones only, or everything but them
ctest --test-dir <build> -L heavy
ctest --test-dir <build> -LE heavy -j 4
```

Running a test program directly (debugger, one function, verbose output):

```powershell
$t = Join-Path $env:TEMP ('fw-test-' + [guid]::NewGuid().ToString('N').Substring(0,8))
$env:FW_DATA_DIR = "$t\appdata"; $env:D97BETA_CONFIG_DIR = "$t\bp-config"
$env:D97BETA_DATA_DIR = "$t\bp-data"; $env:D97BETA_ENGINE_HOME = "$t\bp-home"
$env:FW_OFFLINE = '1'; $env:FW_NO_POWER_ACTIONS = '1'; $env:QT_QPA_PLATFORM = 'offscreen'
& "<build>\tests\tst_exfat_format.exe" -o "$t\result.txt,txt" -maxwarnings 0   # all functions
& "<build>\tests\tst_exfat_format.exe" someFunction -o "$t\one.txt,txt" -v2   # one function
Get-Content "$t\result.txt"
```

**Qt Test and redirected output:** a test program writes nothing to a
redirected or piped stdout on Windows. Always give `-o <file>,txt` (or
`-o -,txt` with `QT_FORCE_STDERR_LOGGING=1`, as CTest does) and read the file.
Test programs find the bundled ampr-emu-ng runtime next to themselves
(`<build>/tests/engines/ampr-emu-ng/libSceAmpr.sprx`, copied by the build);
`FW_AMPR_RUNTIME` names another file.

## Test matrix by area

| Area | Programs | Links |
|---|---|---|
| Core, queue, settings | `tst_core`, `tst_core_extra`, `tst_settings_jobs`, `tst_legacy_import`, `tst_paths_launcher` | all modules |
| Archives as sources | `tst_zip`, `tst_archive_source` (7z made with libarchive's writer, RAR samples from libarchive's sources) | all modules |
| Languages | `tst_i18n` (mechanism), `tst_i18n_source` (the source list and every catalogue) | all modules |
| exFAT | `tst_exfat_format`, `tst_exfat_scan`, `tst_exfat_roundtrip` (heavy), `tst_exfat_edit`, `tst_exfat_jobs`, `tst_exfat_hostile`, `tst_exfat_ampr`, `tst_exfat_bench` (heavy), `tst_exfat_windows` (opt-in) | `fw_engine_exfat` |
| FFPKG (UFS2) | `tst_ufs2_format`, `tst_ufs2_roundtrip` (heavy), `tst_ufs2_jobs`, `tst_ufs2_hostile`, `tst_ufs2_ampr`, `tst_ufs2_reference`, `tst_ufs2_compat` (opt-in tools) | `fw_engine_ufs2` |
| PFS / FFPFSC | `tst_pfs_golden`, `tst_pfs_roundtrip`, `tst_pfs_pipeline`, `tst_pfs_jobs`, `tst_pfs_hostile`, `tst_pfs_ampr_index`, `tst_pfs_benchmark` (heavy) | `fw_engine_pfs` |
| FPKG | `tst_fpkg_rules` (GP5, scenario and `param.json` goldens), `tst_fpkg_build` (end to end with the fake publisher), `tst_backport_update` | `fw_engine_fpkg` + `fw_fake_pubcmd` |
| zstd / LZ4 packs | `tst_ampr` | `fw_engine_ampr` |
| Cross-engine | `tst_integration`, `tst_integration_safety`, `tst_image_compare`, `tst_zip_source`, `tst_ampr_ftp`, `tst_zstd_games` | `fw_engine_integration` |
| DLC | `tst_dlc_ini`, `tst_dlc_convert`, `tst_dlc_lookup` | `fw_engine_dlc` |
| Game metadata | `tst_gameinfo` | `fw_engine_gameinfo` |
| Backport readers | `tst_backport_self` | `fw_engine_backport` |
| Tools | `tst_tools`, `tst_dlcemu`, `tst_dumpcheck` | `fw_engine_tools` |
| PS5 connect | `tst_ps5_ftp`, `tst_ps5_content`, `tst_ps5_services`, `tst_ps5_diagnostics`, `tst_ps5_crashdumps` (all against `FakePs5.h`) | `fw_connect_ps5` |
| Update, notify | `tst_update`, `tst_notify` | own module |
| UI (offscreen) | `tst_app_ui`, `tst_ui_widgets`, `tst_rtl_layout`, `tst_language_row`, `tst_app_shell_context`, `tst_app_shell_ps5`, `tst_pages_home_queue`, `tst_add_to_queue`, `tst_queue_finish`, `tst_build_page` (`app/buildpage/`), `tst_image_tools`, `tst_dlc_page`, `tst_pagesb_editors`, `tst_app_ps5_tools`, `tst_ps5_tools_ui`, `tst_issues_library`, `tst_issues_portable` | all modules + `fw_app_lib` |
| Backport page and engine host | `tst_backport_engine`, `tst_backport_page`, `tst_pagesb_backport` | all modules + `fw_app_lib` |
| Single-file launcher | `tst_onefile` (Windows; packs a tiny payload and runs the stub) | all modules |

UI tests are left out when `FW_BUILD_APP=OFF`. `docs/UI.md` section 5 says
what the main UI programs check.

**Resource locks.** `tst_exfat_bench`, `tst_exfat_roundtrip`,
`tst_pfs_benchmark` and `tst_ufs2_roundtrip` carry the label `heavy` and the
lock `fw_heavy_disk` (never side by side). `tst_backport_engine`,
`tst_backport_page` and `tst_pagesb_backport` share `fw_backport_process_name`:
one of them starts a stand-in named like the engine. A test may also skip when a
real `PS5BackportGUI.exe` is running on the PC.

## Opt-in checks (skipped unless set)

| Variable | Test | Needs |
|---|---|---|
| `FW_WINDOWS_MOUNT_TEST=1` | `tst_exfat_windows` | Administrator rights (Mount-DiskImage, chkdsk). Agents do not run it; the CI runner does |
| `FW_UFS2TOOL_DLL`, `FW_DOTNET`, `FW_FSCK_UFS`, `FW_DUMPFS_UFS`, `FW_MAKEFS` | `tst_ufs2_compat` and UFS2 helpers | The reference tools (`docs/engines/UFS2.md`) |
| `FW_LAZY_MKPFS_DIR` | `tst_pfs_benchmark`, `tests/data/pfs/make_golden.py` | A Lazy_MkPFS checkout (not in the repository) |
| `FW_BACKPORT_E2E_GAME`, `FW_BACKPORT_E2E_COPY_DIR`, `FW_BACKPORT_ENGINE` | `tst_backport_engine`, `tst_backport_page` | A copy of a game folder; never the original dump |
| `FW_BACKPORT_E2E_TOOLKIT` (with `FW_BACKPORT_E2E_GAME`) | `tst_backport_update endToEnd`; adds an FPKG build to `tst_zstd_games realGameEndToEnd` | The owner's own Sony Publishing Tools and a game: the owner runs these, agents do not |
| `FW_DLC_E2E_CLI` | `tst_dlc_convert`, `tst_dlc_page` | A real fpkg-cli |
| `FW_ZSTD_E2E_GAME` | `tst_zstd_games realGameEndToEnd` | A game folder (the test works on a copy); agents use a synthetic one |
| `FW_FPKG_COMPARE_DUMP`, `_OUT`, `_MODE` | `tst_fpkg_rules` | Comparison with the reference builder (`tests/data/fpkg/compare_with_reference.py`) |

Keep and shot switches for looking at results by hand: `FW_EXFAT_KEEP_SAMPLE`,
`FW_EXFAT_KEEP_EDIT`, `FW_E2E_KEEP`, `FW_UI_SHOT_DIR` (`tst_app_ui`),
`FW_UI_GRAB_DIR` (`tst_ui_widgets`), `FW_SHOT_DIR` (`tst_backport_page`),
`FW_PS5_SHOTS` (`tst_app_ps5_tools`), `FW_TEST_HEARTBEAT=<file>`
(`tst_backport_page`, to find a hang).

## Benchmarks

The benchmarks take the Performance settings from the environment, resolved
as the app resolves them (`tests/engines/BenchSettings.h`):

| Variable | Meaning |
|---|---|
| `FW_BENCH_MODE` | `lowImpact`, `balanced` or `maximum` (`performance.mode`) |
| `FW_BENCH_THREADS` | `performance.workerThreads` (0 = from the mode) |
| `FW_BENCH_BUFFER_MIB` | `performance.ioBufferMiB` (1-64) |
| `FW_EXFAT_BENCH_MIB` | Data size of `tst_exfat_bench` |
| `FW_UFS2_BENCH_MIB` | Data size of `tst_ufs2_roundtrip benchmark` |
| `FW_PFS_BENCH_MIB`, `FW_PFS_BENCH_LEVEL`, `FW_PFS_BENCH_REPEAT`, `FW_PFS_BENCH_SINGLE=0` | Size, zlib level, best of N, leave the one-thread build out (`tst_pfs_benchmark`; set `QTEST_FUNCTION_TIMEOUT` for large sizes) |

Commands and the measured numbers: `docs/PERFORMANCE.md`. Use a Release
build and the isolation variables above.

## Fixtures

`tests/data/**` is compared byte for byte (`-text` in `.gitattributes`). Each
fixture set has the script that made it (`tests/data/pfs/make_golden.py`,
`tests/data/fpkg/make_goldens.py`, `tests/core/make_archive_fixtures.py`);
these are developer tools run with Python 3, not part of the app. Fixtures are
synthetic: no commercial game data. The root `.gitignore` ignores `*.exe`,
`*.pkg`, `*.exfat`, `*.log` and similar everywhere, so a new fixture with such
an extension needs `git add -f` or an exception.

## Workflows

* `native-windows.yml` (by hand or from the release workflow): MSVC + Qt 6.8.3,
  all tests with `FW_WINDOWS_MOUNT_TEST=1`, a failing test rerun with
  `-o <file>,txt`, then deploy, one-file exe, self-test, screenshots, launch.
* `native-linux.yml` (by hand): GCC Debug with AddressSanitizer and UBSan,
  `LSAN_OPTIONS=suppressions=native/tests/lsan.supp`.

## Writing a test

* Put it in the folder of the module it tests; a module test links only that
  module, so it builds while another module is broken. File names are target
  names and must be unique under `native/tests`.
* Write the failing test first, then the fix.
* Synthetic data only; build images with the engines themselves; never
  contact a PS5 (use `FakePs5.h`) and never run Sony tools (use
  `fw_fake_pubcmd`). Label such results MOCK.
* Helper executables a module's tests need go in that folder's `extra.cmake`.
