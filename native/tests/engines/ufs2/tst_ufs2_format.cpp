// Format-level pieces: the .NET Random port, UFS2Tool's geometry and sizing
// rules, parameter validation, directory packing and name encoding.

#include "Ufs2TestUtil.h"

#include "support/SparseFile.h"

#include <QTemporaryDir>

using namespace ufs2test;

class TestUfs2Format : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_appData;

private slots:
    void initTestCase()
    {
        QVERIFY(m_appData.isValid());
        fwtest::isolateAppData(m_appData);
    }

    void dotNetRandomMatchesDotNet()
    {
        // Printed by .NET 8: new Random(seed).Next(1, int.MaxValue), six times.
        const QList<QPair<int, QList<qint32>>> expected = {
            {1, {534011718, 237820880, 1002897798, 1657007234, 1412011072, 929393559}},
            {2, {1655911537, 867932563, 356479430, 2115372437, 234085668, 658591161}},
            {3, {630327709, 1498044246, 1857544709, 426253993, 1203643911, 387788763}},
            {17, {1304539646, 1729673220, 1397622145, 400915894, 1892557431, 891522485}},
            {1000, {325467165, 506683626, 1623525913, 2344573, 1485571032, 980737479}},
        };
        for (const auto& [seed, values] : expected) {
            DotNetRandom r(seed);
            for (qint32 v : values)
                QCOMPARE(r.next(1, INT_MAX), v);
        }
        DotNetRandom r(1);
        qint32 last = 0;
        for (int i = 0; i < 1000; ++i)
            last = r.next(1, INT_MAX);
        QCOMPARE(last, qint32(88617824));
    }

    void geometryMatchesUfs2Tool()
    {
        // UFS2Tool 4.0.0 / dumpfs.ufs for an 11,010,048-byte image with the
        // legacy parameters: ncg 1, fpg 256, ipg 256, sblkno 2, cblkno 3,
        // iblkno 4, dblkno 5, cssize 65536, cgsize 65536, maxfilesize below.
        auto g = computeGeometry(FormatParams{}, 11010048);
        QVERIFY(g.ok());
        QCOMPARE(g->ncg, 1);
        QCOMPARE(g->fpg, 256);
        QCOMPARE(g->ipg, 256);
        QCOMPARE(g->sblkno, 2);
        QCOMPARE(g->cblkno, 3);
        QCOMPARE(g->iblkno, 4);
        QCOMPARE(g->dblkno, 5);
        QCOMPARE(g->csSize, 65536);
        QCOMPARE(g->cgSize, 65536);
        QCOMPARE(g->totalFrags, qint64(168));
        QCOMPARE(g->maxFileSize, qint64(36033195603132415LL));
        QCOMPARE(g->contigSumSize, 1);
        QCOMPARE(g->fsbtodb, 7);
        // 525,336,576 bytes (the 30-file case): 16 groups of 512 fragments.
        auto g2 = computeGeometry(FormatParams{}, 525336576);
        QVERIFY(g2.ok());
        QCOMPARE(g2->ncg, 16);
        QCOMPARE(g2->ipg, 512);
        QCOMPARE(g2->fpg, 512);
        QCOMPARE(g2->dblkno, 6);
    }

    void autoSizeMatchesUfs2Tool()
    {
        // UFS2Tool printed "Auto-sized to: 11,010,048 bytes" for eboot.bin
        // (200,000 bytes) + sce_sys/param.json (24 bytes).
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QVERIFY(mkfile(tmp.filePath("src/eboot.bin"), 200000, 1));
        QVERIFY(mktext(tmp.filePath("src/sce_sys/param.json"), "{\"titleId\":\"PPSA00000\"}\n"));
        CancellationToken token;
        auto tree = scanLocalFolder(tmp.filePath("src"), token);
        QVERIFY(tree.ok());
        QCOMPARE(ufs2toolAutoSize(tree.value(), FormatParams{}), qint64(11010048));
        // ...and 525,336,576 bytes for 30 files of 1..30 MB (the multi-group case).
        // Only the sizes count, so the files are sparse where the file system
        // can do that: 465 MB of sizes, next to nothing on disk.
        QTemporaryDir tmp2;
        QVERIFY(tmp2.isValid());
        QVERIFY(QDir().mkpath(tmp2.filePath("src")));
        for (int i = 1; i <= 30; ++i) {
            const QString path = tmp2.filePath(QStringLiteral("src/f%1.bin").arg(i));
            const qint64 size = qint64(i) * 1000000;
            if (fwtest::makeSparseFile(path, size) < 0) {
                QFile f(path);
                QVERIFY(f.open(QIODevice::WriteOnly) && f.resize(size));
            }
        }
        auto tree2 = scanLocalFolder(tmp2.filePath("src"), token);
        QVERIFY(tree2.ok());
        QCOMPARE(ufs2toolAutoSize(tree2.value(), FormatParams{}), qint64(525336576));
    }

    void validatesParameters()
    {
        QVERIFY(validateParams(FormatParams{}).ok());
        QVERIFY(validateParams(FormatParams{32768, 32768, 0, 131072}).ok());
        QVERIFY(validateParams(FormatParams{65536, 8192, 99, 524288}).ok());
        // The legacy UI offered 128 KiB blocks; UFS2Tool rejects them.
        FWTEST_COMPARE_ERROR(validateParams(FormatParams{131072, 65536, 0, 262144}), ErrorCode::InvalidArgument);
        QVERIFY(!validateParams(FormatParams{32768, 65536, 0, 262144}).ok()); // fragment > block
        QVERIFY(!validateParams(FormatParams{65536, 4096, 0, 262144}).ok());  // 16 fragments per block
        QVERIFY(!validateParams(FormatParams{65536, 65536, 100, 262144}).ok());
        QVERIFY(!validateParams(FormatParams{65536, 65536, -1, 262144}).ok());
        QVERIFY(!computeGeometry(FormatParams{}, 65536 * 15).ok());   // under 16 blocks
        QVERIFY(!computeGeometry(FormatParams{}, 65536 * 20 + 512).ok()); // not fragment aligned
    }

    void directoryPacking()
    {
        // "." + ".." + n entries of 100-byte names (112-byte records, 4 per
        // 512-byte chunk) in 65,536-byte blocks: 128 chunks per block.
        std::vector<DirEnt> entries;
        for (int i = 0; i < 1000; ++i)
            entries.push_back({quint32(3 + i), kDtReg, QByteArray(100, char('a' + i % 26)) + QByteArray::number(i).rightJustified(4, '0')});
        auto nb = dirBlocksNeeded(entries, 65536);
        QVERIFY(nb.ok());
        auto blocks = buildDirBlocks(2, 2, entries, nb.value(), 65536);
        QVERIFY(blocks.ok());
        QCOMPARE(blocks->size(), qsizetype(nb.value()) * 65536);
        // Walk the chunks: every record stays inside its 512-byte chunk, the
        // entries come back in order, and empty chunks hold padding records.
        int seen = 0;
        const QByteArray& b = blocks.value();
        for (qsizetype chunk = 0; chunk < b.size(); chunk += 512) {
            int off = 0;
            while (off < 512) {
                const char* p = b.constData() + chunk + off;
                const int reclen = rd16(p + 4);
                QVERIFY(reclen >= direntSize(quint8(p[7])) && reclen % 4 == 0 && off + reclen <= 512);
                const quint32 ino = rd32(p);
                if (ino != 0) {
                    const QByteArray name(p + 8, quint8(p[7]));
                    if (seen == 0)
                        QCOMPARE(name, QByteArray("."));
                    else if (seen == 1)
                        QCOMPARE(name, QByteArray(".."));
                    else
                        QCOMPARE(name, entries[size_t(seen - 2)].name);
                    ++seen;
                }
                off += reclen;
            }
        }
        QCOMPARE(seen, 1002);
        // 255 bytes is the longest name; 256 is refused.
        std::vector<DirEnt> longName{{3, kDtReg, QByteArray(255, 'x')}};
        QVERIFY(dirBlocksNeeded(longName, 65536).ok());
        longName[0].name = QByteArray(256, 'x');
        QVERIFY(!dirBlocksNeeded(longName, 65536).ok());
    }

    void encodesNames()
    {
        QCOMPARE(encodeName(QStringLiteral("eboot.bin")).value(), QByteArray("eboot.bin"));
        QCOMPARE(encodeName(QStringLiteral("日本語.txt")).value(), QStringLiteral("日本語.txt").toUtf8());
        QVERIFY(encodeName(QString(255, u'a')).ok());
        QVERIFY(!encodeName(QString(256, u'a')).ok());
        QVERIFY(!encodeName(QString(100, QChar(0x65E5))).ok()); // 300 UTF-8 bytes
        QVERIFY(!encodeName(QStringLiteral("..")).ok());
        QVERIFY(!encodeName(QStringLiteral("a/b")).ok());
        QVERIFY(!encodeName(QString()).ok());
    }

    void inodeRoundTrip()
    {
        Inode in;
        in.mode = kIfReg | kPerm0555;
        in.nlink = 3;
        in.size = 0x123456789LL;
        in.blocks = 99;
        in.mtime = 1793769709;
        in.gen = -5;
        in.dirdepth = 7;
        for (int i = 0; i < kNDirect; ++i)
            in.db[i] = 1000 + i;
        in.ib[2] = 1LL << 40;
        char buf[kInodeSize];
        serializeInode(in, buf);
        const Inode out = parseInode(buf);
        QCOMPARE(out.mode, in.mode);
        QCOMPARE(out.nlink, in.nlink);
        QCOMPARE(out.size, in.size);
        QCOMPARE(out.blocks, in.blocks);
        QCOMPARE(out.mtime, in.mtime);
        QCOMPARE(out.gen, in.gen);
        QCOMPARE(out.dirdepth, in.dirdepth);
        QCOMPARE(out.db[11], qint64(1011));
        QCOMPARE(out.ib[2], qint64(1LL << 40));
        QCOMPARE(rd64(buf + 0x10), in.size); // di_size at 0x10
        QCOMPARE(rd32s(buf + 0x50), in.gen); // di_gen at 0x50
    }
};

QTEST_GUILESS_MAIN(TestUfs2Format)
#include "tst_ufs2_format.moc"
