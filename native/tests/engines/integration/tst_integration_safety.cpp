// What the pack, unpack, extract, stage and convert jobs must never do:
// write outside their output for a crafted pack index or image, delete or
// change something they did not create, lose a game's files behind a link or
// an unreadable folder without saying so, or leave an existing image renamed
// when a conversion fails. Also: zipped sources keep the job's phases, and an
// image built with zstd packs is uploaded like any other.

#include "connect/ps5/Module.h"
#include "core/FileUtil.h"
#include "core/FsLinks.h"
#include "core/Settings.h"
#include "core/Zip.h"
#include "engines/ampr/GameFiles.h"
#include "engines/ampr/GamePack.h"
#include "engines/ampr/Module.h"
#include "engines/ampr/ZstdPack.h"
#include "engines/exfat/Module.h"
#include "engines/fpkg/Module.h"
#include "engines/fpkg/SourceStager.h"
#include "engines/general/Module.h"
#include "engines/integration/GameSources.h"
#include "engines/integration/ImageFiles.h"
#include "engines/integration/Module.h"
#include "engines/pfs/Module.h"
#include "engines/pfs/PfsSource.h"
#include "engines/ufs2/Module.h"
#include "jobs/JobQueue.h"
#include "support/TestSupport.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <cstring>

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
    // Runs a job on this thread, so a test can watch its progress and act
    // (cancel, change a file) at a chosen moment.
    Result<JobOutcome> runDirect(const QString& kind, const QJsonObject& params,
                                 const std::function<void(const ProgressSnapshot&)>& watch = {},
                                 const CancellationToken& token = {})
    {
        FW_TRY_ASSIGN(auto job, jobs.create(kind, params));
        QTemporaryDir temp;
        ProgressReporter reporter(watch ? ProgressReporter::Sink(watch) : ProgressReporter::Sink(), 0);
        reporter.setPhases(job->phases(), job->phaseWeights());
        JobContext ctx{QStringLiteral("direct"), token, &reporter, Log(QStringLiteral("test")), store->snapshot(), temp.path()};
        FW_TRY(job->validate(ctx.settings));
        return job->run(ctx);
    }
};

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

QByteArray compressible(qint64 n, char seed)
{
    QByteArray b(qsizetype(n), '\0');
    for (qint64 i = 0; i < n; ++i)
        b[qsizetype(i)] = char('a' + ((i / 97 + seed) % 7));
    return b;
}

// Everything under `root` (files and folders), relative paths, sorted.
QStringList listAll(const QString& root)
{
    QStringList out;
    QDirIterator it(root, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    while (it.hasNext())
        out << QDir(root).relativeFilePath(it.next());
    out.sort();
    return out;
}

QStringList sorted(QStringList list)
{
    list.sort();
    return list;
}

class BytesReader final : public ampr::SequentialReader {
public:
    explicit BytesReader(QByteArray b) : m_b(std::move(b)) {}
    Result<qint64> read(char* dst, qint64 max) override
    {
        const qint64 n = std::min<qint64>(max, m_b.size() - m_pos);
        std::memcpy(dst, m_b.constData() + m_pos, size_t(n));
        m_pos += n;
        return n;
    }

private:
    QByteArray m_b;
    qint64 m_pos = 0;
};

ampr::PackInput input(const QByteArray& path, const QByteArray& data)
{
    ampr::PackInput p;
    p.path = path;
    p.size = data.size();
    p.open = [data]() -> Result<std::unique_ptr<ampr::SequentialReader>> {
        return std::unique_ptr<ampr::SequentialReader>(new BytesReader(data));
    };
    return p;
}

void zipFolder(const QString& folder, const QString& zipPath, const QString& prefix)
{
    zip::Writer w;
    QVERIFY(w.open(zipPath, true).ok());
    QDirIterator it(folder, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString p = it.next();
        QVERIFY(w.addFile(prefix + QDir(folder).relativeFilePath(p), p, zip::Method::Deflate, {}).ok());
    }
    QVERIFY(w.finish(true).ok());
}

// Renames the entry `from` of an exFAT image to `to` (same length, at most 15
// characters) in its name entry and fixes the entry set's checksum, so only
// the name itself is wrong.
bool renameExfatEntry(const QString& image, const QString& from, const QString& to)
{
    if (from.size() != to.size() || from.size() > 14)
        return false;
    QFile f(image);
    if (!f.open(QIODevice::ReadWrite))
        return false;
    QByteArray all = f.readAll();
    QByteArray pattern("\xC1\x00", 2);
    for (const QChar c : from) {
        pattern.append(char(c.unicode() & 0xFF));
        pattern.append(char(c.unicode() >> 8));
    }
    pattern.append("\0\0", 2);
    for (qsizetype p = all.indexOf(pattern); p >= 0; p = all.indexOf(pattern, p + 1)) {
        if (p < 64 || p % 32 != 0 || quint8(all[p - 64]) != 0x85 || quint8(all[p - 32]) != 0xC0 || quint8(all[p - 63]) != 2
            || quint8(all[p - 29]) != from.size())
            continue;
        for (qsizetype i = 0; i < to.size(); ++i) {
            all[p + 2 + 2 * i] = char(to[i].unicode() & 0xFF);
            all[p + 3 + 2 * i] = char(to[i].unicode() >> 8);
        }
        quint16 sum = 0;
        for (int i = 0; i < 96; ++i) {
            if (i == 2 || i == 3)
                continue;
            sum = quint16(((sum & 1) ? 0x8000u : 0u) + (sum >> 1) + quint8(all[p - 64 + i]));
        }
        all[p - 62] = char(sum & 0xFF);
        all[p - 61] = char(sum >> 8);
        return f.seek(0) && f.write(all) == all.size();
    }
    return false;
}

// Takes away (or gives back) the right to list `dir`. False when this
// machine cannot do that.
bool denyListing(const QString& dir, bool deny)
{
#ifdef Q_OS_WIN
    QProcess p;
    p.start(QStringLiteral("icacls"),
            {QDir::toNativeSeparators(dir), deny ? QStringLiteral("/deny") : QStringLiteral("/remove:d"),
             deny ? QStringLiteral("*S-1-1-0:(RD)") : QStringLiteral("*S-1-1-0")});
    return p.waitForFinished(20000) && p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
#else
    return QFile::setPermissions(dir, deny ? QFileDevice::Permissions() : QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
#endif
}

} // namespace

class TestIntegrationSafety : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QVERIFY(m_root.isValid());
        fwtest::isolateAppData(m_root);
        m_game = m_root.filePath(QStringLiteral("dumps/PPSA01234"));
        QVERIFY(fwtest::writeTextFile(m_game + QStringLiteral("/eboot.bin"), QByteArray("\x54\x14\xF5\xEE", 4) + QByteArray(70000, 'E')));
        QVERIFY(fwtest::writeTextFile(m_game + QStringLiteral("/sce_sys/param.json"),
                                      R"({"titleId":"PPSA01234","contentId":"UP0000-PPSA01234_00-0000000000000000","contentVersion":"01.000.000"})"));
        QVERIFY(fwtest::writeTextFile(m_game + QStringLiteral("/data/level1.bin"), compressible(1500 * 1024, 3)));
        QVERIFY(fwtest::writeTextFile(m_game + QStringLiteral("/data/strings/ja.txt"), compressible(300 * 1024, 5)));
        QVERIFY(fwtest::writeTextFile(m_game + QStringLiteral("/fakelib/libSceNpAuth.sprx"), QByteArray("backport")));
        m_env = std::make_unique<Env>(m_root.filePath(QStringLiteral("env")));
    }

    // ---- crafted names -----------------------------------------------------------------

    // A zstd pack index may list any path that has no "..": "C:/..." passes
    // the format's own check. Nothing may be written there.
    void hostileZstdIndexCannotLeaveTheOutput()
    {
        const QString escaped = m_root.filePath(QStringLiteral("escaped"));
        if (escaped.startsWith(u'/'))
            QSKIP("An absolute path in a pack index needs a drive letter (it starts with '/' here and the index refuses it).");
        const QString game = m_root.filePath(QStringLiteral("hostile-zstd/PPSA06666"));
        QVERIFY(fwtest::writeTextFile(game + QStringLiteral("/eboot.bin"), QByteArray("\x54\x14\xF5\xEE", 4) + QByteArray(100, 'E')));
        QVERIFY(fwtest::writeTextFile(game + QStringLiteral("/sce_sys/param.json"), R"({"titleId":"PPSA06666"})"));
        std::vector<ampr::PackInput> in;
        in.push_back(input("data/ok.bin", compressible(300000, 1)));
        in.push_back(input((escaped + QStringLiteral("/evil.bin")).toUtf8(), compressible(5000, 2)));
        in.push_back(input((escaped + QStringLiteral("/evil.sprx")).toUtf8(), compressible(5000, 3))); // always loose when packed again
        auto packed = ampr::writeZstdPack(std::move(in), {}, game, {});
        QVERIFY2(packed.ok(), packed.ok() ? "" : qPrintable(packed.error().toLogString()));

        const QString outDir = m_root.filePath(QStringLiteral("hostile-zstd/out"));
        QVERIFY(QDir().mkpath(outDir));
        for (const QString& kind : {ampr::kUnpackJob, integration::kGameExtractJob, ampr::kPackJob}) {
            auto r = m_env->run(kind, {{QStringLiteral("source"), game}, {QStringLiteral("output"), outDir + QStringLiteral("/result")}});
            QVERIFY2(r && r->state == JobState::Failed, qPrintable(kind + u' ' + describe(r)));
            QVERIFY2(r->error->message.contains(QLatin1String("unsafe path")), qPrintable(r->error->toLogString()));
            QVERIFY2(!QFileInfo::exists(escaped), qPrintable(kind));
            QCOMPARE(listAll(outDir), QStringList()); // no output, no ".part", no marker
        }
        // FPKG staging of the packed folder.
        auto stager = fpkg::folderStagerFor(game);
        QVERIFY(stager);
        const QString dest = m_root.filePath(QStringLiteral("hostile-zstd/stage"));
        QVERIFY(QDir().mkpath(dest));
        const Status staged = stager->stage(game, dest, {}, nullptr);
        QVERIFY(!staged.ok());
        QVERIFY2(staged.error().message.contains(QLatin1String("unsafe path")), qPrintable(staged.error().toLogString()));
        QVERIFY(!QFileInfo::exists(escaped));
        QCOMPARE(listAll(dest), QStringList());
    }

    // An exFAT image whose folder is named "..": the reader reports the name
    // as a problem but still lists the entry.
    void hostileImageTreeCannotLeaveTheOutput()
    {
        const QString src = m_root.filePath(QStringLiteral("hostile-image/src"));
        QVERIFY(fwtest::writeTextFile(src + QStringLiteral("/eboot.bin"), QByteArray(2000, 'E')));
        QVERIFY(fwtest::writeTextFile(src + QStringLiteral("/sce_sys/param.json"), R"({"titleId":"PPSA06667"})"));
        QVERIFY(fwtest::writeTextFile(src + QStringLiteral("/ZQ/evil.bin"), QByteArray(3000, 'x')));
        const QString image = m_root.filePath(QStringLiteral("hostile-image/game.exfat"));
        auto built = m_env->run(QStringLiteral("image.exfat.build"), {{QStringLiteral("source"), src}, {QStringLiteral("output"), image}});
        QVERIFY2(built && built->state == JobState::Succeeded, qPrintable(describe(built)));
        QVERIFY(renameExfatEntry(image, QStringLiteral("ZQ"), QStringLiteral("..")));
        {
            auto files = integration::openGameFiles(image, {});
            QVERIFY2(files.ok(), files.ok() ? "" : qPrintable(files.error().toLogString()));
            QVERIFY2((*files)->find(QStringLiteral("../evil.bin")) >= 0, "the crafted name must reach the game's file list");
        }

        const QString outDir = m_root.filePath(QStringLiteral("hostile-image/out"));
        QVERIFY(QDir().mkpath(outDir));
        auto r = m_env->run(integration::kGameExtractJob, {{QStringLiteral("source"), image}, {QStringLiteral("output"), outDir + QStringLiteral("/result")}});
        QVERIFY2(r && r->state == JobState::Failed, qPrintable(describe(r)));
        QVERIFY2(r->error->message.contains(QLatin1String("unsafe path")), qPrintable(r->error->toLogString()));
        QCOMPARE(listAll(outDir), QStringList());

        auto stager = fpkg::sourceStagerFor(image);
        QVERIFY(stager);
        const QString dest = m_root.filePath(QStringLiteral("hostile-image/stage"));
        QVERIFY(QDir().mkpath(dest));
        const Status staged = stager->stage(image, dest, {}, nullptr);
        QVERIFY(!staged.ok());
        QVERIFY2(staged.error().message.contains(QLatin1String("unsafe path")), qPrintable(staged.error().toLogString()));
        QCOMPARE(listAll(dest), QStringList());
    }

    // ---- outputs the job did not create ----------------------------------------------------

    void packAndUnpackOnlyRemoveWhatTheyCreated()
    {
        const QString base = m_root.filePath(QStringLiteral("owned"));
        QVERIFY(QDir().mkpath(base));
        const QJsonObject pack{{QStringLiteral("source"), m_game}, {QStringLiteral("output"), base + QStringLiteral("/packed")}};
        const QString packed = base + QStringLiteral("/packed");

        // The output is an existing file: refused, the file stays.
        QVERIFY(fwtest::writeTextFile(packed, "mine"));
        for (const QString& kind : {ampr::kPackJob, ampr::kUnpackJob, integration::kGameExtractJob}) {
            auto r = m_env->runDirect(kind, pack);
            QVERIFY2(!r.ok(), qPrintable(kind));
            QCOMPARE(r.error().code, ErrorCode::AlreadyExists);
            QVERIFY2(r.error().message.contains(QLatin1String("not a folder")), qPrintable(r.error().toLogString()));
            QCOMPARE(fileBytes(packed), QByteArray("mine"));
        }
        QVERIFY(QFile::remove(packed));

        // The (empty) output gains a file while the job is packing: the job
        // fails at the end and the folder with the user's file is untouched.
        QVERIFY(QDir().mkpath(packed));
        bool dropped = false;
        auto late = m_env->runDirect(ampr::kPackJob, pack, [&](const ProgressSnapshot& s) {
            if (!dropped && s.phase == u"Packing")
                dropped = fwtest::writeTextFile(packed + QStringLiteral("/keep.txt"), "mine");
        });
        QVERIFY(dropped);
        QVERIFY(!late.ok());
        QCOMPARE(late.error().code, ErrorCode::AlreadyExists);
        QCOMPARE(listAll(base), QStringList({QStringLiteral("packed"), QStringLiteral("packed/keep.txt")}));
        QCOMPARE(fileBytes(packed + QStringLiteral("/keep.txt")), QByteArray("mine"));
        // And before it starts: run() checks again after validate().
        auto before = m_env->runDirect(ampr::kPackJob, pack);
        QVERIFY(!before.ok());
        QCOMPARE(before.error().code, ErrorCode::AlreadyExists);
        QCOMPARE(fileBytes(packed + QStringLiteral("/keep.txt")), QByteArray("mine"));
        QVERIFY(QFile::remove(packed + QStringLiteral("/keep.txt")));

        // Cancelled half-way: the empty folder the user made is still there
        // and still empty; nothing else is left.
        CancellationSource cancel;
        auto cancelled = m_env->runDirect(ampr::kPackJob, pack, [&](const ProgressSnapshot& s) {
            if (s.phase == u"Packing" && s.done > 0)
                cancel.cancel();
        }, cancel.token());
        QVERIFY(!cancelled.ok());
        QVERIFY2(cancelled.error().isCancelled(), qPrintable(cancelled.error().toLogString()));
        QCOMPARE(listAll(base), QStringList({QStringLiteral("packed")}));

        // A source file that changes while it is packed fails the job half-way.
        const QString moving = m_root.filePath(QStringLiteral("owned-src/PPSA01234"));
        for (const QString& rel : {QStringLiteral("eboot.bin"), QStringLiteral("sce_sys/param.json"), QStringLiteral("data/level1.bin")})
            QVERIFY(fwtest::writeTextFile(moving + u'/' + rel, fileBytes(m_game + u'/' + rel)));
        bool shrunk = false;
        auto failed = m_env->runDirect(ampr::kPackJob, {{QStringLiteral("source"), moving}, {QStringLiteral("output"), packed}},
                                       [&](const ProgressSnapshot& s) {
                                           if (!shrunk && s.phase == u"Packing")
                                               shrunk = QFile::resize(moving + QStringLiteral("/data/level1.bin"), 1000);
                                       });
        QVERIFY(shrunk);
        QVERIFY(!failed.ok());
        QVERIFY(!failed.error().isCancelled());
        QCOMPARE(listAll(base), QStringList({QStringLiteral("packed")}));

        // A good run fills the folder.
        auto good = m_env->runDirect(ampr::kPackJob, pack);
        QVERIFY2(good.ok(), good.ok() ? "" : qPrintable(good.error().toLogString()));
        QVERIFY(QFile::exists(packed + QStringLiteral("/ampr_zstd.index")));
        QVERIFY(!QFileInfo::exists(packed + QStringLiteral(".part")));
        QVERIFY(!QFileInfo::exists(packed + QStringLiteral(".part.writing")));

        // Unpack: a damaged pack volume fails half-way, a cancel stops it; an
        // output folder the user made stays (empty), a new one never appears.
        const QString broken = base + QStringLiteral("/broken");
        for (const QString& rel : listAll(packed))
            if (QFileInfo(packed + u'/' + rel).isFile())
                QVERIFY(fwtest::writeTextFile(broken + u'/' + rel, fileBytes(packed + u'/' + rel)));
        {
            QFile vol(broken + QStringLiteral("/ampr_zstd-000.pak"));
            QVERIFY(vol.open(QIODevice::ReadWrite));
            QVERIFY(vol.seek(vol.size() - 20));
            char c = 0;
            QVERIFY(vol.read(&c, 1) == 1);
            c = char(c ^ 0x5A);
            QVERIFY(vol.seek(vol.size() - 20));
            QVERIFY(vol.write(&c, 1) == 1);
        }
        const QString un = base + QStringLiteral("/unpacked");
        QVERIFY(QDir().mkpath(un));
        auto damaged = m_env->runDirect(ampr::kUnpackJob, {{QStringLiteral("source"), broken}, {QStringLiteral("output"), un}});
        QVERIFY(!damaged.ok());
        QVERIFY(QFileInfo(un).isDir());
        QCOMPARE(listAll(un), QStringList());
        QVERIFY(!QFileInfo::exists(un + QStringLiteral(".part")));
        QVERIFY(!QFileInfo::exists(un + QStringLiteral(".part.writing")));

        const QString un2 = base + QStringLiteral("/unpacked-new");
        CancellationSource cancel2;
        auto stopped = m_env->runDirect(ampr::kUnpackJob, {{QStringLiteral("source"), packed}, {QStringLiteral("output"), un2}},
                                        [&](const ProgressSnapshot& s) {
                                            if (s.phase == u"Unpacking" && s.done > 0)
                                                cancel2.cancel();
                                        }, cancel2.token());
        QVERIFY(!stopped.ok());
        QVERIFY(stopped.error().isCancelled());
        QVERIFY(!QFileInfo::exists(un2));
        QVERIFY(!QFileInfo::exists(un2 + QStringLiteral(".part")));
        QVERIFY(!QFileInfo::exists(un2 + QStringLiteral(".part.writing")));

        // A leftover "<output>.part" that is not this app's is refused, not removed.
        QVERIFY(fwtest::writeTextFile(un2 + QStringLiteral(".part/theirs.txt"), "theirs"));
        auto foreign = m_env->runDirect(ampr::kUnpackJob, {{QStringLiteral("source"), packed}, {QStringLiteral("output"), un2}});
        QVERIFY(!foreign.ok());
        QCOMPARE(foreign.error().code, ErrorCode::AlreadyExists);
        QCOMPARE(fileBytes(un2 + QStringLiteral(".part/theirs.txt")), QByteArray("theirs"));
        QVERIFY(removeTreeSafely(un2 + QStringLiteral(".part")).ok());

        auto fine = m_env->runDirect(ampr::kUnpackJob, {{QStringLiteral("source"), packed}, {QStringLiteral("output"), un2}});
        QVERIFY2(fine.ok(), fine.ok() ? "" : qPrintable(fine.error().toLogString()));
        QCOMPARE(fileBytes(un2 + QStringLiteral("/data/level1.bin")), fileBytes(m_game + QStringLiteral("/data/level1.bin")));
    }

    // The packed folder is kept: its loose files are copies, so changing one
    // in place never changes the dump it was made from.
    void packedFolderSharesNothingWithTheSource()
    {
        const QString out = m_root.filePath(QStringLiteral("shared/packed"));
        auto r = m_env->run(ampr::kPackJob, {{QStringLiteral("source"), m_game}, {QStringLiteral("output"), out}});
        QVERIFY2(r && r->state == JobState::Succeeded, qPrintable(describe(r)));
        const QByteArray original = fileBytes(m_game + QStringLiteral("/eboot.bin"));
        QCOMPARE(fileBytes(out + QStringLiteral("/eboot.bin")), original);
        for (const QString& rel : {QStringLiteral("eboot.bin"), QStringLiteral("sce_sys/param.json"), QStringLiteral("fakelib/libSceNpAuth.sprx")}) {
            QFile f(out + u'/' + rel);
            QVERIFY(f.open(QIODevice::ReadWrite));
            QVERIFY(f.write("PATCHED") == 7);
        }
        QCOMPARE(fileBytes(m_game + QStringLiteral("/eboot.bin")), original);
        QVERIFY(fileBytes(m_game + QStringLiteral("/sce_sys/param.json")).startsWith("{"));
        QCOMPARE(fileBytes(m_game + QStringLiteral("/fakelib/libSceNpAuth.sprx")), QByteArray("backport"));
    }

    // ---- sources that are not what they seem ---------------------------------------------------

    // A folder that is a junction (a dump spread over two drives) is packed
    // like the plain builds pack it; a link back into the game is left out
    // with a warning instead of silently.
    void linkedFoldersArePackedAndLoopsAreReported()
    {
        const QString game = m_root.filePath(QStringLiteral("linked/PPSA07777"));
        const QString far = m_root.filePath(QStringLiteral("linked-elsewhere/assets"));
        QVERIFY(fwtest::writeTextFile(game + QStringLiteral("/eboot.bin"), QByteArray("\x54\x14\xF5\xEE", 4) + QByteArray(100, 'E')));
        QVERIFY(fwtest::writeTextFile(game + QStringLiteral("/sce_sys/param.json"), R"({"titleId":"PPSA07777"})"));
        QVERIFY(fwtest::writeTextFile(game + QStringLiteral("/data/near.bin"), compressible(300000, 1)));
        QVERIFY(fwtest::writeTextFile(far + QStringLiteral("/far.bin"), compressible(400000, 2)));
        if (!createDirectoryJunction(game + QStringLiteral("/assets"), far).ok()
            || !createDirectoryJunction(game + QStringLiteral("/data/back"), game).ok())
            QSKIP("Folder links cannot be created here.");

        auto files = ampr::openFolderGame(game, {});
        QVERIFY2(files.ok(), files.ok() ? "" : qPrintable(files.error().toLogString()));
        QVERIFY((*files)->find(QStringLiteral("assets/far.bin")) >= 0);
        QCOMPARE((*files)->find(QStringLiteral("data/back/eboot.bin")), -1);
        QCOMPARE((*files)->warnings().size(), 1);
        QVERIFY2((*files)->warnings().first().contains(QLatin1String("data/back")), qPrintable((*files)->warnings().first()));

        const QString out = m_root.filePath(QStringLiteral("linked-out/packed"));
        auto r = m_env->run(ampr::kPackJob, {{QStringLiteral("source"), game}, {QStringLiteral("output"), out}});
        QVERIFY2(r && r->state == JobState::Succeeded, qPrintable(describe(r)));
        QVERIFY2(r->outcome->warnings.join(u'\n').contains(QLatin1String("leads back into its own folder")),
                 qPrintable(r->outcome->warnings.join(u'\n')));
        auto packed = ampr::openFolderGame(out, {});
        QVERIFY(packed.ok());
        auto logical = ampr::logicalGame(*packed, {});
        QVERIFY(logical.ok());
        const int i = (*logical)->find(QStringLiteral("assets/far.bin"));
        QVERIFY2(i >= 0, "the files behind the junction must be in the packed game");
        QCOMPARE((*logical)->items()[size_t(i)].size, qint64(400000));
        // The links are removed before the temporary folder is (a junction is never followed when deleting).
        QVERIFY(removeDirectoryJunction(game + QStringLiteral("/assets")).ok());
        QVERIFY(removeDirectoryJunction(game + QStringLiteral("/data/back")).ok());
    }

    // A folder the user may not list is not an empty folder: every scan fails
    // and names it.
    void unreadableFolderFailsTheScan()
    {
        const QString game = m_root.filePath(QStringLiteral("denied/PPSA08888"));
        QVERIFY(fwtest::writeTextFile(game + QStringLiteral("/eboot.bin"), QByteArray("\x54\x14\xF5\xEE", 4) + QByteArray(100, 'E')));
        QVERIFY(fwtest::writeTextFile(game + QStringLiteral("/sce_sys/param.json"), R"({"titleId":"PPSA08888"})"));
        QVERIFY(fwtest::writeTextFile(game + QStringLiteral("/data/a.bin"), compressible(300000, 1)));
        QVERIFY(fwtest::writeTextFile(game + QStringLiteral("/locked/secret.bin"), compressible(300000, 2)));
        const QString locked = game + QStringLiteral("/locked");
        if (!denyListing(locked, true))
            QSKIP("Access to a folder cannot be taken away here.");
        struct Restore {
            QString dir;
            ~Restore() { (void)denyListing(dir, false); }
        } restore{locked};
        if (!QDir(locked).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty())
            QSKIP("This account can still list the folder (administrator or root).");

        pfs::PlanOptions po;
        auto plan = pfs::planFolder(game, po, {});
        QVERIFY(!plan.ok());
        QCOMPARE(plan.error().code, ErrorCode::PermissionDenied);
        QVERIFY2(plan.error().message.contains(QLatin1String("locked")), qPrintable(plan.error().toLogString()));

        auto files = ampr::openFolderGame(game, {});
        QVERIFY(!files.ok());
        QCOMPARE(files.error().code, ErrorCode::PermissionDenied);

        auto tree = integration::openFileTree(game, {});
        QVERIFY(!tree.ok());
        QCOMPARE(tree.error().code, ErrorCode::PermissionDenied);

        const QString out = m_root.filePath(QStringLiteral("denied-out"));
        QVERIFY(QDir().mkpath(out));
        auto pack = m_env->run(ampr::kPackJob, {{QStringLiteral("source"), game}, {QStringLiteral("output"), out + QStringLiteral("/packed")}});
        QVERIFY2(pack && pack->state == JobState::Failed, qPrintable(describe(pack)));
        auto image = m_env->run(QStringLiteral("image.pfs.build"), {{QStringLiteral("source"), game}, {QStringLiteral("output"), out + QStringLiteral("/g.ffpfs")}});
        QVERIFY2(image && image->state == JobState::Failed, qPrintable(describe(image)));
        QCOMPARE(listAll(out), QStringList());
    }

    // ---- zipped sources ----------------------------------------------------------------------

    void zippedSourcesKeepTheJobsPhases()
    {
        const QString zip = m_root.filePath(QStringLiteral("zips/PPSA01234.zip"));
        QVERIFY(QDir().mkpath(QFileInfo(zip).absolutePath()));
        zipFolder(m_game, zip, QStringLiteral("PPSA01234/"));
        if (QTest::currentTestFailed())
            return;
        const QString out = m_root.filePath(QStringLiteral("zips/out"));
        QVERIFY(QDir().mkpath(out));
        struct Case {
            QString kind;
            QJsonObject params;
            QStringList expected;
        };
        const std::vector<Case> cases = {
            {ampr::kPackJob, {{QStringLiteral("source"), zip}, {QStringLiteral("output"), out + QStringLiteral("/packed")}},
             {QStringLiteral("Unpacking ZIP"), QStringLiteral("Preparing"), QStringLiteral("Packing"), QStringLiteral("Checking packs")}},
            {QStringLiteral("image.pfs.build"), {{QStringLiteral("source"), zip}, {QStringLiteral("output"), out + QStringLiteral("/plain.ffpfs")}},
             {QStringLiteral("Unpacking ZIP"), QStringLiteral("Scanning"), QStringLiteral("Compressing and writing"), QStringLiteral("Verifying")}},
            {integration::kZstdBuildJob,
             {{QStringLiteral("format"), QStringLiteral("ffpfs")}, {QStringLiteral("source"), zip}, {QStringLiteral("output"), out + QStringLiteral("/zstd.ffpfs")}},
             {QStringLiteral("Unpacking ZIP"), QStringLiteral("Preparing"), QStringLiteral("Packing"), QStringLiteral("Checking packs"),
              QStringLiteral("Scanning"), QStringLiteral("Compressing and writing"), QStringLiteral("Verifying")}},
        };
        for (const Case& c : cases) {
            QStringList seen;
            int lastCount = 0;
            auto r = m_env->runDirect(c.kind, c.params, [&](const ProgressSnapshot& s) {
                if (seen.isEmpty() || seen.last() != s.phase)
                    seen << s.phase;
                lastCount = s.phaseCount;
            });
            QVERIFY2(r.ok(), r.ok() ? "" : qPrintable(c.kind + u' ' + r.error().toLogString()));
            // The job may enter "Preparing"/"Scanning" before the ZIP is unpacked.
            while (!seen.isEmpty() && seen.first() != QLatin1String("Unpacking ZIP"))
                seen.removeFirst();
            QCOMPARE(seen, c.expected);
            QCOMPARE(lastCount, int(c.expected.size()) - 1);
        }
    }

    // ---- conversion ----------------------------------------------------------------------------

    // An existing output keeps its name and its bytes until the new image is
    // ready; a cancelled conversion leaves it exactly as it was.
    void convertNeverSetsTheOldImageAside()
    {
        const QString dir = m_root.filePath(QStringLiteral("convert"));
        QVERIFY(QDir().mkpath(dir));
        const QString exfat = dir + QStringLiteral("/game.exfat");
        auto built = m_env->run(QStringLiteral("image.exfat.build"), {{QStringLiteral("source"), m_game}, {QStringLiteral("output"), exfat}});
        QVERIFY2(built && built->state == JobState::Succeeded, qPrintable(describe(built)));
        const QStringList before = listAll(dir);

        struct Target {
            QString format, method;
        };
        for (const Target& t : {Target{QStringLiteral("ffpfsc"), QStringLiteral("direct")}, Target{QStringLiteral("ffpkg"), QStringLiteral("repack")},
                                Target{QStringLiteral("ffpfs"), QStringLiteral("repack")}}) {
            const QString out = dir + QStringLiteral("/out.") + t.format;
            QVERIFY(fwtest::writeTextFile(out, "the previous image"));
            const QJsonObject params{{QStringLiteral("image"), exfat}, {QStringLiteral("format"), t.format}, {QStringLiteral("output"), out},
                                     {QStringLiteral("method"), t.method}, {QStringLiteral("replaceExisting"), true}};
            // Without permission to replace it: refused.
            QJsonObject keep = params;
            keep.remove(QStringLiteral("replaceExisting"));
            QVERIFY(!m_env->runDirect(integration::kConvertJob, keep).ok());

            // Cancelled once work is under way.
            CancellationSource cancel;
            bool presentThroughout = true;
            auto stopped = m_env->runDirect(integration::kConvertJob, params, [&](const ProgressSnapshot& s) {
                presentThroughout = presentThroughout && fileBytes(out) == QByteArray("the previous image");
                if (s.done > 0)
                    cancel.cancel();
            }, cancel.token());
            QVERIFY2(!stopped.ok(), qPrintable(t.format));
            QVERIFY2(stopped.error().isCancelled(), qPrintable(stopped.error().toLogString()));
            QVERIFY2(presentThroughout, "the existing output must keep its name while the conversion runs");
            QCOMPARE(fileBytes(out), QByteArray("the previous image"));
            QCOMPARE(listAll(dir), sorted(QStringList(before) << QFileInfo(out).fileName())); // no .old, .fw-old or .part

            // Finished: replaced by the new image.
            auto done = m_env->runDirect(integration::kConvertJob, params);
            QVERIFY2(done.ok(), done.ok() ? "" : qPrintable(t.format + u' ' + done.error().toLogString()));
            QVERIFY(done->verification != Verification::Failed);
            QVERIFY(QFileInfo(out).size() > 1000);
            QCOMPARE(listAll(dir), sorted(QStringList(before) << QFileInfo(out).fileName()));
            QVERIFY(QFile::remove(out));
        }

        // A container converts back to the image it holds, over an existing file too.
        const QString box = dir + QStringLiteral("/box.ffpfsc");
        auto boxed = m_env->runDirect(integration::kConvertJob, {{QStringLiteral("image"), exfat}, {QStringLiteral("format"), QStringLiteral("ffpfsc")},
                                                               {QStringLiteral("output"), box}});
        QVERIFY2(boxed.ok(), boxed.ok() ? "" : qPrintable(boxed.error().toLogString()));
        const QString back = dir + QStringLiteral("/back.exfat");
        QVERIFY(fwtest::writeTextFile(back, "the previous image"));
        auto unboxed = m_env->runDirect(integration::kConvertJob, {{QStringLiteral("image"), box}, {QStringLiteral("format"), QStringLiteral("exfat")},
                                                                 {QStringLiteral("output"), back}, {QStringLiteral("replaceExisting"), true}});
        QVERIFY2(unboxed.ok(), unboxed.ok() ? "" : qPrintable(unboxed.error().toLogString()));
        QCOMPARE(fileBytes(back), fileBytes(exfat));
        QCOMPARE(listAll(dir), sorted(QStringList(before) << QStringLiteral("back.exfat") << QStringLiteral("box.ffpfsc")));
    }

    // ---- upload after a build ------------------------------------------------------------------

    void zstdBuildsAreQueuedForUploadAndPackedFoldersAreNot()
    {
        Env env(m_root.filePath(QStringLiteral("upload-env")));
        QVERIFY(env.store->setValue(ps5::keys::kConsoleIp, QStringLiteral("127.0.0.1")).ok());
        integration::PostBuildActions actions(*env.queue, *env.store);
        QSignalSpy queued(&actions, &integration::PostBuildActions::uploadQueued);
        const QString out = m_root.filePath(QStringLiteral("upload"));
        QVERIFY(QDir().mkpath(out));
        auto z = env.run(integration::kZstdBuildJob,
                         {{QStringLiteral("format"), QStringLiteral("exfat")}, {QStringLiteral("source"), m_game}, {QStringLiteral("output"), out + QStringLiteral("/z.exfat")}},
                         {{integration::keys::kAutoUpload, true}});
        QVERIFY2(z && z->state == JobState::Succeeded, qPrintable(describe(z)));
        QVERIFY(QTest::qWaitFor([&] { return queued.count() == 1; }, 5000));
        const auto up = env.queue->record(queued.first().at(1).toString());
        QVERIFY(up);
        QCOMPARE(up->kind, ps5::jobkinds::kUpload);
        QCOMPARE(up->parameters.value(QStringLiteral("sources")).toArray().first().toString(), out + QStringLiteral("/z.exfat"));
        env.queue->cancel(up->id);

        auto p = env.run(ampr::kPackJob, {{QStringLiteral("source"), m_game}, {QStringLiteral("output"), out + QStringLiteral("/packed")}},
                         {{integration::keys::kAutoUpload, true}});
        QVERIFY2(p && p->state == JobState::Succeeded, qPrintable(describe(p)));
        QTest::qWait(300);
        QCOMPARE(queued.count(), 1);
    }

    // ---- the zstd build's own details -------------------------------------------------------

    void zstdBuildTitleKeepsTheFoldersWholeName()
    {
        const QString out = m_root.filePath(QStringLiteral("titles/x.exfat"));
        const auto title = [&](const QJsonObject& extra, const QString& source) {
            QJsonObject p{{QStringLiteral("format"), QStringLiteral("exfat")}, {QStringLiteral("source"), source}, {QStringLiteral("output"), out}};
            for (auto it = extra.begin(); it != extra.end(); ++it)
                p.insert(it.key(), it.value());
            auto job = m_env->jobs.create(integration::kZstdBuildJob, p);
            return job.ok() ? (*job)->title() : QStringLiteral("(no job)");
        };
        QCOMPARE(title({}, m_root.filePath(QStringLiteral("titles/Astro Bot v1.020"))), QStringLiteral("Astro Bot v1.020"));
        const QString zip = m_root.filePath(QStringLiteral("titles/Game v1.5.zip"));
        QVERIFY(fwtest::writeTextFile(zip, "not a real archive"));
        QCOMPARE(title({}, zip), QStringLiteral("Game v1.5"));
        QCOMPARE(title({{QStringLiteral("title"), QStringLiteral("My title")}}, zip), QStringLiteral("My title"));
    }

    // The PFS exclusions mean the same with and without zstd packs: a
    // trailing "/" or a leading "./" does not change what a pattern matches.
    void zstdBuildReadsPfsExclusionsLikeThePfsScan()
    {
        Env env(m_root.filePath(QStringLiteral("excl-env")));
        QVERIFY(env.store->setValue(pfs::keys::kExcludes, QStringList{QStringLiteral("ja.txt/"), QStringLiteral("./fakelib/libSceNpAuth.sprx")}).ok());
        const QString dir = m_root.filePath(QStringLiteral("excl"));
        QVERIFY(QDir().mkpath(dir));
        for (const bool zstd : {false, true}) {
            const QString image = dir + (zstd ? QStringLiteral("/zstd.ffpfs") : QStringLiteral("/plain.ffpfs"));
            auto r = zstd ? env.run(integration::kZstdBuildJob, {{QStringLiteral("format"), QStringLiteral("ffpfs")}, {QStringLiteral("source"), m_game},
                                                                 {QStringLiteral("output"), image}})
                          : env.run(QStringLiteral("image.pfs.build"), {{QStringLiteral("source"), m_game}, {QStringLiteral("output"), image}});
            QVERIFY2(r && r->state == JobState::Succeeded, qPrintable(describe(r)));
            auto files = integration::openGameFiles(image, {});
            QVERIFY(files.ok());
            auto logical = ampr::logicalGame(*files, {});
            QVERIFY(logical.ok());
            QVERIFY2((*logical)->find(QStringLiteral("data/level1.bin")) >= 0, qPrintable(image));
            QCOMPARE((*logical)->find(QStringLiteral("data/strings/ja.txt")), -1);
            QCOMPARE((*logical)->find(QStringLiteral("fakelib/libSceNpAuth.sprx")), -1);
        }
    }

    // A ZIP source gets the same early output check as a folder.
    void zippedBuildRefusesAFolderAsItsOutputAtOnce()
    {
        const QString zip = m_root.filePath(QStringLiteral("early/PPSA01234.zip"));
        QVERIFY(QDir().mkpath(m_root.filePath(QStringLiteral("early/out.ffpfs")))); // a folder of that name
        zipFolder(m_game, zip, QStringLiteral("PPSA01234/"));
        if (QTest::currentTestFailed())
            return;
        auto job = m_env->jobs.create(QStringLiteral("image.pfs.build"),
                                      {{QStringLiteral("source"), zip}, {QStringLiteral("output"), m_root.filePath(QStringLiteral("early/out.ffpfs"))},
                                       {QStringLiteral("replaceExisting"), true}});
        QVERIFY(job.ok());
        const Status st = (*job)->validate(m_env->store->snapshot());
        QVERIFY(!st.ok());
        QCOMPARE(st.error().code, ErrorCode::InvalidArgument);
        QVERIFY2(st.error().message.contains(QLatin1String("not a folder")), qPrintable(st.error().message));
    }

private:
    QTemporaryDir m_root;
    QString m_game;
    std::unique_ptr<Env> m_env;
};

QTEST_GUILESS_MAIN(TestIntegrationSafety)
#include "tst_integration_safety.moc"
