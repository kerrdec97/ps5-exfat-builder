// exFAT format helpers: checksums, the up-case table, name hashes,
// timestamps, names, geometry, the legacy sizing rules, and reading a volume
// formatted by mkfs.exfat (exfatprogs).

#include "ExfatTestUtil.h"

#include "engines/exfat/ExfatFormat.h"
#include "engines/exfat/ExfatReader.h"
#include "engines/exfat/ExfatWriter.h"
#include "support/TestSupport.h"

#include <QTemporaryDir>
#include <QTest>
#include <QTimeZone>

using namespace fw;
using namespace fw::exfat;

class TestExfatFormat : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_tmp;

private slots:
    void initTestCase()
    {
        QVERIFY(m_tmp.isValid());
        fwtest::isolateAppData(m_tmp);
    }

    void upcaseTableIsTheSpecificationsRecommendedTable()
    {
        const UpcaseTable& t = UpcaseTable::recommended();
        QCOMPARE(t.data().size(), 5836);
        QCOMPARE(t.checksum(), 0xE619D30Du);
        QCOMPARE(int(t.map(u'a')), int(u'A'));
        QCOMPARE(int(t.map(u'z')), int(u'Z'));
        QCOMPARE(int(t.map(u'A')), int(u'A'));
        QCOMPARE(int(t.map(u'1')), int(u'1'));
        QCOMPARE(int(t.map(char16_t(0x00E9))), 0x00C9); // é -> É
        QCOMPARE(int(t.map(char16_t(0x044F))), 0x042F); // я -> Я
        QCOMPARE(int(t.map(char16_t(0x03C9))), 0x03A9); // ω -> Ω
        QCOMPARE(int(t.map(char16_t(0xFF41))), 0xFF21); // fullwidth a -> A
        QCOMPARE(int(t.map(char16_t(0x65E5))), 0x65E5); // CJK unchanged
        QCOMPARE(t.upcase(QStringLiteral("eboot.bin")), QStringLiteral("EBOOT.BIN"));
    }

    void nameHashIgnoresCase()
    {
        const UpcaseTable& t = UpcaseTable::recommended();
        QCOMPARE(t.nameHash(QStringLiteral("eboot.bin")), t.nameHash(QStringLiteral("EBOOT.BIN")));
        QVERIFY(t.nameHash(QStringLiteral("eboot.bin")) != t.nameHash(QStringLiteral("eboot.bim")));
        // Hand-computed from the specification's algorithm (§7.6.4) for "A".
        quint16 h = 0;
        for (const quint8 b : {quint8(0x41), quint8(0x00)})
            h = quint16(((h & 1) ? 0x8000 : 0) + (h >> 1) + b);
        QCOMPARE(t.nameHash(QStringLiteral("a")), h);
    }

    void checksumsFollowTheSpecification()
    {
        // Entry-set checksum skips bytes 2 and 3.
        QByteArray set(3 * 32, '\0');
        set[0] = static_cast<char>(static_cast<unsigned char>(0x85));
        set[1] = 2;
        auto* p = reinterpret_cast<uchar*>(set.data());
        const quint16 before = entrySetChecksum(p, 3);
        set[2] = char(0x12);
        set[3] = char(0x34);
        QCOMPARE(entrySetChecksum(p, 3), before);
        set[4] = 1;
        QVERIFY(entrySetChecksum(p, 3) != before);

        // Boot checksum skips VolumeFlags and PercentInUse.
        BootInfo info;
        auto g = computeGeometry(64 * kMiB, 512, 32768);
        QVERIFY(g.ok());
        info.geo = g.value();
        info.rootCluster = 4;
        info.serial = 0x12345678;
        QByteArray region = buildBootRegion(info);
        auto* r = reinterpret_cast<uchar*>(region.data());
        const quint32 sum = bootChecksum(r, 512);
        QCOMPARE(get32(r + 11 * 512), sum);
        QCOMPARE(get32(r + 11 * 512 + 508), sum);
        r[106] ^= 0xFF;
        r[112] ^= 0xFF;
        QCOMPARE(bootChecksum(r, 512), sum);
        r[100] ^= 0x01;
        QVERIFY(bootChecksum(r, 512) != sum);
    }

    void bootRegionParsesBackAndRejectsDamage()
    {
        BootInfo info;
        auto g = computeGeometry(1 * kGiB, 512, 65536);
        QVERIFY(g.ok());
        info.geo = g.value();
        info.rootCluster = 7;
        info.serial = 0xCAFEF00D;
        info.percentInUse = 42;
        const QByteArray region = buildBootRegion(info);
        auto parsed = parseBootRegion(region);
        QVERIFY2(parsed.ok(), qPrintable(parsed.ok() ? QString() : parsed.error().toLogString()));
        QCOMPARE(parsed->geo.clusterCount, info.geo.clusterCount);
        QCOMPARE(parsed->geo.fatOffset, info.geo.fatOffset);
        QCOMPARE(parsed->geo.clusterHeapOffset, info.geo.clusterHeapOffset);
        QCOMPARE(parsed->rootCluster, 7u);
        QCOMPARE(parsed->serial, 0xCAFEF00Du);
        QCOMPARE(int(parsed->percentInUse), 42);

        for (int offset : {0, 3, 72, 92, 108, 510, 512 + 509, 11 * 512 + 3}) {
            QByteArray bad = region;
            bad[offset] = char(bad[offset] ^ 0x5A);
            auto e = parseBootRegion(bad);
            QVERIFY2(!e.ok(), qPrintable(QString::number(offset)));
            FWTEST_COMPARE_ERROR(e, ErrorCode::CorruptData);
            QVERIFY(e.error().message.startsWith(QStringLiteral("The image is damaged")));
        }
    }

    void geometryIsAlignedAndWithinLimits()
    {
        for (int cluster : {4096, 32768, 65536, 1 << 20}) {
            auto g = computeGeometry(10 * kGiB + 12345, 512, cluster);
            QVERIFY(g.ok());
            QCOMPARE(g->fatOffsetBytes() % kMiB, 0);
            QCOMPARE(g->heapOffsetBytes() % std::max<qint64>(kMiB, cluster), 0);
            QVERIFY(qint64(g->fatLength) * 512 >= (qint64(g->clusterCount) + 2) * 4);
            QVERIFY(g->heapOffsetBytes() + qint64(g->clusterCount) * cluster <= g->volumeBytes());
            QVERIFY(g->volumeBytes() - (g->heapOffsetBytes() + qint64(g->clusterCount) * cluster) < cluster);
        }
        auto s4k = computeGeometry(1 * kGiB, 4096, 65536);
        QVERIFY(s4k.ok());
        QCOMPARE(s4k->bytesPerSectorShift, 12);
        QVERIFY(!computeGeometry(1 * kMiB, 512, 32768).ok());       // too small
        QVERIFY(!computeGeometry(1 * kGiB, 1024 * 3, 32768).ok());  // bad sector size
        QVERIFY(!computeGeometry(1 * kGiB, 512, 3000).ok());        // bad cluster size
        QVERIFY(!computeGeometry(1 * kGiB, 512, 64 * kMiB).ok());   // cluster > 32 MiB
        QVERIFY(!computeGeometry(32 * 1024 * kGiB, 512, 4096).ok()); // > 2^32 clusters
    }

    void timestampsRoundTripWithTheirOffset()
    {
        const QDate d(2021, 5, 6);
        const QTime t(7, 8, 9, 123);
        for (int offsetSec : {0, 3600, -18000, 19800, 45900}) {
            const QDateTime in(d, t, QTimeZone(offsetSec));
            const Timestamp ts = encodeTimestamp(in);
            QVERIFY(ts.utcOffset & 0x80);
            const QDateTime out = decodeTimestamp(ts.value, ts.tenMs, ts.utcOffset);
            QVERIFY(out.isValid());
            // 10 ms resolution: 07:08:09.123 -> .120
            QCOMPARE(out.toMSecsSinceEpoch(), in.toMSecsSinceEpoch() - 3);
        }
        // Clamped range and invalid input.
        const Timestamp old = encodeTimestamp(QDateTime(QDate(1970, 1, 1), QTime(0, 0), QTimeZone::utc()));
        QCOMPARE(decodeTimestamp(old.value, old.tenMs, old.utcOffset).date().year(), 1980);
        const Timestamp none = encodeTimestamp(QDateTime());
        QVERIFY(decodeTimestamp(none.value, none.tenMs, none.utcOffset).isValid());
        QVERIFY(!decodeTimestamp(0, 0, 0).isValid()); // month 0
    }

    void namesAndLabelsAreChecked()
    {
        QVERIFY(checkFileName(QStringLiteral("eboot.bin")).isEmpty());
        QVERIFY(checkFileName(QString(255, u'x')).isEmpty());
        QVERIFY(!checkFileName(QString(256, u'x')).isEmpty());
        QVERIFY(!checkFileName(QString()).isEmpty());
        QVERIFY(!checkFileName(QStringLiteral(".")).isEmpty());
        QVERIFY(!checkFileName(QStringLiteral("..")).isEmpty());
        for (const char* bad : {"a:b", "a*b", "a?b", "a\"b", "a<b", "a>b", "a|b", "a\\b", "a/b"})
            QVERIFY2(!checkFileName(QString::fromLatin1(bad)).isEmpty(), bad);
        QVERIFY(!checkFileName(QStringLiteral("tab\there")).isEmpty());
        QVERIFY(checkFileName(QString::fromUtf8("日本語 🎮")).isEmpty());
        QVERIFY(checkVolumeLabel(QStringLiteral("OSFIMG")).isEmpty());
        QVERIFY(checkVolumeLabel(QString()).isEmpty());
        QVERIFY(checkVolumeLabel(QStringLiteral("PPSA01234AB")).isEmpty());
        QVERIFY(!checkVolumeLabel(QStringLiteral("PPSA01234ABC")).isEmpty());
        QVERIFY(!checkVolumeLabel(QStringLiteral("A:B")).isEmpty());
    }

    void automaticClusterIs64KiB()
    {
        // ShadowMount+ 1.7 mounts exFAT through its LVD/BFS fast path only with
        // 64 KiB clusters, so Automatic no longer picks 32 KiB for small files
        // (the rule of earlier versions, Get-OptimalExfatClusterSizeFromStats).
        QCOMPARE(kAutoClusterBytes, 65536);
        SourceTree t;
        t.nodes.resize(3);
        t.nodes[0].isDir = true;
        t.nodes[0].children = {1, 2};
        for (int i = 1; i <= 2; ++i) {
            t.nodes[size_t(i)].name = QStringLiteral("small%1.bin").arg(i);
            t.nodes[size_t(i)].relativePath = t.nodes[size_t(i)].name;
            t.nodes[size_t(i)].size = 10;
            t.nodes[size_t(i)].parent = 0;
        }
        t.fileCount = 2;
        t.totalBytes = 20; // average file far below 1 MiB
        WriterOptions o;
        auto plan = planImage(t, o);
        QVERIFY2(plan.ok(), qPrintable(plan.ok() ? QString() : plan.error().toLogString()));
        QCOMPARE(plan->geo.clusterBytes(), 65536);
        QVERIFY(plan->clusterChoice.contains(QStringLiteral("automatic")));
        // An explicit size is kept for experts; the log says what ShadowMount+ needs.
        o.clusterBytes = 32768;
        plan = planImage(t, o);
        QVERIFY(plan.ok());
        QCOMPARE(plan->geo.clusterBytes(), 32768);
        QVERIFY(plan->clusterChoice.contains(QStringLiteral("ShadowMount+")));
    }

    // The legacy script's rule (Get-OptimalImageSizeBytesFromStats, as it is
    // recorded in native/docs/LEGACY_INVENTORY.md): data rounded up to clusters,
    // 4 bytes of FAT and one bitmap bit per data cluster, 512 bytes per file and
    // folder, 64 MiB; a spare of 1 % kept between 1 GiB and 8 GiB; at least the
    // raw bytes plus 1 GiB; rounded up to 1 MiB. The sizes below were worked
    // out by hand from that rule (the script itself is not in this repository),
    // so they do not repeat the code's arithmetic.
    void legacySizeRuleGivesTheRecordedSizes()
    {
        const auto tree = [](const QList<qint64>& fileSizes, int folders) {
            SourceTree t;
            t.nodes.resize(size_t(1 + folders + fileSizes.size()));
            for (int i = 0; i <= folders; ++i)
                t.nodes[size_t(i)].isDir = true; // the root, then the folders
            for (qsizetype i = 0; i < fileSizes.size(); ++i) {
                t.nodes[size_t(1 + folders + i)].size = fileSizes[i];
                t.totalBytes += fileSizes[i];
            }
            t.fileCount = fileSizes.size();
            t.dirCount = folders;
            return t;
        };
        // 100 bytes, 64 KiB and 10 GiB + 1 in one folder, 64 KiB clusters:
        // data 10,737,614,848 (163,843 clusters), FAT 655,372, bitmap 20,481,
        // entries 2,048, base 10,805,401,613; 1 % is under 1 GiB, so the spare
        // is 1 GiB: 11,879,143,437, rounded up to 11,329 MiB.
        QCOMPARE(legacyImageBytes(tree({100, 65536, 10 * kGiB + 1}, 1), 65536), qint64(11879317504LL));
        // One 10-byte file, 32 KiB clusters: base 67,142,149 plus the 1 GiB
        // minimum spare, rounded up to 1,089 MiB.
        QCOMPARE(legacyImageBytes(tree({10}, 0), 32768), qint64(1141899264LL));
        // Five files of 200 GiB and one of 123,456,789 bytes in two folders:
        // base 1,073,999,998,556; 1 % is over 8 GiB, so the spare is 8 GiB:
        // 1,082,589,933,148, rounded up to 1,032,439 MiB.
        QCOMPARE(legacyImageBytes(tree({200 * kGiB, 200 * kGiB, 200 * kGiB, 200 * kGiB, 200 * kGiB, 123456789}, 2), 65536),
                 qint64(1082590756864LL));
    }

    void readsAVolumeFormattedByMkfsExfat()
    {
        QString mkfs = QStandardPaths::findExecutable(QStringLiteral("mkfs.exfat"));
        if (mkfs.isEmpty())
            mkfs = QStandardPaths::findExecutable(QStringLiteral("mkfs.exfat"), {QStringLiteral("/usr/sbin"), QStringLiteral("/sbin")});
        if (mkfs.isEmpty())
            QSKIP("mkfs.exfat (exfatprogs) not installed");
        const QString img = m_tmp.filePath(QStringLiteral("mkfs.img"));
        {
            QFile f(img);
            QVERIFY(f.open(QIODevice::WriteOnly));
            QVERIFY(f.resize(64 * kMiB));
        }
        QProcess p;
        p.start(mkfs, {QStringLiteral("-c"), QStringLiteral("32K"), QStringLiteral("-L"), QStringLiteral("MKFSVOL"), img});
        QVERIFY(p.waitForFinished(60000));
        QCOMPARE(p.exitCode(), 0);
        auto reader = ExfatReader::open(img);
        QVERIFY2(reader.ok(), qPrintable(reader.ok() ? QString() : reader.error().toLogString()));
        QCOMPARE(reader.value()->volume().label, QStringLiteral("MKFSVOL"));
        QVERIFY(reader.value()->volume().upcaseIsRecommended);
        QVERIFY(reader.value()->volume().backupBootMatches);
        QCOMPARE(reader.value()->geometry().clusterBytes(), 32768);
        CancellationToken token;
        auto tree = reader.value()->readTree(token);
        QVERIFY(tree.ok());
        QVERIFY2(tree->problems.isEmpty(), qPrintable(tree->problems.join(u'\n')));
        QCOMPARE(tree->entries.size(), size_t(1));
    }
};

QTEST_GUILESS_MAIN(TestExfatFormat)
#include "tst_exfat_format.moc"
