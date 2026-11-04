#pragma once

// UFS2 on-disk format, as written by UFS2Tool `newfs -D` for PS5 .ffpkg images.
//
// The layout rules (geometry, sizing, allocation order, directory packing,
// inode generation numbers) are a port of UFS2Tool 4.0.0 (commit c123639) by
// SvenGDK, Copyright (c) 2026, SvenGDK, BSD-2-Clause — see
// LICENSES/UFS2Tool-BSD-2-Clause.txt. Field offsets follow FreeBSD
// sys/ufs/ffs/fs.h and sys/ufs/ufs/dinode.h / dir.h. Where UFS2Tool and
// FreeBSD newfs differ, this file follows UFS2Tool (that is what the PS5
// loaders have been fed so far) and says so.

#include "core/Result.h"

#include <QByteArray>
#include <QString>
#include <QtEndian>

#include <cstdint>
#include <vector>

namespace fw::ufs2 {

// ---- constants (FreeBSD fs.h / dinode.h / dir.h) ---------------------------------
inline constexpr qint32 kUfs2Magic = 0x19540119;   // FS_UFS2_MAGIC
inline constexpr qint32 kCgMagic = 0x090255;       // CG_MAGIC
inline constexpr qint64 kSuperblockOffset = 65536; // SBLOCK_UFS2
inline constexpr int kSuperblockSize = 8192;       // SBLOCKSIZE
inline constexpr int kSectorSize = 512;            // newfs -S 512 (locked by the legacy app)
inline constexpr int kDevBSize = 512;              // DEV_BSIZE, unit of di_blocks
inline constexpr int kInodeSize = 256;             // sizeof(struct ufs2_dinode)
inline constexpr quint32 kRootIno = 2;             // UFS_ROOTINO
inline constexpr int kNDirect = 12;                // UFS_NDADDR
inline constexpr int kNIndirect = 3;               // UFS_NIADDR
inline constexpr int kDirBlockSize = 512;          // DIRBLKSIZ
inline constexpr int kMaxNameLen = 255;            // UFS_MAXNAMLEN
inline constexpr int kMaxBSize = 65536;            // MAXBSIZE
inline constexpr int kCgHeaderBase = 168;          // sizeof(struct cg) without bitmaps
inline constexpr int kCsumSize = 16;               // sizeof(struct csum)
inline constexpr int kMaxFrag = 8;                 // MAXFRAG

inline constexpr quint8 kDtUnknown = 0;
inline constexpr quint8 kDtDir = 4;
inline constexpr quint8 kDtReg = 8;
inline constexpr quint8 kDtLnk = 10;

inline constexpr quint16 kIfMt = 0xF000;
inline constexpr quint16 kIfDir = 0x4000;
inline constexpr quint16 kIfReg = 0x8000;
inline constexpr quint16 kIfLnk = 0xA000;
inline constexpr quint16 kPerm0555 = 0x16D; // UFS2Tool gives files and directories r-xr-xr-x

// ---- little-endian access ---------------------------------------------------------
inline quint16 rd16(const char* p) { return qFromLittleEndian<quint16>(p); }
inline quint32 rd32(const char* p) { return qFromLittleEndian<quint32>(p); }
inline qint32 rd32s(const char* p) { return qFromLittleEndian<qint32>(p); }
inline qint64 rd64(const char* p) { return qFromLittleEndian<qint64>(p); }
inline void wr16(char* p, quint16 v) { qToLittleEndian<quint16>(v, p); }
inline void wr32(char* p, quint32 v) { qToLittleEndian<quint32>(v, p); }
inline void wr32s(char* p, qint32 v) { qToLittleEndian<qint32>(v, p); }
inline void wr64(char* p, qint64 v) { qToLittleEndian<qint64>(v, p); }

inline qint64 alignUp(qint64 v, qint64 a) { return ((v + a - 1) / a) * a; }
inline qint64 ceilDiv(qint64 v, qint64 d) { return (v + d - 1) / d; }
int log2Int(qint64 v);
bool isPowerOfTwo(qint64 v);

// ---- newfs parameters ---------------------------------------------------------------
// The options the legacy app passed to `UFS2Tool newfs -O 2 -b B -f F -m M -S 512 -i I`.
struct FormatParams {
    int blockSize = 65536;      // -b
    int fragmentSize = 65536;   // -f
    int minFreePercent = 0;     // -m
    int bytesPerInode = 262144; // -i
};

// UFS2Tool's ValidateParameters() for the options above (sector size 512).
Status validateParams(const FormatParams& p);

// Cylinder-group geometry of an image of `totalBytes` (UFS2Tool WriteFilesystem()).
struct Geometry {
    FormatParams params;
    int bsize = 0, fsize = 0, fpb = 0; // block, fragment, fragments per block
    qint64 totalBytes = 0;
    qint64 totalFrags = 0;            // fs_size
    int ipg = 0;                      // inodes per group
    int fpg = 0;                      // fragments per group
    int ncg = 0;                      // cylinder groups
    int sblkno = 0, cblkno = 0, iblkno = 0, dblkno = 0;
    int inopb = 0;                    // inodes per block
    int maxContig = 0, contigSumSize = 0;
    int cgSize = 0;                   // fs_cgsize
    int cgBlockLen = 0;               // bytes UFS2Tool writes for each CG header
    int csSize = 0;                   // fs_cssize
    int csFrags = 0;                  // howmany(cssize, fsize)
    int csFragsBlk = 0;               // cssize rounded up to whole blocks, in fragments
    int clusterSumOff = 0, clusterOff = 0, nextFreeOff = 0;
    int inodeMapOff = 0, fragMapOff = 0;
    int inodeMapBytes = 0, fragMapBytes = 0;
    int bshift = 0, fshift = 0, fragshift = 0, fsbtodb = 0;
    qint64 dsize = 0;                 // fs_dsize
    qint64 maxFileSize = 0;
    qint64 metaSpace = 0;
    int nindir = 0;

    // Fragments of cylinder group `cg` inside the file system (the last one may be short).
    int usableFrags(int cg) const;
    qint64 cgStartFrag(int cg) const { return qint64(cg) * fpg; }
    quint32 maxInodes() const { return quint32(qint64(ncg) * ipg); }
};

int inodesPerGroup(const FormatParams& p, qint64 totalBytes);
Result<Geometry> computeGeometry(const FormatParams& p, qint64 totalBytes);

// ---- .NET System.Random(seed) -------------------------------------------------------
// UFS2Tool seeds `new Random(cg + 1)` for the generation numbers of the inodes
// newfs initialises; .NET's seeded generator is Knuth's subtractive generator
// (the "Net5CompatSeedImpl"). Ported so images are byte-identical.
class DotNetRandom {
public:
    explicit DotNetRandom(qint32 seed);
    qint32 next(qint32 minValue, qint32 maxValue);

private:
    qint32 internalSample();
    qint32 m_seed[56] = {};
    int m_inext = 0;
    int m_inextp = 21;
};

// ---- inodes ---------------------------------------------------------------------------
struct Inode {
    quint16 mode = 0;
    qint16 nlink = 0;
    quint32 uid = 0, gid = 0, blksize = 0;
    qint64 size = 0;
    qint64 blocks = 0; // DEV_BSIZE units
    qint64 atime = 0, mtime = 0, ctime = 0, birthtime = 0;
    qint32 mtimensec = 0, atimensec = 0, ctimensec = 0, birthnsec = 0;
    qint32 gen = 0;
    quint32 kernflags = 0, flags = 0;
    qint32 extsize = 0;
    qint64 extb[2] = {0, 0};
    qint64 db[kNDirect] = {};
    qint64 ib[kNIndirect] = {};
    qint64 modrev = 0;
    quint32 dirdepth = 0; // di_freelink / di_dirdepth union
    quint32 ckhash = 0;

    quint16 type() const { return mode & kIfMt; }
    bool isDir() const { return type() == kIfDir; }
    bool isReg() const { return type() == kIfReg; }
    bool isLink() const { return type() == kIfLnk; }
};

void serializeInode(const Inode& in, char* out256);
Inode parseInode(const char* in256);

// ---- directories ------------------------------------------------------------------------
struct DirEnt {
    quint32 ino = 0;
    quint8 type = kDtUnknown;
    QByteArray name; // on-disk bytes (UTF-8)
};

// DIRECTSIZ(namlen): header + name + NUL, rounded up to 4.
inline int direntSize(int nameLen) { return (8 + nameLen + 1 + 3) & ~3; }

// UFS2Tool CalculateDirBlocksNeeded(): blocks for "." + ".." + entries.
Result<int> dirBlocksNeeded(const std::vector<DirEnt>& entries, int bsize);
// UFS2Tool WriteDirBlocks(): `nblocks * bsize` bytes holding ".", ".." and the
// entries, DIRBLKSIZ chunks closed by stretching their last entry and empty
// chunks filled with d_ino = 0 padding entries.
Result<QByteArray> buildDirBlocks(quint32 selfIno, quint32 parentIno, const std::vector<DirEnt>& entries,
                                  int nblocks, int bsize);

} // namespace fw::ufs2
