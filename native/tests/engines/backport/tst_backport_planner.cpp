// Backport preflight: every refusal is found before anything is written
// (S3-S12, S30-S33, S38), as one plan with all findings.

#include "BackportTestEnv.h"

#include "engines/backport/BackportManifest.h"
#include "engines/backport/BackportPlanner.h"
#include "engines/backport/BackupArchive.h"
#include "support/TestSupport.h"

#include <QTest>

using namespace fw;
using namespace fw::backport;
using namespace fwtest::bp;

namespace {

BackportPlan plan(const BackportOptions& o)
{
    auto p = planBackport(o, CancellationToken());
    return p.ok() ? p.value() : BackportPlan{};
}

bool refuses(const BackportPlan& p, Code c)
{
    for (const Issue& i : p.issues)
        if (i.code == c && i.severity == Severity::Refuse)
            return true;
    return false;
}

QString messages(const BackportPlan& p)
{
    QStringList m;
    for (const Issue& i : p.issues)
        m << QStringLiteral("[%1/%2] %3").arg(severityName(i.severity), codeName(i.code), i.message);
    return m.join(u'\n');
}

} // namespace

class TstBackportPlanner : public QObject {
    Q_OBJECT
    QTemporaryDir m_root;

private slots:
    void initTestCase()
    {
        QVERIFY(m_root.isValid());
        fwtest::isolateAppData(m_root);
    }

    void okPlanWritesNothing() // S31: the plan only reads the game
    {
        Env e;
        const auto before = snapshotTree(e.game);
        const BackportPlan p = plan(e.options());
        QVERIFY2(p.canRun(), qPrintable(messages(p)));
        QCOMPARE(p.gameSdk.value_or(-1), 7);
        QCOMPARE(p.prefix, QStringLiteral("PPSA01234 Game (01.000.000)"));
        QCOMPARE(p.stagedFakelibTargets,
                 QStringList({QStringLiteral("fakelib/libSceAgc.sprx"), QStringLiteral("fakelib/libSceNpAuth.sprx")}));
        QVERIFY(p.summary().contains(QStringLiteral("game folder patched")));
        QCOMPARE(snapshotTree(e.game), before);
        QCOMPARE(filesIn(e.orig), QStringList()); // S10 probe removed
        QCOMPARE(filesIn(e.bp), QStringList());
    }

    void targetSdkRules() // S3, S4 (port of test_preflight_ok_and_refusals)
    {
        Env e;
        auto o = e.options();
        o.targetSdk = 8;
        BackportPlan p = plan(o);
        QVERIFY(refuses(p, Code::TargetAboveGameSdk));
        QCOMPARE(p.blocking()->message, QStringLiteral("Target SDK 8 is higher than this game's own SDK 7. Choose SDK 7 or lower."));
        o.targetSdk = 7;
        QVERIFY(plan(o).canRun());
        o.targetSdk = 11; // a real pair, but above the game's SDK 7
        QVERIFY(refuses(plan(o), Code::TargetAboveGameSdk));
        o.targetSdk = 14;
        QVERIFY(refuses(plan(o), Code::TargetSdkInvalid)); // never clamped
        o.targetSdk = 0;
        QVERIFY(refuses(plan(o), Code::TargetSdkInvalid));
    }

    void gameSdkComesFromEbootNotParamJson() // D7, P9
    {
        Env e;
        // param.json says SDK 7 / FW 7.50; make the eboot an SDK 5 build.
        writeBytes(e.game + "/eboot.bin", selfOf(f1(0x40, 0x05000033, 0x09590001)));
        auto o = e.options();
        o.targetSdk = 6;
        BackportPlan p = plan(o);
        QVERIFY(refuses(p, Code::TargetAboveGameSdk));
        QVERIFY(messages(p).contains(QStringLiteral("eboot.bin says SDK 5")));
        // An eboot without a readable SDK cannot be checked.
        writeBytes(e.game + "/eboot.bin", selfOf(f6()));
        QVERIFY(refuses(plan(o), Code::GameSdkUnknown));
    }

    void ebootMustBeAtTheRoot() // S5
    {
        Env e;
        QFile::rename(e.game + "/eboot.bin", e.game + "/sce_module/eboot2.bin");
        BackportPlan p = plan(e.options());
        QVERIFY(refuses(p, Code::NotAGame));
        QVERIFY(refuses(plan([&] { auto o = e.options(); o.game = e.game + "/missing"; return o; }()), Code::NotAGame));
    }

    void encryptedSelfRefusedUpFront() // S32, D2
    {
        Env e(EncryptedRight);
        BackportPlan p = plan(e.options());
        QVERIFY(refuses(p, Code::SelfEncrypted));
        QVERIFY(p.blocking()->message.contains(QStringLiteral("sce_sys/about/right.sprx")));
        auto o = e.options();
        o.encryptedPolicy = EncryptedPolicy::LeaveUnchanged;
        BackportPlan q = plan(o);
        QVERIFY2(q.canRun(), qPrintable(messages(q)));
        QCOMPARE(q.leaveUnchanged, QStringList({QStringLiteral("sce_sys/about/right.sprx")}));
    }

    void unrewritableExecutableRefusedUpFront() // D4 up front
    {
        Env e;
        writeBytes(e.game + "/sce_module/bad.prx", selfOf(f5()));
        BackportPlan p = plan(e.options());
        QVERIFY(refuses(p, Code::ParamSizeInvalid));
        Env e2;
        QByteArray arm = f2();
        put16(arm, 0x12, 0x28);
        writeBytes(e2.game + "/sce_module/arm.prx", arm);
        BackportPlan p2 = plan(e2.options());
        QVERIFY(refuses(p2, Code::SignUnsupported));
        QVERIFY(messages(p2).contains(QStringLiteral("The SDK 4 build was refused")));
    }

    void originalsFolderRequired() // S9
    {
        Env e;
        auto o = e.options();
        o.originalsDir.clear();
        BackportPlan p = plan(o);
        QVERIFY(refuses(p, Code::ZipFolderInvalid));
        o = e.options();
        o.applyToGame = false;
        o.backportDir.clear();
        QVERIFY(refuses(plan(o), Code::ZipFolderInvalid)); // would produce nothing
    }

    void zipFolderRules() // S10, S30, S38
    {
        Env e;
        const QString inside = e.game + "/backups";
        QDir().mkpath(inside);
        auto o = e.options();
        o.originalsDir = inside;
        QVERIFY(messages(plan(o)).contains(QStringLiteral("inside the game folder")));
        o = e.options();
        o.backportDir = inside;
        QVERIFY(refuses(plan(o), Code::ZipFolderInvalid));
        o = e.options();
        o.backportDir = e.orig + "/nested";
        QDir().mkpath(o.backportDir);
        QVERIFY(messages(plan(o)).contains(QStringLiteral("must not be inside each other")));
        o = e.options();
        o.backportDir = e.orig; // same folder: fine, the names differ
        QVERIFY(plan(o).canRun());
        o = e.options();
        o.originalsDir = e.tmp.filePath("missing");
        QVERIFY(messages(plan(o)).contains(QStringLiteral("does not exist")));
        // Long final ZIP path (S10/S28): refused, with the length named.
        QString deep = e.tmp.filePath("z");
        while (deep.size() < 200)
            deep += QStringLiteral("/abcdefghij");
        QDir().mkpath(deep);
        o = e.options();
        o.originalsDir = deep;
        QVERIFY(messages(plan(o)).contains(QStringLiteral("Windows only guarantees 259")));
    }

    void fakelibRules() // S8, S6 (port of test_preflight_fakelib)
    {
        Env e;
        QVERIFY(plan(e.options()).canRun());
        auto o = e.options();
        o.fakelibDir = e.game + "/fakelib";
        writeBytes(e.game + "/fakelib/libSceAgc.sprx", fakeLib(5));
        QVERIFY(messages(plan(o)).contains(QStringLiteral("outside the game folder")));
        // The game's fakelib already holds the set: already backported.
        Env s;
        QDir().mkpath(s.game + "/fakelib");
        QFile::copy(s.fakelib + "/libSceAgc.sprx", s.game + "/fakelib/libSceAgc.sprx");
        QFile::copy(s.fakelib + "/libSceNpAuth.sprx", s.game + "/fakelib/libSceNpAuth.sprx");
        BackportPlan p = plan(s.options());
        QVERIFY(refuses(p, Code::AlreadyBackported));
        QVERIFY(p.blocking()->message.contains(QStringLiteral("already contains backport files")));
        // A nested set is refused.
        Env n;
        auto no = n.options();
        no.fakelibDir = n.tmp.filePath("parent");
        writeBytes(no.fakelibDir + "/fakelib/libSceAgc.sprx", fakeLib(1));
        QVERIFY(refuses(plan(no), Code::FakelibInvalid));
    }

    void anotherToolsFakelibIsKeptAndExtrasListed() // S14, D3 / G1(d)
    {
        Env e(OtherFakelib);
        BackportPlan p = plan(e.options());
        QVERIFY2(p.canRun(), qPrintable(messages(p)));
        QCOMPARE(p.fakelibState, FakelibState::Other);
        QCOMPARE(p.replacedFakelibFiles, QStringList({QStringLiteral("fakelib/libSceAgc.sprx")}));
        QCOMPARE(p.fakelibExtras, QStringList({QStringLiteral("fakelib/libSceAmpr.sprx")}));
        QVERIFY(messages(p).contains(QStringLiteral("libSceAmpr.sprx")));
    }

    void nestedEbootGetsItsOwnFakelib() // G1(h)
    {
        Env e(NestedEboot);
        BackportPlan p = plan(e.options());
        QVERIFY(p.stagedFakelibTargets.contains(QStringLiteral("sub/fakelib/libSceAgc.sprx")));
        QCOMPARE(p.stagedFakelibTargets.size(), 4);
    }

    void manifestVerdictRules() // S7
    {
        Env e;
        const QString path = manifestPathFor(e.orig, QStringLiteral("PPSA01234"));
        Manifest m = newManifest(path, e.game, QStringLiteral("PPSA01234"), QStringLiteral("Game"));
        recordOriginals(m, {e.orig + "/x.zip", "00", {{"eboot.bin", QString::fromLatin1(sha(readBytes(e.game + "/eboot.bin")))}}});
        TargetRecord t;
        t.sdk = 4;
        t.postHashes.insert(QStringLiteral("eboot.bin"), QString::fromLatin1(sha("patched eboot")));
        recordTarget(m, t);
        QVERIFY(writeManifest(m).ok());
        BackportPlan p = plan(e.options());
        QVERIFY2(p.canRun(), qPrintable(messages(p))); // still the originals
        QCOMPARE(p.verdict, Verdict::Original);
        writeBytes(e.game + "/eboot.bin", "patched eboot");
        QCOMPARE(plan(e.options()).verdict, Verdict::Patched);
        QVERIFY(refuses(plan(e.options()), Code::AlreadyBackported));
        writeBytes(e.game + "/eboot.bin", selfOf(f1(0x40, 0x07000038, 0x09590002)));
        BackportPlan d = plan(e.options());
        QCOMPARE(d.verdict, Verdict::Differs);
        QVERIFY(!d.canRun());
        QCOMPARE(d.blocking()->severity, Severity::Confirm);
        auto o = e.options();
        o.confirmDiffers = true;
        QVERIFY(plan(o).canRun());
    }

    void freeSpaceChecked() // S12 with a fake probe
    {
        Env e;
        auto o = e.options();
        o.freeSpaceMarginBytes = 1024 * 1024;
        o.diskSpaceProbe = [&](const QString& p) -> Result<DiskSpace> {
            DiskSpace d;
            d.rootPath = p.startsWith(e.orig) ? QStringLiteral("Z:/") : QStringLiteral("C:/");
            d.available = p.startsWith(e.orig) ? 1000 : (qint64(1) << 40);
            return d;
        };
        BackportPlan p = plan(o);
        QVERIFY(refuses(p, Code::InsufficientSpace));
        QVERIFY(p.blocking()->message.contains(QStringLiteral("originals ZIP")));
        o.diskSpaceProbe = [](const QString&) -> Result<DiskSpace> {
            DiskSpace d;
            d.rootPath = QStringLiteral("C:/");
            d.available = qint64(1) << 40;
            return d;
        };
        QVERIFY(plan(o).canRun());
        // Every use on one volume is summed.
        qint64 need = 0;
        o.diskSpaceProbe = [](const QString&) -> Result<DiskSpace> {
            DiskSpace d;
            d.rootPath = QStringLiteral("C:/");
            d.available = 0;
            return d;
        };
        BackportPlan q = plan(o);
        QCOMPARE(int(q.space.size()), 1);
        QVERIFY(q.space[0].uses.contains(QStringLiteral("staging")));
        QVERIFY(q.space[0].uses.contains(QStringLiteral("backport ZIP")));
        need = q.space[0].needBytes;
        QVERIFY(need > 3 * (readBytes(e.game + "/eboot.bin").size()));
    }

    void longStagingPathIsWarned() // S11 (long-path aware, S28)
    {
        Env e;
        auto o = e.options();
        o.stagingHint = e.tmp.filePath(QString(240, u's'));
        BackportPlan p = plan(o);
        QVERIFY(p.canRun());
        QVERIFY(p.longestStagingPath > kMaxWinPath);
        bool warned = false;
        for (const Issue& i : p.issues)
            warned = warned || (i.code == Code::PathTooLong && i.severity == Severity::Warn);
        QVERIFY(warned);
    }

    void nothingToDoAndLeftovers()
    {
        Env e;
        QFile::remove(e.game + "/sce_module/libc.prx");
        writeBytes(e.game + "/eboot.bin", selfOf(f7()));
        auto o = e.options();
        o.fakelibDir.clear();
        o.encryptedPolicy = EncryptedPolicy::LeaveUnchanged;
        QVERIFY(refuses(plan(o), Code::NothingToDo) || refuses(plan(o), Code::GameSdkUnknown));
        Env g(LeftoverBak);
        QVERIFY(messages(plan(g.options())).contains(QStringLiteral("eboot.bin.bak")));
    }
};

QTEST_GUILESS_MAIN(TstBackportPlanner)
#include "tst_backport_planner.moc"
