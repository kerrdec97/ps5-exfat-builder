#pragma once

// Read-only access to an exFAT image file: validates the boot region (main
// checksum, backup copy), the up-case table and the allocation bitmap, walks
// every directory checking entry-set checksums, name hashes, cluster ranges,
// FAT chains and cross-links, and streams file contents.
//
// Damage is reported as ErrorCode::CorruptData with a user-facing message
// ("The image is damaged: ..."); the technical reason goes to Error::detail.

#include "core/Cancellation.h"
#include "core/RandomSource.h"
#include "core/Result.h"
#include "engines/exfat/ExfatFormat.h"

#include <QFile>
#include <QHash>
#include <QStringList>

#include <memory>
#include <vector>

namespace fw::exfat {

struct Extent {
    quint32 cluster = 0; // first cluster of the run
    quint32 count = 0;   // clusters in the run
};

struct ImageEntry {
    QString name;
    QString path;          // '/' separated, relative to the root; empty for the root
    bool isDir = false;
    quint16 attributes = 0;
    qint64 size = 0;       // DataLength
    qint64 validSize = 0;  // ValidDataLength (bytes beyond it read as zero)
    quint32 firstCluster = 0;
    bool noFatChain = false;
    std::vector<Extent> extents;
    QDateTime created, modified, accessed;
    int parent = -1;
    std::vector<int> children;
    qint64 entryOffset = -1; // byte offset of the File entry in the image file
    qint64 entryIndex = -1;  // index of the File entry in the parent's directory data
    int entryCount = 0;      // entries in the set (File + secondaries)

    qint64 allocatedClusters() const
    {
        qint64 n = 0;
        for (const Extent& e : extents)
            n += e.count;
        return n;
    }
};

struct VolumeInfo {
    BootInfo boot;
    QString label;
    bool backupBootMatches = false;
    QString backupProblem;
    qint64 imageFileSize = 0;
    std::vector<Extent> rootExtents;
    std::vector<Extent> bitmapExtents;
    qint64 bitmapLength = 0;
    std::vector<Extent> upcaseExtents;
    qint64 upcaseLength = 0;
    bool upcaseIsRecommended = false;
    int activeFat = 0;
};

struct ImageTree {
    std::vector<ImageEntry> entries; // [0] is the root directory
    QHash<QString, int> index;       // exact path -> entry
    qint64 totalBytes = 0;
    qint64 fileCount = 0;
    qint64 dirCount = 0;
    qint64 usedClusters = 0;   // clusters the metadata and the tree occupy
    qint64 problemCount = 0;   // all problems, even beyond the listed ones
    QStringList problems;      // structural damage (first 50)
    QStringList warnings;      // harmless oddities (lost clusters, dirty flag)

    int find(const QString& path) const { return index.value(path, -1); }
    const ImageEntry& root() const { return entries.front(); }
};

class ImageFileStream;

class ExfatReader {
public:
    // Opens and validates the volume structures (boot region, root directory,
    // allocation bitmap, up-case table). Fails with CorruptData when damaged.
    static Result<std::unique_ptr<ExfatReader>> open(const QString& imagePath);
    // The same for an image that is not a file of its own (an .exfat inside an
    // .ffpfsc); imagePath() is then the source's description.
    static Result<std::unique_ptr<ExfatReader>> open(std::shared_ptr<RandomSource> source);
    // A content stream on the same image (file or source).
    std::unique_ptr<ImageFileStream> openStream() const;
    const std::shared_ptr<RandomSource>& source() const { return m_source; }

    const QString& imagePath() const { return m_path; }
    const VolumeInfo& volume() const { return m_vol; }
    const Geometry& geometry() const { return m_vol.boot.geo; }
    const UpcaseTable& upcase() const { return m_upcase; }

    // Walks every directory. Structural damage is collected in
    // ImageTree::problems; an error is returned only for I/O failure or
    // cancellation. With checkAllocation the allocation bitmap is compared
    // with the clusters the tree really uses.
    Result<ImageTree> readTree(const CancellationToken& token, bool checkAllocation = true);

    // Fails with CorruptData (listing the first problems) when `tree` has any.
    static Status requireIntact(const ImageTree& tree);

    // Reads the whole allocation bitmap.
    Result<QByteArray> readBitmap();

private:
    ExfatReader() = default;
    Status openImpl(const QString& path);
    Status openVolume();
    QIODevice* io() { return m_dev ? static_cast<QIODevice*>(m_dev.get()) : &m_file; }
    Result<quint32> fatEntry(quint32 cluster);
    // Follows a FAT chain. `expected` < 0: until end of chain (a folder
    // without a length of its own, so at most the largest folder). Returns
    // false with an empty `problem` when `token` was cancelled.
    bool followChain(quint32 first, qint64 expected, std::vector<Extent>& out, QString& problem,
                     const CancellationToken* token = nullptr);
    Result<QByteArray> readExtents(const std::vector<Extent>& extents, qint64 bytes);

    QString m_path;
    QFile m_file;
    std::shared_ptr<RandomSource> m_source;
    std::unique_ptr<SourceDevice> m_dev;
    VolumeInfo m_vol;
    UpcaseTable m_upcase;
    QHash<qint64, QByteArray> m_fatCache;
};

// Independent read handle on an image, so several threads can read files at
// once (one stream per thread).
class ImageFileStream {
public:
    ImageFileStream(const QString& imagePath, const Geometry& geo);
    ImageFileStream(std::shared_ptr<RandomSource> source, const Geometry& geo);
    Status open();
    void close()
    {
        m_file.close();
        m_dev.reset();
    }
    // Reads up to `len` bytes of `e` from `offset`. Returns the bytes read
    // (0 at the end of the file). Bytes past ValidDataLength read as zero.
    Result<qint64> read(const ImageEntry& e, qint64 offset, char* buf, qint64 len);

private:
    QIODevice* io() { return m_dev ? static_cast<QIODevice*>(m_dev.get()) : &m_file; }

    QString m_path;
    Geometry m_geo;
    QFile m_file;
    std::shared_ptr<RandomSource> m_source;
    std::unique_ptr<SourceDevice> m_dev;
};

} // namespace fw::exfat
