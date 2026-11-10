#pragma once

// Verification and extraction of PFS / FFPFSC images.
//
// Verification is strict: every file the build packed must be in the image
// with the same bytes, and the image must hold nothing else. It compares
// against the same SourcePlan the build used (exclusions, generated
// ampr_emu.index and .pfskeep markers included), so a deliberately excluded
// file can never cause a false failure (audit H2). A container built from a
// folder is checked through its nested pfs_image.dat without extracting it.
//
// The only tolerated difference is the PFS format's own: a PFSC-compressed
// file's logical size is rounded up to the block size with zero bytes
// (Lazy_MkPFS build_pfs: size_compressed = ceil(raw / bs) * bs). Those files
// are counted in VerifyReport::paddedFiles.
//
// Extraction follows Lazy_MkPFS inspect.py extract_pfs_image; `deep` unpacks
// the nested image of a container (MkPFS "unpack --deep").
// GPL-3.0-or-later.

#include "core/Cancellation.h"
#include "core/Result.h"
#include "engines/pfs/PfsBuilder.h"
#include "engines/pfs/PfsSource.h"

#include <QString>
#include <QStringList>

#include <functional>

namespace fw::pfs {

enum class VerifyDepth { Structure, Full };

struct VerifyExpectation {
    enum class Kind { None, Folder, File };
    Kind kind = Kind::None;
    OutputFormat format = OutputFormat::Container;
    const SourcePlan* plan = nullptr; // Kind::Folder
    QString file;                     // Kind::File: the source image
    QByteArray fileName;              // Kind::File: its name inside the container
};

struct VerifyReport {
    bool passed = true;
    QStringList problems; // first 50, user-facing
    qint64 problemCount = 0;
    qint64 filesChecked = 0;
    qint64 bytesChecked = 0;
    qint64 paddedFiles = 0;
    bool nested = false;
    bool contentCompared = false;
    QStringList warnings;
    QString summary;

    void fail(const QString& what);
};

struct ProgressHooks {
    std::function<void(qint64)> advance;
    std::function<void(const QString&)> item;
};

// Structural damage (bad metadata, a block that does not decompress) is
// returned as ErrorCode::CorruptData; content differences make passed false.
Result<VerifyReport> verifyImage(const QString& image, const VerifyExpectation& expect, VerifyDepth depth,
                                 const CancellationToken& token, const ProgressHooks& hooks = {});

// Quick facts about an image (opens and validates the structure only).
struct ImageSummary {
    bool nested = false;          // a container holding a nested PFS image
    QByteArray singleFile;        // the file's name when the image holds exactly one file
    qint64 timestamp = 0;         // inode time = build time (of the nested image when nested)
    qint64 files = 0;
    qint64 dirs = 0;
    qint64 logicalBytes = 0;      // of the target (nested) image's files
    qint64 outerFiles = 0;        // files of the image itself (1 for a container)
    qint64 outerBytes = 0;        // their logical size: what a not-deep extraction writes
    qint64 imageBytes = 0;
    int blockSize = 0;
    bool caseInsensitive = false;
    QStringList warnings;
};
Result<ImageSummary> inspectImage(const QString& image);

// Bytes verifyImage will report through the progress hook.
Result<qint64> verifyWorkBytes(const QString& image, const VerifyExpectation& expect, VerifyDepth depth);

struct ExtractOptions {
    bool deep = true;       // unpack the nested image of a container
    bool overwrite = false; // allow a non-empty output folder (files are replaced)
    qint64 freeSpaceMargin = 256LL * 1024 * 1024;
};

struct ExtractReport {
    QString output;
    qint64 files = 0;
    qint64 dirs = 0;
    qint64 bytes = 0;
    bool nested = false;
    QStringList warnings;
    QStringList renamed; // "image path → name on disk" for names this drive cannot store as they are
};

// Unpacks the image into `outputDir`. A new or empty folder is filled through
// "<outputDir>.part" and renamed at the end, so a cancelled or failed
// extraction leaves nothing behind. Every output path is built from image
// names that pass isSafeEntryName() and is checked to stay inside the folder
// (core/PathSafety.h); names the drive cannot store (on Windows: forbidden
// characters, trailing dots or spaces, device names such as CON; names that
// then collide get " (2)") are changed and listed in ExtractReport::renamed.
Result<ExtractReport> extractImage(const QString& image, const QString& outputDir, const ExtractOptions& options,
                                   const CancellationToken& token, const ProgressHooks& hooks = {});

} // namespace fw::pfs
