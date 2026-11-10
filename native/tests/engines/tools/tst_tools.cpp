// Tools module: Language Stripper detection, verified backup + removal,
// restore, safety refusals, and the problem-report bundle with redaction.

#include "core/Paths.h"
#include "core/Zip.h"
#include "engines/tools/LanguageStripper.h"
#include "engines/tools/ReportBundle.h"
#include "support/TestSupport.h"

#include <QDir>
#include <QFile>
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
        auto escape = base;
        escape.remove = {QStringLiteral("../GameB/eboot.bin")};
        QVERIFY(!stripLanguages(escape, CancellationToken(), nullptr).ok());
        auto notGame = base;
        notGame.gameRoot = m_root.filePath(QStringLiteral("backups2"));
        QVERIFY(!stripLanguages(notGame, CancellationToken(), nullptr).ok());
        // Nothing was removed by any refused plan.
        QVERIFY(QFileInfo::exists(g + QStringLiteral("/audio/ja/vo.bin")));
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

    void redactionCoversEverySpelling()
    {
        const QString home = QDir::homePath();
        const QString user = QFileInfo(home).fileName();
        const QString native = QDir::toNativeSeparators(home);
        QString json = native;
        json.replace(QStringLiteral("\\"), QStringLiteral("\\\\"));
        const QString text = home + QStringLiteral("/a | ") + native + QStringLiteral("\\b | ") + json + QStringLiteral(" | ")
            + QString::fromLatin1(QUrl::toPercentEncoding(home, "/:")) + QStringLiteral(" | D:\\Users\\") + user
            + QStringLiteral("\\Games | /home/") + user + QStringLiteral("/x");
        const QString red = redactUserPaths(text);
        QVERIFY2(!red.contains(home, Qt::CaseInsensitive), qPrintable(red));
        QVERIFY2(!red.contains(native, Qt::CaseInsensitive), qPrintable(red));
        if (user.size() >= 2)
            QVERIFY2(!red.contains(QStringLiteral("\\") + user + QStringLiteral("\\"), Qt::CaseInsensitive)
                         && !red.contains(QStringLiteral("/") + user + QStringLiteral("/"), Qt::CaseInsensitive), qPrintable(red));
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
        };
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
            QVERIFY(z.entries().size() >= 7);
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
