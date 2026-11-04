// Core infrastructure: Result, cancellation, progress, logging, formatting,
// file utilities, process runner.

#include "core/Cancellation.h"
#include "core/FileUtil.h"
#include "core/Log.h"
#include "core/ProcessRunner.h"
#include "core/Progress.h"
#include "core/Result.h"
#include "support/TestSupport.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <atomic>
#include <thread>

using namespace fw;

class TstCore : public QObject {
    Q_OBJECT
    QTemporaryDir m_root;

private slots:
    void initTestCase() { QVERIFY(m_root.isValid()); fwtest::isolateAppData(m_root); }

    void resultCarriesValueOrError()
    {
        Result<int> ok = 42;
        QVERIFY(ok.ok());
        QCOMPARE(ok.value(), 42);
        Result<int> bad = Error(ErrorCode::NotFound, QStringLiteral("nope"), QStringLiteral("detail"));
        QVERIFY(!bad.ok());
        QCOMPARE(bad.error().code, ErrorCode::NotFound);
        QVERIFY(bad.error().toLogString().contains(QStringLiteral("detail")));
        Status s;
        QVERIFY(s.ok());
        auto fn = [](bool fail) -> Status {
            FW_TRY(fail ? Status(Error(ErrorCode::IoError, QStringLiteral("x"))) : Status());
            return success();
        };
        QVERIFY(fn(false).ok());
        QCOMPARE(fn(true).error().code, ErrorCode::IoError);
    }

    void cancellationRunsCallbacksOnce()
    {
        CancellationSource src;
        auto token = src.token();
        int calls = 0;
        auto reg = token.onCancel([&] { ++calls; });
        QVERIFY(token.checkpoint());
        src.cancel(QStringLiteral("test"));
        src.cancel(QStringLiteral("again"));
        QCOMPARE(calls, 1);
        QVERIFY(token.isCancelled());
        QVERIFY(!token.checkpoint());
        QCOMPARE(token.reason(), QStringLiteral("test"));
        // Registering after cancel runs immediately.
        int late = 0;
        auto reg2 = token.onCancel([&] { ++late; });
        QCOMPARE(late, 1);
    }

    void unregisteredCallbackDoesNotRun()
    {
        CancellationSource src;
        int calls = 0;
        {
            auto reg = src.token().onCancel([&] { ++calls; });
        }
        src.cancel();
        QCOMPARE(calls, 0);
    }

    void pauseBlocksCheckpointUntilResume()
    {
        CancellationSource src;
        auto token = src.token();
        src.pause();
        std::atomic<bool> passed{false};
        std::thread t([&] {
            token.checkpoint();
            passed = true;
        });
        QTest::qWait(150);
        QVERIFY(!passed.load());
        src.resume();
        t.join();
        QVERIFY(passed.load());
    }

    void cancelWakesPausedCheckpoint()
    {
        CancellationSource src;
        auto token = src.token();
        src.pause();
        std::atomic<int> r{-1};
        std::thread t([&] { r = token.checkpoint() ? 1 : 0; });
        QTest::qWait(100);
        src.cancel();
        t.join();
        QCOMPARE(r.load(), 0);
    }

    void progressComputesFractionsAndThrottles()
    {
        std::atomic<int> emits{0};
        ProgressSnapshot last;
        std::mutex m;
        ProgressReporter p([&](const ProgressSnapshot& s) {
            ++emits;
            std::lock_guard l(m);
            last = s;
        }, 1000);
        p.setPhases({QStringLiteral("Scan"), QStringLiteral("Write")}, {1.0, 3.0});
        p.beginPhase(1, 1000);
        for (int i = 0; i < 100; ++i)
            p.advance(5);
        p.flush();
        auto s = p.snapshot();
        QCOMPARE(s.phase, QStringLiteral("Write"));
        QCOMPARE(s.done, qint64(500));
        QVERIFY(qAbs(s.overallFraction - (1.0 + 3.0 * 0.5) / 4.0) < 1e-9);
        QVERIFY(emits.load() <= 3); // beginPhase flush + final flush (+ maybe one)
    }

    void progressIsThreadSafe()
    {
        ProgressReporter p({}, 50);
        p.begin(QStringLiteral("Work"), 8 * 100000);
        std::vector<std::thread> ts;
        for (int t = 0; t < 8; ++t)
            ts.emplace_back([&] { for (int i = 0; i < 100000; ++i) p.advance(1); });
        for (auto& t : ts)
            t.join();
        QCOMPARE(p.snapshot().done, qint64(800000));
        QCOMPARE(p.snapshot().overallFraction, 1.0);
    }

    void formatsFollowTheDesignSystem()
    {
        QCOMPARE(formatBytes(0), QStringLiteral("0 B"));
        QCOMPARE(formatBytes(1536), QStringLiteral("1.5 KB"));
        QCOMPARE(formatBytes(qint64(48.2 * 1024 * 1024 * 1024)), QStringLiteral("48.2 GB"));
        QCOMPARE(formatCount(18442), QStringLiteral("18,442"));
        QCOMPARE(formatTimeLeft(65 * 1000), QStringLiteral("About 1 min left"));
        QCOMPARE(formatTimeLeft(30 * 1000), QStringLiteral("Less than a minute left"));
        QCOMPARE(formatTimeLeft(125 * 60 * 1000), QStringLiteral("About 2 h 5 min left"));
        QCOMPARE(formatRate(418.0 * 1024 * 1024), QStringLiteral("418 MB/s"));
    }

    void loggerWritesTextAndJsonAndRingBuffer()
    {
        auto ring = std::make_shared<RingBufferSink>(100);
        auto file = std::make_shared<FileLogSink>(m_root.filePath(QStringLiteral("logs")), QStringLiteral("t"), 1 << 20, 2);
        Logger::instance().addSink(ring);
        Logger::instance().addSink(file);
        Log(QStringLiteral("exfat"), QStringLiteral("J1")).info(QStringLiteral("hello"), {{QStringLiteral("n"), 3}});
        Logger::instance().flush();
        Logger::instance().removeSink(ring);
        Logger::instance().removeSink(file);
        const auto recs = ring->records();
        QVERIFY(!recs.empty());
        QCOMPARE(recs.back().source, QStringLiteral("exfat"));
        QCOMPARE(recs.back().jobId, QStringLiteral("J1"));
        QFile t(file->textPath()), j(file->jsonPath());
        QVERIFY(t.open(QIODevice::ReadOnly));
        QVERIFY(j.open(QIODevice::ReadOnly));
        QVERIFY(t.readAll().contains("[exfat] {J1} hello"));
        QVERIFY(j.readAll().contains("\"source\":\"exfat\""));
    }

    void logRotationKeepsLimitedFiles()
    {
        const QString dir = m_root.filePath(QStringLiteral("rot"));
        auto file = std::make_shared<FileLogSink>(dir, QStringLiteral("r"), 2048, 2);
        Logger::instance().addSink(file);
        for (int i = 0; i < 200; ++i)
            Log(QStringLiteral("t")).info(QStringLiteral("message number %1 with some padding text").arg(i));
        Logger::instance().removeSink(file);
        file.reset();
        const QStringList files = QDir(dir).entryList({QStringLiteral("r*.log")}, QDir::Files);
        QVERIFY(files.contains(QStringLiteral("r.log")));
        QVERIFY(files.contains(QStringLiteral("r.1.log")));
        QVERIFY(files.size() <= 3);
    }

    void partFileCommitsOrCleansUp()
    {
        const QString target = m_root.filePath(QStringLiteral("out/a.bin"));
        QDir().mkpath(m_root.filePath(QStringLiteral("out")));
        {
            PartFile p(target);
            QVERIFY(fwtest::writeTextFile(p.partPath(), "partial"));
        }
        QVERIFY(!QFile::exists(target));
        QVERIFY(!QFile::exists(target + QStringLiteral(".part")));
        {
            PartFile p(target);
            QVERIFY(fwtest::writeTextFile(p.partPath(), "v1"));
            QVERIFY(p.commit().ok());
        }
        QVERIFY(QFile::exists(target));
        {
            PartFile p(target);
            QVERIFY(fwtest::writeTextFile(p.partPath(), "v2"));
            auto st = p.commit(false);
            QVERIFY(!st.ok());
            QCOMPARE(st.error().code, ErrorCode::AlreadyExists);
            QVERIFY(p.commit(true).ok());
        }
        QFile f(target);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), QByteArray("v2"));
    }

    void tempDirIsRemoved()
    {
        QString path;
        {
            TempDir t(QStringLiteral("unit"));
            QVERIFY(t.isValid());
            path = t.path();
            QVERIFY(QFileInfo(path).fileName().startsWith(QStringLiteral("fw-unit-")));
            QVERIFY(fwtest::writeTextFile(t.filePath(QStringLiteral("x/y.txt")), "x"));
        }
        QVERIFY(!QFileInfo::exists(path));
    }

    void copyAndHashStreamLargeFiles()
    {
        const QString src = m_root.filePath(QStringLiteral("big.bin"));
        QVERIFY(fwtest::writeRandomFile(src, 9 * 1024 * 1024 + 123, 7));
        const QString dst = m_root.filePath(QStringLiteral("copy/big.bin"));
        ProgressReporter p;
        p.begin(QStringLiteral("Copy"), QFileInfo(src).size());
        QVERIFY(copyFileStreaming(src, dst, CancellationToken(), &p).ok());
        QCOMPARE(p.snapshot().done, QFileInfo(src).size());
        auto eq = filesEqual(src, dst, CancellationToken());
        QVERIFY(eq.ok() && eq.value());
        auto h1 = sha256File(src, CancellationToken());
        auto h2 = sha256File(dst, CancellationToken());
        QVERIFY(h1.ok() && h2.ok());
        QCOMPARE(h1.value(), h2.value());
        QCOMPARE(h1.value().size(), 64);
    }

    void copyHonoursCancellation()
    {
        const QString src = m_root.filePath(QStringLiteral("c.bin"));
        QVERIFY(fwtest::writeRandomFile(src, 2 * 1024 * 1024, 1));
        CancellationSource cs;
        cs.cancel();
        const QString dst = m_root.filePath(QStringLiteral("c-copy.bin"));
        auto st = copyFileStreaming(src, dst, cs.token());
        QVERIFY(!st.ok());
        QVERIFY(st.error().isCancelled());
        QVERIFY(!QFile::exists(dst));
        QVERIFY(!QFile::exists(dst + QStringLiteral(".part")));
    }

    void insufficientSpaceHasUserMessage()
    {
        auto st = ensureFreeSpace(m_root.path(), qint64(1) << 60);
        QVERIFY(!st.ok());
        QCOMPARE(st.error().code, ErrorCode::InsufficientSpace);
        QVERIFY(st.error().message.startsWith(QStringLiteral("Not enough space on ")));
        QVERIFY(st.error().message.contains(QStringLiteral("choose another output folder")));
        QVERIFY(ensureFreeSpace(m_root.path(), 1024, 0).ok());
    }

    void scanTreeListsAndExcludes()
    {
        const QString root = m_root.filePath(QStringLiteral("game"));
        QVERIFY(fwtest::writeTextFile(root + QStringLiteral("/eboot.bin"), "12345"));
        QVERIFY(fwtest::writeTextFile(root + QStringLiteral("/sce_sys/param.json"), "{}"));
        QVERIFY(fwtest::writeTextFile(root + QStringLiteral("/sce_sys/playgo-chunk.dat"), "pg"));
        QVERIFY(fwtest::writeTextFile(root + QStringLiteral("/Thumbs.db"), "t"));
        QDir().mkpath(root + QStringLiteral("/empty"));
        ScanOptions opt;
        opt.excludes = {QStringLiteral("Thumbs.db"), QStringLiteral("sce_sys/playgo*")};
        auto tree = scanTree(root, CancellationToken(), opt);
        QVERIFY(tree.ok());
        QStringList rels;
        for (const auto& e : tree->entries)
            rels << e.relativePath;
        QCOMPARE(rels, (QStringList{QStringLiteral("eboot.bin"), QStringLiteral("empty"), QStringLiteral("sce_sys"),
                                    QStringLiteral("sce_sys/param.json")}));
        QCOMPARE(tree->fileCount, qint64(2));
        QCOMPARE(tree->totalBytes, qint64(7));
        QCOMPARE(tree->dirCount, qint64(2));
    }

    void removeTreeRefusesDangerousTargets()
    {
        QVERIFY(!removeTreeSafely(QDir::rootPath()).ok());
        QVERIFY(!removeTreeSafely(QDir::homePath()).ok());
        const QString inside = m_root.filePath(QStringLiteral("del/sub"));
        QVERIFY(fwtest::writeTextFile(inside + QStringLiteral("/f.txt"), "x"));
        QVERIFY(!removeTreeSafely(inside, m_root.filePath(QStringLiteral("elsewhere"))).ok());
        QVERIFY(removeTreeSafely(m_root.filePath(QStringLiteral("del")), m_root.path()).ok());
        QVERIFY(!QFileInfo::exists(inside));
    }

    void isSameOrInsideHandlesPrefixes()
    {
        QVERIFY(isSameOrInside(QStringLiteral("/a/b/c"), QStringLiteral("/a/b")));
        QVERIFY(isSameOrInside(QStringLiteral("/a/b"), QStringLiteral("/a/b/")));
        QVERIFY(!isSameOrInside(QStringLiteral("/a/bc"), QStringLiteral("/a/b")));
    }

    void processCollectsLinesAndExitCode()
    {
        ProcessSpec spec;
        spec.program = fwtest::helperPath();
        spec.arguments = {QStringLiteral("print"), QStringLiteral("5")};
        QStringList lines;
        auto r = runProcess(spec, CancellationToken(), [&](const QString& l, bool) { lines << l; });
        QVERIFY(r.ok());
        QVERIFY(r->succeeded());
        QCOMPARE(lines.size(), 5);
        QCOMPARE(lines.last(), QStringLiteral("line 5"));

        spec.arguments = {QStringLiteral("exit"), QStringLiteral("7")};
        r = runProcess(spec, CancellationToken());
        QVERIFY(r.ok());
        QCOMPARE(r->exitCode, 7);
        QVERIFY(!r->succeeded());
    }

    void processSplitsCarriageReturnProgress()
    {
        ProcessSpec spec;
        spec.program = fwtest::helperPath();
        spec.arguments = {QStringLiteral("cr")};
        QStringList lines;
        auto r = runProcess(spec, CancellationToken(), [&](const QString& l, bool) { lines << l; });
        QVERIFY(r.ok());
        QCOMPARE(lines.size(), 6);
        QCOMPARE(lines.first(), QStringLiteral("progress 0%"));
        QCOMPARE(lines.last(), QStringLiteral("done"));
    }

    void processSeparateStderr()
    {
        ProcessSpec spec;
        spec.program = fwtest::helperPath();
        spec.arguments = {QStringLiteral("stderr")};
        spec.mergeStderr = false;
        bool sawErr = false;
        auto r = runProcess(spec, CancellationToken(), [&](const QString& l, bool isErr) {
            if (isErr && l.contains(QStringLiteral("failed")))
                sawErr = true;
        });
        QVERIFY(r.ok());
        QCOMPARE(r->exitCode, 3);
        QVERIFY(sawErr);
        QVERIFY(r->errorOutput.contains(QStringLiteral("something failed")));
    }

    void processCancelStopsQuickly()
    {
        ProcessSpec spec;
        spec.program = fwtest::helperPath();
        spec.arguments = {QStringLiteral("spawn-sleep"), QStringLiteral("20000")};
        CancellationSource cs;
        QElapsedTimer t;
        t.start();
        std::thread canceller([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            cs.cancel();
        });
        auto r = runProcess(spec, cs.token());
        canceller.join();
        QVERIFY(r.ok());
        QVERIFY(r->cancelled);
        QVERIFY2(t.elapsed() < 8000, qPrintable(QString::number(t.elapsed())));
    }

    void processTimeout()
    {
        ProcessSpec spec;
        spec.program = fwtest::helperPath();
        spec.arguments = {QStringLiteral("sleep"), QStringLiteral("20000")};
        spec.timeout = std::chrono::milliseconds(400);
        auto r = runProcess(spec, CancellationToken());
        QVERIFY(r.ok());
        QVERIFY(r->timedOut);
        QVERIFY(!r->succeeded());
    }

    void processMissingProgramIsAnError()
    {
        ProcessSpec spec;
        spec.program = m_root.filePath(QStringLiteral("does-not-exist.exe"));
        auto r = runProcess(spec, CancellationToken());
        QVERIFY(!r.ok());
        QCOMPARE(r.error().code, ErrorCode::ExternalToolFailed);
    }
};

QTEST_GUILESS_MAIN(TstCore)
#include "tst_core.moc"
