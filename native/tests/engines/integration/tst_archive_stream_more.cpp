// The builds that stream an archived dump since the "extend it" decision on
// GitHub #37 (the first ones, exFAT and FFPKG, are tst_archive_stream's):
//
//  * an uncompressed PFS image (.ffpfs) from a ZIP, 7z or RAR, and a
//    compressed one (.ffpfsc) from a ZIP, byte for byte the image built from
//    the same archive unpacked to a folder, verified against the digests
//    taken while writing (.ffpfs) or against the ZIP again (.ffpfsc);
//  * zstd packs (ampr.pack, image.build.zstd): the packed folder made in one
//    pass through the archive holds byte for byte the packs, loose files and
//    runtime that packing the unpacked folder gives (one archive id for
//    both); ampr_emu.index differs at most in the runtime's time (written
//    when the build runs). A ZIP goes straight into the packs; a 7z or RAR
//    goes through a spool of compressed chunks in the job's temporary folder.
//
// Then the jobs: what streams and what the log says, how much of the
// temporary folder they use, a damaged entry, an entry larger than listed, a
// cancel in the middle of the pass, and the builds that still unpack first.
// 7z archives come from libarchive's writer (solid); RAR from libarchive's
// samples. All synthetic: MOCK as far as the console goes (whether the
// console mounts these images and reads these packs is HARDWARE REQUIRED,
// as for any build).

#include "connect/ps5/Module.h"
#include "core/ArchiveReader.h"
#include "core/ArchiveSource.h"
#include "core/ArchiveStream.h"
#include "core/Dependencies.h"
#include "core/FileUtil.h"
#include "core/Log.h"
#include "core/Settings.h"
#include "core/Zip.h"
#include "engines/ampr/GameFiles.h"
#include "engines/ampr/GamePack.h"
#include "engines/ampr/Module.h"
#include "engines/exfat/Module.h"
#include "engines/fpkg/Module.h"
#include "engines/general/Module.h"
#include "engines/integration/GameSources.h"
#include "engines/integration/Module.h"
#include "engines/pfs/AmprIndex.h"
#include "engines/pfs/Module.h"
#include "engines/pfs/PfsBuilder.h"
#include "engines/pfs/PfsVerify.h"
#include "engines/ufs2/Module.h"
#include "jobs/Job.h"
#include "support/TestSupport.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>

#include <archive.h>
#include <archive_entry.h>

#include <algorithm>
#include <atomic>
#include <mutex>

using namespace fw;
namespace fwa = fw::archive;

namespace {

constexpr qint64 kMtime = 1717243200;          // 2024-06-01 12:00:00 UTC
constexpr qint64 kBuildTime = 1731600000;      // one fixed "now" for both builds
const std::array<char, 16> kArchiveId = {'s', 't', 'r', 'e', 'a', 'm', '-', 't', 'e', 's', 't', '-', 'i', 'd', '0', '1'};

QByteArray readAll(const QString& p)
{
    QFile f(p);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

bool writeAll(const QString& p, const QByteArray& data)
{
    QDir().mkpath(QFileInfo(p).absolutePath());
    QFile f(p);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(data) == data.size();
}

QByteArray sha(const QString& p)
{
    QFile f(p);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash h(QCryptographicHash::Sha256);
    h.addData(&f);
    return h.result().toHex();
}

QByteArray noise(qsizetype size, quint32 seed)
{
    QByteArray b(size, Qt::Uninitialized);
    quint32 x = seed * 2654435761u + 1;
    for (qsizetype i = 0; i < size; ++i) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        b[i] = char(x >> 11);
    }
    return b;
}

QByteArray text(int lines)
{
    QByteArray t;
    for (int i = 0; i < lines; ++i)
        t += "line " + QByteArray::number(i % 97) + " of the game's strings\n";
    return t;
}

struct Item {
    QString name;
    QByteArray data;
    bool folder = false;
    qint64 mtime = kMtime;
};

Item file(const QString& n, const QByteArray& d, qint64 t = kMtime) { return {n, d, false, t}; }
Item folder(const QString& n) { return {n, {}, true, kMtime}; }

// A made-up game with plain ASCII names (PFS takes no others), listed in an
// order unlike the image's and the packs': nested and empty folders, an
// empty file, 5 MiB that do not compress and text that does, a run of zeros,
// the AMPR library (so a PFS build adds ampr_emu.index and a pack build
// replaces the runtime), files that stay loose and files that are packed.
QList<Item> game(const QString& prefix = {})
{
    QByteArray zeros(2 * 1024 * 1024 + 5, '\0');
    zeros[0] = 'Z';
    zeros[zeros.size() - 1] = 'z';
    return {
        file(prefix + QStringLiteral("zz-last-in-the-image.bin"), noise(70000, 9), kMtime + 3600),
        file(prefix + QStringLiteral("data/big.bin"), noise(5 * 1024 * 1024 + 17, 4), kMtime - 86400 * 400),
        file(prefix + QStringLiteral("data/deep/deeper/deepest/text.txt"), text(30000), kMtime + 1),
        file(prefix + QStringLiteral("data/zero.bin"), QByteArray()),
        file(prefix + QStringLiteral("data/zeros.bin"), zeros),
        folder(prefix + QStringLiteral("data/empty folder")),
        file(prefix + QStringLiteral("fakelib/libSceAmpr.sprx"), QByteArray("old runtime ") + noise(3000, 21)),
        file(prefix + QStringLiteral("fakelib/libSceNpAuth.sprx"), QByteArray("backport")),
        file(prefix + QStringLiteral("dlc_emu.ini"), "[dlc]\n"),
        file(prefix + QStringLiteral("sce_sys/icon0.png"), noise(4000, 2)),
        file(prefix + QStringLiteral("sce_sys/param.json"), "{\"titleId\":\"PPSA01234\",\"titleName\":\"Stream test\"}"),
        file(prefix + QStringLiteral("Eboot.BIN.bak"), noise(1000, 7)),
        file(prefix + QStringLiteral("eboot.bin"), QByteArray("\x4F\x15\x3D\x1D", 4) + noise(100000, 1), kMtime + 7),
    };
}

qint64 bytesOf(const QList<Item>& items)
{
    qint64 n = 0;
    for (const Item& i : items)
        n += i.data.size();
    return n;
}

la_ssize_t appendToFile(struct archive*, void* client, const void* buffer, size_t length)
{
    QFile* f = static_cast<QFile*>(client);
    return f->write(static_cast<const char*>(buffer), qint64(length)) == qint64(length) ? la_ssize_t(length) : -1;
}

// libarchive's 7z writer: every file in one block, so the archive is solid.
bool write7z(const QString& path, const QList<Item>& items, const char* compression = "lzma2")
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    struct archive* a = archive_write_new();
    bool ok = archive_write_set_format_7zip(a) == ARCHIVE_OK;
    const QByteArray options = QByteArray("7zip:compression=") + compression + ",7zip:compression-level=1";
    ok = ok && archive_write_set_options(a, options.constData()) == ARCHIVE_OK;
    archive_write_set_bytes_in_last_block(a, 1);
    ok = ok && archive_write_open(a, &out, nullptr, appendToFile, nullptr) == ARCHIVE_OK;
    for (const Item& item : items) {
        if (!ok)
            break;
        struct archive_entry* e = archive_entry_new();
        archive_entry_copy_pathname_w(e, item.name.toStdWString().c_str());
        archive_entry_set_mtime(e, time_t(item.mtime), 0);
        if (item.folder) {
            archive_entry_set_filetype(e, AE_IFDIR);
            archive_entry_set_perm(e, 0755);
        } else {
            archive_entry_set_filetype(e, AE_IFREG);
            archive_entry_set_perm(e, 0644);
            archive_entry_set_size(e, item.data.size());
        }
        ok = archive_write_header(a, e) == ARCHIVE_OK;
        if (ok && !item.folder && !item.data.isEmpty())
            ok = archive_write_data(a, item.data.constData(), size_t(item.data.size())) == la_ssize_t(item.data.size());
        archive_entry_free(e);
    }
    ok = archive_write_close(a) == ARCHIVE_OK && ok;
    archive_write_free(a);
    return ok && out.flush();
}

bool writeZip(const QString& path, const QList<Item>& items, zip::Method method)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    zip::Writer w;
    if (!w.open(path, true).ok())
        return false;
    for (const Item& i : items) {
        const QDateTime t = QDateTime::fromSecsSinceEpoch(i.mtime);
        const Status st = i.folder ? w.addDirectory(i.name + u'/', t) : w.addData(i.name, i.data, method, t);
        if (!st.ok())
            return false;
    }
    return w.finish().ok();
}

QByteArray uudecode(const QByteArray& txt)
{
    QByteArray out;
    bool inside = false;
    for (QByteArray line : txt.split('\n')) {
        while (line.endsWith('\r'))
            line.chop(1);
        if (!inside) {
            inside = line.startsWith("begin ");
            continue;
        }
        if (line == "end")
            break;
        if (line.isEmpty())
            continue;
        const auto six = [&](qsizetype i) { return i < line.size() ? (uchar(line.at(i)) - 32) & 63 : 0; };
        const int count = six(0);
        QByteArray bytes;
        for (qsizetype i = 1; bytes.size() < count; i += 4) {
            const int v = (six(i) << 18) | (six(i + 1) << 12) | (six(i + 2) << 6) | six(i + 3);
            bytes.append(char(v >> 16)).append(char(v >> 8)).append(char(v));
        }
        out += bytes.left(count);
    }
    return out;
}

qint64 folderBytes(const QString& root)
{
    qint64 n = 0;
    QDirIterator it(root, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        n += it.fileInfo().size();
    }
    return n;
}

QStringList listTop(const QString& dir)
{
    return QDir(dir).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDir::Name);
}

// Every file below `root` -> bytes (relative paths).
QMap<QString, QByteArray> snapshot(const QString& root)
{
    QMap<QString, QByteArray> out;
    QDirIterator it(root, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    const QDir r(root);
    while (it.hasNext()) {
        it.next();
        out.insert(r.relativeFilePath(it.filePath()), readAll(it.filePath()));
    }
    return out;
}

// Every folder below `root` (relative paths).
QStringList foldersOf(const QString& root)
{
    QStringList out;
    QDirIterator it(root, QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden, QDirIterator::Subdirectories);
    const QDir r(root);
    while (it.hasNext()) {
        it.next();
        out << r.relativeFilePath(it.filePath());
    }
    out.sort();
    return out;
}

// The game's files read back out of a packed folder (packs decoded).
QMap<QString, QByteArray> unpackedContents(const QString& packed)
{
    QMap<QString, QByteArray> out;
    auto base = ampr::openFolderGame(packed, CancellationToken());
    if (!base.ok())
        return out;
    auto logical = ampr::logicalGame(base.value(), CancellationToken());
    if (!logical.ok())
        return out;
    ampr::GameFiles& g = *logical.value();
    for (int i = 0; i < int(g.items().size()); ++i) {
        const auto& it = g.items()[size_t(i)];
        if (it.isDir)
            continue;
        QByteArray b(qsizetype(it.size), '\0');
        qint64 done = 0;
        while (done < it.size) {
            auto n = g.read(i, done, b.data() + done, it.size - done);
            if (!n.ok() || *n <= 0)
                break;
            done += *n;
        }
        out.insert(it.path, b);
    }
    return out;
}

// A registry with every module these jobs need, and a runner that keeps the
// job's temporary folder until the test has looked at it.
struct Env {
    std::shared_ptr<SettingsSchema> schema = std::make_shared<SettingsSchema>();
    DependencyRegistry deps;
    JobFactory jobs;
    QVariantMap defaults;
    Env()
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
        defaults.insert(exfat::keys::kSpareSpace, QStringLiteral("fixed"));
        defaults.insert(exfat::keys::kSpareSpaceMiB, 4);
        defaults.insert(general::keys::kFreeSpaceMarginMiB, 16);
        defaults.insert(ampr::keys::kZstdLevel, 3);
    }
    SettingsValues settings(const QVariantMap& overrides = {}) const
    {
        QVariantMap merged = defaults;
        for (auto it = overrides.cbegin(); it != overrides.cend(); ++it)
            merged.insert(it.key(), schema->normalize(it.key(), it.value()).value());
        return SettingsValues(schema, {}, merged);
    }
    struct Run {
        Result<JobOutcome> result = Error(ErrorCode::Internal, QStringLiteral("not run"));
        QStringList log;
        qint64 peakTempBytes = 0;
        qint64 tempBytesAfter = 0;
        QStringList tempFilesAfter; // what the job left in its temporary folder (TempDir's own marker aside)
        QString log1() const { return log.join(u'\n'); }
    };
    Run run(const QString& kind, const QJsonObject& params, CancellationSource* cancel = nullptr,
            std::function<void(const ProgressSnapshot&)> sink = {}, const QVariantMap& overrides = {}) const
    {
        Run out;
        auto job = jobs.create(kind, params, overrides);
        if (!job.ok()) {
            out.result = job.takeError();
            return out;
        }
        const SettingsValues s = settings(overrides);
        if (auto v = job.value()->validate(s); !v.ok()) {
            out.result = v.takeError();
            return out;
        }
        TempDir temp(QStringLiteral("stream-more"));
        int calls = 0;
        ProgressReporter reporter(
            [&](const ProgressSnapshot& p) {
                // How much the job keeps in its temporary folder while it works.
                if ((calls++ % 4) == 0)
                    out.peakTempBytes = std::max(out.peakTempBytes, folderBytes(temp.path()));
                if (sink)
                    sink(p);
            },
            0);
        reporter.setPhases(job.value()->phases(), job.value()->phaseWeights(s));
        static std::atomic<int> runs{0};
        const QString jobId = QStringLiteral("stream-more-%1").arg(++runs);
        std::mutex logMutex;
        auto sinkLog = std::make_shared<CallbackSink>([&](const LogRecord& r) {
            if (r.jobId != jobId)
                return;
            std::lock_guard lock(logMutex);
            out.log << r.message;
        });
        Logger::instance().addSink(sinkLog);
        JobContext ctx;
        ctx.jobId = jobId;
        ctx.token = cancel ? cancel->token() : CancellationToken();
        ctx.progress = &reporter;
        ctx.log = Log(kind, jobId);
        ctx.settings = s;
        ctx.tempDir = temp.path();
        out.result = job.value()->run(ctx);
        Logger::instance().removeSink(sinkLog);
        out.tempBytesAfter = folderBytes(temp.path());
        QDirIterator left(temp.path(), QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
        while (left.hasNext()) {
            left.next();
            if (left.fileName() != TempDir::ownerMarkerName())
                out.tempFilesAfter << QDir(temp.path()).relativeFilePath(left.filePath());
        }
        out.peakTempBytes = std::max(out.peakTempBytes, out.tempBytesAfter);
        return out;
    }
};

QString why(const Status& s) { return s.ok() ? QString() : s.error().toLogString(); }
template <typename T>
QString why(const Result<T>& r)
{
    return r.ok() ? QString() : r.error().toLogString();
}

} // namespace

class TstArchiveStreamMore : public QObject {
    Q_OBJECT
    QTemporaryDir m_root;
    int m_serial = 0;
    Env m_env;

    QString fresh(const QString& name)
    {
        const QString dir = m_root.filePath(QStringLiteral("t%1").arg(++m_serial));
        QDir().mkpath(dir);
        return QDir(dir).filePath(name);
    }

    QString sample(const QString& name)
    {
        const QByteArray t = readAll(QStringLiteral(FW_LIBARCHIVE_TEST_DATA "/") + name + QStringLiteral(".uu"));
        const QString out = fresh(name);
        return !t.isEmpty() && writeAll(out, uudecode(t)) ? out : QString();
    }

    QString makeArchive(const QString& kind, const QString& prefix = {}, QList<Item> items = {})
    {
        if (items.isEmpty())
            items = game(prefix);
        if (kind == u"zip-deflate" || kind == u"zip-store") {
            const QString p = fresh(QStringLiteral("PPSA01234.zip"));
            return writeZip(p, items, kind == u"zip-store" ? zip::Method::Store : zip::Method::Deflate) ? p : QString();
        }
        const QString p = fresh(QStringLiteral("PPSA01234.7z"));
        const QByteArray method = kind.mid(3).toLatin1(); // "7z-lzma2" -> "lzma2"
        return write7z(p, items, method.constData()) ? p : QString();
    }

    // ---- PFS at engine level ---------------------------------------------------------------

    // The image of `fmt` streamed from the archive's game, verified in full.
    QString pfsStreamed(const fwa::GameListing& g, pfs::OutputFormat fmt, const SettingsValues& s)
    {
        auto plan = pfs::planArchive(g, pfs::planOptionsFrom(s, kBuildTime), CancellationToken());
        if (!plan.ok())
            return QStringLiteral("!") + why(plan);
        pfs::BuildOptions bo = pfs::buildOptionsFrom(s, kBuildTime);
        bo.threads = 3;
        const QString image = fresh(fmt == pfs::OutputFormat::FolderImage ? QStringLiteral("s.ffpfs") : QStringLiteral("s.ffpfsc"));
        if (fmt == pfs::OutputFormat::FolderImage) {
            auto w = pfs::writeFolderImageFromArchive(plan.value(), g, bo, image, CancellationToken());
            if (!w.ok())
                return QStringLiteral("!") + why(w);
        } else {
            pfs::BuildInput in;
            in.plan = plan.value();
            auto w = pfs::writeImage(in, fmt, bo, image, CancellationToken());
            if (!w.ok())
                return QStringLiteral("!") + why(w);
        }
        pfs::VerifyExpectation ex;
        ex.kind = pfs::VerifyExpectation::Kind::Folder;
        ex.format = fmt;
        ex.plan = &plan.value();
        auto rep = pfs::verifyImage(image, ex, pfs::VerifyDepth::Full, CancellationToken());
        if (!rep.ok() || !rep->passed)
            return QStringLiteral("!verify: ") + (rep.ok() ? rep->summary : why(rep));
        return image;
    }

    QString pfsFromFolder(const QString& root, pfs::OutputFormat fmt, const SettingsValues& s)
    {
        auto plan = pfs::planFolder(root, pfs::planOptionsFrom(s, kBuildTime), CancellationToken());
        if (!plan.ok())
            return QStringLiteral("!") + why(plan);
        pfs::BuildOptions bo = pfs::buildOptionsFrom(s, kBuildTime);
        bo.threads = 2;
        pfs::BuildInput in;
        in.plan = plan.value();
        const QString image = fresh(QStringLiteral("f.img"));
        auto w = pfs::writeImage(in, fmt, bo, image, CancellationToken());
        return w.ok() ? image : QStringLiteral("!") + why(w);
    }

    // ---- zstd packs at engine level ---------------------------------------------------------

    ampr::PackGameOptions packOptions() const
    {
        ampr::PackGameOptions o;
        o.level = 3;
        o.threads = 3;
        o.verify = true;
        o.archiveId = kArchiveId;
        o.freeSpaceMarginBytes = 0;
        return o;
    }

    // The packed folder made in one pass through the archive; `spoolDir` is
    // left empty again.
    QString packedStreamed(const fwa::GameListing& g, const QString& spoolDir, ampr::PackGameReport* report = nullptr)
    {
        auto source = std::make_shared<ampr::ArchiveGame>(g, kBuildTime);
        const QString out = fresh(QStringLiteral("streamed-packed"));
        auto r = ampr::packGameFromArchive(source, out, packOptions(), spoolDir, CancellationToken(), nullptr, Log(QStringLiteral("t")));
        if (!r.ok())
            return QStringLiteral("!") + why(r);
        if (report)
            *report = r.value();
        return out;
    }

    QString packedFromFolder(const QString& folder)
    {
        auto files = ampr::openFolderGame(folder, CancellationToken());
        if (!files.ok())
            return QStringLiteral("!") + why(files);
        const QString out = fresh(QStringLiteral("folder-packed"));
        auto r = ampr::packGame(files.value(), out, packOptions(), CancellationToken(), nullptr, Log(QStringLiteral("t")));
        return r.ok() ? out : QStringLiteral("!") + why(r);
    }

    // Two packed folders hold the same files with the same bytes; their
    // ampr_emu.index may differ only in the times (the runtime's row carries
    // the time each build wrote it).
    static QString samePackedFolder(const QString& a, const QString& b)
    {
        QMap<QString, QByteArray> sa = snapshot(a), sb = snapshot(b);
        if (sa.keys() != sb.keys())
            return QStringLiteral("files: %1 / %2").arg(sa.keys().join(u','), sb.keys().join(u','));
        for (auto it = sa.cbegin(); it != sa.cend(); ++it) {
            const QByteArray& other = sb.value(it.key());
            if (it.value() == other)
                continue;
            if (it.key() == pfs::kAmprIndexName && pfs::amprIndexesDifferOnlyInTimes(it.value(), other))
                continue;
            return QStringLiteral("%1 differs").arg(it.key());
        }
        if (foldersOf(a) != foldersOf(b))
            return QStringLiteral("folders: %1 / %2").arg(foldersOf(a).join(u','), foldersOf(b).join(u','));
        return {};
    }

    static QString failed(const QString& out) { return out.startsWith(u'!') ? out.mid(1) : QString(); }

private slots:
    void initTestCase()
    {
        QVERIFY(m_root.isValid());
        fwtest::isolateAppData(m_root);
        QVERIFY2(ampr::bundledRuntimeProblem().isEmpty(), qPrintable(ampr::bundledRuntimeProblem()));
    }

    // ---- ZIP entries read on demand ---------------------------------------------------------

    void zipEntryReaderReadsInAnyPieces()
    {
        const QString a = makeArchive(QStringLiteral("zip-deflate"));
        zip::Reader r;
        QVERIFY(r.open(a).ok());
        const zip::Entry* e = r.find(QStringLiteral("data/deep/deeper/deepest/text.txt"));
        QVERIFY(e);
        zip::EntryReader in;
        QVERIFY(in.open(a, *e).ok());
        QByteArray got;
        QByteArray buf(70000, '\0');
        for (int step = 1;; step = step * 3 % 65521 + 1) {
            auto n = in.read(buf.data(), std::min<qint64>(step, buf.size()));
            QVERIFY2(n.ok(), qPrintable(why(n)));
            if (*n == 0)
                break;
            got.append(buf.constData(), qsizetype(*n));
        }
        QCOMPARE(got, text(30000));
        // One byte changed in the stored (deflated) data: a checksum or stream error, never the data.
        QByteArray bytes = readAll(a);
        const qsizetype lh = qsizetype(e->localHeaderOffset);
        const qsizetype at = lh + 30 + qFromLittleEndian<quint16>(bytes.constData() + lh + 26) + qFromLittleEndian<quint16>(bytes.constData() + lh + 28) + 40;
        bytes[at] = char(bytes[at] ^ 0x21);
        QVERIFY(writeAll(a, bytes));
        zip::EntryReader bad;
        QVERIFY(bad.open(a, *e).ok());
        Status st = success();
        for (;;) {
            auto n = bad.read(buf.data(), buf.size());
            if (!n.ok()) {
                st = n.error();
                break;
            }
            if (*n == 0)
                break;
        }
        QVERIFY2(!st.ok(), "a damaged entry read without an error");
        QCOMPARE(st.error().code, ErrorCode::CorruptData);
    }

    // ---- PFS: byte for byte the unpacked folder's image ------------------------------------

    void pfsStreamedMatchesUnpackedFolder_data()
    {
        QTest::addColumn<QString>("kind");
        QTest::addColumn<QString>("prefix");
        QTest::addColumn<int>("format"); // 0 .ffpfs, 1 .ffpfsc
        for (const char* k : {"zip-deflate", "zip-store", "7z-lzma2", "7z-store", "7z-ppmd"})
            for (const QString& prefix : {QString(), QStringLiteral("PPSA01234-app0/")}) {
                QTest::newRow(qPrintable(QStringLiteral("ffpfs %1 %2").arg(QLatin1String(k), prefix.isEmpty() ? QStringLiteral("contents") : QStringLiteral("folder"))))
                    << QString::fromLatin1(k) << prefix << 0;
                if (QByteArray(k).startsWith("zip"))
                    QTest::newRow(qPrintable(QStringLiteral("ffpfsc %1 %2").arg(QLatin1String(k), prefix.isEmpty() ? QStringLiteral("contents") : QStringLiteral("folder"))))
                        << QString::fromLatin1(k) << prefix << 1;
            }
    }

    void pfsStreamedMatchesUnpackedFolder()
    {
        QFETCH(QString, kind);
        QFETCH(QString, prefix);
        QFETCH(int, format);
        const pfs::OutputFormat fmt = format == 0 ? pfs::OutputFormat::FolderImage : pfs::OutputFormat::Container;
        const QString a = makeArchive(kind, prefix);
        QVERIFY(!a.isEmpty());
        auto g = fwa::listArchiveGame(a);
        QVERIFY2(g.ok(), qPrintable(why(g)));
        QCOMPARE(g->bytes, bytesOf(game()));
        auto root = fwa::unpackArchiveSource(a, fresh(QStringLiteral("temp")), CancellationToken(), nullptr, 0);
        QVERIFY2(root.ok(), qPrintable(why(root)));
        for (const QVariantMap& o : {QVariantMap(),
                                     // exclusions (a pattern, a folder) and no empty-folder markers
                                     QVariantMap{{pfs::keys::kExcludes, QStringList{QStringLiteral("*.bak"), QStringLiteral("deep")}},
                                                 {pfs::keys::kKeepEmptyFolders, false}},
                                     // 16 KiB blocks
                                     QVariantMap{{pfs::keys::kBlockSize, QStringLiteral("16384")}}}) {
            const SettingsValues s = m_env.settings(o);
            const QString streamed = pfsStreamed(g.value(), fmt, s);
            QVERIFY2(failed(streamed).isEmpty(), qPrintable(failed(streamed)));
            const QString folderImage = pfsFromFolder(root.value(), fmt, s);
            QVERIFY2(failed(folderImage).isEmpty(), qPrintable(failed(folderImage)));
            QCOMPARE(QFileInfo(streamed).size(), QFileInfo(folderImage).size());
            QCOMPARE(sha(streamed), sha(folderImage));
        }
    }

    // libarchive's RAR samples (no game in them: the whole archive is built).
    // Names outside ASCII are refused by PFS whether streamed or unpacked.
    void pfsRarStreamedMatchesUnpackedFolder_data()
    {
        QTest::addColumn<QString>("name");
        for (const char* n : {"test_read_format_rar5_multiple_files.rar", "test_read_format_rar5_multiple_files_solid.rar",
                              "test_read_format_rar5_solid.rar", "test_read_format_rar5_stored.rar",
                              "test_read_format_rar5_compressed.rar", "test_read_format_rar5_win32.rar",
                              "test_read_format_rar5_unicode.rar", "test_read_format_rar5_blake2.rar",
                              "test_read_format_rar_compress_normal.rar", "test_read_format_rar_unicode.rar"})
            QTest::newRow(n) << QString::fromLatin1(n);
    }

    void pfsRarStreamedMatchesUnpackedFolder()
    {
        QFETCH(QString, name);
        const QString a = sample(name);
        QVERIFY2(!a.isEmpty(), "the sample is not in the libarchive sources");
        auto g = fwa::listWholeArchiveForTesting(a);
        QVERIFY2(g.ok(), qPrintable(why(g)));
        auto listing = fwa::listArchive(a);
        QVERIFY(listing.ok());
        const QString dest = fresh(QStringLiteral("unpacked"));
        auto x = fwa::extractArchive(listing.value(), dest, {}, CancellationToken());
        QVERIFY2(x.ok(), qPrintable(why(x)));
        const SettingsValues s = m_env.settings();
        auto folderPlan = pfs::planFolder(dest, pfs::planOptionsFrom(s, kBuildTime), CancellationToken());
        auto archivePlan = pfs::planArchive(g.value(), pfs::planOptionsFrom(s, kBuildTime), CancellationToken());
        QCOMPARE(archivePlan.ok(), folderPlan.ok());
        if (!folderPlan.ok()) {
            QCOMPARE(archivePlan.error().code, folderPlan.error().code); // names outside ASCII, both ways
            return;
        }
        const QString streamed = pfsStreamed(g.value(), pfs::OutputFormat::FolderImage, s);
        QVERIFY2(failed(streamed).isEmpty(), qPrintable(failed(streamed)));
        const QString folderImage = pfsFromFolder(dest, pfs::OutputFormat::FolderImage, s);
        QVERIFY2(failed(folderImage).isEmpty(), qPrintable(failed(folderImage)));
        QCOMPARE(sha(streamed), sha(folderImage));
    }

    // The archive's order is not the image's: the result does not change.
    void pfsEntryOrderDoesNotMatter()
    {
        QList<Item> reversed = game();
        std::reverse(reversed.begin(), reversed.end());
        const QString a1 = makeArchive(QStringLiteral("7z-lzma2")), a2 = makeArchive(QStringLiteral("7z-lzma2"), {}, reversed);
        auto g1 = fwa::listArchiveGame(a1), g2 = fwa::listArchiveGame(a2);
        QVERIFY(g1.ok() && g2.ok());
        const SettingsValues s = m_env.settings();
        const QString i1 = pfsStreamed(g1.value(), pfs::OutputFormat::FolderImage, s);
        const QString i2 = pfsStreamed(g2.value(), pfs::OutputFormat::FolderImage, s);
        QVERIFY2(failed(i1).isEmpty() && failed(i2).isEmpty(), qPrintable(failed(i1) + failed(i2)));
        QCOMPARE(sha(i1), sha(i2));
        const QString z1 = makeArchive(QStringLiteral("zip-deflate")), z2 = makeArchive(QStringLiteral("zip-deflate"), {}, reversed);
        auto g3 = fwa::listArchiveGame(z1), g4 = fwa::listArchiveGame(z2);
        QVERIFY(g3.ok() && g4.ok());
        const QString c1 = pfsStreamed(g3.value(), pfs::OutputFormat::Container, s);
        const QString c2 = pfsStreamed(g4.value(), pfs::OutputFormat::Container, s);
        QVERIFY2(failed(c1).isEmpty() && failed(c2).isEmpty(), qPrintable(failed(c1) + failed(c2)));
        QCOMPARE(sha(c1), sha(c2));
    }

    // The read-back verification of a streamed .ffpfs compares each file
    // with the digest taken while it was written: one changed byte is found.
    void pfsVerificationFindsAChangedByte()
    {
        const QString a = makeArchive(QStringLiteral("7z-lzma2"));
        auto g = fwa::listArchiveGame(a);
        QVERIFY(g.ok());
        const SettingsValues s = m_env.settings();
        auto plan = pfs::planArchive(g.value(), pfs::planOptionsFrom(s, kBuildTime), CancellationToken());
        QVERIFY(plan.ok());
        const QString image = fresh(QStringLiteral("v.ffpfs"));
        QVERIFY(pfs::writeFolderImageFromArchive(plan.value(), g.value(), pfs::buildOptionsFrom(s, kBuildTime), image, CancellationToken()).ok());
        pfs::VerifyExpectation ex;
        ex.kind = pfs::VerifyExpectation::Kind::Folder;
        ex.format = pfs::OutputFormat::FolderImage;
        ex.plan = &plan.value();
        auto good = pfs::verifyImage(image, ex, pfs::VerifyDepth::Full, CancellationToken());
        QVERIFY(good.ok() && good->passed);
        QByteArray bytes = readAll(image);
        const qsizetype at = bytes.indexOf(noise(5 * 1024 * 1024 + 17, 4).mid(1000, 64));
        QVERIFY(at > 0);
        bytes[at] = char(bytes[at] ^ 1);
        QVERIFY(writeAll(image, bytes));
        auto bad = pfs::verifyImage(image, ex, pfs::VerifyDepth::Full, CancellationToken());
        QVERIFY(bad.ok());
        QVERIFY(!bad->passed);
        QCOMPARE(bad->problemCount, qint64(1));
        QVERIFY2(bad->problems.value(0).contains(QStringLiteral("data/big.bin")), qPrintable(bad->problems.value(0)));
    }

    // ---- PFS jobs --------------------------------------------------------------------------

    void pfsJobsStreamOrUnpack_data()
    {
        QTest::addColumn<QString>("archive");
        QTest::addColumn<QString>("format");
        QTest::addColumn<bool>("compressFiles");
        QTest::addColumn<bool>("streams");
        QTest::newRow("ffpfs zip") << "zip-deflate" << "ffpfs" << false << true;
        QTest::newRow("ffpfs 7z") << "7z-lzma2" << "ffpfs" << false << true;
        QTest::newRow("ffpfsc zip") << "zip-deflate" << "ffpfsc" << false << true;
        QTest::newRow("ffpfsc 7z (unpacked first)") << "7z-lzma2" << "ffpfsc" << false << false;
        QTest::newRow("ffpfs zip, per-file compression (unpacked first)") << "zip-deflate" << "ffpfs" << true << false;
    }

    void pfsJobsStreamOrUnpack()
    {
        QFETCH(QString, archive);
        QFETCH(QString, format);
        QFETCH(bool, compressFiles);
        QFETCH(bool, streams);
        const QString a = makeArchive(archive, QStringLiteral("PPSA01234-app0/"));
        QVERIFY(!a.isEmpty());
        const QByteArray before = sha(a);
        const QString outDir = QFileInfo(fresh(QStringLiteral("out/x"))).absolutePath();
        const QString out = QDir(outDir).filePath(QStringLiteral("game.") + format);
        const QVariantMap o{{pfs::keys::kCompressFiles, compressFiles}};
        Env::Run r = m_env.run(pfs::kinds::kBuild, {{QStringLiteral("source"), a}, {QStringLiteral("output"), out},
                                                    {QStringLiteral("format"), format}, {QStringLiteral("timestamp"), kBuildTime}},
                               nullptr, {}, o);
        QVERIFY2(r.result.ok(), qPrintable(why(r.result) + u'\n' + r.log1()));
        QVERIFY2(r.result->verification == Verification::Passed, qPrintable(r.result->verificationDetail));
        QCOMPARE(listTop(outDir), QStringList{QFileInfo(out).fileName()});
        QCOMPARE(sha(a), before);
        QCOMPARE(QFileInfo::exists(a), true); // never deleted
        if (streams) {
            QVERIFY2(r.log1().contains(QStringLiteral("Building straight from")), qPrintable(r.log1()));
            QVERIFY2(!r.log1().contains(QStringLiteral("Unpacked")), qPrintable(r.log1()));
            // The game is about 8 MiB; nothing of it went to the temporary folder.
            QVERIFY2(r.peakTempBytes < 64 * 1024, qPrintable(QString::number(r.peakTempBytes)));
        } else {
            QVERIFY2(r.log1().contains(QStringLiteral("Unpacking")) && r.log1().contains(QStringLiteral("first:")), qPrintable(r.log1()));
            QVERIFY2(r.peakTempBytes >= bytesOf(game()) / 2, qPrintable(QString::number(r.peakTempBytes)));
        }
        // The same image as the job builds from the unpacked folder.
        auto root = fwa::unpackArchiveSource(a, fresh(QStringLiteral("temp")), CancellationToken(), nullptr, 0);
        QVERIFY(root.ok());
        const QString out2 = QDir(QFileInfo(fresh(QStringLiteral("out2/x"))).absolutePath()).filePath(QStringLiteral("game.") + format);
        Env::Run f = m_env.run(pfs::kinds::kBuild, {{QStringLiteral("source"), root.value()}, {QStringLiteral("output"), out2},
                                                    {QStringLiteral("format"), format}, {QStringLiteral("timestamp"), kBuildTime}},
                               nullptr, {}, o);
        QVERIFY2(f.result.ok(), qPrintable(why(f.result)));
        QCOMPARE(sha(out), sha(out2));
    }

    // A damaged entry fails the build; there is no image and no .fw.part.
    void pfsDamagedEntryLeavesNoImage_data()
    {
        QTest::addColumn<QString>("archive");
        QTest::addColumn<QString>("format");
        for (const char* k : {"zip-store", "zip-deflate", "7z-lzma2", "7z-store"})
            QTest::newRow(qPrintable(QStringLiteral("ffpfs %1").arg(QLatin1String(k)))) << QString::fromLatin1(k) << QStringLiteral("ffpfs");
        for (const char* k : {"zip-store", "zip-deflate"})
            QTest::newRow(qPrintable(QStringLiteral("ffpfsc %1").arg(QLatin1String(k)))) << QString::fromLatin1(k) << QStringLiteral("ffpfsc");
    }

    void pfsDamagedEntryLeavesNoImage()
    {
        QFETCH(QString, archive);
        QFETCH(QString, format);
        const QString a = makeArchive(archive);
        QVERIFY(!a.isEmpty());
        QByteArray bytes = readAll(a);
        const qsizetype at = archive.endsWith(u"store") ? bytes.indexOf(noise(5 * 1024 * 1024 + 17, 4).mid(3000000, 64)) : bytes.size() / 2;
        QVERIFY(at > 0);
        bytes[at + 10] = char(bytes[at + 10] ^ 0x5A);
        QVERIFY(writeAll(a, bytes));
        const QString outDir = QFileInfo(fresh(QStringLiteral("x"))).absolutePath();
        Env::Run r = m_env.run(pfs::kinds::kBuild, {{QStringLiteral("source"), a},
                                                    {QStringLiteral("output"), QDir(outDir).filePath(QStringLiteral("game.") + format)},
                                                    {QStringLiteral("format"), format}});
        QVERIFY2(!r.result.ok(), "a damaged archive was built");
        QCOMPARE(r.result.error().code, ErrorCode::CorruptData);
        QCOMPARE(listTop(outDir), QStringList());
    }

    // The list says one size, the data another: the pass stops.
    void pfsEntryLargerThanListedFails()
    {
        const QString a = makeArchive(QStringLiteral("zip-store"));
        QByteArray bytes = readAll(a);
        const QByteArray name = QByteArrayLiteral("data/big.bin");
        qsizetype cd = -1;
        for (qsizetype i = bytes.indexOf(QByteArray("PK\x01\x02", 4)); i >= 0; i = bytes.indexOf(QByteArray("PK\x01\x02", 4), i + 4))
            if (bytes.mid(i + 46, name.size()) == name)
                cd = i;
        QVERIFY(cd > 0);
        const quint32 size = qFromLittleEndian<quint32>(bytes.constData() + cd + 24);
        qToLittleEndian<quint32>(size - 1, bytes.data() + cd + 24);
        QVERIFY(writeAll(a, bytes));
        for (const QString& format : {QStringLiteral("ffpfs"), QStringLiteral("ffpfsc")}) {
            const QString outDir = QFileInfo(fresh(QStringLiteral("x"))).absolutePath();
            Env::Run r = m_env.run(pfs::kinds::kBuild, {{QStringLiteral("source"), a},
                                                        {QStringLiteral("output"), QDir(outDir).filePath(QStringLiteral("g.") + format)},
                                                        {QStringLiteral("format"), format}});
            QVERIFY2(!r.result.ok(), qPrintable(QStringLiteral("built %1 from an archive whose list does not match its data").arg(format)));
            QCOMPARE(r.result.error().code, ErrorCode::CorruptData);
            QCOMPARE(listTop(outDir), QStringList());
        }
    }

    // Cancel in the middle of the pass: no image, no .fw.part, an older image kept.
    void pfsCancelMidStreamLeavesNoImage_data()
    {
        QTest::addColumn<QString>("archive");
        QTest::addColumn<QString>("format");
        QTest::newRow("ffpfs zip") << "zip-deflate" << "ffpfs";
        QTest::newRow("ffpfs 7z") << "7z-lzma2" << "ffpfs";
        QTest::newRow("ffpfsc zip") << "zip-deflate" << "ffpfsc";
    }

    void pfsCancelMidStreamLeavesNoImage()
    {
        QFETCH(QString, archive);
        QFETCH(QString, format);
        const QString a = makeArchive(archive);
        const QString outDir = QFileInfo(fresh(QStringLiteral("x"))).absolutePath();
        const QString out = QDir(outDir).filePath(QStringLiteral("game.") + format);
        QVERIFY(writeAll(out, "PREVIOUS IMAGE"));
        CancellationSource cancel;
        std::atomic<qint64> seen{0};
        Env::Run r = m_env.run(pfs::kinds::kBuild,
                               {{QStringLiteral("source"), a}, {QStringLiteral("output"), out}, {QStringLiteral("format"), format},
                                {QStringLiteral("replaceExisting"), true}},
                               &cancel, [&](const ProgressSnapshot& s) {
                                   if (s.phaseIndex == 1 && s.done > 1024 * 1024) {
                                       seen = s.done;
                                       cancel.cancel();
                                   }
                               });
        QVERIFY(!r.result.ok());
        QVERIFY2(r.result.error().isCancelled() || cancel.token().isCancelled(), qPrintable(why(r.result)));
        QVERIFY(seen > 0);
        QCOMPARE(listTop(outDir), QStringList{QFileInfo(out).fileName()});
        QCOMPARE(readAll(out), QByteArray("PREVIOUS IMAGE"));
    }

    // ---- zstd packs at engine level -------------------------------------------------------

    void packedStreamedMatchesUnpackedFolder_data()
    {
        QTest::addColumn<QString>("kind");
        QTest::addColumn<QString>("prefix");
        for (const char* k : {"zip-deflate", "zip-store", "7z-lzma2", "7z-store"}) {
            QTest::newRow(qPrintable(QStringLiteral("%1 contents").arg(QLatin1String(k)))) << QString::fromLatin1(k) << QString();
            QTest::newRow(qPrintable(QStringLiteral("%1 folder").arg(QLatin1String(k)))) << QString::fromLatin1(k)
                                                                                         << QStringLiteral("PPSA01234-app0/");
        }
    }

    void packedStreamedMatchesUnpackedFolder()
    {
        QFETCH(QString, kind);
        QFETCH(QString, prefix);
        QList<Item> items = game(prefix);
        // A name outside ASCII is no problem for the packs.
        items << file(prefix + QStringLiteral("data/データ — ü.bin"), noise(300000, 3));
        const QString a = makeArchive(kind, prefix, items);
        QVERIFY(!a.isEmpty());
        auto g = fwa::listArchiveGame(a);
        QVERIFY2(g.ok(), qPrintable(why(g)));
        const QString spool = fresh(QStringLiteral("spool"));
        QDir().mkpath(spool);
        ampr::PackGameReport rep;
        const QString streamed = packedStreamed(g.value(), spool, &rep);
        QVERIFY2(failed(streamed).isEmpty(), qPrintable(failed(streamed)));
        QCOMPARE(listTop(spool), QStringList()); // the spool is gone
        QVERIFY(rep.packedFiles >= 5);
        QCOMPARE(rep.linkedFiles, qint64(0));
        auto root = fwa::unpackArchiveSource(a, fresh(QStringLiteral("temp")), CancellationToken(), nullptr, 0);
        QVERIFY2(root.ok(), qPrintable(why(root)));
        const QString folderPacked = packedFromFolder(root.value());
        QVERIFY2(failed(folderPacked).isEmpty(), qPrintable(failed(folderPacked)));
        const QString diff = samePackedFolder(streamed, folderPacked);
        QVERIFY2(diff.isEmpty(), qPrintable(diff));
        // The packs are the pack engine's: every file of the game reads back from them.
        const QMap<QString, QByteArray> back = unpackedContents(streamed);
        for (const Item& i : items)
            if (!i.folder && !i.name.endsWith(QStringLiteral("libSceAmpr.sprx")))
                QCOMPARE(back.value(i.name.mid(prefix.size())), i.data);
    }

    // libarchive's RAR samples, as for PFS (any names: the packs take them).
    void packedRarStreamedMatchesUnpackedFolder_data() { pfsRarStreamedMatchesUnpackedFolder_data(); }

    void packedRarStreamedMatchesUnpackedFolder()
    {
        QFETCH(QString, name);
        const QString a = sample(name);
        QVERIFY2(!a.isEmpty(), "the sample is not in the libarchive sources");
        auto g = fwa::listWholeArchiveForTesting(a);
        QVERIFY2(g.ok(), qPrintable(why(g)));
        auto listing = fwa::listArchive(a);
        QVERIFY(listing.ok());
        const QString dest = fresh(QStringLiteral("unpacked"));
        QVERIFY(fwa::extractArchive(listing.value(), dest, {}, CancellationToken()).ok());
        const QString spool = fresh(QStringLiteral("spool"));
        QDir().mkpath(spool);
        const QString streamed = packedStreamed(g.value(), spool);
        const QString folderPacked = packedFromFolder(dest);
        // A sample that is all loose files has nothing to pack: refused alike.
        if (!failed(folderPacked).isEmpty()) {
            QVERIFY2(!failed(streamed).isEmpty(), qPrintable(failed(folderPacked)));
            return;
        }
        QVERIFY2(failed(streamed).isEmpty(), qPrintable(failed(streamed)));
        QCOMPARE(listTop(spool), QStringList());
        const QString diff = samePackedFolder(streamed, folderPacked);
        QVERIFY2(diff.isEmpty(), qPrintable(diff));
    }

    // A 7z in reverse order of the packs and the image: the same packs.
    void packedEntryOrderDoesNotMatter()
    {
        QList<Item> reversed = game();
        std::reverse(reversed.begin(), reversed.end());
        const QString a1 = makeArchive(QStringLiteral("7z-lzma2")), a2 = makeArchive(QStringLiteral("7z-lzma2"), {}, reversed);
        auto g1 = fwa::listArchiveGame(a1), g2 = fwa::listArchiveGame(a2);
        QVERIFY(g1.ok() && g2.ok());
        const QString spool = fresh(QStringLiteral("spool"));
        QDir().mkpath(spool);
        const QString p1 = packedStreamed(g1.value(), spool), p2 = packedStreamed(g2.value(), spool);
        QVERIFY2(failed(p1).isEmpty() && failed(p2).isEmpty(), qPrintable(failed(p1) + failed(p2)));
        const QString diff = samePackedFolder(p1, p2);
        QVERIFY2(diff.isEmpty(), qPrintable(diff));
    }

    // The pack stream on its own: files pushed in record order (direct) or
    // in any order through a spool give writeZstdPack()'s bytes.
    void packStreamMatchesWriteZstdPack()
    {
        std::vector<QByteArray> data;
        std::vector<ampr::PackInput> inputs;
        for (int i = 0; i < 9; ++i) {
            data.push_back(i % 2 ? noise(300000 + i * 77777, quint32(i)) : text(9000 + i * 3000));
            ampr::PackInput in;
            in.path = QByteArray("dir") + QByteArray::number(i % 3) + "/file" + QByteArray::number(i) + ".bin";
            in.size = data.back().size();
            in.open = [&data, i]() -> Result<std::unique_ptr<ampr::SequentialReader>> {
                struct Mem final : ampr::SequentialReader {
                    QByteArray d;
                    qint64 pos = 0;
                    Result<qint64> read(char* dst, qint64 max) override
                    {
                        const qint64 n = std::min<qint64>(max, d.size() - pos);
                        std::memcpy(dst, d.constData() + pos, size_t(n));
                        pos += n;
                        return n;
                    }
                };
                auto m = std::make_unique<Mem>();
                m->d = data[size_t(i)];
                return std::unique_ptr<ampr::SequentialReader>(std::move(m));
            };
            inputs.push_back(in);
        }
        ampr::ZstdPackOptions zo;
        zo.level = 3;
        zo.threads = 2;
        zo.archiveId = kArchiveId;
        zo.volumeLimit = 64 + 3 * 262144; // several volumes
        const QString ref = fresh(QStringLiteral("ref"));
        QDir().mkpath(ref);
        auto written = ampr::writeZstdPack(inputs, zo, ref, CancellationToken());
        QVERIFY2(written.ok(), qPrintable(why(written)));
        QVERIFY(written->volumes.size() > 2);
        for (const bool spooled : {false, true}) {
            const QString out = fresh(QStringLiteral("out"));
            QDir().mkpath(out);
            const QString spool = spooled ? fresh(QStringLiteral("chunks.spool")) : QString();
            auto stream = ampr::ZstdPackStream::create(inputs, zo, out, spool);
            QVERIFY2(stream.ok(), qPrintable(why(stream)));
            std::vector<int> order = (*stream)->recordOrder();
            if (spooled)
                std::reverse(order.begin(), order.end());
            for (const int i : order) {
                QVERIFY((*stream)->begin(i).ok());
                const QByteArray& d = data[size_t(i)];
                for (qsizetype at = 0; at < d.size();) {
                    const qsizetype n = std::min<qsizetype>(d.size() - at, 1 + (at % 100003));
                    QVERIFY((*stream)->data(d.constData() + at, n).ok());
                    at += n;
                }
                QVERIFY((*stream)->end().ok());
            }
            auto done = (*stream)->finish(CancellationToken());
            QVERIFY2(done.ok(), qPrintable(why(done)));
            stream->reset();
            QCOMPARE(snapshot(out), snapshot(ref));
            if (spooled)
                QVERIFY(!QFileInfo::exists(spool));
            QVERIFY(ampr::verifyWrittenZstdPack(out, done.value(), CancellationToken()).ok());
        }
        // Direct: out of record order is refused, and nothing is left behind.
        const QString out = fresh(QStringLiteral("refused"));
        QDir().mkpath(out);
        {
            auto stream = ampr::ZstdPackStream::create(inputs, zo, out);
            QVERIFY(stream.ok());
            const std::vector<int> order = (*stream)->recordOrder();
            QVERIFY((*stream)->begin(order[1]).ok());
            QVERIFY((*stream)->data(data[size_t(order[1])].constData(), data[size_t(order[1])].size()).ok());
            QVERIFY((*stream)->end().ok());
            QVERIFY(!(*stream)->begin(order[0]).ok());
        }
        QCOMPARE(listTop(out), QStringList());
    }

    // ---- zstd jobs ------------------------------------------------------------------------

    void packJobStreams_data()
    {
        QTest::addColumn<QString>("archive");
        QTest::newRow("zip") << "zip-deflate";
        QTest::newRow("7z") << "7z-lzma2";
    }

    void packJobStreams()
    {
        QFETCH(QString, archive);
        const QString a = makeArchive(archive, QStringLiteral("PPSA01234-app0/"));
        const QByteArray before = sha(a);
        const QString outDir = QFileInfo(fresh(QStringLiteral("out/x"))).absolutePath();
        const QString out = QDir(outDir).filePath(QStringLiteral("packed"));
        Env::Run r = m_env.run(ampr::kPackJob, {{QStringLiteral("source"), a}, {QStringLiteral("output"), out}});
        QVERIFY2(r.result.ok(), qPrintable(why(r.result) + u'\n' + r.log1()));
        QCOMPARE(r.result->verification, Verification::Passed);
        QCOMPARE(listTop(outDir), QStringList{QStringLiteral("packed")});
        QCOMPARE(sha(a), before);
        QVERIFY2(r.log1().contains(QStringLiteral("Building straight from")), qPrintable(r.log1()));
        QVERIFY2(!r.log1().contains(QStringLiteral("Unpacked")), qPrintable(r.log1()));
        // From a ZIP nothing goes to the temporary folder; from a 7z the packs'
        // chunks, at most as large as the files they hold (about 8 MiB here).
        if (archive.startsWith(u"zip"))
            QVERIFY2(r.peakTempBytes < 64 * 1024, qPrintable(QString::number(r.peakTempBytes)));
        else
            QVERIFY2(r.peakTempBytes <= bytesOf(game()), qPrintable(QString::number(r.peakTempBytes)));
        QCOMPARE(r.tempFilesAfter, QStringList());
        const QMap<QString, QByteArray> back = unpackedContents(out);
        for (const Item& i : game(QStringLiteral("PPSA01234-app0/")))
            if (!i.folder && !i.name.endsWith(QStringLiteral("libSceAmpr.sprx")))
                QCOMPARE(back.value(i.name.mid(QStringLiteral("PPSA01234-app0/").size())), i.data);
    }

    // image.build.zstd (compressed exFAT, the ZSTD image, FFPKG and PFS with
    // packs) from an archive: packed in one pass, then built from the packed
    // work folder as from a folder; the image holds the game.
    void zstdImageJobStreams_data()
    {
        QTest::addColumn<QString>("archive");
        QTest::addColumn<QString>("format");
        for (const char* a : {"zip-deflate", "7z-lzma2"})
            for (const char* f : {"exfat", "ffpkg", "ffpfsc"})
                QTest::newRow(qPrintable(QStringLiteral("%1 %2").arg(QLatin1String(f), QLatin1String(a)))) << QString::fromLatin1(a)
                                                                                                         << QString::fromLatin1(f);
    }

    void zstdImageJobStreams()
    {
        QFETCH(QString, archive);
        QFETCH(QString, format);
        const QString a = makeArchive(archive, QStringLiteral("PPSA01234-app0/"));
        const QString outDir = QFileInfo(fresh(QStringLiteral("out/x"))).absolutePath();
        const QString out = QDir(outDir).filePath(QStringLiteral("game.") + format);
        Env::Run r = m_env.run(integration::kZstdBuildJob, {{QStringLiteral("source"), a}, {QStringLiteral("output"), out},
                                                            {QStringLiteral("format"), format}});
        QVERIFY2(r.result.ok(), qPrintable(why(r.result) + u'\n' + r.log1()));
        QVERIFY2(r.result->verification == Verification::Passed, qPrintable(r.result->verificationDetail));
        QCOMPARE(listTop(outDir), QStringList{QFileInfo(out).fileName()}); // the work folder is gone
        QVERIFY2(r.log1().contains(QStringLiteral("Building straight from")), qPrintable(r.log1()));
        QVERIFY2(!r.log1().contains(QStringLiteral("Unpacked")), qPrintable(r.log1()));
        if (archive.startsWith(u"zip"))
            QVERIFY2(r.peakTempBytes < 64 * 1024, qPrintable(QString::number(r.peakTempBytes)));
        else
            QVERIFY2(r.peakTempBytes <= bytesOf(game()), qPrintable(QString::number(r.peakTempBytes)));
        QCOMPARE(r.tempFilesAfter, QStringList());
        // The image holds the game: extracted with its packs unpacked.
        const QString x = fresh(QStringLiteral("extracted"));
        Env::Run e = m_env.run(integration::kGameExtractJob, {{QStringLiteral("source"), out}, {QStringLiteral("output"), x}});
        QVERIFY2(e.result.ok(), qPrintable(why(e.result)));
        const QMap<QString, QByteArray> back = snapshot(x);
        for (const Item& i : game(QStringLiteral("PPSA01234-app0/")))
            if (!i.folder && !i.name.endsWith(QStringLiteral("libSceAmpr.sprx")))
                QCOMPARE(back.value(i.name.mid(QStringLiteral("PPSA01234-app0/").size())), i.data);
    }

    // A damaged entry, and a cancel in the middle of the pass: the job fails,
    // nothing is left (no packed folder, no .part, no work folder, no spool).
    void zstdDamagedOrCancelledLeavesNothing_data()
    {
        QTest::addColumn<QString>("archive");
        QTest::addColumn<QString>("kind");
        QTest::addColumn<bool>("cancelIt");
        for (const char* a : {"zip-store", "zip-deflate", "7z-lzma2", "7z-store"})
            for (const bool c : {false, true}) {
                QTest::newRow(qPrintable(QStringLiteral("pack %1 %2").arg(QLatin1String(a), c ? QStringLiteral("cancel") : QStringLiteral("damaged"))))
                    << QString::fromLatin1(a) << ampr::kPackJob << c;
                QTest::newRow(qPrintable(QStringLiteral("image %1 %2").arg(QLatin1String(a), c ? QStringLiteral("cancel") : QStringLiteral("damaged"))))
                    << QString::fromLatin1(a) << integration::kZstdBuildJob << c;
            }
    }

    void zstdDamagedOrCancelledLeavesNothing()
    {
        QFETCH(QString, archive);
        QFETCH(QString, kind);
        QFETCH(bool, cancelIt);
        const QString a = makeArchive(archive);
        if (!cancelIt) {
            QByteArray bytes = readAll(a);
            const qsizetype at = archive.endsWith(u"store") ? bytes.indexOf(noise(5 * 1024 * 1024 + 17, 4).mid(3000000, 64)) : bytes.size() / 2;
            QVERIFY(at > 0);
            bytes[at + 10] = char(bytes[at + 10] ^ 0x5A);
            QVERIFY(writeAll(a, bytes));
        }
        const QString outDir = QFileInfo(fresh(QStringLiteral("x"))).absolutePath();
        const QString out = QDir(outDir).filePath(kind == ampr::kPackJob ? QStringLiteral("packed") : QStringLiteral("game.exfat"));
        CancellationSource cancel;
        Env::Run r = m_env.run(kind, {{QStringLiteral("source"), a}, {QStringLiteral("output"), out}, {QStringLiteral("format"), QStringLiteral("exfat")}},
                               &cancel, [&](const ProgressSnapshot& s) {
                                   // As soon as the pass has written something (eboot.bin, loose,
                                   // comes first): the packs' progress comes a batch at a time.
                                   if (cancelIt && s.phaseIndex == 1 && s.done > 64 * 1024)
                                       cancel.cancel();
                               });
        QVERIFY2(!r.result.ok(), "the job did not fail");
        if (cancelIt)
            QVERIFY2(r.result.error().isCancelled() || cancel.token().isCancelled(), qPrintable(why(r.result)));
        else
            QCOMPARE(r.result.error().code, ErrorCode::CorruptData);
        QCOMPARE(listTop(outDir), QStringList());
        QCOMPARE(r.tempFilesAfter, QStringList());
    }

    // A game that is itself packed in the archive is unpacked first, and says why.
    void archiveOfAPackedGameIsUnpackedFirst()
    {
        const QString a = makeArchive(QStringLiteral("7z-lzma2"));
        const QString packedOut = QFileInfo(fresh(QStringLiteral("p/x"))).absolutePath() + QStringLiteral("/packed");
        Env::Run p = m_env.run(ampr::kPackJob, {{QStringLiteral("source"), a}, {QStringLiteral("output"), packedOut}});
        QVERIFY2(p.result.ok(), qPrintable(why(p.result)));
        QList<Item> items;
        const QMap<QString, QByteArray> packedFiles = snapshot(packedOut);
        for (auto it = packedFiles.cbegin(); it != packedFiles.cend(); ++it)
            items << file(it.key(), it.value());
        const QString b = makeArchive(QStringLiteral("zip-deflate"), {}, items);
        const QString again = QFileInfo(fresh(QStringLiteral("q/x"))).absolutePath() + QStringLiteral("/repacked");
        Env::Run r = m_env.run(ampr::kPackJob, {{QStringLiteral("source"), b}, {QStringLiteral("output"), again}});
        QVERIFY2(r.result.ok(), qPrintable(why(r.result) + u'\n' + r.log1()));
        QVERIFY2(r.log1().contains(QStringLiteral("already zstd-packed")) && r.log1().contains(QStringLiteral("Unpacked")), qPrintable(r.log1()));
    }
};

QTEST_GUILESS_MAIN(TstArchiveStreamMore)
#include "tst_archive_stream_more.moc"
