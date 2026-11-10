// Throughput of the exFAT engine: write, full verification, extraction.
// Logs MB/s; there is no pass/fail threshold (machines differ too much).
// FW_EXFAT_BENCH_MIB sets the amount of large-file data. The default of 64
// keeps an ordinary test run light (the same code runs, on less data); for
// numbers worth quoting set 256 or more on a machine that does nothing else.

#include "ExfatTestUtil.h"

#include "engines/exfat/ExfatExtract.h"
#include "engines/exfat/ExfatVerify.h"
#include "support/TestSupport.h"

#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

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
        WriterOptions o;
        o.label = QStringLiteral("BENCH");
        o.spare = SpareMode::Fixed;
        const auto plan = build(m_src, m_image, o);
        if (!plan.ok())
            qWarning().noquote() << plan.error().toLogString();
        return plan.ok();
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_tmp.isValid());
        fwtest::isolateAppData(m_tmp);
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

    void writeThroughput()
    {
        CancellationToken token;
        QElapsedTimer t;
        t.start();
        auto tree = scanSource(m_src, {}, token);
        QVERIFY(tree.ok());
        const qint64 scanMs = t.elapsed();
        WriterOptions o;
        o.label = QStringLiteral("BENCH");
        o.spare = SpareMode::Fixed;
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
        for (int threads : {1, std::max(2, QThread::idealThreadCount())}) {
            VerifyOptions vo;
            vo.full = true;
            vo.threads = threads;
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
        QElapsedTimer t;
        t.start();
        auto r = verifyImageAgainstFolder(m_image, m_src, {}, vo, token);
        QVERIFY(r.ok() && r->passed());
        qInfo().noquote() << QStringLiteral("BENCH verify (structure): %1 ms").arg(t.elapsed());
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
