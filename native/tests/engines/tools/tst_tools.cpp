// Tools module: Language Stripper detection, verified backup + removal,
// restore, safety refusals, and the problem-report bundle with redaction.

#include "core/Zip.h"
#include "engines/tools/LanguageStripper.h"
#include "engines/tools/Module.h"
#include "engines/tools/ReportBundle.h"
#include "support/TestSupport.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

using namespace fw;
using namespace fw::tools;

namespace {
QByteArray readFile(const QString& p)
{
    QFile f(p);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

// The error of a call that must have failed (a marker error, not undefined
// behaviour, when it succeeded).
template <typename R>
Error errorOf(const R& result)
{
    return result.ok() ? Error(ErrorCode::Internal, QStringLiteral("<the call succeeded>")) : result.error();
}
} // namespace

class TstTools : public QObject {
    Q_OBJECT
    QTemporaryDir m_root;

    QString makeGame(const QString& name)
    {
        const QString g = m_root.filePath(name);
        const auto w = [&](const QString& rel, const QByteArray& data) { fwtest::writeTextFile(g + u'/' + rel, data); };
        w(QStringLiteral("eboot.bin"), "ELF");
        w(QStringLiteral("sce_sys/param.json"), "{}");
        w(QStringLiteral("sce_sys/ja/strings.txt"), "system ja"); // inside sce_sys: never touched
        w(QStringLiteral("data/en-US/text.bin"), QByteArray(1000, 'e'));
        w(QStringLiteral("data/ja-JP/text.bin"), QByteArray(2000, 'j'));
        w(QStringLiteral("data/zh-Hant/text.bin"), QByteArray(300, 'z'));
        w(QStringLiteral("audio/en/vo.bin"), QByteArray(500, 'E'));
        w(QStringLiteral("audio/ja/vo.bin"), QByteArray(700, 'J'));
        w(QStringLiteral("ui/menu.bin"), "ui");                       // "ui" is not a language
        w(QStringLiteral("Content/Paks/Game_lang_ja.pak"), QByteArray(4000, 'p'));
        w(QStringLiteral("Content/Paks/Game_lang_ja.sig"), "sig");
        w(QStringLiteral("Content/Paks/Game.pak"), "core");
        w(QStringLiteral("streams/audio_fr.psarc"), QByteArray(600, 'f'));
        w(QStringLiteral("movies/intro_de.mp4"), QByteArray(800, 'd'));
        w(QStringLiteral("sound/vo_es.bnk"), QByteArray(900, 's'));
        return g;
    }

private slots:
    void initTestCase() { QVERIFY(m_root.isValid()); fwtest::isolateAppData(m_root); }

    void detectsLanguagesLikeTheLegacyRules()
    {
        const QString g = makeGame(QStringLiteral("GameA"));
        auto scan = scanLanguages(g, CancellationToken());
        QVERIFY(scan.ok());
        QStringList paths;
        for (const auto& i : scan->items)
            paths << i.relativePath + u'=' + i.code;
        QCOMPARE(paths, (QStringList{
                             QStringLiteral("audio/en=en"), QStringLiteral("audio/ja=ja"),
                             QStringLiteral("Content/Paks/Game_lang_ja.pak=ja"), QStringLiteral("Content/Paks/Game_lang_ja.sig=ja"),
                             QStringLiteral("data/en-US=en"), QStringLiteral("data/ja-JP=ja"), QStringLiteral("data/zh-Hant=zh"),
                             QStringLiteral("movies/intro_de.mp4=de"), QStringLiteral("sound/vo_es.bnk=es"),
                             QStringLiteral("streams/audio_fr.psarc=fr")}));
        QCOMPARE(scan->codes(), (QStringList{QStringLiteral("de"), QStringLiteral("en"), QStringLiteral("es"), QStringLiteral("fr"),
                                             QStringLiteral("ja"), QStringLiteral("zh")}));
        const auto it = std::find_if(scan->items.begin(), scan->items.end(), [](const LanguageItem& i) { return i.relativePath == u"data/en-US"; });
        QVERIFY(it != scan->items.end());
        QCOMPARE(it->language, QStringLiteral("English (US)"));
        QCOMPARE(it->bytes, qint64(1000));
        // JSON round trip
        QCOMPARE(int(LanguageScan::fromJson(scan->toJson()).items.size()), int(scan->items.size()));
    }

    void batchFindsGamesOneLevelDown()
    {
        makeGame(QStringLiteral("lib/G1"));
        makeGame(QStringLiteral("lib/sub/G2"));
        fwtest::writeTextFile(m_root.filePath(QStringLiteral("lib/notes.txt")), "x");
        const QStringList roots = findGameRoots(m_root.filePath(QStringLiteral("lib")));
        QCOMPARE(roots.size(), 2);
    }

    void stripBacksUpVerifiesRemovesAndRestores()
    {
        const QString g = makeGame(QStringLiteral("GameB"));
        const QByteArray jaPak = readFile(g + QStringLiteral("/Content/Paks/Game_lang_ja.pak"));
        StripPlan plan;
        plan.gameRoot = g;
        plan.remove = {QStringLiteral("data/ja-JP"), QStringLiteral("audio/ja"), QStringLiteral("Content/Paks/Game_lang_ja.pak"),
                       QStringLiteral("Content/Paks/Game_lang_ja.sig")};
        plan.backupFolder = m_root.filePath(QStringLiteral("backups"));
        QDir().mkpath(plan.backupFolder);
        ProgressReporter progress;
        auto r = stripLanguages(plan, CancellationToken(), &progress);
        QVERIFY2(r.ok(), r.ok() ? "" : qPrintable(r.error().toLogString()));
        QCOMPARE(r->removed, 4);
        QVERIFY(r->failed.isEmpty());
        QCOMPARE(r->bytesFreed, qint64(2000 + 700 + 4000 + 3));
        QVERIFY(!QFileInfo::exists(g + QStringLiteral("/data/ja-JP")));
        QVERIFY(!QFileInfo::exists(g + QStringLiteral("/Content/Paks/Game_lang_ja.pak")));
        QVERIFY(QFileInfo::exists(g + QStringLiteral("/data/en-US/text.bin")));
        QVERIFY(QFileInfo::exists(g + QStringLiteral("/sce_sys/ja/strings.txt")));
        zip::Reader z;
        QVERIFY(z.open(r->backupZip).ok());
        QVERIFY(z.find(QStringLiteral("data/ja-JP/text.bin")));
        QVERIFY(z.find(QStringLiteral("farewell-language-strip.json")));

        auto restored = restoreLanguages(r->backupZip, g, CancellationToken(), nullptr);
        QVERIFY(restored.ok());
        QCOMPARE(restored.value(), 4);
        QCOMPARE(readFile(g + QStringLiteral("/Content/Paks/Game_lang_ja.pak")), jaPak);
        QCOMPARE(readFile(g + QStringLiteral("/audio/ja/vo.bin")), QByteArray(700, 'J'));
        QVERIFY(!QFileInfo::exists(g + QStringLiteral("/farewell-language-strip.json")));
    }

    void stripRefusesUnsafePlans()
    {
        const QString g = makeGame(QStringLiteral("GameC"));
        StripPlan base;
        base.gameRoot = g;
        base.remove = {QStringLiteral("audio/ja")};
        base.backupFolder = m_root.filePath(QStringLiteral("backups2"));
        QDir().mkpath(base.backupFolder);

        auto inside = base;
        inside.backupFolder = g + QStringLiteral("/bk");
        QVERIFY(!stripLanguages(inside, CancellationToken(), nullptr).ok());
        auto noBackup = base;
        noBackup.backupFolder.clear();
        QVERIFY(!stripLanguages(noBackup, CancellationToken(), nullptr).ok());
        auto system = base;
        system.remove = {QStringLiteral("sce_sys/ja")};
        QVERIFY(!stripLanguages(system, CancellationToken(), nullptr).ok());
        // Items outside the game: the files exist (so that is not why the plan
        // is refused), and they are still there afterwards.
        const QString outsideFile = m_root.filePath(QStringLiteral("outside-c/audio/ja/keep.bin"));
        QVERIFY(fwtest::writeTextFile(outsideFile, "not part of the game"));
        for (const QString& item : {QStringLiteral("../outside-c/audio/ja"), QStringLiteral("audio/../../outside-c/audio/ja"),
                                    QDir::toNativeSeparators(QFileInfo(outsideFile).absolutePath()),
                                    QFileInfo(outsideFile).absolutePath()}) {
            auto escape = base;
            escape.remove = {item};
            const auto refused = stripLanguages(escape, CancellationToken(), nullptr);
            QVERIFY2(!refused.ok(), qPrintable(item));
            QVERIFY2(errorOf(refused).message.contains(QStringLiteral("is not inside the game folder")), qPrintable(item));
            QCOMPARE(readFile(outsideFile), QByteArray("not part of the game"));
        }
        auto notGame = base;
        notGame.gameRoot = m_root.filePath(QStringLiteral("backups2"));
        QVERIFY(!stripLanguages(notGame, CancellationToken(), nullptr).ok());
        // Nothing was removed by any refused plan.
        QVERIFY(QFileInfo::exists(g + QStringLiteral("/audio/ja/vo.bin")));
        QVERIFY(QFileInfo::exists(g + QStringLiteral("/sce_sys/ja/strings.txt")));
        QVERIFY(QDir(base.backupFolder).entryList(QDir::Files).isEmpty());
    }

    void cancelledStripRemovesNothing()
    {
        const QString g = makeGame(QStringLiteral("GameD"));
        StripPlan plan;
        plan.gameRoot = g;
        plan.remove = {QStringLiteral("audio/ja")};
        plan.backupFolder = m_root.filePath(QStringLiteral("backups3"));
        QDir().mkpath(plan.backupFolder);
        CancellationSource cs;
        cs.cancel();
        auto r = stripLanguages(plan, cs.token(), nullptr);
        QVERIFY(!r.ok() && r.error().isCancelled());
        QVERIFY(QFileInfo::exists(g + QStringLiteral("/audio/ja/vo.bin")));
        QVERIFY(QDir(plan.backupFolder).entryList(QDir::Files).isEmpty());
    }

    // A backup that cannot be read back is no backup: nothing is removed.
    void damagedBackupRemovesNothing()
    {
        const QString g = makeGame(QStringLiteral("GameL"));
        StripPlan plan;
        plan.gameRoot = g;
        plan.remove = {QStringLiteral("data/ja-JP"), QStringLiteral("audio/ja")};
        plan.backupFolder = m_root.filePath(QStringLiteral("backups-l"));
        QDir().mkpath(plan.backupFolder);
        bool damaged = false;
        // The backup is written and its check is about to start: one byte of a
        // stored file changes on the disk.
        ProgressReporter progress([&](const ProgressSnapshot& p) {
            if (p.phaseIndex < 1 || damaged)
                return;
            const QStringList backups = QDir(plan.backupFolder).entryList(QDir::Files);
            if (backups.size() != 1)
                return;
            QFile f(QDir(plan.backupFolder).filePath(backups.first()));
            if (!f.open(QIODevice::ReadWrite))
                return;
            const qint64 at = f.readAll().indexOf(QByteArray(64, 'J'));
            damaged = at > 0 && f.seek(at + 10) && f.write("x") == 1;
        }, 0);
        const auto r = stripLanguages(plan, CancellationToken(), &progress);
        QVERIFY2(damaged, "the backup was not found when its check began");
        QCOMPARE(errorOf(r).code, ErrorCode::VerificationFailed);
        QCOMPARE(readFile(g + QStringLiteral("/audio/ja/vo.bin")), QByteArray(700, 'J'));
        QCOMPARE(readFile(g + QStringLiteral("/data/ja-JP/text.bin")), QByteArray(2000, 'j'));
        // What could not be verified is not left behind as if it were a backup.
        QVERIFY(QDir(plan.backupFolder).entryList(QDir::Files).isEmpty());
    }

    // Removal acts on what the verified backup holds: a file that changed, or
    // appeared, after the backup pass stays where it is.
    void stripLeavesWhatChangedOrAppearedAfterTheBackup()
    {
        const QString g = makeGame(QStringLiteral("GameE"));
        fwtest::writeTextFile(g + QStringLiteral("/data/ja-JP/sub/more.bin"), QByteArray(50, 'm'));
        fwtest::writeTextFile(g + QStringLiteral("/data/ja-JP/sub/still-being-written.bin"), QByteArray(60, 'w'));
        StripPlan plan;
        plan.gameRoot = g;
        plan.remove = {QStringLiteral("data/ja-JP"), QStringLiteral("audio/ja")};
        plan.backupFolder = m_root.filePath(QStringLiteral("backups-e"));
        QDir().mkpath(plan.backupFolder);
        bool interfered = false;
        // Another program writes into the folder once the backup pass is over.
        ProgressReporter progress([&](const ProgressSnapshot& p) {
            if (p.phaseIndex < 1 || interfered)
                return;
            interfered = true;
            fwtest::writeTextFile(g + QStringLiteral("/data/ja-JP/sub/still-being-written.bin"), QByteArray(90, 'W'));
            fwtest::writeTextFile(g + QStringLiteral("/data/ja-JP/new-save.dat"), "arrived late");
        }, 0);
        auto r = stripLanguages(plan, CancellationToken(), &progress);
        QVERIFY2(r.ok(), r.ok() ? "" : qPrintable(r.error().toLogString()));
        QVERIFY(interfered);
        // Neither file is in the backup in this form, so neither was deleted.
        QCOMPARE(readFile(g + QStringLiteral("/data/ja-JP/sub/still-being-written.bin")), QByteArray(90, 'W'));
        QCOMPARE(readFile(g + QStringLiteral("/data/ja-JP/new-save.dat")), QByteArray("arrived late"));
        // Everything that was backed up unchanged is gone.
        QVERIFY(!QFileInfo::exists(g + QStringLiteral("/data/ja-JP/text.bin")));
        QVERIFY(!QFileInfo::exists(g + QStringLiteral("/data/ja-JP/sub/more.bin")));
        QVERIFY(!QFileInfo::exists(g + QStringLiteral("/audio/ja")));
        QCOMPARE(r->removed, 1);
        QCOMPARE(r->failed, QStringList{QStringLiteral("data/ja-JP")});
        QVERIFY(r->leftInPlace.contains(QStringLiteral("data/ja-JP/sub/still-being-written.bin")));
        QVERIFY(r->leftInPlace.contains(QStringLiteral("data/ja-JP/new-save.dat")));
        QCOMPARE(r->bytesFreed, qint64(2000 + 50 + 700));
        zip::Reader z;
        QVERIFY(z.open(r->backupZip).ok());
        QCOMPARE(z.readAll(*z.find(QStringLiteral("data/ja-JP/sub/still-being-written.bin"))).value(), QByteArray(60, 'w'));
        QVERIFY(!z.find(QStringLiteral("data/ja-JP/new-save.dat")));
    }

    // Restore puts back only what a valid manifest lists, and says what it skipped.
    void restoreValidatesTheManifestAndNeverOverwrites()
    {
        const QString g = makeGame(QStringLiteral("GameF"));
        const QString zips = m_root.filePath(QStringLiteral("crafted"));
        QDir().mkpath(zips);
        const auto makeZip = [&](const QString& name, const QByteArray& manifest,
                                 const std::vector<std::pair<QString, QByteArray>>& files) {
            const QString path = zips + u'/' + name;
            zip::Writer w;
            if (!w.open(path).ok())
                return QString();
            for (const auto& [n, data] : files)
                if (!w.addData(n, data).ok())
                    return QString();
            if (!manifest.isNull() && !w.addData(QStringLiteral("farewell-language-strip.json"), manifest).ok())
                return QString();
            return w.finish().ok() ? path : QString();
        };
        const auto refused = [&](const QString& zip) {
            auto r = restoreLanguageBackup(zip, g, CancellationToken(), nullptr);
            return !zip.isEmpty() && !r.ok();
        };
        const QByteArray payload("payload");
        // No manifest at all, a manifest that is not JSON, one without a list.
        QVERIFY(refused(makeZip(QStringLiteral("none.zip"), QByteArray(), {{QStringLiteral("audio/xx/a.bin"), payload}})));
        QVERIFY(refused(makeZip(QStringLiteral("garbage.zip"), "not json", {{QStringLiteral("audio/xx/a.bin"), payload}})));
        QVERIFY(refused(makeZip(QStringLiteral("nolist.zip"), "{\"game\":\"GameF\"}", {{QStringLiteral("audio/xx/a.bin"), payload}})));
        QVERIFY(refused(makeZip(QStringLiteral("badlist.zip"), "{\"game\":\"GameF\",\"removed\":[7]}", {})));
        // A file the manifest does not cover (here a library the game would load).
        QVERIFY(refused(makeZip(QStringLiteral("extra.zip"), "{\"game\":\"GameF\",\"removed\":[\"audio/xx\"]}",
                                {{QStringLiteral("audio/xx/a.bin"), payload}, {QStringLiteral("fakelib/libc.prx"), payload}})));
        QVERIFY(refused(makeZip(QStringLiteral("prefix.zip"), "{\"game\":\"GameF\",\"removed\":[\"audio/xx\"]}",
                                {{QStringLiteral("audio/xxl/a.bin"), payload}})));
        // A manifest that lists what the stripper never removes.
        QVERIFY(refused(makeZip(QStringLiteral("system.zip"), "{\"game\":\"GameF\",\"removed\":[\"sce_module\"]}",
                                {{QStringLiteral("sce_module/libx.prx"), payload}})));
        QVERIFY(refused(makeZip(QStringLiteral("eboot.zip"), "{\"game\":\"GameF\",\"removed\":[\"eboot.bin\"]}", {})));
        QVERIFY(refused(makeZip(QStringLiteral("escape.zip"), "{\"game\":\"GameF\",\"removed\":[\"../GameA/audio\"]}", {})));
        // File names that leave the game folder, under an item the manifest lists.
        const QString landing = m_root.filePath(QStringLiteral("restore-escape"));
        const QByteArray listed("{\"game\":\"GameF\",\"removed\":[\"audio/xx\"]}");
        QVERIFY(refused(makeZip(QStringLiteral("dotdot.zip"), listed,
                                {{QStringLiteral("audio/xx/a.bin"), payload},
                                 {QStringLiteral("audio/xx/../../../restore-escape/up.bin"), payload}})));
        QVERIFY(refused(makeZip(QStringLiteral("absolute.zip"), listed,
                                {{QStringLiteral("audio/xx/a.bin"), payload}, {landing + QStringLiteral("/absolute.bin"), payload}})));
        QVERIFY(refused(makeZip(QStringLiteral("rooted.zip"), listed,
                                {{QStringLiteral("audio/xx/a.bin"), payload}, {QStringLiteral("/restore-escape/rooted.bin"), payload}})));
        QVERIFY(refused(makeZip(QStringLiteral("backslash.zip"), listed,
                                {{QStringLiteral("audio/xx/a.bin"), payload},
                                 {QStringLiteral("audio\\xx\\..\\..\\..\\restore-escape\\back.bin"), payload}})));
        QVERIFY(!QFileInfo::exists(landing));
        QVERIFY(!QFileInfo::exists(g + QStringLiteral("/restore-escape")));
        // Refused means nothing was written.
        QVERIFY(!QFileInfo::exists(g + QStringLiteral("/audio/xx")));
        QVERIFY(!QFileInfo::exists(g + QStringLiteral("/audio/xxl")));
        QVERIFY(!QFileInfo::exists(g + QStringLiteral("/fakelib")));
        QVERIFY(!QFileInfo::exists(g + QStringLiteral("/sce_module")));

        // A real backup from another game: restored, existing files kept and reported.
        const QString other = makeGame(QStringLiteral("GameG"));
        StripPlan plan;
        plan.gameRoot = other;
        plan.remove = {QStringLiteral("audio/ja"), QStringLiteral("data/ja-JP")};
        plan.backupFolder = m_root.filePath(QStringLiteral("backups-g"));
        QDir().mkpath(plan.backupFolder);
        auto strip = stripLanguages(plan, CancellationToken(), nullptr);
        QVERIFY2(strip.ok(), strip.ok() ? "" : qPrintable(strip.error().toLogString()));
        QVERIFY(QFile::remove(g + QStringLiteral("/audio/ja/vo.bin")));
        fwtest::writeTextFile(g + QStringLiteral("/data/ja-JP/text.bin"), "mine, different");
        auto r = restoreLanguageBackup(strip->backupZip, g, CancellationToken(), nullptr);
        QVERIFY2(r.ok(), r.ok() ? "" : qPrintable(r.error().toLogString()));
        QCOMPARE(r->restored, 1);
        QCOMPARE(r->skippedExisting, QStringList{QStringLiteral("data/ja-JP/text.bin")});
        QCOMPARE(r->backupGame, QStringLiteral("GameG"));
        QVERIFY(r->otherGame);
        QCOMPARE(readFile(g + QStringLiteral("/data/ja-JP/text.bin")), QByteArray("mine, different"));
        QCOMPARE(readFile(g + QStringLiteral("/audio/ja/vo.bin")), QByteArray(700, 'J'));
        // Back into its own game: same name, nothing skipped; time stamps come back too.
        auto own = restoreLanguageBackup(strip->backupZip, other, CancellationToken(), nullptr);
        QVERIFY(own.ok());
        QVERIFY(!own->otherGame);
        QCOMPARE(own->restored, 2);
        QVERIFY(own->skippedExisting.isEmpty());
    }

    // Folder names that only look like language codes are not offered for removal.
    void lookAlikeFoldersAreNotLanguages()
    {
        const QString g = m_root.filePath(QStringLiteral("GameH"));
        const auto w = [&](const QString& rel) { fwtest::writeTextFile(g + u'/' + rel, "data"); };
        w(QStringLiteral("eboot.bin"));
        w(QStringLiteral("gfx/bg/sky.bin"));        // backgrounds, not Bulgarian
        w(QStringLiteral("gfx/hd-Glow/a.bin"));     // not a language with a script name
        w(QStringLiteral("gfx/fx-123/a.bin"));
        w(QStringLiteral("tables/id/table.bin"));   // ids, not Indonesian
        w(QStringLiteral("world/it/items.bin"));
        w(QStringLiteral("loc/en/a.bin"));          // next to another language folder
        w(QStringLiteral("loc/fr/a.bin"));
        w(QStringLiteral("text/it/a.bin"));         // alone, but in a language container
        w(QStringLiteral("pack/hi-IN/a.bin"));      // a region spelling of a language the list does not know
        w(QStringLiteral("pack/es-419/a.bin"));
        auto scan = scanLanguages(g, CancellationToken());
        QVERIFY(scan.ok());
        QStringList paths;
        for (const auto& i : scan->items)
            paths << i.relativePath;
        QCOMPARE(paths, (QStringList{QStringLiteral("loc/en"), QStringLiteral("loc/fr"), QStringLiteral("pack/es-419"),
                                     QStringLiteral("pack/hi-IN"), QStringLiteral("text/it")}));
    }

    void cancelWhileCheckingTheBackupIsACancel()
    {
        const QString g = makeGame(QStringLiteral("GameI"));
        StripPlan plan;
        plan.gameRoot = g;
        plan.remove = {QStringLiteral("audio/ja")};
        plan.backupFolder = m_root.filePath(QStringLiteral("backups-i"));
        QDir().mkpath(plan.backupFolder);
        CancellationSource cs;
        ProgressReporter progress([&](const ProgressSnapshot& p) {
            if (p.phaseIndex >= 1)
                cs.cancel();
        }, 0);
        auto r = stripLanguages(plan, cs.token(), &progress);
        QVERIFY(!r.ok());
        QVERIFY2(errorOf(r).isCancelled(), qPrintable(errorOf(r).toLogString()));
        QVERIFY(QFileInfo::exists(g + QStringLiteral("/audio/ja/vo.bin")));
        QVERIFY(QDir(plan.backupFolder).entryList(QDir::Files).isEmpty());
    }

    void restoreKeepsTheOriginalFileTimes()
    {
        const QString g = makeGame(QStringLiteral("GameJ"));
        const QString file = g + QStringLiteral("/audio/ja/vo.bin");
        const QDateTime old = QDateTime(QDate(2021, 3, 4), QTime(5, 6, 8));
        {
            QFile f(file);
            QVERIFY(f.open(QIODevice::ReadWrite));
            QVERIFY(f.setFileTime(old, QFileDevice::FileModificationTime));
        }
        StripPlan plan;
        plan.gameRoot = g;
        plan.remove = {QStringLiteral("audio/ja")};
        plan.backupFolder = m_root.filePath(QStringLiteral("backups-j"));
        QDir().mkpath(plan.backupFolder);
        auto r = stripLanguages(plan, CancellationToken(), nullptr);
        QVERIFY2(r.ok(), r.ok() ? "" : qPrintable(r.error().toLogString()));
        QVERIFY(!QFileInfo::exists(file));
        auto back = restoreLanguageBackup(r->backupZip, g, CancellationToken(), nullptr);
        QVERIFY(back.ok());
        // (ZIP time stamps have a two-second step.)
        QVERIFY2(qAbs(QFileInfo(file).lastModified().secsTo(old)) <= 2, qPrintable(QFileInfo(file).lastModified().toString(Qt::ISODate)));
    }

    // ---- the jobs ---------------------------------------------------------------------
    void jobsReportPartialWorkTruthfully()
    {
        auto schema = std::make_shared<SettingsSchema>();
        DependencyRegistry deps;
        JobFactory jobs;
        ModuleContext mc{*schema, deps, jobs};
        registerModule(mc);
        const SettingsValues settings(schema, {}, {});
        const auto context = [&](ProgressReporter& progress) {
            JobContext ctx;
            ctx.jobId = QStringLiteral("t");
            ctx.progress = &progress;
            ctx.log = Log(QStringLiteral("test"));
            ctx.settings = settings;
            ctx.tempDir = m_root.filePath(QStringLiteral("job-temp"));
            return ctx;
        };

        // A backup folder inside the game is refused before it is created there.
        const QString g = makeGame(QStringLiteral("GameK"));
        {
            auto job = jobs.create(QStringLiteral("tools.languages.strip"),
                                   {{QStringLiteral("gameRoot"), g},
                                    {QStringLiteral("remove"), QJsonArray{QStringLiteral("audio/ja")}},
                                    {QStringLiteral("backupFolder"), g + QStringLiteral("/my-backups")}});
            QVERIFY(job.ok());
            ProgressReporter progress;
            JobContext ctx = context(progress);
            QVERIFY(!job.value()->run(ctx).ok());
            QVERIFY(!QFileInfo::exists(g + QStringLiteral("/my-backups")));
            QVERIFY(QFileInfo::exists(g + QStringLiteral("/audio/ja/vo.bin")));
        }
        // Part of the selection could not be removed: the outcome (with the backup) is kept.
        QString backupZip;
        {
            const QString backups = m_root.filePath(QStringLiteral("backups-k"));
            auto job = jobs.create(QStringLiteral("tools.languages.strip"),
                                   {{QStringLiteral("gameRoot"), g},
                                    {QStringLiteral("remove"), QJsonArray{QStringLiteral("audio/ja"), QStringLiteral("data/ja-JP")}},
                                    {QStringLiteral("backupFolder"), backups}});
            bool interfered = false;
            ProgressReporter progress([&](const ProgressSnapshot& p) {
                if (p.phaseIndex >= 1 && !interfered) {
                    interfered = true;
                    fwtest::writeTextFile(g + QStringLiteral("/data/ja-JP/late.bin"), "late");
                }
            }, 0);
            JobContext ctx = context(progress);
            auto o = job.value()->run(ctx);
            QVERIFY2(o.ok(), o.ok() ? "" : qPrintable(o.error().toLogString()));
            QCOMPARE(o->verification, Verification::Inconclusive);
            QCOMPARE(int(o->outputs.size()), 1);
            backupZip = o->outputs.first();
            QVERIFY(QFileInfo(backupZip).isFile());
            QVERIFY2(o->summary.startsWith(QStringLiteral("1 of 2 item(s) could not be removed completely.")), qPrintable(o->summary));
            QVERIFY(o->summary.contains(QDir::toNativeSeparators(backupZip)));
            QCOMPARE(o->details.value(QStringLiteral("removed")).toInt(), 1);
            QVERIFY(o->warnings.join(u'\n').contains(QStringLiteral("data/ja-JP/late.bin")));
            QVERIFY(QFileInfo::exists(g + QStringLiteral("/data/ja-JP/late.bin")));
        }
        // Restore says what it left alone.
        {
            fwtest::writeTextFile(g + QStringLiteral("/audio/ja/vo.bin"), "already here");
            auto job = jobs.create(QStringLiteral("tools.languages.restore"),
                                   {{QStringLiteral("backupZip"), backupZip}, {QStringLiteral("gameRoot"), g}});
            ProgressReporter progress;
            JobContext ctx = context(progress);
            auto o = job.value()->run(ctx);
            QVERIFY2(o.ok(), o.ok() ? "" : qPrintable(o.error().toLogString()));
            QCOMPARE(o->details.value(QStringLiteral("restored")).toInt(), 1);
            QCOMPARE(o->details.value(QStringLiteral("skippedExisting")).toInt(), 1);
            QVERIFY2(o->summary.startsWith(QStringLiteral("Restored 1 file(s). 1 already in the game")), qPrintable(o->summary));
            QVERIFY(o->warnings.join(u'\n').contains(QStringLiteral("audio/ja/vo.bin")));
            QCOMPARE(readFile(g + QStringLiteral("/audio/ja/vo.bin")), QByteArray("already here"));
        }
        // A batch scan of a folder without any game says so.
        {
            const QString empty = m_root.filePath(QStringLiteral("no-games"));
            QDir().mkpath(empty + QStringLiteral("/just-a-folder"));
            auto job = jobs.create(QStringLiteral("tools.languages.scan"),
                                   {{QStringLiteral("folder"), empty}, {QStringLiteral("batch"), true}});
            ProgressReporter progress;
            JobContext ctx = context(progress);
            auto o = job.value()->run(ctx);
            QVERIFY(o.ok());
            QVERIFY2(o->summary.startsWith(QStringLiteral("No game was found in ")), qPrintable(o->summary));
        }
    }

    void redactionCoversEverySpelling()
    {
        const QString home = QDir::homePath();
        const QString user = QFileInfo(home).fileName();
        const QString native = QDir::toNativeSeparators(home);
        QString json = native;
        json.replace(QStringLiteral("\\"), QStringLiteral("\\\\"));
        const QString encoded = QString::fromLatin1(QUrl::toPercentEncoding(home, "/:"));
        const QString text = home + QStringLiteral("/a | ") + native + QStringLiteral("\\b | ") + json + QStringLiteral(" | ")
            + encoded + QStringLiteral(" | D:\\Users\\") + user
            + QStringLiteral("\\Games | /home/") + user + QStringLiteral("/x");
        const QString red = redactUserPaths(text);
        // The messages name the spelling, not the text: a failure must not put the user's name into the test log.
        QVERIFY2(!red.contains(home, Qt::CaseInsensitive), "the home path with forward slashes is still there");
        QVERIFY2(!red.contains(native, Qt::CaseInsensitive), "the home path with native separators is still there");
        QVERIFY2(!red.contains(json, Qt::CaseInsensitive), "the JSON-escaped home path is still there");
        QVERIFY2(!red.contains(encoded, Qt::CaseInsensitive), "the percent-encoded home path is still there");
        if (user.size() >= 2) {
            const QString encodedUser = QString::fromLatin1(QUrl::toPercentEncoding(user));
            QVERIFY2(!red.contains(QStringLiteral("\\") + user + QStringLiteral("\\"), Qt::CaseInsensitive)
                         && !red.contains(QStringLiteral("/") + user + QStringLiteral("/"), Qt::CaseInsensitive),
                     "the user name is still there as a folder name");
            QVERIFY2(!red.contains(QStringLiteral("/") + encodedUser + QStringLiteral("/"), Qt::CaseInsensitive)
                         && !red.endsWith(QStringLiteral("/") + encodedUser, Qt::CaseInsensitive)
                         && !red.contains(QStringLiteral("/") + encodedUser + QStringLiteral(" "), Qt::CaseInsensitive),
                     "the percent-encoded user name is still there as a folder name");
        }
        // What is not a user path stays.
        QVERIFY(red.contains(QStringLiteral(" | ")));
    }

    void reportBundleRedactsAndVerifies()
    {
        const QString logs = m_root.filePath(QStringLiteral("logs"));
        const QString home = QDir::toNativeSeparators(QDir::homePath());
        fwtest::writeTextFile(logs + QStringLiteral("/farewell.log"), (QStringLiteral("opened ") + home + QStringLiteral("/Games/x\n")).toUtf8());
        fwtest::writeTextFile(logs + QStringLiteral("/farewell.jsonl"), "{}\n");
        const QString settings = m_root.filePath(QStringLiteral("settings.json"));
        fwtest::writeTextFile(settings, (QStringLiteral("{\"storage.defaultOutputFolder\": \"") + QDir::homePath()
                                         + QStringLiteral("/out\", \"general.discordWebhookUrl\": \"https://discord.com/api/webhooks/42/SeCrEt_token-1\"}")).toUtf8());
        ReportOptions opt;
        opt.destinationFolder = m_root.filePath(QStringLiteral("reports"));
        QDir().mkpath(opt.destinationFolder);
        opt.logsFolder = logs;
        opt.extraFiles = {settings};
        opt.diagnostics = {{QStringLiteral("deps"), QStringLiteral("ok")}};
        auto r = buildReportBundle(opt, CancellationToken(), nullptr);
        QVERIFY2(r.ok(), r.ok() ? "" : qPrintable(r.error().toLogString()));
        QVERIFY(r->contents.contains(QStringLiteral("system.json")));
        QVERIFY(r->contents.contains(QStringLiteral("logs/farewell.log")));
        QVERIFY(r->contents.contains(QStringLiteral("settings.json")));
        zip::Reader z;
        QVERIFY(z.open(r->zipPath).ok());
        const QByteArray log = z.readAll(*z.find(QStringLiteral("logs/farewell.log"))).value();
        const QByteArray st = z.readAll(*z.find(QStringLiteral("settings.json"))).value();
        QVERIFY2(!log.contains(home.toUtf8()), log.constData());
        QVERIFY(log.contains("~"));
        QVERIFY(!st.contains(QDir::homePath().toUtf8()));
        QVERIFY(!st.contains("SeCrEt"));
        QVERIFY(st.contains("https://discord.com/api/webhooks/42/<token hidden>"));
        // Redaction off keeps the paths, never the webhook token.
        opt.redactUserPaths = false;
        auto plain = buildReportBundle(opt, CancellationToken(), nullptr);
        QVERIFY(plain.ok());
        zip::Reader z2;
        QVERIFY(z2.open(plain->zipPath).ok());
        QVERIFY(z2.readAll(*z2.find(QStringLiteral("logs/farewell.log"))).value().contains(home.toUtf8()));
        QVERIFY(!z2.readAll(*z2.find(QStringLiteral("settings.json"))).value().contains("SeCrEt"));
        QCOMPARE(redactSecrets(QStringLiteral("\"https:\\/\\/discordapp.com\\/api\\/v10\\/webhooks\\/7\\/abc-DEF\"")),
                 QStringLiteral("\"https:\\/\\/discordapp.com\\/api\\/v10\\/webhooks\\/7\\/<token hidden>\""));
        QCOMPARE(redactSecrets(QStringLiteral("no secrets here")), QStringLiteral("no secrets here"));
    }

    // Of an over-long history the newest records are kept, from the start of a line.
    void reportKeepsTheEndOfAVeryLongFile()
    {
        const QString base = m_root.filePath(QStringLiteral("longfile"));
        QByteArray history;
        const QByteArray filler(2000, 'x');
        int n = 0;
        while (history.size() < 17 * 1024 * 1024)
            history += "{\"n\":" + QByteArray::number(n++) + ",\"note\":\"" + filler + "\"}\n";
        fwtest::writeTextFile(base + QStringLiteral("/history.jsonl"), history);
        ReportOptions opt;
        opt.destinationFolder = base + QStringLiteral("/out");
        QDir().mkpath(opt.destinationFolder);
        opt.extraFiles = {base + QStringLiteral("/history.jsonl")};
        opt.redactUserPaths = false;
        auto r = buildReportBundle(opt, CancellationToken(), nullptr);
        QVERIFY2(r.ok(), r.ok() ? "" : qPrintable(r.error().toLogString()));
        zip::Reader z;
        QVERIFY(z.open(r->zipPath).ok());
        const QByteArray kept = z.readAll(*z.find(QStringLiteral("history.jsonl"))).value();
        QVERIFY(kept.size() <= 16 * 1024 * 1024);
        QVERIFY(kept.size() > 15 * 1024 * 1024);
        QVERIFY2(kept.startsWith("{\"n\":"), kept.left(40).constData());                      // a whole first line
        QVERIFY(kept.contains("{\"n\":" + QByteArray::number(n - 1) + ","));                   // the newest record
        QVERIFY(!kept.contains("{\"n\":0,"));                                                  // the oldest is what was dropped
        // No report is left behind under a temporary name.
        QCOMPARE(QDir(opt.destinationFolder).entryList(QDir::Files).size(), 1);
    }

    // Nothing secret from the settings, the queue, the history or the logs may
    // be anywhere in the ZIP; the keys stay so it is visible they were set.
    void reportBundleHidesPasscodesKeysAndPublicAddresses()
    {
        const QString base = m_root.filePath(QStringLiteral("secrets"));
        const QString logs = base + QStringLiteral("/logs");
        const QByteArrayList secrets = {
            "PASSCODE-SETTING-0123456789abcdef", "SRC-PASSCODE-fedcba9876543210ABCD", "00112233445566778899aabbccddeeff",
            "WebHookTokenAAA111", "T000/B000/SLACKISHSECRET", "PASSCODE-QUEUE-zzzzzzzzzzzzzzzzzzzz",
            "PASSCODE-OVERRIDE-yyyyyyyyyyyyyyyyy", "ffeeddccbbaa99887766554433221100", "DEEP-NESTED-PASSCODE-xxxxxxxxxxxxxx",
            "PASSCODE-HISTORY-wwwwwwwwwwwwwwwww", "PASSCODE-IN-DETAIL-vvvvvvvvvvvvvvv", "0f1e2d3c4b5a69788796a5b4c3d2e1f0",
            "LOG-PASSCODE-uuuuuuuuuuuuuuuuuuuuu", "LOG-PASSCODE-EQ-ttttttttttttttttt", "LOG-PASSCODE-COLON-ssssssssssssss",
            "LOG-ESCAPED-PASSCODE-rrrrrrrrrrrr", "API-TOKEN-qqqqqqqqqqqqqqqq", "DIAG-SECRET-pppppppppppp",
            "203.0.113.7", "198.51.100.23", "8.8.4.4", "console.example.net",
            "CRASH-PASSCODE-oooooooooooo", "DUMP-MEMORY-SECRET-nnnnnnnn",
        };
        // The crash report (text) belongs in the ZIP; the memory dump next to it never does.
        fwtest::writeTextFile(logs + QStringLiteral("/crashes/crash-20261101-101010.txt"),
                              "Unhandled exception 0xC0000005\npasscode=CRASH-PASSCODE-oooooooooooo\n");
        fwtest::writeTextFile(logs + QStringLiteral("/crashes/crash-20261101-101010.dmp"), "MDMP DUMP-MEMORY-SECRET-nnnnnnnn");
        fwtest::writeTextFile(
            base + QStringLiteral("/settings.json"),
            "{\n  \"version\": 3,\n  \"values\": {\n"
            "    \"fpkg.passcode\": \"PASSCODE-SETTING-0123456789abcdef\",\n"
            "    \"dlc.sourcePasscode\": \"SRC-PASSCODE-fedcba9876543210ABCD\",\n"
            "    \"dlc.entitlementKey\": \"00112233445566778899aabbccddeeff\",\n"
            "    \"general.discordWebhookUrl\": \"https://discord.com/api/webhooks/42/WebHookTokenAAA111\",\n"
            "    \"notify.webhookUrl\": \"https://hooks.example.org/services/T000/B000/SLACKISHSECRET\",\n"
            "    \"ps5.consoleIp\": \"203.0.113.7\",\n"
            "    \"fpkg.unsetPasscode\": \"\",\n"
            "    \"storage.defaultOutputFolder\": \"D:/Images\"\n  }\n}\n");
        fwtest::writeTextFile(
            base + QStringLiteral("/queue.json"),
            "{\"jobs\": [{\"kind\": \"fpkg.build\", \"params\": {\"passcode\": \"PASSCODE-QUEUE-zzzzzzzzzzzzzzzzzzzz\","
            " \"entitlementKey\": \"ffeeddccbbaa99887766554433221100\", \"host\": \"192.168.1.50\","
            " \"items\": [{\"options\": {\"sourcePasscode\": \"DEEP-NESTED-PASSCODE-xxxxxxxxxxxxxx\", \"keep\": \"visible-value\"}}]},"
            " \"overrides\": {\"fpkg.passcode\": \"PASSCODE-OVERRIDE-yyyyyyyyyyyyyyyyy\", \"ps5.consoleIp\": \"console.example.net\"}}]}");
        fwtest::writeTextFile(
            base + QStringLiteral("/history.jsonl"),
            "{\"id\":\"a\",\"params\":{\"passcode\":\"PASSCODE-HISTORY-wwwwwwwwwwwwwwwww\",\"ip\":\"198.51.100.23\"},"
            "\"error\":{\"detail\":\"fpkg-cli build --passcode PASSCODE-IN-DETAIL-vvvvvvvvvvvvvvv --entitlement-key=0f1e2d3c4b5a69788796a5b4c3d2e1f0 out\"}}\n"
            "{\"id\":\"b\",\"params\":{\"host\":\"10.0.0.7\"}}\n"
            "this line is damaged {\"passcode\": \"PASSCODE-HISTORY-wwwwwwwwwwwwwwwww\"\n");
        fwtest::writeTextFile(
            logs + QStringLiteral("/app.log"),
            "12:00:01 run: tool.exe --passcode LOG-PASSCODE-uuuuuuuuuuuuuuuuuuuuu --out x.pkg\n"
            "12:00:02 run: tool.exe --passcode=LOG-PASSCODE-EQ-ttttttttttttttttt\n"
            "12:00:03 passcode: LOG-PASSCODE-COLON-ssssssssssssss (from settings)\n"
            "12:00:04 job {\\\"passcode\\\":\\\"LOG-ESCAPED-PASSCODE-rrrrrrrrrrrr\\\",\\\"kind\\\":\\\"fpkg.build\\\"}\n"
            "12:00:05 connected to 192.168.1.50:2121, relay 8.8.4.4, console 203.0.113.7\n"
            "12:00:06 Windows 10.0.26300 build, tool 1.2.3\n");
        fwtest::writeTextFile(logs + QStringLiteral("/events.jsonl"),
                              "{\"event\":\"upload\",\"apiToken\":\"API-TOKEN-qqqqqqqqqqqqqqqq\",\"host\":\"192.168.1.50\"}\n");

        ReportOptions opt;
        opt.destinationFolder = base + QStringLiteral("/out");
        QDir().mkpath(opt.destinationFolder);
        opt.logsFolder = logs;
        opt.extraFiles = {base + QStringLiteral("/settings.json"), base + QStringLiteral("/history.jsonl"),
                          base + QStringLiteral("/queue.json")};
        opt.diagnostics = {{QStringLiteral("deps"), QJsonObject{{QStringLiteral("clientSecret"), QStringLiteral("DIAG-SECRET-pppppppppppp")}}}};
        for (const bool redactPaths : {true, false}) {
            opt.redactUserPaths = redactPaths;
            auto r = buildReportBundle(opt, CancellationToken(), nullptr);
            QVERIFY2(r.ok(), r.ok() ? "" : qPrintable(r.error().toLogString()));
            // The raw archive first (entries are deflated, but names and stored data are not).
            const QByteArray raw = readFile(r->zipPath);
            zip::Reader z;
            QVERIFY(z.open(r->zipPath).ok());
            QByteArray all;
            for (const auto& e : z.entries())
                all += z.readAll(e).value() + '\n';
            QVERIFY(z.entries().size() >= 8);
            const zip::Entry* crash = z.find(QStringLiteral("logs/crashes/crash-20261101-101010.txt"));
            QVERIFY(crash);
            QVERIFY(z.readAll(*crash).value().contains("0xC0000005"));
            for (const auto& e : z.entries())
                QVERIFY2(!e.name.endsWith(QStringLiteral(".dmp")), qPrintable(e.name));
            for (const QByteArray& s : secrets) {
                QVERIFY2(!all.contains(s), s.constData());
                QVERIFY2(!raw.contains(s), s.constData());
            }
            // The keys are still there, so it is visible that they were set.
            const QByteArray st = z.readAll(*z.find(QStringLiteral("settings.json"))).value();
            for (const char* key : {"fpkg.passcode", "dlc.sourcePasscode", "dlc.entitlementKey", "general.discordWebhookUrl",
                                    "notify.webhookUrl", "ps5.consoleIp"})
                QVERIFY2(st.contains(key), key);
            QVERIFY(st.contains("<hidden>"));
            QVERIFY(st.contains("https://discord.com/api/webhooks/42/<token hidden>"));
            QVERIFY(st.contains("D:/Images"));
            const QJsonObject values = QJsonDocument::fromJson(st).object().value(QStringLiteral("values")).toObject();
            QCOMPARE(values.value(QStringLiteral("fpkg.passcode")).toString(), QStringLiteral("<hidden>"));
            QCOMPARE(values.value(QStringLiteral("fpkg.unsetPasscode")).toString(), QString()); // unset stays visibly unset
            // Local addresses stay (they help with a connection problem); the rest of each file too.
            const QByteArray queue = z.readAll(*z.find(QStringLiteral("queue.json"))).value();
            QVERIFY(queue.contains("192.168.1.50"));
            QVERIFY(queue.contains("visible-value"));
            QVERIFY(queue.contains("fpkg.build"));
            const QByteArray log = z.readAll(*z.find(QStringLiteral("logs/app.log"))).value();
            QVERIFY(log.contains("192.168.1.50:2121"));
            QVERIFY(log.contains("10.0.26300"));
            QVERIFY(log.contains("--out x.pkg"));
            QVERIFY(log.contains("tool 1.2.3"));
            const QByteArray history = z.readAll(*z.find(QStringLiteral("history.jsonl"))).value();
            QVERIFY(history.contains("10.0.0.7"));
            QCOMPARE(history.count('\n'), 3); // still one record per line
        }

        // A file that is not valid JSON falls back to the text rules.
        QCOMPARE(redactSecrets(QStringLiteral("{\"a\": {\"fpkg.passcode\": \"abc\\\"def\", broken")),
                 QStringLiteral("{\"a\": {\"fpkg.passcode\": \"<hidden>\", broken"));
        QCOMPARE(redactSecrets(QStringLiteral("[\"--passcode\", \"abc\"]")).contains(QStringLiteral("abc")), false);
        QCOMPARE(redactSecrets(QStringLiteral("the non-secret value stays")), QStringLiteral("the non-secret value stays"));
    }
};

QTEST_GUILESS_MAIN(TstTools)
#include "tst_tools.moc"
