// Images built straight from an archived dump (GitHub #37, core/ArchiveStream.h):
// no copy of the game on disk.
//
// The central claim: an image streamed from a ZIP, 7z or RAR is byte for byte
// the image built from the same archive unpacked to a folder
// (unpackArchiveSource / extractArchive), with the unpack path's time policy
// (a file keeps the time the archive lists; creation and access times, and a
// folder's times, are the time of the build, given here as one fixed time to
// both builds). That is checked for exFAT and, on Windows (where the
// unpacked folder lists in NTFS order), for FFPKG, at engine level so both
// builds can be given the same volume serial / fs_id and times.
//
// Then the jobs: what streams and what the log says, no game-sized data in
// the job's temporary folder, a damaged entry, an entry whose size differs
// from the list, a cancel in the middle of the pass (no image, no .fw.part),
// and the read-back verification catching a changed byte.
//
// 7z archives are made with libarchive's writer (solid: it puts every file in
// one block); there is no free RAR writer, so RAR is tested with libarchive's
// own samples (RAR 4, RAR 5, solid, stored, Windows times). All synthetic:
// MOCK as far as the console goes.

#include "core/ArchiveReader.h"
#include "core/ArchiveSource.h"
#include "core/ArchiveStream.h"
#include "core/Dependencies.h"
#include "core/FileUtil.h"
#include "core/Log.h"
#include "core/Settings.h"
#include "core/Zip.h"
#include "engines/exfat/ExfatAmpr.h"
#include "engines/exfat/ExfatArchive.h"
#include "engines/exfat/ExfatVerify.h"
#include "engines/exfat/ExfatWriter.h"
#include "engines/exfat/Module.h"
#include "engines/general/Module.h"
#include "engines/ufs2/Module.h"
#include "engines/ufs2/Ufs2Ampr.h"
#include "engines/ufs2/Ufs2Archive.h"
#include "engines/ufs2/Ufs2Reader.h"
#include "engines/ufs2/Ufs2Verify.h"
#include "engines/ufs2/Ufs2Writer.h"
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

constexpr qint64 kMtime = 1717243200;                       // 2024-06-01 12:00:00 UTC
const QDateTime kBuildTime = QDateTime::fromSecsSinceEpoch(1731600000); // a fixed "now" for both builds
constexpr quint32 kSerial = 0x1234ABCD;

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

struct Item {
    QString name;
    QByteArray data;
    bool folder = false;
    qint64 mtime = kMtime;
    bool link = false;
};

Item file(const QString& n, const QByteArray& d, qint64 t = kMtime) { return {n, d, false, t, false}; }
Item folder(const QString& n) { return {n, {}, true, kMtime, false}; }

// A made-up game, listed in an order that is NOT the order of the image
// (which goes by name, folders first): the archive's last entries go first
// into the image. Nested and empty folders, an empty file, names outside
// ASCII, 5 MiB that do not compress, a run of 2 MiB of zeros (left as a
// hole in the image), different times.
QList<Item> game(const QString& prefix = {})
{
    QByteArray zeros(2 * 1024 * 1024 + 5, '\0');
    zeros[0] = 'Z';
    zeros[zeros.size() - 1] = 'z';
    return {
        file(prefix + QStringLiteral("zz-last-in-the-image.bin"), noise(70000, 9), kMtime + 3600),
        file(prefix + QStringLiteral("data/big.bin"), noise(5 * 1024 * 1024 + 17, 4), kMtime - 86400 * 400),
        file(prefix + QStringLiteral("data/deep/deeper/deepest/text.txt"), QByteArray(200000, 'a'), kMtime + 1),
        file(prefix + QStringLiteral("data/データ — ü.bin"), noise(257, 3)),
        file(prefix + QStringLiteral("data/zero.bin"), QByteArray()),
        file(prefix + QStringLiteral("data/zeros.bin"), zeros),
        folder(prefix + QStringLiteral("data/empty folder")),
        file(prefix + QStringLiteral("sce_sys/icon0.png"), noise(4000, 2)),
        file(prefix + QStringLiteral("sce_sys/param.json"), "{\"titleId\":\"PPSA01234\",\"titleName\":\"Stream test\"}"),
        file(prefix + QStringLiteral("Eboot.BIN.bak"), noise(1000, 7)),
        file(prefix + QStringLiteral("eboot.bin"), noise(100000, 1), kMtime + 7),
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
        } else if (item.link) {
            archive_entry_set_filetype(e, AE_IFLNK);
            archive_entry_set_perm(e, 0777);
            archive_entry_copy_symlink_w(e, L"eboot.bin");
        } else {
            archive_entry_set_filetype(e, AE_IFREG);
            archive_entry_set_perm(e, 0644);
            archive_entry_set_size(e, item.data.size());
        }
        ok = archive_write_header(a, e) == ARCHIVE_OK;
        if (ok && !item.folder && !item.link && !item.data.isEmpty())
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

QByteArray uudecode(const QByteArray& text)
{
    QByteArray out;
    bool inside = false;
    for (QByteArray line : text.split('\n')) {
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

// The unpack path's times on a scanned folder: what unpacking leaves as
// "now" (a file's creation and access, a folder's three times) becomes the
// one build time both builds are given.
void unpackTimes(exfat::SourceTree& tree)
{
    for (exfat::SourceNode& n : tree.nodes) {
        n.created = n.accessed = kBuildTime;
        if (n.isDir)
            n.modified = kBuildTime;
    }
}

exfat::WriterOptions exfatOptions()
{
    exfat::WriterOptions o;
    o.serial = kSerial;
    o.label = QStringLiteral("OSFIMG");
    o.spare = exfat::SpareMode::Fixed;
    o.spareBytes = 4 * 1024 * 1024;
    o.syncToDisk = false;
    o.bufferBytes = 1024 * 1024;
    return o;
}

ufs2::WriterOptions ufs2Options()
{
    ufs2::WriterOptions o;
    o.formatTime = kBuildTime.toSecsSinceEpoch();
    o.populateTime = kBuildTime.toSecsSinceEpoch();
    o.fsId1 = 0x5A5A;
    return o;
}

// A registry with the modules these jobs need, and a runner that keeps the
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
        defaults.insert(exfat::keys::kSpareSpace, QStringLiteral("fixed"));
        defaults.insert(exfat::keys::kSpareSpaceMiB, 4);
        defaults.insert(general::keys::kFreeSpaceMarginMiB, 16);
    }
    SettingsValues settings(const QVariantMap& overrides) const
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
        TempDir temp(QStringLiteral("stream-test"));
        int calls = 0;
        ProgressReporter reporter(
            [&](const ProgressSnapshot& p) {
                // How much the job keeps in its temporary folder while it writes.
                if (p.phaseIndex >= 1 && (calls++ % 8) == 0)
                    out.peakTempBytes = std::max(out.peakTempBytes, folderBytes(temp.path()));
                if (sink)
                    sink(p);
            },
            0);
        reporter.setPhases(job.value()->phases(), job.value()->phaseWeights(s));
        static std::atomic<int> runs{0};
        const QString jobId = QStringLiteral("stream-test-%1").arg(++runs);
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
        out.peakTempBytes = std::max(out.peakTempBytes, out.tempBytesAfter);
        return out;
    }
};

} // namespace

class TstArchiveStream : public QObject {
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
        const QByteArray text = readAll(QStringLiteral(FW_LIBARCHIVE_TEST_DATA "/") + name + QStringLiteral(".uu"));
        const QString out = fresh(name);
        return !text.isEmpty() && writeAll(out, uudecode(text)) ? out : QString();
    }

    // An archive of game() in one of the forms the tests use.
    QString makeArchive(const QString& kind, const QString& prefix = {})
    {
        const QList<Item> items = game(prefix);
        if (kind == u"zip-deflate" || kind == u"zip-store") {
            const QString p = fresh(QStringLiteral("PPSA01234.zip"));
            return writeZip(p, items, kind == u"zip-store" ? zip::Method::Store : zip::Method::Deflate) ? p : QString();
        }
        const QString p = fresh(QStringLiteral("PPSA01234.7z"));
        const QByteArray method = kind.mid(3).toLatin1(); // "7z-lzma2" -> "lzma2"
        return write7z(p, items, method.constData()) ? p : QString();
    }

    // The exFAT image streamed from `archive` (engine level), verified by reading it back.
    QString exfatStreamed(const fwa::GameListing& g, const exfat::ScanRules& rules = {})
    {
        auto tree = exfat::treeFromArchive(g, rules, kBuildTime);
        if (!tree.ok())
            return QStringLiteral("!") + tree.error().toLogString();
        const exfat::WriterOptions o = exfatOptions();
        auto plan = exfat::planImage(tree.value(), o);
        if (!plan.ok())
            return QStringLiteral("!") + plan.error().toLogString();
        const QString image = fresh(QStringLiteral("streamed.exfat"));
        auto written = exfat::writeImageFromArchive(tree.value(), plan.value(), g, image, o, CancellationToken());
        if (!written.ok())
            return QStringLiteral("!") + written.error().toLogString();
        exfat::VerifyOptions vo;
        auto rep = exfat::verifyImage(image, exfat::expectedFromArchive(tree.value(), written->stream.digests), vo, CancellationToken());
        if (!rep.ok() || !rep->passed())
            return QStringLiteral("!verify: ") + (rep.ok() ? rep->summary() : rep.error().toLogString());
        return image;
    }

    QString exfatFromFolder(const QString& folder, const exfat::ScanRules& rules = {})
    {
        auto tree = exfat::scanSource(folder, rules, CancellationToken());
        if (!tree.ok())
            return QStringLiteral("!") + tree.error().toLogString();
        unpackTimes(tree.value());
        const exfat::WriterOptions o = exfatOptions();
        auto plan = exfat::planImage(tree.value(), o);
        if (!plan.ok())
            return QStringLiteral("!") + plan.error().toLogString();
        const QString image = fresh(QStringLiteral("folder.exfat"));
        auto written = exfat::writeImage(tree.value(), plan.value(), image, o, CancellationToken());
        return written.ok() ? image : QStringLiteral("!") + written.error().toLogString();
    }

    QString ufs2Streamed(const fwa::GameListing& g)
    {
        auto tree = ufs2::treeFromArchive(g, kBuildTime);
        if (!tree.ok())
            return QStringLiteral("!") + tree.error().toLogString();
        ufs2::WriterOptions o = ufs2Options();
        o.leaveArchiveData = true;
        ufs2::Ufs2Writer w(tree.value(), o);
        if (auto p = w.plan(); !p.ok())
            return QStringLiteral("!") + p.error().toLogString();
        const QString image = fresh(QStringLiteral("streamed.ffpkg"));
        auto written = ufs2::writeImageFromArchive(w, tree.value(), g, image, 1024 * 1024, CancellationToken());
        if (!written.ok())
            return QStringLiteral("!") + written.error().toLogString();
        auto reader = ufs2::Ufs2Reader::open(image);
        if (!reader.ok())
            return QStringLiteral("!") + reader.error().toLogString();
        auto cmp = ufs2::compareImage(*reader.value(), tree.value(), ufs2::CompareDepth::Full, CancellationToken(), nullptr);
        if (!cmp.ok() || !cmp->ok())
            return QStringLiteral("!compare: ") + (cmp.ok() ? cmp->summary(ufs2::CompareDepth::Full) : cmp.error().toLogString());
        return image;
    }

    QString ufs2FromFolder(const QString& folder)
    {
        auto tree = ufs2::scanLocalFolder(folder, CancellationToken());
        if (!tree.ok())
            return QStringLiteral("!") + tree.error().toLogString();
        ufs2::Ufs2Writer w(tree.value(), ufs2Options());
        if (auto p = w.plan(); !p.ok())
            return QStringLiteral("!") + p.error().toLogString();
        const QString image = fresh(QStringLiteral("folder.ffpkg"));
        auto written = w.write(image, CancellationToken(), nullptr);
        return written.ok() ? image : QStringLiteral("!") + written.error().toLogString();
    }

    static QString failed(const QString& image) { return image.startsWith(u'!') ? image.mid(1) : QString(); }

private slots:
    void initTestCase()
    {
        QVERIFY(m_root.isValid());
        fwtest::isolateAppData(m_root);
    }

    // ---- byte for byte: streamed == unpacked folder -------------------------------------

    void exfatStreamedMatchesUnpackedFolder_data()
    {
        QTest::addColumn<QString>("kind");
        QTest::addColumn<QString>("prefix");
        for (const char* k : {"zip-deflate", "zip-store", "7z-lzma2", "7z-store", "7z-ppmd"}) {
            QTest::newRow(qPrintable(QStringLiteral("%1 contents").arg(QLatin1String(k)))) << QString::fromLatin1(k) << QString();
            QTest::newRow(qPrintable(QStringLiteral("%1 folder").arg(QLatin1String(k))))
                << QString::fromLatin1(k) << QStringLiteral("PPSA01234-app0/");
        }
    }

    void exfatStreamedMatchesUnpackedFolder()
    {
        QFETCH(QString, kind);
        QFETCH(QString, prefix);
        const QString a = makeArchive(kind, prefix);
        QVERIFY(!a.isEmpty());
        auto g = fwa::listArchiveGame(a);
        QVERIFY2(g.ok(), g.ok() ? "" : qPrintable(g.error().toLogString()));
        QCOMPARE(g->bytes, bytesOf(game()));
        QCOMPARE(g->rootPrefix, prefix);
        const QString streamed = exfatStreamed(g.value());
        QVERIFY2(failed(streamed).isEmpty(), qPrintable(failed(streamed)));
        auto root = fwa::unpackArchiveSource(a, fresh(QStringLiteral("temp")), CancellationToken(), nullptr, 0);
        QVERIFY2(root.ok(), root.ok() ? "" : qPrintable(root.error().toLogString()));
        const QString folder = exfatFromFolder(root.value());
        QVERIFY2(failed(folder).isEmpty(), qPrintable(failed(folder)));
        QCOMPARE(QFileInfo(streamed).size(), QFileInfo(folder).size());
        QCOMPARE(sha(streamed), sha(folder));
        // The exclusion rules too: a pattern, a folder, the hidden rule.
        exfat::ScanRules rules;
        rules.excludePatterns = {QStringLiteral("*.bak"), QStringLiteral("deep/")};
        rules.excludeHidden = true;
        const QString s2 = exfatStreamed(g.value(), rules);
        const QString f2 = exfatFromFolder(root.value(), rules);
        QVERIFY2(failed(s2).isEmpty() && failed(f2).isEmpty(), qPrintable(failed(s2) + failed(f2)));
        QCOMPARE(sha(s2), sha(f2));
        QVERIFY(sha(s2) != sha(streamed));
    }

    // libarchive's RAR samples (no game in them: the whole archive is built).
    void rarStreamedMatchesUnpackedFolder_data()
    {
        QTest::addColumn<QString>("name");
        for (const char* n : {"test_read_format_rar5_multiple_files.rar", "test_read_format_rar5_multiple_files_solid.rar",
                              "test_read_format_rar5_solid.rar", "test_read_format_rar5_stored.rar",
                              "test_read_format_rar5_compressed.rar", "test_read_format_rar5_win32.rar",
                              "test_read_format_rar5_unicode.rar", "test_read_format_rar5_blake2.rar",
                              "test_read_format_rar_compress_normal.rar", "test_read_format_rar_unicode.rar"})
            QTest::newRow(n) << QString::fromLatin1(n);
    }

    void rarStreamedMatchesUnpackedFolder()
    {
        QFETCH(QString, name);
        const QString a = sample(name);
        QVERIFY2(!a.isEmpty(), "the sample is not in the libarchive sources");
        auto g = fwa::listWholeArchiveForTesting(a);
        QVERIFY2(g.ok(), g.ok() ? "" : qPrintable(g.error().toLogString()));
        QVERIFY(g->files > 0);
        // Every listed file has the time unpacking gives it (RAR 5 times are read from the headers here).
        auto listing = fwa::listArchive(a);
        QVERIFY(listing.ok());
        const QString dest = fresh(QStringLiteral("unpacked"));
        auto x = fwa::extractArchive(listing.value(), dest, {}, CancellationToken());
        QVERIFY2(x.ok(), x.ok() ? "" : qPrintable(x.error().toLogString()));
        for (const fwa::GameItem& it : g->items)
            if (!it.isDir && it.modified.isValid())
                QCOMPARE(QFileInfo(QDir(dest).filePath(it.path)).lastModified().toSecsSinceEpoch(), it.modified.toSecsSinceEpoch());
        const QString streamed = exfatStreamed(g.value());
        QVERIFY2(failed(streamed).isEmpty(), qPrintable(failed(streamed)));
        const QString folder = exfatFromFolder(dest);
        QVERIFY2(failed(folder).isEmpty(), qPrintable(failed(folder)));
        QCOMPARE(sha(streamed), sha(folder));
#ifdef Q_OS_WIN
        const QString u1 = ufs2Streamed(g.value());
        const QString u2 = ufs2FromFolder(dest);
        QVERIFY2(failed(u1).isEmpty() && failed(u2).isEmpty(), qPrintable(failed(u1) + failed(u2)));
        QCOMPARE(sha(u1), sha(u2));
#endif
    }

    void ufs2StreamedMatchesUnpackedFolder_data()
    {
        QTest::addColumn<QString>("kind");
        for (const char* k : {"zip-deflate", "7z-lzma2", "7z-store"})
            QTest::newRow(k) << QString::fromLatin1(k);
    }

    void ufs2StreamedMatchesUnpackedFolder()
    {
        QFETCH(QString, kind);
        const QString a = makeArchive(kind, QStringLiteral("Game/"));
        QVERIFY(!a.isEmpty());
        auto g = fwa::listArchiveGame(a);
        QVERIFY2(g.ok(), g.ok() ? "" : qPrintable(g.error().toLogString()));
        const QString streamed = ufs2Streamed(g.value());
        QVERIFY2(failed(streamed).isEmpty(), qPrintable(failed(streamed)));
#ifndef Q_OS_WIN
        QSKIP("A folder build takes the file system's listing order; the streamed build takes NTFS order (Windows only).");
#else
        auto root = fwa::unpackArchiveSource(a, fresh(QStringLiteral("temp")), CancellationToken(), nullptr, 0);
        QVERIFY2(root.ok(), root.ok() ? "" : qPrintable(root.error().toLogString()));
        const QString folder = ufs2FromFolder(root.value());
        QVERIFY2(failed(folder).isEmpty(), qPrintable(failed(folder)));
        QCOMPARE(QFileInfo(streamed).size(), QFileInfo(folder).size());
        QCOMPARE(sha(streamed), sha(folder));
#endif
    }

    // The ZIP is read in the image's order, a 7z in its own: the archive's
    // first entry is the image's last file, and the result is the same.
    void entryOrderDoesNotMatter()
    {
        QList<Item> items = game();
        std::reverse(items.begin(), items.end());
        const QString z1 = fresh(QStringLiteral("a.7z")), z2 = fresh(QStringLiteral("b.7z"));
        QVERIFY(write7z(z1, game()));
        QVERIFY(write7z(z2, items));
        auto g1 = fwa::listArchiveGame(z1), g2 = fwa::listArchiveGame(z2);
        QVERIFY(g1.ok() && g2.ok());
        const QString i1 = exfatStreamed(g1.value()), i2 = exfatStreamed(g2.value());
        QVERIFY2(failed(i1).isEmpty() && failed(i2).isEmpty(), qPrintable(failed(i1) + failed(i2)));
        QCOMPARE(sha(i1), sha(i2));
        // A ZIP keeps DOS times (two-second steps), so it is compared with a ZIP.
        const QString zipped = fresh(QStringLiteral("c.zip")), zipped2 = fresh(QStringLiteral("d.zip"));
        QVERIFY(writeZip(zipped, game(), zip::Method::Deflate));
        QVERIFY(writeZip(zipped2, items, zip::Method::Deflate));
        auto g3 = fwa::listArchiveGame(zipped), g4 = fwa::listArchiveGame(zipped2);
        QVERIFY(g3.ok() && g4.ok());
        const QString i3 = exfatStreamed(g3.value()), i4 = exfatStreamed(g4.value());
        QVERIFY2(failed(i3).isEmpty() && failed(i4).isEmpty(), qPrintable(failed(i3) + failed(i4)));
        QCOMPARE(sha(i4), sha(i3));
    }

    // ---- the jobs ---------------------------------------------------------------------------

    void jobsStreamAndKeepTheTempFolderSmall_data()
    {
        QTest::addColumn<QString>("kind");
        QTest::addColumn<QString>("archive");
        for (const QString& k : {exfat::kJobBuild, ufs2::kinds::kBuild})
            for (const char* a : {"zip-deflate", "7z-lzma2"})
                QTest::newRow(qPrintable(k + u' ' + QLatin1String(a))) << k << QString::fromLatin1(a);
    }

    void jobsStreamAndKeepTheTempFolderSmall()
    {
        QFETCH(QString, kind);
        QFETCH(QString, archive);
        const QString a = makeArchive(archive, QStringLiteral("PPSA01234-app0/"));
        QVERIFY(!a.isEmpty());
        const QByteArray before = sha(a);
        const QString outDir = QFileInfo(fresh(QStringLiteral("out/x"))).absolutePath();
        const QString out = QDir(outDir).filePath(kind == exfat::kJobBuild ? QStringLiteral("game.exfat") : QStringLiteral("game.ffpkg"));
        Env::Run r = m_env.run(kind, {{QStringLiteral("source"), a}, {QStringLiteral("output"), out}});
        QVERIFY2(r.result.ok(), r.result.ok() ? "" : qPrintable(r.result.error().toLogString() + u'\n' + r.log.join(u'\n')));
        QCOMPARE(r.result->verification, Verification::Passed);
        QCOMPARE(listTop(outDir), QStringList{QFileInfo(out).fileName()});
        QCOMPARE(sha(a), before);
        QVERIFY2(r.log.join(u'\n').contains(QStringLiteral("Building straight from")), qPrintable(r.log.join(u'\n')));
        QVERIFY2(!r.log.join(u'\n').contains(QStringLiteral("Unpacked")), qPrintable(r.log.join(u'\n')));
        // The game is about 7.5 MiB; nothing of it was written to the temporary folder.
        QVERIFY2(r.peakTempBytes < 64 * 1024, qPrintable(QString::number(r.peakTempBytes)));
        // The image holds the game: verified against the game unpacked to a folder.
        auto root = fwa::unpackArchiveSource(a, fresh(QStringLiteral("temp")), CancellationToken(), nullptr, 0);
        QVERIFY(root.ok());
        const QString verifyKind = kind == exfat::kJobBuild ? exfat::kJobVerify : ufs2::kinds::kVerify;
        Env::Run v = m_env.run(verifyKind, {{QStringLiteral("image"), out}, {QStringLiteral("source"), root.value()}});
        QVERIFY2(v.result.ok(), v.result.ok() ? "" : qPrintable(v.result.error().toLogString()));
        QVERIFY2(v.result->verification == Verification::Passed, qPrintable(v.result->verificationDetail));
    }

    // The exFAT build's eboot.bin rule applies to the archive's game.
    void exfatJobRequiresEbootInTheArchive()
    {
        QList<Item> items = game();
        items.removeIf([](const Item& i) { return i.name == u"eboot.bin"; });
        const QString a = fresh(QStringLiteral("noeboot.7z"));
        QVERIFY(write7z(a, items));
        const QString out = fresh(QStringLiteral("x.exfat"));
        Env::Run r = m_env.run(exfat::kJobBuild, {{QStringLiteral("source"), a}, {QStringLiteral("output"), out}});
        QVERIFY(!r.result.ok());
        QCOMPARE(r.result.error().code, ErrorCode::NotFound);
        QVERIFY(r.result.error().message.contains(QStringLiteral("eboot.bin")));
        QVERIFY(listTop(QFileInfo(out).absolutePath()).isEmpty());
    }

    // A damaged entry fails the build; there is no image and no .fw.part.
    void damagedEntryLeavesNoImage_data()
    {
        QTest::addColumn<QString>("job");
        QTest::addColumn<QString>("archive");
        for (const QString& k : {exfat::kJobBuild, ufs2::kinds::kBuild})
            for (const char* a : {"zip-store", "zip-deflate", "7z-lzma2", "7z-store"})
                QTest::newRow(qPrintable(k + u' ' + QLatin1String(a))) << k << QString::fromLatin1(a);
    }

    void damagedEntryLeavesNoImage()
    {
        QFETCH(QString, job);
        QFETCH(QString, archive);
        const QString a = makeArchive(archive);
        QVERIFY(!a.isEmpty());
        QByteArray bytes = readAll(a);
        // A byte in the middle of big.bin's stored or packed data (the lists
        // of files are at the end of a ZIP and of these 7z archives).
        const qsizetype at = archive.endsWith(u"store") ? bytes.indexOf(noise(5 * 1024 * 1024 + 17, 4).mid(3000000, 64)) : bytes.size() / 2;
        QVERIFY(at > 0);
        bytes[at + 10] = char(bytes[at + 10] ^ 0x5A);
        QVERIFY(writeAll(a, bytes));
        const QString outDir = QFileInfo(fresh(QStringLiteral("x"))).absolutePath();
        const QString out = QDir(outDir).filePath(QStringLiteral("game.img"));
        Env::Run r = m_env.run(job, {{QStringLiteral("source"), a}, {QStringLiteral("output"), out}});
        QVERIFY2(!r.result.ok(), "a damaged archive was built");
        QCOMPARE(r.result.error().code, ErrorCode::CorruptData);
        QCOMPARE(listTop(outDir), QStringList());
    }

    // The list says one size, the data another: the pass stops.
    void entrySizeDifferentFromTheListFails_data()
    {
        QTest::addColumn<int>("delta");
        QTest::newRow("listed larger") << 1;
        QTest::newRow("listed smaller") << -1;
    }

    void entrySizeDifferentFromTheListFails()
    {
        QFETCH(int, delta);
        const QString a = makeArchive(QStringLiteral("zip-store"));
        QVERIFY(!a.isEmpty());
        QByteArray bytes = readAll(a);
        // Central directory record of data/big.bin: uncompressed size at +24.
        const QByteArray name = QByteArrayLiteral("data/big.bin");
        qsizetype cd = -1;
        for (qsizetype i = bytes.indexOf(QByteArray("PK\x01\x02", 4)); i >= 0; i = bytes.indexOf(QByteArray("PK\x01\x02", 4), i + 4))
            if (bytes.mid(i + 46, name.size()) == name)
                cd = i;
        QVERIFY(cd > 0);
        const quint32 size = qFromLittleEndian<quint32>(bytes.constData() + cd + 24);
        qToLittleEndian<quint32>(quint32(qint64(size) + delta), bytes.data() + cd + 24);
        QVERIFY(writeAll(a, bytes));
        const QString outDir = QFileInfo(fresh(QStringLiteral("x"))).absolutePath();
        Env::Run r = m_env.run(exfat::kJobBuild, {{QStringLiteral("source"), a}, {QStringLiteral("output"), QDir(outDir).filePath(QStringLiteral("g.exfat"))}});
        QVERIFY2(!r.result.ok(), "built from an archive whose list does not match its data");
        QCOMPARE(r.result.error().code, ErrorCode::CorruptData);
        QCOMPARE(listTop(outDir), QStringList());
    }

    // Cancel in the middle of the pass: no image, no .fw.part, an older image kept.
    void cancelMidStreamLeavesNoImage_data()
    {
        QTest::addColumn<QString>("job");
        QTest::addColumn<QString>("archive");
        for (const QString& k : {exfat::kJobBuild, ufs2::kinds::kBuild})
            for (const char* a : {"zip-deflate", "7z-lzma2"})
                QTest::newRow(qPrintable(k + u' ' + QLatin1String(a))) << k << QString::fromLatin1(a);
    }

    void cancelMidStreamLeavesNoImage()
    {
        QFETCH(QString, job);
        QFETCH(QString, archive);
        const QString a = makeArchive(archive);
        QVERIFY(!a.isEmpty());
        const QString outDir = QFileInfo(fresh(QStringLiteral("x"))).absolutePath();
        const QString out = QDir(outDir).filePath(QStringLiteral("game.img"));
        QVERIFY(writeAll(out, "PREVIOUS IMAGE"));
        CancellationSource cancel;
        std::atomic<qint64> seenInPass{0};
        Env::Run r = m_env.run(
            job, {{QStringLiteral("source"), a}, {QStringLiteral("output"), out}, {QStringLiteral("replaceExisting"), true}}, &cancel,
            [&](const ProgressSnapshot& s) {
                if (s.phaseIndex == 1 && s.done > 1024 * 1024) {
                    seenInPass = s.done;
                    cancel.cancel();
                }
            });
        QVERIFY(!r.result.ok());
        QVERIFY2(r.result.error().isCancelled() || cancel.token().isCancelled(), qPrintable(r.result.error().toLogString()));
        QVERIFY(seenInPass > 0);
        QCOMPARE(listTop(outDir), QStringList{QStringLiteral("game.img")});
        QCOMPARE(readAll(out), QByteArray("PREVIOUS IMAGE"));
    }

    // The read-back verification compares the image with the digests taken
    // while the files were written: one changed byte is found.
    void verificationFindsAChangedByte()
    {
        const QString a = makeArchive(QStringLiteral("7z-lzma2"));
        auto g = fwa::listArchiveGame(a);
        QVERIFY(g.ok());
        auto tree = exfat::treeFromArchive(g.value(), {}, kBuildTime);
        QVERIFY(tree.ok());
        auto plan = exfat::planImage(tree.value(), exfatOptions());
        QVERIFY(plan.ok());
        const QString image = fresh(QStringLiteral("v.exfat"));
        auto written = exfat::writeImageFromArchive(tree.value(), plan.value(), g.value(), image, exfatOptions(), CancellationToken());
        QVERIFY(written.ok());
        const auto expected = exfat::expectedFromArchive(tree.value(), written->stream.digests);
        auto good = exfat::verifyImage(image, expected, exfat::VerifyOptions(), CancellationToken());
        QVERIFY(good.ok() && good->passed());
        // Change one byte of big.bin in the image.
        QByteArray bytes = readAll(image);
        const qsizetype at = bytes.indexOf(noise(5 * 1024 * 1024 + 17, 4).mid(1000, 64));
        QVERIFY(at > 0);
        bytes[at] = char(bytes[at] ^ 1);
        QVERIFY(writeAll(image, bytes));
        auto bad = exfat::verifyImage(image, expected, exfat::VerifyOptions(), CancellationToken());
        QVERIFY(bad.ok());
        QVERIFY(!bad->passed());
        QCOMPARE(bad->mismatches.size(), size_t(1));
        QCOMPARE(bad->mismatches.front().path, QStringLiteral("data/big.bin"));
        QCOMPARE(bad->mismatches.front().kind, QStringLiteral("content"));
    }

    // Links are left out, as unpacking leaves them out; a name the archive
    // lists twice, or one that would leave the game, stops the build first.
    void linksAreLeftOutAsWhenUnpacking()
    {
        QList<Item> items = game();
        Item l;
        l.name = QStringLiteral("data/link-to-eboot");
        l.link = true;
        items << l;
        const QString a = fresh(QStringLiteral("links.7z"));
        QVERIFY(write7z(a, items));
        auto g = fwa::listArchiveGame(a);
        QVERIFY2(g.ok(), g.ok() ? "" : qPrintable(g.error().toLogString()));
        QCOMPARE(g->links, qint64(1));
        QCOMPARE(g->find(QStringLiteral("data/link-to-eboot")), -1);
        const QString streamed = exfatStreamed(g.value());
        QVERIFY2(failed(streamed).isEmpty(), qPrintable(failed(streamed)));
        auto root = fwa::unpackArchiveSource(a, fresh(QStringLiteral("temp")), CancellationToken(), nullptr, 0);
        QVERIFY(root.ok());
        QCOMPARE(sha(streamed), sha(exfatFromFolder(root.value())));
    }

    // ampr_emu.index from an archive: added when missing, kept when it is the
    // one the build would write (read out of the archive to compare), rebuilt
    // otherwise; the image verifies either way.
    void amprIndexFromTheArchive_data()
    {
        QTest::addColumn<QString>("job");
        QTest::addColumn<QString>("index"); // none, current, stale
        QTest::addColumn<QString>("action");
        for (const QString& k : {exfat::kJobBuild, ufs2::kinds::kBuild}) {
            QTest::newRow(qPrintable(k + QStringLiteral(" none"))) << k << QStringLiteral("none") << QStringLiteral("added");
            QTest::newRow(qPrintable(k + QStringLiteral(" current"))) << k << QStringLiteral("current") << QString();
            QTest::newRow(qPrintable(k + QStringLiteral(" stale"))) << k << QStringLiteral("stale") << QStringLiteral("rebuilt");
        }
    }

    void amprIndexFromTheArchive()
    {
        QFETCH(QString, job);
        QFETCH(QString, index);
        QFETCH(QString, action);
        QList<Item> items = game();
        items << file(QStringLiteral("fakelib/libSceAmpr.sprx"), noise(3000, 21));
        if (index != u"none") {
            // The index of these files, as the build makes it (the tree of the
            // archive without an index); "stale" changes one byte of a row.
            const QString probe = fresh(QStringLiteral("probe.7z"));
            QVERIFY(write7z(probe, items));
            auto g = fwa::listArchiveGame(probe);
            QVERIFY(g.ok());
            QByteArray built;
            if (job == exfat::kJobBuild) {
                auto t = exfat::treeFromArchive(g.value(), {}, kBuildTime);
                QVERIFY(t.ok());
                built = exfat::amprIndexFor(t.value());
            } else {
                auto t = ufs2::treeFromArchive(g.value(), kBuildTime);
                QVERIFY(t.ok());
                built = ufs2::amprIndexFor(t.value());
            }
            QVERIFY(built.size() > 100);
            if (index == u"stale")
                built[built.size() - 3] = char(built[built.size() - 3] ^ 1);
            items << file(QStringLiteral("ampr_emu.index"), built);
        }
        const QString a = fresh(QStringLiteral("ampr.7z"));
        QVERIFY(write7z(a, items));
        const QString outDir = QFileInfo(fresh(QStringLiteral("x"))).absolutePath();
        const QString out = QDir(outDir).filePath(QStringLiteral("game.img"));
        Env::Run r = m_env.run(job, {{QStringLiteral("source"), a}, {QStringLiteral("output"), out}});
        QVERIFY2(r.result.ok(), r.result.ok() ? "" : qPrintable(r.result.error().toLogString() + u'\n' + r.log.join(u'\n')));
        QVERIFY2(r.result->verification == Verification::Passed, qPrintable(r.result->verificationDetail));
        QCOMPARE(r.result->details.value(QStringLiteral("amprIndex")).toString(), action);
        if (index == u"current")
            QVERIFY2(r.log.join(u'\n').contains(QStringLiteral("kept")), qPrintable(r.log.join(u'\n')));
    }

    // ContentDigest is the same however the bytes are cut into pieces.
    void digestIgnoresHowBytesAreCut()
    {
        const QByteArray data = noise(300000, 11);
        fwa::ContentDigest whole, parts;
        whole.add(data.constData(), data.size());
        for (qsizetype at = 0; at < data.size();) {
            const qsizetype n = std::min<qsizetype>(data.size() - at, 1 + (at % 7777));
            parts.add(data.constData() + at, n);
            at += n;
        }
        QCOMPARE(parts.value(), whole.value());
        fwa::ContentDigest other;
        QByteArray changed = data;
        changed[5] = char(changed[5] ^ 1);
        other.add(changed.constData(), changed.size());
        QVERIFY(other.value() != whole.value());
    }
};

QTEST_GUILESS_MAIN(TstArchiveStream)
#include "tst_archive_stream.moc"
