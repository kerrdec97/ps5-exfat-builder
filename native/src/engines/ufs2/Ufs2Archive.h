#pragma once

// FFPKG (UFS2) images built straight from an archived dump (GitHub #37): no
// copy of the game on disk (core/ArchiveStream.h).
//
// The tree is what scanLocalFolder() gives for the folder
// unpackArchiveSource() would make: every file and folder (empty ones too),
// no links, each folder's entries in the order an NTFS listing gives them
// (by their names in upper case), which is the order UFS2Tool, and this
// writer, take them in. So on Windows (where the unpacked folder is on NTFS)
// the image is byte for byte the image of that folder, given the same
// format/populate times and fs_id. Inode times are the build's time either
// way; a file's listed modification time only feeds ampr_emu.index.

#include "core/ArchiveSource.h"
#include "core/ArchiveStream.h"
#include "core/Cancellation.h"
#include "core/Progress.h"
#include "core/Result.h"
#include "engines/ufs2/Ufs2Tree.h"
#include "engines/ufs2/Ufs2Writer.h"

#include <QDateTime>

namespace fw::ufs2 {

// `buildTime` stands in for a time the archive does not list.
Result<SourceTree> treeFromArchive(const archive::GameListing& game, const QDateTime& buildTime,
                                   const CancellationToken& token = {});

// Where each item of the game goes in the image `writer` planned for `tree`.
std::vector<archive::Placement> archivePlacements(const SourceTree& tree, const Ufs2Writer& writer,
                                                  const archive::GameListing& game);

struct ArchiveWriteStats {
    WriteStats metadata;         // Ufs2Writer::write() with the archive's data left out
    archive::StreamStats stream; // the pass through the archive
};

// Writes the image `writer` planned for `tree` (its options must have
// leaveArchiveData set), then the archive's files, then flushes the image to
// the device. Afterwards every node from the archive carries its digest
// (TreeNode::hasDigest), so compareImage() checks it against the image.
Result<ArchiveWriteStats> writeImageFromArchive(Ufs2Writer& writer, SourceTree& tree, const archive::GameListing& game,
                                                const QString& path, qint64 bufferBytes, const CancellationToken& token,
                                                ProgressReporter* progress = nullptr);

} // namespace fw::ufs2
