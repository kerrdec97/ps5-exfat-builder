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
//    switch and an SDK 5 rebuild of the produced ZIP. The game folder is
//    copied into a temporary folder the first time one of these tests needs
//    it (under FW_BACKPORT_E2E_COPY_DIR when set, else the system's temporary
//    folder) and only the copy is given to the engine; the other tests never
//    wait for the copy. Too little free space for it skips these tests; a
//    link in the dump fails them (the copy would not be that dump). The
//    user's own PS5 Backport settings and ledger are never touched:
//    D97BETA_CONFIG_DIR / D97BETA_DATA_DIR point at temporary folders.

#include "app/backport/BackportCore.h"
#include "app/backport/BackportHost.h"
#include "core/FileUtil.h"
#include "core/FsLinks.h"
#include "support/TestSupport.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QStorageInfo>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

using namespace fw::bp;

namespace {

bool writeFile(const QString& path, const QByteArray& data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
}

// Copies the folder `from` (files and sub-folders) into `to`. A symbolic link
// or junction in it is refused, not left out: the copy would not be the dump
// the test says it uses. `why` then names what stopped the copy.
bool copyTree(const QString& from, const QString& to, QString& why)
{
    if (!QDir().mkpath(to)) {
        why = QStringLiteral("could not create %1").arg(QDir::toNativeSeparators(to));
        return false;
    }
    const QDir src(from);
    for (const QFileInfo& fi : src.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System)) {
        if (fi.isSymbolicLink() || fi.isJunction()) {
            why = QStringLiteral("the game folder holds a link: %1").arg(QDir::toNativeSeparators(fi.absoluteFilePath()));
            return false;
        }
        const QString target = QDir(to).filePath(fi.fileName());
        if (fi.isDir()) {
            if (!copyTree(fi.absoluteFilePath(), target, why))
                return false;
        } else if (!QFile::copy(fi.absoluteFilePath(), target)) {
            why = QStringLiteral("could not copy %1 to %2").arg(QDir::toNativeSeparators(fi.absoluteFilePath()), QDir::toNativeSeparators(target));
            return false;
        }
    }
    return true;
}

// The bytes of the files below `folder` (links are not followed).
qint64 treeBytes(const QString& folder)
{
    qint64 bytes = 0;
    QDirIterator it(folder, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    while (it.hasNext())
        bytes += it.nextFileInfo().size();
    return bytes;
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

// In an end-to-end test: the copy of the game folder (m_game), or a skip or a
// failure of this test only when there is none.
#define FW_NEED_GAME_COPY()                                                                                            \
    do {                                                                                                               \
        prepareGameCopy();                                                                                             \
        if (!m_gameSkip.isEmpty())                                                                                     \
            QSKIP(qPrintable(m_gameSkip));                                                                             \
        QVERIFY2(m_gameFailure.isEmpty(), qPrintable(m_gameFailure));                                                  \
        QVERIFY(QFileInfo(m_game).isDir());                                                                            \
    } while (false)

class TestBackportEngine : public QObject {
    Q_OBJECT
private:
    QTemporaryDir m_cfg, m_data, m_home;
    std::unique_ptr<QTemporaryDir> m_gameCopy; // made by the first end-to-end test
    QString m_engine, m_game;
    bool m_gameTried = false;
    QString m_gameSkip, m_gameFailure; // why there is no copy

    // The engine only ever sees a copy: the dump named by FW_BACKPORT_E2E_GAME
    // is read once, here, and never handed to a worker. The copy is made when
    // an end-to-end test first asks for it (a dump is tens of GB; the other
    // tests run without it), on the drive of FW_BACKPORT_E2E_COPY_DIR when set.
    void prepareGameCopy()
    {
        if (m_gameTried)
            return;
        m_gameTried = true;
        const QString original = qEnvironmentVariable("FW_BACKPORT_E2E_GAME");
        if (m_engine.isEmpty() || original.isEmpty()) {
            m_gameSkip = QStringLiteral("set FW_BACKPORT_ENGINE and FW_BACKPORT_E2E_GAME (an unmodified game dump; the engine works on a copy)");
            return;
        }
        if (!QFileInfo(original).isDir()) {
            m_gameFailure = QStringLiteral("FW_BACKPORT_E2E_GAME is not a folder: %1").arg(original);
            return;
        }
        const QString parent = qEnvironmentVariable("FW_BACKPORT_E2E_COPY_DIR", QDir::tempPath());
        if (!QFileInfo(parent).isDir()) {
            m_gameFailure = QStringLiteral("FW_BACKPORT_E2E_COPY_DIR is not a folder: %1").arg(parent);
            return;
        }
        const qint64 needed = treeBytes(original) + 1024LL * 1024 * 1024; // and a margin for the engine's own files
        const QStorageInfo volume(parent);
        if (volume.isValid() && volume.bytesAvailable() < needed) {
            m_gameSkip = QStringLiteral("not enough free space in %1 for a copy of the game folder (%2 MB needed, %3 MB free); "
                                        "set FW_BACKPORT_E2E_COPY_DIR to a folder on a drive with room")
                             .arg(QDir::toNativeSeparators(parent))
                             .arg(needed / (1024 * 1024))
                             .arg(volume.bytesAvailable() / (1024 * 1024));
            return;
        }
        m_gameCopy = std::make_unique<QTemporaryDir>(QDir(parent).filePath(QStringLiteral("fw-backport-e2e-XXXXXX")));
        if (!m_gameCopy->isValid()) {
            m_gameFailure = QStringLiteral("could not create a temporary folder in %1").arg(QDir::toNativeSeparators(parent));
            return;
        }
        const QString copy = QDir(m_gameCopy->path()).filePath(QFileInfo(QDir::cleanPath(original)).fileName());
        QString why;
        if (!copyTree(original, copy, why)) {
            m_gameFailure = QStringLiteral("could not copy the game folder: %1").arg(why);
            m_gameCopy.reset(); // what was copied goes
            return;
        }
        m_game = copy;
        qInfo() << "the end-to-end tests work on a copy:" << QDir::toNativeSeparators(m_game);
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_cfg.isValid() && m_data.isValid() && m_home.isValid());
        qputenv("D97BETA_CONFIG_DIR", QDir::toNativeSeparators(m_cfg.path()).toUtf8());
        qputenv("D97BETA_DATA_DIR", QDir::toNativeSeparators(m_data.path()).toUtf8());
        // The engine's own work folder too: an opt-in real run must not share
        // the one the installed app and the standalone program use.
        qputenv("D97BETA_ENGINE_HOME", QDir::toNativeSeparators(m_home.path()).toUtf8());
        m_engine = qEnvironmentVariable("FW_BACKPORT_ENGINE");
        QVERIFY(folders().configDir.startsWith(QDir::fromNativeSeparators(m_cfg.path()), Qt::CaseInsensitive));
    }

    // The copy the end-to-end tests work on is the dump or nothing: files and
    // folders as they are, and a link in it refused by name (never left out).
    void gameCopyRefusesLinks()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString dump = root.filePath(QStringLiteral("dump"));
        QVERIFY(writeFile(dump + QStringLiteral("/eboot.bin"), "elf"));
        QVERIFY(writeFile(dump + QStringLiteral("/sce_sys/param.json"), "{}"));
        QCOMPARE(treeBytes(dump), qint64(5));
        QString why;
        QVERIFY2(copyTree(dump, root.filePath(QStringLiteral("copy")), why), qPrintable(why));
        QVERIFY(QFileInfo(root.filePath(QStringLiteral("copy/sce_sys/param.json"))).isFile());
        QCOMPARE(treeBytes(root.filePath(QStringLiteral("copy"))), qint64(5));

        const QString outside = root.filePath(QStringLiteral("outside"));
        QVERIFY(writeFile(outside + QStringLiteral("/other.bin"), "x"));
        fw::JunctionGuard link(dump + QStringLiteral("/sce_sys/linked"));
        if (const fw::Status made = fw::createDirectoryJunction(link.path(), outside); !made.ok())
            QSKIP(qPrintable(QStringLiteral("No folder link can be made here (%1)").arg(made.error().message)));
        QVERIFY(!copyTree(dump, root.filePath(QStringLiteral("copy2")), why));
        QVERIFY2(why.contains(QStringLiteral("holds a link")) && why.contains(QStringLiteral("linked")), qPrintable(why));
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
        QVERIFY(writeFile(g1 + QStringLiteral("/eboot.bin"), QByteArray("\x7f" "ELF", 4) + QByteArray(64, 'x')));
        QVERIFY(writeFile(g1 + QStringLiteral("/sce_sys/param.json"),
                  R"({"titleId":"PPSA11111","contentVersion":"01.000.000","sdkVersion":"0x0800000000000000",
                     "requiredSystemSoftwareVersion":"0x0700000000000000",
                     "localizedParameters":{"defaultLanguage":"en-US","en-US":{"titleName":"First Game"}}})"));
        // A game folder is not searched further.
        QVERIFY(writeFile(g1 + QStringLiteral("/sub/eboot.bin"), "x"));
        const QString g2 = root.filePath(QStringLiteral("PPSA22222 Second Game 02.000.000"));
        QVERIFY(writeFile(g2 + QStringLiteral("/eboot.bin"), "x"));
        // Skipped folders.
        QVERIFY(writeFile(root.filePath(QStringLiteral("$RECYCLE.BIN/x/eboot.bin")), "x"));
        QVERIFY(writeFile(root.filePath(QStringLiteral("System Volume Information/eboot.bin")), "x"));
        const QStringList all = discoverGames({root.path()}, 6, {});
        QCOMPARE(all.size(), 2);
        QCOMPARE(discoverGames({root.path()}, 1, {}).size(), 1); // "a/b/…" is too deep for depth 1
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
        QVERIFY(writeFile(game + QStringLiteral("/eboot.bin"), "x"));
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
        QVERIFY(writeFile(elf, QByteArray("\x7f" "ELF", 4) + QByteArray(100, 0)));
        QVERIFY(checkUserLibrary(elf, QStringLiteral("ampr")).error.contains(QStringLiteral("plain ELF")));
        const QString junk = root.filePath(QStringLiteral("junk.sprx"));
        QVERIFY(writeFile(junk, QByteArray(100, 'j')));
        QVERIFY(checkUserLibrary(junk, QStringLiteral("ampr")).error.contains(QStringLiteral("not a signed PS5 library")));
        const QString self = root.filePath(QStringLiteral("mine.sprx"));
        QVERIFY(writeFile(self, QByteArray("\x4f\x15\x3d\x1d", 4) + QByteArray(200, 1)));
        const UserLibrary u = checkUserLibrary(self, QStringLiteral("playgo"));
        QVERIFY(u.ok);
        QVERIFY(u.label.startsWith(QStringLiteral("your file mine.sprx (")));
        QCOMPARE(shortLabel(QStringLiteral("Known-good AMPR (82484486)"), false, {}), QStringLiteral("known-good 82484486"));
        QCOMPARE(shortLabel(QStringLiteral("Unreal Engine AMPR (0aadd31d)"), false, {}), QStringLiteral("UE 0aadd31d"));
        QCOMPARE(shortLabel(QStringLiteral("drakmor ampr_emu 0.4.2.1 AMPR (2feef6a2)"), false, {}), QStringLiteral("drakmor ampr_emu 0.4.2.1"));
    }

    void settingsRoundTrip()
    {
        QVERIFY(writeFile(folders().settingsPath, R"({"klog_ip":"192.168.1.2","geometry":"1x1","scan_depth":40})"));
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

    // settings.json belongs to the standalone program too: a save changes the
    // asked keys in the file as it is NOW and keeps everything else.
    void settingsSaveMergesIntoTheFile()
    {
        QVERIFY(writeFile(folders().settingsPath,
                  R"({"scan_roots":["D:\\A"],"output_dir":"D:\\Out","backup_dir":"D:\\Bak","scan_depth":6,)"
                  R"("klog_ip":"10.0.0.9","future_key":{"keep":[1,2,3]}})"));
        SettingsFile state = SettingsFile::Missing;
        Settings s = loadSettings(&state);
        QCOMPARE(state, SettingsFile::Ok);
        QCOMPARE(s.scanRoots, QStringList{QStringLiteral("D:\\A")});

        // The other program adds a location and moves the output folder after this app loaded.
        QVERIFY(writeFile(folders().settingsPath,
                  R"({"scan_roots":["D:\\A","E:\\B"],"output_dir":"D:\\Elsewhere","backup_dir":"D:\\Bak","scan_depth":6,)"
                  R"("klog_ip":"10.0.0.9","future_key":{"keep":[1,2,3]},"geometry":"900x700"})"));
        s.scanDepth = 3; // the one thing the user changed here
        QString err;
        QVERIFY2(saveSettings(s, ScanDepthKey, &err), qPrintable(err));
        QFile f(folders().settingsPath);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
        QCOMPARE(o.value(QStringLiteral("scan_depth")).toInt(), 3);
        QCOMPARE(o.value(QStringLiteral("scan_roots")).toArray().size(), 2); // not rolled back to this app's copy
        QCOMPARE(o.value(QStringLiteral("output_dir")).toString(), QStringLiteral("D:\\Elsewhere"));
        QCOMPARE(o.value(QStringLiteral("geometry")).toString(), QStringLiteral("900x700"));
        QCOMPARE(o.value(QStringLiteral("klog_ip")).toString(), QStringLiteral("10.0.0.9"));
        QCOMPARE(o.value(QStringLiteral("future_key")).toObject().value(QStringLiteral("keep")).toArray().size(), 3); // unknown keys survive
    }

    void settingsFoldersAreWrittenNative()
    {
        QFile::remove(folders().settingsPath);
        Settings s;
        s.scanRoots = {QStringLiteral("D:/Games")};
        s.outputDir = QStringLiteral("D:/Out/Sub/");
        s.backupDir = QStringLiteral("D:\\Bak");
        QVERIFY(saveSettings(s, OutputDirKey | BackupDirKey | ScanRootsKey));
        QFile f(folders().settingsPath);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
        QCOMPARE(o.value(QStringLiteral("output_dir")).toString(), QStringLiteral("D:\\Out\\Sub"));
        QCOMPARE(o.value(QStringLiteral("backup_dir")).toString(), QStringLiteral("D:\\Bak"));
        QCOMPARE(o.value(QStringLiteral("scan_roots")).toArray().first().toString(), QStringLiteral("D:\\Games"));
        QVERIFY(o.contains(QStringLiteral("klog_port"))); // a new file gets the standalone program's other keys
        QCOMPARE(nativeFolder(QString()), QString());
        QCOMPARE(loadSettings().outputDir, QStringLiteral("D:\\Out\\Sub")); // both separator forms are read
    }

    // A file that exists but cannot be parsed (cut short by a crash, bytes that
    // are not UTF-8) is never replaced: its content is all there is to recover.
    void unreadableSettingsFileIsNotOverwritten()
    {
        const QByteArray cutShort = QByteArray("{\"scan_roots\": [\"D:/Games\", \"E:/Mo");
        const QByteArray notUtf8 = QByteArray("{\"output_dir\":\"D:/Sp") + QByteArray::fromHex("e4df") + QByteArray("\"}");
        const QList<QByteArray> damaged = {cutShort, notUtf8, QByteArray("[1,2,3]")};
        for (const QByteArray& bytes : damaged) {
            QVERIFY(writeFile(folders().settingsPath, bytes));
            SettingsFile state = SettingsFile::Ok;
            QString why;
            const Settings s = loadSettings(&state, &why);
            QCOMPARE(state, SettingsFile::Unreadable);
            QVERIFY(!why.isEmpty());
            QVERIFY(s.scanRoots.isEmpty());
            Settings mine;
            mine.scanRoots = {QStringLiteral("D:\\New")};
            QString err;
            QVERIFY(!saveSettings(mine, ScanRootsKey, &err));
            QVERIFY2(err.contains(QStringLiteral("cannot be read")), qPrintable(err));
            QVERIFY(!saveSettings(mine)); // the four-key form refuses as well
            QFile f(folders().settingsPath);
            QVERIFY(f.open(QIODevice::ReadOnly));
            QCOMPARE(f.readAll(), bytes);
        }
        // A file with nothing in it holds nothing to lose: it counts as absent.
        QVERIFY(writeFile(folders().settingsPath, "  \r\n"));
        SettingsFile state = SettingsFile::Ok;
        (void)loadSettings(&state);
        QCOMPARE(state, SettingsFile::Missing);
        Settings mine;
        mine.scanRoots = {QStringLiteral("D:\\New")};
        QVERIFY(saveSettings(mine, ScanRootsKey));
        QCOMPARE(loadSettings().scanRoots, mine.scanRoots);
        QFile::remove(folders().settingsPath);
    }

    // An apply whose journal is still being written is running, not interrupted.
    void unfinishedAppliesLeaveARunningApplyAlone()
    {
        QTemporaryDir root;
        const QString journal = QDir::toNativeSeparators(root.filePath(QStringLiteral("bak/PPSA11111/APPLY_JOURNAL.json")));
        QVERIFY(writeFile(journal, R"({"state":"IN_PROGRESS"})"));
        const QString gameDir = QDir::toNativeSeparators(root.filePath(QStringLiteral("Games/PPSA11111")));
        QVERIFY(QDir().mkpath(gameDir)); // a game folder that is gone has nothing to restore into
        const QJsonObject row{{QStringLiteral("state"), QStringLiteral("IN_PROGRESS")},
                              {QStringLiteral("journal"), journal},
                              {QStringLiteral("game_dir"), gameDir},
                              {QStringLiteral("backup_dir"), QFileInfo(journal).absolutePath()}};
        QVERIFY(writeFile(folders().ledgerPath, QJsonDocument(QJsonObject{{QStringLiteral("applied"), QJsonArray{row}}}).toJson()));
        QVERIFY(unfinishedApplies(kApplyQuietSeconds).empty()); // written this second
        QCOMPARE(unfinishedApplies(0).size(), size_t(1));       // the rule itself still finds it
        {
            QFile jf(journal);
            QVERIFY(jf.open(QIODevice::ReadWrite));
            QVERIFY(jf.setFileTime(QDateTime::currentDateTime().addSecs(-2 * kApplyQuietSeconds), QFileDevice::FileModificationTime));
        }
        const auto stale = unfinishedApplies(kApplyQuietSeconds);
        QCOMPARE(stale.size(), size_t(1));
        QCOMPARE(stale.front().value(QStringLiteral("journal")).toString(), journal);
        // A finished journal is never offered, whatever the ledger says.
        QVERIFY(writeFile(journal, R"({"state":"ROLLED_BACK"})"));
        QVERIFY(unfinishedApplies(0).empty());
        QFile::remove(folders().ledgerPath);
    }

    // PS5 Backport GUI 0.3.5 (public since 17 Nov) writes the same ledger with
    // two more unfinished states. A folder in one still holds backport files:
    // it is "applied" here too, and its restore is offered.
    void ledgerStatesOfNewerEngines()
    {
        QTemporaryDir root;
        auto game = [&](const QString& id) {
            const QString g = QDir::toNativeSeparators(root.filePath(QStringLiteral("Games/") + id));
            QDir().mkpath(g);
            return g;
        };
        auto journalAt = [&](const QString& id, const QByteArray& body) {
            const QString j = QDir::toNativeSeparators(root.filePath(QStringLiteral("bak/%1/APPLY_JOURNAL.json").arg(id)));
            writeFile(j, body);
            QFile f(j);
            if (f.open(QIODevice::ReadWrite))
                f.setFileTime(QDateTime::currentDateTime().addSecs(-2 * kApplyQuietSeconds), QFileDevice::FileModificationTime);
            return j;
        };
        const QString restoring = game(QStringLiteral("PPSA00001")), rollback = game(QStringLiteral("PPSA00002")),
                      leftNothing = game(QStringLiteral("PPSA00003")), applied = game(QStringLiteral("PPSA00004"));
        const QString gone = QDir::toNativeSeparators(root.filePath(QStringLiteral("Games/PPSA00005")));
        auto row = [](const QString& dir, const QString& state, const QString& journal) {
            return QJsonObject{{QStringLiteral("game_dir"), dir}, {QStringLiteral("state"), state},
                               {QStringLiteral("journal"), journal}, {QStringLiteral("started_at"), QStringLiteral("2026-11-17T10:00:00")}};
        };
        const QJsonArray rows{
            1, QStringLiteral("not a row"), QJsonValue(), // damaged rows are left out
            row(restoring, QStringLiteral("APPLIED"), journalAt(QStringLiteral("PPSA00001"), R"({"state":"RESTORING","worker_started":1.5})")),
            row(rollback, QStringLiteral("ROLLBACK_INCOMPLETE"), QDir::toNativeSeparators(root.filePath(QStringLiteral("bak/none/APPLY_JOURNAL.json")))),
            row(leftNothing, QStringLiteral("APPLIED"),
                journalAt(QStringLiteral("PPSA00003"), R"({"state":"ROLLBACK_INCOMPLETE","backport_files_left":false})")),
            row(applied, QStringLiteral("APPLIED"), journalAt(QStringLiteral("PPSA00004"), R"({"state":"APPLIED"})")),
            row(gone, QStringLiteral("APPLIED"), journalAt(QStringLiteral("PPSA00005"), R"({"state":"ROLLBACK_INCOMPLETE"})")),
        };
        QVERIFY(writeFile(folders().ledgerPath, QJsonDocument(QJsonObject{{QStringLiteral("applied"), rows}}).toJson()));
        const auto cleanup = qScopeGuard([] { QFile::remove(folders().ledgerPath); });

        QCOMPARE(readLedger().size(), 5); // the three that are not objects are gone
        QCOMPARE(activeLedgerRecord(restoring).value(QStringLiteral("state")).toString(), QStringLiteral("RESTORING")); // the journal's word
        QCOMPARE(activeLedgerRecord(rollback).value(QStringLiteral("state")).toString(), QStringLiteral("ROLLBACK_INCOMPLETE")); // no journal: the record's
        QVERIFY(activeLedgerRecord(leftNothing).isEmpty()); // the undo left none of the backport's files
        QCOMPARE(activeLedgerRecord(applied).value(QStringLiteral("state")).toString(), QStringLiteral("APPLIED"));
        QVERIFY(readGame(restoring).appliedRecord.value(QStringLiteral("state")).toString() == QLatin1String("RESTORING"));

        QStringList offered;
        bool newer = false;
        for (const QJsonObject& r : unfinishedApplies(kApplyQuietSeconds)) {
            offered << QFileInfo(r.value(QStringLiteral("game_dir")).toString()).fileName();
            if (r.value(QStringLiteral("game_dir")).toString() == restoring)
                newer = r.value(QStringLiteral("written_by_035")).toBool();
        }
        // RESTORING is offered; an unreadable journal, an undo that left nothing,
        // a finished apply and a folder that is gone are not.
        QCOMPARE(offered, QStringList{QStringLiteral("PPSA00001")});
        QVERIFY(newer);
    }

    // A record or settings file saved with a byte-order mark (an editor, the
    // standalone program 0.3.5 reads both kinds) is read, not taken as empty.
    void filesWithAByteOrderMarkAreRead()
    {
        const QString game = QDir::toNativeSeparators(m_data.filePath(QStringLiteral("bom-game")));
        QDir().mkpath(game);
        const QByteArray bom("\xEF\xBB\xBF");
        QVERIFY(writeFile(folders().ledgerPath,
                          bom + QJsonDocument(QJsonObject{{QStringLiteral("applied"),
                                                           QJsonArray{QJsonObject{{QStringLiteral("game_dir"), game},
                                                                                  {QStringLiteral("state"), QStringLiteral("APPLIED")}}}}})
                                    .toJson()));
        QCOMPARE(activeLedgerRecord(game).value(QStringLiteral("state")).toString(), QStringLiteral("APPLIED"));
        QFile::remove(folders().ledgerPath);
        QVERIFY(writeFile(folders().settingsPath, bom + R"({"scan_roots":["D:\\Bom"],"scan_depth":4})"));
        SettingsFile state = SettingsFile::Missing;
        const Settings s = loadSettings(&state);
        QCOMPARE(state, SettingsFile::Ok);
        QCOMPARE(s.scanRoots, QStringList{QStringLiteral("D:\\Bom")});
        QCOMPARE(s.scanDepth, 4);
        QFile::remove(folders().settingsPath);
    }

    // A param.json far larger than any real one (a damaged or crafted dump) is
    // not read into memory: the game is listed without its metadata, at once.
    void aHugeParamJsonIsNotRead()
    {
        QTemporaryDir root;
        const QString g = root.filePath(QStringLiteral("Some Game"));
        QVERIFY(writeFile(g + QStringLiteral("/eboot.bin"), "x"));
        QFile p(g + QStringLiteral("/sce_sys/param.json"));
        QDir().mkpath(QFileInfo(p).absolutePath());
        QVERIFY(p.open(QIODevice::WriteOnly));
        QVERIFY(p.write(R"({"titleId":"PPSA12345",)") > 0);
        QVERIFY(p.resize(9LL * 1024 * 1024)); // 9 MiB, the limit is 8
        p.close();
        QElapsedTimer t;
        t.start();
        const Game game = readGame(g);
        QVERIFY(game.titleId.isEmpty());
        QVERIFY2(t.elapsed() < 1000, qPrintable(QString::number(t.elapsed())));
    }

    // The worker refuses a Target SDK above the game's own; the page says it
    // before the run with this rule (0.3.4 worker.py, 0.3.5 gui.sdk_ceiling).
    void sdkCeilingRule()
    {
        QVERIFY(sdkCeiling(QStringLiteral("7.00"), 9) == std::optional<int>(7));
        QVERIFY(sdkCeiling(QStringLiteral("3.00"), 5) == std::optional<int>(4));
        QVERIFY(!sdkCeiling(QStringLiteral("7.00"), 7));
        QVERIFY(!sdkCeiling(QStringLiteral("7.00"), 4)); // SDK 4 is always allowed
        QVERIFY(!sdkCeiling(QStringLiteral("2.00"), 4));
        QVERIFY(!sdkCeiling(QString(), 13)); // unknown SDK: the worker decides
        QVERIFY(sdkCeiling(QStringLiteral("10.50"), 11) == std::optional<int>(10));
    }

    // 0.3.5's free-space rules: the backup needs 1.6x (it holds the loose
    // originals and their ZIP), and output and backup on one drive are
    // checked together.
    void preflightSpaceRules()
    {
        const SpaceNeed n = spaceNeed(1000LL * 1000 * 1000);
        QCOMPARE(n.output, qint64(2200LL * 1000 * 1000 + 50LL * 1024 * 1024));
        QCOMPARE(n.backup, qint64(1600LL * 1000 * 1000 + 20LL * 1024 * 1024));

        QTemporaryDir root;
        const QString game = root.filePath(QStringLiteral("game"));
        QVERIFY(writeFile(game + QStringLiteral("/eboot.bin"), "x")); // no executables: 50 MB + 20 MB needed
        const QString home = engineHome();
        setFreeSpaceProbeForTests([home](const QString& p) -> qint64 {
            if (QDir::cleanPath(p).startsWith(QDir::cleanPath(home), Qt::CaseInsensitive))
                return 100LL * 1000 * 1000 * 1000;
            return 60LL * 1024 * 1024; // each folder alone fits, the two together do not
        });
        const auto restore = qScopeGuard([] { setFreeSpaceProbeForTests({}); });
        const Preflight p = preflight(root.filePath(QStringLiteral("out")), root.filePath(QStringLiteral("bak")), {game});
        QVERIFY2(p.errors.join(u'|').contains(QStringLiteral("on the same drive and together need about")), qPrintable(p.errors.join(u'|')));
        setFreeSpaceProbeForTests([](const QString&) -> qint64 { return 100LL * 1000 * 1000 * 1000; });
        const Preflight ok = preflight(root.filePath(QStringLiteral("out")), root.filePath(QStringLiteral("bak")), {game});
        QVERIFY2(ok.errors.isEmpty(), qPrintable(ok.errors.join(u'|')));
    }

    // The standalone program shares the ledger: while one runs that this app
    // did not start, nothing is offered as "interrupted".
    void aForeignEngineProcessIsSeen()
    {
#ifndef Q_OS_WIN
        QSKIP("process list is read on Windows only");
#else
        // The check is by program name over the whole PC: with the real
        // program open it would hold before the stand-in starts.
        if (foreignEngineRunning())
            QSKIP("A program named PS5BackportGUI.exe is already running on this PC; close it to run this test.");
        QTemporaryDir root;
        const QString exe = root.filePath(QStringLiteral("PS5BackportGUI.exe"));
        QVERIFY(QFile::copy(fwtest::helperPath(), exe));
        // The helper starts a second copy through cmd.exe, which exits at once:
        // that copy has no living parent, as a program started by the user.
        // Both sleep until the test ends them (the second names itself in a file).
        const QString pidFile = root.filePath(QStringLiteral("second.pid"));
        QProcess own;
        const auto endBoth = qScopeGuard([&] {
            QFile f(pidFile);
            if (f.open(QIODevice::ReadOnly))
                fwtest::endProcess(f.readAll().trimmed().toLongLong());
            own.kill();
            own.waitForFinished(10000);
            // The folder (with the program in it) can only go once both have ended.
            (void)QTest::qWaitFor([] { return !foreignEngineRunning(); }, 15000);
        });
        own.start(exe, {QStringLiteral("spawn-sleep"), QStringLiteral("120000"), QDir::toNativeSeparators(pidFile)});
        QVERIFY(own.waitForStarted(10000));
        QVERIFY(own.waitForReadyRead(10000)); // "spawned"
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(pidFile), 30000);
        QTRY_VERIFY_WITH_TIMEOUT(foreignEngineRunning(), 30000);
        const QString journal = root.filePath(QStringLiteral("APPLY_JOURNAL.json"));
        QVERIFY(writeFile(journal, R"({"state":"IN_PROGRESS"})"));
        {
            QFile jf(journal);
            QVERIFY(jf.open(QIODevice::ReadWrite));
            QVERIFY(jf.setFileTime(QDateTime::currentDateTime().addSecs(-2 * kApplyQuietSeconds), QFileDevice::FileModificationTime));
        }
        QVERIFY(writeFile(folders().ledgerPath,
                  QJsonDocument(QJsonObject{{QStringLiteral("applied"),
                                             QJsonArray{QJsonObject{{QStringLiteral("state"), QStringLiteral("IN_PROGRESS")},
                                                                    {QStringLiteral("journal"), QDir::toNativeSeparators(journal)}}}}})
                      .toJson()));
        QCOMPARE(unfinishedApplies(kApplyQuietSeconds).size(), size_t(1));
        QVERIFY(interruptedApplies().empty()); // not offered while that program runs
        QFile::remove(folders().ledgerPath);
#endif
    }

    // A job file left by a killed app is removed once its writer is gone.
    void staleJobFilesAreSwept()
    {
        QVERIFY(ensureFolders());
        const QString jobs = folders().jobsDir;
        const QString stale = QDir(jobs).filePath(QStringLiteral("job_4_1700000000000.json")); // PID 4 is never a user process
        const QString fresh = QDir(jobs).filePath(QStringLiteral("job_4_1700000000001.json"));
        const QString mine = QDir(jobs).filePath(QStringLiteral("job_%1_1700000000002.json").arg(QCoreApplication::applicationPid()));
        const QString dead = QDir(jobs).filePath(QStringLiteral("job_999999996_1700000000003.json"));
        for (const QString& p : {stale, fresh, mine, dead})
            QVERIFY(writeFile(p, "{}"));
        const QDateTime old = QDateTime::currentDateTime().addDays(-3);
        for (const QString& p : {stale, mine, dead}) {
            QFile f(p);
            QVERIFY(f.open(QIODevice::ReadWrite));
            QVERIFY(f.setFileTime(old, QFileDevice::FileModificationTime));
        }
        // Any program will do as the "engine" here: the sweep runs before the
        // worker is started, and the helper just exits.
        const QByteArray engineBefore = qgetenv("FW_BACKPORT_ENGINE");
        qputenv("FW_BACKPORT_ENGINE", QDir::toNativeSeparators(fwtest::helperPath()).toUtf8());
        Worker w;
        bool finished = false;
        QObject::connect(&w, &Worker::finished, [&finished](const QJsonObject&) { finished = true; });
        QString err;
        const bool started = w.start(QJsonObject{{QStringLiteral("cmd"), QStringLiteral("selftest")}}, &err);
        if (engineBefore.isEmpty())
            qunsetenv("FW_BACKPORT_ENGINE");
        else
            qputenv("FW_BACKPORT_ENGINE", engineBefore);
        QVERIFY2(started, qPrintable(err));
        QTRY_VERIFY_WITH_TIMEOUT(finished, 30000);
        QVERIFY(!QFileInfo::exists(dead));  // old, and no such process
        QVERIFY(QFileInfo::exists(stale));  // old, but a process with that id is alive
        QVERIFY(QFileInfo::exists(fresh));  // not a day old
        QVERIFY(QFileInfo::exists(mine));   // its process (this test) is alive
        for (const QString& p : {stale, fresh, mine, dead})
            QFile::remove(p);
    }

    // H1 (settled, fake engine): a worker that outlives the app (orphaned
    // during its export or apply step) has lost the reader of its standard
    // output, and its next progress line fails at once. The engine's own emit()
    // (worker.py, 0.3.4 and 0.3.5-test4 alike) does not catch that error, so
    // the job raises in whatever step it was in: the host cannot prevent it
    // once it is gone, only wait for the worker (MainWindow's close).
    void anOrphanedWorkersNextLineFails()
    {
        QTemporaryDir root;
        const QString record = root.filePath(QStringLiteral("writer.txt"));
        QProcess host; // stands in for the app: reads one line, then exits without closing its end
        host.start(fwtest::helperPath(), {QStringLiteral("orphan"), QStringLiteral("500"), QDir::toNativeSeparators(record)});
        QVERIFY(host.waitForFinished(30000));
        QCOMPARE(host.exitCode(), 0);
        QVERIFY2(host.readAllStandardOutput().startsWith("read {\"event\":\"progress\""), "the writer's first line reached its reader");
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(record), 20000); // the writer ended (500 lines take 10 s)
        QTest::qWait(100);
        QFile f(record);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray said = f.readAll();
        qInfo().noquote() << "orphaned writer:" << said.trimmed();
        QVERIFY2(said.startsWith("write failed at "), said.constData());
        QVERIFY(said.mid(16).split(' ').first().toInt() < 100); // at once, not after the pipe's buffer filled
    }

    void missingEngine()
    {
        if (!m_engine.isEmpty())
            QSKIP("FW_BACKPORT_ENGINE is set");
        // Asked before anything is started: with an engine in reach the
        // start below would run it.
        if (!engineExecutable().isEmpty())
            QSKIP("an engine is installed next to the test");
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
        QVERIFY(!mods.isEmpty());
        for (auto it = mods.begin(); it != mods.end(); ++it)
            QVERIFY2(it.value().toBool(), qPrintable(it.key()));
        QDir jobs(folders().jobsDir);
        QVERIFY(jobs.entryList(QDir::Files).isEmpty()); // the job file was removed
        qInfo() << "engine revision" << r.result.value(QStringLiteral("revision")).toString() << "pinned" << kEngineVersion << kEngineRevision;
    }

    // The worker protocol of the bundled engine on a synthetic game (no game
    // data): every key this host sends is accepted, including the optional
    // offline_mode of 0.3.5, progress lines carry stage / pct / phase, one
    // result line ends the job, and the game folder is left as it was (Apply
    // off). Whether the engine builds or refuses the stand-in is its own call.
    void engineProtocolOnASyntheticGame()
    {
        if (m_engine.isEmpty())
            QSKIP("set FW_BACKPORT_ENGINE");
        QTemporaryDir root;
        const QString game = QDir::toNativeSeparators(root.filePath(QStringLiteral("games/PPSA99999 Synthetic Game")));
        QByteArray elf("\x7f" "ELF\x02\x01\x01", 7);
        elf += QByteArray(4096 - elf.size(), '\0');
        QVERIFY(writeFile(game + QStringLiteral("/eboot.bin"), elf));
        QVERIFY(writeFile(game + QStringLiteral("/sce_sys/param.json"),
                          R"({"titleId":"PPSA99999","contentVersion":"01.000.000","sdkVersion":"0x0700000000000000",)"
                          R"("requiredSystemSoftwareVersion":"0x0700000000000000",)"
                          R"("localizedParameters":{"defaultLanguage":"en-US","en-US":{"titleName":"Synthetic Game"}}})"));
        auto snapshot = [&] {
            QStringList s;
            QDirIterator it(game, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                const QFileInfo fi = it.nextFileInfo();
                auto h = fw::sha256File(fi.absoluteFilePath(), {});
                s << QStringLiteral("%1 %2").arg(QDir(game).relativeFilePath(fi.absoluteFilePath()), h.ok() ? h.value() : QStringLiteral("?"));
            }
            s.sort();
            return s;
        };
        const QStringList before = snapshot();
        const Game g = readGame(game);
        QCOMPARE(g.sdk, QStringLiteral("7.00"));
        const Run r = runJob({{QStringLiteral("cmd"), QStringLiteral("backport")},
                              {QStringLiteral("game"), QJsonObject{{QStringLiteral("path"), g.path}, {QStringLiteral("title_id"), g.titleId},
                                                                    {QStringLiteral("title"), g.title}, {QStringLiteral("version"), g.version},
                                                                    {QStringLiteral("sdk"), g.sdk}, {QStringLiteral("fw"), g.fw}}},
                              {QStringLiteral("output_root"), QDir::toNativeSeparators(root.filePath(QStringLiteral("out")))},
                              {QStringLiteral("backup_root"), QDir::toNativeSeparators(root.filePath(QStringLiteral("bak")))},
                              {QStringLiteral("options"), QJsonObject{{QStringLiteral("apply"), false}, {QStringLiteral("zip"), true},
                                                                      {QStringLiteral("target_sdk"), 4}, {QStringLiteral("pro_fix"), false},
                                                                      {QStringLiteral("pssr2"), false}, {QStringLiteral("offline_mode"), true}}}},
                             600000);
        QVERIFY(r.finished);
        const QString status = r.result.value(QStringLiteral("status")).toString();
        qInfo().noquote() << "synthetic game:" << status << r.result.value(QStringLiteral("headline")).toString() << "|" << r.progress.size()
                          << "progress lines | result keys:" << r.result.keys().join(u',');
        for (const QJsonObject& ev : r.progress) {
            QVERIFY2(ev.value(QStringLiteral("stage")).isString(), qPrintable(QJsonDocument(ev).toJson(QJsonDocument::Compact)));
            QVERIFY2(ev.value(QStringLiteral("pct")).isDouble(), qPrintable(QJsonDocument(ev).toJson(QJsonDocument::Compact)));
        }
        const QStringList known{QStringLiteral("DONE"), QStringLiteral("PARTIAL"), QStringLiteral("FAILED"), QStringLiteral("CANCELLED"),
                                QStringLiteral("NO_CHANGE")};
        QVERIFY2(known.contains(status), qPrintable(status));
        QVERIFY(!r.result.value(QStringLiteral("headline")).toString().isEmpty());
        QVERIFY2(!r.result.value(QStringLiteral("headline")).toString().contains(QStringLiteral("unknown job")), "a key was refused");
        qInfo().noquote() << "options the engine recorded:" << QJsonDocument(r.result.value(QStringLiteral("options")).toObject()).toJson(QJsonDocument::Compact);
        if (r.result.contains(QStringLiteral("options")))
            QVERIFY(r.result.value(QStringLiteral("options")).toObject().value(QStringLiteral("offline_mode")).toBool());
        QCOMPARE(snapshot(), before); // Apply off: the game folder is untouched
        QVERIFY(QDir(folders().jobsDir).entryList(QDir::Files).isEmpty());
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
        FW_NEED_GAME_COPY();
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
        FW_NEED_GAME_COPY();
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
