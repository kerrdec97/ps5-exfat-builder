# Backport (`native/src/app/backport/`, `native/src/engines/backport/`)

The app does **not** contain a backport engine of its own. The Backport tab drives the bundled
**PS5 Backport engine** (the owner's PS5 Backport GUI, `PS5BackportGUI.exe`) as a separate process.
Every build, SDK rebuild, PS5 Pro build, AMPR / PlayGo switch and restore runs in that program's
worker, so the results are the ones the standalone program produces.

**Which engine is bundled.** By default the owner's published **testing build 0.3.5-test4**
(GitHub `v0.3.5-test4`, 17 Nov 2026, SHA-256 `68d09ea5…e6f4`, backend `3e08eda5-19422409a6`; its
release text says "Testing build, not a release"). The app says so wherever it names the engine
(About, the engine line in Backups & support, the Activity log: "testing build"). The last regular
release, **0.3.4** (`early-testing-beta-0.3.4`, `cf8eaa88…cf42`, backend `3e08eda5-914d90065d`), is
still buildable with one option:

    cmake -DFW_BACKPORT_ENGINE_VERSION=0.3.4 -DFW_BACKPORT_ENGINE_EXE=<PS5BackportGUI-0.3.4.exe> ...

The option picks version, URL, SHA-256 and backend revision together in `cmake/BackportEngine.cmake`
and reaches the code as `bp::kEngineVersion`, `bp::kEngineRevision` and `bp::kEngineIsTestingBuild`
(the one source of every version text). With 0.3.4 the Offline mode tick is hidden.
`packaging/make-release.ps1` checks the bundled file against the first SHA-256 line of that file,
which is the default version's; a release made with 0.3.4 needs that check adjusted by the owner.

test4 was built from an uncommitted tree: its exact source is GUI commit `8b6b031e` plus the patch
saved at build time (owner's note). Everything this app reads from it (job and result keys, ledger
states, space rules, texts) was taken from that state only, never from the newer worktree.

A native C++ backport pipeline existed in the v5.0 previews (planner, SELF writer, backup archive,
apply journal, restore, SDK Fixer, detectors, `backport.*` settings and job kinds). It was removed on
purpose before 5.0.1. What is left of it in `src/engines/backport/` is a handful of small, read-only
helpers the tab uses for its game list; they are listed under "Native helpers" below.
[`../BACKPORT_PARITY.md`](../BACKPORT_PARITY.md) is the specification that pipeline was written from.
It is kept as a description of the formats and of the original program's behaviour, not as a
description of code in this repository.

## Where things are

| Part | Files | What it does |
|---|---|---|
| The page | `src/app/pages/BackportPage.*` | A native replacement for the original program's window, one-to-one: game library (scan folders, search, tick games), progress and activity, Save to (backport ZIPs, originals backup ZIPs), options (Target SDK, Apply, PS5 Pro fix, PS5 Pro build), Backups & support, Start Backport / Cancel, "Switch AMPR / PlayGo…". |
| Engine host | `src/app/backport/BackportHost.*` (`fw::bp`) | Finds the engine executable, starts one worker process per job, speaks its protocol, cancels it. |
| Window-side logic | `src/app/backport/BackportCore.*` (`fw::bp`) | What ran in the original program's window process and therefore runs natively: game discovery and metadata, the already-backported check, reads of the applied-backports ledger, the per-user settings file, the pre-run destination and free-space checks, the AMPR / PlayGo variant list and user-library checks, the support-logs ZIP. Rules and texts follow the original program. |
| Native helpers | `src/engines/backport/*` (`fw::backport`, library `fw_engine_backport`) | Read-only readers used by `BackportCore.cpp` (next section). |
| Packaging | `cmake/BackportEngine.cmake` | Bundles the engine as `engines/backport/PS5BackportGUI.exe` next to the app, pinned by SHA-256 (`FW_BACKPORT_ENGINE_VERSION` = `0.3.5-test4` (default) or `0.3.4`; `FW_BACKPORT_ENGINE_EXE` = a local copy of that exe, or `FW_FETCH_BACKPORT_ENGINE=ON` to download it once). The engine is a separate, closed-source program by the same author. |

The module `fw_engine_backport` registers **no settings, no dependencies and no job kinds**
(`Module.cpp`). Backport runs are not jobs of the app's queue; the page owns its worker.

## The engine process

From `BackportHost.h` (the authority for the protocol):

* Executable: `FW_BACKPORT_ENGINE` (environment), then `<app>\engines\backport\PS5BackportGUI.exe`,
  then the same under the app's parent folders (developer builds). Not found: the page says so and
  nothing runs.
* One process per job: `PS5BackportGUI.exe --worker <job.json>`. The job file is written to
  `<DATA_DIR>\jobs` as UTF-8 without a BOM and deleted afterwards. Working directory = `DATA_DIR`,
  every `PYTHON*` variable removed, no console window.
* stdout: one JSON object per line; `progress` and `result` events are used, anything else is ignored.
  stderr is drained continuously (the last 200 lines are kept for the failure text).
* stdin stays open for the worker's whole life (end of file means cancel). Cancel = `cancel\n`; after
  1.5 s the process tree is killed, but only while the last progress phase is `engine` (export and
  apply finish or roll back their current step themselves).
* No result line: `CANCELLED` when cancel was asked, otherwise `FAILED` ("The backport worker stopped
  unexpectedly") with the stderr tail.
* The engine's `selftest` job reports its revision and engine root (`EngineInfo`).

Per-user folders are the original program's, so a backport applied by the standalone PS5 Backport GUI
is recognised here and the other way round:

| Folder | Default | Holds |
|---|---|---|
| `CONFIG_DIR` | `%APPDATA%\Deckerr97BackportGuiBeta` | `settings.json`, `applied_backports.json` (the ledger, written by the worker) |
| `DATA_DIR` | `%LOCALAPPDATA%\Deckerr97BackportGuiBeta` | `jobs`, `logs`, `logs\runs` |

`D97BETA_CONFIG_DIR` and `D97BETA_DATA_DIR` override them, as in the original program. **Tests must
set both** (to temporary folders) so the user's real settings and ledger are never touched; CTest
also points both into the build folder for every test (`tests/CMakeLists.txt`).

Because the files are shared: `settings.json` is merged per key and is not overwritten when it cannot
be read; folders are written with native separators; an unfinished apply is not offered for restore
while a PS5 Backport GUI that this app did not start is running, or while the journal was written in
the last five minutes (`kApplyQuietSeconds`), and no restore (offered or chosen by hand) starts while
that program runs. A run keeps the output and backup folders its preflight checked for all of its
games; the two folder fields are locked while it runs, and a change the standalone program makes to
`settings.json` meanwhile applies to the next run only.

**Engine home and the standalone program.** Both programs extract their engine to the same home
(`%SystemDrive%\D97B` while Windows long paths are off, otherwise `%LOCALAPPDATA%\D97B`;
`D97BETA_ENGINE_HOME` overrides it). Every worker job clears the engine's work folders first, and a
0.3.4 window started while another revision's worker runs removes the other revision folders. The
app does not give its worker a home of its own (that would extract a second 120 MB copy); instead no
job of any kind starts while a PS5 Backport GUI that this app did not start is running (Start
Backport, SDK rebuild, Pro build, AMPR / PlayGo switch, restore), and a run whose next game would
start while that program runs stops there, as after a cancel ("Run stopped: PS5 Backport GUI was
started meanwhile"). A standalone window opened in the middle of one job is not detected until that
job ends. test4 may also choose `D97C`…`D97F` when another Windows account owns `D97B`; the app's
free-space check looks at `D97B` only.

**What the host does for a 0.3.5 engine** (all of it harmless with 0.3.4):

* Ledger: `ROLLBACK_INCOMPLETE` and `RESTORING` (record or journal; the journal has the last word)
  are active and unfinished: the row says "Restore needed" and the restore is offered; an undo that
  left none of the backport's files (`backport_files_left: false`) is finished; rows that are not
  objects are ignored; a game folder that no longer exists is not offered for a restore.
* Before a run: the worker's own Target SDK rule (no build above the game's own SDK, Target SDK 4
  always allowed) marks the row "Needs SDK ≤ N" and is said before the run, which then builds the
  others only if the user agrees; the same rule disables "Rebuild … with SDK N" for such games.
  Space: the backup folder needs 1.6× the executables + 20 MB (0.3.4 asked 1.1×), and output and
  backup on one drive must fit together.
* SDK rebuild of the row's own ZIP sends `game_dir`, `unchanged` (the run's
  `unchanged_not_exported`) and `source_files` (its `source.files_read_by_engine`), so 0.3.5 rebuilds
  the executables the first build left unchanged. A ZIP chosen in the file dialog sends neither.
* Results: functions without a replacement (`engine.missing_functions`), Offline mode
  (`offline_mode.note`), `no_sdk_field_note`, `sdk_ceiling_note`, a restore's `notes` (or its
  `left_in_place` / `kept_newer_files` / `derived_kept` lists) and `leftover_dirs` are shown in the
  game's details and the Activity log.
* **Offline mode (skip PSN sign-in)**: a tick in Options, off at every start and never saved, as in
  that program; sent as `options.offline_mode: true` only when ticked. Its texts are test4's.

**Closing.** The destination check before a run counts as busy, so closing the window or the power
action after the queue waits for it, and a run whose check finishes during the close is not started
("Run not started: the app is closing"). A worker that is still exporting or applying when the app
exits loses the reader of its output: its next progress line fails at once (settled with a stand-in,
`tst_backport_engine::anOrphanedWorkersNextLineFails`), and the engine's `emit()` (0.3.4 and test4
alike) does not catch that error, so the step it was in stops with an exception. Only the engine can
make that safe; the host waits for the worker when the window is closed (MainWindow).

**From the dump check (#26).** Library › Check dump › "Backport…" opens the Backport tab with that
game listed, ticked and selected (`navigateWith{folder, select}`). When the dump looks backported or
half-restored and the ledger has its apply with a backup journal, the button is "Restore originals…"
and the tab offers that game's restore (`restore: true`), asking first as by hand.

### Worker protocol proof with the real test4 exe

Run on 23 Nov 2026 with the bundled file (SHA-256 `68d09ea5…e6f4`), isolated `FW_DATA_DIR`,
`D97BETA_CONFIG_DIR`, `D97BETA_DATA_DIR`, `D97BETA_ENGINE_HOME` (the test's temporary folders),
`FW_OFFLINE=1`, `FW_NO_POWER_ACTIONS=1`:
`FW_BACKPORT_ENGINE=<build>\bin\engines\backport\PS5BackportGUI.exe tst_backport_engine engineSelftest
engineProtocolOnASyntheticGame engineRefusesNonGame`. Selftest: revision `3e08eda5-19422409a6`. A
`backport` job on a synthetic stand-in game with every key the host sends plus `offline_mode`: five
progress lines (stage, pct, phase), one result line, status `FAILED` (the stand-in `eboot.bin` is not
a real executable: "BuildRefused … use a fully decrypted game dump"), the engine recorded the options
as sent (`offline_mode: true`), the game folder unchanged. The user's own engine homes and settings
were not touched. This proves the protocol, not a backport: no game was built and nothing ran on a
console.

## Native helpers (`src/engines/backport/`)

All of them only read. None writes into a game folder.

| Helper | Files | Used for |
|---|---|---|
| `selfio::hasSelfMagic`, `selfio::inspect`, `selfio::unwrap` | `SelfReader.*`, `SelfFormat.h`, `ElfImage.*` | Rebuilds the ELF from a fake-signed (unencrypted, uncompressed) SELF so the SDK of an `eboot.bin` can be read. Encrypted, compressed and malformed files are refused with a reason. |
| `findParams`, `elfSdkMajor` | `SceParam.*` | Reads the PS5 / PS4 SDK values from the process or module param of an ELF (`PT_SCE_PROCPARAM` / `PT_SCE_MODULE_PARAM`: size at +0x00, magic at +0x08, PS4 SDK at +0x10, PS5 SDK at +0x14). |
| `bcdMajor`, `bcdMinor`, `sdkVersionText` | `SdkTable.*` | SDK value → "4.50" text (top byte major, second byte minor, both BCD). |
| `parsePs5VersionField` | `GameMetadata.*` | `sdkVersion` / `requiredSystemSoftwareVersion` of a `param.json` → major, minor. |
| `kCanonicalTrio` | `CanonicalTrio.h` | Names, sizes and SHA-256 of the three fakelib files every package of the engine ships (data only, no files): one match in `fakelib/` marks a game as already backported. |
| `Code`, `makeError`, `codeOf`, `kMaxExecBytes` | `Common.*` | The readers' error codes (carried as a `[Code]` prefix in `Error::detail`) and the 1 GiB size limit. |

How the game list uses them (`BackportCore.cpp`): `ebootSdk()` reads `eboot.bin` (up to 512 MiB),
unwraps it when it is a SELF, and shows the PS5 SDK of its process param; `param.json` versions are
shown through `parsePs5VersionField`; the already-backported check hashes only files whose name and
size match a pinned library. The page uses these readers for its list only; everything a backport
does is the engine's.

Limits that protect the list from hostile files (a dump from someone else is untrusted input):

* every read in the SELF and ELF readers is bounds-checked without overflow;
* program headers must lie inside a 1 GiB executable, and `unwrap()` refuses to produce more than
  twice the SELF's own size plus 16 MiB (a fake-signed SELF is not compressed, so its ELF cannot be
  larger than that);
* the version segment is taken from the bytes after the SELF only when its size matches exactly.

### Licences

`SelfFormat.h` and `SelfReader.*` derive from `make_fself.py` by john-tornblom (ps5-payload-dev/sdk,
GNU GPL version 3), as modified by Nazky, and carry an attribution header; the reader is
reimplemented from the format description rather than translated from `decrypt_fself.py`. make_fself
is listed in `THIRD_PARTY_NOTICES-native.md`. `SceParam.*` is clean-room from the documented facts
(offsets, magics); no code from the old Python app's SDK patcher was used. The native code bundles
and downloads no library, firmware table or fakelib binary; the trio pins are hashes only.

## Tests

| Test | Covers |
|---|---|
| `tests/engines/backport/tst_backport_self.cpp` | The native readers on synthetic files built in memory: a fake-signed SELF unwraps to its ELF and SDK; program headers and a version segment that would inflate the output are refused; truncated and encrypted input is refused without crashing; an ELF without a param has no SDK. |
| `tests/app/tst_backport_engine.cpp` | The engine host. Always: scanner rules, preflight refusals, AMPR / PlayGo user-file checks, the settings round trip, worker failure paths. With `FW_BACKPORT_ENGINE` set: the engine's selftest and the protocol (progress, result, cancel and tree kill). With `FW_BACKPORT_E2E_GAME` as well: the game folder is copied into a temporary folder (under `FW_BACKPORT_E2E_COPY_DIR` when set) the first time an end-to-end test needs it, then a full backport of the copy with Apply off, a switch and an SDK rebuild, into temporary folders. The other tests never wait for the copy; too little free space for it skips the end-to-end tests, and a link in the dump fails them. |
| `tests/app/tst_backport_page.cpp` | The page. |
| `tests/app/tst_pagesb_backport.cpp` | The page's rules with a stand-in engine (a command script that speaks the worker protocol): one worker at a time, settings merging, the standalone program holding back every job, the Target SDK ceiling, Offline mode in the job, the destination check counting as busy, the dump check hand-over, the engine version texts, the SDK rebuild keys and the 0.3.5 result keys. MOCK: never evidence of what the engine or a console does. |


Not tested here: anything on a PS5. Whether a console loads a backport is a property of the bundled
engine, not of this repository's code.
