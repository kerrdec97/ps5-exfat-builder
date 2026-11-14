#pragma once

// PFS / FFPFSC image builder.
//
// Port of Lazy_MkPFS lazy_mkpfs/build.py — build_pfs (folder → PFS, optional
// per-file PFSC compression) and build_pfs_stream_single_file (one file →
// compressed PFS container) — and pack_folder/pack_file. Copyright the
// Lazy_MkPFS (Nazky) and MkPFS (PSBrew) authors; GPL-3.0-or-later.
//
// Output kinds:
//   * FolderImage (.ffpfs): the folder as a PFS image. Files are stored raw
//     (MkPFS "pack folder --no-compress", the legacy app's .ffpfs) unless
//     per-file compression is on (Lazy main_folder --no-exfat).
//   * Container (.ffpfsc): a compressed PFS holding exactly one file. From a
//     folder the file is the uncompressed folder image named pfs_image.dat
//     (the ShadowMount+ two-step method of the legacy PFS tab), generated on
//     the fly — it is never written to disk. From an image file (.exfat,
//     .ffpkg, ...) the file is packed as is (Lazy main_file / "pack file").
//
// Differences from Lazy_MkPFS (documented in native/docs/engines/PFS.md):
//   * the image is always ndblock × block size long (Lazy's disk mode;
//     Lazy's RAM mode leaves the last block short);
//   * per-file compression works with any thread count (Lazy crashes with
//     one worker or a single compressible file);
//   * the source folder is never written to (ampr_emu.index and .pfskeep
//     markers are added to the image only).

#include "core/Cancellation.h"
#include "core/Log.h"
#include "core/Result.h"
#include "engines/pfs/PfsLayout.h"
#include "engines/pfs/PfsSource.h"

#include <QString>

#include <functional>

namespace fw::pfs {

inline const QByteArray kNestedImageName = QByteArrayLiteral("pfs_image.dat");

enum class OutputFormat { Container, FolderImage };

struct PfscParams {
    bool compress = true;
    double minCompressMiB = 1.0; // smaller files are stored raw
    int blockGain = 1;           // minimum gain (%) for a 64 KiB block to be stored compressed
    int minFileGain = 0;         // minimum gain (%) for a whole file to be stored compressed
    bool skipExecutables = false;

    qint64 minCompressBytes() const { return qint64(minCompressMiB * 1024.0 * 1024.0); }
};

struct BuildOptions {
    LayoutOptions layout;
    int level = 9;
    int threads = 1;
    // Container (.ffpfsc): Lazy pack_file defaults.
    PfscParams container{true, 1.0, 1, 0, false};
    // Folder image (.ffpfs) per-file compression: Lazy pack_folder defaults, off by default.
    PfscParams files{false, 5.0, 5, 0, true};
};

struct BuildInput {
    bool isFolder = true;
    SourcePlan plan;  // folder
    QString file;     // single file (container from an image)
    QByteArray fileName; // name inside the image (single file); defaults to the file's name
};

struct BuildStats {
    qint64 files = 0;
    qint64 compressedFiles = 0;
    qint64 sourceBytes = 0;     // bytes of the packed files
    qint64 storedBytes = 0;     // their payload bytes in the image
    qint64 allCompressedBytes = 0; // if every block were stored compressed (Lazy statistics)
    qint64 nestedBytes = 0;     // size of the nested folder image (folder → container)
    qint64 imageBytes = 0;
    bool containerCompressed = false;
    int threads = 1;
    double seconds = 0;
    // Where the pipeline spent its time (compression runs on the pool). The
    // input is read by a reader thread when the pool has workers, else by the
    // writing thread.
    double readSeconds = 0;
    double waitSeconds = 0;  // the writing thread waiting for the next block in order (read and compressed)
    double writeSeconds = 0;
};

struct BuildHooks {
    std::function<void(qint64)> advance;          // source bytes processed
    std::function<void(const QString&)> item;     // current file
};

// Upper bound of the output size (files that do not shrink are stored raw).
Result<qint64> estimateImageSize(const BuildInput& input, OutputFormat format, const BuildOptions& options);
// Bytes the progress hooks will report in total.
qint64 progressTotal(const BuildInput& input);

// Writes the image to `path` (created or truncated). The caller owns the
// .part handling; on error the file is left for the caller to remove.
Result<BuildStats> writeImage(const BuildInput& input, OutputFormat format, const BuildOptions& options,
                              const QString& path, const CancellationToken& token, const BuildHooks& hooks = {},
                              const Log& log = Log(QStringLiteral("pfs")));

} // namespace fw::pfs
