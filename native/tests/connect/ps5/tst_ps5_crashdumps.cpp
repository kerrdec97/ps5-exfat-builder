// Crash-dump capture (docs/BACKPORT_PARITY.md matrix #46, CrashDumps.h),
// ported from the owner's PS5 Log Sender and its tests (test_coredump.py):
// old folders are the baseline and are left alone; a new crash folder is
// fetched core first and zipped; the .prosperodmp.tmp is dropped; a folder
// without a core is reported; an unreachable console is one warning. Against
// the in-process fake FTP server on 127.0.0.1, never a real console.

#include "FakePs5.h"
#include "Ps5TestUtil.h"

#include "connect/ps5/CrashDumps.h"
#include "connect/ps5/Module.h"
#include "core/Zip.h"
#include "support/ClosedPort.h"
#include "support/TestSupport.h"

#include <QDir>
#include <QFile>
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <QTest>

using namespace fw;
using namespace fw::ps5;
using fake::FakeFtpServer;

namespace {

const QString kRoot = QStringLiteral("/user/devlog/system/sce_coredumps.0");

ConsoleEndpoint endpoint(quint16 port)
{
    ConsoleEndpoint ep;
    ep.host = QStringLiteral("127.0.0.1");
    ep.ftpPort = port;
    ep.connectTimeoutMs = 2000;
    ep.idleTimeoutMs = 5000;
    return ep;
}

QByteArray randomBytes(int n, quint32 seed)
{
    QRandomGenerator g(seed);
    QByteArray b(n, '\0');
    for (int i = 0; i < n; ++i)
        b[i] = char(g.bounded(256));
    return b;
}

bool writeFile(const QString& path, const QByteArray& data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(data) == data.size();
}

QByteArray readFile(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

CrashCaptureOptions fastOptions(const QString& out)
{
    CrashCaptureOptions o;
    o.outputDir = out;
    o.pollMs = 100;
    o.settleMs = 400;
    o.folderTimeoutMs = 4000;
    o.runForMs = 15000; // a safety net: every test stops itself sooner
    return o;
}

} // namespace

class TstPs5CrashDumps : public QObject {
    Q_OBJECT
    QTemporaryDir m_tmp;
    int m_n = 0;

    QString fresh(const QString& what) { return m_tmp.filePath(QStringLiteral("%1-%2").arg(what).arg(++m_n)); }

private slots:
    void initTestCase()
    {
        QVERIFY(m_tmp.isValid());
        fwtest::isolateAppData(m_tmp);
    }

    void namesAreSafe()
    {
        QCOMPARE(safeCrashFolderName(QStringLiteral("PPSA34008_1792103554")), QStringLiteral("PPSA34008_1792103554"));
        QCOMPARE(safeCrashFolderName(QStringLiteral("a:b*c?. ")), QStringLiteral("a_b_c_"));
        QCOMPARE(safeCrashFolderName(QStringLiteral("..")), QStringLiteral("crash"));
        QCOMPARE(crashIpTag(QStringLiteral("192.168.1.20")), QStringLiteral("192-168-1-20"));
        QCOMPARE(crashIpTag(QString()), QStringLiteral("console"));
    }

    void newCrashIsSavedCoreFirstAndZipped()
    {
        const QString root = fresh(QStringLiteral("console"));
        QVERIFY(writeFile(root + kRoot + "/PPSA00001_1000/report.log", "old crash"));
        FakeFtpServer::Options so;
        so.root = root;
        FakeFtpServer srv(so);
        const QString out = fresh(QStringLiteral("out"));
        const QByteArray core = randomBytes(96 * 1024, 7);
        const QString crash = root + kRoot + "/PPSA34008_1792103554";
        CancellationSource cancel;
        QStringList seen;
        bool crashWritten = true;
        auto r = captureCrashDumps(endpoint(srv.port()), fastOptions(out), cancel.token(), [&](const QString& text) {
            seen << text;
            if (text.contains(QStringLiteral("older crash folder"))) { // baseline taken: now the game crashes
                crashWritten = writeFile(crash + "/report.log", "report") && writeFile(crash + "/core.prosperodmp", core)
                    && writeFile(crash + "/core.prosperodmp.tmp", "partial core");
                if (!crashWritten)
                    cancel.cancel(); // no crash will appear: do not wait for one
            }
            if (text.startsWith(QStringLiteral("Crash report ready")))
                cancel.cancel();
        });
        QVERIFY(crashWritten);
        QVERIFY2(r.ok(), qPrintable(r.ok() ? QString() : r.error().message));
        QCOMPARE(r->baseline, QStringList{QStringLiteral("PPSA00001_1000")});
        QCOMPARE(int(r->crashes.size()), 1);
        const CapturedCrash& c = r->crashes.front();
        QCOMPARE(c.folder, QStringLiteral("PPSA34008_1792103554"));
        QVERIFY(c.hasCore);
        QCOMPARE(c.files, QStringList({QStringLiteral("core.prosperodmp"), QStringLiteral("report.log")})); // .tmp dropped
        QCOMPARE(readFile(c.localDir + "/core.prosperodmp"), core);
        QVERIFY(!QFileInfo::exists(c.localDir + "/core.prosperodmp.tmp"));
        QVERIFY(!QFileInfo::exists(out + "/crash_dumps/PPSA00001_1000")); // the old crash is left alone
        QVERIFY(r->stoppedByUser);
        // The ZIP: named for the console and the folder, holding the folder.
        const QString zipName = QFileInfo(c.zipPath).fileName();
        QVERIFY2(zipName.startsWith(QStringLiteral("PS5_logs_127-0-0-1_")), qPrintable(zipName));
        QVERIFY2(zipName.endsWith(QStringLiteral("_crash_PPSA34008_1792103554.zip")), qPrintable(zipName));
        zip::Reader z;
        QVERIFY(z.open(c.zipPath).ok());
        QVERIFY(z.find(QStringLiteral("capture.txt")));
        // The note names the folder that was really watched.
        const auto noteBytes = z.readAll(*z.find(QStringLiteral("capture.txt")));
        QVERIFY(noteBytes.ok());
        const QByteArray note = noteBytes.value();
        QVERIFY2(note.contains("Console folder: " + kRoot.toUtf8() + "/PPSA34008_1792103554\n"), note.constData());
        QVERIFY(z.find(QStringLiteral("crash_dumps/PPSA34008_1792103554/report.log")));
        QVERIFY(z.find(QStringLiteral("crash_dumps/PPSA34008_1792103554/core.prosperodmp")));
        COMPARE_VALUE(z.readAll(*z.find(QStringLiteral("crash_dumps/PPSA34008_1792103554/core.prosperodmp"))), core);
        QVERIFY(!z.find(QStringLiteral("crash_dumps/PPSA34008_1792103554/core.prosperodmp.tmp")));
        // The core was fetched before the report (core first).
        const QStringList cmds = srv.commands();
        qsizetype coreAt = -1, reportAt = -1;
        for (qsizetype i = 0; i < cmds.size(); ++i) {
            if (coreAt < 0 && cmds[i].startsWith(QStringLiteral("RETR")) && cmds[i].contains(QStringLiteral("core.prosperodmp")))
                coreAt = i;
            if (reportAt < 0 && cmds[i].startsWith(QStringLiteral("RETR")) && cmds[i].contains(QStringLiteral("report.log")))
                reportAt = i;
        }
        QVERIFY(coreAt >= 0 && reportAt > coreAt);
        QVERIFY(seen.join(u'\n').contains(QStringLiteral("Crash dump secured: core.prosperodmp")));
    }

    void folderWithoutACoreIsReported()
    {
        const QString root = fresh(QStringLiteral("console"));
        QVERIFY(QDir().mkpath(root + kRoot));
        FakeFtpServer::Options so;
        so.root = root;
        FakeFtpServer srv(so);
        const QString out = fresh(QStringLiteral("out"));
        CancellationSource cancel;
        auto opt = fastOptions(out);
        opt.folderTimeoutMs = 1200; // the console never writes the core
        bool crashWritten = true;
        auto r = captureCrashDumps(endpoint(srv.port()), opt, cancel.token(), [&](const QString& text) {
            if (text.startsWith(QStringLiteral("Watching for new crashes"))) {
                crashWritten = writeFile(root + kRoot + "/PPSA22222_5/report.log", "report only");
                if (!crashWritten)
                    cancel.cancel(); // no crash will appear: do not wait for one
            }
            if (text.startsWith(QStringLiteral("Crash report ready")))
                cancel.cancel();
        });
        QVERIFY(crashWritten);
        QVERIFY(r.ok());
        QCOMPARE(int(r->crashes.size()), 1);
        QVERIFY(!r->crashes.front().hasCore);
        QVERIFY(r->crashes.front().warning.contains(QStringLiteral("never wrote a .prosperodmp core")));
        QVERIFY(QFileInfo::exists(r->crashes.front().zipPath)); // still reported, with what was there
    }

    void unreachableConsoleIsOneWarning()
    {
        const QString out = fresh(QStringLiteral("out"));
        auto opt = fastOptions(out);
        opt.runForMs = 2500;
        CancellationSource cancel;
        const fwtest::ClosedPort closed;
        QVERIFY(closed.isValid());
        auto r = captureCrashDumps(endpoint(closed.port()), opt, cancel.token());
        QVERIFY(r.ok());
        QCOMPARE(r->warnings.size(), 1);
        QVERIFY(r->warnings.front().contains(QStringLiteral("waiting for the console FTP service")));
        QVERIFY(r->crashes.empty());
        QVERIFY(!r->stoppedByUser);
    }

    void refusesWithoutHostOrFolder()
    {
        CancellationSource cancel;
        ConsoleEndpoint none;
        COMPARE_ERROR_CODE(captureCrashDumps(none, fastOptions(fresh(QStringLiteral("o"))), cancel.token()), ErrorCode::InvalidArgument);
        // Refused before anything is contacted or written (the port is never used).
        const fwtest::ClosedPort closed;
        QVERIFY(closed.isValid());
        CrashCaptureOptions noOut;
        COMPARE_ERROR_CODE(captureCrashDumps(endpoint(closed.port()), noOut, cancel.token()), ErrorCode::InvalidArgument);
    }

    void jobIsRegistered()
    {
        auto schema = std::make_shared<SettingsSchema>();
        DependencyRegistry deps;
        JobFactory jobs;
        ModuleContext ctx{*schema, deps, jobs};
        registerModule(ctx);
        QVERIFY(jobs.contains(jobkinds::kCrashDumps));
        auto job = jobs.create(jobkinds::kCrashDumps, QJsonObject{{QStringLiteral("host"), QStringLiteral("10.0.0.2")}});
        QVERIFY(job.ok());
        QCOMPARE(job.value()->phases(), QStringList{QStringLiteral("Watching")});
        QVERIFY(job.value()->subtitle().endsWith(QStringLiteral("PS5 Logs")));
    }
};

QTEST_GUILESS_MAIN(TstPs5CrashDumps)
#include "tst_ps5_crashdumps.moc"
