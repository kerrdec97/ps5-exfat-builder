# Backport (`native/src/app/backport/`, `native/src/engines/backport/`)

The app does **not** contain a backport engine of its own. The Backport tab drives the bundled
**PS5 Backport engine 0.3.4** (the owner's PS5 Backport GUI release, `PS5BackportGUI.exe`) as a
separate process. Every build, SDK rebuild, PS5 Pro build, AMPR / PlayGo switch and restore runs in
that program's worker, so the results are the ones the standalone program produces.

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
| Packaging | `cmake/BackportEngine.cmake` | Bundles the engine as `engines/backport/PS5BackportGUI.exe` next to the app, pinned by SHA-256 (`FW_BACKPORT_ENGINE_EXE` = a local copy of the release, or `FW_FETCH_BACKPORT_ENGINE=ON` to download it once). The engine is a separate, closed-source program by the same author. |

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
set both** (to temporary folders) so the user's real settings and ledger are never touched.

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
size match a pinned library.

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
(offsets, magics); no code from the unlicensed SDK patcher was used. No Sony library, firmware table
or fakelib binary is bundled or downloaded; the trio pins are hashes only.

## Tests

| Test | Covers |
|---|---|
| `tests/engines/backport/tst_backport_self.cpp` | The native readers on synthetic files built in memory: a fake-signed SELF unwraps to its ELF and SDK; program headers and a version segment that would inflate the output are refused; truncated and encrypted input is refused without crashing; an ELF without a param has no SDK. |
| `tests/app/tst_backport_engine.cpp` | The engine host. Always: scanner rules, preflight refusals, AMPR / PlayGo user-file checks, the settings round trip, worker failure paths. With `FW_BACKPORT_ENGINE` set: the engine's selftest and the protocol (progress, result, cancel and tree kill). With `FW_BACKPORT_E2E_GAME` as well: a full backport with Apply off, a switch and an SDK rebuild, into temporary folders. |
| `tests/app/tst_backport_page.cpp` | The page. |

Run them with `ctest -R backport`.

Not tested here: anything on a PS5. Whether a console loads a backport is a property of the bundled
engine, not of this repository's code.
