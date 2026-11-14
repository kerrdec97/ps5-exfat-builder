// Settings (schema, validation, persistence, migration, overrides, legacy
// import) and the job queue (run, fail, cancel, pause, verification, history,
// persistence and restart recovery).

#include "core/FileUtil.h"
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
#include <QJsonArray>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QTest>

#include <atomic>
#include <cmath>

using namespace fw;

namespace {

// A controllable job for queue tests.
class FakeJob : public Job {
public:
    explicit FakeJob(QJsonObject p) : m_p(std::move(p)) {}
    QString kind() const override { return QStringLiteral("test.fake"); }
    QString title() const override { return m_p.value(QStringLiteral("title")).toString(QStringLiteral("Fake")); }
    QStringList phases() const override { return {QStringLiteral("One"), QStringLiteral("Two")}; }
    // Three worker threads give the first phase three quarters of the bar.
    using Job::phaseWeights;
    QList<double> phaseWeights(const SettingsValues& s) const override
    {
        return s.integer(general::keys::kWorkerThreads) == 3 ? QList<double>{3, 1} : phaseWeights();
    }
    QJsonObject parameters() const override { return m_p; }
    bool runsWhileHeld() const override { return m_p.value(QStringLiteral("system")).toBool(); }
    Status validate(const SettingsValues&) const override
    {
        if (m_p.value(QStringLiteral("invalid")).toBool())
            return Error(ErrorCode::InvalidArgument, QStringLiteral("Bad job"));
        return success();
    }
    // The titles of the jobs in the order they began to run.
    static QStringList& runOrder()
    {
        static QStringList order;
        return order;
    }
    static QMutex& runOrderMutex()
    {
        static QMutex m;
        return m;
    }
    // What the last cancelled job saw: 1 the app closing, 0 a Cancel, -1 none yet.
    static std::atomic<int>& cancelledByShutdown()
    {
        static std::atomic<int> seen{-1};
        return seen;
    }
    Result<JobOutcome> run(JobContext& ctx) override
    {
        {
            QMutexLocker lock(&runOrderMutex());
            runOrder() << title();
        }
        const int steps = m_p.value(QStringLiteral("steps")).toInt(10);
        const int stepMs = m_p.value(QStringLiteral("stepMs")).toInt(5);
        ctx.progress->beginPhase(0, steps);
        for (int i = 0; i < steps; ++i) {
            if (!ctx.token.checkpoint()) {
                cancelledByShutdown() = ctx.token.isShutdown() ? 1 : 0;
                return m_p.value(QStringLiteral("keepAfterCancel")).toBool() ? afterCancel(ctx) : Error::cancelled();
            }
            QThread::msleep(stepMs);
            ctx.progress->advance(1);
        }
        if (m_p.value(QStringLiteral("failAfterFirst")).toBool())
            return Error(ErrorCode::IoError, QStringLiteral("Disk on fire"));
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
    // As a job whose verification a cancel stopped (GitHub #97): it keeps its
    // finished work when it may, runs a last check of "checkMs" on a token
    // only closing the app stops, writes "keptFile" for the test to see, and
    // then fails as a kept image fails that cannot take its name.
    Result<JobOutcome> afterCancel(JobContext& ctx)
    {
        if (!ctx.token.keepFinishedWork())
            return Error(ErrorCode::IoError, QStringLiteral("A read was cut short by the cancel")); // the cancel's own doing
        if (const int ms = m_p.value(QStringLiteral("checkMs")).toInt(); ms > 0 && !ctx.token.untilShutdown().sleepFor(ms))
            return Error::cancelled();
        if (const QString kept = m_p.value(QStringLiteral("keptFile")).toString(); !kept.isEmpty())
            (void)writeFileAtomic(kept, "kept");
        return Error(ErrorCode::PermissionDenied, QStringLiteral("The image was built, but could not replace out.bin because that file "
                                                                 "is in use. It was saved as out (new).bin."));
    }

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
        // No module registers an importer today, so the test brings its own:
        // it needs two legacy keys at once to produce one value.
        int calls = 0;
        reg.schema->addLegacyImporter([&calls, a](const QJsonObject& file) {
            ++calls;
            QVariantMap out;
            const QJsonArray folders = file.value(QStringLiteral("bp_folders")).toArray();
            if (folders.size() == 1 && folders.at(0).toString() == a)
                out.insert(general::keys::kDefaultOutputDir, file.value(QStringLiteral("bp_scan_folder")).toString());
            return out;
        });
        SettingsStore store(reg.schema, m_root.filePath(QStringLiteral("s8/settings.json")));
        QVERIFY(store.load().ok());
        auto n = store.importLegacy(legacy);
        QVERIFY(n.ok());
        QCOMPARE(calls, 1);
        QCOMPARE(n.value(), 2); // one mapped key (shutdown_action is never taken over) and the importer's one
        QCOMPARE(QDir::cleanPath(store.value(general::keys::kDefaultOutputDir).toString()), QDir::cleanPath(b));
        QCOMPARE(store.value(general::keys::kAfterQueueAction).toString(), QStringLiteral("none"));
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

    // A list of folders is joined on import: the folders already there stay
    // first, the imported ones that are new (compared as paths) follow.
    void legacyImportJoinsAFolderList()
    {
        fwtest::Registry reg;
        const QString key = QStringLiteral("library.folders");
        const SettingDef* def = reg.schema->find(key);
        QVERIFY(def && def->folderList);
        const QString legacy = m_root.filePath(QStringLiteral("legacy-folders.json"));
        QVERIFY(fwtest::writeTextFile(legacy, R"({"lib_folders": ["D:\\PS5\\Games", "g:/games/", "  ", "E:\\More"]})"));
        SettingsStore store(reg.schema, m_root.filePath(QStringLiteral("s9/settings.json")));
        QVERIFY(store.load().ok());
        QVERIFY(store.setValue(key, QStringList{QStringLiteral("G:\\Games")}).ok());
        auto n = store.importLegacy(legacy);
        QVERIFY(n.ok());
        QCOMPARE(n.value(), 1);
        QCOMPARE(store.value(key).toStringList(),
                 (QStringList{QStringLiteral("G:\\Games"), QStringLiteral("D:\\PS5\\Games"), QStringLiteral("E:\\More")}));
    }

    // Reset section and Reset all leave a setting that is the user's own
    // list (keptOnReset); reset() of that key still resets it.
    void resetLeavesSettingsKeptOnReset()
    {
        fwtest::Registry reg;
        const QString key = QStringLiteral("library.folders");
        const SettingDef* def = reg.schema->find(key);
        QVERIFY(def && def->keptOnReset);
        SettingsStore store(reg.schema, m_root.filePath(QStringLiteral("s10/settings.json")));
        QVERIFY(store.load().ok());
        QVERIFY(store.setValue(key, QStringList{QStringLiteral("D:\\Games")}).ok());
        QVERIFY(store.setValue(general::keys::kMaxConcurrentJobs, 2).ok());
        QSignalSpy spy(&store, &SettingsStore::changed);
        store.resetSection(def->section);
        store.resetAll();
        QCOMPARE(store.value(key).toStringList(), QStringList{QStringLiteral("D:\\Games")});
        QVERIFY(store.isDefault(general::keys::kMaxConcurrentJobs));
        for (const auto& call : spy)
            QVERIFY(call.at(0).toString() != key);
        store.reset(key);
        QVERIFY(store.isDefault(key));
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

    // A job's temporary folder in the folder chosen in Settings has the name
    // and the marker the sweep asks for; the user's own folders there do not.
    void jobTempFolderInACustomFolderIsRecognisedByTheSweep()
    {
        fwtest::Registry reg;
        addFakeKind(reg.jobs);
        const QString custom = m_root.filePath(QStringLiteral("q12/user temp"));
        QVERIFY(fwtest::writeTextFile(custom + QStringLiteral("/fw-11.00/PS5UPDATE.PUP"), "mine"));
        SettingsStore store(reg.schema, m_root.filePath(QStringLiteral("q12/settings.json")));
        QVERIFY(store.load().ok());
        QVERIFY(store.setValue(general::keys::kTempFolder, custom).ok());
        QVERIFY(store.setValue(general::keys::kKeepJobTemp, true).ok());
        JobQueue q(store, reg.jobs);
        auto id = q.enqueue(QStringLiteral("test.fake"), {});
        QVERIFY(id.ok());
        QVERIFY(waitForState(q, id.value(), JobState::Succeeded));
        const QString kept = q.record(id.value())->outcome->outputs.first(); // the job's temp folder
        QVERIFY(QFileInfo(kept).isDir());
        QVERIFY(isSameOrInside(kept, custom));
        QVERIFY(TempDir::isOwnedFolder(kept));
        QCOMPARE(Paths::sweepStaleTempIn(custom, 0), 1);
        QVERIFY(!QFileInfo::exists(kept));
        QVERIFY(QFileInfo::exists(custom + QStringLiteral("/fw-11.00/PS5UPDATE.PUP")));
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
        // A title from somebody else's dump that a spreadsheet would run as a formula.
        QVERIFY(q.enqueue(QStringLiteral("test.fake"), {{QStringLiteral("title"), QStringLiteral("=HYPERLINK(\"http://x\",\"Astro\")")}}).ok());
        QVERIFY(q.enqueue(QStringLiteral("test.fake"), {{QStringLiteral("title"), QStringLiteral("@SUM(1)")}}).ok());
        QVERIFY(QTest::qWaitFor([&] { return finished.count() == 4; }, 10000));
        const QString csv = m_root.filePath(QStringLiteral("csv/history.csv"));
        QVERIFY(history.exportCsv(csv).ok());
        QFile f(csv);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray all = f.readAll();
        QVERIFY(all.startsWith("\xEF\xBB\xBF" "finished,started,title"));
        QVERIFY(all.contains("\"Game, \"\"Deluxe\"\"\""));
        QVERIFY(all.contains("Second,test.fake,Failed"));
        QVERIFY(all.contains(",\"'=HYPERLINK(\"\"http://x\"\",\"\"Astro\"\")\",test.fake,"));
        QVERIFY(all.contains(",'@SUM(1),test.fake,"));
        QCOMPARE(all.count("\r\n"), 5);
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
        // A job that failed its verification is not complete: no 100 %.
        const double failedAt = q.record(b.value())->progress.overallFraction;
        QVERIFY2(failedAt > 0.9 && failedAt < 1.0, qPrintable(QString::number(failedAt)));
        // Retry creates a new job from the same parameters.
        auto again = q.retry(a.value());
        QVERIFY(again.ok());
        QVERIFY(waitForState(q, again.value(), JobState::Failed));
    }

    // The queue declares a job's stage shares for the settings the job runs
    // with, its own overrides included, before the job begins.
    void queueDeclaresTheSharesForTheJobsSettings()
    {
        fwtest::Registry reg;
        addFakeKind(reg.jobs);
        SettingsStore store(reg.schema, m_root.filePath(QStringLiteral("q13/settings.json")));
        QVERIFY(store.load().ok());
        JobQueue q(store, reg.jobs);
        auto weighted = q.enqueue(QStringLiteral("test.fake"), {{QStringLiteral("failAfterFirst"), true}},
                                  {{general::keys::kWorkerThreads, 3}});
        auto plain = q.enqueue(QStringLiteral("test.fake"), {{QStringLiteral("failAfterFirst"), true}});
        QVERIFY(weighted.ok() && plain.ok());
        QVERIFY(waitForState(q, weighted.value(), JobState::Failed));
        QVERIFY(waitForState(q, plain.value(), JobState::Failed));
        // Both stopped at the end of their first phase.
        const double w = q.record(weighted.value())->progress.overallFraction;
        const double p = q.record(plain.value())->progress.overallFraction;
        QVERIFY2(std::abs(w - 0.75) < 0.01, qPrintable(QString::number(w)));
        QVERIFY2(std::abs(p - 0.5) < 0.01, qPrintable(QString::number(p)));
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

    // A job that keeps its finished work after a cancel (GitHub #97) and then
    // cannot give it its name: the queue reports that error, which says where
    // the work is, instead of a plain "Cancelled".
    void aKeptJobsLaterErrorIsReported()
    {
        fwtest::Registry reg;
        addFakeKind(reg.jobs);
        SettingsStore store(reg.schema, m_root.filePath(QStringLiteral("q4k/settings.json")));
        QVERIFY(store.load().ok());
        JobQueue q(store, reg.jobs);
        auto kept = q.enqueue(QStringLiteral("test.fake"), {{QStringLiteral("steps"), 1000}, {QStringLiteral("stepMs"), 10},
                                                            {QStringLiteral("keepAfterCancel"), true}});
        QVERIFY(kept.ok());
        QVERIFY(waitForState(q, kept.value(), JobState::Running));
        q.cancel(kept.value());
        QVERIFY(waitForState(q, kept.value(), JobState::Failed, 5000));
        const auto rec = q.record(kept.value());
        QVERIFY(rec && rec->error);
        QCOMPARE(rec->error->code, ErrorCode::PermissionDenied);
        QVERIFY2(rec->error->message.contains(QStringLiteral("out (new).bin")), qPrintable(rec->error->message));
    }

    // Closing the app keeps nothing a job has not finished: a job that would
    // keep its work after a Cancel does not, and one the user cancelled that
    // runs its last check on the work it keeps is stopped there, so the app
    // does not wait for that check to end.
    void closingTheAppKeepsNothingUnfinished()
    {
        fwtest::Registry reg;
        addFakeKind(reg.jobs);
        SettingsStore store(reg.schema, m_root.filePath(QStringLiteral("q4s/settings.json")));
        QVERIFY(store.load().ok());
        const QString keptA = m_root.filePath(QStringLiteral("q4s/a.kept")), keptB = m_root.filePath(QStringLiteral("q4s/b.kept"));
        {
            JobQueue q(store, reg.jobs);
            auto a = q.enqueue(QStringLiteral("test.fake"), {{QStringLiteral("steps"), 1000}, {QStringLiteral("stepMs"), 10},
                                                             {QStringLiteral("keepAfterCancel"), true}, {QStringLiteral("keptFile"), keptA}});
            QVERIFY(a.ok());
            QVERIFY(waitForState(q, a.value(), JobState::Running));
            QVERIFY(q.shutdown(5000));
        }
        QVERIFY(!QFileInfo::exists(keptA));
        {
            JobQueue q(store, reg.jobs);
            auto b = q.enqueue(QStringLiteral("test.fake"), {{QStringLiteral("steps"), 1000}, {QStringLiteral("stepMs"), 10},
                                                             {QStringLiteral("keepAfterCancel"), true}, {QStringLiteral("checkMs"), 60000},
                                                             {QStringLiteral("keptFile"), keptB}});
            QVERIFY(b.ok());
            QVERIFY(waitForState(q, b.value(), JobState::Running));
            q.cancel(b.value());
            QCOMPARE(q.record(b.value())->state, JobState::Cancelling);
            QTest::qWait(200); // the job is in its last check
            QElapsedTimer closing;
            closing.start();
            QVERIFY(q.shutdown(20000));
            QVERIFY2(closing.elapsed() < 10000, qPrintable(QString::number(closing.elapsed())));
        }
        QVERIFY(!QFileInfo::exists(keptB));
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

    void busyPathsOverlapRules()
    {
        const QString a = QDir::cleanPath(m_root.filePath(QStringLiteral("x/game")));
        QVERIFY(busyPathsOverlap({a}, {a}));
        QVERIFY(busyPathsOverlap({a}, {a + QStringLiteral("/sce_sys/param.json")}));
        QVERIFY(busyPathsOverlap({a + QStringLiteral("/eboot.bin")}, {a}));
        QVERIFY(!busyPathsOverlap({a}, {a + QStringLiteral("2")})); // "game2" is not inside "game"
        QVERIFY(!busyPathsOverlap({}, {a}));
#ifdef Q_OS_WIN
        QVERIFY(busyPathsOverlap({a}, {a.toUpper()}));
#endif
        FakeJob j(QJsonObject{{QStringLiteral("image"), QStringLiteral("rel/x.exfat")}, {QStringLiteral("steps"), 3}});
        QCOMPARE(j.busyPaths(), QStringList{QDir::cleanPath(QDir::current().absoluteFilePath(QStringLiteral("rel/x.exfat")))});
    }

    // Two jobs on the same image never run at the same time, even when the
    // concurrency limit would allow it; a job on another image runs alongside.
    void jobsOnTheSamePathNeverRunTogether()
    {
        fwtest::Registry reg;
        addFakeKind(reg.jobs);
        SettingsStore store(reg.schema, m_root.filePath(QStringLiteral("q9/settings.json")));
        QVERIFY(store.load().ok());
        QVERIFY(store.setValue(general::keys::kMaxConcurrentJobs, 3).ok());
        JobQueue q(store, reg.jobs);
        const QString img = m_root.filePath(QStringLiteral("q9/game.exfat"));
        const QString other = m_root.filePath(QStringLiteral("q9/other.exfat"));
        auto a = q.enqueue(QStringLiteral("test.fake"), {{QStringLiteral("image"), img}, {QStringLiteral("steps"), 30}});
        auto b = q.enqueue(QStringLiteral("test.fake"), {{QStringLiteral("image"), img}, {QStringLiteral("steps"), 30}});
        auto c = q.enqueue(QStringLiteral("test.fake"), {{QStringLiteral("image"), other}, {QStringLiteral("steps"), 30}});
        QVERIFY(a.ok() && b.ok() && c.ok());
        bool together = false;
        int maxSeen = 0;
        connect(&q, &JobQueue::jobChanged, this, [&] {
            const auto ra = q.record(a.value()), rb = q.record(b.value());
            if (ra && rb && ra->state == JobState::Running && rb->state == JobState::Running)
                together = true;
            maxSeen = std::max(maxSeen, q.runningCount());
        });
        QSignalSpy finished(&q, &JobQueue::jobFinished);
        QVERIFY(QTest::qWaitFor([&] { return finished.count() == 3; }, 15000));
        QVERIFY(!together);
        QCOMPARE(maxSeen, 2);
        QCOMPARE(q.record(b.value())->state, JobState::Succeeded);
    }

    void failedJobKeepsItsProgress()
    {
        fwtest::Registry reg;
        addFakeKind(reg.jobs);
        SettingsStore store(reg.schema, m_root.filePath(QStringLiteral("q10/settings.json")));
        QVERIFY(store.load().ok());
        QVERIFY(store.setValue(general::keys::kAutoRetryCount, 0).ok());
        JobQueue q(store, reg.jobs);
        auto id = q.enqueue(QStringLiteral("test.fake"), {{QStringLiteral("fail"), true}, {QStringLiteral("steps"), 4}});
        QVERIFY(id.ok());
        QVERIFY(waitForState(q, id.value(), JobState::Failed));
        QVERIFY2(q.record(id.value())->progress.overallFraction > 0.4,
                 qPrintable(QString::number(q.record(id.value())->progress.overallFraction)));
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

        // A job cancelled while it was still waiting never ran: it does not
        // count as a failure for that option.
        q.holdQueue(true);
        auto runs = q.enqueue(QStringLiteral("test.fake"), {});
        auto waits = q.enqueue(QStringLiteral("test.fake"), {});
        QVERIFY(runs.ok() && waits.ok());
        q.cancel(waits.value());
        QVERIFY(waitForState(q, waits.value(), JobState::Cancelled));
        QVERIFY(!q.record(waits.value())->started.isValid());
        q.holdQueue(false);
        QVERIFY(QTest::qWaitFor([&] { return finished.count() == 4; }, 5000));
        QCOMPARE(finished.last().at(2).toInt(), 1); // still reported as cancelled
        QCOMPARE(due.count(), 2);

        // One cancelled after it had started does.
        auto slow = q.enqueue(QStringLiteral("test.fake"), {{QStringLiteral("steps"), 400}, {QStringLiteral("stepMs"), 20}});
        auto after = q.enqueue(QStringLiteral("test.fake"), {});
        QVERIFY(slow.ok() && after.ok());
        QVERIFY(waitForState(q, slow.value(), JobState::Running));
        q.cancel(slow.value());
        QVERIFY(QTest::qWaitFor([&] { return finished.count() == 5; }, 10000));
        QVERIFY(q.record(slow.value())->started.isValid());
        QCOMPARE(due.count(), 2);

        // With the option off the action runs after failures too, even when
        // every job failed.
        QVERIFY(store.setValue(general::keys::kAfterQueueOnlyIfOk, false).ok());
        QVERIFY(q.enqueue(QStringLiteral("test.fake"), {{QStringLiteral("fail"), true}}).ok());
        QVERIFY(QTest::qWaitFor([&] { return finished.count() == 6; }, 5000));
        QCOMPARE(due.count(), 3);
        QCOMPARE(due.last().at(2).toInt(), 0); // none succeeded
        QCOMPARE(due.last().at(3).toInt(), 1);
        // A run cancelled in full did nothing worth a shutdown.
        q.holdQueue(true);
        auto only = q.enqueue(QStringLiteral("test.fake"), {});
        QVERIFY(only.ok());
        q.cancel(only.value());
        QVERIFY(waitForState(q, only.value(), JobState::Cancelled));
        q.holdQueue(false);
        QTest::qWait(300);
        QCOMPARE(due.count(), 3);
        QCOMPARE(finished.count(), 6);
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
        QVERIFY(q2.restoreQueue().ok()); // restored jobs wait for the user without an explicit hold
        const auto recs = q2.records();
        QCOMPARE(int(recs.size()), 2);
        QCOMPARE(recs[0].title, QStringLiteral("Running"));
        QCOMPARE(recs[0].state, JobState::Interrupted);
        // After a crash nobody removed the job's .part file: the text must not claim otherwise.
        QVERIFY2(recs[0].error->message.contains(QStringLiteral(".part file) may still be")), qPrintable(recs[0].error->message));
        QCOMPARE(recs[1].title, QStringLiteral("Waiting"));
        QCOMPARE(recs[1].state, JobState::Queued);
        QTest::qWait(200);
        QCOMPARE(q2.record(recs[1].id)->state, JobState::Queued);
        q2.holdQueue(false);
        QVERIFY(waitForState(q2, recs[1].id, JobState::Succeeded));
    }

    // A running job learns why it was stopped: by the user's Cancel, or by
    // the app closing. Only a Cancel keeps work that was being verified; when
    // the app closes the queue cannot record what the job does any more, so
    // the job discards it and comes back as interrupted.
    void closingTheAppIsToldApartFromACancel()
    {
        fwtest::Registry reg;
        addFakeKind(reg.jobs);
        SettingsStore store(reg.schema, m_root.filePath(QStringLiteral("q14/settings.json")));
        QVERIFY(store.load().ok());
        const QJsonObject longJob{{QStringLiteral("steps"), 1000}, {QStringLiteral("stepMs"), 10}};
        JobQueue q(store, reg.jobs);
        FakeJob::cancelledByShutdown() = -1;
        auto cancelled = q.enqueue(QStringLiteral("test.fake"), longJob);
        QVERIFY(cancelled.ok());
        QVERIFY(waitForState(q, cancelled.value(), JobState::Running));
        q.cancel(cancelled.value());
        QVERIFY(waitForState(q, cancelled.value(), JobState::Cancelled));
        QCOMPARE(FakeJob::cancelledByShutdown().load(), 0);
        auto closed = q.enqueue(QStringLiteral("test.fake"), longJob);
        QVERIFY(closed.ok());
        QVERIFY(waitForState(q, closed.value(), JobState::Running));
        QVERIFY(q.shutdown(5000));
        QCOMPARE(FakeJob::cancelledByShutdown().load(), 1);
    }

    // A waiting job that no longer validates (its drive is unplugged) is kept
    // as Interrupted with the reason, not dropped from the queue.
    void restoredJobThatCannotRunIsKept()
    {
        fwtest::Registry reg;
        addFakeKind(reg.jobs);
        const QString queueFile = m_root.filePath(QStringLiteral("q11/queue.json"));
        QVERIFY(QDir().mkpath(QFileInfo(queueFile).absolutePath()));
        {
            QFile f(queueFile);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(QJsonDocument(QJsonObject{{QStringLiteral("jobs"), QJsonArray{QJsonObject{
                {QStringLiteral("kind"), QStringLiteral("test.fake")},
                {QStringLiteral("state"), jobStateName(JobState::Queued)},
                {QStringLiteral("params"), QJsonObject{{QStringLiteral("title"), QStringLiteral("Unplugged")},
                                                       {QStringLiteral("invalid"), true}}}}}}})
                        .toJson());
        }
        SettingsStore store(reg.schema, m_root.filePath(QStringLiteral("q11/settings.json")));
        QVERIFY(store.load().ok());
        JobQueue q(store, reg.jobs);
        q.setPersistencePath(queueFile);
        QVERIFY(q.restoreQueue().ok());
        const auto recs = q.records();
        QCOMPARE(int(recs.size()), 1);
        QCOMPARE(recs[0].title, QStringLiteral("Unplugged"));
        QCOMPARE(recs[0].state, JobState::Interrupted);
        QVERIFY(recs[0].error.has_value());
        QVERIFY2(recs[0].error->message.contains(QStringLiteral("Bad job")), qPrintable(recs[0].error->message));
    }

    // A queue file that cannot be read is reported and set aside, not
    // silently replaced by an empty queue at the next save. One set aside
    // earlier is kept too: a second damaged file gets a name of its own.
    void damagedQueueFileIsSetAside()
    {
        fwtest::Registry reg;
        addFakeKind(reg.jobs);
        const QString queueFile = m_root.filePath(QStringLiteral("q13/queue.json"));
        SettingsStore store(reg.schema, m_root.filePath(QStringLiteral("q13/settings.json")));
        QVERIFY(store.load().ok());
        const QList<QByteArray> damagedFiles{QByteArray("{ \"jobs\": [ {\"kind\": \"test.fake\""), QByteArray("[1, 2]"),
                                             QByteArray("{\"jobs\": \"none\"}")};
        const QStringList asideNames{QStringLiteral("queue.json.damaged"), QStringLiteral("queue.json (2).damaged"),
                                     QStringLiteral("queue.json (3).damaged")};
        for (qsizetype i = 0; i < damagedFiles.size(); ++i) {
            const QByteArray& damaged = damagedFiles.at(i);
            QVERIFY(fwtest::writeTextFile(queueFile, damaged));
            JobQueue q(store, reg.jobs);
            q.setPersistencePath(queueFile);
            const Status st = q.restoreQueue();
            QVERIFY(!st.ok());
            QCOMPARE(st.error().code, ErrorCode::CorruptData);
            QVERIFY2(st.error().message.contains(asideNames.at(i)), qPrintable(st.error().message));
            QVERIFY(q.records().empty());
            QVERIFY(!QFile::exists(queueFile));
            for (qsizetype k = 0; k <= i; ++k) {
                QFile aside(QFileInfo(queueFile).dir().filePath(asideNames.at(k)));
                QVERIFY(aside.open(QIODevice::ReadOnly));
                QCOMPARE(aside.readAll(), damagedFiles.at(k));
            }
            // The queue works on from empty.
            QVERIFY(q.enqueue(QStringLiteral("test.fake"), {}).ok());
            QVERIFY(QFile::exists(queueFile));
        }
    }

    // Saved jobs this build cannot create: a job type of another build stays
    // in the file untouched (that build restores it); a job whose type exists
    // but refuses the saved parameters is shown with the reason.
    void savedJobsThatCannotBeCreatedAreNotDropped()
    {
        fwtest::Registry reg;
        addFakeKind(reg.jobs);
        reg.jobs.add(QStringLiteral("test.picky"), QStringLiteral("Picky"), [](const QJsonObject& p) -> Result<std::unique_ptr<Job>> {
            if (p.value(QStringLiteral("reject")).toBool())
                return Error(ErrorCode::InvalidArgument, QStringLiteral("The saved file list is gone."));
            return std::unique_ptr<Job>(new FakeJob(p));
        });
        const QJsonObject privateJob{{QStringLiteral("kind"), QStringLiteral("private.publish")},
                                     {QStringLiteral("title"), QStringLiteral("Only the other build")},
                                     {QStringLiteral("state"), jobStateName(JobState::Queued)},
                                     {QStringLiteral("added"), QStringLiteral("2026-01-02T03:04:05")},
                                     {QStringLiteral("extra"), QStringLiteral("kept as it is")},
                                     {QStringLiteral("params"), QJsonObject{{QStringLiteral("title"), QStringLiteral("Other")}}}};
        const QJsonArray saved{privateJob,
                               QJsonObject{{QStringLiteral("kind"), QStringLiteral("test.fake")},
                                           {QStringLiteral("state"), jobStateName(JobState::Queued)},
                                           {QStringLiteral("params"), QJsonObject{{QStringLiteral("title"), QStringLiteral("Mine")}}}},
                               QJsonObject{{QStringLiteral("kind"), QStringLiteral("test.picky")},
                                           {QStringLiteral("title"), QStringLiteral("Picky one")},
                                           {QStringLiteral("state"), jobStateName(JobState::Queued)},
                                           {QStringLiteral("params"), QJsonObject{{QStringLiteral("reject"), true}}}}};
        const QString queueFile = m_root.filePath(QStringLiteral("q14/queue.json"));
        QVERIFY(fwtest::writeTextFile(queueFile, QJsonDocument(QJsonObject{{QStringLiteral("version"), 1},
                                                                           {QStringLiteral("jobs"), saved}}).toJson()));
        const auto savedJobs = [&] {
            QFile f(queueFile);
            return f.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(f.readAll()).object().value(QStringLiteral("jobs")).toArray()
                                               : QJsonArray();
        };
        SettingsStore store(reg.schema, m_root.filePath(QStringLiteral("q14/settings.json")));
        QVERIFY(store.load().ok());
        {
            JobQueue q(store, reg.jobs);
            q.setPersistencePath(queueFile);
            QSignalSpy held(&q, &JobQueue::heldChanged);
            QVERIFY(q.restoreQueue().ok());
            QCOMPARE(held.count(), 1); // restored jobs wait: the pages hear about it
            const auto recs = q.records();
            QCOMPARE(int(recs.size()), 2);
            QCOMPARE(recs[0].title, QStringLiteral("Mine"));
            QCOMPARE(recs[0].state, JobState::Queued);
            QCOMPARE(recs[1].title, QStringLiteral("Picky one"));
            QCOMPARE(recs[1].state, JobState::Interrupted);
            QVERIFY2(recs[1].error->message.contains(QStringLiteral("The saved file list is gone.")), qPrintable(recs[1].error->message));
            QVERIFY(!q.retry(recs[1].id).ok()); // still refused, with the creator's reason
            QVERIFY(savedJobs().contains(privateJob));
            QCOMPARE(savedJobs().size(), 3);
            // Later saves keep it too.
            QVERIFY(q.remove(recs[1].id));
            QVERIFY(q.remove(recs[0].id));
            QCOMPARE(savedJobs(), QJsonArray{privateJob});
        }
        // The build that knows the job type gets it back as it was saved.
        reg.jobs.add(QStringLiteral("private.publish"), QStringLiteral("Publish"),
                     [](const QJsonObject& p) -> Result<std::unique_ptr<Job>> { return std::unique_ptr<Job>(new FakeJob(p)); });
        JobQueue q2(store, reg.jobs);
        q2.setPersistencePath(queueFile);
        QVERIFY(q2.restoreQueue().ok());
        const auto recs = q2.records();
        QCOMPARE(int(recs.size()), 1);
        QCOMPARE(recs[0].title, QStringLiteral("Other"));
        QCOMPARE(recs[0].state, JobState::Queued);
    }

    // Removing the last waiting job ends the run: the "queue finished"
    // notification for the job that did run is not lost.
    void removingTheLastWaitingJobFinishesTheRun()
    {
        fwtest::Registry reg;
        addFakeKind(reg.jobs);
        SettingsStore store(reg.schema, m_root.filePath(QStringLiteral("q15/settings.json")));
        QVERIFY(store.load().ok());
        JobQueue q(store, reg.jobs);
        QueueCompletionWatcher w(q, store);
        QSignalSpy finished(&w, &QueueCompletionWatcher::queueFinished);
        q.holdQueue(true);
        auto a = q.enqueue(QStringLiteral("test.fake"), {{QStringLiteral("title"), QStringLiteral("A")}});
        auto b = q.enqueue(QStringLiteral("test.fake"), {{QStringLiteral("title"), QStringLiteral("B")}});
        QVERIFY(a.ok() && b.ok());
        q.startNow(a.value()); // "Build now" on A; B keeps waiting
        QVERIFY(waitForState(q, a.value(), JobState::Succeeded));
        QTest::qWait(100);
        QCOMPARE(finished.count(), 0); // B is still to come
        QVERIFY(q.remove(b.value()));
        QCOMPARE(finished.count(), 1);
        QCOMPARE(finished.first().at(0).toInt(), 1);
        QCOMPARE(w.lastRunJobIds(), QStringList{a.value()});
    }

    // GitHub #96: a job that runs beside the queue (the update check a few
    // seconds after start, a problem report) is not a run of the user's: alone
    // it starts no countdown and no "queue finished", and in a run it is not
    // counted.
    void completionWatcherLeavesOutJobsBesideTheQueue()
    {
        fwtest::Registry reg;
        addFakeKind(reg.jobs);
        SettingsStore store(reg.schema, m_root.filePath(QStringLiteral("q16/settings.json")));
        QVERIFY(store.load().ok());
        QVERIFY(store.setValue(general::keys::kAfterQueueAction, QStringLiteral("sleep")).ok());
        JobQueue q(store, reg.jobs);
        QueueCompletionWatcher w(q, store);
        QSignalSpy due(&w, &QueueCompletionWatcher::actionDue);
        QSignalSpy finished(&w, &QueueCompletionWatcher::queueFinished);
        auto check = q.enqueue(QStringLiteral("test.fake"), {{QStringLiteral("title"), QStringLiteral("Update check")}, {QStringLiteral("system"), true}});
        QVERIFY(check.ok());
        QVERIFY(q.record(check.value())->runsWhileHeld);
        QVERIFY(waitForState(q, check.value(), JobState::Succeeded));
        QTest::qWait(100);
        QCOMPARE(finished.count(), 0);
        QCOMPARE(due.count(), 0);
        // A failed one does not block the action of a run either.
        auto user = q.enqueue(QStringLiteral("test.fake"), {{QStringLiteral("title"), QStringLiteral("Build")}, {QStringLiteral("steps"), 30}});
        auto failing = q.enqueue(QStringLiteral("test.fake"), {{QStringLiteral("system"), true}, {QStringLiteral("fail"), true}});
        QVERIFY(user.ok() && failing.ok());
        QVERIFY(QTest::qWaitFor([&] { return due.count() == 1; }, 5000));
        QCOMPARE(finished.count(), 1);
        QCOMPARE(finished.first().at(0).toInt(), 1);
        QCOMPARE(finished.first().at(1).toInt(), 0);
        QCOMPARE(w.lastRunJobIds(), QStringList{user.value()});
    }

    // A queue restored at start-up never arms the action: not the restore,
    // not taking its jobs out, not cancelling a waiting one, also when the
    // watcher was there before the restore. A restored job that the queue
    // then runs in this session is part of that session's run.
    void completionWatcherIgnoresARestoredQueue()
    {
        fwtest::Registry reg;
        addFakeKind(reg.jobs);
        const QString queueFile = m_root.filePath(QStringLiteral("q17/queue.json"));
        const auto saved = [](const QString& title, JobState state) {
            return QJsonObject{{QStringLiteral("kind"), QStringLiteral("test.fake")},
                               {QStringLiteral("title"), title},
                               {QStringLiteral("state"), jobStateName(state)},
                               {QStringLiteral("params"), QJsonObject{{QStringLiteral("title"), title}}}};
        };
        QVERIFY(fwtest::writeTextFile(queueFile, QJsonDocument(QJsonObject{{QStringLiteral("version"), 1},
                                                                           {QStringLiteral("jobs"), QJsonArray{saved(QStringLiteral("Was running"), JobState::Running),
                                                                                                               saved(QStringLiteral("Interrupted before"), JobState::Interrupted),
                                                                                                               saved(QStringLiteral("Waiting 1"), JobState::Queued),
                                                                                                               saved(QStringLiteral("Waiting 2"), JobState::Queued)}}})
                                                     .toJson()));
        SettingsStore store(reg.schema, m_root.filePath(QStringLiteral("q17/settings.json")));
        QVERIFY(store.load().ok());
        QVERIFY(store.setValue(general::keys::kAfterQueueAction, QStringLiteral("shutdown")).ok());
        QVERIFY(store.setValue(general::keys::kAfterQueueOnlyIfOk, false).ok()); // the widest rule: any ended job would count
        JobQueue q(store, reg.jobs);
        q.setPersistencePath(queueFile);
        QueueCompletionWatcher w(q, store);
        QSignalSpy due(&w, &QueueCompletionWatcher::actionDue);
        QSignalSpy finished(&w, &QueueCompletionWatcher::queueFinished);
        QVERIFY(q.restoreQueue().ok());
        QVERIFY(q.isHeld());
        QString waiting1, waiting2;
        QStringList interrupted;
        for (const JobRecord& r : q.records()) {
            if (r.title == QLatin1String("Waiting 1"))
                waiting1 = r.id;
            else if (r.title == QLatin1String("Waiting 2"))
                waiting2 = r.id;
            else if (r.state == JobState::Interrupted)
                interrupted << r.id;
        }
        QCOMPARE(interrupted.size(), 2);
        QTest::qWait(100);
        q.cancel(waiting1);
        for (const QString& id : interrupted)
            QVERIFY(q.remove(id));
        QTest::qWait(100);
        QCOMPARE(due.count(), 0);
        QCOMPARE(finished.count(), 0);
        // "Start queue": the restored job runs now, in this session.
        q.holdQueue(false);
        QVERIFY(waitForState(q, waiting2, JobState::Succeeded));
        QVERIFY(QTest::qWaitFor([&] { return due.count() == 1; }, 5000));
        QCOMPARE(finished.count(), 1);
        QCOMPARE(finished.first().at(0).toInt(), 1);
        QCOMPARE(finished.first().at(2).toInt(), 1); // the one cancelled while it waited
        QCOMPARE(w.lastRunJobIds(), (QStringList{waiting1, waiting2}));
    }

    // A job that failed, waits for its automatic retry and is cancelled then
    // had run: with "Only if every job succeeded" the computer stays on.
    void aJobCancelledWhileWaitingToRetryCountsAsStarted()
    {
        fwtest::Registry reg;
        addFakeKind(reg.jobs);
        SettingsStore store(reg.schema, m_root.filePath(QStringLiteral("q18/settings.json")));
        QVERIFY(store.load().ok());
        QVERIFY(store.setValue(general::keys::kAfterQueueAction, QStringLiteral("sleep")).ok());
        QVERIFY(store.setValue(general::keys::kAutoRetryCount, 1).ok());
        QVERIFY(store.setValue(general::keys::kAutoRetryDelay, 600).ok());
        JobQueue q(store, reg.jobs);
        QueueCompletionWatcher w(q, store);
        QSignalSpy due(&w, &QueueCompletionWatcher::actionDue);
        QSignalSpy finished(&w, &QueueCompletionWatcher::queueFinished);
        auto good = q.enqueue(QStringLiteral("test.fake"), {{QStringLiteral("title"), QStringLiteral("Good")}});
        auto flaky = q.enqueue(QStringLiteral("test.fake"), {{QStringLiteral("title"), QStringLiteral("Flaky q18")}, {QStringLiteral("failTimes"), 5}});
        QVERIFY(good.ok() && flaky.ok());
        QVERIFY(QTest::qWaitFor([&] {
            auto r = q.record(flaky.value());
            return r && r->isWaitingToRetry();
        }, 5000));
        QVERIFY(!q.record(flaky.value())->started.isValid()); // reset for the retry
        q.cancel(flaky.value());
        QVERIFY(QTest::qWaitFor([&] { return finished.count() == 1; }, 5000));
        QCOMPARE(finished.first().at(0).toInt(), 1);
        QCOMPARE(finished.first().at(2).toInt(), 1);
        QCOMPARE(due.count(), 0);
    }

    // Retry on a job interrupted in an earlier session: the retry is the job
    // now, and the old record does not come back at the next start.
    void aRetriedInterruptedJobIsNotRestoredAgain()
    {
        fwtest::Registry reg;
        addFakeKind(reg.jobs);
        const QString queueFile = m_root.filePath(QStringLiteral("q19/queue.json"));
        QVERIFY(fwtest::writeTextFile(queueFile, QJsonDocument(QJsonObject{{QStringLiteral("version"), 1},
                                                                           {QStringLiteral("jobs"), QJsonArray{QJsonObject{
                                                                                                        {QStringLiteral("kind"), QStringLiteral("test.fake")},
                                                                                                        {QStringLiteral("title"), QStringLiteral("Cut off")},
                                                                                                        {QStringLiteral("state"), jobStateName(JobState::Running)},
                                                                                                        {QStringLiteral("params"), QJsonObject{{QStringLiteral("title"), QStringLiteral("Cut off")}}}}}}})
                                                     .toJson()));
        SettingsStore store(reg.schema, m_root.filePath(QStringLiteral("q19/settings.json")));
        QVERIFY(store.load().ok());
        const auto savedStates = [&] {
            QStringList states;
            QFile f(queueFile);
            if (f.open(QIODevice::ReadOnly))
                for (const QJsonValue& v : QJsonDocument::fromJson(f.readAll()).object().value(QStringLiteral("jobs")).toArray())
                    states << v.toObject().value(QStringLiteral("state")).toString();
            return states;
        };
        {
            JobQueue q(store, reg.jobs);
            q.setPersistencePath(queueFile);
            QVERIFY(q.restoreQueue().ok());
            QCOMPARE(int(q.records().size()), 1);
            const JobRecord old = q.records().front();
            QCOMPARE(old.state, JobState::Interrupted);
            QCOMPARE(savedStates(), QStringList{jobStateName(JobState::Interrupted)});
            auto again = q.retry(old.id);
            QVERIFY(again.ok());
            QVERIFY(waitForState(q, again.value(), JobState::Succeeded));
            QCOMPARE(q.record(old.id)->state, JobState::Interrupted); // still listed in this session
            QVERIFY(savedStates().isEmpty());
        }
        JobQueue q2(store, reg.jobs);
        q2.setPersistencePath(queueFile);
        QVERIFY(q2.restoreQueue().ok());
        QVERIFY(q2.records().empty());
    }

    // "Build 3 images" with the queue idle or paused: the jobs run in the
    // order given, ahead of the other waiting jobs, which keep waiting.
    void startNowRunsSeveralJobsInTheirOrder()
    {
        fwtest::Registry reg;
        addFakeKind(reg.jobs);
        SettingsStore store(reg.schema, m_root.filePath(QStringLiteral("q20/settings.json")));
        QVERIFY(store.load().ok());
        JobQueue q(store, reg.jobs);
        q.holdQueue(true);
        auto other = q.enqueue(QStringLiteral("test.fake"), {{QStringLiteral("title"), QStringLiteral("Other q20")}});
        QStringList batch;
        for (const QString& t : {QStringLiteral("A q20"), QStringLiteral("B q20"), QStringLiteral("C q20")}) {
            auto r = q.enqueue(QStringLiteral("test.fake"), {{QStringLiteral("title"), t}});
            QVERIFY(r.ok());
            batch << r.value();
        }
        {
            QMutexLocker lock(&FakeJob::runOrderMutex());
            FakeJob::runOrder().clear();
        }
        q.startNow(batch);
        for (const QString& id : batch)
            QVERIFY(waitForState(q, id, JobState::Succeeded));
        QStringList order;
        {
            QMutexLocker lock(&FakeJob::runOrderMutex());
            order = FakeJob::runOrder();
        }
        QCOMPARE(order, (QStringList{QStringLiteral("A q20"), QStringLiteral("B q20"), QStringLiteral("C q20")}));
        QCOMPARE(q.record(other.value())->state, JobState::Queued);
        q.holdQueue(false);
        QVERIFY(waitForState(q, other.value(), JobState::Succeeded));
    }

    // Cancel while paused: the job leaves Paused for Cancelling and ends
    // Cancelled once, in the history too, and it counts as a job that had run.
    void cancelWhilePausedEndsCancelled()
    {
        fwtest::Registry reg;
        addFakeKind(reg.jobs);
        SettingsStore store(reg.schema, m_root.filePath(QStringLiteral("q21/settings.json")));
        QVERIFY(store.load().ok());
        QVERIFY(store.setValue(general::keys::kAfterQueueAction, QStringLiteral("sleep")).ok());
        JobHistory history(m_root.filePath(QStringLiteral("q21/history.jsonl")));
        JobQueue q(store, reg.jobs);
        q.setHistory(&history);
        QueueCompletionWatcher w(q, store);
        QSignalSpy due(&w, &QueueCompletionWatcher::actionDue);
        QSignalSpy finished(&w, &QueueCompletionWatcher::queueFinished);
        QSignalSpy ended(&q, &JobQueue::jobFinished);
        auto ok = q.enqueue(QStringLiteral("test.fake"), {{QStringLiteral("title"), QStringLiteral("Before q21")}});
        auto id = q.enqueue(QStringLiteral("test.fake"), {{QStringLiteral("title"), QStringLiteral("Paused q21")}, {QStringLiteral("steps"), 1000}, {QStringLiteral("stepMs"), 10}});
        QVERIFY(ok.ok() && id.ok());
        QVERIFY(waitForState(q, id.value(), JobState::Running));
        q.pauseJob(id.value());
        QCOMPARE(q.record(id.value())->state, JobState::Paused);
        q.cancel(id.value());
        QCOMPARE(q.record(id.value())->state, JobState::Cancelling);
        q.resumeJob(id.value()); // no way back from Cancelling
        QCOMPARE(q.record(id.value())->state, JobState::Cancelling);
        q.pauseJob(id.value());
        QCOMPARE(q.record(id.value())->state, JobState::Cancelling);
        QVERIFY(waitForState(q, id.value(), JobState::Cancelled, 5000));
        QVERIFY(QTest::qWaitFor([&] { return finished.count() == 1; }, 5000));
        int endedPaused = 0;
        for (const QList<QVariant>& args : ended)
            endedPaused += args.at(0).toString() == id.value() ? 1 : 0;
        QCOMPARE(endedPaused, 1);
        int inHistory = 0;
        for (const HistoryEntry& h : history.recent(50))
            inHistory += h.id == id.value() && h.state == jobStateName(JobState::Cancelled) ? 1 : 0;
        QCOMPARE(inHistory, 1);
        QCOMPARE(due.count(), 0); // it had started: "Only if every job succeeded" keeps the computer on
    }

    // "Clear history" that cannot remove the file keeps showing the history:
    // it is still there and would be back after the next start.
    void historyThatCannotBeClearedStaysVisible()
    {
#ifdef Q_OS_WIN
        JobHistory history(m_root.filePath(QStringLiteral("hist/history.jsonl")));
        JobRecord rec;
        rec.id = QStringLiteral("J1");
        rec.kind = QStringLiteral("test.fake");
        rec.title = QStringLiteral("Kept");
        rec.state = JobState::Succeeded;
        QVERIFY(history.append(rec).ok());
        QCOMPARE(int(history.recent(10).size()), 1);
        {
            QFile holder(m_root.filePath(QStringLiteral("hist/history.jsonl"))); // open without delete sharing
            QVERIFY(holder.open(QIODevice::ReadOnly));
            QVERIFY(!history.clear().ok());
            QCOMPARE(int(history.recent(10).size()), 1);
        }
        QVERIFY(history.clear().ok());
        QVERIFY(history.recent(10).empty());
#else
        QSKIP("Windows file sharing rules");
#endif
    }

    // A hand-edited schema version far below zero must not make start-up
    // count through two thousand million migration steps.
    void absurdSchemaVersionLoadsAtOnce()
    {
        fwtest::Registry reg;
        const QString path = m_root.filePath(QStringLiteral("s9/settings.json"));
        QVERIFY(fwtest::writeTextFile(path, R"({"schemaVersion":-2000000000,"values":{"logging.level":"debug"}})"));
        SettingsStore store(reg.schema, path);
        QElapsedTimer timer;
        timer.start();
        QVERIFY(store.load().ok());
        QVERIFY2(timer.elapsed() < 2000, qPrintable(QString::number(timer.elapsed())));
        QCOMPARE(store.value(general::keys::kLogLevel).toString(), QStringLiteral("debug"));
    }

    // Range errors name the limits as whole numbers, not "1.04858e+06".
    void rangeErrorsPrintPlainNumbers()
    {
        SettingsSchema schema;
        SettingDef spare;
        spare.key = QStringLiteral("test.spareMiB");
        spare.section = SettingsSchema::sectionOrder().first();
        spare.label = QStringLiteral("Fixed free space");
        spare.type = SettingType::Int;
        spare.defaultValue = 0;
        spare.minimum = 0;
        spare.maximum = 1048576;
        schema.add(spare);
        SettingDef ratio = spare;
        ratio.key = QStringLiteral("test.ratio");
        ratio.label = QStringLiteral("Ratio");
        ratio.type = SettingType::Double;
        ratio.minimum = 0.5;
        ratio.maximum = 12345678.25;
        schema.add(ratio);
        SettingDef free = spare;
        free.key = QStringLiteral("test.unbounded");
        free.maximum = 0;
        schema.add(free);
        auto bad = schema.normalize(spare.key, 2000000);
        QVERIFY(!bad.ok());
        QCOMPARE(bad.error().message, QStringLiteral("Fixed free space: must be between 0 and 1048576."));
        QCOMPARE(schema.normalize(ratio.key, 0.1).error().message, QStringLiteral("Ratio: must be between 0.5 and 12345678.25."));
        QCOMPARE(schema.normalize(spare.key, 1048576).value().toLongLong(), 1048576);
        // "inf" and 1e300 are numbers, but no whole number the app can store.
        QVERIFY(!schema.normalize(free.key, QStringLiteral("inf")).ok());
        QVERIFY(!schema.normalize(free.key, 1e300).ok());
        QCOMPARE(schema.normalize(free.key, QStringLiteral("-12")).value().toLongLong(), -12);
    }
};

QTEST_GUILESS_MAIN(TstSettingsJobs)
#include "tst_settings_jobs.moc"
