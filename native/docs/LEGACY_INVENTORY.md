# Legacy inventory — engines and settings (exFAT Image Builder 4.1.0, Farewell Edition)

| | |
|---|---|
| Snapshot | branch checked out from `farewell-edition-dev`, HEAD `b298c2d` (2026-11-04). All `file:line` references are to this commit. Function names are the stable reference if lines drift. |
| Scope | `exfat_builder.py` (27,549 lines), `vendor/lazy_mkpfs/*`, `ui/mkpfs_runner.py`, `ui/experimental_mkpfs.py`, `ui/tab_{pfs,exfat,ffpkg,extract,convert,edit,ffpkg_edit,unibuild,images,dump_rename,settings,advanced}.py`, `ui/dump_rename.py`, `ui/shared/*`, `docs/farewell/{TECHNICAL_AUDIT,DEPENDENCIES,STATUS}.md`, `tests/*`. Some neighbours were read because the engines call them (`ui/tab_files.py`, `ui/build_ampr_index.py`, `ui/progress_state.py`, `ui/tq_model.py`). |
| Not in scope | FPKG (`core/fpkg/*`, FPKG job type) → see `native/docs/FPKG_PARITY.md`. Backport (`Backport.py` blobs, `core/backport_safety.py`, `ui/tab_backports.py`, `ui/panel_bp_auto.py`, Backport/SDK-fixer/Batch-SDK methods in `exfat_builder.py`) → see `native/docs/BACKPORT_PARITY.md`. UI layout and page structure → see `native/docs/LEGACY_UI_INVENTORY.md`. |
| Method | Static reading only. The embedded `make_image.bat` and `New-OsfExfatImage.ps1` were decoded from `_BAT_B64` / `_PS1_B64` and read in full. Every settings key was found with an AST scan of all non-vendored `.py` files (subscript reads/writes, `.get/.setdefault/.pop/.update` on `_settings` and its aliases, `_persist_setting`, dynamic `'prefix' + x` keys), then each key was checked by hand. The external `mkpfs==0.0.9` wheel (pinned in `requirements-build.txt`, **not vendored**) was downloaded and read to confirm CLI flags. Nothing was run on Windows. |
| Markers | **UNVERIFIED** = read from code but not observed at runtime, or depends on external tool behaviour. **NEW** = a finding not listed in `TECHNICAL_AUDIT.md`. |

---

## 0. Overview

The legacy app has **seven image engines**, all driven from Tk callbacks with worker threads:

| # | Engine | Visible entry point | Hidden/legacy entry points | External tool(s) |
|---|---|---|---|---|
| E1 | Folder → **.exfat** | Build tab, type *exFAT* (`ui/tab_unibuild.py`) | legacy exFAT tab (`ui/tab_exfat.py`, frame key `build`), Library *Build All* | `cmd.exe` → `make_image.bat` → PowerShell 5.1 → OSFMount + `format.com` + `robocopy` |
| E2 | Folder → **.ffpkg** (UFS2) | Build tab, type *ffpkg* | legacy ffpkg tab (`ui/tab_ffpkg.py`) | embedded UFS2Tool 4.0.0 (.NET 8+) `newfs` |
| E3 | Folder → intermediate → **.ffpfsc** | Build tab, type *PFS*, source *folder* | legacy pipeline mode (`pipeline_to_pfs`) | E1 or E2, then Lazy_MkPFS (vendored) or MkPFS 0.0.9 (pip) |
| E4 | Existing .exfat/.ffpkg → **.ffpfsc** | Build tab, type *PFS*, source *image* | legacy PFS tab *Convert* + convert queue (`ui/tab_pfs.py`), "Convert to .ffpfsc?" hand-off | Lazy_MkPFS or MkPFS |
| E5 | Folder → **.ffpfs** / .ffpfsc (two-step) | — | legacy PFS tab *Build* + queue (`ui/tab_pfs.py`) | MkPFS `pack folder` (+ `pack file`) |
| E6 | **Convert** .exfat ↔ .ffpkg | Convert tab (`ui/tab_convert.py`) | — | OSFMount, UFS2Tool `newfs`/`extract`/`mount_udf`, `format`, robocopy |
| E7 | **Extract** .exfat / .ffpkg / .ffpfs / .ffpfsc | Extract tab (`ui/tab_extract.py`) | legacy PFS tab *Extract*, legacy ffpkg extract pane | OSFMount + robocopy; UFS2Tool `mount_udf` (Dokany v2) + robocopy; MkPFS `unpack [--deep]` |
| E8 | **Edit** an image | Edit tab (`ui/tab_edit.py`) | Files tab (`ui/tab_files.py`) | OSFMount rw mount; UFS2Tool `ls/add/replace/delete/extract/chmod/newfs`; MkPFS `unpack` |

Shared services: game-root detection, metadata (param.sfo/param.json/pfs-version.dat/nptitle.dat), naming, AMPR index generation, free-space and filesystem pre-flight, OSFMount discovery and robust dismount, stale-mount sweep, tool extraction with integrity re-check, child-process kill on close, temp sweep, session logging, history, updater, shutdown-after-queue, notifications.

Counts (detailed in §4 and §6): **108 live persisted settings keys** (+3 migration-only keys), **33 non-persisted per-job options**, **173 migration-table features**.

---

## 1. Image pipelines

### 1.0 Shared building blocks

| Block | Location | Behaviour to preserve |
|---|---|---|
| Game-root resolution | `exfat_builder.py:349` `resolve_game_root_deep` | BFS from the picked folder for a directory containing `eboot.bin`. Limits: depth 5, 4,000 folders (`truncated`), cancel callback. Skips dot-dirs and `sce_sys, sce_module, fakelib, image, $recycle.bin, system volume information, __macosx`. Exactly one candidate → root; several → caller shows a chooser (`ui/tab_unibuild.py:164` `_choose_game_root`); none → use folder as-is. Also run inside the exFAT worker if `eboot.bin` is missing at build time (`exfat_builder.py:10932`). |
| Metadata | `parse_sfo` :176, `find_meta_file` :208 (root, `sce_sys`, then walk ≤5 deep, case-insensitive), `parse_param_json` :237, `parse_nptitle_dat` :267, `get_game_info` :442 | Priority: param.sfo (`TITLE`/`TITLE_00`, `TITLE_ID`, `VERSION`/`APP_VER`) → `pfs-version.dat` overrides version → param.json (`titleId`, `version`/`masterVersion`, `localizedParameters[defaultLanguage\|en-US\|en-GB].titleName`, top-level `titleName`/`name`) only if sfo gave neither title nor id → nptitle.dat first line for title. Version normalised to `NN.NNN.NNN` (2-part gets `.000`). Title sanitised. Title-id fallback: regex `(PPSA\|CUSA\|PPLH)\d{5}` on folder name (`ui/tab_unibuild.py:1571`). Required firmware: param.json `requiredSystemSoftwareVersion`/`systemVersion`/`SYSTEM_VER` (`exfat_builder.py:14723`). |
| Filename sanitiser | `sanitize_filename` :275 | Smart quotes → ASCII, dashes → `-`, `…` → `...`, bullets → space, ®©™℠ dropped, C0/C1 control chars dropped, strips `\ / : * ? " < > \| & ^ % ! `` ` `` $`, collapses whitespace, strips leading/trailing `. ` , **caps at 80 chars**, never empty (`'game'`). |
| Legacy name | `build_exfat_name` :509 | `"<TITLE_ID> <Title> (<version>).exfat"`, `game.exfat` if nothing known. |
| Unified name presets | `ui/tab_unibuild.py:1318` `_gen_name` | `ppsa` = ID; `ppsa_title` = ID + title; `ppsa_title_ver` (default) = ID + title + ` (ver)`; `ppsa_title_ver_fw` = … + ` (FW<n>)` (firmware auto from param.json or override `1..13`); `custom` template tokens `%TITLE% %PPSA% %VERSION% %FIRMWARE% %TYPE% %DATE%` (case-insensitive), then empty `()`/`[]`/`{}` removed (4 passes), whitespace collapsed, spaces inside brackets trimmed, `-_–—` stripped at ends. Fallback = folder name. Sanitised, then extension `.exfat/.ffpkg/.ffpfsc/.pkg`. |
| Output collision (unified) | `ui/tab_unibuild.py:2172-2190` | If target exists: *Overwrite* / *Save numbered copy* → `"<stem> (N)<ext>"` / *Don't add*. FPKG never overwrites (builder numbers it). Output folder inside the dump is refused (`:2130`). |
| AMPR index | `ui/tab_unibuild.py:2329` `_maybe_build_ampr_index` → `ui/build_ampr_index.py:206` `build_index_local` | Before every unified exFAT/ffpkg build (also the PFS intermediate): if `<dump>/fakelib/libSceAmpr.sprx` exists, (re)write `<dump>/ampr_emu.index` (format §2.5). **Writes into the user's source dump (NEW)**. Failure is logged and the build continues. Case-insensitive path collisions are kept (first wins) because `allow_case_collisions=True`. |
| Space estimate (Build tab) | `ui/tab_unibuild.py:1418` `_update_space` | Off-thread with generation counter. exFAT/ffpkg/FPKG: output needs `src × 1.12`. PFS from folder: output `src × 2.05`, temp `src × 1.0` (same drive as output) or `× 2.2`. PFS from image: output `src × 1.05` if source on the output drive else `× 2.05`. Shown as "~55–95 %" compressed estimate. Low space → confirm on Add; *Build All* re-checks per drive (sum of `need_out` + max `need_tmp`) (`:3309`). |
| Tool extraction | `extract_scripts` :529, `extract_ufs2tool` :852, `scripts_intact` :552, `ufs2tool_intact` :563 | Each extraction is a fresh `tempfile.mkdtemp` (`exfat_builder_*`, `exfat_builder_ufs2_*`) under `temp_dir` or `%TEMP%`. Scripts and UFS2Tool files are compared byte-for-byte with the embedded blobs before every use and re-extracted when changed or missing (audit M7). UFS2Tool gets a generated `UFS2Tool.runtimeconfig.json` (`tfm net8.0`, `rollForward: LatestMajor`, framework `Microsoft.NETCore.App 8.0.0`). |
| Process kill | `kill_child_processes` :2760 | psutil: all descendants of this PID → terminate → wait 5 s → kill. Used only on close. |
| Previous image set-aside | `set_aside_previous_image` :2789, `settle_previous_image` :2804 | Before an exFAT build: `name.exfat` → `name.previous.exfat` (`.previous2`, …). Success: delete the set-aside. Failure: failed new image → `name.failed.exfat`, set-aside restored (audit M2). |
| Final move | `_move_final` :24193 | Same path: verify exists. Same volume: `os.replace`. Cross-volume (WinError 17/EXDEV) or any rename error: 8 MiB chunked copy to `dst.part`, fsync, copystat, size check, `os.replace`, re-check size, **then** delete source. On any failure the source is left intact and a `RuntimeError` names where it is. |
| Disk/volume probes | `_volume_filesystem` :2278 (GetDriveTypeW + GetVolumeInformationW → NTFS/exFAT/FAT32/FAT/network/unknown), `_is_cloud_synced_path` :2331 (substring `onedrive, google drive, googledrive, dropbox, iclouddrive, icloud drive, creative cloud`), `_free_space` :2344, `ui/tab_unibuild.py:2693` hard-link probe (create + `os.link` in a temp dir) | Used by PFS pre-flight and staging decisions. |
| Drive letters | `ui/shared/drives.py` `used_letter_mask` | `GetLogicalDrives` + network mappings from `HKCU\Network` and every `HKEY_USERS\<sid>\Network` (elevated process does not see the user's mappings, issues #64/#42). Python mounts pick **Z→E**; the PS1 picks **D→Z** and also unions `Get-PSDrive`, `DriveInfo`, `GetLogicalDrives`, `HKCU:\Network`. |
| Stale-mount sweep | `_sweep_stale_osf_mounts` :9794, `osf_labelled_volumes` :2846 | Dismounts drives D–Z whose label is `OSFIMG`, `PS5DATA` or `PS5IMG`, except the system drive, any drive with `\Windows\System32`, `exclude_letter`, and any `(letter, serial)` that was already mounted when the app started (audit M5). Runs before every verify (excluding the active build letter) and on close. |
| Robust dismount | `_dismount_drive_robust` :9601 | Within `max_wait_seconds`: `osfmount -D -m X:` (force) → `-d -m X:` → `osfmount -l` → `-D -m <device#>:` → `mountvol X: /D` → `FSCTL_LOCK_VOLUME` (0x90018) + `FSCTL_DISMOUNT_VOLUME` (0x90020) via `DeviceIoControl`; ~2 s between steps. Returns whether the letter is gone. |
| Thread-safe UI calls | `exfat_builder.py:3288-3381` | `tk.Misc.after/after_idle/after_cancel` are monkey-patched (`_ts_after`) so worker threads queue calls to a 15 ms pump on the Tk thread (audit M3 partial fix). |

### 1.1 E1 — Folder → .exfat (OSFMount + embedded batch/PowerShell + robocopy)

**Entry and inputs.** Unified: `_dispatch_exfat` (`ui/tab_unibuild.py:2369`) creates a `QueueItem(src, outdir, name, size_override_gb=job.exfat_size or 'auto')`, appends it to the hidden legacy queue, sets `_unified_active`, and calls `ExFATBuilder._process_next([idx], 0)`. Legacy: `_add_to_queue` :6068 → `_run_queue` :11930 → `_process_next` :10856.

**Pre-build checklist (legacy `_run_queue` only, Tk thread).** OSFMount found; `eboot.bin` in each source; output not equal to/inside the source; output not a drive root; write test file `.write_test`; free ≥ `folder size × 1.05` (dump measured once per run, still on the UI thread — audit M4 residual); existing output → "will be overwritten". Then a time estimate (300 MB/s + 30 s per game) and confirm.

**Steps (`_process_next` worker):**

| # | Step | Exact command / rule | Where |
|---|---|---|---|
| 1 | Resolve game root if `eboot.bin` missing | `resolve_game_root_deep` | `exfat_builder.py:10932` |
| 2 | Image size override | per-item `'auto'` → `''` (auto); per-item number → that GB; **else** global `adv_img_size_override` | :10950-10962 |
| 3 | Set aside existing output | `set_aside_previous_image(out_path)` | :10968 |
| 4 | Reject `"` in any path, warn on `%` | bat/out/source/OSFMount paths | :10979 |
| 5 | Re-extract scripts if changed | `scripts_intact` / `extract_scripts(temp_dir)` | :10997 |
| 6 | Python pre-flight (worker) | `_preflight_source` :26978 — source exists; `eboot.bin`; walk counts files/dirs/bytes/longest path/largest file; abort > 500,000 files; abort 0 files; abort if output free < `bytes × 1.05 + 2 GiB`; warn path ≥ 248 chars | :11091 |
| 7 | Run batch | `cmd.exe /d /s /c ""<bat>" "<out>" "<src>" "<osf>" "<cluster>" "<sector>" "<threads>" "<retries>" "<wait>" "<exclHidden 0\|1>" "<imgGB>""`, `cwd=<bat dir>`, env `EXFAT_SCRIPT_DIR=<bat dir>`, `EXFAT_IMGSIZEGB=<GB>` if set, stdout+stderr merged, decoded **`oem`** on Windows (audit M10), `CREATE_NO_WINDOW` | :10984-11140 |
| 8 | Batch `make_image.bat` v5 | resolves PS1 from `EXFAT_SCRIPT_DIR` or `%~dp0`; checks PS1 (exit 2), source dir (3), `eboot.bin` (4, lists folder); builds `-ImagePath -SourceDir -ForceOverwrite [-OSFMountPath] [-ClusterSizeOverride] [-SectorSize] [-CopyThreads] [-Retries] [-RetryWait] [-ExcludeHidden] [-ImageSizeGBOverride]`; runs `%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe -NoProfile -ExecutionPolicy Bypass -File <ps1>` (bare `powershell.exe` only if the full path is missing); PS exit ≠ 0 → `del /f /q <image>`, exit rc; exit 0 but no image → 99 | decoded `_BAT_B64` |
| 9 | PS1 `New-OsfExfatImage.ps1` v2 | see table below | decoded `_PS1_B64` |
| 10 | Post-process dismount | stop drive poll, sleep 0.5 s, `_dismount_drive_robust(letter, 30)` | :11180 |
| 11 | Result | `_item_done(rc, …)` on the Tk thread; exception → `_item_error` | :10419, :10614 |

**PS1 sequence** (`$ErrorActionPreference='Stop'`, en-US culture forced):

| Phase | Behaviour |
|---|---|
| Guards | `Test-Admin` (throw if not elevated); source dir; `eboot.bin`; create output folder; existing image removed (`-ForceOverwrite` always passed — the Python set-aside already moved it). |
| Enumeration | `Get-SourceStats`: streaming `Directory.EnumerateDirectories/EnumerateFiles` (AllDirectories). Skips `*.exfat`, `*.ffpkg`, and hidden files when `-ExcludeHidden`. Collects FileCount, DirCount, RawBytes, DataBytes (cluster-aligned), HasLargeFile (any file ≥ 1 GiB), LongestPath. Progress line every 10,000 files: `[INFO] Enumeration progress: N files / X GB`. |
| Cluster size | Override if `-ClusterSizeOverride > 0`; else enumerate at 32 KiB, choose **64 KiB if average file size ≥ 1 MiB, else 32 KiB** (0 files → 32 KiB), and re-enumerate if 64 KiB. |
| Image size | `-ImageSizeGBOverride > 0` → `GB × 1 GiB`. Else `Get-OptimalImageSizeBytesFromStats`: `dataClusters = ceil(DataBytes/cluster)`; `fat = dataClusters × 4`; `bitmap = ceil(dataClusters/8)`; `entries = (files + dirs) × 512`; `base = DataBytes + fat + bitmap + entries + 64 MiB`; `spare = ceil(base/100)` clamped to **[1 GiB, 8 GiB]**; `total = max(base + spare, RawBytes + 1 GiB)`; rounded up to 1 MiB. |
| Free-space pre-flight | Output volume free ≥ size + 256 MiB (skipped for UNC/unknown). |
| Threads / media | App always passes `-CopyThreads` (≥1), so media detection is skipped. (If 0: `Get-SourceMediaKind` in a job with 8 s timeout → HDD 8, network 8, SSD 32, unknown 16.) UNC source → `mediaKind='network'`. |
| Unbuffered I/O | `/J` when any file ≥ 1 GiB **or** total ≥ 16 GiB. |
| Locate OSFMount | `-OSFMountPath` if it exists, else `Get-Command osfmount.com/OSFMount.exe`, then Program Files (x86)/PassMark candidates; prefer sibling `osfmount.com` over `OSFMount.exe`. |
| [1/4] Attach | `osfmount -a -t file -f <img> -s <bytes> -m <X:> -o rw,rem` via hidden `ProcessStartInfo`; timeout `900 + 15 s × ceil(DataBytes/GiB)`; error text mapped to hints (space / access denied / driver / in use); wait ≤ 20 s for the letter (`DriveInfo`, not WMI). **Sector size is never passed** (comment: OSFMount has no sector option). |
| [2/4] Format | `format.com X: /FS:exFAT /A:<32K\|64K\|…> /Q /V:OSFIMG /X /Y` (one attempt despite "retry strategies"); success = `DriveInfo.DriveFormat == exFAT` within 5 s. **`-SectorSize` is not passed to format.com either (NEW: the sector setting has no effect).** |
| [3/4] Copy | `robocopy <src> X:\ /E /COPY:DAT /DCOPY:DAT /R:<n> /W:<n>` + (`/NDL /NC` if ≤ 50,000 files, prints `[ROBO-MODE] perfile`; else `/NFL /NDL`, `[ROBO-MODE] summary`) + `/ETA /XF *.exfat /XF *.ffpkg /MT:<n>` + `[/J]` + `[/COMPRESS]` (network only) + `[/XA:H]`. Exit ≥ 8 with `/J` → retry once without `/J` (issue #62). Exit ≥ 16 → throw. Exit 8–15 → `[WARN]` only (audit H1: the Python strict compare is what catches missing files). |
| Copy check | `Get-TreeFileStats X:\` (skips `System Volume Information`, `$RECYCLE.BIN`; throws if the root cannot be listed) → `[COPY-STATS] dest_files= dest_bytes= source_files= source_bytes= robocopy_exit=`. Then volume used bytes must be ≥ 90 % of source RawBytes, else throw "Copy verification FAILED". |
| Flush | P/Invoke `CreateFileW(\\.\X:)` + `FlushFileBuffers` (warn on failure). |
| [4/4] Dismount | `finally`: leave cwd if on the volume; `osfmount -d -m X:` (and `X:\`) up to 6 attempts, 120 s each, 0.5 s apart. |

**Output parsing (`_handle_line` :10202).** Drops bare `NN%` lines; keeps an 80-line tail for error dialogs; `[ROBO-MODE]`; per-file lines (`\t` separated) → current file + counter; `[COPY-STATS]` → structured counts (`parse_copy_stats_line` :2523); localized robocopy summary → `RobocopySummaryParser` :2533 (second six-integer row after a dashed separator = file row; present = total − failed); `logical volume on X:` → mounted letter, start drive-poll free-space telemetry at `[3/4]`; step markers `[1/4]..[4/4]` map to `STEPS` (0–20–50–90–100 %) :2270.

**Empty-output guard (`_build_output_is_empty` :10348, runs even with verification off).** Rejects only on structured evidence: `[COPY-STATS]` with source_files > 0 and dest files = 0 and dest bytes = 0; or a known robocopy count of 0; or `_verify_image_file_only` failing (size ≥ 64 MiB and `EXFAT   ` at offset 3). Error-87 text alone never rejects. A rejected image is **not deleted** (issues #72–#80).

**Verification** (§1.8 details): skipped if `adv_skip_verify`; else worker → `_verify_image` :23561 (native MkPFS verify when `pfs_backend == 'mkpfs'`, else/then OSFMount mount walk) → `_verify_done` :23930 (suspiciously small: source > 10 MiB and image < 5 % of source → fail) → `_verify_done_final` :24072.

**Completion (`_verify_done_final`).** Writes history (`success = verdict`), status, exports log (`_FAILED` suffix on failure), support nudge (legacy single queue only), **delete source only if content-verified** (§3.4), notification (sound + PowerShell balloon + Discord), optional SHA-256 of the whole image (logged only), then legacy-only follow-ups: pipeline to .ffpfsc (`pipeline_to_pfs`), auto FTP upload (`adv_auto_upload`), FTP prompt (`ftp_auto`). Verification failure → queue dot `warn`, log, modal only for a single-item run.

**Failure (`_item_done` rc ≠ 0).** Restore previous image (`settle_previous_image(..., False)`), history `success=False`, then **auto-retry** `retry_count` times after 2 s (also for unified exFAT jobs), else unified → continue with next; legacy → dialog with up to 6 `[ERROR]`/throw lines from the tail and "Continue with remaining items?".

**Cancellation.** None during the copy. Unified *Cancel* only stops later jobs; a running exFAT build completes and is **kept** (audit H4 fix). On window close: confirmation, then `kill_child_processes()` before dismount/sweep (audit H3 fix).

**Temp files.** Script folder `exfat_builder_<rand>` (bat, ps1, icon); PS1 `GetTempFileName()` pairs for format stdout/stderr (deleted); `.write_test` in output dir; summary `<stem>.summary.json` beside the image (`_write_build_summary` :12137: schema 1, app version, timestamps, duration, game, output path/size, SHA-256 of first+last 1 MiB, settings used).

**Naming.** Unified: §1.0. Legacy: `build_exfat_name`. Volume label always `OSFIMG`.

### 1.2 E2 — Folder → .ffpkg (UFS2Tool)

| Item | Behaviour | Where |
|---|---|---|
| Entry | Unified `_dispatch_ffpkg` (`ui/tab_unibuild.py:2399`) → appends `{'folder','odir','name','status':'waiting'}` to `_ffpkg_queue` → `_ffpkg_process_next([idx],0)`. Legacy `_ffpkg_add_to_queue` :7947 → `_ffpkg_run_queue` :8560. | |
| Pre-checks | UFS2Tool extracted and intact; nothing else (no free-space pre-flight in the engine; Build tab estimate only). | :8560 |
| Command | `UFS2Tool.exe newfs -O 2 -b <adv_ffpkg_block> -f <adv_ffpkg_frag, clamped ≤ block> -m <adv_ffpkg_minfree> -S 512 -i <adv_ffpkg_inode> -D <folder> <out>.part`, `cwd=<exe dir>`, merged stdout decoded **UTF-8** (UNVERIFIED that UFS2Tool writes UTF-8; audit M10 suspected OEM). Stale `.part` removed first. | `exfat_builder.py:8417-8433` |
| Sizing | UFS2Tool sizes the image itself from `-D` (no app-side size). | |
| Progress | Lines: `Writing cylinder groups` (stage 0–30 %), `Adding files to image` (30–95 %, starts size poll), `Populated image with` (done). | `_ffpkg_handle_line` :8155 |
| Finalize | `finalize_ffpkg_part` :2646: rc 0 + `.part` exists and > 0 → `os.replace(.part, out)`; rc 0 + no output → −2; rename failure → −3; rc ≠ 0 → delete `.part`. An older image with the same name survives until success (audit H6). | |
| Verification | **None** beyond rc and the `.part` size. | |
| Delete source | Only if `adv_delete_source_ffpkg` and `ffpkg_safe_to_delete_source` :2675 (source has `eboot.bin`, image size ≥ sum of all source file sizes, every file readable). | :8273 |
| Errors | rc ≠ 0 → `dotnet_runtime_hint` :3012 maps apphost codes `0x80008081/83/96` and text (`hostfxr`, "you must install .net") to an install hint. Unified → continue; legacy → "Continue with remaining items?". | :8304 |
| Retry | None. | |
| Cancellation | None (finishes, kept). Close kills the process tree. | |
| End of queue | `summarize_outcomes` :2701 counts real statuses (audit H5); shutdown trigger `ffpkg`; legacy single image → "Convert to .ffpfsc?" hand-off. | :8350 |

### 1.3 E3 — Folder → intermediate → .ffpfsc (unified PFS job)

`_dispatch_pfs` (`ui/tab_unibuild.py:2552`):

1. Intermediate format `via` = `exfat` (default) or `ffpkg` (job snapshot of `pfs_intermediate_via`).
2. Intermediate name = sanitised **PPSA/title id** if detected, else final stem; `+ .exfat/.ffpkg`.
3. Intermediate folder `_choose_intermediate_dir` :2431 (measures the dump **on the Tk thread**, audit M4 residual): use `temp_dir` only if set, exists, on a **different drive letter** than output, free ≥ `est × 1.3`, and (for `ffpkg`) supports hard links; otherwise output folder; abort the job if output free < `est × 1.05`.
4. Build the intermediate through E1 or E2 (same code, same verification, same delete-source rules). `ok == 'warn'` → stop, keep intermediate, job `warn` (issue #86). Missing intermediate → fail.
5. Convert with `ExFATBuilder._pipeline_convert_then_next(inter, cb, explicit_output_path=<outdir>/<stem>.ffpfsc, allow_experimental=True, backend=job.backend, zlib_backend=job.zlib_backend, cancel_check=lambda: not running, notify_on_cancel=True)` (`exfat_builder.py:24375`), `delete_source_on_success=True` → the intermediate is deleted after a good convert.
6. Result from `pipeline_convert_result(final)`; a leftover intermediate that could not be deleted does not fail the job (issue-audit H1). Records `out_path`, `space_saved`, `compression %`.

**`_pipeline_convert_then_next` engine** (worker thread):

| Branch | Behaviour |
|---|---|
| Lazy (default) | Taken when backend `lazy` and `allow_experimental` and source `.exfat/.ffpkg` and an explicit output. Level = `pfs_compression_level` clamped 1–9 (default 9); CPU = `pfs_lazy_cpu_count` (no UI, always 0 → Lazy auto = `cpu_count − 1`, although the log says "all cores" — NEW); zlib backend `zlib` or `isa-l`. Existing output deleted. `run_lazy_mkpfs` (§1.3.1). `RESULT_OK` → result True, delete intermediate, **return with no further check** (no mkpfs verify, no "suspiciously small" check — NEW). `RESULT_CANCELLED` → no fallback, `on_continue` still called (`notify_on_cancel`). `RESULT_FALLBACK` or exception → stock MkPFS. |
| Stock MkPFS | `run_mkpfs(['pack','file','--version','PS5','--inode-bits','32','--cpu-count',<cpu>,'--compression-level',<level>, ['--temp-folder',<temp_dir>], <src>, <pack_out>])`. `level,cpu = _pfs_pack_tuning(settings)` :2471: `ceil = max(1, min(4, cores−1))`, `frac = 0.5 + 0.5·(level−1)/8`, `cap = max(2, round(ceil·frac))` if ceil ≥ 2 else 1, `cpu = min(ceil, cap)` (memory cap for mkpfs 0.0.9's unbounded imap backlog). |
| Temp redirect | If `temp_dir` exists, is on another drive than the output, and has free ≥ `src × 2.2`: write the `.ffpfsc` to `temp_dir/<name>` and later `_move_final` it beside the output. |
| Pre-flight | `_pfs_preflight_check` :2360 on the output dir **and** `dirname(pack_out)` (mkpfs writes `pack_out + '.tmp'`): FAT32 → **refuse**; free < `est × 1.5` (est = source size) → **refuse**; exFAT/FAT/network/cloud-synced with source or output ≥ 4 GiB → warn dialog, continue. A guard exception continues (fail-open, audit L5). |
| Progress | `[###---] NN% phase @ speed ETA t` parsed by `ui/mkpfs_runner._PROGRESS_RE` (throttled 4 Hz); size poller every 2 s when silent > 3 s. Lazy: fragments parsed by `ui/progress_state.parse_backend_line`; 4 s silence → indeterminate. |
| Retry | rc ≠ 0 and cpu > 1 and not cancelled → delete partials, rerun with `--cpu-count 1`. |
| Cancel | `cancel_check` true → partial `pack_out` and final removed, intermediate kept, no fallback. |
| Success test | rc == 0 **and** output mtime ≥ start − 5 s **and not** (source > 2 GiB and output < 1 % of source → rc forced to 1, source kept). Then delete source image (pipeline intermediates only) or log "source kept" (`delete_source_on_success=False`). |
| Failure | `errno 22`/"invalid argument" seen → "Disk rejected a large write … use local NTFS" dialog; else "convert failed — image kept". `finally`: result defaults to False; `on_continue` scheduled unless cancelled-without-notify. |

#### 1.3.1 Lazy_MkPFS runner (`ui/experimental_mkpfs.py`)

| Item | Behaviour |
|---|---|
| Source resolution | `_resolve_lazy_source` :113: `experimental_mkpfs_folder` if it contains `main_file.py` (**runs user-supplied Python inside the elevated app**), else bundled (`sys._MEIPASS/vendor/lazy_mkpfs`, `_MEIPASS/lazy_mkpfs`, `<exe>/vendor/lazy_mkpfs`, `<exe>/tools/lazy_mkpfs`), else dev tree `vendor/lazy_mkpfs`; missing → fallback. |
| Command | Frozen: `<exe> --__lazy_worker__ file <lazy_dir> <src> <out> [--zlib-isa] --zlib-level <L> [--cpu-count N] -v`; dev: `python <lazy_dir>/main_file.py …`. ISA-L clamps level to 0–3 (default 3). `cwd=<lazy_dir>`, env `PYTHONUNBUFFERED=1`, `PYTHONIOENCODING=utf-8:replace`, stdout decoded UTF-8 (issues #85/#69), read in 1 KiB chunks on a reader thread so `\r` progress arrives live. |
| Worker shim | `exfat_builder.py:27500` → `lazy_worker_main` :494: puts `lazy_dir` on `sys.path`, imports `lazy_mkpfs.pack_file`, execs `main_file.py` with patched `sys.argv`. Exit codes 3–7 for shim failures. |
| Result | Cancel → terminate (5 s) → kill, remove output only if it did not exist before → `RESULT_CANCELLED`. rc ≠ 0, no output, output < 1 MiB or not larger than a pre-existing file → `RESULT_FALLBACK` (deletes what it created). Else `RESULT_OK`. |
| Known traps (NEW) | Lazy `pack_file` truncates the output **file name to 60 characters** (`sanitize_output_path`, `vendor/lazy_mkpfs/lazy_mkpfs/pack_file.py:6`); with a longer name the app's existence check fails, it silently falls back to stock MkPFS, and the truncated Lazy file is left behind (UNVERIFIED at runtime). Lazy writes to RAM (`SpooledTemporaryFile`, up to 80 % of free RAM; spills to system temp) or `<out>.tmp`; the app's `temp_dir` is logged but not used. |

#### 1.3.2 Stock MkPFS runner (`ui/mkpfs_runner.py`)

| Item | Behaviour |
|---|---|
| Availability | `mkpfs_version()` :123 (`__version__` → `importlib.metadata` → `'(installed)'`); `mkpfs_supports_deep_unpack()` :174 = `mkpfs.exfat.open_exfat` exists. |
| Execution | `run_mkpfs` :473 holds a module lock (one mkpfs at a time). Primary: child process `[sys.executable, '--__mkpfs_worker__', *argv]` (frozen) or `python -m mkpfs` (dev), `CREATE_NO_WINDOW`, `PYTHONIOENCODING=utf-8:replace`, stdin null, unbuffered, stdout+stderr merged, 4 KiB reader thread, `cancel_check` polled every 0.25 s → terminate, 5 s, kill. Fallback if the process cannot start: in-process `mkpfs.__main__.main` with `--cpu-count 1` (skipped if cancel pending). |
| Output capture | `_LineCapture` :230: `\r` = progress overwrite, `\n` = log line; progress callbacks throttled to 4 Hz, 0 % and 100 % always delivered, duplicate percentages dropped. |
| Verify verdict | `classify_verify_result` :81: rc 0 → PASS; rc 2 or output containing `usage:`, `unrecognized arguments`, `invalid choice`, `no such file`, `errno 2`, `permission denied`, `errno 13` → TOOL_ERROR; else FAIL. |
| Exit codes | MkPFS 0.0.9 returns 1 when its default post-pack structure check (or `--verify`) fails **after writing the file** — every caller must require rc == 0 (audit C1). |

### 1.4 E4 — Existing .exfat/.ffpkg → .ffpfsc

**Unified (`_dispatch_pfs_from_image`, `ui/tab_unibuild.py:2766`).** Output `<outdir>/<stem>.ffpfsc`. ASCII source path → pass the original straight to the convert engine with `delete_source_on_success=False` (the original is never touched). Non-ASCII path → hard link `<srcdir>/.pfs_stage/<ascii stem><ext>` if the source volume supports links, else copy (8 MiB chunks, progress, fsync, cancellable) into `mkdtemp('.pfs_stage_')` under `temp_dir` → output dir → system temp, the first with free > source + 256 MiB (last resort: the source folder). After convert: rejected output (`result False`) deleted; good output `_move_final`'d to the final path; stage dir removed unless the output landed inside it.

**Legacy PFS tab Convert (`ui/tab_pfs.py:1448` `_run_convert`, queue `_cq_*` :1912-2077).** Inputs: `.exfat/.ffpkg`, output folder/name (forced `.ffpfsc`), optional temp, CPU cores, **Verify** checkbox. Work dir `<temp or outdir>/.pfs_convert_work` (wiped first, removed after). Staging: source volume can hard-link → `--temp-folder <srcdir>/.mkpfs_tmp` (**created next to the user's image**); else copy the image to a hard-link-capable drive with room (`work_dir` or `mkdtemp('pfs_stage_')`) with its own `.mkpfs_tmp`; else let mkpfs try symlink staging. Non-ASCII basename → link/copy to `mkdtemp('pfs_name_')/<ascii>`. Command: `pack file --version PS5 --inode-bits 32 --cpu-count <min(user, safe cap)> --compression-level <level> [--verify] [--temp-folder …] <src> <out>`. Success **only** if rc == 0 and the output is fresh (audit C1 fix); then optional delete source (`pfs_delete_source_after_convert`, rc 0 only); rc 1 → "wrote the image but its check found errors; output removed, source kept". Output deleted on any failure. Overwrite prompt (silent in queue mode). Shutdown trigger `pfs` when not in queue.

**Hand-off.** After a single legacy exFAT/ffpkg build, "Convert to .ffpfsc?" switches to the hidden PFS tab and pre-fills Convert (`_pfs_open_convert_with`, `ui/tab_pfs.py:1325`).

### 1.5 E5 — Legacy PFS tab: folder → .ffpfs / two-step .ffpfsc (`ui/tab_pfs.py:809` `_build_job`)

| Step | Behaviour |
|---|---|
| Validation `_validate_job` :699 | Source folder; game root = folder or one level down containing `sce_sys/param.json` (confirm if none); refuse if the top level of the root contains `.ffpfs/.ffpfsc/.exfat/.ffpkg` (mkpfs `pack folder` has no exclude); refuse output inside the source. Duplicate queue output → confirm. |
| Empty dirs | `_seed_empty_dir_markers` :158 writes zero-byte **`.pfskeep` into every empty directory of the user's dump** (mkpfs drops empty dirs; UE games need them); removed in `finally`. NEW: modifies the source during the build. |
| Step 1 | `pack folder --no-compress --no-adjust-output-file-extension --version PS5 --inode-bits 32 --cpu-count <n> [--temp-folder <temp>/.mkpfs_tmp] <root> <target>`; target = final `.ffpfs`, or `<outdir>/pfs_image.dat` for `.ffpfsc` (ShadowMount+ only recognises a nested file named exactly `pfs_image.dat`). rc ≠ 0 or no target → fail. |
| Step 2 (.ffpfsc) | `pack file --version PS5 --inode-bits 32 --cpu-count <min(n, safe cap)> --compression-level <level> [--temp-folder …] pfs_image.dat <out>`; `--temp-folder` dropped if it cannot hard-link. Success rc == 0 and output exists. |
| Cleanup | Nested `pfs_image.dat` and `.mkpfs_tmp` removed; failed output removed. `no space left`/`errno 28` → "Out of disk space". |
| Ignored option (NEW) | `version_ps5` checkbox is stored in the job but `--version PS5` is hard-coded. |
| Queue | `queued/running/done/failed`, pause flag, per-queue "shut down when finished" checkbox that temporarily forces `shutdown_on_pfs` (:1191). |

### 1.6 Unified build tab (orchestration of E1–E4 and FPKG)

See §3.1 for the job model. Dispatch `_uq_dispatch` (`ui/tab_unibuild.py:3009`) records `_final_preexists = (path, size, mtime)` and routes by `type`. Failure cleanup `_uq_cleanup_job` :3045: intermediate removed **only on cancel**, kept otherwise; exFAT/ffpkg outputs kept on failure ("builders delete their own partials"); for other types the final output is removed only if this run wrote it (size/mtime differ from the pre-existing file); FPKG keeps finished packages.

### 1.7 E6 — Convert between formats (`ui/tab_convert.py`)

| Direction | Steps | Gaps vs. Build (keep or fix deliberately) |
|---|---|---|
| exFAT → ffpkg `_do_exfat_to_ffpkg` :469 | Confirm overwrite (existing output removed). Mount source `osfmount -a -t file -f <src> -m <X:> -o rw` (90 s timeout, letter Z→E, defensive dismount first). `UFS2Tool newfs -O 2 -b 32768 -f 4096 -S 512 -D X:\ <out>` streaming progress (`init` 0–25 %, `files` 25–100 %, parses `NN%`, `(n / N files)`, `x GiB / y GiB`). Robust dismount in `finally`. "Convert complete". | **NEW:** source exFAT mounted **read-write**; newfs block/frag hard-coded (Advanced ffpkg settings ignored; no `-m`, `-i`); written straight to the final name (no `.part`), partial output **not removed** on newfs failure; no verification; .NET hint on failure. |
| ffpkg → exFAT `_do_ffpkg_to_exfat` :847 | Work dir `tempfile.mkdtemp('ffpkg_to_exfat_')` in **system temp** (not `temp_dir`, not swept at startup — NEW). Extract `UFS2Tool extract <src> <work>/dump` (retry with trailing `/`); on failure or > 2 GiB source → Dokan mount-copy engine (§1.9). Size = `extracted × 1.10` rounded up to 64 MiB (min 64 MiB), sparse-allocated by seek+write. Mount `-o rw`; `cmd.exe /c format X: /FS:exFAT /Q /Y /V:` with newline-fed stdin (3 min timeout); `robocopy <dump> X:\ /E /COPY:DAT /DCOPY:DAT /R:1 /W:1 /NP /ETA` decoded OEM, rc ≥ 8 fail. Dismount; work dir removed; partial output removed on exception. | Audit M8: no cluster size, empty label (sweep cannot recognise it), no `/X`, no flush, no verification. |

### 1.8 Verification methods

| Method | Used by | Rule |
|---|---|---|
| File-level sanity `_verify_image_file_only` `exfat_builder.py:23352` | exFAT empty-output guard; mount-failure fallback | exists, > 0, ≥ 64 MiB, bytes 3..10 == `EXFAT   `. A pass here is **inconclusive**, never "content verified". |
| Native MkPFS `_verify_image_mkpfs_native` :23408 | exFAT verify when `pfs_backend == 'mkpfs'` and `mkpfs.exfat.open_exfat` exists | `mkpfs verify <img> [--source-dir <src>]`. `--source-dir` omitted when `source_has_build_exclusions` (`decrypted` dir, nested image, hidden file with exclude-hidden) (audit H2). rc 0 with source → PASS + content-verified; rc 0 without source → structure only, continue to mount walk; TOOL_ERROR → mount walk; FAIL → fail, no fallback. |
| OSFMount mount walk `_verify_image` :23561 | exFAT (default) | Sweep stale mounts (exclude active letter); 4 attempts on fresh letters Z→E, each with a defensive dismount and `osfmount -a -t file -f <img> -m X: -o ro,rem` (90 s); all fail → file-level check → inconclusive. Wait ≤ 90 s for the drive (issue #86); timeout → inconclusive. `verify_walk_tree` :2580 on both sides (skips `decrypted` dirs, `.exfat .ffpkg .ffpfs .ffpfsc`, hidden files when excluded; relative paths lower-cased). `eboot.bin` must exist on the image. **`image_matches_source` :2631: every source file present at ≥ source size** (audit H1) → else fail naming up to 15 files largest first (`describe_missing_files` :2864). Then content-verified; byte/count tolerances only change the message. |
| Corrupt-size check `_verify_done` :23930 | exFAT | source > 10 MiB and image < 5 % of source → fail. |
| Retry verify | exFAT | Branch keyed on `'file count'` in a failure message never fires (audit L3, dead code; it still uses the old 0.5 % tolerance). |
| Optional SHA-256 | exFAT, `adv_checksum_verify` | Whole-image hash logged; not compared with anything. |
| ffpkg | E2 | rc 0 + non-empty `.part` only. Delete-source uses the size rule. |
| mkpfs post-pack | E3/E4/E5 stock | mkpfs default structure check (rc 1 on failure); `--verify` (full payload) only from the PFS tab Convert checkbox. |
| Lazy | E3/E4 Lazy | Lazy's own `validate_image_quick` (header magic/version/block size/readonly/mode/inode table, no write bits, readonly flags) + app's ≥ 1 MiB size check. No payload check. |
| Convert tab | E6 | None. |
| Extract | E7 | robocopy rc < 8 (exFAT, ffpkg); mkpfs rc 0 (PFS). |

### 1.9 E7 — Extract (`ui/tab_extract.py`, routed by extension `_route_extract` :1046)

| Format | Engine | Behaviour |
|---|---|---|
| `.exfat` | `ExFATBuilder._run_extract` `exfat_builder.py:11662` | Dest = `<outdir>/<name>` (merge confirm). OSFMount required. Mount `-o ro,rem` on Z→E (180 s timeout), wait ≤ 90 s. Walk to total bytes. `robocopy X:\ <dest> /E /COPY:DAT /DCOPY:DA /R:2 /W:2 /NP [/MT:<exfat_extract_mt_threads>]` (MT only if `exfat_extract_robocopy_mt`), output decoded **UTF-8** (NEW: M10 residual; build/convert use OEM). rc ≥ 8 → "incomplete" failure. Progress = destination folder size every 2 s. Dismount `osfmount -d` (120 s) → robust fallback. No cancel. |
| `.ffpkg` | `_run_ffpkg_extract_tab` :1178 → `ExFATBuilder._ffpkg_mount_copy_extract` :5607 | UFS2Tool `mount_udf -o ro <img> <X:>` (needs Dokany v2, `_find_dokan_driver` :5319), wait ≤ 15 s; walk; **refuse files ≥ 4 GiB** (bundled Dokan mount cannot serve them); free ≥ total × 1.03; `robocopy X:\ <dest> /E /COPY:DAT /DCOPY:DAT /R:1 /W:1 /NP` (OEM); cooperative cancel (`_extract_cancel`); always `umount_udf` + terminate in `finally`. `UFS2Tool extract` is avoided (reads each file into RAM, ~2 GiB cap). |
| `.ffpfs` / `.ffpfsc` | `_run_pfs_extract_tab` :1286 | `mkpfs unpack <img> <dest> [--overwrite] [--deep]`; `--deep` only for `.ffpfsc` with backend `mkpfs` and deep capability, preceded by `mkpfs tree --deep <img>` entry count; deep failure → plain unpack. A `.ffpfsc` that yields a nested `.exfat/.ffpkg/pfs_image.dat` tells the user to extract that next (no auto-chaining). Disk-full detection (`no space left`/`errno 28`). No cancel. |
| Legacy PFS tab Extract | `ui/tab_pfs.py:2121` | `mkpfs unpack <src> <outdir>/<stem> [--overwrite]`. |

### 1.10 E8 — Edit an image (`ui/tab_edit.py`)

| Format | Mode | Operations and safety |
|---|---|---|
| `.exfat` | Mounted edit (`ui/tab_files.py` + `_fm_*` `exfat_builder.py:8872-9347`) | `osfmount -a -t file -f <img> -m X: -o rw,rem` (120 s); browse/add files/add folder/new folder/replace/delete/rename/extract selection; `image_space_shortfall` :2969 refuses copies that do not fit (needed − replaced + 4 MiB slack > free, issue #33); overwrite dialog (skip / skip all / overwrite all); dismount with retry. *Apply backport* overlays a folder and writes `<image stem>.backup-<ts>.zip` of replaced files next to the image (`ui/tab_files.py:697`). |
| `.ffpkg` | In-place UFS2 edit (`ui/tab_ffpkg_edit.py`) | `UFS2Tool ls/add/replace/delete/rename(?)/extract/chmod -R` per action (`_run_ufs2` :793, 120 s default timeout, mbcs decode); mkdir via a `.keep` placeholder; hex preview via single-file extract. *Apply backport* rebuild: `extract` → overlay → `newfs` with Advanced ffpkg params → `_bp_atomic_swap` :3376 (same volume `os.replace`, else `.new` sidecar). Compare two ffpkgs (`_compare_ffpkgs` :3433). |
| `.ffpfs/.ffpfsc` | Workspace (`_build_pfs_workspace` :187) | `mkpfs unpack <img> <workspace> [--overwrite]`; nested `.exfat/.ffpkg` handed to the editors above; rebuild is routed to the Build tab (never repacks in place, never overwrites the source). |

### 1.11 Audit fixes that the native app must preserve (requirements)

| ID | Requirement for the native app | Legacy implementation (keep as reference) | Legacy test |
|---|---|---|---|
| C1 | A PFS pack/convert succeeds **only** on exit code 0 (or the native engine's explicit pass) **and** a freshly written output. Any other result deletes the output and keeps the source. | `ui/tab_pfs.py:1780` `rc == 0 and file_ok`; `_pipeline_convert_then_next` rc 0 | `tests/test_data_safety.py` `MkpfsExitCodes` |
| C2 | "Delete source" runs only after a **content** comparison passed for that exact output (tri-state PASS/FAIL/INCONCLUSIVE; skipped/inconclusive never deletes); deletion runs off the UI thread and logs why it was skipped. ffpkg: until a real verifier exists, at least the size rule. | `_mark_content_verified` / `_take_content_verified` :23551; `_verify_done_final` :24072; `ffpkg_safe_to_delete_source` :2675; `_rmtree_in_background` :23539 | `FfpkgDeleteSource`, `BackgroundDelete` |
| H1 | No tolerance for missing data: every copied source file must exist on the image at full size; robocopy exit ≥ 8 must at least be a warning in the result. | `image_matches_source` :2631, `describe_missing_files`. Partial in legacy: `robocopy_exit` is parsed from `[COPY-STATS]` (:2529) but never used, so with verification skipped an exit 8–15 build still ends as "Built". | `StrictImageComparison`, `VerificationHelpers` |
| H2 | A native/whole-folder verify must apply the same exclusions as the copy (or not compare against the raw folder). | `source_has_build_exclusions` :2615 | `SourceExclusions` |
| H3 | Every child process (and its tree) is tracked and stopped on close/cancel **before** dismounting or sweeping. | `kill_child_processes` in `_on_close` :4061 | `ChildProcesses` |
| H4 | Cancelling never deletes a finished output; only unfinished work and run-created temp/intermediates are removed; the cancel dialog must say exactly what happens. | `_uq_done` / `_uq_cleanup_job` | — (GUI) |
| H5 | End-of-queue summaries count real outcomes (done / warn / failed / cancelled), and auto-shutdown runs only after background work ends. | `summarize_outcomes` :2701; `_uq_finish` | `QueueSummary` |
| H6 | Outputs are written as `<name>.part` and renamed only on success; failures remove the partial and keep an older image of the same name. | `ffpkg_part_path`/`finalize_ffpkg_part`; `_move_final` | `FfpkgPartFiles`, `tests/test_ufs2tool_windows.py` |
| M1 | "Complete"/green and history `success` are written only after verification; inconclusive has its own state. | `_item_done` shows "Built — verifying…", history in `_verify_done_final` | — |
| M2 | Rebuilding over an existing image sets the old one aside and restores it if the new build fails (failed new image kept as `.failed`). | `set_aside_previous_image`, `settle_previous_image` | `RebuildKeepsPreviousImage` |
| M3 | Workers never touch widgets; detection results carry a generation token so a stale result cannot rename an image. **Still open in legacy** (`_detect_game_info` race). | `_ts_after` patch; `_space_gen` in unibuild | — |
| M4 | No tree walks or rmtree on the UI thread. **Still open in legacy** for `_run_queue` and `_choose_intermediate_dir`. | verify size walk and delete moved to workers | `BackgroundDelete` |
| M5 | Only volumes this process mounted are swept; never force-dismount user mounts; graceful before forced dismount. | `_preexisting_osf_volumes` + `osf_labelled_volumes` | `StaleMountSweep` |
| M6 | Updater: only in the packaged build, refuses while a build runs, requires `<exe>.sha256` (fail closed), size check, timeouts on every request, closes through the normal close path. | `_auto_update` :26353 | `UpdaterEnvironment` |
| M7 | Elevated code never runs scripts/tools from a location another user process can change between check and use; system tools by full path. | per-run `mkdtemp`, `scripts_intact`/`ufs2tool_intact` before each use, full PowerShell path in the bat, fresh private Backport folder | `ExtractedToolIntegrity` |
| M8 | One finalize path (verify, status, delete-source, cancel, staging) for every pipeline. **Still open in legacy** (Convert tab, PFS tab and unified differ). | — | — |
| M9 | Startup recovery of the app's own temp folders older than 24 h; `.part`/`.tmp` leftovers detectable. | `sweep_old_temp_dirs` :577 (prefixes `exfat_builder_ufs2_`, `exfat_builder_bp_`, `exfat_builder_abp_`, `exfat_builder_`) | `TempLeftoverSweep` |
| M10 | Decode console tools in the code page they write (robocopy/cmd/PowerShell: OEM), or force UTF-8 on the child. | `encoding='oem'` in build and Convert robocopy | `tests/test_console_encoding_windows.py` |
| L1 | Settings: atomic write; non-object JSON set aside; history atomic and kept on read error. | `load_settings` :132, `save_settings` :152, `_save_history` :12102 | `SettingsAndHistoryFiles` |
| L2 | Ctrl+B / Esc act on the visible Build queue. | `_kbd_build` / `_kbd_cancel` :4094 | — |
| L3, L6 | Open in legacy (dead retry branch; quoting in `relaunch_as_admin`, `%` regex, language rebuild guard). Do not port the dead code. | — | — |

---

## 2. Formats

### 2.1 What `vendor/lazy_mkpfs` implements

Vendored commit `78bdd00` (2026-06-18) of the Lazy fork of PSBrew/MkPFS, GPL-3.0 (inherited). Local patches: pip auto-install disabled in `__init__.py` and `compression._auto_install`. The app only ever calls **`main_file.py` → `pack_file()` → `build_pfs_stream_single_file()`** (one file, unsigned, unencrypted, 32-bit inodes, case-insensitive, block size 0x10000). `build_pfs` (folders), `pack_folder`, `pack_batch`, `inspect`, `extract_pfs_image`, `verify_pfs`, `create_exfat_image`, signing and encryption are present but **unused** by the app.

All integers little-endian. `PFS_MAGIC = 20130315` (0x0133232B). Block size default 0x10000.

#### PFS header (block 0, `_pack_pfs_header_block`, `build.py:247`; parser `inspect.py:94`)

| Offset | Type | Field / value written |
|---|---|---|
| 0x000 | i64 | version: 1 = PS4, **2 = PS5** |
| 0x008 | i64 | magic 20130315 |
| 0x010 | i64 | id = 0 |
| 0x018 | u8 ×4 | fmode 0, clean 0, **readonly 1** (0x1A), reserved 0 |
| 0x01C | u16 | mode: 0x1 signed, 0x2 64-bit inodes, 0x4 encrypted, **0x8 case-insensitive** (app: 0x8) |
| 0x01E | u16 | 0 |
| 0x020 | u32 | block size |
| 0x024 | u32 | nbackup = 0 |
| 0x028 | i64 | nblock = 1 |
| 0x030 | i64 | dinode count |
| 0x038 | i64 | ndblock (total data blocks = image size / block size) |
| 0x040 | i64 | dinode block count |
| 0x048 | i64 | 0 (superroot field, not written) |
| 0x050 | 0x310 bytes | "inode-block" pseudo-inode in S64 layout (`build_inode_block_sig_s64`): mode 0 @0, nlink 1 @2, flags 0x10 (unsigned) or 0 (signed) @4, size and size_compressed = dinode_blocks × block size @8/@0x10, 4 timestamps @0x18, blocks = dinode block count @0x60, direct pointers as i64 at `0x68 + i·40 + 32` (unsigned: db[0] = 1, rest 0), 5 indirect at `0x68 + 12·40 + i·40 + 32` = 0 |
| 0x368 | u32 | 1 when unsigned and unencrypted |
| 0x36C | u32 | 1 when signed or encrypted |
| 0x370 | 16 bytes | seed (zero) |
| 0x380 | 0x5A0 | header digest (HMAC, signed mode only) |

#### Inode, unsigned D32 layout (0xA8 bytes; `types.py` `Inode.to_bytes`)

| Offset | Type | Field |
|---|---|---|
| 0x00 | u16 | mode: 0x4000 dir, 0x8000 file; permission bits; written as **r-x for o/g/u = 0x16D** |
| 0x02 | u16 | nlink (dirs: 2 + subdirs, uroot 3+) |
| 0x04 | u32 | flags: 0x1 COMPRESSED, 0x10 READONLY, 0x20000 INTERNAL (super-root, flat_path_table, collision_resolver), 0x4\|0x8 extra when signed |
| 0x08 | i64 | `size` — for a **compressed** file this is the **stored (PFSC) size** |
| 0x10 | i64 | `size_compressed` — for a compressed file this is the **logical size rounded up to the block size**; for uncompressed files and dirs = size |
| 0x18 | i64 ×4 | atime, mtime, ctime, birthtime (all = build time) |
| 0x38 | u32 ×4 | nanoseconds (0) |
| 0x48 | u32, u32 | uid 0, gid 0 |
| 0x50 | u64, u64 | unk1 0, unk2 0 |
| 0x60 | u32 | blocks (≥ 1) |
| 0x64 | i32 ×12 | direct block pointers; unsigned layout stores **contiguous extents**: db[0] = first block, db[1..11] = −1 |
| 0x94 | i32 ×5 | indirect pointers (0) |

Inodes are packed `block_size / 0xA8` per block (390 at 64 KiB) and never straddle a block. Signed layouts (unused): S32 0x2C8 bytes (pointer table at 0x64, entries = 32-byte HMAC + i32), S64 0x310 (table at 0x68, 32-byte HMAC + i64), with one/two-level indirect blocks of signature records.

#### Directory entry (`Dirent.to_bytes`)

`u32 inode, i32 type, i32 name_len, i32 entry_size` + ASCII name (non-ASCII → error) + zero pad; `entry_size = name_len + 17` rounded up to 8. Types: 2 file, 3 dir, 4 `.`, 5 `..`. Directory payloads are padded to whole blocks. For the root (`uroot`) `..` points to itself.

#### Fixed inode numbering and super-root

| Inode | Name | Notes |
|---|---|---|
| 0 | super-root (dir, INTERNAL) | dirents: `flat_path_table` (file), [`collision_resolver` (file)], `uroot` (dir) |
| 1 | `flat_path_table` (file, INTERNAL) | FPT blob |
| 2 | (`collision_resolver`, only when needed; then uroot moves to 3 and all inodes are renumbered) | |
| 2/3 | `uroot` (dir) | the game root |
| then | directories sorted case-insensitively by relative path, then files sorted case-insensitively | |

#### Flat path table and collision resolver (`inspect.py:332-400`)

* Hash: `h = 0; for c in path: h = (ord(upper(c)) + 31·h) mod 2^32` over the full path with a leading `/` and forward slashes (`/sce_sys/param.json`); directories included, root excluded.
* FPT = sorted-by-hash array of `u32 hash, u32 value`; value = inode number, `| 0x20000000` for directories; for a hash shared by several paths `0x80000000 | offset` into the collision blob.
* Collision blob: for each colliding hash (ascending), dirents whose **name is the full path**, then 0x18 zero bytes.

#### Block layout of a single-file image (what the app produces via Lazy)

| Block | Content |
|---|---|
| 0 | header (written as a placeholder, rewritten at the end with the final `ndblock`) |
| 1 | inode table (4 inodes: super-root, FPT, uroot, file) |
| 2 | super-root dirents |
| 3 | FPT (one entry: hash of `/<file name>` → inode 3) |
| 4 | reserved empty block (excluded from encryption) |
| 5 | uroot dirents (`.`, `..`, file) |
| 6… | file payload: PFSC container (§2.2) if compression helped, else raw bytes zero-padded to whole blocks |

The image is truncated to `ndblock × block_size`. Post-write `validate_image_quick` (`build.py:1082`) re-reads the header and inodes. Folder images (`build_pfs`, used only by stock MkPFS in E5) follow the same scheme with every directory and file laid out contiguously after the FPT/collision blocks.

### 2.2 PFSC compressed-file container (`compression.py`)

| Offset | Type | Field |
|---|---|---|
| 0x00 | i32 | magic 0x43534650 (`PFSC`) |
| 0x04 | i32 | unk4 = 0 |
| 0x08 | i32 | unk8 = 6 |
| 0x0C | i32 | logical block size 0x10000 |
| 0x10 | i64 | logical block size again |
| 0x18 | i64 | block-offset table offset = 0x400 |
| 0x20 | u64 | data offset (= header size) |
| 0x28 | i64 | data length = block count × 0x10000 (logical, padded) |
| 0x400 | u64 × (n+1) | absolute offsets of each stored block within the PFSC payload, first = data offset, last = end |

* Header size = 0x10000, plus whole 64 KiB blocks when the table exceeds 0xFC00 bytes (more than 8,063 blocks ≈ 504 MiB).
* Each 64 KiB logical block (last one zero-padded) is `zlib.compress(block, level)` — **zlib format (RFC 1950 header + Adler-32), default window**. Stored compressed if shorter than 64 KiB and gain ≥ `threshold_gain` % (`pack_file`: threshold 1, min file gain 0); otherwise stored raw (64 KiB). A reader treats any stored block shorter than 64 KiB as compressed — there is no per-block flag.
* Whole file stored raw (inode not COMPRESSED) if no block compressed or the container is not smaller than the raw file. Files < 1 MiB are not compressed (`--min-compress-size` default 1.0 MB in `main_file.py`).
* Backends: stdlib `zlib`, `zlib-ng`, `isa-l` (`isal_zlib`), selected per process; missing backend → stdlib. ISA-L levels 0–3. Upstream MkPFS reported ISA-L streams the PS5 inflater rejects (DEPENDENCIES §3, R8) — treat as risky. Output with zlib is deterministic for a given level (UNVERIFIED across zlib versions).
* Parallelism: single file ≥ 256 MiB → `ThreadPoolExecutor` with `cpu_count` workers (`0` → `max(1, cores − 1)`), at most `workers × 8` blocks in flight, output strictly in order.
* Reader validation (`_parse_pfsc_header` :542): magic, unk4 0, unk8 6, block size 0x10000 twice, length aligned, table at 0x400, data offset ≥ 0x10000, offsets monotonic and in range, decompressed block exactly 64 KiB.

### 2.3 Crypto (`crypto.py`) — present, unused by the app

| Item | Definition |
|---|---|
| EKPFS | 32 bytes, default all zero |
| Derived key `k(i)` | `HMAC-SHA256(EKPFS, LE32(i) ‖ seed16)` |
| Sign key | `k(2)`; each signature = `HMAC-SHA256(signKey, block with its own 32-byte slot zeroed)`; levels 0–4 signed bottom-up; header digest over 0x5A0 bytes at 0x380 |
| Encryption | AES-XTS-128: `k(1)` → tweak key = bytes 0–15, data key = bytes 16–31; cipher key = data ‖ tweak; sector 0x1000; sector number = absolute offset / 0x1000; tweak = `LE64(sector) ‖ 0`; encrypts every sector from block 1 on, skipping reserved empty blocks. "newCrypt": base key = `HMAC-SHA256(EKPFS, seed)` first. |
| Defect (NEW) | `build_pfs` signed path calls `read_image_bytes(out, offset, size)` but the function takes `(fh, header, offset, size, …)` → TypeError. Signed folder images cannot be built with this copy. Do not port signing without a test vector. |

### 2.4 Stock MkPFS 0.0.9 (pip, not vendored) — what the app relies on

Read from the wheel (`mkpfs/cli.py`). Commands used: `pack folder`, `pack file`, `unpack`, `verify`, `tree --deep`. Flags used by the app: `--no-compress`, `--no-adjust-output-file-extension`, `--version PS5`, `--inode-bits 32`, `--cpu-count`, `--compression-level` (default 9), `--temp-folder`, `--verify`, `--overwrite`, `--deep`, `--source-dir`. Defaults the app inherits: post-pack quick structure verification on (rc 1 on failure), `--threshold-gain 0`, `--block-size auto` (64 KiB), case-insensitive, `--skip-executable-compression` on, `--rename-inner-image` on (the inner file of `pack file` becomes the title id if the name contains one, else ASCII alphanumerics ≤ 15 chars + extension, so `pfs_image.dat` stays `pfs_image.dat`), direct-to-image streaming for `pack file`, temp/output `<out>.tmp`. MkPFS 0.0.9 also ships a **pure forward-only exFAT writer** (`mkpfs pack exfat`, `exfat_writer.py`: 512-byte sectors, FAT at sector 128, one FAT, contiguous allocation with explicit chains, cluster 32 KiB or 64 KiB for large average files, OS metadata excluded) and a native exFAT reader (`mkpfs.exfat.open_exfat`). The legacy app never uses `pack exfat` — a candidate OSFMount-free exFAT path for the native app (UNVERIFIED on a PS5).

Exclusive to Lazy vs. stock: `skip_executable_compression` list (`eboot*.bin`, `param*.sfx`, `*.prx`, `*.sprx`, `*.json`, `*.txt`, `*.png`, `*keystone`, anything under `sce_module`/`sce_sys`) applies to folder builds only.

### 2.5 AMPR index `ampr_emu.index` (`ui/build_ampr_index.py`; Lazy copy `vendor/lazy_mkpfs/lazy_mkpfs/ampr_index.py`)

| Part | Layout |
|---|---|
| Header (48 bytes, `<8sIIQQQII`) | magic `AMPRIDX3`, u32 version 3, u32 record size 24, u64 row count, u64 path-blob length, u64 hash-table offset, u32 slot size 16, u32 slot count |
| Records (24 bytes each, `<IIQq`), sorted by lower-case path | u32 path offset in blob, u32 path length (without NUL), u64 file size, i64 mtime (seconds) |
| Path blob | `/app0/<relative path with />` UTF-8, NUL-terminated |
| Padding | zero to a 16-byte boundary |
| Hash slots (16 bytes, `<QII`) | u64 hash, u32 record index + 1 (0 = empty), u32 flags (bit 0 = duplicate hash) |

Hash = FNV-1a 64 (offset 1469598103934665603, prime 1099511628211) over the **Unicode code points** of the lower-cased, forward-slash path (`h ^= ord(ch)`), 0 mapped to 1. Slot count = smallest power of two ≥ 2 × rows (minimum 2); linear probing. Difference: the app's builder also flags the *new* slot as duplicate; Lazy's copy flags only the old one. Paths containing tab/CR/LF are skipped; the index excludes itself and its `.tmp`; written to `<index>.tmp` then replaced. Also buildable from an FTP root and uploadable (Advanced › AMPR Index).

### 2.6 `create_exfat.py` (Lazy) — not a pure exFAT writer, unused

Sizes with 64 KiB clusters by default (`total_alloc + FAT (4 B/cluster) + bitmap + (files+dirs) × 256 + 32 MiB`, spare = base/200 clamped [64 MiB, 512 MiB], min raw + 64 MiB, rounded to MiB) and then delegates formatting to the OS: Linux loop device + `mkfs.exfat -c <K> -L <label>`, macOS `newfs_exfat`, Windows `fsutil file createnew` + OSFMount `-o rw` + `cmd.exe /c format X: /FS:exFAT /Q /Y /A:<K|M> /V:<label>` + parallel copy. Default label `PS5exfat`. Not referenced by the app.

### 2.7 Other formats the engines read or write

| Format | Where | Notes |
|---|---|---|
| exFAT boot sector check | `_verify_image_file_only` | `EXFAT   ` at offset 3; ≥ 64 MiB |
| param.sfo | `parse_sfo` :176 | magic `\0PSF` (0x46535000), header `<IIIII>` (magic, version, key table, data table, count), 16-byte entries `<HHIII>` (key off, fmt, len, max len, data off); fmt 0x0204 UTF-8, 0x0404 u32 |
| UFS2 (ffpkg) | UFS2Tool | App-chosen newfs params: UFS2 (`-O 2`), block 64 KiB, fragment 64 KiB, minfree 0 %, sector **512 (locked; 4096 breaks on Windows)**, 256 KiB per inode. Convert tab uses 32 KiB/4 KiB. |
| `.ffpfs` vs `.ffpfsc` | E5/E4 | `.ffpfs` = uncompressed PFS of the dump; `.ffpfsc` = PFS with one PFSC-compressed nested image (`.exfat`, `.ffpkg` or `pfs_image.dat`) |
| summary.json, history, queue JSON | §3 | |

---

## 3. Queue / job model

### 3.1 Unified Build queue (`ui/tab_unibuild.py`, in memory only)

| Aspect | Behaviour |
|---|---|
| Job types | `exfat`, `ffpkg`, `pfs` (folder via intermediate, or `from_image`), `fpkg` (→ FPKG_PARITY.md) |
| Job fields | `src, outdir, name, type, temp, via, ppsa, from_image, src_ext, exfat_size ('auto' or GB), backend, zlib_backend, status, need_out, need_tmp, out_drive, tmp_drive, fpkg{tool,level,chunks,verify,keep,ref_pkg}`; runtime `_t0, duration, _final_path, _inter_path, _inter_is_artifact, _inter_size, _final_preexists, out_path, space_saved, compression, error, companion_path` (`:2212`) |
| States | `queued → running → done \| warn \| failed \| cancelled \| skipped` (`None` result = skipped) |
| Ordering | FIFO; only `queued` rows can be moved up/down or removed; Clear while running removes queued rows only |
| Run | `_uq_build_all` :3309: refuses if a legacy exFAT/ffpkg build is busy; per-drive space confirm; **failed/skipped jobs re-queued** (manual retry); persists `temp_dir` and `output_dir`; sequential `_uq_run_next` :3160 |
| Retry | Manual (Build All again); exFAT also auto-retries `retry_count` times inside the engine |
| Cancel | `_uq_cancel` :1007 sets `cancel`, `running=False`; PFS/FPKG stop via `cancel_check`; exFAT/ffpkg finish and are kept; remaining jobs do not start |
| Finish | `_uq_finish` :3187: clears hidden legacy queue entries, totals (outputs, total size, duration, compression %, space saved), headline cancelled > failed > complete, toast, shutdown trigger `unibuild` (not after cancel) |
| Per-job snapshot | `backend`, `zlib_backend`, `via`, `temp`, size mode captured at Add. **NEW:** exFAT/ffpkg/PFS dispatch read the global `temp_dir`, not `job['temp']` (only FPKG uses it). |
| Persistence | None (queue is lost on exit). |

### 3.2 Legacy exFAT queue (hidden `build` tab, `QueueItem` `exfat_builder.py:3048`)

Fields `game_folder, output_dir, output_name, game_title, title_id, version, status, size_override_gb` (+ runtime `_retry_count, _previous_image, _src_size`). States `waiting → building → done | warn | failed` (`error` also tolerated). Run order = indices of `waiting` items. Per-item size override dialog (`_show_size_override_dialog` :11293, values coerced by `coerce_size_gb` :3031). "Pause after current" (`_toggle_pause_queue` :12780) is honoured **only on the failure → Continue path** (`_check_pause_then_next` is called only from `_item_done` failure) — NEW defect. Save/Load queue to a user-chosen JSON list of `{game_folder, output_dir, output_name, game_title, title_id, version}` (`_queue_save` :13088, `_queue_load` :13113; missing folders skipped; size overrides not saved). Library *Build All* (`_lib_build_all` :13144) fills this queue and shows the hidden tab.

### 3.3 Other queues

| Queue | Location | Model |
|---|---|---|
| Legacy ffpkg | `_ffpkg_queue` dicts `{folder, odir, name, status}` | `waiting → building → done \| failed` |
| PFS build | `ui/tab_pfs.py` `queue` dicts `{game_root, out_path, name, outdir, work_dir, fmt, version_ps5, cpu_cores, temp_dir, status}` | `queued → running → done \| failed`, pause, per-queue shutdown checkbox |
| PFS convert | `ui/tab_pfs.py` `cq_*` | same states; dialogs suppressed while running |
| FTP transfers | `ui/tq_model.py` `TransferItem` | `queued → uploading → done \| failed \| cancelled`, serial, cancel flag, no persistence |

### 3.4 Delete-source options and safety rules

| Option (key) | Applies to | Rule as implemented | Gap to fix in native |
|---|---|---|---|
| `adv_delete_source` | exFAT (legacy and unified, incl. PFS intermediates' source dump) | After `_verify_done_final(ok)` **and** `_take_content_verified(out)` (native mkpfs PASS with `--source-dir`, or strict mount walk). Skipped verify / inconclusive → kept + log. rmtree on a worker. | Toggle is not persisted by its own control (§4 note). |
| `adv_delete_source_ffpkg` | ffpkg | rc 0 and `ffpkg_safe_to_delete_source` (image ≥ source bytes, `eboot.bin`, all files readable) | No content verification exists for UFS2. |
| `pfs_delete_source_after_convert` | legacy PFS tab Convert only | rc 0, fresh output, output path ≠ source | Not applied by unified image→.ffpfsc (always keeps source). |
| Pipeline intermediate | E3 / legacy pipeline | Deleted after a good convert (`delete_source_on_success=True`); kept when result is bad, suspiciously small, cancelled, or flagged by verify (`warn`) | — |
| User-selected image (E4) | unified | Never deleted (`delete_source_on_success=False`) | — |

### 3.5 Persisted files besides settings

| File | Location | Content / rules |
|---|---|---|
| Settings | `~/.exfat_builder_settings.json` (`_settings_path` :110) | §4 |
| History | `~/.exfat_builder_history.json` (`_history_path` :10345) | list, newest first, max 50, `{title, title_id, version, folder, output, success, time}`; written atomically via `.tmp`; unreadable file renamed `.corrupt`. **Only exFAT builds are recorded** (failure in `_item_done`, verdict in `_verify_done_final`). Read by `ui/tab_history.py` (stats, filter, CSV export, delete all). |
| PS5 activity | `~/.exfat_builder_ps5_activity.json` | last 20 `{kind: payload\|connection\|error, detail, time}` |
| Session log | `<logs_dir>/exfat_builder_<YYYY-MM-DD_HH-MM-SS>.log` + `latest.log` | every Output Log line mirrored (thread-safe); banner (version, Windows, Python, MkPFS version/deep capability); pruned > 30 days, keep newest 50 |
| Build log export | `<logs_dir>/<YYYYmmdd_HHMMSS>_<image stem>[_FAILED].log` | Output Log text after each exFAT verdict |
| Crash log | `~/exfat_builder_logs/crash_<ts>.log` | always default folder (ignores `logs_dir`, NEW) |
| Cover cache | `~/exfat_builder_logs/covers_norm/` | normalised 260×390 covers (Pillow, formats PNG/JPEG/WEBP/BMP/DDS, ≤ 8192² px) |
| Build summary | `<image stem>.summary.json` | §1.1 |
| Bug report | user-chosen | includes settings snapshot with keys matching password/token/secret/apikey/credential/private_key redacted (`_redact_settings` :3439) and private paths scrubbed |

---

## 4. Settings

### 4.1 Storage semantics

* File: `os.path.expanduser('~')/.exfat_builder_settings.json` — one flat JSON object, `indent=2`. Under over-the-shoulder UAC `~` is the admin account's profile (audit L1).
* Load (`load_settings` :132): missing → `{}`; unreadable or not an object → renamed to `.corrupt`, `{}` (silent). `_migrate_settings` :113 renames `exfat_experimental_mkpfs` → `experimental_mkpfs_enabled` (never read afterwards) and `experimental_mkpfs_dir` → `experimental_mkpfs_folder`.
* Save (`save_settings` :152): `<file>.tmp`, flush, fsync, `os.replace`; errors to stderr only. Callers save immediately, debounced 1.5 s (`_save_settings_debounced` :4938, tab switches), or rely on the close handler (`_on_close` writes `window_geometry`, `language`).
* Values are stored with mixed types (many numbers as strings). Defaults live at each read site; there is no schema.

### 4.2 Persisted keys (108 live keys)

"Functional" = **Yes** (read and changes behaviour), **Partial** (only some paths), **Legacy-only** (hidden legacy tab/queue only), **UI state**, **Orphan** (read, but nothing in the UI writes it), **Dead** (written or migrated but nothing reads it, or the reader does nothing). Line numbers are `exfat_builder.py` unless a file is named.

#### Build, image engines, PFS

| Key | Type / values | Default | Set by (UI control) | Read by | Functional | Notes |
|---|---|---|---|---|---|---|
| `output_dir` | path | `''` | Build › Output folder Browse (`tab_unibuild.py:266`, immediate); Build All (`tab_unibuild.py:3366`); legacy browse :7618/:7677; Library Build All :13157; Library quick add :22880 | init :4334; `tab_unibuild.py:119` | Yes | Also decides whether Output is "user chosen" |
| `temp_dir` | path | `''` (→ system temp) | Settings › Build › Temp folder Browse (:7681, "takes effect on next launch") / Reset (`tab_settings.py:523`); Build › Temp folder (`tab_unibuild.py:276`); Build All (`:3362`) | script/UFS2Tool/Backport extraction; startup sweep; PFS `--temp-folder` + redirect; intermediate staging; image staging; Clear Temp; temp size; Lazy (logged only) | Yes | Script folder only at launch; everything else live |
| `osfmount_path` | path | `''` | Settings › OSFMount Browse/Detect (:20346, :20354, :20372) | `_find_osfmount` :8760 → bat arg 3 | Yes | Folder or file accepted; `.com` sibling preferred |
| `adv_cluster` | `Auto`,`32768`,`65536`,`131072`,`262144` | `Auto` | Advanced › Build Parameters › Cluster size | `_get_cluster_size_arg` :10139 → `-ClusterSizeOverride` | Yes | UI says "Auto (64K)" but PS1 auto picks **32K when average file < 1 MiB** (NEW) |
| `adv_sector` | `512`,`4096` | `512` | Advanced › Sector size | `_get_sector_size_arg` → `-SectorSize` | **Dead** (NEW) | PS1 only logs it; not passed to OSFMount or format.com; recorded in summary.json |
| `adv_threads` | `1`–`128` (string) | `1` | Advanced › Threads; Build › Advanced performance › copy cores (`tab_unibuild.py:1067`) | `_get_threads_arg` → robocopy `/MT:n`; logged by pipeline | Yes | 1 = sequential (recommended); never 0, so PS1 media detection never runs |
| `adv_retries` | int string | `3` | Advanced › Robocopy retry count (/R) | bat arg 7 → `/R:n` | Yes | PS1 own default 2 is never used |
| `adv_retry_wait` | int string (s) | `3` | Advanced › Retry wait (/W) | bat arg 8 → `/W:n` | Yes | |
| `adv_skip_verify` | bool | false | Advanced › Skip post-build re-mount check | `_item_done` | Yes | Also prevents delete-source |
| `adv_excl_hidden` | bool | false | Advanced › Exclude hidden files | bat arg 9 → `/XA:H`; PS1 stats; verify walks; native verify exclusion test | Yes | |
| `adv_img_size_override` | GB string | `''` | Advanced › Post-Build › Image size override (saves via `_adv_save_post_build`); the same field in the Build Parameters card saves via `_save_adv_params`, which **does not write this key** | `_process_next` :10962 only when the item has no per-item size | **Legacy-only** (NEW) | Unified exFAT jobs always pass `'auto'` or a custom size, so the global override never applies to Build-tab jobs |
| `lowercase_names` | bool | false | none (no widget) | `_apply_name_case` :23309 → Backport ZIP prefixes only | Orphan | Despite the name, never applied to image names |
| `adv_delete_source` | bool | false | Advanced › Post-Build › Danger zone (toggle saves through `_save_adv_params`, which omits this key; persisted only when another Post-Build control calls `_adv_save_post_build`) | `_verify_done_final` | Yes (persistence gap, NEW) | §3.4 |
| `adv_delete_source_ffpkg` | bool | false | same as above | `_ffpkg_item_done` | Yes (persistence gap, NEW) | §3.4 |
| `adv_auto_upload` | bool | false | Advanced › Post-Build › Auto-upload .exfat to PS5 | `_verify_done_final` (not in unified mode) | Legacy-only | FTP upload of the image |
| `adv_checksum_verify` | bool | false | Advanced › Post-Build › Checksum verification | `_verify_done_final` | Yes | SHA-256 logged only |
| `adv_robo_log` | bool | false | none visible (`_build_advanced_post_build` :19911 is never called) | nothing | Dead | No `/LOG` is ever passed |
| `adv_robo_log_dir` | path | `''` | none visible | nothing | Dead | |
| `adv_write_benchmark` | string | `—` | none (`_run_write_benchmark` does not exist) | Advanced › Performance pill | Dead | "Run" shows "(no benchmark)" |
| `adv_ffpkg_block` | `32768`,`65536`,`131072` | `65536` | Advanced › ffpkg block size (-b) | newfs `-b` (build, ffpkg editor rebuild) | Yes | Convert tab ignores it |
| `adv_ffpkg_frag` | `32768`,`65536` | `65536` | Advanced › ffpkg fragment (-f) | newfs `-f` (clamped ≤ block) | Yes | |
| `adv_ffpkg_minfree` | int % string | `0` | Advanced › Min Free % (-m) | newfs `-m` | Yes | |
| `adv_ffpkg_inode` | `131072`,`262144`,`524288` | `262144` | Advanced › Bytes/inode (-i) | newfs `-i` | Yes | Sector `-S 512` is locked |
| `retry_count` | int 0,1,2,3,5 | 3 | Settings › Build › Auto-retry on failure | `_item_done` :10530 | Yes | exFAT builds only (legacy and unified), 2 s delay |
| `check_updates_on_boot` | bool | true | Settings › Build › Updates switch | `__init__` :4493 (after 3 s) | Yes | |
| `pipeline_to_pfs` | bool | false | Settings › Build › PFS Convert › pipeline switch | `_pipeline_to_pfs_enabled` :24185 (legacy exFAT/ffpkg queues) | Legacy-only | Unified has its own PFS type |
| `pfs_delete_source_after_convert` | bool | false | Settings › Build › PFS Convert | `ui/tab_pfs.py:1795` | Partial | Legacy PFS Convert only |
| `exfat_extract_robocopy_mt` | bool | false | Settings › Build › Extraction (.exfat) | `_run_extract` :11799 | Yes | |
| `exfat_extract_mt_threads` | int 1–128 | 32 | Settings › Build › Extraction threads | `_run_extract` :11802 | Yes | |
| `pfs_backend` | `lazy`,`mkpfs` | `lazy` | Settings › PFS convert backend radio (`tab_settings.py:713`); Build › PFS backend (`tab_unibuild.py:469`, `:1098`) | per-job backend; pipeline default; native exFAT verify gate :23437; Extract `--deep` gate (`tab_extract.py:1339`) | Yes | One key controls four unrelated behaviours |
| `experimental_mkpfs_folder` | path | `''` | Settings › PFS convert backend › Lazy folder override | `_resolve_lazy_source`; Build tab label | Yes | Executes user-chosen Python elevated |
| `pfs_compression_level` | int 1–9 (zlib) / 0–3 (ISA-L) | 9 | Build › Advanced performance › compression level / ISA-L level (`tab_unibuild.py:1138-1208`) | `_pfs_pack_tuning`; Lazy `--zlib-level` | Yes | Also scales the stock-mkpfs worker cap |
| `pfs_lazy_zlib_backend` | `zlib`,`isa-l` | `zlib` | Build › Advanced performance › engine (`:520`, `:1131`) | per-job `zlib_backend` | Yes | ISA-L: PS5 inflater risk |
| `pfs_lazy_cpu_count` | int | 0 | none | pipeline Lazy `--cpu-count` :24441 | Orphan | Always auto |
| `pfs_intermediate_via` | `exfat`,`ffpkg` | `exfat` | Build › PFS "Intermediate" choice (`tab_unibuild.py:438`) | job `via` | Yes | |
| `name_preset` | `ppsa`,`ppsa_title`,`ppsa_title_ver`,`ppsa_title_ver_fw`,`custom` | `ppsa_title_ver` | Build › Output › Name preset | `_gen_name` | Yes | |
| `name_custom_template` | string | `%TITLE% (%VERSION%) (%FIRMWARE%) (%PPSA%)` | Build › Output › Template | `_gen_name` | Yes | |
| `ffpkg_output_dir` | path | `''` | legacy ffpkg tab output (:7989); Library quick-add ffpkg (:22899) | `ui/tab_ffpkg.py:51` | Legacy-only | |
| `ffpkg_extract_outdir` | path | `''` | legacy ffpkg extract pane (:5285) | :5168 | Legacy-only | Pane reachable only from the hidden ffpkg tab (UNVERIFIED) |
| `fpkg_pubtools_path`, `fpkg_compression`, `fpkg_chunk_count`, `fpkg_tool_verify`, `fpkg_keep_project` | see FPKG_PARITY.md | — | Build › Output format › FPKG (`tab_unibuild.py:603`) | FPKG job | Yes | → `native/docs/FPKG_PARITY.md` |

#### App, UI state, logging, notifications

| Key | Type | Default | Set by | Read by | Functional | Notes |
|---|---|---|---|---|---|---|
| `last_tab` | tab key | `home` | every tab switch (:5032, debounced) | startup (:4903) with legacy-key migration | Yes | `build/ffpkg/pfs` → `unibuild`, `files` → `edit` |
| `sidebar_collapsed` | bool | false | sidebar collapse (:4582) | :4653 | Yes | |
| `window_geometry` | `WxH+X+Y` | `''` → 1280×820 | close (:4020) | init :4280, :4507 (clamped on screen) | Yes | |
| `settings_active_rail` | `osfmount`,`build`,`logs`,`ftp`,`notifications`,`theme`,`shutdown` | `osfmount` | Settings rail | `tab_settings.py:296` | UI state | |
| `adv_active_subtab` | `params`,`post`,`lang`,`ampr`,`dlc` | `params` | Advanced pills | `tab_advanced.py:232` | UI state | |
| `ffpkg_active_subtab` | string | `build` | legacy ffpkg sub-tabs (:5136) | :5130 | UI state, legacy | |
| `y2jb_active_subtab` | string | `connection` | Y2JB sub-tabs (`tab_y2jb.py:183`) | `tab_y2jb.py:207` | UI state | |
| `language` | `en` + 16 codes (`zh de fr es pt ja ko ru ar it nl pl tr th vi id`) | `en` | Settings › Language (`tab_language.py:548`); close (:4028) | `set_language` at init | Yes | Translation JSON import is a stub ("not yet wired up") |
| `theme` | `dark` | `dark` | Settings › Theme (:26746) | init :4447 (`light` coerced to `dark`) | Effectively constant | |
| `notify_sound` | bool | true | Settings › Notifications › Play sound | `_notify` :26177 | Yes | `winsound.MessageBeep` + PowerShell balloon |
| `discord_webhook` | URL | `''` | none | `_discord_notify` :26210 | Orphan | Works only if hand-edited |
| `logs_dir` | path | `''` → `~/exfat_builder_logs` | Settings › Logs Browse (:25094); `_save_extra_settings` | `_get_logs_dir`; bug report | Yes | Crash log ignores it |
| `recent_folders` | list (≤10) | `[]` | `_add_to_recent` :13083 | `_update_recent_menu` | Legacy-only (UNVERIFIED visible) | |
| `release_last_seen_version` | string | `''` | release modal (`ui/release_notes.py:100`) | `ui/release_notes.py:95` | Yes | One-time "what's new" |
| `support_op_count` | int | 0 | `note_successful_operation` (`release_notes.py:456`) | same | Yes | Support-prompt frequency |
| `support_prompt_last_ts` | epoch | 0 | `release_notes.py:479` | `release_notes.py:457` | Yes | |

#### Library, dumps, Language Stripper

| Key | Type | Default | Set by | Read by | Functional |
|---|---|---|---|---|---|
| `lib_folders` | list of paths | `[]` | Library add/remove folder (:21982, :22005) | init :4434, `_lib_scan` | Yes |
| `lib_auto_scan` | bool | true | none | init :4495 (scan after 1.5 s) | Orphan (always on) |
| `img_scan_dirs` | list of paths | `[]` | Library › Built Images add/remove folder (:12827, :12842) | `_img_scan` :12856 (extensions `.exfat .ffpkg .ffpfs .ffpfsc`, :12854); `tab_images.py:47` | Yes |
| `dumps_last_src` | path | `''` | Dump Rename scan (:23093; saved on next write) | `ui/dump_rename.py:138` | Yes |
| `adv_lang_mode` | `single` / batch | `single` | Advanced › Language Stripper mode radio (no writer) | `ui/tab_advanced.py:1841` | Orphan (resets each launch) |
| `adv_lang_folder` | path | `''` | Advanced › Language Stripper folder (`tab_advanced.py:1949`) | `tab_advanced.py:1854` | Yes |

#### Shutdown after a queue

| Key | Type | Default | Set by | Read by | Functional |
|---|---|---|---|---|---|
| `shutdown_action` | `none`,`shutdown`,`restart`,`sleep` | `none` | Settings › Auto-Shutdown action | `_trigger_shutdown` :20256; queue ends | Yes (`shutdown /s /t 5`, `shutdown /r /t 5`, `rundll32 powrprof.dll,SetSuspendState 0,1,0`) |
| `shutdown_delay` | s: 0,30,60,120,300,600 | 60 | Settings › Auto-Shutdown delay | countdown dialog (cancellable) | Yes |
| `shutdown_on_unibuild` | bool | false | Settings › trigger "Build tab queue finished" | `_uq_finish` | Yes |
| `shutdown_on_exfat` | bool | false | "exFAT queue finished" | legacy exFAT queue end | Legacy-only |
| `shutdown_on_ffpkg` | bool | false | "ffpkg queue finished" | legacy ffpkg queue end | Legacy-only |
| `shutdown_on_pfs` | bool | false | "PFS build finished"; PFS queue checkbox forces it temporarily | PFS tab queue/convert end | Legacy-only |
| `shutdown_on_backport` | bool | false | "Backport finished" | Backport (→ BACKPORT_PARITY.md) | Yes |
| `shutdown_on_ftp` | bool | false | "FTP upload finished" | FTP upload end | Yes |

#### PS5 connection, FTP, payloads, ShadowMount+/MicroMount, Y2JB

| Key | Type | Default | Set by | Read by | Functional |
|---|---|---|---|---|---|
| `ftp_ip` | IPv4 string | `''` | Settings › PS5 FTP › Console IP (`_save_ftp_settings` :25630); auto-detect; Y2JB connection (:20581) | FTP, status-bar ping, Y2JB, default for klog/payload IPs, PS5 Manager | Yes |
| `ftp_port` | int | 2121 | Settings › PS5 FTP › FTP port | FTP; PS5 Manager | Yes |
| `ftp_path` | remote path | `/data/etaHEN/games/` | Settings › PS5 FTP › PS5 games path | upload destination | Yes |
| `ftp_auto` | bool | false | Settings › PS5 FTP › Auto-upload after build | legacy `_verify_done_final`/`_ffpkg_item_done` prompt | Legacy-only |
| `auto_ip` | bool | false | none | `_auto_ip_var` (never used) | Dead |
| `ps5_ip` | IPv4 | falls back to `ftp_ip` | none | status-bar ping :26869 | Orphan |
| `ftp_local_path` | path | `~` | FTP tab local pane navigation (`tab_ftp.py:944`) | `tab_ftp.py:71` | Yes |
| `klog_ip` | IPv4 | `ftp_ip` | Klog connect (:19040) | `tab_klog.py:43` | Yes |
| `klog_port` | int | 3232 | Klog connect (:19041) | Klog, ShadowMount, PS5 Manager | Yes |
| `pl_ip` | IPv4 | `ftp_ip` | Payloads connection (:18479) | `tab_payloads.py:41` | Yes |
| `pl_port` | int | 9090 | Payloads connection (:18480) | Payloads, PS5 Manager | Yes |
| `payloads` | list of payload dicts | `[]` | Payloads add/edit/delete (:18485, `tab_payloads.py:314`) | Payloads list | Yes |
| `al_dir` | path | `''` | Autoloader folder browse (:18821) | Autoloader editor, PS5 Manager readiness | Yes |
| `shadowmount_config` | dict | `{}` | ShadowMount+ config editor save (`tab_shadowmount.py:2908`, `:3056`) | `tab_shadowmount.py:1024` | Yes |
| `smp_payload_path` | path | `''` | ShadowMount+ payload (`:3170`) | `:1767` | Yes |
| `smp_payload_port` | int | 9021 | ShadowMount+ payload (`:3169`) | `:1768`, `:3158` | Yes |
| `micromount_config` | dict | `{}` | MicroMount config save (`tab_micromount.py:1775`, `:1873`) | `:853` | Yes |
| `mm_payload_path` | path | `''` | MicroMount payload (`:1944`) | `:1076` | Yes |
| `mm_payload_port` | int | 9021 | MicroMount payload (`:1943`) | `:1077` | Yes |
| `y2jb_ftp_port` | string | `2121` | Y2JB connection (:20579) | :4475 | Yes |
| `y2jb_dpi_port` | string | `12800` | Y2JB connection (:20580) | :4476 | Yes |
| `y2jb_pkg_usa`, `y2jb_pkg_eu`, `y2jb_pkg_jp` | paths | `''` | Y2JB install `.pkg` browse (`'y2jb_pkg_' + region.lower()`, :18144, `tab_y2jb.py:272`) | :4478-4480 | Yes |

#### Backport (pointer — owned by BACKPORT_PARITY.md)

| Key | Default | Set by | Functional |
|---|---|---|---|
| `abp_fakelib` | `''` | Backport run (:16974) | Yes |
| `abp_last_sdk` | `'4'` | :22530 | Yes |
| `abp_notes` | `''` | `panel_bp_auto.py:136` | Yes |
| `abp_output` | `''` | none | Orphan |
| `abp_target_sdk` | None | :14371 | Yes |
| `abp_zip_orig_dest` | `''` | :15208, :22471, `panel_bp_auto.py:130` | Yes |
| `abp_zip_pat_dest` | `''` | :15211, :22472, `panel_bp_auto.py:133` | Yes |
| `bp_folders` | `[]` | `tab_backports.py:1565` | Yes |
| `bp_scan_folder` | `''` | :13995, `tab_backports.py:1566` | Yes |
| `bpr_log` | `[]` | :13921 (backport report log) | Yes |

#### Migration-only keys (not counted in the 108)

| Key | Behaviour |
|---|---|
| `exfat_experimental_mkpfs` | renamed on load to `experimental_mkpfs_enabled` |
| `experimental_mkpfs_enabled` | written by the migration, read nowhere → Dead |
| `experimental_mkpfs_dir` | renamed on load to `experimental_mkpfs_folder` |

Tally of the 108 live keys by the "Functional" column above: Yes 80 (includes the 10 Backport and 5 FPKG pointer keys), Legacy-only 10, UI state 4 (one of them legacy), Partial 1, Effectively constant 1 (`theme`), Orphan 7, Dead 5.

### 4.3 Non-persisted per-job / per-run options (33)

| # | Option | Scope | Values / default | Where |
|---|---|---|---|---|
| 1 | Output type | Build tab job | `exfat` (default), `ffpkg`, `pfs`, `fpkg` | `tab_unibuild.py:130` |
| 2 | PFS source mode | Build tab | `folder` (default) / `image` | `:132` |
| 3 | exFAT image size mode | Build tab job | `auto` / `custom` | `:585` |
| 4 | exFAT custom size GB | Build tab job | float > 0 | `:586` |
| 5 | Firmware override for naming | Build tab | `auto`, `1`–`13` | `:137` |
| 6 | Output name (editable) | Build tab job | from preset | `name_var` |
| 7 | Overwrite / numbered copy decision | Build tab job | dialog | `:2177` |
| 8 | Low-space "add anyway" | Build tab job | dialog | `:2209` |
| 9 | Game-root choice (multiple candidates) | Build tab | chooser | `:164` |
| 10 | FPKG base package (update build) | FPKG job | path | `:671` → FPKG_PARITY |
| 11 | Backend / zlib backend / via / temp snapshot | Build tab job | captured from settings at Add | `:2212` |
| 12 | Cancel | Build queue | flag | `:1007` |
| 13 | Per-item image size override | legacy exFAT queue item | GB | `_show_size_override_dialog` :11293 |
| 14 | Pause after current | legacy exFAT queue | toggle | :12780 |
| 15 | "Continue with remaining items?" | legacy exFAT/ffpkg | dialog | :10560, :8314 |
| 16 | PFS format | PFS tab job | `ffpfs` / `ffpfsc` | `tab_pfs.py` `fmt_var` |
| 17 | PFS CPU cores | PFS tab job | int, default from cores | `tab_pfs.py:363` |
| 18 | PFS "PS5 version" checkbox | PFS tab job | ignored (always PS5) | `:354` |
| 19 | PFS temp folder | PFS tab job | path | `:355` |
| 20 | PFS queue shutdown checkbox | PFS queue | bool | `:630` |
| 21 | Convert verify (`--verify`) | PFS Convert job | bool | `verify_c_var` |
| 22 | Convert CPU cores | PFS Convert job | int | `cpu_cores_c_var` |
| 23 | Convert temp folder | PFS Convert job | path | `tempdir_c_var` |
| 24 | Extract output folder + name | Extract | path, stem | `tab_extract.py` |
| 25 | Extract overwrite/merge | Extract | confirm | `_run_extract`, PFS `--overwrite` |
| 26 | Extract cancel | Extract (.ffpkg only) | flag | `tab_extract.py:727` |
| 27 | Convert direction + output folder/name | Convert tab | e2f / f2e | `tab_convert.py` |
| 28 | Dump Rename naming preset | Dump Rename | `ppsa`, `ppsa_title`, `ppsa_title_v` (default) | :23012 |
| 29 | Dump Rename destination | Dump Rename | empty = in place | `_dumps_dst_var` |
| 30 | Dump Rename sanitize / uppercase ID | Dump Rename | hidden vars, sanitize on | :23042 |
| 31 | Dump Rename row selection | Dump Rename | checkboxes | `_dumps_items` |
| 32 | Edit overwrite choice | Edit (exFAT/ffpkg) | skip / skip all / overwrite (all) | `_fm_overwrite_dialog`, `_ask_overwrite` |
| 33 | Close during build | app | "Close anyway" / "Keep building" | `_on_close` :3968 |

---

## 5. External dependencies and detection

| Dependency | Used for | Detection / handling | Where |
|---|---|---|---|
| Administrator rights | everything (OSFMount, format, FSCTL, robocopy into mounts) | `IsUserAnAdmin`; if not, `ShellExecuteW('runas', sys.executable, script + args)` and exit (only when run as `__main__`); PS1 `Test-Admin` throws; window title shows `[Administrator]` | :916, :922, :947 |
| OSFMount (PassMark) | exFAT build, verify, extract, edit, convert | `_find_osfmount` :8695: settings path (file or folder; a lone `OSFMount.exe` is deferred because it may be the installer, #86) → registry `HKLM/HKLM WOW6432Node/HKCU SOFTWARE\PassMark\OSFMount` and `…\Uninstall\OSFMount_is1` (`InstallLocation`/`Path`/`InstallDir`) → Program Files/x86/W6432 `OSFMount` and `PassMark\OSFMount` → PATH; prefers `osfmount.com`. Banner if missing (:7707). Version shown via `GetFileVersionInfoW` and PE arch (`tab_settings.py:365`, `:392`). No minimum version. | |
| PowerShell 5.1 | build script; toast; HW detect | full path `%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe` in the bat; bare `powershell` for toast/HW detect | bat, :26202, `tab_advanced.py:83` |
| cmd.exe, format.com, robocopy.exe, mountvol, shutdown, rundll32, fsutil (Lazy only), tasklist (updater) | build/convert/extract/dismount/power | assumed present (Windows built-ins); robocopy `/COMPRESS` only for UNC sources | |
| .NET 8+ runtime (x64) | UFS2Tool | Reactive: `dotnet_runtime_hint` on failure; Check button runs `dotnet --list-runtimes` / `dotnet --version`, accepts `Microsoft.NETCore.App ≥ 8` (`dotnet_runtime_line_ok` :3005); runtimeconfig `rollForward: LatestMajor` (works on .NET 10 per STATUS) | :7992 |
| UFS2Tool 4.0.0 (embedded, BSD-2) + DokanNet 2.0.1 (embedded, MIT) | ffpkg build/extract/edit/convert | extracted on demand, byte-checked before use | :852, :563 |
| Dokany v2 driver | `mount_udf` (ffpkg extract/convert) | `_find_dokan_driver` :5319: `System32\drivers\dokan2.sys` (or dokan1/dokan.sys) **and** `System32\dokan2.dll` (or loadable by name); else offer the Dokany releases page | |
| MkPFS 0.0.9 (pip, GPL-3.0; needs `zlib-ng`, `cryptography`) | stock PFS pack/unpack/verify/tree | import probe; capability probe `mkpfs.exfat.open_exfat` | `ui/mkpfs_runner.py` |
| Lazy_MkPFS (vendored, GPL-3.0; needs `cryptography`, optional `isal`/`zlib-ng`) | default `.ffpfsc` convert | bundled/dev/override resolution | `ui/experimental_mkpfs.py` |
| psutil | child-process kill, diagnostics, disk I/O | import at use; kill is skipped without it | :2760 |
| Pillow | covers (formats limited to PNG/JPEG/WEBP/BMP/DDS) | required | :717 |
| tkinterdnd2 | drag and drop | optional (`_DND_AVAILABLE`) | :26 |
| tkinterweb, capstone | help renderer, audio-fix disassembly | optional, not bundled | DEPENDENCIES §1c |
| GitHub API / releases | update check and self-update | `api.github.com/repos/kerrdec97/ps5-exfat-builder/releases/latest` (5 s check, 10/30 s download timeouts), `<exe>.sha256` required | :26312, :26353 |
| Discord webhook | notifications | URL from settings (no UI) | :26210 |
| PS5 services (FTP 2121/2122, klog 3232, payload 9090/9021, DPI 12800) | PS5 features | auto-detect scans `x.x.x.1–254` on 2121/2122 (0.3 s, 50 threads) | :26235 |
| Sony Publishing Tools | FPKG | → FPKG_PARITY.md | |
| 7-Zip / unrar / WinRAR | Backport archives | → BACKPORT_PARITY.md | |
| WMI / Storage cmdlets, `wmic` | HW recommendation (Advanced), PS1 media detection (never reached) | `Get-PhysicalDisk` → `wmic diskdrive` fallback, 5 s / 3 s timeouts | `tab_advanced.py:66` |

---

## 6. Migration table

Suggested modules follow `native/docs/ARCHITECTURE.md` (`src/core`, `src/jobs`, `src/engines/{general,gameinfo,exfat,pfs,ufs2,fpkg,backport}`, `src/connect/ps5`, `src/app`). Line numbers are `exfat_builder.py` unless a file is named.

### Build (54)

| # | Feature | Legacy location | Notes | Suggested C++ module |
|---|---|---|---|---|
| 1 | Unified Build page orchestration | `ui/tab_unibuild.py:106` | type tiles, source, output, summary, queue | `app/pages/BuildPage` + `jobs/JobQueue` |
| 2 | Source browse + game-root resolution | `tab_unibuild.py:209`, `resolve_game_root_deep` :349 | limits depth 5 / 4,000 dirs; chooser | `engines/gameinfo/GameRoot` |
| 3 | Metadata detection (sfo/json/pfs-version/nptitle) | `get_game_info` :442 | priority order §1.0 | `engines/gameinfo/ParamJson`, `ParamSfo` |
| 4 | Title-id fallback from folder name | `tab_unibuild.py:1571` | regex PPSA/CUSA/PPLH | `engines/gameinfo` |
| 5 | Required firmware detection | `_abp_read_required_fw` :14723 | for `%FIRMWARE%` | `engines/gameinfo` |
| 6 | Cover art lookup + normalisation cache | `_load_cover_art` :10645, `_normalize_cover` :721 | `sce_sys/icon0.png`, ≤4 levels, skip `decrypted`/`fakelib` | `engines/gameinfo/Covers` |
| 7 | Filename sanitiser | `sanitize_filename` :275 | 80-char cap, cmd metachars | `engines/gameinfo/Naming` |
| 8 | Name presets + custom template | `tab_unibuild.py:1318` | tokens, bracket cleanup | `engines/gameinfo/Naming` |
| 9 | Legacy name format | `build_exfat_name` :509 | queue loader/library | `engines/gameinfo/Naming` |
| 10 | Output collision handling | `tab_unibuild.py:2177` | overwrite / numbered / skip | `jobs` + `core/FileUtil` |
| 11 | Output-inside-dump refusal | `tab_unibuild.py:2130`, `_run_queue` :11969 | | `jobs/Job::validate` |
| 12 | Space estimate per type | `tab_unibuild.py:1418` | multipliers §1.0 | `core/FileUtil::ensureFreeSpace` + engines |
| 13 | Per-drive space check at Build All | `tab_unibuild.py:3309` | sum need_out + max need_tmp | `jobs/JobQueue` |
| 14 | AMPR index generation before build | `tab_unibuild.py:2329` | writes into dump (decide: keep or stage) | `engines/gameinfo/AmprIndex` |
| 15 | AMPR index FTP build/upload | `ui/build_ampr_index.py:418`, `tab_advanced` AMPR pane | | `engines/gameinfo/AmprIndex` + `connect/ps5` |
| 16 | exFAT pre-build checklist | `_run_queue` :11930 | write test, drive root, space | `engines/exfat/ExfatBuildJob::validate` |
| 17 | exFAT Python pre-flight scan | `_preflight_source` :26978 | 500k files, 0 files, 1.05×+2 GiB | `engines/exfat` |
| 18 | Previous-image set-aside/restore | :2789, :2804 | `.previous`, `.failed` | `core/FileUtil` (`PartFile`) |
| 19 | Embedded bat + PS1 extraction & integrity | :529, :552 | replaced by native code or signed resources | `engines/exfat` (or drop) |
| 20 | exFAT cluster auto rule | PS1 `Get-OptimalExfatClusterSizeFromStats` | 64K if avg ≥ 1 MiB else 32K | `engines/exfat/ExfatLayout` |
| 21 | exFAT image-size formula | PS1 `Get-OptimalImageSizeBytesFromStats` | spare 1–8 GiB, min raw+1 GiB | `engines/exfat/ExfatLayout` |
| 22 | Image size override (global / per item / per job) | :10947 | unify semantics | `engines/exfat` setting + per-job override |
| 23 | Free-space pre-flight (+256 MiB) | PS1 | | `core/FileUtil` |
| 24 | OSFMount attach with size-scaled timeout | PS1 `Invoke-OsfHidden` | `-o rw,rem` | `engines/exfat/OsfMount` (if OSFMount path kept) |
| 25 | format.com exFAT with label OSFIMG | PS1 `Invoke-FormatVolume` | `/X /Y /Q` | `engines/exfat/OsfMount` or native writer |
| 26 | robocopy copy with adaptive flags | PS1 | `/J`, `/MT`, `/XF`, `/XA:H`, `/COMPRESS`, retry w/o `/J` | `engines/exfat/RobocopyCopier` or native copy |
| 27 | Post-copy count `[COPY-STATS]` | PS1 `Get-TreeFileStats` | locale-proof | `engines/exfat` |
| 28 | Volume used ≥ 90 % check | PS1 | | `engines/exfat` |
| 29 | Volume flush before dismount | PS1 `VolFlush` | | `engines/exfat` |
| 30 | Robust dismount chain | `_dismount_drive_robust` :9601 | 5 strategies | `engines/exfat/OsfMount` |
| 31 | Build output parser (robocopy/PS1) | `_handle_line` :10202 | locale-proof parser | `engines/exfat` (if robocopy kept) |
| 32 | Robocopy localized summary parser | `RobocopySummaryParser` :2533 | tests exist | `engines/exfat` |
| 33 | Live telemetry (drive poll, dest walk, current file) | :25302-25630 | | `core/Progress` |
| 34 | Empty-output guard | `_build_output_is_empty` :10348 | structured evidence only | `engines/exfat/ExfatVerify` |
| 35 | Auto-retry exFAT builds | `_item_done` :10530 | `retry_count` | `jobs/JobQueue` retry policy |
| 36 | Failure excerpt for dialogs | `_extract_failure_lines` :10567 | | `jobs/JobOutcome` |
| 37 | Build summary.json | `_write_build_summary` :12137 | partial SHA-256 | `engines/exfat` |
| 38 | UFS2 newfs build with `.part` | `_ffpkg_process_next` :8350, `finalize_ffpkg_part` :2646 | params §2.7 | `engines/ufs2/Ufs2BuildJob` |
| 39 | UFS2 progress parser | `_ffpkg_handle_line` :8155 | 3 stages | `engines/ufs2` |
| 40 | .NET runtime hint | `dotnet_runtime_hint` :3012 | | `engines/ufs2` dependency probe |
| 41 | UFS2Tool runtimeconfig roll-forward | `extract_ufs2tool` :852 | | `engines/ufs2` |
| 42 | PFS job via intermediate | `tab_unibuild.py:2552` | | `engines/pfs/PfsFromFolderJob` |
| 43 | Intermediate location choice | `_choose_intermediate_dir` `tab_unibuild.py:2431` | different drive, 1.3×, hard links for ffpkg | `engines/pfs` |
| 44 | PFS from existing image + non-ASCII staging | `tab_unibuild.py:2766` | link or copy | `engines/pfs/PfsFromImageJob` + `core/FsLinks` |
| 45 | Convert engine (Lazy → stock fallback) | `_pipeline_convert_then_next` :24375 | replace by native PFS writer | `engines/pfs/PfsWriter` |
| 46 | Stock-mkpfs worker memory cap | `_pfs_pack_tuning` :2471 | native writer: bounded in-flight blocks | `engines/pfs` |
| 47 | Temp-drive redirect + final move | :24611-24680, `_move_final` :24193 | 2.2× free | `engines/pfs` + `core/FileUtil` |
| 48 | PFS pre-flight (FAT32 refuse, 1.5× space, risky FS warn) | `_pfs_preflight_check` :2360 | | `engines/pfs` |
| 49 | Single-core retry on failure | :24890 | likely unnecessary natively | `engines/pfs` |
| 50 | "Suspiciously small" output check | :25004 | apply to every backend | `engines/pfs/PfsVerify` |
| 51 | EINVAL "disk rejected large write" message | :25052 | | `engines/pfs` |
| 52 | Legacy PFS two-step (.ffpfs/.ffpfsc) | `ui/tab_pfs.py:809` | `pfs_image.dat` name, `.pfskeep` | `engines/pfs/PfsFolderJob` |
| 53 | Stray-image refusal in dump | `ui/tab_pfs.py:739` | | `engines/pfs` validate |
| 54 | Lazy_MkPFS override folder | `ui/experimental_mkpfs.py:113` | security: drop or sandbox | `engines/pfs` (decision) |

### Verify (13)

| # | Feature | Legacy location | Notes | Suggested C++ module |
|---|---|---|---|---|
| 55 | Tri-state verdict + content-verified marker | :23551 | PASS/FAIL/INCONCLUSIVE | `jobs/JobOutcome::verification` |
| 56 | exFAT file-level sanity | `_verify_image_file_only` :23352 | | `engines/exfat/ExfatVerify` |
| 57 | Native verify via mkpfs (`--source-dir` rules) | `_verify_image_mkpfs_native` :23408 | native: own exFAT reader | `engines/exfat/ExfatReader` |
| 58 | Mount-walk verify with fresh letters | `_verify_image` :23561 | 4 attempts, 90 s | `engines/exfat/ExfatVerify` |
| 59 | Tree walk with build exclusions | `verify_walk_tree` :2580 | `decrypted`, image exts, hidden | `core/FileUtil::walk` |
| 60 | Strict per-file comparison | `image_matches_source` :2631 | audit H1 | `engines/exfat/ExfatVerify` |
| 61 | Missing-file report | `describe_missing_files` :2864 | top 15 by size | `jobs/JobOutcome` |
| 62 | Corrupt-size check | `_verify_done` :23930 | < 5 % of source | `engines/exfat` |
| 63 | Optional full SHA-256 | :24137 | log only | `engines/general` (hash service) |
| 64 | mkpfs verify verdict classification | `classify_verify_result` `ui/mkpfs_runner.py:81` | TOOL_ERROR vs FAIL | `engines/pfs/PfsVerify` |
| 65 | PFS structure validation | Lazy `validate_image_quick` `build.py:1082` | | `engines/pfs/PfsReader` |
| 66 | ffpkg delete-source size rule | `ffpkg_safe_to_delete_source` :2675 | interim until UFS2 reader | `engines/ufs2` |
| 67 | PFS full verify (`--verify`) option | `ui/tab_pfs.py:1703` | payload hash vs source | `engines/pfs/PfsVerify` |

### Extract (10)

| # | Feature | Legacy location | Notes | Suggested C++ module |
|---|---|---|---|---|
| 68 | Format routing by extension | `ui/tab_extract.py:1046` | | `app/pages/ExtractPage` |
| 69 | exFAT extract (OSFMount ro + robocopy) | `_run_extract` :11662 | `/MT` option, rc ≥ 8 fail | `engines/exfat/ExfatExtractJob` (native reader preferred) |
| 70 | ffpkg extract via Dokan mount + robocopy | `_ffpkg_mount_copy_extract` :5607 | ≥ 4 GiB refusal, 1.03× space, cancel | `engines/ufs2/Ufs2ExtractJob` |
| 71 | ffpkg whole-image `UFS2Tool extract` | `_spawn_ufs2_extract` `tab_extract.py:1159`, `_run_ffpkg_extract` :5904 | 2 GiB RAM cap | `engines/ufs2` |
| 72 | Dokany v2 detection | `_find_dokan_driver` :5319 | | `engines/ufs2` dependency probe |
| 73 | ffpkg mount as drive (browse) | `_run_ffpkg_mount` :5374, unmount :5526, unmount on exit :5586 | | `engines/ufs2` |
| 74 | PFS unpack (+ `--deep`, pre-scan, fallback) | `_run_pfs_extract_tab` `tab_extract.py:1286` | | `engines/pfs/PfsExtractJob` |
| 75 | Nested-image detection after unpack | `tab_extract.py:1551` | offer chaining | `engines/pfs` |
| 76 | Destination-size progress poller | `tab_extract.py:1465` | | `core/Progress` |
| 77 | Legacy PFS tab extract | `ui/tab_pfs.py:2121` | duplicate | (merge into 74) |

### Edit (12)

| # | Feature | Legacy location | Notes | Suggested C++ module |
|---|---|---|---|---|
| 78 | Edit router by format | `ui/tab_edit.py:71`, `_edit_open` :149 | | `app/pages/EditPage` |
| 79 | exFAT rw mount + browse | `_fm_mount` :9149, `_fm_refresh` :9251 | | `engines/exfat/ExfatEditor` (native writer/rw reader or OSFMount) |
| 80 | exFAT add file/folder with space check | `_fm_add_file` :8872, `_fm_add_folder` :8928, `image_space_shortfall` :2969 | issue #33 | `engines/exfat/ExfatEditor` |
| 81 | exFAT delete / rename / new folder / replace | :9037, :9220, :9318 | | `engines/exfat/ExfatEditor` |
| 82 | exFAT extract selected | `ui/tab_files.py:648` | | `engines/exfat` |
| 83 | exFAT apply backport overlay + backup zip | `ui/tab_files.py:697` | → BACKPORT_PARITY | `engines/backport` |
| 84 | ffpkg in-place verbs (ls/add/replace/delete/rename/stat/extract/chmod) | `ui/tab_ffpkg_edit.py:793-1843` | | `engines/ufs2/Ufs2Editor` |
| 85 | ffpkg mkdir via `.keep` | `_new_folder` `tab_ffpkg_edit.py:1668` | | `engines/ufs2/Ufs2Editor` |
| 86 | ffpkg hex preview | `_fetch_hex` `tab_ffpkg_edit.py:1826` | | `app` + `engines/ufs2` |
| 87 | ffpkg rebuild + atomic swap | `tab_ffpkg_edit.py:2043`, `_bp_atomic_swap` :3376 | | `engines/ufs2` + `core/FileUtil` |
| 88 | ffpkg compare two images | `_compare_ffpkgs` `tab_ffpkg_edit.py:3433` | report file | `engines/ufs2` |
| 89 | PFS workspace (unpack → edit → rebuild via Build) | `_build_pfs_workspace` `tab_edit.py:187` | | `engines/pfs` + `app/pages/EditPage` |

### Convert (5)

| # | Feature | Legacy location | Notes | Suggested C++ module |
|---|---|---|---|---|
| 90 | exFAT → ffpkg | `ui/tab_convert.py:469` | fix rw mount, `.part`, Advanced params | `engines/ufs2/ConvertExfatToUfs2Job` |
| 91 | ffpkg → exFAT | `ui/tab_convert.py:847` | 1.10× size, 64 MiB align; add verify | `engines/exfat/ConvertUfs2ToExfatJob` |
| 92 | Image → .ffpfsc (legacy Convert + queue) | `ui/tab_pfs.py:1448`, `:1912` | C1 rule | `engines/pfs/PfsFromImageJob` |
| 93 | "Convert to .ffpfsc?" hand-off | :10904, :8388, `ui/tab_pfs.py:1325` | | `app` |
| 94 | Legacy pipeline mode (`pipeline_to_pfs`) | `_verify_done_final` :24161, `_ffpkg_item_done` :8283 | superseded by PFS job type | (drop; map setting) |

### Queue (14)

| # | Feature | Legacy location | Notes | Suggested C++ module |
|---|---|---|---|---|
| 95 | Unified job list (add/remove/move/clear/select) | `tab_unibuild.py:2111-2290` | queued-only edits | `jobs/JobQueue` |
| 96 | Job dispatch by type | `_uq_dispatch` `tab_unibuild.py:3009` | | `jobs/JobFactory` |
| 97 | Job states + finish summary | `_uq_done` :3115, `_uq_finish` :3187 | | `jobs/JobQueue`, `jobs/JobHistory` |
| 98 | Failure cleanup rules | `_uq_cleanup_job` :3045 | pre-existing check | `jobs` + `core/FileUtil` |
| 99 | Cancel semantics | `_uq_cancel` :1007 | native: real kill | `core/Cancellation` |
| 100 | Re-queue failed/skipped on Build All | `tab_unibuild.py:3354` | | `jobs/JobQueue` |
| 101 | Legacy exFAT queue + statuses | `QueueItem` :3048, `_render_queue` :11496 | | (map to jobs) |
| 102 | Legacy queue save/load JSON | `_queue_save` :13088, `_queue_load` :13113 | native: persistent queue | `jobs/JobQueue` persistence |
| 103 | Pause after current | :12780, :7741 | fix (failure path only) | `jobs/JobQueue` |
| 104 | Per-item size override | :11293 | | per-job override |
| 105 | ETA per game and queue | `_update_queue_total_eta` :23316, `_format_eta` :9867 | | `core/Progress` |
| 106 | End-of-queue outcome summary | `summarize_outcomes` :2701 | | `jobs/JobQueue` |
| 107 | Library Build All → queue | `_lib_build_all` :13144 | | `app/pages/LibraryPage` |
| 108 | Requeue from path (Dump Rename/Library) | `_add_to_queue_from_path` :23279 | switches to hidden tab (UNVERIFIED UI) | `app` |

### Dump rename (5)

| # | Feature | Legacy location | Notes | Suggested C++ module |
|---|---|---|---|---|
| 109 | Scan source folder for dumps | `_dumps_scan` :23078, `_dumps_find_eboot_root` :22993 | eboot at root or one child; ID-named folders | `engines/gameinfo/DumpScanner` |
| 110 | Proposed name per preset | `_dumps_proposed_name` :23012 | | `engines/gameinfo/Naming` |
| 111 | Rename / move with no overwrite | `_dumps_apply` :23209 | skip if target exists; rescan after | `engines/gameinfo/DumpRenameJob` |
| 112 | Confidence/status per row | `ui/dump_rename.py:264-290` | | `app/pages/DumpRenamePage` |
| 113 | Async cover per row | `ui/dump_rename.py:692` | | `app` + `engines/gameinfo/Covers` |

### Settings (8)

| # | Feature | Legacy location | Notes | Suggested C++ module |
|---|---|---|---|---|
| 114 | Settings file load/save/migrate | :110-175 | atomic, `.corrupt` | `core/Settings` (+ `addLegacyKey` import) |
| 115 | Legacy settings import (108 keys) | §4.2 | map or drop each key | `core/Settings` legacy map |
| 116 | Settings page sections | `ui/tab_settings.py:246` | OSFMount, Build, Logs, FTP, Notifications, Theme, Auto-Shutdown | `app/pages/SettingsPage` (schema-generated) |
| 117 | Advanced page (params, post-build, language stripper, AMPR, DLC-emu) | `ui/tab_advanced.py:162` | reset all, save, HW recommendation | `app/pages/AdvancedPage` + engines |
| 118 | Reset to defaults (Advanced) | `tab_advanced.py:2603`, :20067, :20111 | | `core/Settings` |
| 119 | HW-based recommendation | `tab_advanced.py:66` | Get-PhysicalDisk/wmic | `core/Dependencies` or `engines/general` |
| 120 | Window geometry restore (on-screen clamp) | :4280, :4507 | | `app/MainWindow` |
| 121 | Last tab / sidebar / sub-tab restore | :4903, :4582 | | `app` |

### Logging (8)

| # | Feature | Legacy location | Notes | Suggested C++ module |
|---|---|---|---|---|
| 122 | Session log + latest.log + banner | `_init_session_log` :27170 | | `core/Log` |
| 123 | Log pruning (30 days / 50 files) | `_prune_old_logs` :27234 | | `core/Log` |
| 124 | Per-build log export (`_FAILED`) | `_export_log` :25132 | | `core/Log` / `jobs` |
| 125 | Thread-safe log pump + tagging | `_ensure_log_pump` :27138, `_classify_log_tag` :27272 | | `core/Log` + `app/models/LogModel` |
| 126 | Logs folder: browse/open/count/clear | :25089-25132 | | `app/pages/SettingsPage` |
| 127 | Crash handler + clipboard report | `_crash_handler` :27510 | | `app/main.cpp` |
| 128 | Bug report file with redaction | `create_bug_report_file` :3460, `_redact_settings` :3439 | | `app` + `core/Log` |
| 129 | Unified progress model (bytes first, never fabricate) | `ui/progress_state.py:181` | | `core/Progress` |

### Temp / cleanup (8)

| # | Feature | Legacy location | Notes | Suggested C++ module |
|---|---|---|---|---|
| 130 | Startup sweep of own temp folders > 24 h | `sweep_old_temp_dirs` :577 | | `core/Paths` |
| 131 | Clear Temp button (> 24 h) | `_clear_temp` :7763, `clear_temp_folder` :884 | also check other running jobs | `app/pages/SettingsPage` + `core/Paths` |
| 132 | Temp size display | `_refresh_temp_size` :11202 | off UI thread | `core/Paths` |
| 133 | Child process kill on close | `kill_child_processes` :2760, `_on_close` :3968 | job objects | `core/ProcessRunner` |
| 134 | Stale OSFMount sweep (own mounts only) | :9794, :2846 | | `engines/exfat/OsfMount` |
| 135 | Force-dismount picker (system drive hidden) | `_force_dismount_all` :9347 | | `app` + `engines/exfat` |
| 136 | Delete source off the UI thread | `_rmtree_in_background` :23539 | | `core/FileUtil` |
| 137 | Removal of run-created staging (`.pfs_stage*`, `.mkpfs_tmp`, `.pfs_convert_work`, `pfs_image.dat`, `.pfskeep`) | `tab_unibuild.py:2825-2831`, `ui/tab_pfs.py:1062`, `:1870` | native: private `ctx.tempDir` | `jobs` temp dir |

### Updater (4)

| # | Feature | Legacy location | Notes | Suggested C++ module |
|---|---|---|---|---|
| 138 | Update check on boot (GitHub latest) | `_check_for_updates` :26312, `_parse_version_tuple` :26287 | | `core/Updater` (new) |
| 139 | Update notice dialog | `_show_update_notice` :26336 | | `app` |
| 140 | Self-update (sha256 required, size check, timeouts, refuse while building) | `_auto_update` :26353 | | `core/Updater` |
| 141 | Updater batch (wait PID, backup, retry move ×8, relaunch, verify, rollback, cleanup) | :26549-26690, `updater_environment` :2952 | native: separate updater exe | `packaging/` updater |

### Misc (32)

| # | Feature | Legacy location | Notes | Suggested C++ module |
|---|---|---|---|---|
| 142 | Admin check + runas relaunch | :916-950 | native: manifest `requireAdministrator` (decision) | `packaging/` |
| 143 | OSFMount discovery + banner + version | `_find_osfmount` :8695, :7707, `tab_settings.py:365` | | `core/Dependencies` |
| 144 | .NET check dialog | `_ffpkg_check_dotnet` :7992 | | `core/Dependencies` |
| 145 | MkPFS availability/capability banner | `ui/mkpfs_runner.py:123-210` | native writer removes the need | `core/Dependencies` |
| 146 | Network-mapped drive letters | `ui/shared/drives.py` | | `core/FileUtil` (drive letters) |
| 147 | Volume filesystem / cloud path detection | :2278, :2331 | | `core/FileUtil` |
| 148 | Hard-link capability probe | `tab_unibuild.py:2693`, `tab_pfs.py:204` | | `core/FsLinks` |
| 149 | Shutdown/restart/sleep after queue with countdown | :20147-20336 | | `core/Power` |
| 150 | Completion sound + balloon + Discord | `_notify` :26177, :26210 | | `app` notifications |
| 151 | Keyboard shortcuts (Ctrl+B, Esc, Ctrl+Q, Ctrl+Shift+T, F5) | :4094-4147 | | `app/MainWindow` |
| 152 | Release-notes modal + support nudges | `ui/release_notes.py` | | `app` |
| 153 | Close confirmation during build / support popup | `_on_close` :3968, `_show_support_popup` :3882 | | `app/MainWindow` |
| 154 | Translations (16 languages) | `_TRANSLATIONS` :977, `ui/tab_language.py` | export only; import stub | `app` (Qt Linguist) |
| 155 | History page (stats, filter, CSV, clear) | `ui/tab_history.py`, :20394 | exFAT only today; native: all job types | `jobs/JobHistory` + `app` |
| 156 | Library: source dumps (folders, scan, filter, cards, dedupe) | :21950-22403, `ui/tab_library.py` | | `app/pages/LibraryPage` + `engines/gameinfo` |
| 157 | Library: built images (scan dirs, filters, batch FTP upload) | :12820-13072, `ui/tab_images.py` | `_LIBRARY_IMAGE_EXTS` | `app/pages/LibraryPage` |
| 158 | Library quick add (exFAT / ffpkg) | `_lib_quick_add` :22853, :22890 | | `app` |
| 159 | Language Stripper (single/batch) | `_ls_*` :19392-19911, `tab_advanced` lang pane | | `engines/gameinfo` or new module |
| 160 | DLC-emu INI editor | `ui/dlc_emu_ini.py`, Advanced DLC pane | | → FPKG_PARITY/BACKPORT (owner TBD) |
| 161 | Audio-fix scan/patch | `ui/audio_*` | | → BACKPORT_PARITY (owner TBD) |
| 162 | FTP upload of an image (resume/progress/cancel) | `_ftp_upload_file` :26025, `_ftp_upload_to` :25917 | | `connect/ps5/Ftp` |
| 163 | FTP tab (dual pane, transfer queue, diagnostics, speed test) | `ui/tab_ftp.py`, :6161-7520, `ui/tq_model.py` | not inventoried in depth here | `connect/ps5` |
| 164 | PS5 FTP browser (cut/paste/rename/delete, protected paths) | `_br_*` :7046-7520 | | `connect/ps5` |
| 165 | PS5 auto-detect IP | `_auto_detect_ip` :26235 | | `connect/ps5` |
| 166 | PS5 status-bar ping | :26837-26897 | uses `ps5_ip` fallback | `connect/ps5` |
| 167 | Installed games / PS5 storage | `_show_installed_games` :25152 | | `connect/ps5` |
| 168 | Payload sender + list | `ui/tab_payloads.py`, :18478-18816 | | `connect/ps5` |
| 169 | Autoloader editor | :18816-19033 | | `connect/ps5` |
| 170 | Kernel log viewer (filter, wrap, export) | `ui/tab_klog.py`, :19033-19246 | | `connect/ps5` |
| 171 | Y2JB (connection, install pkg, patch, files, autoloader, app.db patch) | `ui/tab_y2jb.py`, :17998-21820 | | `connect/ps5` |
| 172 | ShadowMount+ / MicroMount config + payload | `ui/tab_shadowmount.py`, `ui/tab_micromount.py` | | `connect/ps5` |
| 173 | PS5 Manager overview + recent activity | `ui/tab_ps5_mgr.py`, :12061-12102, :13235 | | `connect/ps5` + `app` |

Total features: **173** (Build 54, Verify 13, Extract 10, Edit 12, Convert 5, Queue 14, Dump rename 5, Settings 8, Logging 8, Temp/cleanup 8, Updater 4, Misc 32). FPKG and Backport engine features are excluded (see their parity documents).

---

## 7. Findings for the port (not in TECHNICAL_AUDIT.md unless noted)

| # | Finding | Evidence | Suggested decision |
|---|---|---|---|
| F1 | Sector-size setting has no effect | PS1 passes nothing to OSFMount or format.com | Drop the setting or implement it in the native writer |
| F2 | "Auto (64K)" cluster label is wrong for small-file games | PS1 auto = 32K when average file < 1 MiB | Document the real rule or always use 64K |
| F3 | Global image-size override ignored by Build-tab jobs | `_dispatch_exfat` passes `'auto'` | One rule: per-job override, else auto |
| F4 | Delete-source toggles not persisted by their own control | Danger-zone rows save via `_save_adv_params` | Native schema persists every control |
| F5 | Dead/orphan keys | `adv_sector`, `adv_robo_log*`, `adv_write_benchmark`, `auto_ip`, `experimental_mkpfs_enabled` dead; `discord_webhook`, `lib_auto_scan`, `pfs_lazy_cpu_count`, `ps5_ip`, `lowercase_names`, `abp_output`, `adv_lang_mode` orphan | Map only functional keys; give orphans a UI or drop |
| F6 | Legacy "Pause after current" works only after a failure | `_check_pause_then_next` called only from the failure path | Implement pause in `JobQueue` |
| F7 | Lazy truncates output names to 60 chars → silent fallback + stray file | `pack_file.sanitize_output_path` | Native writer: no truncation; long-name test |
| F8 | Lazy output gets no post-convert check from the app | Lazy `RESULT_OK` returns immediately | Same verification for every backend |
| F9 | Convert exFAT→ffpkg mounts the source **rw**, ignores Advanced UFS2 params, no `.part`, leaves partial output | `ui/tab_convert.py:585`, `:629` | ro mount, PartFile, shared params |
| F10 | ffpkg→exFAT temp under system temp with non-app prefix | `mkdtemp('ffpkg_to_exfat_')` | private job temp |
| F11 | exFAT Extract robocopy decoded as UTF-8 (audit M10 residual), `/DCOPY:DA` | `exfat_builder.py:11808`, `:11884` | native copy, no robocopy |
| F12 | Source dump modified: `ampr_emu.index` written into the dump; `.pfskeep` markers; `.mkpfs_tmp` beside source images | §1.0, §1.5, §1.4 | Decide explicitly; prefer injecting AMPR index/markers into the image instead of the source |
| F13 | Per-job `temp` captured but not used by exFAT/ffpkg/PFS | `tab_unibuild.py:2212` vs `_choose_intermediate_dir` | Immutable per-job settings snapshot |
| F14 | `pfs_backend` drives four behaviours (convert backend, native verify, deep extract, label) | §4.2 | Split into separate settings or remove with the native engine |
| F15 | PFS tab `version_ps5` checkbox ignored | `--version PS5` hard-coded | Drop |
| F16 | Crash log ignores `logs_dir` | `_crash_handler` | Use `core/Log` paths |
| F17 | Vendored Lazy signed-image path cannot run (wrong arity) | `build.py` → `crypto.read_image_bytes` | Do not port signing without vectors |
| F18 | MkPFS 0.0.9 already contains a pure exFAT writer and reader | `mkpfs/exfat_writer.py`, `mkpfs/exfat.py` (GPL-3.0) | Reference for an OSFMount-free `engines/exfat` (validate on PS5) |
| F19 | `_add_to_queue_from_path` shows the hidden legacy Build frame while adding to the unified queue | `_switch_tab('build')` + `_uq_load_folder_and_add` | UNVERIFIED UI effect; native has one queue |
| F20 | History records exFAT builds only | `_save_history` callers | Native history for every job type |
| F21 | Open audit items: M3 (detection race), M4 (`_run_queue`, `_choose_intermediate_dir` walks), M8 (duplicate pipelines), L3, L6 | `docs/farewell/STATUS.md` | Native design must close them |

## 8. Could not verify

* Any Windows runtime behaviour: OSFMount, format.com prompts in other locales, robocopy exit codes on real games, UFS2Tool output encoding, Dokany, PS5 mounting of `.ffpfsc` built by Lazy vs. MkPFS, ISA-L streams on a PS5.
* Whether the hidden legacy exFAT/ffpkg/PFS tabs are reachable in ways other than the hand-offs listed (Library Build All, "Convert to .ffpfsc?", `_switch_tab('build'|'pfs')`).
* MkPFS 0.0.9 internals beyond the CLI flags and the functions quoted (the wheel was read, not executed).
* Determinism of PFSC output across zlib / zlib-ng / ISA-L versions.
