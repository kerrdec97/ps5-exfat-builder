// The PFS builder's pipeline with a worker pool: the input is read ahead on
// a thread of its own while the calling thread writes. Its failures must
// come out as they did on one thread: a source that ends early is an error,
// a cancel stops the build at once, and nothing is left running afterwards.
// (Byte-identical output for any thread count: tst_pfs_golden and
// tst_pfs_roundtrip.)

#include "engines/pfs/PfsBuilder.h"
#include "engines/pfs/PfsSource.h"

#include "engines/pfs/PfsTestUtil.h"
#include "support/TestSupport.h"

#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QTest>

#include <atomic>

using namespace fw;
using namespace fw::pfs;

namespace {

constexpr qint64 kTime = 1700000000;
constexpr qint64 kMiB = 1024 * 1024;

BuildOptions options(int threads, bool compressFiles = false)
{
    BuildOptions o;
    o.layout.time = kTime;
    o.threads = threads;
    o.level = 1;
    o.files.compress = compressFiles;
    return o;
}

// A game-like folder: a few files of several MiB (compressed per file in a
// folder image) and small ones.
bool makeGame(const QString& root)
{
    bool ok = pfstest::writeFile(root + QStringLiteral("/eboot.bin"), pfstest::genContent(QStringLiteral("random"), 300000, 1), kTime);
    ok = ok && pfstest::writeFile(root + QStringLiteral("/sce_sys/param.json"), pfstest::genContent(QStringLiteral("text"), 2000, 2), kTime);
    for (int i = 0; ok && i < 4; ++i)
        ok = pfstest::writeFile(root + QStringLiteral("/data/pack%1.bin").arg(i), pfstest::genContent(QStringLiteral("mixed"), 7 * kMiB + i, 10 + i), kTime);
    for (int i = 0; ok && i < 30; ++i)
        ok = pfstest::writeFile(root + QStringLiteral("/data/small/%1.txt").arg(i), pfstest::genContent(QStringLiteral("text"), 3000 + i, 50 + i), kTime);
    return ok;
}

Result<SourcePlan> planOf(const QString& root)
{
    PlanOptions po;
    po.time = kTime;
    return planFolder(root, po, CancellationToken());
}

} // namespace

class TestPfsPipeline : public QObject {
    Q_OBJECT

    QTemporaryDir m_tmp;

private slots:
    void initTestCase()
    {
        QVERIFY(m_tmp.isValid());
        fwtest::isolateAppData(m_tmp);
    }

    void aSourceThatEndsEarlyIsAnError_data()
    {
        QTest::addColumn<int>("threads");
        QTest::addColumn<bool>("container");
        QTest::newRow("folder image, 1 thread") << 1 << false;
        QTest::newRow("folder image, 4 threads") << 4 << false;
        QTest::newRow("container, 1 thread") << 1 << true;
        QTest::newRow("container, 4 threads") << 4 << true;
    }

    // A file cut short after it was planned (another program truncated it):
    // the build stops with the reader's error, whichever thread read it.
    void aSourceThatEndsEarlyIsAnError()
    {
        QFETCH(int, threads);
        QFETCH(bool, container);
        const QString tag = QStringLiteral("%1-%2").arg(threads).arg(container);
        const QString src = m_tmp.filePath(QStringLiteral("short-") + tag);
        QVERIFY(makeGame(src));
        BuildInput in;
        auto plan = planOf(src);
        QVERIFY(plan.ok());
        in.plan = std::move(plan).value();
        {
            QFile f(src + QStringLiteral("/data/pack2.bin"));
            QVERIFY(f.open(QIODevice::ReadWrite));
            QVERIFY(f.resize(3 * kMiB));
        }
        const QString out = m_tmp.filePath(QStringLiteral("short-%1.img").arg(tag));
        QElapsedTimer t;
        t.start();
        auto st = writeImage(in, container ? OutputFormat::Container : OutputFormat::FolderImage, options(threads, true), out,
                             CancellationToken());
        QVERIFY2(!st.ok(), "a source that ended early was packed");
        QCOMPARE(st.error().code, ErrorCode::IoError);
        QVERIFY2(t.elapsed() < 60000, "the build did not stop");
    }

    void cancellingStopsTheBuild_data()
    {
        QTest::addColumn<int>("threads");
        QTest::addColumn<bool>("container");
        QTest::newRow("folder image, 4 threads") << 4 << false;
        QTest::newRow("container, 4 threads") << 4 << true;
        QTest::newRow("container, 1 thread") << 1 << true;
    }

    // Cancel from the progress callback (called by the reader thread for a
    // container, by the writing thread otherwise) after the first MiB.
    void cancellingStopsTheBuild()
    {
        QFETCH(int, threads);
        QFETCH(bool, container);
        const QString tag = QStringLiteral("%1-%2").arg(threads).arg(container);
        const QString src = m_tmp.filePath(QStringLiteral("cancel-") + tag);
        QVERIFY(makeGame(src));
        BuildInput in;
        auto plan = planOf(src);
        QVERIFY(plan.ok());
        in.plan = std::move(plan).value();
        CancellationSource cs;
        std::atomic<qint64> seen{0};
        BuildHooks hooks;
        hooks.advance = [&](qint64 n) {
            if (seen.fetch_add(n) + n > kMiB)
                cs.cancel();
        };
        const QString out = m_tmp.filePath(QStringLiteral("cancel-%1.img").arg(tag));
        auto st = writeImage(in, container ? OutputFormat::Container : OutputFormat::FolderImage, options(threads, true), out,
                             cs.token(), hooks);
        QVERIFY2(!st.ok(), "the cancelled build finished");
        QVERIFY2(st.error().isCancelled(), qPrintable(st.error().toLogString()));
        QVERIFY(seen.load() < in.plan.totalBytes);
        // Nothing still reads or writes the image: it can be replaced at once.
        QVERIFY(QFile::remove(out));
    }

    // The same input built again and again with a pool: the image does not
    // change (the reader thread runs ahead by a different amount each time).
    void repeatedBuildsAreIdentical()
    {
        const QString src = m_tmp.filePath(QStringLiteral("repeat"));
        QVERIFY(makeGame(src));
        BuildInput in;
        auto plan = planOf(src);
        QVERIFY(plan.ok());
        in.plan = std::move(plan).value();
        for (int threads : {1, 2, 3, 8, 16}) {
            for (bool container : {false, true}) {
                const QString out = m_tmp.filePath(QStringLiteral("repeat-%1-%2.img").arg(threads).arg(container));
                auto st = writeImage(in, container ? OutputFormat::Container : OutputFormat::FolderImage, options(threads, true), out,
                                     CancellationToken());
                QVERIFY2(st.ok(), st.ok() ? "" : qPrintable(st.error().toLogString()));
                QVERIFY(st->compressedFiles > 0 || container);
                const QString sha = pfstest::sha256(out);
                QVERIFY(!sha.isEmpty());
                const QString key = container ? QStringLiteral("c") : QStringLiteral("f");
                if (threads == 1)
                    m_reference.insert(key, sha);
                else
                    QCOMPARE(sha, m_reference.value(key));
                QFile::remove(out);
            }
        }
    }

private:
    QHash<QString, QString> m_reference;
};

QTEST_GUILESS_MAIN(TestPfsPipeline)
#include "tst_pfs_pipeline.moc"
