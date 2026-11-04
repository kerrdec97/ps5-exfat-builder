// End-to-end backport runs on synthetic games (docs/BACKPORT_PARITY.md §6.5
// "Engine integration", "ApplyJournal", "Restore"): build-only leaves the
// game untouched, golden outputs, failures produce nothing, cancellation,
// journaled apply rollback and crash recovery, restore, long paths, jobs.

#include "BackportTestEnv.h"

#include "core/Zip.h"
#include "engines/backport/ApplyJournal.h"
#include "engines/backport/BackportEngine.h"
#include "engines/backport/BackportJobs.h"
#include "engines/backport/BackportManifest.h"
#include "engines/backport/BackportRunner.h"
#include "engines/backport/BackportSettings.h"
#include "engines/backport/Module.h"
#include "engines/backport/RestoreService.h"
#include "engines/backport/SceParam.h"
#include "engines/backport/SelfReader.h"
#include "support/TestSupport.h"

#include <QJsonArray>
#include <QTest>

using namespace fw;
using namespace fw::backport;
using namespace fwtest::bp;

namespace {

const QByteArray kGoldenF1Pair4 = "10b80a5af227262378e246bcc723e2d92b34f1b48d6c839aaef5e43d109151f8";
const QByteArray kGoldenF2Pair4Nid = "6f93078810d17f6862d9ca29aa1c7f75a73232f11098213cb94e7f00f6a1bc68";

QStringList zipNames(const QString& path)
{
    zip::Reader r;
    if (!r.open(path).ok())
        return {QStringLiteral("<unreadable>")};
    QStringList out;
    for (const zip::Entry& e : r.entries())
        out << e.name;
    return out;
}

QStringList zipsIn(const QString& dir)
{
    return QDir(dir).entryList(QDir::Files | QDir::Hidden, QDir::Name);
}

Status failAt(const QString& wantPoint, const QString& wantItem, const QString& point, const QString& item)
{
    if (point == wantPoint && (wantItem.isEmpty() || item.contains(wantItem)))
        return makeError(Code::ApplyFailed, QStringLiteral("injected failure at %1 %2").arg(point, item));
    return success();
}

QString stagingKeptIn(const QString& dir)
{
    const QStringList l = QDir(dir).entryList({QStringLiteral("fw-backport-staging-*")}, QDir::Dirs);
    return l.isEmpty() ? QString() : l.front();
}

} // namespace

class TstBackportRun : public QObject {
    Q_OBJECT
    QTemporaryDir m_root;

private slots:
    void initTestCase()
    {
        QVERIFY(m_root.isValid());
        fwtest::isolateAppData(m_root);
    }

    // ---- full run, apply on (G1 a) -----------------------------------------------
    void selfGameBackportedAndApplied()
    {
        Env e;
        const auto before = snapshotTree(e.game);
        auto r = runBackport(e.options(), e.ctx);
        QVERIFY2(r.ok(), qPrintable(r.ok() ? QString() : r.error().toLogString()));
        QCOMPARE(r->status, RunStatus::Succeeded);
        QVERIFY(r->applied);
        QCOMPARE(QFileInfo(r->originalsZip).fileName(), QStringLiteral("PPSA01234 Game (01.000.000) original files.zip"));
        QCOMPARE(QFileInfo(r->backportZip).fileName(), QStringLiteral("FW 4xx PPSA01234 Game (01.000.000) backport files.zip"));
        QCOMPARE(zipNames(r->originalsZip),
                 QStringList({QStringLiteral("eboot.bin"), QStringLiteral("sce_module/libc.prx"), QStringLiteral("sce_sys/param.json")}));
        QCOMPARE(zipNames(r->backportZip),
                 QStringList({QStringLiteral("eboot.bin"), QStringLiteral("fakelib/libSceAgc.sprx"),
                              QStringLiteral("fakelib/libSceNpAuth.sprx"), QStringLiteral("sce_module/libc.prx")}));
        // Deterministic output equal to the Python engine's (Appendix A).
        QCOMPARE(sha(readBytes(e.game + "/eboot.bin")), kGoldenF1Pair4);
        QCOMPARE(sha(readBytes(e.game + "/sce_module/libc.prx")), kGoldenF2Pair4Nid);
        QCOMPARE(readBytes(e.game + "/fakelib/libSceAgc.sprx"), readBytes(e.fakelib + "/libSceAgc.sprx"));
        QVERIFY(!QFileInfo::exists(e.game + "/fakelib/readme.txt")); // only libraries are staged
        // Untouched: everything that is not an executable.
        QCOMPARE(readBytes(e.game + "/sce_sys/param.json"), before.value("sce_sys/param.json"));
        QCOMPARE(readBytes(e.game + "/data/level1.dat"), before.value("data/level1.dat"));
        // Originals in the ZIP are the bytes from before the run.
        zip::Reader z;
        QVERIFY(z.open(r->originalsZip).ok());
        QCOMPARE(z.readAll(*z.find("eboot.bin")).value(), before.value("eboot.bin"));
        // Manifest schema 2 (S21), journal done (S35).
        auto m = readManifest(r->manifestPath);
        QVERIFY(m.ok() && m.value());
        QCOMPARE(m.value()->schemaVersion(), 2);
        QCOMPARE(m.value()->currentTarget().value_or(QString()), QStringLiteral("4xx"));
        QVERIFY(m.value()->applied());
        QCOMPARE(m.value()->addedFiles(),
                 QStringList({QStringLiteral("fakelib/libSceAgc.sprx"), QStringLiteral("fakelib/libSceNpAuth.sprx")}));
        auto j = ApplyJournal::load(r->journalPath);
        QVERIFY(j.ok());
        QCOMPARE(j->state(), JournalState::Done);
        QVERIFY(ApplyJournal::findInterrupted(e.journals).empty());
        // No leftovers in the game: no .bak, no .fwpart.
        for (const QString& rel : snapshotTree(e.game).keys())
            QVERIFY2(!rel.endsWith(".bak") && !rel.endsWith(".fwpart") && !rel.endsWith(".part"), qPrintable(rel));
        QCOMPARE(zipsIn(e.orig).filter(QStringLiteral(".part")), QStringList());
        // A second run is refused as already backported (S6 and S7).
        auto again = runBackport(e.options(), e.ctx);
        QVERIFY(!again.ok());
        QCOMPARE(codeOf(again.error()), Code::AlreadyBackported);
        auto noFakelib = e.options(false);
        auto again2 = runBackport(noFakelib, e.ctx);
        QVERIFY(!again2.ok());
        QCOMPARE(codeOf(again2.error()), Code::AlreadyBackported); // manifest hashes
    }

    void buildOnlyLeavesGameUntouched() // S31, D1, G1(b)/(c)
    {
        for (unsigned v : {unsigned(SelfOnly), unsigned(ElfOnly), unsigned(Mixed)}) {
            Env e(v);
            const auto before = snapshotTree(e.game);
            auto o = e.options();
            o.applyToGame = false;
            auto r = runBackport(o, e.ctx);
            QVERIFY2(r.ok(), qPrintable(r.ok() ? QString() : r.error().toLogString()));
            QVERIFY(!r->applied);
            QCOMPARE(snapshotTree(e.game), before);
            QVERIFY(r->summary.contains(QStringLiteral("not changed")));
            // P1: no .bak anywhere, not in the game and not in staging.
            for (const QString& rel : snapshotTree(e.tmp.path()).keys())
                QVERIFY2(!rel.endsWith(QStringLiteral(".bak")), qPrintable(rel));
            // Staged outputs are SELFs.
            zip::Reader z;
            QVERIFY(z.open(r->backportZip).ok());
            for (const zip::Entry& en : z.entries())
                if (!en.name.startsWith(QStringLiteral("fakelib/"))) // the set is carried unchanged (D3)
                    QVERIFY2(selfio::hasSelfMagic(z.readAll(en).value()), qPrintable(en.name));
            QCOMPARE(sha(z.readAll(*z.find("eboot.bin")).value()), kGoldenF1Pair4);
            auto m = readManifest(r->manifestPath);
            QVERIFY(!m.value()->applied());
            QVERIFY(!m.value()->currentTarget().has_value());
        }
    }

    void eachTargetSdkLandsInTheGame_data()
    {
        QTest::addColumn<int>("n");
        for (int n = 1; n <= 7; ++n)
            QTest::newRow(qPrintable(QStringLiteral("SDK %1").arg(n))) << n;
    }
    void eachTargetSdkLandsInTheGame()
    {
        QFETCH(int, n);
        Env e;
        auto o = e.options(false);
        o.targetSdk = n;
        auto r = runBackport(o, e.ctx);
        QVERIFY2(r.ok(), qPrintable(r.ok() ? QString() : r.error().toLogString()));
        QVERIFY(QFileInfo(r->backportZip).fileName().startsWith(QStringLiteral("FW %1xx ").arg(n)));
        auto u = selfio::unwrap(readBytes(e.game + "/eboot.bin"));
        QVERIFY(u.ok());
        quint32 ps5 = 0, ps4 = 0;
        QCOMPARE(elfSdkMajor(u.value(), &ps5, &ps4).value_or(-1), n);
        QCOMPARE(ps5, sdkPair(n)->ps5);
        QCOMPARE(ps4, sdkPair(n)->ps4);
        // libc rename: applied up to SDK 6, not above (the pattern is the original one).
        const QByteArray libc = readBytes(e.game + "/sce_module/libc.prx");
        QCOMPARE(libc.contains("IWIBBdTHit4#A#B"), n <= 6);
        QCOMPARE(libc.contains("4h6F1LLbTiw#A#B"), n > 6);
    }

    void nidModeOffLeavesImports()
    {
        Env e;
        auto o = e.options(false);
        o.nidMode = NidMode::Off;
        auto r = runBackport(o, e.ctx);
        QVERIFY(r.ok());
        QVERIFY(readBytes(e.game + "/sce_module/libc.prx").contains("4h6F1LLbTiw#A#B"));
    }

    void anotherToolsFakelibKeptInOriginals() // S14, D3 / G1(d)
    {
        Env e(OtherFakelib);
        const QByteArray ampr = readBytes(e.game + "/fakelib/libSceAmpr.sprx");
        const QByteArray theirAgc = readBytes(e.game + "/fakelib/libSceAgc.sprx");
        auto r = runBackport(e.options(), e.ctx);
        QVERIFY2(r.ok(), qPrintable(r.ok() ? QString() : r.error().toLogString()));
        QVERIFY(zipNames(r->originalsZip).contains(QStringLiteral("fakelib/libSceAgc.sprx")));
        zip::Reader z;
        QVERIFY(z.open(r->originalsZip).ok());
        QCOMPARE(z.readAll(*z.find("fakelib/libSceAgc.sprx")).value(), theirAgc);
        QCOMPARE(readBytes(e.game + "/fakelib/libSceAmpr.sprx"), ampr); // extra: never re-signed (P3)
        QVERIFY(!zipNames(r->backportZip).contains(QStringLiteral("fakelib/libSceAmpr.sprx")));
        QVERIFY(r->warnings.join(u'|').contains(QStringLiteral("libSceAmpr.sprx")));
    }

    void nestedEbootGetsFakelib() // G1(h)
    {
        Env e(NestedEboot);
        auto r = runBackport(e.options(), e.ctx);
        QVERIFY(r.ok());
        QCOMPARE(readBytes(e.game + "/sub/fakelib/libSceAgc.sprx"), readBytes(e.fakelib + "/libSceAgc.sprx"));
        QCOMPARE(sha(readBytes(e.game + "/sub/eboot.bin")), kGoldenF1Pair4);
    }

    void encryptedRefusedOrLeftUnchanged() // S32 / G1(e)
    {
        {
            Env e(EncryptedRight);
            const auto before = snapshotTree(e.game);
            auto r = runBackport(e.options(), e.ctx);
            QVERIFY(!r.ok());
            QCOMPARE(codeOf(r.error()), Code::SelfEncrypted);
            QCOMPARE(snapshotTree(e.game), before);
            QCOMPARE(zipsIn(e.orig), QStringList());
            QCOMPARE(zipsIn(e.bp), QStringList());
        }
        {
            Env e(EncryptedRight);
            const QByteArray right = readBytes(e.game + "/sce_sys/about/right.sprx");
            auto o = e.options();
            o.encryptedPolicy = EncryptedPolicy::LeaveUnchanged;
            auto r = runBackport(o, e.ctx);
            QVERIFY(r.ok());
            QCOMPARE(r->status, RunStatus::Partial);
            QCOMPARE(r->leftUnchanged, QStringList({QStringLiteral("sce_sys/about/right.sprx")}));
            QCOMPARE(readBytes(e.game + "/sce_sys/about/right.sprx"), right);
            QCOMPARE(toOutcome(r.value()).verification, Verification::Inconclusive);
            QVERIFY(r->summary.startsWith(QStringLiteral("Partial")));
        }
    }

    // ---- failures produce nothing (D4, S19, S34) -------------------------------------
    void perFileFailureProducesNothing()
    {
        Env e;
        const auto before = snapshotTree(e.game);
        auto o = e.options();
        o.testHook = [](const QString& p, const QString& i) { return failAt("build.file", "libc.prx", p, i); };
        auto r = runBackport(o, e.ctx);
        QVERIFY(!r.ok());
        QVERIFY(r.error().message.startsWith(QStringLiteral("No safe backport was produced")));
        QCOMPARE(snapshotTree(e.game), before);
        QCOMPARE(zipsIn(e.orig), QStringList());
        QCOMPARE(zipsIn(e.bp), QStringList());
        QVERIFY(!stagingKeptIn(e.tmp.filePath("temp")).isEmpty()); // backport.keepStagingOnFailure
    }

    void snapshotErrorStopsTheRun() // S13
    {
        Env e;
        const QString victim = e.game + "/sce_module/libc.prx";
        auto o = e.options();
        o.testHook = [victim](const QString& p, const QString& i) -> Status {
            if (p == u"build.file" && i.endsWith(QStringLiteral("libc.prx")))
                QFile::remove(victim); // the copy of this file will fail
            return success();
        };
        auto r = runBackport(o, e.ctx);
        QVERIFY(!r.ok());
        QCOMPARE(codeOf(r.error()), Code::Unreadable);
        QVERIFY(r.error().message.contains(QStringLiteral("before patching")));
        QCOMPARE(zipsIn(e.orig), QStringList());
        QCOMPARE(zipsIn(e.bp), QStringList());
    }

    void fileChangedDuringTheRunIsRefused()
    {
        Env e;
        const QString victim = e.game + "/sce_module/libc.prx";
        auto o = e.options();
        o.testHook = [victim](const QString& p, const QString& i) -> Status {
            if (p == u"build.file" && i.endsWith(QStringLiteral("libc.prx")))
                writeBytes(victim, selfOf(f2(42))); // changed after the scan hashed it
            return success();
        };
        auto r = runBackport(o, e.ctx);
        QVERIFY(!r.ok());
        QCOMPARE(codeOf(r.error()), Code::GameChanged);
        QCOMPARE(zipsIn(e.bp), QStringList());
    }

    void verificationCatchesAParamOutsideTheSignedSegments() // V3, A.5; backport.verifyBuild
    {
        // PROCPARAM in a gap no SELF segment covers: the patch would be lost.
        QByteArray elf = f1();
        elf.append(QByteArray(0x100, '\0'));
        put64(elf, 0x40 + 2 * 0x38 + 0x08, 0x5A80);
        put64(elf, 0x5A80, 0x60);
        put32(elf, 0x5A88, 0x4942524F);
        put32(elf, 0x5A94, 0x07000038);
        auto p = processExecutable(elf, *sdkPair(4), NidMode::Auto, true);
        QVERIFY(!p.ok());
        QCOMPARE(codeOf(p.error()), Code::VerifyFailed);
        QVERIFY(processExecutable(elf, *sdkPair(4), NidMode::Auto, false).ok());
        {
            Env off;
            writeBytes(off.game + "/eboot.bin", elf);
            auto oo = off.options(false);
            oo.verifyBuild = false; // the setting is read: the same file now builds
            oo.applyToGame = false;
            auto ro = runBackport(oo, off.ctx);
            QVERIFY2(ro.ok(), qPrintable(ro.ok() ? QString() : ro.error().toLogString()));
            QVERIFY(toOutcome(ro.value()).verificationDetail.contains(QStringLiteral("build checks turned off")));
        }

        Env e;
        writeBytes(e.game + "/eboot.bin", elf);
        const auto before = snapshotTree(e.game);
        auto o = e.options();
        o.keepStagingOnFailure = false;
        auto r = runBackport(o, e.ctx);
        QVERIFY(!r.ok());
        QCOMPARE(codeOf(r.error()), Code::VerifyFailed);
        QCOMPARE(snapshotTree(e.game), before);
        QCOMPARE(zipsIn(e.bp), QStringList());
        QVERIFY(stagingKeptIn(e.tmp.filePath("temp")).isEmpty());
    }

    void originalsZipFailureProducesNothing()
    {
        Env e;
        const auto before = snapshotTree(e.game);
        auto o = e.options();
        o.testHook = [](const QString& p, const QString& i) { return failAt("zip.member", "param.json", p, i); };
        auto r = runBackport(o, e.ctx);
        QVERIFY(!r.ok());
        QCOMPARE(codeOf(r.error()), Code::ZipWriteFailed);
        QCOMPARE(snapshotTree(e.game), before);
        QCOMPARE(zipsIn(e.orig), QStringList()); // no .part either
        QCOMPARE(zipsIn(e.bp), QStringList());
    }

    void backportZipFailureIsPartialAndAppliesNothing() // S29
    {
        Env e;
        const auto before = snapshotTree(e.game);
        auto o = e.options();
        o.testHook = [](const QString& p, const QString& i) { return failAt("zip.commit", "backport files", p, i); };
        auto r = runBackport(o, e.ctx);
        QVERIFY(!r.ok());
        QVERIFY(r.error().message.startsWith(QStringLiteral("Partial")));
        QVERIFY(r.error().message.contains(QStringLiteral("original files.zip"))); // names the originals
        QCOMPARE(snapshotTree(e.game), before);
        QCOMPARE(zipsIn(e.orig).size(), 1);
        QVERIFY(zipsIn(e.orig).front().endsWith(QStringLiteral("original files.zip")));
        QCOMPARE(zipsIn(e.bp), QStringList());
    }

    void earlierZipsAreNeverOverwritten() // S17
    {
        Env e;
        const QString first = e.orig + "/PPSA01234 Game (01.000.000) original files.zip";
        writeBytes(first, "the real originals");
        auto o = e.options();
        o.applyToGame = false;
        auto r = runBackport(o, e.ctx);
        QVERIFY(r.ok());
        QVERIFY(r->originalsZip.endsWith(QStringLiteral("original files (2).zip")));
        QCOMPARE(readBytes(first), QByteArray("the real originals"));
    }

    // ---- cancellation ---------------------------------------------------------------------
    void cancelBeforeAndDuringBuild()
    {
        {
            Env e;
            const auto before = snapshotTree(e.game);
            e.cancel.cancel();
            auto r = runBackport(e.options(), e.ctx);
            QVERIFY(!r.ok());
            QVERIFY(r.error().isCancelled());
            QCOMPARE(snapshotTree(e.game), before);
            QCOMPARE(zipsIn(e.orig), QStringList());
        }
        {
            Env e;
            const auto before = snapshotTree(e.game);
            auto o = e.options();
            CancellationSource* src = &e.cancel;
            o.testHook = [src](const QString& p, const QString&) {
                if (p == u"zip.member")
                    src->cancel();
                return success();
            };
            auto r = runBackport(o, e.ctx);
            QVERIFY(!r.ok());
            QVERIFY(r.error().isCancelled());
            QVERIFY(r.error().message.contains(QStringLiteral("not changed")));
            QCOMPARE(snapshotTree(e.game), before);
            QCOMPARE(zipsIn(e.orig), QStringList());
            QCOMPARE(zipsIn(e.bp), QStringList());
        }
    }

    void cancelDuringApplyRollsBack()
    {
        Env e;
        const auto before = snapshotTree(e.game);
        auto o = e.options();
        CancellationSource* src = &e.cancel;
        int n = 0;
        o.testHook = [src, &n](const QString& p, const QString&) {
            if (p == u"apply.file" && ++n == 3)
                src->cancel();
            return success();
        };
        auto r = runBackport(o, e.ctx);
        QVERIFY(!r.ok());
        QVERIFY(r.error().isCancelled());
        QVERIFY(r.error().message.contains(QStringLiteral("rolled back")));
        QCOMPARE(snapshotTree(e.game), before);
        QVERIFY(!QFileInfo::exists(e.game + "/fakelib")); // created folder removed
    }

    // ---- journaled apply (S35) --------------------------------------------------------------
    void applyFailureRollsBackExactly()
    {
        Env e(OtherFakelib);
        const auto before = snapshotTree(e.game);
        auto o = e.options();
        o.testHook = [](const QString& p, const QString& i) { return failAt("apply.file", "libSceNpAuth", p, i); };
        auto r = runBackport(o, e.ctx);
        QVERIFY(!r.ok());
        QVERIFY(r.error().message.contains(QStringLiteral("rolled back")));
        QVERIFY(r.error().message.startsWith(QStringLiteral("Partial")));
        QCOMPARE(snapshotTree(e.game), before);
        const auto journals = QDir(e.journals).entryList({"*.json"});
        QCOMPARE(journals.size(), 1);
        auto j = ApplyJournal::load(e.journals + "/" + journals.front());
        QVERIFY(j.ok());
        QCOMPARE(j->state(), JournalState::RolledBack);
        // The manifest says nothing was applied.
        auto m = readManifest(manifestPathFor(e.orig, QStringLiteral("PPSA01234")));
        QVERIFY(!m.value()->applied());
    }

    void crashDuringApplyIsRecoverable()
    {
        Env e;
        const auto before = snapshotTree(e.game);
        auto o = e.options();
        int n = 0;
        o.testHook = [&n](const QString& p, const QString&) -> Status {
            if (p == u"apply.crash" && ++n == 3)
                return makeError(Code::ApplyFailed, QStringLiteral("power cut"));
            return success();
        };
        auto r = runBackport(o, e.ctx);
        QVERIFY(!r.ok());
        QVERIFY(snapshotTree(e.game) != before); // half applied, like a real crash
        // Remove the staging snapshots too: rollback must work from the ZIP alone.
        QVERIFY(QDir(e.temp + "/staging").removeRecursively());
        const auto pending = ApplyJournal::findInterrupted(e.journals);
        QCOMPARE(int(pending.size()), 1);
        QCOMPARE(QDir::cleanPath(pending.front().game), QDir::cleanPath(e.game));
        // A new run on this game is refused until the interrupted one is rolled back.
        auto blocked = runBackport(e.options(), e.ctx);
        QVERIFY(!blocked.ok());
        QCOMPARE(codeOf(blocked.error()), Code::InterruptedApply);
        // Recovery through the restore job, as the app offers it at start-up.
        SettingsSchema schema;
        DependencyRegistry deps;
        JobFactory jobs;
        ModuleContext mc{schema, deps, jobs};
        registerModule(mc);
        auto job = jobs.create(kJobRestore, QJsonObject{{"journal", pending.front().path}});
        QVERIFY(job.ok());
        QVERIFY(job.value()->validate(SettingsValues()).ok());
        e.progress->setPhases(job.value()->phases());
        auto out = job.value()->run(e.ctx);
        QVERIFY2(out.ok(), qPrintable(out.ok() ? QString() : out.error().toLogString()));
        QCOMPARE(snapshotTree(e.game), before);
        QVERIFY(ApplyJournal::findInterrupted(e.journals).empty());
    }

    void rollbackLeavesForeignContentAlone()
    {
        Env e;
        auto o = e.options();
        int n = 0;
        o.testHook = [&n](const QString& p, const QString&) -> Status {
            if (p == u"apply.crash" && ++n == 2)
                return makeError(Code::ApplyFailed, QStringLiteral("power cut"));
            return success();
        };
        QVERIFY(!runBackport(o, e.ctx).ok());
        // Someone edits an applied file before the rollback.
        writeBytes(e.game + "/eboot.bin", "user edit");
        auto pending = ApplyJournal::findInterrupted(e.journals);
        QCOMPARE(int(pending.size()), 1);
        auto j = ApplyJournal::load(pending.front().path);
        QStringList problems;
        QVERIFY(!j->rollback(CancellationToken(), &problems).ok());
        QCOMPARE(problems.size(), 1);
        QCOMPARE(readBytes(e.game + "/eboot.bin"), QByteArray("user edit"));
        QCOMPARE(j->state(), JournalState::RollbackFailed);
        QCOMPARE(int(ApplyJournal::findInterrupted(e.journals).size()), 1); // still offered
    }

    void journalsArePrunedByAge() // backport.journalRetentionDays
    {
        QTemporaryDir d;
        auto done = ApplyJournal::create(d.filePath("a.json"), "a", d.path(), d.filePath("x.zip"), {});
        QVERIFY(done.ok());
        QVERIFY(done->apply(CancellationToken()).ok()); // no entries: done
        auto pending = ApplyJournal::create(d.filePath("b.json"), "b", d.path(), d.filePath("x.zip"), {});
        QVERIFY(pending.ok());
        QCOMPARE(ApplyJournal::prune(d.path(), 30), 0); // too young
        for (const QString& f : {d.filePath("a.json"), d.filePath("b.json")}) {
            QFile file(f);
            QVERIFY(file.open(QIODevice::ReadWrite));
            QVERIFY(file.setFileTime(QDateTime::currentDateTime().addDays(-40), QFileDevice::FileModificationTime));
        }
        QCOMPARE(ApplyJournal::prune(d.path(), 30), 1);
        QVERIFY(!QFileInfo::exists(d.filePath("a.json")));
        QVERIFY(QFileInfo::exists(d.filePath("b.json"))); // a pending journal is never pruned
    }

    // ---- restore (S20, S36, D6) ---------------------------------------------------------
    void restoreRemovesAddedFilesAndAllowsANewRun()
    {
        Env e;
        const auto before = snapshotTree(e.game);
        auto r = runBackport(e.options(), e.ctx);
        QVERIFY(r.ok());
        auto rep = restoreOriginals({e.game, r->originalsZip, e.orig}, CancellationToken());
        QVERIFY2(rep.ok(), qPrintable(rep.ok() ? QString() : rep.error().toLogString()));
        QVERIFY2(rep->ok(), qPrintable(rep->errors.join(u'|')));
        QCOMPARE(rep->restored, 2);
        QCOMPARE(rep->unchanged, 1); // param.json
        QCOMPARE(rep->removedAdded,
                 QStringList({QStringLiteral("fakelib/libSceAgc.sprx"), QStringLiteral("fakelib/libSceNpAuth.sprx")}));
        QCOMPARE(snapshotTree(e.game), before);
        QVERIFY(!QFileInfo::exists(e.game + "/fakelib"));
        auto m = readManifest(rep->manifestPath);
        QVERIFY(!m.value()->currentTarget().has_value());
        // P6: the next run is not refused.
        auto again = runBackport(e.options(), e.ctx);
        QVERIFY2(again.ok(), qPrintable(again.ok() ? QString() : again.error().toLogString()));
        QVERIFY(again->originalsZip.endsWith(QStringLiteral("(2).zip")));
        // findOriginalsZip picks the recorded one.
        QCOMPARE(findOriginalsZip(e.game, e.orig), again->originalsZip);
        // S37: nothing ever deletes an originals ZIP (both are still there).
        QVERIFY(QFileInfo::exists(r->originalsZip));
        QVERIFY(QFileInfo::exists(again->originalsZip));
    }

    void restoreRefusesZipSlipBeforeWriting() // port of Restore.test_zip_slip_refused
    {
        Env e;
        const auto before = snapshotTree(e.game);
        const QString zp = e.orig + "/evil original files.zip";
        {
            zip::Writer w;
            QVERIFY(w.open(zp).ok());
            QVERIFY(w.addData("eboot.bin", "orig").ok());
            QVERIFY(w.addData("sce_module/libc.prx", "libc").ok());
            QVERIFY(w.addData("../escape.txt", "bad").ok());
            QVERIFY(w.addData("C:/win.txt", "bad").ok());
            QVERIFY(w.addData("sub\\..\\..\\evil.txt", "bad").ok());
            QVERIFY(w.finish().ok());
        }
        auto rep = restoreOriginals({e.game, zp, QString()}, CancellationToken());
        QVERIFY(rep.ok());
        QVERIFY(!rep->ok());
        QCOMPARE(rep->refused.size(), 3);
        QCOMPARE(rep->restored, 0);
        QCOMPARE(snapshotTree(e.game), before); // nothing written
        QVERIFY(!QFileInfo::exists(e.tmp.filePath("games/escape.txt")));
        QVERIFY(!QFileInfo::exists(e.tmp.filePath("evil.txt")));
    }

    void safeMemberTargets() // port of Restore.test_safe_member_target
    {
        QTemporaryDir d;
        QCOMPARE(safeMemberTarget(d.path(), "a/b.bin").value_or(QString()), QStringLiteral("a/b.bin"));
        QCOMPARE(safeMemberTarget(d.path(), "./a//b.bin").value_or(QString()), QStringLiteral("a/b.bin"));
        for (const char* bad : {"", "/", "../x", "a/../../x", "D:x", "\\\\srv\\s\\x", "/abs.txt", "C:/win.txt",
                                "sub\\..\\..\\evil.txt", "a/b:c"})
            QVERIFY2(!safeMemberTarget(d.path(), QString::fromLatin1(bad)).has_value(), bad);
    }

    void restoreRefusesAChangedZip() // manifest hash mismatch
    {
        Env e;
        auto o = e.options();
        o.applyToGame = false;
        auto r = runBackport(o, e.ctx);
        QVERIFY(r.ok());
        QFile f(r->originalsZip);
        QVERIFY(f.open(QIODevice::Append));
        f.write("tampered");
        f.close();
        const auto before = snapshotTree(e.game);
        auto rep = restoreOriginals({e.game, r->originalsZip, e.orig}, CancellationToken());
        QVERIFY(!rep.ok());
        QCOMPARE(codeOf(rep.error()), Code::ManifestMismatch);
        QCOMPARE(snapshotTree(e.game), before);
    }

    void partialRestoreIsAFailure() // S20
    {
        Env e;
        auto r = runBackport(e.options(), e.ctx);
        QVERIFY(r.ok());
        // Another ZIP whose eboot.bin is not the recorded original.
        const QString other = e.tmp.filePath("other original files.zip");
        {
            zip::Writer w;
            QVERIFY(w.open(other).ok());
            QVERIFY(w.addData("eboot.bin", "not the original").ok());
            zip::Reader orig;
            QVERIFY(orig.open(r->originalsZip).ok());
            QVERIFY(w.addData("sce_module/libc.prx", orig.readAll(*orig.find("sce_module/libc.prx")).value()).ok());
            QVERIFY(w.finish().ok());
        }
        auto rep = restoreOriginals({e.game, other, e.orig}, CancellationToken());
        QVERIFY(rep.ok());
        QVERIFY(!rep->ok());
        QCOMPARE(rep->errors.size(), 1);
        QVERIFY(rep->errors.front().contains(QStringLiteral("eboot.bin")));
        QCOMPARE(rep->restored, 1);
        // Through the job the partial restore is a failed job.
        auto schema = std::make_shared<SettingsSchema>();
        DependencyRegistry deps;
        JobFactory jobs;
        ModuleContext mc{*schema, deps, jobs};
        registerModule(mc);
        auto job = jobs.create(kJobRestore, QJsonObject{{"game", e.game}, {"zip", other}});
        QVERIFY(job.ok());
        e.progress->setPhases(job.value()->phases());
        e.ctx.settings = SettingsValues(schema, QVariantMap{{keys::kOriginalsZipFolder, e.orig}}, {});
        auto out = job.value()->run(e.ctx);
        QVERIFY(!out.ok());
        QVERIFY(out.error().message.startsWith(QStringLiteral("Partial restore")));
    }

    void restoreWithoutRecordNeedsConsent()
    {
        Env e;
        const QString zp = e.tmp.filePath("loose original files.zip");
        {
            zip::Writer w;
            QVERIFY(w.open(zp).ok());
            QVERIFY(w.addData("eboot.bin", "orig").ok());
            QVERIFY(w.finish().ok());
        }
        // No record names this ZIP: refused, nothing changed.
        const auto before = snapshotTree(e.game);
        auto refused = restoreOriginals({e.game, zp, QString()}, CancellationToken());
        QVERIFY(!refused.ok());
        QCOMPARE(codeOf(refused.error()), Code::ManifestMismatch);
        QCOMPARE(snapshotTree(e.game), before);
        // With the user's explicit consent it is restored, with a warning.
        RestoreOptions ro{e.game, zp, QString()};
        ro.allowUnrecorded = true;
        auto rep = restoreOriginals(ro, CancellationToken());
        QVERIFY(rep.ok() && rep->ok());
        QCOMPARE(rep->restored, 1);
        QVERIFY(rep->warnings.join(u'|').contains(QStringLiteral("No backport record")));
        QCOMPARE(readBytes(e.game + "/eboot.bin"), QByteArray("orig"));
    }

    // ---- a ZIP folder that is the game's parent (review bug 3) --------------------------
    void zipFolderThatIsTheGamesParentHidesNothing()
    {
        {
            Env e;
            writeBytes(e.game + "/sce_module/libenc.sprx", f7()); // must make the run refuse (S32)
            auto o = e.options();
            o.originalsDir = e.tmp.filePath("games"); // parent of the game folder
            o.backportDir.clear();
            auto plan = planBackport(o, CancellationToken());
            QVERIFY(plan.ok());
            QVERIFY(plan->scan.find(QStringLiteral("sce_module/libc.prx")));
            QVERIFY(plan->scan.find(QStringLiteral("sce_module/libenc.sprx")));
            const auto before = snapshotTree(e.game);
            auto r = runBackport(o, e.ctx);
            QVERIFY(!r.ok());
            QCOMPARE(codeOf(r.error()), Code::SelfEncrypted);
            QCOMPARE(snapshotTree(e.game), before);
        }
        {
            Env e;
            auto o = e.options();
            o.originalsDir = e.tmp.filePath("games");
            o.backportDir = e.tmp.filePath("games"); // both ZIPs next to the games
            const QByteArray libcBefore = readBytes(e.game + "/sce_module/libc.prx");
            auto r = runBackport(o, e.ctx);
            QVERIFY2(r.ok(), qPrintable(r.ok() ? QString() : r.error().toLogString()));
            QVERIFY(readBytes(e.game + "/sce_module/libc.prx") != libcBefore);
            QCOMPARE(sha(readBytes(e.game + "/sce_module/libc.prx")), kGoldenF2Pair4Nid);
            QVERIFY(zipNames(r->originalsZip).contains(QStringLiteral("sce_module/libc.prx")));
            QCOMPARE(QFileInfo(r->originalsZip).absolutePath(), QFileInfo(e.tmp.filePath("games")).absoluteFilePath());
        }
    }

    // ---- records are per game folder (review bug 2) ------------------------------------
    void restoreNeverUsesAnotherDumpsOriginals()
    {
        Env e; // game A
        auto r = runBackport(e.options(), e.ctx);
        QVERIFY(r.ok());
        const QByteArray aManifest = readBytes(r->manifestPath);
        // Game B: another dump of the same title, never backported.
        const QString b = e.tmp.filePath("games/PPSA01234-v1.05");
        makeGame(b, SelfOnly);
        writeBytes(b + "/eboot.bin", selfOf(f1(0x40, 0x06000038, 0x10090001)));
        writeBytes(b + "/sce_module/libc.prx", selfOf(f2(9)));
        const auto bBefore = snapshotTree(b);
        QCOMPARE(findOriginalsZip(b, e.orig), QString()); // never guessed from the title ID
        auto autoRestore = restoreOriginals({b, QString(), e.orig}, CancellationToken());
        QVERIFY(!autoRestore.ok());
        QCOMPARE(codeOf(autoRestore.error()), Code::RestoreFailed);
        QCOMPARE(snapshotTree(b), bBefore);
        // Choosing A's ZIP for B: nothing of B is overwritten, the restore fails.
        auto chosen = restoreOriginals({b, r->originalsZip, e.orig}, CancellationToken());
        QVERIFY(chosen.ok());
        QVERIFY(!chosen->ok());
        QCOMPARE(chosen->restored, 0);
        QCOMPARE(chosen->skipped.size(), 2);
        QVERIFY(!chosen->ownRecord);
        QCOMPARE(snapshotTree(b), bBefore);
        QCOMPARE(readBytes(r->manifestPath), aManifest); // A's record untouched
        // A still restores from its own record.
        auto a = restoreOriginals({e.game, QString(), e.orig}, CancellationToken());
        QVERIFY(a.ok() && a->ok());
        QVERIFY(a->ownRecord);
    }

    void backportOfAnotherDumpKeepsBothRecords()
    {
        Env e; // game A
        const auto aOriginal = snapshotTree(e.game);
        auto ra = runBackport(e.options(), e.ctx);
        QVERIFY(ra.ok());
        const QByteArray aManifest = readBytes(ra->manifestPath);
        const QString b = e.tmp.filePath("games/PPSA01234-v1.05");
        makeGame(b, SelfOnly);
        writeBytes(b + "/eboot.bin", selfOf(f1(0x40, 0x06000038, 0x10090001)));
        const auto bOriginal = snapshotTree(b);
        auto ob = e.options();
        ob.game = b;
        ob.targetSdk = 4;
        auto rb = runBackport(ob, e.ctx);
        QVERIFY2(rb.ok(), qPrintable(rb.ok() ? QString() : rb.error().toLogString()));
        QVERIFY(rb->manifestPath != ra->manifestPath);
        QVERIFY(rb->manifestPath.contains(QStringLiteral("/folders/")));
        QCOMPARE(readBytes(ra->manifestPath), aManifest); // never clobbered
        QCOMPARE(findOriginalsZip(e.game, e.orig), ra->originalsZip);
        QCOMPARE(findOriginalsZip(b, e.orig), rb->originalsZip);
        auto restoreA = restoreOriginals({e.game, QString(), e.orig}, CancellationToken());
        auto restoreB = restoreOriginals({b, QString(), e.orig}, CancellationToken());
        QVERIFY(restoreA.ok() && restoreA->ok());
        QVERIFY(restoreB.ok() && restoreB->ok());
        QCOMPARE(snapshotTree(e.game), aOriginal);
        QCOMPARE(snapshotTree(b), bOriginal);
    }

    void copyOfABackportedDumpIsRefused()
    {
        Env e;
        auto r = runBackport(e.options(), e.ctx);
        QVERIFY(r.ok());
        // The user copies the backported folder: its files are A's backported files.
        const QString c = e.tmp.filePath("games/PPSA01234-copy");
        const auto backported = snapshotTree(e.game);
        for (auto it = backported.cbegin(); it != backported.cend(); ++it)
            writeBytes(c + "/" + it.key(), it.value());
        auto oc = e.options(false);
        oc.game = c;
        auto rc = runBackport(oc, e.ctx);
        QVERIFY(!rc.ok());
        QCOMPARE(codeOf(rc.error()), Code::AlreadyBackported);
        QVERIFY(rc.error().message.contains(QStringLiteral("PPSA01234-app")));
    }

    void restoreLeavesFilesWithUnrecordedContent()
    {
        Env e;
        auto r = runBackport(e.options(), e.ctx);
        QVERIFY(r.ok());
        writeBytes(e.game + "/eboot.bin", selfOf(f1(0x40, 0x05000033, 0x09590001))); // a later hand patch
        const QByteArray handPatched = readBytes(e.game + "/eboot.bin");
        auto rep = restoreOriginals({e.game, QString(), e.orig}, CancellationToken());
        QVERIFY(rep.ok());
        QVERIFY(!rep->ok());
        QCOMPARE(rep->skipped.size(), 1);
        QVERIFY(rep->skipped.front().startsWith(QStringLiteral("eboot.bin")));
        QCOMPARE(readBytes(e.game + "/eboot.bin"), handPatched);
        QCOMPARE(rep->restored, 1); // libc.prx held the recorded backported bytes
        QVERIFY(!rep->manifestUpdated); // a partial restore does not mark the game restored
    }

    // ---- long paths (S28) ----------------------------------------------------------------
    void longGameAndStagingPaths()
    {
        QString name = QStringLiteral("PPSA01234-app");
        Env e(SelfOnly, name);
        // A deep executable: the path in the game and in staging exceeds 259 characters.
        QString deep = QStringLiteral("sce_module");
        while (QString(e.temp + "/staging/unwrapped/" + deep).size() < 300)
            deep += QStringLiteral("/very_long_folder_name");
        writeBytes(e.game + "/" + deep + "/libdeep.prx", selfOf(f2(9)));
        auto o = e.options(false);
        auto r = runBackport(o, e.ctx);
        QVERIFY2(r.ok(), qPrintable(r.ok() ? QString() : r.error().toLogString()));
        QVERIFY(r->warnings.join(u'|').contains(QStringLiteral("long-path")));
        QVERIFY(zipNames(r->backportZip).contains(deep + "/libdeep.prx"));
        QVERIFY(selfio::hasSelfMagic(readBytes(e.game + "/" + deep + "/libdeep.prx")));
    }

    // ---- nothing to do, determinism ------------------------------------------------------
    void nothingToDoWhenAlreadyAtTarget()
    {
        Env e;
        QFile::remove(e.game + "/sce_module/libc.prx");
        QByteArray patched = f1();
        QVERIFY(patchSdk(patched, *sdkPair(4)).ok());
        writeBytes(e.game + "/eboot.bin", selfOf(patched));
        const auto before = snapshotTree(e.game);
        auto r = runBackport(e.options(false), e.ctx);
        QVERIFY(r.ok());
        QCOMPARE(r->status, RunStatus::NothingToDo);
        QVERIFY(r->summary.startsWith(QStringLiteral("No backport is needed")));
        QCOMPARE(snapshotTree(e.game), before);
        QCOMPARE(zipsIn(e.orig), QStringList());
    }

    void deterministicOutputs()
    {
        Env a, b;
        auto ra = runBackport(a.options(), a.ctx);
        auto rb = runBackport(b.options(), b.ctx);
        QVERIFY(ra.ok() && rb.ok());
        QCOMPARE(snapshotTree(a.game), snapshotTree(b.game));
        QCOMPARE(zipNames(ra->backportZip), zipNames(rb->backportZip));
    }

    // ---- jobs through the factory ---------------------------------------------------------
    void jobsRunThroughTheFactory()
    {
        Env e;
        auto schema = std::make_shared<SettingsSchema>();
        DependencyRegistry deps;
        JobFactory jobs;
        ModuleContext mc{*schema, deps, jobs};
        registerModule(mc);
        QVariantMap values{{keys::kOriginalsZipFolder, e.orig},
                           {keys::kBackportZipFolder, e.bp},
                           {keys::kFakelibFolder, e.fakelib},
                           {keys::kScanFolders, QStringList{e.tmp.filePath("games")}}};
        const SettingsValues settings(schema, values, {});

        auto scan = jobs.create(kJobScan, QJsonObject{{"game", e.game}});
        QVERIFY(scan.ok());
        e.progress->setPhases(scan.value()->phases());
        e.ctx.settings = settings;
        auto so = scan.value()->run(e.ctx);
        QVERIFY2(so.ok(), qPrintable(so.ok() ? QString() : so.error().toLogString()));
        QCOMPARE(so->details.value("sdk").toInt(), 7);
        QCOMPARE(so->details.value("canRun").toBool(), true);
        QCOMPARE(so->details.value("executables").toArray().size(), 2);
        QVERIFY(snapshotTree(e.game).size() > 0);

        auto lib = jobs.create(kJobScan, QJsonObject{});
        QVERIFY(lib.ok());
        auto lo = lib.value()->run(e.ctx);
        QVERIFY(lo.ok());
        QCOMPARE(lo->details.value("games").toArray().size(), 1);

        auto run = jobs.create(kJobRun, QJsonObject{{"game", e.game}}, QVariantMap{{keys::kTargetSdk, QStringLiteral("5")}});
        QVERIFY(run.ok());
        QVERIFY(run.value()->validate(settings).ok());
        QCOMPARE(run.value()->phases(), QStringList({"Scan", "Patch", "Package", "Verify", "Apply"}));
        e.progress->setPhases(run.value()->phases());
        JobContext ctx = e.ctx;
        ctx.settings = SettingsValues(schema, values, run.value()->settingsOverrides);
        auto ro = run.value()->run(ctx);
        QVERIFY2(ro.ok(), qPrintable(ro.ok() ? QString() : ro.error().toLogString()));
        QCOMPARE(ro->verification, Verification::Passed);
        QVERIFY(ro->summary.startsWith(QStringLiteral("Backported to SDK 5")));
        QCOMPARE(ro->outputs.size(), 3);
        auto u = selfio::unwrap(readBytes(e.game + "/eboot.bin"));
        QCOMPARE(elfSdkMajor(u.value()).value_or(-1), 5);

        auto bad = jobs.create(kJobRun, QJsonObject{{"game", e.tmp.filePath("nope")}});
        QVERIFY(!bad.value()->validate(settings).ok());
        const SettingsValues noOrig(schema, QVariantMap{}, {});
        QVERIFY(!jobs.create(kJobRun, QJsonObject{{"game", e.game}}).value()->validate(noOrig).ok()); // S9

        auto restore = jobs.create(kJobRestore, QJsonObject{{"game", e.game}});
        QVERIFY(restore.ok());
        e.progress->setPhases(restore.value()->phases());
        auto rs = restore.value()->run(ctx); // finds the ZIP through the originals folder setting
        QVERIFY2(rs.ok(), qPrintable(rs.ok() ? QString() : rs.error().toLogString()));
        QVERIFY(!QFileInfo::exists(e.game + "/fakelib"));
    }
};

QTEST_GUILESS_MAIN(TstBackportRun)
#include "tst_backport_run.moc"
