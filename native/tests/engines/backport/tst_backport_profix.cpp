// PS5 Pro fix, shader-assert part (docs/BACKPORT_PARITY.md matrix #32,
// ProFix.h): the byte rule for guarded "s_trap 1" sites (ported from the
// owner's Backport engine families_v2.shader_trap_sites and its tests
// test_pro_fix_options.py), the patch, and the run: opt-in, root eboot.bin
// only, "... Pro fix backport files.zip".

#include "BackportTestEnv.h"

#include "engines/backport/BackportEngine.h"
#include "engines/backport/ProFix.h"
#include "engines/backport/SelfReader.h"
#include "support/TestSupport.h"

#include <QFileInfo>
#include <QJsonObject>
#include <QTest>

using namespace fw;
using namespace fw::backport;
using namespace fwtest::bp;

namespace {

constexpr quint32 kTrap = 0xBF920001u;
constexpr quint32 kNop = 0xBF800000u;

// F1 (code PT_LOAD at file 0x1000 = VA 0, data PT_LOAD at file 0x5800 =
// VA 0x8000) with two real sites and every kind of non-site.
QByteArray f1WithTraps()
{
    QByteArray b = f1();
    put32(b, 0x1100, 0xBF850001u); // s_cbranch_scc1 +1
    put32(b, 0x1104, kTrap);       // site 1 (VA 0x104)
    put32(b, 0x1200, 0xBF820001u); // s_branch +1: not a guard
    put32(b, 0x1204, kTrap);
    put32(b, 0x1300, 0xBF850002u); // skips two instructions: not a guard
    put32(b, 0x1304, kTrap);
    put32(b, 0x1400, 0x12345678u); // unguarded trap
    put32(b, 0x1404, kTrap);
    put32(b, 0x1501, 0xBF890001u); // guard + trap at an unaligned VA (0x505)
    put32(b, 0x1505, kTrap);
    put32(b, 0x5840, 0xBF880001u); // s_cbranch_execz +1, in the data PT_LOAD
    put32(b, 0x5844, kTrap);       // site 2 (VA 0x8044)
    return b;
}

// F2 (PRX) with one guarded trap in its code segment.
QByteArray f2WithTrap()
{
    QByteArray b = f2();
    put32(b, 0x1100, 0xBF840001u);
    put32(b, 0x1104, kTrap);
    return b;
}

QByteArray unwrapFile(const QString& path)
{
    const QByteArray d = readBytes(path);
    if (fw::backport::elf::hasElfMagic(d))
        return d;
    auto u = selfio::unwrap(d);
    return u.ok() ? u.value() : QByteArray();
}

} // namespace

class TstBackportProFix : public QObject {
    Q_OBJECT
    QTemporaryDir m_root;

private slots:
    void initTestCase()
    {
        QVERIFY(m_root.isValid());
        fwtest::isolateAppData(m_root);
    }

    void guardRule()
    {
        for (quint32 hi = 0xBF84; hi <= 0xBF89; ++hi)
            QVERIFY(isShaderTrapGuard((hi << 16) | 1));
        QVERIFY(!isShaderTrapGuard(0xBF820001u)); // s_branch
        QVERIFY(!isShaderTrapGuard(0xBF830001u));
        QVERIFY(!isShaderTrapGuard(0xBF8A0001u));
        QVERIFY(!isShaderTrapGuard(0xBF850002u)); // skip count 2
        QVERIFY(!isShaderTrapGuard(0xBF850000u));
    }

    void findsOnlyGuardedAlignedTraps()
    {
        auto sites = findShaderTrapSites(f1WithTraps());
        QVERIFY2(sites.ok(), qPrintable(sites.ok() ? QString() : sites.error().message));
        QCOMPARE(int(sites->size()), 2);
        QCOMPARE(sites->at(0).fileOffset, quint64(0x1104));
        QCOMPARE(sites->at(0).va, quint64(0x104));
        QCOMPARE(sites->at(1).fileOffset, quint64(0x5844));
        QCOMPARE(sites->at(1).va, quint64(0x8044));
        // The plain fixtures have none.
        QCOMPARE(int(findShaderTrapSites(f1()).value().size()), 0);
        QCOMPARE(int(findShaderTrapSites(f2()).value().size()), 0);
    }

    void patchIsExactAndIdempotent()
    {
        const QByteArray before = f1WithTraps();
        QByteArray e = before;
        auto n = applyShaderTrapNops(e);
        QVERIFY(n.ok());
        QCOMPARE(n.value(), 2);
        QCOMPARE(e.size(), before.size());
        QCOMPARE(get32(e, 0x1104), kNop);
        QCOMPARE(get32(e, 0x5844), kNop);
        // Nothing else changed, the non-sites included.
        int changed = 0;
        for (qsizetype i = 0; i < e.size(); ++i)
            if (e[i] != before[i])
                ++changed;
        QVERIFY(changed > 0 && changed <= 8);
        QCOMPARE(get32(e, 0x1204), kTrap);
        QCOMPARE(get32(e, 0x1304), kTrap);
        QCOMPARE(get32(e, 0x1404), kTrap);
        QCOMPARE(qFromLittleEndian<quint32>(e.constData() + 0x1505), kTrap);
        // Already no-ops: nothing to do.
        QByteArray again = e;
        QCOMPARE(applyShaderTrapNops(again).value(), 0);
        QCOMPARE(again, e);
    }

    void refusesWhatIsNotAnElf64()
    {
        QCOMPARE(codeOf(findShaderTrapSites(QByteArray("not an elf at all, just text")).error()), Code::ElfInvalid);
        QByteArray e32 = f1WithTraps();
        e32[4] = 1; // ELFCLASS32
        QVERIFY(!findShaderTrapSites(e32).ok());
        QByteArray cut = f1WithTraps().left(0x3000); // the code segment runs past the end
        QCOMPARE(codeOf(findShaderTrapSites(cut).error()), Code::ElfInvalid);
    }

    void pipelinePatchesOnlyWhenAsked()
    {
        const QByteArray self = selfOf(f1WithTraps());
        auto plain = processExecutable(self, *sdkPair(4), NidMode::Auto, true);
        QVERIFY(plain.ok());
        QCOMPARE(plain->shaderTrapNops, 0);
        QCOMPARE(get32(plain->elf, 0x1104), kTrap);

        ExecutableFixes fixes;
        fixes.shaderTrapNops = true;
        auto pro = processExecutable(self, *sdkPair(4), NidMode::Auto, true, nullptr, fixes);
        QVERIFY2(pro.ok(), qPrintable(pro.ok() ? QString() : pro.error().message));
        QCOMPARE(pro->shaderTrapNops, 2);
        auto u = selfio::unwrap(pro->output); // V1-V3 passed; the signed file carries the no-ops
        QVERIFY(u.ok());
        QCOMPARE(get32(u.value(), 0x1104), kNop);
        QCOMPARE(get32(u.value(), 0x5844), kNop);
        QCOMPARE(elfSdkMajor(u.value()).value_or(-1), 4);
    }

    void runPatchesOnlyTheRootEbootAndNamesTheZip()
    {
        Env e;
        QVERIFY(writeBytes(e.game + "/eboot.bin", selfOf(f1WithTraps())));
        QVERIFY(writeBytes(e.game + "/sce_module/libc.prx", selfOf(f2WithTrap())));
        auto o = e.options(false);
        o.proFix = true;
        auto r = runBackport(o, e.ctx);
        QVERIFY2(r.ok(), qPrintable(r.ok() ? QString() : r.error().toLogString()));
        const QString name = QFileInfo(r->backportZip).fileName();
        QVERIFY2(name.startsWith(QStringLiteral("FW 4xx ")), qPrintable(name));
        QVERIFY2(name.endsWith(QStringLiteral(" Pro fix backport files.zip")), qPrintable(name));
        const QJsonObject pro = r->details.value(QStringLiteral("proFix")).toObject();
        QCOMPARE(pro.value(QStringLiteral("sites")).toInt(), 2);
        QCOMPARE(pro.value(QStringLiteral("family")).toString(), QLatin1String(kShaderTrapFamily));
        // Applied to the game: the eboot has the no-ops, the module keeps its trap.
        const QByteArray eboot = unwrapFile(e.game + "/eboot.bin");
        QCOMPARE(get32(eboot, 0x1104), kNop);
        QCOMPARE(get32(eboot, 0x5844), kNop);
        const QByteArray libc = unwrapFile(e.game + "/sce_module/libc.prx");
        QCOMPARE(get32(libc, 0x1104), kTrap);
    }

    void proFixWithoutSitesKeepsTheNormalName()
    {
        Env e; // the plain fixture eboot has no traps
        auto o = e.options(false);
        o.proFix = true;
        auto r = runBackport(o, e.ctx);
        QVERIFY2(r.ok(), qPrintable(r.ok() ? QString() : r.error().toLogString()));
        QVERIFY(!QFileInfo(r->backportZip).fileName().contains(QStringLiteral("Pro fix")));
        QCOMPARE(r->details.value(QStringLiteral("proFix")).toObject().value(QStringLiteral("applied")).toBool(), false);
    }

    void proFixOffLeavesTheTraps()
    {
        Env e;
        QVERIFY(writeBytes(e.game + "/eboot.bin", selfOf(f1WithTraps())));
        auto r = runBackport(e.options(false), e.ctx);
        QVERIFY2(r.ok(), qPrintable(r.ok() ? QString() : r.error().toLogString()));
        QVERIFY(!QFileInfo(r->backportZip).fileName().contains(QStringLiteral("Pro fix")));
        QVERIFY(!r->details.contains(QStringLiteral("proFix")));
        QCOMPARE(get32(unwrapFile(e.game + "/eboot.bin"), 0x1104), kTrap);
    }
};

QTEST_GUILESS_MAIN(TstBackportProFix)
#include "tst_backport_profix.moc"
