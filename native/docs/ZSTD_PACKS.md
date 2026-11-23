# zstd packs, packed games and images inside .ffpfsc

What the app does with ampr-emu-ng's zstd packs and ps5-dump-forge's formats,
and which reported problems it handles on its side. Sources: ampr-emu-ng 0.3.3
(docs/zstd-format.md, 38 issues and PRs) and ps5-dump-forge 0.5.4 (code,
docs, 36 issues and PRs), November 2026.

## Where it lives

| Piece | Code |
|---|---|
| Format, packer, reader | `src/engines/ampr/ZstdPack.*` |
| Rules (what stays loose) | `src/engines/ampr/PackRules.*` |
| Packing / unpacking a game, the staged output folder | `src/engines/ampr/GamePack.*` |
| Where stored names go on this computer (containment) | `src/engines/ampr/HostPaths.*` |
| Jobs `ampr.pack` (Build page › Packed folder, Library › Pack with zstd…; an archived dump is packed straight from the archive, see below) and `ampr.unpack` | `src/engines/ampr/Module.cpp` |
| Packed game as plain files, folder reader | `src/engines/ampr/GameFiles.*` |
| LZ4 (AMPRPAK4) reader, read only | `src/engines/ampr/Lz4Pack.*` |
| Bundled runtime (pinned SHA-256) | `src/engines/ampr/Runtime.*`, `cmake/AmprRuntime.cmake` |
| Jobs `image.build.zstd`, `game.extract`, FPKG staging | `src/engines/integration/GameSources.*` |
| Images inside an .ffpfsc | `src/engines/integration/ImageFiles.cpp`, `src/core/RandomSource.*` |

## The format (as written)

`ampr_zstd.index` (AMPRZPK1) + `ampr_zstd-NNN.pak` (AMPRZDT1) at the game
root. 256 KiB chunks, one zstd frame each (content size and checksum on,
window 256 KiB), stored raw when zstd does not make the chunk smaller. Volumes
of at most 4 GiB − 64 KiB. Records sorted by FNV-1a 64 of the path, then the
path bytes. Volume headers are written last, the index after them. The output
does not depend on the thread count.

## Rules

* Always loose: empty files; `eboot.bin` anywhere and every `.elf .self .prx
  .sprx`; every `.ini` (any folder, any case), so `dlc_emu.ini` stays editable
  and on disk at the game root, where Kraken Mount and dlc_emu look for it, as
  ps5-dump-forge 0.5.5 does; the root `sce_sys/`, `sce_module/` and `fakelib*`
  folders; the AMPR files.
* Strategy *everything* (default): every other file is packed (ampr-emu-ng
  0.3.0+ serves plain reads and directory listings of packed files; mmap of a
  packed file is not served, which no measured game does). *Compatible*: also root files,
  containers (.pak, .utoc), configs, media and video folders stay loose.
* `fakelib/libSceAmpr.sprx` becomes the bundled ampr-emu-ng; no other fakelib
  file is touched. The old runtime is named in the log; when it was a newer
  ampr-emu-ng than the bundled one, the job warns that it was replaced (the
  packs and the index were checked with the bundled one, so it is still the
  one written; copy the newer file back by hand if the game ran better).
  `identifyRuntime` reads both the 0.3.x string `ampr-emu-ng 0.3.3` and the
  0.4.x/0.5.x notice `ampr-emu-ng: v0.5.0 (zstd) running`.
* `ampr_emu.index` is written afresh from the game's files as the game sees
  them (loose, packed, the new runtime), on-disk case kept; the old journal
  (`ampr_commands.bin`) and logs are left out. A file whose path the runtime
  cannot list (`amprRuntimeAccepts`, see `engines/PFS.md`) is left out of it
  with a warning in the log and the outcome, when packing and when an unpack
  writes a fresh index.
* A folder that holds only packed files is not created on disk.
* The loose files of a *Packed folder* (`ampr.pack`) are copies: the folder
  is kept, so it shares no file data with the dump it was made from and
  editing one never changes the other. Only the temporary work folder of
  `image.build.zstd` hard-links them from the source (same NTFS volume;
  read-only files are copied there too).
* `ampr.pack` and `ampr.unpack` write `<output>.part` (marker file
  `<output>.part.writing`) and rename it when everything is done. The output
  may be absent or an empty folder; that is checked when the job is queued,
  when it starts and again before the rename, and only the staged folder is
  ever removed. A finished folder that cannot take its name (the chosen one
  got files meanwhile, or is open elsewhere) is kept as `<output>.new` (or, if
  even that rename fails, as `<output>.part` without its marker, so a later run
  refuses it instead of removing it) and the error says so
  (`StagedFolder::commitOrKeep`). Both renames are tried again for a moment
  while a virus scanner or the search indexer still holds a file of the
  folder (`renameWithoutCopying`). A cancel while `ampr.pack` checks the packs
  ("Checking packs") keeps the finished folder, complete with its
  `ampr_emu.index`, as with the check turned off: Skipped, "Verification was
  cancelled, so the packs are not verified." (`PackGameReport::verifyCancelled`);
  a pack that does not read back still fails the job. Closing the app at that
  point keeps nothing (`CancellationToken::keepFinishedWork`): the staged
  folder goes and the job comes back as Interrupted. `image.build.zstd` stops
  there instead (the image, its real work, is not made yet). `game.extract`
  works the same way (marker `<output>.part.extracting`); a cancel during its
  verification keeps the extracted files, reported as not verified.
* The source folder is scanned like the plain image builds scan it: symbolic
  links and junctions are followed. A link that leads back into its own
  folders, or whose target is missing, is left out and listed as a warning.
  A folder that cannot be listed fails the job.
* Names from a pack index or an image are untrusted. A path with `..`, an
  empty component, a backslash or a drive letter fails the job before
  anything is written (`HostPaths`); other names this drive cannot store are
  renamed with a warning, and two names that are one file here get ` (2)`.
  FPKG staging refuses renamed names instead (a package must hold the
  game's own names).
* An image built with zstd packs is uploaded after the build like any other
  when "Upload new images to the PS5" is on; a packed folder is not.
* Progress of `image.build.zstd`: Preparing 2 %, Packing 50 %, Checking packs
  6 %, then the image build's own stages, which share the remaining 42 % as the
  build shares its own bar. The build reports on a reporter of its own whose
  stages and steps (the FFPKG "Checking structure" first half of Verifying)
  the job enters in turn, so a stage the build enters again goes on from where
  it stood. A stage that does not run takes no share: Checking packs with the
  check turned off (its 6 % goes to Packing), and the build's Verifying with
  verification turned off (the FFPKG build keeps 5 % there for its structure
  check). `ampr.pack` alone: Preparing 2 %, Packing 75 %, Checking packs 23 %
  (Packing 98 % with the check turned off).

## Packing from an archive (GitHub #37)

`ampr.pack` and every `image.build.zstd` (compressed exFAT, the ZSTD image,
FFPKG and PFS with packs) take a `.zip`, `.7z` or `.rar` and pack the game in
**one pass through the archive** (`packGameFromArchive`, `GamePack.h`); the
game is never unpacked. The game comes from the archive's list
(`ampr::ArchiveGame`: what unpacking would make of it, links left out) and
goes through the same rules as a folder (`packGame()` and it share them):
loose and packed files, the files left out, the bundled runtime in place of
`fakelib/libSceAmpr.sprx`, a fresh `ampr_emu.index`. Then:

* **Loose files** are written from the archive into the output (the packed
  folder, or the work folder of an image build) with the times the archive
  lists; nothing is hard-linked (there is no source file to link).
* **Packed files** go to `ZstdPackStream`, which compresses each 256 KiB
  chunk exactly as `writeZstdPack()` does. A **ZIP** is read in the packs'
  record order, so the chunks go straight into the volumes: nothing is
  written but the pack. A **7z or RAR** gives its files in its own order:
  each chunk is compressed as it comes and its stored bytes go to
  `zstd-chunks.spool` in the job's temporary folder, and once the pass is
  done the volumes are written from the spool in record order (each chunk
  checked against its CRC as it is read back) and the spool is removed. The
  spool grows to the packs' stored size, at most the packed files' size (a
  chunk is stored raw when zstd does not shrink it); while the volumes are
  written the drive holds the spool and the packs side by side. The
  free-space check counts both (on one drive, their sum).
* The old runtime is looked at (to warn when it was newer than the bundled
  one) and `eboot.bin`'s first bytes (the plain ELF warning) when the pass
  reaches them; from a ZIP `eboot.bin` is read first.

Every entry's checksum is verified while it is decoded
(`archive::visitGameFiles`); an entry that is damaged, larger or smaller than
listed, missing or listed twice fails the job, and a cancel stops it; either
way nothing is left: no packed folder, no `.part`, no work folder, no spool.
The packs are then read back against their CRCs as from a folder ("Checking
packs"), and an image build goes on from the work folder exactly as from a
folder.

**What is the same as from the unpacked folder.** Byte for byte, given the
same archive id (random for every build, so two builds of one folder differ
there too): `ampr_zstd.index` and every `ampr_zstd-NNN.pak` (records, chunks,
volume layout; the chunk order inside the volumes is the record order
whatever the archive's order), the loose files, the runtime and the folders.
`ampr_emu.index` lists the same rows; the runtime's row carries the time the
build wrote the runtime, as from a folder (`tst_archive_stream_more`: ZIP
stored and deflated, solid 7z LZMA2 and stored, libarchive's RAR samples, a
7z in reverse order, `ZstdPackStream` against `writeZstdPack()` with several
volumes). So the console sees nothing that differs from a pack made from a
folder; no pack is ever laid out in archive order.

**Unpacked first:** an archive whose game is itself zstd- or LZ4-packed
(`ampr_zstd.index` or `ampr_assets.index` at its root): its packs are read
file by file, in no fixed order, to be packed again. The log says so
(`archivePackProblem`).

## Reported problems and what the app does

| Report | Handled here |
|---|---|
| Silksong crashed: index said `Il2cppUserAssemblies.prx`, file was `Il2CppUserAssemblies.prx` (ampr-emu-ng #15) | Case is kept exactly in both indexes |
| Forge replaced `libSceAmpr.sprx` and rewrote `ampr_emu.index` on every conversion; Uncharted, Tintin broke (dump-forge #21) | Only packing touches them; a plain build or extract leaves them as they are |
| Backported games ship other fakelib libraries (ampr-emu-ng #2, #4, #9, #31, #34) | Only `fakelib/libSceAmpr.sprx` is replaced |
| ShadowMount+'s emulator updates replace the runtime (dump-forge #10, #23; SMP 1.7 config v2 `[emulators] update_enabled`, on by default, and `auto_update_ampr`, which fetches drakmor's ampr_emu without a zstd reader) | Every zstd build warns about it (`packedGameMounterNote`) and names Kraken Mount, which brings its own ampr-emu-ng and mounts folders, not images |
| `dlc_emu.ini` packed, so neither Kraken Mount nor the DLC editor finds it (dump-forge 0.5.5) | Every `.ini` stays loose |
| Days Gone left 58.7 GB loose with the old default (ampr-emu-ng #3) | Pack everything by default; *compatible* is an option |
| ASTRO BOT started with the keep-loose profile but not fully packed (dump-forge #27) | The *compatible* strategy |
| A plain ELF eboot never starts (dump-forge 9ab367c, ampr-emu-ng #8) | Warned in the log as soon as it is seen, before packing, and again in the job's result |
| Old LZ4 packs must be decoded before repacking (ampr-emu-ng #21) | LZ4 and zstd packed sources are unpacked on the fly and packed again |
| Packing into an image needs space and twice the CPU (dump-forge #15) | Packs go to a work folder next to the output, loose files hard-linked; removed after the build or after a crash. An archived dump is packed in one pass with no unpacked copy (above) |
| Corrupt or truncated sources (ampr-emu-ng #7) | Every chunk decoded again while packing, every volume read back against its CRCs, the image verified against the packed folder |
| .ffpfsc holding an image (dump-forge default, Meaners' nested `pfs_image.dat`) | Opened straight through for extract, FPKG and the title |
| FPKG from images needed a full extra copy | Only the files the package keeps are staged, packs unpacked, next to the output |
| Runtime-only problems (pread threads, Wolverine snow scene, Crimson Desert, …) | Not fixable by a packer; the known titles are listed in the release notes |

## Not done

* LZ4 packs are read, never written (decided: LZ4 needs traces).
* TOML pack profiles (dump-forge #13, #15, #31) and packing straight to the
  console over FTP (dump-forge #5) are not implemented.
* Whole-image zstd streams are not written or opened (decided). Lithiumwow's
  PS5ZST `.exfat.zst` is one plain zstd stream of a whole exFAT image (level 3,
  not seekable, no licence); it needs a closed console ELF to restore it, and
  no published loader mounts it: ShadowMount+ (`.ffpkg .exfat .ffpfs .ffpfsc`
  only) and Kraken Mount both refuse it. A file starting with the zstd magic
  `28 B5 2F FD` is not a mountable image. (ModGames44's `.zstd`, which Kraken
  Mount MOD reads, is a different thing: a bare exFAT image with zstd packs
  inside; see the 5.2-cycle report on GitHub #98.)
* YouTube packages for Y2JB are not bundled or downloaded: they are Google's
  app. The Y2JB tab installs them from the user's own folder; a private build
  can carry the owner's own copies (`make-release.ps1 -PrivateYoutube`).

## .zstd single-file images (GitHub #98, experimental)

The ".zstd single file" that ModGames44's closed MODGAMES ZSTD Container
writes and that only Kraken Mount MOD 0.2.5 and later mounts is, by the
reader's published source (kraken-mount-MOD `src/image.rs`, GPL-3.0; read
for the interface only, nothing copied), **a bare exFAT volume image**: the
exFAT boot sector at byte 0 (`EXFAT   ` at bytes 3 to 10), attached whole
with 512-byte sectors, the game at the image root, its files in the zstd
packs described above. There is no zstd frame, header, index or footer
around the volume; the "zstd" in the name is the packs inside. That is what
`image.build.zstd` with format `exfat` writes, named `.zstd` instead of
`.exfat`.

* Build page tile "ZSTD image" (badge "Experimental"; also in Library ›
  Build selected): queues `image.build.zstd` with format `exfat`, zstd packs
  on whatever Settings say, and the output `<name>.zstd`. The image's sector
  size is always 512 (the only size Kraken Mount MOD attaches with); a
  `.zstd` output with another format is refused.
* Opening: wherever images are opened (Image tools, Library, extract,
  convert, compare, edit, drops and file dialogs) a `.zstd` counts as an
  exFAT image once its first sector says so
  (`engines/integration/ZstdImage.h`). A file starting with a zstd frame
  (`28 B5 2F FD`, or a skippable frame `5x 2A 4D 18`), such as PS5ZST's
  `.exfat.zst`, is refused as "a zstd-compressed stream, not a mountable
  image"; anything else (a PFSC container renamed `.zstd`) as "not an exFAT
  image".
* What loads it: Kraken Mount MOD only. ShadowMount+ does not mount a
  `.zstd`: rename it to `.exfat` for ShadowMount+ (the packed-game warning
  about "Update game fakelib emulators" is for that case).
* Not proven: that MODGAMES ZSTD Container writes exactly this (its source,
  a sample and its chunk size are unpublished). Every result here is MOCK;
  a `.zstd` mounted and started on a console is HARDWARE REQUIRED.
* Tests: `tst_zstd_games` (`zstdSingleFileImage`,
  `zstdFilesThatAreNotImagesAreRefused`), `tst_build_page`
  (`zstdImageTile`).
