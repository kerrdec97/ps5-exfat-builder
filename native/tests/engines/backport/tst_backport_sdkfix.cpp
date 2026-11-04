// SDK Fixer, SDK marker only (docs/BACKPORT_PARITY.md §2.18, S24, P14): copy
// mode leaves the source untouched and removes a half-written copy; in-place
// mode writes a verified backup ZIP, applies through a journal (rollback,
// crash recovery) and keeps an undo record; fakelib off, libc rename off,
// param files unchanged; nothing to do when already at the target.

#include "BackportTestEnv.h"

#include "core/Zip.h"
#include "engines/backport/ApplyJournal.h"
#include "engines/backport/BackportJobs.h"
#include "engines/backport/BackportManifest.h"
#include "engines/backport/BackportPlanner.h"
#include "engines/backport/BackportSettings.h"
#include "engines/backport/Module.h"
#include "engines/backport/RestoreService.h"
#include "engines/backport/RunReport.h"
#include "engines/backport/SceParam.h"
#include "engines/backport/SdkFixer.h"
#include "engines/backport/SelfReader.h"
#include "support/TestSupport.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QTest>

using namespace fw;
using namespace fw::backport;
using namespace fwtest::bp;

namespace {

const QByteArray kNidFrom = "4h6F1LLbTiw#A#B";
const QByteArray kNidTo = "IWIBBdTHit4#A#B";

SdkFixOptions fixOptions(const Env& e, SdkFixMode mode, int sdk)
{
    SdkFixOptions o;
    o.folder = e.game;
    o.targetSdk = sdk;
    o.mode = mode;
    o.outputDir = e.tmp.filePath(QStringLiteral("out"));
    QDir().mkpath(o.outputDir);
    o.originalsDir = e.orig;
    o.journalDir = e.journals;
    o.freeSpaceMarginBytes = 0;
    return o;
}

int sdkOf(const QString& path)
{
    const QByteArray d = readBytes(path);
    if (selfio::hasSelfMagic(d)) {
        auto u = selfio::unwrap(d);
        return u.ok() ? elfSdkMajor(u.value()).value_or(-1) : -2;
    }
    return elfSdkMajor(d).value_or(-1);
}

QStringList zipNames(const QString& path)
{
    zip::Reader r;
    if (!r.open(path).ok())
        return {};
    QStringList out;
    for (const zip::Entry& e : r.entries())
        out << e.name;
    return out;
}

Status failAt(const QString& wantPoint, const QString& wantItem, const QString& point, const QString& item)
{
    if (point == wantPoint && item == wantItem)
        return makeError(Code::ApplyFailed, QStringLiteral("injected at %1 %2").arg(point, item));
    return success();
}

struct Jobs {
    std::shared_ptr<SettingsSchema> schema = std::make_shared<SettingsSchema>();
    DependencyRegistry deps;
    JobFactory factory;
    Jobs()
    {
        ModuleContext mc{*schema, deps, factory};
        registerModule(mc);
    }
};

} // namespace

class TstBackportSdkFix : public QObject {
    Q_OBJECT
    QTemporaryDir m_root;

private slots:
    void initTestCase()
    {
        QVERIFY(m_root.isValid());
        fwtest::isolateAppData(m_root);
    }

    void copyNames_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<QString>("expected");
        QTest::newRow("plain") << "PPSA01234-app" << "PPSA01234-app SDK 5";
        QTest::newRow("fw tag") << "FW 4xx PPSA01234 Game (01.000.000)" << "PPSA01234 Game (01.000.000) SDK 5";
        QTest::newRow("fw minor") << "Game FW 4.03xx" << "Game SDK 5";
        QTest::newRow("old sdk") << "Game SDK 4 - Copy" << "Game SDK 5";
        QTest::newRow("copy n") << "Game - Copy (2)" << "Game SDK 5";
        QTest::newRow("count") << "Game (3)" << "Game SDK 5";
        QTest::newRow("empty") << "FW 4xx" << "Game SDK 5";
    }
    void copyNames()
    {
        QFETCH(QString, name);
        QFETCH(QString, expected);
        QCOMPARE(sdkFixCopyName(name, 5), expected);
    }

    // P14: the legacy SDK Fixer patched an ELF-only source in place even in copy mode.
    void copyModeLeavesTheSourceUntouched()
    {
        Env e(ElfOnly | LeftoverBak);
        const auto before = snapshotTree(e.game);
        auto r = runSdkFix(fixOptions(e, SdkFixMode::Copy, 5), e.ctx);
        QVERIFY2(r.ok(), qPrintable(r.ok() ? QString() : r.error().toLogString()));
        QCOMPARE(snapshotTree(e.game), before);
        QCOMPARE(QFileInfo(r->output).fileName(), QStringLiteral("PPSA01234-app SDK 5"));
        QCOMPARE(QFileInfo(r->output).absolutePath(), e.tmp.filePath("out"));
        QVERIFY(!QFileInfo::exists(r->output + ".fwpart"));
        const auto copy = snapshotTree(r->output);
        QCOMPARE(copy.keys(), before.keys()); // same files: no fakelib, no new .bak
        QCOMPARE(r->rewritten, 2);
        QCOMPARE(sdkOf(r->output + "/eboot.bin"), 5);
        QCOMPARE(sdkOf(r->output + "/sce_module/libc.prx"), 5);
        QVERIFY(selfio::hasSelfMagic(copy.value("eboot.bin")));
        // libc rename off (S24): the original pattern stays, the replacement is not introduced.
        auto lib = selfio::unwrap(copy.value("sce_module/libc.prx"));
        QVERIFY(lib.ok());
        QVERIFY(lib->contains(kNidFrom));
        QVERIFY(!lib->contains(kNidTo));
        QCOMPARE(copy.value("sce_sys/param.json"), before.value("sce_sys/param.json"));
        QCOMPARE(copy.value("data/level1.dat"), before.value("data/level1.dat"));
        QCOMPARE(copy.value("eboot.bin.bak"), before.value("eboot.bin.bak"));
        // Report next to the records; no backport record is created by a copy.
        QCOMPARE(QFileInfo(r->reportJson).absolutePath(), QDir(e.orig).filePath("BackportBackup/PPSA01234/reports"));
        QVERIFY(!QFileInfo::exists(QDir(e.orig).filePath("BackportBackup/PPSA01234/manifest.json")));
        const QJsonObject rep = QJsonDocument::fromJson(readBytes(r->reportJson)).object();
        QCOMPARE(rep.value("kind").toString(), QStringLiteral("sdkFix"));
        QCOMPARE(rep.value("target").toObject().value("ps5Sdk").toString(), QStringLiteral("0x05000033"));
        QVERIFY(rep.value("verification").toObject().value("copy").toString().contains("SHA-256"));
        const JobOutcome o = toOutcome(r.value());
        QCOMPARE(o.verification, Verification::Passed);
        QVERIFY(o.outputs.contains(r->output));
        // A second copy gets a free name.
        auto r2 = runSdkFix(fixOptions(e, SdkFixMode::Copy, 5), e.ctx);
        QVERIFY(r2.ok());
        QCOMPARE(QFileInfo(r2->output).fileName(), QStringLiteral("PPSA01234-app SDK 5 (2)"));
    }

    void copyFailureRemovesTheHalfWrittenCopy()
    {
        Env e;
        const auto before = snapshotTree(e.game);
        SdkFixOptions o = fixOptions(e, SdkFixMode::Copy, 5);
        o.testHook = [](const QString& p, const QString& i) { return failAt("copy.file", "sce_sys/param.json", p, i); };
        auto r = runSdkFix(o, e.ctx);
        QVERIFY(!r.ok());
        QVERIFY(r.error().message.contains(QStringLiteral("half-written copy was removed")));
        QCOMPARE(snapshotTree(e.game), before);
        QCOMPARE(QDir(e.tmp.filePath("out")).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden), QStringList());
        QVERIFY(QFileInfo(QDir::fromNativeSeparators(reportFromDetail(r.error().detail))).isFile());
        // Cancelled before the copy: nothing either.
        SdkFixOptions c = fixOptions(e, SdkFixMode::Copy, 5);
        c.testHook = [&e](const QString& p, const QString&) {
            if (p == u"copy.file")
                e.cancel.cancel();
            return success();
        };
        auto rc = runSdkFix(c, e.ctx);
        QVERIFY(!rc.ok());
        QCOMPARE(codeOf(rc.error()), Code::Cancelled);
        QCOMPARE(QDir(e.tmp.filePath("out")).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden), QStringList());
        QCOMPARE(snapshotTree(e.game), before);
    }

    void inPlaceWritesABackupAJournalAndAnUndoRecord()
    {
        Env e;
        const auto before = snapshotTree(e.game);
        auto r = runSdkFix(fixOptions(e, SdkFixMode::InPlace, 5), e.ctx);
        QVERIFY2(r.ok(), qPrintable(r.ok() ? QString() : r.error().toLogString()));
        QCOMPARE(r->output, QDir::cleanPath(e.game));
        QCOMPARE(sdkOf(e.game + "/eboot.bin"), 5);
        QCOMPARE(sdkOf(e.game + "/sce_module/libc.prx"), 5);
        auto lib = selfio::unwrap(readBytes(e.game + "/sce_module/libc.prx"));
        QVERIFY(lib->contains(kNidFrom) && !lib->contains(kNidTo)); // rename off although SDK 5 <= 6
        const auto after = snapshotTree(e.game);
        QCOMPARE(after.keys(), before.keys()); // no fakelib, no .bak, no .fwpart
        QCOMPARE(after.value("sce_sys/param.json"), before.value("sce_sys/param.json"));
        // Backup ZIP: the replaced originals, verified.
        QCOMPARE(QFileInfo(r->backupZip).fileName(), QStringLiteral("PPSA01234 Game (01.000.000) SDK 5 fix original files.zip"));
        QCOMPARE(QFileInfo(r->backupZip).absolutePath(), e.orig);
        QCOMPARE(zipNames(r->backupZip), QStringList({"eboot.bin", "sce_module/libc.prx"}));
        zip::Reader z;
        QVERIFY(z.open(r->backupZip).ok());
        QCOMPARE(z.readAll(*z.find("eboot.bin")).value(), before.value("eboot.bin"));
        // Journal (like Apply) and the undo record with the backup records.
        auto j = ApplyJournal::load(r->journalPath);
        QVERIFY(j.ok());
        QCOMPARE(j->state(), JournalState::Done);
        QCOMPARE(QFileInfo(r->journalPath).absolutePath(), e.journals);
        QCOMPARE(QFileInfo(r->recordPath).absolutePath(), QDir(e.orig).filePath("BackportBackup/PPSA01234/sdk-fixes"));
        QCOMPARE(latestSdkFixRecord(e.orig, e.game), r->recordPath);
        QVERIFY(ApplyJournal::findInterrupted(e.journals).empty());
        const JobOutcome o = toOutcome(r.value());
        QCOMPARE(o.verification, Verification::Passed);
        QVERIFY(o.outputs.contains(r->backupZip));
        QVERIFY(r->summary.contains(QStringLiteral("Undo")));
    }

    void undoRestoresTheFolder()
    {
        Env e;
        const auto before = snapshotTree(e.game);
        auto r = runSdkFix(fixOptions(e, SdkFixMode::InPlace, 5), e.ctx);
        QVERIFY(r.ok());
        QVERIFY(snapshotTree(e.game) != before);
        Jobs jobs;
        const QVariantMap values{{keys::kOriginalsZipFolder, e.orig}};
        auto undo = jobs.factory.create(kJobRestore, QJsonObject{{"game", e.game}, {"undoSdkFix", true}});
        QVERIFY(undo.ok());
        QVERIFY(undo.value()->title().startsWith(QStringLiteral("Undo the SDK fix")));
        QVERIFY(undo.value()->validate(SettingsValues(jobs.schema, values, {})).ok());
        JobContext ctx = e.ctx;
        ctx.settings = SettingsValues(jobs.schema, values, {});
        auto o = undo.value()->run(ctx);
        QVERIFY2(o.ok(), qPrintable(o.ok() ? QString() : o.error().toLogString()));
        QCOMPARE(snapshotTree(e.game), before);
        QVERIFY(latestSdkFixRecord(e.orig, e.game).isEmpty()); // the record is now rolled back
        auto again = jobs.factory.create(kJobRestore, QJsonObject{{"game", e.game}, {"undoSdkFix", true}}).value()->run(ctx);
        QVERIFY(!again.ok());
        QCOMPARE(codeOf(again.error()), Code::RestoreFailed);
    }

    void applyFailureRollsBack()
    {
        Env e;
        const auto before = snapshotTree(e.game);
        SdkFixOptions o = fixOptions(e, SdkFixMode::InPlace, 5);
        o.testHook = [](const QString& p, const QString& i) { return failAt("apply.file", "sce_module/libc.prx", p, i); };
        auto r = runSdkFix(o, e.ctx);
        QVERIFY(!r.ok());
        QVERIFY(r.error().message.contains(QStringLiteral("rolled back")));
        QCOMPARE(snapshotTree(e.game), before);
        QVERIFY(latestSdkFixRecord(e.orig, e.game).isEmpty());
        QCOMPARE(QDir(e.orig).entryList({"*fix original files.zip"}, QDir::Files).size(), 1); // the backup is kept
    }

    void crashDuringApplyIsRecoverable()
    {
        Env e;
        const auto before = snapshotTree(e.game);
        SdkFixOptions o = fixOptions(e, SdkFixMode::InPlace, 5);
        o.testHook = [](const QString& p, const QString& i) { return failAt("apply.crash", "sce_module/libc.prx", p, i); };
        auto r = runSdkFix(o, e.ctx);
        QVERIFY(!r.ok());
        QVERIFY(snapshotTree(e.game) != before); // eboot.bin was applied before the "crash"
        const auto pending = ApplyJournal::findInterrupted(e.journals);
        QCOMPARE(pending.size(), size_t(1));
        QVERIFY(samePath(pending.front().game, e.game));
        // A new fix, or a backport, of this folder is refused until it is rolled back.
        auto blocked = runSdkFix(fixOptions(e, SdkFixMode::InPlace, 5), e.ctx);
        QVERIFY(!blocked.ok());
        QCOMPARE(codeOf(blocked.error()), Code::InterruptedApply);
        auto plan = planBackport(e.options(), e.ctx.token);
        QVERIFY(plan.ok() && !plan->canRun());
        // Start-up recovery: the staging snapshot is gone, the backup ZIP is enough.
        QVERIFY(removeTreeSafely(QDir(e.temp).filePath("staging"), e.temp).ok());
        auto j = ApplyJournal::load(pending.front().path);
        QVERIFY(j.ok());
        QVERIFY(j->rollback(CancellationToken()).ok());
        QCOMPARE(snapshotTree(e.game), before);
    }

    void alreadyAtTheTargetIsNothingToDo()
    {
        Env e;
        QVERIFY(runSdkFix(fixOptions(e, SdkFixMode::InPlace, 5), e.ctx).ok());
        const auto fixed = snapshotTree(e.game);
        const int zips = QDir(e.orig).entryList({"*.zip"}, QDir::Files).size();
        auto again = runSdkFix(fixOptions(e, SdkFixMode::InPlace, 5), e.ctx);
        QVERIFY2(again.ok(), qPrintable(again.ok() ? QString() : again.error().toLogString()));
        QVERIFY(again->nothingToDo);
        QCOMPARE(again->unchanged, 2);
        QCOMPARE(snapshotTree(e.game), fixed);
        QCOMPARE(QDir(e.orig).entryList({"*.zip"}, QDir::Files).size(), zips); // no second backup
        QCOMPARE(toOutcome(again.value()).verification, Verification::Skipped);
        auto copy = runSdkFix(fixOptions(e, SdkFixMode::Copy, 5), e.ctx);
        QVERIFY(copy.ok() && copy->nothingToDo);
        QCOMPARE(QDir(e.tmp.filePath("out")).entryList(QDir::AllEntries | QDir::NoDotAndDotDot), QStringList());
    }

    // An in-place fix of a backported folder is added to its record: the S7
    // verdict still says "patched", and a restore of the backport originals
    // recognises the fixed files.
    void fixOfABackportedFolderIsKnownToItsRecord()
    {
        Env e;
        const auto original = snapshotTree(e.game);
        auto b = runBackport(e.options(), e.ctx);
        QVERIFY2(b.ok(), qPrintable(b.ok() ? QString() : b.error().toLogString()));
        auto r = runSdkFix(fixOptions(e, SdkFixMode::InPlace, 5), e.ctx);
        QVERIFY2(r.ok(), qPrintable(r.ok() ? QString() : r.error().toLogString()));
        QVERIFY(!r->warnings.filter(QStringLiteral("higher than the SDK 4")).isEmpty()); // raises the marker
        QCOMPARE(r->rewritten, 2); // fakelib/ never processed (S33)
        auto m = readManifest(b->manifestPath);
        QVERIFY(m.ok() && m.value());
        QCOMPARE(m.value()->doc.value("sdk_fixes").size(), 1);
        auto plan = planBackport(e.options(), e.ctx.token);
        QVERIFY(plan.ok());
        QCOMPARE(plan->verdict, Verdict::Patched);
        RestoreOptions ro;
        ro.game = e.game;
        ro.originalsDir = e.orig;
        auto rep = restoreOriginals(ro, e.ctx.token);
        QVERIFY2(rep.ok() && rep->ok(), qPrintable(rep.ok() ? (rep->skipped + rep->errors).join("; ") : rep.error().toLogString()));
        QCOMPARE(snapshotTree(e.game), original);
    }

    void refusals()
    {
        Env enc(EncryptedRight);
        const auto before = snapshotTree(enc.game);
        auto r = runSdkFix(fixOptions(enc, SdkFixMode::InPlace, 5), enc.ctx);
        QVERIFY(!r.ok());
        QCOMPARE(codeOf(r.error()), Code::SelfEncrypted);
        QCOMPARE(snapshotTree(enc.game), before);
        QVERIFY(QDir(enc.orig).entryList({"*.zip"}, QDir::Files).isEmpty());
        QVERIFY(reportFromDetail(r.error().detail).isEmpty()); // refused before anything: no report

        Env e;
        SdkFixOptions noOrig = fixOptions(e, SdkFixMode::InPlace, 5);
        noOrig.originalsDir.clear();
        QCOMPARE(codeOf(runSdkFix(noOrig, e.ctx).error()), Code::ZipFolderInvalid);
        QCOMPARE(codeOf(runSdkFix(fixOptions(e, SdkFixMode::Copy, 14), e.ctx).error()), Code::TargetSdkInvalid);
        SdkFixOptions inside = fixOptions(e, SdkFixMode::Copy, 5);
        inside.outputDir = e.game + "/data";
        QCOMPARE(codeOf(runSdkFix(inside, e.ctx).error()), Code::ZipFolderInvalid);
        SdkFixOptions full = fixOptions(e, SdkFixMode::Copy, 5);
        full.diskSpaceProbe = [](const QString&) -> Result<DiskSpace> { return DiskSpace{1024, 1 << 30, "/", "test"}; };
        QCOMPARE(codeOf(runSdkFix(full, e.ctx).error()), Code::InsufficientSpace);

        Jobs jobs;
        const SettingsValues none(jobs.schema, {}, {});
        QVERIFY(!jobs.factory.create(kJobSdkFix, QJsonObject{{"folder", e.game}, {"mode", "inPlace"}}).value()->validate(none).ok());
        QVERIFY(!jobs.factory.create(kJobSdkFix, QJsonObject{{"folder", e.game}, {"mode", "sideways"}}).value()->validate(none).ok());
        QVERIFY(jobs.factory.create(kJobSdkFix, QJsonObject{{"folder", e.game}}).value()->validate(none).ok());
    }

    void jobRunsThroughTheFactory()
    {
        Env e;
        Jobs jobs;
        const QVariantMap values{{keys::kOriginalsZipFolder, e.orig}, {keys::kTargetSdk, QStringLiteral("6")}};
        const SettingsValues settings(jobs.schema, values, {});
        auto job = jobs.factory.create(kJobSdkFix, QJsonObject{{"folder", e.game}, {"outputDir", e.tmp.filePath("out")}});
        QDir().mkpath(e.tmp.filePath("out"));
        QVERIFY(job.ok());
        QCOMPARE(job.value()->phases(), QStringList({"Scan", "Patch", "Write", "Verify"}));
        QCOMPARE(sdkFixOptionsFrom(settings, job.value()->parameters()).targetSdk, 6); // from backport.targetSdk
        QCOMPARE(sdkFixOptionsFrom(settings, QJsonObject{{"folder", e.game}, {"targetSdk", 3}}).targetSdk, 3);
        JobContext ctx = e.ctx;
        ctx.settings = settings;
        e.progress->setPhases(job.value()->phases());
        auto o = job.value()->run(ctx);
        QVERIFY2(o.ok(), qPrintable(o.ok() ? QString() : o.error().toLogString()));
        QCOMPARE(sdkOf(e.tmp.filePath("out/PPSA01234-app SDK 6/eboot.bin")), 6);
        QVERIFY(QFileInfo(QDir::fromNativeSeparators(o->details.value("report").toObject().value("html").toString())).isFile());
    }
};

QTEST_GUILESS_MAIN(TstBackportSdkFix)
#include "tst_backport_sdkfix.moc"
