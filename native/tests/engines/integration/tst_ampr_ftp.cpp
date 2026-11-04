// AMPR index tool: ampr.index.ftp (console folder over FTP, upload with a
// .bak) and ampr.index.local (local folder) against the in-process
// FakeFtpServer. The index built from the console folder must be byte for
// byte the index built from the same folder on disk.

#include "connect/ps5/FakePs5.h"

#include "connect/ps5/FtpClient.h"
#include "connect/ps5/Module.h"
#include "core/Settings.h"
#include "engines/general/Module.h"
#include "engines/integration/AmprFtp.h"
#include "engines/integration/Module.h"
#include "engines/pfs/AmprIndex.h"
#include "engines/pfs/AmprJobs.h"
#include "engines/pfs/Module.h"
#include "support/TestSupport.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QTest>
#include <QTimeZone>

#include <algorithm>

using namespace fw;
using fake::FakeFtpServer;

namespace {

QByteArray readAll(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

bool put(const QString& path, const QByteArray& bytes, const QDateTime& when)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(bytes) != bytes.size() || !f.flush())
        return false;
    return f.setFileTime(when, QFileDevice::FileModificationTime); // after the data reached the file
}

struct Env {
    std::shared_ptr<SettingsSchema> schema = std::make_shared<SettingsSchema>();
    DependencyRegistry deps;
    JobFactory jobs;
    Env()
    {
        ModuleContext ctx{*schema, deps, jobs};
        general::registerModule(ctx);
        pfs::registerModule(ctx);
        ps5::registerModule(ctx);
        integration::registerModule(ctx);
    }
    SettingsValues settings(quint16 ftpPort) const
    {
        return SettingsValues(schema, {{ps5::keys::kConsoleIp, QStringLiteral("127.0.0.1")}, {ps5::keys::kFtpPort, ftpPort},
                                       {ps5::keys::kConnectTimeout, 3}, {ps5::keys::kIdleTimeout, 5}}, {});
    }
    Result<JobOutcome> run(const QString& kind, const QJsonObject& params, const SettingsValues& s, CancellationToken token = {})
    {
        auto job = jobs.create(kind, params);
        if (!job.ok())
            return job.takeError();
        FW_TRY(job.value()->validate(s));
        ProgressReporter progress;
        progress.setPhases(job.value()->phases());
        JobContext ctx;
        ctx.progress = &progress;
        ctx.settings = s;
        ctx.token = token;
        ctx.log = Log(QStringLiteral("test"));
        return job.value()->run(ctx);
    }
};

// A small APR game: nested folders, spaces, a non-ASCII name, case-only
// variants, the emulator's own files and an old index (all left out).
void makeApp0(const QString& app0)
{
    const QDateTime t0(QDate(2025, 6, 1), QTime(10, 20, 30), QTimeZone::utc());
    int i = 0;
    auto at = [&] { return t0.addSecs(3600 * (++i)); };
    QVERIFY(put(app0 + QStringLiteral("/eboot.bin"), QByteArray(4096, 'E'), at()));
    QVERIFY(put(app0 + QStringLiteral("/sce_sys/param.json"), "{\"titleId\":\"PPSA00001\"}", at()));
    QVERIFY(put(app0 + QStringLiteral("/fakelib/libSceAmpr.sprx"), QByteArray("\x7f" "ELF", 4) + QByteArray(100, 'a'), at()));
    QVERIFY(put(app0 + QStringLiteral("/Data/Level 1/map.bin"), QByteArray(777, 'm'), at()));
    QVERIFY(put(app0 + QStringLiteral("/Data/Level 1/Sound.PAK"), QByteArray(1234, 's'), at()));
    QVERIFY(put(app0 + QStringLiteral("/data/other.bin"), QByteArray(5, 'o'), at())); // "Data" vs "data"
    QVERIFY(put(app0 + QStringLiteral("/Data/Ä-umlaut.txt"), "u", at()));
    QVERIFY(put(app0 + QStringLiteral("/Data/zero.bin"), QByteArray(), at()));
    QVERIFY(put(app0 + QStringLiteral("/Data/CASE.bin"), "1", at()));
    QVERIFY(put(app0 + QStringLiteral("/Data/case.bin"), "22", at())); // dropped: case-only variant
    QVERIFY(put(app0 + QStringLiteral("/ampr_emu.index"), "old index", at()));
    QVERIFY(put(app0 + QStringLiteral("/ampr_commands.bin"), "cmd", at()));
    QVERIFY(put(app0 + QStringLiteral("/apr_emu.log"), "log", at()));
    QVERIFY(QDir().mkpath(app0 + QStringLiteral("/empty folder")));
}

QByteArray localIndex(const QString& app0)
{
    auto scan = pfs::scanAmprFolder(app0);
    if (!scan.ok())
        return {};
    return pfs::buildAmprIndex(pfs::amprToolRows(scan->files).rows);
}

} // namespace

class TstAmprFtp : public QObject {
    Q_OBJECT
    QTemporaryDir m_tmp;
    int m_n = 0;

    QString fresh(const QString& name)
    {
        const QString p = m_tmp.filePath(QStringLiteral("%1-%2").arg(++m_n).arg(name));
        QDir().mkpath(p);
        return p;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_tmp.isValid());
        fwtest::isolateAppData(m_tmp);
    }

    void toolRulesSkipOwnFilesAndKeepTheFirstCaseVariant()
    {
        const auto rows = pfs::amprToolRows({{"b/Z.bin", 1, 1},
                                             {"ampr_emu.index", 9, 9},
                                             {"AMPR_EMU.INDEX.BAK", 9, 9},
                                             {"ampr_emu.index.tmp", 9, 9},
                                             {"ampr_commands.bin", 9, 9},
                                             {"apr_emu.log", 9, 9},
                                             {"sub/ampr_commands.bin", 3, 3}, // only the top-level one is the emulator's
                                             {"tab\there.bin", 1, 1},
                                             {"b/z.bin", 2, 2},
                                             {"a.bin", 3, 3}});
        QCOMPARE(int(rows.rows.size()), 3);
        QCOMPARE(rows.rows[0].path, QByteArray("/app0/a.bin"));
        QCOMPARE(rows.rows[1].path, QByteArray("/app0/b/Z.bin")); // 'Z' < 'z' in byte order: kept
        QCOMPARE(rows.rows[2].path, QByteArray("/app0/sub/ampr_commands.bin"));
        QCOMPARE(rows.skippedOwn.size(), 5);
        QCOMPARE(rows.skippedControl, QStringList{QStringLiteral("tab\there.bin")});
        QCOMPARE(rows.collisions, QStringList{QStringLiteral("/app0/b/Z.bin <- /app0/b/z.bin")});
        // Walk order does not change the result.
        const auto reversed = pfs::amprToolRows({{"a.bin", 3, 3}, {"b/z.bin", 2, 2}, {"b/Z.bin", 1, 1}});
        QCOMPARE(pfs::buildAmprIndex(reversed.rows), pfs::buildAmprIndex(pfs::amprToolRows({{"b/Z.bin", 1, 1}, {"b/z.bin", 2, 2}, {"a.bin", 3, 3}}).rows));
    }

    void consoleIndexEqualsLocalIndex_data()
    {
        QTest::addColumn<bool>("mlsd");
        QTest::newRow("MLSD") << true;
        QTest::newRow("LIST + MDTM") << false;
    }

    void consoleIndexEqualsLocalIndex()
    {
        QFETCH(bool, mlsd);
        Env env;
        const QString root = fresh(QStringLiteral("console"));
        const QString app0 = root + QStringLiteral("/data/etaHEN/games/PPSA00001-app0");
        makeApp0(app0);
        const QByteArray expected = localIndex(app0);
        QVERIFY(expected.startsWith("AMPRIDX3"));
        // The fixture's times are what it set (the index records them).
        const auto scan = pfs::scanAmprFolder(app0);
        const auto eboot = std::find_if(scan->files.begin(), scan->files.end(), [](const pfs::AmprFile& f) { return f.relPath == "eboot.bin"; });
        QCOMPARE(eboot->mtime, QDateTime(QDate(2025, 6, 1), QTime(11, 20, 30), QTimeZone::utc()).toSecsSinceEpoch());
        const QByteArray oldIndex = readAll(app0 + QStringLiteral("/ampr_emu.index"));

        FakeFtpServer::Options o;
        o.root = root;
        o.mlsd = mlsd;
        FakeFtpServer srv(o);
        auto out = env.run(integration::kAmprFtpJob, {{QStringLiteral("remoteFolder"), QStringLiteral("/data/etaHEN/games/PPSA00001-app0/")}},
                           env.settings(srv.port()));
        QVERIFY2(out.ok(), out.ok() ? "" : qPrintable(out.error().toLogString()));
        QCOMPARE(out->verification, Verification::Passed);
        // Uploaded index == the index of the same folder on disk, byte for byte.
        const QByteArray uploaded = readAll(app0 + QStringLiteral("/ampr_emu.index"));
        QCOMPARE(uploaded.size(), expected.size());
        QVERIFY(uploaded == expected);
        // The old index was kept as .bak (upstream fix), no temporary left.
        QCOMPARE(readAll(app0 + QStringLiteral("/ampr_emu.index.bak")), oldIndex);
        QVERIFY(!QFileInfo::exists(app0 + QStringLiteral("/ampr_emu.index.tmp")));
        QCOMPARE(out->details.value(QStringLiteral("files")).toInt(), 9);
        QCOMPARE(out->details.value(QStringLiteral("backup")).toString(), QStringLiteral("/data/etaHEN/games/PPSA00001-app0/ampr_emu.index.bak"));
        QCOMPARE(out->details.value(QStringLiteral("collisions")).toArray().size(), 1);
        QVERIFY(out->details.value(QStringLiteral("hasAmprLibrary")).toBool());
        QCOMPARE(srv.sawCommand(QStringLiteral("MDTM")), !mlsd);

        // Second run: the new index and its .bak are left out, so the bytes stay the same.
        auto again = env.run(integration::kAmprFtpJob, {{QStringLiteral("remoteFolder"), QStringLiteral("/data/etaHEN/games/PPSA00001-app0")}},
                             env.settings(srv.port()));
        QVERIFY(again.ok());
        QVERIFY(readAll(app0 + QStringLiteral("/ampr_emu.index")) == expected);
        QVERIFY(readAll(app0 + QStringLiteral("/ampr_emu.index.bak")) == expected);

        // The local tool writes the same bytes into a copy of the folder.
        const QString copy = fresh(QStringLiteral("local-copy"));
        makeApp0(copy);
        auto local = env.run(pfs::kAmprLocalJob, {{QStringLiteral("folder"), copy}}, env.settings(srv.port()));
        QVERIFY2(local.ok(), local.ok() ? "" : qPrintable(local.error().toLogString()));
        QCOMPARE(local->verification, Verification::Passed);
        QVERIFY(readAll(copy + QStringLiteral("/ampr_emu.index")) == expected);
        QCOMPARE(local->outputs, QStringList{QDir(copy).filePath(QStringLiteral("ampr_emu.index"))});
        QVERIFY(!QFileInfo::exists(copy + QStringLiteral("/ampr_emu.index.part")));
    }

    void refusesAParentFolderAndBadParameters()
    {
        Env env;
        const QString root = fresh(QStringLiteral("parent"));
        makeApp0(root + QStringLiteral("/data/etaHEN/games/GameA"));
        QVERIFY(put(root + QStringLiteral("/data/etaHEN/games/GameB/eboot.bin"), "b", QDateTime::currentDateTimeUtc()));
        FakeFtpServer::Options o;
        o.root = root;
        FakeFtpServer srv(o);
        const SettingsValues s = env.settings(srv.port());
        auto parent = env.run(integration::kAmprFtpJob, {{QStringLiteral("remoteFolder"), QStringLiteral("/data/etaHEN/games")}}, s);
        QVERIFY(!parent.ok());
        QVERIFY(parent.error().message.contains(QStringLiteral("not a game app0 folder")));
        QVERIFY(parent.error().message.contains(QStringLiteral("2 subfolder(s)")));
        QVERIFY(!QFileInfo::exists(root + QStringLiteral("/data/etaHEN/games/ampr_emu.index")));
        QVERIFY(!srv.sawCommand(QStringLiteral("STOR")));

        auto missing = env.run(integration::kAmprFtpJob, {{QStringLiteral("remoteFolder"), QStringLiteral("/data/nope")}}, s);
        QVERIFY(!missing.ok());
        QCOMPARE(missing.error().code, ErrorCode::NotFound);
        QVERIFY(!env.run(integration::kAmprFtpJob, {{QStringLiteral("remoteFolder"), QStringLiteral("relative/app0")}}, s).ok());
        QCOMPARE(env.run(integration::kAmprFtpJob, {{QStringLiteral("remoteFolder"), QStringLiteral("/system/app0")}}, s).error().code,
                 ErrorCode::PermissionDenied);
        const SettingsValues noHost(env.schema, {}, {});
        QVERIFY(!env.run(integration::kAmprFtpJob, {{QStringLiteral("remoteFolder"), QStringLiteral("/data/etaHEN/games/GameA")}}, noHost).ok());

        // Local tool: "Not an APR title?" unless forced.
        const QString plain = fresh(QStringLiteral("not-apr"));
        QVERIFY(put(plain + QStringLiteral("/eboot.bin"), "e", QDateTime::currentDateTimeUtc()));
        auto refused = env.run(pfs::kAmprLocalJob, {{QStringLiteral("folder"), plain}}, s);
        QVERIFY(!refused.ok());
        QVERIFY(refused.error().message.contains(QStringLiteral("does not appear to be an APR title")));
        auto forced = env.run(pfs::kAmprLocalJob, {{QStringLiteral("folder"), plain}, {QStringLiteral("force"), true}}, s);
        QVERIFY(forced.ok());
        QVERIFY(!forced->warnings.isEmpty());
        QVERIFY(readAll(plain + QStringLiteral("/ampr_emu.index")).startsWith("AMPRIDX3"));
    }

    void renameRefusedLeavesTheOldIndex()
    {
        Env env;
        const QString root = fresh(QStringLiteral("norename"));
        const QString app0 = root + QStringLiteral("/g/app0");
        makeApp0(app0);
        FakeFtpServer::Options o;
        o.root = root;
        o.mlsd = true;
        o.renameRefused = true;
        FakeFtpServer srv(o);
        auto out = env.run(integration::kAmprFtpJob, {{QStringLiteral("remoteFolder"), QStringLiteral("/g/app0")}}, env.settings(srv.port()));
        QVERIFY(!out.ok());
        QVERIFY(out.error().message.contains(QStringLiteral("nothing was changed")));
        QCOMPARE(readAll(app0 + QStringLiteral("/ampr_emu.index")), QByteArray("old index"));
        QVERIFY(!QFileInfo::exists(app0 + QStringLiteral("/ampr_emu.index.tmp")));
        QVERIFY(!QFileInfo::exists(app0 + QStringLiteral("/ampr_emu.index.bak")));
    }

    void cancelWritesNothingAndUploadKeepsABackup()
    {
        Env env;
        const QString root = fresh(QStringLiteral("cancel"));
        const QString app0 = root + QStringLiteral("/g/app0");
        makeApp0(app0);
        FakeFtpServer::Options o;
        o.root = root;
        o.mlsd = true;
        FakeFtpServer srv(o);
        // Cancelled while walking: nothing is written on the console.
        CancellationSource cancel;
        cancel.cancel(QStringLiteral("test"));
        auto out = env.run(integration::kAmprFtpJob, {{QStringLiteral("remoteFolder"), QStringLiteral("/g/app0")}}, env.settings(srv.port()),
                           cancel.token());
        QVERIFY(!out.ok());
        QVERIFY(out.error().isCancelled());
        QCOMPARE(readAll(app0 + QStringLiteral("/ampr_emu.index")), QByteArray("old index"));
        QVERIFY(!QFileInfo::exists(app0 + QStringLiteral("/ampr_emu.index.tmp")));

        // uploadAmprIndex on its own: the old index becomes the .bak.
        ps5::FtpOptions fo;
        fo.connectTimeoutMs = 3000;
        fo.replyTimeoutMs = 3000;
        fo.idleTimeoutMs = 1500;
        ps5::FtpClient c(fo);
        QVERIFY(c.open(QStringLiteral("127.0.0.1"), srv.port()).ok());
        auto up = integration::uploadAmprIndex(c, QStringLiteral("/g/app0"), QByteArray("AMPRIDX3new"));
        QVERIFY(up.ok());
        QCOMPARE(readAll(app0 + QStringLiteral("/ampr_emu.index")), QByteArray("AMPRIDX3new"));
        QCOMPARE(readAll(app0 + QStringLiteral("/ampr_emu.index.bak")), QByteArray("old index"));
    }
};

QTEST_GUILESS_MAIN(TstAmprFtp)
#include "tst_ampr_ftp.moc"
