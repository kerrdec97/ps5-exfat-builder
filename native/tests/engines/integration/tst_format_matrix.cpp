// End-to-end integrity matrix: every output format the app builds (.exfat,
// .ffpkg, .ffpfs, .ffpfsc, a packed zstd folder, and images whose game is in
// zstd packs) from every source kind it reads (a folder, a ZIP, a 7z, an
// existing image through image.convert), built and extracted through the real
// job classes on the job queue, then compared file by file: names, folder
// structure, sizes and SHA-256 of every file.
//
// Rows also cover the inputs that break such tools: empty and deeply nested
// folders, a path over 260 characters, Unicode names (CJK, combining marks,
// emoji, Greek, Hangul), names that differ only in letter case, zero-byte
// files, an existing output with and without "replace", damaged images and
// archives, and a cancel in every stage of every job. Each row asserts what
// the documentation says (native/docs/engines/*.md, docs/ZSTD_PACKS.md,
// docs/ARCHITECTURE.md). A row whose behaviour is wrong or undocumented is
// marked QEXPECT_FAIL with its finding id (kKnown below) instead of being
// weakened; an unexpected pass then fails the run.
//
// Data is synthetic and small by default (a few MiB per fixture). The big run
// adds a sparse file of FW_MATRIX_BIG_MIB MiB + 1 byte (4096 gives a file just
// over 4 GiB) to every format's round trip; it needs several times that much
// free space. FW_MATRIX_REPORT=<file> writes the row table as Markdown.
//
// FPKG packages need Sony's publishing tools: those rows build against the
// stand-in tool of the FPKG tests (fake_pubcmd.cpp) and are MOCK results, not
// evidence that a console accepts the package. LZ4 packs are read only in the
// app; their rows read a hand-built AMPRPAK4 game as tst_ampr does.

#include "core/ArchiveSource.h"
#include "core/FileUtil.h"
#include "core/Settings.h"
#include "core/Zip.h"
#include "connect/ps5/Module.h"
#include "engines/ampr/Module.h"
#include "engines/ampr/ZstdPack.h"
#include "engines/exfat/Module.h"
#include "engines/fpkg/FpkgSettings.h"
#include "engines/fpkg/Gp5.h"
#include "engines/fpkg/Module.h"
#include "engines/general/Module.h"
#include "engines/integration/GameSources.h"
#include "engines/integration/Module.h"
#include "engines/pfs/Module.h"
#include "engines/ufs2/Module.h"
#include "jobs/JobQueue.h"
#include "support/TestSupport.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>
#include <QTemporaryDir>
#include <QTest>

#include <archive.h>
#include <archive_entry.h>
#include <lz4.h>

#include <atomic>
#include <cstring>
#include <functional>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#endif

using namespace fw;

namespace {

constexpr qint64 kKiB = 1024;
constexpr qint64 kMiB = 1024 * 1024;

// ---- known findings: rows that assert the documented behaviour and fail ----------
//
// Key: a regular expression over "<test function>:<data tag>:<step>". The
// value is the finding id and a short reason (report 06a-format-matrix.md).
struct Known {
    const char* pattern;
    const char* finding;
};
const Known kKnown[] = {
    // libarchive's 7z reader turns names into Unicode NFC (archive_string.c
    // setup_converter: SCONV_NORMALIZATION_C for UTF-16 names on every system
    // but macOS): a file whose name has a combining mark comes out renamed.
    {"roundTrip:[^:]+/7z/unicode:compare", "F-MX-01 7z names are normalised to NFC"},
    // image.convert (repack) extracts to this drive first: names it cannot
    // store are renamed ("LEVEL1.BIN (2)") and the new image is "Verified"
    // with names the source image does not have; only a warning says so.
    {"caseOnlyNamesInImage::to-exfat-refused", "F-MX-03 convert renames case twins instead of refusing"},
    // F-MX-04 (FPKG from an .ffpfsc packaged the image's .pfskeep markers as
    // files instead of declaring the empty folders) is fixed in
    // GameSources.cpp planStage: the row asserts it again.
    {nullptr, nullptr},
};

const char* knownFailure(const char* step)
{
    const QString key = QStringLiteral("%1:%2:%3")
                            .arg(QString::fromLatin1(QTest::currentTestFunction()),
                                 QString::fromUtf8(QTest::currentDataTag() ? QTest::currentDataTag() : ""),
                                 QString::fromLatin1(step));
    for (const Known& k : kKnown) {
        if (!k.pattern)
            continue;
        if (QRegularExpression(QStringLiteral("^(?:%1)$").arg(QString::fromLatin1(k.pattern))).match(key).hasMatch())
            return k.finding;
    }
    return nullptr;
}

// ---- the row table ---------------------------------------------------------------

struct RowResult {
    QString area, row, result, note;
};
QList<RowResult>& rowResults()
{
    static QList<RowResult> rows;
    return rows;
}

// Records the row when the test function returns: PASS, FAIL, or what the row
// set (XFAIL <finding>, SKIP, MOCK, refused as documented).
struct RowGuard {
    QString area;
    QString verdict = QStringLiteral("PASS");
    QString note;
    explicit RowGuard(QString a) : area(std::move(a)) {}
    ~RowGuard()
    {
        QString v = verdict;
        if (QTest::currentTestFailed() && !v.startsWith(u"XFAIL"))
            v = QStringLiteral("FAIL");
        rowResults() << RowResult{area, QString::fromUtf8(QTest::currentDataTag() ? QTest::currentDataTag() : ""), v, note};
    }
};

// A check of the documented behaviour. A known finding turns it into an
// expected failure (an unexpected pass fails the row).
#define MCHECK(g, step, cond, msg)                                                   \
    do {                                                                             \
        if (const char* known_ = knownFailure(step)) {                               \
            (g).verdict = QStringLiteral("XFAIL ") + QString::fromLatin1(known_);    \
            QEXPECT_FAIL("", known_, Abort);                                         \
        }                                                                            \
        QVERIFY2(cond, qPrintable(QStringLiteral("[%1] ").arg(QLatin1String(step)) + (msg))); \
    } while (0)

// ---- the job environment (as tst_integration_safety) -----------------------------

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
        (void)store->setValue(general::keys::kAutoRetryCount, 0);
        (void)store->setValue(general::keys::kVerifyAfterBuild, true);
        (void)store->setValue(general::keys::kVerifyMode, QStringLiteral("full"));
        queue = std::make_unique<JobQueue>(*store, jobs);
    }
    // Through the queue, as the app runs a job.
    std::optional<JobRecord> run(const QString& kind, const QJsonObject& params, const QVariantMap& overrides = {})
    {
        auto id = queue->enqueue(kind, params, overrides);
        if (!id.ok()) {
            JobRecord r;
            r.kind = kind;
            r.state = JobState::Failed;
            r.error = id.error();
            return r; // refused when queued (the queue validates first)
        }
        (void)QTest::qWaitFor([&] { auto r = queue->record(id.value()); return r && isTerminal(r->state); }, 600000);
        return queue->record(id.value());
    }
    // On this thread, so a test can act at a chosen moment of the job.
    Result<JobOutcome> runDirect(const QString& kind, const QJsonObject& params, const QVariantMap& overrides = {},
                                 const std::function<void(const ProgressSnapshot&)>& watch = {},
                                 const CancellationToken& token = {})
    {
        FW_TRY_ASSIGN(auto job, jobs.create(kind, params));
        QTemporaryDir temp;
        ProgressReporter reporter(
            [&](const ProgressSnapshot& s) {
                if (watch)
                    watch(s);
            },
            0);
        const SettingsValues settings = store->snapshot().withOverrides(overrides);
        JobContext ctx{QStringLiteral("direct"), token, &reporter, Log(QStringLiteral("matrix")), settings, temp.path()};
        reporter.setPhases(job->phases(), job->phaseWeights(ctx.settings));
        FW_TRY(job->validate(ctx.settings));
        return job->run(ctx);
    }
};

QString describe(const std::optional<JobRecord>& r)
{
    if (!r)
        return QStringLiteral("no record");
    QString s = jobStateName(r->state) + QStringLiteral(": ");
    if (r->error)
        s += errorCodeName(r->error->code) + QStringLiteral(" ") + r->error->toLogString();
    else if (r->outcome)
        s += verificationName(r->outcome->verification) + QStringLiteral(" ") + r->outcome->summary + QStringLiteral(" | ")
           + r->outcome->verificationDetail + QStringLiteral(" | warnings: ") + r->outcome->warnings.join(QStringLiteral(" / "));
    return s;
}

QString describe(const Result<JobOutcome>& r)
{
    if (!r.ok())
        return QStringLiteral("error ") + errorCodeName(r.error().code) + QStringLiteral(" ") + r.error().toLogString();
    return QStringLiteral("outcome ") + verificationName(r->verification) + QStringLiteral(" ") + r->summary + QStringLiteral(" | ")
         + r->verificationDetail + QStringLiteral(" | warnings: ") + r->warnings.join(QStringLiteral(" / "));
}

bool succeeded(const std::optional<JobRecord>& r)
{
    return r && r->state == JobState::Succeeded && r->outcome && r->outcome->verification != Verification::Failed;
}

// ---- bytes and files ------------------------------------------------------------

QByteArray noise(qint64 n, quint32 seed)
{
    QByteArray b(qsizetype(n), Qt::Uninitialized);
    quint32 x = seed * 2654435761u + 1;
    for (qint64 i = 0; i < n; ++i) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        b[qsizetype(i)] = char(x >> 11);
    }
    return b;
}

QByteArray compressible(qint64 n, char seed)
{
    QByteArray b(qsizetype(n), '\0');
    for (qint64 i = 0; i < n; ++i)
        b[qsizetype(i)] = char('a' + ((i / 97 + seed) % 7));
    return b;
}

bool put(const QString& root, const QString& rel, const QByteArray& data)
{
    return fwtest::writeTextFile(QDir(root).filePath(rel), data);
}

QByteArray fileBytes(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

QByteArray sha256Of(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QByteArrayLiteral("<unreadable>");
    QCryptographicHash h(QCryptographicHash::Sha256);
    if (!h.addData(&f))
        return QByteArrayLiteral("<unreadable>");
    return h.result().toHex();
}

// The entries directly in `dir`, sorted (hidden ones included).
QStringList listTop(const QString& dir)
{
    QStringList l = QDir(dir).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    l.sort();
    return l;
}

bool copyFolder(const QString& from, const QString& to)
{
    if (!QDir().mkpath(to))
        return false;
    QDirIterator it(from, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString p = it.next();
        const QString target = QDir(to).filePath(QDir(from).relativeFilePath(p));
        if (it.fileInfo().isDir() ? !QDir().mkpath(target) : !QFile::copy(p, target))
            return false;
    }
    return true;
}

QString freshDir(const QString& path)
{
    (void)removeTreeSafely(path);
    QDir().mkpath(path);
    return path;
}

// ---- manifests ------------------------------------------------------------------

struct Entry {
    bool dir = false;
    qint64 size = 0;
    QByteArray sha; // empty: any content (a file the app adds, as documented)
};
using Manifest = QMap<QString, Entry>;

Manifest manifestOf(const QString& root, bool hashes = true)
{
    Manifest m;
    QDirIterator it(root, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString p = it.next();
        const QFileInfo fi = it.fileInfo();
        Entry e;
        e.dir = fi.isDir();
        if (!e.dir) {
            e.size = fi.size();
            if (hashes)
                e.sha = sha256Of(p);
        }
        m.insert(QDir(root).relativeFilePath(p), e);
    }
    return m;
}

// "" when `actual` is what `expected` describes; otherwise the first
// differences, one per line.
QString diffManifests(const Manifest& expected, const Manifest& actual)
{
    QStringList out;
    for (auto it = expected.begin(); it != expected.end(); ++it) {
        const auto a = actual.constFind(it.key());
        if (a == actual.cend()) {
            out << QStringLiteral("missing: ") + it.key();
            continue;
        }
        if (a->dir != it->dir)
            out << QStringLiteral("kind differs: ") + it.key();
        else if (!it->dir && !it->sha.isEmpty() && (a->size != it->size || a->sha != it->sha))
            out << QStringLiteral("content differs: %1 (%2 bytes expected, %3 found)").arg(it.key()).arg(it->size).arg(a->size);
    }
    for (auto it = actual.begin(); it != actual.end(); ++it)
        if (!expected.contains(it.key()))
            out << QStringLiteral("not expected: %1%2").arg(it.key(), it->dir ? QStringLiteral("/") : QStringLiteral(" (%1 bytes)").arg(it->size));
    if (out.size() > 12) {
        const qsizetype n = out.size();
        out = out.mid(0, 12);
        out << QStringLiteral("... %1 differences in all").arg(n);
    }
    return out.join(u'\n');
}

// Folders of `m` that hold nothing.
QStringList emptyFolders(const Manifest& m)
{
    QStringList out;
    for (auto it = m.begin(); it != m.end(); ++it) {
        if (!it->dir)
            continue;
        const QString prefix = it.key() + u'/';
        const auto j = m.lowerBound(prefix);
        if (j == m.end() || !j.key().startsWith(prefix))
            out << it.key();
    }
    return out;
}

// ---- archives -------------------------------------------------------------------

bool zipFolder(const QString& folder, const QString& zipPath)
{
    zip::Writer w;
    if (!w.open(zipPath, true).ok())
        return false;
    QDirIterator it(folder, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden, QDirIterator::Subdirectories);
    QStringList entries;
    while (it.hasNext())
        entries << it.next();
    entries.sort();
    for (const QString& p : entries) {
        const QString rel = QDir(folder).relativeFilePath(p);
        const Status st = QFileInfo(p).isDir() ? w.addDirectory(rel) : w.addFile(rel, p, zip::Method::Deflate, {});
        if (!st.ok()) {
            qWarning() << "zip:" << st.error().toLogString();
            return false;
        }
    }
    return w.finish(true).ok();
}

// A ZIP of the given names (for names a Windows folder cannot hold).
bool zipItems(const QString& zipPath, const QList<std::pair<QString, QByteArray>>& items)
{
    zip::Writer w;
    if (!w.open(zipPath, true).ok())
        return false;
    for (const auto& [name, data] : items)
        if (!w.addData(name, data, zip::Method::Store).ok())
            return false;
    return w.finish(true).ok();
}

la_ssize_t appendToFile(struct archive*, void* client, const void* buffer, size_t length)
{
    QFile* f = static_cast<QFile*>(client);
    return f->write(static_cast<const char*>(buffer), qint64(length)) == qint64(length) ? la_ssize_t(length) : -1;
}

// 7z with libarchive's writer (as tst_archive_source): `items` are (name,
// data, isDir). Every file goes into one solid LZMA2 block.
bool write7z(const QString& path, const QList<std::tuple<QString, QByteArray, bool>>& items)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    struct archive* a = archive_write_new();
    bool ok = archive_write_set_format_7zip(a) == ARCHIVE_OK;
    ok = ok && archive_write_set_options(a, "7zip:compression=lzma2,7zip:compression-level=1") == ARCHIVE_OK;
    archive_write_set_bytes_in_last_block(a, 1);
    ok = ok && archive_write_open(a, &out, nullptr, appendToFile, nullptr) == ARCHIVE_OK;
    for (const auto& [name, data, isDir] : items) {
        if (!ok)
            break;
        struct archive_entry* e = archive_entry_new();
        archive_entry_copy_pathname_w(e, name.toStdWString().c_str());
        archive_entry_set_mtime(e, time_t(1717243200), 0);
        if (isDir) {
            archive_entry_set_filetype(e, AE_IFDIR);
            archive_entry_set_perm(e, 0755);
        } else {
            archive_entry_set_filetype(e, AE_IFREG);
            archive_entry_set_perm(e, 0644);
            archive_entry_set_size(e, data.size());
        }
        ok = archive_write_header(a, e) == ARCHIVE_OK;
        if (ok && !isDir && !data.isEmpty())
            ok = archive_write_data(a, data.constData(), size_t(data.size())) == la_ssize_t(data.size());
        archive_entry_free(e);
    }
    ok = archive_write_close(a) == ARCHIVE_OK && ok;
    archive_write_free(a);
    return ok && out.flush();
}

bool sevenZipFolder(const QString& folder, const QString& path)
{
    QList<std::tuple<QString, QByteArray, bool>> items;
    QDirIterator it(folder, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden, QDirIterator::Subdirectories);
    QStringList entries;
    while (it.hasNext())
        entries << it.next();
    entries.sort();
    for (const QString& p : entries) {
        const bool dir = QFileInfo(p).isDir();
        items << std::make_tuple(QDir(folder).relativeFilePath(p), dir ? QByteArray() : fileBytes(p), dir);
    }
    return write7z(path, items);
}

// ---- fixtures -------------------------------------------------------------------

const QString kParam = QStringLiteral(
    "{\"titleId\":\"PPSA04242\",\"contentId\":\"UP0000-PPSA04242_00-MATRIXTEST000000\",\"contentVersion\":\"01.000.000\","
    "\"applicationCategoryType\":0,\"applicationDrmType\":\"free\",\"localizedParameters\":{\"defaultLanguage\":\"en-US\","
    "\"en-US\":{\"titleName\":\"Matrix Test\"}}}");

// A long name component (48 characters) of the long-path shape.
QString longComponent(int i)
{
    return QStringLiteral("long-folder-name-%1-").arg(i, 2, 10, QLatin1Char('0')).leftJustified(48, u'x');
}

// The difficult inputs, each on top of the same small game.
const QStringList kShapes = {QStringLiteral("basic"),    QStringLiteral("empty-folders"), QStringLiteral("deep"),
                             QStringLiteral("longpath"), QStringLiteral("unicode"),       QStringLiteral("zero-byte")};

bool makeShape(const QString& d, const QString& shape, qint64 bigBytes = 0)
{
    bool ok = put(d, QStringLiteral("eboot.bin"), QByteArray("\x54\x14\xF5\xEE", 4) + noise(60000, 1))
           && put(d, QStringLiteral("sce_sys/param.json"), kParam.toUtf8())
           && put(d, QStringLiteral("sce_sys/keystone"), QByteArray(96, 'K'))
           && put(d, QStringLiteral("sce_module/libc.prx"), QByteArray("\x54\x14\xF5\xEE", 4) + noise(2000, 2))
           && put(d, QStringLiteral("data/level1.bin"), noise(300 * kKiB, 3))
           && put(d, QStringLiteral("data/strings/en.txt"), compressible(200 * kKiB, 4))
           && put(d, QStringLiteral("readme.txt"), "hello");
    if (shape == u"empty-folders") {
        for (const char* p : {"empty", "data/empty-a", "a/b/c"})
            ok = ok && QDir().mkpath(QDir(d).filePath(QLatin1String(p)));
    } else if (shape == u"deep") {
        QString p;
        for (int i = 0; i < 30; ++i)
            p += QStringLiteral("n%1/").arg(i, 2, 10, QLatin1Char('0'));
        ok = ok && put(d, p + QStringLiteral("leaf.bin"), noise(1000, 5)) && put(d, QStringLiteral("n00/n01/mid.bin"), "mid");
    } else if (shape == u"longpath") {
        QString p;
        for (int i = 0; i < 6; ++i)
            p += longComponent(i) + u'/';
        ok = ok && put(d, p + QStringLiteral("a-file-with-a-long-name-too.bin"), noise(5000, 6));
    } else if (shape == u"unicode") {
        ok = ok && put(d, QStringLiteral("データ/日本語ファイル.bin"), noise(3000, 7))
             && put(d, QStringLiteral("café.txt"), "combining acute") // e + U+0301, kept decomposed
             && put(d, QStringLiteral("emoji_\U0001F600/\U0001F3AE.bin"), noise(1000, 8))
             && put(d, QStringLiteral("Ελληνικά/κείμενο.txt"), "greek") && put(d, QStringLiteral("한국어.dat"), "hangul")
             && put(d, QStringLiteral("中文/简体.txt"), "cjk");
    } else if (shape == u"zero-byte") {
        ok = ok && put(d, QStringLiteral("data/empty.bin"), QByteArray()) && put(d, QStringLiteral("zero.dat"), QByteArray());
    } else if (shape == u"cancel") {
        // Enough work in every stage for a cancel to land inside it.
        for (int i = 0; i < 48; ++i)
            ok = ok && put(d, QStringLiteral("data/c/f%1.bin").arg(i, 2, 10, QLatin1Char('0')), noise(256 * kKiB, 100 + quint32(i)));
    } else if (shape == u"compressible") {
        // Every block compresses, so damage to an .ffpfsc lands in zlib data.
        for (int i = 0; i < 4; ++i)
            ok = ok && put(d, QStringLiteral("data/z%1.bin").arg(i), compressible(512 * kKiB, char(i)));
    } else if (shape == u"big") {
        // Sparse: a file just over the size asked for, data at both ends.
        const QString path = QDir(d).filePath(QStringLiteral("data/big.bin"));
        QDir().mkpath(QFileInfo(path).absolutePath());
#ifdef Q_OS_WIN
        HANDLE h = CreateFileW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            return false;
        DWORD ret = 0;
        DeviceIoControl(h, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &ret, nullptr);
        LARGE_INTEGER li;
        li.QuadPart = bigBytes;
        const bool sized = SetFilePointerEx(h, li, nullptr, FILE_BEGIN) && SetEndOfFile(h);
        CloseHandle(h);
        ok = ok && sized;
#else
        {
            QFile f(path);
            ok = ok && f.open(QIODevice::WriteOnly) && f.resize(bigBytes);
        }
#endif
        QFile f(path);
        ok = ok && f.open(QIODevice::ReadWrite) && f.write(noise(1 * kMiB, 9)) == kMiB && f.seek(bigBytes - 4096)
             && f.write(noise(4096, 10)) == 4096;
    }
    return ok;
}

// ---- formats ----------------------------------------------------------------------

struct Fmt {
    QString id;          // exfat, ffpkg, ffpfs, ffpfsc, packed, zstd-exfat, ...
    QString buildKind;
    QString ext;         // ".exfat"..., "" for a folder
    QString extractKind; // the format's own extractor
    QString inner;       // zstd: the image format inside
    bool pfs() const { return ext == u".ffpfs" || ext == u".ffpfsc"; }
    bool zstd() const { return id == u"packed" || id.startsWith(u"zstd-"); }
};

const QList<Fmt>& formats()
{
    static const QList<Fmt> f = {
        {QStringLiteral("exfat"), exfat::kJobBuild, QStringLiteral(".exfat"), exfat::kJobExtract, {}},
        {QStringLiteral("ffpkg"), ufs2::kinds::kBuild, QStringLiteral(".ffpkg"), ufs2::kinds::kExtract, {}},
        {QStringLiteral("ffpfs"), pfs::kinds::kBuild, QStringLiteral(".ffpfs"), pfs::kinds::kExtract, {}},
        {QStringLiteral("ffpfsc"), pfs::kinds::kBuild, QStringLiteral(".ffpfsc"), pfs::kinds::kExtract, {}},
        {QStringLiteral("packed"), ampr::kPackJob, QString(), ampr::kUnpackJob, {}},
        {QStringLiteral("zstd-exfat"), integration::kZstdBuildJob, QStringLiteral(".exfat"), integration::kGameExtractJob, QStringLiteral("exfat")},
        {QStringLiteral("zstd-ffpkg"), integration::kZstdBuildJob, QStringLiteral(".ffpkg"), integration::kGameExtractJob, QStringLiteral("ffpkg")},
        {QStringLiteral("zstd-ffpfs"), integration::kZstdBuildJob, QStringLiteral(".ffpfs"), integration::kGameExtractJob, QStringLiteral("ffpfs")},
        {QStringLiteral("zstd-ffpfsc"), integration::kZstdBuildJob, QStringLiteral(".ffpfsc"), integration::kGameExtractJob, QStringLiteral("ffpfsc")},
    };
    return f;
}

const Fmt& fmt(const QString& id)
{
    for (const Fmt& f : formats())
        if (f.id == id)
            return f;
    qFatal("unknown format %s", qPrintable(id));
    return formats().front();
}

QString outputIn(const Fmt& f, const QString& dir, const QString& name = QStringLiteral("game"))
{
    return QDir(dir).filePath(name + f.ext);
}

QJsonObject buildParams(const Fmt& f, const QString& source, const QString& output)
{
    QJsonObject p{{QStringLiteral("source"), source}, {QStringLiteral("output"), output}};
    if (f.buildKind == pfs::kinds::kBuild)
        p.insert(QStringLiteral("format"), f.ext.mid(1));
    if (!f.inner.isEmpty())
        p.insert(QStringLiteral("format"), f.inner);
    return p;
}

QJsonObject extractParams(const QString& kind, const QString& image, const QString& output)
{
    if (kind == ampr::kUnpackJob || kind == integration::kGameExtractJob)
        return {{QStringLiteral("source"), image}, {QStringLiteral("output"), output}};
    QJsonObject p{{QStringLiteral("image"), image}, {QStringLiteral("output"), output}};
    if (kind == pfs::kinds::kExtract)
        p.insert(QStringLiteral("deep"), true);
    return p;
}

QString extractKindForImage(const QString& image)
{
    if (image.endsWith(u".exfat"))
        return exfat::kJobExtract;
    if (image.endsWith(u".ffpkg"))
        return ufs2::kinds::kExtract;
    return pfs::kinds::kExtract;
}

QString runtimePath()
{
    return qEnvironmentVariable("FW_AMPR_RUNTIME");
}

// What an extraction of `f` built from a game whose files are `source` holds,
// by the documentation:
//  * PFS images keep an empty folder with an empty .pfskeep file in it
//    (pfs.keepEmptyFolders, on by default; "This adds an empty .pfskeep file
//    to each empty folder so it survives").
//  * Packing puts the bundled ampr-emu-ng in fakelib/libSceAmpr.sprx and
//    writes a fresh ampr_emu.index (docs/ZSTD_PACKS.md, Rules).
Manifest expectedAfter(const Fmt& f, const Manifest& source)
{
    Manifest m = source;
    if (f.pfs() || f.inner == u"ffpfs" || f.inner == u"ffpfsc") {
        // An empty file: the SHA-256 of nothing.
        const QByteArray none = QByteArrayLiteral("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
        for (const QString& d : emptyFolders(source))
            m.insert(d + QStringLiteral("/.pfskeep"), Entry{false, 0, none});
    }
    if (f.zstd()) {
        m.insert(QStringLiteral("fakelib"), Entry{true, 0, {}});
        const QString rt = runtimePath();
        Entry e{false, QFileInfo(rt).size(), rt.isEmpty() ? QByteArray() : sha256Of(rt)};
        m.insert(QStringLiteral("fakelib/libSceAmpr.sprx"), e);
        m.insert(QStringLiteral("ampr_emu.index"), Entry{false, 0, {}});
    }
    return m;
}

// The documented refusal for a format and a shape, "" when the build must work.
// A zstd build into PFS is not refused for the Unicode shape: its non-ASCII
// files are all packed (only eboot, executables, sce_sys, sce_module, fakelib
// and empty files stay loose, ZSTD_PACKS.md), so the image holds ASCII names.
QString refusalFor(const Fmt& f, const QString& shape)
{
    if (f.pfs() && f.inner.isEmpty() && shape == u"unicode")
        return QStringLiteral("PFS images only support plain ASCII file names"); // PFS.md, Directory entry: "Names are ASCII only"
    return {};
}

// ---- an LZ4-packed game (AMPRPAK4), hand-built as in tst_ampr --------------------

void put32(QByteArray& b, int off, quint32 v)
{
    for (int i = 0; i < 4; ++i)
        b[off + i] = char((v >> (8 * i)) & 0xFF);
}
void put64(QByteArray& b, int off, quint64 v)
{
    for (int i = 0; i < 8; ++i)
        b[off + i] = char((v >> (8 * i)) & 0xFF);
}
quint32 crc(const char* d, qint64 n) { return ampr::crc32(d, n); }

// One packed file (`p0`, two 64 KiB-block chunks, the second raw) and one
// loose record (eboot.bin), with the CRC sidecar. Returns the packed data.
QByteArray buildLz4Game(const QString& game, const QByteArray& p0)
{
    if (!fwtest::writeTextFile(game + QStringLiteral("/eboot.bin"), QByteArray("\x54\x14\xF5\xEE", 4))
        || !fwtest::writeTextFile(game + QStringLiteral("/sce_sys/param.json"), kParam.toUtf8()))
        return {};
    const QByteArray data = compressible(65536, 4) + noise(30000, 5);
    const QByteArray c0 = data.left(65536);
    QByteArray z0(LZ4_compressBound(65536), '\0');
    const int z0n = LZ4_compress_default(c0.constData(), z0.data(), 65536, int(z0.size()));
    if (z0n <= 0 || z0n >= 65536)
        return {};
    z0.truncate(z0n);
    const QByteArray c1 = data.mid(65536);
    const quint32 page = 4096;
    const quint64 payloadOff = page;
    const quint64 off0 = payloadOff;
    const quint64 off1 = ((off0 + quint64(z0n) + 63) / 64) * 64;
    quint64 fileSize = off1 + quint64(c1.size());
    fileSize = ((fileSize + page - 1) / page) * page;
    const quint64 payload = fileSize - payloadOff;
    const QByteArray buildId(16, 'B');

    QByteArray vol(qsizetype(fileSize), '\0');
    std::memcpy(vol.data(), "AMPRDAT3", 8);
    put32(vol, 8, 3);
    put32(vol, 12, 64);
    put32(vol, 16, 0);
    put32(vol, 20, 2);
    std::memcpy(vol.data() + 24, buildId.constData(), 16);
    put64(vol, 40, payloadOff);
    put64(vol, 48, payload);
    put32(vol, 56, crc(vol.constData(), 64));
    std::memcpy(vol.data() + off0, z0.constData(), size_t(z0n));
    std::memcpy(vol.data() + off1, c1.constData(), size_t(c1.size()));
    if (!fwtest::writeTextFile(game + QStringLiteral("/ampr_assets-000.pak"), vol))
        return {};

    const QByteArray p1 = "/app0/eboot.bin";
    const QByteArray v0 = "ampr_assets-000.pak";
    const QByteArray strings = p0 + '\0' + p1 + '\0' + v0 + '\0';
    const quint64 fc = 2, cc = 2, pc = 1;
    const quint64 filesOff = 128, chunksOff = filesOff + 48 * fc, packsOff = chunksOff + 12 * cc, stringsOff = packsOff + 32 * pc;
    QByteArray m(qsizetype(stringsOff + quint64(strings.size())), '\0');
    std::memcpy(m.data(), "AMPRPAK4", 8);
    put32(m, 8, 4);
    put32(m, 12, 128);
    put32(m, 20, 0x01020304);
    std::memcpy(m.data() + 24, buildId.constData(), 16);
    put64(m, 40, fc);
    put64(m, 48, cc);
    put32(m, 56, quint32(pc));
    put32(m, 60, 48);
    put32(m, 64, 12);
    put32(m, 68, 32);
    put64(m, 72, filesOff);
    put64(m, 80, chunksOff);
    put64(m, 88, packsOff);
    put64(m, 96, stringsOff);
    put64(m, 104, quint64(strings.size()));
    int f = int(filesOff);
    put64(m, f + 8, quint64(data.size()));
    put32(m, f + 24, 0);
    put32(m, f + 28, 2);
    put32(m, f + 32, 0);
    put32(m, f + 36, quint32(p0.size()));
    put32(m, f + 40, 1);
    m[f + 44] = 16;
    f += 48;
    put32(m, f + 32, quint32(p0.size() + 1));
    put32(m, f + 36, quint32(p1.size()));
    put64(m, int(chunksOff), off0);
    put32(m, int(chunksOff) + 8, quint32(z0n - 1) | (1u << 20) | (4u << 22));
    put64(m, int(chunksOff) + 12, off1);
    put32(m, int(chunksOff) + 20, quint32(c1.size() - 1));
    put64(m, int(packsOff), payload);
    put64(m, int(packsOff) + 8, fileSize);
    put32(m, int(packsOff) + 16, quint32(p0.size() + 1 + p1.size() + 1));
    put32(m, int(packsOff) + 20, quint32(v0.size()));
    put32(m, int(packsOff) + 24, 2);
    put32(m, int(packsOff) + 28, page);
    std::memcpy(m.data() + stringsOff, strings.constData(), size_t(strings.size()));
    put32(m, 112, crc(m.constData() + 128, m.size() - 128));
    {
        QByteArray head = m.left(128);
        std::memset(head.data() + 116, 0, 4);
        put32(m, 116, crc(head.constData(), 128));
    }
    if (!fwtest::writeTextFile(game + QStringLiteral("/ampr_assets.index"), m))
        return {};
    QByteArray sidecar(qsizetype(48 + 4 * cc), '\0');
    std::memcpy(sidecar.data(), "AMPRCRC1", 8);
    put32(sidecar, 8, 1);
    put32(sidecar, 12, 48);
    std::memcpy(sidecar.data() + 16, buildId.constData(), 16);
    put64(sidecar, 32, cc);
    put32(sidecar, 48, crc(c0.constData(), c0.size()));
    put32(sidecar, 52, crc(c1.constData(), c1.size()));
    put32(sidecar, 40, crc(sidecar.constData() + 48, sidecar.size() - 48));
    if (!fwtest::writeTextFile(game + QStringLiteral("/ampr_assets.index.crc"), sidecar))
        return {};
    return data;
}

// ---- damage ---------------------------------------------------------------------

bool overwrite(const QString& path, qint64 offset, const QByteArray& bytes)
{
    QFile f(path);
    return f.open(QIODevice::ReadWrite) && f.seek(offset) && f.write(bytes) == bytes.size();
}

bool xorRange(const QString& path, qint64 offset, qint64 length)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadWrite) || !f.seek(offset))
        return false;
    QByteArray b = f.read(length);
    for (char& c : b)
        c = char(c ^ 0x5A);
    return f.seek(offset) && f.write(b) == b.size();
}

bool truncateTo(const QString& path, qint64 size)
{
    QFile f(path);
    return f.resize(size);
}

// The offset of `needle` in the file, -1 when absent.
qint64 findInFile(const QString& path, const QByteArray& needle)
{
    const QByteArray all = fileBytes(path);
    return all.indexOf(needle);
}

// ---- the FPKG stand-in toolkit -----------------------------------------------------

#ifdef Q_OS_WIN
void setEnvW(const char* name, const QString& value)
{
    _wputenv_s(reinterpret_cast<const wchar_t*>(QString::fromLatin1(name).utf16()), reinterpret_cast<const wchar_t*>(value.utf16()));
}
#else
void setEnvW(const char* name, const QString& value) { qputenv(name, value.toLocal8Bit()); }
#endif

} // namespace

class TestFormatMatrix : public QObject {
    Q_OBJECT

    QTemporaryDir m_root;
    std::unique_ptr<Env> m_env;
    QHash<QString, QString> m_fixtures; // shape -> game folder
    QHash<QString, QString> m_sources;  // shape/kind -> source path
    QHash<QString, QString> m_images;   // format/shape -> built image
    QString m_tools;
    qint64 m_bigBytes = 0;

    QString path(const QString& rel) const { return m_root.filePath(rel); }

    QString fixture(const QString& shape)
    {
        if (const auto it = m_fixtures.constFind(shape); it != m_fixtures.cend())
            return *it;
        const QString d = freshDir(path(QStringLiteral("fixtures/") + shape + QStringLiteral("/PPSA04242")));
        if (!makeShape(d, shape, m_bigBytes))
            return {};
        m_fixtures.insert(shape, d);
        return d;
    }

    // The game of `shape` as a folder, ZIP or 7z.
    QString sourceOf(const QString& shape, const QString& kind)
    {
        const QString game = fixture(shape);
        if (game.isEmpty() || kind == u"folder")
            return game;
        const QString key = shape + u'/' + kind;
        if (const auto it = m_sources.constFind(key); it != m_sources.cend())
            return *it;
        const QString file = path(QStringLiteral("sources/%1/PPSA04242.%2").arg(shape, kind));
        QDir().mkpath(QFileInfo(file).absolutePath());
        const bool ok = kind == u"zip" ? zipFolder(game, file) : sevenZipFolder(game, file);
        if (!ok)
            return {};
        m_sources.insert(key, file);
        return file;
    }

    // An image of `format` built from the folder of `shape` (cached).
    QString imageOf(const QString& format, const QString& shape)
    {
        const QString key = format + u'/' + shape;
        if (const auto it = m_images.constFind(key); it != m_images.cend())
            return *it;
        const Fmt& f = fmt(format);
        const QString game = fixture(shape);
        const QString out = outputIn(f, freshDir(path(QStringLiteral("images/%1-%2").arg(format, shape))), QStringLiteral("PPSA04242"));
        auto r = m_env->run(f.buildKind, buildParams(f, game, out));
        if (!succeeded(r)) {
            qWarning() << "could not build" << key << describe(r);
            return {};
        }
        m_images.insert(key, out);
        return out;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_root.isValid());
        fwtest::isolateAppData(m_root);
        // The FPKG engine and its stand-in tool use the system temporary
        // folder and %ProgramData%: both go under the test's own folder.
        qunsetenv("SCE_PROSPERO_PUBTOOLS");
        const QString sysTemp = path(QStringLiteral("sys-temp")), programData = path(QStringLiteral("program-data"));
        QVERIFY(QDir().mkpath(sysTemp) && QDir().mkpath(programData));
        for (const char* v : {"TMP", "TEMP", "TMPDIR"})
            setEnvW(v, QDir::toNativeSeparators(sysTemp));
        setEnvW("ProgramData", QDir::toNativeSeparators(programData));
        m_tools = path(QStringLiteral("tools"));
        QVERIFY(QDir().mkpath(QDir(m_tools).filePath(QStringLiteral("toolchain/ext"))));
        QVERIFY(QFile::copy(QStringLiteral(FW_FAKE_PUBCMD), QDir(m_tools).filePath(QStringLiteral("toolchain/prospero-pub-cmd.exe"))));
        QVERIFY(QFile::copy(QStringLiteral(FW_FAKE_PUBCMD), QDir(m_tools).filePath(QStringLiteral("prospero-dds2png.exe"))));
        QVERIFY(put(m_tools, QStringLiteral("toolchain/libScePubTools.dll"), ""));

        m_env = std::make_unique<Env>(path(QStringLiteral("env")));
        QVERIFY(m_env->store->setValue(fpkg::keys::kPubToolsPath, m_tools).ok());
        QVERIFY2(!runtimePath().isEmpty() && QFileInfo::exists(runtimePath()),
                 "FW_AMPR_RUNTIME must name the bundled ampr-emu-ng runtime (CTest sets it).");
        bool ok = false;
        const qint64 big = qEnvironmentVariableIntValue("FW_MATRIX_BIG_MIB", &ok);
        if (ok && big > 0)
            m_bigBytes = big * kMiB + 1;
    }

    void cleanupTestCase()
    {
        int pass = 0, fail = 0, xfail = 0, other = 0;
        QStringList lines{QStringLiteral("| Area | Row | Result | Note |"), QStringLiteral("|---|---|---|---|")};
        for (const RowResult& r : rowResults()) {
            if (r.result.startsWith(u"PASS"))
                ++pass;
            else if (r.result == u"FAIL")
                ++fail;
            else if (r.result.startsWith(u"XFAIL"))
                ++xfail;
            else
                ++other;
            QString note = r.note;
            note.replace(u'|', u'/').replace(u'\n', u' ');
            lines << QStringLiteral("| %1 | `%2` | %3 | %4 |").arg(r.area, r.row, r.result, note);
        }
        qInfo().noquote() << QStringLiteral("matrix: %1 rows, %2 pass, %3 fail, %4 expected failures, %5 other")
                                 .arg(rowResults().size())
                                 .arg(pass)
                                 .arg(fail)
                                 .arg(xfail)
                                 .arg(other);
        const QString report = qEnvironmentVariable("FW_MATRIX_REPORT");
        if (!report.isEmpty()) {
            QFile f(report);
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
                f.write((lines.join(u'\n') + u'\n').toUtf8());
        }
        m_env.reset();
    }

    // ---- source -> build -> extract -> compare ----------------------------------------

    void roundTrip_data()
    {
        QTest::addColumn<QString>("format");
        QTest::addColumn<QString>("source");
        QTest::addColumn<QString>("shape");
        for (const Fmt& f : formats())
            for (const QString& source : {QStringLiteral("folder"), QStringLiteral("zip"), QStringLiteral("7z")})
                for (const QString& shape : kShapes)
                    QTest::newRow(qPrintable(QStringLiteral("%1/%2/%3").arg(f.id, source, shape))) << f.id << source << shape;
        // The big run: a sparse file of FW_MATRIX_BIG_MIB MiB + 1 byte, folder sources only.
        for (const Fmt& f : formats())
            QTest::newRow(qPrintable(QStringLiteral("%1/folder/big").arg(f.id))) << f.id << QStringLiteral("folder") << QStringLiteral("big");
    }

    void roundTrip()
    {
        QFETCH(QString, format);
        QFETCH(QString, source);
        QFETCH(QString, shape);
        RowGuard g(QStringLiteral("round trip"));
        if (shape == u"big" && m_bigBytes == 0) {
            g.verdict = QStringLiteral("SKIP");
            g.note = QStringLiteral("big run only (FW_MATRIX_BIG_MIB)");
            QSKIP("Set FW_MATRIX_BIG_MIB (4096 for a file just over 4 GiB) for the big run.");
        }
        const Fmt& f = fmt(format);
        const QString game = fixture(shape);
        QVERIFY(!game.isEmpty());
        const Manifest before = manifestOf(game);
        const QString src = sourceOf(shape, source);
        QVERIFY(!src.isEmpty());
        const QByteArray srcHash = source == u"folder" ? QByteArray() : sha256Of(src);
        const QString tag = QString::fromUtf8(QTest::currentDataTag()).replace(u'/', u'-');
        const QString buildDir = freshDir(path(QStringLiteral("rt/%1/build").arg(tag)));
        const QString out = outputIn(f, buildDir);

        auto built = m_env->run(f.buildKind, buildParams(f, src, out));
        const QString refusal = refusalFor(f, shape);
        if (!refusal.isEmpty()) {
            MCHECK(g, "build", built && built->state == JobState::Failed && built->error && built->error->message.contains(refusal),
                   describe(built));
            MCHECK(g, "leftover", listTop(buildDir).isEmpty(), listTop(buildDir).join(u", "));
            g.verdict = QStringLiteral("PASS (refused as documented)");
            g.note = refusal;
            return;
        }
        MCHECK(g, "build", succeeded(built) && built->outcome->verification == Verification::Passed, describe(built));
        MCHECK(g, "leftover", listTop(buildDir) == QStringList{QFileInfo(out).fileName()}, listTop(buildDir).join(u", "));
        if (source == u"folder")
            MCHECK(g, "source", diffManifests(before, manifestOf(game)).isEmpty(), QStringLiteral("the source folder changed"));
        else
            MCHECK(g, "source", sha256Of(src) == srcHash, QStringLiteral("the archive changed"));

        const Manifest expected = expectedAfter(f, before);
        // The format's own extractor.
        const QString x1 = freshDir(path(QStringLiteral("rt/%1/x1").arg(tag)));
        auto ex = m_env->run(f.extractKind, extractParams(f.extractKind, out, x1 + QStringLiteral("/game")));
        MCHECK(g, "extract", succeeded(ex), describe(ex));
        MCHECK(g, "extract-leftover", listTop(x1) == QStringList{QStringLiteral("game")}, listTop(x1).join(u", "));
        const QString d1 = diffManifests(expected, manifestOf(x1 + QStringLiteral("/game")));
        MCHECK(g, "compare", d1.isEmpty(), d1);
        // And through game.extract, which reads every format (packs unpacked).
        if (f.extractKind != integration::kGameExtractJob) {
            const QString x2 = freshDir(path(QStringLiteral("rt/%1/x2").arg(tag)));
            auto ex2 = m_env->run(integration::kGameExtractJob, extractParams(integration::kGameExtractJob, out, x2 + QStringLiteral("/game")));
            MCHECK(g, "extract2", succeeded(ex2), describe(ex2));
            const QString d2 = diffManifests(expected, manifestOf(x2 + QStringLiteral("/game")));
            MCHECK(g, "compare2", d2.isEmpty(), d2);
        }
        g.note = QStringLiteral("%1 files").arg(std::count_if(before.begin(), before.end(), [](const Entry& e) { return !e.dir; }));
        // Disk space: a big row's outputs go as soon as the row is done.
        if (shape == u"big")
            (void)removeTreeSafely(path(QStringLiteral("rt/%1").arg(tag)));
    }

    // ---- image -> other image (image.convert) -------------------------------------------

    void conversions_data()
    {
        QTest::addColumn<QString>("from");
        QTest::addColumn<QString>("to");
        QTest::addColumn<QString>("method");
        QTest::addColumn<QString>("shape");
        const QStringList images = {QStringLiteral("exfat"), QStringLiteral("ffpkg"), QStringLiteral("ffpfs"), QStringLiteral("ffpfsc")};
        for (const QString& shape : {QStringLiteral("basic"), QStringLiteral("empty-folders"), QStringLiteral("unicode")})
            for (const QString& from : images)
                for (const QString& to : images)
                    if (from != to)
                        QTest::newRow(qPrintable(QStringLiteral("%1->%2/repack/%3").arg(from, to, shape))) << from << to << QStringLiteral("repack") << shape;
        for (const QString& shape : {QStringLiteral("basic"), QStringLiteral("unicode")})
            for (const QString& from : {QStringLiteral("exfat"), QStringLiteral("ffpkg")})
                QTest::newRow(qPrintable(QStringLiteral("%1->ffpfsc/direct/%2").arg(from, shape))) << from << QStringLiteral("ffpfsc")
                                                                                                    << QStringLiteral("direct") << shape;
    }

    void conversions()
    {
        QFETCH(QString, from);
        QFETCH(QString, to);
        QFETCH(QString, method);
        QFETCH(QString, shape);
        RowGuard g(QStringLiteral("conversion"));
        const Fmt& ff = fmt(from);
        const Fmt& tf = fmt(to);
        if (!refusalFor(ff, shape).isEmpty()) {
            g.verdict = QStringLiteral("SKIP");
            g.note = QStringLiteral("no .%1 of this shape (refused as documented)").arg(from);
            QSKIP("The source format cannot hold this shape (see roundTrip).");
        }
        const QString image = imageOf(from, shape);
        QVERIFY(!image.isEmpty());
        const QByteArray imageHash = sha256Of(image);
        // What the source image holds, as its own extractor gives it back
        // (already checked against the game in roundTrip).
        const QString ref = freshDir(path(QStringLiteral("conv-ref/%1-%2").arg(from, shape)));
        auto rx = m_env->run(ff.extractKind, extractParams(ff.extractKind, image, ref + QStringLiteral("/game")));
        QVERIFY2(succeeded(rx), qPrintable(describe(rx)));
        const Manifest held = manifestOf(ref + QStringLiteral("/game"));

        const QString tag = QString::fromUtf8(QTest::currentDataTag()).replace(u'/', u'-').replace(QStringLiteral("->"), QStringLiteral("-to-"));
        const QString dir = freshDir(path(QStringLiteral("conv/%1").arg(tag)));
        const QString out = outputIn(tf, dir);
        QJsonObject params{{QStringLiteral("image"), image}, {QStringLiteral("format"), to}, {QStringLiteral("output"), out}};
        if (method == u"repack")
            params.insert(QStringLiteral("method"), method);
        auto r = m_env->run(integration::kConvertJob, params);
        MCHECK(g, "source-image", sha256Of(image) == imageHash, QStringLiteral("the source image changed"));
        // A repack builds the target from the source's files as they are
        // (runBuild asItIs): the target's own rules decide what it can hold.
        const QString refusal = method == u"repack" ? refusalFor(tf, shape) : QString();
        if (!refusal.isEmpty()) {
            MCHECK(g, "convert", r && r->state == JobState::Failed && r->error && r->error->message.contains(refusal), describe(r));
            MCHECK(g, "leftover", listTop(dir).isEmpty(), listTop(dir).join(u", "));
            g.verdict = QStringLiteral("PASS (refused as documented)");
            g.note = refusal;
            return;
        }
        MCHECK(g, "convert", succeeded(r) && r->outcome->verification == Verification::Passed, describe(r));
        MCHECK(g, "leftover", listTop(dir) == QStringList{QFileInfo(out).fileName()}, listTop(dir).join(u", "));

        if (method == u"direct") {
            // The container holds the image file itself, under an ASCII name;
            // game.extract opens it through to the image's files.
            const QString x = freshDir(path(QStringLiteral("conv/%1-x").arg(tag)));
            auto ex = m_env->run(integration::kGameExtractJob, extractParams(integration::kGameExtractJob, out, x + QStringLiteral("/game")));
            MCHECK(g, "extract", succeeded(ex), describe(ex));
            const QString d = diffManifests(held, manifestOf(x + QStringLiteral("/game")));
            MCHECK(g, "compare", d.isEmpty(), d);
            // And back: the container converts to the image it holds, byte for byte.
            const QString back = QDir(dir).filePath(QStringLiteral("back") + ff.ext);
            auto b = m_env->run(integration::kConvertJob,
                                {{QStringLiteral("image"), out}, {QStringLiteral("format"), from}, {QStringLiteral("output"), back}});
            MCHECK(g, "back", succeeded(b), describe(b));
            MCHECK(g, "back-bytes", sha256Of(back) == imageHash, QStringLiteral("the image unpacked from the container is not the original"));
            g.note = QStringLiteral("and back to .%1, byte-identical").arg(from);
            return;
        }
        const QString x = freshDir(path(QStringLiteral("conv/%1-x").arg(tag)));
        auto ex = m_env->run(tf.extractKind, extractParams(tf.extractKind, out, x + QStringLiteral("/game")));
        MCHECK(g, "extract", succeeded(ex), describe(ex));
        // PFS keeps an empty folder with a .pfskeep marker; that marker is a
        // file of the image it came from (and comes out as one).
        const Manifest expected = expectedAfter(tf, held);
        const QString d = diffManifests(expected, manifestOf(x + QStringLiteral("/game")));
        MCHECK(g, "compare", d.isEmpty(), d);
    }

    // ---- edits ----------------------------------------------------------------------

    void editRoundTrip_data()
    {
        QTest::addColumn<QString>("format");
        QTest::addColumn<QString>("strategy");
        QTest::newRow("exfat/auto") << QStringLiteral("exfat") << QStringLiteral("auto");
        QTest::newRow("exfat/rebuild") << QStringLiteral("exfat") << QStringLiteral("rebuild");
        QTest::newRow("exfat/in-place") << QStringLiteral("exfat") << QStringLiteral("inplace");
        QTest::newRow("ffpkg/rebuild") << QStringLiteral("ffpkg") << QStringLiteral("rebuild");
    }

    void editRoundTrip()
    {
        QFETCH(QString, format);
        QFETCH(QString, strategy);
        RowGuard g(QStringLiteral("edit"));
        const Fmt& f = fmt(format);
        const QString tag = QString::fromUtf8(QTest::currentDataTag()).replace(u'/', u'-');
        const QString dir = freshDir(path(QStringLiteral("edit/%1").arg(tag)));
        // The expected tree: a copy of the game with the same changes made on disk.
        const QString expectedDir = dir + QStringLiteral("/expected");
        QVERIFY(makeShape(expectedDir, QStringLiteral("unicode")));
        const QString image = outputIn(f, dir);
        auto built = m_env->run(f.buildKind, buildParams(f, expectedDir, image));
        QVERIFY2(succeeded(built), qPrintable(describe(built)));

        const QString local = freshDir(dir + QStringLiteral("/local"));
        QVERIFY(put(local, QStringLiteral("added.bin"), noise(70000, 20)));
        QVERIFY(put(local, QStringLiteral("bigger.bin"), noise(400 * kKiB, 21)));
        QVERIFY(put(local, QStringLiteral("same-size.bin"), noise(300 * kKiB, 22)));
        QVERIFY(put(local, QStringLiteral("folder/one.txt"), "one"));
        QVERIFY(put(local, QStringLiteral("folder/ñandú/two.txt"), "two"));
        QJsonArray ops;
        const bool exfatOps = format == u"exfat";
        const auto op = [&](const char* kind, const QString& imagePath, const QString& localPath = {}) {
            QJsonObject o{{QStringLiteral("op"), QLatin1String(kind)}};
            o.insert(exfatOps ? QStringLiteral("imagePath") : QStringLiteral("path"), exfatOps ? imagePath : u'/' + imagePath);
            if (!localPath.isEmpty())
                o.insert(exfatOps ? QStringLiteral("localPath") : QStringLiteral("source"), localPath);
            ops.append(o);
        };
        if (strategy == u"inplace") {
            // Provably safe in place: a delete and a replacement that fits.
            op("delete", QStringLiteral("readme.txt"));
            op("replace", QStringLiteral("data/level1.bin"), QDir(local).filePath(QStringLiteral("same-size.bin")));
            QVERIFY(QFile::remove(QDir(expectedDir).filePath(QStringLiteral("readme.txt"))));
            QVERIFY(QFile::remove(QDir(expectedDir).filePath(QStringLiteral("data/level1.bin"))));
            QVERIFY(QFile::copy(QDir(local).filePath(QStringLiteral("same-size.bin")), QDir(expectedDir).filePath(QStringLiteral("data/level1.bin"))));
        } else {
            op("add", QStringLiteral("data/added.bin"), QDir(local).filePath(QStringLiteral("added.bin")));
            op("add", QStringLiteral("データ/folder"), QDir(local).filePath(QStringLiteral("folder")));
            op("replace", QStringLiteral("data/level1.bin"), QDir(local).filePath(QStringLiteral("bigger.bin")));
            op("delete", QStringLiteral("readme.txt"));
            op("delete", QStringLiteral("emoji_\U0001F600"));
            op("mkdir", QStringLiteral("new/empty/folder"));
            QVERIFY(QFile::copy(QDir(local).filePath(QStringLiteral("added.bin")), QDir(expectedDir).filePath(QStringLiteral("data/added.bin"))));
            QVERIFY(put(expectedDir, QStringLiteral("データ/folder/one.txt"), "one"));
            QVERIFY(put(expectedDir, QStringLiteral("データ/folder/ñandú/two.txt"), "two"));
            QVERIFY(QFile::remove(QDir(expectedDir).filePath(QStringLiteral("data/level1.bin"))));
            QVERIFY(QFile::copy(QDir(local).filePath(QStringLiteral("bigger.bin")), QDir(expectedDir).filePath(QStringLiteral("data/level1.bin"))));
            QVERIFY(QFile::remove(QDir(expectedDir).filePath(QStringLiteral("readme.txt"))));
            QVERIFY(removeTreeSafely(QDir(expectedDir).filePath(QStringLiteral("emoji_\U0001F600"))).ok());
            QVERIFY(QDir().mkpath(QDir(expectedDir).filePath(QStringLiteral("new/empty/folder"))));
            if (format == u"ffpkg") {
                op("rename", QStringLiteral("中文"), {});
                ops[ops.size() - 1] = QJsonObject{{QStringLiteral("op"), QStringLiteral("rename")},
                                                  {QStringLiteral("path"), QStringLiteral("/中文")},
                                                  {QStringLiteral("newName"), QStringLiteral("中文-renamed")}};
                QVERIFY(QDir(expectedDir).rename(QStringLiteral("中文"), QStringLiteral("中文-renamed")));
            }
        }
        QVariantMap overrides;
        if (format == u"exfat")
            overrides.insert(exfat::keys::kEditStrategy, strategy == u"rebuild" ? QStringLiteral("rebuild") : QStringLiteral("auto"));
        const QString editKind = format == u"exfat" ? exfat::kJobEdit : ufs2::kinds::kEdit;
        auto r = m_env->runDirect(editKind, {{QStringLiteral("image"), image}, {QStringLiteral("operations"), ops}}, overrides);
        MCHECK(g, "edit", r.ok() && r->verification == Verification::Passed, describe(r));
        if (strategy == u"inplace")
            MCHECK(g, "in-place", r->details.value(QStringLiteral("inPlace")).toBool(), QStringLiteral("the edit was not made in place"));
        else if (format == u"exfat")
            MCHECK(g, "rebuilt", !r->details.value(QStringLiteral("inPlace")).toBool(), QStringLiteral("an add was made in place"));
        MCHECK(g, "leftover", listTop(dir) == (QStringList{QStringLiteral("expected"), QFileInfo(image).fileName(), QStringLiteral("local")}),
               listTop(dir).join(u", "));
        const QString x = freshDir(path(QStringLiteral("edit/%1-x").arg(tag)));
        auto ex = m_env->run(f.extractKind, extractParams(f.extractKind, image, x + QStringLiteral("/game")));
        MCHECK(g, "extract", succeeded(ex), describe(ex));
        const QString d = diffManifests(manifestOf(expectedDir), manifestOf(x + QStringLiteral("/game")));
        MCHECK(g, "compare", d.isEmpty(), d);
        g.note = QStringLiteral("%1 operations").arg(ops.size());
    }

    // ---- an existing output, with and without replace -----------------------------------

    void existingOutput_data()
    {
        QTest::addColumn<QString>("format");
        QTest::addColumn<bool>("replace");
        for (const Fmt& f : formats()) {
            QTest::newRow(qPrintable(f.id + QStringLiteral("/keep"))) << f.id << false;
            QTest::newRow(qPrintable(f.id + QStringLiteral("/replace"))) << f.id << true;
        }
        QTest::newRow("convert/keep") << QStringLiteral("convert") << false;
        QTest::newRow("convert/replace") << QStringLiteral("convert") << true;
        QTest::newRow("packed/empty-folder") << QStringLiteral("packed-empty") << true;
    }

    void existingOutput()
    {
        QFETCH(QString, format);
        QFETCH(bool, replace);
        RowGuard g(QStringLiteral("existing output"));
        const QString tag = QString::fromUtf8(QTest::currentDataTag()).replace(u'/', u'-');
        const QString dir = freshDir(path(QStringLiteral("existing/%1").arg(tag)));
        const QString game = fixture(QStringLiteral("basic"));
        QVERIFY(!game.isEmpty());

        if (format == u"packed" || format == u"packed-empty") {
            // ampr.pack: the output may be absent or an empty folder; a
            // folder with anything in it is refused and left as it is.
            const QString out = QDir(dir).filePath(QStringLiteral("packed"));
            QVERIFY(QDir().mkpath(out));
            if (format == u"packed")
                QVERIFY(put(out, QStringLiteral("mine.txt"), "mine"));
            auto r = m_env->runDirect(ampr::kPackJob, {{QStringLiteral("source"), game}, {QStringLiteral("output"), out}});
            if (format == u"packed") {
                MCHECK(g, "refused", !r.ok() && r.error().code == ErrorCode::AlreadyExists, describe(r));
                MCHECK(g, "untouched", listTop(out) == QStringList{QStringLiteral("mine.txt")} && fileBytes(QDir(out).filePath(QStringLiteral("mine.txt"))) == "mine",
                       listTop(out).join(u", "));
                g.note = QStringLiteral("a folder with files is never used (no replace option)");
            } else {
                MCHECK(g, "used", r.ok(), describe(r));
                MCHECK(g, "packed", QFileInfo::exists(QDir(out).filePath(QStringLiteral("ampr_zstd.index"))), listTop(out).join(u", "));
            }
            MCHECK(g, "leftover", listTop(dir) == QStringList{QStringLiteral("packed")}, listTop(dir).join(u", "));
            return;
        }

        QString kind;
        QJsonObject params;
        QString out;
        if (format == u"convert") {
            const QString image = imageOf(QStringLiteral("exfat"), QStringLiteral("basic"));
            QVERIFY(!image.isEmpty());
            out = QDir(dir).filePath(QStringLiteral("game.ffpkg"));
            kind = integration::kConvertJob;
            params = {{QStringLiteral("image"), image}, {QStringLiteral("format"), QStringLiteral("ffpkg")}, {QStringLiteral("output"), out}};
        } else {
            const Fmt& f = fmt(format);
            out = outputIn(f, dir);
            kind = f.buildKind;
            params = buildParams(f, game, out);
        }
        const QByteArray old("an older image the user keeps");
        QVERIFY(put(dir, QFileInfo(out).fileName(), old));
        // FFPKG, PFS and conversions take "replaceExisting"; the exFAT build
        // reads the exfat.overwriteExisting setting (a zstd build passes both on).
        params.insert(QStringLiteral("replaceExisting"), replace);
        const QVariantMap overrides{{exfat::keys::kOverwriteExisting, replace}};
        auto r = m_env->runDirect(kind, params, overrides);
        if (!replace) {
            MCHECK(g, "refused", !r.ok() && r.error().code == ErrorCode::AlreadyExists, describe(r));
            MCHECK(g, "untouched", fileBytes(out) == old, QStringLiteral("the existing output changed"));
        } else {
            MCHECK(g, "replaced", r.ok() && r->verification == Verification::Passed, describe(r));
            MCHECK(g, "new", fileBytes(out) != old && QFileInfo(out).size() > old.size(), QStringLiteral("the output was not replaced"));
            const QString x = freshDir(path(QStringLiteral("existing/%1-x").arg(tag)));
            const QString ek = format == u"convert" ? ufs2::kinds::kExtract : fmt(format).extractKind;
            auto ex = m_env->run(ek, extractParams(ek, out, x + QStringLiteral("/game")));
            MCHECK(g, "extract", succeeded(ex), describe(ex));
        }
        MCHECK(g, "leftover", listTop(dir) == QStringList{QFileInfo(out).fileName()}, listTop(dir).join(u", "));
    }

    // Every extractor refuses a folder that already holds something, and
    // leaves it as it was. PFS has an "overwrite" switch: then the image's
    // files go into it and the user's other files stay.
    void existingExtractFolder_data()
    {
        QTest::addColumn<QString>("format");
        QTest::addColumn<QString>("kind");
        QTest::addColumn<bool>("overwrite");
        QTest::newRow("exfat") << QStringLiteral("exfat") << exfat::kJobExtract << false;
        QTest::newRow("ffpkg") << QStringLiteral("ffpkg") << ufs2::kinds::kExtract << false;
        QTest::newRow("ffpfsc") << QStringLiteral("ffpfsc") << pfs::kinds::kExtract << false;
        QTest::newRow("ffpfsc/overwrite") << QStringLiteral("ffpfsc") << pfs::kinds::kExtract << true;
        QTest::newRow("game.extract") << QStringLiteral("ffpkg") << integration::kGameExtractJob << false;
        QTest::newRow("ampr.unpack") << QStringLiteral("packed") << ampr::kUnpackJob << false;
    }

    void existingExtractFolder()
    {
        QFETCH(QString, format);
        QFETCH(QString, kind);
        QFETCH(bool, overwrite);
        RowGuard g(QStringLiteral("existing extract folder"));
        const QString image = imageOf(format, QStringLiteral("basic"));
        QVERIFY(!image.isEmpty());
        const QString tag = QString::fromUtf8(QTest::currentDataTag()).replace(u'/', u'-');
        const QString dir = freshDir(path(QStringLiteral("existing-x/%1").arg(tag)));
        const QString out = QDir(dir).filePath(QStringLiteral("game"));
        QVERIFY(put(out, QStringLiteral("mine.txt"), "mine"));
        QJsonObject params = extractParams(kind, image, out);
        if (overwrite)
            params.insert(QStringLiteral("overwrite"), true);
        auto r = m_env->run(kind, params);
        if (!overwrite) {
            MCHECK(g, "refused", r && r->state == JobState::Failed && r->error && r->error->code == ErrorCode::AlreadyExists, describe(r));
            MCHECK(g, "untouched", listTop(out) == QStringList{QStringLiteral("mine.txt")}, listTop(out).join(u", "));
        } else {
            MCHECK(g, "extracted", succeeded(r), describe(r));
            Manifest expected = expectedAfter(fmt(format), manifestOf(fixture(QStringLiteral("basic"))));
            expected.insert(QStringLiteral("mine.txt"), Entry{false, 4, sha256Of(QDir(out).filePath(QStringLiteral("mine.txt")))});
            MCHECK(g, "kept", fileBytes(QDir(out).filePath(QStringLiteral("mine.txt"))) == "mine", QStringLiteral("the user's file changed"));
            const QString d = diffManifests(expected, manifestOf(out));
            MCHECK(g, "compare", d.isEmpty(), d);
        }
        MCHECK(g, "leftover", listTop(dir) == QStringList{QStringLiteral("game")}, listTop(dir).join(u", "));
    }

    // ---- damaged images and archives ------------------------------------------------------

    void corrupted_data()
    {
        QTest::addColumn<QString>("format");
        QTest::addColumn<QString>("damage");
        QTest::newRow("exfat/boot-sector-zeroed") << QStringLiteral("exfat") << QStringLiteral("zero:0:512");
        QTest::newRow("exfat/truncated") << QStringLiteral("exfat") << QStringLiteral("truncate:half");
        QTest::newRow("ffpkg/superblock-zeroed") << QStringLiteral("ffpkg") << QStringLiteral("zero:65536:8192");
        QTest::newRow("ffpkg/truncated") << QStringLiteral("ffpkg") << QStringLiteral("truncate:half");
        QTest::newRow("ffpfs/header-zeroed") << QStringLiteral("ffpfs") << QStringLiteral("zero:0:4096");
        QTest::newRow("ffpfs/truncated") << QStringLiteral("ffpfs") << QStringLiteral("truncate:half");
        QTest::newRow("ffpfsc/header-zeroed") << QStringLiteral("ffpfsc") << QStringLiteral("zero:0:4096");
        QTest::newRow("ffpfsc/truncated") << QStringLiteral("ffpfsc") << QStringLiteral("truncate:half");
        QTest::newRow("ffpfsc/compressed-blocks-damaged") << QStringLiteral("ffpfsc") << QStringLiteral("xor:middle");
        QTest::newRow("packed/volume-damaged") << QStringLiteral("packed") << QStringLiteral("xor:volume");
        QTest::newRow("zstd-exfat/packs-damaged") << QStringLiteral("zstd-exfat") << QStringLiteral("xor:frame");
        QTest::newRow("zstd-ffpkg/packs-damaged") << QStringLiteral("zstd-ffpkg") << QStringLiteral("xor:frame");
    }

    // A damaged image: its extraction fails with a clear message (CorruptData),
    // nothing is written under the output's name or next to it, and the image
    // itself is not changed.
    void corrupted()
    {
        QFETCH(QString, format);
        QFETCH(QString, damage);
        RowGuard g(QStringLiteral("corrupted input"));
        const Fmt& f = fmt(format);
        const QString shape = damage.startsWith(u"xor:") ? QStringLiteral("compressible") : QStringLiteral("basic");
        const QString clean = imageOf(format, shape);
        QVERIFY(!clean.isEmpty());
        const QString tag = QString::fromUtf8(QTest::currentDataTag()).replace(u'/', u'-');
        const QString dir = freshDir(path(QStringLiteral("corrupt/%1").arg(tag)));
        QString image = QDir(dir).filePath(QFileInfo(clean).fileName());
        if (f.ext.isEmpty()) {
            QVERIFY(copyFolder(clean, image));
        } else {
            QVERIFY(QFile::copy(clean, image));
        }
        const QStringList parts = damage.split(u':');
        if (parts[0] == u"zero") {
            QVERIFY(overwrite(image, parts[1].toLongLong(), QByteArray(parts[2].toInt(), '\0')));
        } else if (parts[0] == u"truncate") {
            QVERIFY(truncateTo(image, QFileInfo(image).size() / 2));
        } else if (damage == u"xor:volume" || damage == u"xor:frame") {
            // Inside the first zstd frame of the packs (the volume's middle may
            // be padding, which nothing reads).
            // In an image: where the same bytes of the packed folder's volume
            // are (the packs do not depend on the image they go into).
            const QString packed = imageOf(QStringLiteral("packed"), shape);
            QVERIFY(!packed.isEmpty());
            const QString vol = QDir(packed).filePath(QStringLiteral("ampr_zstd-000.pak"));
            const QByteArray volBytes = fileBytes(vol);
            const qint64 frame = volBytes.indexOf(QByteArray("\x28\xB5\x2F\xFD", 4));
            QVERIFY2(frame > 0 && frame + 300 < volBytes.size(), "no zstd frame found in the volume");
            const QString file = damage == u"xor:volume" ? QDir(image).filePath(QStringLiteral("ampr_zstd-000.pak")) : image;
            const qint64 at = damage == u"xor:volume" ? frame : findInFile(file, volBytes.mid(frame, 300));
            QVERIFY2(at > 0, "the volume's bytes were not found in the image");
            QVERIFY(xorRange(file, at + 8, 256));
        } else {
            QVERIFY(xorRange(image, QFileInfo(image).size() / 2, 64 * kKiB));
        }
        const QByteArray damaged = f.ext.isEmpty() ? QByteArray() : sha256Of(image);
        const Manifest damagedTree = f.ext.isEmpty() ? manifestOf(image) : Manifest();
        // A cut-short uncompressed PFS image is read as a "short (RAM-mode)
        // image": the missing tail reads as zeros (PFS.md, Untrusted images).
        // Documented: the extraction finishes and says so in a warning.
        const bool shortPfs = format == u"ffpfs" && parts[0] == u"truncate";
        for (const QString& kind : {f.extractKind, integration::kGameExtractJob}) {
            const QString xdir = freshDir(QDir(dir).filePath(QStringLiteral("x-") + QString(kind).replace(u'.', u'-')));
            auto r = m_env->run(kind, extractParams(kind, image, xdir + QStringLiteral("/game")));
            const QString step = kind == integration::kGameExtractJob ? QStringLiteral("game.extract") : QStringLiteral("extract");
            const QByteArray stepName = step.toLatin1();
            if (shortPfs) {
                MCHECK(g, stepName.constData(), succeeded(r), kind + u' ' + describe(r));
                const bool warned = std::any_of(r->outcome->warnings.begin(), r->outcome->warnings.end(),
                                                [](const QString& w) { return w.contains(QStringLiteral("shorter than its header says")); });
                const QByteArray warnStep = (step + QStringLiteral("-warning")).toLatin1();
                MCHECK(g, warnStep.constData(), warned, kind + u' ' + describe(r));
                MCHECK(g, "input-untouched", sha256Of(image) == damaged, QStringLiteral("the damaged image changed"));
                g.verdict = QStringLiteral("PASS (documented: short image, zeros + warning)");
                g.note = QStringLiteral("see F-MX-02 (owner decision)");
                continue;
            }
            MCHECK(g, stepName.constData(), r && r->state == JobState::Failed && r->error && r->error->code == ErrorCode::CorruptData,
                   kind + u' ' + describe(r));
            MCHECK(g, "nothing-written", listTop(xdir).isEmpty(), kind + QStringLiteral(" left: ") + listTop(xdir).join(u", "));
            if (f.ext.isEmpty())
                MCHECK(g, "input-untouched", diffManifests(damagedTree, manifestOf(image)).isEmpty(), QStringLiteral("the packed folder changed"));
            else
                MCHECK(g, "input-untouched", sha256Of(image) == damaged, QStringLiteral("the damaged image changed"));
            if (r && r->error)
                g.note = errorCodeName(r->error->code) + QStringLiteral(": ") + r->error->message.left(120);
        }
    }

    // A damaged image as the source of a conversion over an existing output
    // (replace on): the job fails and the existing output stays as it was.
    void corruptedConversionKeepsOutput()
    {
        RowGuard g(QStringLiteral("corrupted input"));
        const QString clean = imageOf(QStringLiteral("ffpkg"), QStringLiteral("basic"));
        QVERIFY(!clean.isEmpty());
        const QString dir = freshDir(path(QStringLiteral("corrupt/convert")));
        const QString image = QDir(dir).filePath(QStringLiteral("game.ffpkg"));
        QVERIFY(QFile::copy(clean, image));
        QVERIFY(overwrite(image, 65536, QByteArray(8192, '\0')));
        const QString out = QDir(dir).filePath(QStringLiteral("game.exfat"));
        QVERIFY(put(dir, QStringLiteral("game.exfat"), "the user's older image"));
        auto r = m_env->run(integration::kConvertJob, {{QStringLiteral("image"), image}, {QStringLiteral("format"), QStringLiteral("exfat")},
                                                       {QStringLiteral("output"), out}, {QStringLiteral("replaceExisting"), true}});
        MCHECK(g, "convert", r && r->state == JobState::Failed && r->error && r->error->code == ErrorCode::CorruptData, describe(r));
        MCHECK(g, "untouched", fileBytes(out) == "the user's older image", QStringLiteral("the existing output changed"));
        MCHECK(g, "leftover", listTop(dir) == (QStringList{QStringLiteral("game.exfat"), QStringLiteral("game.ffpkg")}), listTop(dir).join(u", "));
    }

    // Changed file data (not structure) is found by a full verification
    // against the game folder (raw data has no checksum in these formats).
    void changedDataFailsVerification_data()
    {
        QTest::addColumn<QString>("format");
        QTest::newRow("exfat") << QStringLiteral("exfat");
        QTest::newRow("ffpkg") << QStringLiteral("ffpkg");
        QTest::newRow("ffpfs") << QStringLiteral("ffpfs");
    }

    void changedDataFailsVerification()
    {
        QFETCH(QString, format);
        RowGuard g(QStringLiteral("corrupted input"));
        const QString clean = imageOf(format, QStringLiteral("basic"));
        QVERIFY(!clean.isEmpty());
        const QString dir = freshDir(path(QStringLiteral("changed/%1").arg(format)));
        const QString image = QDir(dir).filePath(QFileInfo(clean).fileName());
        QVERIFY(QFile::copy(clean, image));
        // A byte in the middle of data/level1.bin (incompressible, stored as is).
        const QByteArray probe = noise(300 * kKiB, 3).mid(150000, 64);
        const qint64 at = findInFile(image, probe);
        QVERIFY2(at > 0, "the file's data was not found in the image");
        QVERIFY(xorRange(image, at, 1));
        const QString kind = format == u"exfat" ? exfat::kJobVerify : format == u"ffpkg" ? ufs2::kinds::kVerify : pfs::kinds::kVerify;
        auto r = m_env->run(kind, {{QStringLiteral("image"), image}, {QStringLiteral("source"), fixture(QStringLiteral("basic"))}});
        MCHECK(g, "verify", r && r->outcome && r->outcome->verification == Verification::Failed, describe(r));
        g.note = r && r->outcome ? r->outcome->verificationDetail.left(120) : QString();
    }

    // A damaged archive: the build fails clearly and writes nothing.
    void corruptedArchive_data()
    {
        QTest::addColumn<QString>("kind");
        QTest::addColumn<QString>("format");
        for (const QString& k : {QStringLiteral("zip"), QStringLiteral("7z")})
            for (const QString& f : {QStringLiteral("exfat"), QStringLiteral("ffpkg"), QStringLiteral("ffpfsc"), QStringLiteral("packed")})
                QTest::newRow(qPrintable(QStringLiteral("%1-truncated/%2").arg(k, f))) << k << f;
    }

    void corruptedArchive()
    {
        QFETCH(QString, kind);
        QFETCH(QString, format);
        RowGuard g(QStringLiteral("corrupted input"));
        const QString good = sourceOf(QStringLiteral("basic"), kind);
        QVERIFY(!good.isEmpty());
        const QString tag = QString::fromUtf8(QTest::currentDataTag()).replace(u'/', u'-');
        const QString dir = freshDir(path(QStringLiteral("corrupt-archive/%1").arg(tag)));
        const QString archive = QDir(dir).filePath(QStringLiteral("PPSA04242.") + kind);
        QVERIFY(QFile::copy(good, archive));
        QVERIFY(truncateTo(archive, QFileInfo(archive).size() * 6 / 10));
        const QByteArray before = sha256Of(archive);
        const QString outDir = freshDir(QDir(dir).filePath(QStringLiteral("out")));
        const Fmt& f = fmt(format);
        auto r = m_env->run(f.buildKind, buildParams(f, archive, outputIn(f, outDir)));
        MCHECK(g, "build", r && r->state == JobState::Failed && r->error && r->error->code == ErrorCode::CorruptData, describe(r));
        MCHECK(g, "nothing-written", listTop(outDir).isEmpty(), listTop(outDir).join(u", "));
        MCHECK(g, "archive-untouched", sha256Of(archive) == before, QStringLiteral("the archive changed"));
        if (r && r->error)
            g.note = errorCodeName(r->error->code) + QStringLiteral(": ") + r->error->message.left(120);
    }

    // ---- names that differ only in letter case --------------------------------------------

    // A Windows folder cannot hold them; an archive can. The archive rules
    // refuse them (ARCHITECTURE.md, Archives as build sources).
    void caseOnlyNamesInArchive_data()
    {
        QTest::addColumn<QString>("kind");
        QTest::addColumn<QString>("format");
        for (const QString& k : {QStringLiteral("zip"), QStringLiteral("7z")})
            for (const Fmt& f : formats())
                QTest::newRow(qPrintable(QStringLiteral("%1/%2").arg(k, f.id))) << k << f.id;
    }

    void caseOnlyNamesInArchive()
    {
        QFETCH(QString, kind);
        QFETCH(QString, format);
        RowGuard g(QStringLiteral("case-only names"));
        const QString dir = freshDir(path(QStringLiteral("case/%1-%2").arg(kind, format)));
        const QString archive = QDir(dir).filePath(QStringLiteral("PPSA04242.") + kind);
        const QList<std::pair<QString, QByteArray>> items = {{QStringLiteral("eboot.bin"), QByteArray("\x54\x14\xF5\xEE", 4) + noise(1000, 1)},
                                                              {QStringLiteral("sce_sys/param.json"), kParam.toUtf8()},
                                                              {QStringLiteral("data/x.bin"), "lower"},
                                                              {QStringLiteral("data/X.BIN"), "upper"}};
        if (kind == u"zip") {
            QVERIFY(zipItems(archive, items));
        } else {
            QList<std::tuple<QString, QByteArray, bool>> seven;
            for (const auto& [n, d] : items)
                seven << std::make_tuple(n, d, false);
            QVERIFY(write7z(archive, seven));
        }
        const QString outDir = freshDir(QDir(dir).filePath(QStringLiteral("out")));
        const Fmt& f = fmt(format);
        auto r = m_env->run(f.buildKind, buildParams(f, archive, outputIn(f, outDir)));
        MCHECK(g, "refused", r && r->state == JobState::Failed && r->error && r->error->message.contains(QStringLiteral("differ only in upper and lower case")),
               describe(r));
        MCHECK(g, "nothing-written", listTop(outDir).isEmpty(), listTop(outDir).join(u", "));
        g.note = QStringLiteral("refused as documented");
    }

    // An .ffpkg (UFS2 tells letter case apart) can hold both names. Extracting
    // it on Windows keeps both, one renamed " (2)" and reported; converting it
    // to exFAT (which does not tell case apart) is refused.
    void caseOnlyNamesInImage()
    {
        RowGuard g(QStringLiteral("case-only names"));
        const QString clean = imageOf(QStringLiteral("ffpkg"), QStringLiteral("basic"));
        QVERIFY(!clean.isEmpty());
        const QString dir = freshDir(path(QStringLiteral("case-image")));
        const QString image = QDir(dir).filePath(QStringLiteral("game.ffpkg"));
        QVERIFY(QFile::copy(clean, image));
        QVERIFY(put(dir, QStringLiteral("local/upper.bin"), "UPPER"));
        const QJsonArray ops{QJsonObject{{QStringLiteral("op"), QStringLiteral("add")},
                                         {QStringLiteral("path"), QStringLiteral("/data/LEVEL1.BIN")},
                                         {QStringLiteral("source"), QDir(dir).filePath(QStringLiteral("local/upper.bin"))}}};
        auto e = m_env->runDirect(ufs2::kinds::kEdit, {{QStringLiteral("image"), image}, {QStringLiteral("operations"), ops}});
        MCHECK(g, "edit", e.ok() && e->verification == Verification::Passed, describe(e));

        const QString x = freshDir(QDir(dir).filePath(QStringLiteral("x")));
        auto ex = m_env->run(ufs2::kinds::kExtract, extractParams(ufs2::kinds::kExtract, image, x + QStringLiteral("/game")));
        MCHECK(g, "extract", succeeded(ex), describe(ex));
        const QStringList data = listTop(x + QStringLiteral("/game/data"));
        MCHECK(g, "both-kept", data.size() == 3 && data.contains(QStringLiteral("level1.bin")), data.join(u", "));
        bool renamedReported = false;
        for (const QString& w : ex->outcome->warnings)
            renamedReported = renamedReported || (w.contains(QStringLiteral("Renamed for this drive")) && w.contains(QStringLiteral("(2)")));
        MCHECK(g, "renamed-reported", renamedReported, ex->outcome->warnings.join(u'\n'));
        QByteArray other;
        for (const QString& n : data)
            if (n != u"level1.bin" && n != u"strings")
                other = fileBytes(x + QStringLiteral("/game/data/") + n);
        MCHECK(g, "renamed-content", other == "UPPER", QString::fromLatin1(other.left(20)));
        MCHECK(g, "original-content", sha256Of(x + QStringLiteral("/game/data/level1.bin")) == sha256Of(fixture(QStringLiteral("basic")) + QStringLiteral("/data/level1.bin")),
               QStringLiteral("level1.bin changed"));

        const QString exfatOut = QDir(dir).filePath(QStringLiteral("game.exfat"));
        auto c = m_env->run(integration::kConvertJob, {{QStringLiteral("image"), image}, {QStringLiteral("format"), QStringLiteral("exfat")},
                                                       {QStringLiteral("output"), exfatOut}});
        MCHECK(g, "to-exfat-refused", c && c->state == JobState::Failed && c->error && c->error->message.contains(QStringLiteral("differ only in upper/lower case")),
               describe(c));
        MCHECK(g, "to-exfat-nothing", !QFileInfo::exists(exfatOut), QStringLiteral("an exFAT image was written"));
    }

    // ---- cancellation -------------------------------------------------------------------

    void cancellation_data()
    {
        QTest::addColumn<QString>("kind");
        QTest::addColumn<QString>("format");
        QTest::addColumn<int>("phase");
        const auto rows = [](const QString& name, const QString& kind, const QString& format, int phases) {
            for (int i = 0; i < phases; ++i)
                QTest::newRow(qPrintable(QStringLiteral("%1/phase%2").arg(name).arg(i))) << kind << format << i;
        };
        rows(QStringLiteral("build-exfat"), exfat::kJobBuild, QStringLiteral("exfat"), 3);
        rows(QStringLiteral("build-ffpkg"), ufs2::kinds::kBuild, QStringLiteral("ffpkg"), 3);
        rows(QStringLiteral("build-ffpfs"), pfs::kinds::kBuild, QStringLiteral("ffpfs"), 3);
        rows(QStringLiteral("build-ffpfsc"), pfs::kinds::kBuild, QStringLiteral("ffpfsc"), 3);
        rows(QStringLiteral("build-packed"), ampr::kPackJob, QStringLiteral("packed"), 3);
        rows(QStringLiteral("build-zstd-exfat"), integration::kZstdBuildJob, QStringLiteral("zstd-exfat"), 6);
        rows(QStringLiteral("build-zstd-ffpfsc"), integration::kZstdBuildJob, QStringLiteral("zstd-ffpfsc"), 6);
        rows(QStringLiteral("build-exfat-from-zip"), exfat::kJobBuild, QStringLiteral("exfat+zip"), 3);
        rows(QStringLiteral("extract-exfat"), exfat::kJobExtract, QStringLiteral("exfat"), 3);
        rows(QStringLiteral("extract-ffpkg"), ufs2::kinds::kExtract, QStringLiteral("ffpkg"), 3);
        rows(QStringLiteral("extract-ffpfsc"), pfs::kinds::kExtract, QStringLiteral("ffpfsc"), 1);
        rows(QStringLiteral("extract-packed"), ampr::kUnpackJob, QStringLiteral("packed"), 2);
        rows(QStringLiteral("extract-game-ffpkg"), integration::kGameExtractJob, QStringLiteral("ffpkg"), 3);
        rows(QStringLiteral("convert-exfat-to-ffpkg"), integration::kConvertJob, QStringLiteral("exfat>ffpkg"), 2);
        rows(QStringLiteral("convert-exfat-to-ffpfsc-direct"), integration::kConvertJob, QStringLiteral("exfat>ffpfsc"), 3);
        rows(QStringLiteral("edit-exfat"), exfat::kJobEdit, QStringLiteral("exfat"), 3);
        rows(QStringLiteral("edit-ffpkg"), ufs2::kinds::kEdit, QStringLiteral("ffpkg"), 4);
    }

    // A cancel in each stage. Before the work is finished: the job ends as
    // cancelled, nothing is left under the final name or next to it, the
    // source is untouched and an image being edited is as it was. During the
    // check of finished work (GitHub #97, ARCHITECTURE.md Job rules): the work
    // is kept as with verification turned off and reported as not verified,
    // except image.build.zstd's pack check (its image is not made yet).
    void cancellation()
    {
        QFETCH(QString, kind);
        QFETCH(QString, format);
        QFETCH(int, phase);
        RowGuard g(QStringLiteral("cancellation"));
        const QString tag = QString::fromUtf8(QTest::currentDataTag()).replace(u'/', u'-');
        const QString dir = freshDir(path(QStringLiteral("cancel/%1").arg(tag)));
        const QString game = fixture(QStringLiteral("cancel"));
        QVERIFY(!game.isEmpty());
        const Manifest gameBefore = manifestOf(game, false);

        QJsonObject params;
        QString output;      // what the job makes (file or folder)
        QString input;       // the image or folder it reads, never changed (an edited image: as it was, or edited)
        bool editing = false;
        QJsonArray ops;
        const bool build = kind == exfat::kJobBuild || kind == ufs2::kinds::kBuild || kind == pfs::kinds::kBuild || kind == ampr::kPackJob
                        || kind == integration::kZstdBuildJob;
        if (build) {
            const QString fid = format == u"exfat+zip" ? QStringLiteral("exfat") : format;
            const Fmt& f = fmt(fid);
            output = f.ext.isEmpty() ? QDir(dir).filePath(QStringLiteral("packed")) : outputIn(f, dir);
            input = format == u"exfat+zip" ? sourceOf(QStringLiteral("cancel"), QStringLiteral("zip")) : game;
            QVERIFY(!input.isEmpty());
            params = buildParams(f, input, output);
        } else if (kind == integration::kConvertJob) {
            const QStringList ft = format.split(u'>');
            input = imageOf(ft[0], QStringLiteral("cancel"));
            QVERIFY(!input.isEmpty());
            output = QDir(dir).filePath(QStringLiteral("game.") + ft[1]);
            params = {{QStringLiteral("image"), input}, {QStringLiteral("format"), ft[1]}, {QStringLiteral("output"), output}};
        } else if (kind == exfat::kJobEdit || kind == ufs2::kinds::kEdit) {
            editing = true;
            const QString clean = imageOf(format, QStringLiteral("cancel"));
            QVERIFY(!clean.isEmpty());
            input = QDir(dir).filePath(QFileInfo(clean).fileName());
            QVERIFY(QFile::copy(clean, input));
            const QString local = QDir(path(QStringLiteral("cancel"))).filePath(tag + QStringLiteral("-local.bin"));
            QVERIFY(put(QFileInfo(local).absolutePath(), QFileInfo(local).fileName(), noise(3 * kMiB, 77)));
            QJsonObject o{{QStringLiteral("op"), QStringLiteral("add")}};
            if (kind == exfat::kJobEdit) {
                o.insert(QStringLiteral("imagePath"), QStringLiteral("data/added.bin"));
                o.insert(QStringLiteral("localPath"), local);
            } else {
                o.insert(QStringLiteral("path"), QStringLiteral("/data/added.bin"));
                o.insert(QStringLiteral("source"), local);
            }
            ops.append(o);
            params = {{QStringLiteral("image"), input}, {QStringLiteral("operations"), ops}};
            output = input;
        } else {
            input = imageOf(format, QStringLiteral("cancel"));
            QVERIFY(!input.isEmpty());
            output = QDir(dir).filePath(QStringLiteral("game"));
            params = extractParams(kind, input, output);
        }
        const QByteArray inputHash = QFileInfo(input).isFile() ? sha256Of(input) : QByteArray();
        const Manifest inputTree = QFileInfo(input).isDir() ? manifestOf(input, false) : Manifest();
        const QStringList dirBefore = listTop(dir);

        auto job = m_env->jobs.create(kind, params);
        QVERIFY(job.ok());
        const QStringList phases = (*job)->phases();
        MCHECK(g, "phases", phase < phases.size(), phases.join(u", "));
        const QString phaseName = phases[phase];
        g.note = phaseName;

        CancellationSource cancel;
        std::atomic<bool> fired{false};
        auto r = m_env->runDirect(kind, params, {}, [&](const ProgressSnapshot& s) {
            if (!fired.load() && s.phaseIndex == phase && !s.phaseNames.isEmpty()) {
                fired = true;
                cancel.cancel();
            }
        }, cancel.token());
        MCHECK(g, "reached", fired.load(), QStringLiteral("the job never reported stage %1 (%2): %3").arg(phase).arg(phaseName, describe(r)));

        const bool checking = phaseName.startsWith(u"Verif") || (kind == ampr::kPackJob && phaseName == u"Checking packs");
        const bool replacing = phaseName == u"Replacing original"; // after the edited image verified
        if (checking || replacing) {
            // Kept, as with verification turned off: not called verified.
            MCHECK(g, "kept", r.ok(), describe(r));
            if (checking)
                MCHECK(g, "not-verified", r->verification == Verification::Skipped && r->details.value(QStringLiteral("verificationCancelled")).toBool(),
                       describe(r));
            MCHECK(g, "output", QFileInfo::exists(output), QStringLiteral("nothing under the final name"));
            g.note += QStringLiteral(": kept, ") + verificationName(r->verification);
        } else {
            MCHECK(g, "cancelled", !r.ok() && r.error().code == ErrorCode::Cancelled, describe(r));
            if (editing)
                MCHECK(g, "image-as-it-was", sha256Of(input) == inputHash, QStringLiteral("the cancelled edit changed the image"));
            else
                MCHECK(g, "no-output", !QFileInfo::exists(output), QStringLiteral("%1 exists after the cancel").arg(QFileInfo(output).fileName()));
            g.note += QStringLiteral(": cancelled");
        }
        // Nothing else next to it: no .fw.part, .part, marker, work folder or "(new)".
        QStringList expectedDir = dirBefore;
        if (QFileInfo::exists(output) && !expectedDir.contains(QFileInfo(output).fileName()))
            expectedDir << QFileInfo(output).fileName();
        expectedDir.sort();
        MCHECK(g, "leftover", listTop(dir) == expectedDir, listTop(dir).join(u", "));
        MCHECK(g, "game-untouched", diffManifests(gameBefore, manifestOf(game, false)).isEmpty(), QStringLiteral("the game folder changed"));
        if (!editing) {
            if (!inputHash.isEmpty())
                MCHECK(g, "input-untouched", sha256Of(input) == inputHash, QStringLiteral("the input changed"));
            else
                MCHECK(g, "input-untouched", diffManifests(inputTree, manifestOf(input, false)).isEmpty(), QStringLiteral("the input changed"));
        }
    }

    // ---- LZ4 packs (read only) ------------------------------------------------------------

    void lz4Packs_data()
    {
        QTest::addColumn<QString>("kind");
        QTest::newRow("game.extract") << integration::kGameExtractJob;
        QTest::newRow("ampr.unpack") << ampr::kUnpackJob;
        QTest::newRow("ampr.pack-then-unpack") << ampr::kPackJob;
        QTest::newRow("zstd-exfat-then-extract") << integration::kZstdBuildJob;
    }

    void lz4Packs()
    {
        QFETCH(QString, kind);
        RowGuard g(QStringLiteral("LZ4 packs (read)"));
        const QString tag = QString::fromUtf8(QTest::currentDataTag());
        const QString dir = freshDir(path(QStringLiteral("lz4/%1").arg(tag)));
        const QString game = QDir(dir).filePath(QStringLiteral("PPSA04242"));
        const QByteArray data = buildLz4Game(game, "/app0/data/big.bin");
        QVERIFY(!data.isEmpty());
        // The game as it sees itself: the packed file and the loose ones.
        const QString logical = QDir(dir).filePath(QStringLiteral("logical"));
        QVERIFY(put(logical, QStringLiteral("eboot.bin"), QByteArray("\x54\x14\xF5\xEE", 4)));
        QVERIFY(put(logical, QStringLiteral("sce_sys/param.json"), kParam.toUtf8()));
        QVERIFY(put(logical, QStringLiteral("data/big.bin"), data));
        const Manifest packedBefore = manifestOf(game);
        Manifest expected = manifestOf(logical);
        const QString out = QDir(dir).filePath(QStringLiteral("out"));
        if (kind == integration::kGameExtractJob || kind == ampr::kUnpackJob) {
            auto r = m_env->run(kind, extractParams(kind, game, out));
            MCHECK(g, "unpack", succeeded(r), describe(r));
        } else {
            const Fmt& f = fmt(kind == ampr::kPackJob ? QStringLiteral("packed") : QStringLiteral("zstd-exfat"));
            const QString built = kind == ampr::kPackJob ? QDir(dir).filePath(QStringLiteral("repacked")) : QDir(dir).filePath(QStringLiteral("repacked.exfat"));
            auto r = m_env->run(f.buildKind, buildParams(f, game, built));
            MCHECK(g, "repack", succeeded(r), describe(r));
            auto u = m_env->run(f.extractKind, extractParams(f.extractKind, built, out));
            MCHECK(g, "unpack", succeeded(u), describe(u));
            expected = expectedAfter(f, expected);
        }
        const QString d = diffManifests(expected, manifestOf(out));
        MCHECK(g, "compare", d.isEmpty(), d);
        MCHECK(g, "source-untouched", diffManifests(packedBefore, manifestOf(game)).isEmpty(), QStringLiteral("the LZ4 game changed"));
        g.note = QStringLiteral("hand-built AMPRPAK4 fixture (as tst_ampr)");
    }

    // ---- FPKG (MOCK: the stand-in tool) ------------------------------------------------------

    void fpkgMock_data()
    {
        QTest::addColumn<QString>("source");
        QTest::addColumn<QString>("shape");
        for (const QString& source : {QStringLiteral("folder"), QStringLiteral("zip"), QStringLiteral("7z"), QStringLiteral("exfat"),
                                      QStringLiteral("ffpkg"), QStringLiteral("ffpfsc")})
            for (const QString& shape : {QStringLiteral("basic"), QStringLiteral("empty-folders"), QStringLiteral("unicode"), QStringLiteral("zero-byte")})
                QTest::newRow(qPrintable(QStringLiteral("%1/%2").arg(source, shape))) << source << shape;
    }

    // The package itself is the stand-in's (it cannot be read back); what the
    // app controls is the project it hands to img_create: every file of the
    // game under its own path with its own bytes, every empty folder declared.
    void fpkgMock()
    {
        QFETCH(QString, source);
        QFETCH(QString, shape);
        RowGuard g(QStringLiteral("FPKG (MOCK)"));
        g.verdict = QStringLiteral("PASS (MOCK)");
        g.note = QStringLiteral("stand-in prospero-pub-cmd; not console evidence");
        const bool image = source == u"exfat" || source == u"ffpkg" || source == u"ffpfsc";
        if (image && !refusalFor(fmt(source), shape).isEmpty()) {
            g.verdict = QStringLiteral("SKIP");
            g.note = QStringLiteral("no .%1 of this shape").arg(source);
            QSKIP("The image format cannot hold this shape.");
        }
        const QString src = image ? imageOf(source, shape) : sourceOf(shape, source);
        QVERIFY(!src.isEmpty());
        const QString tag = QString::fromUtf8(QTest::currentDataTag()).replace(u'/', u'-');
        const QString out = freshDir(path(QStringLiteral("fpkg/%1").arg(tag)));
        auto r = m_env->runDirect(QStringLiteral("fpkg.build"), {{QStringLiteral("source"), src}, {QStringLiteral("output"), out}},
                                  {{fpkg::keys::kKeepIntermediate, true}, {fpkg::keys::kPlayGoChunkCount, 4}});
        // Documented refusals (FPKG.md): fpkg.build takes "a folder, .gp5, or
        // an image with a registered stager", not an archive; and the path
        // rules (ASCII names the Publishing Tools accept) are checked before
        // packing.
        QString refusal;
        if (source == u"zip" || source == u"7z")
            refusal = QStringLiteral("Choose a game folder, a .gp5 project or");
        else if (shape == u"unicode")
            refusal = QStringLiteral("not accepted by the Publishing Tools");
        if (!refusal.isEmpty()) {
            MCHECK(g, "refused", !r.ok() && r.error().message.contains(refusal), describe(r));
            MCHECK(g, "nothing-written", listTop(out).isEmpty(), listTop(out).join(u", "));
            g.verdict = QStringLiteral("PASS (MOCK, refused as documented)");
            g.note = refusal;
            return;
        }
        MCHECK(g, "build", r.ok() && r->verification == Verification::Passed, describe(r));
        QStringList pkgs = QDir(out).entryList({QStringLiteral("*.pkg")}, QDir::Files);
        QStringList gp5s = QDir(out).entryList({QStringLiteral("*.gp5")}, QDir::Files);
        MCHECK(g, "outputs", pkgs.size() == 1 && gp5s.size() == 1, listTop(out).join(u", "));
        auto project = fpkg::readGp5(QDir(out).filePath(gp5s.first()));
        QVERIFY2(project.ok(), project.ok() ? "" : qPrintable(project.error().toLogString()));

        // The game's files outside sce_sys (sce_sys is rewritten by the build:
        // param.json patched, playgo files generated) and its empty folders.
        const Manifest game = manifestOf(fixture(shape));
        QStringList want, wantEmpty, got, gotEmpty, contentProblems;
        for (auto it = game.begin(); it != game.end(); ++it)
            if (!it->dir && !it.key().startsWith(u"sce_sys/"))
                want << it.key();
        for (const QString& e : emptyFolders(game))
            if (!e.startsWith(u"sce_sys"))
                wantEmpty << e;
        for (const fpkg::Gp5FileEntry& e : project->files) {
            // Generated by the build: PlayGo fillers for chunks without data
            // (fpkg.fillEmptyChunks) and the per-language files (FPKG.md).
            if (e.dst.startsWith(u"sce_sys/") || e.dst.startsWith(u"playgo-chunks/") || e.dst.startsWith(u"playgo-languages/"))
                continue;
            got << e.dst;
            const QString file = fpkg::resolveGp5SourcePath(*project, e.src);
            if (QFileInfo::exists(file) && game.contains(e.dst) && sha256Of(file) != game.value(e.dst).sha)
                contentProblems << e.dst;
        }
        for (const QString& e : project->emptyDirs)
            gotEmpty << e;
        want.sort();
        got.sort();
        wantEmpty.sort();
        gotEmpty.sort();
        MCHECK(g, "files", want == got, QStringLiteral("project lists %1, game has %2").arg(got.join(u", "), want.join(u", ")));
        MCHECK(g, "content", contentProblems.isEmpty(), contentProblems.join(u", "));
        MCHECK(g, "empty-folders", wantEmpty == gotEmpty, QStringLiteral("declared %1, game has %2").arg(gotEmpty.join(u", "), wantEmpty.join(u", ")));
    }
};

QTEST_GUILESS_MAIN(TestFormatMatrix)
#include "tst_format_matrix.moc"
