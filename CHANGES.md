# exFAT Image Builder — v3.0.0

**Released:** May 2026
**Previous public release:** v2.8.0 (May 13, 2026 — ShadowMount+ tab redesign)
**Status:** ✅ Stable

---

## 🎯 Headline changes

Three big features land in this release, plus a pile of supporting work
that finally makes the ffpkg side of the app match the polish of the
exFAT side:

1. **🛠 Apply Backport** — a one-click button on both Edit ffpkg and
   Edit exFAT that overlays a BackPork-style backport tree onto an
   existing image and rebuilds. Replaces the manual extract → patch
   → newfs dance.
2. **🔄 Convert tab gets ffpkg → exFAT** — the Convert tab previously
   only went exFAT → ffpkg. The reverse direction is now there too,
   so you can pull a ffpkg back out into a mountable exFAT image
   without firing up the UFS2Tool GUI.
3. **📊 Rich progress dialog with ETA** — every long-running operation
   (Apply backport rebuild, both Convert directions) now shows a
   proper progress dialog with stage label, live file count, byte
   throughput, elapsed time, and ETA — not just an indeterminate
   spinner.

---

## 💾 Disk-space requirements (READ THIS FIRST)

Several operations in v3.0.0 need a meaningful chunk of free space to
work, because UFS2Tool's pipeline produces an extracted dump folder
alongside the source and output. Plan for the following on whatever
drive your temp folder lives on (Settings → Temp folder):

| Operation | Free space needed (× source image size) | Why |
|---|---|---|
| **Extract image** (existing) | ~1.0× | One dump folder, no rebuild. |
| **Apply Backport** (Edit ffpkg / Edit exFAT) | **~2.1×** | Source image + extracted dump + new rebuilt image, with the source preserved as a `.bak` until success. |
| **Convert exFAT → ffpkg** | ~1.0× | Mounts the source in place via OSFMount; newfs streams directly to the output. |
| **Convert ffpkg → exFAT** | **~2.1×** | Source ffpkg + extracted dump + new ~1.1× exFAT image. |
| **Compare ffpkgs** | ~2.0× | Both inputs extracted side-by-side into temp dumps. |

**Worked example.** A 60 GB game ffpkg you want to apply a backport to
on firmware 4.51 needs roughly **126 GB** of free temp space during
the rebuild (60 GB original + 60 GB dump + ~6 GB new image while the
old one is still on disk as backup). The backup is removed once the
new image is verified, so the steady-state on-disk usage settles back
to ~60 GB once Apply Backport completes.

If your temp drive is tight, change it under **Settings → Temp folder**
to point at a roomier disk. The tool checks free space before starting
Apply Backport and warns if there's an issue, but it can't always know
the final size in advance, so over-provision.

---

## 🆕 New features in detail

### Apply Backport button (Edit ffpkg & Edit exFAT)

Both editor tabs grew a 📥 Apply Backport button in their toolbars.
Click it, point it at a BackPork-style backport folder (the kind
BestPig publishes — eboot.bin + downgraded sprx/prx siblings), and
the rebuild takes care of the rest:

1. **Backup** the source image as `<name>.bak`.
2. **Extract** the source via one `UFS2Tool extract <img> <dump_dir>`
   call. The recursive form preserves empty directories — important
   because PS5 games crash on missing `fakelib` folders and similar
   empty stubs.
3. **Overlay** the backport tree onto the dump. Files in the backport
   replace their counterparts; new files are added.
4. **Rebuild** with `UFS2Tool newfs -O 2 -b BLOCK -f FRAG -m MINFR
   -S 512 -i INODE -D <dump_dir> <new_img>`, preserving the source
   image's block/fragment/inode parameters so the rebuild is
   byte-equivalent to the original layout.
5. **Atomically swap** the new image into place; on any failure the
   `.bak` is restored.

The rebuild approach was chosen over per-file `UFS2Tool replace`
calls after a six-bug-fix iteration loop (see legacy notes
`CHANGES_v206_legacy.md` for the full history). The short version:
per-file edits silently dropped empty directories, which PS5 games
need to exist. Full rebuild via `extract → overlay → newfs` is the
only path that reliably produces byte-equivalent output.

### 🔍 Compare tool (Edit ffpkg)

Sister feature to Apply Backport. A 🔍 Compare button extracts two
ffpkgs, walks both dumps, and writes `<imageA>.compare.txt` listing
every difference: files only in A, files only in B, files in both
with different sizes / SHA-256 / mode / BSD flags. Confirms whether
your manually-edited image is byte-equivalent to one rebuilt via
Apply Backport. Used during development to validate the rebuild
approach was producing identical output to the manual flow.

### 🔄 Convert tab — ffpkg → exFAT direction

The Convert tab previously had a single card (exFAT → ffpkg) and a
docstring that said the reverse was "intentionally absent —
UFS2Tool's extract path was unreliable". That rationale no longer
holds: the Apply Backport flow has empirically validated UFS2Tool's
recursive extract works correctly.

New flow when you click **Convert to exFAT**:
1. `UFS2Tool extract <src.ffpkg> <dump_dir>` (preserves empty dirs).
2. Pre-scan the dump for total bytes and file count.
3. Allocate a blank `.exfat` file at `bytes × 1.10`, rounded up to
   64 MB alignment. The 10% covers exFAT cluster waste and directory
   metadata; the 64 MB alignment keeps OSFMount happy.
4. Find a free drive letter G–Z.
5. Mount the blank file read-write via OSFMount.
6. `cmd.exe /c format <letter> /FS:exFAT /Q /Y /V:` with `\n\n\n`
   piped to stdin so the "Insert new disk... press ENTER" prompt
   doesn't hang the worker. 3-minute timeout.
7. `robocopy <dump_dir> <letter>\` with `/E /COPY:DAT /DCOPY:DAT
   /R:1 /W:1 /NP /ETA`. Exit codes 0–7 are accepted (robocopy
   convention); 8+ is a real failure.
8. `app._dismount_drive_robust(letter, max_wait_seconds=20)`.
9. Clean up the temp dump folder.

Both Convert buttons cross-disable while one is running, so you
can't accidentally fire two conversions concurrently.

### 📊 Rich progress dialog

The new `_RebuildProgress` class is a `Toplevel` window with:

- **Bold stage label** that updates as the operation moves through
  its phases ("Mounting source image", "Building .ffpkg with
  UFS2Tool newfs", "Copying 1,847 files", etc.).
- **Detail line** showing live file counts and byte progress where
  available — e.g. `127 / 1,847 files  •  2.34 / 18.42 GB`.
- **Weighted progress bar** that scales each stage to a realistic
  fraction of total time. For Apply backport: backup 5%, extract
  30%, overlay 5%, newfs init 15%, newfs files 40%, swap 5%. For
  Convert exFAT → ffpkg: mount 5%, newfs 92%, dismount 3%. For
  Convert ffpkg → exFAT: extract 50%, prep 3%, copy 45%, dismount
  2%.
- **Elapsed time + ETA** derived from a rolling 15-second sample
  window of overall progress.
- **`-topmost` modal-style** — sits on top, ignores the close
  button mid-rebuild so the user can't accidentally orphan a
  half-finished image.

Sources of progress data:
- UFS2Tool newfs emits `Adding files... NN% (x/y files, X GiB/Y GiB)`
  on stdout — parsed live.
- UFS2Tool extract emits nothing useful, so a background poll thread
  watches bytes-on-disk in the dump folder vs the source ffpkg size,
  updating every ~0.7s.
- Robocopy's `New File` / `Newer` lines are counted against the
  pre-scanned total file count.

---

## 🧹 UI cleanup

### OUTPUT LOG no longer auto-opens

In every prior version the OUTPUT LOG drawer at the bottom of the
window auto-opened whenever a long-running operation started, when
an item failed in the queue, and on every single log line written
by the ffpkg and auto-backport flows. This was disruptive — the
drawer would pop up mid-task and obscure the actual progress
display.

In v3.0.0 the OUTPUT LOG only opens when the user clicks the
toggle label at the bottom of the window. Every internal call site
that previously forced it open has been neutered. The same policy
applies to the secondary OUTPUT LOG drawer inside the ffpkg Build
tab. If you want to follow operations in the text log, click the
toggle once — the drawer stays open from then on for the rest of
the session.

### Inspector panel: Replace and Delete buttons removed

Both **Edit ffpkg** and **Edit exFAT** had three action buttons at
the bottom of the right-hand Inspector pane: Replace, Rename, Delete.
Two of those (Replace and Delete) duplicated buttons of the same
name in the file-list toolbar at the top, which led to a busy,
cluttered right pane with two ways to do the same thing.

In v3.0.0:
- **Edit ffpkg Inspector**: only Rename remains (full-width button).
- **Edit exFAT Inspector**: only Extract remains (full-width button).
  Extract stays because it's the *one* selection-specific action
  that the toolbar doesn't offer.

The toolbar Replace and Delete buttons are unchanged.

### Convert tab subtitle and layout

- Page-head subtitle updated to "Convert between .exfat and .ffpkg"
  (was: "Convert an .exfat image into a .ffpkg image").
- Two cards stacked vertically, each with its own status line and
  Convert button. Both buttons cross-disable when either operation
  is running.

---

## 🔧 Internal / behind-the-scenes

- `_RebuildProgress` constructor now accepts optional `weights` and
  `initial_stage` kwargs, so any tab can instantiate it with
  appropriate stage weights for its own workflow. Backward
  compatible — existing call sites work unchanged.
- `_run()` in `tab_convert.py` gained an optional `progress_cb`
  parameter that's invoked for each output line, used to parse
  newfs and robocopy progress into the dialog.
- Per-file edit helpers in `tab_ffpkg_edit.py` (`_extract_to_backup`,
  `_bulk_add_new_subtrees`, `_ufs_get_mode`, `_ufs_set_mode`,
  `_ufs_chmod_*`, `_ffpkg_extract_walk`) are retained as dead code
  for now. They were superseded by the full-rebuild approach in
  v2.0.6f when it became clear that custom directory walks silently
  dropped empty directories. Kept around in case a future surgical
  edit path needs them.

---

## 🐛 Bugfixes from the v2.0.6 development series

The internal v2.0.6 sub-series (v2.0.6a → v2.0.6i) iterated through
nine builds while getting Apply Backport right. The fixes that
shipped:

- **v2.0.6f — Empty directories preserved during rebuild.** The
  initial Apply Backport implementation used a custom
  `_ffpkg_extract_walk` that only emitted files, silently dropping
  empty directories. PS5 games crashed at startup on the missing
  `fakelib` folder. Fix: use `UFS2Tool extract <img> <dump>` directly
  (the recursive form preserves empty dirs natively).
- **v2.0.6f — newfs parameters preserved from source.** Rebuilt
  images were using default newfs parameters instead of the source
  image's block size / fragment size / minfree / inode density.
  Fix: probe the source image with `UFS2Tool stat /` before extract,
  reuse those values on the newfs call.
- **v2.0.6g — Compare reports byte-identical when actually identical.**
  Validated against Hot Shots Golf 2: a manually-edited ffpkg and an
  Apply-Backport-rebuilt ffpkg compared as byte-equivalent. The
  earlier crash on that title was traced to a stale OSFMount cache,
  not the rebuild output.
- **v2.0.6h — Progress dialog can't be closed mid-rebuild.**
  `WM_DELETE_WINDOW` is hooked to a no-op so the user can't
  accidentally orphan a half-finished image. The dialog goes
  through atomic swap and only then auto-closes.

Full per-sub-version detail is in `CHANGES_v206_legacy.md` for
reference; you don't need to read it.

---

## 📦 Files in this release

- `exfat_builder.py` — main entry point (version bumped to 3.0.0,
  log auto-open neutered).
- `ui/tab_ffpkg_edit.py` — Edit ffpkg tab, Apply Backport, Compare,
  `_RebuildProgress` class (now reusable).
- `ui/tab_files.py` — Edit exFAT tab, Apply Backport, Inspector
  cleanup.
- `ui/tab_convert.py` — Convert tab, both directions, rich progress
  dialog integration.
- `CHANGES.md` — this file.
- `CHANGES_v206_legacy.md` — preserved sub-version notes from the
  v2.0.6 development series.
- `RELEASE.md` — step-by-step instructions for pushing this
  release to GitHub.
- `X_STATUS.md` — three pre-written social posts for announcement.

Drop these into your existing v2.8.0 / v2.9.0 install, overwriting
the same paths. The version string in the app's header will read
`v3.0.0` on next launch.

---

## ⚠️ Notes

- **BackPork payload still required** for backported games to run
  on PS5. See https://github.com/BestPig/BackPork.
- **OSFMount still required** for any operation that mounts an
  image (Edit exFAT, both Convert directions). The tool will tell
  you if it's missing.
- **UFS2Tool is bundled** — no separate install needed.
- **.NET 8 Runtime** (not SDK) is needed for UFS2Tool. The app
  detects and prompts if it's missing.

---

*— DecKerr97*
