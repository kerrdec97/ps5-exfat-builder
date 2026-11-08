#pragma once

// Packs a game into a new folder with zstd packs (the folder an image build
// then takes as its source):
//
//   * the source is read as plain files (a zstd or LZ4 packed source is
//     unpacked on the fly and packed again);
//   * the always-loose files and, with the compatible strategy, the
//     keep-loose list stay loose: hard-linked from the source folder when
//     possible (no copy, no space), else copied;
//   * every other file goes into ampr_zstd.index + ampr_zstd-NNN.pak;
//   * fakelib/libSceAmpr.sprx becomes the bundled ampr-emu-ng (only that
//     file; the rest of fakelib/ is the game's);
//   * the old AMPR journal and logs are left out and ampr_emu.index is
//     written afresh from the files the folder holds (on-disk case kept);
//   * every chunk is decoded again while packing and every volume is read
//     back against its CRCs.
//
// The source is never changed.

#include "core/Cancellation.h"
#include "core/Log.h"
#include "core/Progress.h"
#include "core/Result.h"
#include "engines/ampr/GameFiles.h"
#include "engines/ampr/PackRules.h"
#include "engines/ampr/Runtime.h"

#include <QString>
#include <QStringList>

#include <functional>
#include <memory>

namespace fw::ampr {

struct PackGameOptions {
    int level = kZstdDefaultLevel;
    int threads = 0;
    PackStrategy strategy = PackStrategy::Everything;
    bool verify = true;
    // Files and folders the user leaves out of the build (true = leave out).
    std::function<bool(const QString& relPath, bool isDir)> exclude;
    // When the source is this plain folder, loose files are hard-linked from it.
    QString linkFromFolder;
    qint64 freeSpaceMarginBytes = 256LL * 1024 * 1024;
    // Progress phases of the caller (-1 = do not change phases).
    int packPhase = -1;
    int verifyPhase = -1;
};

struct PackGameReport {
    PackKind sourcePacks = PackKind::None; // the source was already packed
    qint64 files = 0;          // files in the output (logical)
    qint64 packedFiles = 0;
    qint64 packedBytes = 0;    // logical bytes inside the packs
    qint64 storedBytes = 0;    // bytes of the pack volumes
    qint64 volumes = 0;
    qint64 looseFiles = 0;
    qint64 looseBytes = 0;
    qint64 linkedFiles = 0;    // loose files hard-linked instead of copied
    qint64 excludedFiles = 0;
    QStringList droppedFiles;  // journal, logs, old packs
    RuntimeInfo previousRuntime;
    QString runtime;           // "ampr-emu-ng 0.3.3"
    bool amprIndexWritten = false;
    QStringList looseSummary;  // "executable or module: 12 files, 80.1 MB"
    QStringList warnings;
    int threads = 1;
    int level = kZstdDefaultLevel;
    qint64 elapsedMs = 0;

    QString summary() const;   // "4,810 files packed (38.4 GB from 90.6 GB), 312 loose"
};

Result<PackGameReport> packGame(std::shared_ptr<GameFiles> source, const QString& outFolder, const PackGameOptions& options,
                                const CancellationToken& token, ProgressReporter* progress, const Log& log);

// Writes every file of `source` (read as plain files: packs unpacked) into
// `outFolder`, leaving out the pack files. Used by extraction ("unpack").
struct UnpackReport {
    PackKind kind = PackKind::None;
    qint64 files = 0;
    qint64 bytes = 0;
    qint64 unpackedFiles = 0; // files that came out of packs
    bool amprIndexWritten = false;
    QStringList droppedFiles;
};
struct UnpackOptions {
    // Rewrite ampr_emu.index for the unpacked files when the game has one.
    bool refreshAmprIndex = true;
    std::function<bool(const QString& relPath, bool isDir)> exclude;
    qint64 freeSpaceMarginBytes = 256LL * 1024 * 1024;
};
Result<UnpackReport> unpackGame(std::shared_ptr<GameFiles> source, const QString& outFolder, const UnpackOptions& options,
                                const CancellationToken& token, ProgressReporter* progress, const Log& log);

// Sequential reader over one item of a GameFiles.
std::unique_ptr<SequentialReader> itemReader(std::shared_ptr<GameFiles> files, int index);

} // namespace fw::ampr
