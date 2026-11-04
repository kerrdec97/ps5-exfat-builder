#pragma once

// Builds a UFS2 image (.ffpkg) from a SourceTree, producing the same bytes as
// `UFS2Tool newfs -O 2 -b B -f F -m M -S 512 -i I -D <folder> <image>` (UFS2Tool
// 4.0.0, c123639) for the same tree, timestamps and fs_id.
//
// Two passes: plan() replays UFS2Tool's allocation (inode numbers, block
// addresses, directory blocks, cylinder-group bitmaps and counts) without
// touching the sources; write() then emits the image strictly front to back.
// Source reads and image writes overlap through a small pool of large buffers
// handed to a writer thread.

#include "core/Cancellation.h"
#include "core/Progress.h"
#include "core/Result.h"
#include "engines/ufs2/Ufs2Format.h"
#include "engines/ufs2/Ufs2Tree.h"

#include <memory>

namespace fw::ufs2 {

struct WriterOptions {
    FormatParams params;
    // UFS2Tool takes "now" twice: when it formats the empty file system
    // (superblock and cylinder-group times, fs_id) and again when it adds the
    // files (every inode's times). -1 = current time; populateTime -1 = formatTime.
    qint64 formatTime = -1;
    qint64 populateTime = -1;
    // fs_id[1]. UFS2Tool uses (time >> 16) ^ process id; -1 = (time >> 16) ^ random.
    qint64 fsId1 = -1;
    // Image size in bytes; 0 = UFS2Tool's auto-size rule (newfs -D without a size).
    qint64 sizeBytes = 0;
    qint64 ioBufferBytes = 4 * 1024 * 1024;
    int ioBuffers = 3;
};

struct PlanSummary {
    qint64 imageBytes = 0;    // length of the image file
    qint64 autoSizeBytes = 0; // what UFS2Tool's rule gives for this tree
    bool enlarged = false;    // the rule was too small for this tree and was grown
    Geometry geometry;
    quint32 inodesUsed = 0;
    qint64 dataBytes = 0;
    qint64 files = 0;
    qint64 dirs = 0;
};

struct WriteStats {
    qint64 imageBytes = 0;
    qint64 dataBytes = 0;
    qint64 elapsedMs = 0;
    double bytesPerSec = 0;
};

// UFS2Tool CalculateDirectorySizes(dir, bsize) + CalculateImageSize() + the
// inode check in CreateImageFromDirectory(): the size newfs -D picks.
qint64 ufs2toolAutoSize(const SourceTree& tree, const FormatParams& params);

class Ufs2Writer {
public:
    Ufs2Writer(const SourceTree& tree, WriterOptions options);
    ~Ufs2Writer();
    Ufs2Writer(const Ufs2Writer&) = delete;
    Ufs2Writer& operator=(const Ufs2Writer&) = delete;

    // Lays the image out. Cheap compared with write(): no source file is read.
    Result<PlanSummary> plan();
    // Writes the planned image to `path` (created or truncated). Progress
    // advances by source bytes copied. On failure the file is left for the
    // caller to delete (callers write to a PartFile).
    Result<WriteStats> write(const QString& path, const CancellationToken& token, ProgressReporter* progress);

    struct Plan; // layout state, private to Ufs2Writer.cpp

private:
    const SourceTree& m_tree;
    WriterOptions m_opt;
    std::unique_ptr<Plan> m_plan;
    PlanSummary m_summary;
};

} // namespace fw::ufs2
