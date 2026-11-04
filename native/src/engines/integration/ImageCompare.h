#pragma once

// Compares two file trees (images of any format, or an image and a folder)
// through ImageFiles.h: files and folders only on one side, files whose size
// differs and, at full depth, files whose bytes differ (both sides streamed
// and compared chunk by chunk; the first differing offset is reported).
//
// A is the reference: "missing" = only in A (missing from B), "extra" = only
// in B. Paths are compared exactly (case-sensitive, as on the PS5's UFS2).
// The only tolerated size difference is the PFS format's own: a PFSC-
// compressed file is stored with its size rounded up to the block size with
// zero bytes; such a file matches when the other side is the unpadded size
// (and, at full depth, the padding is zero). It is counted in paddedFiles.
//
// Behavioural reference: the legacy ffpkg tab's "Compare" (_compare_ffpkgs,
// _ffpkg_write_compare_report): only in A / only in B (files and folders),
// different size, different hash. UFS2 mode and BSD-flag differences are not
// compared: the native readers do not expose them for every format.

#include "core/Cancellation.h"
#include "core/Progress.h"
#include "core/Result.h"
#include "engines/integration/ImageFiles.h"

#include <QJsonObject>
#include <QStringList>

#include <vector>

namespace fw::integration {

enum class CompareDepth { Structure, Full };

QString compareDepthName(CompareDepth depth); // "structure" | "full"

struct CompareOptions {
    CompareDepth depth = CompareDepth::Full;
    int listLimit = 500;           // entries kept per list; the counts stay exact
    qint64 chunkBytes = 1 << 20;   // full depth: bytes read per side per step
};

struct SizeDifference {
    QString path;
    qint64 a = 0, b = 0;
};

struct ContentDifference {
    QString path;
    qint64 offset = 0; // first byte that differs
};

struct UnreadableFile {
    QString path;
    QString message;
};

struct CompareReport {
    QString a, b;                     // paths as given
    QString aKind, bKind;             // FileTree::describe()
    CompareDepth depth = CompareDepth::Full;
    qint64 filesA = 0, filesB = 0, dirsA = 0, dirsB = 0, bytesA = 0, bytesB = 0;

    // Counts are exact; the lists hold at most CompareOptions::listLimit entries.
    qint64 missingCount = 0, extraCount = 0, missingDirCount = 0, extraDirCount = 0;
    qint64 sizeCount = 0, contentCount = 0, unreadableCount = 0;
    QStringList missing, extra, missingDirs, extraDirs;
    std::vector<SizeDifference> sizeDifferent;
    std::vector<ContentDifference> contentDifferent;
    std::vector<UnreadableFile> unreadable;

    qint64 filesInBoth = 0;      // files present on both sides
    qint64 contentCompared = 0;  // files whose bytes were compared (full depth)
    qint64 bytesCompared = 0;
    qint64 paddedFiles = 0;
    int listLimit = 500;
    QStringList warnings;

    qint64 differenceCount() const
    {
        return missingCount + extraCount + missingDirCount + extraDirCount + sizeCount + contentCount + unreadableCount;
    }
    bool identical() const { return differenceCount() == 0; }
    bool truncated() const;
    QString summary() const;

    // The JobOutcome::details shape (documented in services/JobSubmit.h).
    QJsonObject toJson() const;
};

// Compares two open trees. Progress (when given): one phase, `phase`, with the
// bytes to compare (full) or the files to check (structure) as its total.
Result<CompareReport> compareTrees(FileTree& a, FileTree& b, const CompareOptions& options,
                                   const CancellationToken& token, ProgressReporter* progress = nullptr, int phase = 0);

} // namespace fw::integration
