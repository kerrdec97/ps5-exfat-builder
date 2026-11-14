// PFS engine: settings (declared, read, legacy import), the three queue jobs
// (build / verify / extract), cancellation and failure clean-up, and the
// audit rules C1 (a failed check is a failure, the source is kept) and H2
// (exclusions never cause false verification failures).

#include "engines/general/Module.h"
#include "engines/pfs/AmprJobs.h"
#include "engines/pfs/Module.h"
#include "engines/pfs/PfsJobs.h"

#include "core/FileUtil.h"
#include "engines/pfs/PfsTestUtil.h"
#include "jobs/JobQueue.h"
#include "support/ProgressAudit.h"
#include "support/SparseFile.h"
#include "support/TestSupport.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QStorageInfo>
#include <QTemporaryDir>
#include <QTest>

#include <atomic>
#include <cmath>

using namespace fw;
using namespace fw::pfs;

namespace {

struct Env {
    std::shared_ptr<SettingsSchema> schema = std::make_shared<SettingsSchema>();
    DependencyRegistry deps;
    JobFactory jobs;
    Env()
    {
        ModuleContext ctx{*schema, deps, jobs};
        general::registerModule(ctx);
        pfs::registerModule(ctx);
    }
};

struct RunResult {
    Result<JobOutcome> outcome = Error(ErrorCode::Internal, QStringLiteral("not run"));
    QStringList phasesSeen;
};

// Runs a job synchronously on this thread, like the queue's worker would
// (the phases and their shares declared for the job's settings); `audit`
// watches the progress.
RunResult runJob(Job& job, const SettingsValues& settings, const CancellationToken& token = {},
                 std::function<void(const ProgressSnapshot&)> onProgress = {}, fwtest::ProgressAudit* audit = nullptr)
{
    RunResult rr;
    ProgressReporter reporter(
        [&](const ProgressSnapshot& s) {
            if (rr.phasesSeen.isEmpty() || rr.phasesSeen.back() != s.phase)
                rr.phasesSeen << s.phase;
            if (audit)
                audit->report(s);
            if (onProgress)
                onProgress(s);
        },
        0);
    if (audit)
        audit->watch(&reporter);
    reporter.setPhases(job.phases(), job.phaseWeights(settings));
    QTemporaryDir temp;
    if (!temp.isValid()) {
        rr.outcome = Error(ErrorCode::Internal, QStringLiteral("the test could not make a temporary folder for the job"));
        return rr;
    }
    JobContext ctx;
    ctx.jobId = QStringLiteral("test");
    ctx.token = token;
    ctx.progress = &reporter;
    ctx.log = Log(job.kind(), ctx.jobId);
    ctx.settings = settings;
    ctx.tempDir = temp.path();
    rr.outcome = job.run(ctx);
    if (audit) {
        reporter.finish();
        audit->finish(reporter.snapshot(), rr.outcome.ok() && rr.outcome->verification != Verification::Failed);
        audit->watch(nullptr);
    }
    return rr;
}

// The same for a job fresh from the factory. A job the factory refused is
// not run: the refusal is the outcome, for the caller to assert on.
RunResult runJob(const Result<std::unique_ptr<Job>>& job, const SettingsValues& settings, const CancellationToken& token = {},
                 std::function<void(const ProgressSnapshot&)> onProgress = {}, fwtest::ProgressAudit* audit = nullptr)
{
    if (!job.ok()) {
        RunResult rr;
        rr.outcome = Error(ErrorCode::Internal, QStringLiteral("the factory refused the job: ") + job.error().toLogString());
        return rr;
    }
    return runJob(**job, settings, token, std::move(onProgress), audit);
}

QJsonObject params(std::initializer_list<std::pair<QString, QJsonValue>> kv)
{
    QJsonObject o;
    for (const auto& [k, v] : kv)
        o.insert(k, v);
    return o;
}

} // namespace

class TestPfsJobs : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QVERIFY(m_appData.isValid());
        fwtest::isolateAppData(m_appData);
        m_spec = pfstest::loadJson(QStringLiteral("golden_cases.json"));
        QVERIFY(!m_spec.isEmpty());
    }

    void settingsAreDeclared()
    {
        Env env;
        const QStringList pfsKeys = {keys::kCompressionLevel,   keys::kContainerMinCompressMiB, keys::kContainerBlockGain,
                                     keys::kCompressFiles,      keys::kFileMinCompressMiB,      keys::kFileBlockGain,
                                     keys::kFileMinGain,        keys::kSkipExecutables,         keys::kBlockSize,
                                     keys::kCaseInsensitive,    keys::kImageVersion,            keys::kExcludes,
                                     keys::kKeepEmptyFolders,   keys::kAmprIndex,               keys::kThreads,
                                     keys::kDeleteSourceAfterConvert};
        int declared = 0;
        for (const auto* d : env.schema->all()) {
            if (!d->key.startsWith(QStringLiteral("pfs.")))
                continue;
            ++declared;
            QVERIFY2(pfsKeys.contains(d->key), qPrintable(d->key));
            QVERIFY2(!d->description.isEmpty(), qPrintable(d->key));
            QVERIFY2(env.schema->normalize(d->key, d->defaultValue).ok(), qPrintable(d->key));
            QCOMPARE(d->section, QStringLiteral("PFS"));
        }
        QCOMPARE(declared, pfsKeys.size());
        QVERIFY(env.jobs.contains(kinds::kBuild));
        QVERIFY(env.jobs.contains(kinds::kVerify));
        QVERIFY(env.jobs.contains(kinds::kExtract));
    }

    // Every pfs.* setting reaches the engine options.
    void settingsAreRead()
    {
        Env env;
        SettingsStore store(env.schema, m_appData.filePath(QStringLiteral("read/settings.json")));
        const SettingsValues defaults = store.snapshot();
        const BuildOptions d = buildOptionsFrom(defaults, 42);
        QCOMPARE(d.level, 9);
        QCOMPARE(d.layout.blockSize, 0x10000);
        QVERIFY(d.layout.caseInsensitive);
        QCOMPARE(d.layout.version, kVersionPs5);
        QCOMPARE(d.layout.time, qint64(42));
        QCOMPARE(d.container.minCompressMiB, 1.0);
        QCOMPARE(d.container.blockGain, 1);
        QVERIFY(!d.files.compress);
        QCOMPARE(d.files.minCompressMiB, 5.0);
        QCOMPARE(d.files.blockGain, 5);
        QCOMPARE(d.files.minFileGain, 0);
        QVERIFY(d.files.skipExecutables);
        QCOMPARE(d.threads, general::workerThreads(defaults));
        const PlanOptions p = planOptionsFrom(defaults, 42);
        QCOMPARE(p.excludes.size(), 4);
        QVERIFY(p.keepEmptyFolders);
        QCOMPARE(p.ampr, AmprMode::Missing);
        QCOMPARE(verifyDepthFrom(defaults), std::optional<VerifyDepth>(VerifyDepth::Full));

        auto norm = store.normalizeOverrides({{keys::kCompressionLevel, 3},
                                              {keys::kBlockSize, QStringLiteral("4096")},
                                              {keys::kCaseInsensitive, false},
                                              {keys::kImageVersion, QStringLiteral("ps4")},
                                              {keys::kContainerMinCompressMiB, 0.5},
                                              {keys::kContainerBlockGain, 7},
                                              {keys::kCompressFiles, true},
                                              {keys::kFileMinCompressMiB, 2.0},
                                              {keys::kFileBlockGain, 11},
                                              {keys::kFileMinGain, 13},
                                              {keys::kSkipExecutables, false},
                                              {keys::kExcludes, QStringList{QStringLiteral("x")}},
                                              {keys::kKeepEmptyFolders, false},
                                              {keys::kAmprIndex, QStringLiteral("refresh")},
                                              {keys::kThreads, 3},
                                              {general::keys::kVerifyMode, QStringLiteral("structure")}});
        QVERIFY2(norm.ok(), norm.ok() ? "" : qPrintable(norm.error().message));
        const SettingsValues s = store.snapshot(*norm);
        const BuildOptions o = buildOptionsFrom(s, 1);
        QCOMPARE(o.level, 3);
        QCOMPARE(o.layout.blockSize, 4096);
        QVERIFY(!o.layout.caseInsensitive);
        QCOMPARE(o.layout.version, kVersionPs4);
        QCOMPARE(o.container.minCompressMiB, 0.5);
        QCOMPARE(o.container.blockGain, 7);
        QVERIFY(o.files.compress);
        QCOMPARE(o.files.minCompressMiB, 2.0);
        QCOMPARE(o.files.blockGain, 11);
        QCOMPARE(o.files.minFileGain, 13);
        QVERIFY(!o.files.skipExecutables);
        QCOMPARE(o.threads, std::min(3, std::max(1, QThread::idealThreadCount()))); // never more threads than cores
        const PlanOptions po = planOptionsFrom(s, 1);
        QCOMPARE(po.excludes, QStringList{QStringLiteral("x")});
        QVERIFY(!po.keepEmptyFolders);
        QCOMPARE(po.ampr, AmprMode::Refresh);
        QCOMPARE(verifyDepthFrom(s), std::optional<VerifyDepth>(VerifyDepth::Structure));
        auto off = store.normalizeOverrides({{general::keys::kVerifyAfterBuild, false}, {keys::kAmprIndex, QStringLiteral("off")}});
        QVERIFY(off.ok());
        QVERIFY(!verifyDepthFrom(store.snapshot(*off)));
        QCOMPARE(planOptionsFrom(store.snapshot(*off), 0).ampr, AmprMode::Off);
        auto automatic = store.normalizeOverrides({{keys::kAmprIndex, QStringLiteral("auto")}});
        QVERIFY2(automatic.ok(), automatic.ok() ? "" : qPrintable(automatic.error().message));
        QCOMPARE(planOptionsFrom(store.snapshot(*automatic), 0).ampr, AmprMode::Auto);
        QVERIFY(!store.normalizeOverrides({{keys::kAmprIndex, QStringLiteral("sometimes")}}).ok());
    }

    void legacySettingsImport()
    {
        Env env;
        SettingsStore store(env.schema, m_appData.filePath(QStringLiteral("legacy/settings.json")));
        const QString legacy = m_appData.filePath(QStringLiteral("legacy/.exfat_builder_settings.json"));
        QJsonObject o;
        o.insert(QStringLiteral("pfs_compression_level"), 0); // ISA-L "Fast" level: clamped to zlib 1
        o.insert(QStringLiteral("pfs_lazy_cpu_count"), 6);
        o.insert(QStringLiteral("pfs_delete_source_after_convert"), true);
        o.insert(QStringLiteral("pfs_lazy_zlib_backend"), QStringLiteral("isa-l")); // not ported
        o.insert(QStringLiteral("pfs_backend"), QStringLiteral("lazy"));            // one native engine
        QVERIFY(writeFileAtomic(legacy, QJsonDocument(o).toJson()).ok());
        auto n = store.importLegacy(legacy);
        QVERIFY(n.ok());
        QCOMPARE(*n, 3);
        QCOMPARE(store.value(keys::kCompressionLevel).toInt(), 1);
        QCOMPARE(store.value(keys::kThreads).toInt(), 6);
        QCOMPARE(store.value(keys::kDeleteSourceAfterConvert).toBool(), true);
        o.insert(QStringLiteral("pfs_compression_level"), 6);
        QVERIFY(writeFileAtomic(legacy, QJsonDocument(o).toJson()).ok());
        QVERIFY(store.importLegacy(legacy).ok());
        QCOMPARE(store.value(keys::kCompressionLevel).toInt(), 6);
    }

    void buildJob_data()
    {
        QTest::addColumn<QString>("source");
        QTest::addColumn<QString>("output");
        QTest::newRow("folder to ffpfsc") << QStringLiteral("folder") << QStringLiteral("game.ffpfsc");
        QTest::newRow("folder to ffpfs") << QStringLiteral("folder") << QStringLiteral("game.ffpfs");
        QTest::newRow("image to ffpfsc") << QStringLiteral("file") << QStringLiteral("PPSA01234.ffpfsc");
    }

    void buildJob()
    {
        QFETCH(QString, source);
        QFETCH(QString, output);
        Env env;
        SettingsStore store(env.schema, m_appData.filePath(QStringLiteral("bj/settings.json")));
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString src = source == u"folder" ? makeGame(tmp) : makeImage(tmp);
        const QString out = tmp.filePath(QStringLiteral("out/") + output);
        auto job = env.jobs.create(kinds::kBuild, params({{QStringLiteral("source"), src}, {QStringLiteral("output"), out}}));
        QVERIFY(job.ok());
        QVERIFY((*job)->validate(store.snapshot()).ok());
        auto rr = runJob(job, store.snapshot());
        QVERIFY2(rr.outcome.ok(), rr.outcome.ok() ? "" : qPrintable(rr.outcome.error().toLogString()));
        const JobOutcome& o = *rr.outcome;
        QCOMPARE(o.verification, Verification::Passed);
        QVERIFY2(o.verificationDetail.contains(QStringLiteral("byte for byte")), qPrintable(o.verificationDetail));
        QCOMPARE(o.outputs, QStringList{QDir::cleanPath(out)});
        QVERIFY(QFileInfo(out).isFile());
        QVERIFY(!QFileInfo::exists(PartFile::partPathFor(out)));
        QCOMPARE(rr.phasesSeen, (QStringList{QStringLiteral("Scanning"), QStringLiteral("Compressing and writing"),
                                             QStringLiteral("Verifying")}));
        QVERIFY(QFileInfo::exists(src)); // never deleted by default

        // The standalone verify job agrees, with and without the source.
        auto vj = env.jobs.create(kinds::kVerify, params({{QStringLiteral("image"), out}, {QStringLiteral("source"), src}}));
        auto vr = runJob(vj, store.snapshot());
        QVERIFY2(vr.outcome.ok(), vr.outcome.ok() ? "" : qPrintable(vr.outcome.error().toLogString()));
        QCOMPARE(vr.outcome->verification, Verification::Passed);
        auto vs = env.jobs.create(kinds::kVerify, params({{QStringLiteral("image"), out}}));
        auto vsr = runJob(vs, store.snapshot());
        QVERIFY(vsr.outcome.ok());
        QCOMPARE(vsr.outcome->verification, Verification::Passed);
    }

    void buildThroughQueue()
    {
        Env env;
        SettingsStore store(env.schema, m_appData.filePath(QStringLiteral("q/settings.json")));
        JobQueue q(store, env.jobs);
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString src = makeGame(tmp);
        const QString out = tmp.filePath(QStringLiteral("game.ffpfsc"));
        auto id = q.enqueue(kinds::kBuild, params({{QStringLiteral("source"), src}, {QStringLiteral("output"), out}}),
                            {{keys::kCompressionLevel, 1}});
        QVERIFY2(id.ok(), id.ok() ? "" : qPrintable(id.error().message));
        QVERIFY(QTest::qWaitFor([&] {
            auto r = q.record(*id);
            return r && isTerminal(r->state);
        }, 300000));
        auto rec = q.record(*id);
        QVERIFY(rec);
        QCOMPARE(rec->state, JobState::Succeeded);
        QVERIFY(rec->outcome);
        QCOMPARE(rec->outcome->verification, Verification::Passed);
        QVERIFY(QFileInfo(out).isFile());

        // Output exists: refused unless replaceExisting.
        auto again = q.enqueue(kinds::kBuild, params({{QStringLiteral("source"), src}, {QStringLiteral("output"), out}}));
        QVERIFY(!again.ok());
        FWTEST_COMPARE_ERROR(again, ErrorCode::AlreadyExists);
    }

    void validateRules()
    {
        Env env;
        SettingsStore store(env.schema, m_appData.filePath(QStringLiteral("v/settings.json")));
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString src = makeGame(tmp);
        // What validate() says. A job the factory itself refuses is reported
        // as an internal error, which none of the checks below expects.
        auto check = [&](const QJsonObject& p) -> Status {
            auto job = env.jobs.create(kinds::kBuild, p);
            if (!job.ok())
                return Error(ErrorCode::Internal, QStringLiteral("the factory refused the job: ") + job.error().toLogString());
            return (*job)->validate(store.snapshot());
        };
        auto inside = check(params({{QStringLiteral("source"), src}, {QStringLiteral("output"), src + QStringLiteral("/x.ffpfsc")}}));
        QVERIFY(!inside.ok());
        FWTEST_COMPARE_ERROR(inside, ErrorCode::InvalidArgument);
        FWTEST_COMPARE_ERROR(check(params({{QStringLiteral("source"), tmp.filePath(QStringLiteral("nope"))},
                                           {QStringLiteral("output"), tmp.filePath(QStringLiteral("x.ffpfsc"))}})),
                             ErrorCode::NotFound);
        FWTEST_COMPARE_ERROR(check(params({{QStringLiteral("source"), src}, {QStringLiteral("output"), tmp.filePath(QStringLiteral("x"))},
                                           {QStringLiteral("format"), QStringLiteral("zip")}})),
                             ErrorCode::InvalidArgument);

        // An existing image is refused, or replaced when asked. (That it
        // survives a rebuild which is cancelled or fails its check is tested
        // in cancellationLeavesNoOutput and verificationFailureRemovesImage.)
        const QString out = tmp.filePath(QStringLiteral("game.ffpfsc"));
        QVERIFY(pfstest::writeFile(out, "old image", -1));
        FWTEST_COMPARE_ERROR(check(params({{QStringLiteral("source"), src}, {QStringLiteral("output"), out}})),
                             ErrorCode::AlreadyExists);
        auto job = env.jobs.create(kinds::kBuild, params({{QStringLiteral("source"), src}, {QStringLiteral("output"), out},
                                                          {QStringLiteral("replaceExisting"), true}}));
        auto rr = runJob(job, store.snapshot());
        QVERIFY2(rr.outcome.ok(), rr.outcome.ok() ? "" : qPrintable(rr.outcome.error().toLogString()));
        QVERIFY(QFileInfo(out).size() > 1000);
        QVERIFY(!QFileInfo::exists(PartFile::partPathFor(out)));
    }

    void cancellationLeavesNoOutput_data()
    {
        QTest::addColumn<int>("phase");
        QTest::addColumn<bool>("replacing");
        QTest::addColumn<bool>("closing");
        QTest::newRow("while writing") << 1 << false << false;
        QTest::newRow("while writing, replacing an image") << 1 << true << false;
        // Closing the app is no Cancel the user chose: nothing is kept unverified.
        QTest::newRow("app closed while verifying") << 2 << false << true;
        QTest::newRow("app closed while verifying, replacing an image") << 2 << true << true;
    }

    // A build cancelled before its image is finished leaves nothing of its
    // own, and an image it was to replace is still there, byte for byte. (A
    // cancel during verification: cancelDuringVerificationSavesTheImage.)
    void cancellationLeavesNoOutput()
    {
        QFETCH(int, phase);
        QFETCH(bool, replacing);
        QFETCH(bool, closing);
        Env env;
        SettingsStore store(env.schema, m_appData.filePath(QStringLiteral("c/settings.json")));
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString src = makeGame(tmp, 16 * 1024 * 1024);
        const QString out = tmp.filePath(QStringLiteral("game.ffpfsc"));
        const QByteArray previous("the image that was here before");
        if (replacing)
            QVERIFY(pfstest::writeFile(out, previous, -1));
        auto job = env.jobs.create(kinds::kBuild, params({{QStringLiteral("source"), src}, {QStringLiteral("output"), out},
                                                          {QStringLiteral("replaceExisting"), replacing}}));
        CancellationSource cs;
        auto rr = runJob(job, store.snapshot(), cs.token(), [&](const ProgressSnapshot& s) {
            if (s.phaseIndex == phase && s.done > 1024 * 1024) {
                if (closing)
                    cs.shutdown();
                else
                    cs.cancel();
            }
        });
        QVERIFY(cs.isCancelled());
        QVERIFY(!rr.outcome.ok());
        QVERIFY2(rr.outcome.error().isCancelled(), qPrintable(rr.outcome.error().toLogString()));
        QVERIFY(!QFileInfo::exists(PartFile::partPathFor(out)));
        if (replacing) {
            QCOMPARE(pfstest::readAll(out), previous);
            QCOMPARE(QDir(tmp.path()).entryList(QDir::Files), QStringList{QStringLiteral("game.ffpfsc")});
        } else {
            QVERIFY(!QFileInfo::exists(out));
            QCOMPARE(QDir(tmp.path()).entryList(QDir::Files), QStringList());
        }
    }

    void cancelDuringVerificationSavesTheImage_data()
    {
        QTest::addColumn<bool>("replacing");
        QTest::newRow("new image") << false;
        QTest::newRow("replacing an image") << true;
    }

    // GitHub #97: Cancel while the finished image is verified saves it as with
    // verification turned off. It replaces an older image only now, the job
    // does not fail, it says the image is not verified, and a source image is
    // never deleted for it.
    void cancelDuringVerificationSavesTheImage()
    {
        QFETCH(bool, replacing);
        Env env;
        SettingsStore store(env.schema, m_appData.filePath(QStringLiteral("vc/settings.json")));
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString src = makeGame(tmp, 16 * 1024 * 1024);
        const QString out = tmp.filePath(QStringLiteral("game.ffpfsc"));
        if (replacing)
            QVERIFY(pfstest::writeFile(out, QByteArray("the image that was here before"), -1));
        auto job = env.jobs.create(kinds::kBuild, params({{QStringLiteral("source"), src}, {QStringLiteral("output"), out},
                                                          {QStringLiteral("replaceExisting"), replacing}}));
        CancellationSource cs;
        std::atomic<bool> cancelled{false};
        auto rr = runJob(job, store.snapshot(), cs.token(), [&](const ProgressSnapshot& s) {
            if (s.phaseIndex == 2 && s.done > 1024 * 1024 && !cancelled.exchange(true))
                cs.cancel();
        });
        QVERIFY(cancelled);
        QVERIFY2(rr.outcome.ok(), rr.outcome.ok() ? "" : qPrintable(rr.outcome.error().toLogString()));
        QCOMPARE(rr.outcome->verification, Verification::Skipped);
        QVERIFY2(rr.outcome->verificationDetail.contains(QStringLiteral("cancelled")), qPrintable(rr.outcome->verificationDetail));
        QCOMPARE(rr.outcome->outputs, QStringList{out});
        QVERIFY(!QFileInfo::exists(PartFile::partPathFor(out)));
        QCOMPARE(QDir(tmp.path()).entryList(QDir::Files), QStringList{QStringLiteral("game.ffpfsc")});
        auto check = runJob(env.jobs.create(kinds::kVerify, params({{QStringLiteral("image"), out}, {QStringLiteral("source"), src}})),
                            store.snapshot());
        QVERIFY2(check.outcome.ok() && check.outcome->verification == Verification::Passed,
                 check.outcome.ok() ? qPrintable(check.outcome->verificationDetail) : qPrintable(check.outcome.error().toLogString()));

        // An image converted from another one: the source image is kept.
        const QString img = makeImage(tmp);
        const QString out2 = tmp.filePath(QStringLiteral("converted.ffpfsc"));
        auto norm = store.normalizeOverrides({{keys::kDeleteSourceAfterConvert, true}});
        QVERIFY(norm.ok());
        CancellationSource cs2;
        auto rr2 = runJob(env.jobs.create(kinds::kBuild, params({{QStringLiteral("source"), img}, {QStringLiteral("output"), out2}})),
                          store.snapshot(*norm), cs2.token(), [&](const ProgressSnapshot& s) {
                              if (s.phaseIndex == 2)
                                  cs2.cancel();
                          });
        QVERIFY2(rr2.outcome.ok(), rr2.outcome.ok() ? "" : qPrintable(rr2.outcome.error().toLogString()));
        QCOMPARE(rr2.outcome->verification, Verification::Skipped);
        QVERIFY(QFileInfo::exists(out2));
        QVERIFY(QFileInfo::exists(img));
        QVERIFY(!rr2.outcome->details.contains(QStringLiteral("sourceDeleted")));
    }

    // A difference found before the cancel still fails the build: the new
    // image is not saved (not even unverified) and the previous one stays.
    void aMismatchFoundBeforeTheCancelFailsTheBuild()
    {
        Env env;
        SettingsStore store(env.schema, m_appData.filePath(QStringLiteral("vbad/settings.json")));
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString src = makeGame(tmp, 16 * 1024 * 1024);
        const QString out = tmp.filePath(QStringLiteral("game.ffpfsc"));
        const QByteArray previous("the image that was here before");
        QVERIFY(pfstest::writeFile(out, previous, -1));
        auto job = env.jobs.create(kinds::kBuild, params({{QStringLiteral("source"), src}, {QStringLiteral("output"), out},
                                                          {QStringLiteral("replaceExisting"), true}}));
        CancellationSource cs;
        std::atomic<bool> changed{false}, cancelled{false};
        auto rr = runJob(job, store.snapshot(), cs.token(), [&](const ProgressSnapshot& s) {
            if (s.phaseIndex != 2)
                return;
            // Files are compared in name order: Audio/BGM.at9 changes before it
            // is compared, the cancel comes while data/big.bin is.
            if (!changed.exchange(true)) {
                QFile f(src + QStringLiteral("/Audio/BGM.at9"));
                char c = 0;
                if (f.open(QIODevice::ReadWrite) && f.seek(1000) && f.getChar(&c) && f.seek(1000))
                    f.putChar(char(c ^ 0x55));
            } else if (s.done > 1024 * 1024 && !cancelled.exchange(true)) {
                cs.cancel();
            }
        });
        QVERIFY(cancelled);
        QVERIFY2(rr.outcome.ok(), rr.outcome.ok() ? "" : qPrintable(rr.outcome.error().toLogString()));
        QCOMPARE(rr.outcome->verification, Verification::Failed);
        QVERIFY2(rr.outcome->verificationDetail.contains(QStringLiteral("Audio/BGM.at9")), qPrintable(rr.outcome->verificationDetail));
        QVERIFY2(rr.outcome->verificationDetail.contains(QStringLiteral("cancelled")), qPrintable(rr.outcome->verificationDetail));
        QVERIFY(!rr.outcome->details.contains(QStringLiteral("verificationCancelled")));
        QVERIFY(rr.outcome->outputs.isEmpty());
        QCOMPARE(pfstest::readAll(out), previous);
        QVERIFY(!QFileInfo::exists(PartFile::partPathFor(out)));
    }

    // The image it replaces is open in another program: the finished image is
    // kept next to it instead of being deleted with its .part.
    void finishedImageIsKeptWhenTheOldOneCannotBeReplaced()
    {
#ifndef Q_OS_WIN
        QSKIP("An open file can be replaced on this platform.");
#endif
        Env env;
        SettingsStore store(env.schema, m_appData.filePath(QStringLiteral("locked/settings.json")));
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString src = makeGame(tmp, 2 * 1024 * 1024);
        const QString out = tmp.filePath(QStringLiteral("locked.ffpfsc"));
        const QByteArray previous("the image that was here before");
        QVERIFY(pfstest::writeFile(out, previous, -1));
        QFile holder(out);
        QVERIFY(holder.open(QIODevice::ReadOnly)); // opened without delete sharing
        auto rr = runJob(env.jobs.create(kinds::kBuild, params({{QStringLiteral("source"), src}, {QStringLiteral("output"), out},
                                                                {QStringLiteral("replaceExisting"), true}})),
                         store.snapshot());
        holder.close();
        QVERIFY(!rr.outcome.ok());
        const QString kept = tmp.filePath(QStringLiteral("locked (new).ffpfsc"));
        QVERIFY2(rr.outcome.error().message.contains(QStringLiteral("built and verified")), qPrintable(rr.outcome.error().toLogString()));
        QVERIFY2(rr.outcome.error().message.contains(QDir::toNativeSeparators(kept)), qPrintable(rr.outcome.error().toLogString()));
        QCOMPARE(pfstest::readAll(out), previous);
        QVERIFY(!QFileInfo::exists(PartFile::partPathFor(out)));
        auto check = runJob(env.jobs.create(kinds::kVerify, params({{QStringLiteral("image"), kept}, {QStringLiteral("source"), src}})),
                            store.snapshot());
        QVERIFY2(check.outcome.ok() && check.outcome->verification == Verification::Passed,
                 check.outcome.ok() ? qPrintable(check.outcome->verificationDetail) : qPrintable(check.outcome.error().toLogString()));

        // The same after a Cancel during the verification: the image is kept
        // unverified next to the old one, and the error saying where is the
        // job's own (the token says so), not a plain "Cancelled" (the queue
        // keeps it: JobQueue tests, aKeptJobsLaterErrorIsReported).
        const QString src2 = makeGame(tmp, 16 * 1024 * 1024);
        QVERIFY(QFile::remove(kept));
        QVERIFY(holder.open(QIODevice::ReadOnly));
        CancellationSource cs;
        std::atomic<bool> cancelled{false};
        auto rr2 = runJob(env.jobs.create(kinds::kBuild, params({{QStringLiteral("source"), src2}, {QStringLiteral("output"), out},
                                                                 {QStringLiteral("replaceExisting"), true}})),
                          store.snapshot(), cs.token(), [&](const ProgressSnapshot& s) {
                              if (s.phaseIndex == 2 && s.done > 1024 * 1024 && !cancelled.exchange(true))
                                  cs.cancel();
                          });
        holder.close();
        QVERIFY(cancelled);
        QVERIFY(!rr2.outcome.ok());
        QVERIFY2(!rr2.outcome.error().isCancelled(), qPrintable(rr2.outcome.error().toLogString()));
        QVERIFY(cs.token().finishedWorkKept());
        QVERIFY2(rr2.outcome.error().message.contains(QDir::toNativeSeparators(kept)), qPrintable(rr2.outcome.error().toLogString()));
        QVERIFY2(!rr2.outcome.error().message.contains(QStringLiteral("verified")), qPrintable(rr2.outcome.error().toLogString()));
        QCOMPARE(pfstest::readAll(out), previous);
        QVERIFY(QFileInfo(kept).size() > 1024 * 1024);
        QVERIFY(!QFileInfo::exists(out + QStringLiteral(".part")));
    }

    // ampr.index.local: the folder is checked again when the job runs (asset
    // packs may have appeared since it was queued), and a new index replaces
    // the folder's own only once it has read back as written.
    void amprIndexToolChecksTheFolderAgainAndReadsBackFirst()
    {
        Env env;
        SettingsStore store(env.schema, m_appData.filePath(QStringLiteral("ampr/settings.json")));
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString app0 = tmp.filePath(QStringLiteral("app0"));
        QVERIFY(pfstest::writeFile(app0 + QStringLiteral("/eboot.bin"), QByteArray(5000, 'e'), -1));
        QVERIFY(pfstest::writeFile(app0 + QStringLiteral("/fakelib/libSceAmpr.sprx"), QByteArray(300, 'a'), -1));
        QVERIFY(pfstest::writeFile(app0 + QStringLiteral("/data/a.bin"), QByteArray(7000, 'd'), -1));
        const QString index = app0 + QStringLiteral("/ampr_emu.index");
        QVERIFY(pfstest::writeFile(index, QByteArray("the folder's own index"), -1));

        auto job = env.jobs.create(kAmprLocalJob, params({{QStringLiteral("folder"), app0}}));
        QVERIFY(job.ok());
        QVERIFY((*job)->validate(store.snapshot()).ok());
        QVERIFY(pfstest::writeFile(app0 + QStringLiteral("/ampr_assets.index"), QByteArray("packs"), -1));
        auto refused = runJob(**job, store.snapshot());
        QVERIFY(!refused.outcome.ok());
        QCOMPARE(refused.outcome.error().code, ErrorCode::InvalidArgument);
        QCOMPARE(pfstest::readAll(index), QByteArray("the folder's own index"));
        QVERIFY(QFile::remove(app0 + QStringLiteral("/ampr_assets.index")));

        auto rr = runJob(env.jobs.create(kAmprLocalJob, params({{QStringLiteral("folder"), app0}})), store.snapshot());
        QVERIFY2(rr.outcome.ok(), rr.outcome.ok() ? "" : qPrintable(rr.outcome.error().toLogString()));
        QCOMPARE(rr.outcome->verification, Verification::Passed);
        QCOMPARE(rr.outcome->outputs, QStringList{QDir(app0).filePath(QStringLiteral("ampr_emu.index"))});
        QVERIFY(pfstest::readAll(index) != QByteArray("the folder's own index"));
        QVERIFY(!QFileInfo::exists(PartFile::partPathFor(index)));

        // With the shares the queue declares, the scan (the long part) has the
        // bar and writing the one small index file only its end.
        auto again = env.jobs.create(kAmprLocalJob, params({{QStringLiteral("folder"), app0}}));
        QVERIFY(again.ok());
        double atWrite = -1.0;
        ProgressReporter reporter(
            [&](const ProgressSnapshot& s) {
                if (s.phaseIndex == 1 && atWrite < 0)
                    atWrite = s.overallFraction;
            },
            0);
        reporter.setPhases((*again)->phases(), (*again)->phaseWeights());
        QTemporaryDir jobTemp;
        JobContext ctx;
        ctx.jobId = QStringLiteral("test");
        ctx.progress = &reporter;
        ctx.log = Log((*again)->kind(), ctx.jobId);
        ctx.settings = store.snapshot();
        ctx.tempDir = jobTemp.path();
        QVERIFY((*again)->run(ctx).ok());
        QVERIFY2(std::abs(atWrite - 0.95) < 0.001, qPrintable(QString::number(atWrite)));
        QVERIFY(reporter.snapshot().overallFraction > 0.99);
    }

    void verificationFailureRemovesImage_data()
    {
        QTest::addColumn<bool>("replacing");
        QTest::newRow("new image") << false;
        QTest::newRow("replacing an image") << true;
    }

    // A failed check fails the job, removes the new image and keeps the
    // source; an image the new one was to replace is still there, byte for
    // byte.
    void verificationFailureRemovesImage()
    {
        QFETCH(bool, replacing);
        Env env;
        SettingsStore store(env.schema, m_appData.filePath(QStringLiteral("f/settings.json")));
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString img = makeImage(tmp);
        const QByteArray original = pfstest::readAll(img);
        QVERIFY(original.size() > 1000);
        const QString out = tmp.filePath(QStringLiteral("out.ffpfsc"));
        const QByteArray previous("the image that was here before");
        if (replacing)
            QVERIFY(pfstest::writeFile(out, previous, -1));
        auto norm = store.normalizeOverrides({{keys::kDeleteSourceAfterConvert, true}});
        QVERIFY(norm.ok());
        auto job = env.jobs.create(kinds::kBuild, params({{QStringLiteral("source"), img}, {QStringLiteral("output"), out},
                                                          {QStringLiteral("replaceExisting"), replacing}}));
        bool changed = false;
        auto rr = runJob(job, store.snapshot(*norm), {}, [&](const ProgressSnapshot& s) {
            if (s.phaseIndex == 2 && !changed) { // the source changes after it was packed
                changed = true;
                QByteArray b = original;
                b[1000] = char(b[1000] ^ 1);
                (void)writeFileAtomic(img, b);
            }
        });
        QVERIFY(changed);
        QVERIFY2(rr.outcome.ok(), rr.outcome.ok() ? "" : qPrintable(rr.outcome.error().toLogString()));
        QCOMPARE(rr.outcome->verification, Verification::Failed);
        QVERIFY2(rr.outcome->verificationDetail.contains(QStringLiteral("Different content")),
                 qPrintable(rr.outcome->verificationDetail));
        QVERIFY(rr.outcome->outputs.isEmpty());
        if (replacing)
            QCOMPARE(pfstest::readAll(out), previous);
        else
            QVERIFY(!QFileInfo::exists(out));
        QVERIFY(!QFileInfo::exists(PartFile::partPathFor(out)));
        QVERIFY(QFileInfo::exists(img)); // never deleted after a failed check
    }

    // The room for the image is checked before anything is written: a source
    // larger than the free space is refused at once, on any PC (the source is
    // a sparse file, so it costs nothing to make).
    void freeSpaceIsCheckedBeforeWriting()
    {
        Env env;
        SettingsStore store(env.schema, m_appData.filePath(QStringLiteral("space/settings.json")));
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString img = tmp.filePath(QStringLiteral("PPSA09999.exfat"));
        const qint64 size = QStorageInfo(tmp.path()).bytesAvailable() + 1024LL * 1024 * 1024;
        const qint64 allocated = fwtest::makeSparseFile(img, size, {{0, QByteArrayLiteral("not really an image")}});
        if (allocated < 0 || allocated > 64 * 1024 * 1024) {
            QFile::remove(img);
            QSKIP("this file system does not create sparse files");
        }
        const QString out = tmp.filePath(QStringLiteral("out/game.ffpfsc"));
        // What an interrupted build of this image left goes before the check,
        // which must count its space as free (no file is left after it).
        QVERIFY(pfstest::writeFile(PartFile::partPathFor(out), "left by a crash", -1));
        auto job = env.jobs.create(kinds::kBuild, params({{QStringLiteral("source"), img}, {QStringLiteral("output"), out}}));
        auto rr = runJob(job, store.snapshot());
        FWTEST_COMPARE_ERROR(rr.outcome, ErrorCode::InsufficientSpace);
        QVERIFY2(rr.outcome.error().message.startsWith(QStringLiteral("Not enough space on")), qPrintable(rr.outcome.error().message));
        QVERIFY(!QFileInfo::exists(out));
        QVERIFY(!QFileInfo::exists(PartFile::partPathFor(out)));
        QVERIFY(QFileInfo::exists(img)); // the source is untouched
    }

    void deleteSourceAfterConvert()
    {
        Env env;
        SettingsStore store(env.schema, m_appData.filePath(QStringLiteral("d/settings.json")));
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString img = makeImage(tmp);
        auto on = store.normalizeOverrides({{keys::kDeleteSourceAfterConvert, true}});
        QVERIFY(on.ok());
        auto job = env.jobs.create(kinds::kBuild, params({{QStringLiteral("source"), img},
                                                          {QStringLiteral("output"), tmp.filePath(QStringLiteral("a.ffpfsc"))}}));
        auto rr = runJob(job, store.snapshot(*on));
        QVERIFY(rr.outcome.ok());
        QCOMPARE(rr.outcome->verification, Verification::Passed);
        QVERIFY(!QFileInfo::exists(img));
        QVERIFY(rr.outcome->details.value(QStringLiteral("sourceDeleted")).toBool());

        // Structure-only verification never deletes the source.
        const QString img2 = makeImage(tmp);
        auto structure = store.normalizeOverrides({{keys::kDeleteSourceAfterConvert, true}, {general::keys::kVerifyMode, QStringLiteral("structure")}});
        QVERIFY(structure.ok());
        auto job2 = env.jobs.create(kinds::kBuild, params({{QStringLiteral("source"), img2},
                                                           {QStringLiteral("output"), tmp.filePath(QStringLiteral("b.ffpfsc"))}}));
        auto rr2 = runJob(job2, store.snapshot(*structure));
        QVERIFY(rr2.outcome.ok());
        QVERIFY(QFileInfo::exists(img2));
        QVERIFY(!rr2.outcome->warnings.isEmpty());

        // Folders are never deleted.
        const QString folder = makeGame(tmp);
        auto job3 = env.jobs.create(kinds::kBuild, params({{QStringLiteral("source"), folder},
                                                           {QStringLiteral("output"), tmp.filePath(QStringLiteral("c.ffpfsc"))}}));
        QVERIFY(runJob(job3, store.snapshot(*on)).outcome.ok());
        QVERIFY(QFileInfo(folder).isDir());

        // Verification off: reported as skipped.
        auto offv = store.normalizeOverrides({{general::keys::kVerifyAfterBuild, false}});
        QVERIFY(offv.ok());
        auto job4 = env.jobs.create(kinds::kBuild, params({{QStringLiteral("source"), folder},
                                                           {QStringLiteral("output"), tmp.filePath(QStringLiteral("d.ffpfsc"))}}));
        auto rr4 = runJob(job4, store.snapshot(*offv));
        QVERIFY(rr4.outcome.ok());
        QCOMPARE(rr4.outcome->verification, Verification::Skipped);
    }

    // The bar of a build runs to the end with checking on and off: with
    // "Checking PFS images" off, Verifying (which does not run) takes no share
    // (the queue declares the shares for the job's settings). From a folder
    // and from an image.
    void buildRunsItsBarToTheEnd()
    {
        Env env;
        SettingsStore store(env.schema, m_appData.filePath(QStringLiteral("bar/settings.json")));
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString folder = makeGame(tmp);
        const QString image = makeImage(tmp);
        auto off = store.normalizeOverrides({{general::keys::kVerifyAfterBuild, false}});
        QVERIFY(off.ok());
        int n = 0;
        for (const auto& [name, values] : {std::pair{"checking on", store.snapshot()}, std::pair{"checking off", store.snapshot(*off)}}) {
            for (const QString& source : {folder, image}) {
                const QString what = QStringLiteral("%1, %2").arg(QLatin1String(name), QFileInfo(source).fileName());
                fwtest::ProgressAudit audit;
                auto job = env.jobs.create(kinds::kBuild, params({{QStringLiteral("source"), source},
                                                                  {QStringLiteral("output"), tmp.filePath(QStringLiteral("bar-%1.ffpfsc").arg(++n))}}));
                auto rr = runJob(job, values, {}, {}, &audit);
                QVERIFY2(rr.outcome.ok(), rr.outcome.ok() ? "" : qPrintable(what + u' ' + rr.outcome.error().toLogString()));
                QVERIFY2(rr.outcome->verification != Verification::Failed, qPrintable(what));
                QVERIFY2(audit.problems().isEmpty(), qPrintable(what + QStringLiteral(": ") + audit.problems().join(u'\n')));
            }
        }
    }

    // Audit H2 through the jobs: leftover images in the dump are left out and
    // the standalone verify job uses the same exclusions.
    void verifyJobUsesBuildExclusions()
    {
        Env env;
        SettingsStore store(env.schema, m_appData.filePath(QStringLiteral("h2/settings.json")));
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString src = makeGame(tmp);
        QVERIFY(pfstest::writeFile(src + QStringLiteral("/old_build.ffpkg"), "leftover image", -1));
        QVERIFY(QDir().mkpath(src + QStringLiteral("/saves/empty")));
        QVERIFY(pfstest::writeFile(src + QStringLiteral("/fakelib/libSceAmpr.sprx"), "prx", -1));
        const QString out = tmp.filePath(QStringLiteral("game.ffpfsc"));
        auto job = env.jobs.create(kinds::kBuild, params({{QStringLiteral("source"), src}, {QStringLiteral("output"), out}}));
        auto rr = runJob(job, store.snapshot());
        QVERIFY(rr.outcome.ok());
        QCOMPARE(rr.outcome->verification, Verification::Passed);

        // Later, standalone: same plan (AMPR index and .pfskeep markers included).
        auto vj = env.jobs.create(kinds::kVerify, params({{QStringLiteral("image"), out}, {QStringLiteral("source"), src}}));
        auto vr = runJob(vj, store.snapshot());
        QVERIFY(vr.outcome.ok());
        QCOMPARE(vr.outcome->verification, Verification::Passed);
        QVERIFY(vr.outcome->warnings.isEmpty());

        // The generated index stores the files' times: a dump that was copied
        // or touched since the build still verifies, with a note.
        QVERIFY(pfstest::setMtime(src + QStringLiteral("/fakelib/libSceAmpr.sprx"), 1650000000));
        auto touched = runJob(env.jobs.create(kinds::kVerify, params({{QStringLiteral("image"), out}, {QStringLiteral("source"), src}})),
                              store.snapshot());
        QVERIFY(touched.outcome.ok());
        QCOMPARE(touched.outcome->verification, Verification::Passed);
        QVERIFY2(touched.outcome->warnings.join(u'\n').contains(QLatin1String("other file times")),
                 qPrintable(touched.outcome->warnings.join(u'\n')));

        // A real change is still caught.
        QVERIFY(pfstest::writeFile(src + QStringLiteral("/data/extra.bin"), "new", -1));
        auto vr2 = runJob(env.jobs.create(kinds::kVerify, params({{QStringLiteral("image"), out}, {QStringLiteral("source"), src}})),
                          store.snapshot());
        QVERIFY(vr2.outcome.ok());
        QCOMPARE(vr2.outcome->verification, Verification::Failed);
        // The generated index lists every file, so it differs as well.
        QStringList problems;
        for (const auto& v : vr2.outcome->details.value(QStringLiteral("problems")).toArray())
            problems << v.toString();
        QVERIFY2(problems.contains(QStringLiteral("Missing from the image: data/extra.bin")), qPrintable(problems.join(u'\n')));
        QVERIFY(problems.first().startsWith(QStringLiteral("Different size: ampr_emu.index")));
        QCOMPARE(problems.size(), 2);

        // A damaged image is reported as CorruptData.
        QFile f(out);
        QVERIFY(f.open(QIODevice::ReadWrite));
        QVERIFY(f.seek(8));
        QVERIFY(f.write("XXXXXXXX", 8) == 8);
        f.close();
        auto vr3 = runJob(env.jobs.create(kinds::kVerify, params({{QStringLiteral("image"), out}})), store.snapshot());
        QVERIFY(!vr3.outcome.ok());
        FWTEST_COMPARE_ERROR(vr3.outcome, ErrorCode::CorruptData);
    }

    // Lazy_MkPFS pack_folder: a wrapper folder is searched for the game (eboot.bin).
    void findsGameRootInWrapperFolder()
    {
        Env env;
        SettingsStore store(env.schema, m_appData.filePath(QStringLiteral("root/settings.json")));
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString inner = makeGame(tmp);
        const QString wrapper = tmp.filePath(QStringLiteral("wrapper"));
        QVERIFY(QDir().mkpath(wrapper));
        QVERIFY(pfstest::writeFile(wrapper + QStringLiteral("/readme.txt"), "not part of the game", -1));
        QVERIFY(QDir().rename(inner, wrapper + QStringLiteral("/PPSA01234-app")));
        const QString a = tmp.filePath(QStringLiteral("a.ffpfsc"));
        const QString b = tmp.filePath(QStringLiteral("b.ffpfsc"));
        auto ra = runJob(env.jobs.create(kinds::kBuild, params({{QStringLiteral("source"), wrapper}, {QStringLiteral("output"), a},
                                                                  {QStringLiteral("timestamp"), 1700000000}})),
                         store.snapshot());
        QVERIFY(ra.outcome.ok());
        QCOMPARE(ra.outcome->details.value(QStringLiteral("gameRoot")).toString(),
                 QDir::cleanPath(wrapper + QStringLiteral("/PPSA01234-app")));
        auto rb = runJob(env.jobs.create(kinds::kBuild, params({{QStringLiteral("source"), wrapper + QStringLiteral("/PPSA01234-app")},
                                                                  {QStringLiteral("output"), b},
                                                                  {QStringLiteral("timestamp"), 1700000000}})),
                         store.snapshot());
        QVERIFY(rb.outcome.ok());
        QCOMPARE(pfstest::sha256(a), pfstest::sha256(b));
        auto vr = runJob(env.jobs.create(kinds::kVerify, params({{QStringLiteral("image"), a}, {QStringLiteral("source"), wrapper}})),
                         store.snapshot());
        QVERIFY(vr.outcome.ok());
        QCOMPARE(vr.outcome->verification, Verification::Passed);
        // Without detection the wrapper itself is packed.
        const QString c = tmp.filePath(QStringLiteral("c.ffpfsc"));
        auto rc = runJob(env.jobs.create(kinds::kBuild, params({{QStringLiteral("source"), wrapper}, {QStringLiteral("output"), c},
                                                                  {QStringLiteral("findGameRoot"), false}})),
                         store.snapshot());
        QVERIFY(rc.outcome.ok());
        QCOMPARE(rc.outcome->details.value(QStringLiteral("gameRoot")).toString(), QDir::cleanPath(wrapper));
    }

    void extractJob()
    {
        Env env;
        SettingsStore store(env.schema, m_appData.filePath(QStringLiteral("x/settings.json")));
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString src = makeGame(tmp);
        const QString out = tmp.filePath(QStringLiteral("game.ffpfsc"));
        QVERIFY(runJob(env.jobs.create(kinds::kBuild, params({{QStringLiteral("source"), src}, {QStringLiteral("output"), out}})),
                       store.snapshot())
                    .outcome.ok());
        const QString dest = tmp.filePath(QStringLiteral("unpacked"));
        auto rr = runJob(env.jobs.create(kinds::kExtract, params({{QStringLiteral("image"), out}, {QStringLiteral("output"), dest}})),
                         store.snapshot());
        QVERIFY2(rr.outcome.ok(), rr.outcome.ok() ? "" : qPrintable(rr.outcome.error().toLogString()));
        QVERIFY(rr.outcome->details.value(QStringLiteral("nested")).toBool());
        auto tree = scanTree(src, CancellationToken());
        QVERIFY(tree.ok());
        int compared = 0;
        for (const FileEntry& e : tree->entries) {
            if (e.isDir)
                continue;
            QVERIFY2(QFileInfo(dest + u'/' + e.relativePath).isFile(), qPrintable(e.relativePath));
            QCOMPARE(pfstest::readAll(dest + u'/' + e.relativePath), pfstest::readAll(src + u'/' + e.relativePath));
            ++compared;
        }
        QVERIFY(compared > 0);
    }

private:
    QString makeGame(QTemporaryDir& tmp, qint64 extra = 0)
    {
        const QString src = tmp.filePath(QStringLiteral("game%1").arg(m_counter++));
        // A source that could not be written fails the test here; the other
        // test functions still run.
        bool ok = pfstest::makeTree(src, m_spec.value(QStringLiteral("trees")).toObject().value(QStringLiteral("game")).toArray(),
                                    1600000000);
        if (ok && extra > 0)
            ok = pfstest::writeFile(src + QStringLiteral("/data/big.bin"), pfstest::genContent(QStringLiteral("mixed"), extra, 3), -1);
        if (!ok)
            QTest::qFail("the game folder could not be created", __FILE__, __LINE__);
        return src;
    }

    QString makeImage(QTemporaryDir& tmp)
    {
        const QString img = tmp.filePath(QStringLiteral("PPSA0%1.exfat").arg(1000 + m_counter++));
        if (!pfstest::writeFile(img, pfstest::genContent(QStringLiteral("mixed"), 3 * 1024 * 1024 + 99, 4), -1))
            QTest::qFail("the source image could not be created", __FILE__, __LINE__);
        return img;
    }

    QTemporaryDir m_appData;
    QJsonObject m_spec;
    int m_counter = 0;
};

QTEST_GUILESS_MAIN(TestPfsJobs)
#include "tst_pfs_jobs.moc"
