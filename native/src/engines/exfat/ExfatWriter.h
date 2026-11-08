#pragma once

// Builds a raw exFAT volume image from a scanned folder, in one sequential
// pass and without mounting anything.
//
// Layout (see docs/engines/EXFAT.md): a bare volume (no partition table, as
// the legacy OSFMount "logical volume" + format.com produced), main and backup
// boot regions, one FAT, then the cluster heap: allocation bitmap, up-case
// table, root directory, every sub-directory, then file data in path order.
// Every file and folder is one contiguous run of clusters (NoFatChain), so the
// allocation bitmap is a single prefix of set bits.

#include "core/Cancellation.h"
#include "core/Log.h"
#include "core/Progress.h"
#include "core/Result.h"
#include "engines/exfat/ExfatFormat.h"
#include "engines/exfat/SourceScan.h"

#include <QString>

#include <functional>
#include <memory>
#include <vector>

namespace fw::exfat {

enum class SpareMode {
    Legacy, // the legacy rule: 1 % of the image, at least 1 GiB, at most 8 GiB
    Fixed,  // exactly WriterOptions::spareBytes of free space
};

// Where the bytes of one file come from while the image is written: a file
// on disk (builds) or an entry of another image (edits that rebuild).
class NodeReader {
public:
    virtual ~NodeReader() = default;
    virtual qint64 size() const = 0;
    virtual qint64 read(char* buffer, qint64 length) = 0; // bytes read, 0 at the end, < 0 on error
    virtual QString errorString() const = 0;
};
using NodeOpener = std::function<Result<std::unique_ptr<NodeReader>>(const SourceNode&, const CancellationToken&)>;

struct WriterOptions;
// Opens SourceNode::absolutePath with the options' retry policy.
Result<std::unique_ptr<NodeReader>> openLocalNode(const SourceNode& node, const WriterOptions& options,
                                                  const CancellationToken& token);

struct WriterOptions {
    int sectorBytes = 512;
    int clusterBytes = 0;          // 0 = automatic (kAutoClusterBytes)
    qint64 volumeBytes = 0;        // > 0: use this image size (legacy "image size override")
    SpareMode spare = SpareMode::Legacy;
    qint64 spareBytes = 0;         // SpareMode::Fixed
    QString label;                 // up to 11 characters, may be empty
    bool fatChains = false;        // write FAT chains instead of NoFatChain runs
    qint64 bufferBytes = 4 * kMiB; // one read/write buffer
    int bufferCount = 6;           // buffers in flight between reader and writer
    int openRetries = 3;           // source files that cannot be opened
    int retryWaitMs = 3000;
    quint32 serial = 0;            // 0 = derived from the current time
    bool syncToDisk = true;        // flush the image to the device before returning
    NodeOpener opener;             // empty: read SourceNode::absolutePath from disk
};

struct NodePlacement {
    quint32 firstCluster = 0; // 0 for empty files
    quint32 clusters = 0;
    qint64 dataLength = 0;    // file size, or directory bytes (whole clusters)
};

struct ImagePlan {
    Geometry geo;
    QString label;
    quint32 serial = 0;
    bool fatChains = false;
    quint32 bitmapCluster = 0, bitmapClusters = 0;
    quint32 upcaseCluster = 0, upcaseClusters = 0;
    std::vector<NodePlacement> nodes; // parallel to SourceTree::nodes; [0] = root
    std::vector<int> dirOrder;        // directories in cluster order (root first)
    std::vector<int> fileOrder;       // files in cluster order (empty files included)
    quint32 usedClusters = 0;         // clusters 2 .. 2 + usedClusters - 1 are in use
    QString clusterChoice;            // why this cluster size, for the log
    QString sizeChoice;               // why this image size, for the log

    qint64 dataEndBytes() const { return geo.clusterOffset(kFirstDataCluster + usedClusters); }
    quint8 percentInUse() const
    {
        return geo.clusterCount ? quint8(qint64(usedClusters) * 100 / geo.clusterCount) : 0;
    }
};

// The automatic cluster size: always 64 KiB. ShadowMount+ 1.7 mounts exFAT
// images through its LVD type-5 / BFS fast path only with 64 KiB clusters
// (SMP 1.7 New-OsfExfatImage.ps1 and mkexfat.sh fix it at 65536). Earlier
// versions of this app picked 32 KiB when the average file was below 1 MiB.
inline constexpr int kAutoClusterBytes = 65536;
// The legacy automatic image size (Get-OptimalImageSizeBytesFromStats).
qint64 legacyImageBytes(const SourceTree& tree, int clusterBytes);

// Validates names (length, characters, case-insensitive duplicates), picks the
// cluster and image size and assigns clusters. No I/O.
Result<ImagePlan> planImage(const SourceTree& tree, const WriterOptions& options);

struct WriteStats {
    qint64 bytesWritten = 0; // file data
    qint64 filesWritten = 0;
    qint64 elapsedMs = 0;
};

// Writes the planned image to `path` (created or truncated). The caller owns
// the .part/rename protocol. Progress: bytes of file data and files.
Result<WriteStats> writeImage(const SourceTree& tree, const ImagePlan& plan, const QString& path,
                              const WriterOptions& options, const CancellationToken& token,
                              ProgressReporter* progress = nullptr, const Log* log = nullptr);

} // namespace fw::exfat
