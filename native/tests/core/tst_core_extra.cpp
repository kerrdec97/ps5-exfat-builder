// Ordered JSON (Python-compatible writer), junctions/links, raw process output,
// the started hook and the CPU limit.

#include "core/CrashHandler.h"
#include "core/FileUtil.h"
#include "core/FsLinks.h"
#include "core/Paths.h"
#include "core/OrderedJson.h"
#include "core/PathSafety.h"
#include "core/ProcessRunner.h"
#include "support/TestSupport.h"

#include <QDir>
#include <QElapsedTimer>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QThreadPool>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <utime.h>
#endif

using namespace fw;

namespace {
QByteArray readAll(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

// Makes a folder look as if nothing in it had changed for `days` days.
bool setFolderAgeDays(const QString& folder, int days)
{
    const QDateTime when = QDateTime::currentDateTimeUtc().addDays(-days);
#ifdef Q_OS_WIN
    const HANDLE h = CreateFileW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(folder).utf16()), FILE_WRITE_ATTRIBUTES,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                 FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    const quint64 ticks = quint64(when.toMSecsSinceEpoch()) * 10000ull + 116444736000000000ull; // 100 ns since 1601
    FILETIME ft;
    ft.dwLowDateTime = DWORD(ticks & 0xFFFFFFFFu);
    ft.dwHighDateTime = DWORD(ticks >> 32);
    const bool ok = SetFileTime(h, nullptr, nullptr, &ft);
    CloseHandle(h);
    return ok;
#else
    const struct utimbuf times{time_t(when.toSecsSinceEpoch()), time_t(when.toSecsSinceEpoch())};
    return ::utime(QFile::encodeName(folder).constData(), &times) == 0;
#endif
}
} // namespace

class TstCoreExtra : public QObject {
    Q_OBJECT
    QTemporaryDir m_root;

private slots:
    void initTestCase() { QVERIFY(m_root.isValid()); fwtest::isolateAppData(m_root); }

    // Deleting a tree (delete source, temp folders, the stale-temp sweep)
    // removes a junction or symlink inside it, never what it points at.
    void deletesNeverFollowLinks()
    {
        const QString outside = m_root.filePath(QStringLiteral("links/outside"));
        QVERIFY(QDir().mkpath(outside + QStringLiteral("/deep")));
        fwtest::writeTextFile(outside + QStringLiteral("/keep.txt"), "precious");
        fwtest::writeTextFile(outside + QStringLiteral("/deep/keep2.txt"), "precious");

        const QString tree = m_root.filePath(QStringLiteral("links/tree"));
        QVERIFY(QDir().mkpath(tree + QStringLiteral("/sub")));
        fwtest::writeTextFile(tree + QStringLiteral("/sub/file.txt"), "mine");
        QVERIFY(createDirectoryJunction(tree + QStringLiteral("/sub/link"), outside).ok());
        QVERIFY(isJunctionOrSymlink(tree + QStringLiteral("/sub/link")));
        QVERIFY(!isJunctionOrSymlink(tree + QStringLiteral("/sub")));

        // A scan does not walk into the link.
        auto scanned = scanTree(tree, CancellationToken());
        QVERIFY(scanned.ok());
        for (const auto& e : scanned->entries)
            QVERIFY2(!e.relativePath.startsWith(QStringLiteral("sub/link")), qPrintable(e.relativePath));
        QCOMPARE(scanned->fileCount, qint64(1));

        QVERIFY(removeTreeSafely(tree).ok());
        QVERIFY(!QFileInfo::exists(tree));
        QVERIFY(QFileInfo::exists(outside + QStringLiteral("/keep.txt")));
        QVERIFY(QFileInfo::exists(outside + QStringLiteral("/deep/keep2.txt")));

        // A job's temporary folder with a link left inside it.
        {
            TempDir t(QStringLiteral("links"), m_root.filePath(QStringLiteral("links")));
            QVERIFY(t.isValid());
            QVERIFY(createDirectoryJunction(t.filePath(QStringLiteral("alias")), outside).ok());
        }
        QVERIFY(QFileInfo::exists(outside + QStringLiteral("/keep.txt")));
        QVERIFY(QFileInfo::exists(outside + QStringLiteral("/deep/keep2.txt")));

        // A link handed to removeTreeSafely directly: only the link goes.
        const QString direct = m_root.filePath(QStringLiteral("links/direct"));
        QVERIFY(createDirectoryJunction(direct, outside).ok());
        QVERIFY(removeTreeSafely(direct).ok());
        QVERIFY(!QFileInfo::exists(direct) || !QFileInfo(direct).isDir());
        QVERIFY(QFileInfo::exists(outside + QStringLiteral("/keep.txt")));
    }

    void jsonRoundTripIsByteIdenticalToPython()
    {
        const QByteArray pretty = readAll(fwtest::dataDir() + QStringLiteral("/json/python_indent2.json"));
        QVERIFY(!pretty.isEmpty());
        auto doc = json::parse(pretty);
        QVERIFY2(doc.ok(), doc.ok() ? "" : qPrintable(doc.error().toLogString()));
        QCOMPARE(json::write(doc.value(), {2, false, false}), pretty);

        const QByteArray compactAscii = readAll(fwtest::dataDir() + QStringLiteral("/json/python_compact_ascii.json"));
        QCOMPARE(json::write(doc.value(), {-1, true, false}), compactAscii);
    }

    void jsonPreservesOrderAndNumbers()
    {
        auto doc = json::parse(R"({"z":1,"a":{"y":2,"b":3},"n":12345678901234567890,"f":1.50})");
        QVERIFY(doc.ok());
        json::Value v = doc.value();
        QCOMPARE(v.keys(), (QStringList{QStringLiteral("z"), QStringLiteral("a"), QStringLiteral("n"), QStringLiteral("f")}));
        QCOMPARE(v.value(QStringLiteral("n")).numberText(), QStringLiteral("12345678901234567890"));
        QCOMPARE(v.value(QStringLiteral("f")).numberText(), QStringLiteral("1.50"));
        QCOMPARE(v.path(QStringLiteral("a.b")).toInt(), qint64(3));
        v[QStringLiteral("a")].insert(QStringLiteral("y"), json::Value(QStringLiteral("changed")));
        v.insert(QStringLiteral("new"), json::Value(true));
        QVERIFY(v.remove(QStringLiteral("z")));
        QCOMPARE(json::write(v, {-1, false, false}),
                 QByteArray(R"({"a": {"y": "changed", "b": 3}, "n": 12345678901234567890, "f": 1.50, "new": true})"));
        QCOMPARE(json::write(json::Value(2.0), {-1}), QByteArray("2.0"));
        // A double is written as Python writes a float, whatever its size.
        QCOMPARE(json::write(json::Value(-0.0), {-1}), QByteArray("-0.0"));
        QCOMPARE(json::write(json::Value(1e16), {-1}), QByteArray("1e+16"));
        QCOMPARE(json::write(json::Value(123456789012345680.0), {-1}), QByteArray("1.2345678901234568e+17"));
        QCOMPARE(json::write(json::Value(-1234.5), {-1}), QByteArray("-1234.5"));
        QCOMPARE(json::write(json::Value(9007199254740992.0), {-1}), QByteArray("9007199254740992.0"));
    }

    void jsonPythonFloatRepr()
    {
        QCOMPARE(json::pythonFloatRepr(1.5), QStringLiteral("1.5"));
        QCOMPARE(json::pythonFloatRepr(100000.0), QStringLiteral("100000.0"));
        QCOMPARE(json::pythonFloatRepr(1e16), QStringLiteral("1e+16"));
        QCOMPARE(json::pythonFloatRepr(1.5e-5), QStringLiteral("1.5e-05"));
        QCOMPARE(json::pythonFloatRepr(0.0001), QStringLiteral("0.0001"));
        QCOMPARE(json::pythonFloatRepr(-2.25), QStringLiteral("-2.25"));
        QCOMPARE(json::pythonFloatRepr(123456789012345680.0), QStringLiteral("1.2345678901234568e+17"));
        QCOMPARE(json::pythonFloatRepr(0.1), QStringLiteral("0.1"));
        auto doc = json::parse(R"({"a":1.50,"b":1e5,"c":7})");
        QVERIFY(doc.ok());
        json::WriteOptions w{-1};
        w.pythonFloats = true;
        QCOMPARE(json::write(doc.value(), w), QByteArray(R"({"a": 1.5, "b": 100000.0, "c": 7})"));
    }

    // A crafted file with one huge object must not stall the thread that
    // reads it (a list search per member is quadratic), and a huge number is
    // no integer.
    void jsonCopesWithHugeObjectsAndNumbers()
    {
        constexpr int kMembers = 60000;
        QByteArray text("{");
        for (int i = 0; i < kMembers; ++i)
            text += "\"k" + QByteArray::number(i) + "\":" + QByteArray::number(i) + ",";
        text += "\"k7\":\"again\",\"big\":1e300}";
        QElapsedTimer timer;
        timer.start();
        auto doc = json::parse(text);
        QVERIFY(doc.ok());
        json::Value v = doc.value();
        QCOMPARE(v.keys().size(), kMembers + 1);
        QCOMPARE(v.keys().at(7), QStringLiteral("k7")); // duplicate key: first position, last value
        QCOMPARE(v.value(QStringLiteral("k7")).toString(), QStringLiteral("again"));
        QCOMPARE(v.value(QStringLiteral("k59999")).toInt(), qint64(59999));
        QVERIFY(v.remove(QStringLiteral("k3")));
        QVERIFY(!v.contains(QStringLiteral("k3")));
        QCOMPARE(v.indexOf(QStringLiteral("k4")), 3);
        QCOMPARE(v.value(QStringLiteral("k59999")).toInt(), qint64(59999));
        v.insert(QStringLiteral("new"), json::Value(true));
        QCOMPARE(v.indexOf(QStringLiteral("new")), kMembers);
        const QByteArray written = json::write(v, {-1, false, false});
        QVERIFY(written.startsWith("{\"k0\": 0, \"k1\": 1, \"k2\": 2, \"k4\": 4, "));
        QVERIFY(written.endsWith("\"big\": 1e300, \"new\": true}"));
        QVERIFY2(timer.elapsed() < 20000, qPrintable(QString::number(timer.elapsed())));
        QCOMPARE(v.value(QStringLiteral("big")).toInt(-1), qint64(-1));
        QCOMPARE(json::Value::numberLiteral(QStringLiteral("-1e300")).toInt(7), qint64(7));
        QCOMPARE(json::Value::numberLiteral(QStringLiteral("1e3")).toInt(), qint64(1000));
    }

    // NTFS compares names one character at a time: "ß" is not "SS" there.
    void nameKeyComparesLikeTheFileSystem()
    {
        if (!hostUsesWindowsNames()) {
            QCOMPARE(hostNameKey(QStringLiteral("Ab")), QStringLiteral("Ab"));
            return;
        }
        QCOMPARE(hostNameKey(QStringLiteral("Ab.dat")), QStringLiteral("AB.DAT"));
        QCOMPARE(hostNameKey(QString::fromUtf8("d\xc3\xa9mo")), hostNameKey(QString::fromUtf8("D\xc3\x89MO")));
        QVERIFY(hostNameKey(QString::fromUtf8("stra\xc3\x9f" "e.dat")) != hostNameKey(QStringLiteral("STRASSE.dat")));
        QCOMPARE(hostNameKey(QString::fromUtf8("stra\xc3\x9f" "e.dat")).size(), 10);
        QCOMPARE(hostNameKey(QString::fromUtf8("\xcf\x83")), hostNameKey(QString::fromUtf8("\xcf\x82"))); // σ and ς
    }

    void jsonRejectsMalformedWithPosition()
    {
        for (const char* bad : {"{", "{\"a\":}", "[1,]", "{\"a\" 1}", "tru", "\"unterminated", "{} x", "01", "1.", "\"\\x\""}) {
            auto r = json::parse(QByteArray(bad));
            QVERIFY2(!r.ok(), bad);
            QCOMPARE(r.error().code, ErrorCode::CorruptData);
            QVERIFY(r.error().message.contains(QStringLiteral("line 1")));
        }
        json::ParseOptions tolerant;
        tolerant.allowTrailingCommas = true;
        tolerant.allowComments = true;
        QVERIFY(json::parse("// c\n{\"a\": [1,2,], /* x */ \"b\": 1,}", tolerant).ok());
        QVERIFY(json::parse("\xEF\xBB\xBF{\"bom\":true}").ok());
        QVERIFY(json::parse(QByteArray(600, '[') + QByteArray(600, ']')).ok() == false); // depth limit
    }

    void junctionPointsAtTargetAndRemovesOnlyTheLink()
    {
        const QString target = m_root.filePath(QStringLiteral("Trò chơi ÄÖ/sub"));
        QVERIFY(fwtest::writeTextFile(target + QStringLiteral("/file.txt"), "content"));
        const QString link = m_root.filePath(QStringLiteral("ascii-link"));
        auto st = createDirectoryJunction(link, target);
        QVERIFY2(st.ok(), st.ok() ? "" : qPrintable(st.error().toLogString()));
        QVERIFY(isLinkOrReparsePoint(link));
        QCOMPARE(readAll(link + QStringLiteral("/file.txt")), QByteArray("content"));
        QVERIFY(!createDirectoryJunction(link, target).ok()); // exists
        {
            JunctionGuard guard(link);
        }
        QVERIFY(!QFileInfo::exists(link) && !isLinkOrReparsePoint(link));
        QCOMPARE(readAll(target + QStringLiteral("/file.txt")), QByteArray("content")); // target untouched
        // A real folder is never removed as if it were a link.
        QVERIFY(!removeDirectoryJunction(target).ok());
        QVERIFY(QFileInfo::exists(target + QStringLiteral("/file.txt")));
        // A target given with ".." is stored cleaned, so the link resolves.
        const QString dotted = m_root.filePath(QStringLiteral("dotted-link"));
        QVERIFY(createDirectoryJunction(dotted, target + QStringLiteral("/../sub")).ok());
        QCOMPARE(readAll(dotted + QStringLiteral("/file.txt")), QByteArray("content"));
        QVERIFY(removeDirectoryJunction(dotted).ok());
    }

    // A scan lists each folder once and never looks below a junction, also
    // one that points back at its own parent; an excluded folder is skipped
    // with everything in it.
    void scanSkipsLinksAndExcludedFolders()
    {
        const QString root = m_root.filePath(QStringLiteral("scan"));
        QVERIFY(fwtest::writeTextFile(root + QStringLiteral("/a/b/file.bin"), "12345"));
        QVERIFY(fwtest::writeTextFile(root + QStringLiteral("/a/cache.tmp/inner/x.bin"), "123"));
        QVERIFY(fwtest::writeTextFile(root + QStringLiteral("/z.txt"), "1"));
        QVERIFY(QDir().mkpath(root + QStringLiteral("/empty")));
        QVERIFY(createDirectoryJunction(root + QStringLiteral("/a/b/loop"), root).ok());
        const auto rels = [](const FileTree& t) {
            QStringList out;
            for (const auto& e : t.entries)
                out << e.relativePath;
            return out;
        };
        auto all = scanTree(root, CancellationToken());
        QVERIFY(all.ok());
        QCOMPARE(rels(all.value()),
                 (QStringList{QStringLiteral("a"), QStringLiteral("a/b"), QStringLiteral("a/b/file.bin"),
                              QStringLiteral("a/cache.tmp"), QStringLiteral("a/cache.tmp/inner"),
                              QStringLiteral("a/cache.tmp/inner/x.bin"), QStringLiteral("empty"), QStringLiteral("z.txt")}));
        QCOMPARE(all->fileCount, qint64(3));
        QCOMPARE(all->dirCount, qint64(5));
        QCOMPARE(all->totalBytes, qint64(9));
        ScanOptions opt;
        opt.excludes = {QStringLiteral("*.tmp")};
        auto some = scanTree(root, CancellationToken(), opt);
        QVERIFY(some.ok());
        QCOMPARE(rels(some.value()), (QStringList{QStringLiteral("a"), QStringLiteral("a/b"), QStringLiteral("a/b/file.bin"),
                                                  QStringLiteral("empty"), QStringLiteral("z.txt")}));
        // With links followed, a link back to a folder already walked ends there.
        const QString outside = m_root.filePath(QStringLiteral("scan-outside"));
        QVERIFY(fwtest::writeTextFile(outside + QStringLiteral("/far.bin"), "12"));
        QVERIFY(createDirectoryJunction(root + QStringLiteral("/empty/out"), outside).ok());
        opt.excludes.clear();
        opt.followSymlinks = true;
        auto followed = scanTree(root, CancellationToken(), opt);
        QVERIFY(followed.ok());
        QVERIFY(rels(followed.value()).contains(QStringLiteral("empty/out/far.bin")));
        QVERIFY(rels(followed.value()).contains(QStringLiteral("a/b/loop")));
        QVERIFY(!rels(followed.value()).contains(QStringLiteral("a/b/loop/z.txt")));
        QCOMPARE(followed->entries.size(), all->entries.size() + 3);
        QVERIFY(removeDirectoryJunction(root + QStringLiteral("/empty/out")).ok());
        QVERIFY(removeDirectoryJunction(root + QStringLiteral("/a/b/loop")).ok());
    }

    void rawOutputAndStartedHook()
    {
        ProcessSpec spec;
        spec.program = fwtest::helperPath();
        spec.arguments = {QStringLiteral("cr")};
        QByteArray raw;
        qint64 startedPid = 0;
        spec.onRawOutput = [&](const QByteArray& chunk, bool) { raw += chunk; };
        spec.onStarted = [&](qint64 pid) { startedPid = pid; };
        auto r = runProcess(spec, CancellationToken());
        QVERIFY(r.ok());
        QVERIFY(startedPid > 0);
        QVERIFY(raw.contains("progress 50%\r"));
        QVERIFY(raw.endsWith("done\n") || raw.endsWith("done\r\n"));
    }

    void outputVolumeChecks()
    {
        auto ok = checkOutputVolume(m_root.path(), 1024);
        QVERIFY(ok.ok());
        const QString cloud = m_root.filePath(QStringLiteral("Users/me/OneDrive/Games"));
        QDir().mkpath(cloud);
        auto warn = checkOutputVolume(cloud, 1024);
        QVERIFY(warn.ok());
        // One warning more than the temporary folder itself gets (which may have
        // some of its own on a removable, network or FAT drive): the cloud one.
        QCOMPARE(warn->size(), ok->size() + 1);
        QCOMPARE(warn->filter(QStringLiteral("cloud")).size(), 1);
        QVERIFY(ok->filter(QStringLiteral("cloud")).isEmpty());
        const VolumeInfo v = volumeInfo(m_root.path());
        QVERIFY(!v.fileSystem.isEmpty());
        QVERIFY(volumeInfo(m_root.filePath(QStringLiteral("not/yet/created"))).rootPath == v.rootPath);
    }

    void tempUsageCountsOwnTemp()
    {
        const qint64 before = Paths::tempUsageBytes();
        {
            TempDir t(QStringLiteral("usage"));
            QVERIFY(fwtest::writeTextFile(t.filePath(QStringLiteral("a.bin")), QByteArray(5000, 'x')));
            const qint64 marker = QFileInfo(t.filePath(TempDir::ownerMarkerName())).size();
            QVERIFY(marker > 0);
            QCOMPARE(Paths::tempUsageBytes(), before + 5000 + marker);
        }
        QCOMPARE(Paths::tempUsageBytes(), before);
    }

    // QFile keeps small writes in memory: write() succeeds and the bytes only
    // reach the drive when the file is flushed or closed. A writer that does
    // not check that step commits an empty or cut-off file as good.
    void finishWrittenFileReportsTheWriteThatFailsLate()
    {
        const QString path = m_root.filePath(QStringLiteral("finish/refused.bin"));
        QVERIFY(fwtest::writeTextFile(path, "x"));
        {
            // A handle that cannot be written to stands in for a full drive.
            QFile readOnly(path);
            QVERIFY(readOnly.open(QIODevice::ReadOnly));
            QFile out;
            QVERIFY(out.open(readOnly.handle(), QIODevice::WriteOnly));
            QCOMPARE(out.write(QByteArray(1000, 'y')), qint64(1000)); // buffered: no error yet
            const Status st = finishWrittenFile(out, QStringLiteral("refused.bin"));
            QVERIFY(!st.ok());
            QCOMPARE(st.error().code, ErrorCode::IoError);
            QVERIFY(st.error().message.contains(QStringLiteral("refused.bin")));
            QVERIFY(!out.isOpen());
        }
        QCOMPARE(readAll(path), QByteArray("x"));

        const QString good = m_root.filePath(QStringLiteral("finish/good.bin"));
        const QDateTime stamp(QDate(2024, 2, 29), QTime(12, 34, 56));
        QFile out(good);
        QVERIFY(out.open(QIODevice::WriteOnly));
        QCOMPARE(out.write(QByteArray(1000, 'y')), qint64(1000));
        QVERIFY(finishWrittenFile(out, QStringLiteral("good.bin"), stamp).ok());
        QVERIFY(!out.isOpen());
        QCOMPARE(readAll(good), QByteArray(1000, 'y'));
        QCOMPARE(QFileInfo(good).lastModified(), stamp);
    }

    // The start-up sweep and "Clear" in a temporary folder the user chose:
    // that folder is the user's, so only a job folder that carries the app's
    // marker is removed, whatever else is called "fw-…".
    void sweepRemovesOnlyFoldersTheAppCreated()
    {
        const QString custom = m_root.filePath(QStringLiteral("user temp"));
        const QString outside = m_root.filePath(QStringLiteral("sweep-outside"));
        QVERIFY(fwtest::writeTextFile(outside + QStringLiteral("/keep.txt"), "precious"));

        // The user's own folders: a firmware folder, and one that even has the
        // name of a job folder but not the marker.
        const QString firmware = custom + QStringLiteral("/fw-11.00");
        const QString lookalike = custom + QStringLiteral("/fw-job-J101010-001-20260101101010-abcdef");
        QVERIFY(fwtest::writeTextFile(firmware + QStringLiteral("/PS5UPDATE.PUP"), "mine"));
        QVERIFY(fwtest::writeTextFile(lookalike + QStringLiteral("/data.bin"), "mine"));
        // A marker alone is not enough either: the name must be a job folder's.
        const QString marked = custom + QStringLiteral("/fw-backups");
        QVERIFY(fwtest::writeTextFile(marked + QStringLiteral("/save.bin"), "mine"));

        QString stale, fresh;
        {
            TempDir t(QStringLiteral("job-J101010-002"), custom);
            QVERIFY(t.isValid());
            t.keep();
            stale = t.path();
            QVERIFY(TempDir::isOwnedFolder(stale));
            QVERIFY(fwtest::writeTextFile(t.filePath(QStringLiteral("zip-source/eboot.bin")), QByteArray(5000, 'x')));
            // What a crashed job can leave behind: a junction to a real folder.
            QVERIFY(createDirectoryJunction(t.filePath(QStringLiteral("alias")), outside).ok());
            QVERIFY(QFile::copy(t.filePath(TempDir::ownerMarkerName()), marked + u'/' + TempDir::ownerMarkerName()));
        }
        {
            TempDir t(QStringLiteral("job-J101010-003"), custom);
            QVERIFY(t.isValid());
            t.keep();
            fresh = t.path();
        }
        QVERIFY(!TempDir::isOwnedFolder(firmware));
        QVERIFY(!TempDir::isOwnedFolder(lookalike));
        const qint64 marker = QFileInfo(fresh + u'/' + TempDir::ownerMarkerName()).size();
        QCOMPARE(Paths::tempUsageBytesIn(custom), 5000 + 2 * marker); // the two job folders, nothing of the user's

        for (const QString& old : {firmware, lookalike, marked, stale})
            QVERIFY2(setFolderAgeDays(old, 3), qPrintable(old));
        QCOMPARE(Paths::sweepStaleTempIn(custom, 24), 1);
        QVERIFY(!QFileInfo::exists(stale));
        QVERIFY(QFileInfo(fresh).isDir());
        QCOMPARE(readAll(firmware + QStringLiteral("/PS5UPDATE.PUP")), QByteArray("mine"));
        QCOMPARE(readAll(lookalike + QStringLiteral("/data.bin")), QByteArray("mine"));
        QCOMPARE(readAll(marked + QStringLiteral("/save.bin")), QByteArray("mine"));
        QCOMPARE(readAll(outside + QStringLiteral("/keep.txt")), QByteArray("precious")); // the junction was not followed

        // A link with a job folder's name that points at a folder with a marker
        // is not a folder the app created.
        const QString linked = custom + QStringLiteral("/fw-job-J101010-004-20260101101010-abcdef");
        QVERIFY(QFile::copy(fresh + u'/' + TempDir::ownerMarkerName(), outside + u'/' + TempDir::ownerMarkerName()));
        QVERIFY(createDirectoryJunction(linked, outside).ok());
        QVERIFY(!TempDir::isOwnedFolder(linked));

        // "Clear" (no age limit) takes the fresh job folder too, and still
        // nothing else.
        QCOMPARE(Paths::sweepStaleTempIn(custom, 0), 1);
        QVERIFY(!QFileInfo::exists(fresh));
        QVERIFY(isJunctionOrSymlink(linked));
        QCOMPARE(readAll(outside + QStringLiteral("/keep.txt")), QByteArray("precious"));
        QCOMPARE(readAll(firmware + QStringLiteral("/PS5UPDATE.PUP")), QByteArray("mine"));
        QCOMPARE(readAll(lookalike + QStringLiteral("/data.bin")), QByteArray("mine"));
        QCOMPARE(Paths::tempUsageBytesIn(custom), qint64(0));
        QVERIFY(removeDirectoryJunction(linked).ok());
    }

    // The app's own temp root holds nothing but its folders: there every old
    // "fw-" folder goes, also one left by a version that wrote no marker. The
    // background variant does both sweeps off the calling thread.
    void sweepOfTheOwnTempRootAndInTheBackground()
    {
        const QString legacy = Paths::tempRoot() + QStringLiteral("/fw-job-J090909-001-20250101090909-0a0b0c");
        const QString recent = Paths::tempRoot() + QStringLiteral("/fw-y2jb-20250101090909-0a0b0c");
        const QString notOurs = Paths::tempRoot() + QStringLiteral("/other");
        QVERIFY(fwtest::writeTextFile(legacy + QStringLiteral("/left.bin"), "old"));
        QVERIFY(fwtest::writeTextFile(recent + QStringLiteral("/left.bin"), "new"));
        QVERIFY(fwtest::writeTextFile(notOurs + QStringLiteral("/left.bin"), "x"));
        QVERIFY(setFolderAgeDays(legacy, 2));
        QVERIFY(setFolderAgeDays(notOurs, 2));
        QCOMPARE(Paths::sweepStaleTemp(24), 1);
        QVERIFY(!QFileInfo::exists(legacy));
        QVERIFY(QFileInfo::exists(recent) && QFileInfo::exists(notOurs));

        const QString custom = m_root.filePath(QStringLiteral("user temp 2"));
        const QString firmware = custom + QStringLiteral("/fw-9.00");
        QVERIFY(fwtest::writeTextFile(firmware + QStringLiteral("/PS5UPDATE.PUP"), "mine"));
        QString stale;
        {
            TempDir t(QStringLiteral("job-J111111-001"), custom);
            QVERIFY(t.isValid());
            t.keep();
            stale = t.path();
        }
        QVERIFY(setFolderAgeDays(firmware, 2));
        QVERIFY(setFolderAgeDays(stale, 2));
        QVERIFY(setFolderAgeDays(recent, 2));
        Paths::sweepStaleTempInBackground({custom}, 24);
        QVERIFY(QThreadPool::globalInstance()->waitForDone(30000));
        QVERIFY(!QFileInfo::exists(stale));
        QVERIFY(!QFileInfo::exists(recent));
        QCOMPARE(readAll(firmware + QStringLiteral("/PS5UPDATE.PUP")), QByteArray("mine"));
        QVERIFY(QDir(notOurs).removeRecursively());
    }

    void crashFilesAreListedNewestFirst()
    {
        const QString dir = m_root.filePath(QStringLiteral("crashes"));
        crash::install(dir);
        QVERIFY(QFileInfo(dir).isDir());
        QVERIFY(crash::previousCrashFiles(dir).isEmpty());
        QVERIFY(fwtest::writeTextFile(dir + QStringLiteral("/crash-20260101-000000.txt"), "old"));
        QVERIFY(fwtest::writeTextFile(dir + QStringLiteral("/other.txt"), "x"));
        QCOMPARE(crash::previousCrashFiles(dir).size(), 1);
        // A later crash comes first (later by its name and by its file time).
        const QString newer = dir + QStringLiteral("/crash-20260302-101500.txt");
        QVERIFY(fwtest::writeTextFile(newer, "new"));
        {
            QFile old(dir + QStringLiteral("/crash-20260101-000000.txt"));
            QVERIFY(old.open(QIODevice::ReadWrite));
            QVERIFY(old.setFileTime(QDateTime::currentDateTime().addDays(-60), QFileDevice::FileModificationTime));
        }
        const QStringList listed = crash::previousCrashFiles(dir);
        QCOMPARE(listed.size(), 2);
        QCOMPARE(QFileInfo(listed.first()).fileName(), QStringLiteral("crash-20260302-101500.txt"));
    }

    void cpuLimitCanBeAppliedWhileRunning()
    {
        ProcessSpec spec;
        spec.program = fwtest::helperPath();
        spec.arguments = {QStringLiteral("sleep"), QStringLiteral("300")};
        bool applied = false, widened = false;
        spec.onStarted = [&](qint64 pid) {
            applied = setProcessCpuLimit(pid, 1);
            widened = setProcessCpuLimit(pid, 0);
        };
        auto r = runProcess(spec, CancellationToken());
        QVERIFY(r.ok());
        QVERIFY(applied);
        QVERIFY(widened);
        QVERIFY(!setProcessCpuLimit(0, 1));
    }
};

QTEST_GUILESS_MAIN(TstCoreExtra)
#include "tst_core_extra.moc"
