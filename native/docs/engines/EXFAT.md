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
the console. Legacy references (the Python app of versions 1 to 4, no longer
in this repository): `New-OsfExfatImage.ps1` and `make_image.bat`
(embedded base64 in `exfat_builder.py` as `_PS1_B64` / `_BAT_B64`; decode them
to read), `exfat_builder.py` (`verify_walk_tree`, `_get_cluster_size_arg`,
`set_aside_previous_image`), `ui/tab_advanced.py` (Filesystem layout card).

| Decision | Native engine | Legacy reference |
|---|---|---|
| Container | **Bare volume**: the boot sector is at offset 0, no MBR/GPT. `PartitionOffset` = 0. | PS1 mounts the raw file with `osfmount -a -t file -f <img> -s <size> -m X: -o rw,rem` as a *logical* volume, then `format.com X: /FS:exFAT` formats the whole file. |
| Sector size | 512 bytes (setting `exfat.sectorSize`: 512 or 4096) | `adv_sector`, default 512. The PS1 says 4096 is "honoured by format.com", but `format.com` has no sector option and OSFMount was never told, so legacy images were always 512. 4096 needs the loader configured (`lvd_exfat_sector_size`). |
| Cluster size, automatic | **Always 64 KiB** (`kAutoClusterBytes`). ShadowMount+ 1.7 mounts exFAT through its LVD type-5 / BFS fast path only with 64 KiB clusters (SMP 1.7 `New-OsfExfatImage.ps1`, `mkexfat.sh`). The other sizes stay available as explicit choices, and the log says when one is used. | Legacy `Get-OptimalExfatClusterSizeFromStats` picked 64 KiB when the average file was ≥ 1 MiB, else 32 KiB (its Advanced page said "Auto (64K)"). That rule is obsolete since SMP 1.7. |
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
| Free space | Full image size + `storage.freeSpaceMarginMiB` before writing; FAT32 output refused for images ≥ 4 GiB (also for a rebuild edit that grows past 4 GiB, and for extracting a file ≥ 4 GiB onto FAT32) | PS1 preflight: image + 256 MB |
| Links | File links are followed (content copied). Folder links/junctions are followed unless they point back into their own parent chain (skipped with a warning). A broken link stops the build. A Windows shortcut (`.lnk`) is not a link: it is copied as the small file it is, whatever it points at. | robocopy default |

## Jobs

| Kind | Parameters | Phases | Notes |
|---|---|---|---|
| `image.exfat.build` | `source` (a folder, or a ZIP of a dump, which is unpacked into the job's temporary folder first), `output` (file, or an existing folder → `<source name>.exfat`, without `.zip` for a ZIP), optional `label`, optional `excludes[]` | Scanning → Writing image → Verifying | Writes `<output>.part`, verifies the `.part`, then renames (an existing image is set aside by `PartFile` and restored if the rename fails). Verification failure: nothing is renamed, an older image stays. If the rename fails (the old image is mounted or open elsewhere) the finished image is kept as `<name> (new).exfat` and the error says so. |
| `image.exfat.verify` | `image`, optional `source`, optional `excludes[]` | Reading image → Comparing | With `source`: `verification.mode` decides full or structure. Without it the image is checked on its own: structure, and in full mode every file is read back. In both modes a damaged image (boot region and backup boot region included) is a failed verification, not an error. |
| `image.exfat.extract` | `image`, `output` (new or empty folder) | Reading image → Extracting → Verifying | Extracts into `<output>.part` (with a marker file so a stale one can be recognised as ours; an image whose root holds that name in any case is refused), verifies it against the image, then renames. A `.part` folder without the marker is never touched. If the rename fails the extracted folder is kept as `<output>.new`. Every output path is checked to stay inside the folder (`core/PathSafety.h`); names exFAT allows but the drive cannot store as they are (on Windows trailing dots or spaces and device names such as `CON`; anywhere names made only of dots and spaces) are changed, reported as "Renamed for this drive" warnings and verified under their image names. |
| `image.exfat.edit` | `image`, `operations[]` of `{op: add\|replace\|delete\|mkdir, imagePath, localPath}` | Planning → Writing → Verifying | See *Editing images*. Always verified (mode from `verification.mode`). |

Every job checks `ctx.token` at least every 4 MiB and every 64 files (the
undo journal of an in-place edit included), logs
milestones with `info` and buffer/timing detail with `debug`, and returns
per-file mismatch details in `JobOutcome::details.verification`
(`mismatches[]` of `{path, kind, detail}`, up to 200, plus counts).

**Images from elsewhere** (`ExfatReader`). The reader ends on any input, in
time and memory proportional to the file: a `VolumeLength` whose byte size
overflows is refused, so the cluster count and both bitmaps are bounded by
the real file size; a FAT chain is followed for at most the clusters its
data length needs (256 MiB for the root folder); a folder whose clusters
already belong to something else (for example to one of its own parents) is
reported and not listed again; the walk stops once the entries claim more
clusters than the volume has, at 256 nested folders, or at 10,000,000
entries. All of these are reported as damage.

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
warning. A ZIP source is never deleted. `removeTreeSafely` refuses drive
roots and the home folder.

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
  for the journal plus the margin is checked first. Writing the journal is
  part of the Writing phase (progress, Cancel). Once the edit has verified the
  journal is deleted; if another program holds it, it is emptied, or, when it
  cannot even be opened, disowned by a marker `<image>.edit-journal.done`
  holding its header and checksum. Recovery removes an empty or disowned
  journal without applying it, so a verified edit is never rolled back.
* **Rebuild** for everything else (adds, mkdir, replacements that need more
  clusters, `exfat.editStrategy = rebuild`): the edited tree is written to
  `<image>.part` — unchanged files are streamed from the original image —
  with the original cluster size, sector size, label and serial; verified
  against the expected tree (unchanged files byte-compared with the
  original); then swapped in with set-aside/restore (if the swap fails
  because the original is open elsewhere, the edited image is kept as
  `<name> (new).exfat`). If the edited contents
  no longer fit, the image grows using the automatic size rule. Free space for
  the whole new image is checked first.

Operations that cancel each other out (create a folder, then delete it) write
nothing: `EditResult::changed` is false and the job reports no verification.

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
| `exfat.amprIndex` | Build | `auto` | yes | none (the legacy app always rebuilt the index, in the dump) |
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
`storage.freeSpaceMarginMiB`. The legacy exFAT switches
`adv_skip_verify` → `verification.verifyAfterBuild` (inverted) and
`adv_checksum_verify = true` → `verification.mode = full` (off keeps the new
default, full, instead of downgrading to the legacy size walk) are mapped by
the general module (`engines/general/Module.cpp`), which owns those keys.

Legacy options deliberately not carried over: OSFMount path (no OSFMount),
robocopy threads (`adv_threads`; the writer is sequential by design and cannot
fragment), robocopy log, write benchmark, auto-upload (FTP module), naming
presets (Build page / gameinfo).

### ampr_emu.index (`exfat.amprIndex`)

A game with `fakelib/libSceAmpr.sprx` needs `ampr_emu.index` (the AMPR
emulator's list of the game's files, format in `PFS.md`) at its top folder.
The legacy app wrote one into the dump before every build
(`_maybe_build_ampr_index`). Here the dump folder is never written: the build
decides after the scan (`ExfatAmpr.*`, `pfs::decideAmprIndex`), writes a new
index into the job's temporary folder and puts that file into the scanned tree
as the root's `ampr_emu.index`, in place of the dump's own entry or as a new
entry where the scanner would have listed it. A build that adds or changes
nothing leaves the tree alone, so its image is byte for byte what it was before
the setting existed.

| Value | Label | What the build does when the game has `fakelib/libSceAmpr.sprx` |
|---|---|---|
| `auto` (default) | When the game uses the AMPR emulator | Adds the index when the dump has none; rebuilds it when the dump's differs from a freshly built one in more than file times (`pfs::amprIndexesDifferOnlyInTimes`, so a copied or re-saved dump is not "outdated"). A game with asset packs (`ampr_zstd.index`, `ampr_assets.index[.runtime]` at the top) keeps its index: it lists the files inside the packs. |
| `missing` | Add when the dump has none | Adds one; a dump's own index is copied as it is. |
| `refresh` | Always rebuild it | Adds or replaces it. |
| `off` | Never add or change it | Nothing. |

The rows are the files the image holds (exclusions applied) by the rules of
the AMPR index tool (`pfs::amprToolRows`), so an index made with
*AMPR index (folder)* is up to date. The job log says what was done:
`ampr_emu.index: added (the dump has none)`, `rebuilt (it did not match the
game's files)`, `kept (up to date)`, `not needed (no
fakelib/libSceAmpr.sprx)`; the outcome details carry `amprIndex` (`added` /
`rebuilt`) when the image differs from the folder. The build's verification
scans the folder again and expects the new index in place of the folder's.
`image.exfat.verify` against a folder accepts an image whose `ampr_emu.index`
is the index of the folder's files (also with other file times) when the
folder has none or another one. `image.compare` (Image tools › Verify against
a folder) lists such an index as the one difference it is and adds a line
saying where it comes from. `image.convert` and the zstd build turn the
setting off for their own builds: the tree they hand over goes in as it is.

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
* `tst_exfat_ampr` — `exfat.amprIndex`: a missing index is added and an outdated
  one rebuilt in the image only (every file of the dump hashed before and
  after), an up-to-date one (also with other file times) kept byte for byte,
  a packed game's index kept, `off` and the other choices, a game without the
  AMPR library builds the bytes it built before, the verify job, source
  deletion.
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
