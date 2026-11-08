#pragma once

// Which files of a game go into zstd packs (ps5-dump-forge docs/packing.md
// and docs/LZ4.md v0.5.4; ampr-emu-ng docs/zstd-format.md v0.3.3).
//
// Always loose, whatever the strategy: empty files; eboot.bin (any folder, any
// case) and every .elf .self .prx .sprx; everything in a root sce_sys/ or
// sce_module/ folder or a root folder whose name starts with "fakelib"; the
// AMPR files (ampr_emu.index, ampr_assets.index[.crc|.runtime],
// ampr_commands.bin, ampr_emu.log, apr_emu.log) and the zstd pack's own files.
// The system loads executables and modules itself, outside the runtime.
//
// Strategies:
//   Everything  pack every other file (ampr-emu-ng serves plain reads,
//               directory listings and mmap of packed files). The smallest.
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

// Titles reported not to work packed with ampr-emu-ng 0.3.3 (issue trackers of
// ampr-emu-ng and ps5-dump-forge, 2026-11). Shown as a caution, never enforced.
QStringList knownPackProblemTitles();

} // namespace fw::ampr
