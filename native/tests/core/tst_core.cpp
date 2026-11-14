// Core infrastructure: Result, cancellation, progress, logging, formatting,
// file utilities, process runner.

#include "core/Cancellation.h"
#include "core/FileUtil.h"
#include "core/Log.h"
#include "core/ProcessRunner.h"
#include "core/Progress.h"
#include "core/Result.h"
#include "core/Tr.h"
#include "support/TestSupport.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QLoggingCategory>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <limits>
#include <thread>

using namespace fw;

namespace {

// A clock the test moves by hand, for the reporter's speed and time left.
struct FakeClock {
    std::atomic<qint64> ms{1'000'000};
    void advance(qint64 by) { ms += by; }
    void attach(ProgressReporter& p)
    {
        p.setClockForTesting([this] { return ProgressReporter::Clock::time_point(std::chrono::milliseconds(ms.load())); });
    }
};

// The percentage the views print (they round down).
int ui100(double fraction) { return int(std::floor(std::clamp(fraction, 0.0, 1.0) * 100.0 + 1e-9)); }

} // namespace

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

    // The app closing is told apart from a Cancel: the engines discard work
    // still being verified then, instead of keeping it unverified.
    void aShutdownIsToldApartFromACancel()
    {
        CancellationSource closing, user, plain;
        QVERIFY(!closing.token().isShutdown()); // not cancelled at all
        closing.cancel(CancellationToken::shutdownReason());
        user.cancel(QStringLiteral("user"));
        plain.cancel();
        QVERIFY(closing.token().isShutdown());
        QVERIFY(!user.token().isShutdown() && user.token().isCancelled());
        QVERIFY(!plain.token().isShutdown() && plain.token().isCancelled());
        // The first reason stands: closing after a Cancel keeps it a Cancel.
        user.cancel(CancellationToken::shutdownReason());
        QVERIFY(!user.token().isShutdown());
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
        // The verdicts are kept until the thread is joined: a check that
        // returned with the thread still running would end the whole program.
        const bool sawStart = QTest::qWaitFor([&] { return started.load(); }, 30000);
        reg.reset(); // must block until the callback has returned
        const bool waited = finished.load();
        canceller.join();
        QVERIFY(sawStart);
        QVERIFY(waited);
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
        const bool passedWhilePaused = passed.load();
        src.resume();
        t.join(); // before any check: see unregisterWaitsForRunningCallback
        QVERIFY(!passedWhilePaused);
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

    // GitHub #97 and closing the app: finished work is kept after a Cancel
    // (keepFinishedWork), never after shutdown(); the last checks of kept
    // work run on an untilShutdown() token, which the Cancel does not stop
    // and shutdown() does, also when it comes after the Cancel.
    void finishedWorkIsKeptAfterACancelButNotWhenTheAppCloses()
    {
        CancellationSource running;
        QVERIFY(!running.token().keepFinishedWork()); // nothing was cancelled
        QVERIFY(!running.token().finishedWorkKept());

        CancellationSource user;
        user.cancel(QStringLiteral("user"));
        const CancellationToken checks = user.token().untilShutdown();
        QVERIFY(!checks.isCancelled());
        QVERIFY(checks.checkpoint());
        QVERIFY(user.token().keepFinishedWork());
        QVERIFY(user.token().finishedWorkKept());
        std::atomic<int> woke{-1};
        std::thread waiting([&] { woke = checks.sleepFor(60000) ? 1 : 0; });
        QTest::qWait(50);
        QElapsedTimer t;
        t.start();
        user.shutdown(); // the app closes while the kept work is checked
        waiting.join();
        QCOMPARE(woke.load(), 0);
        QVERIFY(t.elapsed() < 30000);
        QVERIFY(checks.isCancelled());
        QVERIFY(!checks.checkpoint());
        QVERIFY(user.token().isShuttingDown());
        QCOMPARE(user.token().reason(), QStringLiteral("user")); // the first reason stays
        QVERIFY(!user.token().keepFinishedWork()); // no later yes
        QVERIFY(user.token().finishedWorkKept()); // the earlier one stands
        QVERIFY(user.token().untilShutdown().isCancelled()); // made after it: cancelled at once

        CancellationSource closing;
        const CancellationToken before = closing.token().untilShutdown();
        closing.shutdown();
        QVERIFY(closing.isCancelled());
        QCOMPARE(closing.token().reason(), QStringLiteral("shutdown"));
        QVERIFY(before.isCancelled());
        QVERIFY(!closing.token().keepFinishedWork());
        QVERIFY(!closing.token().finishedWorkKept());
        closing.shutdown(); // again: nothing more happens
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
        // beginPhase flush + final flush (+ maybe one): never one per advance,
        // and the last one carries the final numbers.
        QVERIFY2(emits.load() >= 1 && emits.load() <= 3, qPrintable(QString::number(emits.load())));
        std::lock_guard l(m);
        QCOMPARE(last.done, qint64(500));
        QCOMPARE(last.phase, QStringLiteral("Write"));
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
        // All of it, yet not "100 %": the job has not ended (only the queue says so).
        QVERIFY(p.snapshot().overallFraction >= 0.99 && p.snapshot().overallFraction < 1.0);
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
        QVERIFY(plain.snapshot().overallFraction >= 0.99 && plain.snapshot().overallFraction < 1.0);
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

    // Sizes, rates and counts take the interface language's separators, in
    // Latin digits: German "1,5 KB" and "18.442". A language whose numbers
    // are written in other digits (Arabic) keeps the English form, as do
    // English and the pseudo-languages.
    void formatsUseTheInterfaceLanguagesSeparators()
    {
        const QString res = m_root.filePath(QStringLiteral("number-languages"));
        QVERIFY(QDir().mkpath(res));
        for (const QString& code : {QStringLiteral("de"), QStringLiteral("fr"), QStringLiteral("ar")}) {
            const QJsonObject catalogue{{QStringLiteral("code"), code},
                                        {QStringLiteral("name"), code},
                                        {QStringLiteral("englishName"), code},
                                        {QStringLiteral("strings"), QJsonObject{{QStringLiteral("Add to queue"), QStringLiteral("[%1]").arg(code)}}}};
            QVERIFY(fwtest::writeTextFile(res + u'/' + code + QStringLiteral(".json"), QJsonDocument(catalogue).toJson()));
        }
        i18n::configure(res, {});
        const auto english = qScopeGuard([] {
            i18n::load(QStringLiteral("en"));
            i18n::configure({}, {});
        });
        const auto sample = [] {
            return QStringList{formatBytes(512), formatBytes(1536), formatBytes(qint64(1023.9 * 1024)),
                               formatBytes(qint64(48.2 * 1024 * 1024 * 1024)), formatRate(100), formatRate(0),
                               formatRate(1000.0 * 1024), formatRate(418.0 * 1024 * 1024), formatRate(2.5 * 1024 * 1024 * 1024),
                               formatCount(7), formatCount(18442), formatCount(1234567)};
        };
        const QStringList en{QStringLiteral("512 B"),   QStringLiteral("1.5 KB"),   QStringLiteral("1023.9 KB"), QStringLiteral("48.2 GB"),
                             QStringLiteral("<1 KB/s"), QStringLiteral("—"),        QStringLiteral("1000 KB/s"), QStringLiteral("418 MB/s"),
                             QStringLiteral("2.5 GB/s"), QStringLiteral("7"),       QStringLiteral("18,442"),    QStringLiteral("1,234,567")};
        QCOMPARE(sample(), en);

        QVERIFY(i18n::load(QStringLiteral("de")));
        QCOMPARE(sample(), (QStringList{QStringLiteral("512 B"), QStringLiteral("1,5 KB"), QStringLiteral("1023,9 KB"),
                                        QStringLiteral("48,2 GB"), QStringLiteral("<1 KB/s"), QStringLiteral("—"),
                                        QStringLiteral("1000 KB/s"), QStringLiteral("418 MB/s"), QStringLiteral("2,5 GB/s"),
                                        QStringLiteral("7"), QStringLiteral("18.442"), QStringLiteral("1.234.567")}));

        QVERIFY(i18n::load(QStringLiteral("fr")));
        const QString g = QLocale(QLocale::French).groupSeparator(); // a narrow no-break space
        QVERIFY(g.size() == 1 && g.at(0).isSpace());
        QCOMPARE(sample(), (QStringList{QStringLiteral("512 B"), QStringLiteral("1,5 KB"), QStringLiteral("1023,9 KB"),
                                        QStringLiteral("48,2 GB"), QStringLiteral("<1 KB/s"), QStringLiteral("—"),
                                        QStringLiteral("1000 KB/s"), QStringLiteral("418 MB/s"), QStringLiteral("2,5 GB/s"),
                                        QStringLiteral("7"), QStringLiteral("18") + g + QStringLiteral("442"),
                                        QStringLiteral("1") + g + QStringLiteral("234") + g + QStringLiteral("567")}));

        QVERIFY(i18n::load(QStringLiteral("ar")));
        QVERIFY2(QLocale(QStringLiteral("ar")).zeroDigit() != QLatin1String("0"), "Arabic is expected to use Arabic-Indic digits");
        QCOMPARE(sample(), en);

        QVERIFY(i18n::load(i18n::pseudoCode()));
        QCOMPARE(sample(), en);
        QVERIFY(i18n::load(QStringLiteral("en")));
        QCOMPARE(sample(), en);
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
        // The wait after the pause is measured, not assumed: on a busy PC 40 ms
        // can take far longer, and the pause must still not be counted.
        QElapsedTimer sinceResume;
        sinceResume.start();
        p.setPaused(false);
        QTest::qWait(40);
        const auto resumed = p.snapshot();
        const qint64 waited = sinceResume.elapsed();
        QVERIFY2(resumed.elapsedMs >= paused.elapsedMs + 30, qPrintable(QString::number(resumed.elapsedMs)));
        QVERIFY2(resumed.elapsedMs <= paused.elapsedMs + waited + 20, // the 120 ms are not in it
                 qPrintable(QStringLiteral("%1 after %2 ms").arg(resumed.elapsedMs - paused.elapsedMs).arg(waited)));
        QCOMPARE(resumed.phaseMs.value(0), resumed.elapsedMs);
        // A slow phase counted in steps still gets a time left.
        FakeClock clock;
        ProgressReporter slow([](const ProgressSnapshot&) {}, 0);
        clock.attach(slow);
        slow.beginPhase(0, 10, 0, /*unitIsBytes=*/false);
        clock.advance(4000);
        slow.advance(1); // less than one step a second
        QVERIFY(slow.snapshot().ratePerSec > 0.0 && slow.snapshot().ratePerSec < 1.0);
        QVERIFY2(slow.snapshot().etaMs > 0, qPrintable(QString::number(slow.snapshot().etaMs)));
        QVERIFY2(qAbs(slow.snapshot().etaMs - 36000) < 2000, qPrintable(QString::number(slow.snapshot().etaMs))); // 9 steps at 4 s each
    }

    // The bar never passes 100 % and never moves back while the job runs,
    // whatever the engine reports: more than the total, a count set back, a
    // total that grows, a phase entered again, phases declared anew.
    void progressNeverPassesTheEndOrFallsBack()
    {
        ProgressReporter p;
        p.setPhases({QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C")}, {1.0, 2.0, 1.0});
        double last = 0.0, lastPhase = 0.0;
        auto check = [&](const char* step) {
            const ProgressSnapshot s = p.snapshot();
            const QString where = QStringLiteral("%1: overall %2 (was %3), stage %4 (was %5)")
                                      .arg(QLatin1String(step)).arg(s.overallFraction).arg(last).arg(s.phaseProgress).arg(lastPhase);
            QVERIFY2(std::isfinite(s.overallFraction) && s.overallFraction >= last && s.overallFraction < 1.0, qPrintable(where));
            QVERIFY2(s.phaseProgress >= 0.0 && s.phaseProgress < 1.0, qPrintable(where));
            QVERIFY2(s.total == 0 || (s.done >= 0 && s.done <= s.total), qPrintable(where));
            QVERIFY2(s.phaseFraction() <= 1.0, qPrintable(where));
            last = s.overallFraction;
        };
        auto checkStage = [&](const char* step) {
            const double now = p.snapshot().phaseProgress;
            QVERIFY2(now >= lastPhase, qPrintable(QStringLiteral("%1: stage %2 after %3").arg(QLatin1String(step)).arg(now).arg(lastPhase)));
            lastPhase = now;
        };
        p.beginPhase(0, 100);
        p.advance(60);
        check("60 of 100");
        checkStage("60 of 100");
        p.advance(90); // 150 of 100
        check("150 of 100");
        checkStage("150 of 100");
        QCOMPARE(p.snapshot().done, qint64(100));
        p.setDone(20); // the engine counts again from a lower value
        check("set back to 20");
        checkStage("set back to 20");
        p.setTotal(1000); // the total turned out larger
        check("total grew");
        checkStage("total grew");
        p.beginPhase(0, 500); // the phase again, with its real total
        check("phase again");
        checkStage("phase again");
        p.advance(-50); // a negative step does not make a negative count
        QCOMPARE(p.snapshot().done, qint64(0));
        check("negative step");
        p.beginPhase(1, 0); // a stage without a size
        check("indeterminate stage");
        QVERIFY(p.snapshot().indeterminate);
        QCOMPARE(p.snapshot().overallFraction, 0.25); // A is done, B adds nothing it cannot measure
        lastPhase = 0.0;
        p.setPhases({QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C")}, {0.1, 0.1, 5.0}); // weights declared anew
        check("phases declared anew");
        p.beginPhase(0, 10); // back to an earlier phase
        check("earlier phase");
        p.beginPhase(2, 10);
        p.advance(10);
        check("all done");
        QVERIFY(ui100(p.snapshot().overallFraction) < 100); // "99 %", not "100 %", before the job ends
        p.finish();
        check("finished");

        // A weight of 0 is a phase without a share (a build without its check):
        // the packing phase fills the bar, not half of it.
        ProgressReporter noVerify;
        noVerify.setPhases({QStringLiteral("Preparing"), QStringLiteral("Packing"), QStringLiteral("Checking packs")}, {0.05, 0.95, 0.0});
        noVerify.beginPhase(1, 10);
        noVerify.advance(5);
        QVERIFY2(qAbs(noVerify.snapshot().overallFraction - 0.525) < 1e-9, qPrintable(QString::number(noVerify.snapshot().overallFraction)));
        // Weights that are no numbers count as 1; all of them 0 as equal shares.
        ProgressReporter odd;
        odd.setPhases({QStringLiteral("A"), QStringLiteral("B")}, {std::numeric_limits<double>::quiet_NaN(), -3.0});
        odd.beginPhase(1, 4);
        odd.advance(2);
        QCOMPARE(odd.snapshot().overallFraction, 0.75);
        ProgressReporter zero;
        zero.setPhases({QStringLiteral("A"), QStringLiteral("B")}, {0.0, 0.0});
        zero.beginPhase(1, 4);
        zero.advance(2);
        QCOMPARE(zero.snapshot().overallFraction, 0.75);
        ProgressReporter inf;
        inf.setPhases({QStringLiteral("A"), QStringLiteral("B")}, {std::numeric_limits<double>::infinity(), 1.0});
        inf.beginPhase(0, 4);
        inf.advance(2);
        QVERIFY(std::isfinite(inf.snapshot().overallFraction));
    }

    // A phase entered again (a structure check, then the compare) goes on from
    // where it stood instead of holding still or falling back; a phase that
    // ran in no measurable time is not taken for one that was skipped.
    void progressPhaseEnteredAgainGoesOn()
    {
        ProgressReporter p;
        p.setPhases({QStringLiteral("Writing"), QStringLiteral("Verifying"), QStringLiteral("Replacing")}, {1.0, 1.0, 0.0});
        p.beginPhase(1, 100, 0, false); // the structure check, in inodes
        p.advance(40);
        QCOMPARE(p.snapshot().phaseProgress, 0.4);
        p.beginPhase(1, 1000, 10, true); // the content compare, in bytes
        QCOMPARE(p.snapshot().phaseProgress, 0.4);
        p.advance(500);
        QVERIFY2(qAbs(p.snapshot().phaseProgress - 0.7) < 1e-9, qPrintable(QString::number(p.snapshot().phaseProgress)));
        QVERIFY2(qAbs(p.snapshot().overallFraction - 0.85) < 1e-9, qPrintable(QString::number(p.snapshot().overallFraction)));
        QCOMPARE(p.snapshot().done, qint64(500)); // the numbers are the part's own
        QCOMPARE(p.snapshot().total, qint64(1000));
        const ProgressSnapshot s = p.snapshot();
        QCOMPARE(s.phaseEntered, (QList<bool>{false, true, false}));
        p.beginPhase(2, 0);
        p.finish();
        const ProgressSnapshot end = p.snapshot();
        QCOMPARE(end.phaseEntered, (QList<bool>{false, true, true})); // "Replacing" ran, however short
        QCOMPARE(end.phaseNames, (QStringList{QStringLiteral("Writing"), QStringLiteral("Verifying"), QStringLiteral("Replacing")}));

        // Another job's phases on the same reporter (a conversion runs an
        // extraction, then a build): a new measure, not a bar stuck at its end.
        ProgressReporter c;
        c.setPhases({QStringLiteral("Reading image"), QStringLiteral("Extracting")});
        c.beginPhase(1, 10);
        c.advance(10);
        QVERIFY(c.snapshot().overallFraction > 0.99);
        c.setPhases({QStringLiteral("Scanning"), QStringLiteral("Writing image")});
        c.beginPhase(1, 10);
        c.advance(5);
        QCOMPARE(c.snapshot().overallFraction, 0.75);
        QCOMPARE(c.snapshot().phase, QStringLiteral("Writing image"));
    }

    // A stage that maps another job's whole bar (units of its own) shows that
    // job's figures: bytes, files and speed, never its own "537 of 1,000".
    // The bar keeps following its own count, and the figures go with the
    // stage. Snapshots are numbered in the order they were taken.
    void progressShowsTheFiguresOfOtherWork()
    {
        ProgressReporter p;
        p.setPhases({QStringLiteral("Extracting"), QStringLiteral("Building")});
        p.beginMappedPhase(0, 1000);
        p.setDone(500);
        {
            // Nothing to show yet: no count at all, while the bar moves.
            const ProgressSnapshot first = p.snapshot();
            QCOMPARE(first.total, qint64(0));
            QCOMPARE(first.done, qint64(0));
            QVERIFY(!first.unitIsBytes);
            QVERIFY(!first.indeterminate);
            QVERIFY2(qAbs(first.overallFraction - 0.25) < 1e-9, qPrintable(QString::number(first.overallFraction)));
        }
        ProgressSnapshot child;
        child.done = 3 << 20;
        child.total = 8 << 20;
        child.filesDone = 2;
        child.filesTotal = 5;
        child.unitIsBytes = true;
        child.ratePerSec = 1e6;
        p.showFiguresOf(child);
        const ProgressSnapshot s = p.snapshot();
        QCOMPARE(s.done, qint64(3 << 20));
        QCOMPARE(s.total, qint64(8 << 20));
        QCOMPARE(s.filesDone, qint64(2));
        QCOMPARE(s.filesTotal, qint64(5));
        QVERIFY(s.unitIsBytes);
        QVERIFY(!s.indeterminate);
        QCOMPARE(s.ratePerSec, 1e6);
        QVERIFY2(qAbs(s.overallFraction - 0.25) < 1e-9, qPrintable(QString::number(s.overallFraction))); // the own count
        QVERIFY2(qAbs(s.phaseProgress - 0.5) < 1e-9, qPrintable(QString::number(s.phaseProgress)));
        p.beginPhase(1, 10, 0, false);
        const ProgressSnapshot next = p.snapshot();
        QCOMPARE(next.total, qint64(10));
        QVERIFY(!next.unitIsBytes);
        QCOMPARE(next.filesTotal, qint64(0));
        QVERIFY(next.sequence > s.sequence);
        QVERIFY(s.sequence > 0);
    }

    // Speed and time left with a clock the test drives: hidden at first, close
    // to the truth for steady work, calm through a burst, honest in a stall.
    void progressSpeedAndTimeLeftFollowBurstsAndStalls()
    {
        FakeClock clock;
        ProgressReporter p;
        clock.attach(p);
        constexpr qint64 rate = 10'000'000;   // 10 MB/s
        constexpr qint64 total = 2'000'000'000;
        p.setPhases({QStringLiteral("Copying")});
        p.beginPhase(0, total);
        qint64 done = 0;
        qint64 prevEta = -1;
        int etaJumps = 0;
        QString jumps;
        // Feeds `perStep` bytes every 100 ms for `ms`, taking a snapshot every
        // 500 ms; the shown time left may move by about a fifth at most.
        auto run = [&](qint64 ms, qint64 perStep) {
            for (qint64 t = 0; t < ms; t += 100) {
                clock.advance(100);
                p.advance(perStep);
                done += perStep;
                if ((t + 100) % 500 != 0)
                    continue;
                const qint64 eta = p.snapshot().etaMs;
                if (prevEta >= 0 && eta >= 0 && qAbs(eta - (prevEta - 500)) > prevEta / 5 + 1000) {
                    ++etaJumps;
                    jumps += QStringLiteral(" %1->%2").arg(prevEta).arg(eta);
                }
                prevEta = eta;
            }
        };
        run(2500, rate / 10);
        QCOMPARE(p.snapshot().etaMs, qint64(-1)); // too early to tell
        QCOMPARE(p.snapshot().jobEtaMs, qint64(-1));
        run(7500, rate / 10); // 10 s of steady work
        ProgressSnapshot s = p.snapshot();
        const double truth = double(total - done) / double(rate) * 1000.0;
        QVERIFY2(s.etaMs > 0 && qAbs(double(s.etaMs) - truth) < truth * 0.05, qPrintable(QStringLiteral("%1 vs %2").arg(s.etaMs).arg(truth)));
        QVERIFY2(qAbs(s.ratePerSec - double(rate)) < rate * 0.05, qPrintable(QString::number(s.ratePerSec)));
        QVERIFY2(qAbs(s.averageRatePerSec - double(rate)) < rate * 0.05, qPrintable(QString::number(s.averageRatePerSec)));
        QCOMPARE(s.jobEtaMs >= 0, true); // the only phase: the job's is the phase's
        QVERIFY2(qAbs(s.jobEtaMs - s.etaMs) < 1000, qPrintable(QStringLiteral("%1 vs %2").arg(s.jobEtaMs).arg(s.etaMs)));

        // A burst ten times as fast for a second: the speed shows it, the time left barely moves.
        const qint64 etaBefore = s.etaMs;
        run(1000, rate);
        s = p.snapshot();
        QVERIFY2(s.ratePerSec > 2.0 * rate, qPrintable(QString::number(s.ratePerSec)));
        QVERIFY2(s.etaMs > etaBefore * 0.6, qPrintable(QStringLiteral("%1 after %2").arg(s.etaMs).arg(etaBefore)));
        run(20000, rate / 10);

        // A stall of 20 s: the speed falls towards nothing, the time left grows
        // instead of counting down to zero, and without leaps.
        const qint64 etaAtStall = p.snapshot().etaMs;
        run(20000, 0);
        s = p.snapshot();
        QVERIFY2(s.ratePerSec < rate * 0.05, qPrintable(QString::number(s.ratePerSec)));
        QVERIFY2(s.etaMs > etaAtStall, qPrintable(QStringLiteral("%1 after %2").arg(s.etaMs).arg(etaAtStall)));
        QVERIFY2(s.averageRatePerSec < rate, qPrintable(QString::number(s.averageRatePerSec)));
        // Nobody calls advance() during a stall; a snapshot alone shows it.
        const double speedThen = p.snapshot().ratePerSec;
        clock.advance(5000);
        QVERIFY(p.snapshot().ratePerSec < speedThen);
        prevEta = p.snapshot().etaMs; // 5 s between two looks: a larger step is right here
        // Work goes on: the time left comes back down to the truth.
        run(60000, rate / 10);
        s = p.snapshot();
        const double truthAfter = double(total - done) / double(rate) * 1000.0;
        QVERIFY2(qAbs(double(s.etaMs) - truthAfter) < truthAfter * 0.15, qPrintable(QStringLiteral("%1 vs %2").arg(s.etaMs).arg(truthAfter)));
        QVERIFY2(etaJumps == 0, qPrintable(QStringLiteral("%1 leaps:%2").arg(etaJumps).arg(jumps)));
        // The time left counts down between measurements.
        const qint64 a = p.snapshot().etaMs;
        clock.advance(300);
        QCOMPARE(p.snapshot().etaMs, std::max<qint64>(0, a - 300));
        // Paused: everything stands still; the pause is not part of the speed.
        const ProgressSnapshot beforePause = p.snapshot();
        p.setPaused(true);
        clock.advance(60000);
        QCOMPARE(p.snapshot().etaMs, beforePause.etaMs);
        QCOMPARE(p.snapshot().ratePerSec, 0.0);
        p.setPaused(false);
        clock.advance(600);
        p.advance(rate * 9 / 10); // the 300 ms before the pause and the 600 ms after it
        s = p.snapshot();
        QVERIFY2(qAbs(s.ratePerSec - beforePause.ratePerSec) < rate * 0.1, qPrintable(QStringLiteral("%1 vs %2").arg(s.ratePerSec).arg(beforePause.ratePerSec)));
        QVERIFY2(qAbs(s.etaMs - (beforePause.etaMs - 600)) < 3000, qPrintable(QStringLiteral("%1 vs %2").arg(s.etaMs).arg(beforePause.etaMs)));
        p.finish();
        QCOMPARE(p.snapshot().etaMs, qint64(-1));
        QCOMPARE(p.snapshot().ratePerSec, 0.0);
    }

    // A new phase is measured afresh; the job's time left covers the phases
    // still to come and counts on through the change; a phase without a size
    // has no time left at all.
    void progressTimeLeftCoversTheWholeJob()
    {
        FakeClock clock;
        ProgressReporter p;
        clock.attach(p);
        p.setPhases({QStringLiteral("Scanning"), QStringLiteral("Writing"), QStringLiteral("Verifying")}, {0.0, 3.0, 1.0});
        p.beginPhase(0, 0);
        clock.advance(2000);
        QCOMPARE(p.snapshot().jobEtaMs, qint64(-1)); // nobody can say yet
        p.beginPhase(1, 300, 0, false); // 300 steps at 10 a second: 30 s
        for (int i = 0; i < 100; ++i) {
            clock.advance(100);
            p.advance(1);
            (void)p.snapshot();
        }
        ProgressSnapshot s = p.snapshot();
        QVERIFY2(qAbs(s.etaMs - 20000) < 1000, qPrintable(QString::number(s.etaMs)));
        // The verify phase has a third of the writing's share: about 32 s / 3 more.
        const qint64 expectJob = 20000 + (2000 + 10000 + 20000) / 3;
        QVERIFY2(qAbs(s.jobEtaMs - expectJob) < 2500, qPrintable(QStringLiteral("%1 vs %2").arg(s.jobEtaMs).arg(expectJob)));
        QVERIFY(s.jobEtaMs > s.etaMs);
        for (int i = 0; i < 200; ++i) { // the rest of the writing
            clock.advance(100);
            p.advance(1);
            (void)p.snapshot();
        }
        QVERIFY2(qAbs(p.snapshot().jobEtaMs - 32000 / 3) < 1500, qPrintable(QString::number(p.snapshot().jobEtaMs)));

        p.beginPhase(2, 1000, 0, false); // 1000 steps at 100 a second
        s = p.snapshot();
        QCOMPARE(s.etaMs, qint64(-1)); // the new work is not measured yet
        QVERIFY2(s.jobEtaMs > 0, qPrintable(QString::number(s.jobEtaMs))); // the job's estimate counts on
        const qint64 jobBefore = s.jobEtaMs;
        clock.advance(400);
        QCOMPARE(p.snapshot().jobEtaMs, jobBefore - 400);
        for (int i = 0; i < 40; ++i) {
            clock.advance(100);
            p.advance(10);
            (void)p.snapshot();
        }
        s = p.snapshot();
        QVERIFY2(s.etaMs > 4000 && s.etaMs < 9000, qPrintable(QString::number(s.etaMs))); // 600 left: about 6 s
        QVERIFY2(qAbs(s.jobEtaMs - s.etaMs) < 3000, qPrintable(QStringLiteral("%1 vs %2").arg(s.jobEtaMs).arg(s.etaMs))); // the last phase

        // A stage without a size: no time left for it nor for the job.
        p.setPhases({QStringLiteral("Packing"), QStringLiteral("Finishing")});
        p.beginPhase(1, 0);
        clock.advance(1000);
        s = p.snapshot();
        QCOMPARE(s.etaMs, qint64(-1));
        QCOMPARE(s.jobEtaMs, qint64(-1));
        QVERIFY(s.indeterminate);
    }

    // Many workers advance while the UI takes snapshots and the job changes
    // phase: counts add up and the bar only moves forward.
    void progressIsThreadSafeWhileWatched()
    {
        ProgressReporter p([](const ProgressSnapshot&) {}, 1);
        p.setPhases({QStringLiteral("One"), QStringLiteral("Two")});
        p.beginPhase(0, 4 * 50000);
        std::atomic<bool> stop{false};
        std::atomic<int> backwards{0};
        std::thread watcher([&] {
            double last = 0.0;
            while (!stop.load()) {
                const ProgressSnapshot s = p.snapshot();
                if (s.overallFraction < last || s.overallFraction >= 1.0 || !std::isfinite(s.ratePerSec))
                    ++backwards;
                last = s.overallFraction;
            }
        });
        std::vector<std::thread> ts;
        for (int t = 0; t < 4; ++t)
            ts.emplace_back([&] {
                for (int i = 0; i < 50000; ++i) {
                    p.advance(1);
                    if (i % 1000 == 0)
                        p.advanceFiles(1);
                }
            });
        for (auto& t : ts)
            t.join();
        const qint64 doneFirst = p.snapshot().done;
        p.beginPhase(1, 100);
        p.setCurrentItem(QStringLiteral("x"));
        p.advance(100);
        stop = true;
        watcher.join();
        QCOMPARE(doneFirst, qint64(200000));
        QCOMPARE(backwards.load(), 0);
        QCOMPARE(p.snapshot().done, qint64(100));
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

    // Qt's own notes ("OpenType support missing for ...", qt.text.font.db at
    // Info, a dozen per start) are detail, not news: they stay out of an Info
    // log. Qt's warnings, and the app's own Info lines, keep their level.
    void qtsOwnNotesAreDetail()
    {
        const QLoggingCategory fontDb("qt.text.font.db", QtInfoMsg);
        const QLoggingCategory appCat("fw.test", QtInfoMsg);
        const LogLevel before = Logger::instance().minimumLevel();
        // The records of `emitAll` at the minimum level `min`, through the app's handler.
        auto capture = [&](LogLevel min, const std::function<void()>& emitAll) {
            auto ring = std::make_shared<RingBufferSink>(100);
            Logger::instance().addSink(ring);
            Logger::instance().setMinimumLevel(min);
            const QtMessageHandler previous = qInstallMessageHandler(nullptr);
            Logger::installQtMessageHandler();
            emitAll();
            qInstallMessageHandler(previous);
            Logger::instance().setMinimumLevel(before);
            Logger::instance().removeSink(ring);
            QStringList seen;
            for (const LogRecord& r : ring->records())
                if (r.source == QLatin1String("qt"))
                    seen << QStringLiteral("%1 %2 %3").arg(logLevelName(r.level), r.fields.value(QStringLiteral("category")).toString(), r.message);
            return seen;
        };
        QCOMPARE(capture(LogLevel::Info,
                         [&] {
                             qCInfo(fontDb) << "OpenType support missing for Tahoma, script 12";
                             qCWarning(fontDb) << "a real font problem";
                             qCInfo(appCat) << "an app note";
                         }),
                 (QStringList{QStringLiteral("warning qt.text.font.db a real font problem"), QStringLiteral("info fw.test an app note")}));
        // With the detail level on, the note is there, as detail.
        QCOMPARE(capture(LogLevel::Debug, [&] { qCInfo(fontDb) << "OpenType support missing"; }),
                 QStringList{QStringLiteral("debug qt.text.font.db OpenType support missing")});
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
        QVERIFY(!QFile::exists(PartFile::partPathFor(target)));
        {
            PartFile p(target);
            QCOMPARE(p.partPath(), target + QStringLiteral(".fw.part"));
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
                     (QStringList{QStringLiteral("image.bin"), QStringLiteral("image.bin.fw.part")})); // no copy set aside
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
        PartFile p(target);
        QFile scanner(p.partPath());
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

    // A file that is already called "<name>.part" (a download, a game file of
    // that name) is never written over, renamed or removed: the work goes to
    // "<name>.fw.part", which is this app's own and replaced when left over.
    void partFileNeverTakesAFileCalledLikeItsPart()
    {
        const QString dir = m_root.filePath(QStringLiteral("theirs"));
        const QString target = dir + QStringLiteral("/level.bin");
        QVERIFY(fwtest::writeTextFile(target + QStringLiteral(".part"), "the user's download"));
        {
            PartFile p(target);
            QCOMPARE(p.partPath(), target + QStringLiteral(".fw.part"));
            QVERIFY(fwtest::writeTextFile(p.partPath(), "half"));
        } // not committed: only its own file goes
        QCOMPARE(QDir(dir).entryList(QDir::Files), QStringList{QStringLiteral("level.bin.part")});
        QVERIFY(fwtest::writeTextFile(target + QStringLiteral(".fw.part"), "left by an interrupted run"));
        {
            PartFile p(target);
            QVERIFY(!QFileInfo::exists(p.partPath())); // the leftover is gone, so NewOnly works
            QFile f(p.partPath());
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::NewOnly));
            f.write("new");
            f.close();
            QVERIFY(p.commit().ok());
        }
        QFile part(target + QStringLiteral(".part"));
        QVERIFY(part.open(QIODevice::ReadOnly));
        QCOMPARE(part.readAll(), QByteArray("the user's download"));
        part.close();
        QFile f(target);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), QByteArray("new"));
        f.close();
        QCOMPARE(QDir(dir).entryList(QDir::Files, QDir::Name), (QStringList{QStringLiteral("level.bin"), QStringLiteral("level.bin.part")}));
        // copyFileStreaming writes through PartFile too.
        const QString src = m_root.filePath(QStringLiteral("theirs-src.bin"));
        QVERIFY(fwtest::writeTextFile(src, "copied"));
        QVERIFY(copyFileStreaming(src, target, CancellationToken(), nullptr, true).ok());
        QVERIFY(part.open(QIODevice::ReadOnly));
        QCOMPARE(part.readAll(), QByteArray("the user's download"));
    }

    // A "<name>.part" left alone is named in the log, once per file written
    // beside it, so a leftover of an older version can be found and deleted.
    void partFileSaysWhichFileItLeftAlone()
    {
        const QString dir = m_root.filePath(QStringLiteral("left-alone"));
        const QString target = dir + QStringLiteral("/game.exfat");
        QVERIFY(QDir().mkpath(dir));
        auto ring = std::make_shared<RingBufferSink>(100);
        Logger::instance().addSink(ring);
        const auto detach = qScopeGuard([&] { Logger::instance().removeSink(ring); });
        const auto warnings = [&] {
            std::vector<LogRecord> found;
            for (const LogRecord& r : ring->records())
                if (r.level == LogLevel::Warning)
                    found.push_back(r);
            return found;
        };
        { PartFile p(target); }
        QVERIFY(warnings().empty()); // nothing to leave alone
        QVERIFY(fwtest::writeTextFile(target + QStringLiteral(".part"), "an older version's leftover"));
        {
            PartFile p(target);
            QCOMPARE(p.partPath(), target + QStringLiteral(".fw.part"));
        }
        const auto found = warnings();
        QCOMPARE(found.size(), size_t(1));
        const QString message = found.front().message;
        QVERIFY2(message.contains(QDir::toNativeSeparators(target + QStringLiteral(".part"))), qPrintable(message));
        QVERIFY2(message.contains(QDir::toNativeSeparators(target + QStringLiteral(".fw.part"))), qPrintable(message));
        QCOMPARE(found.front().fields.value(QStringLiteral("file")).toString(), QDir::toNativeSeparators(target + QStringLiteral(".part")));
        QFile part(target + QStringLiteral(".part"));
        QVERIFY(part.open(QIODevice::ReadOnly));
        QCOMPARE(part.readAll(), QByteArray("an older version's leftover"));
    }

    // A crash or a power cut leaves the working file of an unfinished write.
    // It is this app's by its name: the next write of the same file (Retry)
    // removes it and works under that name, so the retry needs no room for a
    // second copy and nothing is left behind.
    void partFileTakesBackItsOwnLeftover()
    {
        const QString dir = m_root.filePath(QStringLiteral("own-leftover"));
        const QString target = dir + QStringLiteral("/game.ffpkg");
        QVERIFY(QDir().mkpath(dir));
        QString leftover;
        {
            PartFile p(target);
            leftover = p.partPath();
            QVERIFY(fwtest::writeTextFile(leftover, "half of an image"));
            QVERIFY(QFile::copy(leftover, dir + QStringLiteral("/crash")));
        }
        QVERIFY(QFile::rename(dir + QStringLiteral("/crash"), leftover)); // what the crash left
        {
            PartFile p(target);
            QCOMPARE(p.partPath(), leftover);
            QVERIFY(!QFileInfo::exists(leftover));
            QFile f(p.partPath());
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::NewOnly));
            f.write("the whole image");
            f.close();
            QVERIFY(p.commit().ok());
        }
        QCOMPARE(QDir(dir).entryList(QDir::Files | QDir::Hidden), QStringList{QStringLiteral("game.ffpkg")});
    }

    // NTFS and exFAT allow 255 characters in a name. A file of a name with no
    // room for ".fw.part" is still written: its working name is shortened
    // (the same each time, so a leftover is found again), and an image, a
    // copy or a game file of up to 255 characters gets its name.
    void partFileWritesNamesUpToTheLimit_data()
    {
        QTest::addColumn<int>("length");
        for (const int n : {247, 248, 250, 255})
            QTest::newRow(qPrintable(QString::number(n))) << n;
    }
    void partFileWritesNamesUpToTheLimit()
    {
        QFETCH(int, length);
        const QString dir = m_root.filePath(QStringLiteral("long-%1").arg(length));
        QVERIFY(QDir().mkpath(dir));
        const QString name = QString(length - 4, u'n') + QStringLiteral(".bin");
        const QString target = dir + u'/' + name;
        const QString working = PartFile::partPathFor(target);
        QCOMPARE(PartFile::partPathFor(target), working);
        QCOMPARE(QFileInfo(working).absolutePath(), QFileInfo(target).absolutePath());
        QVERIFY(QFileInfo(working).fileName().size() <= 255);
        QVERIFY(working.endsWith(QStringLiteral(".fw.part")));
        if (length <= 247)
            QCOMPARE(working, target + QStringLiteral(".fw.part"));
        // The same start, another name: another working name.
        QVERIFY(PartFile::partPathFor(dir + u'/' + QString(length - 4, u'n') + QStringLiteral(".bim")) != working);
        // It depends on the name only: a relative path gets the same one (the
        // archive checks compare relative paths with it).
        QCOMPARE(PartFile::partPathFor(QStringLiteral("x/") + name), QStringLiteral("x/") + QFileInfo(working).fileName());

        QVERIFY(fwtest::writeTextFile(working, "left by a crash"));
        {
            PartFile p(target);
            QCOMPARE(p.partPath(), working);
            QVERIFY(!QFileInfo::exists(working)); // the leftover is gone
            QFile f(p.partPath());
            QVERIFY2(f.open(QIODevice::WriteOnly | QIODevice::NewOnly), qPrintable(f.errorString()));
            f.write("whole");
            f.close();
            const Status st = p.commit();
            QVERIFY2(st.ok(), qPrintable(st.ok() ? QString() : st.error().toLogString()));
        }
        QFile written(target);
        QVERIFY(written.open(QIODevice::ReadOnly));
        QCOMPARE(written.readAll(), QByteArray("whole"));
        written.close();
        // A copy to such a name, as the Autoloader and the backport updater make.
        const QString copy = dir + u'/' + QString(length - 4, u'c') + QStringLiteral(".bin");
        const Status copied = copyFileStreaming(target, copy, CancellationToken());
        QVERIFY2(copied.ok(), qPrintable(copied.ok() ? QString() : copied.error().toLogString()));
        QCOMPARE(QDir(dir).entryList(QDir::Files | QDir::Hidden, QDir::Name), (QStringList{QFileInfo(copy).fileName(), name}));
    }

    // Older versions set a replaced file aside as "<name>.fw-old" and could
    // leave it there (a read-only one could not be deleted). Such a file is
    // never taken back: a free name stays free for a new file, and a replace
    // does not touch it.
    void partFileLeavesAnOldSetAsideAlone()
    {
        const QString dir = m_root.filePath(QStringLiteral("old-aside"));
        const QString target = dir + QStringLiteral("/image.bin");
        const auto content = [](const QString& path) {
            QFile f(path);
            return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray("<missing>");
        };
        QVERIFY(fwtest::writeTextFile(target + QStringLiteral(".fw-old"), "years old"));
        {
            PartFile p(target);
            QVERIFY(fwtest::writeTextFile(p.partPath(), "new"));
            const Status st = p.commit(false);
            QVERIFY2(st.ok(), qPrintable(st.ok() ? QString() : st.error().toLogString()));
        }
        QCOMPARE(content(target), QByteArray("new"));
        {
            PartFile p(target);
            QVERIFY(fwtest::writeTextFile(p.partPath(), "newer"));
            QVERIFY(p.commit(true).ok());
        }
        QCOMPARE(content(target), QByteArray("newer"));
        QCOMPARE(content(target + QStringLiteral(".fw-old")), QByteArray("years old"));
        QCOMPARE(QDir(dir).entryList(QDir::Files, QDir::Name), (QStringList{QStringLiteral("image.bin"), QStringLiteral("image.bin.fw-old")}));
    }

    // Replacing a file never leaves its name empty: the old file is replaced
    // in one step, a read-only one too (it loses the flag for that step and
    // gets it back when the replace fails), and nothing is set aside.
    void partFileReplaceNeverLeavesTheNameEmpty()
    {
        const QString dir = m_root.filePath(QStringLiteral("replace"));
        const QString target = dir + QStringLiteral("/image.bin");
        const auto content = [](const QString& path) {
            QFile f(path);
            return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray("<missing>");
        };
        QVERIFY(fwtest::writeTextFile(target, "old"));
        {
            PartFile p(target);
            QVERIFY(fwtest::writeTextFile(p.partPath(), "new"));
            QVERIFY(p.commit(true).ok());
        }
        QCOMPARE(content(target), QByteArray("new"));
        QCOMPARE(QDir(dir).entryList(QDir::Files), QStringList{QStringLiteral("image.bin")});

        // Read-only: replaced all the same, and nothing is left beside it.
        QVERIFY(QFile::setPermissions(target, QFileDevice::ReadOwner | QFileDevice::ReadUser));
        {
            PartFile p(target);
            QVERIFY(fwtest::writeTextFile(p.partPath(), "newer"));
            const Status st = p.commit(true);
            QVERIFY2(st.ok(), qPrintable(st.ok() ? QString() : st.error().toLogString()));
        }
        QCOMPARE(content(target), QByteArray("newer"));
        QCOMPARE(QDir(dir).entryList(QDir::Files), QStringList{QStringLiteral("image.bin")});

#ifdef Q_OS_WIN
        // A read-only file that cannot be replaced (another program has it
        // open) stays as it was, read-only flag and all.
        QVERIFY(QFile::setPermissions(target, QFileDevice::ReadOwner | QFileDevice::ReadUser));
        {
            QFile holder(target); // Qt opens files without delete sharing: a rename is refused
            QVERIFY(holder.open(QIODevice::ReadOnly));
            PartFile p(target);
            QVERIFY(fwtest::writeTextFile(p.partPath(), "newest"));
            const Status st = p.commit(true);
            QVERIFY(!st.ok());
            QCOMPARE(st.error().code, ErrorCode::PermissionDenied);
            QVERIFY(!QFileInfo(target).isWritable());
            QCOMPARE(holder.readAll(), QByteArray("newer"));
            holder.close();
            QVERIFY(p.commit(true).ok());
        }
        QCOMPARE(content(target), QByteArray("newest"));
        QCOMPARE(QDir(dir).entryList(QDir::Files), QStringList{QStringLiteral("image.bin")});
#endif

        // A folder of that name is never set aside.
        const QString folder = dir + QStringLiteral("/folder.bin");
        QVERIFY(fwtest::writeTextFile(folder + QStringLiteral("/inside.txt"), "mine"));
        PartFile p(folder);
        QVERIFY(fwtest::writeTextFile(p.partPath(), "image"));
        const Status st = p.commit(true);
        QVERIFY(!st.ok());
        QCOMPARE(st.error().code, ErrorCode::AlreadyExists);
        QCOMPARE(content(folder + QStringLiteral("/inside.txt")), QByteArray("mine"));
        QVERIFY(!QFileInfo::exists(folder + QStringLiteral(".fw-old")));
    }

    // QFile::rename() copies a file it cannot rename, under the new name;
    // renameWithoutCopying() never does, and never replaces anything.
    void renameWithoutCopyingNeverCopies()
    {
        const QString dir = m_root.filePath(QStringLiteral("nocopy"));
        const QString from = dir + QStringLiteral("/built.pkg"), to = dir + QStringLiteral("/final.pkg");
        QVERIFY(fwtest::writeTextFile(from, "package"));
        QVERIFY(fwtest::writeTextFile(to, "taken"));
        QString why;
        QVERIFY(!renameWithoutCopying(from, to, &why));
        QVERIFY(!why.isEmpty());
        QVERIFY(QFile::remove(to));
#ifdef Q_OS_WIN
        {
            QFile holder(from); // Qt opens files without delete sharing: a rename is refused
            QVERIFY(holder.open(QIODevice::ReadOnly));
            QVERIFY(!renameWithoutCopying(from, to, &why));
            QVERIFY(!QFileInfo::exists(to)); // no copy under the new name
            // One attempt: refused at once, not after the ~1 s wait for a scanner.
            QElapsedTimer t;
            t.start();
            QVERIFY(!renameWithoutCopying(from, to, &why, 1));
            QVERIFY2(t.elapsed() < 90, qPrintable(QString::number(t.elapsed())));
            QVERIFY(!why.isEmpty());
            QVERIFY(!QFileInfo::exists(to));
        }
#endif
        QVERIFY(renameWithoutCopying(from, to));
        QVERIFY(!QFileInfo::exists(from));
        {
            QFile f(to);
            QVERIFY(f.open(QIODevice::ReadOnly));
            QCOMPARE(f.readAll(), QByteArray("package"));
        }
#ifdef Q_OS_WIN
        // A change of case only: the name of the target is the file itself.
        QVERIFY2(renameWithoutCopying(to, dir + QStringLiteral("/FINAL.pkg"), &why), qPrintable(why));
        QCOMPARE(QDir(dir).entryList(QDir::Files), QStringList{QStringLiteral("FINAL.pkg")});
        // Another file of that name is still never replaced.
        QVERIFY(fwtest::writeTextFile(from, "other"));
        QVERIFY(!renameWithoutCopying(from, dir + QStringLiteral("/Final.pkg"), &why));
        QVERIFY(QFileInfo::exists(from));
        QFile kept(dir + QStringLiteral("/FINAL.pkg"));
        QVERIFY(kept.open(QIODevice::ReadOnly));
        QCOMPARE(kept.readAll(), QByteArray("package"));
#endif
    }

    // replaceWithoutCopying(): one step over an existing file, never a copy.
    void replaceWithoutCopyingNeverCopies()
    {
        const QString dir = m_root.filePath(QStringLiteral("replace-nocopy"));
        const QString from = dir + QStringLiteral("/new.bin"), to = dir + QStringLiteral("/old.bin");
        const auto content = [](const QString& path) {
            QFile f(path);
            return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray("<missing>");
        };
        QVERIFY(fwtest::writeTextFile(from, "new"));
        QVERIFY(fwtest::writeTextFile(to, "old"));
#ifdef Q_OS_WIN
        {
            QFile holder(to); // held without delete sharing: the replace is refused
            QVERIFY(holder.open(QIODevice::ReadOnly));
            QString why;
            QVERIFY(!replaceWithoutCopying(from, to, &why));
            QVERIFY(!why.isEmpty());
            QCOMPARE(QDir(dir).entryList(QDir::Files, QDir::Name), (QStringList{QStringLiteral("new.bin"), QStringLiteral("old.bin")}));
        }
        QCOMPARE(content(to), QByteArray("old"));
#endif
        QVERIFY(replaceWithoutCopying(from, to));
        QVERIFY(!QFileInfo::exists(from));
        QCOMPARE(content(to), QByteArray("new"));
    }

    void uniquePathAvoidsExistingFiles()
    {
        const QString p = m_root.filePath(QStringLiteral("u/report.zip"));
        QCOMPARE(uniquePath(p), p);
        QVERIFY(fwtest::writeTextFile(p, "1"));
        QCOMPARE(uniquePath(p), m_root.filePath(QStringLiteral("u/report (2).zip")));
        QVERIFY(fwtest::writeTextFile(m_root.filePath(QStringLiteral("u/report (2).zip.part")), "partial"));
        QCOMPARE(uniquePath(p), m_root.filePath(QStringLiteral("u/report (3).zip")));
        // This app's own working file, of a job that is still running, too.
        QVERIFY(fwtest::writeTextFile(PartFile::partPathFor(m_root.filePath(QStringLiteral("u/report (3).zip"))), "partial"));
        QCOMPARE(uniquePath(p), m_root.filePath(QStringLiteral("u/report (4).zip")));
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
        QVERIFY(!QFile::exists(PartFile::partPathFor(dst)));
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
        QVERIFY(!temp.ok());
        QCOMPARE(temp.error().code, ErrorCode::InsufficientSpace);
        QVERIFY(temp.error().message.startsWith(QStringLiteral("Not enough space on ")));
        QVERIFY(temp.error().message.endsWith(QStringLiteral("or choose another temporary folder in Settings > Storage.")));
        QVERIFY(ensureFreeTempSpace(m_root.path(), 1024, 0).ok());
        // Sizes taken from an archive can be absurd: the sum must not wrap
        // round to "nothing needed".
        const qint64 huge = std::numeric_limits<qint64>::max();
        const Status whole = ensureFreeSpace(m_root.path(), huge);
        QVERIFY(!whole.ok());
        QCOMPARE(whole.error().code, ErrorCode::InsufficientSpace);
        const Status summed = ensureFreeSpace(m_root.path(), huge - 5, huge);
        QVERIFY(!summed.ok());
        QCOMPARE(summed.error().code, ErrorCode::InsufficientSpace);
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
        // The drive root and the home folder are only asked about below: a
        // test never hands them to a function that deletes.
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
        // The helper starts a second process that writes its id to this file:
        // the cancel comes once both run, and must end both.
        const QString pidFile = m_root.filePath(QStringLiteral("grandchild.pid"));
        QFile::remove(pidFile);
        spec.arguments = {QStringLiteral("spawn-sleep"), QStringLiteral("60000"), QDir::toNativeSeparators(pidFile)};
        CancellationSource cs;
        QElapsedTimer t;
        std::atomic<bool> sawGrandchild{false};
        std::atomic<qint64> cancelledAt{0};
        t.start();
        std::thread canceller([&] {
            for (int i = 0; i < 3000 && !QFileInfo::exists(pidFile); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            sawGrandchild = QFileInfo::exists(pidFile);
            cancelledAt = t.elapsed();
            cs.cancel();
        });
        auto r = runProcess(spec, cs.token());
        const qint64 returnedAt = t.elapsed();
        canceller.join();
        QVERIFY(r.ok());
        QVERIFY(r->cancelled);
        QVERIFY2(sawGrandchild.load(), "the helper's second process never started");
        QVERIFY2(returnedAt - cancelledAt.load() < 8000, qPrintable(QString::number(returnedAt - cancelledAt.load())));
        QFile f(pidFile);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const qint64 grandchild = f.readAll().trimmed().toLongLong();
        QVERIFY(grandchild > 0);
        // Nothing keeps running after a cancel: not the tool, not what it started.
        QTRY_VERIFY2_WITH_TIMEOUT(!fwtest::processIsRunning(grandchild), qPrintable(QString::number(grandchild)), 10000);
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
