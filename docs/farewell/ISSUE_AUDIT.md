# Issue audit: exFAT Image Builder Farewell Edition and PS5 Backport GUI

Audit date: 2026-11-03. Read-only: nothing was commented, labelled, closed or opened on GitHub.

- **kerrdec97/ps5-exfat-builder**: all 84 issues were read (#1–#86; #57 and #83 are pull requests), including the bodies and all 198 comments. 30 are open and 54 closed. The code audited is `farewell-edition-dev` at **f151289**. Two commits landed during the audit (`d77448a` Windows CI and `db14125` backport OS-metadata filter), so line numbers below are for **db14125**. Neither commit changes any finding.
- **kerrdec97/ps5-backport-gui**: all 29 issues and 115 comments were read. **5 are open** (#2, #4, #6, #20, #28), not 6 as the brief said; the other 24 are closed. The source is closed, so the status for each issue comes from the owner's comments and the CHANGELOG. Each entry says whether it matters for the main app's backport engine.
- **Attachments could not be fetched.** The proxy blocks user-attachments downloads and pastebin, so I could not read the #86 report, the #80 and #64 logs, the #81 pastebin, or the Backport GUI ZIPs. Findings rest on log text quoted in issue bodies and comments.
- **Tests run on Linux.** I copied the repository and ran its 51 tests with `python3.12 -m unittest`: all pass, 1 skipped. I also fed the robocopy summary parser summaries in 13 languages (`scripts/robo_locales.py`, results in §4).
- **Embedded scripts were checked.** I decoded and read the embedded `New-OsfExfatImage.ps1`, `make_image.bat`, `Backport.py` and `ps5_sdk_version_patcher.py`.

Status key: **Fixed** (fixed in the code, not yet run on Windows) · **Partial** · **Needs info** · **Feature (open)** · **Implemented** (the requested feature exists) · **Limitation** (third-party limit) · **Not a defect** (antivirus, support question, user setup) · **Historic** (fixed in an earlier release, closed).

---

## 1. Summary table: ps5-exfat-builder (84)

| # | State | Title (short) | Status in f151289/db14125 | Repro |
|---|---|---|---|---|
| 86 | open | VERIFY "Mounted drive did not appear"; image deleted on closing prompt; OSFMount install prompts | **Fixed** (deletion, 90 s wait); OSFMount prompt **likely fixed** | Windows + OSFMount |
| 85 | open | pfs build OSError Errno 22 (Lazy_MkPFS) | **Fixed** | Windows, non-UTF-8 code page |
| 84 | open | Stuck on scan | **Needs info** | Windows |
| 82 | open | Stuck on "Creating & mounting image" | **Partial** (bounded and explained; stale mounts not swept) | Windows + wedged OSFMount |
| 81 | open | Verification failed, image missing data | **Partial / needs info** | Windows + OSFMount |
| 80 | open | French locale: summary not parsed, image deleted | **Fixed** | Linux (parser); Windows end-to-end |
| 79 | open | 4.0.2 deletes successful image (FR) + TypeError | **Fixed** | Linux (parser); Windows |
| 78 | open | Download flagged as trojan | **Not a defect** (mitigated) | Windows Defender |
| 77 | open | Unhandled UI error converting to exFAT | **Partial** (crash fixed; underlying preflight error unknown) | Windows |
| 76 | open | Error converting folder (IT locale) | **Fixed** | Linux (parser); Windows |
| 75 | open | Repo code not synced with release | **Fixed** (source restored on farewell branch) | n/a |
| 74 | open | 0 files copied after successful copy | **Fixed** | Linux (parser); Windows |
| 73 | open | Ghost of Yōtei convert error | **Fixed** | Linux (parser); Windows |
| 72 | open | zh-CN: builds rejected and deleted | **Fixed** | Linux (parser); Windows |
| 71 | open | exFAT unpacking hangs after mount | **Partial** (bounded mount, progress; no skip option) | Windows + OSFMount |
| 70 | closed | newfs failed (.NET missing) | **Fixed** (hint) | Windows without .NET 8 |
| 69 | open | ffpkg→pfs OSError Errno 22 | **Fixed** (same as #85) | Windows |
| 68 | open | Games "divided into two identical ones" | **Needs info** | ? |
| 67 | open | Compressed backup dumps | **Feature (open)** | n/a |
| 66 | closed | FFPFSC → exFAT | **Implemented** (Extract accepts .ffpfsc) | n/a |
| 65 | open | Single files >4 GB (ffpkg extract) | **Limitation** (UFS2Tool Dokan) | Windows |
| 64 | closed | Many errors / drive letter F | **Fixed** (PS1 letter picker) | Windows + mapped drive |
| 63 | open | Trojan Wacatac.H!ml | **Not a defect** (mitigated) | Defender |
| 62 | closed | ERROR 87, no files copied | **Fixed** (retry without /J; no text-only reject) | Windows |
| 61 | open | Edit queue while converting | **Implemented** (unified queue) | n/a |
| 60 | open | Firmware in output name | **Implemented** (`%FIRMWARE%` template) | n/a |
| 59 | closed | "Trojan in software" | Not a defect | — |
| 58 | closed | Window drag lag | Not reproducible | Windows |
| 56 | closed | No window + VirusTotal | Not a defect / historic | — |
| 55 | closed | Stuck at 90%→55% compression | Historic / needs info | — |
| 54 | closed | How to run on Windows 7 | Not a defect (community tip) | — |
| 53 | closed | Stuck checking eboot.bin | Fixed (auto-descend to the game root) | — |
| 52 | closed | Stuck at 99% | Not a defect (finished later) | — |
| 51 | closed | py311.dll on Windows 7 | Not planned (Win10/11 only) | — |
| 50 | closed | UnicodeEncodeError cp1252 (Yōtei) | Fixed (UTF-8 workers, ASCII staging) | — |
| 49 | closed | 20 VirusTotal detections | Not a defect (duplicate) | — |
| 48 | closed | Wacapew.C!ml | Not a defect (duplicate) | — |
| 47 | closed | Question about virus reports | Not a defect | — |
| 46 | closed | FATAL ERROR mkpfs cp1256 / Errno 22 | Fixed (same family as #85) | — |
| 45 | closed | exFAT intermediate in output, not temp | Fixed (`_choose_intermediate_dir`) | — |
| 44 | closed | Virus Wacatac | Not a defect | — |
| 43 | closed | Output name as input name | Historic / implemented | — |
| 42 | closed | OSFMount exit code 4 | Fixed (free-space preflight, letter picker, hints) | — |
| 41 | closed | mkpfs limited to 4 threads | Closed, but **the cap is still in the code** (by design) | — |
| 40 | closed | Extraction without progress bar | Fixed (progress, scan notes) | — |
| 39 | open | Moving issues (WinError 17, wrong output) | **Fixed** (`_move_final`, output persistence); needs retest | Windows, 2 drives |
| 38 | closed | Robocopy /MT for extraction | Implemented | — |
| 37 | open | Stream from compressed archive | **Feature (open)** | n/a |
| 36 | closed | .ffpfs (uncompressed) | By design / Extract supports it | — |
| 35 | open | Language not applied to main UI | **Fixed** | Linux (tests) |
| 34 | closed | Wine on Ubuntu | Not supported | — |
| 33 | open | Editing a full exFAT image | **Partial** (refused up front); resize is a feature | Windows |
| 32 | closed | Virus? (Chrome) | Not a defect | — |
| 31 | closed | Extremely slow | Historic (perf); CPU cap remains | — |
| 30 | closed | App hang during copy | Historic (progress added) | — |
| 29 | closed | "Ransomware" alert on format.com | Not a defect (Defender CFA) | — |
| 28 | closed | Failed to open descriptor file (UE) | Historic (fixed in 3.4.x) | console |
| 27 | closed | No exfat/ffpkg→ffpfsc option | Implemented | — |
| 26 | open | Dump fixer | **Feature (open)** | n/a |
| 25 | closed | Temp folder not honoured | Fixed | — |
| 24 | closed | 0% write stuck (3.5.3) | Historic | — |
| 23 | open | Chinese not applied after restart | **Fixed** | Linux (tests) |
| 22 | closed | ascii codec error (Yōtei) | Fixed | — |
| 21 | open | UFS2Tool extract rc=1 (2 GB limit) | **Limitation** (UFS2Tool) | Windows |
| 20 | open | FFPFS in scan | **Partial** (Library yes; rename is folder-only) | n/a |
| 19 | closed | exFAT→FFPFS direct | By design | — |
| 18 | closed | Trojan (3.4.0) | Not a defect | — |
| 17 | closed | Failed to open descriptor (3.4.0) | Historic | console |
| 16 | closed | Output name not updating; image stays mounted | Historic (fixed) | — |
| 15 | closed | Only 6 % compression | Not a defect (data-dependent) | — |
| 14 | closed | Show .ffpfsc extension | Implemented | — |
| 13 | closed | KP with inode-bits 64 | Fixed (`--inode-bits 32`) | console |
| 12 | closed | 1 core while compressing | Fixed (multi-worker) | — |
| 11 | closed | Trojan (3.20) | Not a defect | — |
| 10 | closed | Auto Backport crashes game | Not a defect (needs manual patching) | console |
| 9 | closed | Trojan Wacatac.B!ml | Not a defect | — |
| 8 | closed | Cannot create exfat/ffpkg | User error (add to queue) | — |
| 7 | closed | extract_ufs2tool not defined | Historic (fixed) | — |
| 6 | closed | In-app update: encodings module | Fixed (`updater_environment`) | Windows |
| 5 | closed | File count mismatch | Fixed (byte-based verify) | — |
| 4 | closed | Output file missing | Historic / user environment | — |
| 3 | closed | Verify fails, image still mounted | Fixed (robust dismount, stale-mount sweep, letter retry) | — |
| 2 | closed | Hang adding folder from USB | Fixed (background game-info read) | — |
| 1 | closed | Image not dismounting | Fixed (robust dismount + Force Dismount) | — |

**Status counts (main repo, 84 issues)**

| Status | Open | Closed | Total |
|---|---|---|---|
| Fixed (in code, not yet tested on Windows) | 13 (#86 #85 #80 #79 #76 #75 #74 #73 #72 #69 #39 #35 #23) | 18 (#70 #64 #62 #53 #50 #46 #45 #42 #40 #25 #22 #13 #12 #6 #5 #3 #2 #1) | 31 |
| Partial | 6 (#82 #81 #77 #71 #33 #20) | 0 | 6 |
| Needs info | 2 (#84 #68) | 0 | 2 |
| Implemented (feature exists) | 2 (#61 #60) | 6 (#66 #38 #27 #14 #43 #36) | 8 |
| Feature request (not implemented) | 3 (#67 #37 #26) | 0 | 3 |
| Limitation (UFS2Tool) | 2 (#65 #21) | 0 | 2 |
| Not a defect / AV / support / not reproducible | 2 (#78 #63) | 20 (#59 #58 #56 #54 #52 #51 #49 #48 #47 #44 #34 #32 #29 #19 #18 #15 #11 #10 #9 #8) | 22 |
| Historic (closed after an earlier fix; nothing to verify) | 0 | 10 (#55 #41 #31 #30 #28 #24 #17 #16 #7 #4) | 10 |
| **Total** | **30** | **54** | **84** |

## 2. Summary table: ps5-backport-gui (29)

| # | State | Title (short) | Status (per owner/CHANGELOG) | Relevant to main engine? |
|---|---|---|---|---|
| 29 | closed | Dragon's Dogma 2 v4.0 no load/save (SDK10, 11.60) | Closed without a comment; unresolved | Low (save handling) |
| 28 | open | FF Resonance no audio (CRI 7.1.4) | Fixed in 0.3.4, confirmed on 4.50 and 5.50 (could be closed) | **Yes**: same class as `ui/audio_channel_scan.py` (12→8 channel) |
| 27 | closed | Hollow Knight build refused (existing fakelib) | Fixed 0.3.3; AMPR/PlayGo swap 0.3.4-test2 | **Yes**: `_abp_run` fakelib overlay |
| 26 | closed | Minecraft PRX_NOT_RESOLVED on 4.03 | Unresolved (PSN functions missing <8.60); closed | Indirect |
| 25 | closed | Crash on drag and drop | Fixed 0.3.3 | Low: main `_on_drop` is a different implementation |
| 24 | closed | Fighting Souls refused at SDK 7 | Fixed 0.3.3 (save-mount check) | No (BGUI-only check) |
| 23 | closed | JWE3 build failed | Same fix as #24 | No |
| 22 | closed | Fighting Souls refused at SDK 10 | Same fix as #24 | No |
| 21 | closed | Spider-Man 2 Pro options hidden on 10.01 | Fixed 0.3.3 (Pro check) | No (no PSSR feature in main) |
| 20 | open | Defender flags EXE | False positive; open | Same packaging concern as #78/#63 |
| 19 | closed | Valheim CE-108255-1 | User fixed by repacking the dump | No |
| 18 | closed | Hitman 3 crash (PlayGo) | User setup issue | Indirect (PlayGo) |
| 17 | closed | Dave the Diver crash (dump fakelib AMPR/AppContent) | Workaround + Switch AMPR/PlayGo in 0.3.4-test2 | **Yes**: fakelib overlay keeps the dump's AMPR |
| 16 | closed | Moss crash | Incomplete dump | No |
| 15 | closed | KH 1.5+2.5 black screen SDK7 | Fixed (confirmed) | Indirect |
| 14 | closed | KH 2.8 black screen SDK7 | Fixed 0.3.1 (confirmed) | Indirect |
| 13 | closed | Double Dragon Revive no audio | Fixed 0.3.1 (confirmed on 7.00) | **Yes** (audio fix family) |
| 12 | closed | Mewgenics 0xa0020101 | Duplicate of #9; fixed 0.3.1 | Indirect |
| 11 | closed | Wonderboy no audio | Fixed 0.3.1 | **Yes** (audio) |
| 10 | closed | Story of Seasons no audio | Fixed 0.3.1 (confirmed) | **Yes** (audio) |
| 9 | closed | Mewgenics PRX_NOT_RESOLVED 5.50 | Fixed 0.3.1 (tested on 4.50) | Indirect |
| 8 | closed | Planet of Lana build fails | Fixed (right.sprx "Invalid param magic") | **Yes**: main engine has the same patcher and no right.sprx handling |
| 7 | closed | 007 First Light build fails | Same as #8 | **Yes** |
| 6 | open | Re-backport to a higher SDK / PSSR 2 | Feature, experimental; open | No |
| 5 | closed | KH 1.5+2.5 save-mount loop 10.01 | Fixed 0.3.3 (4.xx save fixes no longer applied to newer SDKs) | Low (main has no save patches) |
| 4 | open | Crisol saves not shown in Load Game (9.00) | Needs per-game investigation | No |
| 3 | closed | Castlevania incomplete report | Invalid report | No |
| 2 | open | MGS4 crash / black screen | Launch fixed 0.3.3 (Il2Cpp case); black screen open | **Yes**: "Invalid param magic" (same patcher); module-name case |
| 1 | closed | Moss incomplete dump | Not a defect | No |

Counts: fixed/confirmed 15 (#28 #27 #25 #24 #23 #22 #21 #15 #14 #13 #12 #11 #10 #9 #8), with #7 plausibly fixed; partial 1 (#2); open feature 1 (#6); needs investigation 1 (#4); not a defect / user or dump issue 6 (#20 #19 #18 #16 #3 #1); unresolved 4 (#29, #26, #17 workaround only, #5 fixed but unconfirmed on 10.01).

---

## 3. Per-issue sections: ps5-exfat-builder

Abbreviations: **EB** = `exfat_builder.py`, **UB** = `ui/tab_unibuild.py`, **PS1** = the embedded `New-OsfExfatImage.ps1` (`_PS1_B64`, line numbers inside the decoded script), **BAT** = `make_image.bat` (`_BAT_B64`).

### Group A: finished images deleted on non-English Windows (#72, #73, #74, #76, #79, #80)

These six issues share one root cause, so they share one analysis.

- **Reports**
  - **#72** (open): zh-CN Windows 11, 156,250 files. A full copy that passed the post-copy check was followed by "[COPY] Reason: 0 of 156250 source files copied" and the image was removed. The reporter correctly identified the regex `Files\s*:\s*(\d+)` as the cause and offered an English-`.mui` robocopy shim as a workaround.
  - **#73** (open): Ghost of Yōtei and Astro Bot, same message, plus the `%d format` crash.
  - **#74** (open): same result with 79,962 files.
  - **#76** (open): Italian Windows. The full log shows `File:    223243    223243 …`, a passed post-copy check, the image deleted, and the TypeError. A comment reports the same for two more games.
  - **#79** (open): French Windows 11. "Fichiers: 52655 …", same outcome. Comments add two more cases, one with a failed dismount. One user went back to 3.6.4.
  - **#80** (open): French Windows 10. The same analysis, plus the suggestion that a count mismatch should warn, not delete, and a note that the check was missing from the v4.0.2 tarball (that is #75).
- **Root cause (v4.0.2)**: with more than 50,000 files the PS1 switches robocopy to `/NFL` summary mode. The only copied-file count was then the English-only regex in the line handler. `_robocopy_file_count` stayed 0, `_build_output_is_empty` reported "0 of N copied", and the rejection path deleted the image. The secondary crash came from `_render_queue` formatting `item.size_override_gb == 'auto'` with `%d`.
- **Current code**
  - **Copy count**: PS1 line 949 now prints `[COPY-STATS] dest_files= … dest_bytes= …`, counted on the mounted volume by `Get-TreeFileStats` (PS1 lines 229–251). EB `_handle_line` (line 9675) reads it through `parse_copy_stats_line` (EB 2405). The fallback, `RobocopySummaryParser` (EB 2429), reads the second six-integer row after the dashed separator and ignores the label.
  - **Rejection rule**: `_build_output_is_empty` (EB 9821) now rejects only on structured evidence (dest_files == 0, or a known robocopy count of 0, or a corrupt container). "Unknown" means "do not reject".
  - **No deletion**: `_item_done` (EB 9892) no longer deletes; it logs "The image was NOT deleted".
  - **Crash**: `coerce_size_gb` (EB 2671) is used in `_render_queue` (EB 10913).
  - **Per-build state**: the counters (`_copy_stats`, `_robocopy_count_known`, the parser) are reset for each build (EB ~10446–10458).
- **Verified on Linux**: the parser returns the right file row for en, it (#76 verbatim), fr (#80), zh with an ASCII or full-width colon (#72), de, ja, ru, pt-BR and an NBSP separator; failed files are counted (`total − failed`). The only miss is thousands-grouped counts, which robocopy does not print, and a miss leads to "unknown → accept", not deletion. The existing tests `tests/test_copy_accounting.py` pass.
- **Remaining risks**
  - **R1 (medium): an unreadable root yields a false 0.** `Get-TreeFileStats` swallows every exception per directory, so if enumerating the volume root fails the script prints `dest_files=0` and Python rejects the build. Auto-retry (default `retry_count` 3, EB ~9982) then rebuilds the image with `-ForceOverwrite` (`Remove-Item`, PS1 line 616). The "image NOT deleted" log line is therefore misleading whenever a retry follows.
  - **R2 (medium): the post-copy check can delete a good image.** The PS1 check (PS1 ~959) compares used bytes with 90 % of *all* source bytes, but robocopy skips `*.exfat`/`*.ffpkg` and, with Exclude hidden, hidden files. A large excluded file in the source therefore makes the PS1 throw, and the BAT `:ps_failed` handler deletes the image.
- **Reproduction**: on a non-English Windows install, build any dump with more than 50,000 files to exFAT with v4.0.2. For Farewell, run the same build and expect `[COPY] Files on the image: N of N` and a kept image.
- **Reproducible?** The parser can be reproduced on Linux; the end-to-end build needs Windows plus OSFMount and a localized robocopy.
- **Proposed resolution**: close all six once a Windows test on a non-English locale passes. Then fix R1 and R2 (§6).
- **Linux**: the parser and rejection policy can be fixed and tested on Linux (done). R1 and R2 are PowerShell changes; their logic can be unit-tested by decoding the PS1, and the behaviour needs Windows.

### #86 (open): "[VERIFY] FAILED … Mounted drive did not appear"; image deleted on closing prompt; repeated OSFMount install prompts
- **Report**: every exFAT verification failed with "Mounted drive did not appear". After the prompt closed, the finished image was deleted (the user had to move it away first), yet the images work. On every verification, the app also asks to install OSFMount, sometimes in a loop, although OSFMount is installed. The attached report could not be fetched.
- **Status: deletion fixed, 90 s wait fixed, OSFMount prompt likely fixed.**
  - **Deletion**: in v4.0.2, `_uq_cleanup_job` (UB 2682) deleted the exFAT on any failure. It now keeps exfat/ffpkg outputs unless the job was cancelled.
  - **Verification flag**: a verification failure now reaches the Build tab as `'warn'` (EB `_process_next` 10291) and shows as **CHECK**. The PFS pipeline does not compress a flagged intermediate (UB `_dispatch_pfs` `_after_build`).
  - **90 s wait**: `_verify_image` (EB 22830) waits up to 90 s for the drive and returns *inconclusive* (ok=True with a file-level check) if it never appears.
  - **OSFMount prompt**: `_find_osfmount` (EB 8173) no longer prefers a lone custom `osfmount.exe`, which could be PassMark's installer. This is covered by `OsfmountDiscovery` in the tests. The diagnosis is plausible but unproven without the report.
- **Reproduction**: Windows 10/11 with OSFMount; build an exFAT in the Build tab with verification on; make the drive slow to appear (Defender scanning, a large volume).
- **Reproducible?** Needs Windows and OSFMount.
- **Root cause (v4.0.2)**: a 6 s drive-appear window, failure treated as fatal, and the cleanup deleting the image.
- **Proposed resolution**: ask the reporter to retest. If the install prompt persists, log the resolved OSFMount path at verification time (it is not logged today) and the exact command run.
- **Linux**: the cleanup rules can be unit-tested if `_uq_cleanup_job` is lifted out of its closure. Verification itself needs Windows.

### #85 (open): pfs build `OSError: [Errno 22] Invalid argument`, and #69 (open): ffpkg→pfs, same traceback
- **Report**: the Lazy_MkPFS worker dies in `pbar.status → sys.stderr.write`, and then again in `print(f"❌ …")`.
- **Root cause**: the parent decoded the worker pipe with the Windows locale codec. An undecodable byte (UTF-8 progress glyphs or ❌ under cp936/cp1256/cp1252) killed the reader thread, the pipe was closed or undrained, and the child's next write raised EINVAL. That is consistent with the traceback.
- **Status: fixed.**
  - **Parent side**: `ui/experimental_mkpfs.py` Popen now uses `encoding='utf-8', errors='replace'` and sets `PYTHONIOENCODING=utf-8:replace`.
  - **Child side**: the worker calls `_prepare_worker_streams` (EB ~2567), which wraps stdout and stderr in `_TolerantStream` (EB 2531), so a failing write can no longer abort the pack. This is covered by `tests/test_runtime_safety.py::WorkerStreams`.
- **Minor leftover**: `lazy_worker_main` (`ui/experimental_mkpfs.py:545`) returns 0 for a `SystemExit` with a non-int code. Lazy only uses int codes today, so the risk is low.
- **Reproduction**: Windows with a non-UTF-8 ANSI code page (e.g. Persian or Chinese); convert an image with the Lazy backend.
- **Reproducible?** Needs Windows. A Linux simulation is possible by forcing the parent to decode with cp1252.
- **Resolution**: close after a Windows retest.
- **Linux**: yes, both sides can be unit-tested (partly done).

### #84 (open): Stuck on scan
- **Report**: two folder dumps hang at step one ("scanning the folder") for an hour. The user also tried selecting `sce_sys`. No log was given.
- **Status: needs info.** Code paths involved:
  - `_browse_src` runs `resolve_game_root_deep` (EB 343) on the Tk thread. It is bounded (4000 dirs, depth 5), so it should not hang for an hour.
  - The build preflight `_preflight_source` (EB 26166) does an unbounded `os.walk` with no cancel and no stall detection.
  - The PS1 `Get-SourceStats` enumerates the tree again.
  - Selecting `sce_sys` makes the eboot check fail ("eboot.bin not found"), which is a different symptom.
  - The likely causes are a source on a cloud or network drive (each `getsize` triggers I/O) or an unresponsive USB device. Unconfirmed.
- **Reproduction**: unknown.
- **Reproducible?** No; needs a log.
- **Resolution**: ask for the Output log and the source drive type. Add a stall notice to `_preflight_source` ("no progress for 60 s, currently in <folder>") and a cancel check.
- **Linux**: the stall-notice logic can be developed and tested on Linux (for example, by mocking a slow `os.walk`).

### #82 (open): Stuck on "Creating & mounting image" after the first .ffpfsc
- **Report**: after the first game, the next job stays at Creating & mounting. Restarting the app does not help; a reboot does.
- **Root cause**: the OSFMount driver is wedged, probably by a leftover mount. The original `osfmount -a` had no timeout.
- **Status: partial.**
  - **Timeout**: PS1 `Invoke-OsfHidden` (PS1 185) now has a timeout, and the attach is bounded at `900 s + 15 s × GB` (PS1 ~818), with a clear message telling the user to use Force Dismount or restart.
  - **But**: for a 200 GB image that bound is still about 65 minutes.
  - **No sweep before builds**: `_sweep_stale_osf_mounts` (EB 9272) runs only before verification and at close, never before a build, so a leftover OSFIMG mount from the previous job is not cleared.
- **Reproduction**: run a queue of two or more dump→ffpfsc jobs on Windows with OSFMount, then interrupt or force a dismount failure.
- **Reproducible?** Needs Windows and OSFMount.
- **Resolution**: call `_sweep_stale_osf_mounts(exclude_letter=None)` before each build in `_process_next`/`_dispatch_exfat`. Log the OSFMount mount list (`osfmount.com -l`) when the attach times out. Consider a shorter attach timeout plus a "still creating" note.
- **Linux**: no (Windows only). The sweep call site can be tested with mocks.

### #81 (open): Verification failed: image is missing data
- **Report**: Rift Apart was dumped with PS5 App Dumper, which itself reported the dump as partial. The build finished, but verification said the image was incomplete. The log is on pastebin and could not be fetched.
- **Status: partial / needs info.**
  - **Fixed**: a false positive when Exclude hidden files is on. The source walk now skips hidden files (`verify_walk_tree(skip_hidden=…)`, EB 2476, used in `_verify_image` EB 22830).
  - **Improved**: missing or short files are now named (`describe_missing_files`).
  - **Not fixed**: if robocopy really skipped files (exit 8 with locked files, PS1 ~935), the shortfall is genuine.
- **Reproduction**: build with Exclude hidden files on from a dump containing hidden files.
- **Reproducible?** The hidden-file part reproduces on Windows; `_is_hidden_file` is always False on Linux.
- **Resolution**: ask the reporter to rerun Farewell and post the `[VERIFY] Files missing or short` list.
- **Linux**: the walk logic is tested (`VerificationHelpers`).

### #78 (open): Latest release download flagged as a trojan; #63 (open): Trojan:Win32/Wacatac.H!ml
- **Status: not a code defect (mitigated).** `build.bat` now uses `--noupx`, embeds version info, writes a SHA-256 file, and builds in a pinned environment. The owner previously said detections were caused by the bundled scripts.
- **Reproduce**: Windows Defender plus the GitHub download. Results vary with the signature version.
- **Resolution**: code-signing; submit the release to Microsoft (WDSI); publish SHA-256 sums in the release notes; consolidate into one tracking issue.
- **Linux**: n/a.

### #77 (open): Unhandled error in a UI handler when converting to exFAT
- **Report**: the build stops right after "[PRE] Source reachable. Checking for eboot.bin…", then a TypeError in `_item_error → _update_queue_dot → _render_queue`.
- **Status: the crash is fixed (`coerce_size_gb`); the underlying error is unknown.** The preflight returned `{'ok': False}` (eboot.bin missing at the top of `D:/homebrew/PPSA17599-…`, or a scan failure). `_item_error` (EB 10066) shows the message only in a dialog and the status bar, and does **not** log it outside unified mode. The crash prevented even the dialog.
- **Reproduction**: queue a folder whose eboot.bin is nested deeper than the auto-resolver can resolve, or several candidate roots.
- **Reproducible?** Needs Windows; on Linux the logic could be checked with mocks.
- **Resolution**: log `msg` in `_item_error` unconditionally; ask the reporter to retest with Farewell.
- **Linux**: yes (a trivial change, testable with a fake app).

### #75 (open): Repo code not synced with release tags
- **Status: fixed on `farewell-edition-dev`.** The v4.0.2 source was restored (bytecode-verified per the CHANGELOG) and the vendored Lazy_MkPFS added.
- **Resolution**: close when the branch is merged to the default branch and tagged.
- **Linux**: n/a.

### #71 (open): exFAT unpacking hangs after mounting
- **Report**: ffpfsc→exfat works; extracting an .exfat mounts it and then hangs at the check. The reporter asked for a skip option.
- **Status: partial.**
  - **Fixed in `_run_extract`** (EB 11079): the mount timeout is 180 s, the drive-appear wait 90 s, the pre-scan prints progress every 10 s, and robocopy failures are reported.
  - **Not fixed**: the scan pre-pass is still mandatory and cannot be cancelled; there is no skip option. The drive letter is chosen with `GetLogicalDrives` only, so it can pick a network-mapped letter (see §6 M5).
- **Reproduction**: extract a large .exfat with OSFMount.
- **Reproducible?** Needs Windows and OSFMount.
- **Resolution**: make the pre-scan optional (use the image's data size from `GetDiskFreeSpaceEx`) and cancellable.
- **Linux**: partly (the scan loop).

### #70 (closed): newfs failed (.NET runtime missing)
- **Status: fixed (hint).** UFS2Tool failures caused by a missing .NET 8 runtime are now detected and link to the download (`DotnetHint` test).
- **Reproduction**: Windows without .NET 8. **Linux**: tested.

### #68 (open): Some games split into two identical ones when creating a PFS
- **Status: needs info.** One sentence and no log. Possible readings: (a) the intermediate .exfat and the .ffpfsc both remain, which is by design when compression fails or the intermediate cannot be removed; (b) a Library duplicate; (c) the Lazy path silently fails to delete the intermediate (see §6 H1).
- **Resolution**: ask for a screenshot and log.

### #67 (open): Support compressed backup dumps; #37 (open): stream from an archive without extracting
- **Status: feature request, not implemented** (the CHANGELOG lists this under known limitations).
- **Resolution**: out of scope for Farewell. If ever done: ZIP/7z extraction to temp with a free-space check before the existing pipeline (streaming into robocopy is not feasible).

### #66 (closed): FFPFSC → exFAT
- **Status: implemented.** The Extract tab accepts `.ffpfsc` (`ui/tab_extract.py:614`, 1328).

### #65 (open): Single files over 4 GB cannot be extracted (ffpkg); #21 (open): UFS2Tool extract failed (rc=1)
- **Reports**: #65 aborts on `pakchunk0-ps5.pak` (29.9 GB). #21 hits UFS2Tool's 2 GB in-memory limit on any large ffpkg; comments say ffpkg→exfat fails the same way.
- **Status: limitation.**
  - `_ffpkg_mount_copy_extract` (EB 5108) detects files of 4 GB or more and aborts cleanly before copying (the Dokan mount cannot serve offsets past 2³²).
  - The `UFS2Tool extract` command has the 2 GB in-memory cap, which `ui/tab_convert.py:975` detects.
- **Reproduction**: an ffpkg with a file over 2 GB on Windows. **Reproducible?** Needs Windows.
- **Resolution**: document workarounds (the Linux kernel `ufs` driver mounts UFS2 read-only, `-t ufs -o ro,ufstype=ufs2`; or keep exFAT intermediates); a pure-Python UFS2 streaming reader would be a real fix. Not for Farewell.
- **Linux**: a UFS2 reader could be developed and tested on Linux.

### #64 (closed): Many errors; drive letter F was the cause
- **Status: fixed in the PS1.** `Get-UsedDriveLetters` (PS1 261) also reads `HKCU:\Network` and `DriveInfo`/`GetLogicalDrives`.
- **Caveat**: Python-side pickers (verify `_pick_free_letter` EB 22964, extract EB 11079, `ui/tab_convert.py`, `tab_files.py`, `tab_ffpkg_edit.py`) still use `GetLogicalDrives` only (§6 M5).

### #62 (closed): ERROR 87, no files copied
- **Status: fixed.** PS1 ~925 retries robocopy once without `/J` when the exit code is 8 or higher. `_build_output_is_empty` no longer rejects on the text "ERROR 87" alone. Tests: `test_error_87_text_alone_does_not_reject`.
- **Reproduction**: Windows/OSFMount volumes where `/J` fails. Needs Windows.

### #61 (open): Edit queue while converting
- **Status: implemented.** In the unified Build tab, queued rows stay editable and removable during a run (`UB` render, `editable = job['status']=='queued'`). `_uq_run_next` picks the next queued job dynamically, so jobs can be added. Clear removes only queued rows while running.
- **Resolution**: close.

### #60 (open): Add firmware to the output name
- **Status: implemented.** Template `%TITLE% (%VERSION%) (%FIRMWARE%) (%PPSA%)` with tokens `%TYPE%` and `%DATE%` (UB 133, 650–672, 1078). A commenter confirmed.
- **Resolution**: close.

### #59, #56, #49, #48, #47, #44, #32, #18, #11, #9 (closed): antivirus reports
- **Status: not a defect** (heuristic or ML detections of an unsigned PyInstaller one-file exe). #56 also mentioned "no window appears", which is consistent with AV blocking the extraction. Same mitigation as #78.

### #58 (closed): Window drag lag
- **Status: not reproducible** (owner). Nothing in the code to act on.

### #55 (closed): Compression went from 90 % back to 55 %; #52 (closed): stuck at 99 %; #24 (closed): 0 % write stuck; #40 (closed): no progress during extraction; #30 (closed): app hang during copy
- **Status: historic progress-reporting problems** in 3.5.x–3.6.x. Progress parsing has since been rewritten (`ui/progress_state.py`, chunked reads in `ui/experimental_mkpfs.py`, extraction scan notes). No logs; nothing actionable.

### #54 (closed): Running on Windows 7; #51 (closed): py311.dll on Windows 7
- **Status: not supported** (Windows 10/11 only). #54 is a community recipe.

### #53 (closed): Stuck on checking for eboot.bin
- **Status: fixed.** `resolve_game_root_deep` (EB 343) and the `_process_next` auto-descent (EB ~10353) find a nested game root.

### #50, #46, #22 (closed): UnicodeEncodeError / ascii codec / Errno 22 with "Yōtei"
- **Status: fixed.** Workers write UTF-8 (`_prepare_worker_streams`). The parent decodes UTF-8. Non-ASCII source images are staged under an ASCII name (UB `_dispatch_pfs_from_image`).

### #45 (closed) and #25 (closed): temp folder not used for the intermediate
- **Status: fixed.** `_choose_intermediate_dir` (UB 2142) uses temp when it is on another drive with space (and supports hard links for ffpkg).

### #43 (closed): Output name the same as the input name
- **Status: implemented.** Custom naming and template.

### #42 (closed): OSFMount exit code 4
- **Status: fixed.** Free-space preflight, letter knowledge and an error-text hint mapping (PS1 ~850–865). Needs Windows.

### #41 (closed): mkpfs limited to 4 threads; #31 (closed): extremely slow; #12 (closed): 1 core
- **Status**: #12 fixed (multi-worker).
- **#41 was closed, but the cap remains**: `_pfs_pack_tuning` (EB 2367) uses at most 4 workers by design (RAM and backlog safety). #31 complaints are consistent with this.
- **Resolution**: optional (low): expose an Advanced "PFS workers" override with a warning.

### #39 (open): Moving issues (WinError 17; output placed in the wrong folder)
- **Report**: moving the .ffpfsc from temp to another drive failed with WinError 17 and the result was left in place. Later comments: the output went to the source folder or to the previous session's output folder; the free-space warning used the old drive; the scroll wheel worked only on the scrollbar.
- **Status: fixed in code; needs a Windows retest.**
  - `_move_final` (EB 23440) copies across volumes with `.part`, fsync, a size check and an atomic rename, and never deletes the source on failure.
  - The output folder is persisted immediately and captured per job at add time (UB `_persist_setting`, `_uq_add`).
  - Touchpad and wheel scrolling were fixed in Farewell.
- **Reproduction**: temp on drive G:, output on E:; build ffpfsc. Needs Windows (or two filesystems on Linux for `_move_final`).
- **Linux**: `_move_final` can be tested on Linux across two tmpfs mounts.

### #38 (closed): Robocopy /MT for extraction
- **Status: implemented** (thread setting, robocopy-based extraction).

### #36 (closed): .ffpfs (uncompressed); #19 (closed): direct exFAT→FFPFS
- **Status: by design.** Build produces .ffpfsc; Extract and Library accept .ffpfs.

### #35 (open): Language not applied to the main UI; #23 (open): zh saved but the UI stays English
- **Status: fixed.**
  - **Cause**: navigation and page titles had no translation entries.
  - **Translations**: they are added for 16 languages (EB ~2124 `_NAV_TRANSLATIONS`).
  - **Startup**: the saved language is applied at startup (`set_language(saved_lang)`, EB ~3955).
  - **Apply**: `ui/tab_language.py::_apply_lang` rebuilds all tabs and switches back to Language. It refuses while a build runs.
  - **Tests**: `tests/test_translations.py` (6 tests) pass on Linux.
- **Reproduction**: select Chinese, Apply, restart.
- **Resolution**: close after a quick Windows check.
- **Linux**: yes (translation tables; the GUI smoke test needs a display).

### #34 (closed): Wine on Ubuntu
- **Status: not supported** (OSFMount driver).

### #33 (open): Editing an exFAT image, "No space left on device"
- **Report**: adding files to a 270 GB image with 128 KB free failed for 22 of 52 files.
- **Status: partial.**
  - The Edit flow now refuses up front when the files do not fit (CHANGELOG; `ImageSpaceCheck` test).
  - **Not implemented**: expanding or resizing an image. Comments also describe copying an .exfat into `.pfs_stage`, fixed since 3.6.2 by direct packing (UB `_prep_worker`).
- **Resolution**: feature, "Rebuild with +N GB", which reuses the image as a source (mount read-only, then build a new image with a size override).
- **Linux**: n/a.

### #29 (closed): "Possible ransomware" alert on format.com
- **Status: not a defect** (Defender Controlled Folder Access blocks format.com). Document an allow-list entry.

### #28, #17 (closed): "Failed to open descriptor file …uproject" (Unreal Engine games after PFS)
- **Status: historic**, fixed in 3.4.x per the owner. Needs a console. No current code pointer.

### #27 (closed): No option to convert an existing exfat/ffpkg to ffpfsc
- **Status: implemented** (PFS source mode "image", UB `_dispatch_pfs_from_image` 2461).

### #26 (open): Dump fixer ("sfc scan" for dumps)
- **Status: feature request, not implemented.** The verification walk and the backport engine contain building blocks (eboot and fakelib detection), but there is no fixer.
- **Resolution**: out of scope for Farewell.

### #20 (open): Include FFPFS in scans
- **Status: partial.** The Library scan includes `.ffpfs` (`_LIBRARY_IMAGE_EXTS`, EB ~12250). Dump Rename (`ui/dump_rename.py`) works on folders only, so image files are not renamed.
- **Resolution**: close the Library part; renaming image files is a small feature.

### #16 (closed): Output name not updating; image stays mounted
- **Status: historic, fixed** (name regenerated when the source changes; robust dismount).

### #15 (closed): RDR only 6 % compression
- **Status: not a defect** (already-compressed data).

### #14 (closed): Show the .ffpfsc extension
- **Status: implemented.**

### #13 (closed): PFS images cause a kernel panic (inode-bits 64)
- **Status: fixed.** `--inode-bits 32` (EB ~23818); Lazy backend.

### #10 (closed): Auto Backport game crashes on 10.40
- **Status: not a defect.** Some games need manual patches. Relevant to the main backport engine (see the Backport GUI notes).

### #8 (closed): Cannot create exfat or ffpkg
- **Status: user error** (add to the queue, then Build all).

### #7 (closed): `extract_ufs2tool is not defined` (1.7.0)
- **Status: historic, fixed.** `extract_ufs2tool` exists (EB 751).

### #6 (closed): In-app update leads to "Failed to import encodings module"
- **Status: fixed.**
  - **Cause**: the new exe inherited `_MEIPASS2`/`_PYI_*`.
  - **Fix**: `updater_environment` (EB ~2595) strips PyInstaller state, and the exe is no longer appended to as a lock test.
  - **Test**: `UpdaterEnvironment`. The end-to-end update needs Windows.

### #5 (closed): File count mismatch (Demon's Souls)
- **Status: fixed.** Verification is byte-based with tolerance, applies the same exclusions on both sides, and retries once with a fresh mount (`_verify_done` EB 23186). The original robocopy exit 9 (locked files) was a real issue in the user's environment.

### #4 (closed): "Output file not found" after OK
- **Status: historic / environment** (antivirus or permissions). Verification lists similar files (EB ~22835).

### #3 (closed): Verification fails at the end, image still mounted; #1 (closed): image not dismounting
- **Status: fixed.** `_dismount_drive_robust`, Force Dismount, `_sweep_stale_osf_mounts` before verification, and a new letter on each attempt.

### #2 (closed): Hang adding a folder from USB
- **Status: fixed.** Game info is read on a background thread (UB `_on_source_changed` `_w`).
- **Note**: the legacy `_on_queue_drop` (EB ~10163) still calls `_detect_game_info` and `_add_to_queue` for each dropped folder on the Tk thread; risk is low.

---

## 4. Robocopy parser experiment (Linux, Python 3.12, repository copy)

`scripts/robo_locales.py` fed summary blocks to `RobocopySummaryParser`:

| Locale / case | Parsed file row | Present |
|---|---|---|
| en | (223243, 223243, 0, 0, 0, 0) | 223243 |
| it (#76 verbatim) | (223243, 223243, 0, 0, 0, 0) | 223243 |
| fr "Fichiers :" (#80) | (223239, 223239, …) | 223239 |
| zh "文件:" (#72) / full-width "：" | (156250, 156250, …) | 156250 |
| de "Dateien:" / ja "ファイル:" / ru "Файлов:" / pt-BR | correct | correct |
| failures (245 total, 245 failed) | (245, 0, 0, 0, 245, 0) | 0 (rejected, correct) |
| NBSP after the colon | correct | correct |
| thousands-grouped counts (hypothetical) | None | count unknown, so not rejected (safe) |

The `[COPY-STATS]` line parses as expected. The full suite (`python3.12 -m unittest discover -s tests`) gives 51 tests, OK, 1 skipped (the GUI smoke test).

---

## 5. Per-issue sections: ps5-backport-gui

The source is closed, so status follows the owner's comments and CHANGELOG. Almost everything needs a PS5 console to reproduce. "Main engine" means `ui/tab_backports.py`, `ui/panel_bp_auto.py`, and EB `_abp_*`/`_bp_*` with the embedded `Backport.py` and the idlesauce/NazkyYT `ps5_sdk_version_patcher.py`.

- **#29 (closed) Dragon's Dogma 2 v4.0 (PPSA09663), SDK 10, FW 11.60, ShadowMountPlus (SM+) 1.7b11.** Report: three errors on a new profile, cannot create a save, black screen with an existing save. Status: closed with no comment and no logs (only "img"), so it looks closed as invalid or incomplete; unresolved. Repro: needs a console. Root cause unknown (save-data handling at a higher SDK). Resolution: request a klog and the Export logs ZIP. Main engine: low relevance (the main engine has no save patches).
- **#28 (open) FINAL FANTASY RESONANCE (PPSA31544) no audio, 5.xx/4.xx.** Root cause (owner): the CRI engine requests 7.1.4 (12 channels), and firmware 4.xx/5.xx allows at most 8. The fix is a 2-byte eboot patch to 7.1, shipped in 0.3.4 and confirmed on 4.50 and 5.50. Status: fixed and confirmed; can be closed. **Main engine: yes.** `ui/audio_channel_scan.py`/`audio_fix_patch.py` hold a single profile for a similar 12→8 channel fix ("DBSparklingZero PPSA15210"). A generic CRI 7.1.4→7.1 profile could be ported. Testable on Linux at byte level; the result needs a console.
- **#27 (closed) Hollow Knight (PPSA34971) build refused, then 0xa0020104 at launch.** The dump already had a `fakelib`. Fix: 0.3.3 replaces the dump's three libraries; 0.3.4-test2 adds Switch AMPR/PlayGo (drakmor ampr_emu 0.4.2.1). Confirmed on 4.03. **Main engine: yes.** In `_abp_run` (EB 16223) the fakelib overlay copies the user's fakelib set *over* the dump's `fakelib/` and backs up overwritten files, but leaves extra dump libraries such as `libSceAmpr.sprx` and `libSceAppContent.sprx` in place, which is the failure mode of #27 and #17.
- **#26 (closed) Minecraft (PPSA17221) PRX_NOT_RESOLVED_FUNCTION on 4.03.** Root cause: 19 missing imports (PSN sign-in functions absent below 8.60, WebSocket and others). Status: unresolved; the owner may add replacements; closed. Main engine: indirect (no NID-coverage report in the main engine).
- **#25 (closed) Crash on drag and drop (0.3.2).** Fixed in 0.3.3 and confirmed. Main engine: low. The main app's `_on_drop` (EB 10150) does not crash, but it silently ignores multi-folder drops (`strip('{}')` on "{a} {b}").
- **#24, #23, #22 (closed) build refused, "No safe automatic candidate" (Fighting Souls, JWE3).** Root cause: BGUI's own final save-data check counted only one of two identical save-mount requests. Fixed in 0.3.3; #24 confirmed by a full playthrough on 7.00. Main engine: not applicable (no such check).
- **#21 (closed) Spider-Man 2 Pro options hidden on 10.01 Pro.** Fixed in 0.3.3 (the Pro answer now comes from the console's own check); confirmed. The PSSR 2 image question moved to #6. Main engine: not applicable.
- **#20 (open) Defender flags the EXE (Wacatac.H!ml, 0.3.2).** A false positive on a one-file packaged Python app; the owner found 0.3.3 clean and kept the issue open. Same remedy as main #78: code-signing and publishing SHA256SUMS.
- **#19 (closed) Valheim CE-108255-1.** The user fixed it by repacking the dump (likely undecrypted files). Not a defect.
- **#18 (closed) Hitman 3 crash.** The game stops at its own check after PlayGo refuses the extracted copy; user setup. Main engine: indirect (PlayGo emulation).
- **#17 (closed) DAVE THE DIVER crash (4.50/6.02/9.60).** Root cause: the dump's own AMPR library, and the DLC emulator's AppContent library failing to load. Workaround given; the Switch AMPR/PlayGo option is in 0.3.4-test2. **Main engine: yes** (same overlay behaviour as #27).
- **#16 (closed) Moss 12→11 crash.** Incomplete dump. **#1 (closed) Moss**: incomplete dump. Not defects.
- **#15, #14 (closed) Kingdom Hearts 1.5+2.5 / 2.8 crash then black screen at SDK 7.** Fixed by 0.3.1 / 0.3.3-test2 and confirmed on 7.00. Indirect relevance.
- **#13, #11, #10 (closed) no audio (Double Dragon Revive, Wonderboy, Story of Seasons).** 0.3.1 audio fix, confirmed on 4.50/7.00/4.03. **Main engine: yes** (audio-fix family; the main engine has one profile only). An unrelated R-Type Delta HD 8.60 no-audio report was buried in #13 and never filed.
- **#12 (closed) and #9 (closed) Mewgenics 0xa0020101.** Fixed in 0.3.1 (tested on 4.50); 5.50 unconfirmed. Indirect.
- **#8 (closed) Planet of Lana and #7 (closed) 007 First Light: builds fail.** Root cause: `sce_sys/about/right.sprx` fails conversion with `Invalid param magic 0x79A6EB9E`; BGUI replaces it with the standard copy. #8 confirmed on 4.03; #7 had no reply. **Main engine: yes.** The main app embeds the same patcher (`_SRC_PATCHER_B64` → `ps5_sdk_version_patcher.py:244-246` returns "Invalid param magic"), and `Backport.py`'s `decrypt_and_sign_pipeline` counts it as a signing failure and leaves the original file in place. Nothing in the main repo handles `right.sprx`. The run ends as "✓ Patched & applied … ✗ Failed: 1" with a warning only; whether the console then accepts the unpatched `right.sprx` is untested. Linux: the classification can be developed and tested on Linux with a sample header.
- **#6 (open) re-backport to a higher SDK / PSSR 2 on PS5 Pro.** Experimental feature. Detailed tester reports show the PSSR 2 test always selects the original model `r40_18`, so the image never changes. Open. Main engine: not applicable.
- **#5 (closed) KH 1.5+2.5 save-mount loop on 10.01.** 4.xx-only save fixes were applied to higher-SDK builds; fixed in 0.3.3, unconfirmed on 10.01. Main engine: low.
- **#4 (open) Crisol (PPSA28603) saves not shown in Load Game, 9.00.** The klog shows the saves are written; Continue is greyed out and a manual save does nothing. Needs per-game investigation. Main engine: no.
- **#3 (closed) Castlevania: empty report.** Invalid.
- **#2 (open) MGS4 Master Collection.**
  - **Build failure**: 0.2.0 failed with `Invalid param magic` in m21.elf (the SDK pair-4 patch). This was fixed by later builds.
  - **Launch crash**: caused by the module-name case `Il2cppUserAssemblies.prx` vs `Il2CppUserAssemblies.prx`; fixed in 0.3.3.
  - **Black screen**: persists from USB too; the game waits behind a system dialog. The user reports a chance of getting in, never via FPKG.
  - **Main engine: yes.** Same patcher error class. The main engine's `_abp_zip_backups`/apply step writes files with the source name, so a case mismatch carried in the dump would remain.

---

## 6. Prioritised remaining defects for the Farewell Edition

I found nothing in the Critical category, meaning no unrecoverable loss of user data. Every path that can still delete or overwrite a finished output leaves either the intermediate or a rebuildable image behind. H1 is the closest: a finished .ffpfsc is deleted, but its intermediate is kept.

### High: false failures that delete finished output, or crashes

**H1. A finished .ffpfsc is deleted when the intermediate image cannot be removed (dump → exFAT/ffpkg → ffpfsc).**
- **Location**: UB `_dispatch_pfs` → inner `_after_convert` (UB 2340–2355): `ok2 = os.path.isfile(final_path) and not os.path.isfile(inter_path)`.
- **Trigger**: EB `_pipeline_convert_then_next` (EB 23616), Lazy success branch (~EB 23773), swallows `os.remove(image_path)` failures silently (the stock branch at ~EB 24268 logs them). If Defender, Explorer or a delayed OSFMount handle keeps the intermediate open, `ok2` is False, and `_uq_done` → `_uq_cleanup_job(reason='failed')` (UB 2682) deletes the **completed** `.ffpfsc` (type 'pfs', not pre-existing) after hours of compression. The intermediate is kept.
- **Fix**:
  1. Compute `ok2 = os.path.isfile(final_path) and os.path.getsize(final_path) > 0`.
  2. If `inter_path` still exists, log "could not delete the intermediate image <path>; delete it manually" and still report success.
  3. In the Lazy branch, log the `os.remove` exception the way the stock branch does.
  4. In `_uq_cleanup_job`, remove a `.ffpfsc` only when the job records that compression did not finish (set `job['_pack_ok']=True` on success and check it).
- **Linux**: yes. Lift `_uq_cleanup_job` and the ok2 rule into a pure helper and unit-test with temp files.

**H2. An unreadable volume root yields `dest_files=0`, a "copy failed" verdict, and an auto-retry that overwrites the image.**
- **Location**: PS1 `Get-TreeFileStats` (PS1 229–251); the per-directory `catch {}` also swallows failure of the *root* enumeration. EB `_build_output_is_empty` (EB 9821) rejects on `dest_files == 0`. `_item_done` (EB 9892) recurses to the failure path, and auto-retry (`retry_count` default 3, EB ~9982) reruns the BAT with `-ForceOverwrite`, which runs `Remove-Item` on the kept image (PS1 616). The "image was NOT deleted" log is then untrue.
- **Fix**:
  1. In the PS1, track whether the root enumeration succeeded and print `dest_files=-1` (or skip `[COPY-STATS]`) when it did not.
  2. In Python, treat `dest_files==0` as evidence only when `dest_bytes==0` too, and the PS1's own post-copy byte check did not pass. It passed whenever exit code 0 is reached, so the two signals contradict each other.
  3. Do not auto-retry a build rejected by the copy-accounting rule; leave the image and mark it CHECK.
- **Linux**: the Python half yes (the existing `OutputRejectionPolicy` tests); the PS1 half needs Windows, or pwsh with a mock directory.

**H3. The PS1 post-copy check counts files robocopy deliberately skipped, so a good image can be deleted.**
- **Location**: PS1 `Get-SourceStats` (PS1 314–377) sums *all* files. robocopy uses `/XF *.exfat /XF *.ffpkg` and optionally `/XA:H` (PS1 ~897–915). The check `usedBytes < RawBytes*0.90` (PS1 ~959–966) throws, and BAT `:ps_failed` runs `del /f /q "%IMAGE%"`.
- **Trigger**: a source folder holding a large `.exfat`/`.ffpkg`, or many hidden bytes with Exclude hidden on.
- **Fix**: exclude the same patterns in `Get-SourceStats` (or keep a separate `CopyBytes` total used by the check), and size the image from `CopyBytes`.
- **Linux**: logic review only; needs Windows/pwsh to run.

### Medium

**M1. The backport engine's fallback "byte-patch mode" writes bogus bytes and reports success.**
- **Location**: EB `_abp_patch_file` (EB 16744) and the fallback branch in `_abp_run` (EB 16223, "Fallback: basic byte-patch mode"). For an ELF it overwrites `e_flags` (0x30–0x31) with `sdk_int`; for a SELF it overwrites header bytes 0x18–0x1B. Neither is the SDK field the real patcher edits (`PT_SCE_PROCPARAM` SDK offsets). Every file is then logged as "✓ applied".
- **Trigger**: any failure to import the embedded `Backport.py` (for example, antivirus removing extracted files).
- **Fix**: remove the fallback. Fail with "Backport engine unavailable: <reason>", and restore the originals already copied to `originals/`.
- **Linux**: yes (unit test with a sample ELF header).

**M2. A stale `Backport.py` is reused when a temp folder is configured.**
- **Location**: EB `extract_backport` (EB 724). With `custom_temp` it writes into the user's temp *root* and skips `Backport.py` when it already exists, while `src/*.py` are always rewritten. After an update, an old `Backport.py` is combined with new `src` modules. The user-writable temp folder is also put on `sys.path[0]`, so any `*.py` there shadows modules.
- **Fix**: always extract to a fresh `tempfile.mkdtemp(prefix='exfat_builder_bp_', dir=custom_temp)`, always write every file, and remove the folder from `sys.path` after the run.
- **Linux**: yes.

**M3. Stale OSFMount mounts are not swept before builds (#82).**
- **Location**: `_sweep_stale_osf_mounts` (EB 9272) is called only by `_verify_image` (EB ~22939) and at close (EB ~3593).
- **Fix**: call it at the start of each build in `_process_next` (EB 10291), before the BAT is started, excluding nothing (no build is active). On attach timeout, log `osfmount.com -l`.
- **Linux**: mock test of the call order only.

**M4. Backport engine: no handling for `right.sprx` "Invalid param magic" and an existing dump `fakelib` with its own AMPR, PlayGo or AppContent libraries.**
- **Related issues**: Backport GUI #7, #8, #2, #27, #17.
- **Location**: EB `_abp_run` (EB 16223) fakelib overlay; the embedded `Backport.py::decrypt_and_sign_pipeline`.
- **Fix**:
  - Report patch-verification failures per file with the BGUI wording ("the file may still be encrypted").
  - Skip `sce_sys/about/right.sprx` or replace it with the standard copy, as BGUI 0.3.3 does.
  - Before overlaying, warn when `fakelib/` already exists, and offer to replace AMPR, PlayGo or AppContent.
- **Linux**: classification logic can be tested; the outcome needs a console.

**M5. Python-side drive-letter pickers ignore network mappings outside the elevated session (#64, #42).**
- **Location**: `_verify_image._pick_free_letter` (EB 22964), `_run_extract` (EB 11079), `_ffpkg_pick_free_drive` (EB ~4863), `ui/tab_convert.py:546,1130`, `ui/tab_files.py:1286`, `ui/tab_ffpkg_edit.py:3180`; all use `GetLogicalDrives` only.
- **Fix**: one helper `used_drive_letters()` combining `GetLogicalDrives`, `HKCU\Network` (winreg) and `GetDriveTypeW`, used everywhere, to match PS1 `Get-UsedDriveLetters`.
- **Linux**: the helper logic yes, with mocked winreg.

**M6. Legacy queue errors are never logged (#77).**
- **Location**: EB `_item_error` (EB 10066) logs `msg` only in unified mode.
- **Fix**: `self._log('[ERROR] %s\n' % msg)` at the top.
- **Linux**: yes.

### Low

- **L1. Non-int `SystemExit` counts as success.** `ui/experimental_mkpfs.py:545` `lazy_worker_main` returns 0 for a `SystemExit` with a non-int code; return 1 instead. Linux: yes.
- **L2. Extraction pre-scan cannot be skipped or cancelled (#71).** EB `_run_extract` (EB 11079): add a cancel check in the walk and an option to skip the pre-scan (use volume used bytes). Linux: partly.
- **L3. No stall notice or cancel in the build preflight (#84).** EB `_preflight_source` (EB 26166): log "no progress for 60 s in <dir>" and honour cancel. Linux: yes.
- **L4. Multi-folder drops on the source field are silently ignored.** EB `_on_drop` (EB 10150) uses `strip('{}')`; parse like `_on_queue_drop`, or take the first folder and log the rest. Linux: yes (pure parsing).
- **L5. The 4-worker mkpfs cap is undocumented (#41/#31).** EB `_pfs_pack_tuning` (EB 2367): expose an Advanced override or document the cap.
- **L6. The OSFMount path used for verification is not logged (#86).** The repeated "install OSFMount" prompt cannot be diagnosed from logs. Log the resolved `_find_osfmount()` path once per verification.
- **L7. Closable issues.** Issue hygiene, not code: close #60, #61 and #66 (already closed) as implemented, and #75 after the merge. Backport GUI #28 can be closed (fixed and confirmed).

### Can these be fixed and tested on Linux?

| Defect | Fix on Linux | Test on Linux |
|---|---|---|
| H1, M1, M2, M6, L1, L3, L4 | yes | yes, as unit tests with the `tests/_support.py` stubs |
| H2 | yes (Python half) | the PS1 half needs Windows |
| M5 | yes (helper logic) | the PS1 half needs Windows |
| H3, M3 | PowerShell / OSFMount changes | Windows needed to run them |
| M4 | logic yes | outcome needs a PS5 console |
