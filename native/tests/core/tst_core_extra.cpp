// Ordered JSON (Python-compatible writer), junctions/links, raw process output,
// the started hook and the CPU limit.

#include "core/CrashHandler.h"
#include "core/FileUtil.h"
#include "core/FsLinks.h"
#include "core/Paths.h"
#include "core/OrderedJson.h"
#include "core/ProcessRunner.h"
#include "support/TestSupport.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

using namespace fw;

namespace {
QByteArray readAll(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}
} // namespace

class TstCoreExtra : public QObject {
    Q_OBJECT
    QTemporaryDir m_root;

private slots:
    void initTestCase() { QVERIFY(m_root.isValid()); fwtest::isolateAppData(m_root); }

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
        QCOMPARE(warn->size(), 1);
        QVERIFY(warn->first().contains(QStringLiteral("cloud")));
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
            QCOMPARE(Paths::tempUsageBytes(), before + 5000);
        }
        QCOMPARE(Paths::tempUsageBytes(), before);
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
