// User-supplied AMPR/PlayGo providers with hash pinning (docs/BACKPORT_PARITY.md
// matrix #34): folder rules, trust on first use, a changed / added / missing
// file refused until re-pinned, providers staged where the fakelib set is
// (replacing the set's or the dump's own copies), the pin job.

#include "BackportTestEnv.h"

#include "core/Zip.h"
#include "engines/backport/BackportJobs.h"
#include "engines/backport/BackportPlanner.h"
#include "engines/backport/BackportSettings.h"
#include "engines/backport/Module.h"
#include "engines/backport/ProviderSet.h"
#include "engines/backport/RestoreService.h"
#include "support/TestSupport.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QTest>

using namespace fw;
using namespace fw::backport;
using namespace fwtest::bp;

namespace {

struct ProvEnv : Env {
    QString prov, pins;
    explicit ProvEnv(unsigned variant = SelfOnly) : Env(variant)
    {
        prov = tmp.filePath(QStringLiteral("providers"));
        pins = tmp.filePath(QStringLiteral("pins/provider-pins.json"));
        writeBytes(prov + "/libSceAmpr.sprx", selfOf(fakeLib(91)));
        writeBytes(prov + "/libScePlayGo.sprx", selfOf(fakeLib(92)));
        writeBytes(prov + "/libSceOther.sprx", selfOf(fakeLib(93))); // not a provider: ignored
        writeBytes(prov + "/README.txt", QByteArray("built from ampr_emu"));
    }
    BackportOptions provOptions(bool withFakelib = true) const
    {
        BackportOptions o = options(withFakelib);
        o.providerDir = prov;
        o.providerPinsPath = pins;
        return o;
    }
};

const Issue* findIssue(const BackportPlan& p, Code c, Severity s)
{
    for (const Issue& i : p.issues)
        if (i.code == c && i.severity == s)
            return &i;
    return nullptr;
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

} // namespace

class TstBackportProviders : public QObject {
    Q_OBJECT
    QTemporaryDir m_root;

private slots:
    void initTestCase()
    {
        QVERIFY(m_root.isValid());
        fwtest::isolateAppData(m_root);
    }

    void folderRules()
    {
        ProvEnv e;
        FakelibSource set;
        QStringList ignored;
        QVERIFY(providerProblems(e.prov, e.game, &set, &ignored).isEmpty());
        QCOMPARE(set.libs.size(), size_t(2));
        QCOMPARE(set.libs.front().name, QStringLiteral("libSceAmpr.sprx"));
        QCOMPARE(ignored, QStringList({"libSceOther.sprx"}));
        QVERIFY(isProviderLibraryName("LIBSCEPLAYGO.PRX"));
        QVERIFY(!isProviderLibraryName("libSceAgc.sprx"));

        QVERIFY(!providerProblems(e.tmp.filePath("missing"), e.game).isEmpty());
        QVERIFY(!providerProblems(e.game, e.game).isEmpty());                       // inside the game
        QDir().mkpath(e.tmp.filePath("bad"));
        writeBytes(e.tmp.filePath("bad/libSceAmpr.sprx"), QByteArray(100, 'x'));   // undersized
        QVERIFY(providerProblems(e.tmp.filePath("bad"), e.game).join(' ').contains("bytes"));
        writeBytes(e.tmp.filePath("bad/libSceAmpr.sprx"), QByteArray(4096, 'x'));  // not ELF/SELF
        QVERIFY(providerProblems(e.tmp.filePath("bad"), e.game).join(' ').contains("ELF/SELF"));
        QDir().mkpath(e.tmp.filePath("nested/sub"));
        writeBytes(e.tmp.filePath("nested/sub/libScePlayGo.sprx"), selfOf(fakeLib(3)));
        QVERIFY(providerProblems(e.tmp.filePath("nested"), e.game).join(' ').contains("sub-folder"));
        QDir().mkpath(e.tmp.filePath("both"));
        writeBytes(e.tmp.filePath("both/libSceAmpr.sprx"), selfOf(fakeLib(3)));
        writeBytes(e.tmp.filePath("both/libSceAmpr.prx"), selfOf(fakeLib(4)));
        QVERIFY(providerProblems(e.tmp.filePath("both"), e.game).join(' ').contains("keep only one"));
        QDir().mkpath(e.tmp.filePath("none"));
        writeBytes(e.tmp.filePath("none/libSceAgc.sprx"), selfOf(fakeLib(3)));
        QVERIFY(providerProblems(e.tmp.filePath("none"), e.game).join(' ').contains("neither"));
    }

    void firstRunPinsAndStagesTheProviders()
    {
        ProvEnv e;
        auto plan = planBackport(e.provOptions(), e.ctx.token);
        QVERIFY(plan.ok());
        QVERIFY(plan->pinProvidersOnRun);
        QVERIFY(findIssue(plan.value(), Code::Ok, Severity::Info));
        QVERIFY(plan->canRun());
        QVERIFY(!QFileInfo::exists(e.pins)); // planning never writes the pins

        auto r = runBackport(e.provOptions(), e.ctx);
        QVERIFY2(r.ok(), qPrintable(r.ok() ? QString() : r.error().toLogString()));
        // Pins recorded with every provider's SHA-256.
        const auto pins = loadProviderPins(e.pins, e.prov);
        QVERIFY(pins);
        QCOMPARE(pins->files.size(), 2);
        QCOMPARE(pins->files.value("libSceAmpr.sprx").sha256, QString::fromLatin1(sha(readBytes(e.prov + "/libSceAmpr.sprx"))));
        // Staged where the fakelib set goes: next to the set's libraries.
        QCOMPARE(readBytes(e.game + "/fakelib/libSceAmpr.sprx"), readBytes(e.prov + "/libSceAmpr.sprx"));
        QCOMPARE(readBytes(e.game + "/fakelib/libScePlayGo.sprx"), readBytes(e.prov + "/libScePlayGo.sprx"));
        QVERIFY(QFileInfo::exists(e.game + "/fakelib/libSceAgc.sprx"));
        QVERIFY(!QFileInfo::exists(e.game + "/fakelib/libSceOther.sprx"));
        QVERIFY(zipNames(r->backportZip).contains("fakelib/libSceAmpr.sprx"));
        // The run report names the providers and their hashes.
        const QJsonObject rep = QJsonDocument::fromJson(readBytes(r->reportJson)).object();
        QCOMPARE(rep.value("inputs").toObject().value("amprPlaygoSha256").toObject().value("libScePlayGo.sprx").toString(),
                 QString::fromLatin1(sha(readBytes(e.prov + "/libScePlayGo.sprx"))));
        QCOMPARE(rep.value("inputs").toObject().value("amprPlaygoPins").toString(), QStringLiteral("pinned by this run"));
    }

    void changedProviderIsRefusedUntilRepinned()
    {
        ProvEnv e;
        FakelibSource set;
        QVERIFY(providerProblems(e.prov, e.game, &set).isEmpty());
        QVERIFY(pinProviders(set, e.pins).ok());
        QVERIFY(checkProviderPins(set, e.pins).ok());

        writeBytes(e.prov + "/libSceAmpr.sprx", selfOf(fakeLib(99))); // replaced behind our back
        const auto before = snapshotTree(e.game);
        auto plan = planBackport(e.provOptions(), e.ctx.token);
        QVERIFY(plan.ok());
        const Issue* i = findIssue(plan.value(), Code::ProviderChanged, Severity::Refuse);
        QVERIFY(i);
        QVERIFY(i->message.contains("libSceAmpr.sprx (pinned "));
        QVERIFY(!plan->canRun());
        auto r = runBackport(e.provOptions(), e.ctx);
        QVERIFY(!r.ok());
        QCOMPARE(codeOf(r.error()), Code::ProviderChanged);
        QCOMPARE(snapshotTree(e.game), before);
        QVERIFY(QDir(e.orig).entryList(QDir::Files).filter(".zip").isEmpty());

        // Re-pinned on purpose: accepted again.
        FakelibSource now;
        QVERIFY(providerProblems(e.prov, e.game, &now).isEmpty());
        QVERIFY(pinProviders(now, e.pins).ok());
        auto again = planBackport(e.provOptions(), e.ctx.token);
        QVERIFY(again.ok() && again->canRun());
        QVERIFY(!again->pinProvidersOnRun);
    }

    void addedOrMissingProviderIsRefused()
    {
        ProvEnv e;
        QFile::remove(e.prov + "/libScePlayGo.sprx");
        FakelibSource set;
        QVERIFY(providerProblems(e.prov, e.game, &set).isEmpty());
        QVERIFY(pinProviders(set, e.pins).ok());
        writeBytes(e.prov + "/libScePlayGo.sprx", selfOf(fakeLib(92)));
        FakelibSource more;
        QVERIFY(providerProblems(e.prov, e.game, &more).isEmpty());
        ProviderPinCheck c = checkProviderPins(more, e.pins);
        QVERIFY(!c.ok());
        QCOMPARE(c.added, QStringList({"libScePlayGo.sprx"}));
        QVERIFY(!planBackport(e.provOptions(), e.ctx.token)->canRun());

        QVERIFY(pinProviders(more, e.pins).ok());
        QFile::remove(e.prov + "/libSceAmpr.sprx");
        FakelibSource fewer;
        QVERIFY(providerProblems(e.prov, e.game, &fewer).isEmpty());
        c = checkProviderPins(fewer, e.pins);
        QCOMPARE(c.missing, QStringList({"libSceAmpr.sprx"}));
        QVERIFY(c.describe().contains("pinned but missing"));
        QVERIFY(!planBackport(e.provOptions(), e.ctx.token)->canRun());
        // Pins of other folders are kept when one folder is pinned.
        FakelibSource other = fewer;
        other.dir = e.tmp.filePath("elsewhere");
        QVERIFY(pinProviders(other, e.pins).ok());
        QVERIFY(loadProviderPins(e.pins, e.prov));
        QVERIFY(loadProviderPins(e.pins, other.dir));
    }

    void providersReplaceTheDumpsOwnCopy()
    {
        ProvEnv e(OtherFakelib); // the dump has fakelib/libSceAgc.sprx and libSceAmpr.sprx from another tool
        const QByteArray dumpAmpr = readBytes(e.game + "/fakelib/libSceAmpr.sprx");
        const QByteArray dumpAgc = readBytes(e.game + "/fakelib/libSceAgc.sprx");
        auto r = runBackport(e.provOptions(false), e.ctx); // no fakelib set: providers only
        QVERIFY2(r.ok(), qPrintable(r.ok() ? QString() : r.error().toLogString()));
        QCOMPARE(readBytes(e.game + "/fakelib/libSceAmpr.sprx"), readBytes(e.prov + "/libSceAmpr.sprx"));
        QCOMPARE(readBytes(e.game + "/fakelib/libSceAgc.sprx"), dumpAgc); // the dump's other library: unchanged
        // The dump's own copy is kept in the originals ZIP (S14).
        zip::Reader z;
        QVERIFY(z.open(r->originalsZip).ok());
        QVERIFY(z.find("fakelib/libSceAmpr.sprx"));
        QCOMPARE(z.readAll(*z.find("fakelib/libSceAmpr.sprx")).value(), dumpAmpr);
        // Restore puts the dump's copy back and removes what was added.
        RestoreOptions ro;
        ro.game = e.game;
        ro.originalsDir = e.orig;
        auto rep = restoreOriginals(ro, e.ctx.token);
        QVERIFY(rep.ok() && rep->ok());
        QCOMPARE(readBytes(e.game + "/fakelib/libSceAmpr.sprx"), dumpAmpr);
        QVERIFY(!QFileInfo::exists(e.game + "/fakelib/libScePlayGo.sprx"));
    }

    void mergeKeepsTheSetsFileName()
    {
        FakelibSource base;
        base.dir = "/set";
        base.libs.push_back({"libSceAgc.sprx", "/set/libSceAgc.sprx", 2000, "aa"});
        base.libs.push_back({"libSceAmpr.sprx", "/set/libSceAmpr.sprx", 2000, "bb"});
        FakelibSource prov;
        prov.dir = "/prov";
        prov.libs.push_back({"libsceampr.sprx", "/prov/libsceampr.sprx", 3000, "cc"});
        prov.libs.push_back({"libScePlayGo.sprx", "/prov/libScePlayGo.sprx", 3000, "dd"});
        const FakelibSource m = withProviders(base, prov);
        QCOMPARE(m.libs.size(), size_t(3));
        const FakelibLib* a = m.find("libSceAmpr.sprx");
        QVERIFY(a);
        QCOMPARE(a->name, QStringLiteral("libSceAmpr.sprx"));
        QCOMPARE(a->sha256, QStringLiteral("cc"));
        QCOMPARE(a->path, QStringLiteral("/prov/libsceampr.sprx"));
        QCOMPARE(withProviders(std::nullopt, prov).dir, QStringLiteral("/prov"));
    }

    void pinJobRecordsAndReportsChanges()
    {
        ProvEnv e;
        auto schema = std::make_shared<SettingsSchema>();
        DependencyRegistry deps;
        JobFactory jobs;
        ModuleContext mc{*schema, deps, jobs};
        registerModule(mc);
        const SettingsValues none(schema, {}, {});
        QVERIFY(!jobs.create(kJobPinProviders, QJsonObject{}).value()->validate(none).ok());
        const QVariantMap values{{keys::kAmprPlaygoFolder, e.prov}};
        const SettingsValues settings(schema, values, {});
        auto job = jobs.create(kJobPinProviders, QJsonObject{});
        QVERIFY(job.ok() && job.value()->validate(settings).ok());
        JobContext ctx = e.ctx;
        ctx.settings = settings;
        auto o = job.value()->run(ctx);
        QVERIFY2(o.ok(), qPrintable(o.ok() ? QString() : o.error().toLogString()));
        QVERIFY(o->summary.startsWith("Pinned libSceAmpr.sprx ("));
        QCOMPARE(o->warnings.size(), 1); // libSceOther.sprx ignored
        QVERIFY(loadProviderPins(defaultProviderPinsPath(), e.prov));

        writeBytes(e.prov + "/libScePlayGo.sprx", selfOf(fakeLib(77)));
        auto again = jobs.create(kJobPinProviders, QJsonObject{});
        auto o2 = again.value()->run(ctx);
        QVERIFY(o2.ok());
        QVERIFY(o2->summary.contains("Earlier pins replaced (changed: libScePlayGo.sprx"));

        // optionsFromSettings carries the folder to the run, and a run with the
        // default pins path accepts the re-pinned folder.
        BackportOptions bo = optionsFromSettings(settings, e.game);
        QCOMPARE(bo.providerDir, e.prov);
        bo.originalsDir = e.orig;
        bo.backportDir = e.bp;
        bo.journalDir = e.journals;
        bo.freeSpaceMarginBytes = 0;
        auto plan = planBackport(bo, e.ctx.token);
        QVERIFY(plan.ok() && plan->canRun());
        QVERIFY(!plan->pinProvidersOnRun);
    }
};

QTEST_GUILESS_MAIN(TstBackportProviders)
#include "tst_backport_providers.moc"
