#pragma once

// PFS / PFSC on-disk format: constants, little-endian helpers, inode/dirent
// serialisation, the flat_path_table hash and the PFSC block-compression
// header.
//
// Port of Lazy_MkPFS (https://github.com/Nazky/Lazy_MkPFS, upstream commit
// 78bdd00c, itself derived from PSBrew/MkPFS): lazy_mkpfs/consts.py,
// types.py, inspect.py (fpt_hash, make_fpt_and_collision_blob) and
// compression.py (PFSC header). Copyright the Lazy_MkPFS and MkPFS authors.
// Licensed under the GNU General Public License v3.0 or later; this native
// port is distributed under the same licence (see native/docs/engines/PFS.md).

#include <QByteArray>
#include <QString>

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace fw::pfs {

// ---- image header ------------------------------------------------------------------
inline constexpr qint64 kPfsMagic = 20130315;
inline constexpr int kVersionPs4 = 1;
inline constexpr int kVersionPs5 = 2;

inline constexpr quint16 kModeSigned = 0x1;
inline constexpr quint16 kMode64BitInodes = 0x2;
inline constexpr quint16 kModeEncrypted = 0x4;
inline constexpr quint16 kModeCaseInsensitive = 0x8;

// ---- inodes ------------------------------------------------------------------------
inline constexpr quint16 kInodeModeOtherRead = 0x001;
inline constexpr quint16 kInodeModeOtherWrite = 0x002;
inline constexpr quint16 kInodeModeOtherExec = 0x004;
inline constexpr quint16 kInodeModeGroupRead = 0x008;
inline constexpr quint16 kInodeModeGroupWrite = 0x010;
inline constexpr quint16 kInodeModeGroupExec = 0x020;
inline constexpr quint16 kInodeModeUserRead = 0x040;
inline constexpr quint16 kInodeModeUserWrite = 0x080;
inline constexpr quint16 kInodeModeUserExec = 0x100;
inline constexpr quint16 kInodeModeDir = 0x4000;
inline constexpr quint16 kInodeModeFile = 0x8000;
inline constexpr quint16 kInodeModeAnyWrite = kInodeModeOtherWrite | kInodeModeGroupWrite | kInodeModeUserWrite;
inline constexpr quint16 kInodeRxOnly = kInodeModeOtherRead | kInodeModeOtherExec | kInodeModeGroupRead
                                        | kInodeModeGroupExec | kInodeModeUserRead | kInodeModeUserExec;

inline constexpr quint32 kInodeFlagCompressed = 0x1;
inline constexpr quint32 kInodeFlagReadonly = 0x10;
inline constexpr quint32 kInodeFlagInternal = 0x20000;

inline constexpr qint32 kDirentFile = 2;
inline constexpr qint32 kDirentDirectory = 3;
inline constexpr qint32 kDirentDot = 4;
inline constexpr qint32 kDirentDotDot = 5;

inline constexpr int kInodeD32Size = 0xA8;
inline constexpr int kInodeS32Size = 0x2C8;
inline constexpr int kInodeS64Size = 0x310;
inline constexpr int kMaxDirectBlocks = 12;
inline constexpr int kMaxIndirectBlocks = 5;
inline constexpr int kSigSize = 32;

// ---- PFSC (per-file block compression) ---------------------------------------------
inline constexpr qint32 kPfscMagic = 0x43534650; // "PFSC"
inline constexpr qint32 kPfscUnk8 = 6;
inline constexpr qint64 kPfscBlock = 0x10000;     // logical block, independent of the PFS block size
inline constexpr int kPfscHeaderSize = 0x30;
inline constexpr qint64 kPfscOffsetsOffset = 0x400;
inline constexpr qint64 kPfscInitialDataOffset = 0x10000;

// ---- little-endian helpers ---------------------------------------------------------
inline void putU16(char* p, quint16 v) { p[0] = char(v & 0xFF); p[1] = char(v >> 8); }
inline void putU32(char* p, quint32 v)
{
    for (int i = 0; i < 4; ++i)
        p[i] = char((v >> (8 * i)) & 0xFF);
}
inline void putU64(char* p, quint64 v)
{
    for (int i = 0; i < 8; ++i)
        p[i] = char((v >> (8 * i)) & 0xFF);
}
inline void putI32(char* p, qint32 v) { putU32(p, quint32(v)); }
inline void putI64(char* p, qint64 v) { putU64(p, quint64(v)); }

inline quint16 getU16(const char* p) { return quint16(quint8(p[0]) | (quint16(quint8(p[1])) << 8)); }
inline quint32 getU32(const char* p)
{
    quint32 v = 0;
    for (int i = 3; i >= 0; --i)
        v = (v << 8) | quint8(p[i]);
    return v;
}
inline quint64 getU64(const char* p)
{
    quint64 v = 0;
    for (int i = 7; i >= 0; --i)
        v = (v << 8) | quint8(p[i]);
    return v;
}
inline qint32 getI32(const char* p) { return qint32(getU32(p)); }
inline qint64 getI64(const char* p) { return qint64(getU64(p)); }

inline qint64 ceilDiv(qint64 a, qint64 b) { return (a + b - 1) / b; }

// ---- inode ---------------------------------------------------------------------------
struct Inode {
    quint32 number = 0;
    quint16 mode = 0;
    quint16 nlink = 0;
    quint32 flags = 0;
    qint64 size = 0;           // stored payload size (PFSC stream size for compressed files)
    qint64 sizeCompressed = 0; // logical size; for compressed files rounded up to the block size
    quint32 blocks = 0;
    std::array<qint64, kMaxDirectBlocks> db{};   // 32-bit on disk for unsigned images
    std::array<qint64, kMaxIndirectBlocks> ib{};
    qint64 time = 0;           // seconds; written to atime/mtime/ctime/birthtime

    bool isDir() const { return (mode & kInodeModeDir) != 0; }
    bool isFile() const { return (mode & kInodeModeFile) != 0; }
    bool isCompressed() const { return (flags & kInodeFlagCompressed) != 0; }
    qint64 storedSize() const { return isCompressed() ? size : sizeCompressed; }
    qint64 logicalSize() const { return isCompressed() ? sizeCompressed : size; }
};

// Writes the unsigned D32 layout (kInodeD32Size bytes) at `dst`.
void writeInodeD32(char* dst, const Inode& ino);

// Parses one inode. `signedLayout` selects S32/S64 (inodeBits 32/64) instead of D32.
Inode parseInode(const char* src, quint32 number, bool signedLayout, int inodeBits);
int inodeSizeFor(bool signedLayout, int inodeBits);

// ---- dirent ----------------------------------------------------------------------------
inline int direntSize(int nameLength)
{
    int size = nameLength + 17;
    const int rem = size % 8;
    if (rem)
        size += 8 - rem;
    return size;
}
// Appends one dirent (name must be ASCII; the caller checks).
void appendDirent(QByteArray& out, quint32 inode, qint32 type, const QByteArray& name);

// ---- header block ------------------------------------------------------------------------
struct HeaderParams {
    int blockSize = 0x10000;
    int version = kVersionPs5;
    quint16 mode = 0;
    qint64 nblock = 1;
    qint64 inodeCount = 0;
    qint64 ndblock = 0;
    qint64 inodeBlockCount = 0;
    qint64 time = 0;
};
// The complete first block of an unsigned, unencrypted image.
QByteArray headerBlock(const HeaderParams& p);

// ---- flat_path_table ---------------------------------------------------------------------
quint32 fptHash(const QByteArray& path, bool caseInsensitive);

// ---- PFSC -----------------------------------------------------------------------------------
// Header + block offset table area for `blockCount` logical blocks (also the
// data start offset of the first stored block).
qint64 pfscHeaderSize(qint64 blockCount);
// Writes the PFSC header area (pfscHeaderSize bytes) for the given block offsets
// (blockCount + 1 entries, offsets[0] == header size).
QByteArray pfscHeaderArea(qint64 blockCount, const std::vector<qint64>& offsets);

struct PfscHeader {
    qint64 blockSize = 0;
    qint64 blockCount = 0;
    qint64 offsetsOffset = 0;
    qint64 dataOffset = 0;
    qint64 logicalSize = 0;
};
// Validates the fixed 0x30-byte header like Lazy's _parse_pfsc_header. Returns
// an empty string on success, otherwise what is wrong.
QString parsePfscHeader(const char* head, qint64 len, PfscHeader* out);

// Python's gain test, evaluated with the same IEEE double operations.
inline bool pfscStoreCompressed(qint64 compressedLen, int thresholdGain)
{
    const double gain = (double(kPfscBlock - compressedLen) / double(kPfscBlock)) * 100.0;
    return compressedLen < kPfscBlock && gain >= double(thresholdGain);
}

// ---- names ---------------------------------------------------------------------------------
// Python str.lower() on ASCII.
QByteArray asciiLower(const QByteArray& s);
bool isAscii(const QString& s);
// Lazy's sort key: case-insensitive code-point order of the relative path.
// Ties are broken by the original bytes so the order is fully deterministic.
bool lessCaseInsensitive(const QByteArray& a, const QByteArray& b);

} // namespace fw::pfs
