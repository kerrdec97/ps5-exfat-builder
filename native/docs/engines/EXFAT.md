# exFAT engine (`src/engines/exfat`)

The native exFAT engine builds, reads, verifies, extracts and edits the raw
`.exfat` volume images that PS5 homebrew loaders (ShadowMount, etaHEN mount
helpers) mount. It is written in C++ from the public Microsoft *exFAT file
system specification* (2019 release; § numbers below refer to it). It needs no
external tool, no administrator rights and no mounted drive: the legacy
OSFMount + `format.com` + `robocopy` pipeline is gone.

| File | Role |
|---|---|
| `ExfatFormat.*`, `UpcaseData.cpp` | On-disk structures: checksums, timestamps, name rules, up-case table, geometry, boot region |
| `SourceScan.*` | Scans a source folder with the exclusion rules (names, sizes, attributes, times) |
| `ExfatWriter.*` | Plans (names, cluster size, image size, cluster assignment) and writes an image in one sequential pass |
| `ExfatReader.*` | Opens and validates an image, walks it, streams file contents (`ImageFileStream`) |
| `ExfatVerify.*` | Strict image ↔ folder (or ↔ expected tree) comparison, full or structure |
| `ExfatExtract.*` | Image → folder, sequential or parallel, timestamps and attributes kept |
| `ExfatEdit.*` | Add / replace / delete / mkdir on an existing image, in place (undo journal) or by rebuild |
| `ExfatJobs.*`, `Module.*` | Queue jobs and the `exfat.*` settings |
| `Pipeline.h`, `PlatformFs.*` | Reader→writer buffer pool; fsync, folder times, Windows attributes, retrying opens |

## Format decisions

The layout follows what the legacy app produced, so images behave the same on
the console. Legacy references: `New-OsfExfatImage.ps1` and `make_image.bat`
(embedded base64 in `exfat_builder.py` as `_PS1_B64` / `_BAT_B64`; decode them
to read), `exfat_builder.py` (`verify_walk_tree`, `_get_cluster_size_arg`,
`set_aside_previous_image`), `ui/tab_advanced.py` (Filesystem layout card).

| Decision | Native engine | Legacy reference |
|---|---|---|
| Container | **Bare volume**: the boot sector is at offset 0, no MBR/GPT. `PartitionOffset` = 0. | PS1 mounts the raw file with `osfmount -a -t file -f <img> -s <size> -m X: -o rw,rem` as a *logical* volume, then `format.com X: /FS:exFAT` formats the whole file. |
| Sector size | 512 bytes (setting `exfat.sectorSize`: 512 or 4096) | `adv_sector`, default 512. The PS1 says 4096 is "honoured by format.com", but `format.com` has no sector option and OSFMount was never told, so legacy images were always 512. 4096 needs the loader configured (`lvd_exfat_sector_size`). |
| Cluster size, automatic | **64 KiB if the average file is ≥ 1 MiB, else 32 KiB** (average rounded half-to-even, as PowerShell's `[Int64]` cast does). 32 KiB for an empty tree. | `Get-OptimalExfatClusterSizeFromStats`. Note: the Advanced page labels Auto as "Auto (64K)", but the script really picks 32 KiB for small-file games. The native label says "Automatic (32 or 64 KB)". |
| Cluster size, explicit | 16 KB … 1 MB (`exfat.clusterSize`); the writer supports 512 B … 32 MiB | `adv_cluster`: Auto, 32K, 64K, 128K, 256K |
| Image size, automatic | `legacyImageBytes()`: data = Σ ⌈size/cluster⌉·cluster; FAT = 4 B per data cluster; bitmap; 512 B per file and folder; + 64 MiB; spare = 1 % of that, at least 1 GiB, at most 8 GiB; at least raw bytes + 1 GiB; rounded up to 1 MiB. If the exact layout needs more (never seen), the size grows to fit. | `Get-OptimalImageSizeBytesFromStats` |
| Image size, fixed | `exfat.imageSizeGiB` × 1024³, rounded down to 1 MiB; fails with "too small … needs at least X" when the tree does not fit | `adv_img_size_override` and the per-item Build size (`size_override_gb`), both GB × 1 GiB |
| Free space in automatic images | `exfat.spareSpace`: Standard (the legacy rule above) or Fixed (`exfat.spareSpaceMiB`, smallest image that keeps that much free) | Legacy had only the standard rule |
| Label | `OSFIMG` by default; policies Fixed / Title ID / None; job parameter `label` wins | The bat never passes `-Label`, so the PS1 default `OSFIMG` was on every image (the close-time sweep keyed on it) |
| FAT and heap alignment | FAT and cluster heap each start on a multiple of max(1 MiB, cluster size) (FAT at 1 MiB for clusters up to 1 MiB); one FAT (`NumberOfFats` = 1) | `format.com` chose its own alignment; any spec-valid layout mounts. 1 MiB matches exfatprogs' `mkfs.exfat` default. |
| Up-case table | The specification's recommended compressed table (§7.2.5.1, 5836 bytes, checksum `0xE619D30D`) | What Windows `format.com` and `mkfs.exfat` write |
| Allocation | Cluster 2: allocation bitmap, then up-case table, root directory, every sub-directory (depth-first, name order), then file data in the same order. **Every file and folder is one contiguous run marked NoFatChain**; the FAT holds chains only for the bitmap, the up-case table and the root directory (§6.4.2). `exfat.allocation = fatChain` writes chains for everything instead. The bitmap is a prefix of set bits. | Windows writes NoFatChain for files copied onto a fresh volume; the legacy Threads = 1 default existed to avoid fragmentation, which cannot happen here |
| Empty files and folders | Empty file: `AllocationPossible`, cluster 0, size 0. Every folder (even empty) gets one cluster. | As Windows |
| Names | UTF-16, 1–255 code units, the §7.7.3 forbidden characters refused, case-insensitive duplicates refused ("differ only in upper/lower case"), entries sorted by up-cased name | Windows sources cannot have such names; Linux sources can |
| Timestamps | Created, modified, accessed of files **and folders**, local time with its UTC offset (`OffsetValid`), 10 ms resolution (2 s for accessed), clamped to 1980–2107 | robocopy `/COPY:DAT /DCOPY:DAT` onto a Windows-mounted volume |
| Attributes | Windows: read-only, hidden, system, archive copied. Elsewhere: archive (+ read-only when not writable). | robocopy `/COPY:DAT` |
| Exclusions | `exfat.excludePatterns` (default `*.exfat *.ffpkg *.ffpfs *.ffpfsc Thumbs.db desktop.ini .DS_Store`, `System Volume Information/`, `$RECYCLE.BIN/`), `exfat.excludeHidden` (files only), job parameter `excludes`. The verifier applies the same rules. | robocopy `/XF *.exfat /XF *.ffpkg`, `/XA:H` with "Exclude hidden files"; `verify_walk_tree` additionally skipped `.ffpfs/.ffpfsc` (inconsistent, fixed here) |
| Source check | `eboot.bin` must be at the top of the source (`exfat.requireEboot`) | bat `:no_eboot`, PS1 check |
| Free space | Full image size + `storage.freeSpaceMarginMiB` before writing; FAT32 output refused for images ≥ 4 GiB | PS1 preflight: image + 256 MB |
| Links | File links are followed (content copied). Folder links/junctions are followed unless they point back into their own parent chain (skipped with a warning). A broken link stops the build. | robocopy default |

## Jobs

| Kind | Parameters | Phases | Notes |
|---|---|---|---|
| `image.exfat.build` | `source`, `output` (file, or an existing folder → `<source name>.exfat`), optional `label`, optional `excludes[]` | Scanning → Writing image → Verifying | Writes `<output>.part`, verifies the `.part`, then renames (an existing image is set aside by `PartFile` and restored if the rename fails). Verification failure: nothing is renamed, an older image stays. |
| `image.exfat.verify` | `image`, `source`, optional `excludes[]` | Reading image → Comparing | `verification.mode` decides full or structure. A damaged image is a failed verification, not an error. |
| `image.exfat.extract` | `image`, `output` (new or empty folder) | Reading image → Extracting → Verifying | Extracts into `<output>.part` (with a marker file so a stale one can be recognised as ours), verifies it against the image, then renames. A `.part` folder without the marker is never touched. |
| `image.exfat.edit` | `image`, `operations[]` of `{op: add\|replace\|delete\|mkdir, imagePath, localPath}` | Planning → Writing → Verifying | See *Editing images*. Always verified (mode from `verification.mode`). |

Every job checks `ctx.token` at least every 4 MiB and every 64 files, logs
milestones with `info` and buffer/timing detail with `debug`, and returns
per-file mismatch details in `JobOutcome::details.verification`
(`mismatches[]` of `{path, kind, detail}`, up to 200, plus counts).

**Verification** (`ExfatVerify`). Strict (audit H1): any missing, extra,
different-type, different-size file or folder fails, as does any structural
damage (bad boot or backup boot region, up-case checksum, entry-set checksum,
name hash, cluster range, FAT chain, cross-linked clusters, clusters in use
but free in the bitmap). Full mode additionally compares every byte, several
files at once (`performance.workerThreads`), in image order; a mismatch names
the first differing byte. Lost clusters (allocated, unused) are a warning.

**Deleting the source** (`exfat.deleteSourceAfterVerify`, audit C2) happens
only when full byte verification passed, the image got its final name, and
nothing in the source was excluded; otherwise the folder is kept with a
warning. `removeTreeSafely` refuses drive roots and the home folder.

## Editing images

The legacy Edit › exFAT "File Manager" mounted the image read-write with
OSFMount and copied files in place. The native editor works on the file
directly and never loses the original before the edited image has verified
(audit M2):

* **In place**, when provably safe: deleting files or folders (InUse bits
  cleared, clusters freed in the bitmap) and replacing a file with one that
  fits its existing contiguous clusters (data overwritten, tail clusters
  freed, entry set rewritten with the new size, times and attributes).
  Before the first byte changes, everything about to be overwritten is saved
  to `<image>.edit-journal` (written as `.part`, flushed, renamed, so a
  journal under its final name is always complete; FNV-1a checksum). A
  failure, a cancellation or a failed verification rolls the image back from
  the journal: the file is byte-identical to before. A journal left by a crash
  is rolled back by the next edit, verify or extract job on that image (or
  `recoverInterruptedEdit`); a damaged journal is refused rather than applied.
  The journal records the image's size and volume serial and is never applied
  to any other image (a build that replaces the image deletes it). Free space
  for the journal plus the margin is checked first.
* **Rebuild** for everything else (adds, mkdir, replacements that need more
  clusters, `exfat.editStrategy = rebuild`): the edited tree is written to
  `<image>.part` — unchanged files are streamed from the original image —
  with the original cluster size, sector size, label and serial; verified
  against the expected tree (unchanged files byte-compared with the
  original); then swapped in with set-aside/restore. If the edited contents
  no longer fit, the image grows using the automatic size rule. Free space for
  the whole new image is checked first.

Paths inside the image are case-insensitive like exFAT itself; `add` into an
existing folder keeps the local name; adding a folder applies the exclusion
rules; `replace` only takes files; parent folders are created as needed.

## Settings

All keys are read by `readSettings()` in `ExfatJobs.cpp`; `perJob` ones may be
overridden for a single job.

| Key | Section | Default | Per job | Legacy key |
|---|---|---|---|---|
| `exfat.clusterSize` | Build | `auto` | yes | `adv_cluster` (`Auto` → `auto`) |
| `exfat.sectorSize` (advanced) | Build | `512` | | `adv_sector` |
| `exfat.imageSizeGiB` | Build | 0 (automatic) | yes | `adv_img_size_override` (blank → 0) |
| `exfat.spareSpace` | Build | `legacy` | yes | |
| `exfat.spareSpaceMiB` | Build | 256 | yes | |
| `exfat.labelPolicy` | Build | `fixed` | yes | |
| `exfat.label` | Build | `OSFIMG` | yes | |
| `exfat.requireEboot` | Build | on | yes | |
| `exfat.excludePatterns` | Build | see above | yes | |
| `exfat.excludeHidden` | Build | off | yes | `adv_excl_hidden` |
| `exfat.overwriteExisting` | Build | on | yes | |
| `exfat.allocation` (advanced) | Build | `contiguous` | | |
| `exfat.openRetries` (advanced) | Build | 3 | | `adv_retries` (robocopy `/R`) |
| `exfat.retryWaitSeconds` (advanced) | Build | 3 | | `adv_retry_wait` (robocopy `/W`) |
| `exfat.editStrategy` | Build | `auto` | yes | |
| `exfat.extractParallel` | Storage | off | yes | `exfat_extract_robocopy_mt` |
| `exfat.extractThreads` | Storage | 32 | yes | `exfat_extract_mt_threads` |
| `exfat.deleteSourceAfterVerify` (advanced) | Verification | off | yes | `adv_delete_source` |

Shared settings read by this module: `verification.verifyAfterBuild`,
`verification.mode`, `performance.workerThreads`, `performance.ioBufferMiB`,
`storage.freeSpaceMarginMiB`. The module also maps the legacy exFAT switches
`adv_skip_verify` → `verification.verifyAfterBuild` (inverted) and
`adv_checksum_verify = true` → `verification.mode = full` (off keeps the new
default, full, instead of downgrading to the legacy size walk).

Legacy options deliberately not carried over: OSFMount path (no OSFMount),
robocopy threads (`adv_threads`; the writer is sequential by design and cannot
fragment), robocopy log, write benchmark, auto-upload (FTP module), naming
presets (Build page / gameinfo).

## Performance

* One sequential pass: metadata first (a few MiB), then file data in image
  order. A reader thread fills a pool of `bufferCount` (6) × `performance.ioBufferMiB`
  buffers while the job thread writes them, so reading and writing overlap and
  memory stays bounded whatever the image size. File contents are never held
  in memory as a whole.
* Zero runs of 1 MiB or more inside file data, and the free space at the end,
  are left as holes (resize + seek): sparse on ext4/btrfs/NTFS-sparse, lazily
  zeroed (valid data length) on NTFS/exFAT. Smaller gaps are written as zeros.
* The image is flushed to the device (`FlushFileBuffers`/`fsync`) before it is
  verified and renamed.
* Full verification compares several files at once; extraction can write
  several files at once (helps Windows with many small files and Defender).

Measured in the development VM (Linux, 4 vCPU, virtio disk, Release build,
`tst_exfat_bench` with `FW_EXFAT_BENCH_MIB=1024`: 2,005 files, 1,032 MiB of
random data, page cache warm for reads):

| Operation | Throughput |
|---|---|
| `dd` copy of 1 GiB with `conv=fsync` (baseline) | 178–279 MB/s |
| Write image (incl. fsync) | 197–290 MB/s — at disk speed |
| Full verify, 1 thread / 4 threads | 1.4–2.2 GB/s / 1.7–4.5 GB/s |
| Structure verify | 32–60 ms for the whole tree |
| Extract sequential / parallel (16 files) | 0.16–2.4 GB/s / 0.4–4.6 GB/s |

The extraction figures swing because this VM faults guest memory in lazily:
the first large batch of new page-cache pages is slow whichever mode runs
first. Real-world numbers are bounded by the drives.

Not done yet: unbuffered I/O (`FILE_FLAG_NO_BUFFERING`) for very large images
on Windows (the legacy `robocopy /J`), which would keep the system file cache
from filling up during a 100 GB build.

## Limits

* Names up to 255 UTF-16 code units; one folder holds at most 256 MiB of
  directory entries (~400,000 long names) (§6.2).
* Cluster count ≤ 2³² − 11: a 32 KiB cluster allows images up to 128 TiB.
* One FAT; no TexFAT, no volume GUID entry, no partition table option.
* Timestamps have 10 ms resolution (accessed: 2 s); creation time cannot be
  set on Linux when extracting.
* Outside Windows, "hidden" means a name starting with a dot, and only the
  read-only attribute can be restored on extraction.
* The reader accepts any spec-valid image (FAT chains, two FATs, foreign
  layouts, mkfs.exfat volumes); the in-place editor only rewrites files that
  are single NoFatChain runs, otherwise it rebuilds.

## Tests

`native/tests/engines/exfat/` (link only `fw_engine_exfat`):

* `tst_exfat_format` — checksums, up-case table (size, checksum, mappings),
  name hash, boot region round trip and damage, geometry limits, timestamps
  with offsets, names and labels, the legacy cluster and size rules against
  the PowerShell formulas, reading a volume made by `mkfs.exfat` (skipped
  without exfatprogs).
* `tst_exfat_roundtrip` — synthetic trees (nested, Unicode, 255-character
  names, empty files and folders, 1,500 small files, a few MiB of random data)
  written with several sector/cluster/allocation options, `fsck.exfat -n`
  clean (skipped without exfatprogs), read back byte for byte; damaged boot
  sector, backup boot region, directory entry, bitmap, up-case table and
  truncated images detected; strict verification (changed byte, missing,
  extra, size); extraction round trip with timestamps; a sparse 5 GiB file.
* `tst_exfat_jobs` — the jobs through the factory, every setting read, legacy
  import, validation, cancellation (no output, no `.part`, previous image
  kept), insufficient space, replacing images, source deletion gating, the
  verify and extract jobs.
* `tst_exfat_edit` — in-place deletes and replacements, rebuild and growth,
  cancellation and failed verification leaving the image byte-identical,
  interrupted-edit recovery, the edit job.
* `tst_exfat_bench` — logs MB/s for write, verify and extract.

Independent cross-check (manual, Linux as root): images from the round-trip and
edit tests (`FW_EXFAT_KEEP_SAMPLE=<dir>`, `FW_EXFAT_KEEP_EDIT=<dir>`) mounted
read-only with the FUSE exFAT driver (`mount.exfat-fuse` on a loop device)
and `diff -r` against the expected folders: identical. `fsck.exfat` checks
entry-set checksums, name hashes, chains and the bitmap, so a clean result is
meaningful.
