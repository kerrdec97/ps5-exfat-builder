#include "engines/exfat/ExfatWriter.h"

#include "core/FileUtil.h"
#include "engines/exfat/Pipeline.h"
#include "engines/exfat/PlatformFs.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QRandomGenerator>

#include <algorithm>
#include <atomic>
#include <cmath>

namespace fw::exfat {

// ---- legacy sizing rules ---------------------------------------------------------

qint64 legacyImageBytes(const SourceTree& tree, int clusterBytes)
{
    const qint64 c = clusterBytes;
    qint64 data = 0;
    for (const SourceNode& n : tree.nodes)
        if (!n.isDir)
            data += ceilDiv(n.size, c) * c;
    const qint64 dataClusters = ceilDiv(data, c);
    const qint64 fat = dataClusters * 4;
    const qint64 bitmap = ceilDiv(dataClusters, 8);
    const qint64 entries = (tree.fileCount + tree.dirCount) * 512;
    const qint64 base = data + fat + bitmap + entries + 64 * kMiB;
    const qint64 spare = std::clamp<qint64>(ceilDiv(base, 100), 1 * kGiB, 8 * kGiB);
    const qint64 total = std::max(base + spare, tree.totalBytes + 1 * kGiB);
    return alignUp(total, kMiB);
}

// ---- planning ------------------------------------------------------------------------

static qint64 entrySetBytes(const QString& name)
{
    return qint64(kEntrySize) * (2 + ceilDiv(name.size(), kNameCharsPerEntry));
}

static QString nativeRel(const SourceTree& tree, int node)
{
    const QString& rel = tree.nodes[size_t(node)].relativePath;
    return rel.isEmpty() ? QDir::toNativeSeparators(tree.root) : QDir::toNativeSeparators(rel);
}

Result<ImagePlan> planImage(const SourceTree& tree, const WriterOptions& o)
{
    if (tree.nodes.empty() || !tree.nodes[0].isDir)
        return Error(ErrorCode::Internal, QStringLiteral("Nothing to put into the image."));
    const UpcaseTable& up = UpcaseTable::recommended();

    if (const QString why = checkVolumeLabel(o.label); !why.isEmpty())
        return Error(ErrorCode::InvalidArgument, QStringLiteral("The volume label \"%1\" cannot be used: %2.").arg(o.label, why));

    // Names: length, characters, and names that differ only in case.
    const size_t count = tree.nodes.size();
    std::vector<qint64> dirBytes(count, 0);
    for (size_t i = 0; i < count; ++i) {
        const SourceNode& n = tree.nodes[i];
        if (i > 0) {
            if (const QString why = checkFileName(n.name); !why.isEmpty())
                return Error(ErrorCode::InvalidArgument,
                             QStringLiteral("%1 cannot go into an exFAT image: %2. Rename it and try again.")
                                 .arg(nativeRel(tree, int(i)), why));
        }
        if (!n.isDir)
            continue;
        qint64 bytes = (i == 0) ? qint64(kEntrySize) * (o.label.isEmpty() ? 2 : 3) : 0;
        QHash<QString, int> seen;
        seen.reserve(qsizetype(n.children.size()));
        for (int c : n.children) {
            const SourceNode& child = tree.nodes[size_t(c)];
            const QString key = up.upcase(child.name);
            if (auto it = seen.constFind(key); it != seen.cend())
                return Error(ErrorCode::InvalidArgument,
                             QStringLiteral("%1 and %2 differ only in upper/lower case. exFAT treats them as the same "
                                            "name; rename one of them and try again.")
                                 .arg(nativeRel(tree, *it), nativeRel(tree, c)));
            seen.insert(key, c);
            bytes += entrySetBytes(child.name);
        }
        if (bytes > kMaxDirectoryBytes)
            return Error(ErrorCode::InvalidArgument,
                         QStringLiteral("The folder %1 holds too many files for one exFAT folder (the limit is about "
                                        "400,000 long names). Split it into sub-folders.")
                             .arg(nativeRel(tree, int(i))));
        dirBytes[i] = bytes;
    }

    // Cluster size.
    ImagePlan plan;
    const int cluster = o.clusterBytes > 0 ? o.clusterBytes : kAutoClusterBytes;
    if (o.clusterBytes <= 0)
        plan.clusterChoice = QStringLiteral("%1 KB (automatic)").arg(cluster / 1024);
    else if (cluster == kAutoClusterBytes)
        plan.clusterChoice = QStringLiteral("%1 KB (set in Settings)").arg(cluster / 1024);
    else
        plan.clusterChoice = QStringLiteral("%1 KB (set in Settings; ShadowMount+ 1.7 and later need 64 KB)").arg(cluster / 1024);

    // Clusters needed, apart from the allocation bitmap (which depends on the
    // volume size).
    const qint64 upcaseClusters = ceilDiv(UpcaseTable::recommended().data().size(), cluster);
    qint64 base = upcaseClusters;
    for (size_t i = 0; i < count; ++i) {
        const SourceNode& n = tree.nodes[i];
        base += n.isDir ? std::max<qint64>(1, ceilDiv(dirBytes[i], cluster)) : ceilDiv(n.size, cluster);
    }
    if (base > qint64(kMaxClusterCount))
        return Error(ErrorCode::InvalidArgument,
                     QStringLiteral("This folder needs more clusters than exFAT allows. Choose a larger cluster size."));

    const qint64 spareClusters =
        (o.volumeBytes <= 0 && o.spare == SpareMode::Fixed) ? ceilDiv(std::max<qint64>(0, o.spareBytes), cluster) : 0;
    const auto needFor = [&](const Geometry& g) { return ceilDiv(g.bitmapBytes(), cluster) + base; };
    // Smallest volume >= start that holds everything plus `extra` free clusters.
    const auto fit = [&](qint64 start, qint64 extra, bool grow) -> Result<Geometry> {
        qint64 v = start;
        for (int iter = 0; iter < 128; ++iter) {
            auto g = computeGeometry(v, o.sectorBytes, cluster);
            if (!g.ok()) {
                if (!grow || g.error().code != ErrorCode::InvalidArgument || v > 4 * kMiB + start * 2)
                    return g.takeError();
                v = alignUp(v + 4 * kMiB, kMiB);
                continue;
            }
            const qint64 need = needFor(g.value()) + extra;
            if (need <= qint64(g->clusterCount))
                return g;
            if (!grow)
                return Error(ErrorCode::InvalidArgument, QStringLiteral("too small"));
            const qint64 deficit = need - qint64(g->clusterCount);
            v = alignUp(v + deficit * cluster + deficit * 4 + cluster, kMiB);
        }
        return Error(ErrorCode::Internal, QStringLiteral("Could not size the image."));
    };
    const qint64 align = std::max<qint64>(kMiB, cluster);
    const qint64 estimate = alignUp(2 * align + qint64(24) * o.sectorBytes + (base + spareClusters + 1) * cluster
                                        + (base + spareClusters) * 4,
                                    kMiB);

    Result<Geometry> geo = Error(ErrorCode::Internal, {});
    if (o.volumeBytes > 0) {
        const qint64 v = std::max<qint64>(o.volumeBytes / kMiB * kMiB, kMiB);
        geo = fit(v, 0, false);
        if (!geo.ok()) {
            auto minimum = fit(estimate, 0, true);
            return Error(ErrorCode::InvalidArgument,
                         QStringLiteral("The image size you set (%1) is too small for this folder, which needs at least "
                                        "%2. Set a larger size, or leave the size on automatic.")
                             .arg(formatBytes(v), formatBytes(minimum.ok() ? minimum->volumeBytes() : estimate)),
                         geo.error().toLogString());
        }
        plan.sizeChoice = QStringLiteral("%1 (fixed size)").arg(formatBytes(geo->volumeBytes()));
    } else if (o.spare == SpareMode::Legacy) {
        const qint64 v = legacyImageBytes(tree, cluster);
        geo = fit(std::max(v, estimate), 0, true);
        // The size the image really gets: `fit` may have grown the estimate.
        plan.sizeChoice = QStringLiteral("%1 (automatic: data, metadata and 1 % spare, 1 to 8 GB)")
                              .arg(geo.ok() ? formatBytes(geo->volumeBytes()) : QString());
    } else {
        geo = fit(estimate, spareClusters, true);
        plan.sizeChoice = QStringLiteral("%1 (automatic: smallest size with %2 free)")
                              .arg(geo.ok() ? formatBytes(geo->volumeBytes()) : QString(), formatBytes(o.spareBytes));
    }
    if (!geo.ok())
        return geo.takeError();
    plan.geo = geo.value();
    plan.label = o.label;
    plan.fatChains = o.fatChains;
    plan.serial = o.serial ? o.serial
                           : quint32(QDateTime::currentMSecsSinceEpoch() & 0xFFFFFFFF) ^ QRandomGenerator::global()->generate();

    // Assign clusters: bitmap, up-case table, directories, then file data.
    plan.nodes.resize(count);
    quint32 next = kFirstDataCluster;
    plan.bitmapCluster = next;
    plan.bitmapClusters = quint32(ceilDiv(plan.geo.bitmapBytes(), cluster));
    next += plan.bitmapClusters;
    plan.upcaseCluster = next;
    plan.upcaseClusters = quint32(upcaseClusters);
    next += plan.upcaseClusters;

    // Depth-first pre-order over the (already sorted) tree.
    std::vector<int> order;
    order.reserve(count);
    {
        std::vector<int> stack{0};
        while (!stack.empty()) {
            const int n = stack.back();
            stack.pop_back();
            order.push_back(n);
            const auto& kids = tree.nodes[size_t(n)].children;
            for (auto it = kids.rbegin(); it != kids.rend(); ++it)
                stack.push_back(*it);
        }
    }
    for (int n : order) {
        if (!tree.nodes[size_t(n)].isDir)
            continue;
        NodePlacement& p = plan.nodes[size_t(n)];
        p.clusters = quint32(std::max<qint64>(1, ceilDiv(dirBytes[size_t(n)], cluster)));
        p.firstCluster = next;
        p.dataLength = qint64(p.clusters) * cluster;
        next += p.clusters;
        plan.dirOrder.push_back(n);
    }
    for (int n : order) {
        const SourceNode& s = tree.nodes[size_t(n)];
        if (s.isDir)
            continue;
        NodePlacement& p = plan.nodes[size_t(n)];
        p.dataLength = s.size;
        p.clusters = quint32(ceilDiv(s.size, cluster));
        p.firstCluster = p.clusters ? next : 0;
        next += p.clusters;
        plan.fileOrder.push_back(n);
    }
    plan.usedClusters = next - kFirstDataCluster;
    Q_ASSERT(plan.usedClusters <= plan.geo.clusterCount);
    return plan;
}

// ---- directory contents --------------------------------------------------------------

static void putTimes(uchar* e, const SourceNode& n)
{
    const Timestamp c = encodeTimestamp(n.created);
    const Timestamp m = encodeTimestamp(n.modified);
    const Timestamp a = encodeTimestamp(n.accessed);
    put32(e + 8, c.value);
    put32(e + 12, m.value);
    put32(e + 16, a.value);
    e[20] = c.tenMs;
    e[21] = m.tenMs;
    e[22] = c.utcOffset;
    e[23] = m.utcOffset;
    e[24] = a.utcOffset;
}

static void appendEntrySet(QByteArray& out, const SourceNode& n, const NodePlacement& p, bool fatChains,
                           const UpcaseTable& up)
{
    const int nameLen = int(n.name.size());
    const int nameEntries = int(ceilDiv(nameLen, kNameCharsPerEntry));
    const int entries = 2 + nameEntries;
    const qsizetype at = out.size();
    out.append(QByteArray(entries * kEntrySize, '\0'));
    auto* e = reinterpret_cast<uchar*>(out.data()) + at;

    e[0] = kTypeFile;
    e[1] = uchar(entries - 1);
    put16(e + 4, quint16((n.attributes & kAttrMask) | (n.isDir ? kAttrDirectory : 0)));
    putTimes(e, n);

    uchar* s = e + kEntrySize;
    s[0] = kTypeStreamExtension;
    s[1] = uchar(kFlagAllocationPossible | ((p.clusters > 0 && !fatChains) ? kFlagNoFatChain : 0));
    s[3] = uchar(nameLen);
    put16(s + 4, up.nameHash(n.name));
    put64(s + 8, quint64(p.dataLength)); // ValidDataLength: everything is written
    put32(s + 20, p.firstCluster);
    put64(s + 24, quint64(p.dataLength));

    const char16_t* name = reinterpret_cast<const char16_t*>(n.name.utf16());
    for (int i = 0; i < nameEntries; ++i) {
        uchar* f = e + kEntrySize * (2 + i);
        f[0] = kTypeFileName;
        for (int j = 0; j < kNameCharsPerEntry; ++j) {
            const int idx = i * kNameCharsPerEntry + j;
            if (idx < nameLen)
                put16(f + 2 + 2 * j, quint16(name[idx]));
        }
    }
    put16(e + 2, entrySetChecksum(e, entries));
}

static QByteArray buildDirectory(const SourceTree& tree, const ImagePlan& plan, int dirNode)
{
    const UpcaseTable& up = UpcaseTable::recommended();
    const NodePlacement& dp = plan.nodes[size_t(dirNode)];
    QByteArray out;
    out.reserve(qsizetype(dp.dataLength));
    if (dirNode == 0) {
        QByteArray e(3 * kEntrySize, '\0');
        auto* p = reinterpret_cast<uchar*>(e.data());
        int at = 0;
        if (!plan.label.isEmpty()) {
            p[0] = kTypeVolumeLabel;
            p[1] = uchar(plan.label.size());
            for (int i = 0; i < plan.label.size(); ++i)
                put16(p + 2 + 2 * i, plan.label.at(i).unicode());
            at = kEntrySize;
        }
        uchar* b = p + at;
        b[0] = kTypeAllocationBitmap;
        b[1] = 0; // first (only) bitmap
        put32(b + 20, plan.bitmapCluster);
        put64(b + 24, quint64(plan.geo.bitmapBytes()));
        uchar* u = b + kEntrySize;
        u[0] = kTypeUpcaseTable;
        put32(u + 4, up.checksum());
        put32(u + 20, plan.upcaseCluster);
        put64(u + 24, quint64(up.data().size()));
        out.append(e.constData(), at + 2 * kEntrySize);
    }
    for (int c : tree.nodes[size_t(dirNode)].children)
        appendEntrySet(out, tree.nodes[size_t(c)], plan.nodes[size_t(c)], plan.fatChains, up);
    Q_ASSERT(out.size() <= dp.dataLength);
    out.append(QByteArray(qsizetype(dp.dataLength - out.size()), '\0'));
    return out;
}

// ---- output -----------------------------------------------------------------------------

namespace {

class LocalNodeReader final : public NodeReader {
public:
    explicit LocalNodeReader(std::unique_ptr<QFile> f) : m_f(std::move(f)) {}
    qint64 size() const override { return m_f->size(); }
    qint64 read(char* b, qint64 n) override { return m_f->read(b, n); }
    QString errorString() const override { return m_f->errorString(); }

private:
    std::unique_ptr<QFile> m_f;
};

} // namespace

Result<std::unique_ptr<NodeReader>> openLocalNode(const SourceNode& node, const WriterOptions& o,
                                                  const CancellationToken& token)
{
    auto local = openForRead(node.absolutePath, o.openRetries, o.retryWaitMs, token);
    if (!local.ok())
        return local.takeError();
    return std::unique_ptr<NodeReader>(new LocalNodeReader(std::move(local).value()));
}

namespace {

// Sequential writer. Runs of zeros of 1 MiB or more are left as holes
// (resize + seek): they read back as zeros on every platform, and
// sparse-capable file systems do not even allocate them. Shorter gaps
// (cluster padding, region alignment) are written as zeros, which avoids a
// resize per file.
class ImageSink {
public:
    static constexpr qint64 kHoleMin = kMiB;

    explicit ImageSink(QFile& f) : m_f(f) {}
    qint64 pos() const { return m_pos; }

    Status write(const char* p, qint64 n)
    {
        if (n <= 0)
            return success();
        FW_TRY(closeGap());
        FW_TRY(raw(p, n));
        m_pos += n;
        m_end = m_pos;
        return success();
    }
    void skip(qint64 n) { m_pos += std::max<qint64>(0, n); }
    Status writeOrSkip(const char* p, qint64 n) { return isAllZero(p, n) ? (skip(n), success()) : write(p, n); }
    Status finish(qint64 totalSize)
    {
        if (!m_f.resize(totalSize))
            return fileError(QStringLiteral("Could not set the image size."), m_f);
        return success();
    }

private:
    Status raw(const char* p, qint64 n)
    {
        qint64 done = 0;
        while (done < n) {
            const qint64 w = m_f.write(p + done, n - done);
            if (w <= 0)
                return fileError(QStringLiteral("Could not write the image."), m_f);
            done += w;
        }
        return success();
    }
    Status closeGap()
    {
        const qint64 gap = m_pos - m_end;
        if (gap <= 0)
            return success();
        if (gap >= kHoleMin) {
            if (!m_f.resize(m_pos) || !m_f.seek(m_pos))
                return fileError(QStringLiteral("Could not write the image."), m_f);
        } else {
            static const std::vector<char> zeros(size_t(64 * 1024), '\0');
            for (qint64 left = gap; left > 0;) {
                const qint64 n = std::min<qint64>(left, qint64(zeros.size()));
                FW_TRY(raw(zeros.data(), n));
                left -= n;
            }
        }
        m_end = m_pos;
        return success();
    }

    QFile& m_f;
    qint64 m_pos = 0; // logical position
    qint64 m_end = 0; // bytes actually written so far (file end)
};

struct ChainRun {
    quint32 start;
    quint32 count;
};

Status writeFat(ImageSink& sink, const ImagePlan& plan, qint64 bufferBytes)
{
    std::vector<ChainRun> chains;
    chains.push_back({plan.bitmapCluster, plan.bitmapClusters});
    chains.push_back({plan.upcaseCluster, plan.upcaseClusters});
    for (int d : plan.dirOrder) {
        const NodePlacement& p = plan.nodes[size_t(d)];
        if (d == 0 || plan.fatChains) // the root directory always needs its chain
            chains.push_back({p.firstCluster, p.clusters});
    }
    if (plan.fatChains)
        for (int f : plan.fileOrder)
            if (plan.nodes[size_t(f)].clusters)
                chains.push_back({plan.nodes[size_t(f)].firstCluster, plan.nodes[size_t(f)].clusters});

    const qint64 entries = qint64(plan.geo.clusterCount) + 2;
    const qint64 perChunk = std::max<qint64>(1024, bufferBytes / 4);
    std::vector<char> buf(size_t(perChunk * 4));
    size_t ri = 0;
    for (qint64 first = 0; first < entries; first += perChunk) {
        const qint64 n = std::min(perChunk, entries - first);
        const qint64 last = first + n; // exclusive
        while (ri < chains.size() && qint64(chains[ri].start) + chains[ri].count <= first)
            ++ri;
        const bool anyChain = ri < chains.size() && qint64(chains[ri].start) < last;
        if (first > 0 && !anyChain) {
            sink.skip(n * 4);
            continue;
        }
        std::fill(buf.begin(), buf.begin() + qsizetype(n * 4), '\0');
        auto* p = reinterpret_cast<uchar*>(buf.data());
        if (first == 0) {
            put32(p, kFatMediaEntry);
            put32(p + 4, kFatEndOfChain);
        }
        for (size_t r = ri; r < chains.size() && qint64(chains[r].start) < last; ++r) {
            const qint64 s = std::max<qint64>(chains[r].start, first);
            const qint64 e = std::min<qint64>(qint64(chains[r].start) + chains[r].count, last);
            const qint64 chainEnd = qint64(chains[r].start) + chains[r].count;
            for (qint64 c = s; c < e; ++c)
                put32(p + (c - first) * 4, c + 1 < chainEnd ? quint32(c + 1) : kFatEndOfChain);
        }
        FW_TRY(sink.write(buf.data(), n * 4));
    }
    sink.skip(plan.geo.fatBytes() - entries * 4);
    return success();
}

Status writeBitmap(ImageSink& sink, const ImagePlan& plan, qint64 bufferBytes)
{
    const qint64 fullBytes = plan.usedClusters / 8;
    const int partialBits = int(plan.usedClusters % 8);
    std::vector<char> ones(size_t(std::min<qint64>(std::max<qint64>(fullBytes, 1), bufferBytes)), char(0xFF));
    for (qint64 done = 0; done < fullBytes;) {
        const qint64 n = std::min<qint64>(qint64(ones.size()), fullBytes - done);
        FW_TRY(sink.write(ones.data(), n));
        done += n;
    }
    qint64 written = fullBytes;
    if (partialBits) {
        const char b = char((1 << partialBits) - 1);
        FW_TRY(sink.write(&b, 1));
        ++written;
    }
    sink.skip(qint64(plan.bitmapClusters) * plan.geo.clusterBytes() - written);
    return success();
}

} // namespace

Result<WriteStats> writeImage(const SourceTree& tree, const ImagePlan& plan, const QString& path,
                              const WriterOptions& o, const CancellationToken& token, ProgressReporter* progress,
                              const Log* log)
{
    QElapsedTimer timer;
    timer.start();
    WriteStats stats;
    const QString display = QFileInfo(path).fileName();
    QFile file(toLongPath(path));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return fileError(QStringLiteral("Could not create %1.").arg(QDir::toNativeSeparators(path)), file);
    ImageSink sink(file);
    const Geometry& g = plan.geo;
    const qint64 clusterBytes = g.clusterBytes();

    // Boot regions, FAT, gaps.
    BootInfo boot;
    boot.geo = g;
    boot.rootCluster = plan.nodes[0].firstCluster;
    boot.serial = plan.serial;
    boot.percentInUse = plan.percentInUse();
    const QByteArray region = buildBootRegion(boot);
    FW_TRY(sink.write(region.constData(), region.size()));
    FW_TRY(sink.write(region.constData(), region.size()));
    sink.skip(g.fatOffsetBytes() - sink.pos());
    FW_TRY(writeFat(sink, plan, o.bufferBytes));
    sink.skip(g.heapOffsetBytes() - sink.pos());
    if (!token.checkpoint())
        return Error::cancelled();

    // Allocation bitmap, up-case table, directories.
    FW_TRY(writeBitmap(sink, plan, o.bufferBytes));
    const QByteArray& upcase = UpcaseTable::recommended().data();
    FW_TRY(sink.write(upcase.constData(), upcase.size()));
    sink.skip(qint64(plan.upcaseClusters) * clusterBytes - upcase.size());
    for (size_t i = 0; i < plan.dirOrder.size(); ++i) {
        const int d = plan.dirOrder[i];
        if (sink.pos() != g.clusterOffset(plan.nodes[size_t(d)].firstCluster))
            return Error(ErrorCode::Internal, QStringLiteral("The image layout is inconsistent."),
                         QStringLiteral("directory %1 at %2").arg(d).arg(sink.pos()));
        const QByteArray bytes = buildDirectory(tree, plan, d);
        FW_TRY(sink.write(bytes.constData(), bytes.size()));
        if ((i & 0x3F) == 0 && !token.checkpoint())
            return Error::cancelled();
    }

    // File data: a reader thread fills buffers while this thread writes them.
    if (log)
        log->debug(QStringLiteral("Writing %1 files (%2) with %3 x %4 buffers")
                       .arg(plan.fileOrder.size())
                       .arg(formatBytes(tree.totalBytes))
                       .arg(o.bufferCount)
                       .arg(formatBytes(o.bufferBytes)));
    ChunkPipeline pipe(o.bufferCount, o.bufferBytes);
    ScopedThread reader(std::thread([&] {
        for (int k = 0; k < int(plan.fileOrder.size()); ++k) {
            const SourceNode& n = tree.nodes[size_t(plan.fileOrder[size_t(k)])];
            if (n.size == 0) {
                if (!pipe.post({nullptr, 0, k, true}))
                    break;
                continue;
            }
            auto opened = o.opener ? o.opener(n, token) : openLocalNode(n, o, token);
            if (!opened.ok()) {
                pipe.setProducerError(opened.takeError());
                break;
            }
            NodeReader& f = *opened.value();
            const QString shown = QDir::toNativeSeparators(n.relativePath);
            if (f.size() != n.size) {
                pipe.setProducerError(Error(ErrorCode::IoError,
                                            QStringLiteral("%1 changed while the image was being built. Make sure "
                                                           "nothing writes to the game folder, then try again.")
                                                .arg(shown),
                                            QStringLiteral("size %1 at scan, %2 now").arg(n.size).arg(f.size())));
                break;
            }
            qint64 left = n.size;
            bool stop = false;
            while (left > 0) {
                if (!token.checkpoint()) {
                    pipe.setProducerError(Error::cancelled());
                    stop = true;
                    break;
                }
                std::vector<char>* buf = pipe.takeFree();
                if (!buf) {
                    stop = true;
                    break;
                }
                const qint64 want = std::min<qint64>(left, qint64(buf->size()));
                qint64 got = 0;
                qint64 r = 0;
                while (got < want && (r = f.read(buf->data() + got, want - got)) > 0)
                    got += r;
                if (got < want) {
                    pipe.giveBack(buf);
                    pipe.setProducerError(Error(ErrorCode::IoError,
                                                r < 0 ? QStringLiteral("Could not read %1.").arg(shown)
                                                      : QStringLiteral("%1 became shorter while the image was being "
                                                                       "built. Make sure nothing writes to the game "
                                                                       "folder, then try again.")
                                                            .arg(shown),
                                                f.errorString()));
                    stop = true;
                    break;
                }
                left -= got;
                if (!pipe.post({buf, got, k, left == 0})) {
                    stop = true;
                    break;
                }
            }
            if (stop)
                break;
            char extra;
            if (f.read(&extra, 1) > 0) {
                pipe.setProducerError(Error(ErrorCode::IoError,
                                            QStringLiteral("%1 grew while the image was being built. Make sure "
                                                           "nothing writes to the game folder, then try again.")
                                                .arg(shown)));
                break;
            }
        }
        pipe.finishProducing();
    }));
    struct AbortOnExit {
        ChunkPipeline& p;
        ScopedThread& t;
        ~AbortOnExit()
        {
            p.abort();
            t.join();
        }
    } abortOnExit{pipe, reader};

    int current = -1;
    qint64 sinceCheck = 0;
    while (auto chunk = pipe.next()) {
        const int node = plan.fileOrder[size_t(chunk->item)];
        const NodePlacement& p = plan.nodes[size_t(node)];
        if (chunk->item != current) {
            current = chunk->item;
            if (progress)
                progress->setCurrentItem(tree.nodes[size_t(node)].relativePath);
            if (p.clusters && sink.pos() != g.clusterOffset(p.firstCluster))
                return Error(ErrorCode::Internal, QStringLiteral("The image layout is inconsistent."),
                             QStringLiteral("file %1 at %2").arg(node).arg(sink.pos()));
        }
        if (chunk->length > 0) {
            const Status st = sink.writeOrSkip(chunk->buffer->data(), chunk->length);
            pipe.giveBack(chunk->buffer);
            if (!st.ok())
                return st.error();
            stats.bytesWritten += chunk->length;
            sinceCheck += chunk->length;
            if (progress)
                progress->advance(chunk->length);
        }
        if (chunk->endOfItem) {
            sink.skip(qint64(p.clusters) * clusterBytes - p.dataLength);
            ++stats.filesWritten;
            ++sinceCheck;
            if (progress)
                progress->advanceFiles(1);
        }
        if (sinceCheck >= 4 * kMiB || (stats.filesWritten % 64) == 0) {
            sinceCheck = 0;
            if (!token.checkpoint())
                return Error::cancelled();
        }
    }
    if (auto e = pipe.producerError())
        return *e;
    if (!token.checkpoint())
        return Error::cancelled();
    if (stats.filesWritten != qint64(plan.fileOrder.size()))
        return Error(ErrorCode::Internal, QStringLiteral("Not every file was written to the image."));
    if (sink.pos() != plan.dataEndBytes())
        return Error(ErrorCode::Internal, QStringLiteral("The image layout is inconsistent."),
                     QStringLiteral("data ends at %1, expected %2").arg(sink.pos()).arg(plan.dataEndBytes()));

    FW_TRY(sink.finish(g.volumeBytes()));
    if (o.syncToDisk)
        FW_TRY(syncFile(file, display));
    file.close();
    if (file.error() != QFileDevice::NoError)
        return fileError(QStringLiteral("Could not write %1.").arg(display), file);
    stats.elapsedMs = timer.elapsed();
    return stats;
}

} // namespace fw::exfat
