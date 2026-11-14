# FPKG parity: reference inventory and the native C++ plan

> **Status:** a record from the v5 rewrite, not kept current. It was the input for writing the native FPKG engine; the proposed file names, setting keys, defaults and the "owner decisions" below are the plan of that time. `engines/FPKG.md` and `docs/FPKG.md` describe the engine as built.

This document is the input for writing the native FPKG engine (`native/src/engines/fpkg/`). It lists everything the
third-party reference app does for PS5 packages, every setting it exposes, the exact way it drives Sony's Publishing
Tools, how our Python engine (`core/fpkg/`) compares, and a proposed C++ design with a test plan.

Conventions:

* **Reference paths** are abbreviated: `C:` = `src/PsViethoa.FpkgBuilder.Core/`, `A:` = `src/PsViethoa.FpkgBuilder.App/`,
  `L:` = `src/PsViethoa.FpkgBuilder.Cli/`, `K:` = `libs/sony-sdk/` (the bundled toolkit), `BR` = `C:Models/BuildRequest.cs`,
  `AS` = `A:Services/AppSettings.cs`, `MW` = `A:Views/MainWindow.axaml`, `VM` = `A:ViewModels/MainViewModel.cs`.
  `file:N` is a line, `file:N-M` a range, all at commit `6e43d85`.
* **Our paths** are repository-relative (`core/fpkg/scan.py`, `native/src/...`).
* **UNVERIFIED** marks anything that could not be checked from source, binaries or a run.
* No code or text of the reference is copied. Everything below is a description of its behaviour in our own words. Short identifiers
  (setting keys, CLI flags, XML element and attribute names, file names, numeric format constants) are quoted because
  they are facts about the tools and file formats. No reference code, UI text or data table is reproduced.

---

## 1. What was inspected, and what we may use

### 1.1 Version

| Item | Value | How it was checked |
|---|---|---|
| Reference repository | `github.com/thanhsondev/PSVIETHOA-FPKG-Builder` | shallow clone |
| Commit inspected | `6e43d85b84a34b282378763690bd130e7917da9b` (2026-11-02, author Nguyễn Thanh Sơn) | `git log` |
| Newest upstream ref | `HEAD` = `refs/heads/main` = `refs/tags/v3.0.0` = `6e43d85` | `git ls-remote` (2026-11-04). No newer commit or tag exists. Open PR heads: `refs/pull/2/head` `a87f928`, `refs/pull/3/head` `4832a68` (= tag v2.2.5); not inspected. |
| App version | 3.0.0 (`Directory.Build.props` `<Version>3.0.0</Version>`) | matches the owner's screenshots (title bar "PSVIETHOA FPKG Builder 3.0.0") |
| Built-in engine | `libs/LibProsperoPkg.dll`, assembly version `1.2.0+a3ee4c1a6c37c6689717416c4962fa544e063e2c`, copyright attribute "SvenGDK 2026", taken from Drakmor's fpkg-gui 0.6.12 | strings of the DLL; PDB Source Link maps to `raw.githubusercontent.com/drakmor/LibProsperoPKG/a3ee4c1…` |
| Sony toolkit profile | `sdk279-plaintext-direct-v3` (Publishing Tools 2.79) | `C:Services/SonySdkToolchain.cs:48`, `K:README.md`, CHANGELOG 2.2.5 |
| Screenshots | `images/1.webp`–`4.webp`: Build PKG page of v3.0.0 on Windows (Sony SDK on, 16 cores, LibProsperoPkg 1.2.0). `images/5.webp` shows a different program (PS5 Backport GUI beta 0.3.4 by Deckerr97) and is out of scope for this document. | converted to PNG and viewed |

Our previous research (`docs/farewell/FPKG_RESEARCH.md`) names the same commit. It was re-verified against the files;
the corrections are listed in §7.3.

### 1.2 Licence status of each part of the reference

| Part | Where | Licence / origin | We may | We may not |
|---|---|---|---|---|
| App, Core, CLI, tests, scripts, docs, CHANGELOG, `vi.json`/`en.json`, design system | `src/`, `tests/`, `scripts/`, `docs/`, `design-system/` | Its authors' own terms (not ours to copy) | Read for behaviour; quote facts (flags, file names, format constants) | Copy code, UI strings, localisation text, data tables, test fixtures, icons, screenshots |
| Toolkit launchers and GP5 script | `K:build.bat`, `K:build-from-folder.ps1`, `K:build-gui.ps1`, `K:scripts/create-gp5-from-folder.py` | Not ours (part of the reference's toolkit folder) | Read for the tool's argument conventions | Copy, vendor, or depend on them |
| Sony Publishing Tools 2.79, as the reference expects them | `K:toolchain/prospero-pub-cmd.exe`, `libScePubTools.dll`, `ext/{da,fa,p2d,ric,sc2}.exe`, `ext/ispc_texcomp.dll`, `ext/libatrac9.dll`; also `libs/libScePubTools.dll` | Sony proprietary SDK | Drive the **user's own copy** through its command line | Bundle, redistribute, download, link to, or ship any part of it |
| `prospero-dds2png.exe` | `K:prospero-dds2png.exe` | Not ours (UNVERIFIED whether it is a Sony or a third-party tool) | Run the user's copy if they have it | Bundle it |
| LibProsperoPkg ("built-in engine", credited to Drakmor) | `libs/LibProsperoPkg.dll/.pdb/.xml` | Upstream `drakmor/LibProsperoPKG` is **GPL-3.0-or-later** (LICENSE + NOTICE "Copyright (C) 2026 SvenGDK", `<PackageLicenseExpression>GPL-3.0-or-later`), verified on `main` `be79990` and `feature/full-ppr-pkg-roundtrip` `5e26c8a` (version 1.2.0, author Roman Tarasov). The exact shipped commit `a3ee4c1` is **not fetchable** ("not our ref"), so the corresponding source of the shipped DLL is not public. | Study the public GPL source for format facts, credited in the third-party notices | Redistribute the reference's DLL |
| LibOrbisPkg (PS4) | `libs/LibOrbisPkg/` | LGPL-3.0 (Maxton), source zip included | Not needed for PS5 FPKG | — |
| PS4 Fake PKG Tools 3.87 (CyB1K patch) + "fast engine" byte patch | `libs/orbis-pub/` | Sony PS4 SDK binaries, patched; no licence | Nothing | Bundle or patch |
| garlic-savemgr | `libs/garlic-savemgr/` | earthonion; source included, **no licence file found** (UNVERIFIED) | Nothing for FPKG | — |
| Wine, Dokan, fonts, icons | bundled by their release scripts | LGPL-2.1 / MIT / OFL / Apache | Not relevant to the engine | — |

**Bottom line.** The native engine must be our own code driving the **user-supplied** `prospero-pub-cmd.exe`. The
"built-in engine" path (LibProsperoPkg: PFS v2/v3, Kraken, shuffle, NAPS, CNT signing with bundled debug keys) is out
of scope: its shipped binary has no public source, it embeds debug key material, and porting the public GPL source is
only possible under GPL terms (owner decision, see §7.1).

---

## 2. Feature inventory of the reference

### 2.1 Pages

| # | Section / page | What it is | Engine | In scope for native FPKG |
|---|---|---|---|---|
| 01 | PS5 FPKG · **Build PKG** | Folder / `.exfat` / `.ffpfsc` / `.ffpkg` / `.gp5` → debug FPKG | Sony SDK (default) or LibProsperoPkg | Yes (Sony SDK path) |
| 02 | PS5 FPKG · **Build Update PKG** | Same page in "update" mode: delta against a base `.pkg` | Sony SDK only | Yes |
| 03 | PS5 FPKG · **Extract PKG** | Inspect, verify, list, preview, extract a `.pkg` | LibProsperoPkg | Partly (via `img_verify` / `img_extract`) |
| 04 | PS5 FPKG · **Queue** | Many sources, 1–4 builds in parallel | as Build PKG | Yes (native job queue) |
| — | Build PKG · **Build DLC packages** (button when `dlc_emu.ini` is found) | One add-on package per `[PSAC]` entry | LibProsperoPkg only | Experimental Sony `prospero_ac` path only |
| — | Extract PKG · **Export DLC template** | `sce_sys` + `.gp5` for rebuilding an add-on | LibProsperoPkg | No |
| 05–09 | PS4 PKG · Extract PKG, Build Base, Build Update, Build DLC, Merge Base + Update | PS4 fake PKGs | LibOrbisPkg + orbis-pub-cmd 3.87 (patched) | No (PS4, patched Sony binaries) |
| 10 | PS5 Connect · **PS5 FTP** | Two-pane FTP client, multi-connection queue | own FTP client | Not FPKG (belongs to `native/src/connect/ps5`) |
| 11 | Save Game · **Resign Save** | Re-own PS4/PS5 saves; garlic-savemgr payload | own + payload | No |
| — | Header / footer | "Debug keys: ready" chip, update badge, language, theme, help | — | Only the dependency status idea |
| — | `fpkg-cli` | Headless build/verify/extract/DLC/PS4 commands | as above | Optional later (headless mode of the native app) |

### 2.2 Build PKG (page 01)

| Aspect | Behaviour | References |
|---|---|---|
| Inputs | Source: app folder with `sce_sys`, `.exfat` (bare, MBR or GPT; app root = first folder with `sce_sys`, breadth-first, depth ≤ 3), `.ffpfsc` (PFS container holding a PFSC-compressed exFAT image), `.ffpkg` (UFS2 image), `.gp5` project. Output folder (auto "`<source>-pkg`" next to source). Temp folder (auto). Package details (Content ID, title, version, kind, image mode). All options of §3. | `MW:137-204`, `C:Services/SourceLocator.cs:44,130-186`, `C:Services/BuildPreparer.cs:31-42` |
| Engine choice | Sony SDK is used when: "Sony SDK" is ticked, kind = Application, image mode = PLAINTEXT_NOAUTH, source is not `.gp5`, the toolchain is found (and Wine/Rosetta on macOS/Linux), and no `sce_sys/trophy2\|uds/*.ucp` is unreadable. Otherwise the built-in engine builds the package, with a log line explaining why. An **update** never falls back: it stops with an error. | `C:Services/BuildEngine.cs:62-136`, `C:Services/SonySdkBuilder.cs:31-41` |
| Pre-flight | Field validation (§3.2), keystone exactly 96 bytes (Sony path), output/temp not inside source, disk-space estimate with a confirm dialog (output ≈ 1.1 × source; temp 1.1 × source for built-in, 0 for Sony + 1.02 × staging), FAT/FAT32 warning, junk-file dialog (may **delete** OS junk from the source after confirmation), output-conflict dialog (overwrite / keep old renamed "`name (1).pkg`" / cancel), AMPR warning when `eboot.bin` imports `libSceAmpr`. | `C:Services/BuildPreparer.cs:110-310`, `C:Services/BuildEngine.cs:918-956,986-1022,1169-1191`, `C:Services/DiskSpaceAdvisor.cs:13-32`, `VM:2952-3010`, `C:Services/OutputConflict.cs:25-125` |
| Pipeline (Sony SDK) | 1 runtime check (VC++ x64 on Windows, auto-installs a bundled `vc_redist.x64.exe /install /quiet /norestart` with UAC; Wine prefix elsewhere) → 2 source preparation (mount via Dokan/hdiutil or extract the image to `<temp>/exfat-<name>-<hash>-<pid>`) → 3 optional online metadata restore → 4 plan (exclusions, DLC-emu mount folders, PlayGo structure, param patch) → 5 ASCII aliases → 6 `[1/3]` GP5 + scenario + `.gp5-assets` → 7 optional pre-scan → 8 `[2/3]` `img_create` → 9 `[3/3]` rename `.partial.pkg` (or legacy convert) → 10 structure check + quick content check (LibProsperoPkg) [+ full check] [+ SHA-256] → 11 confirm DLC-emu items inside the package, log `sdkVersion` change → 12 cleanup. | `C:Services/BuildEngine.cs:34-408,772-916`, `C:Services/SonySdkBuilder.cs:74-481` |
| Outputs | `<ContentID>-A<vv>-V<vv>.pkg` in the output folder; `<stem>-build-logs/` (`01-create-gp5.log`, `02-img-create.log`, `03-postprocess.log` only for legacy toolkits); with "Keep the GP5…": `<stem>.gp5`, `<stem>.playgo-scenario.json`, `.gp5-assets/<stem>/`. | `C:Services/SonySdkBuilder.cs:50-70,119-126,487-533` |
| Naming | `<vv>` is the package-name component of the content version (2-digit major + minor, e.g. `01.000.000` → `0100`, `01.512.000` → `01512`); **A and V both come from `contentVersion`** (the Version field). | `C:Services/SonySdkBuilder.cs:56-60` (component computed inside LibProsperoPkg: `ProsperoContentVersion.PackageNameComponent`, exact rule for minor ≥ 100 UNVERIFIED beyond the two examples in our research) |
| Verification | (a) FIH magic `7F 'F' 'I' 'H'`, size ≥ 4096, signed byte (offset 5) = 0, outer superblock offset (u64 LE at 0x20) + 896 ≤ size, superblock mode (u16 at +0x1C) = 13, marker `PPRPLAIN-NOAUTH!` at superblock + 880; (b) LibProsperoPkg quick content check (CNT signature, PlayGo layout, NAPS, inodes, SI) always; (c) full check (decrypt + test-decompress every block/file) when ticked; (d) SHA-256 when ticked. A failed check is a failed build. The reference does **not** run Sony `img_verify` after a build. | `C:Services/PackageVerifier.cs:24-198`, `C:Services/BuildEngine.cs:837-916` |
| Cancellation | Cooperative token everywhere. During `img_create`: kill the whole process tree; on Wine also `wineserver -k` when no other SDK run is active. `02-img-create.log` gets `exit_code=cancelled`. `.partial.pkg`, `.partial.pkg.remastered.pkg`, `.partial.pkg.naps_metric.json` are deleted; GP5 and logs are kept. | `C:Services/SonySdkRunner.cs:355-385`, `C:Services/SonySdkBuilder.cs:383-388,466-473` |
| Temp and cleanup | Sony path: `<temp>/sdk-tmp-<8 hex>` is `TEMP`/`TMP` of `img_create`, removed in `finally`; image staging and patch-base extraction removed in `finally`; aliases removed on dispose (stale ones swept by PID at the next run); mounts unmounted in `finally`; GP5/scenario/assets removed after success unless kept; `.naps_metric.json` always removed after success; build logs always kept. | `C:Services/SonySdkBuilder.cs:343-347,474-511`, `C:Services/BuildEngine.cs:385-408`, `C:Services/SonySdkPathAliases.cs:59-125` |
| Progress | Phase weights (Sony): runtime 1.5, project 0.5, pre-scan 3, image 90, convert 1, verify 1, SHA-256 6, full verify 8, mount 0.5, extract 8. Image phase percent = count of `=` / 51; bytes written polled every 500 ms. | `C:Services/PhaseCatalog.cs:19-49,56-78`, `C:Services/SonySdkRunner.cs:25,124-147` |

### 2.3 Build Update PKG (page 02)

| Aspect | Behaviour | References |
|---|---|---|
| Inputs | Source: a full new-version folder **or** an "update folder" with only changed/added files (may lack `sce_sys`). Base package: the `.pkg` installed on the console (not a `.remastered.pkg`). Optional "Original game folder" (the base dump) to avoid extracting from the base package. Files to write: Update only / Full game with update / Both. Version bump button (suggests middle group + 1: `01.000.000` → `01.001.000`, `01.999.x` → `02.000.000`). | `MW:295-374`, `VM:996-1145,1166-1200`, `C:Services/SonySdkPatchReference.cs:108-128` |
| Base checks (before compressing) | Exists; extension `.pkg`; not a `.remastered.pkg`; readable; has a supplement (NAPS) region; was **built by Publishing Tools** (probe of `prev_suppl`'s `naps_meta_18.dat` through LibProsperoPkg: decrypts with the library key → built-in engine → refused; signal also visible as a 48-byte vs 72-byte `naps_meta_300` descriptor); same Content ID; new `contentVersion` strictly higher (numeric per dot group); base is not the file being written. | `C:Services/SonySdkPatchReference.cs:19-85`, `C:Services/SonySdkReferenceProbe.cs:18-89`, `C:Services/SonySdkBuilder.cs:143-165` |
| Update-folder merge | If the source lacks any real game file of the base (case-insensitive path compare; `sce_sys/`, `playgo-languages/`, `playgo-chunks/` and script-excluded files do not count) or lacks `sce_sys/param.json`: the missing files are read from the base folder, or **only those files** are extracted from the base package into `<temp>/patch-base-…` (free space checked: missing bytes + 256 MiB), all base directories are recreated, and the CNT `sce_sys` is exported. The GP5 walker merges both trees (update wins, base spelling kept for replaced paths). An update that replaces/adds nothing is refused. Content ID and version default from the base when the update folder has no `param.json`. | `C:Services/BuildEngine.cs:422-556,214-260`, `C:Services/SonySdkProject.cs:1237-1415` |
| Pipeline | As Build PKG with `--ref_pkg_path <base>`; `attributePub` is **not** auto-sized; online metadata restore does not apply. | `C:Services/SonySdkBuilder.cs:207-216`, `C:Services/BuildEngine.cs:1532-1546` |
| Outputs | Tool writes `<stem>.partial.pkg` (delta, no FIH header) and `<stem>.partial.pkg.remastered.pkg` (full image). Renamed to `UPDATE_<ContentID>-A<vv>-V<vv>.pkg` and `<ContentID>-A<vv>-V<vv>.remastered.pkg`. The unwanted one is deleted **after** the structure check. Install note logged (base first, then the patch). | `C:Services/SonySdkBuilder.cs:44-53,401-417`, `C:Services/BuildEngine.cs:772-830` |
| Verification | Structure check on the `.remastered.pkg` only (the delta has no FIH); LibProsperoPkg content check is skipped (the remastered image references base data); DLC-emu confirmation on the remastered package. | `C:Services/BuildEngine.cs:792-830` |
| Conflicts | Only `UPDATE_<id>-….pkg` and `<id>-….remastered.pkg` count as conflicts; the base package is never offered for overwrite. | `C:Services/OutputConflict.cs:56-67`, `C:Services/BuildEngine.cs:986-1022` |
| CLI-only extras | `--sdk-patch-exact` (treat source as complete: missing = deleted), `--sdk-build-base` (build the base from `--sdk-base-folder` first, then the update against it). | `L:CommandLine.cs:614-645,765-790` |

### 2.4 Extract PKG (page 03)

| Aspect | Behaviour | References |
|---|---|---|
| Inputs | `.pkg`; output folder (auto "`<pkg>-extract`"); passcode (32 zeros); what to extract: app tree in "Sony layout" (default on), outer layer (`outer/`), raw CNT entries (`cnt/`), SI package (`si/`), decompress PFSC (default on); file selection with filter / select all / none / invert. | `A:Views/ExtractionView.axaml:116-232`, `A:ViewModels/ExtractionViewModel.cs:169-176,644-680` |
| Info | General block, every `param.json` field, icon, SHA-256 on demand, quick check, full check, retail packages "information only". Preview of a file (text, PNG/JPEG, 4 KiB hex). | `A:Views/ExtractionView.axaml:242-540` |
| Pipeline | LibProsperoPkg reader: PLAINTEXT_NOAUTH read in place (NAPS decompressed on demand), Native (AES-XTS) decrypted to temp first; app tree extraction then CNT `param.json`/icons/PlayGo tables merged into `sce_sys` so the folder can be rebuilt. | `A:ViewModels/ExtractionViewModel.cs:1300-1440`, `C:Services/PackageReader.cs` |
| Safety | Non-empty output folder → confirm dialog. Cancel button. | `A:ViewModels/ExtractionViewModel.cs:1316-1335` |
| Export DLC template | `<pkg>-dlc-template` with `sce_sys` + `<ContentID>.gp5` (DLC type, passcode, entitlement key, original PlayGo) for add-on packages. | `A:ViewModels/ExtractionViewModel.cs:1637-1710` |

### 2.5 Queue (page 04)

| Aspect | Behaviour | References |
|---|---|---|
| Inputs | Sources added by picker or drag-drop; a folder that is not itself a source but holds game folders adds each child. Per-item output folder (default "`<source>-pkg`" when Build PKG's "Auto from source" is on). Concurrency 1–4 (default 2). | `A:ViewModels/QueueViewModel.cs:13-75,156-260`, `AS:177` |
| Settings | Taken from the Build PKG page **when each item starts**; title/version from the item's own `param.json`; no title/version rewrite; no metadata URL; never an update (always a full package). | `VM:433-455` |
| Scheduling | Start / pause (pause = no new starts), cancel all, cancel item, retry, remove, clear finished, move up/down, open output, copy log; two items with the same Content ID and output folder never run together; Wine prefix creation and VC++ install serialised; each run has its own temp/alias/mirror folders. | `A:ViewModels/QueueViewModel.cs:306-520`, `C:Services/SonySdkBuilder.cs:25,91-113`, `C:Services/SonySdkRunner.cs:40-70` |

### 2.6 Build DLC packages (Build PKG sub-feature) and `dlc_emu.ini`

| Aspect | Behaviour | References |
|---|---|---|
| `dlc_emu.ini` | drakmor/dlc_emu format: blocks `[PSAC]` (and others such as `[PSAL]`, `[PSCONS]`, skipped) with `content_id`, `download_status`, optional `mount_point` (`/app0/<dir>`); entries without `content_id` skipped, duplicates dropped, order kept; file capped at 64 KiB. | `C:Services/DlcEmuIni.cs:5-150` |
| Build | Every `[PSAC]` entry → LibProsperoPkg `AdditionalContentData`, debug image, version `01.000.000`, passcode 32 zeros, `GenerateParamJsonIfMissing`, deterministic, image mode from the Build tab (PLAINTEXT_NOAUTH default). Payload: empty folder for `NO_EXTRA_DATA`; otherwise the dump's `mount_point` folder if it has files, else an unlock-only package with a warning. Title "`<game> — <label>`". | `C:Services/DlcPackageBuilder.cs:38-172` |
| Not done | PSAL (entitlement-only) packages: bare CNT, the PS5 debug installer refuses them. Never built through Sony tools. | `C:Services/DlcPackageBuilder.cs:20-29` |

### 2.7 PS4 pages, FTP, Resign Save (out of scope, summary only)

| Page | Mechanism (one line) |
|---|---|
| PS4 Extract | LibOrbisPkg; PFS opened with passcode or recovered FPKG key; output `CUSAxxxxx-base/-update/-dlc` (`image0` + `Sc0`). |
| PS4 Build Base / Update / DLC | GP4 generated from the folder; `orbis-pub-cmd img_create --oformat pkg --no_progress_bar [--skip_digest] --tmp_path <tmp> <gp4> <dir>`; optional `img_verify --passcode … --integrity_check on --format_check off`. |
| PS4 Merge Base + Update | One base PKG with the update merged (update files win, keystone from base, APP_VER 01.00, VERSION from update). |
| PS5 FTP | Two panes, drag-drop, parallel files, segmented `REST` download, resume, ETA, saved hosts (password never saved). |
| Resign Save | Offline `param.sfo` patch (`ACCOUNT_ID`, optional `TITLE_ID`, `.bak`), encrypted saves via garlic-savemgr on the console (ELF loader port 9021, HTTP 8082). |

### 2.8 Components & plugins (Build PKG, advanced)

Rows, each with a real probe: LibProsperoPkg engine version; PS5 debug keys; built-in Kraken encoder; Sony SDK
(toolchain folder + VC++ or Wine version); PS4 tools; native Oodle (`libScePubTools.dll`); copy-free mounting (hdiutil /
Dokan driver running); keep-awake mechanism. "Copy report" and "Check again" buttons. Screenshot 2 shows "8/8 components
ready". `C:Services/ComponentProbe.cs:18-178`, `MW:580-604`.

---

## 3. Settings

### 3.1 Every setting the reference exposes or uses

Columns: **Path** = which engine the setting affects: **SDK** (Sony Publishing Tools path), **BI** (built-in
LibProsperoPkg path), **both**, **app**. **Effect** is the concrete mechanism.

#### 3.1.1 Build PKG · Step 1 "Source & output folders"

| UI label | Key(s) | Type / values | Default | Path | Effect | Refs |
|---|---|---|---|---|---|---|
| Source (Game folder…, .exfat image, .ffpfsc file, .ffpkg file, .gp5 project; drop zone) | `AS.SourcePath`, `BR.SourcePath` | path: folder with `sce_sys`, `.exfat`, `.ffpfsc`, `.ffpkg`, `.gp5` | "" | both | Source resolution (§2.2). `.gp5` forces the built-in engine and `SourceMode=Gp5Project`. | `AS:10`, `BR:23`, `MW:141-175`, `C:Services/BuildPreparer.cs:99-104` |
| Recent sources / Clear history | `AS.RecentSources` | list of paths | [] | app | Picker history only. | `AS:188`, `MW:181-204` |
| Output folder for the .pkg | `AS.OutputFolder`, `BR.OutputFolder` | folder | "" (suggested `<source>-pkg` beside source; for `.gp5` beside the project's parent) | both | Where the package, logs and GP5 go; must not be inside the source (or the `.gp5` folder). | `AS:20`, `BR:28`, `MW:272-287`, `C:Services/BuildPreparer.cs:31-42,188-222` |
| Auto from source | `AS.OutputFolderAuto` (+ one-time migration `OutputAutoRevision`) | bool | true | app | Output follows the source on every pick. | `AS:23,94-96`, `MW:273` |
| Clean now (junk card) | — (action) | — | — | both | Deletes OS junk (`.DS_Store`, `Thumbs.db`, `ehthumbs*.db`, `desktop.ini`, `.localized`, `._*`, `*.url`; folders `__MACOSX`, `.Spotlight-V100`, `.fseventsd`, `.Trashes`, `.TemporaryItems`, `.AppleDouble`, `$RECYCLE.BIN`, `System Volume Information`) **from the source**; also offered by a dialog at build time. Images: read-only, only skipped. | `MW:257-264`, `VM:2985-3010`, `C:Services/JunkFileFinder.cs:12-40,145-180` |

#### 3.1.2 Build PKG · Step 2 "Package details"

| UI label | Key(s) | Type / values | Default | Path | Effect | Refs |
|---|---|---|---|---|---|---|
| Content ID (+ Suggest) | `AS.ContentId`, `BR.ContentId` | 36 chars `^[A-Z]{2}[0-9]{4}-[A-Z]{4}[0-9]{5}_00-[A-Z0-9]{16}$` (UI validator requires `_00`; the GP5 step accepts any two digits) | from `param.json` | both | Output name, conflict search, patch checks. **SDK: the GP5 `content_id` is always read from `param.json`, not from this field.** Suggest (no `param.json` only): `UP9000-<TitleID or PPSA00000>_00-<title letters/digits, padded with 0 to 16>`. | `AS:27`, `BR:33`, `MW:394-405`, `C:Services/ContentIdHelper.cs:9-71`, `C:Services/SonySdkProject.cs:1571-1602` |
| App title | `AS.Title`, `BR.Title` | text | from `param.json` | both | SDK: written to `localizedParameters.<defaultLanguage>.titleName` of the normalised `param.json` when it differs (UI always enables this). BI: title for a generated `param.json`. | `AS:29`, `BR:35,162-176`, `C:Services/ParamJsonPatch.cs:198-210` |
| Content version | `AS.Version`, `BR.Version` | `NN.NNN.NNN` (legacy `NN.NN` canonicalised) | `01.000.000` / from `param.json` | both | SDK: `contentVersion` written when it differs. Output name. Patch version check. | `AS:31`, `BR:37`, `C:Services/ParamJsonPatch.cs:186-196`, `C:Services/VersionHelper.cs:8-28` |
| Package type | `AS.Kind`, `BR.Kind` | Application / Homebrew / DLC with data | Application | BI (SDK forces Application) | Library `Mode` (`Application`, `Homebrew`, `AdditionalContentData`). Non-app → SDK not applicable. | `C:Models/Enums.cs:4-14`, `VM:718-724`, `C:Services/BuildEngine.cs:1569-1574` |
| Image mode (outer layer) | `AS.ImageMode`, `BR.ImageMode` | PLAINTEXT_NOAUTH / Native (AES-XTS) | PLAINTEXT_NOAUTH | BI (SDK forces plaintext) | Library `PublisherImageMode`; verifier expects the marker only for plaintext. | `C:Models/Enums.cs:17-24`, `C:Services/BuildEngine.cs:1592-1594` |

#### 3.1.3 Build PKG · Step 3 "Speed & compression" presets

| Preset | Built-in engine meaning | Sony SDK meaning | Default | Refs |
|---|---|---|---|---|
| Fast | Kraken level 2 | `--compression_level 2` | — | `C:Services/BuildPresets.cs:12`, `VM:295-305` |
| Standard | Kraken level 4 (Normal) | `--compression_level 4` | **BI default** | `C:Services/BuildPresets.cs:14,21` |
| Smallest | Kraken level 7 (Optimal3) | no flag (tool default 7) | **SDK default** | `C:Services/BuildPresets.cs:16`, `VM:298-305` |
| Maximum | Kraken 9 + PFS v3 + shuffle analysis | `--compression_level 9` | — | `C:Services/BuildPresets.cs:19` |
| Custom | any other combination | any other SDK level from the advanced list | — | `VM:820-825` |

Screenshot 1 shows "Standard" selected with Sony SDK on, while screenshot 2 shows the SDK level box empty. Whether that is
the owner's saved level 4 or a display glitch is UNVERIFIED; the code default is "not passed".

#### 3.1.4 Advanced options · "Sony SDK options" (visible only when the SDK is active)

| UI label | Key(s) | Type / values | Default | Path | Effect | Refs |
|---|---|---|---|---|---|---|
| Publishing Tools compression level | `AS.SdkCompressionLevel`, `BR.SdkCompressionLevel`; CLI `--sdk-compression-level` | int? : default (not passed), 9, 8, 6, 5, 4, 3, 2, 1, 0, −1, −2, −3, −4 | not passed (= 7) | SDK | `img_create … --compression_level N`; validated −4..9 (tool message: level must be −4..9). | `AS:77`, `BR:195`, `VM:274`, `MW:529-531`, `C:Services/SonySdkRunner.cs:91-95`, `C:Services/BuildPreparer.cs:244-248` |
| PlayGo fallback when the source has no playgo-chunk.dat | `AS.SdkPlayGoFallback`, `BR.SdkPlayGoFallback`; CLI `--no-sdk-playgo-fallback` | bool | true | SDK | No valid `sce_sys/playgo-chunk.dat` → "script layout" (§4.6 B): N chunks, one chunk per language with a 1 MiB payload, scenarios from the source's `playgo-scenario.json`. Off → minimal 1-chunk/1-scenario `chunk_info` (§4.6 A). | `AS:68`, `BR:230`, `MW:535-538`, `C:Services/BuildEngine.cs:696-740` |
| Keep empty folders in the package | `AS.SdkKeepEmptyFolders`, `BR.SdkKeepEmptyFolders`; CLI `--sdk-drop-empty-folders` | bool | true | SDK | `<empty_dir dst_path="…" />` for every empty (or junk-only) source folder outside `sce_sys` (§4.5). DLC-emu mount folders are declared regardless. | `AS:71`, `BR:238`, `MW:543-546`, `C:Services/SonySdkProject.cs:640-649,692-732,1555-1560` |
| Fill empty PlayGo chunks | `AS.SdkFillEmptyChunks`, `BR.SdkFillEmptyChunks`; CLI `--sdk-keep-empty-chunks` | bool | true | SDK | Each chunk left without data gets `playgo-chunks/NNN.bin` (1 MiB zeros, `pfs_compression="disable"`). | `AS:74`, `BR:246`, `MW:551-554`, `C:Services/SonySdkProject.cs:1079-1117` |
| Pre-scan source files in parallel (Windows Defender) | `AS.SdkPrescan`, `BR.SdkPrescan`; CLI `--no-sdk-prescan` | bool (Windows only) | true | SDK | Before `img_create`, open every source file read-only and read 1 byte, parallelism = clamp(2 × logical cores, 8, 32). Changes nothing in GP5 or package. | `AS:62`, `BR:189`, `MW:558-562`, `C:Services/SonySdkPrescan.cs:22-60`, `C:Services/SonySdkBuilder.cs:326-340` |
| Keep the GP5 and generated files | `AS.SdkKeepIntermediate`, `BR.SdkKeepIntermediate`; CLI `--sdk-keep-intermediate` | bool | false | SDK | Keeps `<stem>.gp5`, `<stem>.playgo-scenario.json`, `.gp5-assets/<stem>/` after success (logs always kept). | `AS:65`, `BR:201`, `MW:567-570`, `C:Services/SonySdkBuilder.cs:487-511` |

#### 3.1.5 Advanced options · image handling and built-in engine (greyed out when the SDK is active)

| UI label | Key(s) | Type / values | Default | Path | Effect | Refs |
|---|---|---|---|---|---|---|
| exFAT image handling | `AS.ExFat`, `BR.ExFat`; CLI `--exfat auto\|mount\|extract` | Automatic / Mount / Extract | Automatic | both | Mount (macOS hdiutil read-only; Windows Dokan virtual drive that can hide junk and overlay `param.json`) or extract to temp. Containers mount only with Dokan. Built-in + metadata restore → extract. | `AS:12`, `BR:26`, `MW:612-620`, `C:Services/BuildEngine.cs:1463-1499` |
| Enable direct mounting / Get Dokan / Check again | `AS.DokanPromptShown` | actions | — | app | Installs the bundled Dokan MSI with UAC (offered once at first start). | `MW:623-649`, `VM:893-985` |
| Source reading mode | `AS.SourceMode`, `BR.SourceMode`; CLI `--source-mode`, `--project` | Automatic (use a top-level `.gp5`) / Folder | Automatic | BI | Library `SourceMode`. | `AS:15`, `MW:655-658`, `C:Services/BuildEngine.cs:1609-1615` |
| Kraken encoder | `AS.KrakenBackend`, `BR.KrakenBackend`; CLI `--backend` | Automatic / Built-in / Native Oodle (`libScePubTools.dll`) / None (uncompressed) | Automatic | BI | Library `KrakenBackend`; Automatic picks the DLL on Windows x64 when found. | `C:Models/Enums.cs:27-40`, `C:Services/BuildPreparer.cs:313-337`, `MW:662-664` |
| libScePubTools.dll (native Oodle, Windows only) | `AS.PublishingToolsPath`, `BR.PublishingToolsPath`; CLI `--dll`; env `LIBPROSPERO_PUBTOOLS_PATH` | file | auto: env, app dir, `libs/`, `C:\SCE\Prospero\Tools\Publishing Tools\bin\`, `%SCE_PROSPERO_SDK_DIR%\..\Tools\Publishing Tools\bin` | BI | Library `PublishingToolsLibraryPath`. | `AS:114`, `MW:667-674`, `C:Services/PublishingToolsLocator.cs:6-61` |
| Kraken level | `AS.KrakenLevel`, `BR.KrakenLevel`; CLI `--level` | int −4..9 (Oodle names HyperFast4…Optimal5) | 4 | BI | `KrakenCompressionLevel`. | `BR:7-9,48`, `MW:677-685`, `C:Services/BuildPresets.cs:59-76` |
| PFS compression format | `AS.PfsFormat`, `BR.PfsFormat`; CLI `--pfs v2\|v3` | v2 / v3 | v2 | BI | `PfsCompressionFormat`; v3 needs PS5 firmware 7.00+ (per library author). | `C:Models/Enums.cs:56-63`, `MW:689-694` |
| Kraken compression block size | `AS.KrakenBlockKiB`, `BR.KrakenBlockKiB`; CLI `--block-size` | 128 / 192 / 256 KiB (validated 128–256) | 256 | BI | `KrakenCompressionBlockSize`. | `BR:18-20,64`, `VM:254`, `MW:697-699` |
| Pre-compression shuffle pattern (PFS v3) | `AS.ShufflePattern`, `BR.ShufflePattern`; CLI `--shuffle` | None + 18 named patterns | None | BI | `PreCompressionShufflePattern`. | `C:Models/Enums.cs:66-87`, `MW:703-707` |
| Automatic shuffle analysis (PFS v3) | `AS.ShuffleAnalysis`, `BR.ShuffleAnalysis`; CLI `--shuffle-analysis` | bool | false | BI | `EnableShufflePatternAnalysis` (only with v3; full effect at level 9). | `MW:710-714` |
| (no UI) | `BR.ShufflePredictionLevel`; CLI `--shuffle-prediction-level` | int? −4..9 / auto | auto | BI | `ShufflePredictionCompressionLevel`. | `BR:73`, `L:CommandLine.cs:573-576` |
| Skip the PFS input compatibility check (expert) | `AS.SkipPfsInputCheck`, `BR.SkipPfsInputCheck`; CLI `--skip-pfs-input-check` | bool | false | BI | `SkipPfsInputDataAllowedCheck`. | `MW:718-721` |
| Optimise the physical inner-image layout | `AS.LayoutOptimization`, `BR.LayoutOptimization`; CLI `--no-layout-optimization` | bool | true | BI | `EnableOuterBlockCoalescing` + `EnableRelocationAlignmentAdjustment`. | `BR:79`, `MW:727-730`, `C:Services/BuildEngine.cs:1607-1608` |
| PlayGo layout (built-in engine) + language list | `AS.PreserveSourcePlayGoLayout`, `AS.PlayGoLanguages`; CLI `--playgo-languages a,b\|all` | Source layout / Generate for languages; 31 language check boxes | Source layout; all languages | BI | `PreserveSourcePlayGoLayout`, `OverrideSourcePlayGoLanguages`, `PlayGoLanguageMask`. | `AS:197,200`, `MW:828-849`, `A:ViewModels/MainViewModel.PlayGo.cs:39-115` |
| SDK version + Override | `AS.OverrideSdk`, `AS.SdkMajor`; CLI `--sdk N` | bool + 1..11 | off / 1 | BI | `SdkVersionOverride` (writes `.sceversion`; library sets `requiredSystemSoftwareVersion` too). | `AS:110-112`, `MW:857-867`, `C:Services/SdkVersions.cs` |
| Deterministic build | `AS.Deterministic`, `BR.Deterministic`; CLI `--no-deterministic` | bool | true | BI | `DeterministicBuild`. | `MW:895-898` |

#### 3.1.6 Advanced options · "PlayGo & param.json" and file filtering

| UI label | Key(s) | Type / values | Default | Path | Effect | Refs |
|---|---|---|---|---|---|---|
| Force applicationDrmType = "standard" (also the "DRM standard" toggle beside Build) | `AS.ForceStandardDrm`, `BR.ForceStandardDrm`; CLI `--keep-drm` | bool | true | both | SDK: always forced (UI shows ticked and locked); normalised `param.json` gets `"standard"` (also when the key is missing or not a string). BI: library rewrites in memory. | `BR:85`, `VM:676-690`, `MW:743-746,1012`, `C:Services/SonySdkProject.cs:808-833` |
| Drop every sce_sys/playgo* file of the dump (also beside Build) | `AS.RemovePlayGoFiles`, `BR.RemovePlayGoFiles`; CLI `--remove-playgo` (`--keep-playgo` accepted) | bool | **false** (settings default and 2.2.6 migration; the view-model field initialiser says true but is overwritten on load) | BI (SDK: shown ticked and locked) | BI: mirror omits `sce_sys/playgo*`. SDK: `sce_sys/playgo-*` are always reserved and never mapped; the tables are still **read** to rebuild the structure. | `AS:194`, `BR:105`, `VM:697-716,771,1969-1978`, `MW:751-754,1014-1015` |
| Clear versionFileUri in param.json | `AS.ClearVersionFileUri`, `BR.ClearVersionFileUri`; CLI `--keep-version-uri` | bool | true | both | Sets a non-empty string `versionFileUri` to `""`. | `AS:206`, `BR:130`, `C:Services/ParamJsonPatch.cs:164-168` |
| Clear the attribute3 flags in param.json | `AS.ClearPlayGoAttributes`, `BR.ClearPlayGoAttributes`; CLI `--clear-attribute3` / `--keep-attribute3` | bool | false (migration rev 2) | both | Sets a non-zero numeric `attribute3` to 0 (loses 120 Hz bit 7, VRR bit 19, PS5 Pro Enhanced bit 23). | `AS:212`, `BR:149`, `C:Services/ParamJsonPatch.cs:170-174` |
| Restore metadata online (needs internet) + optional URL | `AS.RestoreMetadataOnline`, `BR.RestoreMetadataOnline`, `BR.MetadataSourceUrl` (URL not persisted); CLI `--restore-metadata`, `--metadata-url` | bool + URL | false | both | LibProsperoPkg `ProsperoMetadataRecovery` downloads the `*_sc.pkg` piece for the exact Content ID/version via `versionFileUri` (TLS pinned to Sony's CA), restores lost `sce_sys` files into a temp copy, which then overlays `sce_sys` (GP5 points at it). Not for `.gp5` sources or updates. Failures only warn. | `AS:209`, `BR:138-141`, `MW:779-785`, `C:Services/MetadataRestore.cs:53-166` |
| Keep the DLC emulator in the package | `AS.KeepDlcEmu`, `BR.KeepDlcEmu`; CLI `--strip-dlc-emu` / `--no-dlc-emu` | bool | true | both | Off: excludes `dlc_emu.ini` and `fakelib\|fakelib2/{libSceAppContent,libSceNpEntitlementAccess,libSceGameUpdate}.{sprx,prx}`. On: declares every existing `mount_point` folder and every root `addcont<N>` folder as `<empty_dir>` when empty, warns about missing mount folders, and re-reads the built package to confirm. | `AS:203`, `BR:124`, `MW:789-792`, `C:Services/DlcEmuInspector.cs:24-47,112-160`, `C:Services/BuildEngine.cs:616-694` |
| Clean up AMPR emu leftovers | `AS.RemoveAmprLeftovers`, `BR.RemoveAmprLeftovers`; CLI `--keep-ampr` | bool | true | both | Excludes `ampr_emu.index`, `fakelib/ampr_emu.index`, `fakelib/libSceAmpr.sprx` (the SDK script already excludes the root index and both fake modules unconditionally). | `AS:216`, `BR:91`, `C:Services/AmprInspector.cs:49-54` |
| Clean up dump leftovers (_DUBLEX_, Unreal "Saved" folders) | `AS.RemoveDumpLeftovers`, `BR.RemoveDumpLeftovers`; CLI `--keep-dump-leftovers` | bool | true | both | Excludes root `_DUBLEX_/`; `Engine/Saved/` (whole `Engine/` when `Saved` is its only child); `<project>/Saved/` for a root folder that has `Content/Paks`. | `AS:219`, `BR:98`, `C:Services/DumpLeftoverInspector.cs:5-110` |
| Lower requiredSystemSoftwareVersion to the game's SDK | `AS.LowerRequiredFirmware`, `BR.LowerRequiredFirmware`; CLI `--keep-required-fw` | bool | true | both (applies only without an SDK override) | If `requiredSystemSoftwareVersion` > (`sdkVersion` & `0xFFFF000000000000`) and that is non-zero, write the latter as `"0x%016x"`. In SDK mode the box is greyed but its value is still applied; Publishing Tools then rewrites the field anyway (§4.10). | `AS:99`, `BR:156,162-169`, `C:Services/ParamJsonPatch.cs:176-183`, `MW:857-872` |
| Passcode (32 ASCII characters) + show/hide | `BR.Passcode` (not persisted; taken from a `.gp5` project when present) | 32 printable ASCII | 32 × `0` | both | SDK: GP5 `<package passcode="…">`. BI: library passcode. | `BR:39`, `VM:621-622,2529,2557`, `MW:878-890` |

#### 3.1.7 Advanced options · threads, PlayGo count, verification, misc

| UI label | Key(s) | Type / values | Default | Path | Effect | Refs |
|---|---|---|---|---|---|---|
| Compression threads (advanced) / CPU cores (beside Build, SDK only) | `AS.Threads`, `BR.Threads`, `BR.LiveThreads`; CLI `--threads`/`-j` | int 0..256; UI shows the machine's core count; stored 0 = every core | 0 (all) | both | SDK: **live** limit while `img_create` runs: Windows sets the process CPU affinity to the first N cores (≤ 64); macOS/Linux throttle with SIGSTOP/SIGCONT every 100 ms. BI: `KrakenMaxDegreeOfParallelism` (fixed at start). | `AS:41`, `BR:51-58`, `VM:239,1684-1697`, `MW:813-817,1049-1052`, `C:Services/CpuLimiter.cs:17-165`, `C:Services/SonySdkRunner.cs:328-333` |
| PlayGo chunks | `AS.PlayGoChunks` (+ migration `PlayGoDefaultsRevision`, current 3), `BR.PlayGoChunks`; CLI `--playgo` | int 1..255 | 100 | both | SDK: chunk count of the fallback "script layout" (enabled only when the fallback is on). BI: fallback chunk count. A source table supplies its own count. | `AS:79-88`, `BR:11-15,267`, `VM:354`, `MW:820-823` |
| Compute SHA-256 after building | `AS.ComputeSha256`, `BR.ComputeSha256`; CLI `--sha256` | bool | false | both | SHA-256 of the final package (4 MiB reads), logged and on the result card. | `MW:902-905`, `C:Services/PackageVerifier.cs:169-197` |
| Full check after building | `AS.FullVerify`, `BR.FullVerify`; CLI `--full-verify` | bool | false | both (not for updates) | LibProsperoPkg `VerifyPackageFull` instead of quick. | `AS:102`, `BR:258`, `MW:909-912` |
| Keep the computer awake while building | `AS.PreventSleep`, `BR.PreventSleep`; CLI `--no-sleep-guard` | bool | true | both | `SetThreadExecutionState` / `caffeinate` / `systemd-inhibit` for the build. | `AS:108`, `BR:280`, `C:Services/SleepInhibitor.cs` |
| Check for updates at startup | `AS.CheckUpdatesOnStartup` | bool | true | app | GitHub Releases, at most every 6 h. | `AS:191` |
| Temporary folder (intermediate images) + Re-suggest | `AS.TemporaryFolder`, `BR.TemporaryFolder`; CLI `--temp` | folder; must not be inside the source | "" → `<output volume root>/fpkg-temp` unless that is the system volume, else `%TEMP%/fpkg-temp` (macOS: `~/Library/Caches/<app>/temp`) | both | Image staging, patch-base extraction, `sdk-tmp-*` (SDK `TEMP`/`TMP`), metadata workspace, alias base candidate. | `AS:25`, `BR:31`, `MW:933-946`, `C:Services/BuildPreparer.cs:48-76` |
| Sony SDK (toggle beside Build) | `AS.UseSonySdk`, `BR.UseSonySdk`; CLI `--no-sony-sdk` / `--builtin-engine` | bool | true | both | Engine selection (§2.2). | `AS:59`, `BR:183`, `MW:1011` |
| Restore defaults | — (action) | — | — | app | Resets every packaging option (not paths, package details, language, theme, history); passcode back to zeros; SDK major follows the source. | `VM:2017-2099`, `MW:1018-1021` |
| Advanced options expanded, auto-scroll log, theme, window size, last page, sidebar collapsed, language (default `vi`) | `AS.AdvancedExpanded`, `AutoScrollLog`, `Theme`, `WindowWidth/Height`, `LastPage`, `SidebarCollapsed`, `Language` | UI state | — | app | Persisted UI state. | `AS:18,116-133` |

#### 3.1.8 Build Update PKG

| UI label | Key(s) | Type / values | Default | Path | Effect | Refs |
|---|---|---|---|---|---|---|
| (page selection) Build an UPDATE package | `VM.PatchEnabled` (not persisted) | bool | page-driven | SDK | Enables `--ref_pkg_path`. | `VM:996`, `MW:295-302` |
| Files to write: Update only / Full game / Both | `VM.UpdateKind` → `BR.SdkPatchOutput`; CLI `--sdk-patch-output update\|full\|both` | enum | **UI: Update only**; `BuildRequest` and CLI: Both | SDK | Deletes the unwanted file after the structure check. | `VM:1043-1075,2860`, `BR:218-219,285-296`, `L:CommandLine.cs:624-629` |
| Base package (.pkg installed on the console) | `VM.ReferencePackagePath` → `BR.SdkReferencePackage`; CLI `--sdk-reference` | file | "" | SDK | `--ref_pkg_path`; checks of §2.3. | `BR:208`, `MW:324-342` |
| Bump version button | — | action | — | SDK | Sets Version to the suggested next version. | `VM:1166-1182` |
| Original game folder (optional — skips the extraction) | `VM.PatchBaseFolder` → `BR.SdkPatchBaseFolder`; CLI `--sdk-base-folder` | folder with `sce_sys/param.json`, not overlapping the source | "" | SDK | Missing files are read from it instead of extracted from the base package. | `BR:222`, `MW:356-374`, `C:Services/BuildEngine.cs:214-230` |
| (CLI only) | `BR.SdkPatchExactSource`; `--sdk-patch-exact` | bool | false | SDK | Source taken as complete (missing = deleted). | `BR:216` |
| (CLI only) | `--sdk-build-base` | flag | — | SDK | Build the base package from `--sdk-base-folder` first, then the update against it. | `L:CommandLine.cs:765-790,820-835` |

#### 3.1.9 Extract PKG and Queue

| UI label | Key(s) | Type / values | Default | Refs |
|---|---|---|---|---|
| .pkg package file | `AS.ExtractPackagePath` | file | "" | `AS:180` |
| Output folder + Auto from package | `AS.ExtractOutputFolder`, `AS.ExtractOutputAuto` | folder / bool | "" / true (`<pkg>-extract`) | `AS:183,186` |
| Extract the original app tree in Sony layout | `ExtractionViewModel.ExtractAppTree` | bool | true | `A:ViewModels/ExtractionViewModel.cs:169` |
| Extract the outer layer | `ExtractOuter` | bool | false | `:170` |
| Also export raw CNT entries | `ExtractCnt` | bool | false | `:171` |
| Extract the SI package | `ExtractSi` | bool | false | `:172` |
| Decompress inner compressed files (PFSC) | `DecompressPfsc` | bool | true | `:173` |
| Package passcode | `Passcode` | 32 chars | zeros | `:175` |
| Compute SHA-256 / Quick check / Full check / Export DLC template | actions | — | — | `A:Views/ExtractionView.axaml:246-279` |
| Queue: builds at once | `AS.QueueConcurrency` | 1..4 | 2 | `AS:177`, `A:ViewModels/QueueViewModel.cs:22,39,51` |

#### 3.1.10 Environment variables and CLI-only switches

| Name | Effect | Refs |
|---|---|---|
| `PSVIETHOA_SONY_SDK` | Toolkit root or toolchain folder | `C:Services/SonySdkToolchain.cs:43,56-77` |
| `PSVIETHOA_WINE` | Wine binary (macOS/Linux) | `C:Services/SonySdkToolchain.cs:45,80-138` |
| `LIBPROSPERO_PUBTOOLS_PATH`, `SCE_PROSPERO_SDK_DIR` | Native Oodle DLL lookup (built-in engine) | `C:Services/PublishingToolsLocator.cs:7,19-35` |
| `LIBPROSPERO_TEMP_DIR` | Toolkit launcher's temp default (not used by the app) | `K:build-from-folder.ps1:7` |
| `FPKG_LANG` | CLI language | `L:CommandLine.cs:123` |
| `--dry-run`, `--clean-junk`, `--overwrite`, `--keep-existing`, `--quiet`, `--lang`, `--image native`, `--kind app\|homebrew\|dlc` | CLI only | `L:CommandLine.cs:123-200,444-526` |

### 3.2 Validation rules (reference)

| Field | Rule | Refs |
|---|---|---|
| Source | Exists; folder must contain `sce_sys` (except update folders); image must parse and contain an app root; `.gp5` must parse and provide `sce_sys/param.json` | `C:Services/BuildPreparer.cs:115-186` |
| Output / temp | Output required; neither inside the source (nor the `.gp5` folder / its root) | `:188-222` |
| Content ID | regex with `_00` | `:224-227` |
| Passcode | exactly 32 chars, ASCII, no control chars | `:229-232` |
| Version | canonicalisable `NN.NNN.NNN` | `:234-237` |
| Levels | Kraken −4..9; SDK level −4..9; threads 0..256; PlayGo 1..255; SDK 1..11; block 128..256 KiB; prediction −4..9 | `:239-284` |
| Keystone (SDK) | `sce_sys/keystone` exactly 96 bytes (checked inside images too) | `C:Services/BuildEngine.cs:918-956` |

---

## 4. How the reference drives Sony's Publishing Tools

This is the contract our native engine must implement against the **user's own** `prospero-pub-cmd.exe`.

### 4.1 Executables and discovery

| File | Role | Required |
|---|---|---|
| `prospero-pub-cmd.exe` | everything (`img_create`, `img_verify`, `img_extract`, …) | yes |
| `libScePubTools.dll` (next to the exe) | engine library loaded by the exe | yes (the exe will not start without it; UNVERIFIED exact failure) |
| `ext\{da,fa,p2d,ric,sc2}.exe`, `ext\ispc_texcomp.dll`, `ext\libatrac9.dll` | texture / audio / SELF helpers | yes for some content (UNVERIFIED which games need which) |
| `prospero-dds2png.exe` (toolkit root, i.e. parent of `toolchain\`) | DDS → PNG recovery for presentation images | optional |
| `patch-manifest.json` (`profile` key) | toolkit profile id | optional; the reference bundle has none |

Lookup: env `PSVIETHOA_SONY_SDK`, then a `sony-sdk` folder in candidate directories next to the app; a folder is
accepted when `toolchain\prospero-pub-cmd.exe` or `prospero-pub-cmd.exe` exists in it. Only existence is checked.
Windows runtime: `msvcp140.dll`, `vcruntime140.dll`, `vcruntime140_1.dll` must load (`NativeLibrary.TryLoad`). `C:Services/SonySdkToolchain.cs:56-77,179-208,275-296`.

### 4.2 Commands

| Purpose | Command line (argument list, one element per item) | Used by |
|---|---|---|
| Build (full) | `img_create` `--oformat` `nwonly` [`--compression_level` `<-4..9>`] `<project.gp5>` `<out>.partial.pkg` | reference app `C:Services/SonySdkRunner.cs:90-105` |
| Build (update) | same + `--ref_pkg_path` `<base.pkg>` before the project (the app puts it after the level) | `:98-102` |
| Verify | `img_verify` `--passcode` `<32>` `--format_check` `on` `--integrity_check` `on\|off` `--no_progress_bar` `<pkg>` (a compact patch cannot be parsed: verify its `.remastered.pkg`) | toolkit GUI only `K:build-gui.ps1:844-860` |
| Extract | `img_extract` `--passcode` `<32>` `--no_progress_bar` `<pkg>` `<empty dest folder>` (patch → its `.remastered.pkg`) | toolkit GUI only `K:build-gui.ps1:904-935` |
| DDS → PNG | `prospero-dds2png.exe` `<in.dds>` `<out.png>` [`--preserve-alpha`] ; exit 0 = success | app `C:Services/SonySdkBuilder.cs:173-189` |
| Toolkit launcher equivalent | always passes `--compression_level 7` explicitly; never `--no_progress_bar` in the launcher (the script's own `--build` mode does) | `K:build-from-folder.ps1:146-149`, `K:scripts/create-gp5-from-folder.py` (flags only) |

Other subcommands present in the binary's strings (not used by the reference; behaviour and output formats UNVERIFIED):
`img_info`, `img_file_list`, `img_convert` (`--oformat patch|remastered`), `img_copy`, `img_write`, `pkg_info`,
`pkg_compare`, `pkg_compare_delta`, `pkg_file_add/delete`, `pkg_meta_data`, `pkg_cache_create`, `gp5_proj_create/update`,
`gp5_file_add/update/delete` (`--chunk`), `gp5_dir_add/update/delete` (`--virtual true`), `gp5_chunk_add/update/delete`
(`--languages`, `--label`), `gp5_scenario_add/update/delete`, `gp5_playgo_*`, `gp5_global_exclude_*`, `gp5_ref_file_*`,
`gp5_batch`. `img_create` also knows `--tmp_path`, `--skip_digest`, `--no_progress_bar`, `--volume_ts`, `--for_submission`,
`--passcode`, `--entitlement_key`; `--volume_type [prospero_app|prospero_ac|prospero_al]` exists for project commands.

### 4.3 Process setup

| Item | Reference behaviour | Refs |
|---|---|---|
| Program | full path of `prospero-pub-cmd.exe` (Windows); `wine <Z:\…exe>` elsewhere | `C:Services/SonySdkRunner.cs:280-310` |
| Paths given to the tool | every path through `aliases.Map()` (ASCII junction when the real path is non-ASCII) and, under Wine, `Z:` + backslashes | `C:Services/SonySdkBuilder.cs:131-170`, `C:Services/SonySdkToolchain.cs:25,386-390` |
| Working directory | first of: toolchain folder, system temp, GP5 folder that is pure ASCII and exists | `C:Services/SonySdkRunner.cs:149-151` |
| Environment (Windows) | inherited + `TEMP` = `TMP` = `<chosen temp folder>\sdk-tmp-<8 hex>` (created before, deleted after) | `:312-316`; `C:Services/SonySdkBuilder.cs:343-347,474-480` |
| Environment (Wine) | `WINEPREFIX` (private, created with `wineboot.exe --init` once, serialised), `WINEDEBUG=-all`, `WINEDLLOVERRIDES=mscoree,mshtml=;winemenubuilder.exe=d`, `MVK_CONFIG_LOG_LEVEL=0` | `:28-65,293-306` |
| stdin | redirected and closed immediately | `:285,326` |
| stdout | redirected, decoded as UTF-8, read in 256-char blocks and parsed character by character | `:289,335-344` |
| stderr | redirected, read line by line, classified the same way | `:346-353` |
| CPU limit | `CpuLimiter` around the process (live value) | `:328-333` |
| Threads | `img_create` has no thread option; it starts one worker per core | `C:Services/CpuLimiter.cs:6-16` |

### 4.4 Output parsing, success and failure

| Rule | Detail | Refs |
|---|---|---|
| Progress bar | A line whose trimmed text starts with `\|____` arms the bar; every following `=` (no newline in between) is one step of 51; percent = count × 100 / 51 (capped 100). The bar ends at the first line break after at least one `=`. Ruler line and the scale line starting `0        20` are not logged. | `C:Services/SonySdkRunner.cs:25,127-131,205-211,409-479` |
| Bytes written | size of `<out>.partial.pkg` polled every 500 ms | `:133-147` |
| Classification | `[Error] x` → error "SDK: x"; `[Warn] x` → warning, except routine ones (toolchain version invalid or missing; "(online check)"; "has not been built for submission"; "- Perform package verification"; "- Rebuild the package for submission"; "Process finished with warning(s)") → info; `[Info]`, `[Debug]`, `Creating an image…` → info; `wine:` or `:err:` → warning; everything else dropped. | `:205-265` |
| Success | exit code 0 **and** no `[Error]` line **and** the output file exists. Otherwise the failure text = first three error lines, or "exit code N" + hint. | `:12-15,155-169,186-189` |
| Exit-code hints | `0xC0000135` → VC++ x64 runtime missing; `0xC000007B` → damaged/wrong-architecture runtime | `:196-202` |
| Known SDK error texts (for friendly messages) | GP5 format invalid / "reserved node found (dst_path …)"; "invalid attribute value src_path" / "… dst_path"; "File not found"; "Format of the UCP file is not valid (… ucp_header.ucp_digest is invalid)"; "Format of the png file is not valid (sce_sys/pic2.png)"; "Unexpected logical error. (NAPS metadata missing)" (base not built by Publishing Tools); "Compression level should be set to a number between -4 and 9" | CHANGELOG 2.2.x; `C:Services/BuildPreparer.cs:244` |
| After success (full) | If the partial carries the plaintext marker → rename to final; else legacy conversion (out of scope). Delete `.naps_metric.json`. | `C:Services/SonySdkBuilder.cs:420-464` |
| After success (update) | `.partial.pkg.remastered.pkg` must exist, else failure. Rename both. | `:401-417` |
| On any failure | delete partial, partial companion, metric; keep GP5, scenario, assets, logs | `:466-473` |

Build logs (UTF-8 without BOM, LF):

| File | Content | Refs |
|---|---|---|
| `<stem>-build-logs/01-create-gp5.log` | the text the toolkit script would print: excluded files with reasons, DDS conversions, SELF/UCP repairs, the size selection line and warnings | `C:Services/SonySdkProject.cs:156-220`, `C:Services/SonySdkBuilder.cs:230` |
| `<stem>-build-logs/02-img-create.log` | `command=<quoted argv>`, `started_utc=<ISO 8601>`, `finished_utc=…`, `elapsed=<hh:mm:ss.fffffff>`, `exit_code=<n\|cancelled>`, then the `[Error]` lines | `C:Services/SonySdkBuilder.cs:383-390,519-533` |

### 4.5 GP5 project (byte-exact rules)

File `<output>/<stem>.gp5`, UTF-8 **without BOM**, every LF written as CRLF, **no line break after the last line**.
Layout follows Python's ElementTree output (two-space indent, single-quoted XML declaration, `" />"` for empty elements):

```
<?xml version='1.0' encoding='utf-8'?>
<psproject fmt="gp5">
  <volume>
    <volume_type>prospero_app</volume_type>
    <package passcode="P" content_id="C" />
    <chunk_info …>…</chunk_info>          (see §4.6)
  </volume>
  <files>
    <file dst_path="D" src_path="S" [chunk="N"] [pfs_compression="disable"] />
    …
    <empty_dir dst_path="D" />
  </files>
</psproject>
```

| Rule | Detail | Refs |
|---|---|---|
| `content_id` | from the source `param.json` `contentId` (must match `^[A-Z]{2}[0-9]{4}-[A-Z]{4}[0-9]{5}_[0-9]{2}-[A-Z0-9]{16}$`), not from the UI | `C:Services/SonySdkProject.cs:319-320,1571-1602` |
| `passcode` | 32 chars; anything else is an error | `:446-450` |
| Attribute escaping | `&`→`&amp;`, `<`→`&lt;`, `>`→`&gt;`, `"`→`&quot;`, CR→`&#13;`, LF→`&#10;`, TAB→`&#09;` | `:1193-1196` |
| `src_path` | absolute tool path (through aliases / `Z:`); real paths resolved once at the roots, not per file | `:1198-1225`, `C:Services/SonySdkBuilder.cs:167-170` |
| File entry order | 1 `sce_sys/playgo-scenario.json` → the sidecar; 2 recovered PNGs; 3 language payloads then empty-chunk fillers; 4 every walked file in walk order; 5 `<empty_dir>` entries | `C:Services/SonySdkProject.cs:606-649` |
| `chunk` attribute | script layout: on **every** file (including `"0"`); source-table layout: only when > 0; minimal layout: never; payloads/fillers: always | `:606-631` |
| `pfs_compression="disable"` | only on payloads and fillers | `:615-618,1176-1191` |
| Substituted sources | `sce_sys/param.json` → `.gp5-assets/<stem>/sce_sys/param.json`; repaired SELF → `.gp5-assets/<stem>/normalized-self/<rel>`; repaired UCP → `.gp5-assets/<stem>/repaired-ucp/<rel>`; recovered PNG → `.gp5-assets/<stem>/sce_sys/<name>.png` | `:619-631,1043-1077,1119-1159` |
| Walk order | recursive; inside every folder the entries (files and folders together) sorted by Python `casefold()` then code point (stable); sub-folders appear at their sorted position | `:1417-1427` |
| Rejected in walk | any reparse point (symlink/junction) → hard error; non-regular file → excluded | `:1436-1441,1477-1481` |
| Excluded in walk (reason in log) | the output GP5 itself; root `.gp5-assets/`; root `sce_suppl/`, `sce_sc/` (casefold); OS junk files/folders (§3.1.1); option skips (AMPR, DLC emu, dump leftovers) as folders or files; `*.gp4`, `*.gp5`, `*.esbak` anywhere; `*.playgo-scenario.json` anywhere; root `ampr_emu.index`; `fakelib/libsceampr.sprx`, `fakelib/libsceplaygo.sprx` (casefold); `sce_sys/about/` (any depth); `sce_sys/<f>` (direct child, case-insensitive) for f in `pfs-version.dat disc_info.dat ext_info.dat imagedigs.dat playgo-chunk.dat playgo-hash-table.dat playgo-ficm.dat playgo-scenario.json playgo-manifest.xml playgo-chunk.crc pfsimage.xml param.sfo param_cp_values.json pronunciation.sig origin-deltainfo.dat target-deltainfo.dat license.info license.dat origin-param.json target-param.json pfs-region-hints.json origin-relocinfo.dat target-relocinfo.dat` (+ `keystone`, but the keystone is **kept** explicitly); any file under `sce_sys` (any depth) ending `.dds` or `.auth_info` | `:246-317,372-430,1442-1497` |
| Path rule (pre-check) | every `dst_path` must be printable ASCII (0x20–0x7E), without `%` or `;`, with no empty component and no component ending in `.`; violations are listed and the build stops **before** `img_create` | `:350-370`, `C:Services/SonySdkBuilder.cs:233-236` |
| Empty folders | a folder is "empty" when every entry in it is OS junk (or it has none). Declared: `dlc_emu.ini` mount folders first, then (option on) empty source folders. Never declared: inside `sce_sys`, under root `.gp5-assets`/`sce_suppl`/`sce_sc`, paths failing the path rule, paths with a junk component, folders under an option-skipped folder, folders that contain any GP5 file, duplicates (case-insensitive). Folders that became empty only because their files were excluded are **not** declared. | `:640-649,692-732,1555-1560` |

`param.json` normalisation (written to `.gp5-assets/<stem>/sce_sys/param.json`):

| Step | Detail | Refs |
|---|---|---|
| Parse | utf-8 with optional BOM; trailing commas and comments tolerated; must be an object | `C:Services/SonySdkProject.cs:1161-1174` |
| DRM | `applicationDrmType` = `"standard"` (always in SDK mode) | `:808-833` |
| Options | versionFileUri clear; attribute3 clear; required-FW lowering; contentVersion and default-language titleName from the UI | `C:Services/ParamJsonPatch.cs:151-213` |
| Size (full builds only) | §4.7 | `C:Services/SonySdkPackageSize.cs` |
| Serialise | Python `json.dumps(indent=2, ensure_ascii=False)` + `"\n"`, keys in source order (new keys appended), then CRLF; numbers must round-trip exactly as Python prints them | `C:Services/SonySdkProject.cs:828-835`, `C:Services/PythonJson.cs` |

### 4.6 PlayGo `chunk_info` and the scenario sidecar

Three layouts, chosen in this order (`C:Services/BuildEngine.cs:696-740`):

| Layout | When | `chunk_info` | Scenario sidecar (`<stem>.playgo-scenario.json`, CRLF) | Payloads |
|---|---|---|---|---|
| **C · source table** | `sce_sys/playgo-chunk.dat` parses (magic `plgx`, version u16 `0x1000` at 4, stored length u32 at 16 = file size, chunk count u16 at 10 in 1..255, scenario count u16 at 14 in 1..64, default scenario u16 at 20, default language u16 at 36, language mask u64 at 56 containing the default language; sections at 192/208/224/232/240) **and** is not trivial (more than 1 chunk or 1 scenario) | `chunk_count`, `scenario_count`; `<chunks supported_languages="<codes of the header mask in id order>" default_language="<code>">` (bare `<chunks>` if mask 0); `<chunk id languages? label />` with `languages` only when the chunk mask differs from the header mask; `<scenarios default_id>`; each `<scenario id type="playmode" initial_chunk_count="<all chunks>" label>` text = space-separated chunk ids (source order, missing ones appended ascending) | the source `playgo-scenario.json` **bytes verbatim** when it matches (same count, same default id, ids in order, all `type` = playmode); else generated from the scenario labels | fillers only (when the option is on): chunks with no non-empty mapped file |
| **B · script layout (fallback)** | no usable table and "PlayGo fallback" on | `chunk_count=N` (the PlayGo chunks setting), `scenario_count=S`; `<chunks supported_languages="<languages, scenario-file order>" default_language="<chunkDefaultLanguage>">`; each `<chunk id label="Chunk #i" layer_no="0" languages="<assigned languages or all>" />`; each `<scenario id type="<from file>" initial_chunk_count="N" label="<title in default language or Scenario #id>">0-(N-1)</scenario>` (`"0"` when N = 1) | the source `playgo-scenario.json` re-serialised (Python style) if it passes the toolkit's rules, else the default scenario object (warning) | one 1 MiB payload per language `playgo-languages/<NN>-<code>.bin` (NN 1-based, code with chars outside `[A-Za-z0-9._-]` → `_`), language i in chunk min(i, N−1) (so all in chunk 0 when N = 1); then fillers |
| **A · minimal** | no usable table and fallback off, or a trivial table | `chunk_count="1" scenario_count="1"`, bare `<chunks>`, `<chunk id="0" label="Chunk #0" />`, `<scenario id="0" type="playmode" initial_chunk_count="1" label="Scenario #0">0</scenario>` | default scenario object | none |

Rules shared by B and C (fixes made after console reports, CHANGELOG 2.2.7): every scenario lists **every** chunk; every
chunk is initial; fillers keep chunks from being "not installed" (Control stalled at "Installing game… 0GB/0"). Real
console confirmation is UNVERIFIED (`docs/research/ps5-results.md` table is empty).

Scenario-file rules (B, the toolkit script's `write_scenario`): JSON object; `scenarioCount` integer 1..5;
`scenarioDefaultId` in range; `scenarioDefaultLanguage` non-empty and one of the 31 PlayGo codes (case-insensitive,
canonicalised); `scenarios` array with exactly `scenarioCount` objects, unique ids in range, non-empty `type`, and an
object keyed by the default language; `chunkDefaultLanguage` optional (defaults to the scenario default) and must be in
the supported list; `chunkSupportedLanguages` optional, else non-empty array of distinct known codes; when absent the
list is derived (chunk default + every language key found in the scenarios, in order) but **not written back** to the
file. Default object keys, in order: `chunkDefaultLanguage`, `chunkSupportedLanguages`, `scenarioCount`,
`scenarioDefaultId`, `scenarioDefaultLanguage`, `scenarios` = one scenario `{id: 0, type: "playmode", <lang>: {title,
description}}` with the toolkit's fixed title "Scenario #0" and a fixed English description (take the exact value from
the user's toolkit; byte parity of this string matters only for identical packages). `C:Services/SonySdkPlayGo.cs:341-577`.

Source-table file → chunk map (layout C): `playgo-ficm.dat` (u32 version 1 at 0, header size 16 at 8, map size at 12,
then 2 bytes per file: chunk id, reserved) paired with `playgo-hash-table.dat` (u32 version 1 at 0, header size 56 at
8, `7F 'F' 'L' 'T'` at 24, hash count at 36, then u64 hashes in the same order). A file's chunk is looked up by the PS5
flat-path-table hash of `"/" + path` (then of `path`). The hash function lives in LibProsperoPkg
(`ProsperoPs5FlatPathTable.HashPath`) and is UNVERIFIED for us. Misnamed tables (FLT inside the FICM file) are detected
and reported; then every file goes to chunk 0. `C:Services/SonySdkPlayGo.cs:66-86,148-290`.

Language ids: 31 codes, bit `63 − id` in a mask. Our `core/fpkg/playgo.py` has the same 31-code set (alphabetical);
the id order is only needed for layout C and must come from Sony's documentation or the GPL library source (UNVERIFIED in
our repo).

### 4.7 Package size class (`attributePub`) and `kernel.addcontMountLevel` (full builds only)

| Step | Rule | Refs |
|---|---|---|
| Inputs | U = uncompressed bytes of every GP5-mapped file except the source `param.json` (repaired copies counted at their size) + recovered PNGs + payloads + the scenario sidecar; F = number of `<file>` entries in the GP5 (including `param.json`) | `C:Services/SonySdkProject.cs:541-577` |
| Estimate | E = U + max(512 MiB, ceil(U / 100)) + 4096 × F | `C:Services/SonySdkPackageSize.cs:57-58` |
| Mount level | read `kernel.addcontMountLevel` (missing = 0; must be an integer literal 0/1/2; `kernel` must be an object) → if 2 and E > 97,945,387,008 → 1; then if 1 and E > 195,878,191,104 → 0 | `:61-69,104-129` |
| Class | level still 2, or E ≤ 162,514,599,936 → 0; E ≤ 195,878,191,104 → 1; else 2 (SDK 2.79 has nothing larger: warning when E > 260,436,918,272, `img_create` decides); lv2 with F > 500,000 → error | `:71-95,98` |
| Apply | requires `applicationCategoryType` == 0 (missing or non-zero → error); `attributePub` always written; `kernel.addcontMountLevel` written when changed (warning: separately installed DLC may not mount); `kernel.appSizeInGib` removed (only SDK 3.13 lv3 uses it) | `:136-181` |
| Updates | not applied (source `attributePub` kept) | `C:Services/SonySdkBuilder.cs:207-216` |

### 4.8 Presentation images, SELF and UCP inputs

| Input | Rule | Refs |
|---|---|---|
| PNG colour type | `sce_sys/icon0\|pic0\|pic1[_NN].png` must be 8-bit colour type 2 (RGB); `sce_sys/pic2[_NN].png` 8-bit type 6 (RGBA); read from the IHDR (bytes 24 = bit depth, 25 = colour type) | `C:Services/SonySdkProject.cs:839-878` |
| DDS recovery | for every `sce_sys/*.dds` (casefold order): if its PNG is missing, or is a presentation PNG with the wrong mode → convert with `prospero-dds2png` (`--preserve-alpha` unless the PNG must be RGB), in parallel (clamp(cores/2, 2, 8)), check the result's colour type; the bad source PNG is excluded. Two DDS mapping to one PNG → error. A wrong-mode presentation PNG without a DDS → error before `img_create`. No converter and a conversion needed → error. | `:893-1041` |
| SELF repair | files ≥ 0x20 bytes starting with the PS4 SELF magic `4F 15 3D 1D` get the PS5 magic `54 14 F5 EE`; a `.sceversion` trailer that does not start at the boundary stored at header offset 0x10 is re-padded with zeros; the copy goes to `normalized-self/` with the source timestamps; exact trailer rules UNVERIFIED (our own implementation must be tested against real dumps) | `C:Services/SonySdkSelfRepair.cs:5-206`, `C:Services/SonySdkProject.cs:1043-1077` |
| UCP repair | `sce_sys/trophy2/*.ucp`, `sce_sys/uds/*.ucp` ≤ 128 MiB: valid but stale SHA-1 self-digest → corrected copy in `repaired-ucp/`; unreadable UCP → the reference switches to the built-in engine (full builds) because Publishing Tools rejects the build; digest location and coverage are inside LibProsperoPkg (`ProsperoUcp`), UNVERIFIED for us | `C:Services/UcpInspector.cs:6-137`, `C:Services/SonySdkProject.cs:1119-1159`, `C:Services/BuildEngine.cs:72-81` |

### 4.9 ASCII aliases, pre-scan, CPU limit

| Feature | Rule | Refs |
|---|---|---|
| ASCII aliases | Roots (source → `src`, output → `out`, update folder → `upd`, base package folder → `ref`) whose real path has any char ≥ 128 get a link inside an ASCII, writable base folder: system temp, the job temp, `%ProgramData%\…` (Windows), the output drive root; never on the source drive. Folder name `psviethoa-sdk-<pid hex>-<6 chars>`; Windows uses an NTFS junction, POSIX a symlink; the alias is accepted only if the target can be listed through it; removed on dispose; folders of dead PIDs are swept at the next run. | `C:Services/SonySdkPathAliases.cs:6-272` |
| Pre-scan | Windows only; see §3.1.4 | `C:Services/SonySdkPrescan.cs` |
| CPU limit | Windows: `ProcessorAffinity` = low N bits (≤ 64 cores), re-applied when the value changes (polled every 250 ms); "N ≥ cores or ≤ 0" = unlimited | `C:Services/CpuLimiter.cs:40-121` |

### 4.10 Side effects of Publishing Tools on `param.json`

| Field | Observed | Source |
|---|---|---|
| `sdkVersion`, `requiredSystemSoftwareVersion` | rewritten because the toolchain lacks `prospero-llvm-readelf.exe`; CHANGELOG 2.2.0 says both become 0; `docs/research/NOTES.md:52-53` says `sdkVersion = 0` and `requiredSystemSoftwareVersion = 0x1160000000000000` regardless of input. **UNVERIFIED which is right.** The app logs before/after values. | `C:Services/BuildEngine.cs:958-984` |
| `originContentVersion`, `targetContentVersion`, `pubtools` | dropped / rewritten by the tool | NOTES.md |
| `attribute3` | kept unchanged unless the clear option is on (verified by the reference) | CHANGELOG 2.2.1 |
| `sce_sys/about/right.sprx` | reserved; not regenerated by the SDK (packages have none; only the copyright screen is affected) | CHANGELOG 2.2.1 |

---

## 5. Parity matrix

Status of our Python engine: **present**, **partial** (with the difference), **missing**. Recommendation:
**SDK** = implement natively and drive the user's Sony tool; **native** = pure C++ (no Sony binary needed);
**out** = out of scope, with the reason.

### 5.1 Build PKG

| # | Reference feature | Python (`core/fpkg`) | C++ recommendation |
|---|---|---|---|
| 1 | Folder source | present | native |
| 2 | `.exfat` source (bare/MBR/GPT, app root search depth 3) | missing (extract first in the Extract tab) | native: stream-extract the app root with our exFAT reader into `<temp>/stage` (Extract strategy). Mounting: out for v1 (Dokan driver install, admin rights). |
| 3 | `.ffpfsc` source | missing | native via our PFS/PFSC reader (`native/src/engines/pfs`), extract strategy |
| 4 | `.ffpkg` (UFS2) source | missing | native via `native/src/engines/ufs2`, extract strategy |
| 5 | `.gp5` project source | missing | SDK: pass the user's `.gp5` straight to `img_create` after checking every `src_path` exists and is ASCII (reference uses the built-in engine instead). UNVERIFIED with relative `src_path`s and `rootdir` projects. |
| 6 | Source preview (icon, title, version, SDK, size, eboot, PlayGo state) | partial (param only) | native (`gameinfo` module) |
| 7 | Content ID validation + suggestion | partial (validation only, any `_NN`) | native; keep GP5 regex for packing, warn when the suffix is not `_00` |
| 8 | Title / version fields written into normalised `param.json` | missing | native (only when they differ) |
| 9 | GP5 `content_id` taken from `param.json` | present | native; refuse when the UI value differs (reference silently mismatches) |
| 10 | Output folder auto `<source>-pkg` | partial (queue chooses) | native setting |
| 11 | Output conflict: overwrite / keep old as "(1)" / cancel | partial (new file gets "(2)") | native with policy setting `fpkg.outputConflict` |
| 12 | Junk dialog that deletes junk from the source | missing (we only exclude) | **out**: never modify the source; exclude and log instead |
| 13 | AMPR import warning (`libSceAmpr` in the first part of `eboot.bin`) | missing | native |
| 14 | Keystone exactly 96 bytes | present | native |
| 15 | Engine selection with built-in fallback | n/a (Sony only) | **out** (built-in engine: no public source for the shipped DLL, embedded debug keys). Unreadable UCP → stop with a clear message. |
| 16 | VC++ x64 runtime check | present | native probe (`LoadLibraryEx`); point to Microsoft's installer; bundling `vc_redist` is an owner decision |
| 17 | Wine / Rosetta | n/a | out (Windows app); Linux CI uses the fake tool |
| 18 | ASCII aliases (junction) | present (TEMP/ProgramData, `mklink /J`) | native: `DeviceIoControl(FSCTL_SET_REPARSE_POINT)` or `mklink /J`; add never-on-source-drive, list-through check, stale sweep by PID |
| 19 | GP5 writer | partial: entry order differs (payloads after game files), `chunk` written on every file in every layout, CRLF/indent OK | native, byte-exact to §4.5 |
| 20 | Exclusion rules | partial: same core list; ours also drops root `decrypted/` and root `*.exfat/.ffpkg/.ffpfs/.ffpfsc/.pkg`; `.dds`/`.auth_info` only directly in `sce_sys` (reference: any depth); reference also excludes `.gp5-assets`, `sce_suppl`, `sce_sc` only at root | native, align with §4.5 (keep our two extra rules as documented additions) |
| 21 | OS junk rules | present | native |
| 22 | Path rule | partial (ours also rejects a trailing space) | native; keep the stricter rule only if confirmed harmful to the tool (UNVERIFIED) |
| 23 | Symlink / junction rejection | present | native |
| 24 | `<empty_dir>` for empty folders | partial: ours declares folders emptied by exclusions; no "junk-only" rule; no root/sce_sys filters beyond `sce_sys` | native per §4.5 + setting `fpkg.keepEmptyFolders` |
| 25 | DLC-emu mount folders (`mount_point` + root `addcont<N>`) always declared; missing ones warned | partial (only via the empty-folder rule) | native |
| 26 | Keep DLC emulator toggle | missing | native setting |
| 27 | AMPR leftovers toggle | partial (root index + fake modules always excluded; `fakelib/ampr_emu.index` not) | native setting |
| 28 | Dump leftovers (`_DUBLEX_`, `Engine/Saved`, `<project>/Saved`) | missing | native setting |
| 29 | `applicationDrmType = "standard"` | present | native |
| 30 | Clear `versionFileUri` | missing | native setting (default on) |
| 31 | Clear `attribute3` | missing | native setting (default off) |
| 32 | Lower `requiredSystemSoftwareVersion` | missing | native setting; low value because the tool rewrites it (§4.10) |
| 33 | `attributePub` / mount level | partial: ours lowers mount level 1 → 0 above **162.5 GB** (reference: 195.9 GB); ours **errors** above 260 GB (reference: warning); ours skips silently when `applicationCategoryType` is missing (reference: error); ours accepts non-integer mount levels | native per §4.7; keep "skip with warning" for non-game categories as a documented choice |
| 34 | Order-preserving Python-style JSON | present (Python `json`) | native: needs an ordered JSON model (QJsonObject sorts keys) |
| 35 | PNG colour-type check | missing | native (IHDR) |
| 36 | DDS → PNG recovery | missing | SDK: user's `prospero-dds2png.exe` when present; else stop with a clear message. A native BC1–BC7 decoder is possible later. |
| 37 | SELF repair | partial (PS4 magic detected and warned) | native, behind a setting; test on real dumps (UNVERIFIED trailer rules) |
| 38 | UCP digest repair | missing | native once the UCP layout is confirmed (GPL source study or samples); until then detect and stop with the tool's error explained |
| 39 | PlayGo script layout (B) | partial: scenario JSON written in a **different shape** (`initialChunkCount`, `chunks`, `localizedLabels` instead of the source file / default object); no language payloads when N = 1; labels read from `label`/`name` instead of the default-language title | native per §4.6 B — **fix first** |
| 40 | Minimal layout (A) when fallback is off | missing | native |
| 41 | Source-table layout (C) | missing | native; header and FICM/FLT parsing are simple, the flat-path hash is UNVERIFIED (feature flag until confirmed) |
| 42 | Every scenario lists every chunk, all initial | present | native |
| 43 | Empty-chunk fillers | present (always on) | native setting `fpkg.fillEmptyChunks` |
| 44 | `--compression_level` presets | present | native |
| 45 | Windows Defender pre-scan | missing | native (cheap, big win on 95k-file games) |
| 46 | Live CPU-core limit | missing | native (needs a process-start hook in `ProcessRunner`) |
| 47 | `TEMP`/`TMP` per job | present (system temp; reference: chosen temp folder) | native: `<job temp>/sdk-tmp-<id>` on the chosen temp volume |
| 48 | ASCII working directory | present | native |
| 49 | Progress: `=` bar + file-size poll | present | native (raw-chunk callback needed in `ProcessRunner`) |
| 50 | Output classification, routine warnings, exit-code hints, friendly errors | present (ours explains more errors) | native |
| 51 | `01-create-gp5.log`, `02-img-create.log` | partial (02 writes a Python list repr; 01 has our own format) | native, reference key format for 02 |
| 52 | `.partial.pkg` + rename on success, partials removed on failure/cancel | present | native |
| 53 | Plaintext marker required | present (missing marker = failure) | native; legacy post-process conversion: **out** (rebuilds an RSA-3072 CNT header wrap) |
| 54 | Post-build check: header | present | native |
| 55 | Post-build check: LibProsperoPkg quick/full | n/a | SDK: optional `img_verify` (format only / format + integrity) |
| 56 | SHA-256 of the package | partial (function exists, not wired into the build) | native setting |
| 57 | Log `sdkVersion` / required FW rewrite | missing | SDK: read the package `param.json` through `img_extract` of `sce_sys` only if a cheap way exists (UNVERIFIED); else skip |
| 58 | Confirm DLC-emu items inside the package | missing | SDK via `img_file_list` (output format UNVERIFIED); optional |
| 59 | Keep intermediate files option | present | native |
| 60 | Keep computer awake | missing in the FPKG path | native (`SetThreadExecutionState`), app-level helper |
| 61 | Disk-space estimate and FAT32 warning | partial (space only) | native (also warn when output is FAT/FAT32: packages exceed 4 GiB) |
| 62 | Online metadata restore | missing | **out**: needs LibProsperoPkg's recovery code and downloads Sony CDN data; legal and safety risk |
| 63 | Built-in engine options (Kraken, PFS v3, shuffle, block size, layout optimisation, deterministic, SDK override, PlayGo languages, Native image mode, Homebrew/DLC kinds, Oodle DLL) | n/a | **out** (LibProsperoPkg only) |

### 5.2 Build Update PKG

| # | Reference feature | Python | C++ recommendation |
|---|---|---|---|
| 64 | `--ref_pkg_path` build, rename to `UPDATE_…` + `….remastered.pkg` | present | native |
| 65 | Base checks: `.pkg`, not `.remastered.pkg`, not the output file | partial (exists + `.pkg`) | native |
| 66 | Base checks: same Content ID, lower version | missing | SDK: read via `img_info`/`pkg_info` (output UNVERIFIED) or our own CNT reader for PLAINTEXT_NOAUTH packages (UNVERIFIED effort); fall back to a clear warning |
| 67 | "Base must be built by Publishing Tools" probe | missing | partial native heuristic only if the SI ZIP can be read without keys (`naps_meta_300` 72 vs 48 bytes, UNVERIFIED); else warn before the build |
| 68 | Update-folder merge (missing files from base folder or extracted from the base) | missing (source must be complete) | native overlay walk; base extraction with `img_extract` of the **whole** base into temp (selective extraction not available, UNVERIFIED) or from the "original game folder" |
| 69 | Recreate base folders (empty mount folders) in the merge | missing | native |
| 70 | "Exact source" option | n/a (always exact) | native setting (per job) |
| 71 | Output choice update / full / both | partial (both kept) | native per job; default "both" |
| 72 | Version bump suggestion | missing | native |
| 73 | `attributePub` not auto-sized for updates | present | native |
| 74 | Header check on the companion | present | native |
| 75 | Build base then update in one go (`--sdk-build-base`) | missing | queue two dependent jobs (later) |

### 5.3 Extract PKG, Queue, DLC

| # | Reference feature | Python | C++ recommendation |
|---|---|---|---|
| 76 | Package info (header, CNT, every `param.json` field, icon) | partial (header) | SDK `img_info` (UNVERIFIED output) + our header reader |
| 77 | File list, preview, selective extraction | missing | out for v1 (needs our own PFS/PFSC + NAPS reader of PLAINTEXT_NOAUTH images); `img_file_list` for a list only (UNVERIFIED) |
| 78 | Extract whole app tree | present (`img_extract`, empty folder only) | SDK |
| 79 | Merge CNT `sce_sys` into the tree ("Sony layout") | missing | UNVERIFIED whether `img_extract` already writes `sce_sys/param.json`, icons, PlayGo tables; check on real output first |
| 80 | Outer layer / raw CNT / SI export, PFSC decompression toggle | missing | out (LibProsperoPkg internals) |
| 81 | Quick / full check | present (`img_verify` integrity on/off) | SDK |
| 82 | SHA-256 | partial | native |
| 83 | Export DLC template | missing | out |
| 84 | Queue with concurrency, folder-of-games add, per-item output, pause, cancel item | present (unibuild queue) | native job queue (`native/src/jobs`) |
| 85 | Same Content ID + same output never concurrent | UNVERIFIED in Python | native rule in the job's `validate()` / scheduler |
| 86 | Queue items: full packages only, details from each source | present | native |
| 87 | `dlc_emu.ini` parsing | present (`ui/dlc_emu_ini.py`) | native |
| 88 | DLC packages per `[PSAC]` | partial: experimental Sony `prospero_ac` with an add-on `param.json` and a user-supplied entitlement key; reference uses LibProsperoPkg | SDK, experimental, off by default; console behaviour UNVERIFIED |
| 89 | PSAL refused | present | native |
| 90 | PS4 pages, Merge Base + Update | n/a | out (PS4 tools are patched Sony binaries) |
| 91 | PS5 FTP | separate feature | `native/src/connect/ps5` (not FPKG) |
| 92 | Resign Save | n/a | out |
| 93 | Components & plugins | present (tool status line) | native `DependencyRegistry` probes |

---

## 6. Proposed C++ module: `native/src/engines/fpkg/`

### 6.1 Constraints from the existing native scaffolding

* Modules register settings, dependency probes and job kinds through `fw::ModuleContext` (`native/src/jobs/Module.h`).
  `engines/fpkg/CMakeLists.txt` already declares `fw_add_module(fw_engine_fpkg DEPENDS fw_engine_gameinfo)`.
* Engines return `fw::Result<T>`, never throw across module boundaries; `Error::message` is user-facing,
  `Error::detail` goes to the log (`native/src/core/Result.h`).
* Jobs implement `fw::Job` (`kind()`, `title()`, `parameters()`, `validate()`, `run(JobContext&)`) and must write
  through `.part` files (`native/src/jobs/Job.h`).
* Settings are declared in `SettingsSchema` with section, type, default, range, `perJob`, `enabledWhenKey`,
  `dependencyId` (`native/src/core/Settings.h`). Use section `FPKG`, plus `PlayGo` and `Verification` where they fit.
* **Required core additions** (requests to the core owner):
  1. `ProcessRunner`: a raw-chunk output callback (the `=` bar has no newlines), a "started" callback exposing the
     process handle/PID (for CPU affinity), per-run environment (`TEMP`/`TMP`), and kill-tree on cancel (already planned).
  2. An order-preserving JSON DOM with a Python-compatible writer (`indent=2`, `ensure_ascii=False`, number tokens
     preserved). `QJsonObject` sorts keys and cannot be used for `param.json` / scenario output.
  3. A Windows junction helper and a keep-awake helper (`SetThreadExecutionState`) in `core/`.

### 6.2 Files and classes

| File | Class / function | Responsibility |
|---|---|---|
| `Module.h/.cpp` | `registerModule` | settings (§6.3), probes (`sony.pubcmd`, `sony.dds2png`, `msvc.x64`), job kinds (§6.4) |
| `FpkgSettings.h/.cpp` | `FpkgSettings`, `registerFpkgSettings()`, `FpkgSettings::from(const SettingsValues&)` | one snapshot struct for a job |
| `Toolchain.h/.cpp` | `PubToolchain` (exe, root, extDir, dds2png, profile, warnings), `locatePubTools(path)`, `probeVcRuntime()` | accept the exe, its folder, or a toolkit root with `toolchain\` or `bin\`; check `libScePubTools.dll`, `ext\`; read optional `patch-manifest.json`; never search beyond what the user set (plus env `SCE_PROSPERO_PUBTOOLS`, as in Python) |
| `ContentNaming.h/.cpp` | `ContentId`, `ContentVersion` (parse, compare, next, name component), `packageFileName(id, ver, update)`, `companionFileName()` | naming rules of §2.2/§2.3 |
| `ParamJson.h/.cpp` | `ParamDocument` (ordered), `ParamPatch` (drm, uri, attr3, lowerFw, version, title), `PackageSizeChooser` | §4.5 normalisation and §4.7 |
| `SourceScanner.h/.cpp` | `SourceScanner`, `ScanResult` (files, excluded with reasons, skippedJunk, skippedByOption, emptyDirs, badPaths, keystone, totals) | walk with ASCII-casefold order, rules of §4.5, cancellation checkpoint every 2,000 entries, parallel sub-folder listing (merge in order) |
| `DumpRules.h/.cpp` | `JunkRules`, `AmprRules`, `DlcEmuRules` (`DlcEmuIni` parser, mount resolution), `DumpLeftoverRules` | option-driven skips and warnings |
| `PlayGoPlanner.h/.cpp` | `PlayGoTable` (plgx/FICM/FLT parser), `ScenarioFile` (validate, canonicalise, default object), `PlayGoLayout` (A/B/C), `planPayloads()`, `planFillers()` | §4.6 |
| `InputRepair.h/.cpp` | `PngInfo` (IHDR), `DdsRecovery` (tool runner, parallel), `SelfRepair`, `UcpRepair` | §4.8; each behind a setting and a feature flag |
| `Gp5Writer.h/.cpp` | `Gp5Project` model + `writeGp5()` | byte-exact output of §4.5 |
| `AsciiAliases.h/.cpp` | `AsciiAliases` (RAII) | §4.9 |
| `PubCmd.h/.cpp` | `PubCmdCommand` builders (`imgCreate`, `imgVerify`, `imgExtract`, `dds2png`), `PubCmdOutputParser` (bar, classify, explain), `PubCmdRunner` (env, cwd, live CPU limit, file-size poll, cancel) | §4.2–§4.4 |
| `Prescan.h/.cpp`, `CpuLimiter.h/.cpp` | Defender pre-scan, affinity limiter | §4.9 |
| `PackageHeader.h/.cpp` | `checkPackageHeader()`, `sha256File()` | FIH checks of §2.2 |
| `ReferencePackage.h/.cpp` | `inspectBase()`, `UpdateOverlayPlanner` | §2.3 |
| `BuildLogs.h/.cpp` | `writeCreateLog()`, `writeImgCreateLog()` | §4.4 |
| `FpkgBuilder.h/.cpp` | `FpkgBuilder::plan()` (pure, no process) and `FpkgBuilder::run()` | orchestration, cleanup, outcome |
| `Jobs.h/.cpp` | `FpkgBuildJob` (`fpkg.build`, update when a base is set), `FpkgVerifyJob` (`fpkg.verify`), `FpkgExtractJob` (`fpkg.extract`), `FpkgAddonJob` (`fpkg.addon`, experimental) | `fw::Job` adapters |

### 6.3 Settings struct

All keys use the `fpkg.` prefix. Built-in-engine options are deliberately absent.

```cpp
namespace fw::fpkg {

enum class OutputConflict { Ask, KeepBoth, Overwrite, Cancel };
enum class UpdateOutput { Both, UpdateOnly, FullOnly };
enum class PostVerify { HeaderOnly, ToolFormat, ToolFull };

struct FpkgSettings {
    // Tools (section "Tools")
    QString pubToolsPath;                  // fpkg.pubToolsPath: exe, its folder or toolkit root; "" = not set
    QString dds2pngPath;                   // fpkg.dds2pngPath: "" = <toolkit root>/prospero-dds2png.exe if present

    // Packing (section "FPKG")
    std::optional<int> compressionLevel;   // fpkg.compressionLevel: nullopt = not passed (tool default 7); -4..9
    QString passcode = QString(32, '0');   // fpkg.passcode (perJob): 32 printable ASCII
    bool keepIntermediate = false;         // fpkg.keepIntermediate
    int cpuCores = 0;                      // fpkg.cpuCores: 0 = all; live-adjustable while img_create runs
    bool prescan = true;                   // fpkg.prescan (Windows only)
    bool preventSleep = true;              // fpkg.preventSleep (or the app-wide setting)
    QString tempFolder;                    // fpkg.tempFolder: "" = <output volume>/fpkg-temp or %TEMP%/fpkg-temp
    bool outputNextToSource = true;        // fpkg.outputNextToSource: "<source>-pkg"
    OutputConflict outputConflict = OutputConflict::Ask;   // fpkg.outputConflict (queue: KeepBoth)

    // param.json (section "FPKG", group "param.json")
    bool clearVersionFileUri = true;       // fpkg.clearVersionFileUri
    bool clearAttribute3 = false;          // fpkg.clearAttribute3
    bool lowerRequiredFirmware = true;     // fpkg.lowerRequiredFirmware
    bool autoSizeClass = true;             // fpkg.autoSizeClass (full builds; attributePub + mount level)
    QString contentVersionOverride;        // perJob: "" = keep source
    QString titleOverride;                 // perJob: "" = keep source

    // Files (section "FPKG", group "Files")
    bool keepEmptyFolders = true;          // fpkg.keepEmptyFolders
    bool keepDlcEmulator = true;           // fpkg.keepDlcEmulator
    bool removeAmprLeftovers = true;       // fpkg.removeAmprLeftovers
    bool removeDumpLeftovers = true;       // fpkg.removeDumpLeftovers
    bool repairSelf = true;                // fpkg.repairSelf
    bool repairUcp = true;                 // fpkg.repairUcp (5.0.3; false before: off until the UCP layout was confirmed)
    bool recoverPngFromDds = true;         // fpkg.recoverPngFromDds (needs dds2png)

    // PlayGo (section "PlayGo")
    bool playGoFallback = true;            // fpkg.playGoFallback: script layout when no usable table
    int playGoChunkCount = 100;            // fpkg.playGoChunkCount: 1..800 (1..255 before 5.0.3)
    bool fillEmptyChunks = true;           // fpkg.fillEmptyChunks
    bool useSourcePlayGoTable = true;      // fpkg.useSourcePlayGoTable (5.0.3; false before: off until the flat-path hash was confirmed)

    // Verification (section "Verification")
    PostVerify postVerify = PostVerify::ToolFull;  // header always; then img_verify format(+integrity)
    bool computeSha256 = false;            // fpkg.computeSha256

    // Updates (perJob)
    QString basePackage;                   // "" = full build; else --ref_pkg_path
    QString baseFolder;                    // original game folder for update-folder merges
    bool exactSource = false;              // source is complete (missing = deleted)
    UpdateOutput updateOutput = UpdateOutput::Both;
};

} // namespace fw::fpkg
```

Defaults follow the reference except (up to 5.0.2 also `repairUcp` and `useSourcePlayGoTable`, which started off; since
5.0.3 both start on, as in the reference 3.1.0, see §9):
`postVerify` keeps our Python default (`img_verify` with integrity, the toolkit GUI default); `updateOutput` is `Both`
(the reference UI defaults to "update only", its CLI and request default to both; keeping both loses nothing).

Legacy import (`SettingsSchema::addLegacyKey`): `fpkg_pubtools_path` → `fpkg.pubToolsPath`; `fpkg_compression`
("Tool default" → nullopt, "Fast (2)" → 2, "Standard (4)" → 4, "Smallest (7)" → 7, "Maximum (9)" → 9) →
`fpkg.compressionLevel`; `fpkg_chunk_count` → `fpkg.playGoChunkCount`; `fpkg_tool_verify` (false → HeaderOnly) →
`fpkg.postVerify`; `fpkg_keep_project` → `fpkg.keepIntermediate`.

### 6.4 Jobs and phases

| Job kind | Parameters (JSON) | Phases (weight) |
|---|---|---|
| `fpkg.build` | `source`, `output`, optional `basePackage`, `baseFolder`, `exactSource`, `updateOutput`, overrides | Prepare source 8 (only for images or base extraction) · Plan 0.5 · Generated inputs 0.5 · Pre-scan 3 (Windows) · Pack 90 · Finalise 0.5 · Check 1 · Verify with tool 8 (if on) · SHA-256 6 (if on) |
| `fpkg.verify` | `package`, `passcode`, `integrity` | Header 1 · Tool verify 99 |
| `fpkg.extract` | `package`, `destination` (must be empty), `passcode` | Extract (indeterminate + bytes written) |
| `fpkg.addon` | `payloadFolder`, `contentId`, `entitlementKey` | as build, `prospero_ac`, no `chunk_info` |

Build flow (`FpkgBuilder::run`): validate toolchain → resolve source (extract images to a staging folder: a work
folder next to the output, or `<job temp>/stage` when a temporary folder is set; see `engines/FPKG.md`) → read
`param.json`, keystone, base checks → scan + rules → plan PlayGo, payloads, fillers, empty dirs → generate assets in
`<output>/.gp5-assets/<stem>/` → size class → write scenario + GP5 → `01` log → path-rule gate → pre-scan → aliases →
`img_create` into `<stem>.partial.pkg` (live CPU limit, progress) → `02` log → success rules (§4.4) → header check
(on the companion for updates) → conflict policy and rename → `.naps_metric.json` removal → optional `img_verify`,
SHA-256 → cleanup (keep logs; keep GP5/assets on failure or when asked) → `JobOutcome` (outputs, verification, warnings,
details: file count, bytes, size class, PlayGo layout, excluded counts).

### 6.5 Error cases

| Case | `ErrorCode` | When | User message (gist) |
|---|---|---|---|
| Tool not set / not found / wrong file name | `DependencyMissing` | validate | choose your `prospero-pub-cmd.exe` |
| `libScePubTools.dll` or `ext\` missing | warning, then `ExternalToolFailed` if the tool fails | validate / run | the tool folder is incomplete |
| VC++ x64 runtime missing; exit `0xC0000135` / `0xC000007B` | `DependencyMissing` | validate / run | install the Microsoft VC++ 2015–2022 x64 runtime |
| Source missing / no `sce_sys` / image unreadable / no app root in image | `NotFound` / `CorruptData` | validate | — |
| `param.json` missing, not JSON, not an object, bad `contentId`, bad `contentVersion` | `CorruptData` | plan | — |
| UI Content ID differs from `param.json` | `InvalidArgument` | validate | edit `param.json` or clear the field |
| Keystone missing or not 96 bytes | `CorruptData` | plan | the debug toolchain needs the dump's own keystone |
| Output/temp inside source; output on FAT32 (warning); not enough space | `InvalidArgument` / `InsufficientSpace` | validate | — |
| Symlink or junction in the source | `Unsupported` | scan | — |
| Package paths failing the SDK path rule (list) | `Unsupported` | plan, before `img_create` | rename these files |
| Non-ASCII root and no alias possible | `Unsupported` | run | move to an ASCII path or an NTFS temp folder |
| Presentation PNG wrong mode without DDS; DDS needs converting but no `prospero-dds2png`; converter failed | `CorruptData` / `DependencyMissing` / `ExternalToolFailed` | plan | — |
| Unreadable UCP | `CorruptData` | plan | the tool rejects this file; give the exact name |
| Size class: category ≠ 0 (warning, skipped), mount level invalid, lv2 > 500,000 files | warning / `CorruptData` / `Unsupported` | plan | — |
| Scenario file invalid (fallback layout) | warning, default scenario | plan | — |
| Base: missing, not `.pkg`, is a companion, is the output, different Content ID, version not higher, not tool-built | `InvalidArgument` | validate / plan | — |
| Update changes nothing | `InvalidArgument` | plan | — |
| `img_create`: non-zero exit, `[Error]` with exit 0, no output, no companion for an update | `ExternalToolFailed` | run | first error lines + friendly explanation |
| Missing plaintext marker / header check failed | `VerificationFailed` | finalise | these tools do not make installable debug FPKGs |
| `img_verify` failed | warning (package kept) | verify | — |
| Output conflict: user cancels or old file cannot be renamed | `Cancelled` / `AlreadyExists` | finalise | — |
| Cancel | `Cancelled` | any | partial removed; logs kept |

### 6.6 Test plan

**Fake `prospero-pub-cmd`** (`native/tests/engines/fpkg/fake_pubcmd.cpp`, built by CMake into a fake toolkit layout
`<build>/tests/fake-toolkit/toolchain/prospero-pub-cmd[.exe]` with an empty `libScePubTools.dll` and an `ext/` folder,
plus a fake `prospero-dds2png[.exe]` that writes a minimal PNG of the requested colour type). It is our own
implementation of the behaviour already covered by `tests/fakes/fake_pubcmd.py`:

| Behaviour | Control |
|---|---|
| `img_create`: parse the GP5 with `QXmlStreamReader`; check `fmt="gp5"`, `volume_type`, `package` attributes, `chunk_info` presence for `prospero_app` and absence for `prospero_ac`, every `src_path` exists, CRLF line endings, no BOM; print `[Info] Creating an image...`, the `\|____…` ruler and 51 `=` without newlines; create the output early (size polling); write a 0x2000-byte package with `7F FIH`, superblock offset 0x1000 at 0x20, mode 13 at +0x1C, marker at +0x370; with `--ref_pkg_path` also write `<out>.remastered.pkg`; write `<out>.naps_metric.json`; print one routine `[Warn]` | `FAKE_PUBCMD_MODE` = `ok` |
| Tool without the plaintext profile | `stock` (no marker) |
| `[Error] Format of the png file is not valid (sce_sys/pic2.png)` with exit 0 | `error` |
| Exit 3 / exit `0xC0000135` | `crash` / `ntstatus` |
| Exit 0 without output | `nooutput` |
| Slow bar (for cancel and live CPU-limit tests) | `slow` |
| Bar split across writes, CR line endings, no bar at all | `split`, `cr`, `nobar` |
| Fail when any `src_path` is non-ASCII (proves aliasing) or a `dst_path` is reserved | `strictpaths` |
| Record argv, cwd, `TEMP`, `TMP` to `<out>.invocation.json` | always |
| `img_verify` pass/fail; `img_extract` writes a small tree or fails; `img_info` prints fixed text | `FAKE_PUBCMD_VERIFY`, `FAKE_PUBCMD_EXTRACT` |

**Unit tests** (Qt Test, synthetic fixtures written by the tests themselves; nothing taken from the reference):

| Area | Cases |
|---|---|
| Naming | version parse/canonicalise/compare/next; package names for `01.000.000`, `01.020.000`, `01.512.000`; `UPDATE_` and `.remastered.pkg` |
| Ordered JSON | key order kept, new keys appended, non-ASCII raw, integers/floats/`true`/`null` round-trip, 2-space indent, CRLF |
| `ParamPatch` | DRM missing/non-string/`free`; URI empty/non-string; attribute3 0/non-zero; FW lowering only when higher; version/title only when different |
| Size chooser | the four thresholds ± 1 byte; mount level 2→1→0 transitions; lv2 file limit; category missing / non-zero; invalid mount levels (1.0, true, "1") |
| Scanner | casefold order with `_`, digits and mixed case; every exclusion rule with its reason; junk-only folder = empty; emptied-by-exclusion folder ≠ empty; `sce_sys` subfolders never empty dirs; path-rule list; symlink and junction rejection (junction on Windows CI); cancellation at 2,000 entries |
| Dump rules | `_DUBLEX_`; `Engine/Saved` vs `Engine/{Saved,Other}`; `<project>/Saved` only with `Content/Paks`; AMPR candidates; DLC-emu candidates, mount points, `addcont<N>`, missing mounts |
| PlayGo | scenario validation (each rule); default object; payload names and chunk assignment for N = 1, 2, 100, more languages than chunks; fillers incl. name collisions; layouts A/B/C XML text; plgx parser on hand-built tables (bad magic, length, counts, masks, scenario lists); FICM/FLT detection incl. the misnamed case |
| GP5 writer | golden files written by us for small trees (exact bytes), escaping of `& < > " CR LF TAB`, entry order, `chunk` attribute rules, `<empty_dir>` after files, no trailing newline |
| Inputs | PNG IHDR parsing; DDS plan (missing PNG, wrong mode, duplicate); SELF magic rewrite on a synthetic file |
| Output parser | bar counting across arbitrary chunk splits; ruler/scale lines dropped; classification table; routine warnings; exit hints; first-three-errors message |
| Header check | good, stock, signed byte ≠ 0, bad offset, mode ≠ 13, file < 4096 |
| Conflict policy | ask / keep both / overwrite / cancel; companion and base never matched |

**Integration tests** (fake tool): full build, project contents, stock tool = failure, `[Error]` with exit 0 = failure,
exit code and no output, update build (companion, outputs per `updateOutput`, header on companion), cancel (process tree
gone, partials removed, logs kept, `exit_code=cancelled`), verify and extract jobs, non-ASCII game folder (Windows: junction
used, `TEMP`/`TMP` recorded), pre-scan counts, live CPU limit (Windows: affinity mask observed on the fake process), keep
intermediate on/off, two queued builds with the same Content ID and output never overlap.

**Optional local parity test** (developer machines only, skipped by default): when `FW_FPKG_TOOLKIT_SCRIPT` points to the
`create-gp5-from-folder.py` that came with the user's **own** toolkit and Python is available, run it with
`--absolute-paths --keep-keystone --chunk-count 100 --auto-size-profile sdk279` on a synthetic tree and compare the GP5,
scenario and `param.json` bytes with ours. Nothing from that script is stored in the repository.

**Hardware matrix** (manual, owner's tools and console): a small indie title; a multi-language title with a source PlayGo
table; a title without tables (fallback layout); a game with `dlc_emu.ini` and empty `addcont0`; an update over a
tool-built base; a game with non-ASCII folder names; a 95k-file game (pre-scan timing). Record install, launch, voice
languages, DLC mount, update install, and the logs.

---

## 7. Decisions needed, unverified points, corrections

### 7.1 Owner decisions

1. Licence of `native/`: decided. Version 5 is proprietary from 5.0.1 (`packaging/LICENCE.txt`); what the FPKG engine
   took from the public LibProsperoPkg source is credited in `packaging/THIRD_PARTY_NOTICES-native.md`.
2. Whether the native app may bundle Microsoft's `vc_redist.x64.exe` (Microsoft redistributable licence) or only link to it.
3. Default for `postVerify` (full integrity check can take as long as reading the package; speed UNVERIFIED).
4. Whether to accept `.gp5` sources through the Sony tool (the reference never does).

### 7.2 UNVERIFIED

1. Whether `img_create` prints the `=` bar when stdout is a pipe (the reference reads it from a pipe; the toolkit launcher
   says piping through `Tee-Object` stops repainting). The file-size poll is the fallback.
2. Final `sdkVersion` / `requiredSystemSoftwareVersion` values in tool-built packages (0 vs `0x1160000000000000`).
3. Output formats of `img_info`, `pkg_info`, `img_file_list`; whether `img_extract` can extract a subset and whether it
   writes `sce_sys` metadata from the CNT.
4. Effect of `--tmp_path` on `img_create` for PS5 (present in the binary; the reference uses `TEMP`/`TMP` instead).
5. `prospero_ac` add-on packages built through the Sony tool, and how the console treats them.
6. Real-console results for all PlayGo fixes (fillers, every chunk initial, every chunk in every scenario).
7. The PS5 flat-path-table hash, the UCP header layout and digest coverage, the exact `.sceversion` trailer rules.
8. A key-free way to tell a tool-built base package from a LibProsperoPkg-built one.
9. The package-name version component for unusual versions (e.g. minor ≥ 100 handled as "no truncation" only from two
   examples).
10. Whether real toolkits ship a `patch-manifest.json`.
11. Licence of garlic-savemgr; GitHub's licence field for the reference repository.
12. The screenshot state of the compression preset (Standard highlighted, SDK level box blank).
13. Python-compatible float formatting edge cases in `param.json` (exponents, `-0.0`, very large integers).

### 7.3 Corrections to `docs/farewell/FPKG_RESEARCH.md` and divergences in `core/fpkg`

| Topic | Previous text / Python behaviour | Verified reference behaviour |
|---|---|---|
| SDK temp folder | `<userTemp>/sdk-tmp-<8 hex>` | `<chosen temp folder>/sdk-tmp-<8 hex>` (`C:Services/SonySdkBuilder.cs:345`) |
| Pre-scan threads | "16–32" | clamp(2 × cores, 8, 32) |
| CPU limit | Windows affinity only | also macOS/Linux SIGSTOP/SIGCONT throttling (not relevant to us) |
| Post-build `img_verify` | listed as part of the reference flow | the reference never runs `img_verify`; it uses LibProsperoPkg's quick check |
| stdout decoding | console code page | UTF-8 in the reference; `native/src/core/ProcessRunner.h` uses the OEM code page (the tool's output is ASCII, so either works) |
| Output name `A` part | Python uses `masterVersion` when present | both `A` and `V` come from `contentVersion` |
| Mount level 1 → 0 | Python: above 162.5 GB | above 195.9 GB (lv1 limit) |
| Estimate above 260 GB | Python: error | warning; `img_create` decides |
| `applicationCategoryType` missing | Python: treated as 0 | error (auto-size requires 0) |
| `.dds` / `.auth_info` under `sce_sys` | Python: direct children only | any depth |
| Empty folders | Python: folders emptied by exclusions are declared | only empty or junk-only folders (plus DLC mounts) |
| Scenario sidecar | Python writes its own JSON shape | source file (validated) or the toolkit's default object; Python must be fixed |
| GP5 entry order | Python: scenario, files, payloads | scenario, recovered PNGs, payloads/fillers, files, empty dirs |
| Language payloads with 1 chunk | Python: none | all in chunk 0 |
| `RemovePlayGoFiles` default | — | false (settings default; migration to rev 3) |
| Update output default | — | UI "update only"; request/CLI "both" |

## 9. Parity with the reference at 3.1.0 (toolkit sdk-fpkg279-fix15), app version 5.0.3

Sections 3 and 4 describe the reference at 2.2.7 / 3.0.0 (toolkit profile `sdk279-plaintext-direct-v3`, fix12).
Its 3.1.0 release moved Sony SDK mode to Drakmor's `sdk-fpkg279-fix15` (profile `sdk279-plaintext-direct-v6`).
What changed there and is ported here (`libs/sony-sdk/scripts/create-gp5-from-folder.py` and
`src/PsViethoa.FpkgBuilder.Core/Services/SonySdkPlayGo.cs`, `SonySdkPlayGoSource.cs`, `SonySdkUds.cs` at 3.1.0):

| Rule | Reference | Here |
|---|---|---|
| SDK version | `sdkVersion` 0 or missing: the highest version of the `.sceversion` trailer of `eboot.bin` (major and minor) is written; `requiredSystemSoftwareVersion` is raised to `sdkVersion` when lower. App packages only. The fix15 publisher keeps both values; older toolkits write 0. | `applySdkVersionRule` (ParamJson), `executableSdkVersion` (InputRepair); applied again after "Lower the required firmware", so the required version never ends below `sdkVersion`; a warning after `img_verify` when the package carries SDK version 0, and the SDK version messages of `img_verify` count as problems when it carries a real one |
| SELF repair | PS4 signature replaced; a `.sceversion` trailer that starts up to 15 bytes before the size the SELF header names gets zero padding in front | `selfRepairPlan`, `writeSelfRepairCopy` |
| PlayGo chunk count | from the header of the source `playgo-chunk.dat` (1-800 chunks, 1-5 scenarios), else the setting (default 100) | `sourcePlayGoChunkCount` |
| PlayGo source layout | valid `playgo-ficm.dat` + `playgo-hash-table.dat`: chunk labels and languages from the chunk table, each file's chunk by path hash (seed and key from the hash table header, path as UTF-8 with a-z upper-cased, no leading slash; `sce_sys/*.png` also looked up as `.dds`); files the map does not name go to chunk 0; a chunk without label and languages is not declared; the scenario file gets the languages of the chunks. Not used when every file of a multi-chunk table is in chunk 0. | `readSourcePlayGoLayout`, `planToolkit` (PlayGo) |
| Scenarios | app default: every scenario lists every chunk (`0-(N-1)`), every chunk initial; labels from the chunk table when its scenario count matches `playgo-scenario.json`. The retail order and initial counts (`--sdk-retail-scenarios`) are off by default there and not offered here. | same |
| Generated layout | language i (counted from 1) in chunk min(i, N-1); chunk 0 lists every language | same (5.0.0 - 5.0.2 counted from 0) |
| UDS archives | `sce_sys/uds/npbind.dat` names the NPWR id of every `udsNN.ucp`: stale digest recomputed; missing, unreadable, incomplete or foreign archive replaced by an empty one (four definition files) | `readNpbindIds`, `checkUdsUcp`, `emptyUdsUcp` |
| Exclusions | root `decrypted/`, `playgo-languages/`, `entitlements.txt`, `entitlement_key.dat`, `sce_sys/lem_notification.dat`, `*/saved/config/*/*.ini` | `baseExclusionReason` |

Defaults that differed from the reference before 5.0.3 and no longer do: the source PlayGo layout (was off) and
the UCP digest repair (was off). Not ported: `--sdk-version` override, `--playgo-languages` selection (the
Companion's "All 31 PlayGo languages" stays), retail scenarios, AC volumes through the fix15 script.

`native/tests/data/fpkg/compare_with_reference.py` runs the reference script and this app on the same synthetic
folder and compares the projects entry by entry.
