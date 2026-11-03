# PS5 FPKG building: research for the exFAT Image Builder

Reference studied (read-only, never modified): `/home/user/thanhsondev/psviethoa-fpkg-builder`
(PSVIETHOA FPKG Builder 3.0.0, C#/.NET 10 + Avalonia, commit `6e43d85`). **That repository has no
licence file.** Everything below describes its *behaviour* in my own words so that we can write an
independent Python implementation. Do not copy its C#, its PowerShell launchers or the bundled
`create-gp5-from-folder.py` (that script has no licence either). Short command-line strings, XML element names and
on-disk constants are quoted because they are facts about Sony's file formats and tools.

Upstream LibProsperoPkg was shallow-cloned for study into
`/tmp/claude-0/-home-user-ps5-exfat-builder/1641cfe0-4df8-5262-a4e7-af1d894f2c5b/scratchpad/upstream/`
(`lpp/` = `drakmor/LibProsperoPKG` main, `feat/f` = its `feature/full-ppr-pkg-roundtrip` branch, history only).

---

## 0. TL;DR

* **Two engines in the reference.** The default engine is Sony's Publishing Tools 2.79, a *patched* build
  (`toolchain/prospero-pub-cmd.exe` + `libScePubTools.dll` + `ext/*.exe`). The reference runs it with one command:
  `prospero-pub-cmd.exe img_create --oformat nwonly [--compression_level N] [--ref_pkg_path BASE.pkg] PROJECT.gp5 OUT.partial.pkg`.
  The fallback is Drakmor's **LibProsperoPkg.dll** (fpkg-gui 0.6.12 engine, assembly 1.2.0). It is loaded **in-process**
  as a .NET library (`ProsperoPackageBuilder.Build(options, logCallback)`). The reference does not use a CLI for it.
* **The real work is the GP5 project.** It is a flat `<psproject fmt="gp5">` that lists every file with an absolute
  `src_path`. Generated or rewritten inputs (normalised `param.json`, PNGs recovered from DDS, PlayGo placeholder files,
  repaired SELF/UCP files) live in `.gp5-assets/<stem>/` next to the output. The source is never modified.
* **Python can do almost everything except the packing itself.** That covers source scanning, exclusions, `param.json`
  normalisation, PlayGo scenario/chunk XML, GP5 writing, progress parsing, output naming, header verification and
  cleanup. Packing, delta updates and Sony-side verify/extract need the user's `prospero-pub-cmd.exe`.
* **Hard blockers.** (1) An installable FPKG needs the *patched* "plaintext / custom-keystone" toolkit, whose origin and
  licence are unknown. Stock Publishing Tools output is not the PLAINTEXT_NOAUTH form. The older post-process conversion
  rebuilds an RSA-3072 CNT header wrap, so do not try to re-implement it. (2) The DLC path in the reference needs
  LibProsperoPkg, not the Sony tool. (3) The LibProsperoPkg binary in the reference (1.2.0+a3ee4c1) has no reachable
  public source, so it cannot be redistributed under GPL.

---

## 1. The reference's PS5 FPKG pipeline, end to end

Main orchestrator: `src/PsViethoa.FpkgBuilder.Core/Services/BuildEngine.cs` (`BuildAsync`).

### 1.1 Supported inputs and how they are resolved

`Services/SourceLocator.cs` (`Detect`, `Resolve`, `FindAppRoot`) recognises these inputs:

| Input | Detection | How it becomes an app folder |
|---|---|---|
| App folder | directory containing `sce_sys/` | used in place (read-only) |
| `.exfat` | extension or boot signature `"EXFAT   "` (bare volume, MBR or GPT; `ExFat/ExFatImage.cs`) | **mount** (macOS `hdiutil attach -readonly -nobrowse -noverify -noautofsck -plist`; Windows: a read-only Dokan drive served by the app's own exFAT reader, `ExFat/DokanImageMounter.cs`, `ExFatVirtualFileSystem.cs`), else **extract** to temp (`ExFat/ExFatExtractor.cs`: up to 8 parallel files, 4 MiB buffers, pre-sized outputs, OS junk skipped) |
| `.ffpfsc` | PS5 PFS container: superblock version 2, magic `0x1332A0B`, 64 KiB blocks, exactly one inner file (the exFAT image), usually PFSC-compressed (`ExFat/PfsContainer.cs`) | exFAT reader layered over LibProsperoPkg's PFS/PFSC random-access decompressor. Extract only; it cannot be mounted with hdiutil (Dokan can) |
| `.ffpkg` | FreeBSD UFS2: superblock at byte 65536, magic `0x19540119` at superblock offset 1372, 64 KiB blocks, 256-byte inodes, 12 direct and 3 indirect pointers (`ExFat/UfsImage.cs`) | always extracted (`ExFat/UfsExtractor.cs`), because no OS mounts it |
| `.gp5` | extension | read through LibProsperoPkg (`Gp5ProjectInfo.cs`: Normal layout = `rootdir` walk with `global_exclude`/`dir_exclude`/`file_exclude` `;`-separated wildcard masks matched on names; Flat layout = explicit entries). **The Sony path rejects GP5 sources** (`SonySdkBuilder.Supports` → "Sdk.NoGp5Source"), so these always use LibProsperoPkg, via a temp copy of the `.gp5` (`Gp5ProjectRedirect.cs`) |

The app root inside an image is the first directory (depth ≤ 3, breadth-first, junk folders skipped) that contains
`sce_sys`. `BuildEngine.DecideStrategy` chooses mount or extract. hdiutil cannot hide files or overlay files, so it
forces extraction when something must be dropped or patched. Dokan can overlay `param.json` and hide junk. On Windows
without Dokan the image is extracted. With the Sony engine, a mounted drive is fine because the GP5 just points at the
read-only files.

### 1.2 Engine selection

`BuildEngine.BuildAsync` uses Sony SDK mode (default on) only when all of these hold:
* the package kind is Application and the outer image mode is PLAINTEXT_NOAUTH and the source is not a `.gp5`
  (`SonySdkBuilder.Supports`);
* the toolchain is found (`SonySdkToolchain.Resolve`);
* no UCP under `sce_sys/trophy2` or `sce_sys/uds` is unreadable. A sealed UCP makes Publishing Tools reject the whole build,
  so the reference falls back to LibProsperoPkg.

`sce_sys/keystone` must exist and be exactly 96 bytes (`BuildEngine.RequireKeystone`).
If the Sony engine is unavailable, the reference falls back to LibProsperoPkg, **except for update packages**. Those
need Publishing Tools, so the build stops with "Patch.SdkRequired".

### 1.3 Sony SDK path (`SonySdkBuilder.BuildAsync`)

1. **Runtime.** Windows: checks that `msvcp140.dll`, `vcruntime140.dll` and `vcruntime140_1.dll` load (VC++ 2015-2022 x64)
   and, if missing, offers to run a bundled `vc_redist.x64.exe /install /quiet /norestart` with UAC.
   macOS/Linux: Wine with a private WINEPREFIX (`wineboot.exe --init` the first time), `WINEDEBUG=-all`,
   `WINEDLLOVERRIDES=mscoree,mshtml=;winemenubuilder.exe=d`. *(This does not apply to our Windows-only app.)*
2. **ASCII aliases** (`SonySdkPathAliases.cs`). `prospero-pub-cmd.exe` takes its command line in the ANSI code page and
   rejects non-ASCII `src_path` values ("File not found", "invalid attribute value src_path"). For every non-ASCII root
   (source → `src`, output → `out`, update overlay → `upd`, base pkg folder → `ref`), the app creates an NTFS
   **junction** in an ASCII, writable folder: `%TEMP%`, then the job temp folder, `%ProgramData%\…`, the output drive root.
   The source drive is never used. The folder name is `psviethoa-sdk-<pid hex>-<6 chars>`. Every path handed to the SDK
   goes through the junction. Junctions are removed afterwards, and stale ones are swept by PID at the next run.
3. **[1/3] GP5 project** (`SonySdkProject.Create`, §1.5), written next to the final package.
   Paths Publishing Tools will reject are listed, and the build stops *before* the SDK runs (`IsSdkPackagePath`).
   Measured rule: printable ASCII only, no `%`, no `;`, and no path component ending in `.`.
4. **Pre-scan (Windows only, optional, default on)** (`SonySdkPrescan.cs`). The app opens every source file and reads one
   byte, using 16-32 threads. The reason is that Windows Defender scans each file on its first open (about 11 ms), and
   img_create's "Checking files…" phase opens files one at a time. The reference measured about 25 min on 95k files
   without the pre-scan and a few seconds with it. The pre-scan writes nothing and leaves the package unchanged.
5. **[2/3] img_create** (`SonySdkRunner.CreateImageAsync`), see §1.4.
6. **Finish.** With the current toolkit ("fix6+" / profile `sdk279-plaintext-direct-v3`), the patched tool writes the
   PLAINTEXT_NOAUTH package directly. The app checks for the marker (`SonySdkConverter.IsAlreadyPlaintext`) and renames
   `<stem>.partial.pkg` to `<stem>.pkg`. Older toolkits needed a third step, `SonySdkConverter.ConvertInPlace` (a port of
   `postprocess-sdk279-plaintext.py`). That step writes the marker, clears CNT encryption flags, reseals imagedigs, FIH,
   digest table and rollup, rebuilds a "deterministic RSA-3072 CNT header wrap" via LibProsperoPkg
   `ProsperoPublisherRsa.BuildCntHeaderWrap`, and repairs PlayGo CRC32C blocks in the SI zip. **We should not
   re-implement this step.** Support only the direct-plaintext toolkit.
7. **Verify** (`BuildEngine.VerifyOutputAsync` → `PackageVerifier`, §1.8), then optional SHA-256, then cleanup (§1.7).

### 1.4 Exact Sony tool invocation, environment and output parsing

**Toolkit command, as run by the reference** (`SonySdkRunner.CreateImageAsync`, and the toolkit's
`libs/sony-sdk/build-from-folder.ps1`):

```
prospero-pub-cmd.exe img_create --oformat nwonly [--compression_level <-4..9>] [--ref_pkg_path <BASE.pkg>] <PROJECT.gp5> <OUT>.partial.pkg
```

* The toolkit default is level 7, which in the SDK means Oodle "Optimal" and is slow. The reference leaves the flag out
  unless the user picks a preset (Fast 2 / Standard 4 / Smallest 7 / Maximum 9).
* The toolkit's own `--build` bundle mode adds `--no_progress_bar` (`create-gp5-from-folder.py`, `build_bundle`).
  The reference leaves the progress bar on so that it can parse it.
* Other subcommands used by the toolkit GUI (`libs/sony-sdk/build-gui.ps1`):
  * verify: `img_verify --passcode <32 chars> --format_check on --integrity_check on|off --no_progress_bar <pkg>`
  * extract to an empty folder: `img_extract --passcode <32 chars> --no_progress_bar <pkg> <destFolder>`
  * For a delta update, both commands are pointed at the `<pkg>.remastered.pkg` companion instead. The GUI notes that
    img_verify "cannot parse a compact patch".
* The binary also contains `img_info`, `img_file_list`, `img_convert` (patch ↔ remastered), `pkg_info`, `pkg_compare`
  and the `gp5_*` project editors (`gp5_proj_create`, `gp5_file_add --chunk`, `gp5_dir_add --virtual true`,
  `gp5_chunk_add`, `gp5_scenario_add`…). I found these by scanning the binary's strings and did not run them. Other
  flags seen: `--tmp_path`, `--skip_digest`, `--passcode`, `--entitlement_key`, `--volume_type [prospero_app|prospero_ac|prospero_al]`.
  There is **no thread-count flag** for img_create: it starts one worker per core.

**Process setup (Windows):**
* `FileName` is the full path to `prospero-pub-cmd.exe`; arguments are passed as a list.
* Working directory: the toolchain folder if its path is ASCII, else `%TEMP%`, else the GP5 folder.
* stdout and stderr are redirected, and stdin is closed immediately.
* `TEMP` and `TMP` point to a per-job folder `<userTemp>/sdk-tmp-<8 hex>` (the toolkit's equivalent is
  `-TemporaryDirectory` / `LIBPROSPERO_TEMP_DIR`), so that Publishing Tools' scratch data stays off C:. The folder is
  deleted in `finally`.
* No other environment variables are needed on Windows.

**Progress parsing:**
* stdout is read **character by character**. The SDK prints a ruler line beginning `|____` (and a scale line `0        20…`).
  After that it appends up to **51 `=` characters** without a newline until the image is done.
  Percent = count(`=`) × 100 / 51.
* Separately, the size of `<OUT>.partial.pkg` is polled every 500 ms to show "bytes written".
* The phase weights in `PhaseCatalog.cs` (runtime 1.5, project 0.5, prescan 3, image 90, convert 1) show that about 95 % of
  the wall time is img_create.
* The toolkit's `build-from-folder.ps1` notes that piping stdout through `Tee-Object` makes the SDK stop *repainting* the
  bar. The reference reads raw redirected stdout and gets the `=` stream anyway. Verify this on real hardware, and keep
  the file-size poll as the fallback progress signal.

**Log and error classification** (`SonySdkRunner.Classify`):
* `[Error] …` → error. **Any `[Error]` line fails the run even with exit code 0.**
* `[Warn] …` → warning, except routine ones, which are downgraded to info: "The version of the SDK toolchain is invalid
  or missing", "(online check)", "has not been built for submission", "- Perform package verification", "- Rebuild the
  package for submission", "Process finished with warning(s)".
* `[Info]`/`[Debug]` and "Creating an image…" → info. `wine:` and `:err:` → warning. Ruler lines and blank lines are dropped.
* Success means exit code 0, no `[Error]` lines and the output file exists (otherwise "Sdk.NoOutput").
* Windows exit-code hints: `0xC0000135` = DLL missing (VC++ runtime); `0xC000007B` = bad image/architecture.
* Log files written in `<stem>-build-logs/`:
  * `01-create-gp5.log`: what the script would print (excluded files, conversions, size choice).
  * `02-img-create.log`: keys `command=`, `started_utc=`, `finished_utc=`, `elapsed=`, `exit_code=` (or `cancelled`),
    then the error lines.
  * `03-postprocess.log`: only for the legacy conversion.

**Known SDK error messages to map to friendly text:**
* "Format of the GP5 file is not valid … reserved node found (dst_path …)"
* "invalid attribute value dst_path/src_path"
* "File not found"
* "Format of the UCP file is not valid. (… ucp_header.ucp_digest is invalid)"
* "Format of the png file is not valid (sce_sys/pic2.png)"
* "Unexpected logical error. (NAPS metadata missing)" (a base pkg not built by Publishing Tools)
* "contentVersion must be higher"

**CPU limit (optional)** (`CpuLimiter.cs`): on Windows the reference sets the process CPU affinity to N cores, and the
limit can be changed while the build runs. Use `psutil.Process(pid).cpu_affinity([...])` if psutil is available.

### 1.5 GP5 project generation: fields, exclusions, PlayGo, compression

Reference: `SonySdkProject.Create`, `SonySdkPlayGo.ChunkInfoXml`; the toolkit's `create-gp5-from-folder.py` behaves the
same way (`--passcode 0…0 --absolute-paths --keep-keystone --chunk-count N [--auto-size-profile sdk279]`).

**Schema** (layout as written by Python's `ElementTree`, 2-space indent, `<?xml version='1.0' encoding='utf-8'?>`):

```
<psproject fmt="gp5">
  <volume>
    <volume_type>prospero_app</volume_type>            (prospero_ac / prospero_al for add-ons)
    <package passcode="<32 chars>" content_id="<36-char id>" />   (ac: + entitlement_key="<32 hex>"; al: entitlement_key only)
    <chunk_info chunk_count="N" scenario_count="S">          (APP volumes only)
      <chunks supported_languages="en-US ja-JP …" default_language="en-US">
        <chunk id="0" label="Chunk #0" layer_no="0" languages="…" />   … one per chunk 0..N-1
      </chunks>
      <scenarios default_id="0">
        <scenario id="0" type="playmode" initial_chunk_count="N" label="…">0-<N-1></scenario>   ("0" when N == 1)
      </scenarios>
    </chunk_info>
  </volume>
  <files>
    <file dst_path="sce_sys/playgo-scenario.json" src_path="<abs>\<stem>.playgo-scenario.json" chunk="0" />
    <file dst_path="sce_sys/pic0.png" src_path="<abs>\.gp5-assets\<stem>\sce_sys\pic0.png" chunk="0" />   (recovered PNGs)
    <file dst_path="playgo-languages/01-en-US.bin" src_path="…" chunk="1" pfs_compression="disable" />  (1 MiB zero payloads)
    <file dst_path="eboot.bin" src_path="<abs source path>" chunk="0" />                                 (every payload file)
    <empty_dir dst_path="addcont0" />    (reference addition: after the files; same element Publishing Tools writes for `gp5_dir_add --virtual true`)
  </files>
</psproject>
```

* Attribute escaping is ElementTree's (`& < > "` plus CR, LF and TAB as numeric references).
* `sce_sys/param.json` is mapped to the normalised copy, `.gp5-assets/<stem>/sce_sys/param.json`.
* Repaired SELF files map to `.gp5-assets/<stem>/normalized-self/<rel>`; repaired UCPs to `…/repaired-ucp/<rel>`.
* **File order matters.** Publishing Tools lays the image out in GP5 order. The order is a recursive walk with each
  directory's entries sorted by Python `name.casefold()`. Python reproduces this trivially. The reference emulates
  CPython casefold in C# (`PythonCaseFold.cs`).
* On Windows the toolkit script writes the GP5, scenario and `param.json` in text mode, which gives CRLF line endings.
  The reference matches that on purpose because the scenario file is copied verbatim into the package.

**Exclusions** (the script's `collect_files`; the reference adds the starred items):
* Inside `sce_sys/` (files directly in it): `keystone`† `pfs-version.dat disc_info.dat ext_info.dat imagedigs.dat license.info license.dat
  playgo-chunk.dat playgo-hash-table.dat playgo-ficm.dat playgo-scenario.json playgo-manifest.xml playgo-chunk.crc
  pfsimage.xml pfs-region-hints.json param.sfo param_cp_values.json pronunciation.sig origin-/target-deltainfo.dat
  origin-/target-param.json origin-/target-relocinfo.dat`
  * † The keystone is **kept** with `--keep-keystone`. The patched toolkit *requires* the dump's 96-byte keystone instead of
    deriving one from the passcode.
* The whole `sce_sys/about/` directory, and any `sce_sys` file ending in `.dds` or `.auth_info`.
* Root directories `sce_suppl/` and `sce_sc/`; a root `.gp5-assets/`.
* Anywhere: `*.gp4`, `*.gp5`, `*.esbak`, `*.playgo-scenario.json`; root `ampr_emu.index`; `fakelib/libSceAmpr.sprx` and
  `fakelib/libScePlayGo.sprx` (case-insensitive).
* Symbolic links and junctions are a hard error. Non-regular files are skipped.
* \* OS junk: `.DS_Store Thumbs.db ehthumbs*.db desktop.ini .localized`, `._*` AppleDouble files, `*.url`, and folders
  `__MACOSX .Spotlight-V100 .fseventsd .Trashes .TemporaryItems .AppleDouble $RECYCLE.BIN "System Volume Information"`
  (`JunkFileFinder.cs`).
* \* Optional "dump leftovers": root `_DUBLEX_`, `Engine/Saved`, `<project>/Saved` (`DumpLeftoverInspector.cs`).
* \* Optional: the DLC emulator files (`dlc_emu.ini`, `fakelib/libSceAppContent*`, `libSceNpEntitlementAccess*`,
  `libSceGameUpdate*`), when "strip DLC emu" is set.

**`param.json` normalisation** (read as utf-8-sig, written `json.dumps(indent=2, ensure_ascii=False)+"\n"`):
* `applicationDrmType = "standard"` is always set ("free" DRM shows a lock and refuses to start).
* Full APP builds (not updates) also get **automatic package size** (`SonySdkPackageSize.cs`, the script's `choose_package_size`):
  * estimate = mapped uncompressed bytes + max(512 MiB, 1 %) + 4096 × files;
  * `attributePub` = 0 if the estimate ≤ 162,514,599,936 bytes, 1 (lv1) if ≤ 195,878,191,104, 2 (lv2) if ≤ 260,436,918,272;
  * lv2 is limited to 500,000 files;
  * `kernel.addcontMountLevel` 2 is lowered to 1 above 97,945,387,008 bytes, and 1 to 0 above the lv1 limit, with a warning.
    This can break separately installed DLC mounting.
  * It requires `applicationCategoryType == 0`.
* Optional reference extras: clear `versionFileUri`, clear `attribute3`. Do **not** do this by default:
  attribute3 bits 7/19/23 are 120 Hz, VRR and PS5 Pro Enhanced.
* Optional reference extras: set `contentVersion` and default-language `titleName` from the UI.
* Publishing Tools rewrites `param.json` itself. It drops origin/target versions, rewrites `pubtools`, and zeroes
  `sdkVersion` / `requiredSystemSoftwareVersion`, because the toolkit lacks `prospero-llvm-readelf.exe`. The CHANGELOG
  says the values become 0; `docs/research/NOTES.md` saw `requiredSystemSoftwareVersion = 0x1160000000000000`. The two
  sources disagree, so check this on our own builds.

**Presentation images:**
* Every `sce_sys/*.dds` whose PNG is missing or has the wrong colour mode is converted with the toolkit's
  `prospero-dds2png.exe <in.dds> <out.png> [--preserve-alpha]` into `.gp5-assets/<stem>/sce_sys/`.
* `icon0*`, `pic0*` and `pic1*` must be 8-bit RGB (colour type 2); `pic2*` must be 8-bit RGBA (type 6, preserve alpha).
  Other images keep alpha.
* A wrong-mode PNG with no DDS is a hard error.
* The reference also replaces a "PNG" that is not a valid PNG (bad signature or IHDR, interlaced).
* The GP5 maps PNGs only; the SDK generates the DDS entries itself.

**SELF repair** (script `self_repair_plan`): files that start with the PS4 SELF magic `4F 15 3D 1D` get the PS5 magic
`54 14 F5 EE`, and a misaligned `.sceversion` trailer is re-padded. The result goes into `normalized-self/`.

**PlayGo** (CHANGELOG 2.2.0-2.2.7; `SonySdkPlayGo.cs`):
* *Script layout*, used when the dump has no usable `playgo-chunk.dat` (24 of 25 surveyed dumps):
  * N chunks (1-255, default 100); all game files in chunk 0.
  * One 1 MiB uncompressed zero payload per language listed in the dump's `sce_sys/playgo-scenario.json`. When there are
    fewer chunks than languages, the extra languages share the last chunk.
  * Every scenario is initial over all chunks.
  * The scenario JSON needs `scenarioCount` 1-5, contiguous ids, `scenarioDefaultId`, `scenarioDefaultLanguage` from the
    31-language list, a localisation per scenario in the default language, and `chunkDefaultLanguage` /
    `chunkSupportedLanguages`.
  * With no scenario file, a default one is synthesised from `param.json` `localizedParameters.defaultLanguage`.
* *Source layout* (reference only): when `playgo-chunk.dat` (magic `plgx`, version 0x1000, 1-255 chunks, 1-64
  scenarios) plus `playgo-ficm.dat` and `playgo-hash-table.dat` are valid, the original chunk ids, language masks,
  labels and per-file chunk are kept. Files are matched to chunks through the PS5 flat-path hash, which is complex.
* Fixes added in 2.2.7 for in-game failures:
  * every scenario lists **every** chunk (the source order is kept and missing chunks are appended);
  * chunks with no data get a 1 MiB placeholder `playgo-chunks/NNN.bin`, because an empty chunk counts as "not installed"
    (Control stalled at "Installing game… 0GB/0");
  * all chunks are initial (Ghost of Yōtei showed CE-108111-2 otherwise).

**Empty directories** (2.2.7): every empty source folder outside `sce_sys`, and every `dlc_emu.ini` `mount_point`
(`/app0/addcontN`), is declared with `<empty_dir dst_path="…"/>`. The script lists files only, so these folders vanished
and DLC emulation could not mount.

**UCP repair:** `sce_sys/trophy2/*.ucp` and `sce_sys/uds/*.ucp` carry a self-digest. A stale digest makes the SDK
refuse the build, so the reference writes a corrected copy (`UcpInspector.cs`, using a LibProsperoPkg routine). It
falls back to LibProsperoPkg when a UCP is unreadable.

### 1.6 Output naming

* Name: `<contentId>-A<vv>-V<vv>.pkg`, where `<vv>` is the 2-digit major followed by the minor at least 2 digits, with no
  truncation. `01.000.000` gives `…-A0100-V0100.pkg`; `01.512.000` gives `…-A01512-V01512.pkg`.
* Updates: `UPDATE_<contentId>-A<vv>-V<vv>.pkg` plus the companion `<contentId>-A<vv>-V<vv>.remastered.pkg`, renamed from
  the SDK's `<partial>.pkg.remastered.pkg`.
* If the name already exists, the user is asked to overwrite, keep (the old file becomes `… (1).pkg`) or cancel
  (`OutputConflict.cs`). The base package is never offered for overwrite.
* Content ID regex: `^[A-Z]{2}[0-9]{4}-[A-Z]{4}[0-9]{5}_[0-9]{2}-[A-Z0-9]{16}$`. Title ID = characters 7-15.

### 1.7 Temp layout and cleanup

| Item | Where | Removed |
|---|---|---|
| `<stem>.gp5`, `<stem>.playgo-scenario.json` | output folder | after success (unless "keep intermediate"); **kept on failure** for diagnosis |
| `.gp5-assets/<stem>/…` (param.json, PNGs, normalized-self, repaired-ucp, playgo-languages, playgo-chunks) | output folder | after success; the empty `.gp5-assets` root is removed too |
| `<stem>.partial.pkg`, `<stem>.partial.pkg.remastered.pkg`, `<stem>.partial.pkg.naps_metric.json` | output folder | deleted on any failure or cancel; `.naps_metric.json` deleted after success |
| `<stem>-build-logs/` | output folder | always kept |
| `sdk-tmp-<8hex>` (SDK `TEMP`/`TMP`) | user temp folder | `finally` |
| extracted image staging, base-package extraction for updates | user temp folder | `finally` (`BuildEngine` finally block) |
| ASCII junctions `psviethoa-sdk-<pid>-<id>` | `%TEMP%` / ProgramData / output root | `Dispose`; stale ones swept by PID |
| Dokan or hdiutil mount | — | unmounted in `finally` |

Disk estimate (`DiskSpaceAdvisor.cs`): output needs about 1.1 × source. In SDK mode temp needs nothing beyond image
extraction (1.02 × staging). The app warns when temp or output is on FAT/FAT32, because packages exceed 4 GiB.

### 1.8 Verification (`Services/PackageVerifier.cs`)

Structural checks, all implementable in Python:
1. The file is at least 4096 bytes.
2. Bytes 0-3 are `7F 46 49 48` (`\x7FFIH`).
3. FIH byte 5 (the "signed byte") must be `0x00`. A debug FPKG is unsigned.
4. u64 LE at FIH+0x20 is the outer superblock offset; it must be inside the file (+896).
5. u16 LE at superblock+0x1C (outer mode) must equal `13` (0x000D).
6. For PLAINTEXT_NOAUTH, the 16 bytes at superblock+0x370 (880) must be ASCII `PPRPLAIN-NOAUTH!`.
7. Optional SHA-256 of the whole file (4 MiB reads).
8. Informational, via LibProsperoPkg: package type, Content ID, entry count, "official" flag.

Other FIH fields the reference reads (`SonySdkConverter.cs`): outer PFS offset is u64 at 0x10, CNT offset is u64 at 0x58,
and the CNT block starts with `7F 43 4E 54` (`\x7FCNT`).

Engine checks (LibProsperoPkg `ProsperoPackageArchive.VerifyPackageQuick/Full`): segment ranges, CNT signature and entry
digests, PlayGo layout, outer PFS superblock, NAPS, inner PFS inodes, SI directory. The full check also decrypts every
outer block against `imagedigs.dat` and test-decompresses every file. **Without LibProsperoPkg, use the user's Sony tool
`img_verify` instead.**

After the build the reference also re-reads the package to confirm that the DLC-emulator files and mount folders are
inside (`BuildEngine.ConfirmDlcEmuInPackage`). It logs the SDK's `sdkVersion` / `requiredSystemSoftwareVersion`
rewrite (`LogSdkVersionChange`).

### 1.9 Extraction of existing PKGs

`PackageReader.cs` / `PackageInspector.cs` use LibProsperoPkg only (`ProsperoPackageArchive.Inspect`, `ExtractOuterFiles`,
`ExtractCntEntries`, `ExtractSiEntries`, `DecryptOuterPfs`, `ProsperoNapsImage.DecompressRange`).
* PLAINTEXT_NOAUTH packages are read in place, decompressing NAPS in 4 MiB chunks with a 64 MiB LRU cache.
* Native (AES-XTS) packages are decrypted to a temp file first.
* Retail packages are information-only.
* The output is in "Sony layout": `param.json`, icons and PlayGo tables from the CNT are merged back into `sce_sys/`.

The Sony-tool equivalent is `img_extract --passcode … --no_progress_bar <pkg> <emptyDir>`.

### 1.10 Update / delta packages (against a base)

* Publishing Tools 2.79 has no patch volume type. An update is a `prospero_app` with a **higher `contentVersion`**, built
  with `--ref_pkg_path <base.pkg>`. The SDK writes a block-level delta tied to that exact base, plus a full
  `.remastered.pkg` companion.
* `SonySdkPatchReference.Inspect` checks before compression starts that the base exists, is a `.pkg`, has the same
  Content ID and a lower version, and has a supplement region.
* `SonySdkReferenceProbe` rejects a base **built by LibProsperoPkg**. Its `prev_suppl/common/etc/naps_meta_18.dat` is
  encrypted with the library's key, so the SDK fails with "NAPS metadata missing" only after compressing everything
  (4 min on 33 GB). The probe needs LibProsperoPkg's `DecryptMeta18`.
* The source may be a full new version or only an "update folder" holding the changed files. In the second case the app
  fills in the missing files from an "original game folder", or extracts just those files from the base pkg into temp.
  A project missing files produces a version with those files *deleted*.
* An update that changes nothing is refused.
* For updates, `attributePub` is *not* auto-sized.
* Output choice: update only, full game, or both. The unwanted file is deleted after verification; the two forms convert
  into each other with `img_convert`.
* The structure check runs on the `.remastered.pkg`, because a delta has no FIH header.

### 1.11 DLC / add-on packages (`DlcPackageBuilder.cs`, `DlcEmuIni.cs`, `DlcEmuInspector.cs`)

* `dlc_emu.ini` (drakmor/dlc_emu format) has `[PSAC]` blocks with `content_id`, `download_status` and an optional
  `mount_point`. `[PSAL]` and `[PSCONS]` blocks are skipped. The file is capped at 64 KiB.
* Each PSAC entry is built **with LibProsperoPkg only**:
  * mode `AdditionalContentData`, DebugImage, version `01.000.000`, passcode 32 zeros, `GenerateParamJsonIfMissing`,
    deterministic;
  * image mode follows the Build tab (PLAINTEXT_NOAUTH by default since 2.2.7; before that, Native by accident);
  * the payload is an empty folder for `NO_EXTRA_DATA`, or the dump's `<mount_point>` folder when it exists and has files.
    A missing data folder still gives an unlock-only package, with a warning.
* The "entitlement-only" PSAL form (content_type 0x22) is a bare CNT with no FIH or PFS, and the PS5 debug installer
  refuses it. So even data-less DLC is built as AC-with-data.
* The Sony tool supports `prospero_ac` / `prospero_al` volume types and `entitlement_key`. The toolkit script can write
  such a GP5, but its own `--build` mode is APP-only and the reference never builds DLC through the SDK. **DLC through
  `prospero-pub-cmd` is untested**, and the reference reads the entitlement key from `license.info` through LibProsperoPkg.

### 1.12 PFS v2/v3 and Kraken: what is actually implemented

* PFS v2/v3 format, Kraken level, block size 128-256 KiB, shuffle patterns, shuffle analysis, layout optimisation and
  thread count are **only options passed to LibProsperoPkg** (`BuildEngine.CreateOptions`). The app implements none of
  these algorithms. They are greyed out in SDK mode.
* The Kraken encoder and decoder live in LibProsperoPkg (`PFS/Compression/Oodle/*`). On Windows, an optional native Oodle
  backend is used through `libScePubTools.dll` (`PublishingToolsLocator`, env `LIBPROSPERO_PUBTOOLS_PATH`).
* The CHANGELOG notes (per the library author) that PFS v3 packages need PS5 firmware 7.00 or later.
* The CHANGELOG itself admits that several 0.6.12 release-note features ("exclude Unreal Engine runtime", "recreate retail
  PlayGo scenario layout / early-launch flags", "UDS/UCP creation on corruption") are **not in the shipped binaries**.
* In SDK mode, compression is Publishing Tools' own `--compression_level`.

### 1.13 Large files and cancellation

* Large inputs: files over 4 GiB are common (19 of 25 games; the largest was 67.7 GiB). Up to 500k files at lv2. GP5
  writing checks for cancellation every 2,000 entries, and the source walk is parallelised across 8 sub-folders.
* Cancel: `process.Kill(entireProcessTree)`; partial `.partial.pkg`, `.remastered` and metric files are deleted; GP5
  and logs are kept. Cancelling while the GP5 is written or during pre-scan is cooperative.
* CLI exit codes: 0 ok, 1 bad arguments, 2 failed, 3 cancelled.

---

## 2. What the user must provide, and how the reference finds it

**Sony toolkit (the user supplies it; never bundle it).** The reference bundles it in `libs/sony-sdk/`, which is a
legal liability on their side. Layout:
```
<toolkit>\toolchain\prospero-pub-cmd.exe        (patched, profile "sdk279-plaintext-direct-v3")
<toolkit>\toolchain\libScePubTools.dll
<toolkit>\toolchain\ext\{da.exe, fa.exe, p2d.exe, ric.exe, sc2.exe, ispc_texcomp.dll, libatrac9.dll}
<toolkit>\prospero-dds2png.exe                  (optional; DDS→PNG recovery)
<toolkit>\scripts\create-gp5-from-folder.py     (not needed by us)
```
* Lookup (`SonySdkToolchain.FindToolchain`): env `PSVIETHOA_SONY_SDK`, then folders next to the app. Both a root that
  contains `toolchain\` and the toolchain folder itself are accepted. The only check is that `prospero-pub-cmd.exe`
  exists. Profile: `SonySdkToolchain.Profile = "sdk279-plaintext-direct-v3"`.
* The toolkit script's `--build` mode looks for a `patch-manifest.json` with a `profile` key
  (`sdk279-plaintext-direct-v3` / `sdk313-plaintext-direct-v3`) in the Publishing Tools folder. The bundle here has none.
* Python's own default path in that script is `C:\SCE\Prospero\Tools\Publishing Tools_2.7.9-plaintext-custom-keystone-v3\bin`,
  and `PublishingToolsLocator` also tries `C:\SCE\Prospero\Tools\Publishing Tools\bin` and `%SCE_PROSPERO_SDK_DIR%`.

**Other requirements:**
* **VC++ 2015-2022 x64 runtime** (the msvcp140 / vcruntime140 / vcruntime140_1 DLLs).
* **Dokan 2.x** (optional): only for copy-free mounting of images on Windows.
* **.NET**: not needed for the Sony path. It is needed only to host LibProsperoPkg (the reference ships self-contained .NET 10).
* **A 96-byte `sce_sys/keystone`** in the source.
* **NTFS** for the junction folder, and NTFS or exFAT, not FAT32, for output.

**How `ComponentProbe.cs` reports status:** one row per component with Ok, Warning, Missing or N/A. Rows: LibProsperoPkg
engine version, debug keys available, Sony SDK (toolchain dir + VC++ state), Kraken built-in, native Oodle DLL,
mounting (hdiutil, or Dokan driver installed and running), sleep inhibitor, PS4 tools.

**Where `SonySdkPathAliases` is required:** whenever the source, output, temp or base path contains a non-ASCII
character. Many user names and game folders do ("Ghost of Yōtei", Vietnamese or CJK names, a trailing U+F028).

---

## 3. LibProsperoPkg: authorship, upstream, licence, CLI

* The reference credits "Main project: **Drakmor** (LibProsperoPkg)". The DLL is taken from Drakmor's **fpkg-gui 0.6.12**.
* The embedded assembly version is `1.2.0+a3ee4c1a6c37c6689717416c4962fa544e063e2c`.
* The PDB's Source Link maps `C:\dev\ps5\LibProsperoPKG\*` to
  `https://raw.githubusercontent.com/drakmor/LibProsperoPKG/a3ee4c1…/*`.
* **The upstream repo is public:** `https://github.com/drakmor/LibProsperoPKG` (`git ls-remote` works).
  * `main` (HEAD `be79990`): 26 commits, all authored by **SvenGDK**. Version 2.5.0, `<Authors>SvenGDK</Authors>`,
    NOTICE "Copyright (C) 2026 SvenGDK". Parts are derived from maxton's LibOrbisPkg (GPL-3.0).
    `SvenGDK/LibProsperoPKG` also exists (HEAD `748eabf`, tag `v1.0`). So the library appears to originate with SvenGDK,
    and drakmor's repo is his fork or working copy. *This contradicts the reference's "Drakmor" credit somewhat; credit
    both.*
  * `feature/full-ppr-pkg-roundtrip` (HEAD `5e26c8a`): 30 commits by "Roman Tarasov" (presumably drakmor). Version 1.2.0,
    the same version as the bundled DLL. It adds publisher-compatibility, NAPS and plaintext work.
  * **Commit `a3ee4c1` itself is not fetchable** ("not our ref"). The exact source of the shipped DLL is not public.
* **Licence: GPL-3.0-or-later** (LICENSE + NOTICE + `PackageLicenseExpression`) on both branches.
  * Our app is already distributed under GPL-3.0 obligations (Lazy_MkPFS; see `THIRD_PARTY_NOTICES.md`), so the licences
    are compatible.
  * We could use a build from the public source, provided we ship or point to the matching source.
  * We should **not** redistribute the fpkg-gui DLL, because its corresponding source is not available.
* **fpkg-gui** itself: `github.com/drakmor/fpkg-gui` is not publicly reachable (ls-remote asks for credentials, so it is
  private or does not exist). The reference names no URL for it.
* **CLI availability:**
  * The library is designed to be called in-process. Its README says "without an external command-line tool".
  * The drakmor feature branch contains **`src/PprPfsKrakenTool`**, a research console tool (net9.0, GPL). Commands
    include `build-pkg <source-dir> <output-dir> <content-id> <app|ac|al> [passcode] … [deterministic] [strict]`,
    `inspect-pkg`, `check-pkg-fih`, `check-pkg-imagedigs`, `check-pkg-entry-digests`, `check-pkg-signature`,
    `extract-pkg-outer|inner|cnt|si`, `encode-dds`, `roundtrip-gp5`, `selftest`. It is a developer tool with positional
    arguments, not a stable interface.
  * The `main` branch can be built as a **C-ABI shared library** (`libprosperopkg.h`, `.github/native/`) with exports such
    as `lpp_build_package(_ex)`, `lpp_validate_package`, `lpp_extract_package`, `lpp_inspect_package`,
    `lpp_ucp_repair_digest_file`, `lpp_encode_png_to_dds`, `lpp_last_error`. That could be called from Python through
    `ctypes`, but only CI builds produce it (Linux/macOS workflows; no Windows artifact seen). It is version 2.5.0, a
    different engine from the 1.2.0 the reference ships.
  * The reference's own `fpkg-cli` (`src/PsViethoa.FpkgBuilder.Cli`: `build`, `verify`, `pkg-extract`, `dlc-from-ini`…)
    is a CLI, but it has no licence, so we cannot bundle or depend on it.

---

## 4. Known issues and limitations (CHANGELOG / README / docs)

1. **"Package errors after playing a while, DLC not picked up"** (2.2.7 report). Root causes found in SDK mode:
   * scenarios carried only some of the chunks;
   * empty folders, including DLC mount points, were dropped;
   * stale UCP digests (the SDK refused such dumps outright);
   * empty PlayGo chunks stalled installs.

   All four were fixed in the reference's GP5 generation (§1.5). Our generator must include these fixes from day one.
2. **DLC gaps:**
   * DLC is built only through LibProsperoPkg.
   * Entitlement-only (PSAL) packages are refused by the debug installer.
   * `[PSAL]` and `[PSCONS]` blocks are not built.
   * DLC data is found only when the `mount_point` folder exists in the dump.
   * Before 2.2.7 DLC used the Native image mode by mistake.
   * Lowering `addcontMountLevel` for large games can stop separately installed DLC from mounting.
   * DLC-emu requires every `mount_point` folder to exist; it does not create it.
   * The emulator only works when the game runs as a folder (ShadowMount+). An installed package does not load it.
3. **Update limits:**
   * the base pkg must be built by Publishing Tools, not LibProsperoPkg;
   * same Content ID and a strictly higher `contentVersion`;
   * patch-only output has no FIH, and img_verify cannot read it, so verify the `.remastered.pkg`;
   * the 2.2.3 deeper case (supplement present but `naps_meta` missing inside) is not detected cheaply.
4. **Path rules:** non-ASCII, `%`, `;` and a component ending in a dot are all rejected by the SDK. Many reserved
   `sce_sys` names cause "reserved node found".
5. **Metadata side effects:** Publishing Tools zeroes or rewrites `sdkVersion` / `requiredSystemSoftwareVersion` (no
   `prospero-llvm-readelf.exe`). SDK packages have no `sce_sys/about/right.sprx`, because it is reserved and not
   regenerated; that only affects the copyright screen. LibProsperoPkg silently removes `fakelib/libSceAmpr.sprx` and
   `fakelib/libScePlayGo.sprx`.
6. **Game-specific hangs packaging cannot fix:** AMPR games (`libSceAmpr` imports) can hang on the splash screen, and the
   remedy is to run them as a folder. Stellar Blade needed the `_DUBLEX_` / `Engine/Saved` cleanup.
7. **Performance:**
   * Publishing Tools at level 7 is slow (2 h 13 min on a 184 GB game with source and destination on the same USB drive).
   * Windows Defender's first-open scan dominates the file-check phase.
   * There is no thread flag; the reference limits CPU through affinity instead.
8. **Unverified claims:** the voice-chunk fix for Ghost of Yōtei was structurally verified but "PS5 confirmation pending".
   `docs/research/ps5-results.md` has an empty results table.
9. **Toolkit provenance:** the "sdk-fpkg279-fix12" toolkit is a patched Sony SDK of unknown origin with no licence. Its
   README itself says the profile is "diagnostic plaintext/unsigned output, not an installable console image". It
   installs only with homebrew (kstuff / etaHEN).

---

## 5. Integration design for the exFAT Image Builder (Python/Tkinter)

### 5.1 Where it plugs in

`ui/tab_unibuild.py` already does the following:
* builds jobs as dicts (`queue.append({...,'type': t, ...})` around line 1923);
* dispatches them in `_uq_dispatch(job, cb)` (line 2649) to `_dispatch_exfat`, `_dispatch_ffpkg` or `_dispatch_pfs*`;
* finishes through `_uq_done(job, ok)`, where `ok` is `True`, `False`, `'warn'` or `None`;
* reports progress with `_uq_set_progress(pct, step, eta)` (thread-safe; it bounces itself to the Tk thread);
* signals cancellation with `uq_state['cancel']`, read by workers through `cancel_check=lambda: not uq_state.get('running', True)`;
* cleans up with `_uq_cleanup_job`, which keeps finished artifacts and deletes partials this run created.

`ui/mkpfs_runner.py` already has the pattern we need: a subprocess with `CREATE_NO_WINDOW`, a reader thread and
terminate-then-kill on cancel. Reuse it.

Add a fourth type: `'fpkg'` (`_TYPE_EXT['fpkg'] = '.pkg'`, `_TYPE_LABEL['fpkg'] = 'FPKG'`). In `_uq_dispatch`, `t == 'fpkg'`
calls `_dispatch_fpkg(job, cb)`. That starts a worker thread running `FpkgSonyBackend(job, settings).run(progress, log,
cancel_check)` and calls `cb(True / 'warn' / False)` through `app.after(0, …)`.

Job dict additions:
* `src_kind`: one of `folder`, `exfat`, `ffpkg`, `ffpfsc`, `gp5`.
* Build options: `compression_level` (None or -4..9), `chunk_count` (1-255, default 100), `ref_pkg` (None or a path),
  `patch_output` (`'update'`, `'full'` or `'both'`), `keep_intermediate`, `prescan`, `cores`.
* `passcode`: 32 zeros by default.
* Results filled in by the backend: `content_id`, `content_version`, `out_path`, `_final_path`, plus
  `_partial_paths` (list).

Settings (`app._settings`):
* `sony_pubtools_dir`: the user picks the toolkit root or the `toolchain` folder, or `prospero-pub-cmd.exe` itself.
  Never bundled, never downloaded.
* `sony_dds2png_path` (optional) and `fpkg_temp_dir`.

### 5.2 Module layout (new, independent code)

```
core/fpkg/__init__.py
core/fpkg/toolchain.py     locate + validate the user's toolkit; VC++ check; version/profile probe
core/fpkg/source.py        resolve source -> app folder (reuse app's existing exFAT/ffpkg/PFS extract or mount)
core/fpkg/scan.py          walk + exclusions + junk + casefold order + path-rule checks + empty dirs
core/fpkg/param.py         param.json read/normalise (DRM standard, attributePub auto-size, mount level)
core/fpkg/playgo.py        scenario JSON validation/synthesis, chunk_info XML, language/empty-chunk placeholders
core/fpkg/gp5.py           GP5 XML writer (ElementTree, CRLF, attribute order as in §1.5)
core/fpkg/images.py        PNG IHDR colour-mode checks; optional DDS->PNG via user's prospero-dds2png.exe or Pillow
core/fpkg/runner.py        subprocess + char-wise '=' progress + [Error]/[Warn] classification + logs 02-img-create.log
core/fpkg/verify.py        FIH/superblock/marker checks, optional SHA-256, optional img_verify
core/fpkg/aliases.py       ASCII junctions (Windows) for non-ASCII roots; stale sweep by PID
core/fpkg/backend.py       FpkgSonyBackend: orchestrates the steps below, cleanup, result dict
```

### 5.3 `FpkgSonyBackend.run()` steps

1. **Validate the toolchain** (`toolchain.py`):
   * accept a root, its `toolchain\` folder, or the exe path;
   * require `prospero-pub-cmd.exe`, `libScePubTools.dll` and an `ext\` folder containing `ric.exe` and `sc2.exe`, at least;
   * check that the VC++ runtime loads with `ctypes.WinDLL('msvcp140.dll')`, then `vcruntime140.dll` and
     `vcruntime140_1.dll` (catch `OSError`);
   * optionally read `patch-manifest.json` → `profile`;
   * if there is no profile, mark the toolkit "unverified". The first successful build confirms the patched profile
     through the `PPRPLAIN-NOAUTH!` marker check.
   * Store the result in settings.
2. **Resolve the source** (`source.py`):
   * folder: use it in place;
   * `.exfat`, `.ffpkg`, `.ffpfsc`: extract into `<temp>/fpkg-<jobid>/stage` with the app's existing extractors. A
     read-only mount (OSFMount) is also fine, because the GP5 only reads.
   * `.gp5`: phase 2 (pass it to img_create unchanged after ASCII and reserved-name checks, or rewrite it).
3. **Pre-flight**, all before running anything slow:
   * `sce_sys/param.json` parses (utf-8-sig); `contentId` matches the regex; `contentVersion` is parsed;
   * `sce_sys/keystone` is exactly 96 bytes;
   * output and temp are not FAT32; free space ≥ 1.1 × source on the output drive;
   * the output name is computed (§1.6), and the conflict policy follows the queue's existing behaviour (rename, keep both).
4. **Scan** (`scan.py`):
   * a recursive walk with `os.scandir`, entries sorted with `key=lambda e: e.name.casefold()`;
   * a symlink or junction is a hard error;
   * apply the exclusions of §1.5 and the OS-junk rules;
   * record empty directories outside `sce_sys`;
   * collect any destination path that breaks the SDK rules (non-ASCII, `%`, `;`, component ending in `.`) and fail with
     the list. Do this as a cheap pre-check, not after minutes of work.
   * Check cancellation every 2,000 entries.
5. **Generated assets**, all under `<outdir>\.gp5-assets\<stem>\`:
   * `sce_sys/param.json` with `applicationDrmType="standard"` and, for full APP builds only, the `attributePub` /
     mount-level selection;
   * `<stem>.playgo-scenario.json`, copied from the source and validated, or synthesised;
   * `playgo-languages/NN-<lang>.bin` (1 MiB of zeros, `pfs_compression="disable"`);
   * `playgo-chunks/NNN.bin` placeholders for empty chunks;
   * PNGs recovered from DDS (optional);
   * SELF repair (phase 2: detect and warn first).
6. **Write the GP5** (`gp5.py`) with the schema of §1.5:
   * `<empty_dir>` for empty folders and `dlc_emu.ini` mount points;
   * CRLF line endings;
   * every `src_path` absolute and passed through `aliases.map()`.
   * Also write `<stem>-build-logs\01-create-gp5.log`, listing excluded files with their reasons.
7. **Optional pre-scan:** a `ThreadPoolExecutor(16-32)` opens each file and reads one byte. Check cancellation between files.
8. **Run img_create** (`runner.py`):
   ```
   [exe, 'img_create', '--oformat', 'nwonly'] + (['--compression_level', str(lvl)] if lvl is not None else [])
       + (['--ref_pkg_path', ref] if ref else []) + [gp5, partial]
   ```
   * `subprocess.Popen(argv, cwd=<ascii toolchain dir or %TEMP%>, stdin=DEVNULL, stdout=PIPE, stderr=PIPE, env=env,
     creationflags=CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP)`, where `env` = os.environ plus `TEMP`/`TMP` =
     `<temp>\fpkg-<jobid>\sdk-tmp`.
   * The stdout thread calls `os.read(fd, 256)` and decodes leniently (`errors='replace'`, the console code page). It
     ends a line at CR/LF, starts counting `=` after a line that begins `|____`, and sends `pct = 3 + 90 * n / 51` to
     `_uq_set_progress`.
   * The stderr thread reads lines.
   * A 0.5 s timer polls `os.path.getsize(partial)` and shows "written X GB" in the step text, so progress still moves if
     the bar is silent.
   * Classify output as in §1.4 and collect `[Error]` lines.
   * When the process exits, write `02-img-create.log` with keys `command`, `started_utc`, `finished_utc`, `elapsed`
     and `exit_code`.
   * Success = exit code 0, no `[Error]` lines and the partial file exists. Map exit codes `0xC0000135` and `0xC000007B`
     to friendly messages.
   * Optional: when `cores < cpu_count`, apply CPU affinity with psutil.
9. **Finalise:**
   * full build: check the plaintext marker on `partial`. If it is missing, **fail** with "this Publishing Tools build is
     not the plaintext/custom-keystone profile; the output is not an installable FPKG" and keep the logs. Otherwise
     rename `partial` to `<stem>.pkg` and delete `<partial>.naps_metric.json`.
   * update: `<partial>.remastered.pkg` must exist. Rename `partial` to `UPDATE_<stem>.pkg` and the companion to
     `<stem>.remastered.pkg`, then keep the files the user asked for.
10. **Verify** (`verify.py`):
    * run the header checks of §1.8 (on the `.remastered.pkg` for updates);
    * optional SHA-256;
    * optional `img_verify --passcode <pc> --format_check on --integrity_check on --no_progress_bar <pkg>` with the user's
      tool; exit 0 and no `[Error]` lines means a pass.
    * A failure of the optional step returns `'warn'` (the package is kept). A failure of the mandatory header check
      returns `False`.
11. **Cleanup:**
    * success: delete the GP5, the scenario file and `.gp5-assets\<stem>` (and the root folder if it is empty), unless
      `keep_intermediate` is set;
    * always: delete `sdk-tmp`, the staging extraction and the junctions; keep `-build-logs`;
    * failure: also keep the GP5 and assets for diagnosis, and delete the partials.

**Cancel** (`cancel_check()` true, or the queue's `uq_state['cancel']`):
* before img_create: stop cooperatively;
* during img_create: run `taskkill /PID <pid> /T /F` (fallback `proc.kill()`), wait, then delete `partial`,
  `partial.remastered.pkg` and `partial.naps_metric.json`.

This is safe because the SDK writes only to `*.partial.pkg` and the final name appears only on success. The cancel
dialog in `_uq_cancel` should say that FPKG builds stop immediately.

### 5.4 Pure Python vs the user's Sony tool

| Piece | Pure Python | Needs `prospero-pub-cmd.exe` | Needs LibProsperoPkg |
|---|---|---|---|
| Toolchain lookup and validation, VC++ check | yes | (optional probe) | |
| Source resolution: folder, and extraction of `.exfat`/`.ffpkg`/`.ffpfsc` (already in the app) | yes | | |
| `param.json` read, normalisation, auto-size; `param.sfo` reading (PS4-style, if wanted) | yes | | |
| Exclusions, junk, casefold order, path rules, empty dirs | yes | | |
| `playgo-scenario.json` validation, script-layout chunk XML, placeholders | yes | | |
| Source-layout PlayGo (parse `playgo-chunk.dat`, ficm, hash-table, flat-path hash) | possible (complex; phase 2) | | |
| GP5 XML | yes | (`gp5_*` subcommands can also edit it) | |
| PNG colour-mode check | yes | | |
| DDS→PNG (BC7) | via Pillow's DDS decoder (check BC7 DX10 support on real files) | or the toolkit's `prospero-dds2png.exe` | |
| SELF magic/trailer repair | yes (format is simple; phase 2) | | |
| UCP digest check/repair | digest scheme still to be confirmed (detect only at first) | | `lpp_ucp_repair_digest_file` |
| **Packing** (PFS, NAPS, CNT, FIH, Oodle) | **no** | **yes** (`img_create`) | alternative |
| Delta update vs base | no | **yes** (`--ref_pkg_path`) | |
| "Base was built by LibProsperoPkg" probe | no (needs the library's NAPS key) | | yes |
| Header verification (FIH, signed byte, mode 13, marker) | yes | | |
| Deep verify (digests, integrity) | no | `img_verify` | `VerifyPackageQuick/Full` |
| Extract an existing PKG | no | `img_extract` | `ProsperoPackageArchive` |
| DLC (`prospero_ac`) | GP5 only | untested | reference path |
| Legacy plaintext conversion (RSA CNT wrap) | **do not implement** | | |

### 5.5 Things that cannot be done safely, and open questions

1. **Distributing or locating the patched toolkit.** We must never bundle it or link to it. The app should only accept a
   user-chosen path. It should say plainly that a stock Sony Publishing Tools install does not produce the
   PLAINTEXT_NOAUTH debug FPKG, and stop with an error (not a success) when the marker is missing.
2. **Re-implementing the legacy post-process** (RSA-3072 CNT header wrap, entry-digest resealing). This touches signing
   key material and is out of scope.
3. **The "is this base pkg Sony-built?" check without LibProsperoPkg.** We can only warn that the base must have been
   built by Publishing Tools. The SDK will otherwise fail late with "NAPS metadata missing".
4. **DLC packages.** The proven path is LibProsperoPkg (GPL). A `prospero_ac` GP5 through the Sony tool needs testing on
   hardware before we ship it. Entitlement-only PSAL packages do not install.
5. **LibProsperoPkg as an optional engine.** If we want it later, build it ourselves from the public GPL-3.0 source
   (drakmor or SvenGDK `LibProsperoPKG`) as the C-ABI DLL and load it with `ctypes`, or have the user supply it. Ship or
   point to the corresponding source, and credit SvenGDK and drakmor. Do not reuse the fpkg-gui DLL from the reference.
6. **Non-ASCII user paths** need NTFS junctions. Python has no public API for them; options are
   `subprocess.run(['cmd','/c','mklink','/J',alias,real])` or the private `_winapi.CreateJunction`. Create them only on
   NTFS and never on the source drive. If `%TEMP%` itself is non-ASCII (non-ASCII user name), fall back to
   `C:\ProgramData\<app>\aliases`, else refuse with a clear message.
7. **Progress-bar behaviour under a pipe** must be confirmed on a real Windows run (see §1.4). The file-size poll is the
   fallback.
8. **Real-console validation:** the reference's own PS5 results table is empty. Plan a small test matrix (an
   indie-sized game, a multi-language game, a game with DLC-emu, one update) before advertising the feature.
9. **The reference itself:** no licence, and it ships Sony binaries. Do not copy, link, vendor or contact it. Keep this
   research as behavioural notes only.

### Key reference files (for further reading only)

`src/PsViethoa.FpkgBuilder.Core/Services/`: `BuildEngine.cs`, `SonySdkBuilder.cs`, `SonySdkRunner.cs`,
`SonySdkToolchain.cs`, `SonySdkProject.cs`, `SonySdkPlayGo.cs`, `SonySdkPackageSize.cs`, `SonySdkPatchReference.cs`,
`SonySdkReferenceProbe.cs`, `SonySdkConverter.cs`, `SonySdkPathAliases.cs`, `SonySdkPrescan.cs`, `CpuLimiter.cs`,
`PackageVerifier.cs`, `PackageReader.cs`, `DlcPackageBuilder.cs`, `DlcEmuIni.cs`, `DlcEmuInspector.cs`,
`ComponentProbe.cs`, `PublishingToolsLocator.cs`, `DiskSpaceAdvisor.cs`, `JunkFileFinder.cs`, `PhaseCatalog.cs`,
`OutputConflict.cs`.
`ExFat/`: `ExFatImage.cs`, `ExFatExtractor.cs`, `UfsImage.cs`, `UfsExtractor.cs`, `PfsContainer.cs`, `ImageMounter.cs`,
`DokanImageMounter.cs`, `ExFatMounter.cs`.
`libs/sony-sdk/`: `README.md`, `build.bat`, `build-from-folder.ps1`, `build-gui.ps1`, `scripts/create-gp5-from-folder.py`.
`CHANGELOG.md` (2.1.0-3.0.0), `docs/HANDOFF-WINDOWS-2.2.0-test2.md`, `docs/research/NOTES.md`.
