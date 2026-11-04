#pragma once

// Copies every file and folder of an image into a folder on disk, keeping
// timestamps (created where the platform allows, modified, accessed) and the
// Windows attributes. The job wraps this with a "<dest>.part" folder that is
// renamed only after verification, so a partial extraction never appears
// under the final name.

#include "core/Cancellation.h"
#include "core/Progress.h"
#include "core/Result.h"
#include "engines/exfat/ExfatReader.h"

namespace fw::exfat {

struct ExtractOptions {
    bool parallel = false;         // several files at once (helps with many small files)
    int threads = 1;               // parallel mode
    qint64 bufferBytes = 4 * 1024 * 1024;
    int bufferCount = 6;           // sequential mode: buffers in flight
    bool preserveTimes = true;
    bool preserveAttributes = true;
};

struct ExtractStats {
    qint64 files = 0;
    qint64 dirs = 0;
    qint64 bytes = 0;
    qint64 elapsedMs = 0;
    QStringList warnings;
};

// Writes the contents of `tree` below `destDir` (which must exist). Progress
// is advanced by bytes and files written.
Result<ExtractStats> extractTree(const ExfatReader& reader, const ImageTree& tree, const QString& destDir,
                                 const ExtractOptions& options, const CancellationToken& token,
                                 ProgressReporter* progress = nullptr);

} // namespace fw::exfat
