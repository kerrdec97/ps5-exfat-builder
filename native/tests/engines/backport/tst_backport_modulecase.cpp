// Unity module-name case fix (docs/BACKPORT_PARITY.md matrix #41,
// ModuleCaseFix.h), ported from the owner's Backport engine and its tests
// (test_dump_preflight.py module-case cases, test_exporter.py 155-187): the
// spelling the eboot loads ("%s/Modules/<Name>.prx") is used in the backport
// ZIP and by Apply; the originals ZIP, rollback and Restore keep the dump's.

#include "BackportTestEnv.h"

#include "core/Zip.h"
#include "engines/backport/ApplyJournal.h"
#include "engines/backport/BackportPlanner.h"
#include "engines/backport/ModuleCaseFix.h"
#include "engines/backport/RestoreService.h"
#include "support/TestSupport.h"

#include <QTest>

using namespace fw;
using namespace fw::backport;
using namespace fwtest::bp;

namespace {

const QString kDumpRel = QStringLiteral("Media/Modules/Il2cppUserAssemblies.prx");
const QString kGameRel = QStringLiteral("Media/Modules/Il2CppUserAssemblies.prx");

// F1 whose code segment holds the Unity loader strings.
QByteArray unityEboot()
{
    QByteArray b = f1();
    const QByteArray text("\0%s/Modules/Il2CppUserAssemblies.prx\0junk\0%s/Modules/PS5Util.prx\0", 64);
    std::memcpy(b.data() + 0x2000, text.constData(), size_t(text.size()));
    return b;
}

void makeUnityGame(Env& e, const QString& moduleRel = kDumpRel)
{
    QVERIFY(writeBytes(e.game + "/eboot.bin", selfOf(unityEboot())));
    QVERIFY(writeBytes(e.game + "/" + moduleRel, selfOf(f2(7))));
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

QStringList modulesListing(const Env& e)
{
    return QDir(e.game + "/Media/Modules").entryList(QDir::Files, QDir::Name);
}

} // namespace

class TstBackportModuleCase : public QObject {
    Q_OBJECT
    QTemporaryDir m_root;

private slots:
    void initTestCase()
    {
        QVERIFY(m_root.isValid());
        fwtest::isolateAppData(m_root);
    }

    void requestedPathsComeFromTheLoaderStrings()
    {
        const QByteArray raw("\0%s/Modules/Il2CppUserAssemblies.prx\0junk\0%s/Modules/PS5Util.prx\0", 64);
        QCOMPARE(requestedModulePaths(raw),
                 QStringList({QStringLiteral("Media/Modules/Il2CppUserAssemblies.prx"),
                              QStringLiteral("Media/Modules/PS5Util.prx")}));
        QCOMPARE(requestedModulePaths(QByteArray("%s/Modules/Foo.prx.bak\0", 22)),
                 QStringList{QStringLiteral("Media/Modules/Foo.prx")}); // the longest run ending in .prx
        QVERIFY(requestedModulePaths(QByteArray("%s/Modules/.prx")).isEmpty());
        QVERIFY(requestedModulePaths(QByteArray("%s/Modules/x.sprx")).isEmpty());
        QVERIFY(requestedModulePaths(QByteArray("%S/modules/X.prx")).isEmpty()); // case-sensitive lead
        QCOMPARE(requestedModulePaths(QByteArray("%s/Modules/A.prx%s/Modules/A.prx")).size(), 1);
    }

    void planRenamesOnlyAMismatchedSpelling()
    {
        Env e;
        makeUnityGame(e);
        auto p = planBackport(e.options(), CancellationToken());
        QVERIFY(p.ok());
        QCOMPARE(int(p->moduleCase.renames.size()), 1);
        QCOMPARE(p->moduleCase.renames.front().from, kDumpRel);
        QCOMPARE(p->moduleCase.renames.front().to, kGameRel);
        QVERIFY(!p->moduleCase.renames.front().folderCaseDiffers);
        bool warned = false;
        for (const Issue& i : p->issues)
            warned |= i.code == Code::ModuleNameCase && i.severity == Severity::Warn && i.files.contains(kDumpRel);
        QVERIFY(warned);
        QVERIFY(p->canRun());

        Env ok; // already spelled as the game loads it
        makeUnityGame(ok, kGameRel);
        auto q = planBackport(ok.options(), CancellationToken());
        QVERIFY(q.ok());
        QVERIFY(q->moduleCase.renames.empty());
    }

    void zipAndApplyUseTheGamesSpellingRestoreTheDumps()
    {
        Env e;
        makeUnityGame(e);
        const auto before = snapshotTree(e.game);
        auto r = runBackport(e.options(), e.ctx);
        QVERIFY2(r.ok(), qPrintable(r.ok() ? QString() : r.error().toLogString()));
        QVERIFY(zipNames(r->backportZip).contains(kGameRel));
        QVERIFY(!zipNames(r->backportZip).contains(kDumpRel));
        QVERIFY(zipNames(r->originalsZip).contains(kDumpRel));
        QCOMPARE(modulesListing(e), QStringList{QStringLiteral("Il2CppUserAssemblies.prx")});
        // The journal and the record say what was renamed.
        auto j = ApplyJournal::load(r->journalPath);
        QVERIFY(j.ok());
        bool journaled = false;
        for (const JournalEntry& x : j->entries())
            journaled |= x.rel == kDumpRel && x.renameTo == kGameRel && x.renamed;
        QVERIFY(journaled);
        auto m = readManifest(r->manifestPath);
        QVERIFY(m.ok() && m.value());
        QCOMPARE(m.value()->renamedFiles().value(kGameRel), kDumpRel);

        auto rep = restoreOriginals({e.game, r->originalsZip, e.orig}, CancellationToken());
        QVERIFY2(rep.ok() && rep->ok(), qPrintable(rep.ok() ? rep->errors.join(u'|') : rep.error().toLogString()));
        QCOMPARE(modulesListing(e), QStringList{QStringLiteral("Il2cppUserAssemblies.prx")});
        QCOMPARE(snapshotTree(e.game), before);
    }

    void rollbackPutsTheDumpsSpellingBack()
    {
        Env e;
        makeUnityGame(e);
        const auto before = snapshotTree(e.game);
        auto o = e.options();
        o.testHook = [](const QString& p, const QString& i) -> Status {
            if (p == u"apply.file" && i.endsWith(QStringLiteral("libSceNpAuth.sprx"))) // after the module
                return makeError(Code::ApplyFailed, QStringLiteral("injected"));
            return success();
        };
        auto r = runBackport(o, e.ctx);
        QVERIFY(!r.ok());
        QVERIFY(r.error().message.contains(QStringLiteral("rolled back")));
        QCOMPARE(modulesListing(e), QStringList{QStringLiteral("Il2cppUserAssemblies.prx")});
        QCOMPARE(snapshotTree(e.game), before);
    }
};

QTEST_GUILESS_MAIN(TstBackportModuleCase)
#include "tst_backport_modulecase.moc"
