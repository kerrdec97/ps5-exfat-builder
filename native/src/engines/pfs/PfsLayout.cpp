// PFS metadata layout. Port of Lazy_MkPFS lazy_mkpfs/build.py (build_pfs
// unsigned branch, build_pfs_stream_single_file, validate_d32_ranges) and
// inspect.py (make_fpt_and_collision_blob). GPL-3.0-or-later.

#include "engines/pfs/PfsLayout.h"

#include <QDir>

#include <algorithm>
#include <map>

namespace fw::pfs {

namespace {

constexpr qint64 kInt32Max = 0x7FFFFFFF;

Inode makeInode(quint16 mode, quint16 nlink, quint32 flags, qint64 size, qint64 sizeCompressed, quint32 blocks,
                qint64 time)
{
    Inode i;
    i.mode = mode;
    i.nlink = nlink;
    i.flags = flags;
    i.size = size;
    i.sizeCompressed = sizeCompressed;
    i.blocks = blocks;
    i.time = time;
    return i;
}

void setContiguous(Inode& ino, qint64 start)
{
    ino.db[0] = start;
    for (int i = 1; i < kMaxDirectBlocks; ++i)
        ino.db[size_t(i)] = -1;
}

QByteArray parentOf(const QByteArray& rel)
{
    const qsizetype slash = rel.lastIndexOf('/');
    return slash < 0 ? QByteArray() : rel.left(slash);
}

QByteArray nameOf(const QByteArray& rel)
{
    const qsizetype slash = rel.lastIndexOf('/');
    return slash < 0 ? rel : rel.mid(slash + 1);
}

} // namespace

Result<FolderLayout> buildFolderLayout(const SourcePlan& plan, const LayoutOptions& options)
{
    const int bs = options.blockSize;
    if (bs < 0x1000 || bs > 0x10000 || (bs & (bs - 1)) != 0)
        return Error(ErrorCode::InvalidArgument, QStringLiteral("Unsupported PFS block size %1.").arg(bs));

    FolderLayout L;
    L.options = options;

    // ---- directories, created as parents of files (Lazy scan_source_tree) ----
    struct TmpDir {
        QByteArray rel;
        int parent;
        std::vector<int> childDirs, childFiles;
    };
    std::vector<TmpDir> tmp;
    std::map<QByteArray, int> byRel;
    tmp.push_back({QByteArray(), -1, {}, {}});
    byRel.emplace(QByteArray(), 0);
    for (int fi = 0; fi < int(plan.files.size()); ++fi) {
        const QByteArray& rel = plan.files[size_t(fi)].relPath;
        const QByteArray parent = parentOf(rel);
        int cur = 0;
        if (!parent.isEmpty()) {
            QByteArray acc;
            for (const QByteArray& part : parent.split('/')) {
                acc = acc.isEmpty() ? part : acc + '/' + part;
                auto it = byRel.find(acc);
                if (it == byRel.end()) {
                    const int idx = int(tmp.size());
                    tmp.push_back({acc, cur, {}, {}});
                    byRel.emplace(acc, idx);
                    tmp[size_t(cur)].childDirs.push_back(idx);
                    cur = idx;
                } else {
                    cur = it->second;
                }
            }
        }
        tmp[size_t(cur)].childFiles.push_back(fi);
    }
    // Sorted directory order (uroot first: "" sorts before everything).
    std::vector<int> order(tmp.size());
    for (size_t i = 0; i < order.size(); ++i)
        order[i] = int(i);
    std::stable_sort(order.begin(), order.end(),
                     [&](int a, int b) { return lessCaseInsensitive(tmp[size_t(a)].rel, tmp[size_t(b)].rel); });
    std::vector<int> newIndex(tmp.size());
    for (size_t i = 0; i < order.size(); ++i)
        newIndex[size_t(order[i])] = int(i);
    L.dirs.resize(tmp.size());
    for (size_t i = 0; i < order.size(); ++i) {
        const TmpDir& t = tmp[size_t(order[i])];
        FolderLayout::Dir& d = L.dirs[i];
        d.rel = t.rel;
        d.name = t.rel.isEmpty() ? QByteArrayLiteral("uroot") : nameOf(t.rel);
        d.parent = t.parent < 0 ? -1 : newIndex[size_t(t.parent)];
        for (int c : t.childDirs)
            d.childDirs.push_back(newIndex[size_t(c)]);
        // New indices follow the sorted order, so sorting indices sorts by path.
        std::sort(d.childDirs.begin(), d.childDirs.end());
        d.childFiles = t.childFiles; // plan order is already sorted
    }

    // ---- inodes ----------------------------------------------------------------------
    const qint64 now = options.time;
    const quint16 dirMode = kInodeModeDir | kInodeRxOnly;
    const quint16 fileMode = kInodeModeFile | kInodeRxOnly;
    std::vector<Inode> inodes;
    inodes.reserve(3 + L.dirs.size() + plan.files.size());
    inodes.push_back(makeInode(dirMode, 1, kInodeFlagInternal | kInodeFlagReadonly, bs, bs, 1, now)); // super root
    inodes.push_back(makeInode(fileMode, 1, kInodeFlagInternal | kInodeFlagReadonly, 0, 0, 1, now));  // fpt
    inodes.push_back(makeInode(dirMode, 3, kInodeFlagReadonly, bs, bs, 1, now));                      // uroot
    L.dirs[0].inode = 2;
    for (size_t i = 1; i < L.dirs.size(); ++i) {
        L.dirs[i].inode = int(inodes.size());
        inodes.push_back(makeInode(dirMode, 2, kInodeFlagReadonly, bs, bs, 1, now));
    }
    L.fileInode.resize(plan.files.size());
    for (size_t i = 0; i < plan.files.size(); ++i) {
        L.fileInode[i] = int(inodes.size());
        inodes.push_back(makeInode(fileMode, 1, kInodeFlagReadonly, 0, 0, 1, now));
    }

    // ---- flat_path_table hashes; a collision adds the resolver inode at #2 ----------
    struct PathEntry {
        QByteArray full;
        int inode; // index before renumbering
        bool isDir;
    };
    std::vector<PathEntry> paths;
    paths.reserve(L.dirs.size() + plan.files.size());
    for (size_t i = 1; i < L.dirs.size(); ++i)
        paths.push_back({'/' + L.dirs[i].rel, L.dirs[i].inode, true});
    for (size_t i = 0; i < plan.files.size(); ++i)
        paths.push_back({'/' + plan.files[i].relPath, L.fileInode[i], false});
    std::map<quint32, std::vector<int>> byHash; // hash -> path indices (insertion order)
    for (int i = 0; i < int(paths.size()); ++i)
        byHash[fptHash(paths[size_t(i)].full, options.caseInsensitive)].push_back(i);
    const bool collision = std::any_of(byHash.begin(), byHash.end(), [](const auto& kv) { return kv.second.size() > 1; });

    if (collision) {
        // Lazy: inodes = [super, fpt, collision, uroot] + rest, renumbered.
        Inode col = makeInode(fileMode, 1, kInodeFlagInternal | kInodeFlagReadonly, 0, 0, 1, now);
        inodes.insert(inodes.begin() + 2, col);
        L.collisionInode = 2;
        L.urootInode = 3;
        for (auto& d : L.dirs)
            d.inode += 1;
        for (int& f : L.fileInode)
            f += 1;
        for (auto& p : paths)
            p.inode += 1;
    }
    for (size_t i = 0; i < inodes.size(); ++i)
        inodes[i].number = quint32(i);

    // ---- directory entries and link counts ------------------------------------------
    for (size_t i = 0; i < L.dirs.size(); ++i) {
        FolderLayout::Dir& d = L.dirs[i];
        if (d.childDirs.size() > 0xFFF0)
            return Error(ErrorCode::Unsupported,
                         QStringLiteral("The folder %1 has too many subfolders for a PFS image.")
                             .arg(QString::fromLatin1(d.rel)));
        const quint32 self = quint32(d.inode);
        const quint32 parent = i == 0 ? self : quint32(L.dirs[size_t(d.parent)].inode);
        appendDirent(d.blob, self, kDirentDot, QByteArrayLiteral("."));
        appendDirent(d.blob, parent, kDirentDotDot, QByteArrayLiteral(".."));
        for (int c : d.childDirs) {
            appendDirent(d.blob, quint32(L.dirs[size_t(c)].inode), kDirentDirectory, L.dirs[size_t(c)].name);
            inodes[size_t(d.inode)].nlink = quint16(inodes[size_t(d.inode)].nlink + 1);
        }
        for (int f : d.childFiles)
            appendDirent(d.blob, quint32(L.fileInode[size_t(f)]), kDirentFile, plan.files[size_t(f)].name());
    }

    // ---- flat_path_table + collision_resolver (make_fpt_and_collision_blob) ---------
    std::map<quint32, quint32> collisionOffsets;
    if (collision) {
        for (const auto& [h, idx] : byHash) {
            if (idx.size() <= 1)
                continue;
            collisionOffsets[h] = quint32(L.collisionBlob.size());
            for (int pi : idx) {
                const PathEntry& p = paths[size_t(pi)];
                appendDirent(L.collisionBlob, quint32(p.inode), p.isDir ? kDirentDirectory : kDirentFile, p.full);
            }
            L.collisionBlob.append(QByteArray(0x18, '\0'));
        }
    }
    L.fptBlob.reserve(qsizetype(byHash.size()) * 8);
    for (const auto& [h, idx] : byHash) {
        quint32 value;
        if (idx.size() == 1) {
            const PathEntry& p = paths[size_t(idx.front())];
            value = quint32(p.inode) | (p.isDir ? 0x20000000u : 0u);
        } else {
            value = 0x80000000u | collisionOffsets[h];
        }
        char e[8];
        putU32(e, h);
        putU32(e + 4, value);
        L.fptBlob.append(e, 8);
    }

    // ---- super root ----------------------------------------------------------------------
    appendDirent(L.superBlob, quint32(L.fptInode), kDirentFile, QByteArrayLiteral("flat_path_table"));
    if (collision)
        appendDirent(L.superBlob, quint32(L.collisionInode), kDirentFile, QByteArrayLiteral("collision_resolver"));
    appendDirent(L.superBlob, quint32(L.urootInode), kDirentDirectory, QByteArrayLiteral("uroot"));

    // ---- block allocation (unsigned branch) ------------------------------------------
    const qint64 perBlock = bs / kInodeD32Size;
    L.inodeBlockCount = ceilDiv(qint64(inodes.size()), perBlock);
    qint64 nd = 1 + L.inodeBlockCount;
    inodes[size_t(L.superInode)].db[0] = nd; // db[1..] stay 0 for the super root
    nd += inodes[size_t(L.superInode)].blocks;
    {
        Inode& fpt = inodes[size_t(L.fptInode)];
        fpt.size = L.fptBlob.size();
        fpt.sizeCompressed = L.fptBlob.size();
        fpt.blocks = quint32(std::max<qint64>(1, ceilDiv(L.fptBlob.size(), bs)));
        setContiguous(fpt, nd);
        nd += fpt.blocks;
    }
    if (collision) {
        Inode& col = inodes[size_t(L.collisionInode)];
        col.size = L.collisionBlob.size();
        col.sizeCompressed = L.collisionBlob.size();
        col.blocks = quint32(std::max<qint64>(1, ceilDiv(L.collisionBlob.size(), bs)));
        setContiguous(col, nd);
        nd += col.blocks;
    } else {
        nd += 1; // reserved empty block
    }
    for (FolderLayout::Dir& d : L.dirs) {
        Inode& ino = inodes[size_t(d.inode)];
        const qint64 blocks = payloadBlocks(d.blob.size(), bs);
        setContiguous(ino, nd);
        ino.blocks = quint32(blocks);
        ino.size = blocks * bs;
        ino.sizeCompressed = ino.size;
        nd += blocks;
    }
    L.firstFileBlock = nd;
    L.inodes = std::move(inodes);
    return L;
}

qint64 placeFile(FolderLayout& L, int fileIndex, qint64 startBlock, qint64 rawSize, qint64 storedSize, bool compressed)
{
    const int bs = L.options.blockSize;
    Inode& ino = L.inodes[size_t(L.fileInode[size_t(fileIndex)])];
    const qint64 blocks = payloadBlocks(storedSize, bs);
    setContiguous(ino, startBlock);
    ino.blocks = quint32(blocks);
    ino.flags = kInodeFlagReadonly | (compressed ? kInodeFlagCompressed : 0);
    ino.size = storedSize;
    ino.sizeCompressed = compressed ? ceilDiv(rawSize, bs) * bs : storedSize;
    return startBlock + blocks;
}

Status finishLayout(FolderLayout& L, qint64 ndblock)
{
    L.ndblock = ndblock;
    if (ndblock > kInt32Max)
        return Error(ErrorCode::Unsupported,
                     QStringLiteral("The image is too large for the PFS format at this block size (%1 blocks).")
                         .arg(ndblock));
    for (const Inode& ino : L.inodes)
        if (ino.blocks > 0xFFFFFFFFu || ino.db[0] > kInt32Max)
            return Error(ErrorCode::Unsupported, QStringLiteral("A file is too large for the PFS format."));
    return success();
}

QByteArray inodeTableBytes(const std::vector<Inode>& inodes, int blockSize, qint64 inodeBlockCount)
{
    QByteArray table(qsizetype(inodeBlockCount * blockSize), '\0');
    const qint64 perBlock = blockSize / kInodeD32Size;
    for (size_t i = 0; i < inodes.size(); ++i) {
        const qint64 block = qint64(i) / perBlock;
        const qint64 slot = qint64(i) % perBlock;
        writeInodeD32(table.data() + block * blockSize + slot * kInodeD32Size, inodes[i]);
    }
    return table;
}

QByteArray layoutHeader(const FolderLayout& L)
{
    HeaderParams p;
    p.blockSize = L.options.blockSize;
    p.version = L.options.version;
    p.mode = L.mode();
    p.nblock = 1;
    p.inodeCount = qint64(L.inodes.size());
    p.ndblock = L.ndblock;
    p.inodeBlockCount = L.inodeBlockCount;
    p.time = L.options.time;
    return headerBlock(p);
}

qint64 uncompressedImageSize(const FolderLayout& L, const SourcePlan& plan)
{
    qint64 blocks = L.firstFileBlock;
    for (const SourceFile& f : plan.files)
        blocks += payloadBlocks(f.size, L.options.blockSize);
    return blocks * L.options.blockSize;
}

// ---- single file ------------------------------------------------------------------------

Result<SingleFileLayout> buildSingleFileLayout(const QByteArray& name, const LayoutOptions& options)
{
    const int bs = options.blockSize;
    if (bs < 0x1000 || bs > 0x10000 || (bs & (bs - 1)) != 0)
        return Error(ErrorCode::InvalidArgument, QStringLiteral("Unsupported PFS block size %1.").arg(bs));
    if (name.isEmpty() || name.contains('/'))
        return Error(ErrorCode::InvalidArgument, QStringLiteral("Invalid file name for the PFS image."));
    SingleFileLayout L;
    L.options = options;
    L.name = name;
    const qint64 now = options.time;
    const quint16 dirMode = kInodeModeDir | kInodeRxOnly;
    const quint16 fileMode = kInodeModeFile | kInodeRxOnly;
    L.inodes.push_back(makeInode(dirMode, 1, kInodeFlagInternal | kInodeFlagReadonly, bs, bs, 1, now));
    L.inodes.push_back(makeInode(fileMode, 1, kInodeFlagInternal | kInodeFlagReadonly, 0, 0, 1, now));
    L.inodes.push_back(makeInode(dirMode, 3, kInodeFlagReadonly, bs, bs, 1, now));
    L.inodes.push_back(makeInode(fileMode, 1, kInodeFlagReadonly, 0, 0, 1, now));
    for (size_t i = 0; i < L.inodes.size(); ++i)
        L.inodes[i].number = quint32(i);

    char e[8];
    putU32(e, fptHash('/' + name, options.caseInsensitive));
    putU32(e + 4, 3);
    L.fptBlob = QByteArray(e, 8);
    appendDirent(L.urootBlob, 2, kDirentDot, QByteArrayLiteral("."));
    appendDirent(L.urootBlob, 2, kDirentDotDot, QByteArrayLiteral(".."));
    appendDirent(L.urootBlob, 3, kDirentFile, name);
    appendDirent(L.superBlob, 1, kDirentFile, QByteArrayLiteral("flat_path_table"));
    appendDirent(L.superBlob, 2, kDirentDirectory, QByteArrayLiteral("uroot"));

    const qint64 ibc = ceilDiv(qint64(L.inodes.size()), bs / kInodeD32Size);
    qint64 nd = 1 + ibc;
    L.inodes[0].db[0] = nd;
    nd += 1;
    Inode& fpt = L.inodes[1];
    fpt.size = fpt.sizeCompressed = L.fptBlob.size();
    fpt.blocks = 1;
    setContiguous(fpt, nd);
    nd += 1;
    nd += 1; // reserved empty block
    Inode& uroot = L.inodes[2];
    uroot.blocks = quint32(std::max<qint64>(1, ceilDiv(L.urootBlob.size(), bs)));
    uroot.size = uroot.sizeCompressed = qint64(uroot.blocks) * bs;
    setContiguous(uroot, nd);
    nd += uroot.blocks;
    setContiguous(L.inodes[3], nd);
    L.payloadBlock = nd;
    return L;
}

qint64 finishSingleFile(SingleFileLayout& L, qint64 rawSize, qint64 storedSize, bool compressed)
{
    const int bs = L.options.blockSize;
    Inode& f = L.inodes[3];
    f.blocks = quint32(payloadBlocks(storedSize, bs));
    f.size = storedSize;
    f.sizeCompressed = compressed ? ceilDiv(rawSize, bs) * bs : storedSize;
    f.flags = kInodeFlagReadonly | (compressed ? kInodeFlagCompressed : 0);
    return L.payloadBlock + f.blocks;
}

QByteArray singleFileMetadata(const SingleFileLayout& L, qint64 ndblock)
{
    const int bs = L.options.blockSize;
    QByteArray meta(qsizetype(L.payloadBlock * bs), '\0');
    HeaderParams p;
    p.blockSize = bs;
    p.version = L.options.version;
    p.mode = L.mode();
    p.nblock = 1;
    p.inodeCount = qint64(L.inodes.size());
    p.ndblock = ndblock;
    p.inodeBlockCount = ceilDiv(qint64(L.inodes.size()), bs / kInodeD32Size);
    p.time = L.options.time;
    const QByteArray hdr = headerBlock(p);
    std::memcpy(meta.data(), hdr.constData(), size_t(bs));
    const QByteArray table = inodeTableBytes(L.inodes, bs, p.inodeBlockCount);
    std::memcpy(meta.data() + bs, table.constData(), size_t(table.size()));
    auto put = [&](qint64 block, const QByteArray& blob) {
        std::memcpy(meta.data() + block * bs, blob.constData(), size_t(blob.size()));
    };
    put(L.inodes[0].db[0], L.superBlob);
    put(L.inodes[1].db[0], L.fptBlob);
    put(L.inodes[2].db[0], L.urootBlob);
    return meta;
}

} // namespace fw::pfs
