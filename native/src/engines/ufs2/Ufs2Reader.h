#pragma once

// Read-only access to a UFS2 image (.ffpkg): validates the superblock, reads
// inodes, block maps, directories and file content. General UFS2, not only
// UFS2Tool's layout: fragments, holes and all three indirect levels are
// handled. Anything inconsistent fails with ErrorCode::CorruptData. All reads
// go through one mutex-protected file handle, so a reader may be shared by
// threads.

#include "core/Cancellation.h"
#include "core/RandomSource.h"
#include "core/Result.h"
#include "engines/ufs2/Ufs2Format.h"
#include "engines/ufs2/Ufs2Tree.h"

#include <QFile>
#include <QString>

#include <memory>
#include <mutex>
#include <vector>

namespace fw::ufs2 {

struct SuperblockInfo {
    qint32 magic = 0;
    int bsize = 0, fsize = 0, frag = 0;
    int sblkno = 0, cblkno = 0, iblkno = 0, dblkno = 0;
    int ncg = 0, ipg = 0, fpg = 0;
    qint64 size = 0;  // fragments
    qint64 dsize = 0;
    qint64 csaddr = 0;
    int cssize = 0, cgsize = 0, sbsize = 0;
    int bshift = 0, fshift = 0, fragshift = 0, fsbtodb = 0;
    int nindir = 0, inopb = 0;
    int minfree = 0, optim = 0, maxcontig = 0, maxbpg = 0, contigsumsize = 0, maxsymlinklen = 0;
    qint32 flags = 0;
    qint64 maxfilesize = 0;
    qint64 time = 0;
    qint64 sblockloc = 0;
    quint32 id[2] = {0, 0};
    qint64 csNdir = 0, csNbfree = 0, csNifree = 0, csNffree = 0;
    QString volname;
    QString fsmnt;

    qint64 bytes() const { return size * fsize; }
    int usableFrags(int cg) const { return cg == ncg - 1 ? int(size - qint64(cg) * fpg) : fpg; }
    quint32 inodeCount() const { return quint32(qint64(ncg) * ipg); }
};

// Parses and validates a superblock (`data` = SBLOCKSIZE bytes).
Result<SuperblockInfo> parseSuperblock(const char* data, qint64 imageBytes, qint64 location);

struct DirEntry {
    quint32 ino = 0;
    quint8 type = kDtUnknown;
    QByteArray rawName;
    QString name;
};

struct BlockRef {
    qint64 lbn = 0;
    qint64 frag = 0; // first fragment
    int nfrags = 0;
};

struct InodeBlocks {
    std::vector<BlockRef> data;     // allocated data blocks, in logical order
    std::vector<qint64> indirect;   // indirect pointer blocks (full blocks)
    qint64 holes = 0;               // logical blocks without storage
    int strayPointers = 0;          // pointers set beyond the end of the file
};

struct FileExtent {
    qint64 fileOffset = 0;
    qint64 imageOffset = -1; // -1 = hole (reads as zeros)
    qint64 length = 0;
};

class Ufs2Reader : public std::enable_shared_from_this<Ufs2Reader> {
public:
    static Result<std::shared_ptr<Ufs2Reader>> open(const QString& path);
    // An image that is not a file of its own (an .ffpkg inside an .ffpfsc);
    // path() is then the source's description.
    static Result<std::shared_ptr<Ufs2Reader>> open(std::shared_ptr<RandomSource> source);

    const SuperblockInfo& sb() const { return m_sb; }
    QString path() const { return m_path; }
    qint64 fileSize() const { return m_fileSize; }
    qint64 superblockOffset() const { return m_sbOffset; }

    Status readAt(qint64 offset, char* dst, qint64 len) const;

    Result<Inode> readInode(quint32 ino) const;
    // The block map of an inode. `token` (optional) is checked once per
    // indirect block. A map with more blocks than the image holds is refused.
    Result<InodeBlocks> blocks(const Inode& in, quint32 ino, const CancellationToken* token = nullptr) const;
    Result<std::vector<FileExtent>> extents(const Inode& in, quint32 ino) const;
    // `dirBytes` (optional) receives the size of the directory's data.
    Result<std::vector<DirEntry>> readDirectory(quint32 ino, bool includeDots = false,
                                                qint64* dirBytes = nullptr) const;
    Result<QByteArray> readLink(quint32 ino) const;

    // "/", "/a/b" (UTF-8 names). Fails with NotFound for missing paths.
    Result<quint32> resolve(const QString& path) const;
    Result<std::unique_ptr<ContentStream>> openFile(quint32 ino) const;

    // The whole tree (or the subtree at `fsPath`). Symbolic links and special
    // files cannot go into a SourceTree; their paths are added to `skipped`.
    Result<SourceTree> loadTree(const CancellationToken& token, QStringList* skipped = nullptr,
                                const QString& fsPath = QStringLiteral("/")) const;

    // Fails with CorruptData, prefixing the user message.
    Error corrupt(const QString& what) const;

private:
    Ufs2Reader() = default;
    static Result<std::shared_ptr<Ufs2Reader>> findSuperblock(std::shared_ptr<Ufs2Reader> r);
    Status walkIndirect(qint64 ptr, int level, qint64 firstLbn, qint64 nblocks, InodeBlocks& out, quint32 ino,
                        int depth, const CancellationToken* token) const;

    QString m_path;
    qint64 m_fileSize = 0;
    qint64 m_sbOffset = kSuperblockOffset;
    SuperblockInfo m_sb;
    mutable std::mutex m_mutex;
    mutable QFile m_file;
    std::shared_ptr<RandomSource> m_source; // instead of m_file
};

} // namespace fw::ufs2
