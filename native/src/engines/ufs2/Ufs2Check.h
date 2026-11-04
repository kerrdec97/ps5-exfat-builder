#pragma once

// Structural check of a UFS2 image, modelled on FreeBSD fsck_ffs passes 1-5
// (read-only, nothing is repaired): cylinder-group headers and summaries,
// superblock copies, the directory tree ("." / ".." / reachability), block
// pointers (range, duplicates, di_blocks), link counts, and the inode and
// fragment bitmaps against what the inodes actually use.

#include "core/Cancellation.h"
#include "core/Progress.h"
#include "core/Result.h"
#include "engines/ufs2/Ufs2Reader.h"

#include <QStringList>

namespace fw::ufs2 {

struct CheckReport {
    QStringList problems; // empty = consistent
    qint64 problemCount = 0;
    qint64 files = 0;
    qint64 dirs = 0;
    qint64 bytes = 0;
    qint64 inodes = 0;

    bool ok() const { return problemCount == 0; }
    QString summary() const;
};

// I/O failures and cancellation are errors; inconsistencies are reported in
// CheckReport::problems (at most `maxProblems` listed). Progress advances by
// one unit per inode checked.
Result<CheckReport> checkImage(const Ufs2Reader& reader, const CancellationToken& token,
                               ProgressReporter* progress = nullptr, int maxProblems = 50);

} // namespace fw::ufs2
