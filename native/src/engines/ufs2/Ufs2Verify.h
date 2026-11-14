#pragma once

// Comparing an image with the tree it should contain, and extracting an image
// to a folder.

#include "core/Cancellation.h"
#include "core/PathSafety.h"
#include "core/Progress.h"
#include "core/Result.h"
#include "engines/ufs2/Ufs2Reader.h"
#include "engines/ufs2/Ufs2Tree.h"

#include <QStringList>

#include <vector>

namespace fw::ufs2 {

enum class CompareDepth {
    Structure, // names, types and sizes
    Full,      // ...and every byte
};

CompareDepth compareDepthFromSetting(const QString& mode); // "full" / "structure"

struct CompareReport {
    qint64 filesCompared = 0;
    qint64 dirsCompared = 0;
    qint64 bytesCompared = 0;
    QStringList missing; // expected but not in the image
    QStringList extra;   // in the image but not expected
    QStringList changed; // different type, size or content
    // The comparison was cancelled after it had already found a difference:
    // the report is partial, and it fails (never set on a report that is ok).
    bool cancelled = false;

    bool ok() const { return missing.isEmpty() && extra.isEmpty() && changed.isEmpty(); }
    QString summary(CompareDepth depth) const;
};

// Compares `image` (from its root, or `fsPath`) with `expected`. Progress
// advances by expected bytes compared (Full) or by one unit and one file per
// file compared (Structure: declare the stage's total in files).
// A cancel returns Error::cancelled(), unless a difference was found before
// it: then the partial report is returned (CompareReport::cancelled), so the
// work is treated as not verifying. verifyExtracted() does the same.
Result<CompareReport> compareImage(const Ufs2Reader& image, const SourceTree& expected, CompareDepth depth,
                                   const CancellationToken& token, ProgressReporter* progress,
                                   qint64 bufferBytes = 4 * 1024 * 1024, const QString& fsPath = QStringLiteral("/"));

// Reads every file of the image completely (readability check, used when
// there is no source to compare with).
Result<qint64> readEveryFile(const Ufs2Reader& image, const SourceTree& tree, const CancellationToken& token,
                             ProgressReporter* progress, qint64 bufferBytes = 4 * 1024 * 1024);

struct ExtractReport {
    qint64 files = 0;
    qint64 dirs = 0;
    qint64 bytes = 0;
    QStringList renamed; // names the destination file system cannot store as they are
    std::vector<std::pair<int, QString>> written; // tree node -> file written
};

// The name an image entry gets on disk (core/PathSafety.h). With Windows
// rules, characters Windows forbids become '_', trailing dots/spaces go and
// reserved device names (CON, NUL, COM1...) get a '_' prefix; otherwise names
// are unchanged.
using fw::hostFileName;

// Writes `tree` (loaded from `image`) into the existing, empty folder `dest`.
// Names that collide on the destination (after the Windows rules above, or
// differing only in case on Windows) get a " (2)" suffix and are listed in
// ExtractReport::renamed.
// Progress advances by bytes written.
Result<ExtractReport> extractTree(const SourceTree& tree, const QString& dest, const CancellationToken& token,
                                  ProgressReporter* progress, qint64 bufferBytes = 4 * 1024 * 1024);

// Re-reads what extractTree() wrote and compares it with the image.
Result<CompareReport> verifyExtracted(const SourceTree& tree, const ExtractReport& report, CompareDepth depth,
                                      const CancellationToken& token, ProgressReporter* progress,
                                      qint64 bufferBytes = 4 * 1024 * 1024);

} // namespace fw::ufs2
