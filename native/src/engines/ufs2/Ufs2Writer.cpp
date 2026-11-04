// UFS2 image writer. The layout is a port of UFS2Tool 4.0.0 (c123639)
// Ufs2ImageCreator.cs — CreateImageFromDirectory(), WriteFilesystem(),
// PopulateFromDirectory(), ProcessDirectoryContents(), AllocateDataBlock(),
// AllocateAndWriteFile(), WriteInodeAtPosition(), PatchCgAndSuperblock() —
// Copyright (c) 2026, SvenGDK, BSD-2-Clause (LICENSES/UFS2Tool-BSD-2-Clause.txt).
// UFS2Tool formats an empty file system, then adds the files and patches the
// metadata in place; here the final state of every structure is computed up
// front and the image is written once, sequentially.

#include "engines/ufs2/Ufs2Writer.h"

#include "core/FileUtil.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>

#include <algorithm>
#include <climits>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>

namespace fw::ufs2 {

namespace {

constexpr int kEstimatedBytesPerDirEntry = 24; // UFS2Tool EstimatedBytesPerDirEntry
const char* const kTooSmall = "ufs2-image-too-small";

enum class SegKind : quint8 { Recovery, Superblock, CgHeader, InodeTable, Summary, FileData, DirData, Indirect };

struct Segment {
    qint64 offset = 0;
    qint64 length = 0;
    SegKind kind = SegKind::FileData;
    int ref = 0;     // node (FileData), dir (DirData), indirect (Indirect), cg (CgHeader/InodeTable)
    qint64 sub = 0;  // first logical block (FileData/DirData)
};

struct Run {
    qint64 lbn = 0;
    qint64 frag = 0;
    qint64 count = 0;
};

struct PlanInode {
    int node = -1;
    bool isDir = false;
    qint64 size = 0;
    qint64 blocks512 = 0;
    qint16 nlink = 1;
    quint32 dirDepth = 0;
    qint64 db[kNDirect] = {};
    qint64 ib[kNIndirect] = {};
};

struct PlanDir {
    int node = 0;
    quint32 ino = 0;
    quint32 parentIno = 0;
    std::vector<DirEnt> entries;
    int nblocks = 0;
    std::vector<qint64> blocks;
};

struct PlanIndirect {
    qint64 frag = 0;
    int level = 1;
    int fileNode = -1; // level 1: data blocks of this file...
    int dirIdx = -1;   // ...or of this directory
    qint64 firstLbn = 0;
    qint64 count = 0;
    std::vector<qint64> children; // level 2/3: lower-level pointer blocks
};

struct CgCounts {
    int dirs = 0, nbfree = 0, nifree = 0, nffree = 0;
};

qint64 blocks512For(const Geometry& g, qint64 size, int metaBlocks)
{
    // UFS2Tool WriteInodeAtPosition(): files that need indirect blocks count
    // every data block in full; others count the tail at fragment precision.
    const qint64 blocksNeeded = size > 0 ? ceilDiv(size, g.bsize) : 0;
    qint64 dataFrags;
    if (blocksNeeded > kNDirect) {
        dataFrags = blocksNeeded * g.fpb;
    } else {
        dataFrags = (size > 0 ? size / g.bsize : 0) * g.fpb;
        const qint64 tail = size > 0 ? size % g.bsize : 0;
        if (tail > 0)
            dataFrags += ceilDiv(tail, g.fsize);
    }
    return (dataFrags + qint64(metaBlocks) * g.fpb) * (g.fsize / kDevBSize);
}

} // namespace

struct Ufs2Writer::Plan {
    Geometry g;
    qint64 t1 = 0, t2 = 0;
    qint32 fsId0 = 0, fsId1 = 0;
    quint32 nextIno = 3;
    int dirCount = 0;
    std::vector<PlanInode> inodes; // index = ino - 2
    std::vector<PlanDir> dirs;
    std::vector<PlanIndirect> indirects;
    std::vector<std::vector<Run>> runs; // per tree node
    std::vector<Segment> data;          // data, directory and indirect blocks
    std::vector<int> highWater, dirsPerCg;
    std::vector<qint64> tailFree;       // absolute fragments freed after short files
    std::vector<CgCounts> counts;
    QByteArray superblock;
    qint64 dataBytes = 0;
    qint64 emptyFiles = 0;
    // allocation cursor (UFS2Tool currentCg / nextDataFragInCg)
    int curCg = 0;
    int nextFrag = 0;

    int cgLimit(int cg) const { return std::min(g.fpg, g.usableFrags(cg)); }
};

// ---- auto size -------------------------------------------------------------------

qint64 ufs2toolAutoSize(const SourceTree& tree, const FormatParams& p)
{
    const qint64 bsize = p.blockSize;
    const qint64 ppb = bsize / 8;
    qint64 bas = 0;     // blockAlignedSize
    qint64 entries = 0; // files + directories below the root
    auto dirBlocks = [&](const std::vector<int>& kids) {
        const qint64 est = qint64(2 + kids.size()) * kEstimatedBytesPerDirEntry;
        return std::max<qint64>(1, ceilDiv(est, bsize)) * bsize;
    };
    std::vector<int> stack{0};
    while (!stack.empty()) {
        const int d = stack.back();
        stack.pop_back();
        const std::vector<int> kids = tree.liveChildren(d);
        bas += dirBlocks(kids); // this directory (the root included)
        for (int c : kids) {
            ++entries;
            const TreeNode& n = tree.nodes[size_t(c)];
            if (n.isDir()) {
                stack.push_back(c);
                continue;
            }
            const qint64 len = n.size;
            if (len <= 0)
                continue;
            const qint64 fileBlocks = ceilDiv(len, bsize);
            bas += fileBlocks * bsize;
            if (fileBlocks > kNDirect) {
                bas += bsize;
                qint64 remaining = fileBlocks - kNDirect;
                if (remaining > ppb) {
                    remaining -= ppb;
                    const qint64 dind = ceilDiv(remaining, ppb);
                    bas += (1 + dind) * bsize;
                    if (remaining > ppb * ppb) {
                        remaining -= ppb * ppb;
                        const qint64 l2 = ceilDiv(remaining, ppb);
                        const qint64 l1 = ceilDiv(l2, ppb);
                        bas += (1 + l1 + l2) * bsize;
                    }
                }
            }
        }
    }
    // CalculateImageSize(): +10% and +10 MiB, at least 16 blocks, fragment aligned.
    qint64 size = qint64(double(bas) * 1.10) + 10LL * 1024 * 1024;
    size = std::max<qint64>(size, bsize * 16);
    size = alignUp(size, p.fragmentSize);
    // Enough cylinder groups for one inode per entry plus the three reserved ones.
    const qint64 needed = entries + 3;
    const int ipg = inodesPerGroup(p, size);
    const qint64 fpb = p.blockSize / p.fragmentSize;
    const qint64 fpg = qint64(ipg) * fpb;
    const qint64 totalFrags = size / p.fragmentSize;
    const qint64 ncg = std::max<qint64>(1, ceilDiv(totalFrags, fpg));
    if (needed > ncg * ipg) {
        const qint64 requiredBytes = ceilDiv(needed, ipg) * fpg * p.fragmentSize;
        if (requiredBytes > size)
            size = alignUp(requiredBytes, p.fragmentSize);
    }
    return size;
}

// ---- planner -----------------------------------------------------------------------

namespace {

class Planner {
public:
    Planner(Ufs2Writer::Plan& plan, const SourceTree& tree, const Geometry& g) : p(plan), t(tree), g(g) {}
    Status run();

private:
    Result<qint64> alloc();
    void addData(SegKind kind, int ref, qint64 sub, qint64 frag);
    void addFileBlock(int node, qint64 lbn, qint64 frag);
    Status processDir(int node, quint32 selfIno, quint32 depth, std::vector<DirEnt>& entries);
    Status planFile(int node, quint32 ino);
    Status finishDir(int node, quint32 ino, quint32 parentIno, quint32 depth, std::vector<DirEnt>&& entries,
                     std::vector<qint64> preallocated);

    Ufs2Writer::Plan& p;
    const SourceTree& t;
    const Geometry& g;
};

Result<qint64> Planner::alloc()
{
    // AllocateDataBlock(): next whole block in the current group, spilling to the next.
    int limit = p.cgLimit(p.curCg);
    if (p.nextFrag + g.fpb > limit) {
        ++p.curCg;
        if (p.curCg >= g.ncg)
            return Error(ErrorCode::InsufficientSpace, QStringLiteral("The image is too small."), QString::fromLatin1(kTooSmall));
        p.nextFrag = g.dblkno;
        limit = p.cgLimit(p.curCg);
        if (p.nextFrag + g.fpb > limit)
            return Error(ErrorCode::InsufficientSpace, QStringLiteral("The image is too small."), QString::fromLatin1(kTooSmall));
    }
    const qint64 frag = g.cgStartFrag(p.curCg) + p.nextFrag;
    p.nextFrag += g.fpb;
    p.highWater[size_t(p.curCg)] = p.nextFrag;
    return frag;
}

void Planner::addData(SegKind kind, int ref, qint64 sub, qint64 frag)
{
    const qint64 off = frag * g.fsize;
    if (!p.data.empty() && (kind == SegKind::FileData || kind == SegKind::DirData)) {
        Segment& last = p.data.back();
        if (last.kind == kind && last.ref == ref && last.offset + last.length == off
            && last.sub + last.length / g.bsize == sub) {
            last.length += g.bsize;
            return;
        }
    }
    p.data.push_back({off, g.bsize, kind, ref, sub});
}

void Planner::addFileBlock(int node, qint64 lbn, qint64 frag)
{
    auto& runs = p.runs[size_t(node)];
    if (!runs.empty() && runs.back().lbn + runs.back().count == lbn && runs.back().frag + runs.back().count * g.fpb == frag)
        ++runs.back().count;
    else
        runs.push_back({lbn, frag, 1});
    addData(SegKind::FileData, node, lbn, frag);
}

Status Planner::planFile(int node, quint32 ino)
{
    // AllocateAndWriteFile(): 12 direct blocks, then each indirect block is
    // allocated just before the data blocks it points to.
    const TreeNode& n = t.nodes[size_t(node)];
    PlanInode rec;
    rec.node = node;
    rec.size = n.size;
    rec.nlink = 1;
    const qint64 blocksNeeded = n.size > 0 ? ceilDiv(n.size, g.bsize) : 0;
    const qint64 ppb = g.nindir;
    int meta = 0;
    qint64 lbn = 0;
    qint64 lastFrag = 0;
    const qint64 direct = std::min<qint64>(blocksNeeded, kNDirect);
    for (qint64 i = 0; i < direct; ++i) {
        FW_TRY_ASSIGN(qint64 f, alloc());
        rec.db[i] = f;
        addFileBlock(node, lbn++, f);
        lastFrag = f;
    }
    qint64 remaining = blocksNeeded - direct;
    auto level1 = [&](qint64 count) -> Result<qint64> {
        FW_TRY_ASSIGN(qint64 ind, alloc());
        ++meta;
        p.indirects.push_back({ind, 1, node, -1, lbn, count, {}});
        addData(SegKind::Indirect, int(p.indirects.size() - 1), 0, ind);
        for (qint64 j = 0; j < count; ++j) {
            FW_TRY_ASSIGN(qint64 f, alloc());
            addFileBlock(node, lbn++, f);
            lastFrag = f;
        }
        return ind;
    };
    if (remaining > 0) {
        const qint64 single = std::min(remaining, ppb);
        FW_TRY_ASSIGN(qint64 ind, level1(single));
        rec.ib[0] = ind;
        remaining -= single;
    }
    auto level2 = [&](qint64 maxChildren) -> Result<qint64> {
        FW_TRY_ASSIGN(qint64 dind, alloc());
        ++meta;
        p.indirects.push_back({dind, 2, node, -1, 0, 0, {}});
        const int idx = int(p.indirects.size() - 1);
        addData(SegKind::Indirect, idx, 0, dind);
        const qint64 second = std::min(ceilDiv(remaining, ppb), maxChildren);
        for (qint64 i = 0; i < second; ++i) {
            const qint64 thisCount = std::min(remaining, ppb);
            FW_TRY_ASSIGN(qint64 sind, level1(thisCount));
            p.indirects[size_t(idx)].children.push_back(sind);
            remaining -= thisCount;
        }
        return dind;
    };
    if (remaining > 0) {
        // UFS2Tool does not cap this level (it would overflow its buffer
        // beyond nindir^2 blocks, 4 TiB at 64 KiB blocks); FreeBSD caps it.
        FW_TRY_ASSIGN(qint64 dind, level2(ppb));
        rec.ib[1] = dind;
    }
    if (remaining > 0) {
        FW_TRY_ASSIGN(qint64 tind, alloc());
        ++meta;
        p.indirects.push_back({tind, 3, node, -1, 0, 0, {}});
        const int idx = int(p.indirects.size() - 1);
        addData(SegKind::Indirect, idx, 0, tind);
        const qint64 third = std::min(ceilDiv(remaining, ppb * ppb), ppb);
        for (qint64 i = 0; i < third && remaining > 0; ++i) {
            FW_TRY_ASSIGN(qint64 dind, level2(ppb));
            p.indirects[size_t(idx)].children.push_back(dind);
        }
        rec.ib[2] = tind;
        if (remaining > 0)
            return Error(ErrorCode::Unsupported, QStringLiteral("%1 is too large for a UFS2 image.").arg(t.relativePath(node)));
    }
    // Unused tail fragments of a short file's last block are freed.
    if (n.size > 0 && lastFrag != 0 && blocksNeeded <= kNDirect && g.fpb > 1) {
        const qint64 usedFrags = ceilDiv(n.size, g.fsize);
        if (blocksNeeded * g.fpb - usedFrags > 0) {
            qint64 usedInLast = usedFrags % g.fpb;
            if (usedInLast == 0)
                usedInLast = g.fpb;
            for (qint64 tf = usedInLast; tf < g.fpb; ++tf)
                p.tailFree.push_back(lastFrag + tf);
        }
    }
    rec.blocks512 = blocks512For(g, n.size, meta);
    p.inodes[size_t(ino - 2)] = rec;
    p.dataBytes += n.size;
    if (n.size == 0)
        ++p.emptyFiles;
    return success();
}

Status Planner::finishDir(int node, quint32 ino, quint32 parentIno, quint32 depth, std::vector<DirEnt>&& entries,
                          std::vector<qint64> preallocated)
{
    FW_TRY_ASSIGN(int nb, dirBlocksNeeded(entries, g.bsize));
    const int dirIdx = int(p.dirs.size());
    int subdirs = 0;
    for (const DirEnt& e : entries)
        if (e.type == kDtDir)
            ++subdirs;
    PlanDir d;
    d.node = node;
    d.ino = ino;
    d.parentIno = parentIno;
    d.entries = std::move(entries);
    d.nblocks = nb;
    d.blocks = std::move(preallocated);
    p.dirs.push_back(std::move(d));
    for (size_t b = 0; b < p.dirs[size_t(dirIdx)].blocks.size(); ++b)
        addData(SegKind::DirData, dirIdx, qint64(b), p.dirs[size_t(dirIdx)].blocks[b]);
    while (int(p.dirs[size_t(dirIdx)].blocks.size()) < nb) {
        FW_TRY_ASSIGN(qint64 f, alloc());
        const qint64 b = qint64(p.dirs[size_t(dirIdx)].blocks.size());
        p.dirs[size_t(dirIdx)].blocks.push_back(f);
        addData(SegKind::DirData, dirIdx, b, f);
    }
    PlanInode rec;
    rec.node = node;
    rec.isDir = true;
    rec.size = qint64(nb) * g.bsize;
    rec.nlink = qint16(2 + subdirs);
    rec.dirDepth = depth;
    const auto& blocks = p.dirs[size_t(dirIdx)].blocks;
    for (int i = 0; i < std::min(nb, kNDirect); ++i)
        rec.db[i] = blocks[size_t(i)];
    int meta = 0;
    if (nb > kNDirect) {
        // BuildDirectoryAllocation(): one single-indirect block after the data.
        FW_TRY_ASSIGN(qint64 ind, alloc());
        p.indirects.push_back({ind, 1, -1, dirIdx, kNDirect, nb - kNDirect, {}});
        addData(SegKind::Indirect, int(p.indirects.size() - 1), 0, ind);
        rec.ib[0] = ind;
        meta = 1;
        if (nb > kNDirect + g.nindir)
            return Error(ErrorCode::Unsupported, QStringLiteral("A folder has too many entries for this block size."));
    }
    rec.blocks512 = blocks512For(g, rec.size, meta);
    p.inodes[size_t(ino - 2)] = rec;
    return success();
}

Status Planner::processDir(int node, quint32 selfIno, quint32 depth, std::vector<DirEnt>& entries)
{
    // ProcessDirectoryContents(): children in listing order; a sub-directory
    // gets its inode first, then its contents, then its own directory blocks.
    for (int c : t.liveChildren(node)) {
        if (p.nextIno >= g.maxInodes())
            return Error(ErrorCode::InsufficientSpace, QStringLiteral("The image is too small."), QString::fromLatin1(kTooSmall));
        const TreeNode& n = t.nodes[size_t(c)];
        const quint32 ino = p.nextIno++;
        p.inodes.emplace_back();
        if (n.isDir()) {
            std::vector<DirEnt> sub;
            FW_TRY(processDir(c, ino, depth + 1, sub));
            FW_TRY(finishDir(c, ino, selfIno, depth + 1, std::move(sub), {}));
            entries.push_back({ino, kDtDir, n.rawName});
            ++p.dirCount;
            ++p.dirsPerCg[size_t(ino / quint32(g.ipg))];
        } else {
            FW_TRY(planFile(c, ino));
            entries.push_back({ino, kDtReg, n.rawName});
        }
    }
    return success();
}

Status Planner::run()
{
    p.highWater.assign(size_t(g.ncg), 0);
    p.dirsPerCg.assign(size_t(g.ncg), 0);
    p.runs.assign(t.nodes.size(), {});
    p.inodes.clear();
    p.inodes.emplace_back(); // root, inode 2
    p.curCg = 0;
    // CG 0: the summary area (rounded up to whole blocks) and the root's first
    // block are taken by newfs before any file is added.
    const qint64 rootFirst = g.dblkno + g.csFragsBlk;
    p.nextFrag = int(rootFirst + g.fpb);
    p.highWater[0] = p.nextFrag;
    p.dirsPerCg[size_t(kRootIno / quint32(g.ipg))] = 1;
    std::vector<DirEnt> rootEntries;
    FW_TRY(processDir(0, kRootIno, 0, rootEntries));
    FW_TRY(finishDir(0, kRootIno, kRootIno, 0, std::move(rootEntries), {rootFirst}));
    std::stable_sort(p.data.begin(), p.data.end(), [](const Segment& a, const Segment& b) { return a.offset < b.offset; });
    std::sort(p.tailFree.begin(), p.tailFree.end());
    return success();
}

// ---- metadata builders --------------------------------------------------------------

struct CgBuild {
    std::vector<quint8> fragMap;
    CgCounts counts;
    int frsum[kMaxFrag] = {};
    int inodesUsed = 0;
};

CgBuild buildCgMaps(const Ufs2Writer::Plan& p, int cg)
{
    // PatchCgAndSuperblock(): the final bitmaps and counts of one group.
    const Geometry& g = p.g;
    CgBuild b;
    const int usable = g.usableFrags(cg);
    const qint64 first = qint64(cg) * g.ipg;
    b.inodesUsed = p.nextIno > first ? int(std::min<qint64>(p.nextIno - first, g.ipg)) : 0;
    b.counts.nifree = g.ipg - b.inodesUsed;
    b.counts.dirs = p.dirsPerCg[size_t(cg)];
    b.fragMap.assign(size_t(g.fragMapBytes), 0);
    auto setFree = [&](qint64 f) { b.fragMap[size_t(f / 8)] |= quint8(1u << (f % 8)); };
    auto isFree = [&](qint64 f) {
        return f / 8 < g.fragMapBytes && (b.fragMap[size_t(f / 8)] & (1u << (f % 8))) != 0;
    };
    if (cg > 0)
        for (int f = 0; f < g.sblkno && f < usable; ++f)
            setFree(f);
    const int hw = p.highWater[size_t(cg)];
    const int usedData = hw > 0 ? hw - g.dblkno : 0;
    for (int f = g.dblkno + usedData; f < usable; ++f)
        setFree(f);
    if (cg == 0 && g.csFragsBlk > g.csFrags)
        for (int f = g.dblkno + g.csFrags; f < g.dblkno + g.csFragsBlk && f < usable; ++f)
            setFree(f);
    const qint64 cgStart = g.cgStartFrag(cg);
    auto it = std::lower_bound(p.tailFree.begin(), p.tailFree.end(), cgStart);
    for (; it != p.tailFree.end() && *it < cgStart + usable; ++it) {
        const qint64 local = *it - cgStart;
        if (local < qint64(g.fragMapBytes) * 8)
            setFree(local);
    }
    const int totalBlocks = usable / g.fpb;
    for (int blk = 0; blk < totalBlocks; ++blk) {
        int freeIn = 0;
        for (int ff = 0; ff < g.fpb; ++ff)
            if (isFree(qint64(blk) * g.fpb + ff))
                ++freeIn;
        if (freeIn == g.fpb) {
            ++b.counts.nbfree;
            continue;
        }
        b.counts.nffree += freeIn;
        int run = 0;
        for (int ff = 0; ff < g.fpb; ++ff) {
            if (isFree(qint64(blk) * g.fpb + ff)) {
                ++run;
            } else {
                if (run > 0 && run < kMaxFrag)
                    ++b.frsum[run];
                run = 0;
            }
        }
        if (run > 0 && run < kMaxFrag)
            ++b.frsum[run];
    }
    {
        int run = 0;
        for (int f = totalBlocks * g.fpb; f < usable; ++f) {
            if (isFree(f)) {
                ++b.counts.nffree;
                ++run;
            } else {
                if (run > 0 && run < kMaxFrag)
                    ++b.frsum[run];
                run = 0;
            }
        }
        if (run > 0 && run < kMaxFrag)
            ++b.frsum[run];
    }
    return b;
}

QByteArray buildCgHeader(const Ufs2Writer::Plan& p, int cg)
{
    const Geometry& g = p.g;
    const CgBuild b = buildCgMaps(p, cg);
    const int usable = g.usableFrags(cg);
    QByteArray out(g.cgBlockLen, '\0');
    char* o = out.data();
    wr32s(o + 0x04, kCgMagic);
    wr32(o + 0x08, quint32(p.t1 & 0xFFFFFFFF));
    wr32s(o + 0x0C, cg);
    wr32s(o + 0x14, usable); // cg_ndblk
    wr32s(o + 0x18, b.counts.dirs);
    wr32s(o + 0x1C, b.counts.nbfree);
    wr32s(o + 0x20, b.counts.nifree);
    wr32s(o + 0x24, b.counts.nffree);
    for (int i = 0; i < kMaxFrag; ++i)
        wr32s(o + 0x34 + 4 * i, b.frsum[i]);
    wr32s(o + 0x5C, g.inodeMapOff);
    wr32s(o + 0x60, g.fragMapOff);
    wr32s(o + 0x64, g.nextFreeOff);
    const int nclusterblks = g.contigSumSize > 0 ? usable / g.fpb : 0;
    wr32s(o + 0x68, g.clusterSumOff);
    wr32s(o + 0x6C, g.clusterOff);
    wr32s(o + 0x70, nclusterblks);
    wr32s(o + 0x74, g.ipg); // cg_niblk
    int initediblk = std::min(g.ipg, 2 * g.inopb);
    if (b.inodesUsed > 0)
        initediblk = std::min(int(alignUp(b.inodesUsed, g.inopb)), g.ipg);
    wr32s(o + 0x78, initediblk);
    wr64(o + 0x88, p.t1);
    auto* uo = reinterpret_cast<unsigned char*>(o);
    for (int i = 0; i < b.inodesUsed; ++i)
        uo[g.inodeMapOff + i / 8] |= static_cast<unsigned char>(1u << (i % 8));
    std::memcpy(o + g.fragMapOff, b.fragMap.data(), b.fragMap.size());
    if (g.contigSumSize > 0 && nclusterblks > 0) {
        std::vector<quint8> cmap(size_t((nclusterblks + 7) / 8), 0);
        for (int blk = 0; blk < nclusterblks; ++blk) {
            bool allFree = true;
            for (int ff = 0; ff < g.fpb && allFree; ++ff) {
                const int f = blk * g.fpb + ff;
                allFree = f < usable && (b.fragMap[size_t(f / 8)] & (1u << (f % 8))) != 0;
            }
            if (allFree)
                cmap[size_t(blk / 8)] |= quint8(1u << (blk % 8));
        }
        std::memcpy(o + g.clusterOff, cmap.data(), cmap.size());
        std::vector<int> sum(size_t(g.contigSumSize + 1), 0);
        int run = 0;
        for (int blk = 0; blk < nclusterblks; ++blk) {
            if (cmap[size_t(blk / 8)] & (1u << (blk % 8))) {
                ++run;
            } else if (run != 0) {
                ++sum[size_t(std::min(run, g.contigSumSize))];
                run = 0;
            }
        }
        if (run != 0)
            ++sum[size_t(std::min(run, g.contigSumSize))];
        for (int i = 1; i <= g.contigSumSize; ++i)
            wr32s(o + g.clusterSumOff + 4 * i, sum[size_t(i)]);
    }
    return out;
}

QByteArray buildSuperblock(const Ufs2Writer::Plan& p, qint64 ndir, qint64 nbfree, qint64 nifree, qint64 nffree)
{
    // Ufs2Superblock.WriteTo() with the values WriteFilesystem() sets.
    const Geometry& g = p.g;
    QByteArray out(kSuperblockSize, '\0');
    char* o = out.data();
    const qint32 bmask = ~(g.bsize - 1);
    const qint32 fmask = ~(g.fsize - 1);
    wr32s(o + 0x008, g.sblkno);
    wr32s(o + 0x00C, g.cblkno);
    wr32s(o + 0x010, g.iblkno);
    wr32s(o + 0x014, g.dblkno);
    wr32(o + 0x020, quint32(p.t1 & 0xFFFFFFFF));
    wr32(o + 0x024, quint32(g.totalFrags & 0xFFFFFFFF));
    wr32(o + 0x028, quint32(g.dsize & 0xFFFFFFFF));
    wr32s(o + 0x02C, g.ncg);
    wr32s(o + 0x030, g.bsize);
    wr32s(o + 0x034, g.fsize);
    wr32s(o + 0x038, g.fpb);
    wr32s(o + 0x03C, g.params.minFreePercent);
    wr32s(o + 0x048, bmask);
    wr32s(o + 0x04C, fmask);
    wr32s(o + 0x050, g.bshift);
    wr32s(o + 0x054, g.fshift);
    wr32s(o + 0x058, g.maxContig);
    wr32s(o + 0x05C, g.ipg); // fs_maxbpg
    wr32s(o + 0x060, g.fragshift);
    wr32s(o + 0x064, g.fsbtodb);
    wr32s(o + 0x068, kSuperblockSize);
    wr32s(o + 0x074, g.nindir);
    wr32s(o + 0x078, g.inopb);
    wr32s(o + 0x07C, g.fsize / kSectorSize); // fs_old_nspf
    wr32s(o + 0x080, 0);                     // fs_optim = time
    wr32s(o + 0x090, p.fsId0);
    wr32s(o + 0x094, p.fsId1);
    wr32(o + 0x098, quint32(qint64(g.dblkno) & 0xFFFFFFFF)); // fs_old_csaddr
    wr32s(o + 0x09C, g.csSize);
    wr32s(o + 0x0A0, g.cgSize);
    wr32s(o + 0x0B0, g.ncg); // fs_old_ncyl
    wr32s(o + 0x0B4, 1);     // fs_old_cpg
    wr32s(o + 0x0B8, g.ipg);
    wr32s(o + 0x0BC, g.fpg);
    wr32(o + 0x0C0, quint32(ndir & 0xFFFFFFFF));
    wr32(o + 0x0C4, quint32(nbfree & 0xFFFFFFFF));
    wr32(o + 0x0C8, quint32(nifree & 0xFFFFFFFF));
    wr32(o + 0x0CC, quint32(nffree & 0xFFFFFFFF));
    o[0x0D1] = 1;    // fs_clean
    o[0x0D3] = char(0x80); // fs_old_flags = FS_FLAGS_UPDATED
    o[0x0D4] = '/';  // fs_fsmnt
    wr32s(o + 0x35C, kMaxBSize);
    wr64(o + 0x368, g.totalFrags); // fs_providersize
    wr64(o + 0x370, g.metaSpace);
    wr64(o + 0x3E0, kSuperblockOffset); // fs_sblockactualloc
    wr64(o + 0x3E8, kSuperblockOffset); // fs_sblockloc
    wr64(o + 0x3F0, ndir);
    wr64(o + 0x3F8, nbfree);
    wr64(o + 0x400, nifree);
    wr64(o + 0x408, nffree);
    wr64(o + 0x430, p.t1);
    wr64(o + 0x438, g.totalFrags);
    wr64(o + 0x440, g.dsize);
    wr64(o + 0x448, g.dblkno); // fs_csaddr
    wr32s(o + 0x4AC, 16384);   // fs_avgfilesize
    wr32s(o + 0x4B0, 64);      // fs_avgfpdir
    wr64(o + 0x4B8, p.t1);     // fs_mtime
    wr32s(o + 0x524, g.contigSumSize);
    wr32s(o + 0x528, (kNDirect + kNIndirect) * 8); // fs_maxsymlinklen
    wr32s(o + 0x52C, 2);                           // fs_old_inodefmt = FS_44INODEFMT
    wr64(o + 0x530, g.maxFileSize);
    wr64(o + 0x538, ~qint64(bmask));
    wr64(o + 0x540, ~qint64(fmask));
    wr32s(o + 0x54C, -1); // fs_old_postblformat = FS_DYNAMICPOSTBLFMT
    wr32s(o + 0x550, 1);  // fs_old_nrpos
    wr32s(o + 0x55C, kUfs2Magic);
    return out;
}

QByteArray buildRecoverySector(const Ufs2Writer::Plan& p)
{
    // struct fsrecovery in the last 20 bytes of the sector before the superblock.
    const Geometry& g = p.g;
    QByteArray s(kSectorSize, '\0');
    char* o = s.data() + kSectorSize - 20;
    wr32s(o + 0, kUfs2Magic);
    wr32s(o + 4, g.fpg);
    wr32s(o + 8, g.fsbtodb);
    wr32s(o + 12, g.sblkno);
    wr32s(o + 16, g.ncg);
    return s;
}

QByteArray buildSummary(const Ufs2Writer::Plan& p)
{
    const Geometry& g = p.g;
    QByteArray s(qsizetype(g.csFragsBlk) * g.fsize, '\0');
    for (int cg = 0; cg < g.ncg; ++cg) {
        const qsizetype off = qsizetype(cg) * kCsumSize;
        if (off + kCsumSize > s.size())
            break;
        const CgCounts& c = p.counts[size_t(cg)];
        wr32s(s.data() + off + 0, c.dirs);
        wr32s(s.data() + off + 4, c.nbfree);
        wr32s(s.data() + off + 8, c.nifree);
        wr32s(s.data() + off + 12, c.nffree);
    }
    return s;
}

QByteArray buildInodeTable(const Ufs2Writer::Plan& p, int cg)
{
    // SerializeInodeTable(): newfs initialises the first 2 * INOPB inodes of
    // every group with a random generation number (seeded cg + 1, the root
    // inode does not draw one); WriteInodeAtPosition() then writes every
    // used inode in full.
    const Geometry& g = p.g;
    QByteArray out(qsizetype(alignUp(qint64(g.ipg) * kInodeSize, kSectorSize)), '\0');
    char* o = out.data();
    DotNetRandom rng(cg + 1);
    const int inited = std::min(g.ipg, 2 * g.inopb);
    for (int i = 0; i < inited; ++i) {
        if (cg == 0 && i == int(kRootIno))
            continue;
        wr32s(o + qsizetype(i) * kInodeSize + 0x50, rng.next(1, INT_MAX));
    }
    const qint64 first = qint64(cg) * g.ipg;
    const qint64 from = std::max<qint64>(first, kRootIno);
    const qint64 to = std::min<qint64>(p.nextIno, first + g.ipg);
    for (qint64 ino = from; ino < to; ++ino) {
        const PlanInode& r = p.inodes[size_t(ino - 2)];
        Inode in;
        in.mode = quint16((r.isDir ? kIfDir : kIfReg) | kPerm0555);
        in.nlink = r.nlink;
        in.blksize = quint32(g.bsize);
        in.size = r.size;
        in.blocks = r.blocks512;
        in.atime = in.mtime = in.ctime = in.birthtime = p.t2;
        in.gen = 1;
        in.dirdepth = r.isDir ? r.dirDepth : 0;
        for (int i = 0; i < kNDirect; ++i)
            in.db[i] = r.db[i];
        for (int i = 0; i < kNIndirect; ++i)
            in.ib[i] = r.ib[i];
        serializeInode(in, o + (ino - first) * kInodeSize);
    }
    return out;
}

// ---- output pipeline ------------------------------------------------------------------

class OutputPipe {
public:
    OutputPipe(QFile& file, qint64 bufferBytes, int buffers) : m_file(file)
    {
        const int n = std::max(2, buffers);
        m_store.resize(size_t(n));
        for (auto& b : m_store) {
            b.data.resize(qsizetype(bufferBytes));
            m_free.push_back(&b);
        }
        m_cur = m_free.front();
        m_free.pop_front();
        m_cur->offset = 0;
        m_cur->used = 0;
        m_thread = std::thread([this] { writerLoop(); });
    }
    ~OutputPipe() { stop(); }

    qint64 position() const { return m_cur->offset + m_cur->used; }

    // Moves forward to `offset`; small gaps are written as zeros, large ones skipped.
    Status skipTo(qint64 offset)
    {
        const qint64 gap = offset - position();
        if (gap < 0)
            return Error(ErrorCode::Internal, QStringLiteral("Image layout error (overlapping regions)."));
        if (gap <= kZeroFillLimit)
            return zeros(gap);
        FW_TRY(flush());
        m_cur->offset = offset;
        return success();
    }

    Status zeros(qint64 n)
    {
        while (n > 0) {
            FW_TRY_ASSIGN(auto span, reserve(n));
            std::memset(span.first, 0, size_t(span.second));
            commit(span.second);
            n -= span.second;
        }
        return success();
    }

    Status bytes(const char* p, qint64 n)
    {
        while (n > 0) {
            FW_TRY_ASSIGN(auto span, reserve(n));
            std::memcpy(span.first, p, size_t(span.second));
            commit(span.second);
            p += span.second;
            n -= span.second;
        }
        return success();
    }

    Result<std::pair<char*, qint64>> reserve(qint64 want)
    {
        if (m_cur->used == qint64(m_cur->data.size()))
            FW_TRY(flush());
        const qint64 n = std::min(want, qint64(m_cur->data.size()) - m_cur->used);
        return std::make_pair(m_cur->data.data() + m_cur->used, n);
    }
    void commit(qint64 n) { m_cur->used += n; }

    Status flush()
    {
        if (m_cur->used == 0)
            return writerStatus();
        const qint64 next = position();
        Buf* b = m_cur;
        {
            std::unique_lock lock(m_mutex);
            m_full.push_back(b);
            m_cv.notify_all();
            m_cv.wait(lock, [this] { return !m_free.empty(); });
            m_cur = m_free.front();
            m_free.pop_front();
        }
        m_cur->offset = next;
        m_cur->used = 0;
        return writerStatus();
    }

    Status finish(qint64 finalLength)
    {
        FW_TRY(flush());
        stop();
        FW_TRY(writerStatus());
        if (m_file.size() < finalLength && !m_file.resize(finalLength))
            return Error(ErrorCode::IoError, QStringLiteral("Could not finish the image file."), m_file.errorString());
        if (!m_file.flush())
            return Error(ErrorCode::IoError, QStringLiteral("Could not finish the image file."), m_file.errorString());
        return success();
    }

    void stop()
    {
        {
            std::lock_guard lock(m_mutex);
            m_done = true;
            m_cv.notify_all();
        }
        if (m_thread.joinable())
            m_thread.join();
    }

private:
    static constexpr qint64 kZeroFillLimit = 4 * 1024 * 1024;
    struct Buf {
        QByteArray data;
        qint64 offset = 0;
        qint64 used = 0;
    };

    Status writerStatus()
    {
        std::lock_guard lock(m_mutex);
        if (m_error)
            return *m_error;
        return success();
    }

    void writerLoop()
    {
        qint64 filePos = 0;
        for (;;) {
            Buf* b = nullptr;
            {
                std::unique_lock lock(m_mutex);
                m_cv.wait(lock, [this] { return !m_full.empty() || m_done; });
                if (m_full.empty())
                    return;
                b = m_full.front();
                m_full.pop_front();
            }
            bool failed;
            {
                std::lock_guard lock(m_mutex);
                failed = m_error.has_value();
            }
            if (!failed) {
                QString err;
                if (filePos != b->offset && !m_file.seek(b->offset))
                    err = m_file.errorString();
                qint64 done = 0;
                while (err.isEmpty() && done < b->used) {
                    const qint64 n = m_file.write(b->data.constData() + done, b->used - done);
                    if (n <= 0)
                        err = m_file.errorString();
                    else
                        done += n;
                }
                filePos = b->offset + done;
                if (!err.isEmpty()) {
                    std::lock_guard lock(m_mutex);
                    m_error = Error(ErrorCode::IoError,
                                    QStringLiteral("Could not write the image. The drive may be full or disconnected."), err);
                }
            }
            std::lock_guard lock(m_mutex);
            m_free.push_back(b);
            m_cv.notify_all();
        }
    }

    QFile& m_file;
    std::vector<Buf> m_store;
    std::deque<Buf*> m_free, m_full;
    Buf* m_cur = nullptr;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_done = false;
    std::optional<Error> m_error;
    std::thread m_thread;
};

} // namespace

// ---- Ufs2Writer -----------------------------------------------------------------------

Ufs2Writer::Ufs2Writer(const SourceTree& tree, WriterOptions options) : m_tree(tree), m_opt(std::move(options)) {}
Ufs2Writer::~Ufs2Writer() = default;

Result<PlanSummary> Ufs2Writer::plan()
{
    FW_TRY(validateParams(m_opt.params));
    if (m_tree.nodes.empty() || !m_tree.nodes[0].isDir())
        return Error(ErrorCode::InvalidArgument, QStringLiteral("Nothing to put into the image."));
    const qint64 t1 = m_opt.formatTime >= 0 ? m_opt.formatTime : QDateTime::currentSecsSinceEpoch();
    const qint64 t2 = m_opt.populateTime >= 0 ? m_opt.populateTime : t1;
    const qint64 autoSize = ufs2toolAutoSize(m_tree, m_opt.params);
    qint64 size = m_opt.sizeBytes > 0 ? alignUp(m_opt.sizeBytes, m_opt.params.fragmentSize) : autoSize;
    bool enlarged = false;
    for (int attempt = 0;; ++attempt) {
        FW_TRY_ASSIGN(Geometry g, computeGeometry(m_opt.params, size));
        auto plan = std::make_unique<Plan>();
        plan->g = g;
        plan->t1 = t1;
        plan->t2 = t2;
        plan->fsId0 = qint32(quint32(t1 & 0xFFFFFFFF));
        plan->fsId1 = m_opt.fsId1 >= 0 ? qint32(quint32(m_opt.fsId1 & 0xFFFFFFFF))
                                       : qint32(quint32((t1 >> 16) ^ QRandomGenerator::global()->generate()));
        Planner planner(*plan, m_tree, plan->g);
        Status st = planner.run();
        if (st.ok()) {
            m_plan = std::move(plan);
            break;
        }
        const bool tooSmall = st.error().detail == QLatin1String(kTooSmall);
        if (!tooSmall)
            return st.takeError();
        if (m_opt.sizeBytes > 0)
            return Error(ErrorCode::InsufficientSpace,
                         QStringLiteral("The image size you set is too small for these files."));
        if (attempt >= 40)
            return Error(ErrorCode::Internal, QStringLiteral("Could not find an image size that fits these files."));
        // UFS2Tool would stop with "Image is too small"; grow by an eighth instead.
        size = alignUp(size + std::max<qint64>(size / 8, 16LL * 1024 * 1024), m_opt.params.fragmentSize);
        enlarged = true;
    }

    Plan& p = *m_plan;
    const Geometry& g = p.g;
    p.counts.resize(size_t(g.ncg));
    qint64 nbfree = 0, nffree = 0, nifree = 0;
    for (int cg = 0; cg < g.ncg; ++cg) {
        p.counts[size_t(cg)] = buildCgMaps(p, cg).counts;
        nbfree += p.counts[size_t(cg)].nbfree;
        nffree += p.counts[size_t(cg)].nffree;
        nifree += p.counts[size_t(cg)].nifree;
    }
    p.superblock = buildSuperblock(p, 1 + p.dirCount, nbfree, nifree, nffree);

    m_summary = PlanSummary();
    m_summary.geometry = g;
    m_summary.autoSizeBytes = autoSize;
    m_summary.enlarged = enlarged;
    m_summary.inodesUsed = p.nextIno;
    m_summary.dataBytes = p.dataBytes;
    m_summary.files = m_tree.fileCount;
    m_summary.dirs = m_tree.dirCount;
    // A last group too short for its own header still gets one (UFS2Tool
    // writes it past fs_size), so the file can be longer than fs_size.
    qint64 end = g.totalBytes;
    {
        const int last = g.ncg - 1;
        const qint64 base = g.cgStartFrag(last) * g.fsize;
        end = std::max(end, base + qint64(g.iblkno) * g.fsize + alignUp(qint64(g.ipg) * kInodeSize, kSectorSize));
    }
    m_summary.imageBytes = end;
    return m_summary;
}

Result<WriteStats> Ufs2Writer::write(const QString& path, const CancellationToken& token, ProgressReporter* progress)
{
    if (!m_plan)
        FW_TRY(plan());
    const Plan& p = *m_plan;
    const Geometry& g = p.g;
    QElapsedTimer timer;
    timer.start();

    // Every region of the image, front to back.
    std::vector<Segment> segs;
    segs.reserve(p.data.size() + size_t(g.ncg) * 4 + 3);
    segs.push_back({kSuperblockOffset - kSectorSize, kSectorSize, SegKind::Recovery, 0, 0});
    segs.push_back({kSuperblockOffset, kSuperblockSize, SegKind::Superblock, 0, 0});
    for (int cg = 0; cg < g.ncg; ++cg) {
        const qint64 base = g.cgStartFrag(cg) * g.fsize;
        const qint64 cgEnd = base + qint64(g.usableFrags(cg)) * g.fsize;
        if (cg > 0) {
            const qint64 sbOff = base + qint64(g.sblkno) * g.fsize;
            if (sbOff + kSuperblockSize <= cgEnd)
                segs.push_back({sbOff, kSuperblockSize, SegKind::Superblock, cg, 0});
        }
        segs.push_back({base + qint64(g.cblkno) * g.fsize, g.cgBlockLen, SegKind::CgHeader, cg, 0});
        segs.push_back({base + qint64(g.iblkno) * g.fsize, alignUp(qint64(g.ipg) * kInodeSize, kSectorSize),
                        SegKind::InodeTable, cg, 0});
        if (cg == 0)
            segs.push_back({qint64(g.dblkno) * g.fsize, qint64(g.csFragsBlk) * g.fsize, SegKind::Summary, 0, 0});
    }
    segs.insert(segs.end(), p.data.begin(), p.data.end());
    std::stable_sort(segs.begin(), segs.end(), [](const Segment& a, const Segment& b) { return a.offset < b.offset; });
    for (size_t i = 1; i < segs.size(); ++i)
        if (segs[i - 1].offset + segs[i - 1].length > segs[i].offset)
            return Error(ErrorCode::Internal, QStringLiteral("Image layout error (overlapping regions at %1).").arg(segs[i].offset));

    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile out(toLongPath(path));
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return Error(ErrorCode::IoError, QStringLiteral("Could not create %1.").arg(QDir::toNativeSeparators(path)),
                     out.errorString());

    const qint64 bufBytes = std::max<qint64>(alignUp(m_opt.ioBufferBytes, g.bsize), g.bsize);
    OutputPipe pipe(out, bufBytes, m_opt.ioBuffers);
    const QByteArray recovery = buildRecoverySector(p);

    std::unique_ptr<ContentStream> stream;
    int streamNode = -1;
    qint64 streamDone = 0;
    QByteArray dirCache;
    int dirCacheIdx = -1;
    QByteArray block(g.bsize, '\0');
    qint64 segCount = 0;

    auto failRead = [&](int node, const QString& what) {
        return Error(ErrorCode::IoError, QStringLiteral("%1 %2").arg(m_tree.relativePath(node), what));
    };

    for (const Segment& s : segs) {
        if ((++segCount & 0x3F) == 0 && !token.checkpoint())
            return Error::cancelled();
        FW_TRY(pipe.skipTo(s.offset));
        switch (s.kind) {
        case SegKind::Recovery:
            FW_TRY(pipe.bytes(recovery.constData(), recovery.size()));
            break;
        case SegKind::Superblock:
            FW_TRY(pipe.bytes(p.superblock.constData(), p.superblock.size()));
            break;
        case SegKind::CgHeader: {
            const QByteArray h = buildCgHeader(p, s.ref);
            FW_TRY(pipe.bytes(h.constData(), h.size()));
            break;
        }
        case SegKind::InodeTable: {
            const QByteArray t = buildInodeTable(p, s.ref);
            FW_TRY(pipe.bytes(t.constData(), t.size()));
            break;
        }
        case SegKind::Summary: {
            const QByteArray sum = buildSummary(p);
            FW_TRY(pipe.bytes(sum.constData(), sum.size()));
            break;
        }
        case SegKind::DirData: {
            if (dirCacheIdx != s.ref) {
                const PlanDir& d = p.dirs[size_t(s.ref)];
                FW_TRY_ASSIGN(dirCache, buildDirBlocks(d.ino, d.parentIno, d.entries, d.nblocks, g.bsize));
                dirCacheIdx = s.ref;
            }
            FW_TRY(pipe.bytes(dirCache.constData() + s.sub * g.bsize, s.length));
            break;
        }
        case SegKind::Indirect: {
            const PlanIndirect& ind = p.indirects[size_t(s.ref)];
            std::memset(block.data(), 0, size_t(block.size()));
            if (ind.level == 1 && ind.dirIdx >= 0) {
                const auto& blocks = p.dirs[size_t(ind.dirIdx)].blocks;
                for (qint64 k = 0; k < ind.count; ++k)
                    wr64(block.data() + 8 * k, blocks[size_t(ind.firstLbn + k)]);
            } else if (ind.level == 1) {
                const auto& runs = p.runs[size_t(ind.fileNode)];
                auto r = std::upper_bound(runs.begin(), runs.end(), ind.firstLbn,
                                          [](qint64 lbn, const Run& run) { return lbn < run.lbn; });
                if (r == runs.begin())
                    return Error(ErrorCode::Internal, QStringLiteral("Image layout error (indirect block)."));
                --r;
                for (qint64 k = 0; k < ind.count; ++k) {
                    const qint64 lbn = ind.firstLbn + k;
                    while (r != runs.end() && lbn >= r->lbn + r->count)
                        ++r;
                    if (r == runs.end())
                        return Error(ErrorCode::Internal, QStringLiteral("Image layout error (indirect block)."));
                    wr64(block.data() + 8 * k, r->frag + (lbn - r->lbn) * g.fpb);
                }
            } else {
                for (size_t k = 0; k < ind.children.size(); ++k)
                    wr64(block.data() + 8 * k, ind.children[k]);
            }
            FW_TRY(pipe.bytes(block.constData(), g.bsize));
            break;
        }
        case SegKind::FileData: {
            const TreeNode& n = m_tree.nodes[size_t(s.ref)];
            if (streamNode != s.ref) {
                if (stream)
                    return Error(ErrorCode::Internal, QStringLiteral("Image layout error (interleaved files)."));
                FW_TRY_ASSIGN(stream, openContent(m_tree, s.ref));
                streamNode = s.ref;
                streamDone = 0;
                if (progress)
                    progress->setCurrentItem(m_tree.relativePath(s.ref));
            }
            const qint64 fileOff = s.sub * g.bsize;
            if (fileOff != streamDone)
                return Error(ErrorCode::Internal, QStringLiteral("Image layout error (file order)."));
            const qint64 dataLen = std::min(n.size - fileOff, s.length);
            qint64 copied = 0;
            while (copied < dataLen) {
                if (!token.checkpoint())
                    return Error::cancelled();
                FW_TRY_ASSIGN(auto span, pipe.reserve(dataLen - copied));
                FW_TRY_ASSIGN(qint64 got, stream->read(span.first, span.second));
                if (got <= 0)
                    return failRead(s.ref, QStringLiteral("became shorter while the image was being built."));
                pipe.commit(got);
                copied += got;
                if (progress)
                    progress->advance(got);
            }
            FW_TRY(pipe.zeros(s.length - dataLen));
            streamDone += dataLen;
            if (streamDone >= n.size) {
                char probe;
                FW_TRY_ASSIGN(qint64 extra, stream->read(&probe, 1));
                if (extra > 0)
                    return failRead(s.ref, QStringLiteral("became larger while the image was being built."));
                stream.reset();
                streamNode = -1;
                if (progress)
                    progress->advanceFiles(1);
            }
            break;
        }
        }
    }
    if (stream)
        return Error(ErrorCode::Internal, QStringLiteral("Image layout error (unfinished file)."));
    // Files without data blocks (empty files) are done as well.
    if (progress && p.emptyFiles > 0)
        progress->advanceFiles(p.emptyFiles);
    FW_TRY(pipe.finish(m_summary.imageBytes));
    out.close();
    if (out.error() != QFileDevice::NoError)
        return Error(ErrorCode::IoError, QStringLiteral("Could not finish the image file."), out.errorString());

    WriteStats st;
    st.imageBytes = m_summary.imageBytes;
    st.dataBytes = p.dataBytes;
    st.elapsedMs = timer.elapsed();
    st.bytesPerSec = st.elapsedMs > 0 ? double(p.dataBytes) * 1000.0 / double(st.elapsedMs) : 0.0;
    return st;
}

} // namespace fw::ufs2
