// Throughput of the exFAT engine: source scan, write, full verification,
// extraction. Logs MB/s; there is no pass/fail threshold (machines differ too
// much). FW_EXFAT_BENCH_MIB sets the amount of large-file data. The default
// of 64 keeps an ordinary test run light (the same code runs, on less data);
// for numbers worth quoting set 256 or more on a machine that does nothing
// else. FW_BENCH_MODE, FW_BENCH_THREADS and FW_BENCH_BUFFER_MIB run it with
// those Performance settings (engines/BenchSettings.h): the worker threads
// are the verification threads, the buffer size is the writer's, the
// verifier's and the extractor's, as in the app.

#include "ExfatTestUtil.h"

#include "engines/BenchSettings.h"
#include "engines/exfat/ExfatExtract.h"
#include "engines/exfat/ExfatVerify.h"
#include "support/SparseFile.h"
#include "support/TestSupport.h"

#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include <algorithm>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

using namespace fw;
using namespace fw::exfat;
using namespace exfattest;

class TestExfatBench : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_tmp;
    QString m_src, m_image;
    qint64 m_bytes = 0;
    benchsettings::Performance m_perf;

    WriterOptions writerOptions() const
    {
        WriterOptions o;
        o.label = QStringLiteral("BENCH");
        o.spare = SpareMode::Fixed;
        o.bufferBytes = m_perf.bufferBytes;
        return o;
    }

    // Starts every measurement with no dirty pages waiting for the disk, so
    // one step's write-back does not slow down the next one.
    static void settle()
    {
#ifdef Q_OS_UNIX
        ::sync();
#endif
    }

    static QString mbps(qint64 bytes, qint64 ms)
    {
        return ms > 0 ? QString::number(double(bytes) / (1024.0 * 1024.0) / (double(ms) / 1000.0), 'f', 0) + QStringLiteral(" MB/s")
                      : QStringLiteral("n/a");
    }

    // The image the verify and extract measurements read. writeThroughput()
    // leaves it behind; a measurement that is run on its own writes it first.
    bool ensureImage()
    {
        if (QFileInfo::exists(m_image))
            return true;
        const auto plan = build(m_src, m_image, writerOptions());
        if (!plan.ok())
            qWarning().noquote() << plan.error().toLogString();
        return plan.ok();
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_tmp.isValid());
        fwtest::isolateAppData(m_tmp);
        m_perf = benchsettings::fromEnvironment();
        QVERIFY2(m_perf.problem.isEmpty(), qPrintable(m_perf.problem));
        qInfo().noquote() << QStringLiteral("BENCH settings: %1").arg(m_perf.describe());
        m_src = m_tmp.filePath(QStringLiteral("bench-src"));
        const int asked = qEnvironmentVariableIntValue("FW_EXFAT_BENCH_MIB");
        const qint64 mib = asked > 0 ? asked : 64;
        const int big = 4;
        for (int i = 0; i < big; ++i)
            QVERIFY(writeRandom(m_src + QStringLiteral("/big/%1.bin").arg(i), mib * kMiB / big, quint32(i + 1)));
        QVERIFY(writeBytes(m_src + QStringLiteral("/eboot.bin"), "bench"));
        for (int i = 0; i < 2000; ++i)
            QVERIFY(writeRandom(m_src + QStringLiteral("/small/%1/%2.dat").arg(i % 20).arg(i), 4096 + i % 1000, quint32(100 + i)));
        CancellationToken token;
        auto tree = scanSource(m_src, {}, token);
        QVERIFY(tree.ok());
        m_bytes = tree->totalBytes;
        m_image = m_tmp.filePath(QStringLiteral("bench.exfat"));
        qInfo().noquote() << QStringLiteral("Data set: %1 files, %2 MiB").arg(tree->fileCount).arg(m_bytes / kMiB);
    }

    // The scan alone, repeated: one pass over 2,000 files is too short to
    // time on its own.
    void scanThroughput()
    {
        CancellationToken token;
        std::vector<qint64> us;
        qint64 files = 0;
        for (int i = 0; i < 9; ++i) {
            QElapsedTimer t;
            t.start();
            auto tree = scanSource(m_src, {}, token);
            us.push_back(t.nsecsElapsed() / 1000);
            QVERIFY(tree.ok());
            files = tree->fileCount + tree->dirCount;
        }
        std::sort(us.begin(), us.end());
        qInfo().noquote() << QStringLiteral("BENCH scan: %1 entries, median %2 ms (best %3 ms, worst %4 ms of %5)")
                                 .arg(files)
                                 .arg(double(us[us.size() / 2]) / 1000.0, 0, 'f', 1)
                                 .arg(double(us.front()) / 1000.0, 0, 'f', 1)
                                 .arg(double(us.back()) / 1000.0, 0, 'f', 1)
                                 .arg(us.size());
    }

    void writeThroughput()
    {
        CancellationToken token;
        QElapsedTimer t;
        t.start();
        auto tree = scanSource(m_src, {}, token);
        QVERIFY(tree.ok());
        const qint64 scanMs = t.elapsed();
        WriterOptions o = writerOptions();
        o.syncToDisk = true; // include the flush to the device
        auto plan = planImage(tree.value(), o);
        QVERIFY(plan.ok());
        settle();
        t.restart();
        auto ws = writeImage(tree.value(), plan.value(), m_image, o, token);
        QVERIFY2(ws.ok(), qPrintable(ws.ok() ? QString() : ws.error().toLogString()));
        const qint64 ms = t.elapsed();
        qInfo().noquote() << QStringLiteral("BENCH write: %1 MiB in %2 ms = %3 (scan %4 ms, %5 files)")
                                 .arg(m_bytes / kMiB).arg(ms).arg(mbps(m_bytes, ms)).arg(scanMs).arg(tree->fileCount);
    }

    void verifyThroughput()
    {
        QVERIFY(ensureImage());
        CancellationToken token;
        // One thread, then the worker threads of the Performance settings
        // (every core by default).
        std::vector<int> counts{1};
        const int many = m_perf.given ? m_perf.threads : std::max(2, m_perf.threads);
        if (many > 1)
            counts.push_back(many);
        for (int threads : counts) {
            VerifyOptions vo;
            vo.full = true;
            vo.threads = threads;
            vo.bufferBytes = m_perf.bufferBytes;
            QElapsedTimer t;
            t.start();
            auto r = verifyImageAgainstFolder(m_image, m_src, {}, vo, token);
            const qint64 ms = t.elapsed();
            QVERIFY(r.ok());
            QVERIFY2(r->passed(), qPrintable(r->summary()));
            qInfo().noquote() << QStringLiteral("BENCH verify (full, %1 threads): %2 ms = %3")
                                     .arg(threads).arg(ms).arg(mbps(r->bytesCompared, ms));
        }
        VerifyOptions vo;
        vo.full = false;
        vo.bufferBytes = m_perf.bufferBytes;
        QElapsedTimer t;
        t.start();
        auto r = verifyImageAgainstFolder(m_image, m_src, {}, vo, token);
        QVERIFY(r.ok() && r->passed());
        qInfo().noquote() << QStringLiteral("BENCH verify (structure): %1 ms").arg(t.elapsed());
    }

    // The app's default free-space rule (Legacy) leaves a large free tail.
    // The writer skips it; whether it takes room depends on the file being
    // sparse, which the writer does not ask for. Written once into a new
    // file and once into one made sparse beforehand (the attribute stays when
    // the writer truncates it), both flushed to the device.
    void freeTailOnDisk()
    {
        if (qEnvironmentVariableIntValue("FW_EXFAT_BENCH_MIB") <= 0)
            QSKIP("Set FW_EXFAT_BENCH_MIB: the default rule's free tail takes gigabytes on a file system without sparse files.");
        CancellationToken token;
        auto tree = scanSource(m_src, {}, token);
        QVERIFY(tree.ok());
        WriterOptions o = writerOptions();
        o.spare = SpareMode::Legacy;
        o.syncToDisk = true;
        auto plan = planImage(tree.value(), o);
        QVERIFY(plan.ok());
        for (bool sparse : {false, true}) {
            const QString image = m_tmp.filePath(sparse ? QStringLiteral("tail-sparse.exfat") : QStringLiteral("tail-plain.exfat"));
            if (sparse && fwtest::makeSparseFile(image, 0) < 0)
                QSKIP("This file system has no sparse files.");
            settle();
            QElapsedTimer t;
            t.start();
            auto ws = writeImage(tree.value(), plan.value(), image, o, token);
            const qint64 ms = t.elapsed();
            QVERIFY2(ws.ok(), qPrintable(ws.ok() ? QString() : ws.error().toLogString()));
            const qint64 size = QFileInfo(image).size();
            qInfo().noquote() << QStringLiteral("BENCH free tail (%1 file): image %2 MiB, on disk %3 MiB, written in %4 ms = %5")
                                     .arg(sparse ? QStringLiteral("sparse") : QStringLiteral("plain"))
                                     .arg(size / kMiB)
                                     .arg(fwtest::bytesOnDisk(image) / kMiB)
                                     .arg(ms)
                                     .arg(mbps(m_bytes, ms));
            QFile::remove(image);
        }
    }

    void extractThroughput()
    {
        QVERIFY(ensureImage());
        CancellationToken token;
        // Two rounds: on some VMs the first large batch of new page-cache
        // pages is much slower (memory faulted in lazily by the host), which
        // says nothing about the engine. Both rounds are logged.
        int round = 0;
        for (bool parallel : {false, true, false, true}) {
            ++round;
            auto reader = ExfatReader::open(m_image);
            QVERIFY(reader.ok());
            auto tree = reader.value()->readTree(token);
            QVERIFY(tree.ok());
            const QString dest = m_tmp.filePath(parallel ? QStringLiteral("x-par") : QStringLiteral("x-seq"));
            QVERIFY(QDir().mkpath(dest));
            ExtractOptions eo;
            eo.parallel = parallel;
            eo.threads = 16;
            eo.bufferBytes = m_perf.bufferBytes;
            settle();
            QElapsedTimer t;
            t.start();
            auto st = extractTree(*reader.value(), tree.value(), dest, eo, token);
            const qint64 ms = t.elapsed();
            QVERIFY(st.ok());
            QCOMPARE(st->bytes, m_bytes);
            qInfo().noquote() << QStringLiteral("BENCH extract (%1, round %4): %2 ms = %3")
                                     .arg(parallel ? QStringLiteral("parallel, 16 files") : QStringLiteral("sequential"))
                                     .arg(ms).arg(mbps(st->bytes, ms)).arg((round + 1) / 2);
            QDir(dest).removeRecursively();
        }
    }
};

QTEST_GUILESS_MAIN(TestExfatBench)
#include "tst_exfat_bench.moc"
