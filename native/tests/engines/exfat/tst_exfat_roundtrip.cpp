// exFAT writer + reader: synthetic trees are written, checked with
// fsck.exfat, read back and compared byte for byte; damaged images are
// detected; verification is strict; extraction round-trips.

#include "ExfatTestUtil.h"

#include "engines/exfat/ExfatExtract.h"
#include "engines/exfat/ExfatVerify.h"
#include "engines/exfat/PlatformFs.h"
#include "support/TestSupport.h"

#include <QElapsedTimer>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QTest>
#include <QTimeZone>

#ifdef Q_OS_UNIX
#include <sys/stat.h>
#endif

using namespace fw;
using namespace fw::exfat;
using namespace exfattest;

class TestExfatRoundtrip : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_tmp;
    QString m_sample;      // sample tree
    QString m_sampleImage; // its image
    QString path(const QString& rel) const { return m_tmp.filePath(rel); }

    static QString err(const Error& e) { return e.toLogString(); }

    void expectFsckClean(const QString& image)
    {
        const Fsck f = runFsck(image);
        if (!f.available)
            return;
        qInfo().noquote() << "fsck.exfat -n:" << f.output.trimmed().section(u'\n', -1);
        QVERIFY2(f.exitCode == 0, qPrintable(f.output));
        QVERIFY2(!f.output.contains(QStringLiteral("ERROR"), Qt::CaseInsensitive), qPrintable(f.output));
        QVERIFY2(f.output.contains(QStringLiteral("clean")), qPrintable(f.output));
    }

    // Reads every file of the image and compares with `src`.
    void compareImageWithFolder(const QString& image, const QString& src)
    {
        CancellationToken token;
        auto reader = ExfatReader::open(image);
        QVERIFY2(reader.ok(), qPrintable(reader.ok() ? QString() : err(reader.error())));
        auto tree = reader.value()->readTree(token);
        QVERIFY(tree.ok());
        QVERIFY2(tree->problems.isEmpty(), qPrintable(tree->problems.join(u'\n')));
        QVERIFY2(tree->warnings.isEmpty(), qPrintable(tree->warnings.join(u'\n')));
        const auto expected = listing(src);
        QCOMPARE(qsizetype(tree->entries.size() - 1), expected.size());
        ImageFileStream stream(image, reader.value()->geometry());
        QVERIFY(stream.open().ok());
        for (auto it = expected.cbegin(); it != expected.cend(); ++it) {
            const int idx = tree->find(it.key());
            QVERIFY2(idx > 0, qPrintable(it.key()));
            const ImageEntry& e = tree->entries[size_t(idx)];
            QCOMPARE(e.isDir, it.value() < 0);
            if (e.isDir)
                continue;
            QCOMPARE(e.size, it.value());
            QByteArray got(qsizetype(e.size), '\0');
            auto n = stream.read(e, 0, got.data(), e.size);
            QVERIFY(n.ok());
            QCOMPARE(n.value(), e.size);
            QVERIFY2(got == readAllOf(src + u'/' + it.key()), qPrintable(it.key()));
        }
    }

    VerifyReport verifyFolder(const QString& image, const QString& folder, bool full, ScanRules rules = {})
    {
        CancellationToken token;
        VerifyOptions vo;
        vo.full = full;
        vo.threads = 4;
        auto r = verifyImageAgainstFolder(image, folder, rules, vo, token);
        if (!r.ok()) {
            qWarning() << err(r.error());
            return {};
        }
        return r.value();
    }

    static const Mismatch* findMismatch(const VerifyReport& r, const QString& path, const QString& kind)
    {
        for (const Mismatch& m : r.mismatches)
            if (m.path == path && m.kind == kind)
                return &m;
        return nullptr;
    }

    QString copyOf(const QString& image, const QString& name)
    {
        const QString p = path(name);
        QFile::remove(p);
        if (!QFile::copy(image, p))
            return {};
        QFile(p).setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        return p;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_tmp.isValid());
        fwtest::isolateAppData(m_tmp);
        m_sample = path(QStringLiteral("sample"));
        makeSampleTree(m_sample, 1500);
        m_sampleImage = path(QStringLiteral("sample.exfat"));
        WriterOptions o;
        o.spare = SpareMode::Fixed;
        o.spareBytes = 8 * kMiB;
        auto plan = build(m_sample, m_sampleImage, o);
        QVERIFY2(plan.ok(), qPrintable(plan.ok() ? QString() : err(plan.error())));
    }

    void sampleTreeRoundTripsAndPassesFsck()
    {
        const QString src = path(QStringLiteral("legacy-size"));
        makeSampleTree(src);
        const QString image = path(QStringLiteral("legacy-size.exfat"));
        auto plan = build(src, image); // default: legacy automatic size, automatic cluster
        QVERIFY2(plan.ok(), qPrintable(plan.ok() ? QString() : err(plan.error())));
        QCOMPARE(QFileInfo(image).size(), plan->geo.volumeBytes());
        QVERIFY(plan->geo.volumeBytes() >= kGiB); // legacy 1 GiB minimum slack
        QCOMPARE(plan->geo.clusterBytes(), 65536); // automatic = 64 KiB (ShadowMount+ 1.7), even for small files
        expectFsckClean(image);
        compareImageWithFolder(image, src);
        // Manual cross-checks with other exFAT drivers: keep the sample.
        if (const QByteArray keep = qgetenv("FW_EXFAT_KEEP_SAMPLE"); !keep.isEmpty()) {
            const QString dir = QString::fromLocal8Bit(keep);
            QDir(dir).removeRecursively();
            QDir().mkpath(dir);
            QVERIFY(QFile::copy(image, dir + QStringLiteral("/sample.exfat")));
            QVERIFY(QDir().rename(src, dir + QStringLiteral("/sample")));
        }
        if (fsckPath().isEmpty())
            QSKIP("fsck.exfat not installed; the structure was checked by the reader only");
    }

    void manySmallFilesAndCompactSize()
    {
        expectFsckClean(m_sampleImage);
        compareImageWithFolder(m_sampleImage, m_sample);
        CancellationToken token;
        auto reader = ExfatReader::open(m_sampleImage);
        QVERIFY(reader.ok());
        QCOMPARE(reader.value()->volume().label, QStringLiteral("OSFIMG"));
        QVERIFY(reader.value()->volume().backupBootMatches);
        QCOMPARE(reader.value()->volume().boot.partitionOffset, 0); // bare volume, no MBR
        auto tree = reader.value()->readTree(token);
        QVERIFY(tree.ok());
        // Everything contiguous: every entry with data is a single NoFatChain run.
        for (const ImageEntry& e : tree->entries) {
            if (&e == &tree->entries.front() || e.size == 0)
                continue;
            QVERIFY2(e.noFatChain, qPrintable(e.path));
            QCOMPARE(e.extents.size(), size_t(1));
        }
        // Fixed 8 MiB of spare: the image is small.
        const Geometry& g = reader.value()->geometry();
        const qint64 freeBytes = (qint64(g.clusterCount) - tree->usedClusters) * g.clusterBytes();
        QVERIFY2(freeBytes >= 8 * kMiB && freeBytes < 8 * kMiB + 4 * g.clusterBytes() + 2 * kMiB,
                 qPrintable(QString::number(freeBytes)));
    }

    void clusterSectorAndAllocationOptions_data()
    {
        QTest::addColumn<int>("sector");
        QTest::addColumn<int>("cluster");
        QTest::addColumn<bool>("fatChains");
        QTest::newRow("512/4K") << 512 << 4096 << false;
        QTest::newRow("512/16K") << 512 << 16384 << false;
        QTest::newRow("512/64K fat chains") << 512 << 65536 << true;
        QTest::newRow("512/1M") << 512 << (1 << 20) << false;
        QTest::newRow("4096/32K") << 4096 << 32768 << false;
        QTest::newRow("4096/256K fat chains") << 4096 << 262144 << true;
    }
    void clusterSectorAndAllocationOptions()
    {
        QFETCH(int, sector);
        QFETCH(int, cluster);
        QFETCH(bool, fatChains);
        const QString image = path(QStringLiteral("opt-%1-%2-%3.exfat").arg(sector).arg(cluster).arg(fatChains));
        WriterOptions o;
        o.sectorBytes = sector;
        o.clusterBytes = cluster;
        o.fatChains = fatChains;
        o.spare = SpareMode::Fixed;
        o.spareBytes = 2 * kMiB;
        o.label = QString::fromUtf8("Ünï 12345");
        auto plan = build(m_sample, image, o);
        QVERIFY2(plan.ok(), qPrintable(plan.ok() ? QString() : err(plan.error())));
        expectFsckClean(image);
        compareImageWithFolder(image, m_sample);
        auto reader = ExfatReader::open(image);
        QVERIFY(reader.ok());
        QCOMPARE(int(reader.value()->geometry().sectorBytes()), sector);
        QCOMPARE(int(reader.value()->geometry().clusterBytes()), cluster);
        QCOMPARE(reader.value()->volume().label, o.label);
        CancellationToken token;
        auto tree = reader.value()->readTree(token);
        QVERIFY(tree.ok());
        for (const ImageEntry& e : tree->entries)
            if (e.size > 0 && &e != &tree->entries.front())
                QCOMPARE(e.noFatChain, !fatChains);
    }

    void emptyFolderAndNoLabel()
    {
        const QString src = path(QStringLiteral("empty-src"));
        QDir().mkpath(src);
        const QString image = path(QStringLiteral("empty.exfat"));
        WriterOptions o;
        o.label = QString();
        CancellationToken token;
        auto tree = scanSource(src, {}, token);
        QVERIFY(tree.ok());
        o.spare = SpareMode::Fixed;
        o.syncToDisk = false;
        auto plan = planImage(tree.value(), o);
        QVERIFY(plan.ok());
        QVERIFY(writeImage(tree.value(), plan.value(), image, o, token).ok());
        expectFsckClean(image);
        auto reader = ExfatReader::open(image);
        QVERIFY(reader.ok());
        QCOMPARE(reader.value()->volume().label, QString());
        auto t = reader.value()->readTree(token);
        QVERIFY(t.ok());
        QCOMPARE(t->entries.size(), size_t(1));
        QVERIFY(t->problems.isEmpty());
    }

    void namesThatExfatCannotHoldAreRefused()
    {
#ifdef Q_OS_WIN
        QSKIP("Windows itself cannot create these names");
#else
        const QString src = path(QStringLiteral("bad-names"));
        writeBytes(src + QStringLiteral("/eboot.bin"), "x");
        writeBytes(src + QStringLiteral("/Data.bin"), "1");
        writeBytes(src + QStringLiteral("/DATA.BIN"), "2");
        auto r = build(src, path(QStringLiteral("bad-names.exfat")));
        QVERIFY(!r.ok());
        QCOMPARE(r.error().code, ErrorCode::InvalidArgument);
        QVERIFY2(r.error().message.contains(QStringLiteral("differ only in upper/lower case")), qPrintable(r.error().message));

        const QString src2 = path(QStringLiteral("bad-names2"));
        writeBytes(src2 + QStringLiteral("/what?.txt"), "1");
        auto r2 = build(src2, path(QStringLiteral("bad-names2.exfat")));
        QVERIFY(!r2.ok());
        QVERIFY2(r2.error().message.contains(QStringLiteral("what?.txt")), qPrintable(r2.error().message));
#endif
    }

    void exclusionsAreAppliedAndVerifiedConsistently()
    {
        const QString src = path(QStringLiteral("excl"));
        makeSampleTree(src, 5);
        writeBytes(src + QStringLiteral("/Thumbs.db"), "junk");
        writeBytes(src + QStringLiteral("/data/desktop.ini"), "junk");
        writeBytes(src + QStringLiteral("/old build.exfat"), "nested image");
        writeBytes(src + QStringLiteral("/decrypted/x.elf"), "decrypted");
        writeBytes(src + QStringLiteral("/.hidden"), "hidden");
        ScanRules rules;
        rules.excludePatterns = {QStringLiteral("*.exfat"), QStringLiteral("thumbs.db"), QStringLiteral("desktop.ini"),
                                 QStringLiteral("decrypted/")};
        rules.excludeHidden = true;
        const QString image = path(QStringLiteral("excl.exfat"));
        WriterOptions o;
        o.spare = SpareMode::Fixed;
        auto plan = build(src, image, o, rules);
        QVERIFY(plan.ok());
        auto reader = ExfatReader::open(image);
        QVERIFY(reader.ok());
        CancellationToken token;
        auto tree = reader.value()->readTree(token);
        QVERIFY(tree.ok());
        for (const char* p : {"Thumbs.db", "data/desktop.ini", "old build.exfat", "decrypted", "decrypted/x.elf"})
            QVERIFY2(tree->find(QString::fromLatin1(p)) < 0, p);
#ifndef Q_OS_WIN
        QVERIFY(tree->find(QStringLiteral(".hidden")) < 0);
#endif
        QVERIFY(tree->find(QStringLiteral("data/big.bin")) > 0);
        // Verification with the same rules passes; without them it fails strictly.
        QVERIFY(verifyFolder(image, src, true, rules).passed());
        const VerifyReport strict = verifyFolder(image, src, false);
        QVERIFY(!strict.passed());
        QVERIFY(findMismatch(strict, QStringLiteral("Thumbs.db"), QStringLiteral("missing")));
    }

    void corruptBootSectorIsDetected()
    {
        const QString img = copyOf(m_sampleImage, QStringLiteral("bad-boot.exfat"));
        flipByte(img, 100); // VolumeSerialNumber: checksum no longer matches
        auto r = ExfatReader::open(img);
        QVERIFY(!r.ok());
        QCOMPARE(r.error().code, ErrorCode::CorruptData);
        QVERIFY2(r.error().message.contains(QStringLiteral("main boot sector")), qPrintable(r.error().message));
        QVERIFY(r.error().message.contains(QStringLiteral("backup copy is intact")));
        const VerifyReport v = verifyFolder(img, m_sample, false);
        QVERIFY(!v.passed());
        QVERIFY(!v.structureProblems.isEmpty());
    }

    void corruptBackupBootRegionFailsVerification()
    {
        const QString img = copyOf(m_sampleImage, QStringLiteral("bad-backup.exfat"));
        flipByte(img, 12 * 512 + 100);
        auto r = ExfatReader::open(img);
        QVERIFY(r.ok());
        QVERIFY(!r.value()->volume().backupBootMatches);
        const VerifyReport v = verifyFolder(img, m_sample, false);
        QVERIFY(!v.passed());
        QVERIFY2(v.structureProblems.join(u' ').contains(QStringLiteral("backup boot")), qPrintable(v.summary()));
    }

    void corruptDirectoryEntryIsDetected()
    {
        CancellationToken token;
        auto reader = ExfatReader::open(m_sampleImage);
        QVERIFY(reader.ok());
        auto tree = reader.value()->readTree(token);
        QVERIFY(tree.ok());
        const int idx = tree->find(QStringLiteral("data/big.bin"));
        QVERIFY(idx > 0);
        const qint64 entryOffset = tree->entries[size_t(idx)].entryOffset;
        reader.value().reset();

        // A changed name character: entry-set checksum mismatch.
        const QString img = copyOf(m_sampleImage, QStringLiteral("bad-dir.exfat"));
        flipByte(img, entryOffset + 2 * 32 + 2, 0x01);
        auto r = ExfatReader::open(img);
        QVERIFY(r.ok()); // the volume structures are fine
        auto t = r.value()->readTree(token);
        QVERIFY(t.ok());
        QVERIFY(!t->problems.isEmpty());
        const Status intact = ExfatReader::requireIntact(t.value());
        QVERIFY(!intact.ok());
        QCOMPARE(intact.error().code, ErrorCode::CorruptData);
        QVERIFY2(intact.error().message.contains(QStringLiteral("damaged")), qPrintable(intact.error().message));
        QVERIFY(!verifyFolder(img, m_sample, false).passed());

        // A first cluster pointing outside the volume.
        const QString img2 = copyOf(m_sampleImage, QStringLiteral("bad-cluster.exfat"));
        flipByte(img2, entryOffset + 32 + 23, 0x7F);
        auto r2 = ExfatReader::open(img2);
        QVERIFY(r2.ok());
        auto t2 = r2.value()->readTree(token);
        QVERIFY(t2.ok());
        QVERIFY(!t2->problems.isEmpty());
        const Fsck f = runFsck(img2);
        if (f.available)
            QVERIFY2(f.exitCode != 0, qPrintable(f.output)); // fsck agrees it is damaged
    }

    void corruptBitmapUpcaseAndTruncationAreDetected()
    {
        auto reader = ExfatReader::open(m_sampleImage);
        QVERIFY(reader.ok());
        const Geometry g = reader.value()->geometry();
        const Extent bitmap = reader.value()->volume().bitmapExtents.front();
        const Extent upcase = reader.value()->volume().upcaseExtents.front();
        reader.value().reset();
        CancellationToken token;

        const QString img = copyOf(m_sampleImage, QStringLiteral("bad-bitmap.exfat"));
        flipByte(img, g.clusterOffset(bitmap.cluster), 0x01); // cluster 2 (the bitmap itself) marked free
        auto r = ExfatReader::open(img);
        QVERIFY(r.ok());
        auto t = r.value()->readTree(token);
        QVERIFY(t.ok());
        QVERIFY2(t->problems.join(u' ').contains(QStringLiteral("marked free")), qPrintable(t->problems.join(u' ')));

        const QString img2 = copyOf(m_sampleImage, QStringLiteral("bad-upcase.exfat"));
        flipByte(img2, g.clusterOffset(upcase.cluster) + 200);
        auto r2 = ExfatReader::open(img2);
        QVERIFY(!r2.ok());
        QCOMPARE(r2.error().code, ErrorCode::CorruptData);
        QVERIFY(r2.error().message.contains(QStringLiteral("up-case")));

        const QString img3 = copyOf(m_sampleImage, QStringLiteral("truncated.exfat"));
        QVERIFY(QFile(img3).resize(QFileInfo(img3).size() / 2));
        auto r3 = ExfatReader::open(img3);
        QVERIFY(!r3.ok());
        QCOMPARE(r3.error().code, ErrorCode::CorruptData);
        QVERIFY(r3.error().message.contains(QStringLiteral("shorter")));

        const QString notExfat = path(QStringLiteral("not-exfat.bin"));
        QVERIFY(writeRandom(notExfat, 1 << 20, 9));
        auto r4 = ExfatReader::open(notExfat);
        QVERIFY(!r4.ok());
        QCOMPARE(r4.error().code, ErrorCode::CorruptData);
    }

    void verifyIsStrict()
    {
        const QString src = path(QStringLiteral("verify-src"));
        makeSampleTree(src, 20);
        const QString image = path(QStringLiteral("verify.exfat"));
        WriterOptions o;
        o.spare = SpareMode::Fixed;
        QVERIFY(build(src, image, o).ok());
        {
            const VerifyReport full = verifyFolder(image, src, true);
            QVERIFY2(full.passed(), qPrintable(full.summary()));
            QVERIFY(full.bytesCompared > 3 * 1024 * 1024);
            QVERIFY(full.summary().contains(QStringLiteral("byte for byte")));
            QVERIFY(verifyFolder(image, src, false).passed());
        }
        // One changed byte (same size): full fails, structure cannot see it.
        flipByte(src + QStringLiteral("/data/big.bin"), 2 * 1024 * 1024 + 7, 0x10);
        {
            const VerifyReport full = verifyFolder(image, src, true);
            QVERIFY(!full.passed());
            const Mismatch* m = findMismatch(full, QStringLiteral("data/big.bin"), QStringLiteral("content"));
            QVERIFY(m);
            QVERIFY2(m->detail.contains(QStringLiteral("2,097,159")), qPrintable(m->detail));
            QCOMPARE(full.mismatches.size(), size_t(1));
            QVERIFY(verifyFolder(image, src, false).passed());
        }
        flipByte(src + QStringLiteral("/data/big.bin"), 2 * 1024 * 1024 + 7, 0x10);
        // A file only in the source: missing from the image.
        writeBytes(src + QStringLiteral("/data/new.bin"), "new");
        {
            const VerifyReport r = verifyFolder(image, src, false);
            QVERIFY(!r.passed());
            QVERIFY(findMismatch(r, QStringLiteral("data/new.bin"), QStringLiteral("missing")));
        }
        QFile::remove(src + QStringLiteral("/data/new.bin"));
        // A file only in the image: extra.
        QVERIFY(QFile::remove(src + QStringLiteral("/data/medium.bin")));
        QVERIFY(QDir(src).rmdir(QStringLiteral("empty folder")));
        {
            const VerifyReport r = verifyFolder(image, src, true);
            QVERIFY(!r.passed());
            QVERIFY(findMismatch(r, QStringLiteral("data/medium.bin"), QStringLiteral("extra")));
            QVERIFY(findMismatch(r, QStringLiteral("empty folder"), QStringLiteral("extra")));
            QVERIFY(r.summary().contains(QStringLiteral("2 problems")));
            const QJsonObject j = r.toJson();
            QCOMPARE(j.value(QStringLiteral("mismatchCount")).toInt(), 2);
            QCOMPARE(j.value(QStringLiteral("mismatches")).toArray().size(), 2);
        }
        // A different size.
        writeRandom(src + QStringLiteral("/data/medium.bin"), 700 * 1024 + 1, 3);
        QDir().mkpath(src + QStringLiteral("/empty folder"));
        {
            const VerifyReport r = verifyFolder(image, src, false);
            QVERIFY(!r.passed());
            QVERIFY(findMismatch(r, QStringLiteral("data/medium.bin"), QStringLiteral("size")));
        }
    }

    void extractRoundTripsWithTimestamps_data()
    {
        QTest::addColumn<bool>("parallel");
        QTest::newRow("sequential") << false;
        QTest::newRow("parallel") << true;
    }
    void extractRoundTripsWithTimestamps()
    {
        QFETCH(bool, parallel);
        const QString src = path(QStringLiteral("times-src-%1").arg(parallel));
        makeSampleTree(src, 200);
        // Known times on a file and a folder.
        const QDateTime fileTime(QDate(2021, 5, 6), QTime(7, 8, 9, 123), QTimeZone::utc());
        const QDateTime dirTime(QDate(2019, 12, 24), QTime(18, 0, 1, 990), QTimeZone::utc());
        {
            QFile f(src + QStringLiteral("/data/big.bin"));
            QVERIFY(f.open(QIODevice::ReadWrite));
            QVERIFY(f.setFileTime(fileTime, QFileDevice::FileModificationTime));
        }
        QVERIFY(setPathTimesForTest(src + QStringLiteral("/a/b"), dirTime));
        const QString image = path(QStringLiteral("times-%1.exfat").arg(parallel));
        WriterOptions o;
        o.spare = SpareMode::Fixed;
        QVERIFY(build(src, image, o).ok());

        auto reader = ExfatReader::open(image);
        QVERIFY(reader.ok());
        CancellationToken token;
        auto tree = reader.value()->readTree(token);
        QVERIFY(tree.ok());
        const ImageEntry& big = tree->entries[size_t(tree->find(QStringLiteral("data/big.bin")))];
        QCOMPARE(big.modified.toMSecsSinceEpoch(), fileTime.toMSecsSinceEpoch() - 3); // 10 ms resolution
        const QString dest = path(QStringLiteral("extracted-%1").arg(parallel));
        QDir().mkpath(dest);
        ExtractOptions eo;
        eo.parallel = parallel;
        eo.threads = 8;
        auto st = extractTree(*reader.value(), tree.value(), dest, eo, token);
        QVERIFY2(st.ok(), qPrintable(st.ok() ? QString() : err(st.error())));
        QCOMPARE(st->files, tree->fileCount);
        QVERIFY2(st->warnings.isEmpty(), qPrintable(st->warnings.join(u' ')));

        const auto srcList = listing(src);
        QCOMPARE(listing(dest), srcList);
        for (auto it = srcList.cbegin(); it != srcList.cend(); ++it)
            if (it.value() >= 0)
                QVERIFY2(readAllOf(src + u'/' + it.key()) == readAllOf(dest + u'/' + it.key()), qPrintable(it.key()));
        QCOMPARE(QFileInfo(dest + QStringLiteral("/data/big.bin")).lastModified().toMSecsSinceEpoch(),
                 fileTime.toMSecsSinceEpoch() - 3);
        QCOMPARE(QFileInfo(dest + QStringLiteral("/a/b")).lastModified().toMSecsSinceEpoch(),
                 dirTime.toMSecsSinceEpoch());
        // Every extracted modification time is within 10 ms of the source.
        for (auto it = srcList.cbegin(); it != srcList.cend(); ++it) {
            const qint64 a = QFileInfo(src + u'/' + it.key()).lastModified().toMSecsSinceEpoch();
            const qint64 b = QFileInfo(dest + u'/' + it.key()).lastModified().toMSecsSinceEpoch();
            QVERIFY2(a - b >= 0 && a - b < 10, qPrintable(it.key() + QStringLiteral(" %1 vs %2").arg(a).arg(b)));
        }
        // The extracted folder verifies against the image.
        QVERIFY(verifyFolder(image, dest, true).passed());
    }

    void sparseFileLargerThan4GiB()
    {
#ifndef Q_OS_UNIX
        QSKIP("sparse files are only created quickly on this platform's test file systems under Unix");
#else
        const QString src = path(QStringLiteral("huge-src"));
        QDir().mkpath(src);
        const qint64 size = 5 * kGiB + 12345;
        {
            QFile f(src + QStringLiteral("/huge.bin"));
            QVERIFY(f.open(QIODevice::WriteOnly));
            QVERIFY(f.resize(size));
            QVERIFY(f.seek(size - 5));
            QCOMPARE(f.write("tail!", 5), qint64(5));
            QVERIFY(f.seek(4 * kGiB + 1));
            QCOMPARE(f.write("past 4 GiB", 10), qint64(10));
        }
        struct stat sb;
        QVERIFY(::stat(QFile::encodeName(src + QStringLiteral("/huge.bin")).constData(), &sb) == 0);
        if (qint64(sb.st_blocks) * 512 > 64 * kMiB)
            QSKIP("this file system does not create sparse files");
        const QString image = path(QStringLiteral("huge.exfat"));
        WriterOptions o;
        o.spare = SpareMode::Fixed;
        QElapsedTimer t;
        t.start();
        auto plan = build(src, image, o);
        QVERIFY2(plan.ok(), qPrintable(plan.ok() ? QString() : err(plan.error())));
        QVERIFY(QFileInfo(image).size() > size);
        struct stat ib;
        QVERIFY(::stat(QFile::encodeName(image).constData(), &ib) == 0);
        qInfo() << "5 GiB sparse file imaged in" << t.elapsed() << "ms; image uses" << qint64(ib.st_blocks) * 512 / kMiB << "MiB on disk";
        expectFsckClean(image);

        auto reader = ExfatReader::open(image);
        QVERIFY(reader.ok());
        CancellationToken token;
        auto tree = reader.value()->readTree(token);
        QVERIFY(tree.ok());
        const ImageEntry& e = tree->entries[size_t(tree->find(QStringLiteral("huge.bin")))];
        QCOMPARE(e.size, size);
        ImageFileStream s(image, reader.value()->geometry());
        QVERIFY(s.open().ok());
        char buf[16] = {};
        auto r = s.read(e, size - 5, buf, 16);
        QVERIFY(r.ok());
        QCOMPARE(r.value(), qint64(5));
        QCOMPARE(QByteArray(buf, 5), QByteArray("tail!"));
        r = s.read(e, 4 * kGiB + 1, buf, 10);
        QCOMPARE(QByteArray(buf, 10), QByteArray("past 4 GiB"));
        VerifyOptions vo;
        vo.full = true;
        auto v = verifyImageAgainstFolder(image, src, {}, vo, token);
        QVERIFY(v.ok());
        QVERIFY2(v->passed(), qPrintable(v->summary()));
        QCOMPARE(v->bytesCompared, size);
#endif
    }

private:
    static bool setPathTimesForTest(const QString& p, const QDateTime& t) { return setPathTimes(p, t, t, t); }
};

QTEST_GUILESS_MAIN(TestExfatRoundtrip)
#include "tst_exfat_roundtrip.moc"
