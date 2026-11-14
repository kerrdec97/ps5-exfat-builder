// zstd packs through the real image engines: a packed build into every image
// format (read back through the packs), an image wrapped in a PFS container
// read straight through, FPKG staging of packed and wrapped sources (only the
// files the package keeps, packs unpacked), and game.extract.
// FW_BENCH_MODE, FW_BENCH_THREADS and FW_BENCH_BUFFER_MIB run every job with
// those Performance settings (engines/BenchSettings.h), so the packing of
// realGameEndToEnd, which logs each format's time, can be timed per mode.

#include "connect/ps5/Module.h"
#include "core/Settings.h"
#include "engines/BenchSettings.h"
#include "engines/ampr/GameFiles.h"
#include "engines/ampr/Module.h"
#include "engines/ampr/Runtime.h"
#include "engines/exfat/Module.h"
#include "engines/fpkg/FpkgSettings.h"
#include "engines/fpkg/Jobs.h"
#include "engines/fpkg/Module.h"
#include "engines/fpkg/PackageInspect.h"
#include "engines/fpkg/SourceStager.h"
#include "engines/gameinfo/ParamJson.h"
#include "engines/general/Module.h"
#include "engines/integration/GameSources.h"
#include "engines/integration/ImageFiles.h"
#include "engines/integration/Module.h"
#include "engines/pfs/Module.h"
#include "engines/ufs2/Module.h"
#include "jobs/JobQueue.h"
#include "support/TestSupport.h"

#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QTest>

using namespace fw;

namespace {

struct Env {
    std::shared_ptr<SettingsSchema> schema = std::make_shared<SettingsSchema>();
    DependencyRegistry deps;
    JobFactory jobs;
    std::unique_ptr<SettingsStore> store;
    std::unique_ptr<JobQueue> queue;
    explicit Env(const QString& dir)
    {
        ModuleContext ctx{*schema, deps, jobs};
        general::registerModule(ctx);
        exfat::registerModule(ctx);
        ufs2::registerModule(ctx);
        pfs::registerModule(ctx);
        fpkg::registerModule(ctx);
        ampr::registerModule(ctx);
        ps5::registerModule(ctx);
        integration::registerModule(ctx);
        store = std::make_unique<SettingsStore>(schema, dir + QStringLiteral("/settings.json"));
        (void)store->load();
        (void)store->setValue(general::keys::kFreeSpaceMarginMiB, 0);
        // The benchmark's Performance settings, when one is given (checked in initTestCase).
        const benchsettings::Performance perf = benchsettings::fromEnvironment();
        if (perf.given && perf.problem.isEmpty()) {
            (void)store->setValue(general::keys::kPerformanceMode, perf.mode);
            if (qEnvironmentVariableIsSet("FW_BENCH_THREADS"))
                (void)store->setValue(general::keys::kWorkerThreads, qEnvironmentVariableIntValue("FW_BENCH_THREADS"));
            if (qEnvironmentVariableIsSet("FW_BENCH_BUFFER_MIB"))
                (void)store->setValue(general::keys::kIoBufferMiB, qEnvironmentVariableIntValue("FW_BENCH_BUFFER_MIB"));
        }
        queue = std::make_unique<JobQueue>(*store, jobs);
    }
    std::optional<JobRecord> run(const QString& kind, const QJsonObject& params, const QVariantMap& overrides = {})
    {
        auto id = queue->enqueue(kind, params, overrides);
        if (!id.ok()) {
            qWarning() << "enqueue failed:" << id.error().toLogString();
            return std::nullopt;
        }
        (void)QTest::qWaitFor([&] { auto r = queue->record(id.value()); return r && isTerminal(r->state); }, 180000);
        return queue->record(id.value());
    }
};

// False also when the job was refused before it could be queued.
bool succeeded(const std::optional<JobRecord>& r)
{
    return r && r->state == JobState::Succeeded;
}

QString describe(const std::optional<JobRecord>& r)
{
    if (!r)
        return QStringLiteral("no record");
    return jobStateName(r->state) + QStringLiteral(": ") + (r->error ? r->error->toLogString() : (r->outcome ? r->outcome->summary : QString()));
}

QByteArray fileBytes(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

QMap<QString, QByteArray> snapshot(const QString& root, const QStringList& skip = {})
{
    QMap<QString, QByteArray> out;
    QDirIterator it(root, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    const QDir r(root);
    while (it.hasNext()) {
        it.next();
        const QString rel = r.relativeFilePath(it.filePath());
        if (!skip.contains(rel))
            out.insert(rel, fileBytes(it.filePath()));
    }
    return out;
}

// Every file of `files` -> bytes.
QMap<QString, QByteArray> readAll(ampr::GameFiles& files, const QStringList& skip = {})
{
    QMap<QString, QByteArray> out;
    for (int i = 0; i < int(files.items().size()); ++i) {
        const auto& it = files.items()[size_t(i)];
        if (it.isDir || skip.contains(it.path))
            continue;
        QByteArray b(qsizetype(it.size), '\0');
        qint64 done = 0;
        while (done < it.size) {
            auto n = files.read(i, done, b.data() + done, it.size - done);
            if (!n.ok() || *n <= 0)
                break;
            done += *n;
        }
        out.insert(it.path, b);
    }
    return out;
}

} // namespace

class TestZstdGames : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QVERIFY(m_root.isValid());
        fwtest::isolateAppData(m_root);
        const benchsettings::Performance perf = benchsettings::fromEnvironment();
        QVERIFY2(perf.problem.isEmpty(), qPrintable(perf.problem));
        qInfo("zstd games settings: %s", qPrintable(perf.describe()));
        m_game = m_root.filePath(QStringLiteral("dumps/PPSA01234"));
        QVERIFY(fwtest::writeTextFile(m_game + QStringLiteral("/eboot.bin"), QByteArray("\x54\x14\xF5\xEE", 4) + QByteArray(70000, 'E')));
        QVERIFY(fwtest::writeTextFile(m_game + QStringLiteral("/sce_sys/param.json"),
                                      R"({"titleId":"PPSA01234","contentId":"UP0000-PPSA01234_00-0000000000000000","contentVersion":"01.000.000"})"));
        QVERIFY(fwtest::writeRandomFile(m_game + QStringLiteral("/data/level1.bin"), 3 * 1024 * 1024 + 7, 42));
        QByteArray text;
        for (int i = 0; i < 20000; ++i)
            text += "line " + QByteArray::number(i % 50) + "\n";
        QVERIFY(fwtest::writeTextFile(m_game + QStringLiteral("/data/strings/ja.txt"), text));
        QVERIFY(fwtest::writeTextFile(m_game + QStringLiteral("/fakelib/libSceNpAuth.sprx"), QByteArray("backport")));
        QVERIFY(fwtest::writeTextFile(m_game + QStringLiteral("/empty.bin"), QByteArray()));
        m_original = snapshot(m_game);
    }

    void packedBuildsIntoEveryImageFormat()
    {
        Env env(m_root.filePath(QStringLiteral("e1")));
        const QString out = m_root.filePath(QStringLiteral("zstd"));
        QDir().mkpath(out);
        const QStringList ignore = {QStringLiteral("fakelib/libSceAmpr.sprx"), QStringLiteral("ampr_emu.index")};
        for (const QString& fmt : {QStringLiteral("exfat"), QStringLiteral("ffpkg"), QStringLiteral("ffpfs"), QStringLiteral("ffpfsc")}) {
            const QString image = out + QStringLiteral("/game.") + fmt;
            auto r = env.run(integration::kZstdBuildJob, {{QStringLiteral("format"), fmt}, {QStringLiteral("source"), m_game},
                                                          {QStringLiteral("output"), image}});
            QVERIFY2(r && r->state == JobState::Succeeded, qPrintable(fmt + u' ' + describe(r)));
            QVERIFY(r->outcome->summary.startsWith(QLatin1String("zstd:")));
            QVERIFY(QFile::exists(image));
            // No work folder is left behind.
            QCOMPARE(QDir(out).entryList(QStringList{QStringLiteral(".fw-zstd-*")}, QDir::AllEntries | QDir::Hidden).size(), 0);

            // The image holds the packs; read through them it is the original game.
            auto files = integration::openGameFiles(image, {});
            QVERIFY2(files.ok(), files.ok() ? "" : qPrintable(files.error().toLogString()));
            QCOMPARE(ampr::detectPackKind(**files), ampr::PackKind::Zstd);
            QVERIFY((*files)->find(QStringLiteral("data/level1.bin")) < 0); // packed, not loose
            auto logical = ampr::logicalGame(*files, {});
            QVERIFY(logical.ok());
            QMap<QString, QByteArray> got = readAll(**logical, ignore);
            got.remove(QStringLiteral(".pfskeep")); // PFS empty-folder markers, if any
            QCOMPARE(got.keys(), m_original.keys());
            QVERIFY2(got == m_original, qPrintable(fmt));
            const int rt = (*logical)->find(QStringLiteral("fakelib/libSceAmpr.sprx"));
            QVERIFY(rt >= 0);
        }
    }

    // The packer writes ampr_emu.index for the game as the game sees it (the
    // files inside the packs included). The image build that follows must not
    // replace it, whatever the user's "AMPR file index" settings say.
    void packedBuildKeepsThePackersIndex()
    {
        Env env(m_root.filePath(QStringLiteral("e-index")));
        QVERIFY(env.store->setValue(pfs::keys::kAmprIndex, QStringLiteral("refresh")).ok());
        QVERIFY(env.store->setValue(exfat::keys::kAmprIndex, QStringLiteral("refresh")).ok());
        QVERIFY(env.store->setValue(ufs2::keys::kAmprIndex, QStringLiteral("refresh")).ok());
        const QString out = m_root.filePath(QStringLiteral("zstd-index"));
        QDir().mkpath(out);
        for (const QString& fmt : {QStringLiteral("exfat"), QStringLiteral("ffpkg"), QStringLiteral("ffpfs")}) {
            const QString image = out + QStringLiteral("/game.") + fmt;
            auto r = env.run(integration::kZstdBuildJob, {{QStringLiteral("format"), fmt}, {QStringLiteral("source"), m_game},
                                                          {QStringLiteral("output"), image}});
            QVERIFY2(r && r->state == JobState::Succeeded, qPrintable(fmt + u' ' + describe(r)));
            auto files = integration::openGameFiles(image, {});
            QVERIFY2(files.ok(), files.ok() ? "" : qPrintable(files.error().toLogString()));
            const QByteArray index = readAll(**files).value(QStringLiteral("ampr_emu.index"));
            QVERIFY2(index.startsWith("AMPRIDX3"), qPrintable(fmt));
            QVERIFY2(index.contains("/app0/data/level1.bin"), qPrintable(fmt)); // a packed file
            QVERIFY2(!index.contains("ampr_zstd"), qPrintable(fmt));             // not the folder's pack files
        }
    }

    void zstdBuildSafety()
    {
        Env env(m_root.filePath(QStringLiteral("e4")));
        // The user's exclusions apply before packing; a pattern that matches the
        // pack files must not drop them from the image.
        const QString image = m_root.filePath(QStringLiteral("safety/game.exfat"));
        QDir().mkpath(QFileInfo(image).absolutePath());
        auto r = env.run(integration::kZstdBuildJob,
                         {{QStringLiteral("format"), QStringLiteral("exfat")}, {QStringLiteral("source"), m_game},
                          {QStringLiteral("output"), image},
                          {QStringLiteral("excludes"), QJsonArray{QStringLiteral("*.index"), QStringLiteral("*.pak"), QStringLiteral("ja.txt")}}});
        QVERIFY2(r && r->state == JobState::Succeeded, qPrintable(describe(r)));
        auto files = integration::openGameFiles(image, {});
        QVERIFY(files.ok());
        QVERIFY((*files)->find(QStringLiteral("ampr_zstd.index")) >= 0);
        QVERIFY((*files)->find(QStringLiteral("ampr_zstd-000.pak")) >= 0);
        auto logical = ampr::logicalGame(*files, {});
        QVERIFY(logical.ok());
        QCOMPARE((*logical)->find(QStringLiteral("data/strings/ja.txt")), -1); // excluded before packing
        QVERIFY((*logical)->find(QStringLiteral("data/level1.bin")) >= 0);

        // An image inside the game folder is refused before anything is packed.
        auto inside = env.queue->enqueue(integration::kZstdBuildJob, {{QStringLiteral("format"), QStringLiteral("exfat")},
                                                                     {QStringLiteral("source"), m_game},
                                                                     {QStringLiteral("output"), m_game + QStringLiteral("/x.exfat")}});
        QVERIFY(!inside.ok());
    }

    void wrappedImagesOpenStraightThrough()
    {
        Env env(m_root.filePath(QStringLiteral("e2")));
        const QString out = m_root.filePath(QStringLiteral("wrapped"));
        QDir().mkpath(out);
        QVERIFY(succeeded(env.run(QStringLiteral("image.exfat.build"), {{QStringLiteral("source"), m_game}, {QStringLiteral("output"), out + QStringLiteral("/g.exfat")}})));
        QVERIFY(succeeded(env.run(QStringLiteral("image.ffpkg.build"), {{QStringLiteral("source"), m_game}, {QStringLiteral("output"), out + QStringLiteral("/g.ffpkg")}})));
        // The exFAT and UFS2 images packed as they are into .ffpfsc containers.
        for (const QString& inner : {QStringLiteral("g.exfat"), QStringLiteral("g.ffpkg")}) {
            const QString box = out + u'/' + QFileInfo(inner).suffix() + QStringLiteral("-box.ffpfsc");
            auto r = env.run(integration::kConvertJob, {{QStringLiteral("image"), out + u'/' + inner},
                                                        {QStringLiteral("format"), QStringLiteral("ffpfsc")},
                                                        {QStringLiteral("output"), box}});
            QVERIFY2(r && r->state == JobState::Succeeded, qPrintable(describe(r)));
            auto tree = integration::openFileTree(box, {});
            QVERIFY2(tree.ok(), tree.ok() ? "" : qPrintable(tree.error().toLogString()));
            QVERIFY2((*tree)->describe().contains(QLatin1String("inside")), qPrintable((*tree)->describe()));
            auto files = integration::openGameFiles(box, {});
            QVERIFY(files.ok());
            QCOMPARE(readAll(**files), m_original);

            // FPKG staging reads it straight through as well.
            auto stager = fpkg::sourceStagerFor(box);
            QVERIFY(stager);
            QVERIFY(stager->estimateStagedBytes(box) > 3 * 1024 * 1024);
            const QString dest = m_root.filePath(QStringLiteral("stage-") + QFileInfo(box).baseName());
            QDir().mkpath(dest);
            ProgressReporter pr;
            pr.begin(QStringLiteral("Prepare"), 0);
            QVERIFY(stager->stage(box, dest, {}, &pr).ok());
            QCOMPARE(snapshot(dest + QStringLiteral("/app")), m_original);

            // And game.extract.
            const QString ex = m_root.filePath(QStringLiteral("extract-") + QFileInfo(box).baseName());
            auto e = env.run(integration::kGameExtractJob, {{QStringLiteral("source"), box}, {QStringLiteral("output"), ex}});
            QVERIFY2(e && e->state == JobState::Succeeded, qPrintable(describe(e)));
            QCOMPARE(snapshot(ex), m_original);
        }
    }

    void fpkgStagingUnpacksPackedGames()
    {
        Env env(m_root.filePath(QStringLiteral("e3")));
        // A packed folder (with an AMPR journal and an SDK file) and a packed image.
        const QString packed = m_root.filePath(QStringLiteral("packedfolder/PPSA01234"));
        auto p = env.run(ampr::kPackJob, {{QStringLiteral("source"), m_game}, {QStringLiteral("output"), packed}});
        QVERIFY2(p && p->state == JobState::Succeeded, qPrintable(describe(p)));
        QVERIFY(fwtest::writeTextFile(packed + QStringLiteral("/ampr_commands.bin"), "journal"));
        QVERIFY(fwtest::writeTextFile(packed + QStringLiteral("/sce_sys/playgo-chunk.dat"), "sdk"));
        auto stager = fpkg::folderStagerFor(packed);
        QVERIFY2(stager, "a packed folder must be staged (unpacked) for a package");
        QVERIFY(!fpkg::folderStagerFor(m_game));
        const QString dest = m_root.filePath(QStringLiteral("stage-packed"));
        QDir().mkpath(dest);
        ProgressReporter pr;
        pr.begin(QStringLiteral("Prepare"), 0);
        QVERIFY(stager->stage(packed, dest, {}, &pr).ok());
        const QString app = dest + QStringLiteral("/app");
        // The original files: packs unpacked; pack files, journal, SDK file, the
        // AMPR emulator and its index left out.
        const auto staged = snapshot(app);
        QCOMPARE(staged.keys(), m_original.keys());
        QVERIFY(staged == m_original);

        // game.extract of the packed folder, with and without unpacking.
        const QString ex = m_root.filePath(QStringLiteral("extract-packed"));
        auto e = env.run(integration::kGameExtractJob, {{QStringLiteral("source"), packed}, {QStringLiteral("output"), ex}});
        QVERIFY2(e && e->state == JobState::Succeeded, qPrintable(describe(e)));
        QVERIFY(!QFile::exists(ex + QStringLiteral("/ampr_zstd.index")));
        QCOMPARE(fileBytes(ex + QStringLiteral("/data/level1.bin")), m_original.value(QStringLiteral("data/level1.bin")));
        const QString keep = m_root.filePath(QStringLiteral("extract-kept"));
        auto k = env.run(integration::kGameExtractJob, {{QStringLiteral("source"), packed}, {QStringLiteral("output"), keep},
                                                        {QStringLiteral("unpackPacks"), false}});
        QVERIFY2(k && k->state == JobState::Succeeded, qPrintable(describe(k)));
        QVERIFY(QFile::exists(keep + QStringLiteral("/ampr_zstd.index")));
        QVERIFY(!QFile::exists(keep + QStringLiteral("/data/level1.bin")));

        // ampr.unpack (folder to folder).
        const QString un = m_root.filePath(QStringLiteral("unpack-job/PPSA01234"));
        auto u = env.run(ampr::kUnpackJob, {{QStringLiteral("source"), packed}, {QStringLiteral("output"), un}});
        QVERIFY2(u && u->state == JobState::Succeeded, qPrintable(describe(u)));
        QCOMPARE(fileBytes(un + QStringLiteral("/data/strings/ja.txt")), m_original.value(QStringLiteral("data/strings/ja.txt")));
    }

    // ---- a real dump, and an FPKG with the owner's Sony toolkit (skipped without them) ----
    void realGameEndToEnd()
    {
        const QString gameSrc = qEnvironmentVariable("FW_ZSTD_E2E_GAME");
        if (gameSrc.isEmpty())
            QSKIP("Set FW_ZSTD_E2E_GAME (a game folder) for the real-dump run; FW_BACKPORT_E2E_TOOLKIT adds an FPKG build.");
        // A copy of the dump, plus asset files made from its real binaries (a
        // test dump often holds only the executables, which stay loose).
        const QString game = m_root.filePath(QStringLiteral("realgame/") + QFileInfo(gameSrc).fileName());
        QByteArray blob;
        QDirIterator it(gameSrc, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString f = it.next();
            const QString rel = QDir(gameSrc).relativeFilePath(f);
            QDir().mkpath(QFileInfo(QDir(game).filePath(rel)).absolutePath());
            QVERIFY(QFile::copy(f, QDir(game).filePath(rel)));
            if (f.endsWith(QLatin1String(".prx")))
                blob += fileBytes(f);
        }
        QVERIFY(fwtest::writeTextFile(game + QStringLiteral("/Media/Data/data.unity3d"), blob));
        QVERIFY(fwtest::writeRandomFile(game + QStringLiteral("/Media/Data/level0"), 5 * 1024 * 1024 + 11, 7));
        QVERIFY(fwtest::writeTextFile(game + QStringLiteral("/Media/StreamingAssets/strings.json"), QByteArray(200000, '{')));
        Env env(m_root.filePath(QStringLiteral("e9")));
        const QString out = m_root.filePath(QStringLiteral("real"));
        QDir().mkpath(out);
        const QStringList ignore = {QStringLiteral("fakelib/libSceAmpr.sprx"), QStringLiteral("ampr_emu.index")};
        const auto original = snapshot(game, ignore);
        qint64 originalBytes = 0;
        for (const QByteArray& b : original)
            originalBytes += b.size();
        for (const QString& fmt : {QStringLiteral("exfat"), QStringLiteral("ffpkg"), QStringLiteral("ffpfs")}) {
            const QString image = out + QStringLiteral("/real.") + fmt;
            QElapsedTimer timer;
            timer.start();
            auto r = env.run(integration::kZstdBuildJob, {{QStringLiteral("format"), fmt}, {QStringLiteral("source"), game},
                                                          {QStringLiteral("output"), image}});
            const qint64 elapsedMs = timer.elapsed();
            QVERIFY2(r && r->state == JobState::Succeeded, qPrintable(fmt + u' ' + describe(r)));
            qInfo("%s: %s (%lld bytes, dump %lld) in %lld ms", qPrintable(fmt), qPrintable(r->outcome->summary), QFileInfo(image).size(),
                  originalBytes, elapsedMs);
            auto files = integration::openGameFiles(image, {});
            QVERIFY(files.ok());
            auto logical = ampr::logicalGame(*files, {});
            QVERIFY(logical.ok());
            QMap<QString, QByteArray> got = readAll(**logical, ignore);
            got.remove(QStringLiteral(".pfskeep"));
            QCOMPARE(got.keys(), original.keys());
            QVERIFY2(got == original, qPrintable(fmt));
        }

        const QString toolkit = qEnvironmentVariable("FW_BACKPORT_E2E_TOOLKIT");
        if (toolkit.isEmpty())
            return;
        // An installable package straight from the zstd-packed exFAT image: the
        // packs are unpacked into a staging folder next to the output.
        QVERIFY(env.store->setValue(fpkg::keys::kPubToolsPath, toolkit).ok());
        const QString pkgOut = m_root.filePath(QStringLiteral("real-pkg"));
        QDir().mkpath(pkgOut);
        auto b = env.run(fpkg::kJobBuild, {{QStringLiteral("source"), out + QStringLiteral("/real.exfat")}, {QStringLiteral("output"), pkgOut}});
        QVERIFY2(b && b->state == JobState::Succeeded, qPrintable(describe(b)));
        qInfo() << "fpkg" << b->outcome->outputs << b->outcome->summary;
        QVERIFY(!b->outcome->outputs.isEmpty());
        QCOMPARE(QDir(pkgOut).entryList(QStringList{QStringLiteral(".fw-stage-*")}, QDir::AllEntries | QDir::Hidden).size(), 0);
        CancellationSource cs;
        const QString pkg = b->outcome->outputs.first();
        auto id = fpkg::inspectPackage(pkg, cs.token());
        QVERIFY(id.ok());
        QCOMPARE(id->kind, fpkg::PackageKind::Ps5);
        const auto param = gameinfo::parseParamJson(fileBytes(game + QStringLiteral("/sce_sys/param.json")));
        QVERIFY(param.ok());
        QCOMPARE(id->titleId, param->titleId);
    }

private:
    QTemporaryDir m_root;
    QString m_game;
    QMap<QString, QByteArray> m_original;
};

QTEST_GUILESS_MAIN(TestZstdGames)
#include "tst_zstd_games.moc"
