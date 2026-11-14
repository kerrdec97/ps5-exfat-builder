#pragma once

// Compares an image with the folder it should contain. Strict (audit H1):
// any missing, extra or different file or folder, and any structural damage,
// fails verification. No tolerance on byte or file counts.

#include "core/Cancellation.h"
#include "core/Progress.h"
#include "core/Result.h"
#include "engines/exfat/ExfatReader.h"
#include "engines/exfat/SourceScan.h"

#include <QJsonObject>

#include <functional>
#include <vector>

namespace fw::exfat {

struct Mismatch {
    QString path;   // relative, '/' separated
    QString kind;   // missing | extra | type | size | content | name-case | unreadable
    QString detail; // user-facing explanation
};

struct VerifyReport {
    bool full = false;
    qint64 filesChecked = 0;
    qint64 dirsChecked = 0;
    qint64 bytesCompared = 0;
    std::vector<Mismatch> mismatches; // sorted by path
    QStringList structureProblems;    // image damage
    QStringList warnings;             // not failures (lost clusters, ...)
    qint64 elapsedMs = 0;
    // The check was cancelled after it had already found a problem: the
    // report is partial, and it fails (never set on a report that passes).
    bool cancelled = false;

    bool passed() const { return mismatches.empty() && structureProblems.isEmpty(); }
    // "18,442 files compared byte for byte, all identical" / "3 problems: ..."
    QString summary() const;
    QJsonObject toJson(int maxMismatches = 200) const;
};

// One file the image should hold and where its expected bytes come from.
struct ExpectedFile {
    QString path;
    bool isDir = false;
    qint64 size = 0;
    // Exactly one source for the content of a file (full mode):
    QString localPath;         // a file on disk
    const ImageEntry* imageEntry = nullptr; // a file in another image
    // >= 0: the content is this image's own and was not touched (in-place
    // edits); the entry must still start at this cluster.
    qint64 unchangedFirstCluster = -1;
};

struct VerifyOptions {
    bool full = true;            // compare every byte (else names, sizes, structure)
    int threads = 1;             // full mode: files compared in parallel
    qint64 bufferBytes = 4 * 1024 * 1024;
    bool requireBackupBoot = true; // a damaged backup boot region fails verification
    // Called when the comparison starts (total units, total files, unit is bytes).
    std::function<void(qint64, qint64, bool)> onCompareStart;
};

// Verifies `image` against `expected` (the files and folders it must hold,
// nothing more). `otherImage` is the image that ExpectedFile::imageEntry
// points into (edits), or empty. A cancel returns Error::cancelled(), unless
// a problem was found before it: then the partial, failed report is returned
// (VerifyReport::cancelled), so the work is treated as not verifying.
Result<VerifyReport> verifyImage(const QString& image, const std::vector<ExpectedFile>& expected,
                                 const VerifyOptions& options, const CancellationToken& token,
                                 ProgressReporter* progress = nullptr, const QString& otherImage = {},
                                 const Geometry* otherGeometry = nullptr);

// Expected list for "the image holds exactly this folder" (exclusions applied).
std::vector<ExpectedFile> expectedFromFolder(const SourceTree& tree);

// Convenience: scans `folder` with `rules` and verifies the image against it.
Result<VerifyReport> verifyImageAgainstFolder(const QString& image, const QString& folder, const ScanRules& rules,
                                              const VerifyOptions& options, const CancellationToken& token,
                                              ProgressReporter* progress = nullptr);

} // namespace fw::exfat
