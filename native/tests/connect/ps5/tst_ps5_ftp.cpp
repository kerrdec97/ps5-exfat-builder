// FtpClient, listing parsers, remote path rules and the parallel transfer
// engine, against the in-process FakeFtpServer (never a real console).

#include "FakePs5.h"

#include "connect/ps5/Endpoint.h"
#include "connect/ps5/FtpClient.h"
#include "connect/ps5/FtpListing.h"
#include "connect/ps5/FtpTransfer.h"
#include "connect/ps5/Module.h"
#include "connect/ps5/RemotePath.h"
#include "core/FileUtil.h"
#include "support/TestSupport.h"

#include <QBuffer>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QTest>
#include <QTimeZone>

#include <memory>


using namespace fw;
using namespace fw::ps5;
using fake::FakeFtpServer;

namespace {

QByteArray readAll(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

FtpOptions quick()
{
    FtpOptions o;
    o.connectTimeoutMs = 3000;
    o.replyTimeoutMs = 3000;
    o.idleTimeoutMs = 3000;
    return o;
}

ConsoleEndpoint endpoint(quint16 port)
{
    ConsoleEndpoint ep;
    ep.host = QStringLiteral("127.0.0.1");
    ep.ftpPort = port;
    ep.connectTimeoutMs = 3000;
    ep.idleTimeoutMs = 3000;
    return ep;
}

struct Module {
    std::shared_ptr<SettingsSchema> schema = std::make_shared<SettingsSchema>();
    DependencyRegistry deps;
    JobFactory jobs;
    Module()
    {
        ModuleContext ctx{*schema, deps, jobs};
        registerModule(ctx);
    }
};

} // namespace

class TstPs5Ftp : public QObject {
    Q_OBJECT
    QTemporaryDir m_tmp;
    int m_n = 0;

    QString fresh(const QString& name)
    {
        const QString p = m_tmp.filePath(QStringLiteral("%1-%2").arg(++m_n).arg(name));
        QDir().mkpath(p);
        return p;
    }

    FakeFtpServer::Options server(const QString& root)
    {
        FakeFtpServer::Options o;
        o.root = root;
        return o;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_tmp.isValid());
        fwtest::isolateAppData(m_tmp);
    }

    // ---- pure parsers and rules ---------------------------------------------------------
    void listParsersHandleEveryFormat()
    {
        const QDateTime now(QDate(2026, 11, 4), QTime(12, 0), QTimeZone::utc());
        auto a = parseListLine(QStringLiteral("drwxr-xr-x 1 root wheel 4096 Jan 01 12:00 My Games"), now);
        QVERIFY(a);
        QVERIFY(a->isDir);
        QCOMPARE(a->name, QStringLiteral("My Games"));
        QCOMPARE(a->modified.date(), QDate(2026, 1, 1));
        auto b = parseListLine(QStringLiteral("-rw-r--r-- 1 ps5 1234 Dec 31  2024 file  with  spaces.bin"), now);
        QVERIFY(b);
        QCOMPARE(b->size, qint64(1234));
        QCOMPARE(b->name, QStringLiteral("file  with  spaces.bin"));
        QCOMPARE(b->modified.date(), QDate(2024, 12, 31));
        auto future = parseListLine(QStringLiteral("-rw-r--r-- 1 root root 5 Dec 25 10:00 x"), now);
        QCOMPARE(future->modified.date().year(), 2025); // a date in the future is last year
        auto link = parseListLine(QStringLiteral("lrwxrwxrwx 1 root root 7 Jan 1 00:00 usb -> /mnt/usb0"), now);
        QVERIFY(link->isLink);
        QCOMPARE(link->name, QStringLiteral("usb"));
        QCOMPARE(link->linkTarget, QStringLiteral("/mnt/usb0"));
        auto dos = parseListLine(QStringLiteral("01-02-24  03:04PM       <DIR>          Folder X"), now);
        QVERIFY(dos && dos->isDir);
        QCOMPARE(dos->name, QStringLiteral("Folder X"));
        QCOMPARE(dos->modified, QDateTime(QDate(2024, 1, 2), QTime(15, 4), QTimeZone::utc()));
        auto dosFile = parseListLine(QStringLiteral("12-31-99  11:59AM              42 a.txt"), now);
        QCOMPARE(dosFile->size, qint64(42));
        QVERIFY(!parseListLine(QStringLiteral("total 12"), now));
        QVERIFY(!parseListLine(QStringLiteral("drwxr-xr-x 1 root root 4096 Jan 01 12:00 ."), now));
        QVERIFY(!parseListLine(QStringLiteral("garbage"), now));

        auto m = parseMlsdLine(QStringLiteral("type=file;size=99;modify=20240102030405; name with ;semi"));
        QVERIFY(m);
        QCOMPARE(m->name, QStringLiteral("name with ;semi"));
        QCOMPARE(m->size, qint64(99));
        QCOMPARE(m->modified, QDateTime(QDate(2024, 1, 2), QTime(3, 4, 5), QTimeZone::utc()));
        QVERIFY(!parseMlsdLine(QStringLiteral("type=cdir; .")));
        QVERIFY(!parseMlsdLine(QStringLiteral("type=pdir; ..")));
        QVERIFY(parseMlsdLine(QStringLiteral("Type=DIR;Modify=20240101000000; d"))->isDir);
        const auto all = parseListing("type=dir; a\r\ntype=file;size=1; b\r\n", true);
        QCOMPARE(int(all.size()), 2);
    }

    void remotePathRules()
    {
        QCOMPARE(normalizeRemotePath(QStringLiteral("data//etaHEN/games/")), QStringLiteral("/data/etaHEN/games"));
        QCOMPARE(normalizeRemotePath(QString()), QStringLiteral("/"));
        QCOMPARE(joinRemote(QStringLiteral("/data/"), QStringLiteral("x y")), QStringLiteral("/data/x y"));
        QCOMPARE(remoteParent(QStringLiteral("/a")), QStringLiteral("/"));
        QCOMPARE(remoteFileName(QStringLiteral("/a/b.bin")), QStringLiteral("b.bin"));
        for (const char* p : {"/system", "/SYSTEM/lib", "/dev/", "system_ex/x", "/preinst2", "/data/../system", "//proc"})
            QVERIFY2(isProtectedRemotePath(QString::fromLatin1(p)), p);
        for (const char* p : {"/system_data/priv", "/data/system", "/mnt/usb0", "/systemx", "", "/"})
            QVERIFY2(!isProtectedRemotePath(QString::fromLatin1(p)), p);
        QVERIFY(protectedPathWarning(QStringLiteral("/system"), QStringLiteral("delete")).contains(QStringLiteral("BRICK")));
        QCOMPARE(classifyDestination(QStringLiteral("/mnt/usb0/games")).kind, DestinationKind::External);
        QCOMPARE(classifyDestination(QStringLiteral("/data/etaHEN/games/")).kind, DestinationKind::Internal);
        QCOMPARE(classifyDestination(QStringLiteral("/foo")).kind, DestinationKind::Unknown);
    }

    // ---- client against the fake server ----------------------------------------------------
    void loginAnonymousAndWithCredentials()
    {
        auto o = server(fresh(QStringLiteral("login")));
        o.requireLogin = true;
        FakeFtpServer srv(o);
        FtpClient anon(quick());
        auto st = anon.open(QStringLiteral("127.0.0.1"), srv.port());
        QVERIFY(!st.ok());
        QCOMPARE(st.error().code, ErrorCode::PermissionDenied);
        QVERIFY(srv.sawCommand(QStringLiteral("USER anonymous")));
        FtpOptions creds = quick();
        creds.user = QStringLiteral("ps5");
        creds.password = QStringLiteral("secret");
        FtpClient ok(creds);
        QVERIFY(ok.open(QStringLiteral("127.0.0.1"), srv.port()).ok());
        QVERIFY(ok.welcome().contains(QStringLiteral("FakePS5")));
        ok.quit();
        QVERIFY(!ok.isConnected());

        FtpClient refused(quick());
        auto r = refused.open(QStringLiteral("127.0.0.1"), 1);
        QVERIFY(!r.ok());
        // Refused on Linux; on Windows a short timeout may report it as no answer.
        QVERIFY2(r.error().message.contains(QStringLiteral("refused")), qPrintable(r.error().message));
        QVERIFY(r.error().message.contains(QStringLiteral("127.0.0.1:1")));
        QVERIFY(r.error().message.contains(QStringLiteral("FTP server")));
    }

    void listingsInEveryServerFormat_data()
    {
        QTest::addColumn<int>("style");
        QTest::addColumn<bool>("mlsd");
        QTest::newRow("unix") << int(FakeFtpServer::ListStyle::Unix) << false;
        QTest::newRow("unix-no-group") << int(FakeFtpServer::ListStyle::UnixNoGroup) << false;
        QTest::newRow("dos") << int(FakeFtpServer::ListStyle::Dos) << false;
        QTest::newRow("mlsd") << int(FakeFtpServer::ListStyle::Unix) << true;
    }
    void listingsInEveryServerFormat()
    {
        QFETCH(int, style);
        QFETCH(bool, mlsd);
        const QString root = fresh(QStringLiteral("list"));
        QVERIFY(fwtest::writeRandomFile(QDir(root).filePath(QStringLiteral("games/A Game (01.000.000).exfat")), 1234, 1));
        QVERIFY(fwtest::writeRandomFile(QDir(root).filePath(QStringLiteral("games/b.bin")), 7, 2));
        QVERIFY(QDir().mkpath(QDir(root).filePath(QStringLiteral("games/Sub Folder"))));
        auto o = server(root);
        o.listStyle = FakeFtpServer::ListStyle(style);
        o.mlsd = mlsd;
        FakeFtpServer srv(o);
        FtpClient c(quick());
        QVERIFY(c.open(QStringLiteral("127.0.0.1"), srv.port()).ok());
        auto entries = c.list(QStringLiteral("/games"));
        QVERIFY2(entries.ok(), entries.ok() ? "" : qPrintable(entries.error().toLogString()));
        QCOMPARE(int(entries->size()), 3);
        std::map<QString, FtpEntry> by;
        for (const auto& e : entries.value())
            by[e.name] = e;
        QVERIFY(by.count(QStringLiteral("A Game (01.000.000).exfat")));
        QCOMPARE(by[QStringLiteral("A Game (01.000.000).exfat")].size, qint64(1234));
        QVERIFY(!by[QStringLiteral("b.bin")].isDir);
        QVERIFY(by[QStringLiteral("Sub Folder")].isDir);
        QCOMPARE(srv.sawCommand(QStringLiteral("MLSD")), mlsd);
        QCOMPARE(c.features()->mlsd, mlsd);
        // A missing folder is an error, not an empty list.
        QVERIFY(!c.list(QStringLiteral("/nope")).ok());
        QVERIFY(c.isConnected());
    }

    void commandsWorkAndReportErrors()
    {
        const QString root = fresh(QStringLiteral("cmds"));
        QVERIFY(fwtest::writeTextFile(QDir(root).filePath(QStringLiteral("data/f.txt")), "hello"));
        FakeFtpServer srv(server(root));
        FtpClient c(quick());
        QVERIFY(c.open(QStringLiteral("127.0.0.1"), srv.port()).ok());
        QCOMPARE(c.pwd().value(), QStringLiteral("/"));
        QVERIFY(c.cwd(QStringLiteral("/data")).ok());
        QCOMPARE(c.pwd().value(), QStringLiteral("/data"));
        QCOMPARE(c.cwd(QStringLiteral("/missing")).error().code, ErrorCode::NotFound);
        QCOMPARE(c.size(QStringLiteral("/data/f.txt")).value(), qint64(5));
        QVERIFY(c.mdtm(QStringLiteral("/data/f.txt")).ok());
        QVERIFY(c.mkdirs(QStringLiteral("/data/a/b/c")).ok());
        QVERIFY(QFileInfo(QDir(root).filePath(QStringLiteral("data/a/b/c"))).isDir());
        QVERIFY(c.mkdirs(QStringLiteral("/data/a/b/c")).ok()); // existing is fine
        QVERIFY(c.isDirectory(QStringLiteral("/data/a")));
        QVERIFY(!c.isDirectory(QStringLiteral("/data/f.txt")));
        QVERIFY(c.fileExists(QStringLiteral("/data/f.txt")));
        QVERIFY(!c.fileExists(QStringLiteral("/data/none.txt")));
        QVERIFY(c.rename(QStringLiteral("/data/f.txt"), QStringLiteral("/data/g.txt")).ok());
        QVERIFY(QFileInfo(QDir(root).filePath(QStringLiteral("data/g.txt"))).isFile());
        QVERIFY(!c.rename(QStringLiteral("/data/none"), QStringLiteral("/data/x")).ok());
        QVERIFY(c.noop().ok());
        QCOMPARE(c.siteAvail().value(), QStringLiteral("123456789"));
        QVERIFY(c.nameList(QStringLiteral("/data")).value().contains(QStringLiteral("g.txt")));
        QVERIFY(c.upload(QStringLiteral("/data/a/b/c/x.bin"), "xyz").ok());
        QVERIFY(c.removeTree(QStringLiteral("/data/a")).ok());
        QVERIFY(!QFileInfo::exists(QDir(root).filePath(QStringLiteral("data/a"))));
        QVERIFY(c.dele(QStringLiteral("/data/g.txt")).ok());
        QCOMPARE(c.dele(QStringLiteral("/data/g.txt")).error().code, ErrorCode::NotFound);
        QVERIFY(!c.removeTree(QStringLiteral("/")).ok());
        QVERIFY(c.rmd(QStringLiteral("/data")).ok());
    }

    void uploadAndDownloadAreByteExact_data()
    {
        QTest::addColumn<bool>("passive");
        QTest::addColumn<bool>("refusePasv");
        QTest::newRow("pasv") << true << false;
        QTest::newRow("epsv-fallback") << true << true;
        QTest::newRow("active") << false << false;
    }
    void uploadAndDownloadAreByteExact()
    {
        QFETCH(bool, passive);
        QFETCH(bool, refusePasv);
        const QString root = fresh(QStringLiteral("xfer"));
        const QString local = fresh(QStringLiteral("local"));
        const QString src = QDir(local).filePath(QStringLiteral("game.exfat"));
        QVERIFY(fwtest::writeRandomFile(src, 3 * 1024 * 1024 + 17, 42));
        auto o = server(root);
        o.refusePasv = refusePasv;
        FakeFtpServer srv(o);
        FtpOptions opt = quick();
        opt.passive = passive;
        FtpClient c(opt);
        QVERIFY(c.open(QStringLiteral("127.0.0.1"), srv.port()).ok());
        QFile in(src);
        QVERIFY(in.open(QIODevice::ReadOnly));
        qint64 progressed = 0;
        auto st = c.store(QStringLiteral("/game.exfat"), in, 0, [&](qint64 d) { progressed += d; });
        QVERIFY2(st.ok(), st.ok() ? "" : qPrintable(st.error().toLogString()));
        QCOMPARE(progressed, in.size());
        QCOMPARE(readAll(QDir(root).filePath(QStringLiteral("game.exfat"))), readAll(src));

        QByteArray got;
        QBuffer buf(&got);
        buf.open(QIODevice::WriteOnly);
        qint64 down = 0;
        QVERIFY(c.retrieve(QStringLiteral("/game.exfat"), buf, 0, [&](qint64 d) { down += d; }).ok());
        QCOMPARE(got, readAll(src));
        QCOMPARE(down, qint64(got.size()));
        // REST: the second half only.
        QByteArray tail;
        QBuffer tb(&tail);
        tb.open(QIODevice::WriteOnly);
        QVERIFY(c.retrieve(QStringLiteral("/game.exfat"), tb, 1000000).ok());
        QCOMPARE(tail, readAll(src).mid(1000000));
        QCOMPARE(srv.sawCommand(QStringLiteral("EPSV")), refusePasv);
        QCOMPARE(srv.sawCommand(QStringLiteral("PORT")), !passive);
        // A missing file is NotFound and the connection survives.
        QByteArray none;
        QBuffer nb(&none);
        nb.open(QIODevice::WriteOnly);
        QCOMPARE(c.retrieve(QStringLiteral("/none"), nb).error().code, ErrorCode::NotFound);
        QVERIFY(!c.lastTransferStarted());
        QVERIFY(c.noop().ok());
    }

    void uploadReplaceSwapsSafely()
    {
        const QString root = fresh(QStringLiteral("swap"));
        QVERIFY(fwtest::writeTextFile(QDir(root).filePath(QStringLiteral("cfg/config.ini")), "old"));
        FakeFtpServer srv(server(root));
        FtpClient c(quick());
        QVERIFY(c.open(QStringLiteral("127.0.0.1"), srv.port()).ok());
        QVERIFY(c.uploadReplace(QStringLiteral("/cfg/config.ini"), "new contents", QStringLiteral(".new")).ok());
        QCOMPARE(readAll(QDir(root).filePath(QStringLiteral("cfg/config.ini"))), QByteArray("new contents"));
        QCOMPARE(QDir(QDir(root).filePath(QStringLiteral("cfg"))).entryList(QDir::Files), QStringList{QStringLiteral("config.ini")});

        auto o = server(fresh(QStringLiteral("swap2")));
        o.renameRefused = true;
        QVERIFY(fwtest::writeTextFile(QDir(o.root).filePath(QStringLiteral("config.ini")), "keep me"));
        FakeFtpServer srv2(o);
        FtpClient c2(quick());
        QVERIFY(c2.open(QStringLiteral("127.0.0.1"), srv2.port()).ok());
        auto st = c2.uploadReplace(QStringLiteral("/config.ini"), "x", QStringLiteral(".new"));
        QCOMPARE(st.error().code, ErrorCode::Unsupported);
        QCOMPARE(readAll(QDir(o.root).filePath(QStringLiteral("config.ini"))), QByteArray("keep me"));
        QVERIFY(!QFileInfo::exists(QDir(o.root).filePath(QStringLiteral("config.ini.new"))));
    }

    void timeoutsAreReported()
    {
        // A server that accepts but never greets.
        auto o = server(fresh(QStringLiteral("silent")));
        o.silentGreeting = true;
        FakeFtpServer silent(o);
        FtpOptions opt = quick();
        opt.replyTimeoutMs = 400;
        FtpClient c(opt);
        QElapsedTimer t;
        t.start();
        auto st = c.connectToHost(QStringLiteral("127.0.0.1"), silent.port());
        QCOMPARE(st.error().code, ErrorCode::Timeout);
        QVERIFY(t.elapsed() < 3000);

        // A transfer that stops moving fails after the idle timeout.
        auto s2 = server(fresh(QStringLiteral("stall")));
        s2.stallAfterBytes = 100 * 1024;
        FakeFtpServer stall(s2);
        FtpOptions o2 = quick();
        o2.idleTimeoutMs = 700;
        FtpClient c2(o2);
        QVERIFY(c2.open(QStringLiteral("127.0.0.1"), stall.port()).ok());
        QByteArray big(16 * 1024 * 1024, 'x');
        QBuffer b(&big);
        b.open(QIODevice::ReadOnly);
        t.restart();
        auto up = c2.store(QStringLiteral("/big.bin"), b);
        QCOMPARE(up.error().code, ErrorCode::Timeout);
        QVERIFY(up.error().message.contains(QStringLiteral("stalled")));
        QVERIFY(!c2.isConnected());
        QVERIFY(t.elapsed() < 10000);
    }

    // ---- transfer engine ----------------------------------------------------------------------
    void parallelFolderUploadAndDownload()
    {
        const QString root = fresh(QStringLiteral("par"));
        const QString local = fresh(QStringLiteral("parlocal"));
        const QString game = QDir(local).filePath(QString::fromUtf8("Café Game"));
        for (int i = 0; i < 24; ++i)
            QVERIFY(fwtest::writeRandomFile(QDir(game).filePath(QStringLiteral("dir%1/file %2.bin").arg(i % 4).arg(i)),
                                            20000 + i * 7919, quint32(i)));
        QVERIFY(QDir().mkpath(QDir(game).filePath(QStringLiteral("empty"))));
        const QString single = QDir(local).filePath(QStringLiteral("eboot.bin"));
        QVERIFY(fwtest::writeRandomFile(single, 333333, 99));
        auto so = server(root);
        so.throttleBytesPerSec = 4 * 1024 * 1024; // slow enough that transfers overlap
        FakeFtpServer srv(so);

        auto plan = planUpload({game, single}, QStringLiteral("/data/etaHEN/games/"));
        QVERIFY(plan.ok());
        QCOMPARE(int(plan->files.size()), 25);
        QVERIFY(plan->directories.contains(QString::fromUtf8("/data/etaHEN/games/Café Game/empty")));
        TransferOptions opt;
        opt.endpoint = endpoint(srv.port());
        opt.parallel = 4;
        ProgressReporter progress;
        progress.begin(QStringLiteral("Uploading"), plan->totalBytes, 25);
        auto rep = runUpload(plan.value(), opt, {}, &progress);
        QVERIFY2(rep.ok(), rep.ok() ? "" : qPrintable(rep.error().toLogString()));
        QCOMPARE(rep->transferred, 25);
        QCOMPARE(rep->failed, 0);
        QCOMPARE(rep->verified, 25);
        QCOMPARE(progress.snapshot().done, plan->totalBytes);
        QVERIFY2(srv.maxConcurrentTransfers() >= 2, qPrintable(QString::number(srv.maxConcurrentTransfers())));
        for (const auto& f : plan->files)
            QVERIFY2(readAll(srv.path(f.remote)) == readAll(f.local), qPrintable(f.remote));
        QVERIFY(QFileInfo(srv.path(QString::fromUtf8("/data/etaHEN/games/Café Game/empty"))).isDir());

        // And back down, in parallel, into a fresh folder.
        const QString back = fresh(QStringLiteral("back"));
        FtpClient c(quick());
        QVERIFY(c.open(QStringLiteral("127.0.0.1"), srv.port()).ok());
        auto dplan = planDownload(c, {QString::fromUtf8("/data/etaHEN/games/Café Game"), QStringLiteral("/data/etaHEN/games/eboot.bin")}, back);
        QVERIFY(dplan.ok());
        QCOMPARE(int(dplan->files.size()), 25);
        auto drep = runDownload(dplan.value(), opt, {});
        QVERIFY(drep.ok());
        QCOMPARE(drep->transferred, 25);
        for (int i = 0; i < 24; ++i) {
            const QString rel = QStringLiteral("dir%1/file %2.bin").arg(i % 4).arg(i);
            QCOMPARE(readAll(QDir(back).filePath(QString::fromUtf8("Café Game/") + rel)), readAll(QDir(game).filePath(rel)));
        }
        QCOMPARE(readAll(QDir(back).filePath(QStringLiteral("eboot.bin"))), readAll(single));
        // Existing local files are skipped unless overwrite is set.
        auto again = runDownload(dplan.value(), opt, {});
        QCOMPARE(again->skipped, 25);
        QCOMPARE(again->transferred, 0);
        QVERIFY(QDir(back).entryList({QStringLiteral("*.part")}, QDir::Files).isEmpty());
    }

    void droppedTransfersResumeWithRest()
    {
        const QString root = fresh(QStringLiteral("drop"));
        const QString local = fresh(QStringLiteral("droplocal"));
        const QString src = QDir(local).filePath(QStringLiteral("big.bin"));
        QVERIFY(fwtest::writeRandomFile(src, 2 * 1024 * 1024 + 5, 7));
        auto o = server(root);
        o.dropAfterBytes = 700 * 1024;
        FakeFtpServer srv(o);
        TransferOptions opt;
        opt.endpoint = endpoint(srv.port());
        opt.retries = 2;
        auto plan = planUpload({src}, QStringLiteral("/up"));
        auto rep = runUpload(plan.value(), opt, {});
        QVERIFY(rep.ok());
        QCOMPARE(rep->transferred, 1);
        QCOMPARE(readAll(srv.path(QStringLiteral("/up/big.bin"))), readAll(src));
        QVERIFY(srv.sawCommand(QStringLiteral("REST 716800")));

        // Download side: the first RETR is cut, the retry resumes from the .part.
        srv.setDropAfter(500 * 1024);
        const QString down = fresh(QStringLiteral("dropdown"));
        FtpClient c(quick());
        QVERIFY(c.open(QStringLiteral("127.0.0.1"), srv.port()).ok());
        auto dplan = planDownload(c, {QStringLiteral("/up/big.bin")}, down);
        auto drep = runDownload(dplan.value(), opt, {});
        QVERIFY(drep.ok());
        QCOMPARE(drep->transferred, 1);
        QCOMPARE(readAll(QDir(down).filePath(QStringLiteral("big.bin"))), readAll(src));
        int rests = 0;
        for (const QString& cmd : srv.commands())
            rests += cmd.startsWith(QStringLiteral("REST ")) ? 1 : 0;
        QCOMPARE(rests, 2); // one resume per direction
        QVERIFY(srv.sawCommand(QStringLiteral("REST 512000"))); // the fake closes after exactly 500 KiB
    }

    void resumeContinuesAnEarlierPartialUpload()
    {
        const QString root = fresh(QStringLiteral("resume"));
        const QString local = fresh(QStringLiteral("resumelocal"));
        const QString src = QDir(local).filePath(QStringLiteral("game.ffpkg"));
        QVERIFY(fwtest::writeRandomFile(src, 1000000, 3));
        const QByteArray all = readAll(src);
        QVERIFY(fwtest::writeTextFile(QDir(root).filePath(QStringLiteral("g/game.ffpkg")), all.left(400000)));
        QVERIFY(fwtest::writeTextFile(QDir(root).filePath(QStringLiteral("g/done.bin")), all.left(10)));
        const QString done = QDir(local).filePath(QStringLiteral("done.bin"));
        QVERIFY(fwtest::writeTextFile(done, all.left(10)));
        FakeFtpServer srv(server(root));
        TransferOptions opt;
        opt.endpoint = endpoint(srv.port());
        opt.resume = true;
        auto rep = runUpload(planUpload({src, done}, QStringLiteral("/g")).value(), opt, {});
        QVERIFY(rep.ok());
        QCOMPARE(rep->transferred, 1);
        QCOMPARE(rep->skipped, 1);      // already complete
        QCOMPARE(rep->bytes, qint64(600000)); // only the rest was sent
        QCOMPARE(readAll(srv.path(QStringLiteral("/g/game.ffpkg"))), all);
        QCOMPARE(srv.bytesReceived(), qint64(600000));
    }

    void cancellingMidTransferRemovesThePartialRemoteFile()
    {
        const QString root = fresh(QStringLiteral("cancel"));
        const QString local = fresh(QStringLiteral("cancellocal"));
        const QString src = QDir(local).filePath(QStringLiteral("huge.exfat"));
        QVERIFY(fwtest::writeRandomFile(src, 8 * 1024 * 1024, 11));
        // A file that was on the console before must survive a refused write.
        QVERIFY(fwtest::writeTextFile(QDir(root).filePath(QStringLiteral("g/locked.bin")), "original"));
        auto o = server(root);
        o.throttleBytesPerSec = 1024 * 1024; // 8 s for the whole file
        o.readOnlyPaths = {QStringLiteral("/g/locked.bin")};
        o.refuseDeleteWhileOpen = true; // like a Windows-hosted server: the aborted STOR still holds the file
        FakeFtpServer srv(o);
        TransferOptions opt;
        opt.endpoint = endpoint(srv.port());
        CancellationSource cs;
        Result<TransferReport> rep = Error(ErrorCode::Internal, QString());
        // A QThread, like the queue's pool threads: Qt sockets refuse std::thread.
        std::unique_ptr<QThread> worker(
            QThread::create([&] { rep = runUpload(planUpload({src}, QStringLiteral("/g")).value(), opt, cs.token()); }));
        worker->start();
        const bool flowing = QTest::qWaitFor([&] { return srv.bytesReceived() > 300 * 1024; }, 10000);
        const bool partialThere = QFileInfo::exists(srv.path(QStringLiteral("/g/huge.exfat")));
        cs.cancel();
        worker->wait();
        QVERIFY2(flowing, qPrintable(srv.commands().join(QStringLiteral(" | ")) + QStringLiteral(" received=%1").arg(srv.bytesReceived())));
        QVERIFY(partialThere);
        QVERIFY(rep.ok());
        QVERIFY(rep->cancelled);
        QCOMPARE(rep->removedPartials, QStringList{QStringLiteral("/g/huge.exfat")});
        QVERIFY(!QFileInfo::exists(srv.path(QStringLiteral("/g/huge.exfat"))));
        QVERIFY(srv.sawCommand(QStringLiteral("DELE /g/huge.exfat")));

        // Refused STOR (550): nothing was written, so nothing is deleted.
        srv.setThrottle(0);
        const QString locked = QDir(local).filePath(QStringLiteral("locked.bin"));
        QVERIFY(fwtest::writeTextFile(locked, "new data"));
        auto r2 = runUpload(planUpload({locked}, QStringLiteral("/g")).value(), opt, {});
        QVERIFY(r2.ok());
        QCOMPARE(r2->failed, 1);
        QCOMPARE(readAll(srv.path(QStringLiteral("/g/locked.bin"))), QByteArray("original"));
        QVERIFY(r2->removedPartials.isEmpty());
    }

    void noConnectionIsAnError()
    {
        TransferOptions opt;
        opt.endpoint = endpoint(1);
        opt.retries = 0;
        const QString local = fresh(QStringLiteral("noconn"));
        QVERIFY(fwtest::writeTextFile(QDir(local).filePath(QStringLiteral("a")), "a"));
        auto rep = runUpload(planUpload({QDir(local).filePath(QStringLiteral("a"))}, QStringLiteral("/x")).value(), opt, {});
        QVERIFY(!rep.ok());
        QVERIFY2(rep.error().message.contains(QStringLiteral("refused")), qPrintable(rep.error().message));
        QVERIFY(rep.error().message.contains(QStringLiteral("FTP server")));
        QVERIFY(!planUpload({QDir(local).filePath(QStringLiteral("missing"))}, QStringLiteral("/x")).ok());
    }


    // ---- review fixes: regression tests ----------------------------------------------------
    void controlDropBeforeStorRewritesInsteadOfAppending()
    {
        // Reviewer reproducer: the control connection drops on PASV, before the
        // STOR is sent. The retry must not "resume" from the OLD remote file.
        const QString root = fresh(QStringLiteral("ctl-drop"));
        const QString local = fresh(QStringLiteral("ctl-drop-local"));
        QVERIFY(fwtest::writeTextFile(QDir(root).filePath(QStringLiteral("data/game.exfat")), QByteArray(600 * 1024, 'O')));
        const QString src = QDir(local).filePath(QStringLiteral("game.exfat"));
        QVERIFY(fwtest::writeTextFile(src, QByteArray(1024 * 1024, 'N')));
        auto o = server(root);
        o.dropControlOnPasv = 1;
        FakeFtpServer srv(o);
        TransferOptions opt;
        opt.endpoint = endpoint(srv.port());
        opt.retries = 2;
        ProgressReporter progress;
        progress.begin(QStringLiteral("Uploading"), 1024 * 1024, 1);
        auto rep = runUpload(planUpload({src}, QStringLiteral("/data")).value(), opt, {}, &progress);
        QVERIFY(rep.ok());
        QCOMPARE(rep->transferred, 1);
        QCOMPARE(rep->verified, 1);
        const QByteArray got = readAll(srv.path(QStringLiteral("/data/game.exfat")));
        QCOMPARE(got.size(), 1024 * 1024);
        QCOMPARE(got.count('O'), 0);
        QCOMPARE(got, readAll(src));
        QVERIFY(!srv.sawCommand(QStringLiteral("REST")));
        QCOMPARE(progress.snapshot().done, qint64(1024 * 1024));
    }

    void controlDropBeforeStorKeepsAnApprovedResume()
    {
        // With resume on, the earlier partial was approved by the user: a drop
        // before the STOR still resumes from it (and only from it).
        const QString root = fresh(QStringLiteral("ctl-drop-resume"));
        const QString local = fresh(QStringLiteral("ctl-drop-resume-local"));
        const QString src = QDir(local).filePath(QStringLiteral("game.ffpkg"));
        QVERIFY(fwtest::writeRandomFile(src, 1000000, 21));
        const QByteArray all = readAll(src);
        QVERIFY(fwtest::writeTextFile(QDir(root).filePath(QStringLiteral("g/game.ffpkg")), all.left(400000)));
        auto o = server(root);
        o.dropControlOnPasv = 1;
        FakeFtpServer srv(o);
        TransferOptions opt;
        opt.endpoint = endpoint(srv.port());
        opt.resume = true;
        auto rep = runUpload(planUpload({src}, QStringLiteral("/g")).value(), opt, {});
        QVERIFY(rep.ok());
        QCOMPARE(rep->transferred, 1);
        QCOMPARE(readAll(srv.path(QStringLiteral("/g/game.ffpkg"))), all);
        QVERIFY(srv.sawCommand(QStringLiteral("REST 400000")));
        QCOMPARE(srv.bytesReceived(), qint64(600000));
    }

    void transientTransferRepliesAreRetried()
    {
        const QString root = fresh(QStringLiteral("t425"));
        const QString local = fresh(QStringLiteral("t425-local"));
        const QString src = QDir(local).filePath(QStringLiteral("a.bin"));
        QVERIFY(fwtest::writeRandomFile(src, 300000, 4));
        // The reply to the transfer command itself is what lastReply() reports.
        {
            auto o = server(root);
            o.failFirstTransferReply = "425 Can't open data connection.";
            FakeFtpServer srv(o);
            FtpClient c(quick());
            QVERIFY(c.open(QStringLiteral("127.0.0.1"), srv.port()).ok());
            QFile in(src);
            QVERIFY(in.open(QIODevice::ReadOnly));
            QVERIFY(!c.store(QStringLiteral("/a.bin"), in).ok());
            QCOMPARE(c.lastReply().code, 425);
            QVERIFY(!c.lastTransferStarted());
            QVERIFY(!c.lastTransferAccepted());
            QVERIFY(c.isConnected());
        }
        auto o = server(root);
        o.failFirstTransferReply = "425 Can't open data connection.";
        FakeFtpServer up(o);
        TransferOptions opt;
        opt.endpoint = endpoint(up.port());
        auto rep = runUpload(planUpload({src}, QStringLiteral("/")).value(), opt, {});
        QVERIFY(rep.ok());
        QCOMPARE(rep->transferred, 1);
        QCOMPARE(readAll(up.path(QStringLiteral("/a.bin"))), readAll(src));
        int stors = 0;
        for (const QString& cmd : up.commands())
            stors += cmd.startsWith(QStringLiteral("STOR ")) ? 1 : 0;
        QCOMPARE(stors, 2);

        o.failFirstTransferReply = "451 Requested action aborted: local error in processing.";
        FakeFtpServer down(o);
        const QString dl = fresh(QStringLiteral("t451"));
        FtpClient c(quick());
        QVERIFY(c.open(QStringLiteral("127.0.0.1"), down.port()).ok());
        TransferPlan plan;
        plan.files.push_back({QDir(dl).filePath(QStringLiteral("a.bin")), QStringLiteral("/a.bin"), 300000});
        TransferOptions dopt;
        dopt.endpoint = endpoint(down.port());
        auto drep = runDownload(plan, dopt, {});
        QVERIFY(drep.ok());
        QCOMPARE(drep->transferred, 1);
        QCOMPARE(readAll(QDir(dl).filePath(QStringLiteral("a.bin"))), readAll(src));
    }

    void downloadCommitSetsTheOldFileAside()
    {
        const QString dir = fresh(QStringLiteral("commit"));
        const QString fin = QDir(dir).filePath(QStringLiteral("game.exfat"));
        const QString part = fin + QStringLiteral(".part");
        QVERIFY(fwtest::writeTextFile(fin, "old"));
        QVERIFY(fwtest::writeTextFile(part, "new"));
        QVERIFY(commitDownloadedFile(part, fin).ok());
        QCOMPARE(readAll(fin), QByteArray("new"));
        QVERIFY(!QFileInfo::exists(part));
        QCOMPARE(QDir(dir).entryList(QDir::Files), QStringList{QStringLiteral("game.exfat")}); // no aside left

        // The rename of the .part fails: the old file is put back, nothing is lost.
        QVERIFY(!QFileInfo::exists(part));
        auto st = commitDownloadedFile(part, fin);
        QVERIFY(!st.ok());
        QCOMPARE(readAll(fin), QByteArray("new"));
        QCOMPARE(QDir(dir).entryList(QDir::Files), QStringList{QStringLiteral("game.exfat")});
#ifndef Q_OS_WIN
        // A folder that refuses changes: both the old file and the .part survive.
        const QString locked = fresh(QStringLiteral("commit-locked"));
        const QString lf = QDir(locked).filePath(QStringLiteral("x.bin"));
        QVERIFY(fwtest::writeTextFile(lf, "old"));
        QVERIFY(fwtest::writeTextFile(lf + QStringLiteral(".part"), "new"));
        QFile::setPermissions(locked, QFileDevice::ReadOwner | QFileDevice::ExeOwner);
        if (!QFile(QDir(locked).filePath(QStringLiteral("probe"))).open(QIODevice::WriteOnly)) { // not running as root
            auto r = commitDownloadedFile(lf + QStringLiteral(".part"), lf);
            QVERIFY(!r.ok());
            QVERIFY(r.error().message.contains(QStringLiteral(".part")));
            QCOMPARE(readAll(lf), QByteArray("old"));
            QCOMPARE(readAll(lf + QStringLiteral(".part")), QByteArray("new"));
        }
        QFile::setPermissions(locked, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
#endif
    }

    void remoteNamesAreNeverTrusted()
    {
        QVERIFY(remoteNameProblem(QStringLiteral("game.exfat")).isEmpty());
        for (const char* bad : {"", ".", "..", "a/b", "..\\..\\x", "a:stream", "x\ty"})
            QVERIFY2(!remoteNameProblem(QString::fromLatin1(bad)).isEmpty(), bad);
        QVERIFY(!localNameProblem(QStringLiteral("a:b")).isEmpty());
        QCOMPARE(*remoteChild(QStringLiteral("/data"), QStringLiteral("x y ")), QStringLiteral("/data/x y "));
        QCOMPARE(*remoteChild(QStringLiteral("/"), QStringLiteral("x")), QStringLiteral("/x"));
        QVERIFY(!remoteChild(QStringLiteral("/data"), QStringLiteral("..")));
        // joinRemote keeps characters (no backslash conversion, no trimming) and never climbs.
        QCOMPARE(joinRemote(QStringLiteral("/data"), QStringLiteral("a\\b")), QStringLiteral("/data/a\\b"));
        QCOMPARE(joinRemote(QStringLiteral("/data"), QStringLiteral("../../x")), QStringLiteral("/data/x"));
        QCOMPARE(joinRemote(QStringLiteral("/data"), QStringLiteral("dir /f ")), QStringLiteral("/data/dir /f "));
        QCOMPARE(cleanRemotePath(QStringLiteral("/a\\b/../c")), QStringLiteral("/c"));
        QCOMPARE(normalizeRemotePath(QStringLiteral(" \\a\\b ")), QStringLiteral("/a/b")); // typed input still converts

        // Downloads and deletes skip entries whose names would escape.
        const QString root = fresh(QStringLiteral("names"));
        QVERIFY(fwtest::writeTextFile(QDir(root).filePath(QStringLiteral("games/good.bin")), "good"));
        QVERIFY(fwtest::writeTextFile(QDir(root).filePath(QStringLiteral("evil.bin")), "outside"));
        for (bool mlsd : {false, true}) {
            auto o = server(root);
            o.mlsd = mlsd;
            o.extraListNames = {QStringLiteral("..\\..\\evil.bin"), QStringLiteral("good.bin:ads"), QStringLiteral("../up.bin"),
                                QStringLiteral("x\\y.bin")};
            FakeFtpServer srv(o);
            FtpClient c(quick());
            QVERIFY(c.open(QStringLiteral("127.0.0.1"), srv.port()).ok());
            const QString dl = fresh(QStringLiteral("names-dl"));
            auto plan = planDownload(c, {QStringLiteral("/games")}, dl);
            QVERIFY(plan.ok());
            QCOMPARE(int(plan->files.size()), 1);
            QCOMPARE(plan->files.front().remote, QStringLiteral("/games/good.bin"));
            QCOMPARE(plan->skipped.size(), 4);
            TransferOptions opt;
            opt.endpoint = endpoint(srv.port());
            auto rep = runDownload(plan.value(), opt, {});
            QVERIFY(rep.ok());
            QCOMPARE(QDir(dl).entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot), QStringList{QStringLiteral("games")});
            QCOMPARE(QDir(QDir(dl).filePath(QStringLiteral("games"))).entryList(QDir::Files), QStringList{QStringLiteral("good.bin")});
            QVERIFY(!QFileInfo::exists(QDir(dl).filePath(QStringLiteral("up.bin"))));
            QVERIFY(!QFileInfo::exists(QFileInfo(dl).absolutePath() + QStringLiteral("/up.bin")));
            // removeTree deletes the safe entry, refuses the rest, leaves "/evil.bin" alone.
            auto rm = c.removeTree(QStringLiteral("/games"));
            QCOMPARE(rm.error().code, ErrorCode::InvalidArgument);
            QVERIFY(rm.error().message.contains(QStringLiteral("evil.bin")));
            QCOMPARE(readAll(QDir(root).filePath(QStringLiteral("evil.bin"))), QByteArray("outside"));
            QVERIFY(QFileInfo(QDir(root).filePath(QStringLiteral("games"))).isDir());
            QVERIFY(fwtest::writeTextFile(QDir(root).filePath(QStringLiteral("games/good.bin")), "good"));
        }
    }

    void downloadJobReportsSkippedNames()
    {
        Module mod;
        const QString root = fresh(QStringLiteral("names-job"));
        QVERIFY(fwtest::writeTextFile(QDir(root).filePath(QStringLiteral("g/ok.bin")), "ok"));
        auto o = server(root);
        o.extraListNames = {QStringLiteral("..\\x.bin")};
        FakeFtpServer srv(o);
        const SettingsValues s(mod.schema, {{keys::kConsoleIp, QStringLiteral("127.0.0.1")}, {keys::kFtpPort, srv.port()}}, {});
        const QString dl = fresh(QStringLiteral("names-job-dl"));
        auto job = mod.jobs.create(jobkinds::kDownload, {{QStringLiteral("sources"), QJsonArray{QStringLiteral("/g")}},
                                                         {QStringLiteral("localDir"), dl}});
        ProgressReporter progress;
        progress.setPhases(job.value()->phases());
        JobContext ctx;
        ctx.progress = &progress;
        ctx.settings = s;
        ctx.log = Log(QStringLiteral("test"));
        auto out = job.value()->run(ctx);
        QVERIFY(out.ok());
        QCOMPARE(out->details.value(QStringLiteral("skippedNames")).toArray().size(), 1);
        QVERIFY(out->warnings.join(u' ').contains(QStringLiteral("unsafe name")));
        QCOMPARE(readAll(QDir(dl).filePath(QStringLiteral("g/ok.bin"))), QByteArray("ok"));
    }
    // ---- jobs and settings ----------------------------------------------------------------------
    void uploadAndDownloadJobs()
    {
        Module mod;
        const QString root = fresh(QStringLiteral("jobs"));
        const QString local = fresh(QStringLiteral("jobslocal"));
        const QString src = QDir(local).filePath(QStringLiteral("Game"));
        QVERIFY(fwtest::writeRandomFile(QDir(src).filePath(QStringLiteral("eboot.bin")), 50000, 1));
        QVERIFY(fwtest::writeRandomFile(QDir(src).filePath(QStringLiteral("sce_sys/param.json")), 300, 2));
        FakeFtpServer srv(server(root));
        const SettingsValues s(mod.schema,
                               {{keys::kConsoleIp, QStringLiteral("127.0.0.1")},
                                {keys::kFtpPort, srv.port()},
                                {keys::kParallelTransfers, 3},
                                {keys::kConnectTimeout, 3}},
                               {});
        const SettingsValues noIp(mod.schema, {}, {});

        auto job = mod.jobs.create(jobkinds::kUpload, {{QStringLiteral("sources"), QJsonArray{src}}});
        QVERIFY(job.ok());
        QVERIFY(!job.value()->validate(noIp).ok());
        QVERIFY(job.value()->validate(s).ok());
        // The local game folder is busy while it is being read.
        QCOMPARE(job.value()->busyPaths(), QStringList{QDir::cleanPath(QFileInfo(src).absoluteFilePath())});
        QVERIFY(busyPathsOverlap(job.value()->busyPaths(), {QDir(src).filePath(QStringLiteral("eboot.bin"))}));
        ProgressReporter progress;
        progress.setPhases(job.value()->phases());
        JobContext ctx;
        ctx.progress = &progress;
        ctx.settings = s;
        ctx.log = Log(QStringLiteral("test"));
        auto out = job.value()->run(ctx);
        QVERIFY2(out.ok(), out.ok() ? "" : qPrintable(out.error().toLogString()));
        QCOMPARE(out->verification, Verification::Passed);
        QCOMPARE(readAll(srv.path(QStringLiteral("/data/etaHEN/games/Game/eboot.bin"))),
                 readAll(QDir(src).filePath(QStringLiteral("eboot.bin"))));
        QCOMPARE(out->details.value(QStringLiteral("destination")).toString(), QStringLiteral("Internal PS5 storage"));

        // Protected destinations need an explicit opt-in.
        auto prot = mod.jobs.create(jobkinds::kUpload, {{QStringLiteral("sources"), QJsonArray{src}},
                                                        {QStringLiteral("remoteDir"), QStringLiteral("/system/x")}});
        auto pv = prot.value()->validate(s);
        QCOMPARE(pv.error().code, ErrorCode::PermissionDenied);
        QVERIFY(pv.error().message.contains(QStringLiteral("BRICK")));
        auto allowed = mod.jobs.create(jobkinds::kUpload, {{QStringLiteral("sources"), QJsonArray{src}},
                                                           {QStringLiteral("remoteDir"), QStringLiteral("/system/x")},
                                                           {QStringLiteral("allowProtectedPaths"), true}});
        QVERIFY(allowed.value()->validate(s).ok());

        const QString dl = fresh(QStringLiteral("jobsdl"));
        auto down = mod.jobs.create(jobkinds::kDownload, {{QStringLiteral("sources"), QJsonArray{QStringLiteral("/data/etaHEN/games/Game")}},
                                                          {QStringLiteral("localDir"), dl}});
        QVERIFY(down.value()->validate(s).ok());
        auto dout = down.value()->run(ctx);
        QVERIFY2(dout.ok(), dout.ok() ? "" : qPrintable(dout.error().toLogString()));
        QCOMPARE(readAll(QDir(dl).filePath(QStringLiteral("Game/sce_sys/param.json"))),
                 readAll(QDir(src).filePath(QStringLiteral("sce_sys/param.json"))));
        QCOMPARE(dout->verification, Verification::Passed);
    }

    void settingsRegistrationAndLegacyImport()
    {
        Module mod;
        int n = 0;
        for (const auto* d : mod.schema->all()) {
            QVERIFY(d->key.startsWith(QStringLiteral("ps5.")));
            QCOMPARE(d->section, QStringLiteral("PS5 connection"));
            QVERIFY2(mod.schema->normalize(d->key, d->defaultValue).ok(), qPrintable(d->key));
            QVERIFY(!d->description.isEmpty());
            ++n;
        }
        QCOMPARE(n, 15);
        QVERIFY(!mod.schema->normalize(keys::kFtpPort, 70000).ok());
        QVERIFY(!mod.schema->normalize(keys::kGamesFolder, QStringLiteral("data/x")).ok());
        QVERIFY(!mod.schema->normalize(keys::kConsoleIp, QStringLiteral("not an ip!")).ok());
        for (const QString& kind : {jobkinds::kUpload, jobkinds::kDownload, jobkinds::kPayloadSend, jobkinds::kDpiInstall,
                                    jobkinds::kY2jbAutoBlock, jobkinds::kShadowMountStop, jobkinds::kShadowMountEject})
            QVERIFY(mod.jobs.contains(kind));

        const QString legacy = QDir(fresh(QStringLiteral("legacy"))).filePath(QStringLiteral("s.json"));
        QVERIFY(fwtest::writeTextFile(legacy, "{\"ftp_ip\":\"192.168.1.50\",\"ps5_ip\":\"\",\"ftp_port\":1337,"
                                              "\"pl_port\":\"9020\",\"klog_port\":3232,\"y2jb_dpi_port\":\"12801\","
                                              "\"ftp_path\":\"/mnt/usb0/games\",\"smp_payload_port\":9021}"));
        SettingsStore store(mod.schema, QDir(fresh(QStringLiteral("store"))).filePath(QStringLiteral("s.json")));
        QVERIFY(store.load().ok());
        QCOMPARE(store.importLegacy(legacy).value(), 7);
        const ConsoleEndpoint ep = endpointFromSettings(store.snapshot());
        QCOMPARE(ep.host, QStringLiteral("192.168.1.50")); // empty ps5_ip did not wipe it
        QCOMPARE(ep.ftpPort, quint16(1337));
        QCOMPARE(ep.payloadPort, quint16(9020));
        QCOMPARE(ep.dpiPort, quint16(12801));
        QCOMPARE(ep.gamesFolder, QStringLiteral("/mnt/usb0/games"));
        QCOMPARE(ep.connectTimeoutMs, 10000);
        QCOMPARE(ep.idleTimeoutMs, 30000);
        QCOMPARE(ep.parallelTransfers, 2);
        QVERIFY(ep.passive);
        QCOMPARE(ftpOptionsFor(ep).connectTimeoutMs, 10000);
    }

    // ---- research §5: the payload port is elfldr's 9021, never etaHEN's DPI v1 9090 --------
    void payloadPortDefaultsAndMigration()
    {
        Module mod;
        QCOMPARE(mod.schema->find(keys::kPayloadPort)->defaultValue.toInt(), 9021);
        QCOMPARE(mod.schema->find(keys::kSmpApiPort)->defaultValue.toInt(), 10101);
        QCOMPARE(ConsoleEndpoint{}.payloadPort, quint16(9021));
        QVERIFY(mod.schema->find(keys::kFtpPort)->description.contains(QStringLiteral("2120")));
        QVERIFY(mod.schema->find(keys::kKlogPort)->description.contains(QStringLiteral("9081")));
        QVERIFY(mod.schema->find(keys::kDpiPort)->description.contains(QStringLiteral("DPI_v2=1")));

        // The legacy app's 9090 is imported as 9021.
        const QString legacy = QDir(fresh(QStringLiteral("legacy9090"))).filePath(QStringLiteral("s.json"));
        QVERIFY(fwtest::writeTextFile(legacy, "{\"pl_port\":\"9090\"}"));
        SettingsStore imported(mod.schema, QDir(fresh(QStringLiteral("store9090"))).filePath(QStringLiteral("s.json")));
        QVERIFY(imported.load().ok());
        QCOMPARE(imported.importLegacy(legacy).value(), 1);
        QCOMPARE(imported.value(keys::kPayloadPort).toInt(), 9021);

        // A stored 9090 becomes 9021 once; a 9090 set again afterwards stays.
        SettingsStore store(mod.schema, QDir(fresh(QStringLiteral("migr"))).filePath(QStringLiteral("s.json")));
        QVERIFY(store.load().ok());
        QVERIFY(store.setValue(keys::kPayloadPort, 9090).ok());
        const QString marker = QDir(fresh(QStringLiteral("marker"))).filePath(QStringLiteral("ps5-migrations.txt"));
        QVERIFY(migrateLegacyPayloadPort(store, marker));
        QCOMPARE(store.value(keys::kPayloadPort).toInt(), 9021);
        QVERIFY(QFileInfo::exists(marker));
        QVERIFY(store.setValue(keys::kPayloadPort, 9090).ok());
        QVERIFY(!migrateLegacyPayloadPort(store, marker));
        QCOMPARE(store.value(keys::kPayloadPort).toInt(), 9090);
        // Another port is never touched.
        SettingsStore other(mod.schema, QDir(fresh(QStringLiteral("migr2"))).filePath(QStringLiteral("s.json")));
        QVERIFY(other.load().ok());
        QVERIFY(other.setValue(keys::kPayloadPort, 9020).ok());
        QVERIFY(!migrateLegacyPayloadPort(other, QDir(fresh(QStringLiteral("marker2"))).filePath(QStringLiteral("m.txt"))));
        QCOMPARE(other.value(keys::kPayloadPort).toInt(), 9020);
    }

    // ---- research §4.1: ftpsrv SELF decryption, identity, FEAT, no NLST -------------------------
    void ftpsrvSelfModeIsTurnedOffAfterLogin()
    {
        const QString root = fresh(QStringLiteral("ftpsrv"));
        const QString local = fresh(QStringLiteral("ftpsrv-local"));
        // An fself eboot: SELF magic, then data. In SELF mode ftpsrv reports and
        // sends a "decrypted" ELF instead (16 bytes shorter in the fake).
        QByteArray self = QByteArray("\x4f\x15\x3d\x1d", 4);
        for (int i = 0; i < 4000; ++i)
            self += char(i * 7);
        const QString eboot = QDir(local).filePath(QStringLiteral("eboot.bin"));
        QVERIFY(fwtest::writeTextFile(eboot, self));
        FakeFtpServer::Options o;
        o.root = root;
        o.ftpsrv = true;
        QDir().mkpath(root + QStringLiteral("/data/etaHEN/games"));
        FakeFtpServer srv(o);

        FtpClient c(quick());
        QVERIFY(c.open(QStringLiteral("127.0.0.1"), srv.port()).ok());
        QCOMPARE(c.selfMode(), SelfMode::Disabled);
        QVERIFY(srv.sawCommand(QStringLiteral("SELF")));
        QVERIFY(!srv.sawCommand(QStringLiteral("PASS"))); // USER answers 230 on ftpsrv
        const FtpServerIdentity id = c.serverIdentity();
        QCOMPARE(id.name, QStringLiteral("ftpsrv"));
        QCOMPARE(id.version, QStringLiteral("v0.21.1"));
        QCOMPARE(id.label(), QStringLiteral("ftpsrv v0.21.1"));
        auto feats = c.features();
        QVERIFY(feats->ftpsrv && feats->mlsd && feats->rest && feats->size); // FEAT lists only MLSD KILL MTRW SELF

        // Upload with the size check: the file stays, byte for byte.
        TransferOptions opt;
        opt.endpoint = endpoint(srv.port());
        auto rep = runUpload(planUpload({eboot}, QStringLiteral("/data/etaHEN/games")).value(), opt, {});
        QVERIFY(rep.ok());
        QCOMPARE(rep->failed, 0);
        QCOMPARE(rep->verified, 1);
        QCOMPARE(readAll(srv.path(QStringLiteral("/data/etaHEN/games/eboot.bin"))), self);
        QCOMPARE(c.size(QStringLiteral("/data/etaHEN/games/eboot.bin")).value(), qint64(self.size()));
        QCOMPARE(c.download(QStringLiteral("/data/etaHEN/games/eboot.bin")).value(), self); // raw bytes, not the ELF

        // No NLST on ftpsrv: existence comes from the folder listing.
        QVERIFY(c.fileExists(QStringLiteral("/data/etaHEN/games/eboot.bin")));
        QVERIFY(!c.fileExists(QStringLiteral("/data/etaHEN/games/none.bin")));
        QVERIFY(!c.fileExists(QStringLiteral("/data/missing/none.bin")));
        QCOMPARE(c.listedSize(QStringLiteral("/data/etaHEN/games/eboot.bin")).value(), qint64(self.size()));
        QCOMPARE(c.pwd().value(), QStringLiteral("/")); // the listing did not move the working folder
        QVERIFY(!c.siteAvail().ok());                   // 502: free space is not an FTP feature here
        QVERIFY(!srv.sawCommand(QStringLiteral("KILL")));
    }

    void ftpsrvThatKeepsSelfModeDoesNotLoseUploads()
    {
        // A build where SELF cannot be turned off: SIZE lies about fself
        // files, the listing does not, and the upload is kept (it used to be
        // deleted as "the PS5 reports N bytes, expected M").
        const QString root = fresh(QStringLiteral("ftpsrv-self"));
        const QString local = fresh(QStringLiteral("ftpsrv-self-local"));
        const QByteArray self = QByteArray("\x54\x14\xf5\xee", 4) + QByteArray(5000, 'x');
        const QString lib = QDir(local).filePath(QStringLiteral("libSceAmpr.sprx"));
        QVERIFY(fwtest::writeTextFile(lib, self));
        FakeFtpServer::Options o;
        o.root = root;
        o.ftpsrv = true;
        o.selfIgnored = true;
        QDir().mkpath(root + QStringLiteral("/up"));
        FakeFtpServer srv(o);
        FtpClient c(quick());
        QVERIFY(c.open(QStringLiteral("127.0.0.1"), srv.port()).ok());
        QCOMPARE(c.selfMode(), SelfMode::StillEnabled);
        TransferOptions opt;
        opt.endpoint = endpoint(srv.port());
        auto rep = runUpload(planUpload({lib}, QStringLiteral("/up")).value(), opt, {});
        QVERIFY(rep.ok());
        QCOMPARE(rep->failed, 0);
        QCOMPARE(readAll(srv.path(QStringLiteral("/up/libSceAmpr.sprx"))), self);
        QVERIFY(srv.sawCommand(QStringLiteral("MLSD")));

        // Other servers never get SELF.
        FakeFtpServer::Options plain;
        plain.root = root;
        FakeFtpServer other(plain);
        FtpClient c2(quick());
        QVERIFY(c2.open(QStringLiteral("127.0.0.1"), other.port()).ok());
        QCOMPARE(c2.selfMode(), SelfMode::NotApplicable);
        QVERIFY(!other.sawCommand(QStringLiteral("SELF")));
        QCOMPARE(identifyFtpServer(QStringLiteral("Service ready for new user.")).name, QStringLiteral("etaHEN"));
        QCOMPARE(identifyFtpServer(QStringLiteral("zftpd v1.7.0 ready")).version, QStringLiteral("v1.7.0"));
        QCOMPARE(identifyFtpServer(QStringLiteral("FakePS5 FTP ready")).label(), QStringLiteral("FTP server"));
    }
};

QTEST_GUILESS_MAIN(TstPs5Ftp)
#include "tst_ps5_ftp.moc"

