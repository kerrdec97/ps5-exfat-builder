# FFPKG (UFS2) engine — `fw_engine_ufs2`

Native replacement for the UFS2Tool calls the legacy app made for `.ffpkg`
images. Nothing here needs .NET. Sources are in `src/engines/ufs2/`, and tests
are in `tests/engines/ufs2/`.

| File | What it does |
|---|---|
| `Ufs2Format.*` | On-disk constants, newfs parameters and validation, cylinder-group geometry, the .NET `System.Random` port, inode (de)serialisation, directory packing |
| `Ufs2Tree.*` | The tree that goes into an image: scanned from a folder in listing order, or loaded from an image; content streams |
| `Ufs2Writer.*` | Layout planner and streaming writer (byte-identical to UFS2Tool `newfs -D`) |
| `Ufs2Reader.*` | Read-only access: superblock search and validation, inodes, block maps, directories, path lookup, file streams |
| `Ufs2Check.*` | fsck_ffs-style structure check (passes 1–5, read-only) |
| `Ufs2Verify.*` | Comparison with a source tree, read-back of every file, extraction and its verification |
| `Ufs2Edit.*` | Add / replace / delete / rename / mkdir applied to an image's tree |
| `Ufs2Jobs.*`, `Module.*` | Queue jobs and the `ufs2.*` settings |

The layout code is a port of **UFS2Tool 4.0.0** by SvenGDK (the build the
legacy app embeds: `UFS2Tool.dll` sha256 `08f2f2a1…8e3a`, commit `c123639`),
Copyright (c) 2026, SvenGDK, BSD-2-Clause (`LICENSES/UFS2Tool-BSD-2-Clause.txt`).
The source files say which UFS2Tool functions they follow. Field offsets
follow FreeBSD `sys/ufs/ffs/fs.h` and `sys/ufs/ufs/dinode.h` / `dir.h`.

## What the legacy app ran

```
UFS2Tool newfs -O 2 -b <adv_ffpkg_block|65536> -f <adv_ffpkg_frag|65536>
               -m <adv_ffpkg_minfree|0> -S 512 -i <adv_ffpkg_inode|262144>
               -D <game folder> <name>.ffpkg.part
```

`exfat_builder.py` `_ffpkg_process_next`, and `ui/tab_ffpkg_edit.py` for the
rebuild. A fragment larger than the block was reduced to the block size.
The sector size was locked at 512 ("4096 produces broken images on
Windows"). The editor used UFS2Tool's `ls`, `stat`, `add`, `replace`,
`delete`, `rename` and `extract` verbs, and emulated mkdir with a `.keep`
file.

## Format as written (UFS2Tool `newfs -D`)

**Size** (`CalculateDirectorySizes(dir, bsize)` + `CalculateImageSize`): every
file is rounded up to whole blocks, plus its indirect blocks. Every
directory, the root included, counts `max(1, ⌈(2 + entries)·24 / bsize⌉)`
blocks. The image is `(long)(sum · 1.10) + 10 MiB`, at least 16 blocks and
fragment aligned. If the cylinder groups then hold fewer inodes than
`entries + 3`, the size grows to `⌈needed/ipg⌉ · fpg · fsize`. Block size and
`-m` do not otherwise change the size.

**Geometry** (`ComputeInodesPerGroup`, `WriteFilesystem`):
- `ipg = roundup((size / bpi) / ⌈frags / (2048·frag)⌉, inopb)`, at least one
  block of inodes. Then `fpg = ipg · frag`, which is UFS2Tool's own rule and
  not FreeBSD's, and `ncg = ⌈frags / fpg⌉`.
- `sblkno = roundup(⌈(65536 + 8192) / fsize⌉, frag)`, `cblkno = sblkno + roundup(⌈8192/fsize⌉, frag)`,
  `iblkno = cblkno + frag`, `dblkno = iblkno + ⌈ipg/inopb⌉·frag`.
- `maxcontig = max(1, 65536/bsize)`, `contigsumsize = min(maxcontig, 16)`,
  `cssize = roundup(ncg·16, fsize)` at `csaddr = dblkno` (in CG 0),
  `cgsize = roundup(CGSIZE, fsize)`, `fs_dsize = size − sblkno − ncg·(dblkno − sblkno) − ⌈cssize/fsize⌉`,
  `metaspace = blknum(fpg · minfree / 200)`.
- 64 KiB / 64 KiB example: an 11,010,048-byte image has 1 group, `fpg = ipg = 256`, and
  `sblkno 2, cblkno 3, iblkno 4, dblkno 5`.

**Superblock** at 65536, with `fs_sblockloc = 65536`, `fs_clean = 1`,
`fs_old_flags = FS_FLAGS_UPDATED`, `fs_fsmnt = "/"`, no volume name, no flags
(no soft updates), `optim = time`, `avgfilesize 16384`, `avgfpdir 64`,
`maxbpg = ipg`, `maxsymlinklen 120`, `old_inodefmt 2`,
`old_postblformat −1`, `old_nrpos 1` and `old_cpg 1`. Copies go at
`cgbase + sblkno` in **CG 1 and later only** (FreeBSD also writes one in CG
0), when they fit in the group. The `struct fsrecovery` sits in the last 20
bytes of the sector before 65536. `fs_id[0]` is the format time and
`fs_id[1]` is `(time >> 16) ^ pid`. The native writer uses a random number
for that, as FreeBSD newfs does.

**Allocation** (`PopulateFromDirectory`, `ProcessDirectoryContents`, `AllocateDataBlock`):
- Strictly sequential, whole blocks, from `dblkno` in each group. CG 0
  first reserves the summary area (rounded up to a block) and the root's
  first block. A group that cannot fit the next block spills to the next
  group.
- Children are taken **in the order the directory listing returns them**.
  A sub-directory gets its inode first, then its contents, then its own
  directory blocks (plus one single-indirect block when it has more than 12).
- A file gets 12 direct blocks. After that, each indirect block is
  allocated just before the data blocks it points to (single, then double,
  then triple). Short files (≤ 12 blocks) give back the unused fragments of
  their last block.
- Inodes are numbered 3, 4, … in the same order.

**Inodes**: mode `0555` for files and directories, uid/gid 0,
`di_blksize = bsize`, and all four times set to the time the files were
added. `di_gen = 1`, and `di_dirdepth` is set for directories. `di_blocks`
counts the tail at fragment precision for files with at most 12 blocks;
larger files count every block in full. In each group, newfs gives the
first `2·INOPB` inodes a random generation number from .NET
`new Random(cg + 1)` (CG 0 skips the root). `DotNetRandom` reproduces it;
the test checks it against values printed by .NET 8.

**Directories** (`WriteDirBlocks`): ".", ".." and then the entries, in
512-byte `DIRBLKSIZ` chunks. The last entry of a chunk is stretched to the
chunk end, and unused chunks hold `d_ino = 0` entries of 512 bytes.
`d_type` is set and the size is whole blocks.

**Cylinder groups** (`SerializeCylinderGroupHeader` + `PatchCgAndSuperblock`):
- The final bitmaps: inodes `0..n` used, data fragments above the group's
  high-water mark free, and fragments `[0, sblkno)` free in groups > 0.
- `cs`, `frsum` and the cluster map and summary are recomputed from those
  bitmaps. `cg_initediblk` is the number of used inodes rounded up to
  `INOPB`. `cg_old_time` and `cg_time` are the format time.

## Where the native writer deliberately differs

Each of these applies only where UFS2Tool fails or produces something
broken. Everywhere else the output is byte-identical.

1. **Names are UTF-8.** UFS2Tool encodes names with `Encoding.ASCII`, so
   `café.txt` becomes `caf?.txt` and `日本.bin` becomes `??.bin`. This was
   verified with the shipped DLL. Names longer than 255 bytes are refused;
   UFS2Tool would wrap `d_namlen`.
2. **A last cylinder group shorter than its own metadata (`< dblkno`
   fragments) is dropped**, as FreeBSD `mkfs.c` does. UFS2Tool keeps it and
   writes its header and inode table past `fs_size`, with no superblock
   copy, and fsck_ffs then refuses the image ("VALUES IN SUPER BLOCK
   DISAGREE WITH THOSE IN FIRST ALTERNATE"). This happens for roughly 1 % of
   sizes; one example is a single 142,868,480-byte file. A group of exactly
   `dblkno` fragments is still byte-identical (tested).
3. **The auto size grows instead of failing.** When UFS2Tool's rule leaves
   too few blocks or inodes, UFS2Tool stops with "Image is too small". The
   writer grows the size by an eighth until the tree fits, and the job warns.
4. The double-indirect level is capped at `nindir` children. UFS2Tool would
   overflow above 4 TiB at 64 KiB blocks.
5. A source file that changes size while the image is built fails the job.
   UFS2Tool would write whatever it read.

## Reader

- Searches 65536, 8192 and 262144, the FreeBSD order without the floppy
  location. The superblock must record its own location. Some `makefs`
  builds put UFS2 at 8192.
- Validates block and fragment sizes, shifts, group layout, `fs_size`
  against `ncg·fpg`, the summary area, size limits, and that the file holds
  `fs_size·fsize` bytes. A shorter file is reported as an incomplete image.
  Corruption is `ErrorCode::CorruptData` with a user message.
- Handles general UFS2: fragments, holes, all three indirect levels,
  short and long symbolic links (listing only), and directory entries
  checked like `ufs_dirbadentry()`. UFS1 is reported as unsupported.

## Structure check (`checkImage`)

This is a read-only fsck_ffs pass 1–5. It checks:
- every CG header (magic, index, size, map offsets)
- each group's free-block and free-fragment counts and its `frsum`
  against its own bitmap
- the cluster map
- the summary area against the groups, and the superblock totals
- the superblock copies against the primary
- "." and ".." in every directory and reachability from the root
- that every block pointer is in range, not shared, and not marked free
- `di_blocks`, link counts and `d_type`
- that no used fragment is unreferenced and no inode is marked used without
  being referenced
- folders per group

It accepts the NetBSD `makefs` image in the tests and UFS2Tool's own images.

## Jobs

| Kind | Parameters | Phases | Notes |
|---|---|---|---|
| `image.ffpkg.build` | `source`, `output`, `replaceExisting` (false) | Scanning → Writing image → Verifying | Writes `<output>.part`. The image gets its final name only after verification passed (audit H6). An existing image is set aside and restored if replacing fails (M2). Refused when the output is inside the source. |
| `image.ffpkg.verify` | `image`, `source` (optional) | Checking structure → Comparing | With `source`: compares at the configured depth. Without it, in full mode: reads every file back. A damaged image is `Verification::Failed`, not a job error. |
| `image.ffpkg.extract` | `image`, `output` (new or empty folder), `path` ("/") | Reading image → Extracting → Verifying | Extracts into `<output>.part`, marked so a leftover of ours can be replaced, and renames it on success. On Windows, forbidden characters and device names are changed and collisions get " (2)". Each change is reported. File times come from the inodes. |
| `image.ffpkg.edit` | `image`, `operations` | Reading image → Writing image → Verifying → Replacing original | Rebuilds through `<image>.part` and always compares the result with the edited tree, at least by name and size. The original is replaced only after that passes. Images containing links or special files are refused, because a rebuild would drop them. |

Operations (`Ufs2Edit.h`): `{"op":"add","path":"/a/b","source":"C:/x"}`
creates missing parent folders and adds a file or a folder recursively.
`replace` changes the content of a file, or for a folder replaces every
file that exists in both, as UFS2Tool did. `delete` and
`rename` (`newName`) work as named. `mkdir` makes a real directory, so no
`.keep` file is needed. The legacy editor changed the image in place with
UFS2Tool; here an edit can never leave a half-edited image.

Every job writes outputs through `.part` names and checks free space against
`storage.freeSpaceMarginMiB`. It honours cancellation at least every buffer
(4 MiB) and every 64 regions, and reports byte progress.

## Settings

| Key | Type / default | Legacy key | Consumer |
|---|---|---|---|
| `ufs2.blockSize` | enum 32768 / **65536** | `adv_ffpkg_block` | `formatParams()` → writer (`-b`) |
| `ufs2.fragmentSize` | enum 32768 / **65536** | `adv_ffpkg_frag` | writer (`-f`); reduced to the block size if larger |
| `ufs2.minFreePercent` | int 0–99, **0** | `adv_ffpkg_minfree` | writer (`-m`: `fs_minfree`, `fs_metaspace`) |
| `ufs2.bytesPerInode` | enum 131072 / **262144** / 524288 | `adv_ffpkg_inode` | writer (`-i`) |
| `ufs2.deleteSourceAfterBuild` | bool, **false** | `adv_delete_source_ffpkg` | build job: deletes only after a *full* verification passed and when `eboot.bin` is present (the legacy rule) |

All of these are `perJob`. The legacy UI also offered a 128 KiB block size,
but UFS2Tool 4.0.0 rejects it ("Block size must be between 4096 and
65536"), so it is neither offered nor imported. The sector size stays fixed
at 512 and has no setting.

The jobs also read `verification.verifyAfterBuild`, `verification.mode`
(full = every byte, structure = names, types and sizes),
`storage.freeSpaceMarginMiB` and `performance.ioBufferMiB` (buffer size).
The module has no dependency probe and no external-UFS2Tool fallback,
because nothing needs one.

## Validation done (Linux, GCC 13, Qt 6.4, Debug and Release)

- **Byte-identical to UFS2Tool 4.0.0.** This was run against the shipped
  `UFS2Tool.dll` under .NET 8.0.33, with only the build time and `fs_id`
  taken from UFS2Tool's image (`tst_ufs2_compat`, needs `FW_UFS2TOOL_DLL`).
  Cases covered:
  - a game-like tree with the legacy parameters
  - 32 KiB blocks
  - 64K/32K fragments with `-m 8 -i 131072` (freed tails, `frsum`)
  - `-i 524288`
  - a 1,700-entry directory with an indirect block
  - a 130 MiB file using double indirection across 20 groups
  - a last group of exactly `dblkno` fragments
  - a 301 MiB / 3,008-file tree
- **Stored references.** `tests/data/ufs2/*.ffpkg.zz` were made by that
  UFS2Tool from a deterministic tree, for the legacy parameters and for
  64K/32K/m8/i128K. `tst_ufs2_reference` rebuilds them byte for byte on any
  platform without .NET, taking the listing order from the stored image.
- **FreeBSD fsck_ffs 8.2** (Ubuntu `ufsutils`, `FW_FSCK_UFS`) passes native
  images with all five phases and no complaints, including multi-group,
  fragmented and Unicode images. `dumpfs.ufs` reads them. UFS2Tool's
  `fsck_ufs`, `ls` and `extract` accept them, and its extract gives back
  the source tree.
- **NetBSD makefs** (independent writer, 32K/4K, superblock at 8192): the
  reader reads every file back and the structure check passes.
- Round trips are covered by `tst_ufs2_roundtrip`:
  - 4 parameter sets: nested folders, Unicode and 4-byte UTF-8 names,
    255-byte names in ASCII and CJK, empty files and folders, 2,400 small
    files, multi-MiB files
  - comparison detects changed (same size, one byte), missing, extra and
    file/folder swaps
  - corruption detection: bad magic, bad block size, a truncated image,
    random data, a CG header, a bitmap bit, a directory record, an
    out-of-range pointer and a link count
  - cancellation, and a source shrinking during the build
  - extraction with verification, and edits
- Jobs are covered by `tst_ufs2_jobs`:
  - settings reach the superblock
  - verification gates the final name (the source is tampered with
    between writing and verifying)
  - a cancelled build leaves nothing and keeps the old image
  - existing outputs are replaced only when asked
  - the free-space reserve is enforced
  - the danger-zone delete happens only after full verification
  - verify, extract and edit jobs, including failed and cancelled edits
    leaving the original byte-identical
  - legacy settings import
  - an end-to-end `JobQueue` run

**Throughput** (this VM, shared with other builds, warm cache, Release,
301 MiB / 3,008 files, each run the best of two with `sync` in between, six
runs): native 133–1,304 ms (231–2,260 MiB/s), and UFS2Tool 480–686 ms
(439–627 MiB/s) for the same tree. In four of the six runs native was
faster. The timings are dominated by write-back noise on this machine. The Debug test build logs about 120–300 MiB/s
writing and about 650–2,000 MiB/s for the full comparison
(`tst_ufs2_roundtrip benchmark`).

## Running the optional tool checks

```
FW_UFS2TOOL_DLL=/path/UFS2Tool.dll   # the legacy app's embedded DLL (+ dotnet on PATH, or FW_DOTNET)
FW_FSCK_UFS=/path/fsck.ufs           # ufsutils, or fsck.ufs/fsck_ffs on PATH
FW_DUMPFS_UFS=/path/dumpfs.ufs
makefs on PATH (FW_MAKEFS)           # NetBSD makefs, Debian/Ubuntu package "makefs"
ctest --test-dir native/build-ufs2 -R ufs2 --output-on-failure
```

Each check is skipped (`QSKIP`) when its tool is missing. The DLL can be
extracted from `exfat_builder.py` (`_UFS2TOOL_DLL_B64`). It needs a
`UFS2Tool.runtimeconfig.json` (see `extract_ufs2tool()`) and `DokanNet.dll`
beside it.

## Not covered

- **Not loaded on a PS5.** Compatibility rests on the byte-identity with
  UFS2Tool (what users have been loading) and on FreeBSD's fsck.
- **Not compiled with MSVC here.** The code avoids GCC-only features and
  uses Qt for file access (`fw::toLongPath` on opens), but no MSVC build ran
  in this environment.
- **Listing order on Windows** is assumed to be the same for `QDirIterator`
  and .NET (both read the NTFS index order). That was not observed on
  Windows. The order affects only byte-identity, not validity.
- Symbolic links and special files are not written. The scanner follows
  links and refuses special files. Edits refuse images that contain them.
- The legacy "mount as drive" (Dokan) feature has no equivalent; extraction
  reads the image directly.
- Images are written in full apart from large gaps (> 4 MiB), which are
  skipped and left sparse where the file system allows.
