// 7z and RAR archives as build sources (GitHub #37): the same contract as a
// zipped dump. 7z archives are made here with libarchive's writer (linked into
// this test only) or come from tests/data/archives (made by hand with 7-Zip,
// see make_archive_fixtures.py); there is no free RAR writer, so RAR is tested
// with the samples of libarchive's own test suite, decoded from the source
// archive the build fetched. Those hold no game, so they go through
// ArchiveReader directly; finding the game is the same code for every format.

#include "core/ArchiveReader.h"
#include "core/ArchiveSource.h"
#include "core/FileUtil.h"
#include "core/Zip.h"
#include "support/TestSupport.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>

#include <archive.h>
#include <archive_entry.h>

#include <cstring>
#include <limits>

using namespace fw;
namespace fwa = fw::archive; // libarchive's own "struct archive" is in scope here

namespace {

constexpr qint64 kMtime = 1717243200; // 2024-06-01 12:00:00 UTC, an even second

QByteArray readFile(const QString& p)
{
    QFile f(p);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

bool writeFile(const QString& p, const QByteArray& data)
{
    QDir().mkpath(QFileInfo(p).absolutePath());
    QFile f(p);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(data) == data.size();
}

// Bytes no compressor shrinks.
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
    enum Kind { File, Folder, SymLink };
    QString name;
    QByteArray data;
    Kind kind = File;
    QString target;
    qint64 mtime = kMtime;
};

Item file(const QString& name, const QByteArray& data, qint64 mtime = kMtime) { return {name, data, Item::File, {}, mtime}; }
Item folder(const QString& name) { return {name, {}, Item::Folder, {}, kMtime}; }
Item symLink(const QString& name, const QString& target) { return {name, {}, Item::SymLink, target, kMtime}; }

la_ssize_t appendToFile(struct archive*, void* client, const void* buffer, size_t length)
{
    QFile* f = static_cast<QFile*>(client);
    return f->write(static_cast<const char*>(buffer), qint64(length)) == qint64(length) ? la_ssize_t(length) : -1;
}

// `compression`: store, deflate, lzma1, lzma2 or ppmd. libarchive's writer
// puts every file into one block, so each of these archives is solid.
bool write7z(const QString& path, const QList<Item>& items, const char* compression)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    struct archive* a = archive_write_new();
    bool ok = archive_write_set_format_7zip(a) == ARCHIVE_OK;
    const QByteArray options = QByteArray("7zip:compression=") + compression + ",7zip:compression-level=1";
    ok = ok && archive_write_set_options(a, options.constData()) == ARCHIVE_OK;
    archive_write_set_bytes_in_last_block(a, 1); // no padding after the archive's last byte
    ok = ok && archive_write_open(a, &out, nullptr, appendToFile, nullptr) == ARCHIVE_OK;
    for (const Item& item : items) {
        if (!ok)
            break;
        struct archive_entry* e = archive_entry_new();
        archive_entry_copy_pathname_w(e, item.name.toStdWString().c_str());
        archive_entry_set_mtime(e, time_t(item.mtime), 0);
        if (item.kind == Item::Folder) {
            archive_entry_set_filetype(e, AE_IFDIR);
            archive_entry_set_perm(e, 0755);
        } else if (item.kind == Item::SymLink) {
            archive_entry_set_filetype(e, AE_IFLNK);
            archive_entry_set_perm(e, 0777);
            archive_entry_copy_symlink_w(e, item.target.toStdWString().c_str());
        } else {
            archive_entry_set_filetype(e, AE_IFREG);
            archive_entry_set_perm(e, 0644);
            archive_entry_set_size(e, item.data.size());
        }
        ok = archive_write_header(a, e) == ARCHIVE_OK;
        if (ok && item.kind == Item::File && !item.data.isEmpty())
            ok = archive_write_data(a, item.data.constData(), size_t(item.data.size())) == la_ssize_t(item.data.size());
        archive_entry_free(e);
    }
    ok = archive_write_close(a) == ARCHIVE_OK && ok;
    archive_write_free(a);
    return ok && out.flush();
}

// A made-up game: nested folders, an empty folder, an empty file, a name
// outside ASCII (and outside the Western code page) and five MiB that do not
// compress.
QList<Item> dummyGame(const QString& prefix = {})
{
    return {
        file(prefix + QStringLiteral("eboot.bin"), noise(100000, 1)),
        file(prefix + QStringLiteral("sce_sys/param.json"), "{\"titleId\":\"PPSA01234\",\"titleName\":\"Archive test\"}"),
        file(prefix + QStringLiteral("sce_sys/icon0.png"), noise(4000, 2)),
        folder(prefix + QStringLiteral("data/empty folder")),
        file(prefix + QStringLiteral("data/zero.bin"), QByteArray()),
        file(prefix + QStringLiteral("data/データ — ü.bin"), noise(257, 3)),
        file(prefix + QStringLiteral("data/deep/deeper/deepest/text.txt"), QByteArray(200000, 'a')),
        file(prefix + QStringLiteral("data/big.bin"), noise(5 * 1024 * 1024 + 17, 4)),
    };
}

qint64 bytesOf(const QList<Item>& items)
{
    qint64 n = 0;
    for (const Item& i : items)
        n += i.data.size();
    return n;
}

qint64 filesOf(const QList<Item>& items)
{
    qint64 n = 0;
    for (const Item& i : items)
        n += i.kind == Item::File ? 1 : 0;
    return n;
}

QStringList filesUnder(const QString& root)
{
    QStringList out;
    QDirIterator it(root, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    while (it.hasNext())
        out << QDir(root).relativeFilePath(it.next());
    out.sort();
    return out;
}

// Every item is there byte for byte under `root` (items named with
// `prefix` in front), and nothing else is.
QString treeDiffers(const QString& root, const QList<Item>& items, const QString& prefix = {})
{
    QStringList expected;
    for (const Item& i : items) {
        const QString rel = i.name.mid(prefix.size());
        const QString path = QDir(root).filePath(rel);
        if (i.kind == Item::Folder) {
            if (!QFileInfo(path).isDir())
                return QStringLiteral("folder %1 is missing").arg(rel);
            continue;
        }
        if (i.kind != Item::File)
            continue;
        expected << rel;
        if (!QFileInfo(path).isFile())
            return QStringLiteral("%1 is missing").arg(rel);
        if (readFile(path) != i.data)
            return QStringLiteral("%1 differs").arg(rel);
        if (QFileInfo(path).lastModified().toSecsSinceEpoch() != i.mtime)
            return QStringLiteral("%1 has the time %2").arg(rel).arg(QFileInfo(path).lastModified().toSecsSinceEpoch());
    }
    expected.sort();
    const QStringList found = filesUnder(root);
    if (found != expected)
        return QStringLiteral("files found: %1").arg(found.join(QStringLiteral(", ")));
    return {};
}

// The game of tests/data/archives (make_archive_fixtures.py).
QList<Item> fixtureGame()
{
    QByteArray eboot(3000, Qt::Uninitialized), data(257, Qt::Uninitialized);
    for (int i = 0; i < eboot.size(); ++i)
        eboot[i] = char((i * 31 + 7) & 0xFF);
    for (int i = 0; i < data.size(); ++i)
        data[i] = char((i * 7) & 0xFF);
    return {
        file(QStringLiteral("eboot.bin"), eboot),
        file(QStringLiteral("sce_sys/param.json"), "{\"titleId\":\"PPSA01234\",\"titleName\":\"Archive test\"}"),
        file(QStringLiteral("data/データ ü.bin"), data),
        file(QStringLiteral("data/empty.bin"), QByteArray()),
        folder(QStringLiteral("data/empty folder")),
    };
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

// What libarchive's RAR 5 tests keep in "test<magic>.bin".
QByteArray rar5TestData(int magic, int size)
{
    QByteArray b(size, '\0');
    for (int i = 0; i < size / 4; ++i) {
        const int k = i + 1;
        const qint32 v = qMax(0, k * k - 3 * k + (1 + magic));
        qToLittleEndian<qint32>(v, b.data() + i * 4);
    }
    return b;
}

} // namespace

class TstArchiveSource : public QObject {
    Q_OBJECT
    QTemporaryDir m_root;
    int m_serial = 0;

    QString fresh(const QString& name) { return m_root.filePath(QStringLiteral("%1/%2").arg(++m_serial).arg(name)); }

    // A sample of libarchive's test suite, decoded next to its other parts.
    QString sample(const QString& name)
    {
        const QString out = m_root.filePath(QStringLiteral("samples/") + name);
        if (QFile::exists(out))
            return out;
        const QByteArray text = readFile(QStringLiteral(FW_LIBARCHIVE_TEST_DATA "/") + name + QStringLiteral(".uu"));
        return !text.isEmpty() && writeFile(out, uudecode(text)) ? out : QString();
    }

    static QStringList sampleNames(const QString& pattern)
    {
        QStringList names;
        for (QString n : QDir(QStringLiteral(FW_LIBARCHIVE_TEST_DATA)).entryList({pattern + QStringLiteral(".uu")}, QDir::Files, QDir::Name)) {
            n.chop(3);
            names << n;
        }
        return names;
    }

    static bool noPartFiles(const QString& dir)
    {
        for (const QString& f : filesUnder(dir))
            if (f.endsWith(QStringLiteral(".part")))
                return false;
        return true;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_root.isValid());
        fwtest::isolateAppData(m_root);
    }

    void cleanup() { fwa::setArchiveInspectBudgetForTesting(256LL * 1024 * 1024, 5000); }

    void suffixesAndFilter()
    {
        QCOMPARE(fwa::archiveSourceSuffixes(), (QStringList{QStringLiteral("zip"), QStringLiteral("7z"), QStringLiteral("rar")}));
        QCOMPARE(fwa::archiveSourceFilter(), QStringLiteral("Archives (*.zip *.7z *.rar)"));
        for (const char* name : {"a.zip", "a.7z", "a.RAR", "a.7Z"}) {
            const QString p = fresh(QString::fromLatin1(name));
            QVERIFY(writeFile(p, "x"));
            QVERIFY2(fwa::isArchiveSource(p) && fwa::isZipSource(p), name);
        }
        const QString other = fresh(QStringLiteral("a.tar"));
        QVERIFY(writeFile(other, "x"));
        QVERIFY(!fwa::isArchiveSource(other));
        QVERIFY(!fwa::isArchiveSource(m_root.filePath(QStringLiteral("missing.7z"))));
        QVERIFY(!fwa::isArchiveSource(m_root.path())); // a folder
    }

    void sevenZipRoundTrip_data()
    {
        QTest::addColumn<QByteArray>("compression");
        for (const char* c : {"store", "lzma2", "lzma1", "deflate", "ppmd"})
            QTest::newRow(c) << QByteArray(c);
    }
    // The game comes out byte for byte, with its times, its empty folder and
    // its empty file; the old "Zip" names take the archive as well.
    void sevenZipRoundTrip()
    {
        QFETCH(QByteArray, compression);
        const QList<Item> game = dummyGame();
        const QString a = fresh(QStringLiteral("game.7z"));
        QVERIFY(write7z(a, game, compression.constData()));
        QVERIFY(fwa::isZipSource(a));

        auto g = fwa::inspectZip(a);
        QVERIFY2(g.ok(), g.ok() ? "" : qPrintable(g.error().toLogString()));
        QCOMPARE(g->rootPrefix, QString());
        QCOMPARE(g->files, filesOf(game));
        QCOMPARE(g->bytes, bytesOf(game));
        QVERIFY(g->hasEboot && g->hasParam);
        QCOMPARE(g->paramJson, game.at(1).data);
        QVERIFY(g->otherGames.isEmpty());
        QCOMPARE(g->links, qint64(0));
        const Status checked = fwa::checkZipSource(a);
        QVERIFY2(checked.ok(), checked.ok() ? "" : qPrintable(checked.error().toLogString()));

        ProgressReporter progress;
        const QString temp = fresh(QStringLiteral("temp"));
        auto root = fwa::unpackZipSource(a, temp, CancellationToken(), &progress, 0);
        QVERIFY2(root.ok(), root.ok() ? "" : qPrintable(root.error().toLogString()));
        QCOMPARE(QDir::cleanPath(root.value()), QDir::cleanPath(temp + QStringLiteral("/zip-source")));
        QCOMPARE(treeDiffers(root.value(), game), QString());
        const ProgressSnapshot s = progress.snapshot();
        QCOMPARE(s.total, g->bytes);
        QCOMPARE(s.done, g->bytes);
        QCOMPARE(s.filesDone, g->files);
    }

    void sevenZipFromSevenZip_data()
    {
        QTest::addColumn<QString>("name");
        for (const char* n : {"game-lzma2-nonsolid", "game-lzma-solid", "game-ppmd", "game-deflate", "game-bcj-lzma2",
                              "game-delta-lzma2", "game-arm64-lzma2", "game-plain-header"})
            QTest::newRow(n) << QString::fromLatin1(n);
    }
    // Archives written by 7-Zip itself: every file in its own block, the
    // filters it puts in front of LZMA, a list of files that is not packed.
    void sevenZipFromSevenZip()
    {
        QFETCH(QString, name);
        const QString a = fwtest::dataDir() + QStringLiteral("/archives/") + name + QStringLiteral(".7z");
        QVERIFY(QFile::exists(a));
        const QList<Item> game = fixtureGame();
        auto g = fwa::inspectArchive(a);
        QVERIFY2(g.ok(), g.ok() ? "" : qPrintable(g.error().toLogString()));
        QCOMPARE(g->rootPrefix, QString());
        QCOMPARE(g->files, filesOf(game));
        QCOMPARE(g->bytes, bytesOf(game));
        QCOMPARE(g->paramJson, game.at(1).data);
        auto root = fwa::unpackArchiveSource(a, fresh(QStringLiteral("temp")), CancellationToken(), nullptr, 0);
        QVERIFY2(root.ok(), root.ok() ? "" : qPrintable(root.error().toLogString()));
        QCOMPARE(treeDiffers(root.value(), game), QString());
    }

    void gameOneLevelDown()
    {
        // From 7-Zip: the folder itself was archived.
        const QString made = fwtest::dataDir() + QStringLiteral("/archives/game-in-folder.7z");
        auto g = fwa::inspectArchive(made);
        QVERIFY2(g.ok(), g.ok() ? "" : qPrintable(g.error().toLogString()));
        QCOMPARE(g->rootPrefix, QStringLiteral("PPSA01234-app0/"));
        const QString temp = fresh(QStringLiteral("temp"));
        auto root = fwa::unpackArchiveSource(made, temp, CancellationToken(), nullptr, 0);
        QVERIFY2(root.ok(), root.ok() ? "" : qPrintable(root.error().toLogString()));
        QCOMPARE(QDir::cleanPath(root.value()), QDir::cleanPath(temp + QStringLiteral("/zip-source/PPSA01234-app0")));
        QCOMPARE(treeDiffers(root.value(), fixtureGame()), QString());

        // Only the game is unpacked, which is exactly what the free-space
        // check and the progress total counted.
        const QString prefix = QStringLiteral("PPSA01234-app0/");
        QList<Item> items{file(QStringLiteral("Extras/video.bin"), noise(300000, 9))};
        const QList<Item> game = dummyGame(prefix);
        items += game;
        items << folder(QStringLiteral("Extras/empty")) << file(QStringLiteral("readme.txt"), "hello");
        const QString a = fresh(QStringLiteral("wrapped.7z"));
        QVERIFY(write7z(a, items, "lzma2"));
        g = fwa::inspectArchive(a);
        QVERIFY2(g.ok(), g.ok() ? "" : qPrintable(g.error().toLogString()));
        QCOMPARE(g->rootPrefix, prefix);
        QCOMPARE(g->files, filesOf(game));
        QCOMPARE(g->bytes, bytesOf(game));
        QCOMPARE(g->paramJson, game.at(1).data);
        ProgressReporter progress;
        const QString temp2 = fresh(QStringLiteral("temp"));
        root = fwa::unpackArchiveSource(a, temp2, CancellationToken(), &progress, 0);
        QVERIFY2(root.ok(), root.ok() ? "" : qPrintable(root.error().toLogString()));
        QCOMPARE(QDir(temp2 + QStringLiteral("/zip-source")).entryList(QDir::AllEntries | QDir::NoDotAndDotDot),
                 QStringList{QStringLiteral("PPSA01234-app0")});
        QCOMPARE(treeDiffers(root.value(), game, prefix), QString());
        QCOMPARE(progress.snapshot().total, g->bytes);
        QCOMPARE(progress.snapshot().done, g->bytes);
    }

    void twoGamesAreRefusedWithTheirNames()
    {
        const QString a = fresh(QStringLiteral("two.7z"));
        QVERIFY(write7z(a, {file(QStringLiteral("Game One/eboot.bin"), "e"), file(QStringLiteral("Second/sce_sys/param.json"), "{}")}, "store"));
        auto g = fwa::inspectArchive(a);
        QVERIFY(g.ok());
        QCOMPARE(g->otherGames, QStringList{QStringLiteral("Game One")});
        const Status st = fwa::checkArchiveSource(a);
        QVERIFY(!st.ok());
        QCOMPARE(st.error().code, ErrorCode::InvalidArgument);
        QVERIFY2(st.error().message.contains(QStringLiteral("Game One")) && st.error().message.contains(QStringLiteral("Second")),
                 qPrintable(st.error().message));
        const QString temp = fresh(QStringLiteral("temp"));
        QVERIFY(!fwa::unpackArchiveSource(a, temp, CancellationToken(), nullptr, 0).ok());
        QVERIFY(!QFileInfo::exists(temp + QStringLiteral("/zip-source")));
    }

    void archiveWithoutAGame()
    {
        const QString a = fresh(QStringLiteral("none.7z"));
        QVERIFY(write7z(a, {file(QStringLiteral("readme.txt"), "hello")}, "store"));
        const Status st = fwa::checkArchiveSource(a);
        QVERIFY(!st.ok());
        QCOMPARE(st.error().code, ErrorCode::NotFound);
        QVERIFY(st.error().message.contains(QStringLiteral("holds no game")));
        // Not an archive at all, whatever it is called.
        const QString junk = fresh(QStringLiteral("junk.rar"));
        QVERIFY(writeFile(junk, QByteArray(5000, 'j')));
        const Status notOne = fwa::checkArchiveSource(junk);
        QVERIFY(!notOne.ok());
        QVERIFY2(notOne.error().message.contains(QStringLiteral("not a 7z or RAR archive")), qPrintable(notOne.error().message));
    }

    // The name does not decide the reader: the first bytes do.
    void wrongSuffixIsStillRead()
    {
        const QList<Item> game{file(QStringLiteral("eboot.bin"), noise(5000, 5)), file(QStringLiteral("sce_sys/param.json"), "{}")};
        const QString sevenAsZip = fresh(QStringLiteral("really-7z.zip"));
        QVERIFY(write7z(sevenAsZip, game, "lzma2"));
        auto root = fwa::unpackZipSource(sevenAsZip, fresh(QStringLiteral("temp")), CancellationToken(), nullptr, 0);
        QVERIFY2(root.ok(), root.ok() ? "" : qPrintable(root.error().toLogString()));
        QCOMPARE(treeDiffers(root.value(), game), QString());

        const QString zipAsRar = fresh(QStringLiteral("really-zip.rar"));
        {
            zip::Writer w;
            QVERIFY(w.open(zipAsRar).ok());
            QVERIFY(w.addData(QStringLiteral("eboot.bin"), game.at(0).data).ok());
            QVERIFY(w.addData(QStringLiteral("sce_sys/param.json"), "{}").ok());
            QVERIFY(w.finish().ok());
        }
        root = fwa::unpackArchiveSource(zipAsRar, fresh(QStringLiteral("temp")), CancellationToken(), nullptr, 0);
        QVERIFY2(root.ok(), root.ok() ? "" : qPrintable(root.error().toLogString()));
        QCOMPARE(readFile(root.value() + QStringLiteral("/eboot.bin")), game.at(0).data);
    }

    void passwordProtectedIsRefused_data()
    {
        QTest::addColumn<QString>("path");
        const QString ours = fwtest::dataDir() + QStringLiteral("/archives/");
        QTest::newRow("7z data") << ours + QStringLiteral("game-encrypted.7z");
        QTest::newRow("7z list of files") << ours + QStringLiteral("game-encrypted-header.7z");
        QTest::newRow("rar4 data") << sample(QStringLiteral("test_read_format_rar4_encrypted.rar"));
        QTest::newRow("rar4 names") << sample(QStringLiteral("test_read_format_rar4_encrypted_filenames.rar"));
        QTest::newRow("rar5 data") << sample(QStringLiteral("test_read_format_rar5_encrypted.rar"));
        QTest::newRow("rar5 names") << sample(QStringLiteral("test_read_format_rar5_encrypted_filenames.rar"));
    }
    // Said when the archive is chosen, not after half of it is unpacked.
    void passwordProtectedIsRefused()
    {
        QFETCH(QString, path);
        QVERIFY(QFile::exists(path));
        Status st = success();
        auto listing = fwa::listArchive(path);
        if (!listing.ok())
            st = listing.error();
        else
            st = fwa::checkExtractable(listing.value());
        QVERIFY(!st.ok());
        QCOMPARE(st.error().code, ErrorCode::Unsupported);
        QVERIFY2(st.error().message.contains(QStringLiteral("is password-protected. Unpack it yourself and choose the folder.")),
                 qPrintable(st.error().message));
        if (path.endsWith(QStringLiteral(".7z"))) {
            const Status source = fwa::checkArchiveSource(path);
            QVERIFY(!source.ok());
            QVERIFY2(source.error().message.contains(QStringLiteral("password-protected")), qPrintable(source.error().message));
            const QString temp = fresh(QStringLiteral("temp"));
            QVERIFY(!fwa::unpackArchiveSource(path, temp, CancellationToken(), nullptr, 0).ok());
            QVERIFY(!QFileInfo::exists(temp + QStringLiteral("/zip-source")));
        }
    }

    // BZip2 inside 7z is not built in: the job says which method it met.
    void unsupportedMethodIsNamed()
    {
        const QString a = fwtest::dataDir() + QStringLiteral("/archives/game-bzip2.7z");
        const QString temp = fresh(QStringLiteral("temp"));
        auto root = fwa::unpackArchiveSource(a, temp, CancellationToken(), nullptr, 0);
        QVERIFY(!root.ok());
        QCOMPARE(root.error().code, ErrorCode::Unsupported);
        QVERIFY2(root.error().message.contains(QStringLiteral("BZ2")), qPrintable(root.error().message));
        QVERIFY(noPartFiles(temp));
        // A solid archive in the RAR 4 format is refused when it is chosen.
        auto solid = fwa::listArchive(sample(QStringLiteral("test_read_format_rar4_solid_encrypted.rar")));
        QVERIFY(!solid.ok());
        QCOMPARE(solid.error().code, ErrorCode::Unsupported);
        QVERIFY2(solid.error().message.contains(QStringLiteral("solid archive in the old RAR 4 format")), qPrintable(solid.error().message));
    }

    // A download that stopped early: one sentence, and nothing but the
    // temporary folder is left.
    void truncatedArchivesFailCleanly()
    {
        const QString whole = fresh(QStringLiteral("whole.7z"));
        QVERIFY(write7z(whole, dummyGame(), "lzma2"));
        const QByteArray bytes = readFile(whole);
        QVERIFY(bytes.size() > 5 * 1024 * 1024);
        for (const qsizetype keep : {qsizetype(20), qsizetype(31), qsizetype(4096), bytes.size() / 2, bytes.size() - 1}) {
            const QString cut = fresh(QStringLiteral("cut.7z"));
            QVERIFY(writeFile(cut, bytes.left(keep)));
            const Status st = fwa::checkArchiveSource(cut);
            QVERIFY2(!st.ok(), qPrintable(QString::number(keep)));
            QCOMPARE(st.error().code, ErrorCode::CorruptData);
            QVERIFY2(st.error().message.contains(QStringLiteral("cut.7z")) && !st.error().message.contains(u'\n'),
                     qPrintable(st.error().message));
            const QString temp = fresh(QStringLiteral("temp"));
            QVERIFY(!fwa::unpackArchiveSource(cut, temp, CancellationToken(), nullptr, 0).ok());
            QVERIFY(filesUnder(temp).isEmpty());
        }
        for (const QString& name : {QStringLiteral("test_read_format_rar5_multiple_files.rar"),
                                    QStringLiteral("test_read_format_rar_compress_normal.rar")}) {
            const QByteArray rar = readFile(sample(name));
            QVERIFY(rar.size() > 1000);
            // Inside a file's data or a header. (This RAR 4 sample holds one
            // file four times: cut exactly between two of them it is a
            // shorter archive that nothing marks as incomplete, and its last
            // seven bytes are an end record a reader does not need.)
            for (const qsizetype keep : {qsizetype(9), qsizetype(40), rar.size() / 3 + 1, rar.size() / 2 + 7, rar.size() - 40}) {
                const QString cut = fresh(QStringLiteral("cut.rar"));
                QVERIFY(writeFile(cut, rar.left(keep)));
                const QString out = fresh(QStringLiteral("out"));
                auto listing = fwa::listArchive(cut);
                Status st = listing.ok() ? Status(success()) : Status(listing.error());
                if (listing.ok()) {
                    auto r = fwa::extractArchive(listing.value(), out, {}, CancellationToken());
                    if (!r.ok())
                        st = r.error();
                }
                QVERIFY2(!st.ok(), qPrintable(QStringLiteral("%1 cut to %2").arg(name).arg(keep)));
                QVERIFY2(!st.error().message.isEmpty() && !st.error().message.contains(u'\n'), qPrintable(st.error().message));
                QVERIFY(noPartFiles(out));
            }
        }
    }

    // A flipped byte in the middle: the checksum of the file it lies in
    // fails, and the message names that file.
    void damagedDataNamesTheEntry()
    {
        const QList<Item> game = dummyGame();
        const QString a = fresh(QStringLiteral("damaged.7z"));
        QVERIFY(write7z(a, game, "store"));
        QByteArray bytes = readFile(a);
        // Stored one after the other from byte 32: this lands in data/big.bin.
        const qsizetype at = 32 + bytesOf(game) - 1024 * 1024;
        bytes[at] = char(bytes.at(at) ^ 0x55);
        QVERIFY(writeFile(a, bytes));
        QVERIFY(fwa::checkArchiveSource(a).ok()); // the list of files is intact
        const QString temp = fresh(QStringLiteral("temp"));
        auto root = fwa::unpackArchiveSource(a, temp, CancellationToken(), nullptr, 0);
        QVERIFY(!root.ok());
        QCOMPARE(root.error().code, ErrorCode::CorruptData);
        QVERIFY2(root.error().message.contains(QStringLiteral("data/big.bin")), qPrintable(root.error().message));
        QVERIFY(noPartFiles(temp));
        QVERIFY(!QFile::exists(temp + QStringLiteral("/zip-source/data/big.bin")));
    }

    void craftedPathsAreRefused_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<QString>("says");
        QTest::newRow("parent") << QStringLiteral("../evil.bin") << QStringLiteral("unsafe path");
        QTest::newRow("parent, backslash") << QStringLiteral("..\\evil.bin") << QStringLiteral("unsafe path");
        QTest::newRow("parent inside") << QStringLiteral("data/../../evil.bin") << QStringLiteral("unsafe path");
        QTest::newRow("absolute") << QStringLiteral("/evil.bin") << QStringLiteral("unsafe path");
        QTest::newRow("drive") << QStringLiteral("C:/evil.bin") << QStringLiteral("unsafe path");
        QTest::newRow("device") << QStringLiteral("data/CON") << QStringLiteral("cannot store");
        QTest::newRow("stream") << QStringLiteral("data/a:b") << QStringLiteral("cannot store");
        QTest::newRow("trailing dot") << QStringLiteral("data/name.") << QStringLiteral("cannot store");
        QTest::newRow("trailing space") << QStringLiteral("data/name ") << QStringLiteral("cannot store");
    }
    // Refused when the archive is checked, so before anything is written.
    void craftedPathsAreRefused()
    {
        QFETCH(QString, name);
        QFETCH(QString, says);
        const QString a = fresh(QStringLiteral("crafted.7z"));
        QVERIFY(write7z(a, {file(QStringLiteral("eboot.bin"), "e"), file(QStringLiteral("sce_sys/param.json"), "{}"), file(name, "evil")}, "store"));
        // The writer must have kept the name for this to test anything.
        auto listing = fwa::listArchive(a);
        QVERIFY2(listing.ok(), listing.ok() ? "" : qPrintable(listing.error().toLogString()));
        QString kept = name;
        kept.replace(u'\\', u'/');
        bool found = false;
        for (const fwa::ArchiveEntry& e : listing->entries)
            found = found || e.name == kept;
        if (!found)
            QSKIP("libarchive's 7z writer changed this name while writing it; the reader's own check is tested by the other rows");
        const Status st = fwa::checkArchiveSource(a);
        QVERIFY(!st.ok());
        QVERIFY2(st.error().message.contains(says), qPrintable(st.error().message));
        const QString temp = fresh(QStringLiteral("temp"));
        auto root = fwa::unpackArchiveSource(a, temp, CancellationToken(), nullptr, 0);
        QVERIFY(!root.ok());
        QVERIFY(!QFileInfo::exists(temp + QStringLiteral("/zip-source")));
        // The reader itself refuses it too, whoever calls it.
        const QString out = fresh(QStringLiteral("out"));
        auto direct = fwa::extractArchive(listing.value(), out, {}, CancellationToken());
        if (says == QStringLiteral("unsafe path")) {
            QVERIFY(!direct.ok());
            QVERIFY(!QFileInfo::exists(out));
        }
        QVERIFY(!QFile::exists(QFileInfo(out).absolutePath() + QStringLiteral("/evil.bin")));
    }

    void namesThatDifferOnlyInCaseAreRefused()
    {
        const QString a = fresh(QStringLiteral("case.7z"));
        QVERIFY(write7z(a, {file(QStringLiteral("eboot.bin"), "e"), file(QStringLiteral("Data/a.bin"), "1"), file(QStringLiteral("data/b.bin"), "2")}, "store"));
        Status st = fwa::checkArchiveSource(a);
        QVERIFY(!st.ok());
        QCOMPARE(st.error().code, ErrorCode::Unsupported);
        QVERIFY2(st.error().message.contains(QStringLiteral("Data")) && st.error().message.contains(QStringLiteral("data"))
                     && st.error().message.contains(QStringLiteral("upper and lower case")),
                 qPrintable(st.error().message));
        const QString b = fresh(QStringLiteral("case2.7z"));
        QVERIFY(write7z(b, {file(QStringLiteral("eboot.bin"), "e"), file(QStringLiteral("x.bin"), "1"), file(QStringLiteral("X.BIN"), "2")}, "store"));
        st = fwa::checkArchiveSource(b);
        QVERIFY(!st.ok());
        const QString temp = fresh(QStringLiteral("temp"));
        QVERIFY(!fwa::unpackArchiveSource(b, temp, CancellationToken(), nullptr, 0).ok());
        QVERIFY(!QFileInfo::exists(temp + QStringLiteral("/zip-source")));
        // The same name twice is no better.
        const QString c = fresh(QStringLiteral("twice.7z"));
        QVERIFY(write7z(c, {file(QStringLiteral("eboot.bin"), "e"), file(QStringLiteral("x.bin"), "1"), file(QStringLiteral("x.bin"), "2")}, "store"));
        QVERIFY(!fwa::checkArchiveSource(c).ok());
    }

    // Links are never created; the game is unpacked without them and the
    // count says how many there were.
    void linksAreSkippedAndCounted()
    {
        const QString a = fresh(QStringLiteral("links.7z"));
        QVERIFY(write7z(a, {file(QStringLiteral("eboot.bin"), "eboot"), symLink(QStringLiteral("data/link.bin"), QStringLiteral("../eboot.bin")),
                            symLink(QStringLiteral("escape"), QStringLiteral("C:/Windows"))}, "store"));
        auto g = fwa::inspectArchive(a);
        QVERIFY2(g.ok(), g.ok() ? "" : qPrintable(g.error().toLogString()));
        QCOMPARE(g->links, qint64(2));
        QCOMPARE(g->files, qint64(1));
        const QString temp = fresh(QStringLiteral("temp"));
        auto root = fwa::unpackArchiveSource(a, temp, CancellationToken(), nullptr, 0);
        QVERIFY2(root.ok(), root.ok() ? "" : qPrintable(root.error().toLogString()));
        QCOMPARE(filesUnder(root.value()), QStringList{QStringLiteral("eboot.bin")});
        QVERIFY(!QFileInfo::exists(root.value() + QStringLiteral("/escape")));
        QVERIFY(QFileInfo(root.value() + QStringLiteral("/escape")).symLinkTarget().isEmpty());
    }

    void cancellationMidEntry()
    {
        const QList<Item> game = dummyGame();
        const QString a = fresh(QStringLiteral("cancel.7z"));
        QVERIFY(write7z(a, game, "store"));
        CancellationSource cs;
        // Stops once two MiB of the five-MiB file are out.
        ProgressReporter progress([&](const ProgressSnapshot& s) {
            if (s.done > 100000 + 4000 + 257 + 200000 + 2 * 1024 * 1024)
                cs.cancel();
        }, 0);
        const QString temp = fresh(QStringLiteral("temp"));
        auto root = fwa::unpackArchiveSource(a, temp, cs.token(), &progress, 0);
        QVERIFY(!root.ok());
        QVERIFY(root.error().isCancelled());
        QVERIFY(noPartFiles(temp));
        QVERIFY(!QFile::exists(temp + QStringLiteral("/zip-source/data/big.bin")));
        // Cancelled before it starts: nothing is read at all.
        CancellationSource early;
        early.cancel();
        auto listing = fwa::listArchive(a, early.token());
        QVERIFY(!listing.ok() && listing.error().isCancelled());
    }

    void notEnoughSpaceIsSaidBeforeUnpacking()
    {
        const QString a = fresh(QStringLiteral("space.7z"));
        QVERIFY(write7z(a, {file(QStringLiteral("eboot.bin"), noise(50000, 6))}, "store"));
        const QString temp = fresh(QStringLiteral("temp"));
        auto root = fwa::unpackArchiveSource(a, temp, CancellationToken(), nullptr, std::numeric_limits<qint64>::max() / 2);
        QVERIFY(!root.ok());
        QCOMPARE(root.error().code, ErrorCode::InsufficientSpace);
        QVERIFY(filesUnder(temp).isEmpty());
    }

    // The same game from a ZIP and from a 7z: the same files, the same bytes,
    // the same times to the second, the same folders. The image writers put
    // file times into the image.
    void zipAndSevenZipGiveTheSameTree()
    {
        const QList<Item> game = dummyGame();
        const QString z = fresh(QStringLiteral("game.zip"));
        {
            zip::Writer w;
            QVERIFY(w.open(z).ok());
            const QDateTime when = QDateTime::fromSecsSinceEpoch(kMtime);
            for (const Item& i : game) {
                if (i.kind == Item::Folder)
                    QVERIFY(w.addDirectory(i.name, when).ok());
                else
                    QVERIFY(w.addData(i.name, i.data, i.data.size() > 100000 ? zip::Method::Store : zip::Method::Deflate, when).ok());
            }
            QVERIFY(w.finish().ok());
        }
        const QString s = fresh(QStringLiteral("game.7z"));
        QVERIFY(write7z(s, game, "lzma2"));
        auto fromZip = fwa::unpackArchiveSource(z, fresh(QStringLiteral("temp")), CancellationToken(), nullptr, 0);
        auto fromSeven = fwa::unpackArchiveSource(s, fresh(QStringLiteral("temp")), CancellationToken(), nullptr, 0);
        QVERIFY2(fromZip.ok(), fromZip.ok() ? "" : qPrintable(fromZip.error().toLogString()));
        QVERIFY2(fromSeven.ok(), fromSeven.ok() ? "" : qPrintable(fromSeven.error().toLogString()));
        const auto everything = [](const QString& root) {
            QStringList out;
            QDirIterator it(root, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                const QFileInfo fi(it.next());
                const QString rel = QDir(root).relativeFilePath(fi.filePath());
                out << (fi.isDir() ? rel + u'/'
                                   : QStringLiteral("%1|%2|%3|%4").arg(rel).arg(fi.size()).arg(fi.lastModified().toSecsSinceEpoch())
                                         .arg(QString::fromLatin1(readFile(fi.filePath()).toHex().left(64))));
            }
            out.sort();
            return out;
        };
        const QStringList a = everything(fromZip.value()), b = everything(fromSeven.value());
        QCOMPARE(a.size(), 7 + 6); // seven files and six folders
        QCOMPARE(a, b);
        QCOMPARE(treeDiffers(fromZip.value(), game), QString());
        QCOMPARE(treeDiffers(fromSeven.value(), game), QString());
    }

    // param.json behind four MiB of one solid block: read while that is
    // within the budget, left out (not an error) when it is not.
    void inspectRespectsItsBudgetOnASolidArchive()
    {
        const QByteArray param = "{\"titleId\":\"PPSA01234\"}";
        const QString a = fresh(QStringLiteral("solid.7z"));
        QVERIFY(write7z(a, {file(QStringLiteral("data/big.bin"), noise(4 * 1024 * 1024, 7)), file(QStringLiteral("eboot.bin"), "eboot"),
                            file(QStringLiteral("sce_sys/param.json"), param)}, "lzma2"));
        fwa::setArchiveInspectBudgetForTesting(1024 * 1024, -1);
        auto g = fwa::inspectArchive(a);
        QVERIFY2(g.ok(), g.ok() ? "" : qPrintable(g.error().toLogString()));
        QVERIFY(g->hasParam);
        QCOMPARE(g->files, qint64(3));
        QCOMPARE(g->paramJson, QByteArray());
        QVERIFY(fwa::checkArchiveSource(a).ok());

        fwa::setArchiveInspectBudgetForTesting(-1, 0); // no time at all
        g = fwa::inspectArchive(a);
        QVERIFY(g.ok());
        QCOMPARE(g->paramJson, QByteArray());

        fwa::setArchiveInspectBudgetForTesting(64 * 1024 * 1024, 60000);
        g = fwa::inspectArchive(a);
        QVERIFY(g.ok());
        QCOMPARE(g->paramJson, param);
        // Unpacking never depends on the budget.
        fwa::setArchiveInspectBudgetForTesting(1, 0);
        auto root = fwa::unpackArchiveSource(a, fresh(QStringLiteral("temp")), CancellationToken(), nullptr, 0);
        QVERIFY2(root.ok(), root.ok() ? "" : qPrintable(root.error().toLogString()));
        QCOMPARE(readFile(root.value() + QStringLiteral("/sce_sys/param.json")), param);
    }

    // The list is kept between calls, but only while the file is the one
    // that was read.
    void aChangedArchiveIsReadAgain()
    {
        const QString a = fresh(QStringLiteral("changing.7z"));
        QVERIFY(write7z(a, {file(QStringLiteral("eboot.bin"), "one"), file(QStringLiteral("sce_sys/param.json"), "{\"v\":1}")}, "store"));
        auto g = fwa::inspectArchive(a);
        QVERIFY(g.ok());
        QCOMPARE(g->bytes, qint64(3 + 7));
        QCOMPARE(g->paramJson, QByteArray("{\"v\":1}"));
        QVERIFY(write7z(a, {file(QStringLiteral("eboot.bin"), "three"), file(QStringLiteral("more.bin"), "more"),
                            file(QStringLiteral("sce_sys/param.json"), "{\"v\":22}")}, "store"));
        g = fwa::inspectArchive(a);
        QVERIFY(g.ok());
        QCOMPARE(g->files, qint64(3));
        QCOMPARE(g->paramJson, QByteArray("{\"v\":22}"));
    }

    // The free-space check relied on the listed sizes: a file that turns out
    // larger stops the unpacking.
    void moreBytesThanListedStopsTheUnpack()
    {
        const QString a = fresh(QStringLiteral("grows.7z"));
        QVERIFY(write7z(a, {file(QStringLiteral("eboot.bin"), noise(1000, 8))}, "store"));
        auto listing = fwa::listArchive(a);
        QVERIFY(listing.ok());
        QVERIFY(write7z(a, {file(QStringLiteral("eboot.bin"), noise(900000, 8))}, "store"));
        const QString out = fresh(QStringLiteral("out"));
        auto r = fwa::extractArchive(listing.value(), out, {}, CancellationToken());
        QVERIFY(!r.ok());
        QCOMPARE(r.error().code, ErrorCode::CorruptData);
        QVERIFY2(r.error().message.contains(QStringLiteral("larger than the archive says")), qPrintable(r.error().message));
        QVERIFY(filesUnder(out).isEmpty());
        // A file the list does not have is not written either.
        QVERIFY(write7z(a, {file(QStringLiteral("eboot.bin"), noise(1000, 8)), file(QStringLiteral("extra.bin"), "x")}, "store"));
        const QString out2 = fresh(QStringLiteral("out"));
        r = fwa::extractArchive(listing.value(), out2, {}, CancellationToken());
        QVERIFY(r.ok()); // the pass stops once everything listed is out
        QCOMPARE(filesUnder(out2), QStringList{QStringLiteral("eboot.bin")});
    }

    // A header that announces more than the file can hold, or more than the
    // app reads, is refused from its first bytes.
    void hostileSevenZipHeaders()
    {
        const auto archiveWith = [](const QByteArray& header) {
            QByteArray start(32, '\0');
            memcpy(start.data(), "7z\xBC\xAF\x27\x1C\x00\x04", 8);
            qToLittleEndian<quint64>(0, start.data() + 12);
            qToLittleEndian<quint64>(quint64(header.size()), start.data() + 20);
            qToLittleEndian<quint32>(zip::crc32(header), start.data() + 28);
            qToLittleEndian<quint32>(zip::crc32(start.mid(12, 20)), start.data() + 8);
            return start + header;
        };
        // A packed list of files said to be a tebibyte when unpacked.
        const QString big = fresh(QStringLiteral("big-header.7z"));
        QVERIFY(writeFile(big, archiveWith(QByteArray::fromHex("17 06 00 01 09 0a 00 07 0b 01 00 01 23 03 01 01 05 5d 00 00 10 00 0c"
                                                              " f9 00 00 00 00 00 00 00"))));
        Status st = fwa::checkArchiveSource(big);
        QVERIFY(!st.ok());
        QCOMPARE(st.error().code, ErrorCode::Unsupported);
        QVERIFY2(st.error().message.contains(QStringLiteral("list of files too large")), qPrintable(st.error().message));
        // A list of files that claims four thousand million entries in six bytes.
        const QString many = fresh(QStringLiteral("many.7z"));
        QVERIFY(writeFile(many, archiveWith(QByteArray::fromHex("01 05 f0 ff ff ff ff 00 00"))));
        st = fwa::checkArchiveSource(many);
        QVERIFY(!st.ok());
        QVERIFY(!st.error().message.isEmpty());
        // The start header points past the end of the file.
        QByteArray cut = archiveWith(QByteArray(100, '\1'));
        qToLittleEndian<quint64>(5000, cut.data() + 20);
        qToLittleEndian<quint32>(zip::crc32(cut.mid(12, 20)), cut.data() + 8);
        const QString past = fresh(QStringLiteral("past.7z"));
        QVERIFY(writeFile(past, cut));
        st = fwa::checkArchiveSource(past);
        QVERIFY(!st.ok());
        QVERIFY2(st.error().message.contains(QStringLiteral("is incomplete")), qPrintable(st.error().message));
    }

    // ---- RAR: the samples of libarchive's test suite ---------------------------

    void rarSamplesRoundTrip()
    {
        // RAR 5, four files, each in its own stream and all in one (solid).
        for (const char* name : {"test_read_format_rar5_multiple_files.rar", "test_read_format_rar5_multiple_files_solid.rar"}) {
            auto listing = fwa::listArchive(sample(QString::fromLatin1(name)));
            QVERIFY2(listing.ok(), listing.ok() ? name : qPrintable(listing.error().toLogString()));
            QCOMPARE(listing->kind, fwa::ArchiveKind::Rar5);
            QCOMPARE(listing->solid, QByteArray(name).contains("solid"));
            QCOMPARE(listing->entries.size(), size_t(4));
            const QString out = fresh(QStringLiteral("out"));
            auto r = fwa::extractArchive(listing.value(), out, {}, CancellationToken());
            QVERIFY2(r.ok(), r.ok() ? name : qPrintable(r.error().toLogString()));
            QCOMPARE(r->files, qint64(4));
            QCOMPARE(r->bytes, qint64(4 * 4096));
            for (int i = 1; i <= 4; ++i)
                QCOMPARE(readFile(out + QStringLiteral("/test%1.bin").arg(i)), rar5TestData(i, 4096));
        }
        {
            // Only one file out of a solid archive: what lies before it is decoded, not written.
            auto listing = fwa::listArchive(sample(QStringLiteral("test_read_format_rar5_multiple_files_solid.rar")));
            QVERIFY(listing.ok());
            bool within = false;
            auto third = fwa::readArchiveFile(listing.value(), QStringLiteral("TEST3.bin"), 1 << 20, fwa::ReadBudget{}, &within);
            QVERIFY2(third.ok(), third.ok() ? "" : qPrintable(third.error().toLogString()));
            QVERIFY(within);
            QCOMPARE(third.value(), rar5TestData(3, 4096));
        }
        {
            // RAR 5, stored.
            auto listing = fwa::listArchive(sample(QStringLiteral("test_read_format_rar5_stored.rar")));
            QVERIFY(listing.ok());
            QCOMPARE(listing->entries.size(), size_t(1));
            QCOMPARE(listing->entries.front().name, QStringLiteral("helloworld.txt"));
            const QString out = fresh(QStringLiteral("out"));
            QVERIFY(fwa::extractArchive(listing.value(), out, {}, CancellationToken()).ok());
            QCOMPARE(QFileInfo(out + QStringLiteral("/helloworld.txt")).size(), listing->entries.front().size);
        }
        {
            // RAR 4: files, a folder, symbolic links (which are skipped).
            auto listing = fwa::listArchive(sample(QStringLiteral("test_read_format_rar.rar")));
            QVERIFY2(listing.ok(), listing.ok() ? "" : qPrintable(listing.error().toLogString()));
            QCOMPARE(listing->kind, fwa::ArchiveKind::Rar4);
            const QString out = fresh(QStringLiteral("out"));
            auto r = fwa::extractArchive(listing.value(), out, {}, CancellationToken());
            QVERIFY2(r.ok(), r.ok() ? "" : qPrintable(r.error().toLogString()));
            QVERIFY(r->linksSkipped >= 1);
            QCOMPARE(QFileInfo(out + QStringLiteral("/test.txt")).size(), qint64(20));
            QCOMPARE(QFileInfo(out + QStringLiteral("/testdir/test.txt")).size(), qint64(20));
            QVERIFY(!QFileInfo::exists(out + QStringLiteral("/testlink")));
            int folders = 0;
            for (const fwa::ArchiveEntry& e : listing->entries)
                if (e.type == fwa::ArchiveEntry::Type::Directory) {
                    ++folders;
                    QVERIFY2(QFileInfo(out + u'/' + e.name).isDir(), qPrintable(e.name));
                }
            QVERIFY(folders >= 2); // one of them empty
            // Only one folder of it.
            const QString part = fresh(QStringLiteral("out"));
            r = fwa::extractArchive(listing.value(), part, QStringLiteral("TESTDIR/"), CancellationToken());
            QVERIFY(r.ok());
            QCOMPARE(filesUnder(part), QStringList{QStringLiteral("testdir/test.txt")});
        }
        // RAR 4 with its two compression levels, and RAR 5 compressed: every
        // file's checksum is verified while it is read.
        for (const char* name : {"test_read_format_rar_compress_normal.rar", "test_read_format_rar_compress_best.rar",
                                 "test_read_format_rar_binary_data.rar", "test_read_format_rar5_compressed.rar",
                                 "test_read_format_rar5_blake2.rar", "test_read_format_rar5_arm.rar"}) {
            auto listing = fwa::listArchive(sample(QString::fromLatin1(name)));
            QVERIFY2(listing.ok(), listing.ok() ? name : qPrintable(listing.error().toLogString()));
            const QString out = fresh(QStringLiteral("out"));
            auto r = fwa::extractArchive(listing.value(), out, {}, CancellationToken());
            QVERIFY2(r.ok(), r.ok() ? name : qPrintable(QString::fromLatin1(name) + u' ' + r.error().toLogString()));
            qint64 listed = 0;
            for (const fwa::ArchiveEntry& e : listing->entries)
                if (e.type == fwa::ArchiveEntry::Type::File) {
                    listed += e.size;
                    QCOMPARE(QFileInfo(out + u'/' + e.name).size(), e.size);
                }
            QVERIFY(listed > 0);
            QCOMPARE(r->bytes, listed);
        }
    }

    void rarNamesOutsideAscii()
    {
        for (const char* name : {"test_read_format_rar_unicode.rar", "test_read_format_rar5_unicode.rar"}) {
            auto listing = fwa::listArchive(sample(QString::fromLatin1(name)));
            QVERIFY2(listing.ok(), listing.ok() ? name : qPrintable(listing.error().toLogString()));
            bool outsideAscii = false;
            for (const fwa::ArchiveEntry& e : listing->entries)
                for (const QChar ch : e.name) {
                    outsideAscii = outsideAscii || ch.unicode() > 127;
                    QVERIFY2(ch.unicode() != 0xFFFD && ch != u'?', qPrintable(e.name));
                }
            QVERIFY2(outsideAscii, name);
            const QString out = fresh(QStringLiteral("out"));
            auto r = fwa::extractArchive(listing.value(), out, {}, CancellationToken());
            QVERIFY2(r.ok(), r.ok() ? name : qPrintable(r.error().toLogString()));
            for (const fwa::ArchiveEntry& e : listing->entries)
                if (e.type == fwa::ArchiveEntry::Type::File)
                    QVERIFY2(QFileInfo(out + u'/' + e.name).isFile(), qPrintable(e.name));
        }
    }

    // A multi-part RAR is read from its first part; any other part says
    // which file to choose, and a part that is not there is named.
    void multiPartRar()
    {
        struct Set {
            const char* pattern;
            const char* first;
            const char* third;
            int parts;
        };
        for (const Set& set : {Set{"test_read_format_rar5_multiarchive.part*.rar", "test_read_format_rar5_multiarchive.part01.rar",
                                   "test_read_format_rar5_multiarchive.part03.rar", 8},
                               Set{"test_read_format_rar_multivolume.part*.rar", "test_read_format_rar_multivolume.part0001.rar",
                                   "test_read_format_rar_multivolume.part0003.rar", 4}}) {
            const QStringList names = sampleNames(QString::fromLatin1(set.pattern));
            QCOMPARE(names.size(), set.parts);
            for (const QString& n : names)
                QVERIFY(!sample(n).isEmpty());
            const QString first = sample(QString::fromLatin1(set.first));
            auto listing = fwa::listArchive(first);
            QVERIFY2(listing.ok(), listing.ok() ? "" : qPrintable(listing.error().toLogString()));
            QCOMPARE(listing->volumes.size(), set.parts);
            const QString out = fresh(QStringLiteral("out"));
            auto r = fwa::extractArchive(listing.value(), out, {}, CancellationToken());
            QVERIFY2(r.ok(), r.ok() ? "" : qPrintable(r.error().toLogString()));
            qint64 listed = 0;
            for (const fwa::ArchiveEntry& e : listing->entries)
                if (e.type == fwa::ArchiveEntry::Type::File) {
                    listed += e.size;
                    QCOMPARE(QFileInfo(out + u'/' + e.name).size(), e.size);
                }
            QVERIFY(listed > 0);
            QCOMPARE(r->bytes, listed);

            auto later = fwa::listArchive(sample(QString::fromLatin1(set.third)));
            QVERIFY(!later.ok());
            QCOMPARE(later.error().code, ErrorCode::InvalidArgument);
            QVERIFY2(later.error().message.contains(QStringLiteral("Choose the first part, ") + QString::fromLatin1(set.first)),
                     qPrintable(later.error().message));

            // The same set with its third part gone.
            const QString dir = fresh(QStringLiteral("incomplete"));
            for (const QString& n : names)
                if (n != QString::fromLatin1(set.third))
                    QVERIFY(writeFile(dir + u'/' + n, readFile(sample(n))));
            auto incomplete = fwa::listArchive(dir + u'/' + QString::fromLatin1(set.first));
            Status st = incomplete.ok() ? Status(success()) : Status(incomplete.error());
            if (incomplete.ok()) {
                auto e = fwa::extractArchive(incomplete.value(), fresh(QStringLiteral("out")), {}, CancellationToken());
                if (!e.ok())
                    st = e.error();
            }
            QVERIFY(!st.ok());
            QVERIFY2(st.error().message.contains(QString::fromLatin1(set.third)), qPrintable(st.error().message));
        }
    }

    // More sets: several files across the parts, one file across all of
    // them, stored files, and a solid RAR 5 set.
    void multiPartRarSets_data()
    {
        QTest::addColumn<QString>("pattern");
        QTest::addColumn<QString>("first");
        QTest::newRow("rar4 several files") << QStringLiteral("test_rar_multivolume_multiple_files.part*.rar")
                                            << QStringLiteral("test_rar_multivolume_multiple_files.part1.rar");
        QTest::newRow("rar4 one file") << QStringLiteral("test_rar_multivolume_single_file.part*.rar")
                                       << QStringLiteral("test_rar_multivolume_single_file.part1.rar");
        QTest::newRow("rar4 stored") << QStringLiteral("test_rar_multivolume_uncompressed_files.part*.rar")
                                     << QStringLiteral("test_rar_multivolume_uncompressed_files.part01.rar");
        QTest::newRow("rar5 solid") << QStringLiteral("test_read_format_rar5_multiarchive_solid.part*.rar")
                                    << QStringLiteral("test_read_format_rar5_multiarchive_solid.part01.rar");
    }
    void multiPartRarSets()
    {
        QFETCH(QString, pattern);
        QFETCH(QString, first);
        const QStringList names = sampleNames(pattern);
        QVERIFY(names.size() >= 3);
        for (const QString& n : names)
            QVERIFY(!sample(n).isEmpty());
        auto listing = fwa::listArchive(sample(first));
        QVERIFY2(listing.ok(), listing.ok() ? "" : qPrintable(listing.error().toLogString()));
        QCOMPARE(listing->volumes.size(), names.size());
        const QString out = fresh(QStringLiteral("out"));
        auto r = fwa::extractArchive(listing.value(), out, {}, CancellationToken());
        QVERIFY2(r.ok(), r.ok() ? "" : qPrintable(r.error().toLogString()));
        qint64 listed = 0, files = 0;
        for (const fwa::ArchiveEntry& e : listing->entries)
            if (e.type == fwa::ArchiveEntry::Type::File) {
                listed += e.size;
                ++files;
                QCOMPARE(QFileInfo(out + u'/' + e.name).size(), e.size);
            }
        QVERIFY(files > 0 && listed > 0);
        QCOMPARE(r->files, files);
        QCOMPARE(r->bytes, listed);
    }

    // Every RAR and 7z sample of libarchive's suite, most of them written to
    // break a reader: each is listed and, when it lists, unpacked. None may
    // crash, hang or leave a half-written file.
    void hostileSamplesEndWithAnAnswer()
    {
        QStringList names = sampleNames(QStringLiteral("*.rar")) + sampleNames(QStringLiteral("*.7z"));
        QVERIFY(names.size() > 100);
        for (const QString& name : names) // all of them first: the parts of a set belong together
            QVERIFY2(!sample(name).isEmpty(), qPrintable(name));
        int listedCount = 0, unpacked = 0, refused = 0;
        for (const QString& name : names) {
            const QString path = sample(name);
            auto listing = fwa::listArchive(path);
            if (!listing.ok()) {
                QVERIFY2(!listing.error().message.isEmpty(), qPrintable(name));
                ++refused;
                continue;
            }
            ++listedCount;
            const QString out = fresh(QStringLiteral("out"));
            auto r = fwa::extractArchive(listing.value(), out, {}, CancellationToken());
            if (r.ok()) {
                ++unpacked;
                QCOMPARE(qint64(filesUnder(out).size()), r->files);
            } else {
                QVERIFY2(!r.error().message.isEmpty(), qPrintable(name));
                ++refused;
            }
            QVERIFY2(noPartFiles(out), qPrintable(name));
        }
        qInfo("%d samples: %d listed, %d unpacked, %d refused", int(names.size()), listedCount, unpacked, refused);
        QVERIFY(unpacked > 60);
        QVERIFY(refused > 20);
    }

    // What the app's own zstd and the filters of liblzma add to 7z.
    void sevenZipSamplesOfOtherCoders()
    {
        for (const char* name : {"test_read_format_7zip_zstd.7z", "test_read_format_7zip_solid_zstd.7z", "test_read_format_7zip_zstd_bcj.7z",
                                 "test_read_format_7zip_bcj2_lzma2_1.7z", "test_read_format_7zip_lzma2_arm64.7z",
                                 "test_read_format_7zip_delta4_lzma2.7z", "test_read_format_7zip_ppmd.7z",
                                 "test_read_format_7zip_lzma1_lzma2.7z", "test_read_format_7zip_deflate.7z",
                                 "test_read_format_7zip_copy.7z"}) {
            auto listing = fwa::listArchive(sample(QString::fromLatin1(name)));
            QVERIFY2(listing.ok(), listing.ok() ? name : qPrintable(listing.error().toLogString()));
            const QString out = fresh(QStringLiteral("out"));
            auto r = fwa::extractArchive(listing.value(), out, {}, CancellationToken());
            QVERIFY2(r.ok(), r.ok() ? name : qPrintable(QString::fromLatin1(name) + u' ' + r.error().toLogString()));
            QVERIFY2(r->files > 0 && r->bytes > 0, name);
        }
    }
};

QTEST_GUILESS_MAIN(TstArchiveSource)
#include "tst_archive_source.moc"
