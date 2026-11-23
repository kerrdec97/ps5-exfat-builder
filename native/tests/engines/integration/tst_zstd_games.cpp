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
#include "engines/integration/ImageCompare.h"
#include "engines/integration/Module.h"
#include "engines/integration/ZstdImage.h"
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

#include <zstd.h>

#include <algorithm>

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

    // GitHub #98, report 03: the ".zstd" file Kraken Mount MOD mounts is a bare
    // exFAT volume (boot sector at byte 0, "EXFAT   " at 3..10) holding the
    // game with its files in ampr-emu-ng zstd packs. image.build.zstd with
    // format exfat writes exactly that; named .zstd it is the same image.
    // (Only the reader's contract is checked here; MOCK, no console.)
    void zstdSingleFileImage()
    {
        Env env(m_root.filePath(QStringLiteral("e-zstdfile")));
        const QString out = m_root.filePath(QStringLiteral("zstdfile"));
        QDir().mkpath(out);
        const QString exfatImage = out + QStringLiteral("/game.exfat");
        const QString zstdImage = out + QStringLiteral("/game.zstd");
        for (const QString& image : {exfatImage, zstdImage}) {
            auto r = env.run(integration::kZstdBuildJob, {{QStringLiteral("format"), QStringLiteral("exfat")}, {QStringLiteral("source"), m_game},
                                                          {QStringLiteral("output"), image}});
            QVERIFY2(succeeded(r), qPrintable(image + u' ' + describe(r)));
            QVERIFY(r->subtitle.contains(image == zstdImage ? QLatin1String(".zstd") : QLatin1String(".exfat")));
            // The queue and the monitor name the format as the Build page's tile does.
            QCOMPARE(r->subtitle.startsWith(QLatin1String("ZSTD image (experimental)")), image == zstdImage);
            // ShadowMount+ is named for the .exfat only: it does not mount a .zstd (report 03).
            QVERIFY(r->outcome);
            const QStringList& w = r->outcome->warnings;
            const bool smp = std::any_of(w.begin(), w.end(), [](const QString& s) { return s.startsWith(QLatin1String("Packed games need ShadowMount+")); });
            QCOMPARE(smp, image == exfatImage);
            if (image == zstdImage)
                QVERIFY2(w.contains(QStringLiteral("A .zstd file is for Kraken Mount MOD; it is not yet confirmed on a console.")),
                         qPrintable(w.join(u'|')));
        }
        {
            // Kraken Mount MOD attaches a .zstd with 512-byte sectors only: Settings › exFAT ›
            // Sector size 4096 (for ShadowMount+) does not reach it.
            (void)env.store->setValue(exfat::keys::kSectorSize, QStringLiteral("4096"));
            const QString big = out + QStringLiteral("/sector4096.zstd");
            auto r = env.run(integration::kZstdBuildJob, {{QStringLiteral("format"), QStringLiteral("exfat")}, {QStringLiteral("source"), m_game},
                                                          {QStringLiteral("output"), big}});
            (void)env.store->setValue(exfat::keys::kSectorSize, QStringLiteral("512"));
            QVERIFY2(succeeded(r), qPrintable(describe(r)));
            const QByteArray b = fileBytes(big);
            QVERIFY(b.size() >= 512);
            QCOMPARE(quint8(b.at(108)), quint8(9));
            QVERIFY(QFile::remove(big));
        }
        // The reader's contract (kraken-mount-MOD src/image.rs, described in report 03).
        const QByteArray z = fileBytes(zstdImage), e = fileBytes(exfatImage);
        QVERIFY(z.size() >= 512);
        QCOMPARE(z.mid(3, 8), QByteArrayLiteral("EXFAT   "));
        QCOMPARE(quint8(z.at(108)), quint8(9)); // BytesPerSectorShift: 512-byte sectors
        QVERIFY(!z.startsWith(QByteArray("\x28\xB5\x2F\xFD", 4)));
        QCOMPARE(z.size() % 512, 0);
        // The .exfat build of the same job, but for what every build writes
        // afresh (two .exfat builds differ the same way): the volume serial
        // (boot bytes 100..103, and the boot checksums over it), the time
        // stamps of the new pack files and the packs' archive_id (below).
        QCOMPARE(z.size(), e.size());
        QByteArray zb = z.left(512), eb = e.left(512);
        zb.replace(100, 4, QByteArray(4, '\0'));
        eb.replace(100, 4, QByteArray(4, '\0'));
        QCOMPARE(zb, eb);
        auto zt = integration::openFileTree(zstdImage, {});
        QVERIFY2(zt.ok(), zt.ok() ? "" : qPrintable(zt.error().toLogString()));
        for (const char* top : {"eboot.bin", "sce_sys/param.json", "ampr_zstd.index", "fakelib/libSceAmpr.sprx", "ampr_emu.index"})
            QVERIFY2((*zt)->find(QLatin1String(top)) >= 0, top);
        auto et = integration::openFileTree(exfatImage, {});
        QVERIFY(et.ok());
        integration::CompareOptions co;
        co.depth = integration::CompareDepth::Full;
        auto cmp = integration::compareTrees(*zt.value(), *et.value(), co, {});
        QVERIFY2(cmp.ok(), cmp.ok() ? "" : qPrintable(cmp.error().message));
        // Every file the same, but for the pack set's archive_id (16 bytes at
        // 24 of the index and of each volume), which every packing draws anew,
        // and the header checksums over it (index 116..119, volume 60..63;
        // engines/ampr/ZstdPack.h).
        QCOMPARE(cmp->missingCount + cmp->extraCount + cmp->sizeCount + cmp->unreadableCount, qint64(0));
        for (const auto& d : cmp->contentDifferent) {
            QVERIFY2(d.path.startsWith(QLatin1String("ampr_zstd")) && d.offset >= 24 && d.offset < 40, qPrintable(d.path));
            auto a = integration::readSlice(**zt, d.path, 0, 1 << 26), b = integration::readSlice(**et, d.path, 0, 1 << 26);
            QVERIFY(a.ok() && b.ok());
            QCOMPARE(a->data.size(), b->data.size());
            QByteArray am = a->data, bm = b->data;
            const int crcAt = d.path.endsWith(QLatin1String(".index")) ? 116 : 60;
            for (QByteArray* x : {&am, &bm}) {
                x->replace(24, 16, QByteArray(16, '\0'));
                x->replace(crcAt, 4, QByteArray(4, '\0'));
            }
            qsizetype at = 0;
            while (at < am.size() && am.at(at) == bm.at(at))
                ++at;
            QVERIFY2(am == bm, qPrintable(QStringLiteral("%1 differs at %2").arg(d.path).arg(at)));
        }

        // Opened and extracted through the reader: the game as it was.
        auto files = integration::openGameFiles(zstdImage, {});
        QVERIFY2(files.ok(), files.ok() ? "" : qPrintable(files.error().toLogString()));
        QCOMPARE(ampr::detectPackKind(**files), ampr::PackKind::Zstd);
        const QString extracted = m_root.filePath(QStringLiteral("zstdfile-extracted"));
        auto x = env.run(integration::kGameExtractJob, {{QStringLiteral("source"), zstdImage}, {QStringLiteral("output"), extracted}});
        QVERIFY2(succeeded(x), qPrintable(describe(x)));
        QCOMPARE(snapshot(extracted, {QStringLiteral("fakelib/libSceAmpr.sprx"), QStringLiteral("ampr_emu.index")}), m_original);

        // Only an exFAT image can be named .zstd.
        auto wrong = env.queue->enqueue(integration::kZstdBuildJob, {{QStringLiteral("format"), QStringLiteral("ffpkg")},
                                                                    {QStringLiteral("source"), m_game},
                                                                    {QStringLiteral("output"), out + QStringLiteral("/other.zstd")}});
        QVERIFY(!wrong.ok());
        QCOMPARE(wrong.error().message, QStringLiteral("Only an exFAT image with zstd packs can be named .zstd."));
    }

    // A .zstd that is not an exFAT image is refused with what it is: a real zstd
    // stream (made with the bundled zstd, as PS5ZST's .exfat.zst is) and a PFSC
    // container renamed .zstd.
    void zstdFilesThatAreNotImagesAreRefused()
    {
        Env env(m_root.filePath(QStringLiteral("e-zstdbad")));
        const QString out = m_root.filePath(QStringLiteral("zstdbad"));
        QDir().mkpath(out);
        // A whole exFAT image compressed into one zstd frame.
        const QString exfatImage = out + QStringLiteral("/plain.exfat");
        auto built = env.run(QStringLiteral("image.exfat.build"), {{QStringLiteral("source"), m_game}, {QStringLiteral("output"), exfatImage}});
        QVERIFY2(succeeded(built), qPrintable(describe(built)));
        const QByteArray plain = fileBytes(exfatImage);
        QByteArray packed(qsizetype(ZSTD_compressBound(size_t(plain.size()))), '\0');
        const size_t n = ZSTD_compress(packed.data(), size_t(packed.size()), plain.constData(), size_t(plain.size()), 3);
        QVERIFY(!ZSTD_isError(n));
        packed.truncate(qsizetype(n));
        const QString stream = out + QStringLiteral("/stream.zstd");
        QVERIFY(fwtest::writeTextFile(stream, packed));
        QCOMPARE(integration::sniffZstdImage(packed), integration::ZstdImageKind::ZstdStream);
        // A PFSC container renamed .zstd.
        const QString pfsc = out + QStringLiteral("/game.ffpfsc");
        auto pb = env.run(QStringLiteral("image.pfs.build"), {{QStringLiteral("source"), m_game}, {QStringLiteral("output"), pfsc},
                                                            {QStringLiteral("format"), QStringLiteral("ffpfsc")}});
        QVERIFY2(succeeded(pb), qPrintable(describe(pb)));
        const QString renamed = out + QStringLiteral("/renamed.zstd");
        QVERIFY(QFile::copy(pfsc, renamed));

        const QString streamText = QStringLiteral("stream.zstd is a zstd-compressed stream, not a mountable image. Decompress it first.");
        const QString otherText = QStringLiteral("renamed.zstd is not an exFAT image. Only .zstd files that hold an exFAT image can be opened.");
        const struct {
            QString path, text;
        } cases[] = {{stream, streamText}, {renamed, otherText}};
        for (const auto& c : cases) {
            const Status st = integration::checkZstdImage(c.path);
            QVERIFY(!st.ok());
            QCOMPARE(st.error().code, ErrorCode::Unsupported);
            QCOMPARE(st.error().message, c.text);
            // Every way in says the same: the reader, extraction, conversion, comparison.
            auto tree = integration::openFileTree(c.path, {});
            QVERIFY(!tree.ok());
            QCOMPARE(tree.error().message, c.text);
            auto ex = env.queue->enqueue(integration::kGameExtractJob, {{QStringLiteral("source"), c.path},
                                                                        {QStringLiteral("output"), out + QStringLiteral("/x")}});
            QVERIFY(!ex.ok());
            QCOMPARE(ex.error().message, c.text);
            auto cv = env.queue->enqueue(integration::kConvertJob, {{QStringLiteral("image"), c.path},
                                                                    {QStringLiteral("format"), QStringLiteral("ffpkg")},
                                                                    {QStringLiteral("output"), out + QStringLiteral("/x.ffpkg")}});
            QVERIFY(!cv.ok());
            QCOMPARE(cv.error().message, c.text);
            auto cp = env.queue->enqueue(QStringLiteral("image.compare"), {{QStringLiteral("a"), c.path}, {QStringLiteral("b"), exfatImage}});
            QVERIFY(!cp.ok());
            QCOMPARE(cp.error().message, c.text);
        }
        QVERIFY(integration::checkZstdImage(out + QStringLiteral("/missing.zstd")).error().code == ErrorCode::NotFound);
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
