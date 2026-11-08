// End-to-end tests of the Backport tab's engine host (app/backport) against
// the real bundled PS5 Backport engine.
//
//  * Always: scanner rules (folder-name fallback, plausible versions,
//    discovery depth and skips), preflight refusals, AMPR / PlayGo user-file
//    checks, settings round trip, worker failure paths (missing engine).
//  * With FW_BACKPORT_ENGINE=<PS5BackportGUI.exe>: the engine's selftest
//    job, and the protocol (progress events, result, cancel + tree kill).
//  * With FW_BACKPORT_E2E_GAME=<a real, unmodified game dump folder> as well:
//    a full backport (Apply off) into temporary folders, then an AMPR / PlayGo
//    switch and an SDK 5 rebuild of the produced ZIP. The game folder is only
//    read. The user's own PS5 Backport settings and ledger are never touched:
//    D97BETA_CONFIG_DIR / D97BETA_DATA_DIR point at temporary folders.

#include "app/backport/BackportCore.h"
#include "app/backport/BackportHost.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace fw::bp;

namespace {

void writeFile(const QString& path, const QByteArray& data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(data);
}

struct Run {
    QJsonObject result;
    QList<QJsonObject> progress;
    bool finished = false;
};

Run runJob(const QJsonObject& job, int timeoutMs, const std::function<void(Worker&, const QJsonObject&)>& onProgress = {})
{
    Run run;
    Worker w;
    QObject::connect(&w, &Worker::progress, [&](const QJsonObject& ev) {
        run.progress << ev;
        if (onProgress)
            onProgress(w, ev);
    });
    QObject::connect(&w, &Worker::finished, [&](const QJsonObject& r) {
        run.result = r;
        run.finished = true;
    });
    QString err;
    if (!w.start(job, &err)) {
        run.result = {{QStringLiteral("status"), QStringLiteral("NOT_STARTED")}, {QStringLiteral("headline"), err}};
        return run;
    }
    QElapsedTimer t;
    t.start();
    while (!run.finished && t.elapsed() < timeoutMs)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    if (!run.finished)
        w.cancel();
    return run;
}

} // namespace

class TestBackportEngine : public QObject {
    Q_OBJECT
private:
    QTemporaryDir m_cfg, m_data;
    QString m_engine, m_game;

private slots:
    void initTestCase()
    {
        QVERIFY(m_cfg.isValid() && m_data.isValid());
        qputenv("D97BETA_CONFIG_DIR", QDir::toNativeSeparators(m_cfg.path()).toUtf8());
        qputenv("D97BETA_DATA_DIR", QDir::toNativeSeparators(m_data.path()).toUtf8());
        m_engine = qEnvironmentVariable("FW_BACKPORT_ENGINE");
        m_game = qEnvironmentVariable("FW_BACKPORT_E2E_GAME");
        QVERIFY(folders().configDir.startsWith(QDir::fromNativeSeparators(m_cfg.path()), Qt::CaseInsensitive));
    }

    void nameFromFolder()
    {
        auto [t, v] = fw::bp::nameFromFolder(QStringLiteral("PPSA18391 Afterdream (02.000.000)"));
        QCOMPARE(t, QStringLiteral("Afterdream"));
        QCOMPARE(v, QStringLiteral("02.000.000"));
        auto [t2, v2] = fw::bp::nameFromFolder(QStringLiteral("Some_Game_PPSA01234-app0_backup_20260101_101010"));
        QCOMPARE(t2, QStringLiteral("Some Game"));
        QVERIFY(v2.isEmpty());
    }

    void plausibleVersions()
    {
        QVERIFY(plausibleVersion(QStringLiteral("4.00")));
        QVERIFY(plausibleVersion(QStringLiteral("13.60")));
        QVERIFY(!plausibleVersion(QStringLiteral("0.00")));
        QVERIFY(!plausibleVersion(QStringLiteral("70.00")));
        QVERIFY(!plausibleVersion(QStringLiteral("92.00")));
        QVERIFY(!plausibleVersion(QStringLiteral("8")));
    }

    void discoverAndMetadata()
    {
        QTemporaryDir root;
        const QString g1 = root.filePath(QStringLiteral("a/b/PPSA11111 First (01.000.000)"));
        writeFile(g1 + QStringLiteral("/eboot.bin"), QByteArray("\x7f" "ELF", 4) + QByteArray(64, 'x'));
        writeFile(g1 + QStringLiteral("/sce_sys/param.json"),
                  R"({"titleId":"PPSA11111","contentVersion":"01.000.000","sdkVersion":"0x0800000000000000",
                     "requiredSystemSoftwareVersion":"0x0700000000000000",
                     "localizedParameters":{"defaultLanguage":"en-US","en-US":{"titleName":"First Game"}}})");
        // A game folder is not searched further.
        writeFile(g1 + QStringLiteral("/sub/eboot.bin"), "x");
        const QString g2 = root.filePath(QStringLiteral("PPSA22222 Second Game 02.000.000"));
        writeFile(g2 + QStringLiteral("/eboot.bin"), "x");
        // Skipped folders.
        writeFile(root.filePath(QStringLiteral("$RECYCLE.BIN/x/eboot.bin")), "x");
        writeFile(root.filePath(QStringLiteral("System Volume Information/eboot.bin")), "x");
        // Too deep for depth 1.
        const QStringList all = discoverGames({root.path()}, 6, {});
        QCOMPARE(all.size(), 2);
        QCOMPARE(discoverGames({root.path()}, 1, {}).size(), 1);
        // Excluded subtree.
        QCOMPARE(discoverGames({root.path()}, 6, {root.filePath(QStringLiteral("a"))}).size(), 1);

        const Game a = readGame(g1);
        QCOMPARE(a.titleId, QStringLiteral("PPSA11111"));
        QCOMPARE(a.title, QStringLiteral("First Game"));
        QCOMPARE(a.version, QStringLiteral("01.000.000"));
        QCOMPARE(a.sdk, QStringLiteral("8.00"));
        QCOMPARE(a.fw, QStringLiteral("7.00"));
        QVERIFY(!a.alreadyBackported);
        const Game b = readGame(g2);
        QCOMPARE(b.titleId, QStringLiteral("PPSA22222"));
        QCOMPARE(b.title, QStringLiteral("Second Game"));
        QCOMPARE(b.version, QStringLiteral("02.000.000"));
        QVERIFY(b.notes.join(u'|').contains(QStringLiteral("folder name")));
        QVERIFY(executableBytes(g1) > 0);
        QCOMPARE(executableBytes(g2), qint64(0)); // "x" is no ELF/SELF
    }

    void preflightRefusals()
    {
        QTemporaryDir root;
        const QString game = root.filePath(QStringLiteral("game"));
        writeFile(game + QStringLiteral("/eboot.bin"), "x");
        Preflight p = preflight({}, {}, {game});
        QVERIFY(p.errors.join(u'|').contains(QStringLiteral("Backport output folder is not set.")));
        p = preflight(root.filePath(QStringLiteral("out")), root.filePath(QStringLiteral("out/bak")), {game});
        QVERIFY(p.errors.join(u'|').contains(QStringLiteral("must be separate")));
        p = preflight(game + QStringLiteral("/out"), root.filePath(QStringLiteral("bak")), {game});
        QVERIFY(p.errors.join(u'|').contains(QStringLiteral("inside the game folder")));
        p = preflight(root.filePath(QStringLiteral("out")), root.filePath(QStringLiteral("bak")), {game});
        QVERIFY2(p.errors.isEmpty(), qPrintable(p.errors.join(u'|')));
        QVERIFY(QFileInfo(root.filePath(QStringLiteral("out"))).isDir()); // created like the original
    }

    void userLibraryChecks()
    {
        QTemporaryDir root;
        const QString elf = root.filePath(QStringLiteral("libSceAmpr.sprx"));
        writeFile(elf, QByteArray("\x7f" "ELF", 4) + QByteArray(100, 0));
        QVERIFY(checkUserLibrary(elf, QStringLiteral("ampr")).error.contains(QStringLiteral("plain ELF")));
        const QString junk = root.filePath(QStringLiteral("junk.sprx"));
        writeFile(junk, QByteArray(100, 'j'));
        QVERIFY(checkUserLibrary(junk, QStringLiteral("ampr")).error.contains(QStringLiteral("not a signed PS5 library")));
        const QString self = root.filePath(QStringLiteral("mine.sprx"));
        writeFile(self, QByteArray("\x4f\x15\x3d\x1d", 4) + QByteArray(200, 1));
        const UserLibrary u = checkUserLibrary(self, QStringLiteral("playgo"));
        QVERIFY(u.ok);
        QVERIFY(u.label.startsWith(QStringLiteral("your file mine.sprx (")));
        QCOMPARE(shortLabel(QStringLiteral("Known-good AMPR (82484486)"), false, {}), QStringLiteral("known-good 82484486"));
        QCOMPARE(shortLabel(QStringLiteral("Unreal Engine AMPR (0aadd31d)"), false, {}), QStringLiteral("UE 0aadd31d"));
        QCOMPARE(shortLabel(QStringLiteral("drakmor ampr_emu 0.4.2.1 AMPR (2feef6a2)"), false, {}), QStringLiteral("drakmor ampr_emu 0.4.2.1"));
    }

    void settingsRoundTrip()
    {
        writeFile(folders().settingsPath, R"({"klog_ip":"192.168.1.2","geometry":"1x1","scan_depth":40})");
        Settings s = loadSettings();
        QCOMPARE(s.scanDepth, 12); // clamped
        s.scanRoots = {QStringLiteral("D:\\Games")};
        s.outputDir = QStringLiteral("D:\\Out");
        s.backupDir = QStringLiteral("D:\\Bak");
        QVERIFY(saveSettings(s));
        QFile f(folders().settingsPath);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
        QCOMPARE(o.value(QStringLiteral("klog_ip")).toString(), QStringLiteral("192.168.1.2")); // kept
        QCOMPARE(o.value(QStringLiteral("output_dir")).toString(), QStringLiteral("D:\\Out"));
        const Settings back = loadSettings();
        QCOMPARE(back.scanRoots, s.scanRoots);
    }

    void missingEngine()
    {
        if (!m_engine.isEmpty())
            QSKIP("FW_BACKPORT_ENGINE is set");
        Worker w;
        QString err;
        QVERIFY(!w.start(QJsonObject{{QStringLiteral("cmd"), QStringLiteral("selftest")}}, &err));
        QVERIFY(err.contains(QStringLiteral("missing")));
    }

    void engineSelftest()
    {
        if (m_engine.isEmpty())
            QSKIP("set FW_BACKPORT_ENGINE to the bundled PS5BackportGUI.exe");
        const Run r = runJob({{QStringLiteral("cmd"), QStringLiteral("selftest")}}, 600000);
        QCOMPARE(r.result.value(QStringLiteral("status")).toString(), QStringLiteral("DONE"));
        QVERIFY(!r.result.value(QStringLiteral("revision")).toString().isEmpty());
        const QJsonObject mods = r.result.value(QStringLiteral("modules")).toObject();
        for (auto it = mods.begin(); it != mods.end(); ++it)
            QVERIFY2(it.value().toBool(), qPrintable(it.key()));
        QDir jobs(folders().jobsDir);
        QVERIFY(jobs.entryList(QDir::Files).isEmpty()); // the job file was removed
    }

    void engineRefusesNonGame()
    {
        if (m_engine.isEmpty())
            QSKIP("set FW_BACKPORT_ENGINE");
        QTemporaryDir root;
        const QString notGame = root.filePath(QStringLiteral("empty"));
        QDir().mkpath(notGame);
        const Run r = runJob({{QStringLiteral("cmd"), QStringLiteral("backport")},
                              {QStringLiteral("game"), QJsonObject{{QStringLiteral("path"), QDir::toNativeSeparators(notGame)}}},
                              {QStringLiteral("output_root"), QDir::toNativeSeparators(root.filePath(QStringLiteral("out")))},
                              {QStringLiteral("backup_root"), QDir::toNativeSeparators(root.filePath(QStringLiteral("bak")))},
                              {QStringLiteral("options"), QJsonObject{{QStringLiteral("target_sdk"), 4}}}},
                             600000);
        QCOMPARE(r.result.value(QStringLiteral("status")).toString(), QStringLiteral("FAILED"));
        QVERIFY2(r.result.value(QStringLiteral("headline")).toString().contains(QStringLiteral("is not a game folder")),
                 qPrintable(r.result.value(QStringLiteral("headline")).toString()));
    }

    void realBackportEndToEnd()
    {
        if (m_engine.isEmpty() || m_game.isEmpty())
            QSKIP("set FW_BACKPORT_ENGINE and FW_BACKPORT_E2E_GAME (an unmodified game dump; it is only read)");
        QVERIFY(QFileInfo(m_game).isDir());
        const Game g = readGame(m_game);
        QVERIFY2(!g.alreadyBackported, "use an unmodified game folder");
        qInfo() << "game" << g.titleId << g.title << g.version << "sdk" << g.sdk << "fw" << g.fw << g.notes;
        QTemporaryDir root;
        const QString out = QDir::toNativeSeparators(root.filePath(QStringLiteral("Backports")));
        const QString bak = QDir::toNativeSeparators(root.filePath(QStringLiteral("Originals")));
        const Preflight pf = preflight(out, bak, {m_game});
        QVERIFY2(pf.errors.isEmpty(), qPrintable(pf.errors.join(u'|')));
        QElapsedTimer t;
        t.start();
        const Run r = runJob({{QStringLiteral("cmd"), QStringLiteral("backport")},
                              {QStringLiteral("game"), QJsonObject{{QStringLiteral("path"), g.path},
                                                                    {QStringLiteral("title_id"), g.titleId},
                                                                    {QStringLiteral("title"), g.title},
                                                                    {QStringLiteral("version"), g.version},
                                                                    {QStringLiteral("sdk"), g.sdk},
                                                                    {QStringLiteral("fw"), g.fw}}},
                              {QStringLiteral("output_root"), out},
                              {QStringLiteral("backup_root"), bak},
                              {QStringLiteral("options"), QJsonObject{{QStringLiteral("apply"), false}, {QStringLiteral("zip"), true},
                                                                      {QStringLiteral("target_sdk"), 4}, {QStringLiteral("pro_fix"), false},
                                                                      {QStringLiteral("pssr2"), false}}}},
                             3600000);
        const QString status = r.result.value(QStringLiteral("status")).toString();
        qInfo() << "backport" << status << r.result.value(QStringLiteral("headline")).toString() << t.elapsed() / 1000 << "s"
                << r.progress.size() << "progress events";
        QVERIFY2(status == QLatin1String("DONE") || status == QLatin1String("NO_CHANGE"),
                 qPrintable(QJsonDocument(r.result).toJson().left(3000)));
        QVERIFY(!r.progress.isEmpty());
        if (status == QLatin1String("NO_CHANGE"))
            return;
        const QJsonObject zip = r.result.value(QStringLiteral("zip")).toObject();
        QVERIFY(zip.value(QStringLiteral("verified")).toBool());
        const QString zipPath = zip.value(QStringLiteral("path")).toString();
        QVERIFY(QFileInfo(zipPath).isFile());
        QVERIFY(QFileInfo(zipPath).fileName().startsWith(QStringLiteral("FW 4xx ")));
        QVERIFY(QFileInfo(zipPath).absolutePath().compare(QDir::fromNativeSeparators(out), Qt::CaseInsensitive) == 0);
        const QJsonObject stages = r.result.value(QStringLiteral("stages")).toObject();
        QCOMPARE(stages.value(QStringLiteral("apply")).toObject().value(QStringLiteral("state")).toString(), QStringLiteral("SKIPPED"));
        qInfo() << "zip" << zipPath << "fixes" << r.result.value(QStringLiteral("engine")).toObject().value(QStringLiteral("fixes"));

        // The game folder was not touched: the ledger has no row for it.
        QVERIFY(activeLedgerRecord(m_game).isEmpty());

        // Switch AMPR / PlayGo on the produced ZIP (known-good PlayGo).
        const Run sw = runJob({{QStringLiteral("cmd"), QStringLiteral("fakelib")},
                               {QStringLiteral("package"), QDir::toNativeSeparators(zipPath)},
                               {QStringLiteral("output_root"), out},
                               {QStringLiteral("ampr"), QJsonValue()},
                               {QStringLiteral("playgo"), QStringLiteral("a0f20f40")}},
                              600000);
        qInfo() << "fakelib" << sw.result.value(QStringLiteral("status")).toString() << sw.result.value(QStringLiteral("headline")).toString();
        QVERIFY(sw.result.value(QStringLiteral("status")).toString() == QLatin1String("DONE")
                || sw.result.value(QStringLiteral("headline")).toString().contains(QStringLiteral("already has exactly")));

        // SDK 5 rebuild of the FW 4xx ZIP (refused only when the game's own SDK is below 5).
        const Run sdk = runJob({{QStringLiteral("cmd"), QStringLiteral("sdk")},
                                {QStringLiteral("pair"), 5},
                                {QStringLiteral("package"), QDir::toNativeSeparators(zipPath)},
                                {QStringLiteral("output_root"), out},
                                {QStringLiteral("title_id"), g.titleId},
                                {QStringLiteral("title"), g.title},
                                {QStringLiteral("version"), g.version},
                                {QStringLiteral("source_sdk"), g.sdk}},
                               3600000);
        qInfo() << "sdk5" << sdk.result.value(QStringLiteral("status")).toString() << sdk.result.value(QStringLiteral("headline")).toString();
        if (g.sdk.section(u'.', 0, 0).toInt() >= 5) {
            QCOMPARE(sdk.result.value(QStringLiteral("status")).toString(), QStringLiteral("DONE"));
            QVERIFY(QFileInfo(sdk.result.value(QStringLiteral("output")).toString()).fileName().startsWith(QStringLiteral("FW 5xx + ")));
        }
    }

    void cancelKillsTheTree()
    {
        if (m_engine.isEmpty() || m_game.isEmpty())
            QSKIP("set FW_BACKPORT_ENGINE and FW_BACKPORT_E2E_GAME");
        QTemporaryDir root;
        const Game g = readGame(m_game);
        QElapsedTimer t;
        bool sent = false;
        t.start();
        const Run r = runJob({{QStringLiteral("cmd"), QStringLiteral("backport")},
                              {QStringLiteral("game"), QJsonObject{{QStringLiteral("path"), g.path}, {QStringLiteral("title_id"), g.titleId},
                                                                    {QStringLiteral("title"), g.title}, {QStringLiteral("sdk"), g.sdk}}},
                              {QStringLiteral("output_root"), QDir::toNativeSeparators(root.filePath(QStringLiteral("out")))},
                              {QStringLiteral("backup_root"), QDir::toNativeSeparators(root.filePath(QStringLiteral("bak")))},
                              {QStringLiteral("options"), QJsonObject{{QStringLiteral("target_sdk"), 4}}}},
                             600000, [&](Worker& w, const QJsonObject& ev) {
                                 if (!sent && ev.value(QStringLiteral("stage")).toString() == QLatin1String("BASELINE")) {
                                     sent = true;
                                     t.restart();
                                     w.cancel();
                                 }
                             });
        QVERIFY(sent);
        QCOMPARE(r.result.value(QStringLiteral("status")).toString(), QStringLiteral("CANCELLED"));
        QVERIFY2(t.elapsed() < 30000, "cancel took too long");
        QVERIFY(!QFileInfo(root.filePath(QStringLiteral("out"))).exists()
                || QDir(root.filePath(QStringLiteral("out"))).entryList(QStringList{QStringLiteral("*.zip")}, QDir::Files).isEmpty());
    }
};

QTEST_MAIN(TestBackportEngine)
#include "tst_backport_engine.moc"
