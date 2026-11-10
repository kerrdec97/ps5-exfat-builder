// DLC conversion (the ppr-pkg Companion's DLC Converter): template
// param.json, output names, the review plan, INI export, pkg-info parsing,
// PS4 inspection, identity rules, and an end-to-end run with fpkg-cli when
// FW_FPKG_CLI points at it (INI entry -> package, package -> retargeted
// package, read back with pkg-info).

#include "engines/dlc/DlcConvert.h"
#include "engines/dlc/DlcIdentity.h"
#include "engines/dlc/DlcPackage.h"
#include "engines/dlc/DlcSettings.h"
#include "engines/dlc/FpkgCli.h"
#include "engines/dlc/Module.h"
#include "engines/fpkg/Module.h"
#include "engines/general/Module.h"
#include "jobs/Module.h"
#include "support/TestSupport.h"

#include <QDir>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>

using namespace fw;
using namespace fw::dlc;

namespace {

const QString kCid = QStringLiteral("UP9000-PPSA01234_00-TESTDLC000000001");

QByteArray readAll(const QString& p)
{
    QFile f(p);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

DlcItem iniItem(const QString& ini, int entry, const QString& cid, const QString& title)
{
    DlcItem i;
    i.source = ini;
    i.entry = entry;
    i.detectedContentId = cid;
    i.detectedTitleId = titleIdOf(cid);
    i.contentId = cid;
    i.titleId = titleIdOf(cid);
    i.title = title;
    const QFileInfo fi(ini);
    i.sourceSize = fi.size();
    i.sourceModifiedMs = fi.lastModified().toMSecsSinceEpoch();
    return i;
}

} // namespace

class TstDlcConvert : public QObject {
    Q_OBJECT
private:
    QTemporaryDir m_tmp;
    QString dir(const QString& name)
    {
        const QString d = QDir(m_tmp.path()).filePath(name);
        QDir().mkpath(d);
        return d;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_tmp.isValid());
        fwtest::isolateAppData(m_tmp);
    }

    void identity()
    {
        QVERIFY(isContentId(kCid));
        QCOMPARE(titleIdOf(kCid), QStringLiteral("PPSA01234"));
        QCOMPARE(suffixOf(kCid), QStringLiteral("TESTDLC000000001"));
        QCOMPARE(regionText(kCid), QStringLiteral("Americas (inferred from UP)"));
        QCOMPARE(regionText(QStringLiteral("EP9000-PPSA01234_00-TESTDLC000000001")), QStringLiteral("Europe (inferred from EP)"));
        QCOMPARE(retargetContentId(kCid, QStringLiteral("PPSA05678"), QStringLiteral("EP1004")).value(),
                 QStringLiteral("EP1004-PPSA05678_00-TESTDLC000000001"));
        QVERIFY(!retargetContentId(kCid, QStringLiteral("CUSA05678")).ok());
        QVERIFY(validateIdentity(QStringLiteral("PPSA01234"), kCid).ok());
        QVERIFY(validateIdentity(QStringLiteral("PPSA01235"), kCid).error().message.contains(QStringLiteral("must match")));
        QVERIFY(hex32Problem(kZeroKey).isEmpty());
        QVERIFY(!hex32Problem(QStringLiteral("12")).isEmpty());
    }

    void templateParam()
    {
        const QByteArray j = templateParamJson(kCid, QStringLiteral("PPSA01234"), QStringLiteral("Caf\u00e9 <Pack> & \"More\""));
        QVERIFY(j.startsWith("{\r\n    \"contentVersion\": \"01.000.000\",\r\n    \"contentId\": \"UP9000-PPSA01234_00-TESTDLC000000001\","));
        QVERIFY(j.contains("\"titleName\": \"Caf\\u00E9 \\u003CPack\\u003E \\u0026 \\u0022More\\u0022\""));
        QVERIFY(j.endsWith("\"masterVersion\": \"01.00\"\r\n}"));
        QVERIFY(QJsonDocument::fromJson(j).isObject());
        QCOMPARE(QJsonDocument::fromJson(j).object().value(QStringLiteral("localizedParameters")).toObject()
                     .value(QStringLiteral("en-US")).toObject().value(QStringLiteral("titleName")).toString(),
                 QStringLiteral("Caf\u00e9 <Pack> & \"More\""));
    }

    void uniqueNames()
    {
        const QString d = dir(QStringLiteral("names"));
        std::set<QString> used;
        const QString a = uniqueOutput(d, kCid + QStringLiteral(".pkg"), used);
        QCOMPARE(QFileInfo(a).fileName(), kCid + QStringLiteral(".pkg"));
        QCOMPARE(QFileInfo(uniqueOutput(d, kCid + QStringLiteral(".pkg"), used)).fileName(), kCid + QStringLiteral("-2.pkg"));
        fwtest::writeTextFile(QDir(d).filePath(QStringLiteral("x.ini")), "x");
        std::set<QString> fresh;
        QCOMPARE(QFileInfo(uniqueOutput(d, QStringLiteral("x.ini"), fresh)).fileName(), QStringLiteral("x-2.ini"));
    }

    void planAndProblems()
    {
        const QString d = dir(QStringLiteral("plan"));
        const QString ini = QDir(d).filePath(QStringLiteral("dlc_emu.ini"));
        fwtest::writeTextFile(ini, "[PSAC]\r\ncontent_id=UP9000-PPSA01234_00-TESTDLC000000001\r\n");
        DlcItem ok = iniItem(ini, 0, kCid, QStringLiteral("One"));
        DlcItem ps4 = iniItem(ini, 0, QStringLiteral("UP9000-CUSA01234_00-TESTDLC000000001"), QStringLiteral("Four"));
        DlcItem bad = ok;
        bad.contentId = QStringLiteral("broken");
        QVERIFY(itemProblem(ok, false).isEmpty());
        QVERIFY(itemProblem(ps4, false).contains(QStringLiteral("PS4 INI can be exported")));
        QVERIFY(itemProblem(ps4, true).isEmpty());
        QVERIFY(itemProblem(bad, false).contains(QStringLiteral("Content ID must use")));
        const QString plan = buildPlanText({ok, bad}, false, QDir(d).filePath(QStringLiteral("out")));
        QVERIFY(plan.contains(QStringLiteral("OUTPUT: ")));
        QVERIFY(plan.contains(QStringLiteral("BLOCKED: ")));
        QVERIFY(plan.contains(QStringLiteral("INI entries generate PS5 DLC template packages")));
        // Changed input is refused.
        fwtest::writeTextFile(ini, "[PSAC]\r\ncontent_id=UP9000-PPSA01234_00-TESTDLC000000001\r\n; edited\r\n");
        QVERIFY(itemProblem(ok, false).contains(QStringLiteral("Input changed")));
    }

    void exportInis()
    {
        const QString d = dir(QStringLiteral("export"));
        const QString ini = QDir(d).filePath(QStringLiteral("dlc_emu.ini"));
        const QByteArray src = "; my list\r\n[PSAC]\r\ncontent_id=UP9000-PPSA01234_00-TESTDLC000000001\r\nmount_point=/app0/addcont0\r\n"
                               "custom=keep me\r\n\r\n[PSAC]\r\ncontent_id=UP9000-PPSA01234_00-TESTDLC000000002\r\n";
        fwtest::writeTextFile(ini, src);
        DlcItem a = iniItem(ini, 0, kCid, QStringLiteral("TESTDLC000000001"));
        a.titleId = QStringLiteral("PPSA05678");
        a.contentId = QStringLiteral("EP1004-PPSA05678_00-TESTDLC000000001");
        DlcItem b = iniItem(ini, 1, QStringLiteral("UP9000-PPSA01234_00-TESTDLC000000002"), QStringLiteral("TESTDLC000000002"));
        b.titleId = QStringLiteral("PPSA05678");
        b.contentId = QStringLiteral("EP1004-PPSA05678_00-TESTDLC000000002");
        const QString out = dir(QStringLiteral("export/out"));
        QStringList log;
        auto results = exportUpdatedInis({a, b}, out, log);
        QCOMPARE(results.size(), size_t(1)); // same input, same target game and prefix: one file
        QVERIFY2(results[0].error.isEmpty(), qPrintable(results[0].error));
        QCOMPARE(QFileInfo(results[0].path).fileName(), QStringLiteral("dlc_emu-PPSA05678-updated.ini"));
        const QByteArray written = readAll(results[0].path);
        QVERIFY(written.contains("content_id=EP1004-PPSA05678_00-TESTDLC000000001\r\n"));
        QVERIFY(written.contains("content_id=EP1004-PPSA05678_00-TESTDLC000000002\r\n"));
        QVERIFY(written.contains("custom=keep me\r\n"));
        QVERIFY(written.startsWith("; my list\r\n"));
        QCOMPARE(readAll(ini), src); // the original is untouched
        QVERIFY(log.join(u'\n').contains(QStringLiteral("INI SAVED")));
        // A second export never overwrites.
        auto again = exportUpdatedInis({a}, out, log);
        QCOMPARE(QFileInfo(again[0].path).fileName(), QStringLiteral("dlc_emu-PPSA05678-updated-2.ini"));
        // Report
        const QString rep = saveBatchReport(out, {a, b}, {results[0].path, results[0].path}, {QString(), QString()}, false, log);
        QVERIFY(QFileInfo(rep).isFile());
        QVERIFY(QFileInfo(rep).fileName().startsWith(QStringLiteral("DLC-results-")));
        const auto files = QJsonDocument::fromJson(readAll(rep)).object().value(QStringLiteral("Files")).toArray();
        QCOMPARE(files.size(), 2);
        QCOMPARE(files[0].toObject().value(QStringLiteral("Result")).toString(), QStringLiteral("Written; edited identifiers validated"));
    }

    void pkgInfoParsing()
    {
        const QString out = QStringLiteral(
            "[General]\r\n  File: C:\\x\\a.pkg\r\n  Size: 1 MB (1,057,438 byte)\r\n"
            "  Package type: FIH Debug \u2014 installable on a PS5 in debug mode\r\n"
            "  Image mode: PLAINTEXT_NOAUTH \u2014 unencrypted outer layer \u00b7 PPRPLAIN-NOAUTH!\r\n"
            "  Content ID: UP9000-PPSA01234_00-TESTDLC000000001\r\n"
            "  Content type: Additional content with data (AC) (0x21)\r\n  DRM: 0x10\r\n"
            "  Title: Test DLC One\r\n  Version: content 01.000.000 \u00b7 master 01.00\r\n"
            "  Required system software: \u2014\r\n\r\n[param.json]\r\n"
            "  contentId: UP9000-PPSA01234_00-TESTDLC000000001\r\n  contentVersion: 01.000.000\r\n"
            "  localizedParameters.defaultLanguage: en-US\r\n  titleName (en-US): Test DLC One\r\n"
            "  masterVersion: 01.00\r\n  titleId: PPSA01234\r\n  versionFileUri: \r\n");
        const DlcPackageInfo i = parsePkgInfoOutput(out);
        QCOMPARE(i.contentId, kCid);
        QCOMPARE(i.titleId, QStringLiteral("PPSA01234"));
        QCOMPARE(i.title, QStringLiteral("Test DLC One"));
        QCOMPARE(i.version, QStringLiteral("01.000.000"));
        QCOMPARE(i.contentType, 0x21);
        QCOMPARE(i.headerDrm, 0x10);
        QCOMPARE(i.imageMode, QStringLiteral("plaintext"));
        QVERIFY(i.isDlc);
        QVERIFY(i.hasParam);
        QVERIFY2(i.canRebuild, qPrintable(i.rebuildLimitation));
        QVERIFY(i.drm.startsWith(QStringLiteral("Entitlement")));
        // A game package is not DLC; a retail one is inspection-only.
        DlcPackageInfo g = parsePkgInfoOutput(QString(out).replace(QStringLiteral("Additional content with data (AC) (0x21)"),
                                                                   QStringLiteral("Game (0x1A)")));
        QVERIFY(!g.isDlc);
        QVERIFY(!g.canRebuild);
        DlcPackageInfo r = parsePkgInfoOutput(QString(out).replace(QStringLiteral("FIH Debug"), QStringLiteral("FIH Retail")));
        QVERIFY(r.official);
        QVERIFY(r.rebuildLimitation.contains(QStringLiteral("retail")));
    }

    void ps4Inspection()
    {
        // Big-endian header with param.sfo (0x1000) as the only wanted entry.
        QByteArray sfo("\0PSF", 4);
        sfo += QByteArray(16, '\0');
        // one entry: key "TITLE", utf-8 string
        QByteArray keyTable("TITLE\0", 6), dataTable("Old DLC\0", 8);
        const quint32 keyTableOff = 20 + 16, dataTableOff = keyTableOff + quint32(keyTable.size());
        qToLittleEndian<quint32>(keyTableOff, sfo.data() + 8);
        qToLittleEndian<quint32>(dataTableOff, sfo.data() + 12);
        qToLittleEndian<quint32>(1, sfo.data() + 16);
        QByteArray e(16, '\0');
        qToLittleEndian<quint16>(0, e.data());
        qToLittleEndian<quint16>(0x0204, e.data() + 2);
        qToLittleEndian<quint32>(8, e.data() + 4);
        qToLittleEndian<quint32>(0, e.data() + 12);
        sfo = sfo.left(20) + e + keyTable + dataTable;
        QByteArray pkg(0x200, '\0');
        pkg.replace(0, 4, QByteArray("\x7F" "CNT", 4));
        qToBigEndian<quint32>(1, pkg.data() + 0x10);
        qToBigEndian<quint32>(0x100, pkg.data() + 0x18);
        pkg.replace(0x40, 36, "UP9000-CUSA01234_00-OLDPS4DLC0000001");
        qToBigEndian<quint32>(0x1C, pkg.data() + 0x74);
        QByteArray entry(32, '\0');
        qToBigEndian<quint32>(0x1000, entry.data());
        qToBigEndian<quint32>(0x200, entry.data() + 16);
        qToBigEndian<quint32>(quint32(sfo.size()), entry.data() + 20);
        pkg.replace(0x100, 32, entry);
        pkg += sfo;
        const QString p = QDir(dir(QStringLiteral("ps4"))).filePath(QStringLiteral("old.pkg"));
        fwtest::writeTextFile(p, pkg);
        QVERIFY(looksLikePs4Package(p));
        auto i = inspectPs4Package(p);
        QVERIFY2(i.ok(), i.ok() ? "" : qPrintable(i.error().message));
        QVERIFY(i->ps4);
        QVERIFY(i->isDlc);
        QCOMPARE(i->title, QStringLiteral("Old DLC"));
        QCOMPARE(i->titleId, QStringLiteral("CUSA01234"));
        QVERIFY(!i->canRebuild);
        QVERIFY(i->rebuildLimitation.contains(QStringLiteral("PS4 PKG inspection is supported")));
        // Only the hex digits are upper-cased, not the words around them.
        QCOMPARE(i->drm, QStringLiteral("PS4 header DRM 0x0"));
        for (const auto& [k, v] : i->metadata)
            if (k == u"headerContentType")
                QCOMPARE(v, QStringLiteral("0x1C"));
    }

    // A crafted param.sfo inside a PS4 package is refused before it costs memory or time.
    void ps4InspectionRefusesHostileSfo()
    {
        struct Field {
            QByteArray key;
            quint32 length;
        };
        const auto package = [&](const QString& name, const std::vector<Field>& fields, quint32 claimedCount, quint32 dataBytes) {
            QByteArray table, keys;
            for (const Field& f : fields) {
                QByteArray e(16, '\0');
                qToLittleEndian<quint16>(quint16(keys.size()), e.data());
                qToLittleEndian<quint16>(0x0204, e.data() + 2);
                qToLittleEndian<quint32>(f.length, e.data() + 4);
                table += e;
                keys += f.key + '\0';
            }
            table.resize(qsizetype(claimedCount) * 16, '\0');
            QByteArray sfo("\0PSF", 4);
            sfo += QByteArray(16, '\0');
            qToLittleEndian<quint32>(quint32(20 + table.size()), sfo.data() + 8);
            qToLittleEndian<quint32>(quint32(20 + table.size() + keys.size()), sfo.data() + 12);
            qToLittleEndian<quint32>(claimedCount, sfo.data() + 16);
            sfo += table + keys + QByteArray(qsizetype(dataBytes), '\0');
            QByteArray pkg(0x200, '\0');
            pkg.replace(0, 4, QByteArray("\x7F" "CNT", 4));
            qToBigEndian<quint32>(1, pkg.data() + 0x10);
            qToBigEndian<quint32>(0x100, pkg.data() + 0x18);
            pkg.replace(0x40, 36, "UP9000-CUSA01234_00-OLDPS4DLC0000001");
            qToBigEndian<quint32>(0x1C, pkg.data() + 0x74);
            QByteArray entry(32, '\0');
            qToBigEndian<quint32>(0x1000, entry.data());
            qToBigEndian<quint32>(0x200, entry.data() + 16);
            qToBigEndian<quint32>(quint32(sfo.size()), entry.data() + 20);
            pkg.replace(0x100, 32, entry);
            pkg += sfo;
            const QString p = QDir(dir(QStringLiteral("ps4-hostile"))).filePath(name);
            fwtest::writeTextFile(p, pkg);
            return p;
        };
        // A sane file with these helpers parses.
        auto ok = inspectPs4Package(package(QStringLiteral("ok.pkg"), {{"TITLE", 8}, {"TITLE_ID", 8}}, 2, 64));
        QVERIFY2(ok.ok(), ok.ok() ? "" : qPrintable(ok.error().message));
        // Tens of thousands of entries (each would be scanned against all earlier ones).
        auto many = inspectPs4Package(package(QStringLiteral("many.pkg"), {{"A", 0}}, 60000, 0));
        QVERIFY(!many.ok());
        QCOMPARE(many.error().code, ErrorCode::CorruptData);
        // The same key twice with empty values used to slip through.
        auto dup = inspectPs4Package(package(QStringLiteral("dup.pkg"), {{"TITLE", 0}, {"title", 0}}, 2, 0));
        QVERIFY(!dup.ok());
        QVERIFY2(dup.error().message.contains(QStringLiteral("Duplicate")), qPrintable(dup.error().message));
        // One value far larger than any real one.
        auto big = inspectPs4Package(package(QStringLiteral("big.pkg"), {{"TITLE", 1024 * 1024}}, 1, 1024 * 1024));
        QVERIFY(!big.ok());
        QCOMPARE(big.error().code, ErrorCode::CorruptData);
        // Messages keep their case.
        QByteArray cut = readAll(package(QStringLiteral("cut.pkg"), {{"TITLE", 8}}, 1, 64));
        qToBigEndian<quint32>(quint32(cut.size()), reinterpret_cast<uchar*>(cut.data()) + 0x100 + 20); // entry larger than the file
        const QString cutPath = QDir(dir(QStringLiteral("ps4-hostile"))).filePath(QStringLiteral("cut2.pkg"));
        fwtest::writeTextFile(cutPath, cut);
        auto outside = inspectPs4Package(cutPath);
        QVERIFY(!outside.ok());
        QCOMPARE(outside.error().message, QStringLiteral("PS4 entry 0x1000 is outside the package bounds."));
    }

    void cliInfoParsing()
    {
        const FpkgCliInfo info = parseInfoOutput(QStringLiteral("PSVIETHOA\nLibProsperoPkg: 1.2.0\nPS5 debug keys: ready\n[OK] x\n"));
        QCOMPARE(info.engineVersion, QStringLiteral("1.2.0"));
        QVERIFY(info.debugKeysReady);
        QCOMPARE(info.componentLines.size(), 1);
    }

    // ---- end to end with fpkg-cli (skipped without it) -----------------------------------
    void endToEnd()
    {
        const QString cliPath = qEnvironmentVariable("FW_FPKG_CLI");
        if (cliPath.isEmpty())
            QSKIP("Set FW_FPKG_CLI to fpkg-cli.exe for the real DLC engine run.");
        const FpkgCli cli = locateFpkgCli(cliPath);
        QVERIFY2(cli.ok(), qPrintable(cli.problems.join(u' ')));

        auto schema = std::make_shared<SettingsSchema>();
        DependencyRegistry deps;
        JobFactory jobs;
        ModuleContext mc{*schema, deps, jobs};
        general::registerModule(mc);
        fpkg::registerModule(mc);
        dlc::registerModule(mc);
        const QVariantMap sv{{keys::kFpkgCliPath, cliPath}, {keys::kEntitlementKey, QStringLiteral("0123456789ABCDEF0123456789ABCDEF")}};
        const SettingsValues values(schema, sv, {});

        const QString d = dir(QStringLiteral("e2e"));
        const QString ini = QDir(d).filePath(QStringLiteral("dlc_emu.ini"));
        fwtest::writeTextFile(ini, "# Season Pass\r\n[PSAC]\r\ncontent_id=UP9000-PPSA01234_00-TESTDLC000000001\r\n");
        DlcItem a = iniItem(ini, 0, kCid, QStringLiteral("Season Pass"));
        // Retargeted to another region.
        a.titleId = QStringLiteral("PPSA05678");
        a.contentId = QStringLiteral("EP1004-PPSA05678_00-TESTDLC000000001");
        const QString out = dir(QStringLiteral("e2e/out"));
        DlcConvertRequest req;
        req.items = {a};
        req.output = out;
        DlcConvertJob job(req);
        QVERIFY(job.validate(values).ok());
        ProgressReporter reporter([](const ProgressSnapshot&) {}, 0);
        JobContext ctx;
        ctx.jobId = QStringLiteral("DLC-E2E");
        ctx.progress = &reporter;
        ctx.log = Log(job.kind(), ctx.jobId);
        ctx.settings = values;
        ctx.tempDir = dir(QStringLiteral("e2e/tmp1"));
        auto res = job.run(ctx);
        QVERIFY2(res.ok(), res.ok() ? "" : qPrintable(res.error().message));
        QCOMPARE(res->outputs.size(), 1);
        QCOMPARE(QFileInfo(res->outputs[0]).fileName(), QStringLiteral("EP1004-PPSA05678_00-TESTDLC000000001.pkg"));
        CancellationSource cs;
        auto info = inspectDlcPackage(cli, res->outputs[0], kZeroKey, cs.token());
        QVERIFY2(info.ok(), info.ok() ? "" : qPrintable(info.error().message));
        QCOMPARE(info->contentId, QStringLiteral("EP1004-PPSA05678_00-TESTDLC000000001"));
        QCOMPARE(info->title, QStringLiteral("Season Pass"));
        QVERIFY(info->isDlc);
        QVERIFY(info->canRebuild);

        // Rebuild that package for a third region (extract, retarget, rebuild).
        DlcItem p;
        p.source = res->outputs[0];
        p.entry = -1;
        p.detectedContentId = info->contentId;
        p.detectedTitleId = info->titleId;
        p.titleId = QStringLiteral("PPSA09999");
        p.contentId = QStringLiteral("JP0001-PPSA09999_00-TESTDLC000000001");
        p.title = QStringLiteral("Season Pass JP");
        p.version = info->version;
        p.sourceSize = info->size;
        p.sourceModifiedMs = info->modifiedUtc.toMSecsSinceEpoch();
        DlcConvertRequest req2;
        req2.items = {p};
        req2.output = out;
        DlcConvertJob job2(req2);
        ProgressReporter reporter2([](const ProgressSnapshot&) {}, 0);
        ctx.progress = &reporter2;
        ctx.tempDir = dir(QStringLiteral("e2e/tmp2"));
        auto res2 = job2.run(ctx);
        QVERIFY2(res2.ok(), res2.ok() ? "" : qPrintable(res2.error().message));
        auto info2 = inspectDlcPackage(cli, res2->outputs.value(0), kZeroKey, cs.token());
        QVERIFY(info2.ok());
        QCOMPARE(info2->contentId, QStringLiteral("JP0001-PPSA09999_00-TESTDLC000000001"));
        QCOMPARE(info2->titleId, QStringLiteral("PPSA09999"));
        QCOMPARE(info2->title, QStringLiteral("Season Pass JP"));
        // The report was written.
        bool report = false;
        for (const QString& f : QDir(out).entryList({QStringLiteral("DLC-results-*.json")}))
            report = report || !f.isEmpty();
        QVERIFY(report);
    }
};

QTEST_GUILESS_MAIN(TstDlcConvert)
#include "tst_dlc_convert.moc"
