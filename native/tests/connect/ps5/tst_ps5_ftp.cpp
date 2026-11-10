// FtpClient, listing parsers, remote path rules and the parallel transfer
// engine, against the in-process FakeFtpServer (never a real console).

#include "FakePs5.h"
#include "Ps5TestUtil.h"

#include "connect/ps5/Endpoint.h"
#include "connect/ps5/FtpClient.h"
#include "connect/ps5/FtpListing.h"
#include "connect/ps5/FtpTransfer.h"
#include "connect/ps5/Module.h"
#include "connect/ps5/RemotePath.h"
#include "core/FileUtil.h"
#include "support/ClosedPort.h"
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
using ps5test::endpoint;
using ps5test::Module;
using ps5test::quick;
using ps5test::readAll;

namespace {

// Sends one raw command line (none: only reads, for the greeting) and returns
// the reply line that carries the code; empty when nothing came in time. For
// commands FtpClient itself would never send.
QByteArray rawCommand(QTcpSocket& s, const QByteArray& line)
{
    if (!line.isEmpty()) {
        s.write(line + "\r\n");
        s.flush();
    }
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < 10000) {
        while (s.canReadLine()) {
            const QByteArray reply = s.readLine().trimmed();
            if (reply.size() >= 4 && reply.at(3) == ' ')
                return reply;
        }
        if (!s.waitForReadyRead(250) && s.state() != QAbstractSocket::ConnectedState)
            break;
    }
    return {};
}

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
        QVERIFY(future);
        QCOMPARE(future->modified.date().year(), 2025); // a date in the future is last year
        auto link = parseListLine(QStringLiteral("lrwxrwxrwx 1 root root 7 Jan 1 00:00 usb -> /mnt/usb0"), now);
        QVERIFY(link);
        QVERIFY(link->isLink);
        QCOMPARE(link->name, QStringLiteral("usb"));
        QCOMPARE(link->linkTarget, QStringLiteral("/mnt/usb0"));
        auto dos = parseListLine(QStringLiteral("01-02-24  03:04PM       <DIR>          Folder X"), now);
        QVERIFY(dos && dos->isDir);
        QCOMPARE(dos->name, QStringLiteral("Folder X"));
        QCOMPARE(dos->modified, QDateTime(QDate(2024, 1, 2), QTime(15, 4), QTimeZone::utc()));
        auto dosFile = parseListLine(QStringLiteral("12-31-99  11:59AM              42 a.txt"), now);
        QVERIFY(dosFile);
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
        const auto upper = parseMlsdLine(QStringLiteral("Type=DIR;Modify=20240101000000; d"));
        QVERIFY(upper && upper->isDir);
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
        for (const char* p : {"/system", "/SYSTEM/lib", "/dev/", "system_ex/x", "/preinst2", "/data/../system", "//proc",
                              "/system_data/priv/mms/app.db", "/user/app/PPSA01325", "/user/home/1234/savedata", "/mnt/sandbox/x"})
            QVERIFY2(isProtectedRemotePath(QString::fromLatin1(p)), p);
        for (const char* p : {"/data/system", "/mnt/usb0", "/systemx", "/user/data/tmp", "/user/appmeta", "/data/etaHEN/games", "", "/"})
            QVERIFY2(!isProtectedRemotePath(QString::fromLatin1(p)), p);
        // A parent of a protected root is protected too: deleting or renaming it reaches the root.
        for (const char* p : {"/user", "/USER/", "/mnt", "/data/../user", "user"})
            QVERIFY2(isProtectedRemotePath(QString::fromLatin1(p)), p);
        for (const char* p : {"/use", "/user/data", "/mnt/ext0", "/m"})
            QVERIFY2(!isProtectedRemotePath(QString::fromLatin1(p)), p);
        QVERIFY(protectedPathWarning(QStringLiteral("/system"), QStringLiteral("delete")).contains(QStringLiteral("brick")));
        // An upload is judged by what it writes: the folder, and each source's name in it.
        QCOMPARE(protectedUploadTarget(QStringLiteral("/"), {QStringLiteral("C:/dumps/system_ex")}), QStringLiteral("/system_ex"));
        QCOMPARE(protectedUploadTarget(QStringLiteral("/"), {QStringLiteral("C:/dumps/ok"), QStringLiteral("C:/dumps/PREINST/")}), QStringLiteral("/PREINST"));
        QCOMPARE(protectedUploadTarget(QStringLiteral("/user"), {QStringLiteral("C:/x/readme.txt")}), QStringLiteral("/user"));
        QCOMPARE(protectedUploadTarget(QStringLiteral("/user/"), {}), QStringLiteral("/user"));
        QCOMPARE(protectedUploadTarget(QStringLiteral("/"), {QStringLiteral("D:/a/user")}), QStringLiteral("/user"));
        QCOMPARE(protectedUploadTarget(QStringLiteral("/user/data"), {QStringLiteral("D:/a/app")}), QString());
        QCOMPARE(protectedUploadTarget(QStringLiteral("/data/etaHEN/games"), {QStringLiteral("D:/a/system"), QStringLiteral("D:/a/b.exfat")}), QString());
        QCOMPARE(protectedUploadTarget(QStringLiteral("/"), {QStringLiteral("D:/a/data")}), QString());
        QCOMPARE(classifyDestination(QStringLiteral("/mnt/usb0/games")).kind, DestinationKind::External);
        QCOMPARE(classifyDestination(QStringLiteral("/data/etaHEN/games/")).kind, DestinationKind::Internal);
        QCOMPARE(classifyDestination(QStringLiteral("/foo")).kind, DestinationKind::Unknown);
        // Whole path segments: "/database" is not "/data"; /mnt/ext* is the console's own extended storage.
        QCOMPARE(classifyDestination(QStringLiteral("/database/x")).kind, DestinationKind::Unknown);
        QCOMPARE(classifyDestination(QStringLiteral("/data")).kind, DestinationKind::Internal);
        QCOMPARE(classifyDestination(QStringLiteral("/system_ex/app")).kind, DestinationKind::Internal);
        QCOMPARE(classifyDestination(QStringLiteral("/mnt/ext1/games")).kind, DestinationKind::External);
        QVERIFY(classifyDestination(QStringLiteral("/mnt/ext1/games")).label.startsWith(QStringLiteral("Extended storage")));
        QCOMPARE(classifyDestination(QStringLiteral("/mnt/extusb0")).label, QStringLiteral("External USB / exFAT"));
    }

    // ---- the fake itself ------------------------------------------------------------------
    // The fake console serves a folder of this PC. Whatever a client sends, it
    // must not read, change or delete anything outside that folder.
    void fakeServerStaysInsideItsRoot()
    {
        const QString outer = fresh(QStringLiteral("jail"));
        const QString root = QDir(outer).filePath(QStringLiteral("root"));
        const QString outside = QDir(outer).filePath(QStringLiteral("outside.txt"));
        QVERIFY(fwtest::writeTextFile(QDir(root).filePath(QStringLiteral("sub/inside.txt")), "inside"));
        QVERIFY(fwtest::writeTextFile(outside, "outside"));
        const QString top = QStringLiteral("/"), sub = QStringLiteral("/sub");

        QCOMPARE(fake::toLocalPath(root, top, QStringLiteral("/sub/inside.txt")), root + QStringLiteral("/sub/inside.txt"));
        QCOMPARE(fake::toLocalPath(root, sub, QStringLiteral("../sub/./inside.txt")), root + QStringLiteral("/sub/inside.txt"));
        QCOMPARE(fake::toLocalPath(root, sub, QString()), root + QStringLiteral("/sub"));
        QVERIFY(fake::toLocalPath(QString(), top, QStringLiteral("/x")).isEmpty()); // no root: nothing is served
        const QStringList climbing = {QStringLiteral("../outside.txt"),         QStringLiteral("/../outside.txt"),
                                      QStringLiteral("/sub/../../outside.txt"), QStringLiteral("../../outside.txt"),
                                      QStringLiteral("..\\outside.txt"),        QStringLiteral("/sub/..\\..\\outside.txt"),
                                      QStringLiteral("/sub/../..\\outside.txt")};
        for (const QString& p : climbing)
            for (const QString& cwd : {top, sub}) {
                const QString local = fake::toLocalPath(root, cwd, p);
                QVERIFY2(local.isEmpty() || local.startsWith(root + u'/'), qPrintable(p + QStringLiteral(" -> ") + local));
            }

        FakeFtpServer srv(server(root));
        QTcpSocket s;
        s.connectToHost(QHostAddress(QHostAddress::LocalHost), srv.port());
        QVERIFY(s.waitForConnected(10000));
        QVERIFY(rawCommand(s, {}).startsWith("220"));
        QVERIFY(rawCommand(s, "USER anonymous").startsWith("331"));
        QVERIFY(rawCommand(s, "PASS x").startsWith("230"));
        for (const QString& p : climbing) {
            // Outside the root or (where the host keeps the name inside it) not
            // there: refused either way.
            for (const char* cmd : {"DELE", "RMD", "RNFR", "SIZE", "MDTM", "RETR", "CWD", "LIST", "MLSD"}) {
                const QByteArray line = QByteArray(cmd) + ' ' + p.toUtf8();
                const QByteArray reply = rawCommand(s, line);
                QVERIFY2(reply.startsWith('5'), (line + " -> " + reply).constData());
            }
            if (!fake::toLocalPath(root, top, p).isEmpty())
                continue; // a name the host keeps inside the root may be created there
            for (const char* cmd : {"STOR", "APPE", "MKD"}) {
                const QByteArray line = QByteArray(cmd) + ' ' + p.toUtf8();
                const QByteArray reply = rawCommand(s, line);
                QVERIFY2(reply.startsWith("550"), (line + " -> " + reply).constData());
            }
        }
        // A rename cannot move a file out either.
        QVERIFY(rawCommand(s, "RNFR /sub/inside.txt").startsWith("350"));
        (void)rawCommand(s, "RNTO /sub/../../moved.txt");
        QVERIFY(!QFileInfo::exists(QDir(outer).filePath(QStringLiteral("moved.txt"))));
        // The top folder is its own parent.
        QVERIFY(rawCommand(s, "CDUP").startsWith("250"));
        QVERIFY(rawCommand(s, "CDUP").startsWith("250"));
        QVERIFY2(rawCommand(s, "PWD").startsWith("257 \"/\""), "PWD after CDUP at the top");

        QCOMPARE(readAll(outside), QByteArray("outside"));
        QCOMPARE(QDir(outer).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden, QDir::Name),
                 (QStringList{QStringLiteral("outside.txt"), QStringLiteral("root")}));
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
        COMPARE_ERROR_CODE(st, ErrorCode::PermissionDenied);
        QVERIFY(srv.sawCommand(QStringLiteral("USER anonymous")));
        FtpOptions creds = quick();
        creds.user = QStringLiteral("ps5");
        creds.password = QStringLiteral("secret");
        FtpClient ok(creds);
        QVERIFY(ok.open(QStringLiteral("127.0.0.1"), srv.port()).ok());
        QVERIFY(ok.welcome().contains(QStringLiteral("FakePS5")));
        ok.quit();
        QVERIFY(!ok.isConnected());

        const fwtest::ClosedPort closed;
        QVERIFY(closed.isValid());
        FtpClient refused(quick());
        auto r = refused.open(QStringLiteral("127.0.0.1"), closed.port());
        QVERIFY(!r.ok());
        // Refused on Linux; on Windows a short timeout may report it as no answer.
        QVERIFY2(r.error().message.contains(QStringLiteral("refused")), qPrintable(r.error().message));
        QVERIFY(r.error().message.contains(QStringLiteral("127.0.0.1:%1").arg(closed.port())));
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
        const auto features = c.features();
        QVERIFY(features.ok());
        QCOMPARE(features->mlsd, mlsd);
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
        COMPARE_VALUE(c.pwd(), QStringLiteral("/"));
        QVERIFY(c.cwd(QStringLiteral("/data")).ok());
        COMPARE_VALUE(c.pwd(), QStringLiteral("/data"));
        COMPARE_ERROR_CODE(c.cwd(QStringLiteral("/missing")), ErrorCode::NotFound);
        COMPARE_VALUE(c.size(QStringLiteral("/data/f.txt")), qint64(5));
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
        COMPARE_VALUE(c.siteAvail(), QStringLiteral("123456789"));
        const auto names = c.nameList(QStringLiteral("/data"));
        QVERIFY(names.ok() && names.value().contains(QStringLiteral("g.txt")));
        QVERIFY(c.upload(QStringLiteral("/data/a/b/c/x.bin"), "xyz").ok());
        QVERIFY(c.removeTree(QStringLiteral("/data/a")).ok());
        QVERIFY(!QFileInfo::exists(QDir(root).filePath(QStringLiteral("data/a"))));
        QVERIFY(c.dele(QStringLiteral("/data/g.txt")).ok());
        COMPARE_ERROR_CODE(c.dele(QStringLiteral("/data/g.txt")), ErrorCode::NotFound);
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
        // Binary mode was asked for before any file data moved.
        const QStringList sent = srv.commands();
        const qsizetype binaryAt = sent.indexOf(QStringLiteral("TYPE I"));
        const qsizetype storAt = sent.indexOf(QStringLiteral("STOR /game.exfat"));
        QVERIFY2(binaryAt >= 0 && storAt > binaryAt, qPrintable(sent.join(QStringLiteral(" | "))));
        // A missing file is NotFound and the connection survives.
        QByteArray none;
        QBuffer nb(&none);
        nb.open(QIODevice::WriteOnly);
        COMPARE_ERROR_CODE(c.retrieve(QStringLiteral("/none"), nb), ErrorCode::NotFound);
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
        COMPARE_ERROR_CODE(st, ErrorCode::Unsupported);
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
        COMPARE_ERROR_CODE(st, ErrorCode::Timeout);
        QVERIFY(t.elapsed() < 8000); // the 400 ms asked for (with room for a busy PC), not the default 10 s

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
        COMPARE_ERROR_CODE(up, ErrorCode::Timeout);
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
        QVERIFY(again.ok());
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
        QVERIFY(plan.ok());
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
        QVERIFY(dplan.ok());
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
        auto repPlan = planUpload({src, done}, QStringLiteral("/g"));
        QVERIFY(repPlan.ok());
        auto rep = runUpload(repPlan.value(), opt, {});
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
        o.throttleBytesPerSec = 1024 * 1024; // several seconds for the whole file: time to cancel
        o.readOnlyPaths = {QStringLiteral("/g/locked.bin")};
        o.refuseDeleteWhileOpen = true; // like a Windows-hosted server: the aborted STOR still holds the file
        FakeFtpServer srv(o);
        TransferOptions opt;
        opt.endpoint = endpoint(srv.port());
        CancellationSource cs;
        Result<TransferReport> rep = Error(ErrorCode::Internal, QString());
        const auto plan = planUpload({src}, QStringLiteral("/g"));
        QVERIFY(plan.ok());
        // A QThread, like the queue's pool threads: Qt sockets refuse std::thread.
        std::unique_ptr<QThread> worker(QThread::create([&] { rep = runUpload(plan.value(), opt, cs.token()); }));
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
        auto r2Plan = planUpload({locked}, QStringLiteral("/g"));
        QVERIFY(r2Plan.ok());
        auto r2 = runUpload(r2Plan.value(), opt, {});
        QVERIFY(r2.ok());
        QCOMPARE(r2->failed, 1);
        QCOMPARE(readAll(srv.path(QStringLiteral("/g/locked.bin"))), QByteArray("original"));
        QVERIFY(r2->removedPartials.isEmpty());
    }

    // The other direction: a cancelled download leaves no half file on this PC.
    void cancellingADownloadRemovesItsPartFile()
    {
        const QString root = fresh(QStringLiteral("dlcancel"));
        QVERIFY(fwtest::writeRandomFile(QDir(root).filePath(QStringLiteral("g/huge.exfat")), 8 * 1024 * 1024, 12));
        const QString local = fresh(QStringLiteral("dlcancel-local"));
        // What was in the folder before stays.
        QVERIFY(fwtest::writeTextFile(QDir(local).filePath(QStringLiteral("keep.txt")), "mine"));
        auto o = server(root);
        o.throttleBytesPerSec = 1024 * 1024; // seconds for the whole file: time to cancel
        FakeFtpServer srv(o);
        TransferOptions opt;
        opt.endpoint = endpoint(srv.port());
        TransferPlan plan;
        {
            FtpClient c(quick());
            QVERIFY(c.open(QStringLiteral("127.0.0.1"), srv.port()).ok());
            auto p = planDownload(c, {QStringLiteral("/g/huge.exfat")}, local);
            QVERIFY(p.ok());
            plan = p.value();
            c.quit();
        }
        QCOMPARE(int(plan.files.size()), 1);
        const QString target = plan.files.front().local;
        const QString part = target + QStringLiteral(".part");
        ProgressReporter progress;
        progress.begin(QStringLiteral("Downloading"), plan.totalBytes, 1);
        CancellationSource cs;
        Result<TransferReport> rep = Error(ErrorCode::Internal, QString());
        std::unique_ptr<QThread> worker(QThread::create([&] { rep = runDownload(plan, opt, cs.token(), &progress); }));
        worker->start();
        const bool flowing = QTest::qWaitFor([&] { return progress.snapshot().done > 200 * 1024; }, 20000);
        const bool partThere = QFileInfo::exists(part);
        cs.cancel();
        worker->wait();
        QVERIFY2(flowing, qPrintable(srv.commands().join(QStringLiteral(" | "))));
        QVERIFY(partThere);
        QVERIFY(rep.ok());
        QVERIFY(rep->cancelled);
        QCOMPARE(rep->transferred, 0);
        QCOMPARE(rep->removedPartials, QStringList{part});
        QVERIFY(!QFileInfo::exists(part));
        QVERIFY(!QFileInfo::exists(target)); // and nothing that looks like the finished file
        QCOMPARE(QDir(local).entryList(QDir::AllEntries | QDir::NoDotAndDotDot), QStringList{QStringLiteral("keep.txt")});
        QCOMPARE(readAll(QDir(local).filePath(QStringLiteral("keep.txt"))), QByteArray("mine"));
    }

    // The server says 226 but kept only part of the file: the upload counts as
    // failed and the short file does not stay on the console under the game's name.
    void uploadThatArrivesShortIsFailedAndRemoved()
    {
        const QString root = fresh(QStringLiteral("short"));
        const QString local = fresh(QStringLiteral("short-local"));
        const QString game = QDir(local).filePath(QStringLiteral("game.ffpkg"));
        const QString other = QDir(local).filePath(QStringLiteral("other.bin"));
        QVERIFY(fwtest::writeRandomFile(game, 300000, 31));
        QVERIFY(fwtest::writeRandomFile(other, 5000, 32));
        QVERIFY(fwtest::writeTextFile(QDir(root).filePath(QStringLiteral("up/keep.bin")), "console file"));
        auto o = server(root);
        o.corruptStorPath = QStringLiteral("/up/game.ffpkg");
        FakeFtpServer srv(o);
        TransferOptions opt;
        opt.endpoint = endpoint(srv.port());
        auto plan = planUpload({game, other}, QStringLiteral("/up"));
        QVERIFY(plan.ok());
        auto rep = runUpload(plan.value(), opt, {});
        QVERIFY2(rep.ok(), rep.ok() ? "" : qPrintable(rep.error().toLogString()));
        QCOMPARE(rep->failed, 1);
        QCOMPARE(rep->transferred, 1);
        QCOMPARE(rep->errors.size(), 1);
        QVERIFY2(rep->errors.front().startsWith(QStringLiteral("/up/game.ffpkg: the PS5 reports 150000 bytes, expected 300000")),
                 qPrintable(rep->errors.front()));
        QCOMPARE(rep->removedPartials, QStringList{QStringLiteral("/up/game.ffpkg")});
        QVERIFY(rep->leftoverPartials.isEmpty());
        QVERIFY(!rep->outputs.contains(QStringLiteral("/up/game.ffpkg")));
        QVERIFY(!QFileInfo::exists(srv.path(QStringLiteral("/up/game.ffpkg"))));
        QVERIFY(srv.sawCommand(QStringLiteral("DELE /up/game.ffpkg")));
        // The other file of the run and what was on the console before are untouched.
        QCOMPARE(readAll(srv.path(QStringLiteral("/up/other.bin"))), readAll(other));
        QCOMPARE(readAll(srv.path(QStringLiteral("/up/keep.bin"))), QByteArray("console file"));
    }

    void noConnectionIsAnError()
    {
        const fwtest::ClosedPort closed;
        QVERIFY(closed.isValid());
        TransferOptions opt;
        opt.endpoint = endpoint(closed.port());
        opt.retries = 0;
        const QString local = fresh(QStringLiteral("noconn"));
        QVERIFY(fwtest::writeTextFile(QDir(local).filePath(QStringLiteral("a")), "a"));
        auto repPlan = planUpload({QDir(local).filePath(QStringLiteral("a"))}, QStringLiteral("/x"));
        QVERIFY(repPlan.ok());
        auto rep = runUpload(repPlan.value(), opt, {});
        QVERIFY(!rep.ok());
        QVERIFY2(rep.error().message.contains(QStringLiteral("refused")), qPrintable(rep.error().message));
        QVERIFY(rep.error().message.contains(QStringLiteral("FTP server")));
        QVERIFY(!planUpload({QDir(local).filePath(QStringLiteral("missing"))}, QStringLiteral("/x")).ok());
    }

    void givingUpAfterALostConnectionCleansUp()
    {
        const QString local = fresh(QStringLiteral("gonelocal"));
        const QString src = QDir(local).filePath(QStringLiteral("big.bin"));
        QVERIFY(fwtest::writeRandomFile(src, 2 * 1024 * 1024, 21));
        TransferOptions opt;
        opt.retries = 1;

        // The console is back once the retry budget is used up: the
        // half-written file is removed and named in the report.
        {
            const QString root = fresh(QStringLiteral("goneback"));
            auto o = server(root);
            o.dropAfterBytes = 600 * 1024;
            o.goneAfterDrop = 1;
            FakeFtpServer srv(o);
            opt.endpoint = endpoint(srv.port());
            auto repPlan = planUpload({src}, QStringLiteral("/up"));
            QVERIFY(repPlan.ok());
            auto rep = runUpload(repPlan.value(), opt, {});
            QVERIFY2(rep.ok(), rep.ok() ? "" : qPrintable(rep.error().toLogString()));
            QCOMPARE(rep->failed, 1);
            QCOMPARE(rep->transferred, 0);
            QCOMPARE(rep->removedPartials, QStringList{QStringLiteral("/up/big.bin")});
            QVERIFY(rep->leftoverPartials.isEmpty());
            QVERIFY(!QFileInfo::exists(srv.path(QStringLiteral("/up/big.bin"))));
        }
        // The console stays away: the file cannot be removed, and the report
        // (not a bare connection error) says that it may remain.
        {
            const QString root = fresh(QStringLiteral("gone"));
            auto o = server(root);
            o.dropAfterBytes = 600 * 1024;
            o.goneAfterDrop = -1;
            FakeFtpServer srv(o);
            opt.endpoint = endpoint(srv.port());
            auto repPlan = planUpload({src}, QStringLiteral("/up"));
            QVERIFY(repPlan.ok());
            auto rep = runUpload(repPlan.value(), opt, {});
            QVERIFY2(rep.ok(), rep.ok() ? "" : qPrintable(rep.error().toLogString()));
            QCOMPARE(rep->failed, 1);
            QCOMPARE(rep->leftoverPartials, QStringList{QStringLiteral("/up/big.bin")});
            QVERIFY(rep->removedPartials.isEmpty());
            QVERIFY(QFileInfo::exists(srv.path(QStringLiteral("/up/big.bin"))));
        }
        // Download: the .part of a file that was given up does not stay.
        {
            const QString root = fresh(QStringLiteral("gonedown"));
            QVERIFY(QDir().mkpath(QDir(root).filePath(QStringLiteral("up"))));
            QVERIFY(QFile::copy(src, QDir(root).filePath(QStringLiteral("up/big.bin"))));
            auto o = server(root);
            o.dropAfterBytes = 500 * 1024;
            o.goneAfterDrop = -1;
            FakeFtpServer srv(o);
            opt.endpoint = endpoint(srv.port());
            const QString down = fresh(QStringLiteral("gonedownlocal"));
            TransferPlan plan;
            {
                FtpClient c(quick());
                QVERIFY(c.open(QStringLiteral("127.0.0.1"), srv.port()).ok());
                auto p = planDownload(c, {QStringLiteral("/up/big.bin")}, down);
                QVERIFY(p.ok());
                plan = p.value();
                c.quit();
            }
            auto rep = runDownload(plan, opt, {});
            QVERIFY(rep.ok());
            QCOMPARE(rep->failed, 1);
            QCOMPARE(rep->transferred, 0);
            QVERIFY(QDir(down).entryList(QDir::Files).isEmpty());
        }
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
        auto repPlan = planUpload({src}, QStringLiteral("/data"));
        QVERIFY(repPlan.ok());
        auto rep = runUpload(repPlan.value(), opt, {}, &progress);
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
        auto repPlan = planUpload({src}, QStringLiteral("/g"));
        QVERIFY(repPlan.ok());
        auto rep = runUpload(repPlan.value(), opt, {});
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
        auto repPlan = planUpload({src}, QStringLiteral("/"));
        QVERIFY(repPlan.ok());
        auto rep = runUpload(repPlan.value(), opt, {});
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

        // The old file cannot be moved out of the way: both it and the .part survive.
        const QString locked = fresh(QStringLiteral("commit-locked"));
        const QString lf = QDir(locked).filePath(QStringLiteral("x.bin"));
        QVERIFY(fwtest::writeTextFile(lf, "old"));
        QVERIFY(fwtest::writeTextFile(lf + QStringLiteral(".part"), "new"));
#ifdef Q_OS_WIN
        QFile inUse(lf); // Windows does not rename a file another program has open
        QVERIFY(inUse.open(QIODevice::ReadOnly));
        const bool blocked = true;
#else
        // A folder that refuses changes (not for root, who may change it all the same).
        QFile::setPermissions(locked, QFileDevice::ReadOwner | QFileDevice::ExeOwner);
        const bool blocked = !QFile(QDir(locked).filePath(QStringLiteral("probe"))).open(QIODevice::WriteOnly);
#endif
        const Status kept = blocked ? commitDownloadedFile(lf + QStringLiteral(".part"), lf) : Status();
#ifndef Q_OS_WIN
        QFile::setPermissions(locked, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
#endif
        if (!blocked)
            QSKIP("This account can change a read-only folder (root): the blocked replace cannot be shown.");
        QVERIFY(!kept.ok());
        QVERIFY2(kept.error().message.contains(QStringLiteral(".part")), qPrintable(kept.error().message));
        QCOMPARE(readAll(lf), QByteArray("old"));
        QCOMPARE(readAll(lf + QStringLiteral(".part")), QByteArray("new"));
        QCOMPARE(QDir(locked).entryList(QDir::Files, QDir::Name), (QStringList{QStringLiteral("x.bin"), QStringLiteral("x.bin.part")}));
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
            {
                auto o2 = o;
                o2.extraListNames = {QStringLiteral("..\\..\\evil.bin"), QStringLiteral("../up.bin"), QStringLiteral("x\\y.bin")};
                FakeFtpServer srv2(o2);
                FtpClient c2(quick());
                QVERIFY(c2.open(QStringLiteral("127.0.0.1"), srv2.port()).ok());
                auto rm = c2.removeTree(QStringLiteral("/games"));
                COMPARE_ERROR_CODE(rm, ErrorCode::InvalidArgument);
                QVERIFY(rm.error().message.contains(QStringLiteral("evil.bin")));
                for (const QString& cmd : srv2.commands())
                    QVERIFY2(!cmd.contains(QStringLiteral("evil")) && !cmd.contains(QStringLiteral("up.bin")) && !cmd.contains(QStringLiteral("y.bin")),
                             qPrintable(cmd));
                QVERIFY(fwtest::writeTextFile(QDir(root).filePath(QStringLiteral("games/good.bin")), "good"));
            }
            // A name with ':' stays on the console, where it is an ordinary
            // character: it is deleted like any other entry (this fake has no
            // such file, so the delete itself is what fails).
            auto rm = c.removeTree(QStringLiteral("/games"));
            QVERIFY(!rm.ok());
            QVERIFY(srv.sawCommand(QStringLiteral("DELE /games/good.bin:ads")));
            for (const QString& cmd : srv.commands())
                QVERIFY2(!cmd.startsWith(QStringLiteral("DELE")) || !cmd.contains(QStringLiteral("evil")), qPrintable(cmd));
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
        QVERIFY(job.ok());
        ProgressReporter progress;
        progress.setPhases(job.value()->phases());
        JobContext ctx;
        ctx.progress = &progress;
        ctx.settings = s;
        ctx.log = Log(QStringLiteral("test"));
        auto out = job.value()->run(ctx);
        QVERIFY(out.ok());
        QCOMPARE(out->details.value(QStringLiteral("skippedNames")).toArray().size(), 1);
        QVERIFY(out->warnings.join(u' ').contains(QStringLiteral("Not downloaded: ")));
        // Something was left out: not a clean "Passed".
        QCOMPARE(out->verification, Verification::Inconclusive);
        QVERIFY2(out->verificationDetail.contains(QStringLiteral("1 entry was left out")), qPrintable(out->verificationDetail));
        QCOMPARE(readAll(QDir(dl).filePath(QStringLiteral("g/ok.bin"))), QByteArray("ok"));

        // Every source left out: the job fails instead of reporting 0 files as passed.
        auto none = mod.jobs.create(jobkinds::kDownload, {{QStringLiteral("sources"), QJsonArray{QStringLiteral("/g/a:b.bin")}},
                                                          {QStringLiteral("localDir"), fresh(QStringLiteral("names-job-none"))}});
        QVERIFY(none.ok());
        auto nout = none.value()->run(ctx);
        QVERIFY(!nout.ok());
        QVERIFY2(nout.error().message.startsWith(QStringLiteral("Not downloaded: ")), qPrintable(nout.error().message));
    }

    // ---- busy servers, late replies, peers that are not FTP ---------------------------------
    void passiveModeHiccupDoesNotSwitchToEpsv()
    {
        const QString root = fresh(QStringLiteral("pasv421"));
        QVERIFY(fwtest::writeTextFile(QDir(root).filePath(QStringLiteral("a.txt")), "a"));
        auto o = server(root);
        o.failFirstPasvReply = "421 Too many connections.";
        FakeFtpServer srv(o);
        FtpClient c(quick());
        QVERIFY(c.open(QStringLiteral("127.0.0.1"), srv.port()).ok());
        // A busy answer to PASV is an error for this transfer, not "PASV is unknown here".
        auto first = c.list(QStringLiteral("/"));
        QVERIFY(!first.ok());
        QCOMPARE(c.lastReply().code, 421);
        auto second = c.list(QStringLiteral("/"));
        QVERIFY2(second.ok(), second.ok() ? "" : qPrintable(second.error().toLogString()));
        QCOMPARE(int(second->size()), 1);
        QVERIFY(!srv.sawCommand(QStringLiteral("EPSV")));

        // A server without PASV still gets EPSV, and a new connection asks PASV again.
        auto o2 = server(root);
        o2.refusePasv = true;
        FakeFtpServer srv2(o2);
        FtpClient c2(quick());
        QVERIFY(c2.open(QStringLiteral("127.0.0.1"), srv2.port()).ok());
        QVERIFY(c2.list(QStringLiteral("/")).ok());
        QVERIFY(c2.list(QStringLiteral("/")).ok());
        QVERIFY(c2.open(QStringLiteral("127.0.0.1"), srv2.port()).ok());
        QVERIFY(c2.list(QStringLiteral("/")).ok());
        int pasv = 0;
        for (const QString& cmd : srv2.commands())
            pasv += cmd.startsWith(QStringLiteral("PASV")) ? 1 : 0;
        QCOMPARE(pasv, 2); // once per connection, not once per listing
    }

    void lateListingReplyDropsTheConnection_data()
    {
        QTest::addColumn<bool>("mlsd");
        QTest::newRow("MLSD") << true;
        QTest::newRow("LIST") << false;
    }

    void lateListingReplyDropsTheConnection()
    {
        QFETCH(bool, mlsd);
        const QString root = fresh(QStringLiteral("late"));
        QVERIFY(fwtest::writeTextFile(QDir(root).filePath(QStringLiteral("d/a.txt")), "a"));
        auto o = server(root);
        o.mlsd = mlsd;
        o.lateListReplyMs = 1500;
        FakeFtpServer srv(o);
        FtpOptions fo = quick();
        fo.idleTimeoutMs = 300;
        FtpClient c(fo);
        QVERIFY(c.open(QStringLiteral("127.0.0.1"), srv.port()).ok());
        auto r = c.list(QStringLiteral("/d"));
        QVERIFY(!r.ok());
        COMPARE_ERROR_CODE(r, ErrorCode::Timeout);
        // The 226 is still owed: kept, it would be read as the next command's answer.
        QVERIFY(!c.isConnected());
        QVERIFY(c.open(QStringLiteral("127.0.0.1"), srv.port()).ok());
        auto again = c.list(QStringLiteral("/d"));
        QVERIFY2(again.ok(), again.ok() ? "" : qPrintable(again.error().toLogString()));
        QCOMPARE(int(again->size()), 1);
        QVERIFY(c.noop().ok());
    }

    void repliesThatAreNotFtpAreCutOff()
    {
        // Something that is not an FTP server streams bytes without a line end.
        fake::FakeLineServer flood({{{0, QByteArray(200 * 1024, 'x')}}}, false);
        FtpOptions patient = quick();
        patient.replyTimeoutMs = 30000;
        FtpClient c(patient);
        QElapsedTimer t;
        t.start();
        auto st = c.connectToHost(QStringLiteral("127.0.0.1"), flood.port());
        QVERIFY(!st.ok());
        COMPARE_ERROR_CODE(st, ErrorCode::CorruptData);
        QVERIFY(t.elapsed() < 15000); // cut off, well before the reply timeout
        QVERIFY(!c.isConnected());
        // Lines without a reply code, on and on.
        QByteArray chatter;
        for (int i = 0; i < 4000; ++i)
            chatter += "hello there, this is not ftp at all\r\n";
        fake::FakeLineServer talk({{{0, chatter}}}, false);
        auto st2 = c.connectToHost(QStringLiteral("127.0.0.1"), talk.port());
        QVERIFY(!st2.ok());
        COMPARE_ERROR_CODE(st2, ErrorCode::CorruptData);
    }

    void uploadReplaceSurvivesALostConnectionAndShortWrites()
    {
        // The connection dies between "old file set aside" and "new file renamed".
        const QString root = fresh(QStringLiteral("swaplost"));
        const QString cfg = QDir(root).filePath(QStringLiteral("cfg/config.ini"));
        QVERIFY(fwtest::writeTextFile(cfg, "old"));
        auto o = server(root);
        o.dropControlOnRnfr = 2;
        FakeFtpServer srv(o);
        FtpClient c(quick());
        QVERIFY(c.open(QStringLiteral("127.0.0.1"), srv.port()).ok());
        auto st = c.uploadReplace(QStringLiteral("/cfg/config.ini"), "new contents", QStringLiteral(".new"));
        QVERIFY(!st.ok());
        // Not "refused …; the old file was kept": it says where the files are.
        QVERIFY(st.error().code != ErrorCode::Unsupported);
        QVERIFY2(st.error().message.contains(QStringLiteral("/cfg/config.ini.replacing.bak")), qPrintable(st.error().message));
        QVERIFY2(st.error().message.contains(QStringLiteral("/cfg/config.ini.new")), qPrintable(st.error().message));
        QVERIFY(!QFileInfo::exists(cfg));
        QCOMPARE(readAll(cfg + QStringLiteral(".replacing.bak")), QByteArray("old"));
        // The next replace puts the old file back first and then swaps cleanly.
        QVERIFY(c.open(QStringLiteral("127.0.0.1"), srv.port()).ok());
        QVERIFY(c.uploadReplace(QStringLiteral("/cfg/config.ini"), "newer contents", QStringLiteral(".new")).ok());
        QCOMPARE(readAll(cfg), QByteArray("newer contents"));
        QCOMPARE(QDir(QFileInfo(cfg).absolutePath()).entryList(QDir::Files), QStringList{QStringLiteral("config.ini")});

        // A server that answers 226 for a short write must not replace the good file.
        const QString root2 = fresh(QStringLiteral("swapshort"));
        const QString cfg2 = QDir(root2).filePath(QStringLiteral("cfg/config.ini"));
        QVERIFY(fwtest::writeTextFile(cfg2, "keep me"));
        auto o2 = server(root2);
        o2.corruptStorPath = QStringLiteral("/cfg/config.ini.new");
        FakeFtpServer srv2(o2);
        FtpClient c2(quick());
        QVERIFY(c2.open(QStringLiteral("127.0.0.1"), srv2.port()).ok());
        auto cut = c2.uploadReplace(QStringLiteral("/cfg/config.ini"), QByteArray(4000, 'n'), QStringLiteral(".new"));
        QVERIFY(!cut.ok());
        COMPARE_ERROR_CODE(cut, ErrorCode::IoError);
        QCOMPARE(readAll(cfg2), QByteArray("keep me"));
        QCOMPARE(QDir(QFileInfo(cfg2).absolutePath()).entryList(QDir::Files), QStringList{QStringLiteral("config.ini")});
    }

    // The server takes the first rename (old file set aside) and refuses the
    // second (new file into place): the old file must be back under its name.
    void uploadReplacePutsTheOldFileBackWhenTheSwapIsRefused()
    {
        const QString root = fresh(QStringLiteral("swapback"));
        const QString cfg = QDir(root).filePath(QStringLiteral("cfg/config.ini"));
        QVERIFY(fwtest::writeTextFile(cfg, "old"));
        auto o = server(root);
        o.refuseRenameFromPaths = {QStringLiteral("/cfg/config.ini.new")};
        FakeFtpServer srv(o);
        FtpClient c(quick());
        QVERIFY(c.open(QStringLiteral("127.0.0.1"), srv.port()).ok());
        auto st = c.uploadReplace(QStringLiteral("/cfg/config.ini"), "new contents", QStringLiteral(".new"));
        COMPARE_ERROR_CODE(st, ErrorCode::Unsupported);
        QVERIFY2(st.error().message.contains(QStringLiteral("the old file was kept")), qPrintable(st.error().message));
        QVERIFY(c.isConnected());
        QVERIFY(srv.sawCommand(QStringLiteral("RNTO /cfg/config.ini.replacing.bak"))); // it really was set aside …
        QCOMPARE(readAll(cfg), QByteArray("old"));                                      // … and is back
        QCOMPARE(QDir(QFileInfo(cfg).absolutePath()).entryList(QDir::Files), QStringList{QStringLiteral("config.ini")});

        // The old file cannot be put back either: it is not deleted, and the
        // message says under which name it is now.
        const QString root2 = fresh(QStringLiteral("swapstuck"));
        const QString cfg2 = QDir(root2).filePath(QStringLiteral("cfg/config.ini"));
        QVERIFY(fwtest::writeTextFile(cfg2, "old"));
        auto o2 = server(root2);
        o2.refuseRenameFromPaths = {QStringLiteral("/cfg/config.ini.new"), QStringLiteral("/cfg/config.ini.replacing.bak")};
        FakeFtpServer srv2(o2);
        FtpClient c2(quick());
        QVERIFY(c2.open(QStringLiteral("127.0.0.1"), srv2.port()).ok());
        auto stuck = c2.uploadReplace(QStringLiteral("/cfg/config.ini"), "new contents", QStringLiteral(".new"));
        QVERIFY(!stuck.ok());
        QVERIFY2(stuck.error().message.contains(QStringLiteral("/cfg/config.ini.replacing.bak")), qPrintable(stuck.error().message));
        QCOMPARE(readAll(cfg2 + QStringLiteral(".replacing.bak")), QByteArray("old"));
        QVERIFY(!QFileInfo::exists(cfg2));
    }

    void listNamesKeepLeadingSpacesAndColonsStayOnTheConsole()
    {
        const QDateTime now(QDate(2025, 1, 15), QTime(12, 0), QTimeZone::utc());
        // One separator before the name; further spaces belong to it.
        auto e = parseListLine(QStringLiteral("-rw-r--r-- 1 root root 5 Jan 01 12:00  save.bin"), now);
        QVERIFY(e);
        QCOMPARE(e->name, QStringLiteral(" save.bin"));
        auto plain = parseListLine(QStringLiteral("-rw-r--r-- 1 root root 5 Jan  1  2024 save.bin"), now);
        QVERIFY(plain);
        QCOMPARE(plain->name, QStringLiteral("save.bin"));
        // 29 February without a year: the last year that had one.
        auto leap = parseListLine(QStringLiteral("-rw-r--r-- 1 root root 5 Feb 29 08:30 leap.bin"), now);
        QVERIFY(leap);
        QCOMPARE(leap->modified.date(), QDate(2024, 2, 29));

        // ':' is an ordinary character on the console: allowed where the name stays there.
        QCOMPARE(*listedChild(QStringLiteral("/dumps"), QStringLiteral("Save: slot 1.dat")), QStringLiteral("/dumps/Save: slot 1.dat"));
        QVERIFY(listedNameProblem(QStringLiteral("Save: slot 1.dat")).isEmpty());
        for (const char* bad : {"", ".", "..", "a/b", "..\\x", "a\\b", "line\nbreak"})
            QVERIFY2(!listedChild(QStringLiteral("/dumps"), QString::fromLatin1(bad)), bad);
        // Never for a name that reaches this PC.
        QVERIFY(!remoteChild(QStringLiteral("/dumps"), QStringLiteral("Save: slot 1.dat")));
        QVERIFY(!localNameProblem(QStringLiteral("Save: slot 1.dat")).isEmpty());
    }

    void sourcesKeepTheirSpacesAndDuplicateTargetsAreLeftOut()
    {
        Module mod;
        const QString root = fresh(QStringLiteral("dups"));
        FakeFtpServer srv(server(root));
        const SettingsValues s(mod.schema, {{keys::kConsoleIp, QStringLiteral("127.0.0.1")}, {keys::kFtpPort, srv.port()}}, {});
        const QString local = fresh(QStringLiteral("dupslocal"));
        // A name that begins with a space is a different file: it is not trimmed away.
        const QString spaced = QDir(local).filePath(QStringLiteral(" lead.txt"));
        QVERIFY(fwtest::writeTextFile(spaced, "spaced"));
        auto up = mod.jobs.create(jobkinds::kUpload, {{QStringLiteral("sources"), QJsonArray{spaced, QStringLiteral("   ")}},
                                                      {QStringLiteral("remoteDir"), QStringLiteral("/up")}});
        QVERIFY(up.ok());
        QVERIFY2(up.value()->validate(s).ok(), "a source with a leading space is found");
        ProgressReporter progress;
        progress.setPhases(up.value()->phases());
        JobContext ctx;
        ctx.progress = &progress;
        ctx.settings = s;
        ctx.log = Log(QStringLiteral("test"));
        auto uout = up.value()->run(ctx);
        QVERIFY2(uout.ok(), uout.ok() ? "" : qPrintable(uout.error().toLogString()));
        QCOMPARE(readAll(srv.path(QStringLiteral("/up/ lead.txt"))), QByteArray("spaced"));

        // Two sources for one target: the first is sent, the second reported.
        const QString a = QDir(local).filePath(QStringLiteral("a/x.bin")), b = QDir(local).filePath(QStringLiteral("b/x.bin"));
        QVERIFY(fwtest::writeTextFile(a, "from a"));
        QVERIFY(fwtest::writeTextFile(b, "from b!"));
        auto plan = planUpload({a, b}, QStringLiteral("/up"));
        QVERIFY(plan.ok());
        QCOMPARE(int(plan->files.size()), 1);
        QCOMPARE(plan->totalBytes, qint64(6));
        QCOMPARE(plan->skipped.size(), 1);
        QVERIFY(plan->skipped.front().contains(QStringLiteral("/up/x.bin")));
        auto both = mod.jobs.create(jobkinds::kUpload, {{QStringLiteral("sources"), QJsonArray{a, b}}, {QStringLiteral("remoteDir"), QStringLiteral("/up")}});
        QVERIFY(both.ok());
        auto bout = both.value()->run(ctx);
        QVERIFY(bout.ok());
        QCOMPARE(readAll(srv.path(QStringLiteral("/up/x.bin"))), QByteArray("from a"));
        QCOMPARE(bout->verification, Verification::Inconclusive);
        QVERIFY(bout->warnings.join(u' ').contains(QStringLiteral("Not uploaded: ")));

        // Download: two remote names that are one name on this PC.
        FtpClient c(quick());
        QVERIFY(c.open(QStringLiteral("127.0.0.1"), srv.port()).ok());
        auto dplan = planDownload(c, {QStringLiteral("/up/x.bin"), QStringLiteral("/up/x.bin")}, fresh(QStringLiteral("dupsdl")));
        QVERIFY(dplan.ok());
        QCOMPARE(int(dplan->files.size()), 1);
        QCOMPARE(dplan->skipped.size(), 1);
    }

    void folderUploadSendsOneMkdPerFolderAndSummarisesSkips()
    {
        const QString root = fresh(QStringLiteral("mkd"));
        const QString local = fresh(QStringLiteral("mkdlocal"));
        const QString deep = QDir(local).filePath(QStringLiteral("deep"));
        for (int i = 0; i < 25; ++i)
            QVERIFY(fwtest::writeRandomFile(QDir(deep).filePath(QStringLiteral("l1/l2/%1f%2.bin").arg(i % 2 ? QStringLiteral("l3/") : QString()).arg(i)),
                                            500 + i, quint32(60 + i)));
        FakeFtpServer srv(server(root));
        TransferOptions opt;
        opt.endpoint = endpoint(srv.port());
        auto plan = planUpload({deep}, QStringLiteral("/up"));
        QVERIFY(plan.ok());
        auto rep = runUpload(plan.value(), opt, {});
        QVERIFY(rep.ok());
        QCOMPARE(rep->transferred, 25);
        // /up, /up/deep, …/l1, …/l2, …/l3: one MKD each, not one per descendant.
        QStringList mkds;
        for (const QString& cmd : srv.commands())
            if (cmd.startsWith(QStringLiteral("MKD ")))
                mkds << cmd.mid(4);
        mkds.sort();
        QCOMPARE(mkds, (QStringList{QStringLiteral("/up"), QStringLiteral("/up/deep"), QStringLiteral("/up/deep/l1"),
                                    QStringLiteral("/up/deep/l1/l2"), QStringLiteral("/up/deep/l1/l2/l3")}));
        // A resume run over files that are all there: 25 skips, 20 warnings and one summary line.
        opt.resume = true;
        auto again = runUpload(plan.value(), opt, {});
        QVERIFY(again.ok());
        QCOMPARE(again->skipped, 25);
        QCOMPARE(again->warnings.size(), 21);
        QVERIFY2(again->warnings.last().startsWith(QStringLiteral("5 more file(s)")), qPrintable(again->warnings.last()));
    }

    void skippedAndFailedFilesCountInTheProgress()
    {
        const QString root = fresh(QStringLiteral("count"));
        const QString local = fresh(QStringLiteral("countlocal"));
        QStringList sources;
        for (int i = 0; i < 4; ++i) {
            const QString f = QDir(local).filePath(QStringLiteral("f%1.bin").arg(i));
            QVERIFY(fwtest::writeRandomFile(f, 2000 + i, quint32(30 + i)));
            sources << f;
        }
        QVERIFY(fwtest::writeTextFile(QDir(root).filePath(QStringLiteral("up/f3.bin")), "locked"));
        auto o = server(root);
        o.readOnlyPaths = {QStringLiteral("/up/f3.bin")}; // one file fails
        FakeFtpServer srv(o);
        TransferOptions opt;
        opt.endpoint = endpoint(srv.port());
        opt.retries = 0;
        auto plan = planUpload(sources, QStringLiteral("/up"));
        QVERIFY(plan.ok());
        {
            ProgressReporter progress;
            progress.setPhases({QStringLiteral("Uploading")});
            progress.beginPhase(0, plan->totalBytes, qint64(plan->files.size()));
            auto rep = runUpload(plan.value(), opt, {}, &progress);
            QVERIFY(rep.ok());
            QCOMPARE(rep->transferred, 3);
            QCOMPARE(rep->failed, 1);
            QCOMPARE(progress.snapshot().filesDone, qint64(4));
            // Failures are named by their console path, not a bare file name.
            QVERIFY2(rep->errors.front().startsWith(QStringLiteral("/up/f3.bin: ")), qPrintable(rep->errors.front()));
        }
        // A second run with resume: three files are already there and are skipped.
        opt.resume = true;
        ProgressReporter progress;
        progress.setPhases({QStringLiteral("Uploading")});
        progress.beginPhase(0, plan->totalBytes, qint64(plan->files.size()));
        auto rep = runUpload(plan.value(), opt, {}, &progress);
        QVERIFY(rep.ok());
        QCOMPARE(rep->skipped, 3);
        QCOMPARE(progress.snapshot().filesDone, qint64(4));
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
        QVERIFY(prot.ok());
        auto pv = prot.value()->validate(s);
        COMPARE_ERROR_CODE(pv, ErrorCode::PermissionDenied);
        QVERIFY(pv.error().message.contains(QStringLiteral("brick")));
        auto allowed = mod.jobs.create(jobkinds::kUpload, {{QStringLiteral("sources"), QJsonArray{src}},
                                                           {QStringLiteral("remoteDir"), QStringLiteral("/system/x")},
                                                           {QStringLiteral("allowProtectedPaths"), true}});
        QVERIFY(allowed.ok());
        QVERIFY(allowed.value()->validate(s).ok());
        // So does a folder whose own name makes it land on a protected root,
        // and the parent of one.
        const QString sysEx = QDir(fresh(QStringLiteral("jobsprot"))).filePath(QStringLiteral("system_ex"));
        QVERIFY(fwtest::writeTextFile(QDir(sysEx).filePath(QStringLiteral("x.bin")), "x"));
        for (const auto& [sources, dir] : {std::pair{QJsonArray{sysEx}, QStringLiteral("/")}, std::pair{QJsonArray{src}, QStringLiteral("/user")},
                                           std::pair{QJsonArray{src, sysEx}, QStringLiteral("/")}}) {
            auto named = mod.jobs.create(jobkinds::kUpload, {{QStringLiteral("sources"), sources}, {QStringLiteral("remoteDir"), dir}});
            QVERIFY(named.ok());
            auto nv = named.value()->validate(s);
            QVERIFY2(!nv.ok(), qPrintable(dir));
            COMPARE_ERROR_CODE(nv, ErrorCode::PermissionDenied);
            QVERIFY(nv.error().message.contains(dir == u"/" ? QStringLiteral("/system_ex") : dir));
            auto confirmed = mod.jobs.create(jobkinds::kUpload, {{QStringLiteral("sources"), sources},
                                                                 {QStringLiteral("remoteDir"), dir},
                                                                 {QStringLiteral("allowProtectedPaths"), true}});
            QVERIFY(confirmed.ok());
            QVERIFY(confirmed.value()->validate(s).ok());
        }
        auto plainRoot = mod.jobs.create(jobkinds::kUpload, {{QStringLiteral("sources"), QJsonArray{src}}, {QStringLiteral("remoteDir"), QStringLiteral("/")}});
        QVERIFY(plainRoot.ok());
        QVERIFY(plainRoot.value()->validate(s).ok());

        const QString dl = fresh(QStringLiteral("jobsdl"));
        auto down = mod.jobs.create(jobkinds::kDownload, {{QStringLiteral("sources"), QJsonArray{QStringLiteral("/data/etaHEN/games/Game")}},
                                                          {QStringLiteral("localDir"), dl}});
        QVERIFY(down.ok());
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
        COMPARE_VALUE(store.importLegacy(legacy), 7);
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
        for (const QString& key : {keys::kPayloadPort, keys::kSmpApiPort, keys::kFtpPort, keys::kKlogPort, keys::kDpiPort})
            QVERIFY2(mod.schema->find(key), qPrintable(key));
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
        COMPARE_VALUE(imported.importLegacy(legacy), 1);
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
        QVERIFY(QDir().mkpath(root + QStringLiteral("/data/etaHEN/games")));
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
        QVERIFY(feats);
        QVERIFY(feats->ftpsrv && feats->mlsd && feats->rest && feats->size); // FEAT lists only MLSD KILL MTRW SELF

        // Upload with the size check: the file stays, byte for byte.
        TransferOptions opt;
        opt.endpoint = endpoint(srv.port());
        auto repPlan = planUpload({eboot}, QStringLiteral("/data/etaHEN/games"));
        QVERIFY(repPlan.ok());
        auto rep = runUpload(repPlan.value(), opt, {});
        QVERIFY(rep.ok());
        QCOMPARE(rep->failed, 0);
        QCOMPARE(rep->verified, 1);
        QCOMPARE(readAll(srv.path(QStringLiteral("/data/etaHEN/games/eboot.bin"))), self);
        COMPARE_VALUE(c.size(QStringLiteral("/data/etaHEN/games/eboot.bin")), qint64(self.size()));
        COMPARE_VALUE(c.download(QStringLiteral("/data/etaHEN/games/eboot.bin")), self); // raw bytes, not the ELF

        // No NLST on ftpsrv: existence comes from the folder listing.
        QVERIFY(c.fileExists(QStringLiteral("/data/etaHEN/games/eboot.bin")));
        QVERIFY(!c.fileExists(QStringLiteral("/data/etaHEN/games/none.bin")));
        QVERIFY(!c.fileExists(QStringLiteral("/data/missing/none.bin")));
        COMPARE_VALUE(c.listedSize(QStringLiteral("/data/etaHEN/games/eboot.bin")), qint64(self.size()));
        COMPARE_VALUE(c.pwd(), QStringLiteral("/")); // the listing did not move the working folder
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
        QVERIFY(QDir().mkpath(root + QStringLiteral("/up")));
        FakeFtpServer srv(o);
        FtpClient c(quick());
        QVERIFY(c.open(QStringLiteral("127.0.0.1"), srv.port()).ok());
        QCOMPARE(c.selfMode(), SelfMode::StillEnabled);
        TransferOptions opt;
        opt.endpoint = endpoint(srv.port());
        auto repPlan = planUpload({lib}, QStringLiteral("/up"));
        QVERIFY(repPlan.ok());
        auto rep = runUpload(repPlan.value(), opt, {});
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

