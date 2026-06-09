# Changelog

## [3.6.3] - 2026-06-09

A major update focused on faster workflows, smarter progress tracking,
improved extraction performance, and a cleaner user experience.

### Added
- **Optional multithreaded Robocopy extraction (issue #38, Windows only).**
  A new Settings option ("Extraction (.exfat images) -> Robocopy multithread
  (/MT)") extracts .exfat images with `robocopy /MT:N` instead of the classic
  single-thread copy. Off by default; thread count is configurable (default
  32, range 1-128). Ideal for NVMe/SSD destinations. The extract log states
  which mode is active and echoes the full command line so you can confirm
  the path. The .ffpkg/.ffpfs/.ffpfsc (UFS2Tool/Dokan) extraction path is
  unchanged. Dismount now always runs in a finally block.
- **Permanent dashboard support card** showing live Downloads / Releases
  counts pulled from GitHub, replacing the old startup donation popup.

### Changed
- **Faster existing-image conversions to .ffpfsc.** Converting an existing
  .exfat/.ffpkg to .ffpfsc now hard-links the source into the staging area
  instead of copying it whenever the drive supports it (NTFS), making staging
  near-instant and using no extra disk space. A full copy is now only a
  fallback for drives that can't hard-link (exFAT/FAT32), and that copy runs
  on a background thread with live bytes/speed/ETA instead of freezing the UI.
  The log states the source filesystem and staging mode; the original image is
  never modified.
- **Byte-based progress is now the single source of truth.** Copy progress,
  percentage and ETA are driven by bytes written rather than robocopy's file
  count, fixing the "high percentage too early / Almost done" behaviour on
  games made of a few very large files.
- **Format-aware build phases.** The phase tracker now matches the output
  format: exFAT/ffpkg builds show 5 phases (no compression step), ffpfsc
  builds show 6. Inactive phases are no longer shown for formats that don't
  use them.
- **Release experience is now a single, data-driven modal** shown once per
  new version, with support integrated inline. No more stacked startup
  popups and no separate startup donation popup. Future release notes are
  driven by data only.
- **Experimental mkpfs is now a universal external-tool hook.** The feature
  is no longer branded to one specific tool: generic UI wording and folder
  picker, generic launcher auto-detection that reports the actual launcher
  detected, generic logging, and renamed settings keys
  (`experimental_mkpfs_enabled` / `experimental_mkpfs_folder`) with automatic
  migration so existing saved values carry over.

### Fixed
- **Build progress no longer appears frozen at "Copy Files / 98% / Almost
  done."** After robocopy finishes, Windows/OSFMount can flush and dismount
  silently for minutes. The UI now leaves the copy state the instant the copy
  ends and reports the real stage: "Copy complete - finalizing image" ->
  "Verifying copied data" -> "Please wait - Windows is finishing disk writes"
  -> "Dismounting image" -> "Extraction complete." Progress holds at 99% with
  speed/ETA blanked during the silent window.
- **Build telemetry hardened.** Startup speed spikes (e.g. "10.6 GB/s") are
  filtered with a warm-up gate; implausible burst speeds are clamped and fall
  back to the cumulative average; "Almost done" only appears when genuinely
  near completion; and the file counter can no longer exceed the total (no
  more "532 / 103").

## [3.6.2] - 2026-06-07

Hotfix release. Two confirmed fixes only.

### Fixed
- **UI no longer lags or jumbles when scrolling during large builds.** The
  Output Log pump drained the queue one line at a time, doing a separate
  widget insert, a scroll-to-bottom, and a file write for every line. Under
  robocopy/mkpfs line floods that meant dozens of inserts and forced
  scroll-to-bottom calls per tick, which fought manual scrolling and made
  the log (and page) look jumbled and unreadable. The pump now drains the
  whole queue each tick and writes it in a single transaction: one insert
  pass, one file write, and a single autoscroll that only fires when the
  view is already pinned to the bottom \u2014 so scrolling up to read
  mid-build stays put. The visible log is also capped (trimmed to the most
  recent ~4000 lines past 5000) so the widget can't grow huge and slow every
  redraw. Full logs are still written to disk uncapped; nothing is dropped.

### Experimental (off by default)
- **Optional external lazy_mkpfs for existing .exfat \u2192 .ffpfsc (testing
  only).** A new, OFF-by-default Settings toggle ("Use experimental mkpfs
  for exFAT \u2192 FFPFSC") plus a folder picker ("Experimental mkpfs
  folder") let you point the app at a friend's external lazy_mkpfs tool and
  use it instead of the bundled mkpfs \u2014 but ONLY on the existing-image
  \u2192 .ffpfsc route. Dump-folder builds, ffpkg, extraction, and the stable
  PFS path are untouched. When on and an existing .exfat is packed, the app
  runs the tool's platform launcher (lazy_mkpfs.bat/.sh, or `python main.py`
  if no launcher) with `-i <source.exfat> -o <final.ffpfsc>
  --compression-level N --cpu N --pfs-version PS5 --temp-folder DIR`, writes
  straight to the chosen output path (no .pfs_stage, no source copy), logs
  the full command + stdout/stderr + exit code under `[EXPERIMENTAL]`, and
  the produced file flows through the same verification as stable output.
  Any failure (no Python, missing launcher, invalid folder, dependency
  install failure, non-zero exit, missing/implausibly-small output) logs
  "[EXPERIMENTAL] lazy_mkpfs failed \u2014 falling back to stable mkpfs" and
  the stable path runs instead. The source .exfat is never deleted on this
  route. Isolated in ui/experimental_mkpfs.py for easy removal; not shipped
  as a default. The
  confirmed-stop flow previously matched any klog line containing
  "shadowmount" plus a loose keyword ("cleanup complete/shutdown/released/
  unmounted") because the exact stop output hadn't been captured. With a
  real v1.6 stop capture in hand, confirmation is now anchored to actual
  markers: definitively the kernel "shadowmountplus.elf calls exit()" line
  (SM+ only exits after every LVD unit is released), or as a secondary
  path the "[SHUTDOWN] stop requested" ack followed by every started
  unmount completing. Unmounts are only counted after the ack so a prior
  game's lingering klog lines can't be misread, and the wait now shows a
  live "released N/M mounts" status. Green "safe to unplug" still requires
  confirmation; klog unreachable/dropped/timeout still yields the amber
  unconfirmed warning.
  Packing an existing `.exfat`/`.ffpkg` into `.ffpfsc` used to stage a full
  copy of the source under `.pfs_stage` before compression — turning a 76 GB
  source into 76 GB source + 76 GB stage + output, with a long, sometimes
  UI-freezing copy up front (worst on exFAT source drives, which can't
  hard-link). Investigation confirmed mkpfs `pack file` reads the source in
  place and writes a separate `.ffpfsc`, so the copy was never technically
  required. Now, for normal (ASCII) source paths the original image is
  passed directly to mkpfs and the `.ffpfsc` is written straight to the
  chosen output folder via a new explicit-output-path option — no
  `.pfs_stage`, no hard link, no copy, no rename, and the source is never
  modified or deleted (`delete_source_on_success=False`). The log states
  "Using existing image directly. No staging copy required." with the source
  and output paths. A narrow staging fallback (hard link, then copy with
  progress as a last resort) remains ONLY for non-ASCII source paths, which
  mkpfs can mishandle; it is never used for ASCII paths. During
  the copy phase two progress sources were competing: the byte-based drive
  poller (correct — the status grid already showed e.g. "75.05 GB / 76.11
  GB") and robocopy's per-FILE percentage parsed from stdout. With PS5
  dumps that contain a few huge files, one 72 GB file mid-copy made the top
  line jump to "50% / Almost done" while tens of GB were still pending. The
  byte poller is now the single source of truth during Copy: robocopy
  per-file percentages and per-file ETAs (and any "Almost done" derived from
  them) are ignored while the poller is active, and the top progress line is
  driven from the byte counts + byte-throughput ETA so it matches the grid
  ("Copying game files — 75.05 GB / 76.11 GB"). UI/status only; robocopy
  flags unchanged.
- **Phase tracker is now format-aware.** exFAT and ffpkg builds show five
  phases (Scan Source · Create Image · Format · Copy Files · Finalize) with
  no Compression step; FFPFSC builds show six (Scan Source · Create exFAT ·
  Format · Copy Files · Finalize · Compress). The tracker is rebuilt per
  job at dispatch based on the selected output type, so an exFAT-only build
  no longer displays a Compression phase that never runs. The
  status card used to freeze on "Copy Files / Almost done" through the
  whole post-copy flush, OSFMount dismount, and mkpfs startup — sometimes
  minutes of apparent nothing on large images. The phase tracker is now
  six steps (Scan · Create exFAT · Format · Copy · Finalizing · Preparing
  PFS). After the copy hits 100% it advances to **Phase 5/6 Finalizing**
  with real sub-status (Flushing pending writes / Waiting for Windows to
  finish writing image data / Dismounting OSFMount volume / Verifying
  intermediate), then **Phase 6/6 Preparing PFS compression** with a
  time-based heartbeat ("Preparing PFS compression… 0m 12s elapsed",
  updated every ~2.5s) and an indeterminate pulsing progress bar — no
  fabricated percentages. The moment mkpfs emits its first real progress
  line the heartbeat stops and the bar switches to the real percentage,
  throughput and ETA. If no progress appears after 60s the message softens
  ("still working after 1m 00s; large images can take several minutes…")
  and again after 3 minutes, but the build is never failed on the wait —
  only an actual mkpfs error fails it. The Output Log gains a timestamped
  timeline (Copy phase complete → Flushing → Dismounting → Intermediate
  ready → Starting mkpfs → Compression progress started). During the
  heartbeat the card also shows live Disk Read / Disk Write rates (via
  psutil) so activity is obvious, a rotating sub-status (Opening source
  image / Reading image layout / Preparing compression workers / Waiting
  for first compression progress), and the source image size plus a
  "this can take several minutes on large images" note in the log. Phase
  names are short ("Finalizing Image", "Starting Compression"). Closing the
  app mid-build now asks a context-aware question: "compression has not
  started yet — cancelling now should be safe, but temporary files may
  still need cleanup" before mkpfs begins, versus "compression already
  running — cancelling may take several minutes" once it has. UI/
  status only — no change to compression logic or mkpfs arguments. Every line that
  reaches the Output Log (OSFMount/robocopy/mkpfs/FTP/Klog/ShadowMount/
  mount/unmount/errors/warnings/completion summaries) is now mirrored to
  disk in the logs folder: a timestamped `exfat_builder_YYYY-MM-DD_HH-MM-SS.log`
  plus a rolling `latest.log`, each opened with a startup banner (app
  version, Windows, Python/frozen). Old logs are pruned on startup (older
  than 30 days, then capped at the newest 50). The **Report Issue** flow
  (button already next to Credits) now writes a much fuller
  `exfat_builder_report_*.txt`: app version, timestamp, system info
  (machine/CPU/RAM), free-disk summary across the logs/temp/output drives,
  recent build summary if available, last error/traceback, a settings
  snapshot with sensitive values (anything keyed pass/token/secret/key/
  credential/auth) replaced by `<redacted>`, and the full output log. The
  modal now offers GitHub, **Telegram Group** (t.me/ps5exfatbuilder),
  **Telegram Direct** (@deckerr97), Discord, and Open Report Folder; browser
  opens fall back to copying the URL to the clipboard, and a clear "Could
  not create diagnostic report" error is shown if the report can't be
  written. No automatic API upload is attempted — the user attaches the
  file manually. When packing an
  existing image whose source drive is exFAT/FAT32 (no hard-link support),
  the staging step's `os.link` failed and fell back to a blocking
  `shutil.copyfile` of the entire image (up to hundreds of GB) — run on the
  Tkinter main thread, with no progress — so the window showed "not
  responding" while a second copy of the image appeared under `.pfs_stage`.
  Fixes: (1) all staging/prep now runs on a worker thread, marshalling UI
  updates through `after(...)`, so the app stays responsive; (2) staging
  prefers a zero-copy hard link and only copies when no link is possible;
  (3) when a copy is unavoidable it uses a chunked copy that reports
  `source / total · MB/s · ETA` live, names the source and stage paths in
  the log, and explains why ("source filesystem does not support hard
  links, so a temporary staged copy is required to protect the original");
  (4) preflight checks source size, hard-link support, and destination free
  space, and `.pfs_stage` is never created on an exFAT source drive that
  can't link there — a hard-link-capable drive with room is chosen instead,
  failing before any copy if none has space. The user's original image is
  now passed read-only and never deleted or modified: the convert step is
  invoked with a new `delete_source_on_success=False` flag for
  user-selected sources (throwaway build intermediates still get cleaned up
  as before). Follow-up, not in this patch: a Size / Used / Free readout
  when mounting an image for editing, with a low-free-space warning.
- **Staging guidance for existing-image packs.** A tip under the Build tab
  Temp folder now explains that the Temp folder doubles as the staging
  location and that an NTFS folder gives fast zero-copy (hard-link) staging
  while exFAT/FAT32 force a full image copy. When the tool does fall back to
  a copy, the log now states the source filesystem (e.g. "Current source
  filesystem: exFAT"), the staging mode ("Full copy"), the source and stage
  paths, and points the user to set the Temp folder to an NTFS drive with
  enough free space for faster staging next time. No popups — status card,
  progress text, and log only; the build continues normally. (A dedicated
  "Set Temp Folder" button / staging-drive picker is a possible later
  improvement.) mkpfs compresses
  blocks with a `multiprocessing.Pool`. Because mkpfs ran in-process inside
  this frozen (`--noconsole`) GUI, each Pool worker re-launched the exe and
  re-imported the GUI module under the spawn start method, then touched
  `sys.stdout`/`sys.stderr` — which the runner had swapped for a
  non-picklable capture object and which are `None` in a windowed build —
  producing `AttributeError: 'NoneType' object has no attribute 'write'`
  and a `BrokenPipeError [WinError 232]` that took the whole app down (and
  left `_MEI*` / `exfat_builder_*` temp dirs behind, since shutdown was not
  clean). Fix (Option B): mkpfs now runs in a **separate process**. The exe
  re-invokes itself in a dedicated `--__mkpfs_worker__` mode (GUI-free, real
  stdout/stderr), and the runner pipes that child's output through the same
  progress/log parser. Crucially `multiprocessing.freeze_support()` runs
  first in `__main__`, so mkpfs's own Pool children re-exec that lightweight
  worker path and exit immediately — they never reach the GUI or the
  hijacked streams. If the subprocess cannot start, the runner falls back to
  in-process mkpfs forced to a single core (`--cpu-count 1`), which avoids
  the Pool path entirely. (`build.bat` also now explicitly bundles
  `mkpfs.__main__` for the worker shim.) Bundled mkpfs 0.0.6 changed the default
  zlib compression level from 9 (0.0.5) to 7. Because the app passes
  `--cpu-count <all cores>` and no explicit level, 3.6.1 silently ran level 7,
  where the parallel compression workers outrun the disk-bound writer; mkpfs's
  internal `Pool.imap` result queue then backs up without bound and the
  finished-but-unwritten compressed blocks pile up in RAM, ballooning to
  gigabytes on large games. All `.ffpfsc` build/convert routes now force
  `--compression-level 9` (restoring 3.5.3 throughput-matched behaviour) and
  cap CPU at `cpu_count()-1` (never above the user's selection) for extra
  headroom. The single-file streaming pack path stays enabled, so the fix adds
  no spool. Applies to: dump folder → .ffpfsc, existing .exfat → .ffpfsc,
  existing .ffpkg → .ffpfsc, and the PFS tab build/convert paths. The log now
  prints `mkpfs compression level: 9` and `mkpfs CPU count: N`.
- **FFPKG extract-to-folder failed on real dumps (longstanding).** The bundled
  UFS2Tool `extract` reads each file fully into memory (~2 GB cap, "File too
  large to read into memory"), so it could never extract a real game with
  multi-GB packaged files. Extract-to-folder now mounts the .ffpkg read-only
  (UFS2Tool `mount_udf` / Dokan — the same path Mount-as-drive already uses)
  and robocopies the mounted drive to the output folder, then unmounts. This
  streams through the filesystem driver with no size limit. robocopy exit codes
  0–7 are treated as success; the drive is always unmounted in a `finally`,
  including on cancel/failure. The summary reports source, output, files
  copied, total size, and the robocopy exit code. If the Dokan driver isn't
  installed the app explains it and links the free download. Applies to both
  the dedicated ffpkg Extract sub-tab and the unified Build-tab Extract route.
- **Dump Rename: the Rename button could fall off-screen.** The footer
  (status line, progress bar, Rename selected) was packed after the dumps
  list, so whenever the fixed cards plus the list outgrew the window height,
  Tk clipped the footer first — the button vanished and scrolling couldn't
  reach it (it lives outside the list's scroll canvas). The footer is now
  packed first with side='bottom', so it always keeps its space and the
  list shrinks instead. Same clipping class as the 3.6.0 Advanced-pane fix.
- **Backports: game cards crushed into slivers.** The games grid computed
  its column count as window-width / 200, but a card's natural width is
  ~400px (a 180px cover plus the text column). On wide windows this packed
  up to twice as many columns as actually fit, so the grid compressed every
  card — covers survived but titles, metadata and the Queue/Process buttons
  were squeezed to nothing. Columns are now sized from the real card width
  and kept uniform so all cards share space evenly; stale column weights
  from a previous wider layout are cleared (empty columns no longer claim
  width); and the grid only rebuilds when the column count actually changes
  instead of on every resize pixel.

### Changed — cover art normalization (premium-storefront grid)
- **All cover art is now normalized to one fixed 2:3 portrait format
  before display.** Previously each card showed the raw source artwork
  stretched into a square, so portrait, landscape, square and banner
  icons all rendered as different shapes with inconsistent scaling
  (some filled, some cropped, some letterboxed) — the grid looked
  messy. A new pipeline runs every cover through: border-trim → scale
  to FILL the 2:3 box → center-crop the overflow → save a 260x390 PNG
  master (CSS object-fit: cover, applied consistently, never contain or
  stretch). Cards display only the normalized master, resized to a
  fixed height. Masters are cached under exfat_builder_logs/covers_norm
  and keyed by source path + mtime + size + master dimensions, so
  edited sources re-normalize automatically and any pre-existing
  mixed-size cache migrates itself lazily on next view.
- **Game cards are now a fixed size regardless of content.** The card's
  cover is a fixed 2:3 slab (height-driven, width locked to height x
  2/3) and the info column is a fixed width, so card width and height
  no longer change with cover dimensions, title length, metadata length
  or PPSA length. Long titles truncate with an ellipsis instead of
  growing the card. Applied across the Backports, Library and Images
  grids: identical artwork dimensions, identical card size, uniform
  columns, and column counts computed from the real fixed card width so
  rows align cleanly even with 100+ games.

### Fixed (post-release)
- **Pipeline (build → .ffpfsc) crashed with "cannot access local variable
  'ffpfsc'".** In the convert worker, the Temp-drive move-back fallback
  reassigned the `ffpfsc` variable, which made Python treat `ffpfsc` as
  local to the worker for its entire body — so the earlier reads (before
  that line ever ran) raised UnboundLocalError and every pipeline convert
  failed right after the build finished. The fallback now uses a separate
  `result_path` variable, leaving `ffpfsc` as a clean read from the
  enclosing scope. The built image itself was always fine; only the
  auto-convert step was affected.

### Added — live build progress (unified Build tab)
- **Detailed build status card with a 5-phase tracker.** During a build the
  unified Build tab now shows each pipeline stage (Scan Source → Create
  exFAT → Format → Copy Files → Compress/Finalize) with a per-phase state
  icon (done / running / pending), plus a live metrics grid: source,
  output, current file, progress (GB/GB), files (copied/total), speed, ETA,
  compression (ratio · level · workers), temp used + peak, temp free, CPU
  and RAM. Every metric degrades to '—' when a value isn't available rather
  than showing a fabricated number.
- **Per-file copy visibility.** The copy phase now drops robocopy's /NFL so
  the current filename and an exact copied-file count are shown — but only
  when the source has ≤ 50,000 files. Above that, per-file logging is
  skipped automatically and the display falls back to GB-copied / speed /
  ETA with a periodic file count, so huge-file-count sources aren't slowed
  by log spam.
- **PFS compression telemetry.** During mkpfs the card shows throughput,
  compression ratio, worker count and compression level (9) with a live
  ETA. mkpfs does not expose a per-chunk filename, so none is invented.
- **Temp-space tracking** (folder, currently used, peak used, free) so it's
  clear why large builds need scratch space, and **system stats** (CPU /
  RAM) via the newly bundled psutil.
- **Queue panel** now shows current / next / after, and **milestone log
  lines are timestamped** (e.g. '[16:32:04] Phase 4/5 — Copying game
  files') so the OUTPUT LOG reads like a build timeline.

### Fixed (post-release)
- **Converting an image to .ffpfsc across drives copied the whole source
  first.** When the source (e.g. D:) and the chosen output (e.g. F:) were
  on different drives, the staging step created its work folder on the
  OUTPUT drive, so the instant same-volume hard link failed and the tool
  fell back to copying the entire multi-GB .exfat onto the output drive
  before compression even began — a slow, redundant step. Staging now
  always happens on the SOURCE drive, so the hard link is instant and no
  source bytes are copied. The original file is still never modified (the
  pack step consumes the hard link, not the real file); only the final,
  much smaller .ffpfsc is written to the chosen output drive.

### Added (post-release)
- **Selectable PFS compression level.** A new control on the Build tab (under
  CPU cores) lets you choose the .ffpfsc compression level: 1 (Fastest),
  3 (Fast), 6 (Balanced — new default) or 9 (Maximum). PS5 game data is
  largely incompressible, so high levels spend a lot of CPU for almost no
  size reduction; 6 is a much better speed/size balance than the old forced
  9, and 1–3 are faster still with near-identical output size. The level is
  remembered and applies to every .ffpfsc route (Build pipeline, Convert,
  PFS tab). Any level mounts identically under ShadowMount — zlib
  decompresses level-independently, so the level only affects the encoder.
  To stay clear of the v3.6.1 memory-overflow regression, the worker count
  is automatically scaled to the level (lower levels run fewer workers) so
  faster compression can't outrun the disk writer and balloon RAM.

### Fixed (post-release)
- **Converting very large images to .ffpfsc could fail with "[Errno 22]
  Invalid argument" mid-compression.** mkpfs 0.0.6 compresses blocks in a
  multiprocessing pool; on Windows, a very large single file (70 GB+, i.e.
  over a million 64 KB blocks) packed with several worker processes can
  fault inside the pool's result handling. The convert step now detects a
  failed parallel pack and automatically retries once single-core
  (--cpu-count 1), which avoids the multiprocessing path entirely and
  bounds memory. It's slower, but compression isn't the long pole on a big,
  mostly-incompressible game image, and the retry only triggers on an actual
  failure — successful parallel packs are unaffected. The chosen compression
  level, temp-folder and paths are all preserved across the retry.
- **Build could hang forever right after "Selected filesystem".** The next
  step, source-media detection (HDD/SSD/network, used to pick a robocopy
  thread default), calls Windows WMI/Storage cmdlets (Get-PhysicalDisk,
  Get-Disk, Get-CimInstance) that can block indefinitely when the WMI
  service or storage stack is wedged — freezing the build at "Scan Source"
  with no error. Detection is now skipped entirely when a thread count is
  set (the app always sets one; the network /COMPRESS check uses a cheap
  UNC-prefix test instead of WMI), and on the auto-threads path it runs in
  a background job with a hard 8-second timeout, falling back to 'unknown'
  defaults if Windows doesn't answer.
- **Build could also hang right after the image mounted ("Creating &
  mounting" → never reaching "Formatting").** The post-mount "wait for the
  drive to appear" check and the post-format filesystem check both polled
  WMI (Get-CimInstance Win32_LogicalDisk), so a wedged WMI service stalled
  them the same way. Both now use WMI-free Win32 calls instead — Test-Path
  for drive readiness and System.IO.DriveInfo for the filesystem type — so
  no step in the build path depends on WMI any more.

### Changed (post-release)
- **mkpfs updated 0.0.6 → 0.0.7.** Notable upstream changes: packs are
  faster on game data out of the box (blocks must now gain ≥5% to stay
  compressed, and files smaller than one 64 KB block are stored raw instead
  of being pointlessly compressed); executables and platform files are
  skipped more thoroughly (.json/.txt/.png/keystone and everything under
  sce_sys/ and sce_module/ stay raw — slightly larger .ffpfsc output,
  faster packs, identical mounting behaviour); a temp-space preflight now
  fails fast with a clear error when the temp drive can't hold the
  compression spool; failed packs clean up their partial temp files; and a
  path-hash collision handling fix. Note: 0.0.7 does NOT change the
  parallel compression internals, so this app's level-scaled worker caps
  and the automatic single-core retry for very large files remain in place.

### Fixed (post-release)
- **Status card PROGRESS stayed blank during PFS compression.** The
  PROGRESS field only filled in from a byte count, which mkpfs doesn't
  provide while compressing — it reports a percent of input processed. The
  field now shows that percent during the Compress/Finalize phase instead
  of a dash. (CURRENT FILE and FILES remain blank in this phase by design —
  mkpfs exposes no per-file data.)
- **CPU readout could show 0% spuriously on its first sample.**
  psutil.cpu_percent() returns 0.0 on its first call (it needs two samples
  to compute a delta); the meter is now primed when the process handle is
  created so the first displayed value is real. A sustained 0% during
  compression is genuine, not a bug — it means the pack is disk-bound (the
  compression workers are idle waiting on I/O), not CPU-bound.
- **A large build could produce a tiny, useless .ffpfsc (e.g. 112 MB from a
  78 GB game) reported as "99.9% saved".** Root cause: robocopy returns as
  soon as its writes hit the Windows write cache, so after copying 70 GB+
  into the mounted image, gigabytes of data can still be sitting in RAM as
  dirty pages. The build then dismounted and immediately packed the .exfat
  into a .ffpfsc — but mkpfs read the on-disk image before the cache had
  flushed, saw mostly zeroes, and compressed them to almost nothing,
  yielding a near-empty image that still "succeeded". Two fixes: (1) the
  build now explicitly flushes the volume write cache to the image file
  before dismounting; and (2) the convert step sanity-checks the result — if the
  .ffpfsc is implausibly small versus the source image (under ~1% of a 2 GB+
  image), it's treated as a failed pack, the source .exfat is KEPT (never
  silently deleted), and a clear warning is logged instead of a bogus
  success. The misleading "% saved" figure (which compared against the full,
  partly-empty image size) is no longer reported for these bad packs.
- **Post-copy flush could hang the build indefinitely (same-day hotfix of
  the fix above).** The first cut of the explicit flush used
  `Write-VolumeCache` — a Storage-module CIM cmdlet that rides the same
  WMI / Storage Management Provider as `Get-PhysicalDisk`, the provider
  this release already stopped trusting for media detection because it can
  block forever. Field-confirmed on a 77 GB build: the cmdlet hung 22+
  minutes with ~0 disk activity while the data had long since hit disk
  (the user dismounted manually and the image packed to .ffpfsc perfectly).
  The flush is now `FlushFileBuffers` on a raw `\\.\X:` volume handle — a
  direct kernel call, no CIM, cannot WMI-wedge — run in a background job
  with a heartbeat log line every 15s (elapsed time included) and a 15-min
  hard timeout that falls through to dismount, which performs its own
  driver-level flush anyway. The build can no longer hang here. The flush
  is a single inline `FlushFileBuffers` call on a `\\.\X:` volume handle
  (P/Invoke added via `Add-Type` at script scope); with /J keeping the
  cache drained during the copy it returns near-instantly, and if it's
  ever unavailable the following dismount flushes at driver level as the
  safety net.
- **Robocopy /J (unbuffered I/O) now triggers on total payload size, not
  just single-file size.** The old trigger (any one file ≥ 4 GB) measured
  the wrong thing: a 77 GB dump made of 100 MB–3 GB files floods the
  Windows cache exactly the same — robocopy "finished" at 1.3 GB/s (RAM
  speed), deferring the entire physical write into the opaque post-copy
  flush. /J now engages when any file is ≥ 1 GB OR the total payload is
  ≥ 16 GB, so writes drain to disk during the copy: the copy phase shows
  real speed/ETA and the end flush collapses to seconds.
- **The status card no longer freezes during the post-copy flush.** The
  flush window had no `[n/4]` marker and no percent lines, so the phase
  tracker sat on "Copy Files" with a stale ETA (e.g. a frozen "10s") for
  its whole duration. The worker now recognizes the flush start, the 15s
  heartbeats, the completion line, and the timeout line: the tracker
  advances to Compress/Finalize, the status shows live flush elapsed time,
  and the stale copy ETA/speed readouts are blanked instead of left
  lying.
- **ShadowMount "Safe to unplug" could green-light an unsafe unplug
  (intermittent console crash).** The old flow wrote the
  `/data/shadowmount/STOP` sentinel, slept a blind 3 seconds, and showed
  a green "safe to unplug" — without ever verifying ShadowMountPlus saw
  the sentinel or finished releasing mounts. Cleanup runs roughly a
  second or two per mounted game, so 3s loses to any real library; and if
  the ELF crashed or was never sent this boot, the sentinel is ignored
  entirely while the dialog still claims safe. Unplugging with live
  mounts kernel-panics the console — hence "sometimes fine, sometimes
  crash". The flow is now a confirmed handshake: a temporary klog socket
  is opened first (saved klog port, default 3232; the Klog tab's own
  connection is untouched), then STOP is written, then the app watches
  the kernel log for up to 45 seconds for a ShadowMountPlus cleanup line
  (tolerant case-insensitive match for now: 'shadowmount' plus one of
  'cleanup complete' / 'shutdown' / 'released' / 'unmounted'), ticking a
  live "Waiting for ShadowMountPlus confirmation... Ns" status. The
  green "safe to unplug" dialog is shown ONLY on confirmation. If klog
  is unreachable, drops mid-wait, or the window times out, an amber
  warning is shown instead: STOP was sent but safety is unconfirmed —
  wait ~5s per mounted game and for an idle enclosure LED, check the
  Klog tab, or resend the payload. The temporary klog socket is closed
  on every path.

### Changed (post-release)
- **Compression defaults are now stable-first.** Default level is back to
  **9 (Stable / safest)** and the default worker count is capped at
  **min(4, cores)**. Level 9's slower workers keep pace with the disk writer
  and avoid the memory pressure faster levels can cause; the lower worker
  cap further bounds RAM use. The Build-tab dropdown now lists 9 — Stable
  (default), 6 — Balanced, 3 — Fast, 1 — Fastest; power users can still pick
  a faster level, but the tool no longer silently defaults to 6.
- **Build now verifies the copy landed before finishing.** After the file
  copy, the build checks that the mounted image actually holds at least ~90%
  of the source's data. If it holds far less (an empty or partial image —
  e.g. a "build" that finishes in seconds with nothing copied), the build
  FAILS loudly instead of producing a useless image. Combined with the
  pre-dismount cache flush, this guards both the "empty image" and
  "unflushed image" failure modes.
- **THE empty-image root cause: builds finished in ~20s with a full-size but
  empty .exfat.** Two compounding bugs, both fixed:
  1. The post-mount "wait for drive" check (rewritten in an earlier hotfix to
     drop WMI) used Test-Path on the drive root. Immediately after OSFMount
     mounts, the volume is still RAW (unformatted), so Test-Path "D:\" returns
     false — the wait timed out after 20s and the script threw BEFORE
     formatting or copying anything. The image was created at full size but
     never filled. The wait now uses [System.IO.DriveInfo]::GetDrives() to
     detect the drive LETTER's presence (which is true the moment the device
     mounts, RAW or not), with Test-Path only as a post-format fallback. Still
     no WMI, so it can't hang.
  2. The launcher treated "the .exfat file exists" as build success. OSFMount
     creates that file at full size the instant it mounts, so it always
     exists — even when the build failed right after mounting. A failed,
     empty build was therefore laundered into a "success" and fed to the
     .ffpfsc packer. The launcher now treats the PowerShell exit code as
     ground truth: any non-zero exit is a failure, the partial/empty image is
     deleted, and the convert step never runs on it.

## [3.6.1] - 2026-06-06

### Fixed
- **Intermittent hang/freeze during builds (thread-safety).** The UI
  refactor exposed a latent threading bug: background worker threads (the
  build runner, the mkpfs pipeline logger, and the drive-dismount helper)
  were updating Tk widgets directly. Tkinter/Tcl is single-threaded, so
  cross-thread widget access works most of the time but occasionally wedges
  or crashes the interpreter \u2014 the "hangs sometimes" reports. The output
  log now delivers worker-thread lines through a thread-safe queue drained
  by a main-thread pump, and every widget-touching UI helper
  (`_log`, `_log_clear`, `_set_status`, `_set_progress`,
  `_ffpkg_set_progress`, `_elog`/`_elog_clear`, `_update_extract_bar`,
  `_ffpkg_extract_log_line`/`_ffpkg_extract_set_progress`, `_render_queue`,
  and the unified Build tab's progress/render helpers) now detects an
  off-thread call and reschedules onto the Tk thread automatically. Builds
  no longer depend on every caller remembering to marshal manually.


### Added
- **Optional Extra Files (user-supplied file inclusion).** A new collapsible
  section in the Build tab lets you include your own files/folders in the
  image before packing \u2014 pick folders/files, choose a destination (default
  /app0), and optionally generate `/app0/ampr_emu.index` over the staged
  files so lookups resolve without launching first (for your own
  homebrew/app files). Everything defaults OFF; nothing is added unless you
  select it, and the original source is never modified \u2014 files are copied
  into the writable image only. Applies to exFAT builds and PFS built via
  exFAT; direct .ffpkg packing and re-packing an existing image can't inject
  (the build warns clearly and those jobs build unchanged). If the optional
  step fails (e.g. the index can't be generated), the build aborts with a
  clear error instead of packing an incomplete image. The logic lives in an
  isolated `ui/extra_files.py` module (`copy_optional_extra_files`,
  `generate_ampr_index`, `apply_optional_extra_files_stage`) so the build
  engine just calls one entry point. Selections persist across restarts.
- **Build tab: "Fastest builds & compression" guidance.** A collapsible
  tips panel in the Configure output card explains how to avoid the common
  disk-bound slowdown: put the dump, output, and Temp folder on separate
  physical drives; point Temp at a fast (NVMe) drive with enough free space;
  and that idle cores during compression mean the disk is the bottleneck,
  not the CPU. Defaults collapsed so it doesn't crowd the panel.
- **Report Issue button + diagnostic report.** The Credits footer now has a
  "\U0001f41b Report Issue" button beside Support. Clicking it auto-generates a
  timestamped diagnostic report
  (`exfat_builder_bug_report_YYYYMMDD_HHMMSS.txt` in the logs folder)
  containing the app version, OS, timestamp, and the full Output Log, then
  opens a card-style modal with four routes: GitHub Issues, Discord (copy
  `scottish_deckerr`), Telegram (copy `@deckerr97`), and Open Report Folder
  (reveals the .txt in Explorer to attach). The report is built by a
  reusable `create_bug_report_file()` helper returning `{path, filename}`,
  so future additions (settings snapshot, build summary, diagnostics, crash
  traces, system info) can be folded in without changing the UI.
- **Library & PS5 manager now list .ffpkg and .ffpfsc images.** The Game
  Library ("My Images") and the PS5 file manager previously scanned only
  .exfat files, so the compressed PFS images the Build tab produces
  (.ffpfsc) and .ffpkg images didn't show up. All three formats are now
  detected (locally and over FTP) and render with the correct format badge
  (.ffpfsc shows as "PFS"). Existing per-image actions — upload to PS5,
  reveal in Explorer, delete — already work on any format. (Note: "Open in
  File Manager" still applies only to .exfat, since that route mounts the
  image; the new "Mount as drive" on the Extract tab covers .ffpkg.)
  Dump Rename is unchanged: it scans game *dump folders* (by eboot.bin),
  not finished image files, so .ffpfsc doesn't apply there.
- **ffpkg Extract: "Mount as drive" for large images.** UFS2Tool's
  `extract` reads each file fully into memory and is capped at 2 GB per
  file, so games containing a single packaged file larger than that (e.g.
  It Takes Two's ~12 GB data file) can't be extracted directly. The
  Extract tab now has a **Mount as drive** button that mounts the .ffpkg
  read-only as a Windows drive letter (via UFS2Tool's `mount_udf` +
  Dokan) and opens it in Explorer, so you can browse and copy out exactly
  the files you need with no size limit. The button toggles to **Unmount**
  while mounted, an **Open** shortcut reopens the drive, and any mount is
  released automatically on app exit. If the Dokan driver isn't installed
  the app explains it and links the (free, one-time) download \u2014 the
  same kind of dependency as OSFMount on the build side.
- **Build tab: PFS direct from an existing `.exfat` / `.ffpkg`.** When
  PFS is the chosen output on the main Build tab, a new "PFS — source"
  toggle lets you pick **Existing .exfat / .ffpkg** instead of a game
  dump folder. Point it at an image you already built and it packs
  straight into a `.ffpfsc` (single `mkpfs pack file`) — no dump, no
  intermediate rebuild, no extraction. In this mode the "build
  intermediate via" radios are hidden (there's no intermediate), the
  source picker becomes a file dialog filtered to `*.exfat *.ffpkg`,
  and PPSA / title / version are read from the filename to pre-fill the
  output name. Your original image is never modified or deleted: it's
  staged via a hard link (or a copy across volumes) into a temporary
  `.pfs_stage` folder that is cleaned up after the build, and only the
  staged copy is consumed by the packer. Mirrors the standalone
  Build → PFS sub-tab's "Build from .exfat / .ffpkg" route, but inline
  in the unified Build flow with the mixed queue.

### Changed
- **Bundled mkpfs 0.0.5 \u2192 0.0.6.** Build now pins `mkpfs==0.0.6`
  (build.bat). 0.0.6 brings performance improvements, lower memory
  use, better Windows compatibility, and \u2014 most relevantly \u2014 fixes
  the cross-drive / exFAT temp-folder hardlink failures the PFS tab
  used to work around. PS5 + 32-bit inodes are now mkpfs defaults
  (the flags the app already passes), so this is a clean drop-in: no
  app-code or CLI changes were required. The non-ASCII filename
  fail-fast new in 0.0.6 is already pre-empted by the PFS tab, which
  stages an ASCII-safe nested name before packing.

### Improved
- **PFS packs write to the Temp drive during compression (big speed-up on
  the disk-bound case).** Previously the only contention relief was steering
  mkpfs's spool via `--temp-folder`, but the final `.ffpfsc` still wrote next
  to the source image — so on large games the source-read and output-write
  hammered the *same physical disk* at once, collapsing throughput to a few
  MB/s while the CPU sat idle (the "extremely slow" reports). Now, when the
  Temp folder is on a *different drive* than the source/output, the Build tab
  writes the .ffpfsc to the Temp drive during compression (so the read and
  write hit different disks) and moves the finished file to the output folder
  afterwards. The compressed result is far smaller than the source, so that
  move is cheap next to the time saved. A free-space check guards the Temp
  drive: if it can't safely hold the staged image (~2x the source size), the
  build falls back to writing beside the source instead of risking filling
  the Temp drive mid-pack. When Temp is unset or on the same drive as the
  output, behavior is unchanged. Still bottleneck-bound by physical disks: best results come from source, output, and Temp each on a
  separate drive — ideally an NVMe SSD for Temp.
- **PFS pack honors the Temp folder (faster on disk-bound builds).** mkpfs
  spools the compressed output to a temporary file before moving it into
  place; by default that scratch lives beside the source. The Build tab now
  passes your configured **Temp folder** to mkpfs as `--temp-folder`, so you
  can steer that scratch onto a fast, separate drive. On large PFS packs
  that are bottlenecked by disk I/O rather than CPU (symptom: low MB/s with
  cores nearly idle), putting source, output, and temp on different physical
  drives \u2014 ideally an NVMe SSD \u2014 can multiply throughput. When no Temp
  folder is set, behavior is unchanged.
- **Live progress during the PFS pack/compress stage.** The Build tab's
  progress row and the status bar now show what mkpfs is actually doing
  while packing to `.ffpfsc`: real percent of the input processed, live
  throughput and a normalised ETA — e.g. `Compressing → .ffpfsc · 43% ·
  compressing · 120 MB/s · ETA 5m 20s`. Previously the pipeline watched
  the output file's size, which barely grows until late because mkpfs
  spools — so long compressions looked stalled. The size-watcher is kept
  only as a silent fallback that speaks if no mkpfs progress line has
  arrived for 3 seconds. Applies to both PFS routes on the Build tab
  (from a dump folder and from an existing image).
- **PFS \u2014 "Build from .exfat / .ffpkg".** The PFS route that packs an
  existing image straight into a `.ffpfsc` (single `mkpfs pack file`,
  no extraction, for both `.exfat` and `.ffpkg`) is now labelled as a
  build-from-existing route instead of "Convert", so users who
  already have a built image can find it. Same engine and method as
  before \u2014 wording only.

## [3.6.0] - 2026-06-06

### Added
- **CPU core selection on the Build tab** — a "CPU cores" dropdown in
  the Configure-output card (values scale to the machine's core
  count). It drives the same robocopy /MT threads setting as
  Advanced → Build Parameters → "Threads (safe)", so the two controls
  stay in sync; picking a value clamps (1–128) and saves immediately.
- **What's-new dialog on launch** — a Steam-style release screen:
  MAJOR UI REFRESH hero, four stat counters (7 tabs / 12 UI
  improvements / 3 features / 100% compatible), three real screenshot
  highlight cards (Backports, Advanced, Settings — bundled under
  assets/whatsnew/), a What's-Improved checklist, a community
  supporters banner, and an integrated Support / Open Changelog /
  Let's Build footer. The amber PFS testing-phase notice is kept. The
  separate close-time Ko-fi popup is also kept (it fires on exit, not
  alongside this dialog); it can be removed in one line if preferred.
- **Support popup on exit** — a small thank-you dialog with the Ko-fi
  link (ko-fi.com/deckerr9746220). Either button continues the normal
  close path (settings save, temp cleanup, mount sweep).

### Changed
- **Credits — supporters**: the Ko-fi donations total was removed; the
  panel now simply thanks donors. Total downloads is fetched live from
  the GitHub releases API (the same figures as the github-release-stats
  page), with the optional donors.json value as the offline fallback.
  The "Support the tool" button now opens the project Ko-fi page.

### Changed — thread recommendation is now 1 (anti-fragmentation)
- The hardware detector recommended 16/32 threads on SSD/NVMe, which
  optimised raw copy speed but ignored that multi-threaded robocopy
  (/MT) fragments the output image — bad once it's mounted on the PS5.
  The recommendation is now **1 (single-threaded)** for every drive
  type. The detected model/kind still drives the device name and
  performance/health labels. Side effect: the default of 1 now reads
  as OPTIMAL in the build profile instead of being flagged CUSTOM, and
  the Threads guidance no longer implies "higher is better".

### Fixed — Advanced pane scrolling + layout (from screenshot feedback)
- **Scroll bug (root cause)**: the three Advanced sub-tab panes
  (Build Parameters / Post-Build / Language Stripper) built directly
  into non-scrolling frames, so content taller than the viewport was
  clipped — most visibly Post-Build's Danger Zone falling off the
  bottom. All three panes now build inside a scroll host whose
  scrollregion tracks the content height, so nothing is unreachable
  and future cards can't push content off-screen.
- **Build Parameters layout**: the cramped 300px right sidebar (which
  wrapped text and unbalanced the cards at higher Windows scaling) is
  gone. The four cards now sit in a full-width 2x2 grid with a
  full-width Recommendation Assistant beneath them — Recommended setup
  / Current build profile / Why these defaults across three roomy
  columns.
- **Post-Build**: the Automation Summary became a compact Automation
  Pipeline — the checklist and the Build → Verify → Log → Upload flow
  now hug the left instead of stranding the flow in empty space,
  reclaiming ~100px of vertical room.

### Fixed — Build Parameters layout (from live screenshot feedback)
- **Text clipping**: the forced-uniform 2x2 card rows squeezed the
  Filesystem card (cutting the "Locked. 4096…" help text) while
  stretching the ffpkg card with dead space. Rows now size to content
  and pairs equalize naturally.
- **Card balance**: the three ffpkg rows (block size, fragment, locked
  sector) moved from Filesystem layout into the ffpkg parameters card
  where they belong — Filesystem/Image pair on top, Performance/ffpkg
  pair below, all four visually balanced.
- **Alignment**: Write speed is now a proper label/control row aligned
  with the entries above it; int/text entries share one width.
- **Toggle status** chips became real pill badges (✓ Enabled /
  ○ Disabled).
- **Sidebar**: the Recommended card's border now matches the center
  cards; "Why these defaults?" copy shortened so the sidebar fits
  without clipping.
- **Typography**: larger page title and richer subtitle, bigger card
  titles/subtitles and row labels, more breathing room between rows.

### Changed — Advanced & Settings premiumization pass
- **Build Parameters** side rail gained compact hardware stat cells
  (Threads / Sector / Performance / Health, from the existing drive
  detection) inside the Recommended card, plus a **Current Build
  Profile** card mirroring Cluster / Sector / Threads / Retries /
  Bytes-inode live with per-row status dots and an overall OPTIMAL /
  CUSTOM / RISKY pill (red = the known-risky 4096 sector).
- **Post-Build** gained a full-width **Automation Summary**: a ✓/✗
  enabled-actions checklist and a Build → Verify → Log → Upload
  workflow strip whose steps light up as the matching toggles are
  enabled. The Danger Zone got a red header band and a DESTRUCTIVE
  badge. Every toggle row across Advanced now shows a live Enabled /
  Disabled status chip next to the control.
- **Language Stripper** summary gained a Selected cell and a live
  "Keep: …" line (both read from the future scan stats dict).
- **Settings**: OSFMount summary gained a Detected Path cell; Logs
  shows a premium "No logs available" empty state; Theme now uses the
  preview cards exclusively (the redundant radio row was retired —
  same var, same toggle path); Auto-Shutdown gained a live
  Trigger → Wait → Action workflow preview; SettingsCard headers got
  larger titles and more breathing room app-wide.

### Changed — Advanced & Settings dashboard pass
- **Advanced → Post-Build** became an automation dashboard: Upload /
  Verification / Logging / Image Output cards in a grid plus a
  full-width red-bordered **Danger Zone** for the delete-source
  toggles. Every card carries a live ON / OFF / ARMED state pill; the
  six setting rows keep their exact vars and callbacks.
- **Advanced → Language Stripper** became a feature page: top summary
  strip (Selected Folder — live, Languages Found, Estimated Saving),
  bottom summary (Space To Save / Files To Remove / New Estimated
  Size), and a large primary "Strip Selected Languages" action. The
  scan-derived cells read `app._adv_lang_stats` and show "—" until a
  scan backend populates it (none exists in this build yet).
- **Settings → Logs** became a dashboard: Total Logs / Total Size /
  Failed / Last Log cells plus a Recent Logs table (newest six,
  double-click opens the file), refreshed automatically after builds
  and Clear all logs. Open / Clear buttons unchanged.
- **Settings → Theme** gained two selectable preview cards (Dark
  Purple / Light) with mini window mockups; clicking one drives the
  same `_theme_var` + `_toggle_theme` path as the radios below.
- **Settings → PS5 FTP** summary strip gained an **Auto Upload** cell
  mirroring the pane's auto-upload-after-build toggle.
- `ui/tab_settings.py` now imports `os` explicitly instead of relying
  on the star-import.

### Changed — Backports / Convert / Dump Rename / History / Advanced / Settings / Credits presentation pass
Second presentation pass (after the PS5 section below) bringing the
remaining seven tabs to the Build-tab visual language, matched against
the v3.6/v3.7 design boards. No backend, workflow, pipeline, or
settings-storage logic was touched.

- **Backports — Games** cards now carry the SDK badge on the cover,
  the dump size and date, the source folder, and a per-card action row
  (Queue toggle, Process, ⋮ menu with open-folder). A bottom stats
  strip shows Games / Queued / Completed / Estimated queue time.
  Size and date are computed asynchronously alongside the existing SDK
  detection — display only.
- **Backports — Auto Backport**'s selected-game banner became a full
  hero card: cover, title/PPSA, and a live Detected SDK / Target
  Firmware / Patches / Status strip with a READY TO PATCH badge fed by
  the existing detection vars. The form below is unchanged.
- **Backports — Results** entries became status-edged cards (cover
  tile, PPSA chip, zip chips with NOT FOUND state, Open Folder / Open
  ZIP) with All / Worked / Failed filter pills and live counts. The
  It Worked / Didn't Work workflow and its file handling are untouched.
- **Convert** gains a selected-image hero (game parsed from the
  filename, source → output format, size, READY TO CONVERT badge) and
  compact .exfat → .ffpkg flow chips on both cards.
- **Dump Rename** gains a four-cell stats strip (Dumps Found / Matched
  / Unknown / Already Named), a per-row outcome chip (✓ Will rename /
  ⚠ No match / ✓ Already OK), and the Rename button now shows the live
  selection count.
- **History** rows gained a small async cover thumbnail next to the
  game title, matching the Library cards.
- **Advanced**'s "Recommended for your setup" card got a stronger
  accent border.
- **Settings** sections gained dashboard summary strips above the
  controls: OSFMount (version, detected status, last checked,
  architecture — read from the executable itself), Build (temp usage,
  retries, retry wait, app version), Logs (file count, folder), PS5
  FTP (IP, port, reachability), Notifications (sound state), Theme
  (current theme + palette preview), and Auto-Shutdown (action, delay,
  active rules).
- **Credits** became a dashboard: identity hero with icon tile,
  contributor tile cards, and a supporters panel with a stats strip
  (total supporters, Ko-fi total, downloads — the latter two read from
  optional `kofi_total` / `total_downloads` keys in donors.json and
  show "—" until added). Donor loading is unchanged.

### Changed — PS5 section presentation pass
Pure presentation pass bringing the whole PS5 section in line with the
Build-tab visual language (hero cards, stat strips, library cards, shared
empty states). No protocol, transfer, or workflow logic was touched.

- **ShadowMount+ / MicroMount** now land on an **Overview dashboard**:
  one summary tile per configuration section showing live values from
  the editor rows (click a tile to open the existing editor), plus a
  Recent Activity panel (last payload send / config load / config push
  / log fetch, recorded from the existing status line). An Overview
  row was added to the section rail; the editors, dirty tracking,
  save bar, and search are untouched. The ShadowMount+ hero also
  gained a per-image **Rules** count cell.
- **Y2JB** gained a deployment hero strip (PS5 IP / FTP port / DPI
  port / live install state), a Quick Actions card (Browse PS5,
  Upload Package, Open etaHEN folder — each jumps to the existing FTP
  sub-tabs), and an Install Queue panel mirroring the live install
  progress with a session activity feed and last-installed time.
- **Payloads** gained a payload-bus metrics strip (payload count,
  console IP, default port, last-sent time recorded from the status
  line). Per-card Send buttons already existed and are unchanged.
- **PS5 Manager** is now a "PS5 Control Center": hero card with live stats
  (IP, FTP, storage used/free, games on PS5, local images) fed by the
  existing refresh, plus a quick-actions row that jumps to the other PS5
  sub-tabs. Firmware / etaHEN / temperature cells are present but show "—"
  (no data source in the current refresh worker — wired for a future pass).
- **FTP — Quick Upload** is now a transfer dashboard: connection / target
  path / upload-state strip, a connection-information card, and the upload
  form, progress, and log split into discrete cards.
- **FTP — PS5 Browser** gains a stats strip above the dual panes
  (connection, current path, files listed, selection, upload queue), and
  the transfers panel summary now reflects active/cleared state.
- **Klog Monitor** gains a live stats strip (total lines, lines/sec,
  warnings, errors, connection) derived from the existing line buffer via
  a 1-second UI poll — the stream socket and parser are untouched.
- **Payloads** list rows became spaced library cards (file chip, port,
  per-payload IP, last-sent) and the detail pane gained a READY/MISSING
  badge, effective port, and last-sent facts. "Last sent" displays "—"
  until a future pass records it (recording would be a logic change).
- **Y2JB — Install** replaced the stacked region fields with three
  side-by-side USA / EU / JP region cards (PPSA id, patchable badge, PKG
  picker, readiness status, install button). Same DPI install workflow.
- **ShadowMount+ / MicroMount** toolbars became hero cards with config
  stats (sections, port, config source, live status; MicroMount also
  mirrors target dir / scan paths / interval from the form) and the same
  action buttons. The settings editors below are untouched.

## [3.5.3] - 2026-06-05

First release since 3.4.0. The headline is PFS / MicroMount support — none
of it existed in 3.4.0, so the entire PFS tab and its workflows are new to
anyone updating from the public build.

### Added — PFS / MicroMount
- New **PFS** tab with three workflows: build a dump folder into a PFS image,
  convert an existing image, and extract a PFS image back to a folder.
- **Build**: pack a PS5 game dump into a mountable PFS, with an output-format
  toggle — `.ffpfsc` (compressed container, smaller on disk) or `.ffpfs`
  (uncompressed, full console read speed, single fast pass, no temp file).
- **Convert**: turn an existing `.exfat` or `.ffpkg` straight into a `.ffpfsc`
  in one `mkpfs pack file` — no extraction, no OSFMount.
- **Extract**: unpack a `.ffpfs` or `.ffpfsc` to a folder.
- `mkpfs` bundled and run in-process (works in the frozen exe), with a build
  queue, live progress, and CPU-core selection.

### Fixed
- **Containers now mount on ShadowMount+/MicroMount.** The nested image was
  written as `.pfs_image.dat` (leading dot); the mounter only recognises a
  nested PFS named exactly `pfs_image.dat`, so earlier containers mounted
  empty. Any `.ffpfsc` built before this must be re-packed.
- Nested `.exfat` / `.ffpkg` images keep their extension so the mounter
  recognises them.
- Removed mkpfs `--verify` post-create read-back — builds are roughly twice as
  fast, with byte-identical output.

### Changed
- Convert no longer extracts first (was OSFMount + robocopy for exFAT, UFS2Tool
  for ffpkg) — both formats now pack directly.
- Mounts left open (file manager, ffpkg editor, a build) are dismounted
  automatically and safely when the app closes.
- Removed an orphaned PFS-cards module.
- Extract guidance clarified: an uncompressed `.ffpfs` unpacks straight to game
  files, while a `.ffpfsc` unpacks to its nested image.
