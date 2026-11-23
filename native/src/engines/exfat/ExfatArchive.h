#pragma once

// exFAT images built straight from an archived dump (GitHub #37): no copy of
// the game on disk (core/ArchiveStream.h).
//
// The tree is made from the archive's list exactly as scanning the folder
// that unpackArchiveSource() would make gives it, so the image is byte for
// byte the image of that folder:
//  * the same names, sizes, folders (listed or implied, empty ones too) and
//    exclusion rules; links are left out, as unpacking leaves them out;
//  * the times unpacking leaves: a file's modification time is the one the
//    archive lists (whole seconds; the build's time when it lists none), its
//    creation and access times are when it was written, and a folder's three
//    times are when it was made; here all of those are `buildTime`;
//  * the attributes of a new file (archive) and folder (none).
// The metadata and any file the build made itself (ampr_emu.index) are
// written by writeImage(), the game's files by one pass through the archive.

#include "core/ArchiveSource.h"
#include "core/ArchiveStream.h"
#include "core/Cancellation.h"
#include "core/Log.h"
#include "core/Progress.h"
#include "core/Result.h"
#include "engines/exfat/ExfatVerify.h"
#include "engines/exfat/ExfatWriter.h"
#include "engines/exfat/SourceScan.h"

#include <QDateTime>

#include <vector>

namespace fw::exfat {

Result<SourceTree> treeFromArchive(const archive::GameListing& game, const ScanRules& rules, const QDateTime& buildTime,
                                   const CancellationToken& token = {});

// Where each item of the game goes in the planned image.
std::vector<archive::Placement> archivePlacements(const SourceTree& tree, const ImagePlan& plan,
                                                  const archive::GameListing& game);

// What the finished image must hold: every file from the archive with the
// digest of the bytes written for it, the others from their files on disk.
std::vector<ExpectedFile> expectedFromArchive(const SourceTree& tree, const std::vector<quint64>& digests);

struct ArchiveWriteStats {
    WriteStats metadata;          // writeImage(): metadata and the build's own files
    archive::StreamStats stream;  // the pass through the archive
};

// Writes the planned image to `path`: writeImage() with the archive's files
// left out, then the archive pass, then (options.syncToDisk) a flush to the device.
Result<ArchiveWriteStats> writeImageFromArchive(const SourceTree& tree, const ImagePlan& plan,
                                                const archive::GameListing& game, const QString& path,
                                                const WriterOptions& options, const CancellationToken& token,
                                                ProgressReporter* progress = nullptr, const Log* log = nullptr);

} // namespace fw::exfat
