# Backport engine (`native/src/engines/backport/`)

Module `fw_engine_backport`, namespace `fw::backport`. The spec is
[`../BACKPORT_PARITY.md`](../BACKPORT_PARITY.md); this page says what is built, what each safety
rule's test is, and what is blocked.

**Not tested on a PS5.** Every result below comes from synthetic ELF/SELF fixtures built in the
tests from the format tables (no game data, no Sony binaries). For the fake-signed output the
engine reproduces the Python engine byte for byte (the golden SHA-256 values in Appendix A).
Whether a console loads the result is **UNVERIFIED** by this work.

## Status

| Component | Status | Files |
|---|---|---|
| SDK pair table 1–10, BCD decoding, labels, table hash | Implemented | `SdkTable.*` |
| ELF64 parser with the signer's header rules | Implemented | `ElfImage.*` |
| SDK patch of process/module param +0x10/+0x14 (clean-room, §2.5) | Implemented | `SceParam.*` |
| Fake-SELF writer (GPL-3.0 port of make_fself) | Implemented, golden hashes match | `SelfFormat.h`, `SelfWriter.*` |
| SELF reader / unwrap with encryption, compression and bounds checks | Implemented | `SelfReader.*` |
| libc import rename (data-driven, SDK 6 boundary, applied after signing) | Implemented | `NidPatch.*` |
| Scanner (one scanner for preflight, build and reports) | Implemented | `ExecutableScanner.*` |
| Fakelib set validation and state | Implemented | `FakelibSet.*` |
| Game metadata for ZIP names and title IDs (param.json, param.sfo) | Implemented (module-local) | `GameMetadata.*` |
| Planner: every check before anything is written | Implemented | `BackportPlanner.*`, `BackportOptions.h` |
| Patcher: staging build and build verification V1–V5 | Implemented | `BackportEngine.*` |
| Packager: originals and backport ZIPs, read back before the rename | Implemented (on `core/Zip.h`) | `BackupArchive.*` |
| Manifest schema 1 read, schema 2 written; hash verdict | Implemented | `BackportManifest.*` |
| Journaled Apply with content-based rollback, crash recovery | Implemented | `ApplyJournal.*` |
| Restore (zip-slip refusal, added files removed, manifest checks) | Implemented | `RestoreService.*` |
| Run orchestration (Scan → Patch → Package → Verify → Apply) | Implemented | `BackportRunner.*` |
| Jobs `backport.run`, `backport.restore`, `backport.scan` | Implemented | `BackportJobs.*` |
| Settings and legacy keys | Implemented | `BackportSettings.*`, `Module.*` |
| Rebuild a ZIP with another SDK (#31), SDK Fixer, Results tracker, AMPR index, report export | Later (not in this pass) | — |
| Import-table audit, fix families, audio check, PS5 Pro fix / Pro build, `right.sprx` copy, default fakelib set, save-data fixes, SDK pairs 11–13 | **Blocked** (§7 of the spec); no code and no settings | — |

## Licences

* `SelfFormat.h`, `SelfWriter.*`, `SelfReader.*` are a port of `make_fself.py` by john-tornblom
  (ps5-payload-dev/sdk, GNU GPL version 3), as modified by Nazky, and carry an attribution header.
  The reader is reimplemented from §2.7 rather than translated from `decrypt_fself.py`.
  `THIRD_PARTY_NOTICES-native.md` needs a matching entry (outside this module; see "Core and
  packaging changes").
* `SceParam.*`, `NidPatch.*` and the pipeline are clean-room from the spec's facts. No code from
  `Backport.py`, the SDK patcher gist or the Backport GUI was read or translated. The legacy app's
  own code (`core/backport_safety.py`, `ui/backport_profiles.py`, `exfat_builder.py` helpers such
  as `_sfo_zip_prefix`) was used as the behavioural reference.
* No `right.sprx`, PSSR, kernel files, firmware tables or fakelib binaries are bundled or downloaded.

## A run

| Phase | What happens | The game folder |
|---|---|---|
| Scan | `planBackport()`: scan, all checks, issues collected as Refuse / Confirm / Warn / Info. The only write is the S10 probe file in each ZIP folder (created and removed). | read |
| Patch | `buildBackport()` in `<job temp>/staging`: `originals/<rel>` snapshot, `unwrapped/<rel>` (SELF inputs), `stage/<rel>`. Per executable: unwrap → SDK patch → sign → NID → V1–V3. Then the fakelib set is copied to `stage/fakelib/` and `stage/<dir>/fakelib/` for nested `eboot.bin` folders (V4), and coverage is checked (V5). | read |
| Package | Originals ZIP (required): the snapshot, replaced fakelib files, the game's param.json and `decrypted/` files. Backport ZIP (when a folder is set): everything staged. Both are written to `<final>.part`. | read |
| Verify | Each `.part` is read back (member count, CRC and size of every member, SHA-256 of every member against the hash recorded for its source), then renamed to its final name, which is never an existing file. The manifest is recorded. | read |
| Apply | Only when `backport.applyToGameFolder` is on: the journal is written, each file is copied to `<target>.fwpart`, checked and renamed over the target; every applied file is checked again (V7). On error or cancel the game is rolled back. | written |

Outcomes: **Succeeded**; **Partial** (encrypted files left unchanged by policy: a successful job
with verification Inconclusive; or the backport ZIP or Apply failed: a failed job whose message
starts "Partial:" and names the originals ZIP); **Failed** ("No safe backport was produced: …",
nothing written except possibly a kept staging folder); **NothingToDo** ("No backport is
needed"). Error details start with `[Code]` (`Common.h`, `codeOf()`).

`backport.applyToGameFolder` defaults to **on**, as the legacy app always applied. Backport GUI
defaults to off. This is the owner's decision (D5): flip the default in `BackportSettings.cpp`.

## Settings

All keys are read by this module; blocked features have no setting.

| Key | Section · group | Type, default | Per job / advanced | Read by | Legacy key |
|---|---|---|---|---|---|
| `backport.originalsZipFolder` | Backport · Folders | DirPath, "" | — | `optionsFromSettings` (run, scan); restore job (ZIP and manifest lookup); library scan exclusion | `abp_zip_orig_dest` |
| `backport.backportZipFolder` | Backport · Folders | DirPath, "" | — | `optionsFromSettings`; library scan exclusion | `abp_zip_pat_dest` |
| `backport.fakelibFolder` | Backport · Folders | DirPath, "" (a file path is refused) | per job | `optionsFromSettings` | `abp_fakelib` (a file path is dropped with a warning) |
| `backport.applyToGameFolder` | Backport · Run | Bool, **true** | per job | `optionsFromSettings` → Apply phase, plan line | — |
| `backport.notes` | Backport · Run | String ≤ 500, "" | per job | written into the manifest | `abp_notes` (cut to 500) |
| `backport.scanFolders` | Backport · Game library | StringList, [] | — | `backport.scan` without a game (library mode) | `bp_folders` |
| `backport.scanDepth` | Backport · Game library | Int 1–12, 6 | — | `backport.scan` library mode | — |
| `backport.encryptedSelfPolicy` | Backport · Safety | Enum refuse / leaveUnchanged, refuse | per job, advanced | planner (S32) | — |
| `backport.verifyBuild` | Backport · Safety | Bool, true | advanced | engine (V1–V3); the job outcome says when it was off | — |
| `backport.keepStagingOnFailure` | Backport · Safety | Bool, true | advanced | runner: a failed run's staging is moved to `<temp>/fw-backport-staging-*` | — |
| `backport.journalRetentionDays` | Backport · Safety | Int 1–365, 30 | advanced | runner: `ApplyJournal::prune()` before each Apply | — |
| `backport.targetSdk` | Firmware and SDK · Backport target | Enum "1"…"10", "4"; labels "SDK N · loads on FW N.00+" | per job | `optionsFromSettings` | `abp_target_sdk`, `abp_last_sdk` (values outside 1–10 dropped with a warning) |
| `backport.nidPatchMode` | Firmware and SDK · Backport target | Enum auto / off, auto | advanced | engine | — |

`storage.freeSpaceMarginMiB` (general module) is read as the S12 reserve when it is registered.
`bp_scan_folder` is not imported: `importLegacy()` maps one legacy key to one setting and would
overwrite `bp_folders` (see "Core and packaging changes"). `backport.results` (the Results list)
is not implemented yet.

## Safety rules → tests

Test files are in `native/tests/engines/backport/`: `tst_backport_format.cpp` (F),
`tst_backport_scan.cpp` (Sc), `tst_backport_zip.cpp` (Z), `tst_backport_planner.cpp` (P),
`tst_backport_run.cpp` (R).

| Rule | How it is kept | Tests |
|---|---|---|
| S1 no fallback patching | A file that cannot be rewritten fails the build; nothing is written into the game. | R `perFileFailureProducesNothing`, `verificationCatchesAParamOutsideTheSignedSegments`; P `unrewritableExecutableRefusedUpFront` |
| S2 no engine code from a user-writable place | Native code; nothing is extracted or run from disk. | by construction (no test possible) |
| S3 target is a pair 1–10, never clamped | `sdkPair()`; planner refusal; enum choices; legacy 11 dropped | F `sdkTableValues`; P `targetSdkRules`; Sc `settingsRegisteredAndWellFormed`, `legacyKeysImport` |
| S4 target ≤ game SDK | Planner, game SDK from eboot.bin (D7) | P `targetSdkRules`, `gameSdkComesFromEbootNotParamJson` |
| S5 eboot.bin at the root | Planner and `backport.run` validate() | P `ebootMustBeAtTheRoot`; R `jobsRunThroughTheFactory` |
| S6 fakelib state "same" → already backported | Planner refusal | P `fakelibRules`; R `selfGameBackportedAndApplied`; Sc `fakelibStates` |
| S7 manifest verdict | patched → refuse; differs → Confirm (job parameter `confirmDiffers`) | P `manifestVerdictRules`; Sc `manifestVerdicts`; R `selfGameBackportedAndApplied` |
| S8 fakelib folder rules | `fakelibProblems()` | Sc `fakelibGoodSet`, `fakelibNestedSetRejected`, `fakelibUndersizedAndNonElfRejected`, `fakelibEmptyOrMissing`, `fakelibInsideGameRejected`; P `fakelibRules` |
| S9 originals ZIP folder required, checked first | Planner and validate() | P `originalsFolderRequired`; R `jobsRunThroughTheFactory` |
| S10 ZIP folder: set, exists, not in game, ≤ 259, writable | `zipDestinationProblem()`; worst-case names | Z `destinationRules`, `unwritableDestinationRefused` (skipped when run as root, as in this sandbox); P `zipFolderRules` |
| S11 staging path length | Exact longest staging path computed; over 259 is a **warning**, because all staging I/O is long-path aware (S28) | P `longStagingPathIsWarned`; R `longGameAndStagingPaths` |
| S12 free space on every volume | Staging, both ZIPs and the game (Apply), summed per volume, plus the reserve | P `freeSpaceChecked` |
| S13 snapshot before anything changes; a copy error stops the run | Snapshot to staging; a read/write error ends the run | R `snapshotErrorStopsTheRun`, `fileChangedDuringTheRunIsRefused` |
| S14 another tool's replaced fakelib files kept | In the originals ZIP | R `anotherToolsFakelibKeptInOriginals`; P `anotherToolsFakelibIsKeptAndExtrasListed` |
| S15 OS metadata never executables | Scanner | Sc `osMetadataIsNeverAnExecutable` |
| S16 .bak never executables; none created | Scanner reports them; the engine writes none | Sc `bakFilesAreReportedNotExecutables`; R `buildOnlyLeavesGameUntouched`; P `nothingToDoAndLeftovers` |
| S17 .part → read-back → count → rename; never overwrite | `StagedZip`; `uniqueZipPath()` | Z `writesVerifiesAndNeverOverwrites`, `failureLeavesNothing`, `readBackDetectsChangedSource`, `corruptPartIsCaught`, `namesAndUniquePaths`; R `earlierZipsAreNeverOverwritten` |
| S18 temp originals kept until the ZIP is verified | The game is never changed before Apply; a failed run's staging is kept and logged; rollback works from the ZIP alone | R `perFileFailureProducesNothing`, `crashDuringApplyIsRecoverable` |
| S19 any failure is not success | Errors propagate; outcome mapping | R `perFileFailureProducesNothing`, `originalsZipFailureProducesNothing` |
| S20 restore: zip slip refused; partial = failure | The whole restore is refused before writing anything when one entry is unsafe | R `restoreRefusesZipSlipBeforeWriting`, `safeMemberTargets`, `partialRestoreIsAFailure` |
| S21 manifest after each run, atomic | `writeManifest()` via `writeFileAtomic` | R `selfGameBackportedAndApplied`; Sc `pythonSchema1ManifestReadAndUpgraded` |
| S22 batch: same checks, doubtful → skip | Each game is its own `backport.run`; Confirm blocks unless `confirmDiffers` | P `manifestVerdictRules` |
| S23 archive handles closed | Core reader opens per read; rename/delete after use | Z `writesVerifiesAndNeverOverwrites` |
| S24 SDK Fixer | Not implemented in this pass | — |
| S25 Batch Retag | Not implemented in this pass | — |
| S26 tooling never closes issues | Process rule | — |
| S27 licence | Nothing blocked is bundled; no settings for blocked features | Sc `settingsRegisteredAndWellFormed` |
| S28 `\\?\` internally, ZIP names ≤ 259 | `fw::toLongPath`; S10 length check | R `longGameAndStagingPaths`; Z `destinationRules` |
| S29 Partial is explicit and names the originals | "Partial: … originals: <zip>" | R `backportZipFailureIsPartialAndAppliesNothing`, `applyFailureRollsBackExactly` |
| S30 ZIP folder inside the game refused | `zipDestinationProblem()` | Z `destinationRules`; P `zipFolderRules` |
| S31 nothing in the game changes before Apply | Staging only | P `okPlanWritesNothing`; R `buildOnlyLeavesGameUntouched` and every failure test |
| S32 encrypted / compressed SELFs found while scanning | Refuse by default; `leaveUnchanged` → Partial | F `encryptedAndCompressedDetected`; Sc `encryptedSelfDetectedUpFront`; P `encryptedSelfRefusedUpFront`; R `encryptedRefusedOrLeftUnchanged` |
| S33 fakelib/ and decrypted/ never processed | Scanner areas | Sc `fakelibAndDecryptedAreNeverProcessed`; R `anotherToolsFakelibKeptInOriginals` |
| S34 any per-file failure → no backport ZIP, no apply | Runner | R `perFileFailureProducesNothing`, `verificationCatchesAParamOutsideTheSignedSegments` |
| S35 journal, rollback, recovery at start-up | `ApplyJournal`; a pending journal refuses a new run on that game; `interruptedApplies()` for the app | R `applyFailureRollsBackExactly`, `cancelDuringApplyRollsBack`, `crashDuringApplyIsRecoverable`, `rollbackLeavesForeignContentAlone`, `journalsArePrunedByAge` |
| S36 restore removes added files, checks hashes | Manifest `added_files` (schema 2) or fakelib post hashes (schema 1) | R `restoreRemovesAddedFilesAndAllowsANewRun`, `restoreRefusesAChangedZip` |
| S37 the originals ZIP is never deleted automatically | Nothing in the module deletes a ZIP | R `restoreRemovesAddedFilesAndAllowsANewRun` |
| S38 ZIP folders not scanned; nested ZIP folders refused | Scanner and discovery exclusions; planner | P `zipFolderRules`; Sc `nestedEbootAndExcludedFolders`, `discoverGamesHonoursDepthAndExcludes` |

Implementable rules S1, S3–S23, S27–S38: each has at least one test. S2 holds by construction;
S24, S25 (features not built yet) and S26 (process) have no test.

Format and build checks: golden SHA-256 for F1/F2 × pairs 4/10 and the NID golden (F
`goldenHashes`, `workedExampleHeader`, `nidGolden`; R `selfGameBackportedAndApplied` checks the
same hashes on the files applied to the game); every pair 1–10 (F `eachPairIsWritten`, R
`eachTargetSdkLandsInTheGame`); NID boundary (F `nidBoundaryAndModes`); F3–F13 (F
`moduleWithoutParamIsSkipped`, `paramErrors`, `everyParamSegmentIsPatched`,
`ps5MagicUnwrapsLikeFake`, `paddedHeaderRoundTrips`, `truncatedSelfIsMalformed`,
`truncatedElfRefused`, `elfHeaderRulesRefuseSigning`).

## Python defects not copied

| # | Status | Test |
|---|---|---|
| P1 ELF dumps patched in place, .bak in the game | Fixed (staging only) | R `buildOnlyLeavesGameUntouched` |
| P2 encrypted SELFs "decrypted" into garbage | Fixed (S32) | R `encryptedRefusedOrLeftUnchanged` |
| P3 a dump's fakelib re-signed | Fixed (D3) | R `anotherToolsFakelibKeptInOriginals` |
| P4 failing file still gives a patched game and ZIPs | Fixed (D4) | R `perFileFailureProducesNothing` |
| P5 failures counted twice | Fixed: one `FileResult` per file, listed in the job details | by construction |
| P6 restore leaves fakelib/ | Fixed (D6) | R `restoreRemovesAddedFilesAndAllowsANewRun` |
| P7 "It worked" deletes the originals ZIP | No such action exists (S37) | — |
| P8 SDK 11 offered, "file" fakelib mode | Not offered | Sc `settingsRegisteredAndWellFormed` |
| P9 game SDK from required FW | Fixed (D7) | P `gameSdkComesFromEbootNotParamJson` |
| P10 unwrap assumes `e_phoff == e_ehsize` | Fixed | F `paddedHeaderRoundTrips` |
| P11 AMPR key folding | Not applicable yet (AMPR index not built) | — |
| P12 no free-space check in batch; wrong temp path estimate | Fixed (S12 per game, exact staging path) | P `freeSpaceChecked`, `longStagingPathIsWarned` |
| P13 games scan 2 levels, ZIP folders scanned | Fixed (depth setting, exclusions) | Sc `discoverGamesHonoursDepthAndExcludes` |
| P14 SDK Fixer modifies an ELF source | Not applicable yet (SDK Fixer not built) | — |

## Where this differs from the spec, and why

* **S11** is a warning, not a refusal: staging I/O goes through `fw::toLongPath`, which S28 allows.
* **Restore** refuses the whole ZIP when any entry would escape the game, before writing anything.
  Python restored the safe entries and reported the rest; a ZIP with such entries was not written
  by this app.
* **param.json** is stored in the originals ZIP under its real path inside the game. It is left
  out when it was only found in the parent folder. Python always stored it as `sce_sys/param.json`,
  which a restore would create even when the game had none there.
* The **dump's own fakelib extras** stay in the game unchanged and are not put into the backport
  ZIP (D3).
* Only the **libraries** of the fakelib folder (top-level `.sprx`/`.prx`) are staged; notes and
  metadata are ignored and listed. Python copied the whole folder.
* The ZIP name length check (S10) uses `" (99).part.part"` (15 characters) instead of
  `" (99).part"`. The shared core writer stages its output as `<path>.part` and renames it in
  `finish()`, so the engine passes `<final>.part` as the path; the read-back happens before the
  final rename.
* **Manifest** keys keep their order and unknown members (core `OrderedJson`), `ensure_ascii`
  output like Python's `json.dump`. Schema 2 adds `engine`, `applied`, `added_files`,
  `files_after`, `journal`, `notes`, per-target `applied`/`files_after`, `last_restore_zip`.
  Python's `backported_by_manifest()` reads it (checked once, by hand, against
  `core/backport_safety.py`: "patched" after a run, "original" after a restore; not in CI).
* A **pending apply journal** for the same game refuses a new run (`InterruptedApply`).
* Executables are processed one at a time (not in a pool) to keep memory at about three copies of
  the largest executable.

## Core and packaging changes this module needs (not made here)

1. `THIRD_PARTY_NOTICES-native.md`: add make_fself (john-tornblom, ps5-payload-dev/sdk, GPL-3.0;
   modified by Nazky) for `SelfFormat.h` / `SelfWriter.*` / `SelfReader.*`.
2. App start-up: call `fw::backport::interruptedApplies()` and offer "Roll back the interrupted
   backport of <game>" by queueing `backport.restore` with `{journal: <path>}`.
3. `SettingsSchema::addLegacyKey`: a converter that sees the whole legacy object (or merge
   semantics for StringList), so `bp_scan_folder` can be merged into `bp_folders`.
4. Optional: `fw::zip::Writer` could offer a `finish()` that does not rename (or a verify hook
   before the rename), so callers need not stage through `<final>.part`.
5. `ProgressReporter::beginPhase()` calls `std::clamp(index, 0, size - 1)`, which is undefined
   behaviour when no phases were declared (lo > hi). The queue always declares phases; tests must
   too.
6. DirPath validators that reject missing folders (the pattern in `general` and here) make
   `SettingsStore::load()` reset a ZIP folder on an unplugged drive to empty. Consider checking
   existence only when the setting is used.

## Running the tests

```
cmake -S native -B native/build-backport -G Ninja -DCMAKE_BUILD_TYPE=Debug -DFW_BUILD_APP=OFF
cmake --build native/build-backport --target tst_backport_format tst_backport_scan tst_backport_zip tst_backport_planner tst_backport_run
ctest --test-dir native/build-backport -R backport --output-on-failure
```

Each test links only `fw_engine_backport` and the core test support. The fixtures are in
`BackportFixtures.h` (F1–F13) and `BackportTestEnv.h` (game trees G1 a–h and a job context).
