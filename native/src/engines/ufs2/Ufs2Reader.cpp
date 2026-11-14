#include "engines/ufs2/Ufs2Reader.h"

#include "core/FileUtil.h"

#include <QDir>
#include <QFileInfo>

#include <algorithm>
#include <cstring>
#include <limits>
#include <optional>
#include <set>
#include <utility>

namespace fw::ufs2 {

namespace {

constexpr qint32 kUfs1Magic = 0x00011954;
constexpr qint64 kMaxDirectoryBytes = 256LL * 1024 * 1024;
// Limits for images that were not written here: deeper or larger than any
// real game folder, small enough that a damaged image cannot exhaust memory.
constexpr int kMaxFolderDepth = 256;
constexpr size_t kMaxTreeNodes = 10'000'000;

Error corruptError(const QString& path, const QString& what)
{
    return Error(ErrorCode::CorruptData,
                 QStringLiteral("%1 is damaged or is not an FFPKG (UFS2) image: %2")
                     .arg(QFileInfo(path).fileName(), what),
                 path);
}

QString cString(const char* p, int max)
{
    int n = 0;
    while (n < max && p[n] != 0)
        ++n;
    return QString::fromUtf8(p, n);
}

} // namespace

Result<SuperblockInfo> parseSuperblock(const char* d, qint64 imageBytes, qint64 location)
{
    auto bad = [](const QString& what) {
        return Error(ErrorCode::CorruptData, QStringLiteral("bad superblock (%1).").arg(what));
    };
    SuperblockInfo s;
    s.magic = rd32s(d + 0x55C);
    if (s.magic == kUfs1Magic)
        return Error(ErrorCode::Unsupported, QStringLiteral("UFS1 images are not supported, only UFS2 (.ffpkg)."));
    if (s.magic != kUfs2Magic)
        return bad(QStringLiteral("no UFS2 signature"));
    if (rd64(d + 0x3E8) != location)
        return bad(QStringLiteral("recorded location %1, found at %2").arg(rd64(d + 0x3E8)).arg(location));
    s.sblkno = rd32s(d + 0x008);
    s.cblkno = rd32s(d + 0x00C);
    s.iblkno = rd32s(d + 0x010);
    s.dblkno = rd32s(d + 0x014);
    s.ncg = rd32s(d + 0x02C);
    s.bsize = rd32s(d + 0x030);
    s.fsize = rd32s(d + 0x034);
    s.frag = rd32s(d + 0x038);
    s.minfree = rd32s(d + 0x03C);
    s.bshift = rd32s(d + 0x050);
    s.fshift = rd32s(d + 0x054);
    s.maxcontig = rd32s(d + 0x058);
    s.maxbpg = rd32s(d + 0x05C);
    s.fragshift = rd32s(d + 0x060);
    s.fsbtodb = rd32s(d + 0x064);
    s.sbsize = rd32s(d + 0x068);
    s.nindir = rd32s(d + 0x074);
    s.inopb = rd32s(d + 0x078);
    s.optim = rd32s(d + 0x080);
    s.id[0] = rd32(d + 0x090);
    s.id[1] = rd32(d + 0x094);
    s.cssize = rd32s(d + 0x09C);
    s.cgsize = rd32s(d + 0x0A0);
    s.ipg = rd32s(d + 0x0B8);
    s.fpg = rd32s(d + 0x0BC);
    s.fsmnt = cString(d + 0x0D4, 468);
    s.volname = cString(d + 0x2A8, 32);
    s.sblockloc = rd64(d + 0x3E8);
    s.csNdir = rd64(d + 0x3F0);
    s.csNbfree = rd64(d + 0x3F8);
    s.csNifree = rd64(d + 0x400);
    s.csNffree = rd64(d + 0x408);
    s.time = rd64(d + 0x430);
    s.size = rd64(d + 0x438);
    s.dsize = rd64(d + 0x440);
    s.csaddr = rd64(d + 0x448);
    s.flags = rd32s(d + 0x520);
    s.contigsumsize = rd32s(d + 0x524);
    s.maxsymlinklen = rd32s(d + 0x528);
    s.maxfilesize = rd64(d + 0x530);

    if (!isPowerOfTwo(s.bsize) || s.bsize < 4096 || s.bsize > kMaxBSize)
        return bad(QStringLiteral("block size %1").arg(s.bsize));
    if (!isPowerOfTwo(s.fsize) || s.fsize < 512 || s.fsize > s.bsize)
        return bad(QStringLiteral("fragment size %1").arg(s.fsize));
    if (s.frag != s.bsize / s.fsize || s.frag > kMaxFrag)
        return bad(QStringLiteral("fragments per block %1").arg(s.frag));
    if (s.bshift != log2Int(s.bsize) || s.fshift != log2Int(s.fsize) || s.fragshift != log2Int(s.frag))
        return bad(QStringLiteral("shift values"));
    if (s.sbsize <= 0 || s.sbsize > kSuperblockSize)
        return bad(QStringLiteral("superblock size %1").arg(s.sbsize));
    if (s.nindir != s.bsize / 8 || s.inopb != s.bsize / kInodeSize)
        return bad(QStringLiteral("pointers or inodes per block"));
    if (s.ncg < 1 || s.ipg < 1 || s.fpg < s.frag || s.fpg % s.frag != 0)
        return bad(QStringLiteral("cylinder group layout"));
    if (qint64(s.ncg) * s.ipg > qint64(0xFFFFFFFFu))
        return bad(QStringLiteral("inode count"));
    if (s.size <= 0 || s.size > qint64(s.ncg) * s.fpg || s.size <= qint64(s.ncg - 1) * s.fpg)
        return bad(QStringLiteral("file system size %1 for %2 groups").arg(s.size).arg(s.ncg));
    // The byte size must fit 63 bits, or the comparison with the file below means nothing.
    if (s.size > std::numeric_limits<qint64>::max() / s.fsize)
        return bad(QStringLiteral("file system size %1").arg(s.size));
    const qint64 inodeFrags = ceilDiv(s.ipg, s.inopb) * s.frag;
    if (s.sblkno < 0 || s.cblkno <= s.sblkno || s.iblkno <= s.cblkno || s.iblkno + inodeFrags > s.dblkno
        || s.dblkno > s.fpg)
        return bad(QStringLiteral("cylinder group offsets"));
    if (qint64(s.sblkno) * s.fsize < location + s.sbsize)
        return bad(QStringLiteral("cylinder group metadata overlaps the superblock"));
    if (s.cgsize < kCgHeaderBase || s.cgsize > (s.iblkno - s.cblkno) * qint64(s.fsize) || s.cgsize > s.bsize * 2)
        return bad(QStringLiteral("cylinder group size %1").arg(s.cgsize));
    if (s.cssize < qint64(s.ncg) * kCsumSize || s.csaddr < 0 || s.csaddr > s.size
        || s.csaddr + ceilDiv(s.cssize, s.fsize) > s.size)
        return bad(QStringLiteral("summary area"));
    if (s.maxfilesize <= 0 || s.maxsymlinklen < 0 || s.maxsymlinklen > (kNDirect + kNIndirect) * 8)
        return bad(QStringLiteral("file size limits"));
    if (imageBytes >= 0 && imageBytes < s.bytes())
        return Error(ErrorCode::CorruptData,
                     QStringLiteral("the image is incomplete (%1 of %2 bytes). It was probably not copied or built completely.")
                         .arg(imageBytes)
                         .arg(s.bytes()));
    return s;
}

Error Ufs2Reader::corrupt(const QString& what) const { return corruptError(m_path, what); }

Result<std::shared_ptr<Ufs2Reader>> Ufs2Reader::open(const QString& path)
{
    std::shared_ptr<Ufs2Reader> r(new Ufs2Reader());
    r->m_path = QFileInfo(path).absoluteFilePath();
    r->m_file.setFileName(toLongPath(r->m_path));
    if (!QFileInfo::exists(r->m_path))
        return Error(ErrorCode::NotFound, QStringLiteral("%1 does not exist.").arg(QDir::toNativeSeparators(path)));
    if (!r->m_file.open(QIODevice::ReadOnly))
        return Error(ErrorCode::IoError, QStringLiteral("Could not open %1.").arg(QDir::toNativeSeparators(path)),
                     r->m_file.errorString());
    r->m_fileSize = r->m_file.size();
    return findSuperblock(std::move(r));
}

Result<std::shared_ptr<Ufs2Reader>> Ufs2Reader::open(std::shared_ptr<RandomSource> source)
{
    std::shared_ptr<Ufs2Reader> r(new Ufs2Reader());
    r->m_path = source->describe();
    r->m_fileSize = source->size();
    r->m_source = std::move(source);
    return findSuperblock(std::move(r));
}

Result<std::shared_ptr<Ufs2Reader>> Ufs2Reader::findSuperblock(std::shared_ptr<Ufs2Reader> r)
{
    if (r->m_fileSize < kSuperblockOffset + kSuperblockSize)
        return corruptError(r->m_path, QStringLiteral("the file is too small to hold a UFS2 superblock."));
    // FreeBSD's search order (SBLOCKSEARCH without the floppy location). PS5
    // images and UFS2Tool use 65536; some makefs builds put UFS2 at 8192.
    std::optional<Error> primaryError;
    std::optional<Error> signedError;
    std::optional<Error> unsupported; // a recognised file system this app cannot read (UFS1)
    for (const qint64 loc : {kSuperblockOffset, qint64(8192), qint64(262144)}) {
        if (r->m_fileSize < loc + kSuperblockSize)
            continue;
        QByteArray sbuf(kSuperblockSize, '\0');
        FW_TRY(r->readAt(loc, sbuf.data(), kSuperblockSize));
        auto sb = parseSuperblock(sbuf.constData(), r->m_fileSize, loc);
        if (sb.ok()) {
            r->m_sb = sb.value();
            r->m_sbOffset = loc;
            return r;
        }
        Error e = sb.takeError();
        if (e.code == ErrorCode::Unsupported && !unsupported)
            unsupported = e;
        if (loc == kSuperblockOffset)
            primaryError = e;
        else if (!signedError && rd32s(sbuf.constData() + 0x55C) == kUfs2Magic)
            signedError = e;
    }
    // UFS1 keeps its superblock at 8192, so the primary location only says "no signature".
    if (unsupported)
        return *unsupported;
    // Explain the primary location, unless only another location has a signature.
    // The first location is always read (the size check above), so this is
    // set; the fallback only keeps that obvious to a reader and to analysers.
    Error e = primaryError.value_or(Error(ErrorCode::CorruptData, QStringLiteral("bad superblock (no UFS2 signature).")));
    if (signedError && e.message.contains(QLatin1String("no UFS2 signature")))
        e = *signedError;
    if (e.code == ErrorCode::CorruptData)
        return corruptError(r->m_path, e.message);
    return e;
}

Status Ufs2Reader::readAt(qint64 offset, char* dst, qint64 len) const
{
    if (offset < 0 || len < 0 || offset + len > m_fileSize)
        return corrupt(QStringLiteral("a read at byte %1 (%2 bytes) is outside the image.").arg(offset).arg(len));
    if (m_source)
        return m_source->readAt(offset, dst, len);
    std::lock_guard lock(m_mutex);
    if (!m_file.seek(offset))
        return Error(ErrorCode::IoError, QStringLiteral("Could not read %1.").arg(QFileInfo(m_path).fileName()),
                     m_file.errorString());
    qint64 done = 0;
    while (done < len) {
        const qint64 n = m_file.read(dst + done, len - done);
        if (n <= 0)
            return Error(ErrorCode::IoError, QStringLiteral("Could not read %1.").arg(QFileInfo(m_path).fileName()),
                         m_file.errorString());
        done += n;
    }
    return success();
}

Result<Inode> Ufs2Reader::readInode(quint32 ino) const
{
    if (ino >= m_sb.inodeCount())
        return corrupt(QStringLiteral("inode %1 is out of range.").arg(ino));
    const qint64 cg = ino / quint32(m_sb.ipg);
    const qint64 idx = ino % quint32(m_sb.ipg);
    const qint64 off = (cg * m_sb.fpg + m_sb.iblkno) * qint64(m_sb.fsize) + idx * kInodeSize;
    char buf[kInodeSize];
    FW_TRY(readAt(off, buf, kInodeSize));
    return parseInode(buf);
}

Status Ufs2Reader::walkIndirect(qint64 ptr, int level, qint64 firstLbn, qint64 nblocks, InodeBlocks& out,
                                quint32 ino, int depth, const CancellationToken* token) const
{
    qint64 span = 1; // logical blocks covered by one pointer of this block
    for (int i = 1; i < level; ++i)
        span *= m_sb.nindir;
    if (ptr == 0) {
        const qint64 covered = std::min<qint64>(span * m_sb.nindir, nblocks - firstLbn);
        out.holes += std::max<qint64>(0, covered);
        return success();
    }
    if (ptr < 0 || ptr % m_sb.frag != 0 || ptr + m_sb.frag > m_sb.size)
        return corrupt(QStringLiteral("inode %1 has an invalid indirect block %2.").arg(ino).arg(ptr));
    if (depth > 3)
        return corrupt(QStringLiteral("inode %1 has too many indirect levels.").arg(ino));
    // Every block of a sound map is a block of its own. Pointers that lead
    // back to the same blocks would otherwise multiply without end.
    if (qint64(out.data.size() + out.indirect.size()) > m_sb.size / m_sb.frag + kNDirect)
        return corrupt(QStringLiteral("inode %1 maps more blocks than the image holds.").arg(ino));
    if (token && !token->checkpoint())
        return Error::cancelled();
    out.indirect.push_back(ptr);
    QByteArray blk(m_sb.bsize, '\0');
    FW_TRY(readAt(ptr * m_sb.fsize, blk.data(), m_sb.bsize));
    for (int i = 0; i < m_sb.nindir; ++i) {
        const qint64 childFirst = firstLbn + qint64(i) * span;
        const qint64 p = rd64(blk.constData() + 8 * i);
        if (childFirst >= nblocks) {
            if (p != 0)
                ++out.strayPointers;
            continue;
        }
        if (level == 1) {
            if (p == 0) {
                ++out.holes;
                continue;
            }
            if (p < 0 || p % m_sb.frag != 0 || p + m_sb.frag > m_sb.size)
                return corrupt(QStringLiteral("inode %1 has an invalid block pointer %2.").arg(ino).arg(p));
            out.data.push_back({childFirst, p, m_sb.frag});
        } else {
            FW_TRY(walkIndirect(p, level - 1, childFirst, nblocks, out, ino, depth + 1, token));
        }
    }
    return success();
}

Result<InodeBlocks> Ufs2Reader::blocks(const Inode& in, quint32 ino, const CancellationToken* token) const
{
    InodeBlocks out;
    if (in.size < 0 || in.size > m_sb.maxfilesize)
        return corrupt(QStringLiteral("inode %1 has an invalid size %2.").arg(ino).arg(in.size));
    // Short symbolic links keep their target inside the inode.
    if (in.isLink() && in.size < m_sb.maxsymlinklen && in.blocks == 0)
        return out;
    const qint64 nblocks = ceilDiv(in.size, m_sb.bsize);
    for (int i = 0; i < kNDirect; ++i) {
        const qint64 p = in.db[i];
        if (i >= nblocks) {
            if (p != 0)
                ++out.strayPointers;
            continue;
        }
        if (p == 0) {
            ++out.holes;
            continue;
        }
        int nfrags = m_sb.frag;
        if (qint64(i + 1) * m_sb.bsize > in.size)
            nfrags = int(ceilDiv(in.size - qint64(i) * m_sb.bsize, m_sb.fsize));
        if (p < 0 || p + nfrags > m_sb.size || (p % m_sb.frag) + nfrags > m_sb.frag)
            return corrupt(QStringLiteral("inode %1 has an invalid block pointer %2.").arg(ino).arg(p));
        out.data.push_back({i, p, nfrags});
    }
    qint64 first = kNDirect;
    qint64 span = m_sb.nindir;
    for (int level = 1; level <= kNIndirect; ++level) {
        const qint64 p = in.ib[level - 1];
        if (first >= nblocks) {
            if (p != 0)
                ++out.strayPointers;
        } else {
            FW_TRY(walkIndirect(p, level, first, nblocks, out, ino, 1, token));
        }
        first += span;
        span *= m_sb.nindir;
    }
    return out;
}

Result<std::vector<FileExtent>> Ufs2Reader::extents(const Inode& in, quint32 ino) const
{
    FW_TRY_ASSIGN(InodeBlocks b, blocks(in, ino));
    std::vector<FileExtent> out;
    qint64 pos = 0;
    auto addHole = [&](qint64 until) {
        if (until <= pos)
            return;
        if (!out.empty() && out.back().imageOffset < 0)
            out.back().length += until - pos;
        else
            out.push_back({pos, -1, until - pos});
        pos = until;
    };
    for (const BlockRef& r : b.data) {
        const qint64 off = r.lbn * m_sb.bsize;
        addHole(off);
        const qint64 len = std::min<qint64>(qint64(r.nfrags) * m_sb.fsize, in.size - off);
        const qint64 img = r.frag * m_sb.fsize;
        if (!out.empty() && out.back().imageOffset >= 0 && out.back().fileOffset + out.back().length == off
            && out.back().imageOffset + out.back().length == img)
            out.back().length += len;
        else
            out.push_back({off, img, len});
        pos = off + len;
    }
    addHole(in.size);
    return out;
}

Result<std::vector<DirEntry>> Ufs2Reader::readDirectory(quint32 ino, bool includeDots, qint64* dirBytes) const
{
    FW_TRY_ASSIGN(Inode in, readInode(ino));
    if (!in.isDir())
        return corrupt(QStringLiteral("inode %1 is not a directory.").arg(ino));
    // A directory has no holes, so it cannot be larger than the image.
    if (in.size <= 0 || in.size % kDirBlockSize != 0 || in.size > kMaxDirectoryBytes || in.size > m_fileSize)
        return corrupt(QStringLiteral("directory inode %1 has an invalid size %2.").arg(ino).arg(in.size));
    if (dirBytes)
        *dirBytes = in.size;
    FW_TRY_ASSIGN(std::vector<FileExtent> ext, extents(in, ino));
    QByteArray data(in.size, '\0');
    for (const FileExtent& e : ext) {
        if (e.imageOffset < 0)
            return corrupt(QStringLiteral("directory inode %1 has a hole.").arg(ino));
        FW_TRY(readAt(e.imageOffset, data.data() + e.fileOffset, e.length));
    }
    std::vector<DirEntry> out;
    const quint32 maxIno = m_sb.inodeCount();
    for (qint64 chunk = 0; chunk < in.size; chunk += kDirBlockSize) {
        int off = 0;
        while (off < kDirBlockSize) {
            const char* p = data.constData() + chunk + off;
            const quint32 eino = rd32(p);
            const int reclen = rd16(p + 4);
            const quint8 type = quint8(p[6]);
            const int namlen = quint8(p[7]);
            const int maxsize = kDirBlockSize - off;
            if (reclen == 0 || (reclen & 3) != 0 || reclen > maxsize || reclen < direntSize(namlen))
                return corrupt(QStringLiteral("directory inode %1 has a bad entry at byte %2.").arg(ino).arg(chunk + off));
            if (eino != 0) {
                if (eino >= maxIno || namlen == 0)
                    return corrupt(QStringLiteral("directory inode %1 has a bad entry at byte %2.").arg(ino).arg(chunk + off));
                QByteArray raw(p + 8, namlen);
                if (raw.contains('/') || raw.contains('\0') || p[8 + namlen] != 0)
                    return corrupt(QStringLiteral("directory inode %1 has a bad name at byte %2.").arg(ino).arg(chunk + off));
                const bool isDot = raw == "." || raw == "..";
                if (includeDots || !isDot) {
                    DirEntry e;
                    e.ino = eino;
                    e.type = type;
                    e.name = QString::fromUtf8(raw);
                    e.rawName = std::move(raw);
                    out.push_back(std::move(e));
                }
            }
            off += reclen;
        }
    }
    return out;
}

Result<QByteArray> Ufs2Reader::readLink(quint32 ino) const
{
    FW_TRY_ASSIGN(Inode in, readInode(ino));
    if (!in.isLink())
        return corrupt(QStringLiteral("inode %1 is not a symbolic link.").arg(ino));
    if (in.size < 0)
        return corrupt(QStringLiteral("symbolic link inode %1 has an invalid size %2.").arg(ino).arg(in.size));
    if (in.size < m_sb.maxsymlinklen && in.blocks == 0) {
        char raw[kNDirect * 8 + kNIndirect * 8];
        for (int i = 0; i < kNDirect; ++i)
            wr64(raw + 8 * i, in.db[i]);
        for (int i = 0; i < kNIndirect; ++i)
            wr64(raw + kNDirect * 8 + 8 * i, in.ib[i]);
        return QByteArray(raw, int(in.size));
    }
    if (in.size > 4096)
        return corrupt(QStringLiteral("symbolic link inode %1 is too long.").arg(ino));
    FW_TRY_ASSIGN(std::vector<FileExtent> ext, extents(in, ino));
    QByteArray out(in.size, '\0');
    for (const FileExtent& e : ext)
        if (e.imageOffset >= 0)
            FW_TRY(readAt(e.imageOffset, out.data() + e.fileOffset, e.length));
    return out;
}

Result<quint32> Ufs2Reader::resolve(const QString& path) const
{
    quint32 cur = kRootIno;
    const QStringList parts = path.split(u'/', Qt::SkipEmptyParts);
    for (const QString& part : parts) {
        const QByteArray want = part.toUtf8();
        FW_TRY_ASSIGN(std::vector<DirEntry> entries, readDirectory(cur));
        bool found = false;
        for (const DirEntry& e : entries) {
            if (e.rawName == want) {
                cur = e.ino;
                found = true;
                break;
            }
        }
        if (!found)
            return Error(ErrorCode::NotFound, QStringLiteral("%1 is not in the image.").arg(path));
        if (&part != &parts.back()) {
            FW_TRY_ASSIGN(Inode in, readInode(cur));
            if (!in.isDir())
                return Error(ErrorCode::NotFound, QStringLiteral("%1 is not in the image.").arg(path));
        }
    }
    return cur;
}

namespace {

class ImageFileStream final : public ContentStream {
public:
    ImageFileStream(const Ufs2Reader* r, std::vector<FileExtent> ext) : m_r(r), m_ext(std::move(ext)) {}
    Result<qint64> read(char* dst, qint64 max) override
    {
        qint64 done = 0;
        while (done < max && m_i < m_ext.size()) {
            const FileExtent& e = m_ext[m_i];
            const qint64 n = std::min(max - done, e.length - m_off);
            if (e.imageOffset < 0)
                std::memset(dst + done, 0, size_t(n));
            else
                FW_TRY(m_r->readAt(e.imageOffset + m_off, dst + done, n));
            done += n;
            m_off += n;
            if (m_off == e.length) {
                ++m_i;
                m_off = 0;
            }
        }
        return done;
    }

private:
    const Ufs2Reader* m_r;
    std::vector<FileExtent> m_ext;
    size_t m_i = 0;
    qint64 m_off = 0;
};

} // namespace

Result<std::unique_ptr<ContentStream>> Ufs2Reader::openFile(quint32 ino) const
{
    FW_TRY_ASSIGN(Inode in, readInode(ino));
    if (!in.isReg())
        return corrupt(QStringLiteral("inode %1 is not a regular file.").arg(ino));
    FW_TRY_ASSIGN(std::vector<FileExtent> ext, extents(in, ino));
    return std::unique_ptr<ContentStream>(new ImageFileStream(this, std::move(ext)));
}

Result<SourceTree> Ufs2Reader::loadTree(const CancellationToken& token, QStringList* skipped, const QString& fsPath) const
{
    FW_TRY_ASSIGN(quint32 top, resolve(fsPath));
    FW_TRY_ASSIGN(Inode topInode, readInode(top));
    if (!topInode.isDir())
        return Error(ErrorCode::InvalidArgument, QStringLiteral("%1 is not a folder in the image.").arg(fsPath));
    SourceTree tree;
    tree.rootPath = m_path;
    tree.image = shared_from_this();
    TreeNode root;
    root.kind = NodeKind::Dir;
    root.imageIno = top;
    root.mtime = topInode.mtime;
    tree.nodes.push_back(root);
    std::set<quint32> visited{top};
    std::vector<std::pair<int, int>> stack{{0, 0}}; // node, folder levels above it
    qint64 n = 0;
    qint64 dirBytesTotal = 0;
    while (!stack.empty()) {
        const auto [dirNode, depth] = stack.back();
        stack.pop_back();
        if (!token.checkpoint())
            return Error::cancelled();
        qint64 dirBytes = 0;
        FW_TRY_ASSIGN(std::vector<DirEntry> entries,
                      readDirectory(tree.nodes[size_t(dirNode)].imageIno, false, &dirBytes));
        // Folders of a sound image do not share blocks: together they are
        // smaller than the image. Shared blocks would list the same entries
        // again for every folder.
        dirBytesTotal += dirBytes;
        if (dirBytesTotal > m_fileSize)
            return corrupt(QStringLiteral("its folders take more space than the whole image."));
        for (DirEntry& e : entries) {
            if ((++n & 0xFF) == 0 && !token.checkpoint())
                return Error::cancelled();
            FW_TRY_ASSIGN(Inode in, readInode(e.ino));
            TreeNode node;
            node.name = e.name;
            node.rawName = e.rawName;
            node.parent = dirNode;
            node.imageIno = e.ino;
            node.mtime = in.mtime;
            if (in.isDir()) {
                if (!visited.insert(e.ino).second)
                    return corrupt(QStringLiteral("directory inode %1 is linked more than once.").arg(e.ino));
                node.kind = NodeKind::Dir;
            } else if (in.isReg()) {
                node.kind = NodeKind::File;
                node.size = in.size;
            } else {
                if (skipped) {
                    QString p = tree.relativePath(dirNode);
                    skipped->append(p.isEmpty() ? e.name : p + u'/' + e.name);
                }
                continue;
            }
            if (tree.nodes.size() >= kMaxTreeNodes)
                return corrupt(QStringLiteral("it holds more than %1 files and folders.")
                                   .arg(formatCount(qint64(kMaxTreeNodes))));
            const int idx = int(tree.nodes.size());
            const bool isDir = node.isDir();
            if (isDir && depth + 1 > kMaxFolderDepth)
                return corrupt(QStringLiteral("its folders are nested more than %1 deep.").arg(kMaxFolderDepth));
            tree.nodes.push_back(std::move(node));
            tree.nodes[size_t(dirNode)].children.push_back(idx);
            if (isDir)
                stack.push_back({idx, depth + 1});
        }
    }
    tree.recount();
    return tree;
}

} // namespace fw::ufs2
