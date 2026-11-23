#pragma once

// Which files of a game go into zstd packs (ps5-dump-forge docs/packing.md
// and docs/LZ4.md v0.5.4, the .ini rule of v0.5.5; ampr-emu-ng
// docs/zstd-format.md v0.3.3, rules unchanged up to v0.5.0).
//
// Always loose, whatever the strategy: empty files; eboot.bin (any folder, any
// case) and every .elf .self .prx .sprx; every .ini (any folder, any case), so
// dlc_emu.ini stays editable and visible to Kraken Mount and dlc_emu, as
// ps5-dump-forge 0.5.5 does; everything in a root sce_sys/ or sce_module/
// folder or a root folder whose name starts with "fakelib"; the AMPR files
// (ampr_emu.index, ampr_assets.index[.crc|.runtime], ampr_commands.bin,
// ampr_emu.log, apr_emu.log) and the zstd pack's own files. The system loads
// executables and modules itself, outside the runtime.
//
// Strategies:
//   Everything  pack every other file (ampr-emu-ng serves plain reads and
//               directory listings of packed files; mmap of a packed file
//               fails, and no measured game does it). The smallest.
//   Compatible  also keep root files and the compatibility keep-loose list
//               (containers, configs, media, video folders, protected folders,
//               Insomniac's boot containers) loose: for games that do not
//               start fully packed.

#include <QString>
#include <QStringList>

namespace fw::ampr {

enum class PackStrategy { Everything, Compatible };
PackStrategy packStrategyFromName(const QString& name); // "everything" (default) | "compatible"
QString packStrategyName(PackStrategy s);

// Why `relPath` stays loose under every strategy, or "".
QString alwaysLooseReason(const QString& relPath, qint64 size);
// Why the compatible strategy keeps `relPath` loose, or "".
QString compatibleLooseReason(const QString& relPath);
// Combined: "" = pack it.
QString looseReason(const QString& relPath, qint64 size, PackStrategy strategy);

// Files a pack job leaves out of its output: the old AMPR journal and logs
// (they name files by the old index's order) and an old zstd pack.
bool packJobDrops(const QString& relPath);

// eboot.bin's first bytes: PS5 SELF (54 14 F5 EE) and PS4 SELF (4F 15 3D 1D)
// start; a plain ELF (7F 45 4C 46) never starts on the console.
enum class ExecutableKind { Self, PlainElf, Unknown };
ExecutableKind executableKind(const QByteArray& head);

// Titles reported not to work packed (issue trackers of ampr-emu-ng and
// ps5-dump-forge; bundled runtime 0.5.0, checked on 22 Nov 2026 against
// upstream up to 0.5.0). Shown as a caution, never enforced. A title leaves
// the list only when its issue is closed with the game working: Sonic X
// Shadow Generations went (ampr-emu-ng #8: the runtime works with a complete
// pack; the cause was a packer that dropped folders); Marvel's Wolverine stays
// (#48 closed while it still crashed on 0.3.4; #116, saving, open); Control
// Resonant came (#112, open). The other titles were not re-checked.
QStringList knownPackProblemTitles();

} // namespace fw::ampr
