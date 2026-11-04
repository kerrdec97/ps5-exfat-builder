# PFS / FFPFSC engine (`fw_engine_pfs`)

Native C++ port of **Lazy_MkPFS** (Nazky, <https://github.com/Nazky/Lazy_MkPFS>,
vendored at `vendor/lazy_mkpfs`, upstream commit
`78bdd00c5645b1170420df051c586dd0ca4f918d`), which is itself derived from
**MkPFS** by PSBrew (<https://github.com/PSBrew/MkPFS>). Both are GPL-3.0; this
port is distributed under GPL-3.0-or-later with the native app, and every
source file names what it was ported from. The legacy Python app and the
vendored package are not modified.

For the same input and options the engine writes **byte-identical** images to
Lazy_MkPFS (see [Compatibility](#compatibility)); it is multithreaded,
streaming (no file is held in memory, the nested image of a folder build is
never written to disk), deterministic for any number of threads, and it
verifies what it wrote before the image gets its final name.

## Contents

| File | What it does | Ported from |
|---|---|---|
| `PfsFormat.*` | constants, little-endian helpers, D32 inode, dirent, header block, FPT hash, PFSC header | `consts.py`, `types.py`, `build.py` (`_pack_pfs_header_block`, `build_inode_block_sig_s64`), `inspect.py` (`fpt_hash`), `compression.py` |
| `PfsSource.*` | source scan → `SourcePlan` (order, ASCII rule, exclusions, `.pfskeep`, AMPR) | `build.py` `scan_source_tree`, `ampr_index.py` `ensure_ampr_index` |
| `AmprIndex.*` | `ampr_emu.index` (AMPRIDX3) | `ampr_index.py` |
| `PfsLayout.*` | inode numbering, dirents, flat_path_table, collision_resolver, block allocation | `build.py` `build_pfs` (unsigned branch), `build_pfs_stream_single_file`; `inspect.py` `make_fpt_and_collision_blob` |
| `PfscCodec.*` | bounded deflate worker pool, inflater | `compression.py` |
| `PfsIo.*` | sequential sources (file, memory, virtual folder image), buffered output writer | — |
| `PfsBuilder.*` | ordered compression pipeline; folder image, per-file compression, single-file container, nested container | `build.py`, `compression.py` (`_encode_pfsc_into_handle`) |
| `PfsReader.*` | open + validate an image, tree, streaming decode, random access into a compressed file | `inspect.py` (`inspect_pfs_image`, `iter_inode_logical_blocks`, …) |
| `PfsVerify.*` | strict verification, extraction | `inspect.py` (`validate_source_match`, `extract_pfs_image`) |
| `PfsJobs.*`, `Module.*` | queue jobs, settings, legacy keys | — |

## What it builds

| Output | Source | Equivalent | Notes |
|---|---|---|---|
| **.ffpfsc** (container) | game folder | legacy PFS tab: `mkpfs pack folder --no-compress … pfs_image.dat` then `mkpfs pack file pfs_image.dat out.ffpfsc` | The nested uncompressed folder image `pfs_image.dat` (the name ShadowMount+ requires) is generated on the fly from the source files and compressed directly — never written to disk. |
| **.ffpfsc** (container) | image file (`.exfat`, `.ffpkg`, …) | Lazy `main_file.py SRC OUT` (the path the legacy app drives Lazy through), MkPFS `pack file` | The file keeps its name inside the container. |
| **.ffpfs** (folder image) | game folder | MkPFS `pack folder --no-compress` | Default: files stored raw. |
| **.ffpfs** with per-file compression | game folder | Lazy `main_folder.py --no-exfat` | Off by default (`pfs.compressFiles`): compressed files grow to whole blocks (see below). |
| .ffpfs (single file, raw) | image file | Lazy `main_file.py --no-compress` | Rarely useful; supported because Lazy offers it. |

Lazy's default `main_folder.py` mode (folder → temporary exFAT image via
mkfs/OSFMount → container) needs the exFAT engine; the app chains an
`exfat` job and an `image.pfs.build` job with the `.exfat` as source instead.

## On-disk format

All integers are little-endian. "bs" is the PFS block size (64 KiB by default;
4–64 KiB supported). Unsigned, unencrypted images only.

### Block order of a folder image

```
block 0            header
block 1 … ibc      inode table: D32 inodes (0xA8 bytes), floor(bs / 0xA8) per block, never split
block 1+ibc        super root: dirents flat_path_table, [collision_resolver], uroot (no "." / "..")
next               flat_path_table (fpt.blocks = max(1, ceil(len / bs)))
next               collision_resolver (if any FPT hash collides) — otherwise ONE reserved zero block
next               uroot directory, then every other directory in case-insensitive path order
next               file payloads in case-insensitive path order, each starting on a block
ndblock            = end; image size = ndblock × bs
```

`ibc = ceil(inode_count / floor(bs / 0xA8))`. A payload occupies
`max(1, ceil(stored / bs))` blocks (an empty file still takes one block).

### Header (block 0)

| Offset | Type | Value |
|---|---|---|
| 0x00 | i64 | version: 2 (PS5) or 1 (PS4) |
| 0x08 | i64 | magic 20130315 |
| 0x10 | i64 | 0 |
| 0x18 | u8 ×4 | 0, 0, **1 (read-only)**, 0 |
| 0x1C | u16 | mode: 0x8 case-insensitive (0x1 signed, 0x2 64-bit inodes, 0x4 encrypted — never set) |
| 0x20 | u32 | block size |
| 0x28 | i64 | nblock = 1 |
| 0x30 | i64 | inode count |
| 0x38 | i64 | ndblock |
| 0x40 | i64 | inode-table block count (ibc) |
| 0x50 … 0x35F | S64 inode | the inode table described as an inode: nlink 1, flags 0x10, size = ibc × bs (twice), four times = build time, blocks = ibc, `db[i] = 1 + i` (i < min(ibc, 12)) at 0x68 + 40·i + 32 |
| 0x368 | u32 | 1 (unsigned/unencrypted marker) |

### Inode (D32, 0xA8 bytes)

| Offset | Type | Field |
|---|---|---|
| 0x00 | u16 / u16 / u32 | mode, nlink, flags |
| 0x08 | i64 | `size` — **stored** payload size (the PFSC stream for compressed files) |
| 0x10 | i64 | `size_compressed` — **logical** size; for compressed files `ceil(raw / bs) × bs` |
| 0x18 | i64 ×4 | atime, mtime, ctime, birthtime = build time (seconds) |
| 0x38 | u32 ×4 | nanoseconds = 0 |
| 0x48 | u32, u32, u64, u64 | uid, gid, two unknown = 0 |
| 0x60 | u32 | blocks |
| 0x64 | i32 ×12 | direct blocks: `db[0]` = first block, `db[1…11] = −1` (super root: 0) |
| 0x94 | i32 ×5 | indirect blocks = 0 |

Modes: directories 0x4000|0x16D, files 0x8000|0x16D (r-x for all). Flags:
0x10 read-only on every inode, 0x20000 internal (super root, FPT, collision
resolver), 0x1 compressed. nlink: super root 1, uroot 3 + subfolders, other
folders 2 + subfolders, files 1.

Inode numbers: 0 super root, 1 flat_path_table, 2 uroot, then folders, then
files (both in case-insensitive path order). If the FPT has a collision the
collision_resolver becomes inode 2 and everything from uroot on moves up by one.

### Directory entry

`u32 inode, i32 type, i32 name_len, i32 ent_size, name, zero padding` with
`ent_size = align8(name_len + 17)`. Types: 2 file, 3 folder, 4 ".", 5 "..".
Every folder starts with "." and ".." (uroot's ".." is itself), then
subfolders, then files, each group in case-insensitive name order. Names are
ASCII only. A folder's inode size is its block count × bs.

### flat_path_table and collision_resolver

For every folder (except uroot) and file, `h = fpt_hash("/" + path)` with
`h = (c + 31·h) mod 2³²` over the characters, upper-cased ASCII when the image
is case-insensitive. Entries `u32 hash, u32 value` sorted by hash;
`value = inode | 0x20000000` for folders, or `0x80000000 | offset` into the
collision_resolver when several paths share the hash. The resolver lists, per
colliding hash in hash order, a dirent per path (name = full path, type 3/2)
followed by 0x18 zero bytes.

### PFSC (compressed file payload)

| Offset | Type | Value |
|---|---|---|
| 0x00 | i32 | magic 0x43534650 ("PFSC") |
| 0x04 | i32 | 0 |
| 0x08 | i32 | 6 |
| 0x0C | i32 | logical block 0x10000 |
| 0x10 | i64 | 0x10000 |
| 0x18 | i64 | offset of the offset table: 0x400 |
| 0x20 | u64 | data offset = header size |
| 0x28 | i64 | logical length = blocks × 64 KiB |
| 0x400 | u64 × (n+1) | offset of each stored block (relative to the payload), plus the end |

`n = ceil(raw / 64 KiB)`; header size = 0x10000 plus 64 KiB for every 8 192
blocks the offset table needs beyond 0xFC00 bytes. Each 64 KiB block (the last
one zero-padded) is compressed on its own with zlib (`zlib.compress` = window
15, memLevel 8, default strategy) and stored compressed **only if** the result
is smaller than 64 KiB **and** saves at least the block threshold
(`(65536 − len) / 65536 × 100 ≥ threshold`, evaluated in IEEE doubles exactly
like Python); otherwise the padded raw block is stored (stored length 65 536
means "raw"). The whole file is stored compressed only if at least one block
compressed, the PFSC stream is smaller than the file, and the file saves at
least the minimum file gain; otherwise it is stored raw. The PFSC logical block
is always 64 KiB, whatever the PFS block size.

A compressed file's logical size is `ceil(raw / bs) × bs`: **the PFS format
loses the exact size** of a compressed file and the console sees zero bytes up
to the block boundary. That is why per-file compression is off by default and
why the container stores a whole image file (whose own size is block-aligned).

### Container (single-file image)

Inodes: 0 super root, 1 flat_path_table (one entry, 8 bytes), 2 uroot (`.`,
`..`, the file), 3 the file. Blocks: 0 header, 1 inode table, 2 super root,
3 flat_path_table, 4 reserved (zero), 5 uroot, 6… payload.

Lazy writes the compressed attempt first and, if the file does not shrink,
writes the raw file over it: bytes of the attempt beyond the end of the file
stay in the file's last block (inside the image, outside the file). The port
reproduces this exactly (golden case `file_container_random_fallback`).

### ampr_emu.index (AMPRIDX3)

Generated when the dump has `fakelib/libSceAmpr.sprx` (and, by default, no
index yet). Header `"AMPRIDX3", u32 3, u32 24, u64 rows, u64 path blob
length, u64 hash offset, u32 16, u32 slots`; records `u32 path offset, u32 path
length, u64 size, i64 mtime (s)` for every packed file as `/app0/<path>`,
sorted by lower-cased path (case-insensitive duplicates: first kept); NUL-
terminated path blob; padding to 16; open-addressed hash slots (FNV-1a 64 of
the lower-cased path, 0 → 1; linear probing; 2ⁿ ≥ 2 × rows slots; `u64 hash,
u32 row + 1, u32 flags`, bit 0 set on the earlier slot when a later row has
the same 64-bit hash).

## Settings (`pfs.*`)

All are read by `buildOptionsFrom` / `planOptionsFrom` / `threadsFrom` /
`verifyDepthFrom` or the jobs (test `settingsAreRead`), all may be overridden
per job.

| Key | Section · group | Default | Lazy / MkPFS / legacy equivalent |
|---|---|---|---|
| `pfs.compressionLevel` | PFS · Compression | 9 | `--zlib-level` / `--compression-level`; legacy `pfs_compression_level` (default 9) |
| `pfs.threads` | PFS · Compression (adv.) | 0 = `performance.workerThreads` | `--cpu-count`; legacy `pfs_lazy_cpu_count` |
| `pfs.containerMinCompressMiB` | PFS · Compression (adv.) | 1.0 | `main_file.py --min-compress-size` |
| `pfs.containerBlockGain` | PFS · Compression (adv.) | 1 % | `threshold_gain=1` in `pack_file` |
| `pfs.compressFiles` | PFS · Folder images | off | `main_folder.py --no-exfat` (on) vs MkPFS `--no-compress` (off, legacy) |
| `pfs.fileMinCompressMiB` | PFS · Folder images | 5.0 | `main_folder.py --min-compress-size` |
| `pfs.fileBlockGain` | PFS · Folder images (adv.) | 5 % | `threshold_gain=5` in `pack_folder` |
| `pfs.fileMinGain` | PFS · Folder images (adv.) | 0 % | `min_file_gain` |
| `pfs.skipExecutables` | PFS · Folder images (adv.) | on | `skip_executable_compression` |
| `pfs.blockSize` | PFS · Image format (adv.) | 65536 | `block_size` |
| `pfs.caseInsensitive` | PFS · Image format (adv.) | on | `case_insensitive` |
| `pfs.imageVersion` | PFS · Image format (adv.) | PS5 | `pfs_version`; MkPFS `--version PS5` |
| `pfs.excludes` | PFS · Source folder | `*.exfat *.ffpkg *.ffpfs *.ffpfsc` | none in Lazy; the legacy PFS tab refused dumps with stray images, the exFAT build excluded them |
| `pfs.keepEmptyFolders` | PFS · Source folder | on | legacy PFS tab `_seed_empty_dir_markers` (`.pfskeep`) |
| `pfs.amprIndex` | PFS · Source folder | add when missing | Lazy `ensure_ampr_index`; "refresh" = legacy Build page `_maybe_build_ampr_index` |
| `pfs.deleteSourceAfterConvert` | PFS · After converting | off | legacy `pfs_delete_source_after_convert` |
| `pfs.verify` | Verification · PFS images | full | MkPFS `--verify`; Lazy `main_verify.py` |

Legacy keys imported (`addLegacyKey`): `pfs_compression_level` (clamped to
1–9; ISA-L levels 0–3 become zlib levels), `pfs_lazy_cpu_count`,
`pfs_delete_source_after_convert`. Not mapped, with reason:
`pfs_lazy_zlib_backend` (ISA-L is not ported, see below), `pfs_backend` and
`experimental_mkpfs_folder` (one native engine replaces Lazy and stock MkPFS),
`pfs_intermediate_via` / `pipeline_to_pfs` (Build-page pipeline choices, owned
by the app), `temp_dir` (general `storage.tempFolder`; the engine needs no
temporary files).

Also read: `performance.workerThreads` (through `general::workerThreads`) and
`storage.freeSpaceMarginMiB` (free-space reserve, as ARCHITECTURE.md requires).
`pfs.verify` overlaps the general module's `verification.verifyAfterBuild` /
`verification.mode`; the PFS jobs read only `pfs.verify` (open question for the
app: one switch for every engine, or per-engine ones).

The builder never writes into the source folder: `ampr_emu.index` and
`.pfskeep` markers exist only in the image (Lazy and the legacy app wrote them
into the dump).

## Jobs

| Kind | Parameters | Phases |
|---|---|---|
| `image.pfs.build` | `source` (folder or file), `output`, `format` (`ffpfsc` \| `ffpfs`; default from the extension, `.ffpfs` → folder image), `replaceExisting` (false), `timestamp` (seconds; default now), `title`, `findGameRoot` (true) | Scanning → Compressing and writing → Verifying |
| `image.pfs.verify` | `image`, `source` (folder or file, optional), `depth` (`full` \| `structure`; default full with a source, structure without), `findGameRoot` (true) | Verifying |
| `image.pfs.extract` | `image`, `output` (folder), `deep` (true: unpack the nested `pfs_image.dat`, MkPFS `unpack --deep`), `overwrite` (false) | Extracting |

`findGameRoot`: a folder without `eboot.bin` (or `sce_sys/param.json`) at its
top is searched for the first sub-folder holding `eboot.bin`, as Lazy's
`pack_folder` (`find_game_root`) does; the detected root is logged and stored
in the outcome details (`gameRoot`).

Build: cheap `validate()` (source exists, output not inside the source folder,
no existing output unless `replaceExisting`, ASCII file name); scan; free-space
check for the worst case (nothing compresses) plus the reserve; write
`<output>.part`; verify the `.part`; rename. Engine errors and cancellation
remove the `.part`. A failed verification returns `Verification::Failed` (the
queue fails the job, audit M1/C1) and removes the image; the source is never
deleted then. `pfs.deleteSourceAfterConvert` deletes an image source only after
**full** verification passed; folders are never deleted. Outcome details carry
the statistics (files, compressed files, source/stored/image bytes, MB/s,
threads, timestamp).

### Verification

Strict: every packed file must be in the image with the same bytes, nothing
else may be there (files or folders), and sizes must match. It compares with
the **same source plan the build used** — exclusions, generated
`ampr_emu.index` and `.pfskeep` — so files the build left out never cause a
failure (audit H2: the legacy native `mkpfs verify --source-dir` ignored the
build's exclusions). A standalone verify job rebuilds that plan from the
settings and the image's own build time (the generated files carry it). The
only tolerated difference is the format's: a compressed file whose logical
size is the source size rounded up to the block with zero bytes (counted as
"zero-padded"). Containers built from a folder are verified through the
nested image, without extracting it.

* **full** — decodes every file and compares it byte for byte with the source.
* **structure** — validates the whole layout (header, inode ranges and
  overlaps, super root, FPT and collision resolver, tree, orphans) of the
  image and of a nested image, decompresses every block (zlib's Adler-32
  catches damaged compressed blocks), and compares names and sizes with the
  plan. Raw blocks have no checksum in PFS, so a changed byte in an
  uncompressed block is only found by full verification.

Structural damage and blocks that do not decompress are `ErrorCode::CorruptData`;
content differences are listed (first 50) as "Missing from the image",
"Not in the source", "Folder not in the source", "Different size",
"Different content (first difference at byte N)".

### Untrusted images

An image is read as untrusted input. Beyond Lazy's checks the reader treats as
damage: an entry name that is empty, "." or "..", or holds `/`, `\` or NUL;
an inode whose size does not fit its blocks (data but no blocks included); and
metadata (inode table, super root, directories, FPT, collision resolver) or a
PFSC header, block offset table or stored blocks lying past the bytes the file
really holds. Only file contents in the missing tail of a short (RAM-mode)
image read as zeros, so a crafted header cannot make the reader allocate or
zero-fill a huge table. Extraction builds every output path from names that
pass `isSafeEntryName()` and checks it stays inside the output folder
(`core/PathSafety.h`); names the drive cannot store (on Windows: `<>:"|?*`,
control characters, trailing dots or spaces, device names such as `CON` or
`LPT1`; on any system: names made only of dots and spaces) are changed, names
that then collide get " (2)", and the job lists every change as a warning
("Renamed for this drive: …").

## Compatibility

### Golden tests

`native/tests/data/pfs/golden_cases.json` describes synthetic trees (a
SplitMix64 generator implemented identically in `make_golden.py` and
`PfsTestUtil.h`) and 20 cases. `make_golden.py` builds each with the vendored
Lazy_MkPFS exactly as its CLIs / the legacy app do (`main_folder.py --no-exfat
--no-ram`, `main_file.py`, `build_pfs(compress=False)` for the MkPFS
`--no-compress` image, and the two-step container) with `time.time` pinned,
and stores SHA-256 + size in `golden.json`. `tst_pfs_golden` regenerates the
trees and requires identical bytes:

| Case group | What it covers |
|---|---|
| `folder_raw_*` (8) | 64/32/4 KiB blocks, multi-block directories (180 long names in one folder), case-sensitive mode, an FPT collision (`A_` / `B@`) with the collision resolver, AMPR index, empty folders dropped (Lazy) and kept via markers (legacy PFS tab) |
| `folder_compressed_*` (4) | per-file compression at levels 1/6/9, thresholds, executables kept raw, files that do not shrink (raw fallback), empty files |
| `file_container_*` (6) | mixed / text / incompressible images, levels 1/6/9, below and above the 1 MiB minimum, the raw fallback with Lazy's leftover bytes |
| `folder_container_*` (2) | the nested `pfs_image.dat` method, with and without AMPR |

Result: **all 20 byte-identical** (Linux, zlib 1.3, Python 3.13.18). The
benchmark additionally checks identity on 24–320 MiB inputs.

To regenerate: `python3 -I native/tests/data/pfs/make_golden.py <scratch> --write`
(needs only the `cryptography` package that Lazy imports).

### zlib

Identity was verified with madler zlib 1.3 on both sides (Python's `zlib`
module and the native build; on Windows the native build uses a system/vcpkg
zlib or zlib 1.3.1 from FetchContent, and CPython 3.11 — the legacy release
build — bundles madler zlib). madler zlib's deflate has produced the same
output at default parameters for many releases, but nothing guarantees it: if
a zlib update ever changes it, `tst_pfs_golden` fails and the references must
be regenerated with a Python that links the same zlib. Lazy's optional
**zlib-ng** and **ISA-L** backends (and stock MkPFS, which always uses zlib-ng)
produce different, equally valid deflate streams. They are not ported: images
built with `--zlib-ng` / `--zlib-isa` (legacy "zlib-isa — experimental") are
not reproduced bit for bit, but they read and verify normally.

### Stock MkPFS 0.0.9 (the legacy fallback backend)

Checked against `mkpfs==0.0.9` from PyPI in a scratch virtual environment (not
part of the tests):

* `pack folder --raw --no-compress` produces **the same bytes** as Lazy's
  `build_pfs(compress=False)` and the native folder image for every 64 KiB and
  32 KiB golden case (collisions, AMPR, empty folders included). With 4 KiB
  blocks one header byte differs: stock lists only the first inode-table block
  in the header's inode-table record (offset 0x100 when the table spans two
  blocks); Lazy and the port list every block.
* `pack file` output differs (zlib-ng deflate streams; stock also compresses
  files below 1 MiB), except where nothing compresses.
* **Legacy app note:** since 0.0.9, `pack folder` without `--raw` wraps the
  folder in an exFAT image and compresses it, even with `--no-compress`. The
  legacy PFS tab (`ui/tab_pfs.py`, step 1) passes `--no-compress` without
  `--raw`, so with the bundled 0.0.9 its ".ffpfs" output and its nested
  `pfs_image.dat` are compressed PFS containers holding `<folder>.exfat`
  (verified: one compressed file whose first sector carries the "EXFAT"
  signature), not the uncompressed folder PFS its comments describe; the
  .ffpfsc is then compressed twice. The native engine builds what the
  comments describe (MkPFS `--raw --no-compress`).

### Deliberate differences from Lazy_MkPFS

1. **Image length.** Lazy's default RAM mode (image ≤ 80 % of free RAM) does not
   pad the last block: the file ends at the last byte written. Measured: the
   RAM-mode image is exactly the disk-mode image minus its all-zero tail. The
   native builder always writes `ndblock × bs` bytes (Lazy's disk mode,
   `--no-ram`), which the golden tests use. The reader accepts short images
   (missing tail reads as zeros, with a warning).
2. **Per-file compression with one worker.** Lazy's `build_pfs` crashes
   (`UnboundLocalError: displayed_progress_units`) when it compresses with one
   worker or when exactly one file is eligible (reproduced with the vendored
   code). The native engine produces the multi-worker result for any thread
   count (`threadDeterminism`).
3. **Lazy's own verifier** (`inspect_pfs_image(source=…)`) reports "content
   mismatch" for every compressed file of its own per-file-compressed images
   (zero padding, reproduced). Native verification accepts exactly that
   padding and nothing else.
4. **The source folder is never modified** (Lazy and the legacy app wrote
   `ampr_emu.index` and `.pfskeep` into the dump); the image is identical to
   what they produced from the modified dump.
5. **Signed and encrypted images** are rejected with `Unsupported`. Lazy's
   signed build path fails (`TypeError` in `read_image_bytes`), no app path
   uses either, and nothing available here produces such an image to test
   against.
6. **Errors instead of silent skips**: an unreadable folder fails the scan
   (Lazy skipped it); a source file whose size changes during the build fails
   the job (Lazy silently copied the old length).
7. **Order ties.** Two names equal ignoring case (possible only on
   case-sensitive file systems) are ordered by their bytes; Lazy keeps the
   file system's listing order. Such dumps collide in a case-insensitive
   image anyway.
8. **Symlinks** to files and folders are followed (as `os.scandir().is_file()` /
   `is_dir()` do), but a folder reached twice (link loop) is packed once.
9. **Exclusions and markers** (`pfs.excludes`, `pfs.keepEmptyFolders`) have no
   Lazy equivalent; with the default excludes a dump without stray images is
   packed exactly like Lazy would.

## Performance

Pipeline: the job thread reads the source in 1 MiB chunks (16 PFSC blocks) in
image order and submits them to a bounded pool of `pfs.threads` deflate
workers (each keeps one `z_stream` and uses `deflateReset`); it consumes
results strictly in submission order and writes them through an 8 MiB write
buffer. At most `3 × threads` chunks are in flight (≈ 6 MiB per thread). The
nested image of a folder container is produced on the fly
(`FolderImageSource`), so a folder → .ffpfsc build reads the dump once and
writes only the compressed image — Lazy/MkPFS write and re-read a full-size
`pfs_image.dat`. Per-file compression writes each file's PFSC stream straight
into its final place (no spool files); a file that does not shrink is
rewritten raw, the only extra I/O.

Benchmark (`tst_pfs_benchmark`, Release build, synthetic game-like data: 64 KiB
blocks alternately text and random, level 9 unless noted; MB/s = source MiB
per second; best of 3 alternating runs; outputs byte-identical to Lazy in
every run). Measured on this development VM (4 vCPUs shared with other
builds; four independent single-threaded zlib processes reach only ≈ 1.6× the
throughput of one, so absolute numbers and scaling are pessimistic):

| Input | Level | Native, 1 thread | Native, 4 threads | Lazy_MkPFS `--cpu-count 4` |
|---|---|---|---|---|
| image → .ffpfsc, 320 MiB | 9 | 6.2 MB/s | **18.4 MB/s** | 12.8 MB/s |
| folder → .ffpfsc, 320 MiB (8 files) | 9 | 6.2 MB/s | **11.6 MB/s** | 8.4 MB/s |
| image → .ffpfsc, 320 MiB | 6 | 30.2 MB/s | 41.6 MB/s | **48.0 MB/s** |
| folder → .ffpfsc, 320 MiB (8 files) | 6 | 30.9 MB/s | **51.5 MB/s** | 42.8 MB/s |
| image → .ffpfsc, 24 MiB (ctest default, Debug build, 1 run) | 9 | 5.9 MB/s | **10.9 MB/s** | 5.7 MB/s (one core) |
| folder → .ffpfsc, 24 MiB (ctest default, Debug build, 1 run) | 9 | 5.8 MB/s | **8.3 MB/s** | 5.6 MB/s (one core) |

Single-threaded, native and Lazy run at the same speed (both are zlib-bound:
6.0 vs 6.0 MB/s at level 9 on 64 MiB). With four threads the two are within
the noise of this machine for large single images (the same native case
measured 15.9–18.4 MB/s across runs); the native engine is clearly faster
when Lazy runs single-threaded (one image below 256 MiB) and for folder builds,
where Lazy also writes and re-reads the full-size `pfs_image.dat` (here
320 MiB; for a real game tens of GB of extra disk traffic that the
measurement on a RAM-cached scratch disk understates). Reproduce with
`QTEST_FUNCTION_TIMEOUT=3600000 FW_PFS_BENCH_MIB=320 FW_PFS_BENCH_LEVEL=9
FW_PFS_BENCH_REPEAT=3 tst_pfs_benchmark` on a Release build.

Notes: Lazy compresses a single file on one core below 256 MiB
(`PFSC_SINGLE_FILE_PARALLEL_MIN_SIZE`); the native engine always uses every
thread. In every run the job thread spent 75–98 % of its time waiting for the
compressor pool and under 0.1 s reading (the rest writing), i.e. the build is
CPU-bound in zlib. On this data level 6 is ≈ 5× faster than level 9 single-
threaded (30 vs 6 MB/s); level 9 is the legacy default.

## Tests

| Test | Covers |
|---|---|
| `tst_pfs_golden` | 20 byte-identical golden cases against Lazy_MkPFS |
| `tst_pfs_roundtrip` | build → read → compare for all four modes; verify finds changed / missing / extra / resized files; exclusions do not fail verification (H2); damaged compressed block, inode, dirent, PFSC and PFS headers, truncation → `CorruptData`; short (RAM-mode) images; extract (plain, deep, nested file, overwrite); cancelled extract leaves nothing; 1/4/7 threads → identical bytes; ASCII rule, empty source, AMPR modes |
| `tst_pfs_jobs` | settings declared and read, legacy import, the three jobs, queue integration, validation, cancellation while writing and while verifying leaves no output or `.part`, failed verification removes the image and keeps the source (C1), delete-source rules, standalone verify with the build's exclusions (H2), game-root detection in a wrapper folder, extract job |
| `tst_pfs_hostile` | crafted images: separator / NUL / dot entry names → `CorruptData` and nothing extracted; names a drive cannot store renamed on extraction and reported, nothing written outside the folder; size without blocks and size beyond the blocks → `CorruptData` (verification and extraction fail); a PFSC header declaring a 16 GiB block table in a small file is refused without allocating it; the path-safety helpers |
| `tst_pfs_benchmark` | MB/s native (1 and N threads) vs Lazy_MkPFS on the same input; outputs identical |

`ctest --test-dir native/build-pfs -R pfs --output-on-failure`. The benchmark
honours `FW_PFS_BENCH_MIB`, `FW_PFS_BENCH_LEVEL`, `FW_PFS_BENCH_REPEAT` and
skips the Python half when `python3` with `cryptography` is missing.
