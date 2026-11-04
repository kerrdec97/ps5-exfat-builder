// PFS engine: settings (declared, read, legacy import), the three queue jobs
// (build / verify / extract), cancellation and failure clean-up, and the
// audit rules C1 (a failed check is a failure, the source is kept) and H2
// (exclusions never cause false verification failures).

#include "engines/general/Module.h"
#include "engines/pfs/Module.h"
#include "engines/pfs/PfsJobs.h"

#include "core/FileUtil.h"
#include "engines/pfs/PfsTestUtil.h"
#include "jobs/JobQueue.h"
#include "support/TestSupport.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

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

// Runs a job synchronously on this thread, like the queue's worker would.
RunResult runJob(Job& job, const SettingsValues& settings, const CancellationToken& token = {},
                 std::function<void(const ProgressSnapshot&)> onProgress = {})
{
    RunResult rr;
    ProgressReporter reporter(
        [&](const ProgressSnapshot& s) {
            if (rr.phasesSeen.isEmpty() || rr.phasesSeen.back() != s.phase)
                rr.phasesSeen << s.phase;
            if (onProgress)
                onProgress(s);
        },
        0);
    reporter.setPhases(job.phases());
    QTemporaryDir temp;
    JobContext ctx;
    ctx.jobId = QStringLiteral("test");
    ctx.token = token;
    ctx.progress = &reporter;
    ctx.log = Log(job.kind(), ctx.jobId);
    ctx.settings = settings;
    ctx.tempDir = temp.path();
    rr.outcome = job.run(ctx);
    return rr;
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
                                     keys::kDeleteSourceAfterConvert, keys::kVerify};
        int declared = 0;
        for (const auto* d : env.schema->all()) {
            if (!d->key.startsWith(QStringLiteral("pfs.")))
                continue;
            ++declared;
            QVERIFY2(pfsKeys.contains(d->key), qPrintable(d->key));
            QVERIFY2(!d->description.isEmpty(), qPrintable(d->key));
            QVERIFY2(env.schema->normalize(d->key, d->defaultValue).ok(), qPrintable(d->key));
            QCOMPARE(d->section, d->key == keys::kVerify ? QStringLiteral("Verification") : QStringLiteral("PFS"));
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
                                              {keys::kVerify, QStringLiteral("structure")}});
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
        QCOMPARE(o.threads, 3);
        const PlanOptions po = planOptionsFrom(s, 1);
        QCOMPARE(po.excludes, QStringList{QStringLiteral("x")});
        QVERIFY(!po.keepEmptyFolders);
        QCOMPARE(po.ampr, AmprMode::Refresh);
        QCOMPARE(verifyDepthFrom(s), std::optional<VerifyDepth>(VerifyDepth::Structure));
        auto off = store.normalizeOverrides({{keys::kVerify, QStringLiteral("off")}, {keys::kAmprIndex, QStringLiteral("off")}});
        QVERIFY(!verifyDepthFrom(store.snapshot(*off)));
        QCOMPARE(planOptionsFrom(store.snapshot(*off), 0).ampr, AmprMode::Off);
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
        const QString src = source == u"folder" ? makeGame(tmp) : makeImage(tmp);
        const QString out = tmp.filePath(QStringLiteral("out/") + output);
        auto job = env.jobs.create(kinds::kBuild, params({{QStringLiteral("source"), src}, {QStringLiteral("output"), out}}));
        QVERIFY(job.ok());
        QVERIFY((*job)->validate(store.snapshot()).ok());
        auto rr = runJob(**job, store.snapshot());
        QVERIFY2(rr.outcome.ok(), rr.outcome.ok() ? "" : qPrintable(rr.outcome.error().toLogString()));
        const JobOutcome& o = *rr.outcome;
        QCOMPARE(o.verification, Verification::Passed);
        QVERIFY2(o.verificationDetail.contains(QStringLiteral("byte for byte")), qPrintable(o.verificationDetail));
        QCOMPARE(o.outputs, QStringList{QDir::cleanPath(out)});
        QVERIFY(QFileInfo(out).isFile());
        QVERIFY(!QFileInfo::exists(out + QStringLiteral(".part")));
        QCOMPARE(rr.phasesSeen, (QStringList{QStringLiteral("Scanning"), QStringLiteral("Compressing and writing"),
                                             QStringLiteral("Verifying")}));
        QVERIFY(QFileInfo::exists(src)); // never deleted by default

        // The standalone verify job agrees, with and without the source.
        auto vj = env.jobs.create(kinds::kVerify, params({{QStringLiteral("image"), out}, {QStringLiteral("source"), src}}));
        auto vr = runJob(**vj, store.snapshot());
        QVERIFY2(vr.outcome.ok(), vr.outcome.ok() ? "" : qPrintable(vr.outcome.error().toLogString()));
        QCOMPARE(vr.outcome->verification, Verification::Passed);
        auto vs = env.jobs.create(kinds::kVerify, params({{QStringLiteral("image"), out}}));
        auto vsr = runJob(**vs, store.snapshot());
        QVERIFY(vsr.outcome.ok());
        QCOMPARE(vsr.outcome->verification, Verification::Passed);
    }

    void buildThroughQueue()
    {
        Env env;
        SettingsStore store(env.schema, m_appData.filePath(QStringLiteral("q/settings.json")));
        JobQueue q(store, env.jobs);
        QTemporaryDir tmp;
        const QString src = makeGame(tmp);
        const QString out = tmp.filePath(QStringLiteral("game.ffpfsc"));
        auto id = q.enqueue(kinds::kBuild, params({{QStringLiteral("source"), src}, {QStringLiteral("output"), out}}),
                            {{keys::kCompressionLevel, 1}});
        QVERIFY2(id.ok(), id.ok() ? "" : qPrintable(id.error().message));
        QVERIFY(QTest::qWaitFor([&] {
            auto r = q.record(*id);
            return r && isTerminal(r->state);
        }, 60000));
        auto rec = q.record(*id);
        QCOMPARE(rec->state, JobState::Succeeded);
        QCOMPARE(rec->outcome->verification, Verification::Passed);
        QVERIFY(QFileInfo(out).isFile());

        // Output exists: refused unless replaceExisting.
        auto again = q.enqueue(kinds::kBuild, params({{QStringLiteral("source"), src}, {QStringLiteral("output"), out}}));
        QVERIFY(!again.ok());
        QCOMPARE(again.error().code, ErrorCode::AlreadyExists);
    }

    void validateRules()
    {
        Env env;
        SettingsStore store(env.schema, m_appData.filePath(QStringLiteral("v/settings.json")));
        QTemporaryDir tmp;
        const QString src = makeGame(tmp);
        auto check = [&](const QJsonObject& p) { return (*env.jobs.create(kinds::kBuild, p))->validate(store.snapshot()); };
        auto inside = check(params({{QStringLiteral("source"), src}, {QStringLiteral("output"), src + QStringLiteral("/x.ffpfsc")}}));
        QVERIFY(!inside.ok());
        QCOMPARE(inside.error().code, ErrorCode::InvalidArgument);
        QCOMPARE(check(params({{QStringLiteral("source"), tmp.filePath(QStringLiteral("nope"))},
                               {QStringLiteral("output"), tmp.filePath(QStringLiteral("x.ffpfsc"))}}))
                     .error()
                     .code,
                 ErrorCode::NotFound);
        QCOMPARE(check(params({{QStringLiteral("source"), src}, {QStringLiteral("output"), tmp.filePath(QStringLiteral("x"))},
                               {QStringLiteral("format"), QStringLiteral("zip")}}))
                     .error()
                     .code,
                 ErrorCode::InvalidArgument);

        // replaceExisting: the old image is replaced only after the new one is verified.
        const QString out = tmp.filePath(QStringLiteral("game.ffpfsc"));
        QVERIFY(pfstest::writeFile(out, "old image", -1));
        QCOMPARE(check(params({{QStringLiteral("source"), src}, {QStringLiteral("output"), out}})).error().code,
                 ErrorCode::AlreadyExists);
        auto job = env.jobs.create(kinds::kBuild, params({{QStringLiteral("source"), src}, {QStringLiteral("output"), out},
                                                          {QStringLiteral("replaceExisting"), true}}));
        auto rr = runJob(**job, store.snapshot());
        QVERIFY(rr.outcome.ok());
        QVERIFY(QFileInfo(out).size() > 1000);
    }

    void cancellationLeavesNoOutput_data()
    {
        QTest::addColumn<int>("phase");
        QTest::newRow("while writing") << 1;
        QTest::newRow("while verifying") << 2;
    }

    void cancellationLeavesNoOutput()
    {
        QFETCH(int, phase);
        Env env;
        SettingsStore store(env.schema, m_appData.filePath(QStringLiteral("c/settings.json")));
        QTemporaryDir tmp;
        const QString src = makeGame(tmp, 16 * 1024 * 1024);
        const QString out = tmp.filePath(QStringLiteral("game.ffpfsc"));
        auto job = env.jobs.create(kinds::kBuild, params({{QStringLiteral("source"), src}, {QStringLiteral("output"), out}}));
        CancellationSource cs;
        auto rr = runJob(**job, store.snapshot(), cs.token(), [&](const ProgressSnapshot& s) {
            if (s.phaseIndex == phase && s.done > 1024 * 1024)
                cs.cancel();
        });
        QVERIFY(!rr.outcome.ok());
        QVERIFY(rr.outcome.error().isCancelled());
        QVERIFY(!QFileInfo::exists(out));
        QVERIFY(!QFileInfo::exists(out + QStringLiteral(".part")));
        QCOMPARE(QDir(tmp.path()).entryList(QDir::Files).size(), 0);
    }

    // Audit C1: a failed check fails the job, removes the new image and keeps the source.
    void verificationFailureRemovesImage()
    {
        Env env;
        SettingsStore store(env.schema, m_appData.filePath(QStringLiteral("f/settings.json")));
        QTemporaryDir tmp;
        const QString img = makeImage(tmp);
        const QByteArray original = pfstest::readAll(img);
        const QString out = tmp.filePath(QStringLiteral("out.ffpfsc"));
        auto norm = store.normalizeOverrides({{keys::kDeleteSourceAfterConvert, true}});
        auto job = env.jobs.create(kinds::kBuild, params({{QStringLiteral("source"), img}, {QStringLiteral("output"), out}}));
        bool changed = false;
        auto rr = runJob(**job, store.snapshot(*norm), {}, [&](const ProgressSnapshot& s) {
            if (s.phaseIndex == 2 && !changed) { // the source changes after it was packed
                changed = true;
                QByteArray b = original;
                b[1000] = char(b[1000] ^ 1);
                (void)writeFileAtomic(img, b);
            }
        });
        QVERIFY(changed);
        QVERIFY(rr.outcome.ok());
        QCOMPARE(rr.outcome->verification, Verification::Failed);
        QVERIFY2(rr.outcome->verificationDetail.contains(QStringLiteral("Different content")),
                 qPrintable(rr.outcome->verificationDetail));
        QVERIFY(rr.outcome->outputs.isEmpty());
        QVERIFY(!QFileInfo::exists(out));
        QVERIFY(!QFileInfo::exists(out + QStringLiteral(".part")));
        QVERIFY(QFileInfo::exists(img)); // never deleted after a failed check

    }

    void deleteSourceAfterConvert()
    {
        Env env;
        SettingsStore store(env.schema, m_appData.filePath(QStringLiteral("d/settings.json")));
        QTemporaryDir tmp;
        const QString img = makeImage(tmp);
        auto on = store.normalizeOverrides({{keys::kDeleteSourceAfterConvert, true}});
        auto job = env.jobs.create(kinds::kBuild, params({{QStringLiteral("source"), img},
                                                          {QStringLiteral("output"), tmp.filePath(QStringLiteral("a.ffpfsc"))}}));
        auto rr = runJob(**job, store.snapshot(*on));
        QVERIFY(rr.outcome.ok());
        QCOMPARE(rr.outcome->verification, Verification::Passed);
        QVERIFY(!QFileInfo::exists(img));
        QVERIFY(rr.outcome->details.value(QStringLiteral("sourceDeleted")).toBool());

        // Structure-only verification never deletes the source.
        const QString img2 = makeImage(tmp);
        auto structure = store.normalizeOverrides({{keys::kDeleteSourceAfterConvert, true}, {keys::kVerify, QStringLiteral("structure")}});
        auto job2 = env.jobs.create(kinds::kBuild, params({{QStringLiteral("source"), img2},
                                                           {QStringLiteral("output"), tmp.filePath(QStringLiteral("b.ffpfsc"))}}));
        auto rr2 = runJob(**job2, store.snapshot(*structure));
        QVERIFY(rr2.outcome.ok());
        QVERIFY(QFileInfo::exists(img2));
        QVERIFY(!rr2.outcome->warnings.isEmpty());

        // Folders are never deleted.
        const QString folder = makeGame(tmp);
        auto job3 = env.jobs.create(kinds::kBuild, params({{QStringLiteral("source"), folder},
                                                           {QStringLiteral("output"), tmp.filePath(QStringLiteral("c.ffpfsc"))}}));
        QVERIFY(runJob(**job3, store.snapshot(*on)).outcome.ok());
        QVERIFY(QFileInfo(folder).isDir());

        // Verification off: reported as skipped.
        auto offv = store.normalizeOverrides({{keys::kVerify, QStringLiteral("off")}});
        auto job4 = env.jobs.create(kinds::kBuild, params({{QStringLiteral("source"), folder},
                                                           {QStringLiteral("output"), tmp.filePath(QStringLiteral("d.ffpfsc"))}}));
        auto rr4 = runJob(**job4, store.snapshot(*offv));
        QVERIFY(rr4.outcome.ok());
        QCOMPARE(rr4.outcome->verification, Verification::Skipped);
    }

    // Audit H2 through the jobs: leftover images in the dump are left out and
    // the standalone verify job uses the same exclusions.
    void verifyJobUsesBuildExclusions()
    {
        Env env;
        SettingsStore store(env.schema, m_appData.filePath(QStringLiteral("h2/settings.json")));
        QTemporaryDir tmp;
        const QString src = makeGame(tmp);
        QVERIFY(pfstest::writeFile(src + QStringLiteral("/old_build.ffpkg"), "leftover image", -1));
        QVERIFY(QDir().mkpath(src + QStringLiteral("/saves/empty")));
        QVERIFY(pfstest::writeFile(src + QStringLiteral("/fakelib/libSceAmpr.sprx"), "prx", -1));
        const QString out = tmp.filePath(QStringLiteral("game.ffpfsc"));
        auto job = env.jobs.create(kinds::kBuild, params({{QStringLiteral("source"), src}, {QStringLiteral("output"), out}}));
        auto rr = runJob(**job, store.snapshot());
        QVERIFY(rr.outcome.ok());
        QCOMPARE(rr.outcome->verification, Verification::Passed);

        // Later, standalone: same plan (AMPR index and .pfskeep markers included).
        auto vj = env.jobs.create(kinds::kVerify, params({{QStringLiteral("image"), out}, {QStringLiteral("source"), src}}));
        auto vr = runJob(**vj, store.snapshot());
        QVERIFY(vr.outcome.ok());
        QCOMPARE(vr.outcome->verification, Verification::Passed);

        // A real change is still caught.
        QVERIFY(pfstest::writeFile(src + QStringLiteral("/data/extra.bin"), "new", -1));
        auto vr2 = runJob(**env.jobs.create(kinds::kVerify, params({{QStringLiteral("image"), out}, {QStringLiteral("source"), src}})),
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
        auto vr3 = runJob(**env.jobs.create(kinds::kVerify, params({{QStringLiteral("image"), out}})), store.snapshot());
        QVERIFY(!vr3.outcome.ok());
        QCOMPARE(vr3.outcome.error().code, ErrorCode::CorruptData);
    }

    // Lazy_MkPFS pack_folder: a wrapper folder is searched for the game (eboot.bin).
    void findsGameRootInWrapperFolder()
    {
        Env env;
        SettingsStore store(env.schema, m_appData.filePath(QStringLiteral("root/settings.json")));
        QTemporaryDir tmp;
        const QString inner = makeGame(tmp);
        const QString wrapper = tmp.filePath(QStringLiteral("wrapper"));
        QVERIFY(QDir().mkpath(wrapper));
        QVERIFY(pfstest::writeFile(wrapper + QStringLiteral("/readme.txt"), "not part of the game", -1));
        QVERIFY(QDir().rename(inner, wrapper + QStringLiteral("/PPSA01234-app")));
        const QString a = tmp.filePath(QStringLiteral("a.ffpfsc"));
        const QString b = tmp.filePath(QStringLiteral("b.ffpfsc"));
        auto ra = runJob(**env.jobs.create(kinds::kBuild, params({{QStringLiteral("source"), wrapper}, {QStringLiteral("output"), a},
                                                                  {QStringLiteral("timestamp"), 1700000000}})),
                         store.snapshot());
        QVERIFY(ra.outcome.ok());
        QCOMPARE(ra.outcome->details.value(QStringLiteral("gameRoot")).toString(),
                 QDir::cleanPath(wrapper + QStringLiteral("/PPSA01234-app")));
        auto rb = runJob(**env.jobs.create(kinds::kBuild, params({{QStringLiteral("source"), wrapper + QStringLiteral("/PPSA01234-app")},
                                                                  {QStringLiteral("output"), b},
                                                                  {QStringLiteral("timestamp"), 1700000000}})),
                         store.snapshot());
        QVERIFY(rb.outcome.ok());
        QCOMPARE(pfstest::sha256(a), pfstest::sha256(b));
        auto vr = runJob(**env.jobs.create(kinds::kVerify, params({{QStringLiteral("image"), a}, {QStringLiteral("source"), wrapper}})),
                         store.snapshot());
        QVERIFY(vr.outcome.ok());
        QCOMPARE(vr.outcome->verification, Verification::Passed);
        // Without detection the wrapper itself is packed.
        const QString c = tmp.filePath(QStringLiteral("c.ffpfsc"));
        auto rc = runJob(**env.jobs.create(kinds::kBuild, params({{QStringLiteral("source"), wrapper}, {QStringLiteral("output"), c},
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
        const QString src = makeGame(tmp);
        const QString out = tmp.filePath(QStringLiteral("game.ffpfsc"));
        QVERIFY(runJob(**env.jobs.create(kinds::kBuild, params({{QStringLiteral("source"), src}, {QStringLiteral("output"), out}})),
                       store.snapshot())
                    .outcome.ok());
        const QString dest = tmp.filePath(QStringLiteral("unpacked"));
        auto rr = runJob(**env.jobs.create(kinds::kExtract, params({{QStringLiteral("image"), out}, {QStringLiteral("output"), dest}})),
                         store.snapshot());
        QVERIFY2(rr.outcome.ok(), rr.outcome.ok() ? "" : qPrintable(rr.outcome.error().toLogString()));
        QVERIFY(rr.outcome->details.value(QStringLiteral("nested")).toBool());
        auto tree = scanTree(src, CancellationToken());
        QVERIFY(tree.ok());
        for (const FileEntry& e : tree->entries)
            if (!e.isDir)
                QCOMPARE(pfstest::readAll(dest + u'/' + e.relativePath), pfstest::readAll(src + u'/' + e.relativePath));
    }

private:
    QString makeGame(QTemporaryDir& tmp, qint64 extra = 0)
    {
        const QString src = tmp.filePath(QStringLiteral("game%1").arg(m_counter++));
        if (!pfstest::makeTree(src, m_spec.value(QStringLiteral("trees")).toObject().value(QStringLiteral("game")).toArray(),
                               1600000000))
            qFatal("tree");
        if (extra > 0)
            pfstest::writeFile(src + QStringLiteral("/data/big.bin"), pfstest::genContent(QStringLiteral("mixed"), extra, 3), -1);
        return src;
    }

    QString makeImage(QTemporaryDir& tmp)
    {
        const QString img = tmp.filePath(QStringLiteral("PPSA0%1.exfat").arg(1000 + m_counter++));
        pfstest::writeFile(img, pfstest::genContent(QStringLiteral("mixed"), 3 * 1024 * 1024 + 99, 4), -1);
        return img;
    }

    QTemporaryDir m_appData;
    QJsonObject m_spec;
    int m_counter = 0;
};

QTEST_GUILESS_MAIN(TestPfsJobs)
#include "tst_pfs_jobs.moc"
