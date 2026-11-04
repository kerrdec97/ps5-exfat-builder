#pragma once

// The file tree that goes into (or comes out of) a UFS2 image.
//
// A tree is scanned from a folder on disk (scanLocalFolder) or loaded from an
// existing image (Ufs2Reader::loadTree). Children keep the order in which the
// directory listing returned them: UFS2Tool assigns inode numbers and blocks
// in that order, so keeping it is what makes the images byte-identical (on
// NTFS the listing is sorted by the volume's upper-case collation, elsewhere it
// is the file system's own order). File content comes either from a file on
// disk or from an inode of another image (used by the editor's rebuild).

#include "core/Cancellation.h"
#include "core/Result.h"

#include <QByteArray>
#include <QString>

#include <memory>
#include <vector>

namespace fw::ufs2 {

class Ufs2Reader;

enum class NodeKind : quint8 { Dir, File };

struct TreeNode {
    QString name;          // as shown to the user
    QByteArray rawName;    // bytes stored in the directory entry (UTF-8)
    NodeKind kind = NodeKind::File;
    qint64 size = 0;       // files only
    qint64 mtime = 0;      // seconds since the epoch (informational)
    int parent = -1;
    std::vector<int> children; // directory order
    QString localPath;     // content source on disk (absolute), or
    quint32 imageIno = 0;  // content source in SourceTree::image
    bool removed = false;  // dropped by an edit (kept so indices stay valid)

    bool isDir() const { return kind == NodeKind::Dir; }
};

struct SourceTree {
    std::vector<TreeNode> nodes;                // nodes[0] is the root
    std::shared_ptr<const Ufs2Reader> image;    // content of nodes with imageIno
    QString rootPath;                           // folder or image it came from
    qint64 totalBytes = 0;                      // file bytes (excluding removed nodes)
    qint64 fileCount = 0;
    qint64 dirCount = 0;                        // excluding the root

    // "a/b/c" for node `idx` (empty for the root).
    QString relativePath(int idx) const;
    // Node for "a/b" or "/a/b" (UTF-8 byte comparison per component), or -1.
    int find(const QString& path) const;
    int findChild(int dir, const QByteArray& rawName) const;
    // Live children of `dir` (removed nodes skipped), in order.
    std::vector<int> liveChildren(int dir) const;
    // Recomputes totals after edits.
    void recount();
};

// Encodes a file name for a directory entry; fails for names over 255 bytes or
// names UFS2 cannot store ("", ".", "..", containing '/' or NUL).
Result<QByteArray> encodeName(const QString& name);

// Scans `root` (which becomes the image's root directory). Hidden and system
// files are included, as UFS2Tool does. Symbolic links and junctions are
// followed; a link that loops back into its own ancestors is an error.
Result<SourceTree> scanLocalFolder(const QString& root, const CancellationToken& token);

// Appends the contents of local folder `dir` below tree node `parent`.
Status appendLocalFolder(SourceTree& tree, int parent, const QString& dir, const CancellationToken& token);

// Sequential reader for one file's content.
class ContentStream {
public:
    virtual ~ContentStream() = default;
    // Reads up to `max` bytes; returns 0 at the end.
    virtual Result<qint64> read(char* dst, qint64 max) = 0;
};

Result<std::unique_ptr<ContentStream>> openContent(const SourceTree& tree, int node);

} // namespace fw::ufs2
