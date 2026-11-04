// Tools module: Language Stripper detection, verified backup + removal,
// restore, safety refusals, and the problem-report bundle with redaction.

#include "core/Paths.h"
#include "core/Zip.h"
#include "engines/tools/LanguageStripper.h"
#include "engines/tools/ReportBundle.h"
#include "support/TestSupport.h"

#include <QDir>
#include <QFile>
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
        fwtest::writeTextFile(settings, (QStringLiteral("{\"storage.defaultOutputFolder\": \"") + QDir::homePath() + QStringLiteral("/out\"}")).toUtf8());
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
        // Redaction off keeps the paths.
        opt.redactUserPaths = false;
        auto plain = buildReportBundle(opt, CancellationToken(), nullptr);
        QVERIFY(plain.ok());
        zip::Reader z2;
        QVERIFY(z2.open(plain->zipPath).ok());
        QVERIFY(z2.readAll(*z2.find(QStringLiteral("logs/farewell.log"))).value().contains(home.toUtf8()));
    }
};

QTEST_GUILESS_MAIN(TstTools)
#include "tst_tools.moc"
