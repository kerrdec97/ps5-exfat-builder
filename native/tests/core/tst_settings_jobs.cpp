// Settings (schema, validation, persistence, migration, overrides, legacy
// import) and the job queue (run, fail, cancel, pause, verification, history,
// persistence and restart recovery).

#include "core/Paths.h"
#include "core/Settings.h"
#include "engines/general/Module.h"
#include "jobs/JobHistory.h"
#include "jobs/JobQueue.h"
#include "jobs/QueueCompletionWatcher.h"
#include "support/TestRegistry.h"
#include "support/TestSupport.h"

#include <QFile>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QMutex>
#include <QTest>

using namespace fw;

namespace {

// A controllable job for queue tests.
class FakeJob : public Job {
public:
    explicit FakeJob(QJsonObject p) : m_p(std::move(p)) {}
    QString kind() const override { return QStringLiteral("test.fake"); }
    QString title() const override { return m_p.value(QStringLiteral("title")).toString(QStringLiteral("Fake")); }
    QStringList phases() const override { return {QStringLiteral("One"), QStringLiteral("Two")}; }
    QJsonObject parameters() const override { return m_p; }
    Status validate(const SettingsValues&) const override
    {
        if (m_p.value(QStringLiteral("invalid")).toBool())
            return Error(ErrorCode::InvalidArgument, QStringLiteral("Bad job"));
        return success();
    }
    Result<JobOutcome> run(JobContext& ctx) override
    {
        const int steps = m_p.value(QStringLiteral("steps")).toInt(10);
        const int stepMs = m_p.value(QStringLiteral("stepMs")).toInt(5);
        ctx.progress->beginPhase(0, steps);
        for (int i = 0; i < steps; ++i) {
            if (!ctx.token.checkpoint())
                return Error::cancelled();
            QThread::msleep(stepMs);
            ctx.progress->advance(1);
        }
        ctx.progress->beginPhase(1, 1);
        ctx.progress->advance(1);
        if (m_p.value(QStringLiteral("fail")).toBool())
            return Error(ErrorCode::IoError, QStringLiteral("Disk on fire"));
        if (const int failTimes = m_p.value(QStringLiteral("failTimes")).toInt(); failTimes > 0) {
            static QMap<QString, int> attempts;
            static QMutex m;
            QMutexLocker lock(&m);
            const QString key = m_p.value(QStringLiteral("title")).toString();
            if (++attempts[key] <= failTimes)
                return Error(ErrorCode::IoError, QStringLiteral("Transient failure"));
        }
        if (m_p.value(QStringLiteral("missingTool")).toBool())
            return Error(ErrorCode::DependencyMissing, QStringLiteral("Tool missing"));
        JobOutcome o;
        o.summary = QStringLiteral("ok");
        o.outputs << ctx.tempDir;
        o.verification = m_p.value(QStringLiteral("verifyFail")).toBool() ? Verification::Failed : Verification::Passed;
        o.verificationDetail = QStringLiteral("1 file differs");
        o.details.insert(QStringLiteral("threads"), int(ctx.settings.integer(general::keys::kWorkerThreads)));
        return o;
    }

private:
    QJsonObject m_p;
};

void addFakeKind(JobFactory& f)
{
    f.add(QStringLiteral("test.fake"), QStringLiteral("Fake"),
          [](const QJsonObject& p) -> Result<std::unique_ptr<Job>> { return std::unique_ptr<Job>(new FakeJob(p)); });
}

bool waitForState(JobQueue& q, const QString& id, JobState st, int ms = 10000)
{
    return QTest::qWaitFor([&] {
        auto r = q.record(id);
        return r && r->state == st;
    }, ms);
}

} // namespace

class TstSettingsJobs : public QObject {
    Q_OBJECT
    QTemporaryDir m_root;

private slots:
    void initTestCase() { QVERIFY(m_root.isValid()); fwtest::isolateAppData(m_root); }

    void everyRegisteredSettingIsWellFormed()
    {
        fwtest::Registry reg;
        QVERIFY(!reg.schema->all().empty());
        QSet<QString> keys;
        for (const auto* d : reg.schema->all()) {
            QVERIFY2(!keys.contains(d->key), qPrintable(d->key));
            keys.insert(d->key);
            QVERIFY2(!d->label.isEmpty(), qPrintable(d->key));
            QVERIFY2(SettingsSchema::sectionOrder().contains(d->section), qPrintable(d->key));
            // The default must pass the setting's own validation.
            auto n = reg.schema->normalize(d->key, d->defaultValue);
            QVERIFY2(n.ok(), qPrintable(d->key + QStringLiteral(": ") + (n.ok() ? QString() : n.error().message)));
            if (!d->enabledWhenKey.isEmpty())
                QVERIFY2(reg.schema->contains(d->enabledWhenKey), qPrintable(d->key));
            if (d->type == SettingType::Enum)
                QVERIFY2(!d->choices.isEmpty(), qPrintable(d->key));
            if (!d->dependencyId.isEmpty()) {
                const auto ids = reg.deps.ids();
                QVERIFY2(std::find(ids.begin(), ids.end(), d->dependencyId) != ids.end(),
                         qPrintable(d->key + QStringLiteral(" -> ") + d->dependencyId));
            }
        }
    }

    void normalizeValidatesTypesRangesAndChoices()
    {
        fwtest::Registry reg;
        const auto& s = *reg.schema;
        QCOMPARE(s.normalize(general::keys::kMaxConcurrentJobs, 3).value().toLongLong(), 3);
        QVERIFY(!s.normalize(general::keys::kMaxConcurrentJobs, 99).ok());
        QVERIFY(!s.normalize(general::keys::kMaxConcurrentJobs, 1.5).ok());
        QVERIFY(!s.normalize(general::keys::kMaxConcurrentJobs, QStringLiteral("abc")).ok());
        QCOMPARE(s.normalize(general::keys::kConfirmOnClose, QStringLiteral("off")).value().toBool(), false);
        QVERIFY(!s.normalize(general::keys::kConfirmOnClose, QStringLiteral("maybe")).ok());
        QVERIFY(s.normalize(general::keys::kLogLevel, QStringLiteral("debug")).ok());
        QVERIFY(!s.normalize(general::keys::kLogLevel, QStringLiteral("verbose")).ok());
        QVERIFY(!s.normalize(QStringLiteral("no.such.key"), 1).ok());
        QVERIFY(!s.normalize(general::keys::kDefaultOutputDir, m_root.filePath(QStringLiteral("missing"))).ok());
        QVERIFY(s.normalize(general::keys::kDefaultOutputDir, m_root.path()).ok());
    }

    void storePersistsOnlyNonDefaultsAndReloads()
    {
        fwtest::Registry reg;
        const QString path = m_root.filePath(QStringLiteral("s1/settings.json"));
        {
            SettingsStore store(reg.schema, path);
            QVERIFY(store.load().ok());
            QSignalSpy spy(&store, &SettingsStore::changed);
            QVERIFY(store.setValue(general::keys::kMaxConcurrentJobs, 2).ok());
            QVERIFY(store.setValue(general::keys::kLogLevel, QStringLiteral("info")).ok()); // default
            QCOMPARE(spy.count(), 2);
            QVERIFY(!store.setValue(general::keys::kMaxConcurrentJobs, 0).ok());
            QVERIFY(store.isDefault(general::keys::kLogLevel));
            QVERIFY(store.save().ok());
        }
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const auto doc = QJsonDocument::fromJson(f.readAll()).object();
        QCOMPARE(doc.value(QStringLiteral("schemaVersion")).toInt(), SettingsStore::kSchemaVersion);
        QCOMPARE(doc.value(QStringLiteral("values")).toObject().size(), 1);
        SettingsStore again(reg.schema, path);
        QVERIFY(again.load().ok());
        QCOMPARE(again.value(general::keys::kMaxConcurrentJobs).toInt(), 2);
        again.resetAll();
        QCOMPARE(again.value(general::keys::kMaxConcurrentJobs).toInt(), 1);
    }

    void corruptFileIsSetAsideAndDefaultsUsed()
    {
        fwtest::Registry reg;
        const QString path = m_root.filePath(QStringLiteral("s2/settings.json"));
        QVERIFY(fwtest::writeTextFile(path, "{ not json"));
        SettingsStore store(reg.schema, path);
        QVERIFY(store.load().ok());
        QVERIFY(!QFile::exists(path));
        QCOMPARE(store.loadWarnings().size(), 1);
        QCOMPARE(store.value(general::keys::kMaxConcurrentJobs).toInt(), 1);
    }

    void invalidAndUnknownEntriesAreDroppedWithWarnings()
    {
        fwtest::Registry reg;
        const QString path = m_root.filePath(QStringLiteral("s3/settings.json"));
        QVERIFY(fwtest::writeTextFile(path, R"({"schemaVersion":1,"values":{"performance.maxConcurrentJobs":500,"x.y":1,"logging.level":"debug"}})"));
        SettingsStore store(reg.schema, path);
        QVERIFY(store.load().ok());
        QCOMPARE(store.loadWarnings().size(), 2);
        QCOMPARE(store.value(general::keys::kMaxConcurrentJobs).toInt(), 1);
        QCOMPARE(store.value(general::keys::kLogLevel).toString(), QStringLiteral("debug"));
    }

    void folderOnMissingDriveIsKeptAtLoad()
    {
        fwtest::Registry reg;
        const QString path = m_root.filePath(QStringLiteral("s7/settings.json"));
        const QString gone = m_root.filePath(QStringLiteral("unplugged-drive/Images"));
        QVERIFY(fwtest::writeTextFile(path, QStringLiteral(R"({"schemaVersion":1,"values":{"storage.defaultOutputFolder":"%1"}})").arg(gone).toUtf8()));
        SettingsStore store(reg.schema, path);
        QVERIFY(store.load().ok());
        QCOMPARE(store.value(general::keys::kDefaultOutputDir).toString(), gone); // kept
        QVERIFY(store.loadWarnings().isEmpty());
        QVERIFY(!store.setValue(general::keys::kDefaultOutputDir, gone).ok()); // but cannot be chosen now
    }

    void legacyImportersSeeTheWholeFile()
    {
        fwtest::Registry reg;
        const QString legacy = m_root.filePath(QStringLiteral("legacy2.json"));
        const QString a = m_root.filePath(QStringLiteral("lib-a")), b = m_root.filePath(QStringLiteral("lib-b"));
        QDir().mkpath(a);
        QDir().mkpath(b);
        QVERIFY(fwtest::writeTextFile(legacy, QStringLiteral(R"({"bp_folders": ["%1"], "bp_scan_folder": "%2", "shutdown_action": "sleep", "retry_count": 9})").arg(a, b).toUtf8()));
        SettingsStore store(reg.schema, m_root.filePath(QStringLiteral("s8/settings.json")));
        QVERIFY(store.load().ok());
        auto n = store.importLegacy(legacy);
        QVERIFY(n.ok());
        QVERIFY(n.value() >= 3);
        QCOMPARE(store.value(QStringLiteral("backport.scanFolders")).toStringList(), (QStringList{a, b}));
        QCOMPARE(store.value(general::keys::kAfterQueueAction).toString(), QStringLiteral("sleep"));
        QCOMPARE(store.value(general::keys::kAutoRetryCount).toInt(), 5); // clamped
    }

    void migrationsRunFromOlderVersions()
    {
        fwtest::Registry reg;
        const QString path = m_root.filePath(QStringLiteral("s4/settings.json"));
        QVERIFY(fwtest::writeTextFile(path, R"({"schemaVersion":0,"values":{"oldJobs":3}})"));
        SettingsStore::registerMigration(0, [](QJsonObject& v) {
            v.insert(QStringLiteral("performance.maxConcurrentJobs"), v.take(QStringLiteral("oldJobs")));
        });
        SettingsStore store(reg.schema, path);
        QVERIFY(store.load().ok());
        QCOMPARE(store.value(general::keys::kMaxConcurrentJobs).toInt(), 3);
    }

    void snapshotsAreImmutableAndOverridesApply()
    {
        fwtest::Registry reg;
        SettingsStore store(reg.schema, m_root.filePath(QStringLiteral("s5/settings.json")));
        QVERIFY(store.load().ok());
        auto snap = store.snapshot({{general::keys::kWorkerThreads, 3}});
        QVERIFY(store.setValue(general::keys::kWorkerThreads, 5).ok());
        QCOMPARE(snap.integer(general::keys::kWorkerThreads), qint64(3));
        QVERIFY(snap.isOverridden(general::keys::kWorkerThreads));
        QCOMPARE(store.snapshot().integer(general::keys::kWorkerThreads), qint64(5));
        // Only perJob settings may be overridden.
        QVERIFY(!store.normalizeOverrides({{general::keys::kLogLevel, QStringLiteral("debug")}}).ok());
        QVERIFY(store.normalizeOverrides({{general::keys::kWorkerThreads, 2}}).ok());
        QVERIFY(!store.normalizeOverrides({{general::keys::kWorkerThreads, -1}}).ok());
    }

    void legacyImportMapsKnownKeys()
    {
        fwtest::Registry reg;
        reg.schema->addLegacyKey(QStringLiteral("threads"), general::keys::kWorkerThreads);
        const QString legacy = m_root.filePath(QStringLiteral("legacy.json"));
        QVERIFY(fwtest::writeTextFile(legacy, R"({"threads": 4, "unrelated": true})"));
        SettingsStore store(reg.schema, m_root.filePath(QStringLiteral("s6/settings.json")));
        QVERIFY(store.load().ok());
        auto n = store.importLegacy(legacy);
        QVERIFY(n.ok());
        QCOMPARE(n.value(), 1);
        QCOMPARE(store.value(general::keys::kWorkerThreads).toInt(), 4);
    }

    // ---- queue ------------------------------------------------------------

    void queueRunsJobsToCompletionWithHistory()
    {
        fwtest::Registry reg;
        addFakeKind(reg.jobs);
        SettingsStore store(reg.schema, m_root.filePath(QStringLiteral("q1/settings.json")));
        QVERIFY(store.load().ok());
        JobHistory history(m_root.filePath(QStringLiteral("q1/history.jsonl")));
        JobQueue q(store, reg.jobs);
        q.setHistory(&history);
        QSignalSpy finished(&q, &JobQueue::jobFinished);
        auto id1 = q.enqueue(QStringLiteral("test.fake"), {{QStringLiteral("title"), QStringLiteral("A")}},
                             {{general::keys::kWorkerThreads, 2}});
        auto id2 = q.enqueue(QStringLiteral("test.fake"), {{QStringLiteral("title"), QStringLiteral("B")}});
        QVERIFY(id1.ok() && id2.ok());
        QVERIFY(QTest::qWaitFor([&] { return finished.count() == 2; }, 10000));
        auto r1 = q.record(id1.value());
        QCOMPARE(r1->state, JobState::Succeeded);
        QCOMPARE(r1->outcome->details.value(QStringLiteral("threads")).toInt(), 2); // override reached the job
        QCOMPARE(r1->progress.overallFraction, 1.0);
        QVERIFY(!QFileInfo::exists(r1->outcome->outputs.first())); // job temp dir cleaned up
        const auto recent = history.recent(10);
        QCOMPARE(int(recent.size()), 2);
        QCOMPARE(recent.front().title, QStringLiteral("B"));
        QCOMPARE(recent.front().state, QStringLiteral("Done"));
    }

    void historyExportsCsv()
    {
        fwtest::Registry reg;
        addFakeKind(reg.jobs);
        SettingsStore store(reg.schema, m_root.filePath(QStringLiteral("csv/settings.json")));
        QVERIFY(store.load().ok());
        JobHistory history(m_root.filePath(QStringLiteral("csv/history.jsonl")));
        JobQueue q(store, reg.jobs);
        q.setHistory(&history);
        QSignalSpy finished(&q, &JobQueue::jobFinished);
        QVERIFY(q.enqueue(QStringLiteral("test.fake"), {{QStringLiteral("title"), QStringLiteral("Game, \"Deluxe\"")}}).ok());
        QVERIFY(q.enqueue(QStringLiteral("test.fake"), {{QStringLiteral("title"), QStringLiteral("Second")}, {QStringLiteral("fail"), true}}).ok());
        QVERIFY(QTest::qWaitFor([&] { return finished.count() == 2; }, 10000));
        const QString csv = m_root.filePath(QStringLiteral("csv/history.csv"));
        QVERIFY(history.exportCsv(csv).ok());
        QFile f(csv);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray all = f.readAll();
        QVERIFY(all.startsWith("\xEF\xBB\xBF" "finished,started,title"));
        QVERIFY(all.contains("\"Game, \"\"Deluxe\"\"\""));
        QVERIFY(all.contains("Second,test.fake,Failed"));
        QCOMPARE(all.count("\r\n"), 3);
    }

    void invalidJobIsRejectedAtEnqueue()
    {
        fwtest::Registry reg;
        addFakeKind(reg.jobs);
        SettingsStore store(reg.schema, m_root.filePath(QStringLiteral("q2/settings.json")));
        QVERIFY(store.load().ok());
        JobQueue q(store, reg.jobs);
        QVERIFY(!q.enqueue(QStringLiteral("test.fake"), {{QStringLiteral("invalid"), true}}).ok());
        QVERIFY(!q.enqueue(QStringLiteral("no.such.kind"), {}).ok());
        QVERIFY(!q.enqueue(QStringLiteral("test.fake"), {}, {{general::keys::kLogLevel, QStringLiteral("debug")}}).ok());
        QCOMPARE(int(q.records().size()), 0);
    }

    void failureAndVerificationFailureAreFailed()
    {
        fwtest::Registry reg;
        addFakeKind(reg.jobs);
        SettingsStore store(reg.schema, m_root.filePath(QStringLiteral("q3/settings.json")));
        QVERIFY(store.load().ok());
        JobQueue q(store, reg.jobs);
        auto a = q.enqueue(QStringLiteral("test.fake"), {{QStringLiteral("fail"), true}});
        auto b = q.enqueue(QStringLiteral("test.fake"), {{QStringLiteral("verifyFail"), true}});
        QVERIFY(waitForState(q, a.value(), JobState::Failed));
        QVERIFY(waitForState(q, b.value(), JobState::Failed));
        QCOMPARE(q.record(a.value())->error->code, ErrorCode::IoError);
        QCOMPARE(q.record(b.value())->error->code, ErrorCode::VerificationFailed);
        // Retry creates a new job from the same parameters.
        auto again = q.retry(a.value());
        QVERIFY(again.ok());
        QVERIFY(waitForState(q, again.value(), JobState::Failed));
    }

    void cancelRunningAndQueuedJobs()
    {
        fwtest::Registry reg;
        addFakeKind(reg.jobs);
        SettingsStore store(reg.schema, m_root.filePath(QStringLiteral("q4/settings.json")));
        QVERIFY(store.load().ok());
        JobQueue q(store, reg.jobs);
        auto slow = q.enqueue(QStringLiteral("test.fake"), {{QStringLiteral("steps"), 1000}, {QStringLiteral("stepMs"), 10}});
        auto waiting = q.enqueue(QStringLiteral("test.fake"), {});
        QVERIFY(waitForState(q, slow.value(), JobState::Running));
        QCOMPARE(q.record(waiting.value())->state, JobState::Queued);
        q.cancel(waiting.value());
        QCOMPARE(q.record(waiting.value())->state, JobState::Cancelled);
        q.cancel(slow.value());
        QVERIFY(waitForState(q, slow.value(), JobState::Cancelled, 5000));
    }

    void pauseAndResume()
    {
        fwtest::Registry reg;
        addFakeKind(reg.jobs);
        SettingsStore store(reg.schema, m_root.filePath(QStringLiteral("q5/settings.json")));
        QVERIFY(store.load().ok());
        JobQueue q(store, reg.jobs);
        auto id = q.enqueue(QStringLiteral("test.fake"), {{QStringLiteral("steps"), 100}, {QStringLiteral("stepMs"), 10}});
        QVERIFY(waitForState(q, id.value(), JobState::Running));
        q.pauseJob(id.value());
        QCOMPARE(q.record(id.value())->state, JobState::Paused);
        QTest::qWait(150);
        const qint64 d1 = q.record(id.value())->progress.done;
        QTest::qWait(200);
        const qint64 d2 = q.record(id.value())->progress.done;
        QVERIFY(d2 - d1 <= 1);
        q.resumeJob(id.value());
        QVERIFY(waitForState(q, id.value(), JobState::Succeeded));
    }

    void concurrencyLimitIsRespected()
    {
        fwtest::Registry reg;
        addFakeKind(reg.jobs);
        SettingsStore store(reg.schema, m_root.filePath(QStringLiteral("q6/settings.json")));
        QVERIFY(store.load().ok());
        QVERIFY(store.setValue(general::keys::kMaxConcurrentJobs, 2).ok());
        JobQueue q(store, reg.jobs);
        int maxSeen = 0;
        connect(&q, &JobQueue::jobChanged, this, [&] { maxSeen = std::max(maxSeen, q.runningCount()); });
        QSignalSpy finished(&q, &JobQueue::jobFinished);
        for (int i = 0; i < 5; ++i)
            QVERIFY(q.enqueue(QStringLiteral("test.fake"), {{QStringLiteral("steps"), 20}}).ok());
        QVERIFY(QTest::qWaitFor([&] { return finished.count() == 5; }, 15000));
        QCOMPARE(maxSeen, 2);
    }

    void reorderAndRemove()
    {
        fwtest::Registry reg;
        addFakeKind(reg.jobs);
        SettingsStore store(reg.schema, m_root.filePath(QStringLiteral("q7/settings.json")));
        QVERIFY(store.load().ok());
        JobQueue q(store, reg.jobs);
        q.holdQueue(true);
        auto a = q.enqueue(QStringLiteral("test.fake"), {{QStringLiteral("title"), QStringLiteral("A")}});
        auto b = q.enqueue(QStringLiteral("test.fake"), {{QStringLiteral("title"), QStringLiteral("B")}});
        QVERIFY(q.move(b.value(), 0));
        QCOMPARE(q.records().front().title, QStringLiteral("B"));
        QVERIFY(q.remove(a.value()));
        QCOMPARE(int(q.records().size()), 1);
        q.holdQueue(false);
        QVERIFY(waitForState(q, b.value(), JobState::Succeeded));
    }

    void transientFailuresAreRetriedAutomatically()
    {
        fwtest::Registry reg;
        addFakeKind(reg.jobs);
        SettingsStore store(reg.schema, m_root.filePath(QStringLiteral("q9/settings.json")));
        QVERIFY(store.load().ok());
        QVERIFY(store.setValue(general::keys::kAutoRetryCount, 2).ok());
        QVERIFY(store.setValue(general::keys::kAutoRetryDelay, 0).ok());
        JobQueue q(store, reg.jobs);
        auto ok = q.enqueue(QStringLiteral("test.fake"), {{QStringLiteral("title"), QStringLiteral("R1")}, {QStringLiteral("failTimes"), 2}});
        QVERIFY(waitForState(q, ok.value(), JobState::Succeeded));
        QCOMPARE(q.record(ok.value())->attempt, 3);
        auto gaveUp = q.enqueue(QStringLiteral("test.fake"), {{QStringLiteral("title"), QStringLiteral("R2")}, {QStringLiteral("failTimes"), 5}});
        QVERIFY(waitForState(q, gaveUp.value(), JobState::Failed));
        QCOMPARE(q.record(gaveUp.value())->attempt, 3);
        // Non-transient errors are never retried.
        auto missing = q.enqueue(QStringLiteral("test.fake"), {{QStringLiteral("missingTool"), true}});
        QVERIFY(waitForState(q, missing.value(), JobState::Failed));
        QCOMPARE(q.record(missing.value())->attempt, 1);
    }

    void completionWatcherRequestsPowerAction()
    {
        fwtest::Registry reg;
        addFakeKind(reg.jobs);
        SettingsStore store(reg.schema, m_root.filePath(QStringLiteral("q10/settings.json")));
        QVERIFY(store.load().ok());
        JobQueue q(store, reg.jobs);
        QueueCompletionWatcher w(q, store);
        QSignalSpy due(&w, &QueueCompletionWatcher::actionDue);
        QSignalSpy finished(&w, &QueueCompletionWatcher::queueFinished);
        // Action "none": finished but no action.
        QVERIFY(q.enqueue(QStringLiteral("test.fake"), {}).ok());
        QVERIFY(QTest::qWaitFor([&] { return finished.count() == 1; }, 5000));
        QCOMPARE(due.count(), 0);
        // Shut down after success.
        QVERIFY(store.setValue(general::keys::kAfterQueueAction, QStringLiteral("shutdown")).ok());
        QVERIFY(q.enqueue(QStringLiteral("test.fake"), {}).ok());
        QVERIFY(QTest::qWaitFor([&] { return due.count() == 1; }, 5000));
        QCOMPARE(due.first().at(0).value<power::Action>(), power::Action::Shutdown);
        QCOMPARE(due.first().at(1).toInt(), 60);
        // A failed job blocks the action when "only if all succeeded" is on.
        QVERIFY(q.enqueue(QStringLiteral("test.fake"), {{QStringLiteral("fail"), true}}).ok());
        QVERIFY(QTest::qWaitFor([&] { return finished.count() == 3; }, 5000));
        QCOMPARE(due.count(), 1);
    }

    void deleteSourceOnlyAfterFullVerification()
    {
        fwtest::Registry reg;
        SettingsStore store(reg.schema, m_root.filePath(QStringLiteral("ds/settings.json")));
        QVERIFY(store.load().ok());
        const QString src = m_root.filePath(QStringLiteral("ds/dump"));
        QVERIFY(fwtest::writeTextFile(src + QStringLiteral("/eboot.bin"), "x"));
        JobContext ctx;
        ctx.log = Log(QStringLiteral("test"));
        JobOutcome o;
        o.outputs << m_root.filePath(QStringLiteral("ds/out.exfat"));
        o.verification = Verification::Passed;
        // Off by default.
        ctx.settings = store.snapshot();
        QVERIFY(!general::deleteSourceAfterVerifiedBuild(ctx, src, o));
        QVERIFY(QFileInfo::exists(src));
        // On, but structure-only verification: refused with a warning.
        ctx.settings = store.snapshot({{general::keys::kDeleteSource, true}, {general::keys::kVerifyMode, QStringLiteral("structure")}});
        QVERIFY(!general::deleteSourceAfterVerifiedBuild(ctx, src, o));
        QCOMPARE(o.warnings.size(), 1);
        // Output inside the source: refused.
        ctx.settings = store.snapshot({{general::keys::kDeleteSource, true}});
        JobOutcome inside = o;
        inside.outputs = {src + QStringLiteral("/out.exfat")};
        QVERIFY(!general::deleteSourceAfterVerifiedBuild(ctx, src, inside));
        // Verification not passed: refused.
        JobOutcome unverified = o;
        unverified.verification = Verification::Skipped;
        QVERIFY(!general::deleteSourceAfterVerifiedBuild(ctx, src, unverified));
        QVERIFY(QFileInfo::exists(src));
        // Everything right: deleted.
        QVERIFY(general::deleteSourceAfterVerifiedBuild(ctx, src, o));
        QVERIFY(!QFileInfo::exists(src));
    }

    void restartRecoveryRestoresPendingAndMarksInterrupted()
    {
        fwtest::Registry reg;
        addFakeKind(reg.jobs);
        const QString queueFile = m_root.filePath(QStringLiteral("q8/queue.json"));
        SettingsStore store(reg.schema, m_root.filePath(QStringLiteral("q8/settings.json")));
        QVERIFY(store.load().ok());
        {
            JobQueue q(store, reg.jobs);
            q.setPersistencePath(queueFile);
            auto running = q.enqueue(QStringLiteral("test.fake"), {{QStringLiteral("title"), QStringLiteral("Running")},
                                                                    {QStringLiteral("steps"), 1000}, {QStringLiteral("stepMs"), 10}});
            QVERIFY(q.enqueue(QStringLiteral("test.fake"), {{QStringLiteral("title"), QStringLiteral("Waiting")}}).ok());
            QVERIFY(waitForState(q, running.value(), JobState::Running));
            QVERIFY(q.shutdown(5000)); // simulates closing the app mid-job
        }
        JobQueue q2(store, reg.jobs);
        q2.setPersistencePath(queueFile);
        q2.holdQueue(true);
        QVERIFY(q2.restoreQueue().ok());
        const auto recs = q2.records();
        QCOMPARE(int(recs.size()), 2);
        QCOMPARE(recs[0].title, QStringLiteral("Running"));
        QCOMPARE(recs[0].state, JobState::Interrupted);
        QCOMPARE(recs[1].title, QStringLiteral("Waiting"));
        QCOMPARE(recs[1].state, JobState::Queued);
        q2.holdQueue(false);
        QVERIFY(waitForState(q2, recs[1].id, JobState::Succeeded));
    }
};

QTEST_GUILESS_MAIN(TstSettingsJobs)
#include "tst_settings_jobs.moc"
