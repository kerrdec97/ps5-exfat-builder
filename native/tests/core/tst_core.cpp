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
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <atomic>
#include <limits>
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

    void unregisterWaitsForRunningCallback()
    {
        CancellationSource src;
        std::atomic<bool> started{false}, finished{false};
        auto reg = std::make_unique<CancelRegistration>(src.token().onCancel([&] {
            started = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            finished = true;
        }));
        std::thread canceller([&] { src.cancel(); });
        QVERIFY(QTest::qWaitFor([&] { return started.load(); }, 2000));
        reg.reset(); // must block until the callback has returned
        QVERIFY(finished.load());
        canceller.join();
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

    void progressTracksPhaseTimes()
    {
        ProgressReporter p({}, 50);
        p.setPhases({QStringLiteral("Scan"), QStringLiteral("Write"), QStringLiteral("Verify")});
        QCOMPARE(p.snapshot().elapsedMs, qint64(0));
        QCOMPARE(p.snapshot().phaseMs, (QList<qint64>{0, 0, 0}));
        p.beginPhase(0, 0);
        QTest::qWait(40);
        p.beginPhase(1, 100);
        p.beginPhase(1, 200); // entered again with the real total: the clock keeps running
        QTest::qWait(40);
        auto s = p.snapshot();
        QVERIFY(!s.finished);
        QVERIFY2(s.phaseMs.value(0) >= 30, qPrintable(QString::number(s.phaseMs.value(0))));
        QVERIFY2(s.phaseMs.value(1) >= 30, qPrintable(QString::number(s.phaseMs.value(1))));
        QCOMPARE(s.phaseMs.value(2), qint64(0));
        QVERIFY(s.phaseElapsedMs >= 30);
        QVERIFY(s.elapsedMs >= s.phaseMs.value(0) + s.phaseMs.value(1) - 2);
        p.finish();
        const auto done = p.snapshot();
        QVERIFY(done.finished);
        QTest::qWait(30);
        const auto later = p.snapshot();
        QCOMPARE(later.elapsedMs, done.elapsedMs); // frozen
        QCOMPARE(later.phaseMs, done.phaseMs);
        QCOMPARE(done.phaseMs.size(), 3);
        QVERIFY(done.elapsedMs >= 80);
    }

    // Work a helper does for a job (unpacking a zipped source) keeps the
    // job's phases: the bar neither runs to 100 % nor falls back afterwards.
    void stepKeepsTheDeclaredPhases()
    {
        const QStringList phases{QStringLiteral("Scanning"), QStringLiteral("Writing image")};
        ProgressReporter p;
        p.setPhases(phases, {1.0, 3.0});
        p.beginStep(QStringLiteral("Unpacking ZIP"), 100, 4);
        p.advance(50);
        auto s = p.snapshot();
        QCOMPARE(s.phase, QStringLiteral("Unpacking ZIP"));
        QCOMPARE(s.phaseIndex, 0);
        QCOMPARE(s.phaseCount, 2);
        QCOMPARE(s.total, qint64(100));
        QCOMPARE(s.filesTotal, qint64(4));
        QCOMPARE(s.overallFraction, 0.25 * 0.25); // half of the first half of "Scanning"
        p.advance(50);
        QCOMPARE(p.snapshot().overallFraction, 0.125);
        // The job declares its phases again and starts its first one.
        p.setPhases(phases, {1.0, 3.0});
        p.beginPhase(0, 0);
        s = p.snapshot();
        QCOMPARE(s.phase, QStringLiteral("Scanning"));
        QCOMPARE(s.overallFraction, 0.125); // not back to zero
        p.beginPhase(0, 10);
        p.advance(5);
        QCOMPARE(p.snapshot().overallFraction, 0.25 * 0.75);
        p.beginPhase(1, 10);
        QCOMPARE(p.snapshot().overallFraction, 0.25);
        QCOMPARE(p.snapshot().phaseMs.size(), 2);

        // A job that declares the step as a phase gets exactly that phase.
        ProgressReporter declared;
        declared.setPhases({QStringLiteral("Unpacking ZIP"), QStringLiteral("Scanning")});
        declared.beginPhase(1, 0);
        declared.beginStep(QStringLiteral("Unpacking ZIP"), 10);
        declared.advance(10);
        QCOMPARE(declared.snapshot().phaseIndex, 0);
        QCOMPARE(declared.snapshot().phaseCount, 2);
        QCOMPARE(declared.snapshot().overallFraction, 0.5);

        // No phases declared at all: the step is the only phase, as begin().
        ProgressReporter plain;
        plain.beginStep(QStringLiteral("Unpacking ZIP"), 10);
        plain.advance(10);
        QCOMPARE(plain.snapshot().phase, QStringLiteral("Unpacking ZIP"));
        QCOMPARE(plain.snapshot().phaseCount, 1);
        QCOMPARE(plain.snapshot().overallFraction, 1.0);
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
        // At a unit boundary the rounded value decides the unit.
        QCOMPARE(formatBytes(1023), QStringLiteral("1023 B"));
        QCOMPARE(formatBytes(1024), QStringLiteral("1.0 KB"));
        QCOMPARE(formatBytes(1024 * 1024 - 1), QStringLiteral("1.0 MB")); // not "1024.0 KB"
        QCOMPARE(formatBytes(qint64(1024) * 1024 * 1024 - 1), QStringLiteral("1.0 GB"));
        QCOMPARE(formatBytes(qint64(1023.9 * 1024)), QStringLiteral("1023.9 KB"));
        QCOMPARE(formatRate(0), QStringLiteral("—"));
        QCOMPARE(formatRate(100), QStringLiteral("<1 KB/s")); // not "0 KB/s"
        QCOMPARE(formatRate(600), QStringLiteral("1 KB/s"));
        QCOMPARE(formatRate(1024.0 * 1024 - 1), QStringLiteral("1 MB/s")); // not "1024 KB/s"
        QCOMPARE(formatRate(1024.0 * 1024 * 1024 - 1), QStringLiteral("1.0 GB/s"));
        QCOMPARE(formatRate(2.5 * 1024 * 1024 * 1024), QStringLiteral("2.5 GB/s"));
    }

    // The job monitor's clocks and the rate stand still while a job is
    // paused, and the pause is not part of the elapsed time afterwards.
    void pauseStopsTheClocks()
    {
        ProgressReporter p({}, 50);
        p.setPhases({QStringLiteral("Write"), QStringLiteral("Verify")});
        p.beginPhase(0, 1000);
        QTest::qWait(40);
        p.setPaused(true);
        const auto paused = p.snapshot();
        QVERIFY(paused.elapsedMs >= 30);
        QTest::qWait(120);
        QCOMPARE(p.snapshot().elapsedMs, paused.elapsedMs);
        QCOMPARE(p.snapshot().phaseMs, paused.phaseMs);
        p.setPaused(false);
        QTest::qWait(40);
        const auto resumed = p.snapshot();
        QVERIFY2(resumed.elapsedMs >= paused.elapsedMs + 30, qPrintable(QString::number(resumed.elapsedMs)));
        QVERIFY2(resumed.elapsedMs < paused.elapsedMs + 110, qPrintable(QString::number(resumed.elapsedMs))); // the 120 ms are not in it
        QCOMPARE(resumed.phaseMs.value(0), resumed.elapsedMs);
        // A slow phase counted in steps still gets a time left.
        ProgressReporter slow([](const ProgressSnapshot&) {}, 0);
        slow.beginPhase(0, 10, 0, /*unitIsBytes=*/false);
        QTest::qWait(1200);
        slow.advance(1); // less than one step a second
        QVERIFY(slow.snapshot().ratePerSec > 0.0 && slow.snapshot().ratePerSec < 1.0);
        QVERIFY2(slow.snapshot().etaMs > 0, qPrintable(QString::number(slow.snapshot().etaMs)));
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

    // Another program holds the file that is to be replaced. The commit fails
    // with a clear message and leaves everything as it was; it never answers
    // a refused rename by copying the file (QFile::rename would).
    void partFileCommitNeverCopiesALockedFile()
    {
#ifdef Q_OS_WIN
        const QString dir = m_root.filePath(QStringLiteral("locked"));
        const QString target = dir + QStringLiteral("/image.bin");
        QVERIFY(fwtest::writeTextFile(target, "old image"));
        QFile holder(target); // Qt opens files without delete sharing: a rename is refused
        QVERIFY(holder.open(QIODevice::ReadOnly));
        {
            PartFile p(target);
            QVERIFY(fwtest::writeTextFile(p.partPath(), "new image"));
            const Status st = p.commit(true);
            QVERIFY(!st.ok());
            QCOMPARE(st.error().code, ErrorCode::PermissionDenied);
            QVERIFY(st.error().message.contains(QStringLiteral("image.bin")));
            QVERIFY(!p.committed());
            QCOMPARE(QDir(dir).entryList(QDir::Files, QDir::Name),
                     (QStringList{QStringLiteral("image.bin"), QStringLiteral("image.bin.part")})); // no copy set aside
            QCOMPARE(holder.readAll(), QByteArray("old image"));
            holder.close();
            QVERIFY(p.commit(true).ok());
        }
        QCOMPARE(QDir(dir).entryList(QDir::Files), QStringList{QStringLiteral("image.bin")});
        QFile f(target);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), QByteArray("new image"));
        f.close();

        // The finished .part itself is still held (a scanner): the old file
        // comes back under its name.
        QFile scanner(target + QStringLiteral(".part"));
        PartFile p(target);
        QVERIFY(fwtest::writeTextFile(p.partPath(), "newer image"));
        QVERIFY(scanner.open(QIODevice::ReadOnly));
        const Status st = p.commit(true);
        QVERIFY(!st.ok());
        QCOMPARE(st.error().code, ErrorCode::IoError);
        QVERIFY(st.error().message.contains(QStringLiteral(".part")));
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), QByteArray("new image"));
        scanner.close();
#else
        QSKIP("Windows file sharing rules");
#endif
    }

    void uniquePathAvoidsExistingFiles()
    {
        const QString p = m_root.filePath(QStringLiteral("u/report.zip"));
        QCOMPARE(uniquePath(p), p);
        QVERIFY(fwtest::writeTextFile(p, "1"));
        QCOMPARE(uniquePath(p), m_root.filePath(QStringLiteral("u/report (2).zip")));
        QVERIFY(fwtest::writeTextFile(m_root.filePath(QStringLiteral("u/report (2).zip.part")), "partial"));
        QCOMPARE(uniquePath(p), m_root.filePath(QStringLiteral("u/report (3).zip")));
        // Percent escapes in downloaded names are not placeholders.
        const QString escaped = m_root.filePath(QStringLiteral("u/Update%2BDLC%1%3.pkg"));
        QVERIFY(fwtest::writeTextFile(escaped, "1"));
        QCOMPARE(uniquePath(escaped), m_root.filePath(QStringLiteral("u/Update%2BDLC%1%3 (2).pkg")));
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
            // The marker that tells the app's folders from the user's.
            QVERIFY(QFileInfo(t.filePath(TempDir::ownerMarkerName())).isFile());
            QVERIFY(TempDir::isOwnedFolder(path));
            QVERIFY(!TempDir::isOwnedFolder(t.filePath(QStringLiteral("x"))));
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
        // The temporary drive is full: another output folder would not help.
        const Status temp = ensureFreeTempSpace(m_root.path(), qint64(1) << 60);
        QCOMPARE(temp.error().code, ErrorCode::InsufficientSpace);
        QVERIFY(temp.error().message.startsWith(QStringLiteral("Not enough space on ")));
        QVERIFY(temp.error().message.endsWith(QStringLiteral("or choose another temporary folder in Settings > Storage.")));
        QVERIFY(ensureFreeTempSpace(m_root.path(), 1024, 0).ok());
        // Sizes taken from an archive can be absurd: the sum must not wrap
        // round to "nothing needed".
        const qint64 huge = std::numeric_limits<qint64>::max();
        QCOMPARE(ensureFreeSpace(m_root.path(), huge).error().code, ErrorCode::InsufficientSpace);
        QCOMPARE(ensureFreeSpace(m_root.path(), huge - 5, huge).error().code, ErrorCode::InsufficientSpace);
        QVERIFY(ensureFreeSpace(m_root.path(), -huge, 0).ok());
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
        // What is never deleted as a whole (asked, not tried).
        QVERIFY(isProtectedLocation(QDir::rootPath()));
        QVERIFY(isProtectedLocation(QDir::homePath()));
        QVERIFY(isProtectedLocation(QFileInfo(QDir::homePath()).absolutePath())); // "C:/Users"
        for (const auto location : {QStandardPaths::DesktopLocation, QStandardPaths::DocumentsLocation, QStandardPaths::DownloadLocation}) {
            const QString folder = QStandardPaths::writableLocation(location);
            if (folder.isEmpty())
                continue;
            QVERIFY2(isProtectedLocation(folder), qPrintable(folder));
            QVERIFY2(isProtectedLocation(folder + QStringLiteral("/sub/..")), qPrintable(folder));
            QVERIFY2(!isProtectedLocation(folder + QStringLiteral("/PS5/game-app0")), qPrintable(folder));
        }
#ifdef Q_OS_WIN
        QVERIFY(isProtectedLocation(qEnvironmentVariable("SystemRoot")));
        QVERIFY(isProtectedLocation(QDir::homePath().toUpper()));
#endif
        QVERIFY(!isProtectedLocation(m_root.path()));
        QVERIFY(!isProtectedLocation(inside));
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
        // An absolute path is a program only when it is a file.
        QCOMPARE(findExecutable(fwtest::helperPath()), fwtest::helperPath());
        QVERIFY(findExecutable(m_root.path()).isEmpty());
        QVERIFY(findExecutable(spec.program).isEmpty());
    }
};

QTEST_GUILESS_MAIN(TstCore)
#include "tst_core.moc"
