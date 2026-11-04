// Report-only detectors (docs/BACKPORT_PARITY.md matrix #41 and #40): the
// Unity/Il2Cpp module-name case detector (imports in DYNLIBDATA against the
// files in sce_module/, case-sensitive) and the read-only audio scanner
// (legacy channel-config cluster, twin branches, API names, loose leads and
// the confirmed profiles). Neither changes anything.

#include "BackportTestEnv.h"

#include "engines/backport/AudioScan.h"
#include "engines/backport/BackportJobs.h"
#include "engines/backport/BackportPlanner.h"
#include "engines/backport/BackportSettings.h"
#include "engines/backport/Module.h"
#include "engines/backport/ModuleImports.h"
#include "engines/backport/RunReport.h"
#include "engines/backport/SelfReader.h"
#include "support/TestSupport.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QTest>

using namespace fw;
using namespace fw::backport;
using namespace fwtest::bp;

namespace {

const QStringList kNeeded = {QStringLiteral("libc.prx"), QStringLiteral("Il2CppUserAssemblies.prx")};
const QStringList kModules = {QStringLiteral("Il2CppUserAssemblies")};

const Issue* findIssue(const BackportPlan& p, Code c)
{
    for (const Issue& i : p.issues)
        if (i.code == c)
            return &i;
    return nullptr;
}

// F2 whose DYNLIBDATA holds bare names (no dynamic table): the fallback path.
QByteArray f2WithNames()
{
    QByteArray b = f2();
    const char names[] = "printf#A#B\0" "Il2CppUserAssemblies\0" "x\0" "\x01\x02\x03\0" "libSceAudioOut";
    std::memcpy(b.data() + 0x3000, names, sizeof names);
    return b;
}

QByteArray cluster(quint8 v)
{
    QByteArray c = QByteArray::fromHex("c7459400");
    c.append(char(v));
    c.append(QByteArray::fromHex("0000c703"));
    c.append(char(v));
    c.append(QByteArray::fromHex("0000006a"));
    c.append(char(v));
    c.append(char(0x58));
    return c; // c7 45 94 00 v 00 00 c7 03 v 00 00 00 6a v 58
}

QByteArray filler(qsizetype n)
{
    return QByteArray(n, char(0x90));
}

JobContext jobContext(Env& e, const std::shared_ptr<SettingsSchema>& schema, const QVariantMap& values)
{
    JobContext ctx = e.ctx;
    ctx.settings = SettingsValues(schema, values, {});
    return ctx;
}

} // namespace

class TstBackportDetect : public QObject {
    Q_OBJECT
    QTemporaryDir m_root;

private slots:
    void initTestCase()
    {
        QVERIFY(m_root.isValid());
        fwtest::isolateAppData(m_root);
    }

    // ---- #41 module-name case ------------------------------------------------------------

    void readsImportsFromTheDynamicTable()
    {
        const ModuleImports m = readModuleImports(f1WithImports(kNeeded, kModules));
        QVERIFY(m.fromDynamicTable);
        QCOMPARE(m.names, QStringList({"libc.prx", "Il2CppUserAssemblies.prx", "Il2CppUserAssemblies"}));
        // Through a SELF (the DYNLIBDATA segment is signed, PT_DYNAMIC lies inside it).
        auto u = selfio::unwrap(selfOf(f1WithImports(kNeeded, kModules)));
        QVERIFY(u.ok());
        QCOMPARE(readModuleImports(u.value()).names, m.names);
    }

    void fallsBackToTheDynlibNames()
    {
        const ModuleImports m = readModuleImports(f2WithNames());
        QVERIFY(!m.fromDynamicTable);
        QCOMPARE(m.names, QStringList({"Il2CppUserAssemblies", "libSceAudioOut"})); // NIDs, short and binary tokens dropped
        QCOMPARE(readModuleImports(f2()).names, QStringList()); // only NIDs
        QCOMPARE(readModuleImports(QByteArray("not an elf")).names, QStringList());
    }

    void hostileDynamicTablesNeverCrash()
    {
        QRandomGenerator rng(4242);
        const QByteArray base = f1WithImports(kNeeded, kModules);
        for (int round = 0; round < 400; ++round) {
            QByteArray b = base;
            // Mutate the dynamic table, the string table and the phdr of PT_DYNAMIC.
            for (int k = 0; k < 6; ++k) {
                const qsizetype at = rng.bounded(2) ? 0x5900 + rng.bounded(0x100) : 0x40 + 5 * 0x38 + rng.bounded(0x38);
                b[at] = char(rng.bounded(256));
            }
            const ModuleImports m = readModuleImports(b);
            for (const QString& n : m.names)
                QVERIFY(n.size() <= 255);
        }
        QByteArray huge = base;
        put64(huge, 0x5900 + 8, 0xFFFFFFFFFFFFFF00ull); // DT_SCE_STRTAB far outside
        put64(huge, 0x40 + 5 * 0x38 + 0x20, 0x7FFFFFFFFFFFFFFFull); // PT_DYNAMIC p_filesz
        QVERIFY(readModuleImports(huge).names.size() <= 3);
    }

    void caseMismatchIsAScanIssue()
    {
        Env e;
        writeBytes(e.game + "/eboot.bin", selfOf(f1WithImports(kNeeded, kModules)));
        writeBytes(e.game + "/sce_module/Il2cppUserAssemblies.prx", selfOf(f2(5)));
        const auto before = snapshotTree(e.game);
        auto plan = planBackport(e.options(), e.ctx.token);
        QVERIFY(plan.ok());
        QCOMPARE(plan->moduleCase.size(), size_t(1));
        QCOMPARE(plan->moduleCase.front().importer, QStringLiteral("eboot.bin"));
        QCOMPARE(plan->moduleCase.front().file, QStringLiteral("sce_module/Il2cppUserAssemblies.prx"));
        const Issue* i = findIssue(plan.value(), Code::ModuleNameCase);
        QVERIFY(i);
        QCOMPARE(i->severity, Severity::Warn); // detector only: the backport is not blocked
        QVERIFY(i->message.contains(QStringLiteral("Il2cppUserAssemblies.prx")));
        QVERIFY(i->message.contains(QStringLiteral("not renamed")));
        QVERIFY(plan->canRun());
        QCOMPARE(snapshotTree(e.game), before);

        // A full run keeps the file names exactly as they were.
        auto r = runBackport(e.options(), e.ctx);
        QVERIFY2(r.ok(), qPrintable(r.ok() ? QString() : r.error().toLogString()));
        QVERIFY(QFileInfo::exists(e.game + "/sce_module/Il2cppUserAssemblies.prx"));
        QVERIFY(!r->warnings.filter(QStringLiteral("differ in case")).isEmpty());
    }

    void exactCaseAndUnrelatedModulesAreNotReported()
    {
        Env e;
        writeBytes(e.game + "/eboot.bin", selfOf(f1WithImports(kNeeded, kModules)));
        writeBytes(e.game + "/sce_module/Il2CppUserAssemblies.prx", selfOf(f2(5)));
        auto plan = planBackport(e.options(), e.ctx.token);
        QVERIFY(plan.ok());
        QVERIFY(plan->moduleCase.empty()); // libc.prx is also there, in the same case
        QVERIFY(!findIssue(plan.value(), Code::ModuleNameCase));
    }

    void libraryScanReportsTheMismatch()
    {
        Env e;
        writeBytes(e.game + "/eboot.bin", selfOf(f1WithImports(kNeeded, kModules)));
        writeBytes(e.game + "/sce_module/Il2cppUserAssemblies.sprx", selfOf(f2(5)));
        auto schema = std::make_shared<SettingsSchema>();
        DependencyRegistry deps;
        JobFactory jobs;
        ModuleContext mc{*schema, deps, jobs};
        registerModule(mc);
        auto lib = jobs.create(kJobScan, QJsonObject{{"folders", QJsonArray{e.tmp.filePath("games")}}});
        QVERIFY(lib.ok());
        JobContext ctx = jobContext(e, schema, {});
        auto o = lib.value()->run(ctx);
        QVERIFY(o.ok());
        const QJsonObject g = o->details.value("games").toArray().first().toObject();
        const QJsonArray mm = g.value("moduleCaseMismatches").toArray();
        QCOMPARE(mm.size(), 1);
        QCOMPARE(mm.first().toObject().value("file").toString(), QStringLiteral("sce_module/Il2cppUserAssemblies.sprx"));
        QCOMPARE(g.value("issues").toArray().first().toObject().value("code").toString(), QStringLiteral("ModuleNameCase"));
    }

    // ---- #40 audio, report only ----------------------------------------------------------

    void unpatchedClusterIsFound()
    {
        const QByteArray d = filler(0x100) + cluster(0x0C) + filler(0x100);
        const AudioFileReport r = scanAudioBytes(d, QStringLiteral("eboot.bin"));
        QCOMPARE(r.status, AudioClusterStatus::PresentUnpatched);
        QCOMPARE(r.unpatched.size(), size_t(1));
        QCOMPARE(r.unpatched.front().anchor, qint64(0x100));
        QCOMPARE(r.unpatched.front().siteA, qint64(0x104));
        QCOMPARE(r.unpatched.front().siteB, qint64(0x109));
        QCOMPARE(r.unpatched.front().siteC, qint64(0x10E));
        QCOMPARE(quint8(d[0x104]), quint8(0x0C));
        QCOMPARE(quint8(d[0x109]), quint8(0x0C));
        QCOMPARE(quint8(d[0x10E]), quint8(0x0C));
        QCOMPARE(r.leads.size(), size_t(1)); // the three 0x0C bytes are also a loose lead
        QCOMPARE(r.tier, AudioTier::WeakLead);
    }

    void patchedAndMixedClusters()
    {
        QCOMPARE(scanAudioBytes(filler(64) + cluster(0x08), {}).status, AudioClusterStatus::AlreadyPatched);
        QCOMPARE(scanAudioBytes(cluster(0x08) + filler(64) + cluster(0x0C), {}).status, AudioClusterStatus::Mixed);
    }

    void loneFragmentsNeverCount()
    {
        QByteArray d = filler(0x200);
        const QByteArray b = QByteArray::fromHex("c7030c000000");
        const QByteArray c = QByteArray::fromHex("6a0c58");
        d.replace(0x10, b.size(), b);
        d.replace(0x100, c.size(), c);
        d.replace(0x180, 4, QByteArray::fromHex("c7459400")); // fragment A with a 0x90 value
        const AudioFileReport r = scanAudioBytes(d, {});
        QCOMPARE(r.status, AudioClusterStatus::Missing);
        QVERIFY(r.leads.empty()); // single 0x0C bytes
        QCOMPARE(r.tier, AudioTier::NoMatch);
        // A 4th fragment-A copy far away does not pull in B/C from outside the window.
        QByteArray far = filler(0x400) + QByteArray::fromHex("c74594000c") + filler(0x40) + b + c;
        QCOMPARE(scanAudioBytes(far, {}).status, AudioClusterStatus::Missing);
    }

    void twinBranchAndApiNames()
    {
        QByteArray d = filler(0x80) + QByteArray::fromHex("6a0858") + filler(20) + QByteArray::fromHex("6a0c58") + filler(0x80);
        d += "sceAudioOut2PortCreate";
        const AudioFileReport r = scanAudioBytes(d, {});
        QCOMPARE(r.twins.size(), size_t(1));
        QCOMPARE(r.twins.front().encoding, QStringLiteral("DIRECT_IMM8"));
        QCOMPARE(r.twins.front().eightOffset, qint64(0x80));
        QCOMPARE(r.twins.front().twelveOffset, qint64(0x80 + 3 + 20));
        QCOMPARE(r.twins.front().gap, 23);
        QCOMPARE(r.tier, AudioTier::TwinBranch);
        QCOMPARE(r.apiStrings, QStringList({"sceAudioOut2PortCreate", "sceAudioOut2"}));
        // Too far apart: no twin.
        QByteArray far = QByteArray::fromHex("6a0858") + filler(60) + QByteArray::fromHex("6a0c58");
        QVERIFY(scanAudioBytes(far, {}).twins.empty());
    }

    void leadsAreCappedAndCoalesced()
    {
        QByteArray d;
        for (int i = 0; i < 1000; ++i)
            d += QByteArray::fromHex("0c0c0c") + filler(20);
        const AudioFileReport r = scanAudioBytes(d, {});
        QCOMPARE(int(r.leads.size()), kAudioLeadCap);
        QCOMPARE(r.leads.front().count, 3);
        QCOMPARE(r.leads.front().end - r.leads.front().start, qint64(3));
    }

    void confirmedProfileIsRecognised()
    {
        // SpiderMan2 PPSA03016 01.004.003: both sites at their recorded offsets.
        QByteArray d(0x942500, '\0');
        d.replace(0x942493, 5, QByteArray::fromHex("b8000c0000"));
        d.replace(0x9422B5, 4, QByteArray::fromHex("08f1632d"));
        const AudioFileReport r = scanAudioBytes(d, {});
        QCOMPARE(r.profiles.size(), size_t(1));
        QCOMPARE(r.profiles.front().titleId, QStringLiteral("PPSA03016"));
        QVERIFY(r.profiles.front().allPresent);
        QVERIFY(r.profiles.front().anyUnpatched);
        QCOMPARE(r.profiles.front().sites.at(0).state, QStringLiteral("unpatched"));
        QCOMPARE(r.profiles.front().sites.at(1).state, QStringLiteral("patched"));
        QCOMPARE(r.tier, AudioTier::ConfirmedProfile);
        QVERIFY(scanAudioBytes(QByteArray(0x1000, '\0'), {}).profiles.empty());
    }

    void audioScanJobIsReadOnlyAndWritesAReport()
    {
        Env e;
        QByteArray elf = f1();
        elf.replace(0x2000, 16, cluster(0x0C));
        writeBytes(e.game + "/eboot.bin", selfOf(elf));
        const auto before = snapshotTree(e.game);
        auto schema = std::make_shared<SettingsSchema>();
        DependencyRegistry deps;
        JobFactory jobs;
        ModuleContext mc{*schema, deps, jobs};
        registerModule(mc);
        auto job = jobs.create(kJobAudioScan, QJsonObject{{"game", e.game}});
        QVERIFY(job.ok());
        const QVariantMap values{{keys::kOriginalsZipFolder, e.orig}};
        QVERIFY(job.value()->validate(SettingsValues(schema, values, {})).ok());
        JobContext ctx = jobContext(e, schema, values);
        auto o = job.value()->run(ctx);
        QVERIFY2(o.ok(), qPrintable(o.ok() ? QString() : o.error().toLogString()));
        QCOMPARE(snapshotTree(e.game), before); // read-only
        QCOMPARE(o->verification, Verification::Skipped);
        const QJsonObject summary = o->details.value("summary").toObject();
        QCOMPARE(summary.value("unpatchedClusters").toInt(), 1);
        QVERIFY(summary.value("patch").toString().contains(QStringLiteral("read-only")));
        const QString html = QDir::fromNativeSeparators(o->details.value("report").toObject().value("html").toString());
        QVERIFY(QFileInfo(html).isFile());
        QCOMPARE(QFileInfo(html).absolutePath(), QDir(e.orig).filePath("BackportBackup/PPSA01234/reports"));
        const QJsonObject rep = QJsonDocument::fromJson(readBytes(html.chopped(5) + ".json")).object();
        QCOMPARE(rep.value("kind").toString(), QStringLiteral("audioScan"));
        QCOMPARE(rep.value("outcome").toObject().value("status").toString(), QStringLiteral("reportOnly"));
        QCOMPARE(rep.value("clusters").toArray().size(), 1);
        QVERIFY(QString::fromUtf8(readBytes(html)).contains(QStringLiteral("Audio report (read-only)")));

        // Every executable, on request; a missing game is refused up front.
        auto all = jobs.create(kJobAudioScan, QJsonObject{{"game", e.game}, {"allExecutables", true}});
        auto ao = all.value()->run(ctx);
        QVERIFY(ao.ok());
        QCOMPARE(ao->details.value("files").toArray().size(), 2);
        QVERIFY(!jobs.create(kJobAudioScan, QJsonObject{{"game", e.tmp.filePath("nope")}})
                     .value()
                     ->validate(SettingsValues(schema, values, {}))
                     .ok());
    }
};

QTEST_GUILESS_MAIN(TstBackportDetect)
#include "tst_backport_detect.moc"
