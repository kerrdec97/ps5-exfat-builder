# Performance summary

What each engine does to be fast, what was measured, and where. All numbers
come from the cloud development VM (Linux, 4 vCPUs shared with other builds,
virtio disk). **None was measured on the owner's PC**; real-world speed is
bounded by the drives and by Windows Defender, and should be re-measured
there. Details and reproduction
commands are in each engine's document.

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
* **The UI never waits.** Everything runs on the queue's thread pool; progress
  is coalesced (≤ 10 updates/s per job) so a 3,000-file build does not flood
  the event loop.
* **Sparse output.** exFAT zero runs ≥ 1 MiB and the free tail are holes.

## Measurements

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

| Benchmark | Command (Release build) |
|---|---|
| exFAT | `FW_EXFAT_BENCH_MIB=1024 tst_exfat_bench` |
| UFS2 | `tst_ufs2_roundtrip benchmark` (+ `FW_UFS2TOOL_DLL` for the comparison) |
| PFS | `QTEST_FUNCTION_TIMEOUT=3600000 FW_PFS_BENCH_MIB=320 FW_PFS_BENCH_LEVEL=9 FW_PFS_BENCH_REPEAT=3 tst_pfs_benchmark` |

## Not done yet

* Unbuffered I/O (`FILE_FLAG_NO_BUFFERING`) for very large images on Windows
  (the legacy `robocopy /J`), to keep the system file cache from filling up
  during a 100 GB build.
* zlib-ng / ISA-L compression back-ends (the legacy `pfs_lazy_zlib_backend`);
  the native engine uses zlib only.
