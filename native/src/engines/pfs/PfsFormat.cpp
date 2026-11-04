// PFS / PFSC on-disk format helpers.
//
// Port of Lazy_MkPFS (Nazky, derived from PSBrew/MkPFS), lazy_mkpfs/types.py,
// build.py (_pack_pfs_header_block, build_inode_block_sig_s64), inspect.py
// (fpt_hash) and compression.py (PFSC header). GPL-3.0-or-later.

#include "engines/pfs/PfsFormat.h"

#include <algorithm>

namespace fw::pfs {

void writeInodeD32(char* dst, const Inode& ino)
{
    std::memset(dst, 0, kInodeD32Size);
    putU16(dst + 0x00, ino.mode);
    putU16(dst + 0x02, ino.nlink);
    putU32(dst + 0x04, ino.flags);
    putI64(dst + 0x08, ino.size);
    putI64(dst + 0x10, ino.sizeCompressed);
    for (int i = 0; i < 4; ++i)
        putI64(dst + 0x18 + 8 * i, ino.time); // atime, mtime, ctime, birthtime
    // 0x38: four nanosecond fields, 0x48 uid, 0x4C gid, 0x50/0x58 unknown: all zero
    putU32(dst + 0x60, ino.blocks);
    for (int i = 0; i < kMaxDirectBlocks; ++i)
        putI32(dst + 0x64 + 4 * i, qint32(ino.db[size_t(i)]));
    for (int i = 0; i < kMaxIndirectBlocks; ++i)
        putI32(dst + 0x94 + 4 * i, qint32(ino.ib[size_t(i)]));
}

int inodeSizeFor(bool signedLayout, int inodeBits)
{
    if (!signedLayout)
        return kInodeD32Size;
    return inodeBits == 64 ? kInodeS64Size : kInodeS32Size;
}

Inode parseInode(const char* src, quint32 number, bool signedLayout, int inodeBits)
{
    Inode ino;
    ino.number = number;
    ino.mode = getU16(src + 0x00);
    ino.nlink = getU16(src + 0x02);
    ino.flags = getU32(src + 0x04);
    ino.size = getI64(src + 0x08);
    ino.sizeCompressed = getI64(src + 0x10);
    ino.time = getI64(src + 0x20); // mtime
    ino.blocks = getU32(src + 0x60);
    if (!signedLayout) {
        for (int i = 0; i < kMaxDirectBlocks; ++i)
            ino.db[size_t(i)] = getI32(src + 0x64 + 4 * i);
        for (int i = 0; i < kMaxIndirectBlocks; ++i)
            ino.ib[size_t(i)] = getI32(src + 0x94 + 4 * i);
        return ino;
    }
    const bool wide = inodeBits == 64;
    const int entry = kSigSize + (wide ? 8 : 4);
    int off = wide ? 0x68 : 0x64;
    for (int i = 0; i < kMaxDirectBlocks; ++i, off += entry)
        ino.db[size_t(i)] = wide ? getI64(src + off + kSigSize) : getI32(src + off + kSigSize);
    for (int i = 0; i < kMaxIndirectBlocks; ++i, off += entry)
        ino.ib[size_t(i)] = wide ? getI64(src + off + kSigSize) : getI32(src + off + kSigSize);
    return ino;
}

void appendDirent(QByteArray& out, quint32 inode, qint32 type, const QByteArray& name)
{
    const int entSize = direntSize(int(name.size()));
    const qsizetype at = out.size();
    out.resize(at + entSize);
    char* p = out.data() + at;
    std::memset(p, 0, size_t(entSize));
    putU32(p + 0, inode);
    putI32(p + 4, type);
    putI32(p + 8, qint32(name.size()));
    putI32(p + 12, entSize);
    std::memcpy(p + 16, name.constData(), size_t(name.size()));
}

QByteArray headerBlock(const HeaderParams& p)
{
    QByteArray hdr(p.blockSize, '\0');
    char* h = hdr.data();
    putI64(h + 0x00, p.version);
    putI64(h + 0x08, kPfsMagic);
    putI64(h + 0x10, 0);
    h[0x18] = 0; // fmode
    h[0x19] = 0; // clean
    h[0x1A] = 1; // read-only
    h[0x1B] = 0;
    putU16(h + 0x1C, p.mode);
    putU16(h + 0x1E, 0);
    putU32(h + 0x20, quint32(p.blockSize));
    putU32(h + 0x24, 0);
    putI64(h + 0x28, p.nblock);
    putI64(h + 0x30, p.inodeCount);
    putI64(h + 0x38, p.ndblock);
    putI64(h + 0x40, p.inodeBlockCount);

    // Inode-table "inode" (signed S64 layout) at 0x50, as build_inode_block_sig_s64().
    char* s = h + 0x50;
    putU16(s + 0x00, 0);
    putU16(s + 0x02, 1);
    putU32(s + 0x04, kInodeFlagReadonly);
    const qint64 tableBytes = p.inodeBlockCount * p.blockSize;
    putI64(s + 0x08, tableBytes);
    putI64(s + 0x10, tableBytes);
    for (int i = 0; i < 4; ++i)
        putI64(s + 0x18 + 8 * i, p.time);
    putU32(s + 0x60, quint32(p.inodeBlockCount));
    for (int i = 0; i < kMaxDirectBlocks; ++i) {
        const qint64 block = i < p.inodeBlockCount ? 1 + i : (i == 0 ? 1 : 0);
        putI64(s + 0x68 + i * 40 + 32, block);
    }
    putU32(h + 0x368, 1); // unsigned, unencrypted
    return hdr;
}

quint32 fptHash(const QByteArray& path, bool caseInsensitive)
{
    quint32 h = 0;
    for (char ch : path) {
        quint8 c = quint8(ch);
        if (caseInsensitive && c >= 'a' && c <= 'z')
            c = quint8(c - 'a' + 'A');
        h = quint32(c) + 31u * h;
    }
    return h;
}

qint64 pfscHeaderSize(qint64 blockCount)
{
    const qint64 tableSize = (blockCount + 1) * 8;
    const qint64 capacity = kPfscInitialDataOffset - kPfscOffsetsOffset;
    const qint64 extra = std::max<qint64>(0, tableSize - capacity);
    const qint64 extraBlocks = extra > 0 ? ceilDiv(extra, kPfscBlock) : 0;
    return kPfscInitialDataOffset + extraBlocks * kPfscBlock;
}

QByteArray pfscHeaderArea(qint64 blockCount, const std::vector<qint64>& offsets)
{
    const qint64 headerSize = pfscHeaderSize(blockCount);
    QByteArray area(qsizetype(headerSize), '\0');
    char* a = area.data();
    putI32(a + 0x00, kPfscMagic);
    putI32(a + 0x04, 0);
    putI32(a + 0x08, kPfscUnk8);
    putI32(a + 0x0C, qint32(kPfscBlock));
    putI64(a + 0x10, kPfscBlock);
    putI64(a + 0x18, kPfscOffsetsOffset);
    putU64(a + 0x20, quint64(headerSize));
    putI64(a + 0x28, blockCount * kPfscBlock);
    for (size_t i = 0; i < offsets.size(); ++i)
        putU64(a + kPfscOffsetsOffset + qint64(i) * 8, quint64(offsets[i]));
    return area;
}

QString parsePfscHeader(const char* head, qint64 len, PfscHeader* out)
{
    if (len < kPfscHeaderSize)
        return QStringLiteral("PFSC payload is too small for its header");
    const qint32 magic = getI32(head + 0x00);
    const qint32 unk4 = getI32(head + 0x04);
    const qint32 unk8 = getI32(head + 0x08);
    const qint32 lbs = getI32(head + 0x0C);
    const qint64 lbs2 = getI64(head + 0x10);
    const qint64 offsetsOffset = getI64(head + 0x18);
    const quint64 dataOffset = getU64(head + 0x20);
    const qint64 logical = getI64(head + 0x28);
    if (magic != kPfscMagic)
        return QStringLiteral("invalid PFSC magic 0x%1").arg(quint32(magic), 8, 16, QLatin1Char('0'));
    if (unk4 != 0)
        return QStringLiteral("invalid PFSC field at 0x04 (%1)").arg(unk4);
    if (unk8 != kPfscUnk8)
        return QStringLiteral("invalid PFSC field at 0x08 (%1)").arg(unk8);
    if (lbs != kPfscBlock)
        return QStringLiteral("unsupported PFSC block size %1").arg(lbs);
    if (lbs2 != lbs)
        return QStringLiteral("PFSC block sizes disagree");
    if (logical < 0 || logical % kPfscBlock != 0)
        return QStringLiteral("PFSC logical size %1 is not a multiple of 64 KiB").arg(logical);
    if (offsetsOffset != kPfscOffsetsOffset)
        return QStringLiteral("invalid PFSC offset table position %1").arg(offsetsOffset);
    if (dataOffset < quint64(kPfscInitialDataOffset) || dataOffset > (quint64(1) << 62))
        return QStringLiteral("invalid PFSC data offset %1").arg(dataOffset);
    if (out) {
        out->blockSize = lbs;
        out->blockCount = logical / kPfscBlock;
        out->offsetsOffset = offsetsOffset;
        out->dataOffset = qint64(dataOffset);
        out->logicalSize = logical;
    }
    return {};
}

QByteArray asciiLower(const QByteArray& s)
{
    QByteArray out = s;
    for (char& c : out)
        if (c >= 'A' && c <= 'Z')
            c = char(c - 'A' + 'a');
    return out;
}

bool isAscii(const QString& s)
{
    for (QChar c : s)
        if (c.unicode() > 0x7F)
            return false;
    return true;
}

namespace {
int compareBytes(const QByteArray& a, const QByteArray& b, bool fold)
{
    const qsizetype n = std::min(a.size(), b.size());
    for (qsizetype i = 0; i < n; ++i) {
        quint8 x = quint8(a[i]), y = quint8(b[i]);
        if (fold) {
            if (x >= 'A' && x <= 'Z')
                x = quint8(x - 'A' + 'a');
            if (y >= 'A' && y <= 'Z')
                y = quint8(y - 'A' + 'a');
        }
        if (x != y)
            return x < y ? -1 : 1;
    }
    if (a.size() == b.size())
        return 0;
    return a.size() < b.size() ? -1 : 1;
}
} // namespace

bool lessCaseInsensitive(const QByteArray& a, const QByteArray& b)
{
    const int c = compareBytes(a, b, true);
    if (c != 0)
        return c < 0;
    return compareBytes(a, b, false) < 0;
}

} // namespace fw::pfs
