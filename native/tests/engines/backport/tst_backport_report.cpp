// Per-run reports (docs/BACKPORT_PARITY.md matrix #24): a JSON document and a
// self-contained HTML page next to the game's BackportBackup records, for
// successful, partial and failed runs; none for a run the preflight refused.

#include "BackportTestEnv.h"

#include "core/Paths.h"
#include "engines/backport/BackportRunner.h"
#include "engines/backport/RunReport.h"
#include "support/TestSupport.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QTest>

using namespace fw;
using namespace fw::backport;
using namespace fwtest::bp;

namespace {

QJsonObject readJson(const QString& path)
{
    return QJsonDocument::fromJson(readBytes(path)).object();
}

QJsonObject fileEntry(const QJsonObject& report, const QString& rel)
{
    for (const QJsonValue& v : report.value("files").toArray())
        if (v.toObject().value("file").toString() == rel)
            return v.toObject();
    return {};
}

void checkSelfContained(const QString& html)
{
    QVERIFY(html.startsWith(QStringLiteral("<!doctype html>")));
    QVERIFY(html.contains(QStringLiteral("Content-Security-Policy")));
    QVERIFY(!html.contains(QStringLiteral("<script"), Qt::CaseInsensitive));
    QVERIFY(!html.contains(QStringLiteral("http://"), Qt::CaseInsensitive));
    QVERIFY(!html.contains(QStringLiteral("https://"), Qt::CaseInsensitive));
    QVERIFY(!html.contains(QStringLiteral("<link"), Qt::CaseInsensitive));
    QVERIFY(!html.contains(QStringLiteral("<img"), Qt::CaseInsensitive));
}

} // namespace

class TstBackportReport : public QObject {
    Q_OBJECT
    QTemporaryDir m_root;

private slots:
    void initTestCase()
    {
        QVERIFY(m_root.isValid());
        fwtest::isolateAppData(m_root);
    }

    void successfulRunWritesJsonAndHtmlNextToTheRecords()
    {
        Env e;
        auto r = runBackport(e.options(), e.ctx);
        QVERIFY2(r.ok(), qPrintable(r.ok() ? QString() : r.error().toLogString()));
        QVERIFY(QFileInfo(r->reportJson).isFile());
        QVERIFY(QFileInfo(r->reportHtml).isFile());
        // Next to BackportBackup/<TitleID>/manifest.json, which is unchanged in place.
        QCOMPARE(QFileInfo(r->reportJson).absolutePath(), QDir(e.orig).filePath("BackportBackup/PPSA01234/reports"));
        QCOMPARE(QFileInfo(r->manifestPath).absolutePath(), QDir(e.orig).filePath("BackportBackup/PPSA01234"));
        QVERIFY(QFileInfo(r->reportJson).fileName().endsWith(QStringLiteral(" backport SDK 4.json")));
        QCOMPARE(r->details.value("report").toObject().value("html").toString(), QDir::toNativeSeparators(r->reportHtml));
        QCOMPARE(toOutcome(r.value()).details.value("report").toObject().value("json").toString(),
                 QDir::toNativeSeparators(r->reportJson));

        const QJsonObject rep = readJson(r->reportJson);
        QCOMPARE(rep.value("format").toString(), QStringLiteral("fw-backport-report"));
        QCOMPARE(rep.value("schema").toInt(), 1);
        QCOMPARE(rep.value("kind").toString(), QStringLiteral("backport"));
        QCOMPARE(rep.value("jobId").toString(), QStringLiteral("job-1"));
        QCOMPARE(rep.value("titleId").toString(), QStringLiteral("PPSA01234"));
        QCOMPARE(rep.value("outcome").toObject().value("status").toString(), QStringLiteral("succeeded"));
        // Inputs and the target SDK pair.
        const QJsonObject in = rep.value("inputs").toObject();
        QCOMPARE(in.value("game").toString(), QDir::toNativeSeparators(QDir::cleanPath(e.game)));
        QCOMPARE(in.value("gameSdk").toInt(), 7);
        QCOMPARE(in.value("fakelibFolder").toString(), QDir::toNativeSeparators(e.fakelib));
        QCOMPARE(in.value("applyToGameFolder").toBool(), true);
        const QJsonObject tg = rep.value("target").toObject();
        QCOMPARE(tg.value("sdk").toInt(), 4);
        QCOMPARE(tg.value("ps5Sdk").toString(), QStringLiteral("0x04000031"));
        QCOMPARE(tg.value("ps4Sdk").toString(), QStringLiteral("0x09040001"));
        // Per-file results.
        const QJsonObject eboot = fileEntry(rep, "eboot.bin");
        QCOMPARE(eboot.value("result").toString(), QStringLiteral("rewritten"));
        QCOMPARE(eboot.value("sdkBefore").toInt(), 7);
        QCOMPARE(eboot.value("sdkAfter").toInt(), 4);
        QCOMPARE(eboot.value("sha256After").toString(), QString::fromLatin1(sha(readBytes(e.game + "/eboot.bin"))));
        QVERIFY(eboot.value("why").toString().contains(QStringLiteral("SDK 7 -> 4")));
        QVERIFY(fileEntry(rep, "sce_module/libc.prx").value("why").toString().contains(QStringLiteral("libc import")));
        QCOMPARE(fileEntry(rep, "fakelib/libSceAgc.sprx").value("result").toString(), QStringLiteral("added"));
        // ZIP paths and SHA-256s.
        const QJsonArray zips = rep.value("zips").toArray();
        QCOMPARE(zips.size(), 2);
        for (const QJsonValue& z : zips) {
            const QJsonObject o = z.toObject();
            const QString path = QDir::fromNativeSeparators(o.value("path").toString());
            auto h = sha256File(path, CancellationToken());
            QVERIFY(h.ok());
            QCOMPARE(o.value("sha256").toString(), h.value());
            QCOMPARE(path, o.value("role").toString() == u"originals" ? r->originalsZip : r->backportZip);
        }
        // Verification outcome and the apply journal.
        const QJsonObject ver = rep.value("verification").toObject();
        QVERIFY(ver.value("build").toString().startsWith(QStringLiteral("passed")));
        QVERIFY(ver.value("zips").toString().contains(QStringLiteral("SHA-256")));
        QVERIFY(ver.value("apply").toString().contains(QStringLiteral("V7")));
        QCOMPARE(rep.value("apply").toObject().value("journal").toString(), QDir::toNativeSeparators(r->journalPath));
        QVERIFY(rep.value("warnings").isArray());

        const QString html = QString::fromUtf8(readBytes(r->reportHtml));
        checkSelfContained(html);
        QVERIFY(html.contains(QFileInfo(r->originalsZip).fileName().toHtmlEscaped()));
        QVERIFY(html.contains(QStringLiteral("Succeeded")));
        QVERIFY(html.contains(QStringLiteral("SDK 4 · loads on FW 4.00+")));
    }

    void skippedFilesAndWarningsAreListed()
    {
        Env e(OtherFakelib | LeftoverBak | OsMetadata);
        auto r = runBackport(e.options(), e.ctx);
        QVERIFY2(r.ok(), qPrintable(r.ok() ? QString() : r.error().toLogString()));
        const QJsonObject rep = readJson(r->reportJson);
        QStringList skipped;
        for (const QJsonValue& v : rep.value("skipped").toArray())
            skipped << v.toObject().value("file").toString();
        QVERIFY(skipped.contains("fakelib/libSceAmpr.sprx")); // the dump's own extra: never re-signed
        QVERIFY(skipped.contains("eboot.bin.bak"));
        QVERIFY(skipped.contains("._eboot.bin"));
        QCOMPARE(fileEntry(rep, "fakelib/libSceAgc.sprx").value("result").toString(), QStringLiteral("replaced"));
        bool warned = false;
        for (const QJsonValue& w : rep.value("warnings").toArray())
            warned = warned || w.toString().contains(QStringLiteral("another tool"));
        QVERIFY(warned);
    }

    void failedRunNamesItsReport()
    {
        Env e;
        BackportOptions o = e.options();
        o.testHook = [](const QString& point, const QString& item) -> Status {
            if (point == u"build.file" && item == u"sce_module/libc.prx")
                return makeError(Code::SignUnsupported, QStringLiteral("injected"));
            return success();
        };
        const auto before = snapshotTree(e.game);
        auto r = runBackport(o, e.ctx);
        QVERIFY(!r.ok());
        QCOMPARE(snapshotTree(e.game), before);
        const QString html = QDir::fromNativeSeparators(reportFromDetail(r.error().detail));
        QVERIFY2(QFileInfo(html).isFile(), qPrintable(r.error().detail));
        QCOMPARE(codeOf(r.error()), Code::SignUnsupported); // the marker does not disturb the code
        const QString json = html.chopped(5) + QStringLiteral(".json");
        const QJsonObject rep = readJson(json);
        QCOMPARE(rep.value("outcome").toObject().value("status").toString(), QStringLiteral("failed"));
        QVERIFY(rep.value("outcome").toObject().value("summary").toString().startsWith(QStringLiteral("No safe backport")));
        QCOMPARE(rep.value("outcome").toObject().value("error").toObject().value("code").toString(), QStringLiteral("SignUnsupported"));
        const QJsonObject libc = fileEntry(rep, "sce_module/libc.prx");
        QCOMPARE(libc.value("result").toString(), QStringLiteral("failed"));
        QCOMPARE(libc.value("why").toString(), QStringLiteral("injected"));
        QVERIFY(rep.value("zips").toArray().isEmpty());
        QVERIFY(rep.value("verification").toObject().value("build").toString().startsWith(QStringLiteral("failed")));
        checkSelfContained(QString::fromUtf8(readBytes(html)));
    }

    void partialRunIsReportedAsPartial()
    {
        Env e;
        BackportOptions o = e.options();
        o.testHook = [](const QString& point, const QString& item) -> Status {
            if (point == u"zip.member" && item == u"fakelib/libSceAgc.sprx")
                return makeError(Code::ZipWriteFailed, QStringLiteral("disk full (injected)"));
            return success();
        };
        auto r = runBackport(o, e.ctx);
        QVERIFY(!r.ok());
        QVERIFY(r.error().message.startsWith(QStringLiteral("Partial:")));
        const QString html = QDir::fromNativeSeparators(reportFromDetail(r.error().detail));
        const QJsonObject rep = readJson(html.chopped(5) + QStringLiteral(".json"));
        QCOMPARE(rep.value("outcome").toObject().value("status").toString(), QStringLiteral("partial"));
        bool originals = false, failedBackport = false;
        for (const QJsonValue& z : rep.value("zips").toArray()) {
            originals = originals || z.toObject().value("role").toString() == u"originals";
            failedBackport = failedBackport
                || (z.toObject().value("role").toString() == u"backport" && z.toObject().value("result").toString() == u"failed");
        }
        QVERIFY(originals);
        QVERIFY(failedBackport);
    }

    void refusedRunWritesNoReport()
    {
        Env e;
        BackportOptions o = e.options();
        o.targetSdk = 9; // above the game's SDK 7 (S4)
        auto r = runBackport(o, e.ctx);
        QVERIFY(!r.ok());
        QCOMPARE(codeOf(r.error()), Code::TargetAboveGameSdk);
        QVERIFY(reportFromDetail(r.error().detail).isEmpty());
        QVERIFY(!QFileInfo::exists(QDir(e.orig).filePath("BackportBackup")));
    }

    void reportWithoutOriginalsFolderGoesToAppData()
    {
        Env e;
        QCOMPARE(reportDirForGame(QString(), e.game), defaultReportDir());
        QCOMPARE(reportDirForGame(e.tmp.filePath("missing"), e.game), defaultReportDir());
        QCOMPARE(reportDirForGame(e.orig, e.game), QDir(e.orig).filePath("BackportBackup/PPSA01234/reports"));
        QVERIFY(defaultReportDir().startsWith(Paths::dataDir()));
    }

    void namesAreNeverReused()
    {
        QTemporaryDir d;
        QJsonObject rep = newReport(QStringLiteral("backport"));
        auto a = writeReport(d.path(), QStringLiteral("20261104-120000 backport SDK 4"), rep);
        auto b = writeReport(d.path(), QStringLiteral("20261104-120000 backport SDK 4"), rep);
        QVERIFY(a.ok() && b.ok());
        QVERIFY(a->json != b->json);
        QVERIFY(b->json.endsWith(QStringLiteral("SDK 4 (2).json")));
        QVERIFY(b->html.endsWith(QStringLiteral("SDK 4 (2).html")));
        QVERIFY(reportBaseName(QStringLiteral("a:b")).endsWith(QStringLiteral(" a_b")));
    }

    void htmlEscapesEveryValue()
    {
        QJsonObject rep = newReport(QStringLiteral("backport"));
        rep.insert("title", QStringLiteral("<script>alert(1)</script> & \"x\""));
        QJsonObject outcome{{"status", "failed"}, {"summary", QStringLiteral("<img src=x onerror=1>")}};
        rep.insert("outcome", outcome);
        rep.insert("inputs", QJsonObject{{"game", QStringLiteral("D:\\Games\\<b>bold</b>")}});
        rep.insert("files", QJsonArray{QJsonObject{{"file", QStringLiteral("a&b.prx")}, {"result", "failed"},
                                                   {"why", QStringLiteral("</td><script>x</script>")}}});
        rep.insert("custom", QJsonArray{QStringLiteral("<iframe>")});
        const QString html = renderReportHtml(rep);
        checkSelfContained(html);
        QVERIFY(!html.contains(QStringLiteral("<b>bold")));
        QVERIFY(!html.contains(QStringLiteral("<iframe>")));
        QVERIFY(html.contains(QStringLiteral("&lt;script&gt;alert(1)&lt;/script&gt; &amp; &quot;x&quot;")));
        QVERIFY(html.contains(QStringLiteral("a&amp;b.prx")));
        QVERIFY(html.contains(QStringLiteral("&lt;/td&gt;&lt;script&gt;")));
    }

    void detailMarkerRoundTrips()
    {
        const QString d = appendReportToDetail(QStringLiteral("[ZipWriteFailed] disk full"), QStringLiteral("/r/x y.html"));
        QCOMPARE(reportFromDetail(d), QDir::toNativeSeparators(QStringLiteral("/r/x y.html")));
        QVERIFY(d.startsWith(QStringLiteral("[ZipWriteFailed]")));
        QCOMPARE(reportFromDetail(QStringLiteral("[Internal] nothing")), QString());
        QCOMPARE(appendReportToDetail(QStringLiteral("x"), QString()), QStringLiteral("x"));
    }
};

QTEST_GUILESS_MAIN(TstBackportReport)
#include "tst_backport_report.moc"
