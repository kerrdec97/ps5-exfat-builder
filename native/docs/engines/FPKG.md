# FPKG engine (`native/src/engines/fpkg/`)

Builds, verifies and extracts PS5 debug packages (`.pkg`) by driving the user's **own** Sony Publishing Tools
(`prospero-pub-cmd.exe`). Nothing from Sony is bundled or downloaded. The reference app (PSVIETHOA FPKG Builder 3.0.0)
has no licence: its behaviour is re-implemented from `native/docs/FPKG_PARITY.md`, no code or text is copied. There is
no built-in packer (LibProsperoPkg / Kraken / PFS v3) and no online metadata download.

**What has been tested, and how.** Every test in this document runs against our stand-in tool
(`native/tests/engines/fpkg/fake_pubcmd.cpp`), never against Sony's Publishing Tools or a PS5. The stand-in behaves the
way §4 of the parity document describes the real tool; where that description is UNVERIFIED, so is our engine. Byte
formats (GP5, scenario and `param.json`) are checked against goldens written by Python's own `xml.etree.ElementTree` and
`json` (`native/tests/data/fpkg/make_goldens.py`), because the toolkit's script writes them with those modules.

## Files

| File | Responsibility |
|---|---|
| `Module.h/.cpp` | `registerModule`: settings, probes `sony.pubcmd` and `sony.dds2png`, the four job kinds |
| `FpkgSettings.h/.cpp` | `fpkg.*` keys, `FpkgSettings::from(SettingsValues)` (one immutable snapshot per job), legacy key mapping |
| `Toolchain.h/.cpp` | `locatePubTools()` (exe, its folder, or a toolkit root with `toolchain\` / `bin\`; env `SCE_PROSPERO_PUBTOOLS`), `libScePubTools.dll` / `ext\` / VC++ x64 runtime checks, `patch-manifest.json` profile, `locateDds2png()`, exit-code hints, the two probes |
| `ContentNaming.h/.cpp` | Content ID rules, `ContentVersion` (parse, compare, next, name component), package and companion names, `name (n).pkg` |
| `ParamJson.h/.cpp` | tolerant `param.json` reading, the patch (DRM, versionFileUri, attribute3, required firmware, version, title), package size class, Python-style serialisation |
| `DumpRules.h/.cpp` | OS junk, SDK-generated files, AMPR / DLC-emulator / dump leftovers, the Publishing Tools path rule, `dlc_emu.ini` parser |
| `SourceScanner.h/.cpp` | the walk (case-folded order, exclusions with reasons, empty folders, path-rule list, links rejected), update overlay (source over base) |
| `PlayGo.h/.cpp` | 31 PlayGo languages, scenario-file rules and default object, layouts A/B/C, language payloads and fillers, `playgo-chunk.dat` / FICM / FLT parsing, the PS5 flat-path hash |
| `InputRepair.h/.cpp` | PNG colour types, DDS → PNG plan, PS4 → PS5 SELF signature copy, UCP digest check and repair |
| `Gp5.h/.cpp` | ElementTree-compatible XML writer, the GP5 model and writer, reader for `.gp5` sources |
| `AsciiAliases.h/.cpp` | junction (Windows) / symlink (elsewhere) per non-ASCII root, list-through check, sweep of links left by dead processes |
| `PubCmd.h/.cpp` | command lines, the `=` bar parser and line classification, friendly error texts, the run wrapper (TEMP/TMP, cwd, CPU-core limit, live limit, file-size poll) |
| `PackageHeader.h/.cpp` | FIH / superblock / plaintext-marker check |
| `SourceStager.h/.cpp` | the plug-in interface for image sources and `findAppRoot()` |
| `FpkgBuilder.h/.cpp` | requests (job JSON), `planBuild()` (pure: reads the dump, runs nothing, writes nothing), `runBuild/Addon/Verify/Extract`, output-conflict policy, same-package lock |
| `Jobs.h/.cpp` | `fw::Job` adapters |

`CMakeLists.txt` now depends on `fw_engine_general` only (it used to name `fw_engine_gameinfo`; the engine reads
`param.json` itself and depends on no other feature module).

## Job kinds

| Kind | JSON parameters | Phases (weight) |
|---|---|---|
| `fpkg.build` | `source` (folder, `.gp5`, or an image with a registered stager), `output`?, `basePackage`?, `baseFolder`?, `exactSource`? (bool), `updateOutput`? (`both` \| `update` \| `full`, default `both`), `contentId`? (must equal `param.json`), `contentVersion`?, `title`? | Preparing the source 8 · Planning 0.5 · Writing project files 0.5 · Pre-reading files 3 · Packing 90 · Finishing 0.5 · Checking the package 1 · Verifying with img_verify 8 · Computing SHA-256 6 |
| `fpkg.addon` | `payloadFolder`, `output`?, `contentId`?, `entitlementKey` (32 hex), `volumeType`? (`prospero_ac`; `prospero_al` is refused) | as `fpkg.build` |
| `fpkg.verify` | `package`, `passcode`? (default `fpkg.passcode`), `integrity`? (default true) | Checking the header 1 · Verifying with img_verify 99 |
| `fpkg.extract` | `package`, `destination` (must be empty or absent), `passcode`? | Extracting (indeterminate; bytes written from the free-space change) |

`output` defaults to `storage.defaultOutputFolder`, else `<source>-pkg` next to the source (for a `.gp5`, next to its
folder). A job's `fpkg.*` settings come from its snapshot, so the UI passes the user's answer to the conflict dialog as
the per-job override `fpkg.outputConflict` (a queue job cannot ask).

## Settings

| Key | Section | Type / default | Effect | Legacy key |
|---|---|---|---|---|
| `fpkg.pubToolsPath` | Tools | path, "" | the exe, its folder or a toolkit root; env `SCE_PROSPERO_PUBTOOLS` when empty | `fpkg_pubtools_path` |
| `fpkg.dds2pngPath` | Tools (advanced) | path, "" | "" = `prospero-dds2png.exe` in the toolkit root or next to the exe | — |
| `fpkg.compressionLevel` | FPKG | enum `default`, `9`…`-4` | `--compression_level N`; `default` passes nothing (tool default 7) | `fpkg_compression` ("Tool default" → `default`, "Fast (2)" → `2`, "Standard (4)" → `4`, "Smallest (7)" → `7`, "Maximum (9)" → `9`) |
| `fpkg.passcode` | FPKG (advanced, per job) | 32 printable ASCII, zeros | GP5 `passcode`, `img_verify` / `img_extract --passcode` | — |
| `fpkg.keepIntermediate` | FPKG | bool, false | keep `<stem>.gp5`, `<stem>.playgo-scenario.json`, `.gp5-assets/<stem>/` after success (always kept after a failure) | `fpkg_keep_project` |
| `fpkg.prescan` | FPKG | bool, true | open every game file once in parallel (clamp(2 × threads, 8, 32)) before `img_create`; runs on every OS (it only matters on Windows) | — |
| `fpkg.outputConflict` | FPKG (per job) | enum `keepBoth` (default), `overwrite`, `fail` | keep both: the existing file becomes `name (1).pkg`, the new one takes the name; overwrite: the old file is set aside and restored if the rename fails; fail: refused before packing (also in `validate()`) | — |
| `fpkg.clearVersionFileUri` | FPKG | bool, true | non-empty string → `""` | — |
| `fpkg.clearAttribute3` | FPKG | bool, false | non-zero number → 0 | — |
| `fpkg.lowerRequiredFirmware` | FPKG | bool, true | `requiredSystemSoftwareVersion` > (`sdkVersion` & 0xFFFF000000000000) ≠ 0 → that value as `"0x%016x"` | — |
| `fpkg.autoSizeClass` | FPKG | bool, true | full builds: `attributePub`, `kernel.addcontMountLevel`, removes `kernel.appSizeInGib` | — |
| `fpkg.keepEmptyFolders` | FPKG | bool, true | `<empty_dir>` for empty or junk-only folders outside `sce_sys` | — |
| `fpkg.keepDlcEmulator` | FPKG | bool, true | on: declares `dlc_emu.ini` mount folders and root `addcont<N>` folders without files, warns about missing mount folders; off: leaves out `dlc_emu.ini` and the three DLC modules in `fakelib`/`fakelib2` | — |
| `fpkg.removeAmprLeftovers` | FPKG | bool, true | leaves out `ampr_emu.index`, `fakelib/ampr_emu.index` (`fakelib/libSceAmpr.sprx` is always left out) | — |
| `fpkg.removeDumpLeftovers` | FPKG | bool, true | leaves out root `_DUBLEX_`, `Engine/Saved` (whole `Engine` if `Saved` is its only child), `<project>/Saved` next to `<project>/Content/Paks` | — |
| `fpkg.repairSelf` | FPKG | bool, true | PS4 SELF signature → PS5 signature in `.gp5-assets/<stem>/normalized-self/<rel>`; off: warning | — |
| `fpkg.repairUcp` | FPKG | bool, **false** | checks `sce_sys/trophy2/*.ucp`, `sce_sys/uds/*.ucp` (≤ 128 MiB): stale digest → corrected copy in `repaired-ucp/`; damaged archive → error naming the file | — |
| `fpkg.recoverPngFromDds` | FPKG | bool, true | missing or wrong-mode `sce_sys` PNG with a `.dds` → `prospero-dds2png` in parallel; off: wrong-mode presentation PNG is an error | — |
| `fpkg.playGoFallback` | PlayGo | bool, true | layout B when there is no usable table, else layout A | — |
| `fpkg.playGoChunkCount` | PlayGo (per job) | int 1–255, 100 | chunk count of layout B | `fpkg_chunk_count` |
| `fpkg.fillEmptyChunks` | PlayGo | bool, true | 1 MiB uncompressed `playgo-chunks/NNN.bin` for chunks without data | — |
| `fpkg.useSourcePlayGoTable` | PlayGo (advanced) | bool, **false** | layout C from the dump's `playgo-chunk.dat` + FICM + FLT | — |
| `fpkg.postVerify` | Verification (per job; enabled by `verification.verifyAfterBuild`) | enum `toolFull` (default), `toolFormat`, `headerOnly` | after the header check: `img_verify` with/without integrity; `verification.verifyAfterBuild` off = header only | `fpkg_tool_verify` (false → `headerOnly`, true → `toolFull`) |
| `fpkg.computeSha256` | Verification (per job) | bool, false | SHA-256 of every written package, in the log and `details.sha256` | — |

App-wide settings the engine reads: `performance.workerThreads` (the `img_create` CPU-core limit — 0 or ≥ cores = no
limit — plus pre-scan and DDS parallelism; this replaces the parity document's proposed `fpkg.cpuCores`),
`storage.tempFolder` (through the job's temp folder: image staging, base extraction, the SDK's `TEMP`/`TMP`; replaces
`fpkg.tempFolder`), `storage.defaultOutputFolder` (replaces `fpkg.outputNextToSource`), `storage.freeSpaceMarginMiB`,
`performance.ioBufferMiB` and `verification.verifyAfterBuild`. `performance.keepComputerAwake` is applied by the queue
(no `fpkg.preventSleep`). There is no setting for the reference's built-in-engine options.

**Live CPU limit.** `fw::fpkg::setLiveCpuLimit(jobId, cores)` (PubCmd.h) changes the limit of a running `img_create`
within 250 ms; the job clears it when it ends. The UI has to call it (for example from a "CPU cores" control on the
running job).

## How the Publishing Tools are driven

* `img_create --oformat nwonly [--compression_level N] [--ref_pkg_path <base>] <stem>.gp5 <stem>.partial.pkg`;
  `img_verify --passcode P --format_check on --integrity_check on|off --no_progress_bar <pkg>`;
  `img_extract --passcode P --no_progress_bar <pkg> <empty folder>`; `prospero-dds2png <in.dds> <out.png> [--preserve-alpha]`.
* Every path given to a tool goes through `AsciiAliases`: a root (source, base folder, output, base package folder, the
  SDK temp folder, a `.gp5`'s folder) with a non-ASCII path gets a junction/symlink under
  `<base>/fw-fpkg-aliases/a<pid hex>-<6 hex>-<tag>`, with the base chosen from the system temp, the job temp,
  `%ProgramData%\fw-fpkg` and the output drive root, never inside a source folder.
* Environment: inherited plus `TEMP` = `TMP` = `<job temp>/sdk-tmp-<8 hex>` (created before, removed after).
  Working directory: the toolchain folder if ASCII, else the system temp; a `.gp5` with relative paths runs in its folder.
* stdout is parsed byte by byte: the `|____` ruler arms the bar, each following `=` is 1/51 of the "Packing" phase,
  the scale line is dropped; `[Error]` fails the run even with exit 0; routine `[Warn]` lines are info; `[Info]`,
  `[Debug]`, "Creating an image" are logged. The size of `<stem>.partial.pkg` is polled every 500 ms (shown as
  "… written"). Success = exit 0, no `[Error]`, output present (and `.partial.pkg.remastered.pkg` for updates).
  `0xC0000135` / `0xC000007B` become `DependencyMissing` with the VC++ hint.
* Cancel kills the tool's whole process tree (core `ProcessRunner`), removes `.partial.pkg`, its `.remastered.pkg`
  and `.naps_metric.json`, writes `exit_code=cancelled` to `02-img-create.log` and keeps the project and logs.
* Logs (UTF-8, LF) in `<stem>-build-logs/`: `01-create-gp5.log` (project, counts, every exclusion with its reason,
  image/SELF/UCP repairs, empty folders, `param.json` changes, size class, warnings) and `02-img-create.log`
  (`command=`, `started_utc=`, `finished_utc=`, `elapsed=hh:mm:ss.fffffff`, `exit_code=`, then `[Error]`/`[Warn]` lines).

## Build rules implemented (all covered by tests against our goldens / the stand-in)

* GP5 byte format and entry order: scenario sidecar, recovered PNGs, language payloads, fillers, walked files,
  `<empty_dir>`s; `chunk` on every file in layout B, only when > 0 in C, never in A, always on payloads/fillers;
  `pfs_compression="disable"` on payloads/fillers; `param.json` → `.gp5-assets/<stem>/sce_sys/param.json`.
* Walk order (case-folded, then code point), exclusions of §4.5 (plus our two documented extras: root `decrypted/` and
  root image/package files), links → error, non-regular files left out, path rule (printable ASCII, no `%`/`;`, no empty
  component, no trailing dot) listed before packing, keystone exactly 96 bytes, AMPR import warning.
* Empty folders: empty or junk-only, outside `sce_sys`; folders emptied by exclusions are not declared; DLC mount
  folders first.
* PlayGo A/B/C as in §4.6 (B: language `i` in chunk min(i, N−1); every scenario lists every chunk; all chunks initial).
* `param.json`: DRM always `standard`, options, size class (thresholds ±1 byte tested, mount level 2 → 1 → 0, lv2 file
  limit, category missing → error, category ≠ 0 → warning and skipped, invalid mount level → error), Python-style
  output with CRLF; updates keep their size class.
* Updates: base checks (`.pkg`, not `.remastered.pkg`, not an output, FIH header present), overlay merge from
  `baseFolder` or from the whole base extracted with `img_extract` (payload folders of the base ignored), `exactSource`,
  same Content ID and higher version (with a suggested next version) when the base's `param.json` is known, "nothing to
  update" refused, outputs per `updateOutput`, header check on the `.remastered.pkg`.
* `.gp5` sources: passed to `img_create` unchanged after checking that every `src_path` exists and is ASCII, the path
  rule, `param.json` (name), the 96-byte keystone and the Content ID; the user's project is never removed.
* Image sources: `fw::fpkg::registerSourceStager(std::shared_ptr<SourceStager>)` (SourceStager.h). The stager copies
  the image's tree into `<job temp>/stage`; the engine then finds the app root (first folder with `sce_sys`,
  breadth-first, depth ≤ 3). Without a registered stager `validate()` reports that images are not available yet.
* Add-ons: `prospero_ac` project with the entitlement key and no `chunk_info`; the add-on's own `param.json` is packaged
  unchanged; `prospero_al` refused. `addonsFromDlcEmu(gameFolder)` lists `dlc_emu.ini` entries for the dialog.
* Two builds of the same Content ID into the same folder never pack at the same time (the second waits).
* Free space: output ≈ 1.1 × data (× 2 for updates) + generated files + the reserve; staging 1.02 × image; FAT/FAT32
  output warns.

## Not implemented / differences

| Item | Status |
|---|---|
| Built-in engine, Kraken, PFS v3, Native image mode, Homebrew/DLC kinds via LibProsperoPkg | out of scope (decision) |
| Online metadata restore | out of scope (decision) |
| Deleting OS junk from the source | out: the source is never modified; junk is left out and logged |
| Legacy (non-plaintext) post-conversion | out: a package without the plaintext marker is a failure |
| SELF `.sceversion` trailer re-padding | not implemented: only the signature is replaced (trailer rules UNVERIFIED) |
| Confirming DLC-emulator items inside the built package; logging the tool's `sdkVersion` rewrite | not implemented (`img_file_list` / `img_info` output formats UNVERIFIED) |
| "Base built by the Publishing Tools" probe | not implemented (needs keys or LibProsperoPkg); logged as a requirement |
| Selective extraction of an update's base | not possible with `img_extract`: the whole base is extracted unless `baseFolder` is given |
| `img_verify` failure after a build | reported as verification *Inconclusive* with a warning, package kept (the parity document calls it a warning; whether `img_verify` accepts plaintext debug packages is UNVERIFIED). `fpkg.verify` jobs report it as *Failed*. |
| Pre-scan | runs on every OS when on (the reference: Windows only) |
| Numbers in `param.json` | kept as written (core `OrderedJson`); Python would print `1.50` as `1.5` |
| Non-presentation PNGs | only checked when a `.dds` of the same name exists |

## UNVERIFIED (only tested against the stand-in or our own fixtures)

* Everything about the real tool: argument order, the `=` bar through a pipe, `[Error]` texts, exit codes, the delta
  having no FIH header, `img_verify` / `img_extract` behaviour and output layout.
* Layout C: the `plgx` section layout inside chunk/scenario records (PS4-style offsets assumed: language mask at +0x10,
  label offset at +0x1C, scenario chunk count at +0x16, chunk list offset at +0x18), the FICM/FLT pairing, the PlayGo
  language id order (system-parameter numbering) and that the hash table uses the PS5 flat-path hash. The hash is ported
  from LibProsperoPkg (`ProsperoPs5FlatPathTable.HashBytes`, Copyright (C) 2026 SvenGDK, GPL-3.0-or-later,
  github.com/drakmor/LibProsperoPKG) and checked against a second port in `make_goldens.py`.
* UCP layout and digest coverage (format facts from LibProsperoPkg `ProsperoUcp`, GPL-3.0-or-later).
* Console results of every PlayGo, empty-folder and DLC-mount rule; `prospero_ac` add-ons.

## Tests

`native/tests/engines/fpkg/` (link only `fw_engine_fpkg` + core test support):

* `tst_fpkg_rules` — naming, `param.json` patch, size class, exclusion/leftover rules, scanner order/empty
  folders/overlay/links/cancellation, `dlc_emu.ini`, scenario rules, payloads/fillers, `plgx`/FICM/FLT parsing, hash
  vectors, GP5 escaping, goldens for layouts A/B/C + scenario + `param.json`, planning errors, PNG/DDS/SELF/UCP,
  output parser across random chunk splits, header check, conflict policies, settings and legacy keys, toolchain lookup
  and probes, stagers.
* `tst_fpkg_build` — jobs end to end against the stand-in: full build (argv, TEMP/TMP, cwd, logs, cleanup, progress),
  project contents and compression level, stock/`[Error]`/crash/no-output failures (the `0xC0000135`/`0xC000007B`
  rows exist only on Windows, where the stand-in can return such exit codes; they have not been run yet), bar
  variants, `img_verify` failure, conflict policies, cancellation (process tree incl. a grandchild, no partials),
  CPU-core limit and live change (affinity observed in the stand-in), updates (base folder, extracted base, exact
  source, outputs, version/ID/nothing checks), add-ons, verify/extract jobs, non-ASCII folders through links, `.gp5`
  and image sources, DDS and SELF repairs, same-package serialisation, job factory and `validate()`.
* `extra.cmake` builds `fw_fake_pubcmd` and passes its path as `FW_FAKE_PUBCMD`; regenerate goldens with
  `python3 native/tests/data/fpkg/make_goldens.py`.
