#pragma once

// LZ4 asset packs of ampr_emu (drakmor, GPL-3.0-or-later): ampr_assets.index
// (AMPRPAK4) + ampr_assets-NNN.pak. READ ONLY: this app unpacks LZ4-packed
// games (to build an FPKG, extract or repack them as zstd); it never writes
// LZ4 packs.

#include "core/Cancellation.h"
#include "core/Result.h"

#include <QString>
#include <QStringList>

#include <memory>
#include <vector>

namespace fw::ampr {

class GameFiles;
class RandomAccess;

inline const QString kLz4IndexName = QStringLiteral("ampr_assets.index");

class Lz4PackReader {
public:
    // Reads the manifest of the packed game `files` and checks its volumes.
    static Result<std::unique_ptr<Lz4PackReader>> open(std::shared_ptr<GameFiles> files, const CancellationToken& token);
    ~Lz4PackReader();

    int fileCount() const;
    QString path(int file) const;  // '/'-separated, relative to the game root
    qint64 size(int file) const;
    // The pack's own files (index, its companions, the volumes), left out when unpacking.
    QStringList packFiles() const;
    // Reads up to `len` bytes of `file` from `offset`; 0 at the end.
    Result<qint64> read(int file, qint64 offset, char* dst, qint64 len);

    struct Impl;

private:
    Lz4PackReader();
    std::unique_ptr<Impl> d;
};

} // namespace fw::ampr
