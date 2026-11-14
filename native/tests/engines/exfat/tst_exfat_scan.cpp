// The source scan and the extractor on Windows file attributes and times.
//
//  * The scan takes ordinary files and folders straight from the folder
//    listing. What it records must be exactly what QFileInfo and
//    GetFileAttributesW give for them (the scan used to ask them for every
//    entry): size, exFAT attributes, all three times (same instant, same time
//    spec), hidden-file exclusion, long paths. Folders that hold links or
//    shortcuts are scanned as before; their plain entries get the same values.
//  * Extraction sets times and attributes through the file it has just
//    written; read back they must be those of the image, in both extraction
//    modes, archive bit cleared included.

#include "ExfatTestUtil.h"

#include "core/FileUtil.h"
#include "core/FsLinks.h"
#include "engines/exfat/ExfatExtract.h"
#include "engines/exfat/SourceScan.h"
#include "support/SparseFile.h"
#include "support/TestSupport.h"

#include <QDateTime>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>
#include <QTimeZone>

using namespace fw;
using namespace fw::exfat;
using namespace exfattest;

#ifdef Q_OS_WIN
namespace {

constexpr DWORD kExfatBits = FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE;

std::wstring native(const QString& path) { return toLongPath(path).toStdWString(); }

bool setAttributes(const QString& path, DWORD attributes)
{
    return SetFileAttributesW(native(path).c_str(), attributes) != 0;
}

DWORD attributesOf(const QString& path) { return GetFileAttributesW(native(path).c_str()); }

FILETIME fileTime(const QDateTime& t)
{
    const quint64 ticks = quint64(t.toMSecsSinceEpoch() + 11644473600000LL) * 10000ULL;
    FILETIME ft;
    ft.dwLowDateTime = DWORD(ticks & 0xFFFFFFFFu);
    ft.dwHighDateTime = DWORD(ticks >> 32);
    return ft;
}

bool setTimes(const QString& path, const QDateTime& created, const QDateTime& accessed, const QDateTime& modified)
{
    HANDLE h = CreateFileW(native(path).c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    const FILETIME c = fileTime(created), a = fileTime(accessed), m = fileTime(modified);
    const bool ok = SetFileTime(h, &c, &a, &m) != 0;
    CloseHandle(h);
    return ok;
}

// What the scan recorded for every entry before it read the listing itself:
// QDir's listing, QFileInfo's size and times, GetFileAttributesW (through
// sourceAttributes, which still asks it). Only for folders without links or
// shortcuts, which is what the tests below give it.
struct Expected {
    bool isDir = false;
    qint64 size = 0;
    quint16 attributes = 0;
    QDateTime created, modified, accessed;
    bool hidden = false;
};

void listTheOldWay(const QString& dir, const QString& rel, QMap<QString, Expected>& out)
{
    const QFileInfoList list = QDir(toLongPath(dir)).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                                                                   QDir::NoSort);
    for (const QFileInfo& fi : list) {
        if (fi.isShortcut() || fi.isSymLink() || fi.isJunction())
            continue; // not plain entries
        const QString name = fi.fileName();
        const QString abs = dir + u'/' + name;
        const QString r = rel.isEmpty() ? name : rel + u'/' + name;
        Expected e;
        e.isDir = fi.isDir();
        e.size = e.isDir ? 0 : fi.size();
        e.attributes = sourceAttributes(abs, e.isDir, fi.isWritable());
        e.modified = fi.lastModified();
        e.created = fi.birthTime().isValid() ? fi.birthTime() : e.modified;
        e.accessed = fi.lastRead().isValid() ? fi.lastRead() : e.modified;
        e.hidden = fi.isHidden();
        out.insert(r, e);
        if (e.isDir)
            listTheOldWay(abs, r, out);
    }
}

QString sameTime(const QString& what, const QDateTime& got, const QDateTime& want)
{
    if (got != want || got.timeSpec() != want.timeSpec() || got.isValid() != want.isValid())
        return QStringLiteral("%1: %2 (spec %3), expected %4 (spec %5)")
            .arg(what, got.toString(Qt::ISODateWithMs))
            .arg(int(got.timeSpec()))
            .arg(want.toString(Qt::ISODateWithMs))
            .arg(int(want.timeSpec()));
    return {};
}

// Every node of `tree` against `expected`; empty when they agree. With
// `onlyExpected`, nodes `expected` does not name are not looked at.
QString compare(const SourceTree& tree, const QMap<QString, Expected>& expected, const QStringList& leftOut = {},
                bool onlyExpected = false)
{
    QStringList problems;
    QSet<QString> seen;
    for (size_t i = 1; i < tree.nodes.size(); ++i) {
        const SourceNode& n = tree.nodes[i];
        seen.insert(n.relativePath);
        const auto it = expected.constFind(n.relativePath);
        if (it == expected.cend()) {
            if (!onlyExpected)
                problems << QStringLiteral("%1: not expected").arg(n.relativePath);
            continue;
        }
        const Expected& e = *it;
        if (n.isDir != e.isDir)
            problems << QStringLiteral("%1: isDir %2").arg(n.relativePath).arg(n.isDir);
        if (n.size != e.size)
            problems << QStringLiteral("%1: size %2, expected %3").arg(n.relativePath).arg(n.size).arg(e.size);
        if (n.attributes != e.attributes)
            problems << QStringLiteral("%1: attributes 0x%2, expected 0x%3").arg(n.relativePath).arg(n.attributes, 0, 16).arg(e.attributes, 0, 16);
        // A folder's last access time moves each time it is listed (the
        // passes of both ways list every folder), so it is only compared for files.
        for (const QString& p : {sameTime(n.relativePath + QStringLiteral(" modified"), n.modified, e.modified),
                                 sameTime(n.relativePath + QStringLiteral(" created"), n.created, e.created),
                                 n.isDir ? QString() : sameTime(n.relativePath + QStringLiteral(" accessed"), n.accessed, e.accessed)})
            if (!p.isEmpty())
                problems << p;
        if (n.absolutePath != tree.root + u'/' + n.relativePath)
            problems << QStringLiteral("%1: absolute path %2").arg(n.relativePath, n.absolutePath);
    }
    for (auto it = expected.cbegin(); it != expected.cend(); ++it)
        if (!seen.contains(it.key()) && !leftOut.contains(it.key()))
            problems << QStringLiteral("%1: missing from the scan").arg(it.key());
    return problems.join(u'\n');
}

// Listing a folder changes what the next look at it says: NTFS sets the
// folder's last access time, and brings the copy of its times in the
// parent's listing up to date once the folder has been opened. A first pass
// of both ways over the freshly made tree settles the second part.
void settleFolderTimes(const QString& root)
{
    QMap<QString, Expected> ignored;
    listTheOldWay(root, QString(), ignored);
    CancellationToken token;
    (void)scanSource(root, {}, token);
}

} // namespace
#endif

class TestExfatScan : public QObject {
    Q_OBJECT

    QTemporaryDir m_tmp;

#ifdef Q_OS_WIN
    QString path(const QString& rel) const { return m_tmp.filePath(rel); }
#endif

private slots:
    void initTestCase()
    {
        QVERIFY(m_tmp.isValid());
        fwtest::isolateAppData(m_tmp);
#ifndef Q_OS_WIN
        QSKIP("Windows attributes and folder listings");
#endif
    }

#ifdef Q_OS_WIN
    void plainFoldersAreScannedAsQFileInfoSeesThem()
    {
        const QString src = path(QStringLiteral("plain"));
        QVERIFY(makeSampleTree(src, 60));
        QVERIFY(writeBytes(src + QStringLiteral("/attr/hidden.bin"), "h"));
        QVERIFY(writeBytes(src + QStringLiteral("/attr/system.bin"), "s"));
        QVERIFY(writeBytes(src + QStringLiteral("/attr/readonly.bin"), "r"));
        QVERIFY(writeBytes(src + QStringLiteral("/attr/no-archive.bin"), "n"));
        QVERIFY(writeBytes(src + QStringLiteral("/attr/hidden-system.bin"), "hs"));
        QVERIFY(writeBytes(src + QStringLiteral("/attr/.dotted"), "a dot is not hidden on Windows"));
        QVERIFY(writeBytes(src + QStringLiteral("/attr/Hidden Folder/inside.txt"), "in a hidden folder"));
        QVERIFY(writeBytes(src + QStringLiteral("/attr/Read-only Folder/inside.txt"), "in a read-only folder"));
        QVERIFY(setAttributes(src + QStringLiteral("/attr/hidden.bin"), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_ARCHIVE));
        QVERIFY(setAttributes(src + QStringLiteral("/attr/system.bin"), FILE_ATTRIBUTE_SYSTEM));
        QVERIFY(setAttributes(src + QStringLiteral("/attr/readonly.bin"), FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_ARCHIVE));
        QVERIFY(setAttributes(src + QStringLiteral("/attr/no-archive.bin"), FILE_ATTRIBUTE_NORMAL));
        QVERIFY(setAttributes(src + QStringLiteral("/attr/hidden-system.bin"), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM));
        QVERIFY(setAttributes(src + QStringLiteral("/attr/Hidden Folder"), FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_HIDDEN));
        QVERIFY(setAttributes(src + QStringLiteral("/attr/Read-only Folder"), FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY));
        // Times with milliseconds, before and after the exFAT epoch, in summer and winter.
        const QDateTime created(QDate(2001, 2, 3), QTime(4, 5, 6, 789), QTimeZone::UTC);
        const QDateTime accessed(QDate(2026, 7, 14), QTime(23, 59, 58, 1), QTimeZone::UTC);
        const QDateTime modified(QDate(1979, 12, 31), QTime(12, 0, 0, 999), QTimeZone::UTC);
        QVERIFY(writeBytes(src + QStringLiteral("/times/old.bin"), "old"));
        QVERIFY(setTimes(src + QStringLiteral("/times/old.bin"), created, accessed, modified));
        QVERIFY(setTimes(src + QStringLiteral("/times"), modified, created, accessed));
        // A path far beyond MAX_PATH, and a 4 GiB + 1 size without the data (sparse where it can be).
        QString deep = src + QStringLiteral("/long");
        for (int i = 0; i < 6; ++i)
            deep += u'/' + QString(60, QChar(u'a' + i));
        QVERIFY(QDir().mkpath(toLongPath(deep)));
        QVERIFY(deep.size() > 400);
        {
            QFile f(toLongPath(deep + QStringLiteral("/far.bin")));
            QVERIFY(f.open(QIODevice::WriteOnly));
            QVERIFY(f.write("far") == 3);
        }
        QVERIFY(QDir().mkpath(src + QStringLiteral("/size")));
        const qint64 onDisk = fwtest::makeSparseFile(src + QStringLiteral("/size/large.bin"), 4LL * 1024 * 1024 * 1024 + 1);
        QVERIFY(onDisk >= 0);
        const auto restore = qScopeGuard([&] {
            (void)setAttributes(src + QStringLiteral("/attr/readonly.bin"), FILE_ATTRIBUTE_NORMAL);
            (void)setAttributes(src + QStringLiteral("/attr/Read-only Folder"), FILE_ATTRIBUTE_DIRECTORY);
            QFile::remove(src + QStringLiteral("/size/large.bin"));
        });

        settleFolderTimes(src);
        QMap<QString, Expected> expected;
        listTheOldWay(src, QString(), expected);
        QVERIFY(expected.contains(QStringLiteral("attr/hidden-system.bin")));
        QCOMPARE(expected.value(QStringLiteral("attr/no-archive.bin")).attributes, quint16(0));
        QCOMPARE(expected.value(QStringLiteral("attr/hidden-system.bin")).attributes, quint16(FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM));

        CancellationToken token;
        auto tree = scanSource(src, {}, token);
        QVERIFY2(tree.ok(), tree.ok() ? "" : qPrintable(tree.error().toLogString()));
        const QString problems = compare(*tree, expected);
        QVERIFY2(problems.isEmpty(), qPrintable(problems));
        QCOMPARE(tree->fileCount + tree->dirCount, qint64(expected.size()));
        QVERIFY(tree->warnings.isEmpty());

        // Hidden files left out, hidden folders kept (robocopy /XA:H), and
        // patterns: the same entries and counts as QFileInfo's hidden flag gives.
        ScanRules rules;
        rules.excludeHidden = true;
        rules.excludePatterns = {QStringLiteral("*.dat"), QStringLiteral("times/")};
        auto filtered = scanSource(src, rules, token);
        QVERIFY(filtered.ok());
        QStringList leftOut;
        qint64 excluded = 0;
        for (auto it = expected.cbegin(); it != expected.cend(); ++it) {
            const QString& rel = it.key();
            if (rel == u"times" || rel.startsWith(QLatin1String("times/"))) {
                if (rel == u"times")
                    ++excluded;
                leftOut << rel;
            } else if (!it->isDir && (it->hidden || rel.endsWith(QLatin1String(".dat")))) {
                ++excluded;
                leftOut << rel;
            }
        }
        const QString filteredProblems = compare(*filtered, expected, leftOut);
        QVERIFY2(filteredProblems.isEmpty(), qPrintable(filteredProblems));
        QCOMPARE(filtered->excludedCount, excluded);
        QVERIFY(filtered->excludedSamples.contains(QStringLiteral("times/")));
        QVERIFY(filtered->excludedSamples.contains(QStringLiteral("attr/hidden.bin")));
        for (const SourceNode& n : filtered->nodes)
            QVERIFY2(!leftOut.contains(n.relativePath), qPrintable(n.relativePath));
    }

    // Links and shortcuts are not plain entries: a folder that holds one is
    // scanned with QFileInfo as before, and its other entries get the same
    // values as in a plain folder.
    void foldersWithLinksAndShortcutsKeepTheirRules()
    {
        const QString src = path(QStringLiteral("links"));
        const QString outside = path(QStringLiteral("outside"));
        QVERIFY(writeBytes(outside + QStringLiteral("/behind.bin"), "behind the junction"));
        QVERIFY(writeBytes(src + QStringLiteral("/eboot.bin"), "eboot"));
        QVERIFY(writeBytes(src + QStringLiteral("/mixed/plain.bin"), "plain sibling"));
        QVERIFY(writeBytes(src + QStringLiteral("/mixed/hidden.bin"), "hidden sibling"));
        QVERIFY(setAttributes(src + QStringLiteral("/mixed/hidden.bin"), FILE_ATTRIBUTE_HIDDEN));
        QVERIFY(QDir().mkpath(src + QStringLiteral("/mixed/sub")));
        QVERIFY(writeBytes(src + QStringLiteral("/mixed/sub/inner.bin"), "inner"));
        QVERIFY(writeBytes(src + QStringLiteral("/shortcuts/target.bin"), QByteArray(5000, 't')));
        if (!QFile::link(src + QStringLiteral("/shortcuts/target.bin"), src + QStringLiteral("/shortcuts/to file.lnk"))
            || !QFile::link(outside, src + QStringLiteral("/shortcuts/to folder.lnk")))
            QSKIP("This machine cannot create shortcuts.");
        // The same shortcut with its suffix in capitals (a rename on Windows keeps the file).
        QVERIFY(MoveFileW(native(src + QStringLiteral("/shortcuts/to folder.lnk")).c_str(),
                          native(src + QStringLiteral("/shortcuts/TO FOLDER.LNK")).c_str()));
        QVERIFY(writeBytes(src + QStringLiteral("/shortcuts/plain.bin"), "next to shortcuts"));
        JunctionGuard away(src + QStringLiteral("/mixed/away")), back(src + QStringLiteral("/mixed/back"));
        if (!createDirectoryJunction(away.path(), outside).ok())
            QSKIP("This machine cannot create a junction.");
        QVERIFY(createDirectoryJunction(back.path(), src).ok());

        CancellationToken token;
        {
            // Folders just changed (junctions made in "mixed"): the listing of
            // their parent can still show their old times; the scan has the
            // folder's own.
            auto fresh = scanSource(src, {}, token);
            QVERIFY(fresh.ok());
            for (const SourceNode& n : fresh->nodes) {
                if (!n.isDir || n.relativePath.isEmpty() || n.relativePath.startsWith(QLatin1String("mixed/away")))
                    continue;
                const QFileInfo own(n.absolutePath);
                const QString p = sameTime(n.relativePath + QStringLiteral(" modified"), n.modified, own.lastModified())
                    + sameTime(n.relativePath + QStringLiteral(" created"), n.created, own.birthTime());
                QVERIFY2(p.isEmpty(), qPrintable(p));
            }
        }
        settleFolderTimes(src);
        QMap<QString, Expected> expected;
        listTheOldWay(src, QString(), expected);
        auto tree = scanSource(src, {}, token);
        QVERIFY2(tree.ok(), tree.ok() ? "" : qPrintable(tree.error().toLogString()));
        const auto node = [&](const QString& rel) -> const SourceNode* {
            for (const SourceNode& n : tree->nodes)
                if (n.relativePath == rel)
                    return &n;
            return nullptr;
        };
        // The junction to elsewhere is followed, the one back into the source left out with a warning.
        QVERIFY(node(QStringLiteral("mixed/away/behind.bin")));
        QVERIFY(!node(QStringLiteral("mixed/back")));
        QCOMPARE(tree->warnings.size(), 1);
        // Shortcuts are files of their own size, whatever they point at, in either case of ".lnk".
        for (const QString& rel : {QStringLiteral("shortcuts/to file.lnk"), QStringLiteral("shortcuts/TO FOLDER.LNK")}) {
            const SourceNode* n = node(rel);
            QVERIFY2(n, qPrintable(rel));
            QVERIFY(!n->isDir);
            QFile own(src + u'/' + rel);
            QVERIFY(own.open(QIODevice::ReadOnly));
            QCOMPARE(n->size, own.size());
            QVERIFY(n->size != 5000);
        }
        // Every plain entry, in a folder with links or not, as a plain folder records it.
        QVERIFY(expected.contains(QStringLiteral("mixed/plain.bin")) && expected.contains(QStringLiteral("shortcuts/plain.bin"))
                && expected.contains(QStringLiteral("mixed/sub/inner.bin")) && expected.contains(QStringLiteral("eboot.bin")));
        const QString problems = compare(*tree, expected, {}, true);
        QVERIFY2(problems.isEmpty(), qPrintable(problems));
        QVERIFY(node(QStringLiteral("mixed/hidden.bin"))->attributes & FILE_ATTRIBUTE_HIDDEN);
        // Nothing but those, the shortcuts and what lies behind the junction.
        for (const SourceNode& n : tree->nodes)
            QVERIFY2(n.relativePath.isEmpty() || expected.contains(n.relativePath) || n.relativePath.startsWith(QLatin1String("mixed/away"))
                         || n.relativePath.endsWith(QLatin1String(".lnk"), Qt::CaseInsensitive),
                     qPrintable(n.relativePath));

        // A junction whose folder is gone is named as such (it used to be
        // entered and reported as a folder that cannot be read).
        const QString gone = path(QStringLiteral("gone"));
        QVERIFY(QDir().mkpath(gone));
        JunctionGuard dead(src + QStringLiteral("/mixed/dead"));
        QVERIFY(createDirectoryJunction(dead.path(), gone).ok());
        QVERIFY(QDir(gone).removeRecursively());
        const auto broken = scanSource(src, {}, token);
        FWTEST_COMPARE_ERROR(broken, ErrorCode::NotFound);
        QVERIFY2(broken.error().message.contains(QLatin1String("link whose target is missing")), qPrintable(broken.error().toLogString()));
        QVERIFY2(broken.error().message.contains(QDir::toNativeSeparators(dead.path())), qPrintable(broken.error().toLogString()));
    }

    // Times and attributes of extracted files are set through the file just
    // written; read back they are the image's, in both modes.
    void extractionRestoresTimesAndAttributes_data()
    {
        QTest::addColumn<bool>("parallel");
        QTest::newRow("sequential") << false;
        QTest::newRow("parallel") << true;
    }

    void extractionRestoresTimesAndAttributes()
    {
        QFETCH(bool, parallel);
        const QString tag = parallel ? QStringLiteral("par") : QStringLiteral("seq");
        const QString src = path(QStringLiteral("xsrc-") + tag);
        const QStringList names = {QStringLiteral("normal.bin"), QStringLiteral("hidden.bin"), QStringLiteral("system.bin"),
                                   QStringLiteral("readonly.bin"), QStringLiteral("no-archive.bin"), QStringLiteral("empty.bin"),
                                   QStringLiteral("big.bin")};
        const DWORD attrs[] = {FILE_ATTRIBUTE_ARCHIVE, FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_ARCHIVE, FILE_ATTRIBUTE_SYSTEM,
                               FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_ARCHIVE, FILE_ATTRIBUTE_NORMAL, FILE_ATTRIBUTE_HIDDEN,
                               FILE_ATTRIBUTE_ARCHIVE};
        for (int i = 0; i < names.size(); ++i) {
            const QString f = src + QStringLiteral("/data/") + names[i];
            if (names[i] == u"empty.bin")
                QVERIFY(writeBytes(f, {}));
            else if (names[i] == u"big.bin")
                QVERIFY(writeRandom(f, 9 * 1024 * 1024 + 7, 5)); // more than one buffer
            else
                QVERIFY(writeRandom(f, 1000 + i, quint32(i)));
            QVERIFY(setTimes(f, QDateTime(QDate(2010, 1, 2 + i), QTime(3, 4, 5, 60), QTimeZone::UTC),
                             QDateTime(QDate(2024, 6, 1 + i), QTime(7, 8, 10), QTimeZone::UTC),
                             QDateTime(QDate(2019, 3, 4 + i), QTime(5, 6, 7, 890), QTimeZone::UTC)));
            QVERIFY(setAttributes(f, attrs[i]));
        }
        const auto restore = qScopeGuard([&] { (void)setAttributes(src + QStringLiteral("/data/readonly.bin"), FILE_ATTRIBUTE_NORMAL); });
        const QString image = path(QStringLiteral("x-") + tag + QStringLiteral(".exfat"));
        WriterOptions o;
        o.spare = SpareMode::Fixed;
        QVERIFY(build(src, image, o).ok());
        auto reader = ExfatReader::open(image);
        QVERIFY(reader.ok());
        CancellationToken token;
        auto tree = reader.value()->readTree(token);
        QVERIFY(tree.ok());

        const QString dest = path(QStringLiteral("x-") + tag);
        QVERIFY(QDir().mkpath(dest));
        ExtractOptions eo;
        eo.parallel = parallel;
        eo.threads = 4;
        eo.bufferBytes = 1024 * 1024;
        auto st = extractTree(*reader.value(), tree.value(), dest, eo, token);
        QVERIFY2(st.ok(), st.ok() ? "" : qPrintable(st.error().toLogString()));
        QVERIFY2(st->warnings.isEmpty(), qPrintable(st->warnings.join(u' ')));
        const auto unlock = qScopeGuard([&] { (void)setAttributes(dest + QStringLiteral("/data/readonly.bin"), FILE_ATTRIBUTE_NORMAL); });

        for (int i = 0; i < names.size(); ++i) {
            const QString rel = QStringLiteral("data/") + names[i];
            const int at = tree->find(rel);
            QVERIFY2(at > 0, qPrintable(rel));
            const ImageEntry& e = tree->entries[size_t(at)];
            const QString out = dest + u'/' + rel;
            const DWORD got = attributesOf(out);
            QVERIFY(got != INVALID_FILE_ATTRIBUTES);
            QCOMPARE(got & kExfatBits, DWORD(e.attributes) & kExfatBits);
            QCOMPARE(got & kExfatBits, attrs[i] & kExfatBits); // what the source had
            const QFileInfo fi(out);
            QCOMPARE(fi.lastModified().toMSecsSinceEpoch(), e.modified.toMSecsSinceEpoch());
            QCOMPARE(fi.birthTime().toMSecsSinceEpoch(), e.created.toMSecsSinceEpoch());
            QCOMPARE(fi.lastRead().toMSecsSinceEpoch(), e.accessed.toMSecsSinceEpoch());
            QCOMPARE(readAllOf(out), readAllOf(src + u'/' + rel));
        }
    }
#endif
};

QTEST_GUILESS_MAIN(TestExfatScan)
#include "tst_exfat_scan.moc"
