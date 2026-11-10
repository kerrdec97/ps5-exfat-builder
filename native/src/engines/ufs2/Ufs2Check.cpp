#include "engines/ufs2/Ufs2Check.h"

#include "core/FileUtil.h"

#include <QSet>

#include <algorithm>
#include <deque>
#include <unordered_map>

namespace fw::ufs2 {

QString CheckReport::summary() const
{
    if (ok())
        return QStringLiteral("File system consistent: %1 and %2.")
            .arg(counted(files, "file", "files"), counted(dirs, "folder", "folders"));
    return QStringLiteral("%1 file system problem%2 found. First: %3")
        .arg(formatCount(problemCount), problemCount == 1 ? QString() : QStringLiteral("s"),
             problems.isEmpty() ? QString() : problems.first());
}

namespace {

class Bits {
public:
    explicit Bits(qint64 n = 0) : m_bits(size_t((n + 7) / 8), 0) {}
    bool test(qint64 i) const { return (m_bits[size_t(i >> 3)] >> (i & 7)) & 1; }
    void set(qint64 i) { m_bits[size_t(i >> 3)] |= quint8(1u << (i & 7)); }

private:
    std::vector<quint8> m_bits;
};

inline bool bitAt(const char* base, qint64 i) { return (quint8(base[i >> 3]) >> (i & 7)) & 1; }

struct CgSum {
    qint32 ndir = 0, nbfree = 0, nifree = 0, nffree = 0;
    bool valid = false;
};

struct InodeInfo {
    bool isDir = false;
    qint16 nlink = 0;
    int linkRefs = 0;
};

} // namespace

Result<CheckReport> checkImage(const Ufs2Reader& r, const CancellationToken& token, ProgressReporter* progress,
                               int maxProblems)
{
    const SuperblockInfo& sb = r.sb();
    CheckReport rep;
    auto add = [&](const QString& s) {
        ++rep.problemCount;
        if (rep.problems.size() < maxProblems)
            rep.problems << s;
    };
    const qint64 nfrags = sb.size;
    const quint32 ninodes = sb.inodeCount();
    // The maps below are sized from the superblock. Group maps that cannot
    // fit their header would fail for every group anyway; stop before
    // allocating for inode counts the image cannot back.
    if (ceilDiv(sb.ipg, 8) > sb.cgsize || ceilDiv(sb.fpg, 8) > sb.cgsize) {
        add(QStringLiteral("The cylinder group maps do not fit their headers."));
        return rep;
    }
    Bits freeMap(nfrags), usedMap(nfrags), inoUsed(ninodes);
    std::vector<CgSum> cgs(size_t(sb.ncg));

    // ---- cylinder groups ---------------------------------------------------------
    QByteArray hdr;
    for (int cg = 0; cg < sb.ncg; ++cg) {
        if ((cg & 63) == 0 && !token.checkpoint())
            return Error::cancelled();
        const qint64 base = qint64(cg) * sb.fpg;
        const int usable = sb.usableFrags(cg);
        const qint64 off = (base + sb.cblkno) * qint64(sb.fsize);
        if (off + kCgHeaderBase > r.fileSize()) {
            add(QStringLiteral("Cylinder group %1 header is outside the image.").arg(cg));
            continue;
        }
        hdr.resize(qsizetype(std::min<qint64>(kSectorSize, r.fileSize() - off)));
        FW_TRY(r.readAt(off, hdr.data(), hdr.size()));
        if (rd32s(hdr.constData() + 0x04) != kCgMagic) {
            add(QStringLiteral("Cylinder group %1 has a bad header.").arg(cg));
            continue;
        }
        const qint32 nextfree = rd32s(hdr.constData() + 0x64);
        const qint32 iusedoff = rd32s(hdr.constData() + 0x5C);
        const qint32 freeoff = rd32s(hdr.constData() + 0x60);
        if (nextfree < kCgHeaderBase || nextfree > sb.cgsize || iusedoff < kCgHeaderBase
            || iusedoff + ceilDiv(sb.ipg, 8) > nextfree || freeoff < kCgHeaderBase || freeoff + ceilDiv(sb.fpg, 8) > nextfree) {
            add(QStringLiteral("Cylinder group %1 has invalid map offsets.").arg(cg));
            continue;
        }
        if (nextfree > hdr.size()) {
            if (off + nextfree > r.fileSize()) {
                add(QStringLiteral("Cylinder group %1 header is outside the image.").arg(cg));
                continue;
            }
            hdr.resize(nextfree);
            FW_TRY(r.readAt(off, hdr.data(), nextfree));
        }
        const char* h = hdr.constData();
        if (rd32s(h + 0x0C) != cg)
            add(QStringLiteral("Cylinder group %1 header says it is group %2.").arg(cg).arg(rd32s(h + 0x0C)));
        if (rd32s(h + 0x14) != usable)
            add(QStringLiteral("Cylinder group %1 size is %2 fragments, should be %3.").arg(cg).arg(rd32s(h + 0x14)).arg(usable));
        CgSum& c = cgs[size_t(cg)];
        c.ndir = rd32s(h + 0x18);
        c.nbfree = rd32s(h + 0x1C);
        c.nifree = rd32s(h + 0x20);
        c.nffree = rd32s(h + 0x24);
        c.valid = true;
        int usedInodes = 0;
        for (int i = 0; i < sb.ipg; ++i) {
            if (bitAt(h + iusedoff, i)) {
                inoUsed.set(qint64(cg) * sb.ipg + i);
                ++usedInodes;
            }
        }
        if (c.nifree != sb.ipg - usedInodes)
            add(QStringLiteral("Cylinder group %1 counts %2 free inodes, its map has %3.").arg(cg).arg(c.nifree).arg(sb.ipg - usedInodes));
        bool pastEnd = false;
        for (int f = 0; f < sb.fpg; ++f) {
            if (!bitAt(h + freeoff, f))
                continue;
            if (f >= usable)
                pastEnd = true;
            else
                freeMap.set(base + f);
        }
        if (pastEnd)
            add(QStringLiteral("Cylinder group %1 marks fragments past its end as free.").arg(cg));
        // Free block / fragment counts and the fragment summary from the map.
        int nbfree = 0, nffree = 0;
        qint32 frsum[kMaxFrag] = {};
        auto isFree = [&](int f) { return bitAt(h + freeoff, f); };
        const int nblk = usable / sb.frag;
        for (int b = 0; b < nblk; ++b) {
            int n = 0;
            for (int k = 0; k < sb.frag; ++k)
                n += isFree(b * sb.frag + k) ? 1 : 0;
            if (n == sb.frag) {
                ++nbfree;
                continue;
            }
            nffree += n;
            int run = 0;
            for (int k = 0; k < sb.frag; ++k) {
                if (isFree(b * sb.frag + k)) {
                    ++run;
                } else {
                    if (run > 0 && run < kMaxFrag)
                        ++frsum[run];
                    run = 0;
                }
            }
            if (run > 0 && run < kMaxFrag)
                ++frsum[run];
        }
        {
            int run = 0;
            for (int f = nblk * sb.frag; f < usable; ++f) {
                if (isFree(f)) {
                    ++nffree;
                    ++run;
                } else {
                    if (run > 0 && run < kMaxFrag)
                        ++frsum[run];
                    run = 0;
                }
            }
            if (run > 0 && run < kMaxFrag)
                ++frsum[run];
        }
        if (nbfree != c.nbfree || nffree != c.nffree)
            add(QStringLiteral("Cylinder group %1 counts %2 free blocks and %3 free fragments, its map has %4 and %5.")
                    .arg(cg).arg(c.nbfree).arg(c.nffree).arg(nbfree).arg(nffree));
        for (int i = 1; i < kMaxFrag; ++i) {
            if (rd32s(h + 0x34 + 4 * i) != frsum[i]) {
                add(QStringLiteral("Cylinder group %1 has a wrong fragment summary.").arg(cg));
                break;
            }
        }
        if (sb.contigsumsize > 0) {
            const qint32 clusteroff = rd32s(h + 0x6C);
            const qint32 ncl = rd32s(h + 0x70);
            if (ncl != usable / sb.frag || clusteroff < kCgHeaderBase || clusteroff + ceilDiv(ncl, 8) > nextfree) {
                add(QStringLiteral("Cylinder group %1 has a bad cluster map.").arg(cg));
            } else {
                for (int b = 0; b < ncl; ++b) {
                    bool all = true;
                    for (int k = 0; k < sb.frag && all; ++k)
                        all = isFree(b * sb.frag + k);
                    if (bitAt(h + clusteroff, b) != all) {
                        add(QStringLiteral("Cylinder group %1 cluster map disagrees with its block map.").arg(cg));
                        break;
                    }
                }
            }
        }
    }

    // ---- summary area and totals -------------------------------------------------------
    {
        const qint64 len = ceilDiv(sb.cssize, sb.fsize) * sb.fsize;
        QByteArray cs(qsizetype(len), '\0');
        FW_TRY(r.readAt(sb.csaddr * sb.fsize, cs.data(), len));
        qint64 ndir = 0, nbfree = 0, nifree = 0, nffree = 0;
        for (int cg = 0; cg < sb.ncg; ++cg) {
            const char* p = cs.constData() + qsizetype(cg) * kCsumSize;
            const CgSum& c = cgs[size_t(cg)];
            if (c.valid && (rd32s(p) != c.ndir || rd32s(p + 4) != c.nbfree || rd32s(p + 8) != c.nifree || rd32s(p + 12) != c.nffree))
                add(QStringLiteral("The summary of cylinder group %1 disagrees with the group.").arg(cg));
            ndir += c.ndir;
            nbfree += c.nbfree;
            nifree += c.nifree;
            nffree += c.nffree;
        }
        if (ndir != sb.csNdir || nbfree != sb.csNbfree || nifree != sb.csNifree || nffree != sb.csNffree)
            add(QStringLiteral("The superblock totals disagree with the cylinder groups."));
    }

    // ---- superblock copies ------------------------------------------------------------
    {
        QByteArray primary(0x600, '\0');
        FW_TRY(r.readAt(r.superblockOffset(), primary.data(), primary.size()));
        static const int fields[] = {0x008, 0x00C, 0x010, 0x014, 0x02C, 0x030, 0x034, 0x038, 0x09C, 0x0A0, 0x0B8, 0x0BC, 0x55C};
        QByteArray copy(0x600, '\0');
        for (int cg = 1; cg < sb.ncg; ++cg) {
            const qint64 off = (qint64(cg) * sb.fpg + sb.sblkno) * sb.fsize;
            const qint64 cgEnd = (qint64(cg) * sb.fpg + sb.usableFrags(cg)) * sb.fsize;
            if (off + sb.sbsize > cgEnd || off + copy.size() > r.fileSize())
                continue;
            FW_TRY(r.readAt(off, copy.data(), copy.size()));
            bool same = rd64(copy.constData() + 0x438) == rd64(primary.constData() + 0x438)
                        && rd64(copy.constData() + 0x448) == rd64(primary.constData() + 0x448);
            for (int f : fields)
                same = same && rd32(copy.constData() + f) == rd32(primary.constData() + f);
            if (!same)
                add(QStringLiteral("The superblock copy in cylinder group %1 differs from the primary.").arg(cg));
        }
    }

    // ---- metadata areas -------------------------------------------------------------------
    qint64 dupCount = 0, freeUsed = 0;
    auto markUsed = [&](qint64 first, qint64 count) {
        for (qint64 f = first; f < first + count && f < nfrags; ++f) {
            if (f < 0)
                continue;
            if (usedMap.test(f))
                ++dupCount;
            else
                usedMap.set(f);
            if (freeMap.test(f))
                ++freeUsed;
        }
    };
    for (int cg = 0; cg < sb.ncg; ++cg) {
        const qint64 base = qint64(cg) * sb.fpg;
        const int usable = sb.usableFrags(cg);
        if (cg == 0)
            markUsed(0, std::min(sb.dblkno, usable));
        else if (usable > sb.sblkno)
            markUsed(base + sb.sblkno, std::min(sb.dblkno, usable) - sb.sblkno);
    }
    markUsed(sb.csaddr, ceilDiv(sb.cssize, sb.fsize));
    if (dupCount || freeUsed)
        add(QStringLiteral("File system metadata overlaps itself or is marked free."));
    dupCount = freeUsed = 0;

    // ---- directory tree -----------------------------------------------------------------------
    std::unordered_map<quint32, InodeInfo> seen;
    std::deque<std::pair<quint32, quint32>> queue; // (dir, parent)
    // Blocks the inodes claim and bytes the folders hold. A sound image
    // cannot exceed its own size with either; an image whose inodes or
    // folders share blocks could otherwise keep the check busy for hours.
    qint64 claimedFrags = 0, dirBytesTotal = 0;
    bool overClaimed = false;
    auto visitInode = [&](quint32 ino) -> Status {
        FW_TRY_ASSIGN(Inode in, r.readInode(ino));
        InodeInfo& info = seen[ino];
        info.nlink = in.nlink;
        info.isDir = in.isDir();
        if (!in.isDir() && !in.isReg() && !in.isLink()) {
            add(QStringLiteral("Inode %1 has an unknown file type (mode %2).").arg(ino).arg(in.mode, 0, 8));
            return success();
        }
        auto b = r.blocks(in, ino, &token);
        if (!b.ok()) {
            if (b.error().isCancelled())
                return b.takeError();
            add(b.error().message);
            return success();
        }
        const qint64 before = dupCount, beforeFree = freeUsed;
        qint64 frags = 0;
        for (const BlockRef& br : b->data) {
            markUsed(br.frag, br.nfrags);
            frags += br.nfrags;
        }
        for (qint64 p : b->indirect) {
            markUsed(p, sb.frag);
            frags += sb.frag;
        }
        if (dupCount != before)
            add(QStringLiteral("Inode %1 uses blocks that another inode or the metadata also uses.").arg(ino));
        if (freeUsed != beforeFree)
            add(QStringLiteral("Inode %1 uses blocks that are marked free.").arg(ino));
        if (frags * (sb.fsize / kDevBSize) != in.blocks)
            add(QStringLiteral("Inode %1 block count is %2, should be %3.").arg(ino).arg(in.blocks).arg(frags * (sb.fsize / kDevBSize)));
        if (b->strayPointers > 0)
            add(QStringLiteral("Inode %1 has block pointers past the end of the file.").arg(ino));
        claimedFrags += frags;
        overClaimed = claimedFrags > nfrags;
        if (in.isDir())
            ++rep.dirs;
        else {
            ++rep.files;
            rep.bytes += in.size;
        }
        ++rep.inodes;
        if (progress)
            progress->advance(1);
        return success();
    };

    FW_TRY(visitInode(kRootIno));
    if (!seen[kRootIno].isDir) {
        add(QStringLiteral("The root inode is not a directory."));
        return rep;
    }
    queue.emplace_back(kRootIno, kRootIno);
    while (!queue.empty() && !overClaimed) {
        const auto [dir, parent] = queue.front();
        queue.pop_front();
        if (!token.checkpoint())
            return Error::cancelled();
        qint64 dirBytes = 0;
        auto entries = r.readDirectory(dir, true, &dirBytes);
        dirBytesTotal += dirBytes;
        if (dirBytesTotal > r.fileSize()) {
            overClaimed = true;
            break;
        }
        if (!entries.ok()) {
            add(entries.error().message);
            continue;
        }
        const auto& es = entries.value();
        if (es.size() < 2 || es[0].rawName != "." || es[0].ino != dir)
            add(QStringLiteral("Directory inode %1 does not start with a correct \".\" entry.").arg(dir));
        if (es.size() < 2 || es[1].rawName != ".." || es[1].ino != parent)
            add(QStringLiteral("Directory inode %1 does not have a correct \"..\" entry.").arg(dir));
        QSet<QByteArray> names;
        for (size_t i = 0; i < es.size(); ++i) {
            const DirEntry& e = es[i];
            const bool dot = e.rawName == "." || e.rawName == "..";
            // A kernel would only ever find one of two entries with one name.
            if (!dot && names.contains(e.rawName))
                add(QStringLiteral("Directory inode %1 lists the name \"%2\" twice.").arg(dir).arg(e.name));
            names.insert(e.rawName);
            if (dot) {
                if (i >= 2)
                    add(QStringLiteral("Directory inode %1 has an extra \"%2\" entry.").arg(dir).arg(e.name));
                if (seen.count(e.ino))
                    ++seen[e.ino].linkRefs;
                continue;
            }
            const bool known = seen.count(e.ino) > 0;
            if (!known) {
                FW_TRY(visitInode(e.ino));
                if (overClaimed)
                    break;
                if (seen[e.ino].isDir)
                    queue.emplace_back(e.ino, dir);
            } else if (seen[e.ino].isDir) {
                add(QStringLiteral("Directory inode %1 is linked more than once.").arg(e.ino));
            }
            InodeInfo& info = seen[e.ino];
            ++info.linkRefs;
            const quint8 want = info.isDir ? kDtDir : kDtReg;
            if (e.type != kDtUnknown && e.type != want && e.type != kDtLnk)
                add(QStringLiteral("The entry for inode %1 has the wrong type.").arg(e.ino));
        }
    }
    if (overClaimed) {
        add(QStringLiteral("The files and folders use more blocks than the image holds; the rest was not checked."));
        return rep;
    }
    // "." of every directory and ".." of its sub-directories were counted when
    // their own directory was read; the root's ".." points at itself.
    for (const auto& [ino, info] : seen)
        if (info.nlink != info.linkRefs)
            add(QStringLiteral("Inode %1 has link count %2, should be %3.").arg(ino).arg(info.nlink).arg(info.linkRefs));

    // ---- maps against usage ----------------------------------------------------------------
    {
        qint64 lost = 0, unmarked = 0;
        quint32 firstLost = 0, firstUnmarked = 0;
        for (quint32 ino = 0; ino < ninodes; ++ino) {
            const bool expected = ino < kRootIno || seen.count(ino) > 0;
            const bool marked = inoUsed.test(ino);
            if (marked && !expected) {
                if (!lost++)
                    firstLost = ino;
            } else if (!marked && expected) {
                if (!unmarked++)
                    firstUnmarked = ino;
            }
        }
        if (lost)
            add(QStringLiteral("%1 inodes are marked in use but are not in any folder (first: %2).").arg(lost).arg(firstLost));
        if (unmarked)
            add(QStringLiteral("%1 inodes are in use but marked free (first: %2).").arg(unmarked).arg(firstUnmarked));
        qint64 leaked = 0;
        qint64 firstLeak = -1;
        for (qint64 f = 0; f < nfrags; ++f) {
            if (!freeMap.test(f) && !usedMap.test(f)) {
                if (!leaked++)
                    firstLeak = f;
            }
        }
        if (leaked)
            add(QStringLiteral("%1 fragments are marked in use but nothing uses them (first: %2).").arg(leaked).arg(firstLeak));
        std::vector<qint32> dirsPerCg(size_t(sb.ncg), 0);
        for (const auto& [ino, info] : seen)
            if (info.isDir)
                ++dirsPerCg[size_t(ino / quint32(sb.ipg))];
        for (int cg = 0; cg < sb.ncg; ++cg)
            if (cgs[size_t(cg)].valid && cgs[size_t(cg)].ndir != dirsPerCg[size_t(cg)])
                add(QStringLiteral("Cylinder group %1 counts %2 folders, it has %3.").arg(cg).arg(cgs[size_t(cg)].ndir).arg(dirsPerCg[size_t(cg)]));
    }
    return rep;
}

} // namespace fw::ufs2
