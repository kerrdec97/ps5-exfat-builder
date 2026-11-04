// UFS2 format helpers. Geometry, directory packing and the seeded generator
// are ported from UFS2Tool 4.0.0 (c123639), Copyright (c) 2026, SvenGDK,
// BSD-2-Clause (LICENSES/UFS2Tool-BSD-2-Clause.txt). Function names in the
// comments refer to UFS2Tool's Ufs2ImageCreator.cs.

#include "engines/ufs2/Ufs2Format.h"

#include <algorithm>
#include <climits>
#include <cstring>

namespace fw::ufs2 {

int log2Int(qint64 v)
{
    int r = 0;
    while (v > 1) {
        v >>= 1;
        ++r;
    }
    return r;
}

bool isPowerOfTwo(qint64 v) { return v > 0 && (v & (v - 1)) == 0; }

static Error badParam(const QString& what)
{
    return Error(ErrorCode::InvalidArgument, QStringLiteral("The FFPKG image settings are not valid: %1").arg(what));
}

Status validateParams(const FormatParams& p)
{
    // UFS2Tool ValidateParameters(); sector size is fixed at 512.
    if (!isPowerOfTwo(p.blockSize) || p.blockSize < 4096 || p.blockSize > 65536)
        return badParam(QStringLiteral("the block size must be a power of 2 from 4 KiB to 64 KiB (got %1).").arg(p.blockSize));
    if (!isPowerOfTwo(p.fragmentSize) || p.fragmentSize < 512)
        return badParam(QStringLiteral("the fragment size must be a power of 2 of at least 512 bytes (got %1).").arg(p.fragmentSize));
    if (p.fragmentSize > p.blockSize)
        return badParam(QStringLiteral("the fragment size (%1) cannot exceed the block size (%2).").arg(p.fragmentSize).arg(p.blockSize));
    const int ratio = p.blockSize / p.fragmentSize;
    if (ratio > kMaxFrag || !isPowerOfTwo(ratio))
        return badParam(QStringLiteral("the block size must be 1, 2, 4 or 8 fragments (got %1).").arg(ratio));
    if (p.fragmentSize < kSectorSize)
        return badParam(QStringLiteral("the fragment size cannot be smaller than the 512-byte sector."));
    if (p.minFreePercent < 0 || p.minFreePercent > 99)
        return badParam(QStringLiteral("minimum free space must be 0 to 99% (got %1).").arg(p.minFreePercent));
    if (p.bytesPerInode < 0)
        return badParam(QStringLiteral("bytes per inode cannot be negative."));
    return success();
}

int inodesPerGroup(const FormatParams& p, qint64 totalBytes)
{
    // UFS2Tool ComputeInodesPerGroup(); 2048 = Ufs2Constants.DefaultInodesPerGroup.
    constexpr int kDefaultIpg = 2048;
    if (p.bytesPerInode <= 0)
        return kDefaultIpg;
    const qint64 totalInodes = totalBytes / p.bytesPerInode;
    const int fpb = p.blockSize / p.fragmentSize;
    const qint64 fpgTmp = qint64(kDefaultIpg) * fpb;
    const qint64 totalFrags = totalBytes / p.fragmentSize;
    qint64 ncg = (totalFrags + fpgTmp - 1) / fpgTmp;
    if (ncg < 1)
        ncg = 1;
    qint64 ipg = totalInodes / ncg;
    const int inopb = p.blockSize / kInodeSize;
    if (inopb > 0)
        ipg = ((ipg + inopb - 1) / inopb) * inopb;
    return int(std::max<qint64>(ipg, inopb));
}

int Geometry::usableFrags(int cg) const
{
    if (cg == ncg - 1)
        return int(totalFrags - qint64(cg) * fpg);
    return fpg;
}

Result<Geometry> computeGeometry(const FormatParams& p, qint64 totalBytes)
{
    FW_TRY(validateParams(p));
    Geometry g;
    g.params = p;
    g.bsize = p.blockSize;
    g.fsize = p.fragmentSize;
    g.fpb = g.bsize / g.fsize;
    if (totalBytes < qint64(g.bsize) * 16)
        return Error(ErrorCode::InvalidArgument, QStringLiteral("The image would be too small (minimum 16 blocks)."));
    if (totalBytes % g.fsize != 0)
        return Error(ErrorCode::InvalidArgument, QStringLiteral("The image size must be a multiple of the fragment size."));
    g.totalBytes = totalBytes;
    g.totalFrags = totalBytes / g.fsize;
    g.ipg = inodesPerGroup(p, totalBytes);
    const qint64 fpg = qint64(g.ipg) * g.fpb; // UFS2Tool: fs_fpg = ipg * frag (no -c)
    if (fpg > INT_MAX / 2)
        return Error(ErrorCode::Unsupported, QStringLiteral("These FFPKG settings give cylinder groups that are too large."));
    g.fpg = int(fpg);
    const qint64 ncg = std::max<qint64>(1, (g.totalFrags + g.fpg - 1) / g.fpg);
    if (ncg > INT_MAX / kCsumSize)
        return Error(ErrorCode::Unsupported, QStringLiteral("The image would have too many cylinder groups."));
    g.ncg = int(ncg);

    g.bshift = log2Int(g.bsize);
    g.fshift = log2Int(g.fsize);
    g.fragshift = log2Int(g.fpb);
    g.fsbtodb = log2Int(g.fsize / kSectorSize);
    g.sblkno = int(alignUp((kSuperblockOffset + kSuperblockSize + g.fsize - 1) / g.fsize, g.fpb));
    g.cblkno = g.sblkno + int(alignUp((kSuperblockSize + g.fsize - 1) / g.fsize, g.fpb));
    g.iblkno = g.cblkno + g.fpb;
    g.inopb = g.bsize / kInodeSize;
    const int inodeblks = ((g.ipg + g.inopb - 1) / g.inopb) * g.fpb;
    g.dblkno = g.iblkno + inodeblks;
    if (g.dblkno + g.fpb > g.fpg)
        return Error(ErrorCode::Unsupported, QStringLiteral("These FFPKG settings leave no room for data in a cylinder group."));

    g.maxContig = std::max(1, kMaxBSize / g.bsize);
    g.contigSumSize = std::min(g.maxContig, 16); // FS_MAXCONTIG
    g.inodeMapBytes = (g.ipg + 7) / 8;
    g.fragMapBytes = (g.fpg + 7) / 8;
    {
        // fs_cgsize (CGSIZE macro)
        qint64 raw = kCgHeaderBase + g.inodeMapBytes + g.fragMapBytes + 4;
        if (g.contigSumSize > 0)
            raw += qint64(g.contigSumSize) * 4 + ((g.fpg / g.fpb) + 7) / 8;
        g.cgSize = int(alignUp(raw, g.fsize));
    }
    // Header layout UFS2Tool writes (SerializeCylinderGroupHeader).
    g.inodeMapOff = kCgHeaderBase;
    g.fragMapOff = g.inodeMapOff + g.inodeMapBytes;
    g.nextFreeOff = g.fragMapOff + g.fragMapBytes;
    if (g.contigSumSize > 0) {
        const int rawEnd = g.fragMapOff + (g.fpg + 7) / 8;
        g.clusterSumOff = int(alignUp(rawEnd, 4)) - 4;
        g.clusterOff = g.clusterSumOff + (g.contigSumSize + 1) * 4;
        g.nextFreeOff = g.clusterOff + ((g.fpg / g.fpb) + 7) / 8;
    }
    {
        qint64 len = alignUp(g.nextFreeOff, g.fsize);
        len = std::max<qint64>(len, alignUp(g.fsize, kSectorSize));
        g.cgBlockLen = int(alignUp(len, kSectorSize));
        if (g.cgBlockLen > (g.iblkno - g.cblkno) * g.fsize)
            return Error(ErrorCode::Unsupported,
                         QStringLiteral("These FFPKG settings make the cylinder-group header overlap the inode table."));
    }
    g.csSize = int(alignUp(qint64(g.ncg) * kCsumSize, g.fsize));
    g.csFrags = (g.csSize + g.fsize - 1) / g.fsize;
    g.csFragsBlk = ((g.csSize + g.bsize - 1) / g.bsize) * g.fpb;
    if (g.dblkno + g.csFragsBlk + g.fpb > g.usableFrags(0))
        return Error(ErrorCode::Unsupported, QStringLiteral("The first cylinder group is too small for the file system summary."));

    g.dsize = g.totalFrags - g.sblkno - qint64(g.ncg) * (g.dblkno - g.sblkno) - g.csFrags;
    g.nindir = g.bsize / 8;
    {
        qint64 maxFileSize = qint64(g.bsize) * kNDirect - 1;
        qint64 sizepb = g.bsize;
        for (int i = 0; i < kNIndirect; ++i) {
            sizepb *= g.nindir;
            maxFileSize += sizepb;
        }
        g.maxFileSize = maxFileSize;
    }
    g.metaSpace = ((qint64(g.fpg) * p.minFreePercent / 200) / g.fpb) * g.fpb;
    return g;
}

// ---- .NET System.Random -------------------------------------------------------------

DotNetRandom::DotNetRandom(qint32 seed)
{
    constexpr qint32 kMSeed = 161803398;
    const qint32 subtraction = (seed == INT_MIN) ? INT_MAX : (seed < 0 ? -seed : seed);
    qint32 mj = kMSeed - subtraction;
    m_seed[55] = mj;
    qint32 mk = 1;
    int ii = 0;
    for (int i = 1; i < 55; ++i) {
        if ((ii += 21) >= 55)
            ii -= 55;
        m_seed[ii] = mk;
        mk = qint32(quint32(mj) - quint32(mk)); // C# unchecked arithmetic
        if (mk < 0)
            mk += INT_MAX;
        mj = m_seed[ii];
    }
    for (int k = 1; k < 5; ++k) {
        for (int i = 1; i < 56; ++i) {
            int n = i + 30;
            if (n >= 55)
                n -= 55;
            m_seed[i] = qint32(quint32(m_seed[i]) - quint32(m_seed[1 + n]));
            if (m_seed[i] < 0)
                m_seed[i] += INT_MAX;
        }
    }
    m_inext = 0;
    m_inextp = 21;
}

qint32 DotNetRandom::internalSample()
{
    int locINext = m_inext;
    if (++locINext >= 56)
        locINext = 1;
    int locINextp = m_inextp;
    if (++locINextp >= 56)
        locINextp = 1;
    qint32 retVal = qint32(quint32(m_seed[locINext]) - quint32(m_seed[locINextp]));
    if (retVal == INT_MAX)
        --retVal;
    if (retVal < 0)
        retVal += INT_MAX;
    m_seed[locINext] = retVal;
    m_inext = locINext;
    m_inextp = locINextp;
    return retVal;
}

qint32 DotNetRandom::next(qint32 minValue, qint32 maxValue)
{
    const qint64 range = qint64(maxValue) - minValue;
    // Only the range <= int.MaxValue path is used by UFS2Tool.
    const double sample = double(internalSample()) * (1.0 / double(INT_MAX));
    return qint32(sample * double(range)) + minValue;
}

// ---- inodes ---------------------------------------------------------------------------

void serializeInode(const Inode& in, char* o)
{
    std::memset(o, 0, kInodeSize);
    wr16(o + 0x00, in.mode);
    wr16(o + 0x02, quint16(in.nlink));
    wr32(o + 0x04, in.uid);
    wr32(o + 0x08, in.gid);
    wr32(o + 0x0C, in.blksize);
    wr64(o + 0x10, in.size);
    wr64(o + 0x18, in.blocks);
    wr64(o + 0x20, in.atime);
    wr64(o + 0x28, in.mtime);
    wr64(o + 0x30, in.ctime);
    wr64(o + 0x38, in.birthtime);
    wr32s(o + 0x40, in.mtimensec);
    wr32s(o + 0x44, in.atimensec);
    wr32s(o + 0x48, in.ctimensec);
    wr32s(o + 0x4C, in.birthnsec);
    wr32s(o + 0x50, in.gen);
    wr32(o + 0x54, in.kernflags);
    wr32(o + 0x58, in.flags);
    wr32s(o + 0x5C, in.extsize);
    wr64(o + 0x60, in.extb[0]);
    wr64(o + 0x68, in.extb[1]);
    for (int i = 0; i < kNDirect; ++i)
        wr64(o + 0x70 + 8 * i, in.db[i]);
    for (int i = 0; i < kNIndirect; ++i)
        wr64(o + 0xD0 + 8 * i, in.ib[i]);
    wr64(o + 0xE8, in.modrev);
    wr32(o + 0xF0, in.dirdepth);
    wr32(o + 0xF4, in.ckhash);
}

Inode parseInode(const char* p)
{
    Inode in;
    in.mode = rd16(p + 0x00);
    in.nlink = qint16(rd16(p + 0x02));
    in.uid = rd32(p + 0x04);
    in.gid = rd32(p + 0x08);
    in.blksize = rd32(p + 0x0C);
    in.size = rd64(p + 0x10);
    in.blocks = rd64(p + 0x18);
    in.atime = rd64(p + 0x20);
    in.mtime = rd64(p + 0x28);
    in.ctime = rd64(p + 0x30);
    in.birthtime = rd64(p + 0x38);
    in.mtimensec = rd32s(p + 0x40);
    in.atimensec = rd32s(p + 0x44);
    in.ctimensec = rd32s(p + 0x48);
    in.birthnsec = rd32s(p + 0x4C);
    in.gen = rd32s(p + 0x50);
    in.kernflags = rd32(p + 0x54);
    in.flags = rd32(p + 0x58);
    in.extsize = rd32s(p + 0x5C);
    in.extb[0] = rd64(p + 0x60);
    in.extb[1] = rd64(p + 0x68);
    for (int i = 0; i < kNDirect; ++i)
        in.db[i] = rd64(p + 0x70 + 8 * i);
    for (int i = 0; i < kNIndirect; ++i)
        in.ib[i] = rd64(p + 0xD0 + 8 * i);
    in.modrev = rd64(p + 0xE8);
    in.dirdepth = rd32(p + 0xF0);
    in.ckhash = rd32(p + 0xF4);
    return in;
}

// ---- directories ------------------------------------------------------------------------

namespace {

struct PackEntry {
    quint32 ino;
    quint8 type;
    const QByteArray* name;
    int recLen;
    int minSize;
};

std::vector<PackEntry> packList(const std::vector<DirEnt>& entries, quint32 self, quint32 parent,
                                const QByteArray& dot, const QByteArray& dotdot)
{
    std::vector<PackEntry> all;
    all.reserve(entries.size() + 2);
    all.push_back({self, kDtDir, &dot, direntSize(1), 8 + 1 + 1});
    all.push_back({parent, kDtDir, &dotdot, direntSize(2), 8 + 2 + 1});
    for (const DirEnt& e : entries)
        all.push_back({e.ino, e.type, &e.name, direntSize(int(e.name.size())), 8 + int(e.name.size()) + 1});
    return all;
}

Error nameTooLong(const QByteArray& name)
{
    return Error(ErrorCode::InvalidArgument,
                 QStringLiteral("The name \"%1\" is too long for a UFS2 image (255 bytes at most).")
                     .arg(QString::fromUtf8(name.left(60))));
}

void putEntry(char* p, quint32 ino, int recLen, quint8 type, const QByteArray& name)
{
    wr32(p, ino);
    wr16(p + 4, quint16(recLen));
    p[6] = char(type);
    p[7] = char(quint8(name.size()));
    std::memcpy(p + 8, name.constData(), size_t(name.size()));
}

void putPadding(char* p, int recLen)
{
    wr32(p, 0);
    wr16(p + 4, quint16(recLen));
    p[6] = 0;
    p[7] = 0;
}

} // namespace

Result<int> dirBlocksNeeded(const std::vector<DirEnt>& entries, int bsize)
{
    static const QByteArray dot("."), dotdot("..");
    const auto all = packList(entries, 0, 0, dot, dotdot);
    const int count = int(all.size());
    int blocks = 0;
    int idx = 0;
    while (idx < count) {
        ++blocks;
        int pos = 0;
        while (idx < count) {
            const PackEntry& e = all[size_t(idx)];
            if (e.name->size() > kMaxNameLen || e.minSize > kDirBlockSize)
                return nameTooLong(*e.name);
            int rem = kDirBlockSize - pos % kDirBlockSize;
            if (e.minSize > rem) {
                pos += rem;
                if (pos >= bsize)
                    break;
            }
            if (pos + e.minSize > bsize)
                break;
            bool isLast = (idx == count - 1);
            if (!isLast) {
                const int nextMin = all[size_t(idx) + 1].minSize;
                const int after = pos + e.recLen;
                const int nextPos = after % kDirBlockSize;
                const int nextRem = kDirBlockSize - nextPos;
                isLast = (nextMin > nextRem && nextPos != 0) || (after + nextMin > bsize);
            }
            if (isLast)
                pos += kDirBlockSize - pos % kDirBlockSize;
            else
                pos += e.recLen;
            ++idx;
            if (isLast && pos >= bsize)
                break;
        }
    }
    return std::max(1, blocks);
}

Result<QByteArray> buildDirBlocks(quint32 selfIno, quint32 parentIno, const std::vector<DirEnt>& entries,
                                  int nblocks, int bsize)
{
    static const QByteArray dot("."), dotdot("..");
    const auto all = packList(entries, selfIno, parentIno, dot, dotdot);
    const int count = int(all.size());
    QByteArray out(qsizetype(nblocks) * bsize, '\0');
    int idx = 0;
    for (int b = 0; b < nblocks; ++b) {
        char* blk = out.data() + qsizetype(b) * bsize;
        int pos = 0;
        while (idx < count) {
            const PackEntry& e = all[size_t(idx)];
            if (e.name->size() > kMaxNameLen || e.minSize > kDirBlockSize)
                return nameTooLong(*e.name);
            int recLen = e.recLen;
            int rem = kDirBlockSize - pos % kDirBlockSize;
            if (e.minSize > rem) {
                if (pos + rem >= bsize)
                    break;
                putPadding(blk + pos, rem);
                pos += rem;
            }
            if (pos + e.minSize > bsize)
                break;
            bool isLast = (idx == count - 1);
            if (!isLast) {
                const int nextMin = all[size_t(idx) + 1].minSize;
                const int after = pos + recLen;
                const int nextPos = after % kDirBlockSize;
                const int nextRem = kDirBlockSize - nextPos;
                isLast = (nextMin > nextRem && nextPos != 0) || (after + nextMin > bsize);
            }
            if (isLast)
                recLen = kDirBlockSize - pos % kDirBlockSize;
            putEntry(blk + pos, e.ino, recLen, e.type, *e.name);
            pos += recLen;
            ++idx;
            if (isLast && idx < count) {
                if (pos >= bsize)
                    break;
            } else if (isLast) {
                break;
            }
        }
        if (pos == 0)
            return Error(ErrorCode::Internal, QStringLiteral("Directory packing failed (empty block)."));
        if (pos % kDirBlockSize != 0) {
            const int fill = kDirBlockSize - pos % kDirBlockSize;
            putPadding(blk + pos, fill);
            pos += fill;
        }
        while (pos + kDirBlockSize <= bsize) {
            putPadding(blk + pos, kDirBlockSize);
            pos += kDirBlockSize;
        }
    }
    if (idx < count)
        return Error(ErrorCode::Internal, QStringLiteral("Directory packing failed (%1 of %2 entries written).").arg(idx).arg(count));
    return out;
}

} // namespace fw::ufs2
