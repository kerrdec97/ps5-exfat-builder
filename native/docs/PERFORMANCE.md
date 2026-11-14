# Performance summary

What each engine does to be fast, what was measured, and where. The numbers
come from two machines: the owner's PC (5.0.3, below) and, for the older
rows, the cloud development VM (Linux, 4 vCPUs shared with other builds,
virtio disk). Real-world speed is bounded by the drives and by Windows
Defender. Details are in each engine's document.

## Principles (all engines)

* **Correctness first.** Every build is read back and compared before it is
  renamed from `.part`; no speed setting skips that unless the user turns
  verification off (`verification.verifyAfterBuild`).
* **Streaming.** Sources are read in image order with bounded buffers
  (`performance.ioBufferMiB`); no engine holds a whole file, let alone a whole
  game, in memory.
* **One pass.** Folder → `.ffpfsc` builds the nested PFS image on the fly
  instead of writing and re-reading a full-size intermediate (Lazy_MkPFS and
  the legacy app wrote `pfs_image.dat`).
* **Parallel where the work is CPU-bound** — compression, hashing, full
  verification, extraction of many small files — sized by
  `performance.workerThreads`; disk-bound jobs run one at a time by default
  (`performance.maxConcurrentJobs`).
* **No call per file that can be avoided.** The exFAT source scan takes
  ordinary folders from one directory listing; extraction sets times and
  attributes through the handle it has just written.
* **The UI never waits.** Everything runs on the queue's thread pool; progress
  is coalesced (≤ 10 updates/s per job) so a 3,000-file build does not flood
  the event loop.
* **Holes.** exFAT zero runs ≥ 1 MiB and the free tail are not written. The
  image file is not marked sparse, so on NTFS they still take their room on
  the drive (measured below; an owner decision).

## Measurements on the owner's PC (5.0.3)

AMD Ryzen 7 7800X3D (8 cores / 16 logical CPUs), 31 GB RAM, NVMe, Windows 11,
Defender on; Release build; every data set fits the page cache. Median of 3
runs (range in brackets), MB/s = MiB/s. "Before" is the 5.0.3 candidate
without the three changes below (the same engine code as 5.0.2), "after" is
5.0.3.

| Engine | Operation | Before | After |
|---|---|---|---|
| exFAT | Scan 2,027 files and folders | 150–163 ms | **15.5 ms** (10×) |
| exFAT | Structure verify, whole tree | 159–164 ms | **19–20 ms** |
| exFAT | Full verify, 1 thread / 16 threads (1 GiB, 2,005 files) | 2,143 / 4,003 MB/s | **3,020 / 8,329 MB/s** |
| exFAT | Write image incl. flush (1 GiB, 2,005 files) | 1,166–1,436 MB/s | 1,301–1,373 MB/s (unchanged) |
| exFAT | Extract, one file at a time (default) | 956–1,023 MB/s | **1,221–1,224 MB/s** (+20–28 %) |
| exFAT | Extract, 16 files at once (round 2) | 1,759–1,790 MB/s | 1,905–1,923 MB/s |
| exFAT | Extract 2,005 small files (9 MiB), one / 16 at once | 12 / 17 MB/s | **16 / 19 MB/s** |
| PFS | image → .ffpfsc 512 MiB, level 6, 16 threads | 534 (456–581) MB/s | 580 (579–582) MB/s |
| PFS | image → .ffpfsc 256 MiB, level 9, 16 threads | 156 MB/s | 160 MB/s |
| PFS | image → .ffpfsc 512 MiB, level 1, 16 threads | 1,012 MB/s | 1,030 MB/s |
| PFS | Extract / full verify of the image just built | 1,150–1,250 / 1,280–1,400 MB/s | same |
| UFS2 | Write / full compare, 1,033 MiB, 2,006 files | 1,485 / 4,306 MB/s | same (no change made) |
| zstd | 206 MB synthetic game: pack, exFAT + FFPKG + FFPFS, verify, read back | 3.6 s | same (no change made) |

What changed:

* **Scan** (exFAT build and verification): a folder of ordinary files is
  taken from one `FindFirstFileExW` listing; before, every entry cost two
  path lookups (QFileInfo asking again, then `GetFileAttributesW`), about
  74 µs each with Defender on. Folders holding links or shortcuts are scanned
  as before. Full verification gains because it scans the source too.
* **Extraction**: times and attributes are set through the handle just
  written instead of opening each file twice more (≈ 90 µs less per file).
* **PFS**: the input is read ahead on a thread of its own when there are
  compression workers. No measurable change here: with the source in the page
  cache the 16-worker pool (about 1 GB/s at level 1) is the limit, not the
  writing thread. It can only help when reading is slow (cold cache, hard
  disk, network) at levels 1–4. Output bytes are identical.

### Performance modes

For the image engines a mode sets the worker threads only (Low impact ½,
Balanced ¾, Maximum all logical CPUs: 8 / 12 / 16 here); its priorities apply
to Sony's publisher and the DLC engine.

| Operation | Low impact (8) | Balanced (12) | Maximum (16) |
|---|---|---|---|
| PFS level 6, image 512 MiB | 363 MB/s (0.63) | 492 MB/s (0.85) | 580 MB/s |
| PFS level 9, image 256 MiB | 92 MB/s (0.57) | 129 MB/s (0.80) | 160 MB/s |
| exFAT full verify, cached 1 GiB | 9.1 GB/s | 8.9 GB/s | 8.3 GB/s |
| Peak memory of the exFAT benchmark | 82 MiB | 113 MiB | 146 MiB |

exFAT writing, extraction (own setting, `exfat.extractThreads`) and UFS2 do
not depend on the mode. exFAT verification works one file per thread, so a
game of a few huge files gains little from more threads.

### Numbers behind open decisions

* **Compression level** (default 9), 16 threads, half-random synthetic data:
  level 9 159 MB/s → image 56.5 % of the source; level 6 558 MB/s → 56.9 %;
  level 1 993 MB/s → 59.5 %.
* **Parallel extraction** (`exfat.extractParallel`, default off): 1.3–1.6×
  faster on this NVMe (above); on hard disks reading the image in order wins.
* **Sparse images**: 1 GiB of files with the default spare rule gives a
  2,239 MiB image. A plain file takes 2,239 MiB on the drive; the same image
  in a file marked sparse takes 1,152 MiB and was written 9 % faster.

## Measurements on the development VM (5.0.0)

| Engine | Operation | Native | Reference on the same VM |
|---|---|---|---|
| exFAT | Write 1 GiB image, 2,005 files, incl. fsync | 197–290 MB/s | `dd conv=fsync` of 1 GiB: 178–279 MB/s (disk speed) |
| exFAT | Full verify, 1 / 4 threads | 1.4–2.2 / 1.7–4.5 GB/s | — |
| exFAT | Structure verify, whole tree | 32–60 ms | — |
| UFS2 (FFPKG) | Build 301 MiB, 3,008 files | 231–2,260 MiB/s | UFS2Tool 4.0.0: 439–627 MiB/s (native faster in 4 of 6 runs; write-back noise dominates) |
| PFS | image → .ffpfsc 320 MiB, level 9, 4 threads | 18.4 MB/s | Lazy_MkPFS `--cpu-count 4`: 12.8 MB/s |
| PFS | folder → .ffpfsc 320 MiB, level 9, 4 threads | 11.6 MB/s | Lazy_MkPFS: 8.4 MB/s |
| PFS | image → .ffpfsc 320 MiB, level 6, 4 threads | 41.6 MB/s | Lazy_MkPFS: 48.0 MB/s |
| PFS | folder → .ffpfsc 320 MiB, level 6, 4 threads | 51.5 MB/s | Lazy_MkPFS: 42.8 MB/s |
| PFS | image → .ffpfsc < 256 MiB, level 9 | uses every thread | Lazy compresses a single file below 256 MiB on one core |

Outputs of the UFS2 and PFS engines were byte-identical to UFS2Tool and
Lazy_MkPFS in every benchmark run. The exFAT legacy path (OSFMount + format +
robocopy, administrator) cannot run on Linux, so there is no like-for-like
legacy exFAT number; the native writer runs at the speed of the disk.

FPKG speed is the speed of the user's Publishing Tools (`prospero-pub-cmd`);
the app adds staging (hard links when source and work folder share a volume,
otherwise a copy), the live CPU limit, and a read-back verification.

## Reproduce

Release build, test programs in `<build>/tests`. Every benchmark takes the
Performance settings from the environment, resolved as the app resolves
them (`tests/engines/BenchSettings.h`): `FW_BENCH_MODE` (`lowImpact`,
`balanced`, `maximum`), `FW_BENCH_THREADS` (`performance.workerThreads`, 0 =
from the mode) and `FW_BENCH_BUFFER_MIB` (`performance.ioBufferMiB`); unset
means the app's defaults. Run them with `FW_OFFLINE=1` and the `FW_DATA_DIR` /
`D97BETA_*` folders pointed at a temporary folder.

| Benchmark | Command |
|---|---|
| exFAT | `FW_EXFAT_BENCH_MIB=1024 tst_exfat_bench` (scan ×9, write, verify, free tail, extract) |
| UFS2 | `FW_UFS2_BENCH_MIB=1024 tst_ufs2_roundtrip benchmark` (+ `FW_UFS2TOOL_DLL` for the comparison) |
| PFS | `QTEST_FUNCTION_TIMEOUT=3600000 FW_PFS_BENCH_MIB=512 FW_PFS_BENCH_LEVEL=6 tst_pfs_benchmark` (`FW_PFS_BENCH_SINGLE=0` leaves the one-thread build out, `FW_PFS_BENCH_REPEAT=3` keeps the best of three) |
| zstd packs | `FW_ZSTD_E2E_GAME=<a synthetic game folder> tst_zstd_games realGameEndToEnd` (packs it into exFAT, FFPKG and FFPFS images and reads them back; the time is in the test log) |

## Not done yet

* Unbuffered I/O (`FILE_FLAG_NO_BUFFERING`) for very large images on Windows
  (the legacy `robocopy /J`), to keep the system file cache from filling up
  during a 100 GB build.
* zlib-ng / ISA-L compression back-ends (the legacy `pfs_lazy_zlib_backend`);
  the native engine uses zlib only (level 9 runs at about 10 MB/s per thread).
* exFAT verification splits work by file, so a game made of one or two huge
  files verifies on one or two threads.
* Sparse output on NTFS, parallel extraction by default on SSDs and the
  definition of Balanced are owner decisions; the numbers above are what they
  are decided on.
