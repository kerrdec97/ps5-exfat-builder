# PS5 exFAT Image Builder: technical audit (reliability)

Branch `farewell-edition-dev`, HEAD `db14125`. The branch gained a commit during the audit, so line numbers are approximate. Function names are the stable reference. The repo was read only. Experiments ran in the scratch area.

**Method.** I read the build, verify, pipeline, close and update paths. I decoded the embedded `make_image.bat` and `New-OsfExfatImage.ps1`. I scanned every `threading.Thread` target with AST scripts. I installed `mkpfs==0.0.9` (the pinned version) in a scratch venv to check how its exit codes behave. The unit tests pass in a copy: `python3.12 -m unittest discover -s tests` gives 51 tests OK, 1 skipped, under xvfb.

**Scope note.** The earlier file-deletion review said Build-queue cleanup "keeps completed exFAT/ffpkg images". That holds only for `reason='failed'`. On cancel, a completed exFAT/ffpkg image **is** deleted (see H4).

## Summary

| # | Sev | Area | Location | One-line |
|---|-----|------|----------|----------|
| C1 | **Critical** | Verify / delete | `ui/tab_pfs.py` convert `worker` (~L1776) | mkpfs rc=1 (post-pack verify FAILED) is treated as success, and the source image may then be deleted |
| C2 | **Critical** | Verify / delete | `exfat_builder.py:_verify_done_final` (~L23345), `_ffpkg_item_done` (~L7760) | "Delete source folder" runs after skipped or inconclusive verification (exFAT) and with no verification at all (ffpkg) |
| H1 | High | Verify | `_verify_image` (~L23130), `_verify_done._retry_verify`; PS1 copy step | Up to 0.5 % of bytes and 2 files missing still give "Verified ✓"; robocopy exit 8-15 (failed files) is only a warning |
| H2 | High | Verify | `_verify_image_mkpfs_native` | Native `mkpfs verify --source-dir` ignores the build's exclusions, so it gives false "genuine FAILURE" verdicts (reproduced) |
| H3 | High | Threads / close | `_on_close`, `_process_next.worker`, `_ffpkg_process_next.worker`, `ui/mkpfs_runner._run_mkpfs_subprocess` | Build child processes are never tracked or killed; closing leaves powershell/robocopy/UFS2Tool/mkpfs running and force-dismounts the active image |
| H4 | High | Cancel / delete | `ui/tab_unibuild.py:_uq_cancel`, `_uq_done`, `_uq_cleanup_job` | Cancel cannot stop exFAT/ffpkg; the job runs to the end and the **finished** image is then deleted |
| H5 | High | Status honesty | `_process_next` end (~L10317), `_ffpkg_process_next` end | "N image(s) built successfully!" counts queued items, not successes |
| H6 | High | Partial output | `_ffpkg_item_done`, `_uq_cleanup_job` | A failed ffpkg leaves a half-written `.ffpkg` under its final name, and the unified cleanup "keeps" it; ffpkg is never verified |
| M1 | Medium | Status honesty | `_item_done`, `_verify_done_final` | "Complete!" / history `success=True` are written before verification; inconclusive results are shown in green and as `done` |
| M2 | Medium | Data safety | PS1 `-ForceOverwrite`, bat `:ps_failed`, `_dispatch_exfat` | Rebuilding over an existing image deletes the old image first; if the new build fails, both are gone |
| M3 | Medium | Threading | `_detect_game_info`, `_ffpkg_detect_game`, .NET-check `worker` (~L7542); widespread `x.after()` from workers | Worker threads build widgets and dialogs directly; a stale-result race can give an image the wrong name |
| M4 | Medium | Perf (UI thread) | `_run_queue`, `_verify_done`, `_choose_intermediate_dir`, `_verify_done_final`/`_ffpkg_item_done` rmtree, `_on_close` sweep | Full-tree walks and rmtree run on the Tk thread, so the window shows "Not responding" |
| M5 | Medium | Mounts | `_sweep_stale_osf_mounts`, `_dismount_drive_robust` | Force-dismounts any OSFIMG/PS5DATA/PS5IMG volume, including mounts the user or another tab owns |
| M6 | Medium | Security / update | `_auto_update` | SHA-256 is optional and fails open; no timeout; not gated on `sys.frozen`; `destroy()` skips `_on_close` and ignores running builds |
| M7 | Medium | Security | `extract_scripts`, bat, `extract_ufs2tool`, `extract_backport` | The elevated app runs scripts and exes from user-writable temp; bare `powershell.exe` is resolved from the cwd; Backport.py goes into the temp root and onto `sys.path[0]` and is never refreshed |
| M8 | Medium | Duplicate pipelines | `ui/tab_convert.py` ffpkg→exFAT, `ui/tab_pfs.py`, legacy vs unified | Different format flags, success criteria, verification and delete-source rules for the same operation |
| M9 | Medium | Temp files | mkpfs `.tmp`, `_move_final` `.part`, `extract_ufs2tool`, `extract_backport` | No startup recovery; extract dirs leak on every call; orphaned workers keep writing |
| M10 | Medium (suspected) | Encoding | build `Popen` (~L10542) `encoding='mbcs'`; ffpkg `utf-8` | Console tools write in the OEM code page, so non-ASCII names may be garbled in logs and "current file" |
| L1 | Low | Settings / history | `load_settings`, `_save_history` | A corrupt settings file is silently reset; non-dict JSON crashes every start; history is written non-atomically and reset on any read error |
| L2 | Low | Shortcuts | `_kbd_cancel`, `_kbd_build` | Esc calls methods that don't exist; Ctrl+B/Esc do nothing on the main Build tab |
| L3 | Low | Dead code | `_verify_done` retry branch | Never runs: it is gated on `'file count'`, which only appears in success messages |
| L4 | Low | Dead modules | `ui/page_head.py`, `ui/theme_tokens.py`, `ui/shared/{dashboard,forms,progress,ps5_kit,table}.py` | 1,131 lines with zero imports, still bundled |
| L5 | Low | Error handling | repo-wide | 1,323 single-`pass` handlers; the risky ones are listed in §5 |
| L6 | Low | Misc | `relaunch_as_admin`, log ordering, robocopy % parsing, `_clear_temp` | Small correctness gaps |

---

## Critical

### C1. A failed mkpfs post-pack verification is reported as success, and the source can be deleted
**Location:** `ui/tab_pfs.py`, Convert-to-.ffpfsc panel `worker` (~L1760-1800). This panel is reached from the PFS tab and from the "Convert to .ffpfsc?" hand-off after legacy exFAT/ffpkg builds.

```python
argv2 = ['pack','file', ...] + (['--verify'] if do_verify else []) + ...
rc = run_mkpfs(argv2, ...)
file_ok = os.path.exists(actual) and os.path.getmtime(actual) >= _pack_started - 5
if rc == 0 or (rc == 1 and file_ok):          # <-- rc 1 accepted
    ...
    if app._settings.get('pfs_delete_source_after_convert', False) and file_ok ...:
        os.remove(src)                         # source image deleted
    ... prog_c.set_status('Done ✓ ...'); app_info(parent, 'Convert complete', ...)
```

**Evidence.** In mkpfs 0.0.9 (`cli.py: _run_pack_build` and `cli_mkpfs_pack_file_run`), the post-pack structure verification runs by default and `--verify` turns on full payload verification. The verifier returns `1 if errors else 0` **after the output file has been written**. So `rc==1 and file_ok` means exactly "the image was written and then failed verification".

**Impact.** A corrupt `.ffpfsc` is reported as "Done ✓ / Convert complete". If "delete source after convert" is on, the only good `.exfat`/`.ffpkg` is removed. This is data loss. The unified pipeline (`_pipeline_convert_then_next`) correctly requires `rc == 0`, so the two paths disagree.

**Fix.** Use `if rc == 0:`. Treat any other rc as a failure, keep the source, and rename the output to `*.failed` or delete it. Never run the delete-source branch unless `rc == 0`.

### C2. Source folder deleted without a content-verified pass
**Location:** `exfat_builder.py:_verify_done_final` (~L23345, `if self._adv_delete_source_var.get(): ... _sh.rmtree(src)`), `_ffpkg_item_done` (~L7760).

`_verify_done_final(ok=True, ...)` deletes the source dump when "Delete source folder after exFAT build" is on. `ok=True` comes from all of these:
- `_item_done`: when verification is disabled → `self._verify_done_final(True, 'Build complete (verification skipped)', ...)`. **Nothing was verified.**
- `_verify_image`: `return True, 'OSFMount not found — skipping verify'`; `return True, 'No free drive letter — skipping verify'`; `return True, 'Could not find drive letter: ...'`.
- `_verify_image`: the mount failed 4×, then a header-only check (`_verify_image_file_only`: size ≥ 64 MB plus the `EXFAT   ` magic) → `return True, 'Mount verification could not run ... NOT mounted and contents were not compared'`.
- `_verify_image`: the drive never became readable within 90 s → `return True, 'Verification was inconclusive ...'`.
- The tolerant comparison in H1.

`_ffpkg_item_done` deletes the ffpkg source folder on `returncode == 0` with **no verification of any kind**.

The unified Build tab runs exFAT jobs through the same `_item_done` → `_verify_done_final`, and nothing checks `_unified_active` there. So the global Danger-Zone toggle also applies to unified builds.

**Impact.** The user's only copy of a dump is permanently deleted after a build nobody verified. The rmtree also runs on the Tk thread (M4).

**Fix.** Return a tri-state from `_verify_image`: `PASS | FAIL | INCONCLUSIVE`. Delete only on `PASS` from a content comparison (mount walk or mkpfs). When verification is skipped or inconclusive, or for ffpkg until it has a verifier, skip the delete and log why. Run the rmtree in a worker thread.

---

## High

### H1. The tolerant comparison passes images with missing data; robocopy failures are downgraded
**Locations:** `_verify_image` (~L23125-23140), `_verify_done._retry_verify` (same thresholds), and `New-OsfExfatImage.ps1` (decoded from `_PS1_B64`), copy step:

```powershell
if ($robocopyExitCode -ge 16) { throw ... }
elseif ($robocopyExitCode -gt 7) { Write-Host "[WARN] robocopy completed with warnings ... The image has been created." }
...
$expectMin = [Int64]($stats.RawBytes * 0.90)        # 90 % of bytes is "OK"
```
```python
size_ok  = img_bytes >= src_bytes * 0.995
count_ok = (src_count <= 0 or img_count >= src_count - 2)
```

**Reasoning.** Robocopy exit codes 8-15 mean at least one file **failed** (for example ERROR 32, file locked by AV or Search). The PS1 exits 0. `_build_output_is_empty` flags only `dest_files == 0`. Take a 100 GiB game with one 400 MiB file that failed to copy: 99.6 % of the bytes are present and the count is short by 1, so the result is `Verified ✓ ... 99.61/100.00 GB confirmed`. The code comment itself says "the total byte count is exact", and exFAT reports logical sizes, so the 0.5 % slack has no justification. `verify_walk_tree` already returns per-path `sizes` maps, and `describe_missing_files` already computes the exact differences, but they are only used for the failure message.

**Impact.** False success on an incomplete image. This feeds C2 (source deletion) and pipeline deletion of the intermediate.

**Fix.** After the walk, `missing = describe_missing_files(src_sizes, img_sizes)`; fail if it is non-empty. Keep a count tolerance only for directory or metadata entries. Also parse `robocopy_exit` from `[COPY-STATS]` (already in `self._copy_stats`) and treat ≥ 8 as at least `warn`.

### H2. Native MkPFS verify gives false negatives and has no fallback
**Location:** `_verify_image_mkpfs_native`, used for **exFAT** builds whenever the global `pfs_backend == 'mkpfs'`.

It runs `mkpfs verify <img.exfat> --source-dir <raw source>`. mkpfs 0.0.9 `verify_exfat_image` hashes **every** source file and skips only names starting with `.` plus `thumbs.db`/`desktop.ini`. The build excluded `*.exfat`/`*.ffpkg` (robocopy `/XF`) and, optionally, hidden files (`/XA:H`). `verify_walk_tree` mirrors those exclusions; the native path does not.

**Reproduced** with mkpfs 0.0.9 in a scratch venv: the source holds `eboot.bin`, `sce_sys/param.json` and `old_build.ffpkg`. I packed the robocopy-equivalent tree, then ran verify against the original source. It printed `❌ missing files in exFAT image: old_build.ffpkg` with rc=1. `classify_verify_result` maps that to `FAIL`. The app then logs "genuine failure ... Rebuild the image" and deliberately does not fall back to the mount check. Dumps that contain leftover images are a known pattern; `tab_pfs.py` has a warning for exactly this case.

**Fix.** When the source contains excluded files, or exclude-hidden is on, either pass a filtered hard-link staging tree as `--source-dir`, or treat a FAIL whose only errors are "missing files" matching the exclusion set as PASS. Otherwise return `done=False` so OSFMount verify runs.

### H3. Child processes are not tracked; closing leaves orphans and yanks the active mount
**Locations:**
- `_process_next.worker`: `proc = subprocess.Popen('cmd.exe /d /s /c "..."')` is a local variable.
- `_ffpkg_process_next.worker`: UFS2Tool `proc` is local.
- `mkpfs_runner._run_mkpfs_subprocess`: `proc` is local and terminated only via `cancel_check`.
- `_on_close` terminates only `_extract_active['robo'/'mount']`. It then calls `cleanup_scripts()` and `_sweep_stale_osf_mounts(log_prefix='[CLOSE]')` with **no `exclude_letter`**, and then `destroy()`.
- `_kbd_cancel` calls `self._cancel_build()` / `self._ffpkg_cancel()`, and **neither method exists**.

**Impact.** Closing during an exFAT build (after the "Close anyway" prompt) leaves `cmd → powershell → robocopy` running. The sweep force-dismounts the build's `OSFIMG` volume mid-copy; the orphaned PowerShell then fails and its bat deletes the image. Closing during ffpkg leaves UFS2Tool writing a partial `.ffpkg` under its final name. Closing during PFS compression leaves the frozen `--__mkpfs_worker__` (and its Pool) writing a multi-GB `.ffpfsc.tmp` for possibly hours, which nothing finalizes or cleans up. Daemon threads die with the interpreter, so `_move_final` can leave a `.part` file.

**Fix.** Keep a registry `self._child_procs` (add on Popen, discard on wait). On close or cancel, kill each process **tree**: a Windows Job Object with `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`, or `psutil.Process(pid).children(recursive=True)` then terminate. Wait, then dismount. Pass the active letter as `exclude_letter` only if the copy is still running. Implement `_cancel_build`.

### H4. Unified cancel cannot stop exFAT/ffpkg, then deletes the finished image
**Location:** `ui/tab_unibuild.py:_uq_cancel` sets `uq_state['cancel']=True; uq_state['running']=False`. Nothing in `_process_next`, `_item_done` or `_ffpkg_process_next` reads these flags (grep confirms), so the exFAT/ffpkg image builds **to completion** and is verified. When the callback arrives, `_uq_done` sees `uq_state['cancel']`, sets `job['status']='cancelled'` and calls `_uq_cleanup_job(job, 'cancelled')`:

```python
if job.get('type') in ('exfat','ffpkg') and reason != 'cancelled':   # keep branch skipped on cancel
    ...
else:
    fp = job.get('_final_path') ...
if fp and os.path.isfile(fp) and job.get('status') in (..., 'cancelled'):
    ... os.remove(fp)            # _final_preexists keep-check fails: the PS1 already replaced the file
```

**Impact.** The cancel dialog promises "finishes the current file first, then stops … Jobs already finished are kept". In reality the whole image builds (possibly an hour), and then the completed, verified image is deleted. If an older image with the same name existed, the PS1 `-ForceOverwrite` already removed it (M2), so both are gone. The UI also re-enables "Build" (`running=False`) while the old job is still writing. The PFS stage is cancelled correctly through `cancel_check`.

**Fix.** Do the real termination from H3. Delete only if the job was interrupted, i.e. its process was killed before rc==0. If the build completed, mark it `done`/`kept` and leave it. Reword the dialog to match.

### H5. The queue-end summary claims all items succeeded
**Location:** `_process_next` when `pos >= len(indices)`: `_set_status('All done! ✓ N image(s) built', SUCCESS)` and `app_info('… N image(s) built successfully!')`. `_ffpkg_process_next` does the same: `'%d ffpkg image(s) built successfully!' % len(indices)`.

When the user picks "Continue with remaining items?" after a failure, or when verification flags an item (`warn`), the final message still reports every item as successful. Auto-shutdown (`_trigger_shutdown`) also fires, and it does so while the optional SHA-256 `_chk` thread may still be hashing.

**Fix.** Count `status == 'done'`, `'warn'` and `'failed'` and report each. Run shutdown only after background verify/checksum threads finish.

### H6. Partial ffpkg left under the final name; ffpkg never verified
**Location:** `_ffpkg_process_next` runs `UFS2Tool newfs ... -D <folder> <out_path>` directly into the output folder. The failure branch of `_ffpkg_item_done` never removes `out_path`. The unified `_uq_cleanup_job` comment says "The builders already delete genuinely partial images themselves", which is true only for exFAT (the bat's `:ps_failed`). It logs the partial ffpkg as "kept … for inspection".

**Impact.** A truncated `.ffpkg` with a normal name sits in the output folder and can later be uploaded, converted or mounted.

**Fix.** Write to `<name>.ffpkg.part`, rename on rc==0, delete or rename to `.failed` otherwise. Add at least a size and superblock check, or a UFS2Tool `ls` of the root.

---

## Medium

### M1. Success is announced before verification; inconclusive maps to success
`_item_done` (rc==0) sets the queue dot to `done`, sets progress to 100 % "Complete!", logs `[OK] Done:`, calls `_save_history(..., True)` and writes summary.json, **then** starts the verify thread. If verification fails, the history entry stays `success: True`. `_verify_done_final(ok=True)` calls `_set_status(msg, SUCCESS)`, so "OSFMount not found — skipping verify" and "Mount verification could not run…" appear in green, and the unified job becomes `done` (only `ok=False` becomes `warn`).

**Fix.** Show "Built — verifying…" until the verdict arrives. Write history after verification with `verified: pass|fail|inconclusive|skipped`. Give the inconclusive state its own colour and label.

### M2. Rebuilding over an existing image destroys it before the new one exists
PS1: `if (Test-Path $ImagePath) { ... Remove-Item -LiteralPath $ImagePath -Force }` runs at the start; the bat always passes `-ForceOverwrite`. On a non-zero exit, `:ps_failed` runs `del /f /q "%IMAGE%"`. `_dispatch_exfat` and `_dispatch_ffpkg` build straight into `outdir/name`. The `_final_preexists` bookkeeping in `_uq_dispatch` cannot help, because the file is gone before the first log line. The legacy checklist warns "will be overwritten", but the old image is lost even when the new build fails.

**Fix.** Build to `name + '.building'` in the same directory, then `os.replace` it onto the final name after rc==0 and a verify pass.

### M3. Worker threads touch Tk directly; stale-result race
- `_detect_game_info` and `_ffpkg_detect_game` run as thread targets (via `*_async`). They create `ImageTk.PhotoImage`, call `.config` / `.pack` / `.pack_forget`, and set `output_name`, `_info_*_var` and `_set_status`. There is no generation check. Select folder A (slow USB), then folder B: if A's worker finishes last, it overwrites `output_name` with A's title while `game_folder` is B, so the image gets the wrong name.
- In the .NET-check `worker` (~L7500-7570), the "not found" branch builds a `tk.Toplevel` with `grab_set()` and widgets on the worker thread.
- The `__init__` comment (~L3730) says workers "can't safely call after()". Yet the main build loop does `for line in proc.stdout: self.after(0, self._handle_line, line)`, and about 100 other workers use `self.after`, `parent.after` or `win.after`. CPython's threaded Tcl (Windows builds) marshals these calls, so I could not confirm Tcl state corruption. But each call blocks the worker until the Tk thread services it: a stdout reader stalls whenever the UI thread is busy (M4), which back-pressures robocopy. After `destroy()` the calls raise and the threads die silently. Log lines also interleave out of order, because `after(0,_log)` writes immediately while `_log` from workers goes through the 120 ms queue pump.

**Fix.** Give `_detect_*` a generation token (like `_space_gen` in unibuild). Return plain data from workers and post one closure through a thread-safe queue drained by the existing pump (a generic `self._ui(fn,*a)`). Build the .NET dialog in a function posted to the UI thread.

### M4. Blocking I/O on the UI thread
- `_run_queue`: `_get_folder_size(item.game_folder)` per item (space check), then again per item for the ETA.
- `_verify_done`: `_get_folder_size(game_folder)` (corrupt-size check).
- `ui/tab_unibuild._choose_intermediate_dir`: `_get_folder_size(src_folder)` at dispatch.
- `_verify_done_final`, `_ffpkg_item_done`: `shutil.rmtree(src)` (whole dump).
- `_on_close`: `_sweep_stale_osf_mounts`, up to 15 s per drive.
- `__init__`: `_refresh_temp_size` walks every `exfat_builder_*` temp dir.

On a 100k-file HDD or network dump, each walk takes seconds to minutes.

**Fix.** Reuse `preflight['bytes']` (already captured as `_last_source_size`) or the unibuild `space_state`. Move the remaining walks and rmtree into workers.

### M5. Global sweep force-dismounts volumes the app doesn't own
`_sweep_stale_osf_mounts` dismounts every drive labelled `OSFIMG`/`PS5DATA`/`PS5IMG`. It runs before **every** verify (excluding only `self._mounted_drive`) and on close (excluding nothing). `_dismount_drive_robust` tries `osfmount -D` (forced) **first**, and later `FSCTL_DISMOUNT_VOLUME`. Images built by this app carry `OSFIMG`. So a user's own OSFMount session, or a read-write mount from the Files tab, ffpkg editor or Micromount, is force-dismounted, possibly with pending writes.

**Fix.** Track the letters this process mounted. Sweep only those, or ask first. Try a graceful `-d` before `-D`.

### M6. Auto-updater integrity and lifecycle (`_auto_update`)
- SHA-256 runs only if the release ships a `.sha256` asset, and an error fetching or parsing it is only logged (`self._log('[UPDATE] SHA256 verify could not run…')`). The check fails open. There is no Authenticode check.
- `sha_url` is the last asset ending `.sha256` and is not matched to the chosen exe name.
- `urllib.request.urlretrieve(exe_url, …)` has no timeout, so a stalled connection hangs the dialog forever with no cancel.
- `_check_for_updates` runs on boot by default and is not gated on `sys.frozen`. From source, `sys.executable + '.new'` and the batch `move` would replace **python.exe**, as admin.
- `win.after(1500, lambda: (Popen(bat), self.destroy()))` skips `_on_close` (no settings save, mount sweep or log close) and does not check `_building`, `_unified_active` or `_ffpkg_building`.

**Fix.** Require `<exe name>.sha256` and fail closed. Download with `urlopen(timeout=…)` in chunks. Return early unless `getattr(sys,'frozen',False)`. Refuse while a build is running, and close through the `_on_close` path.

### M7. The elevated process executes from user-writable locations
The process is always elevated (module level ~L837). `extract_scripts` writes the bat and PS1 into `%TEMP%\exfat_builder_*` (or the custom temp) for the whole session. Each build runs `powershell.exe -ExecutionPolicy Bypass -File <that ps1>`. A same-user, unelevated process can rewrite the PS1 and gain admin rights; this is the classic elevated-reads-from-user-writable pattern.

The bat calls bare `powershell.exe` with `cwd=bat_dir`. cmd.exe searches the current directory before PATH, so a planted `powershell.exe` in that directory wins.

`extract_ufs2tool` writes `UFS2Tool.exe`/`.dll`/`DokanNet.dll` into temp and runs them elevated. `extract_backport(custom_temp)` writes `Backport.py` and `src/` **into the root** of the user's temp folder. It skips `Backport.py` if it already exists (`if not os.path.isfile(bp_path)`), so an old or planted file is used forever after updates. The callers then `sys.path.insert(0, bp_tmp)`, so any `*.py` in that folder can shadow later imports.

Python `subprocess` calls with bare names (`dotnet`, `robocopy.exe`, `shutdown`) also search the exe's own folder (often Downloads) first.

**Fix.** Extract to a per-run `mkdtemp` under a directory with an admin-only ACL, or verify SHA-256 of the extracted files immediately before each launch. Call `%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe` by full path. Always overwrite `Backport.py` and use a private subdirectory. Import it with `importlib.util.spec_from_file_location` instead of `sys.path[0]`.

### M8. Duplicate pipelines behave differently
| Operation | Unified / Build | Other path | Difference |
|---|---|---|---|
| Folder → exFAT (format) | PS1 `format.com /FS:exFAT /A:<cluster> /Q /V:OSFIMG /X /Y`, FS check, FlushFileBuffers, COPY-STATS, mount verify | `tab_convert.py` ffpkg→exFAT: `cmd /c format X: /FS:exFAT /Q /Y /V:` | No cluster size, no label (so the sweep can't recognise it), no `/X`, no flush, **no verification**; robocopy `rc<8` accepted; "Convert complete" shown |
| exFAT/ffpkg → .ffpfsc | `_pipeline_convert_then_next`: success only on `rc==0` | `tab_pfs.py` convert worker: `rc==0 or rc==1` (C1) | False success, source deletion |
| Delete source | exFAT: after verify, even inconclusive (C2) | ffpkg: after rc==0 only; PFS: after rc 0 or 1 | Three different rules |
| Cancel | PFS honours `cancel_check` | exFAT/ffpkg ignore it (H4) | — |
| Staging | PFS output staged and `_move_final`'d | exFAT/ffpkg written in place (M2/H6) | — |

**Fix.** One `finalize_image(out_path, src, kind)` helper covering verify, status and delete-source, used by every path.

### M9. Temp and partial files: no recovery after an interruption
These all survive a crash or close with no sweep on the next start:
- mkpfs writes `pack_out + '.tmp'` (see `_pfs_preflight_check`).
- `_move_final` writes `dst + '.part'`.
- A staged `.ffpfsc` can sit in temp.
- Every `extract_ufs2tool()` call creates a new `exfat_builder_ufs2_*` directory and overwrites the global, so `cleanup_ufs2tool()` removes only the last one. `ui/tab_files.py` calls it unconditionally (~L1138), as does `tab_convert`.
- `exfat_builder_bp_*` directories from `extract_backport` are never removed.

`clear_temp_folder` is manual, skips anything newer than 24 h, and `_clear_temp` checks `_building` only after the confirm dialog, ignoring `_ffpkg_building`, `_unified_active` and running PFS converts.

**Fix.** At startup, list `*.part`, `*.ffpfsc.tmp` and stale `exfat_builder_*` in the temp and output folders and offer cleanup. Reuse a single UFS2Tool extraction per session. Remove backport dirs after use.

### M10. Child-output code page (suspected)
The build `Popen` (~L10542, also ~L4937 and ~L5129) decodes `cmd`/PowerShell/robocopy output as `mbcs` (the ANSI code page, cp1252 in the West). Redirected console programs (cmd `echo`, robocopy, PowerShell 5.1 `Write-Host` to a pipe) normally use the **OEM** code page (cp437/850). The updater already writes its batch file as `'oem'` for this reason. The likely result is garbled non-ASCII names in the log and in the "current file" telemetry. Parsing is ASCII-anchored, so verdicts are unaffected. UFS2Tool (.NET) is decoded as UTF-8 and probably writes OEM too. I could not confirm this without Windows.

**Fix.** Decode as `'oem'`, or set `[Console]::OutputEncoding=[Text.UTF8Encoding]::new()` at the top of the PS1 and decode as UTF-8.

---

## Low

- **L1. Settings / history.** `save_settings` is atomic (tmp, fsync, `os.replace`), and all callers are on the Tk thread, so I found no concurrent writers. However:
  - On a corrupt file, `load_settings` renames it to `.corrupt` and returns `{}` without telling the user (output dir, FTP IP and other settings vanish).
  - Valid JSON that isn't a dict (e.g. `[]`) passes through `_migrate_settings` unchanged, so `self._settings.get` crashes on every start.
  - A failing `os.replace` (AV lock) is written only to `sys.stderr`, which is `None` in the windowed build.
  - `_save_history` writes non-atomically and replaces the history with `[]` after any read error.
  - Under over-the-shoulder UAC, `~` resolves to the admin account's profile.

  **Fix:** check `isinstance(data, dict)`, keep a `.bak`, show a one-time toast, and write history the same way as settings.
- **L2.** `_kbd_cancel` calls `_cancel_build` and `_ffpkg_cancel`, which don't exist. `_kbd_build` / `_kbd_cancel` key on the legacy `'build'` tab, but the visible Build tab is `'unibuild'`. The shortcuts dialog advertises "Esc — Cancel running build".
- **L3.** The retry in `_verify_done` runs only when `'file count' in msg.lower()`. That phrase appears only in *success* messages, so the retry is unreachable. Delete it or key it on the failure text.
- **L4. Dead modules** (zero imports anywhere in the app, tools or tests, but bundled by `--collect-submodules ui`): `ui/page_head.py` (203 lines, superseded by `ui/shared/page_head.py`), `ui/theme_tokens.py` (170), `ui/shared/dashboard.py` (137), `forms.py` (249), `progress.py` (171), `ps5_kit.py` (200; referenced only in a docstring), `table.py` (1). The hidden legacy tabs (`_build_build_tab`, `_build_ffpkg_tab`, `_build_pfs_tab`) are **not** dead: the unified tab drives builds through their widgets and state.
- **L5.** There are 1,323 `except …: pass` handlers. The risky ones in critical paths:
  - `_on_close` swallows a `save_settings` failure.
  - `_uq_cleanup_job` removal errors are silent.
  - In `_item_done`, an exception from `_build_output_is_empty` is treated as "not empty" (fails open).
  - A `_pfs_preflight_check` "guard error (continuing)" lets the pack run (fails open).
  - The SHA-256 check in the updater fails open (M6).
  - `_verify_done._retry_verify` logs an exception and falls back to the original verdict.
- **L6.**
  - `relaunch_as_admin` quotes arguments as `'"'+a+'"'`, so an argument ending in `\` breaks.
  - In `_handle_line`, `r'(\d{1,3})%'` also matches robocopy's per-file `12.5%` (the bare-percent filter requires `^\d{1,3}%$`). When the drive poller is inactive, a per-file percentage can drive the overall bar (suspected).
  - `_rebuild_all_tabs` (language Apply) is guarded against exFAT/ffpkg/unified builds but not against a running PFS convert queue, Extract or FTP transfer.

## Items checked and found sound
- Subprocess timeouts: every `subprocess.run` has one, except the `shutdown`/`rundll32` one-shots.
- `shell=True` is never used. The build bat command is quoted and rejects paths containing `"`, warns on `%`, and uses goto-style error handling so `)` in paths is safe.
- `mkpfs_runner` handles cancel: terminate, 5 s wait, then kill.
- `_move_final` (already reviewed) uses a verified copy.
- Locale independence of copy accounting: `[COPY-STATS]` plus the structural robocopy parser.
- `_verify_image` waits up to 90 s for the drive to appear and mounts read-only (`ro,rem`).
- `_uq_build_all` checks space per drive.

## Not covered or not confirmable here
Windows-only behaviour (OSFMount, format.com prompts in non-English locales, code pages, Job Objects) was assessed from code only. I did not trace the Extract tab's own verification, FTP upload atomicity, or the Lazy_MkPFS exit-code semantics.
