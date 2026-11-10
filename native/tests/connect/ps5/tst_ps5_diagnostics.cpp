// "Find my PS5" (the /24 sweep), FTP diagnostics, the speed test and the
// ps5.ftp.diagnose job, against in-process fakes on 127.0.0.1 (never a real
// console).

#include "FakePs5.h"
#include "Ps5TestUtil.h"

#include "connect/ps5/Discovery.h"
#include "connect/ps5/Endpoint.h"
#include "connect/ps5/FtpDiagnostics.h"
#include "connect/ps5/Module.h"
#include "core/Progress.h"
#include "support/ClosedPort.h"
#include "support/TestSupport.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <QtConcurrent/QtConcurrentRun>

#include <memory>

using namespace fw;
using namespace fw::ps5;
using fake::FakeFtpServer;

namespace {

constexpr qint64 kEpoch = 1700000000;

ConsoleEndpoint endpoint(quint16 port)
{
    ConsoleEndpoint ep;
    ep.host = QStringLiteral("127.0.0.1");
    ep.ftpPort = port;
    ep.connectTimeoutMs = 3000;
    ep.idleTimeoutMs = 5000;
    return ep;
}

DiagnoseOptions options(const QString& folder, bool speed = false, qint64 bytes = 256 * 1024)
{
    DiagnoseOptions o;
    o.folder = folder;
    o.speedTest = speed;
    o.speedTestBytes = bytes;
    o.epochSeconds = kEpoch;
    return o;
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

const QString kGames = QStringLiteral("/data/etaHEN/games");
const QString kProbe = QStringLiteral("/data/etaHEN/games/.exfatbuilder_wtest_1700000000.tmp");
const QString kSpeed = QStringLiteral("/data/etaHEN/games/.exfatbuilder_speedtest_1700000000.bin");

} // namespace

class TstPs5Diagnostics : public QObject {
    Q_OBJECT
    QTemporaryDir m_tmp;
    int m_n = 0;

    // A fake console root with the games folder.
    QString freshRoot()
    {
        const QString p = m_tmp.filePath(QStringLiteral("root-%1").arg(++m_n));
        QDir().mkpath(p + kGames);
        return p;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_tmp.isValid());
        fwtest::isolateAppData(m_tmp);
    }

    // ---- Find my PS5 --------------------------------------------------------------------

    void subnetsSkipLoopbackAndLinkLocal()
    {
        const QStringList p = subnetPrefixesFor({QHostAddress(QStringLiteral("127.0.0.1")), QHostAddress(QStringLiteral("169.254.3.4")),
                                                 QHostAddress(QStringLiteral("192.168.1.20")), QHostAddress(QStringLiteral("192.168.1.30")),
                                                 QHostAddress(QStringLiteral("10.0.0.5")), QHostAddress(QStringLiteral("::1")),
                                                 QHostAddress(QStringLiteral("fe80::1")), QHostAddress(QStringLiteral("0.0.0.0")),
                                                 QHostAddress(QStringLiteral("224.0.0.1"))});
        QCOMPARE(p, (QStringList{QStringLiteral("169.254.3"), QStringLiteral("192.168.1"), QStringLiteral("10.0.0")}));
        // Only private ranges are ever swept: not a public address, a carrier
        // (100.64/10) or campus network, or the neighbours of 172.16/12.
        for (const char* a : {"8.8.8.8", "100.64.1.1", "172.15.0.1", "172.32.0.1", "192.167.1.1", "11.0.0.1", "169.253.1.1", "255.255.255.255", "::1"})
            QVERIFY2(!isPrivateLanAddress(QHostAddress(QString::fromLatin1(a))), a);
        for (const char* a : {"10.255.0.1", "172.16.5.5", "172.31.9.9", "192.168.0.1", "169.254.7.7"})
            QVERIFY2(isPrivateLanAddress(QHostAddress(QString::fromLatin1(a))), a);
        QCOMPARE(subnetPrefixesFor({QHostAddress(QStringLiteral("8.8.8.8")), QHostAddress(QStringLiteral("100.64.1.1")),
                                    QHostAddress(QStringLiteral("172.20.1.9"))}),
                 QStringList{QStringLiteral("172.20.1")});
        const QStringList hosts = subnetHosts({QStringLiteral("192.168.1")}, {QStringLiteral("192.168.1.20")});
        QCOMPARE(hosts.size(), 253);
        QCOMPARE(hosts.first(), QStringLiteral("192.168.1.1"));
        QCOMPARE(hosts.last(), QStringLiteral("192.168.1.254"));
        QVERIFY(!hosts.contains(QStringLiteral("192.168.1.20")));
        // Whatever this machine has: never loopback, always a private range.
        // (The rules themselves are checked with fixed addresses above; a PC
        // without a LAN address has nothing to add here.)
        const QStringList own = localSubnetPrefixes();
        for (const QString& prefix : own) {
            QVERIFY2(!prefix.startsWith(QStringLiteral("127.")), qPrintable(prefix));
            QVERIFY2(isPrivateLanAddress(QHostAddress(prefix + QStringLiteral(".1"))), qPrintable(prefix));
        }
        if (own.isEmpty())
            qInfo("This PC has no LAN address: its own network prefixes were not checked.");
    }

    void discoveryFindsListenersOnInjectedHosts()
    {
        const QString root = freshRoot();
        FakeFtpServer::Options greet;
        greet.root = root;
        greet.greeting = "220 ftpsrv 1.2 for PS5 ready";
        FakeFtpServer withBanner(greet);
        FakeFtpServer::Options quiet;
        quiet.root = root;
        quiet.silentGreeting = true;
        FakeFtpServer silent(quiet);
        const fwtest::ClosedPort closed;
        QVERIFY(closed.isValid());

        DiscoveryOptions o;
        // The second host has nothing open and is loopback as well (all of
        // 127/8 is, and the fakes listen on 127.0.0.1 only): no packet leaves
        // this PC.
        o.hosts = {QStringLiteral("127.0.0.1"), QStringLiteral("127.0.0.2")};
        o.ports = {closed.port(), withBanner.port(), silent.port()};
        o.connectTimeoutMs = 300;
        o.bannerWaitMs = 1000;
        int lastDone = -1, lastTotal = -1, lastFound = -1;
        bool wentBack = false;
        auto r = discoverConsoles(o, {}, [&](int done, int total, int found) {
            wentBack = wentBack || done < lastDone;
            lastDone = done;
            lastTotal = total;
            lastFound = found;
        });
        QVERIFY2(r.ok(), r.ok() ? "" : qPrintable(r.error().toLogString()));
        QVERIFY(!wentBack);
        QCOMPARE(int(r->size()), 1);
        const DiscoveredConsole& c = r->front();
        QCOMPARE(c.host, QStringLiteral("127.0.0.1"));
        QCOMPARE(c.openPorts, (QList<quint16>{withBanner.port(), silent.port()}));
        QCOMPARE(c.port, withBanner.port()); // first open port in the probe order
        QCOMPARE(c.banner, QStringLiteral("ftpsrv 1.2 for PS5 ready"));
        QVERIFY(c.rttMs >= 0);
        QCOMPARE(lastDone, 2);
        QCOMPARE(lastTotal, 2);
        QCOMPARE(lastFound, 1);

        // A port that accepts but never greets: found, without a banner.
        o.ports = {silent.port()};
        o.bannerWaitMs = 200;
        auto quietOnly = discoverConsoles(o);
        QVERIFY(quietOnly.ok());
        QCOMPARE(int(quietOnly->size()), 1);
        QVERIFY(quietOnly->front().banner.isEmpty());

        // Nothing listening: an empty result, not an error.
        o.ports = {closed.port()};
        auto none = discoverConsoles(o);
        QVERIFY(none.ok());
        QVERIFY(none->empty());

        o.ports.clear();
        COMPARE_ERROR_CODE(discoverConsoles(o), ErrorCode::InvalidArgument);
    }

    void discoveryIsCancellable()
    {
        FakeFtpServer::Options quiet;
        quiet.root = freshRoot();
        quiet.silentGreeting = true;
        FakeFtpServer silent(quiet);
        DiscoveryOptions o;
        o.hosts = {QStringLiteral("127.0.0.1")};
        o.ports = {silent.port()};
        o.bannerWaitMs = 30000; // would wait 30 s for a greeting
        CancellationSource pre;
        pre.cancel();
        const auto before = discoverConsoles(o, pre.token());
        QVERIFY(!before.ok() && before.error().isCancelled());

        CancellationSource src;
        QElapsedTimer t;
        t.start();
        auto future = QtConcurrent::run([o, token = src.token()] { return discoverConsoles(o, token); });
        QTest::qWait(300);
        src.cancel();
        future.waitForFinished();
        QVERIFY(t.elapsed() < 15000); // stopped by the cancel, not by the 30 s wait
        const auto cancelled = future.result();
        QVERIFY(!cancelled.ok());
        QVERIFY(cancelled.error().isCancelled());
    }

    // ---- diagnostics ----------------------------------------------------------------------

    void serverClassification()
    {
        FtpFeatures f;
        QCOMPARE(classifyFtpServer(f, QStringLiteral("FakePS5 FTP ready")).label, QStringLiteral("Unknown / generic FTP"));
        const QStringList generic = classifyFtpServer(f, {}).evidence;
        QVERIFY(!generic.isEmpty() && generic.first().startsWith(QStringLiteral("No zftpd")));
        f.xcrypt = true;
        auto likely = classifyFtpServer(f, {});
        QCOMPARE(likely.label, QStringLiteral("zftpd"));
        QCOMPARE(likely.confidence, QStringLiteral("likely"));
        f.copy = true;
        QCOMPARE(classifyFtpServer(f, {}).confidence, QStringLiteral("very likely"));
        QCOMPARE(classifyFtpServer({}, QStringLiteral("zftpd 2.0")).confidence, QStringLiteral("likely"));
        // ftpsrv (research §4.1): the banner with its version, and FEAT with KILL/MTRW/SELF.
        FtpFeatures ftpsrv;
        ftpsrv.ftpsrv = true;
        const auto fs = classifyFtpServer(ftpsrv, QStringLiteral("Welcome to ftpsrv.elf running on pid 77 Version: v0.21.1 (built x) Service is ready"));
        QCOMPARE(fs.label, QStringLiteral("ftpsrv v0.21.1"));
        QCOMPARE(fs.confidence, QStringLiteral("very likely"));
        QCOMPARE(classifyFtpServer(ftpsrv, {}).label, QStringLiteral("ftpsrv"));
        QCOMPARE(classifyFtpServer({}, QStringLiteral("Service ready for new user.")).label, QStringLiteral("etaHEN FTP"));
    }

    void diagnosticsPassAgainstTheFakeServer()
    {
        const QString root = freshRoot();
        FakeFtpServer::Options so;
        so.root = root;
        so.mlsd = true;
        so.extraFeatures = " XCRYPT\r\n";
        FakeFtpServer srv(so);
        QStringList steps;
        auto r = diagnoseFtp(endpoint(srv.port()), options(kGames + u'/'), {},
                             [&](DiagnoseStage, const QString& text) { steps << text; });
        QVERIFY2(r.ok(), r.ok() ? "" : qPrintable(r.error().toLogString()));
        const FtpDiagnosis& d = r.value();
        QVERIFY(d.usable());
        QVERIFY(d.reachable);
        QVERIFY(d.rttMs >= 0);
        QCOMPARE(d.banner, QStringLiteral("FakePS5 FTP ready"));
        QVERIFY(d.featOk);
        QVERIFY(d.features.mlsd && d.features.rest && d.features.size && d.features.utf8 && d.features.xcrypt);
        QCOMPARE(d.passive, std::optional<bool>(true));
        QCOMPARE(d.passiveMode, QStringLiteral("PASV"));
        QCOMPARE(d.binary, std::optional<bool>(true));
        QCOMPARE(d.writable, std::optional<bool>(true));
        QVERIFY(d.probeCleaned);
        QCOMPARE(d.folder, kGames);
        QCOMPARE(d.probePath, kProbe);
        QCOMPARE(d.destination, QStringLiteral("Internal PS5 storage"));
        QCOMPARE(d.server.label, QStringLiteral("zftpd")); // XCRYPT + MLSD/UTF8/REST
        QVERIFY(d.error.isEmpty());
        QVERIFY(!d.speed.ran);
        QVERIFY(srv.sawCommand(QStringLiteral("STOR ") + kProbe));
        QVERIFY(srv.sawCommand(QStringLiteral("DELE ") + kProbe));
        QVERIFY(!QFileInfo::exists(srv.path(kProbe)));
        QVERIFY(steps.contains(QStringLiteral("Checking passive mode")));

        // JSON round trip (the job's details).
        const FtpDiagnosis back = FtpDiagnosis::fromJson(d.toJson());
        QCOMPARE(back.toJson(), d.toJson());
        QCOMPARE(back.passive, d.passive);
        QCOMPARE(back.writable, d.writable);
        QVERIFY(!back.speed.readBackMatches.has_value());
    }

    void passiveFallbackAndRefusal()
    {
        const QString root = freshRoot();
        FakeFtpServer::Options so;
        so.root = root;
        so.refusePasv = true;
        {
            FakeFtpServer srv(so);
            auto r = diagnoseFtp(endpoint(srv.port()), options(kGames));
            QVERIFY(r.ok());
            QCOMPARE(r->passive, std::optional<bool>(true));
            QCOMPARE(r->passiveMode, QStringLiteral("EPSV"));
            QCOMPARE(r->writable, std::optional<bool>(true)); // the probe used EPSV
        }
        so.refuseEpsv = true;
        FakeFtpServer none(so);
        auto r = diagnoseFtp(endpoint(none.port()), options(kGames));
        QVERIFY(r.ok());
        QCOMPARE(r->passive, std::optional<bool>(false));
        QCOMPARE(r->writable, std::optional<bool>(false)); // no data connection
        QVERIFY(!r->notes.isEmpty());
        QVERIFY(r->usable()); // reachable and logged in all the same
    }

    void unreachableAndRefusedLogin()
    {
        const fwtest::ClosedPort closed;
        QVERIFY(closed.isValid());
        auto r = diagnoseFtp(endpoint(closed.port()), options(kGames));
        QVERIFY(r.ok());
        QVERIFY(!r->reachable);
        QVERIFY(!r->usable());
        QVERIFY2(r->error.contains(QStringLiteral("127.0.0.1")), qPrintable(r->error));
        QVERIFY(!r->writable.has_value());

        FakeFtpServer::Options so;
        so.root = freshRoot();
        so.requireLogin = true; // anonymous is refused
        FakeFtpServer srv(so);
        auto l = diagnoseFtp(endpoint(srv.port()), options(kGames));
        QVERIFY(l.ok());
        QVERIFY(l->reachable);
        QVERIFY(l->greeted);
        QVERIFY(!l->loggedIn);
        QVERIFY(l->error.contains(QStringLiteral("refused the login")));

        ConsoleEndpoint noHost = endpoint(21);
        noHost.host.clear();
        COMPARE_ERROR_CODE(diagnoseFtp(noHost, options(kGames)), ErrorCode::InvalidArgument);
    }

    void notWritableAndLeftoverProbeAreReported()
    {
        FakeFtpServer::Options so;
        so.root = freshRoot();
        FakeFtpServer srv(so);
        auto missing = diagnoseFtp(endpoint(srv.port()), options(QStringLiteral("/mnt/usb0/games")));
        QVERIFY(missing.ok());
        QCOMPARE(missing->writable, std::optional<bool>(false));
        QVERIFY(missing->writableDetail.startsWith(QStringLiteral("Write denied")));
        QCOMPARE(missing->destination, QStringLiteral("External USB / exFAT"));

        FakeFtpServer::Options keep;
        keep.root = freshRoot();
        keep.refuseDeletePaths = {kProbe};
        FakeFtpServer stuck(keep);
        auto left = diagnoseFtp(endpoint(stuck.port()), options(kGames));
        QVERIFY(left.ok());
        QCOMPARE(left->writable, std::optional<bool>(true));
        QVERIFY(!left->probeCleaned);
        QVERIFY2(left->writableDetail.contains(kProbe), qPrintable(left->writableDetail));
        QVERIFY(QFileInfo::exists(stuck.path(kProbe))); // really left behind, and said so
    }

    void speedTestMeasuresBothWaysAndCleansUp()
    {
        FakeFtpServer::Options so;
        so.root = freshRoot();
        FakeFtpServer srv(so);
        qint64 moved = 0;
        auto r = diagnoseFtp(endpoint(srv.port()), options(kGames, true, kSpeedTestBytes), {}, {},
                             [&](qint64 n) { moved += n; });
        QVERIFY2(r.ok(), r.ok() ? "" : qPrintable(r.error().toLogString()));
        const SpeedTestResult& s = r->speed;
        QVERIFY(s.ran);
        QCOMPARE(s.bytes, kSpeedTestBytes); // the legacy 16 MiB
        QCOMPARE(s.path, kSpeed);
        QVERIFY(s.uploadMBps > 0);
        QVERIFY(s.downloadMBps > 0);
        QCOMPARE(s.readBackMatches, std::optional<bool>(true));
        QVERIFY(s.cleaned);
        QVERIFY(s.error.isEmpty());
        QCOMPARE(moved, 2 * kSpeedTestBytes);
        QVERIFY(srv.sawCommand(QStringLiteral("STOR ") + kSpeed));
        QVERIFY(srv.sawCommand(QStringLiteral("RETR ") + kSpeed));
        QVERIFY(srv.sawCommand(QStringLiteral("DELE ") + kSpeed));
        QVERIFY(!QFileInfo::exists(srv.path(kSpeed)));

        // Not writable: the speed test does not run.
        auto ro = diagnoseFtp(endpoint(srv.port()), options(QStringLiteral("/nope"), true));
        QVERIFY(ro.ok());
        QVERIFY(!ro->speed.ran);
        QVERIFY(!ro->speed.error.isEmpty());
    }

    void speedTestReportsCorruptionAndLeftover()
    {
        FakeFtpServer::Options so;
        so.root = freshRoot();
        so.corruptStorPath = kSpeed; // the server keeps only half the file
        so.refuseDeletePaths = {kSpeed};
        FakeFtpServer srv(so);
        auto r = diagnoseFtp(endpoint(srv.port()), options(kGames, true, 512 * 1024));
        QVERIFY(r.ok());
        QCOMPARE(r->speed.readBackMatches, std::optional<bool>(false));
        QVERIFY(!r->speed.cleaned);
        QVERIFY(QFileInfo::exists(srv.path(kSpeed)));
    }

    void cancelledSpeedTestRemovesItsFile()
    {
        FakeFtpServer::Options so;
        so.root = freshRoot();
        so.throttleBytesPerSec = 1024 * 1024; // 16 MiB would take 16 s
        FakeFtpServer srv(so);
        CancellationSource src;
        auto future = QtConcurrent::run([port = srv.port(), token = src.token()] {
            return diagnoseFtp(endpoint(port), options(kGames, true, kSpeedTestBytes), token);
        });
        QTRY_VERIFY_WITH_TIMEOUT(srv.sawCommand(QStringLiteral("STOR ") + kSpeed), 10000);
        QTest::qWait(300);
        src.cancel();
        future.waitForFinished();
        const auto r = future.result();
        QVERIFY(!r.ok());
        QVERIFY(r.error().isCancelled());
        QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(srv.path(kSpeed)), 15000);
        QVERIFY(srv.sawCommand(QStringLiteral("DELE ") + kSpeed));
    }

    // ---- the job ------------------------------------------------------------------------

    void diagnoseJobEndToEnd()
    {
        Module mod;
        QVERIFY(mod.jobs.contains(jobkinds::kFtpDiagnose));
        FakeFtpServer::Options so;
        so.root = freshRoot();
        FakeFtpServer srv(so);
        const SettingsValues s(mod.schema,
                               {{keys::kConsoleIp, QStringLiteral("127.0.0.1")}, {keys::kFtpPort, srv.port()}, {keys::kConnectTimeout, 3}},
                               {});
        const SettingsValues noIp(mod.schema, {}, {});

        auto job = mod.jobs.create(jobkinds::kFtpDiagnose, {{QStringLiteral("folder"), kGames},
                                                             {QStringLiteral("speedTest"), true},
                                                             {QStringLiteral("speedTestBytes"), 300000}});
        QVERIFY(job.ok());
        QCOMPARE(job.value()->title(), QStringLiteral("PS5 FTP speed test"));
        QCOMPARE(job.value()->phases().size(), 3);
        QVERIFY(!job.value()->validate(noIp).ok());
        QVERIFY(job.value()->validate(s).ok());
        ProgressReporter progress;
        progress.setPhases(job.value()->phases());
        JobContext ctx;
        ctx.progress = &progress;
        ctx.settings = s;
        ctx.log = Log(QStringLiteral("test"));
        auto out = job.value()->run(ctx);
        QVERIFY2(out.ok(), out.ok() ? "" : qPrintable(out.error().toLogString()));
        QCOMPARE(out->verification, Verification::Passed);
        QVERIFY(out->warnings.isEmpty());
        const FtpDiagnosis d = FtpDiagnosis::fromJson(out->details);
        QVERIFY(d.usable());
        QCOMPARE(d.port, srv.port());
        QCOMPARE(d.speed.bytes, qint64(300000));
        QCOMPARE(d.speed.readBackMatches, std::optional<bool>(true));
        QVERIFY(out->summary.contains(QStringLiteral("writable")));
        QVERIFY(out->summary.contains(QStringLiteral("MB/s")));

        // host/port parameters override the settings; an unreachable console
        // fails the job with the outcome kept.
        const fwtest::ClosedPort closed;
        QVERIFY(closed.isValid());
        auto bad = mod.jobs.create(jobkinds::kFtpDiagnose, {{QStringLiteral("host"), QStringLiteral("127.0.0.1")},
                                                             {QStringLiteral("port"), int(closed.port())}});
        QVERIFY(bad.ok());
        auto badOut = bad.value()->run(ctx);
        QVERIFY(badOut.ok());
        QCOMPARE(badOut->verification, Verification::Failed);
        QCOMPARE(badOut->details.value(QStringLiteral("reachable")).toBool(), false);
        QVERIFY(badOut->summary.startsWith(QStringLiteral("Not reachable")));

        // A folder that cannot be written: inconclusive with a warning.
        auto ro = mod.jobs.create(jobkinds::kFtpDiagnose, {{QStringLiteral("folder"), QStringLiteral("/nope")}});
        QVERIFY(ro.ok());
        auto roOut = ro.value()->run(ctx);
        QVERIFY(roOut.ok());
        QCOMPARE(roOut->verification, Verification::Inconclusive);
        QVERIFY(!roOut->warnings.isEmpty());

        auto rel = mod.jobs.create(jobkinds::kFtpDiagnose, {{QStringLiteral("folder"), QStringLiteral("relative")}});
        QVERIFY(rel.ok());
        QVERIFY(rel.value()->validate(s).ok()); // normalised to "/relative"

        // The checks write a probe file: a protected folder needs the
        // confirmation, also when it is the games folder from Settings.
        const SettingsValues sys(mod.schema,
                                 {{keys::kConsoleIp, QStringLiteral("127.0.0.1")}, {keys::kFtpPort, srv.port()},
                                  {keys::kGamesFolder, QStringLiteral("/system_ex/app")}},
                                 {});
        auto fromSettings = mod.jobs.create(jobkinds::kFtpDiagnose, {});
        QVERIFY(fromSettings.ok());
        COMPARE_ERROR_CODE(fromSettings.value()->validate(sys), ErrorCode::PermissionDenied);
        QVERIFY(fromSettings.value()->validate(s).ok());
        auto confirmed = mod.jobs.create(jobkinds::kFtpDiagnose, {{QStringLiteral("allowProtectedPaths"), true}});
        QVERIFY(confirmed.ok());
        QVERIFY(confirmed.value()->validate(sys).ok());
        auto typed = mod.jobs.create(jobkinds::kFtpDiagnose, {{QStringLiteral("folder"), QStringLiteral("/user")}});
        QVERIFY(typed.ok());
        COMPARE_ERROR_CODE(typed.value()->validate(s), ErrorCode::PermissionDenied);
    }
};

QTEST_GUILESS_MAIN(TstPs5Diagnostics)
#include "tst_ps5_diagnostics.moc"
