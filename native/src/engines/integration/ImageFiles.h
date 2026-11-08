#pragma once

// Read-only, format-neutral access to the files of an image or a folder: the
// tree (paths and sizes) and random-access reads of file content, through the
// native readers:
//
//   .exfat            ExfatReader (tree) + ImageFileStream (content)
//   .ffpkg            Ufs2Reader (tree, inode extents, readAt)
//   .ffpfs / .ffpfsc  PfsImage; a container holding one nested PFS image is
//                     opened through to the nested image (as "deep" extraction
//                     does), PFSC blocks are decompressed on the fly
//   a folder          the files on disk (hidden and system files included,
//                     links followed, loops skipped)
//
// Used by image.compare and by the Image tools page's hex preview. A FileTree
// keeps its image open while it lives and must be used by one thread at a
// time; callers that are not jobs (the preview) open it per request and close
// it right away, so no handle stays on an image a queued edit may replace.

#include "core/Cancellation.h"
#include "core/Result.h"

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>

#include <memory>
#include <vector>

namespace fw::integration {

enum class TreeFormat { Folder, Exfat, Ffpkg, Pfs };

struct TreeItem {
    QString path;        // '/'-separated, relative to the root, never empty
    bool isDir = false;
    qint64 size = 0;     // files: logical size in bytes
    // PFSC-compressed files of a PFS image store their size rounded up to the
    // image's block size, padded with zero bytes (Lazy_MkPFS build_pfs). This
    // is that block size for such files, 0 otherwise.
    qint64 padBlock = 0;
    qint64 handle = -1;  // reader-specific (entry index, inode number)
};

class FileTree {
public:
    virtual ~FileTree() = default;

    TreeFormat format() const { return m_format; }
    const QString& rootPath() const { return m_root; }
    QString describe() const; // "exFAT image", "FFPKG (UFS2) image", "FFPFSC image", "folder"
    const std::vector<TreeItem>& items() const { return m_items; } // sorted by path
    int find(const QString& path) const;                          // -1 when absent
    const QStringList& warnings() const { return m_warnings; }
    // A PFS container around one image: that image's file name ("" otherwise).
    const QString& innerImage() const { return m_innerImage; }
    void setInnerImage(const QString& name) { m_innerImage = name; }
    qint64 fileCount() const { return m_files; }
    qint64 dirCount() const { return m_dirs; }
    qint64 totalBytes() const { return m_bytes; }

    // Reads up to `len` bytes of file `index` from `offset`. Returns the bytes
    // read: fewer than `len` only at the end of the file, 0 at or past it.
    virtual Result<qint64> read(int index, qint64 offset, char* buf, qint64 len) = 0;

protected:
    FileTree(TreeFormat format, QString root) : m_format(format), m_root(std::move(root)) {}
    // Sorts the items, builds the index and the totals.
    void finish();

    TreeFormat m_format;
    QString m_root;
    QString m_formatName; // overrides describe() when set
    QString m_innerImage;
    std::vector<TreeItem> m_items;
    QHash<QString, int> m_index;
    QStringList m_warnings;
    qint64 m_files = 0, m_dirs = 0, m_bytes = 0;
};

// True for the image extensions openFileTree reads (.exfat .ffpkg .ffpfs .ffpfsc).
bool isTreeImage(const QString& path);

// Opens an image (by extension) or a folder. Fails with NotFound when the
// path does not exist, Unsupported for other files, CorruptData when the image
// structure is damaged beyond reading.
Result<std::unique_ptr<FileTree>> openFileTree(const QString& path, const CancellationToken& token);

// One slice of one file, for previews.
struct FileSlice {
    QString path;
    qint64 fileSize = 0;
    qint64 offset = 0;
    QByteArray data; // at most the requested length; empty at the end of the file
};

// Reads at most `length` bytes of the file at `path` (image path, '/'
// separated, leading slash optional) from `offset`.
Result<FileSlice> readSlice(FileTree& tree, const QString& path, qint64 offset, qint64 length);

// Opens `imageOrFolder`, reads the slice and closes it again.
Result<FileSlice> readFileSlice(const QString& imageOrFolder, const QString& path, qint64 offset, qint64 length,
                                const CancellationToken& token = {});

} // namespace fw::integration
