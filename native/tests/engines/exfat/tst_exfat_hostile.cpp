// Images made to hang a reader or to exhaust memory: a folder that points
// back at its parent, sizes that overflow, looping cluster chains, entries
// that claim the whole volume many times. A small valid image is patched for
// each case; the reader must report damage, and quickly.

#include "ExfatTestUtil.h"

#include "support/TestSupport.h"

#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QTest>

#include <functional>

using namespace fw;
using namespace fw::exfat;
using namespace exfattest;

namespace {

constexpr qint64 kQuickMs = 1000;

bool patch(const QString& path, qint64 off, const QByteArray& bytes)
{
    QFile f(path);
    return f.open(QIODevice::ReadWrite) && f.seek(off) && f.write(bytes) == bytes.size();
}

QByteArray le32(quint32 v)
{
    QByteArray b(4, '\0');
    put32(reinterpret_cast<uchar*>(b.data()), v);
    return b;
}

QByteArray le64(quint64 v)
{
    QByteArray b(8, '\0');
    put64(reinterpret_cast<uchar*>(b.data()), v);
    return b;
}

// Recomputes the checksum sector of the main boot region (512-byte sectors)
// and copies the region over the backup.
bool fixBootChecksum(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadWrite))
        return false;
    QByteArray region = f.read(512 * kBootRegionSectors);
    if (region.size() != 512 * kBootRegionSectors)
        return false;
    auto* r = reinterpret_cast<uchar*>(region.data());
    const quint32 sum = bootChecksum(r, 512);
    for (int off = 0; off < 512; off += 4)
        put32(r + 11 * 512 + off, sum);
    return f.seek(0) && f.write(region) == region.size() && f.write(region) == region.size();
}

// Changes the entry set at `entryOffset` (File entry first) and stores the
// matching set checksum, as any tool that edits an image would.
bool rewriteEntrySet(const QString& path, qint64 entryOffset, int entryCount, const std::function<void(uchar*)>& change)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadWrite) || !f.seek(entryOffset))
        return false;
    QByteArray set = f.read(qint64(entryCount) * kEntrySize);
    if (set.size() != qint64(entryCount) * kEntrySize)
        return false;
    auto* e = reinterpret_cast<uchar*>(set.data());
    change(e);
    put16(e + 2, entrySetChecksum(e, entryCount));
    return f.seek(entryOffset) && f.write(set) == set.size();
}

// A chain of `depth` empty folders, each inside the one before.
SourceTree nestedFolders(int depth)
{
    SourceTree t;
    SourceNode root;
    root.isDir = true;
    root.attributes = kAttrDirectory;
    t.nodes.push_back(root);
    for (int i = 1; i <= depth; ++i) {
        SourceNode n;
        n.name = QStringLiteral("d");
        n.relativePath = i == 1 ? n.name : t.nodes[size_t(i - 1)].relativePath + QStringLiteral("/d");
        n.isDir = true;
        n.attributes = kAttrDirectory;
        n.parent = i - 1;
        t.nodes[size_t(i - 1)].children.push_back(i);
        t.nodes.push_back(n);
    }
    t.dirCount = depth;
    return t;
}

} // namespace

class TestExfatHostile : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_tmp;
    QString m_image;
    Geometry m_geo;
    quint32 m_rootCluster = 0;
    qint64 m_rootBytes = 0;

    QString path(const QString& name) const { return m_tmp.filePath(name); }

    QString copyOf(const QString& name)
    {
        const QString p = path(name);
        QFile::remove(p);
        return QFile::copy(m_image, p) ? p : QString();
    }

    // Offset and entry count of the entry set of `imagePath` in the sample image.
    bool locate(const QString& imagePath, qint64& offset, int& count)
    {
        CancellationToken token;
        auto reader = ExfatReader::open(m_image);
        if (!reader.ok())
            return false;
        auto tree = reader.value()->readTree(token);
        if (!tree.ok())
            return false;
        const int idx = tree->find(imagePath);
        if (idx <= 0)
            return false;
        offset = tree->entries[size_t(idx)].entryOffset;
        count = tree->entries[size_t(idx)].entryCount;
        return offset > 0;
    }

    WriterOptions smallImage() const
    {
        WriterOptions o;
        o.clusterBytes = 4096;
        o.spare = SpareMode::Fixed;
        o.spareBytes = kMiB;
        return o;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_tmp.isValid());
        fwtest::isolateAppData(m_tmp);
        const QString src = path(QStringLiteral("src"));
        QVERIFY(writeBytes(src + QStringLiteral("/D/inner.txt"), "inside"));
        QVERIFY(writeBytes(src + QStringLiteral("/D/sub/deeper.txt"), "deeper"));
        QVERIFY(writeRandom(src + QStringLiteral("/a.bin"), 200 * 1024, 1));
        QVERIFY(writeRandom(src + QStringLiteral("/b.bin"), 50 * 1024, 2));
        m_image = path(QStringLiteral("good.exfat"));
        auto plan = build(src, m_image, smallImage());
        QVERIFY2(plan.ok(), qPrintable(plan.ok() ? QString() : plan.error().toLogString()));

        CancellationToken token;
        auto reader = ExfatReader::open(m_image);
        QVERIFY(reader.ok());
        auto tree = reader.value()->readTree(token);
        QVERIFY(tree.ok());
        QVERIFY2(tree->problems.isEmpty(), qPrintable(tree->problems.join(u'\n')));
        m_geo = reader.value()->geometry();
        m_rootCluster = reader.value()->volume().boot.rootCluster;
        m_rootBytes = tree->root().size;
        QVERIFY(m_rootBytes > 0);
    }

    // A folder whose clusters are those of its parent used to be listed
    // again and again, one level deeper each time, until memory ran out.
    void folderPointingAtItsParentIsNotFollowed()
    {
        qint64 off = 0;
        int count = 0;
        QVERIFY(locate(QStringLiteral("D"), off, count));
        const QString img = copyOf(QStringLiteral("cycle.exfat"));
        QVERIFY(!img.isEmpty());
        QVERIFY(rewriteEntrySet(img, off, count, [&](uchar* e) {
            uchar* s = e + kEntrySize;
            s[1] = kFlagAllocationPossible | kFlagNoFatChain;
            put64(s + 8, quint64(m_rootBytes));
            put32(s + 20, m_rootCluster);
            put64(s + 24, quint64(m_rootBytes));
        }));

        QElapsedTimer timer;
        timer.start();
        CancellationToken token;
        auto reader = ExfatReader::open(img);
        QVERIFY2(reader.ok(), qPrintable(reader.ok() ? QString() : reader.error().toLogString()));
        for (const bool checkAllocation : {true, false}) {
            auto tree = reader.value()->readTree(token, checkAllocation);
            QVERIFY(tree.ok());
            const QString problems = tree->problems.join(u'\n');
            QVERIFY2(problems.contains(QStringLiteral("cross-linked")), qPrintable(problems));
            QVERIFY2(problems.contains(QStringLiteral("the contents of D were not read")), qPrintable(problems));
            // The root's own entries, nothing below D.
            QCOMPARE(tree->find(QStringLiteral("D/D")), -1);
            QVERIFY(tree->entries.size() <= 8);
            QVERIFY(!ExfatReader::requireIntact(tree.value()).ok());
        }
        QVERIFY2(timer.elapsed() < kQuickMs, qPrintable(QString::number(timer.elapsed())));
    }

    void foldersNestedDeeperThanTheLimitAreNotFollowed_data()
    {
        QTest::addColumn<int>("depth");
        QTest::addColumn<int>("read");
        QTest::newRow("200 deep reads completely") << 200 << 200;
        QTest::newRow("300 deep stops at 256") << 300 << 257;
    }

    void foldersNestedDeeperThanTheLimitAreNotFollowed()
    {
        QFETCH(int, depth);
        QFETCH(int, read);
        const SourceTree src = nestedFolders(depth);
        const WriterOptions o = [this] {
            WriterOptions w = smallImage();
            w.label = QStringLiteral("DEEP");
            w.syncToDisk = false;
            return w;
        }();
        auto plan = planImage(src, o);
        QVERIFY2(plan.ok(), qPrintable(plan.ok() ? QString() : plan.error().toLogString()));
        const QString img = path(QStringLiteral("deep-%1.exfat").arg(depth));
        CancellationToken token;
        const auto written = writeImage(src, plan.value(), img, o, token);
        QVERIFY2(written.ok(), qPrintable(written.ok() ? QString() : written.error().toLogString()));

        QElapsedTimer timer;
        timer.start();
        auto reader = ExfatReader::open(img);
        QVERIFY(reader.ok());
        auto tree = reader.value()->readTree(token);
        QVERIFY(tree.ok());
        QVERIFY2(timer.elapsed() < kQuickMs, qPrintable(QString::number(timer.elapsed())));
        QCOMPARE(tree->dirCount, qint64(read));
        const QString problems = tree->problems.join(u'\n');
        if (read == depth)
            QVERIFY2(problems.isEmpty(), qPrintable(problems));
        else
            QVERIFY2(problems.contains(QStringLiteral("nested more than 256 folders deep")), qPrintable(problems));
    }

    // VolumeLength is a 64-bit sector count. One that overflows as a byte
    // count used to switch off the "file shorter than its volume" test, after
    // which the cluster count could be anything.
    void volumeLengthThatOverflowsIsRefused()
    {
        const QString img = copyOf(QStringLiteral("overflow.exfat"));
        QVERIFY(!img.isEmpty());
        QVERIFY(patch(img, 72, le64(quint64(1) << 55)));
        QVERIFY(patch(img, 84, le32(quint32(1) << 25)));  // FatLength
        QVERIFY(patch(img, 92, le32(kMaxClusterCount))); // ClusterCount
        // The root folder's chain loops over two clusters.
        const qint64 fat = m_geo.fatOffsetBytes();
        QVERIFY(patch(img, fat + qint64(m_rootCluster) * 4, le32(m_rootCluster + 1)));
        QVERIFY(patch(img, fat + qint64(m_rootCluster + 1) * 4, le32(m_rootCluster)));
        QVERIFY(fixBootChecksum(img));

        QElapsedTimer timer;
        timer.start();
        auto reader = ExfatReader::open(img);
        QVERIFY2(timer.elapsed() < kQuickMs, qPrintable(QString::number(timer.elapsed())));
        QVERIFY(!reader.ok());
        QCOMPARE(reader.error().code, ErrorCode::CorruptData);
        QVERIFY2(reader.error().detail.contains(QStringLiteral("VolumeLength")), qPrintable(reader.error().toLogString()));
    }

    // A volume that claims more clusters than the file holds is refused
    // before anything is sized from that count.
    void clusterCountBeyondTheFileIsRefused()
    {
        const QString img = copyOf(QStringLiteral("clusters.exfat"));
        QVERIFY(!img.isEmpty());
        // 64 GiB of 512-byte sectors, with a FAT and a cluster count to match.
        QVERIFY(patch(img, 72, le64(quint64(64) << 21)));
        QVERIFY(patch(img, 84, le32(quint32(1) << 17)));
        QVERIFY(patch(img, 88, le32((quint32(1) << 17) + m_geo.fatOffset)));
        QVERIFY(patch(img, 92, le32(8'000'000)));
        QVERIFY(fixBootChecksum(img));

        QElapsedTimer timer;
        timer.start();
        auto reader = ExfatReader::open(img);
        QVERIFY2(timer.elapsed() < kQuickMs, qPrintable(QString::number(timer.elapsed())));
        QVERIFY(!reader.ok());
        QCOMPARE(reader.error().code, ErrorCode::CorruptData);
        QVERIFY2(reader.error().message.contains(QStringLiteral("shorter than the volume")),
                 qPrintable(reader.error().toLogString()));
    }

    void loopingRootChainIsCutOff()
    {
        const QString img = copyOf(QStringLiteral("root-loop.exfat"));
        QVERIFY(!img.isEmpty());
        QVERIFY(patch(img, m_geo.fatOffsetBytes() + qint64(m_rootCluster) * 4, le32(m_rootCluster)));

        QElapsedTimer timer;
        timer.start();
        auto reader = ExfatReader::open(img);
        QVERIFY2(timer.elapsed() < kQuickMs, qPrintable(QString::number(timer.elapsed())));
        QVERIFY(!reader.ok());
        QCOMPARE(reader.error().code, ErrorCode::CorruptData);
        QVERIFY2(reader.error().message.contains(QStringLiteral("root folder cannot be read")),
                 qPrintable(reader.error().toLogString()));
    }

    // Every entry may claim the whole heap; following all of them costs
    // entries x clusters. The walk stops once more is claimed than exists.
    void entriesClaimingTheWholeVolumeStopTheWalk()
    {
        const QString img = copyOf(QStringLiteral("claims.exfat"));
        QVERIFY(!img.isEmpty());
        const quint64 all = quint64(m_geo.clusterCount) * quint64(m_geo.clusterBytes());
        for (const QString& name : {QStringLiteral("a.bin"), QStringLiteral("b.bin")}) {
            qint64 off = 0;
            int count = 0;
            QVERIFY(locate(name, off, count));
            QVERIFY(rewriteEntrySet(img, off, count, [&](uchar* e) {
                uchar* s = e + kEntrySize;
                s[1] = kFlagAllocationPossible | kFlagNoFatChain;
                put64(s + 8, all);
                put32(s + 20, kFirstDataCluster);
                put64(s + 24, all);
            }));
        }

        QElapsedTimer timer;
        timer.start();
        CancellationToken token;
        auto reader = ExfatReader::open(img);
        QVERIFY(reader.ok());
        auto tree = reader.value()->readTree(token);
        QVERIFY(tree.ok());
        QVERIFY2(timer.elapsed() < kQuickMs, qPrintable(QString::number(timer.elapsed())));
        const QString problems = tree->problems.join(u'\n');
        QVERIFY2(problems.contains(QStringLiteral("claim more clusters than the volume has")), qPrintable(problems));
        QVERIFY(!ExfatReader::requireIntact(tree.value()).ok());
    }

    // A size near 2^63 must not overflow the cluster arithmetic.
    void fileLargerThanTheVolumeIsAProblem()
    {
        qint64 off = 0;
        int count = 0;
        QVERIFY(locate(QStringLiteral("a.bin"), off, count));
        const QString img = copyOf(QStringLiteral("huge-file.exfat"));
        QVERIFY(!img.isEmpty());
        QVERIFY(rewriteEntrySet(img, off, count, [](uchar* e) {
            uchar* s = e + kEntrySize;
            put64(s + 24, quint64(0x7FFFFFFFFFFFFFFFLL));
        }));
        CancellationToken token;
        auto reader = ExfatReader::open(img);
        QVERIFY(reader.ok());
        auto tree = reader.value()->readTree(token);
        QVERIFY(tree.ok());
        const QString problems = tree->problems.join(u'\n');
        QVERIFY2(problems.contains(QStringLiteral("a.bin is larger than the volume")), qPrintable(problems));
    }
};

QTEST_GUILESS_MAIN(TestExfatHostile)
#include "tst_exfat_hostile.moc"
