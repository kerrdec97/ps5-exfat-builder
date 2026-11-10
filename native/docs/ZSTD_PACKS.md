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
| Jobs `ampr.pack` (Build page › Packed folder, Library › Pack with zstd…; a zipped dump is unpacked first) and `ampr.unpack` | `src/engines/ampr/Module.cpp` |
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
  .sprx`; the root `sce_sys/`, `sce_module/` and `fakelib*` folders; the AMPR
  files.
* Strategy *everything* (default): every other file is packed (ampr-emu-ng
  0.3.0+ serves fully packed games). *Compatible*: also root files,
  containers (.pak, .utoc), configs, media and video folders stay loose.
* `fakelib/libSceAmpr.sprx` becomes the bundled ampr-emu-ng; no other fakelib
  file is touched. The old runtime is named in the log.
* `ampr_emu.index` is written afresh from the game's files as the game sees
  them (loose, packed, the new runtime), on-disk case kept; the old journal
  (`ampr_commands.bin`) and logs are left out.
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
  ever removed.
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

## Reported problems and what the app does

| Report | Handled here |
|---|---|
| Silksong crashed: index said `Il2cppUserAssemblies.prx`, file was `Il2CppUserAssemblies.prx` (ampr-emu-ng #15) | Case is kept exactly in both indexes |
| Forge replaced `libSceAmpr.sprx` and rewrote `ampr_emu.index` on every conversion; Uncharted, Tintin broke (dump-forge #21) | Only packing touches them; a plain build or extract leaves them as they are |
| Backported games ship other fakelib libraries (ampr-emu-ng #2, #4, #9, #31, #34) | Only `fakelib/libSceAmpr.sprx` is replaced |
| "Update game fakelib emulators" replaces the runtime (dump-forge #10, #23) | Every zstd build warns about it |
| Days Gone left 58.7 GB loose with the old default (ampr-emu-ng #3) | Pack everything by default; *compatible* is an option |
| ASTRO BOT started with the keep-loose profile but not fully packed (dump-forge #27) | The *compatible* strategy |
| A plain ELF eboot never starts (dump-forge 9ab367c, ampr-emu-ng #8) | Warned in the log as soon as it is seen, before packing, and again in the job's result |
| Old LZ4 packs must be decoded before repacking (ampr-emu-ng #21) | LZ4 and zstd packed sources are unpacked on the fly and packed again |
| Packing into an image needs space and twice the CPU (dump-forge #15) | Packs go to a work folder next to the output, loose files hard-linked; removed after the build or after a crash |
| Corrupt or truncated sources (ampr-emu-ng #7) | Every chunk decoded again while packing, every volume read back against its CRCs, the image verified against the packed folder |
| .ffpfsc holding an image (dump-forge default, Meaners' nested `pfs_image.dat`) | Opened straight through for extract, FPKG and the title |
| FPKG from images needed a full extra copy | Only the files the package keeps are staged, packs unpacked, next to the output |
| Runtime-only problems (pread threads, Wolverine snow scene, Crimson Desert, …) | Not fixable by a packer; the known titles are listed in the release notes |

## Not done

* LZ4 packs are read, never written (decided: LZ4 needs traces).
* TOML pack profiles (dump-forge #13, #15, #31) and packing straight to the
  console over FTP (dump-forge #5) are not implemented.
* YouTube packages for Y2JB are not bundled or downloaded: they are Google's
  app. The Y2JB tab installs them from the user's own folder; a private build
  can carry the owner's own copies (`make-release.ps1 -PrivateYoutube`).
