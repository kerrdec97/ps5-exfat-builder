# exFAT Image Builder v3.6.2

Stability release focused on the build → `.ffpfsc` pipeline: targets the v3.6.1 memory overflow, three separate "build hangs forever" causes, large builds silently producing empty images, and the ShadowMount "Safe to unplug" reliability issue. Also adds a live build status card and a selectable compression level. **The fixes below are in testing — please report back (Report Issue in-app, GitHub, or Discord) whether they hold up on your setup.**

## Fixes under test

**FFPFSC memory overflow regression (3.6.1).** Bundled mkpfs 0.0.6 silently changed its default compression level, letting the parallel compression workers outrun the disk writer — finished-but-unwritten blocks piled up in RAM and ballooned to gigabytes on large games. Compression settings are now explicitly managed on every `.ffpfsc` route (build, convert, PFS tab) and worker counts are scaled so compression shouldn't be able to outrun the writer again.

**Large builds could produce a tiny, useless `.ffpfsc` (e.g. 112 MB from a 78 GB game, reported as "99.9% saved").** robocopy returns as soon as its writes hit the Windows cache, so the pipeline could dismount and pack the image while most of the data was still in RAM — mkpfs read mostly zeroes and "succeeded". The build now flushes the volume write cache to the image before dismounting, and the convert step sanity-checks the result: an implausibly small `.ffpfsc` is treated as a failed pack and your source `.exfat` is kept, never silently deleted.

**Build could hang forever at three different points.** Source-media detection, the post-mount drive-ready wait, and the post-format filesystem check all called Windows WMI/Storage APIs that can block indefinitely when the WMI service is wedged. All three now use direct Win32 calls instead, and the new cache flush is a direct kernel `FlushFileBuffers` call — no step in the build path should touch WMI anymore.

**Copies of big games fake-finishing at RAM speed.** Unbuffered I/O (`/J`) used to require a single 4 GB+ file, so a 77 GB dump made of smaller files flooded the cache, "finished" at 1.3 GB/s, and deferred the entire physical write into an invisible end-of-build flush. `/J` now engages on total payload size (any file ≥ 1 GB or ≥ 16 GB total), so the copy phase should show real speed and a real ETA, with the final flush taking seconds.

**ShadowMount "Safe to unplug" could green-light an unsafe unplug (intermittent console crash).** It wrote the STOP sentinel, waited a blind 3 seconds, and declared safety — without ever confirming ShadowMountPlus released its mounts (cleanup takes ~1–2 s *per mounted game*, and a crashed/never-sent ELF ignores the sentinel entirely). It's now a confirmed handshake: the app watches the PS5 kernel log and only shows the green "safe to unplug" once ShadowMountPlus confirms cleanup. If klog is unreachable or no confirmation arrives within 45 s, you get an amber warning instead — STOP was sent, but don't unplug until the enclosure LED is idle. The klog match is intentionally broad for now and will be tightened with real capture data — reports of false ambers (or worse, false greens) are especially wanted.

**FFPKG extract-to-folder failing on real dumps.** The bundled extractor read each file fully into memory (~2 GB cap), so multi-GB packaged files always failed. Extraction now mounts the `.ffpkg` read-only via Dokan and streams it out with robocopy — no size limit. The drive is always unmounted, even on cancel or failure.

**Converting across drives copying the whole source first.** Staging now happens on the source drive so the instant hard link always works; only the final `.ffpfsc` is written to your chosen output drive.

**Very large image converts dying with "[Errno 22] Invalid argument".** A failed parallel pack on 70 GB+ single files now automatically retries single-core, which bounds memory and avoids the multiprocessing fault. Only triggers on an actual failure.

**Pipeline crash "cannot access local variable 'ffpfsc'"** right after a successful build (the image was always fine; only the auto-convert step crashed).

**UI issues:** the build status card freezing with a stale ETA during the end-of-build flush; PROGRESS staying blank during mkpfs compression (now shows percent); the CPU readout showing a spurious 0% on its first sample; the Dump Rename button falling off-screen on small windows; Backports game cards getting crushed into slivers on wide windows.

## Added

**Live build status card (unified Build tab).** A 5-phase tracker (Scan → Create exFAT → Format → Copy → Compress/Finalize) with live metrics: current file and exact file count during copy, GB progress, speed, ETA, compression ratio/level/workers during mkpfs, temp space used/peak/free, and CPU/RAM. Metrics show '—' rather than fabricated numbers when a value isn't available. Milestone log lines are timestamped so the output log reads like a build timeline.

**Selectable `.ffpfsc` compression level** (1 Fastest / 3 Fast / 6 Balanced — new default / 9 Maximum) on the Build tab. PS5 game data is mostly incompressible, so high levels burn CPU for almost no size gain; 6 is a much better balance than the old forced 9. Any level mounts identically under ShadowMount. Worker counts auto-scale to the level so faster settings can't trigger the memory issue.

## Changed

**mkpfs updated 0.0.6 → 0.0.7.** Faster packs on game data, more thorough raw-storage of executables and `sce_sys`/`sce_module` content, a temp-space preflight that fails fast with a clear error, and partial temp cleanup on failed packs.

**Cover art normalized to a uniform 2:3 portrait format** across the Backports, Library and Images grids — every card is now the same fixed size with consistent fill/crop, long titles truncate instead of stretching cards, and columns are computed from real card width so rows align cleanly even with 100+ games.
