#include "engines/exfat/ExfatReader.h"

#include "core/FileUtil.h"

#include <QDir>
#include <QFileInfo>

#include <algorithm>
#include <bit>

namespace fw::exfat {

namespace {

constexpr qint64 kFatBlock = 64 * 1024;
constexpr int kMaxListedProblems = 50;
// Limits for images that were not written here: deeper or larger than any
// real game folder, small enough that a damaged image cannot exhaust memory.
constexpr int kMaxFolderDepth = 256;
constexpr size_t kMaxTreeEntries = 10'000'000;
constexpr qint64 kChainCheckpoint = 1 << 16; // clusters between two cancel checks

void appendCluster(std::vector<Extent>& v, quint32 c)
{
    if (!v.empty() && v.back().cluster + v.back().count == c)
        ++v.back().count;
    else
        v.push_back({c, 1});
}

// One bit per cluster of the heap, to find clusters used twice.
class ClusterUsage {
public:
    explicit ClusterUsage(quint32 clusterCount) : m_count(clusterCount), m_bits((size_t(clusterCount) + 63) / 64, 0) {}
    // Marks a run; returns the number of clusters that were already marked.
    qint64 mark(const Extent& e)
    {
        qint64 dup = 0;
        for (quint32 i = 0; i < e.count; ++i) {
            const quint64 idx = quint64(e.cluster) - kFirstDataCluster + i;
            if (idx >= m_count)
                break;
            quint64& w = m_bits[size_t(idx / 64)];
            const quint64 bit = quint64(1) << (idx % 64);
            if (w & bit)
                ++dup;
            w |= bit;
        }
        return dup;
    }
    const std::vector<quint64>& bits() const { return m_bits; }
    qint64 count() const
    {
        qint64 n = 0;
        for (quint64 w : m_bits)
            n += std::popcount(w);
        return n;
    }

private:
    quint32 m_count;
    std::vector<quint64> m_bits;
};

class Problems {
public:
    explicit Problems(ImageTree& t) : m_t(t) {}
    void add(const QString& p)
    {
        ++m_t.problemCount;
        if (m_t.problems.size() < kMaxListedProblems)
            m_t.problems << p;
    }

private:
    ImageTree& m_t;
};

QString shownPath(const QString& p) { return p.isEmpty() ? QStringLiteral("the root folder") : p; }

} // namespace

// ---- ExfatReader ----------------------------------------------------------------------

Result<std::unique_ptr<ExfatReader>> ExfatReader::open(const QString& imagePath)
{
    std::unique_ptr<ExfatReader> r(new ExfatReader);
    FW_TRY(r->openImpl(imagePath));
    return r;
}

Result<std::unique_ptr<ExfatReader>> ExfatReader::open(std::shared_ptr<RandomSource> source)
{
    std::unique_ptr<ExfatReader> r(new ExfatReader);
    r->m_path = source->describe();
    r->m_source = source;
    r->m_dev = openSourceDevice(std::move(source));
    FW_TRY(r->openVolume());
    return r;
}

std::unique_ptr<ImageFileStream> ExfatReader::openStream() const
{
    if (m_source)
        return std::make_unique<ImageFileStream>(m_source, m_vol.boot.geo);
    return std::make_unique<ImageFileStream>(m_path, m_vol.boot.geo);
}

Status ExfatReader::openImpl(const QString& path)
{
    m_path = path;
    m_file.setFileName(toLongPath(path));
    if (!QFileInfo::exists(path))
        return Error(ErrorCode::NotFound, QStringLiteral("The image %1 does not exist.").arg(QDir::toNativeSeparators(path)));
    if (!m_file.open(QIODevice::ReadOnly))
        return Error(ErrorCode::IoError, QStringLiteral("Could not open the image %1.").arg(QDir::toNativeSeparators(path)),
                     m_file.errorString());
    return openVolume();
}

Status ExfatReader::openVolume()
{
    m_vol.imageFileSize = io()->size();

    const QByteArray first = io()->read(512);
    if (first.size() < 512)
        return corruptImage(QStringLiteral("the file is too small to be an exFAT image"),
                            QStringLiteral("%1 bytes").arg(m_vol.imageFileSize));
    const int bps = std::max(512, bootSectorBytesPerSector(first));
    io()->seek(0);
    const QByteArray main = io()->read(qint64(bps) * kBootRegionSectors);
    const QByteArray backup = io()->read(qint64(bps) * kBootRegionSectors);
    auto parsed = parseBootRegion(main);
    if (!parsed.ok()) {
        Error e = parsed.takeError();
        if (parseBootRegion(backup).ok())
            e.message = QStringLiteral("The image is damaged: its main boot sector is not valid (the backup copy is "
                                       "intact). It cannot be used safely; rebuild it from the game folder.");
        return e;
    }
    m_vol.boot = parsed.value();
    const Geometry& g = m_vol.boot.geo;

    // The backup region must be a valid copy (VolumeFlags and PercentInUse may differ).
    if (backup.size() == main.size()) {
        QByteArray a = main, b = backup;
        for (int i : {106, 107, 112})
            a[i] = b[i] = 0;
        auto pb = parseBootRegion(backup);
        if (!pb.ok())
            m_vol.backupProblem = QStringLiteral("the backup boot region is not valid: ") + pb.error().detail;
        else if (a.left(bps * 11) != b.left(bps * 11))
            m_vol.backupProblem = QStringLiteral("the backup boot region differs from the main one");
        m_vol.backupBootMatches = m_vol.backupProblem.isEmpty();
    } else {
        m_vol.backupProblem = QStringLiteral("the backup boot region is missing");
    }

    if (m_vol.imageFileSize < g.volumeBytes())
        return corruptImage(QStringLiteral("the file is shorter than the volume it holds (it was probably copied only "
                                           "partly)"),
                            QStringLiteral("file %1 bytes, volume %2 bytes").arg(m_vol.imageFileSize).arg(g.volumeBytes()));
    m_vol.activeFat = (m_vol.boot.numberOfFats == 2 && (m_vol.boot.volumeFlags & 1)) ? 1 : 0;

    // Root directory and its critical entries.
    QString problem;
    if (!followChain(m_vol.boot.rootCluster, -1, m_vol.rootExtents, problem))
        return corruptImage(QStringLiteral("its root folder cannot be read"), problem);
    qint64 rootClusters = 0;
    for (const Extent& e : m_vol.rootExtents)
        rootClusters += e.count;
    if (rootClusters * g.clusterBytes() > kMaxDirectoryBytes)
        return corruptImage(QStringLiteral("its root folder is impossibly large"));
    FW_TRY_ASSIGN(QByteArray root, readExtents(m_vol.rootExtents, rootClusters * g.clusterBytes()));
    const auto* d = reinterpret_cast<const uchar*>(root.constData());
    bool haveBitmap = false, haveUpcase = false;
    quint32 upcaseChecksum = 0;
    for (qint64 off = 0; off + kEntrySize <= root.size(); off += kEntrySize) {
        const uchar* e = d + off;
        if (e[0] == kTypeEndOfDirectory)
            break;
        if (e[0] == kTypeAllocationBitmap && (e[1] & 1) == m_vol.activeFat && !haveBitmap) {
            haveBitmap = true;
            m_vol.bitmapLength = qint64(get64(e + 24));
            if (m_vol.bitmapLength < g.bitmapBytes() || m_vol.bitmapLength > g.bitmapBytes() + g.clusterBytes())
                return corruptImage(QStringLiteral("its allocation bitmap has the wrong size"),
                                    QStringLiteral("bitmap %1 bytes for %2 clusters").arg(m_vol.bitmapLength).arg(g.clusterCount));
            if (!followChain(get32(e + 20), ceilDiv(m_vol.bitmapLength, g.clusterBytes()), m_vol.bitmapExtents, problem))
                return corruptImage(QStringLiteral("its allocation bitmap cannot be read"), problem);
        } else if (e[0] == kTypeUpcaseTable && !haveUpcase) {
            haveUpcase = true;
            upcaseChecksum = get32(e + 4);
            m_vol.upcaseLength = qint64(get64(e + 24));
            if (m_vol.upcaseLength < 2 || m_vol.upcaseLength > 2 * 65536 * 2)
                return corruptImage(QStringLiteral("its up-case table has an impossible size"));
            if (!followChain(get32(e + 20), ceilDiv(m_vol.upcaseLength, g.clusterBytes()), m_vol.upcaseExtents, problem))
                return corruptImage(QStringLiteral("its up-case table cannot be read"), problem);
        } else if (e[0] == kTypeVolumeLabel) {
            const int n = std::min<int>(e[1], kMaxLabelLength);
            QString label;
            for (int i = 0; i < n; ++i)
                label += QChar(get16(e + 2 + 2 * i));
            m_vol.label = label;
        }
    }
    if (!haveBitmap)
        return corruptImage(QStringLiteral("its allocation bitmap is missing"));
    if (!haveUpcase)
        return corruptImage(QStringLiteral("its up-case table is missing"));
    FW_TRY_ASSIGN(QByteArray up, readExtents(m_vol.upcaseExtents, m_vol.upcaseLength));
    const quint32 sum = tableChecksum(reinterpret_cast<const uchar*>(up.constData()), up.size());
    if (sum != upcaseChecksum)
        return corruptImage(QStringLiteral("its up-case table checksum does not match"),
                            QStringLiteral("computed %1, stored %2").arg(sum, 8, 16, QLatin1Char('0')).arg(upcaseChecksum, 8, 16, QLatin1Char('0')));
    FW_TRY_ASSIGN(m_upcase, UpcaseTable::fromData(up));
    m_vol.upcaseIsRecommended = (up == UpcaseTable::recommended().data());
    return success();
}

Result<quint32> ExfatReader::fatEntry(quint32 cluster)
{
    const Geometry& g = m_vol.boot.geo;
    const qint64 off = g.fatOffsetBytes() + qint64(m_vol.activeFat) * g.fatBytes() + qint64(cluster) * 4;
    const qint64 block = off / kFatBlock;
    auto it = m_fatCache.constFind(block);
    if (it == m_fatCache.cend()) {
        if (m_fatCache.size() > 1024)
            m_fatCache.clear();
        if (!io()->seek(block * kFatBlock))
            return Error(ErrorCode::IoError, QStringLiteral("Could not read the image."), io()->errorString());
        QByteArray b = io()->read(kFatBlock);
        if (b.size() < (off - block * kFatBlock) + 4)
            return corruptImage(QStringLiteral("its FAT is cut short"));
        it = m_fatCache.insert(block, b);
    }
    return get32(reinterpret_cast<const uchar*>(it->constData()) + (off - block * kFatBlock));
}

bool ExfatReader::followChain(quint32 first, qint64 expected, std::vector<Extent>& out, QString& problem,
                              const CancellationToken* token)
{
    const Geometry& g = m_vol.boot.geo;
    out.clear();
    problem.clear();
    // A chain that loops is cut off after as many clusters as it can really
    // have: its data length, or the largest folder for the root.
    const qint64 limit = std::min<qint64>(g.clusterCount,
                                          expected >= 0 ? expected : ceilDiv(kMaxDirectoryBytes, g.clusterBytes()));
    quint32 c = first;
    qint64 steps = 0;
    while (true) {
        if (token && steps % kChainCheckpoint == kChainCheckpoint - 1 && !token->checkpoint())
            return false;
        if (!g.isValidCluster(c)) {
            problem = QStringLiteral("cluster %1 is outside the volume").arg(c);
            return false;
        }
        appendCluster(out, c);
        ++steps;
        auto next = fatEntry(c);
        if (!next.ok()) {
            problem = next.error().toLogString();
            return false;
        }
        const quint32 n = next.value();
        if (expected >= 0 && steps == expected) {
            if (n < 0xFFFFFFF8u) {
                problem = QStringLiteral("the cluster chain from %1 is longer than its data").arg(first);
                return false;
            }
            return true;
        }
        if (n >= 0xFFFFFFF8u) // end of chain
            break;
        if (n == kFatBadCluster) {
            problem = QStringLiteral("the cluster chain from %1 runs into a bad cluster").arg(first);
            return false;
        }
        if (steps >= limit) {
            problem = expected >= 0 || limit == qint64(g.clusterCount)
                ? QStringLiteral("the cluster chain from %1 loops").arg(first)
                : QStringLiteral("the cluster chain from %1 is longer than a folder can be").arg(first);
            return false;
        }
        c = n;
    }
    if (expected >= 0 && steps < expected) {
        problem = QStringLiteral("the cluster chain from %1 ends after %2 of %3 clusters").arg(first).arg(steps).arg(expected);
        return false;
    }
    return true;
}

Result<QByteArray> ExfatReader::readExtents(const std::vector<Extent>& extents, qint64 bytes)
{
    const Geometry& g = m_vol.boot.geo;
    QByteArray out;
    out.reserve(qsizetype(bytes));
    for (const Extent& e : extents) {
        if (out.size() >= bytes)
            break;
        const qint64 n = std::min<qint64>(qint64(e.count) * g.clusterBytes(), bytes - out.size());
        if (!io()->seek(g.clusterOffset(e.cluster)))
            return Error(ErrorCode::IoError, QStringLiteral("Could not read the image."), io()->errorString());
        const QByteArray part = io()->read(n);
        if (part.size() != n)
            return corruptImage(QStringLiteral("the file is shorter than its contents"), io()->errorString());
        out += part;
    }
    return out;
}

Result<QByteArray> ExfatReader::readBitmap()
{
    return readExtents(m_vol.bitmapExtents, m_vol.bitmapLength);
}

Status ExfatReader::requireIntact(const ImageTree& tree)
{
    if (tree.problems.isEmpty())
        return success();
    QString what = tree.problems.first();
    if (tree.problemCount > 1)
        what += QStringLiteral(" (and %1)").arg(counted(tree.problemCount - 1, "more problem", "more problems"));
    return corruptImage(what, tree.problems.join(QStringLiteral("; ")));
}

Result<ImageTree> ExfatReader::readTree(const CancellationToken& token, bool checkAllocation)
{
    const Geometry& g = m_vol.boot.geo;
    const qint64 cb = g.clusterBytes();
    ImageTree tree;
    Problems problems(tree);
    ClusterUsage usage(g.clusterCount);
    // Clusters the entries claim. Without cross-links the sum cannot exceed
    // the heap, so the walk stops there instead of following the same chains
    // again for every entry of a damaged image.
    qint64 claimed = 0;
    bool stopped = false;
    for (const auto* list : {&m_vol.bitmapExtents, &m_vol.upcaseExtents, &m_vol.rootExtents})
        for (const Extent& e : *list)
            if (usage.mark(e))
                problems.add(QStringLiteral("the volume metadata overlaps itself"));
    if (m_vol.boot.volumeFlags & 0x2)
        tree.warnings << QStringLiteral("The image is marked as not cleanly unmounted (VolumeDirty).");
    if (!m_vol.backupBootMatches && !m_vol.backupProblem.isEmpty())
        tree.warnings << m_vol.backupProblem.at(0).toUpper() + m_vol.backupProblem.mid(1) + u'.';

    {
        ImageEntry root;
        root.isDir = true;
        root.attributes = kAttrDirectory;
        root.extents = m_vol.rootExtents;
        root.firstCluster = m_vol.boot.rootCluster;
        root.size = root.validSize = root.allocatedClusters() * cb;
        tree.entries.push_back(std::move(root));
    }

    std::vector<int> queue{0};
    std::vector<int> depths{0}; // folder levels above queue[i]
    for (size_t qi = 0; qi < queue.size() && !stopped; ++qi) {
        if (!token.checkpoint())
            return Error::cancelled();
        const int dirIdx = queue[qi];
        const qint64 dirBytes = tree.entries[size_t(dirIdx)].allocatedClusters() * cb;
        const QString dirPath = tree.entries[size_t(dirIdx)].path;
        auto data = readExtents(tree.entries[size_t(dirIdx)].extents, dirBytes);
        if (!data.ok())
            return data.takeError();
        const QByteArray& dir = data.value();
        const auto* d = reinterpret_cast<const uchar*>(dir.constData());
        const qint64 n = dir.size() / kEntrySize;
        const auto imageOffset = [&](qint64 entryIndex) {
            qint64 byte = entryIndex * kEntrySize;
            for (const Extent& e : tree.entries[size_t(dirIdx)].extents) {
                const qint64 run = qint64(e.count) * cb;
                if (byte < run)
                    return g.clusterOffset(e.cluster) + byte;
                byte -= run;
            }
            return qint64(-1);
        };
        QHash<QString, int> names;

        for (qint64 i = 0; i < n;) {
            const uchar* e = d + i * kEntrySize;
            const quint8 type = e[0];
            if (type == kTypeEndOfDirectory)
                break;
            if (!(type & kEntryInUse)) { // deleted or unused
                ++i;
                continue;
            }
            if (type != kTypeFile) {
                const bool rootCritical = dirIdx == 0
                    && (type == kTypeAllocationBitmap || type == kTypeUpcaseTable || type == kTypeVolumeLabel);
                if (!rootCritical && !(type & kEntryBenign))
                    problems.add(QStringLiteral("%1 holds an unknown directory entry (type 0x%2)")
                                     .arg(shownPath(dirPath))
                                     .arg(type, 2, 16, QLatin1Char('0')));
                ++i;
                continue;
            }
            const int secondary = e[1];
            if (secondary < 2 || secondary > 18 || i + secondary >= n) {
                problems.add(QStringLiteral("%1 holds a broken entry set").arg(shownPath(dirPath)));
                ++i;
                continue;
            }
            const int setCount = 1 + secondary;
            const quint16 sum = entrySetChecksum(e, setCount);
            const uchar* s = e + kEntrySize;
            if (sum != get16(e + 2) || s[0] != kTypeStreamExtension) {
                problems.add(QStringLiteral("an entry in %1 is damaged (%2)")
                                 .arg(shownPath(dirPath),
                                      sum != get16(e + 2) ? QStringLiteral("checksum mismatch")
                                                          : QStringLiteral("no stream extension")));
                i += setCount;
                continue;
            }
            const int nameLength = s[3];
            const int nameEntries = int(ceilDiv(nameLength, kNameCharsPerEntry));
            if (nameLength == 0 || nameEntries > secondary - 1) {
                problems.add(QStringLiteral("an entry in %1 has a broken name").arg(shownPath(dirPath)));
                i += setCount;
                continue;
            }
            QString name;
            name.reserve(nameLength);
            bool namesOk = true;
            for (int k = 0; k < nameEntries; ++k) {
                const uchar* f = e + kEntrySize * (2 + k);
                if (f[0] != kTypeFileName)
                    namesOk = false;
                for (int j = 0; j < kNameCharsPerEntry && name.size() < nameLength; ++j)
                    name += QChar(get16(f + 2 + 2 * j));
            }
            for (int k = 1 + nameEntries; k < secondary; ++k) {
                const quint8 t = e[kEntrySize * (1 + k)];
                if ((t & kEntryInUse) && !(t & kEntryBenign))
                    namesOk = false; // unknown critical secondary entry
            }
            const QString path = dirPath.isEmpty() ? name : dirPath + u'/' + name;
            if (!namesOk) {
                problems.add(QStringLiteral("the entry for %1 is damaged").arg(path));
                i += setCount;
                continue;
            }
            if (const QString why = checkFileName(name); !why.isEmpty())
                problems.add(QStringLiteral("%1 has an invalid name (%2)").arg(path, why));
            if (m_upcase.nameHash(name) != get16(s + 4))
                problems.add(QStringLiteral("the name hash of %1 does not match").arg(path));
            const QString key = m_upcase.upcase(name);
            if (names.contains(key))
                problems.add(QStringLiteral("%1 appears twice").arg(path));
            names.insert(key, 1);

            ImageEntry ent;
            ent.name = name;
            ent.path = path;
            ent.attributes = get16(e + 4);
            ent.isDir = ent.attributes & kAttrDirectory;
            ent.created = decodeTimestamp(get32(e + 8), e[20], e[22]);
            ent.modified = decodeTimestamp(get32(e + 12), e[21], e[23]);
            ent.accessed = decodeTimestamp(get32(e + 16), 0, e[24]);
            ent.validSize = qint64(get64(s + 8));
            ent.firstCluster = get32(s + 20);
            ent.size = qint64(get64(s + 24));
            ent.noFatChain = s[1] & kFlagNoFatChain;
            ent.parent = dirIdx;
            ent.entryOffset = imageOffset(i);
            ent.entryIndex = i;
            ent.entryCount = setCount;
            i += setCount;

            if (ent.size < 0 || ent.validSize < 0 || ent.validSize > ent.size) {
                problems.add(QStringLiteral("%1 has impossible sizes").arg(path));
                continue;
            }
            if (ent.isDir && (ent.size % cb != 0 || ent.size > kMaxDirectoryBytes)) {
                problems.add(QStringLiteral("the folder %1 has an impossible size").arg(path));
                continue;
            }
            qint64 dup = 0;
            if (ent.size > 0) {
                if (!(s[1] & kFlagAllocationPossible)) {
                    problems.add(QStringLiteral("%1 has data but no allocation").arg(path));
                    continue;
                }
                if (ent.size > qint64(g.clusterCount) * cb) {
                    problems.add(QStringLiteral("%1 is larger than the volume").arg(path));
                    continue;
                }
                const qint64 need = ceilDiv(ent.size, cb);
                claimed += need;
                if (claimed > qint64(g.clusterCount)) {
                    problems.add(QStringLiteral("the files and folders claim more clusters than the volume has; the rest "
                                                "of the image was not read"));
                    stopped = true;
                    break;
                }
                if (ent.noFatChain) {
                    if (!g.isValidCluster(ent.firstCluster)
                        || qint64(ent.firstCluster) - kFirstDataCluster + need > qint64(g.clusterCount)) {
                        problems.add(QStringLiteral("%1 points outside the volume").arg(path));
                        continue;
                    }
                    ent.extents.push_back({ent.firstCluster, quint32(need)});
                } else {
                    QString why;
                    if (!followChain(ent.firstCluster, need, ent.extents, why, &token)) {
                        if (why.isEmpty())
                            return Error::cancelled();
                        problems.add(QStringLiteral("%1: %2").arg(path, why));
                        continue;
                    }
                }
                for (const Extent& x : ent.extents)
                    dup += usage.mark(x);
                if (dup)
                    problems.add(QStringLiteral("%1 shares %2 with another file (cross-linked)")
                                     .arg(path, counted(dup, "cluster", "clusters")));
            } else if (ent.isDir) {
                // A folder without clusters is legal but unusual; it has no entries.
            }
            if (tree.entries.size() >= kMaxTreeEntries) {
                problems.add(QStringLiteral("the image holds more than %1 files and folders; the rest was not read")
                                 .arg(formatCount(qint64(kMaxTreeEntries))));
                stopped = true;
                break;
            }
            const int idx = int(tree.entries.size());
            tree.entries[size_t(dirIdx)].children.push_back(idx);
            if (ent.isDir) {
                ++tree.dirCount;
                // A folder that shares clusters may be one of its own
                // ancestors: listing it again would never end.
                if (dup)
                    problems.add(QStringLiteral("the contents of %1 were not read (its clusters belong to something else)")
                                     .arg(path));
                else if (depths[qi] + 1 > kMaxFolderDepth)
                    problems.add(QStringLiteral("%1 is nested more than %2 folders deep; its contents were not read")
                                     .arg(path)
                                     .arg(kMaxFolderDepth));
                else {
                    queue.push_back(idx);
                    depths.push_back(depths[qi] + 1);
                }
            } else {
                ++tree.fileCount;
                tree.totalBytes += ent.size;
            }
            tree.entries.push_back(std::move(ent));
        }
    }
    for (int i = 0; i < int(tree.entries.size()); ++i)
        tree.index.insert(tree.entries[size_t(i)].path, i);
    tree.usedClusters = usage.count();

    if (checkAllocation && !stopped) {
        auto bm = readBitmap();
        if (!bm.ok())
            return bm.takeError();
        const QByteArray& bitmap = bm.value();
        const auto& used = usage.bits();
        qint64 usedButFree = 0, lost = 0;
        for (size_t w = 0; w < used.size(); ++w) {
            quint64 alloc = 0;
            for (int b = 0; b < 8; ++b) {
                const qsizetype at = qsizetype(w * 8 + size_t(b));
                if (at < bitmap.size())
                    alloc |= quint64(uchar(bitmap.at(at))) << (8 * b);
            }
            if (w == used.size() - 1 && g.clusterCount % 64)
                alloc &= (quint64(1) << (g.clusterCount % 64)) - 1;
            usedButFree += std::popcount(used[w] & ~alloc);
            lost += std::popcount(alloc & ~used[w]);
        }
        if (usedButFree)
            problems.add(usedButFree == 1
                             ? QStringLiteral("1 cluster in use is marked free in the allocation bitmap")
                             : QStringLiteral("%1 clusters in use are marked free in the allocation bitmap")
                                   .arg(formatCount(usedButFree)));
        if (lost)
            tree.warnings << (lost == 1 ? QStringLiteral("1 cluster is marked in use but belongs to no file (lost cluster).")
                                        : QStringLiteral("%1 clusters are marked in use but belong to no file (lost clusters).")
                                              .arg(formatCount(lost)));
    }
    return tree;
}

// ---- ImageFileStream ---------------------------------------------------------------------

ImageFileStream::ImageFileStream(const QString& imagePath, const Geometry& geo) : m_path(imagePath), m_geo(geo) {}

ImageFileStream::ImageFileStream(std::shared_ptr<RandomSource> source, const Geometry& geo)
    : m_path(source->describe()), m_geo(geo), m_source(std::move(source))
{
}

Status ImageFileStream::open()
{
    if (m_source) {
        m_dev = openSourceDevice(m_source);
        return success();
    }
    m_file.setFileName(toLongPath(m_path));
    if (!m_file.open(QIODevice::ReadOnly))
        return Error(ErrorCode::IoError, QStringLiteral("Could not open the image %1.").arg(QDir::toNativeSeparators(m_path)),
                     m_file.errorString());
    return success();
}

Result<qint64> ImageFileStream::read(const ImageEntry& e, qint64 offset, char* buf, qint64 len)
{
    if (offset >= e.size || len <= 0)
        return qint64(0);
    len = std::min(len, e.size - offset);
    const qint64 cb = m_geo.clusterBytes();
    qint64 done = 0;
    while (done < len) {
        const qint64 pos = offset + done;
        if (pos >= e.validSize) {
            std::memset(buf + done, 0, size_t(len - done));
            done = len;
            break;
        }
        const qint64 clusterIndex = pos / cb;
        qint64 start = 0;
        const Extent* ext = nullptr;
        for (const Extent& x : e.extents) {
            if (clusterIndex < start + x.count) {
                ext = &x;
                break;
            }
            start += x.count;
        }
        if (!ext)
            return corruptImage(QStringLiteral("%1 is shorter on the image than its size says").arg(e.path));
        const qint64 within = pos - clusterIndex * cb;
        const qint64 runLeft = (start + ext->count - clusterIndex) * cb - within;
        const qint64 n = std::min({len - done, runLeft, e.validSize - pos});
        const qint64 at = m_geo.clusterOffset(quint32(ext->cluster + (clusterIndex - start))) + within;
        if (!io()->seek(at))
            return Error(ErrorCode::IoError, QStringLiteral("Could not read the image."), io()->errorString());
        qint64 got = 0;
        while (got < n) {
            const qint64 r = io()->read(buf + done + got, n - got);
            if (r < 0)
                return Error(ErrorCode::IoError, QStringLiteral("Could not read the image."), io()->errorString());
            if (r == 0)
                return corruptImage(QStringLiteral("the file is shorter than its contents"));
            got += r;
        }
        done += n;
    }
    return done;
}

} // namespace fw::exfat
