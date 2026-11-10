// Backport update packages (the ppr-pkg Companion's Backport tab): the pure
// rules (versions, names, wrapper folders, firmware groups, request checks),
// the package inspector (kind, base digest, param.json, delta reference
// search), the SDK Fix toolchain check, and an end-to-end run with the
// owner's real Sony toolkit when FW_BACKPORT_E2E_TOOLKIT (a toolkit folder)
// and FW_BACKPORT_E2E_GAME (a game folder with sce_sys/param.json) are set.

#include "core/FsLinks.h"
#include "core/Paths.h"
#include "engines/fpkg/AsciiAliases.h"
#include "engines/fpkg/BackportUpdate.h"
#include "engines/fpkg/ContentNaming.h"
#include "engines/fpkg/FpkgSettings.h"
#include "engines/fpkg/PackageInspect.h"
#include "engines/fpkg/Toolchain.h"

#include "FpkgTestUtil.h"

#include <QCryptographicHash>
#include <QDirIterator>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>

#include <thread>

using namespace fw;
using namespace fw::fpkg;
using namespace fpkgtest;

class TstBackportUpdate : public QObject {
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
        if (qEnvironmentVariableIsSet("FW_E2E_KEEP"))
            m_tmp.setAutoRemove(false);
        fwtest::isolateAppData(m_tmp);
    }

    void versionCarry()
    {
        QCOMPARE(nextBackportVersion(QStringLiteral("01.000.000")), QStringLiteral("01.000.001"));
        QCOMPARE(nextBackportVersion(QStringLiteral("01.000.999")), QStringLiteral("01.001.000"));
        QCOMPARE(nextBackportVersion(QStringLiteral("01.999.999")), QStringLiteral("02.000.000"));
        QCOMPARE(nextBackportVersion(QStringLiteral("99.999.999")), QString());
        QCOMPARE(nextBackportVersion(QStringLiteral("1.0.0")), QString());
    }

    void updateVersionRules()
    {
        QCOMPARE(backportUpdateVersion(QStringLiteral("01.005.000"), {}).value(), QStringLiteral("01.005.001"));
        QCOMPARE(backportUpdateVersion(QStringLiteral("01.005.000"), QStringLiteral(" 01.006.000 ")).value(), QStringLiteral("01.006.000"));
        auto low = backportUpdateVersion(QStringLiteral("01.005.000"), QStringLiteral("01.005.000"));
        QVERIFY(!low.ok());
        QVERIFY(low.error().message.contains(QStringLiteral("must be greater than the base")));
        auto max = backportUpdateVersion(QStringLiteral("99.999.999"), {});
        QVERIFY(!max.ok());
        QVERIFY(max.error().message.contains(QStringLiteral("maximum version")));
        QVERIFY(!backportUpdateVersion(QStringLiteral("x"), {}).ok());
    }

    void names()
    {
        QCOMPARE(backportPackageName(QStringLiteral("PPSA01234"), QStringLiteral("4xx")), QStringLiteral("PPSA01234-backport-4xx.pkg"));
        QCOMPARE(backportPackageName(QStringLiteral("PPSA01234"), QStringLiteral("5xxplus")), QStringLiteral("PPSA01234-backport-5xxplus.pkg"));
        const QString out = dir(QStringLiteral("names"));
        const QString wanted = QDir(out).filePath(QStringLiteral("PPSA01234-backport-4xx.pkg"));
        QCOMPARE(availableOutputPath(wanted), wanted);
        put(out, QStringLiteral("PPSA01234-backport-4xx.pkg"), "old");
        QCOMPARE(QFileInfo(availableOutputPath(wanted)).fileName(), QStringLiteral("PPSA01234-backport-4xx-2.pkg"));
        put(out, QStringLiteral("PPSA01234-backport-4xx-2.pkg.naps_metric.json"), "{}");
        QCOMPARE(QFileInfo(availableOutputPath(wanted)).fileName(), QStringLiteral("PPSA01234-backport-4xx-3.pkg"));
    }

    void wrapperFolders()
    {
        const QString b = dir(QStringLiteral("wrap"));
        put(b, QStringLiteral("PPSA01234-app/eboot.bin"), "e");
        put(b, QStringLiteral("PPSA01234-app/sce_module/libc.prx"), "m");
        QCOMPARE(QFileInfo(resolveBackportFolder(b)).fileName(), QStringLiteral("PPSA01234-app"));
        // A folder holding only sce_module is not a wrapper.
        const QString m = dir(QStringLiteral("onlymodule"));
        put(m, QStringLiteral("sce_module/libc.prx"), "m");
        QCOMPARE(QDir::cleanPath(resolveBackportFolder(m)), QDir::cleanPath(QFileInfo(m).absoluteFilePath()));
    }

    void firmwareGroups()
    {
        QCOMPARE(firmwareGroupFromName(QStringLiteral("FW 4xx PPSA31544 FINAL FANTASY (01.000.001) backport files.zip")), QStringLiteral("4xx"));
        QCOMPARE(firmwareGroupFromName(QStringLiteral("FW 5xx+ PPSA31544 X backport files.zip")), QStringLiteral("5xxplus"));
        QCOMPARE(firmwareGroupFromName(QStringLiteral("SDK 7 Game.zip")), QStringLiteral("5xxplus"));
        QCOMPARE(firmwareGroupFromName(QStringLiteral("Game.zip")), QString());
    }

    void requestChecks()
    {
        const QString base = dir(QStringLiteral("req"));
        put(base, QStringLiteral("base.pkg"), QByteArray(100, 'x'));
        const QString b4 = dir(QStringLiteral("req/b4"));
        put(b4, QStringLiteral("eboot.bin"), "e");
        BackportUpdateRequest r;
        r.basePackage = QDir(base).filePath(QStringLiteral("base.pkg"));
        FpkgSettings s;
        QVERIFY(validateBackportRequest(r, s).error().message.contains(QStringLiteral("at least one backport")));
        r.backport4xx = b4;
        r.backport5xx = QDir(b4).filePath(QStringLiteral("nested"));
        QDir().mkpath(r.backport5xx);
        QVERIFY(validateBackportRequest(r, s).error().message.contains(QStringLiteral("separate trees")));
        r.backport5xx.clear();
        r.output = QDir(b4).filePath(QStringLiteral("out"));
        QVERIFY(validateBackportRequest(r, s).error().message.contains(QStringLiteral("outside the 4.xx backport files")));
        r.output.clear();
        r.version = QStringLiteral("1.2");
        QVERIFY(validateBackportRequest(r, s).error().message.contains(QStringLiteral("NN.NNN.NNN")));
        // Round trip through the job parameters.
        r.version = QStringLiteral("01.000.001");
        auto back = BackportUpdateRequest::fromJson(r.toJson());
        QVERIFY(back.ok());
        QCOMPARE(back->backport4xx, r.backport4xx);
        QCOMPARE(back->version, r.version);
        QVERIFY(!BackportUpdateRequest::fromJson(QJsonObject{}).ok());
    }

    void inspectFullPackage()
    {
        const QString d = dir(QStringLiteral("inspect"));
        QByteArray pkg("\x7F" "FIH", 4);
        pkg += QByteArray(0x2C, '\0');
        QByteArray digest;
        for (int i = 0; i < 32; ++i)
            digest += char(0xA0 + i);
        pkg += digest;                      // 0x30..0x4F
        pkg += QByteArray(4096, '\0');
        pkg += QByteArray("{\"ageLevel\":{\"default\":0},\"contentId\":\"UP0000-PPSA01234_00-GOLDENTEST000000\","
                          "\"contentVersion\":\"01.002.000\",\"localizedParameters\":{\"defaultLanguage\":\"en-US\","
                          "\"en-US\":{\"titleName\":\"Golden\"}}}");
        pkg += QByteArray(2048, '\0');
        put(d, QStringLiteral("base.pkg"), pkg);
        CancellationSource cs;
        auto id = inspectPackage(QDir(d).filePath(QStringLiteral("base.pkg")), cs.token());
        QVERIFY(id.ok());
        QCOMPARE(id->kind, PackageKind::Ps5);
        QCOMPARE(id->digest, digest);
        QVERIFY(id->hasParam);
        QCOMPARE(id->contentId, QStringLiteral("UP0000-PPSA01234_00-GOLDENTEST000000"));
        QCOMPARE(id->titleId, QStringLiteral("PPSA01234"));
        QCOMPARE(id->version, QStringLiteral("01.002.000"));
        QCOMPARE(id->title, QStringLiteral("Golden"));

        QByteArray delta("\x7F" "LIH", 4);
        delta += QByteArray(200, '\0') + digest + QByteArray(100, '\0') + digest;
        put(d, QStringLiteral("update.pkg"), delta);
        auto di = inspectPackage(QDir(d).filePath(QStringLiteral("update.pkg")), cs.token());
        QVERIFY(di.ok());
        QCOMPARE(di->kind, PackageKind::Ps5Delta);
        QCOMPARE(countOccurrences(QDir(d).filePath(QStringLiteral("update.pkg")), digest, cs.token()).value(), qint64(2));
        put(d, QStringLiteral("short.pkg"), "abc");
        QVERIFY(!inspectPackage(QDir(d).filePath(QStringLiteral("short.pkg")), cs.token()).ok());
    }

    void updateImageHoldsSeveralParams()
    {
        // As Sony's remastered/update images: the new param.json first, then
        // the base's copies with the old version.
        const QString d = dir(QStringLiteral("remastered"));
        const auto obj = [](const char* v) {
            return QByteArray("{\"contentId\":\"UP0000-PPSA01234_00-GOLDENTEST000000\",\"contentVersion\":\"") + v
                 + QByteArray("\",\"titleId\":\"PPSA01234\"}");
        };
        QByteArray pkg("" "FIH", 4);
        pkg += QByteArray(4000, '\0') + obj("01.005.001") + QByteArray(9000, '\0') + obj("01.005.000")
             + QByteArray(2000, '\0') + obj("01.005.000") + QByteArray(100, '\0');
        put(d, QStringLiteral("r.pkg"), pkg);
        CancellationSource cs;
        auto id = inspectPackage(QDir(d).filePath(QStringLiteral("r.pkg")), cs.token());
        QVERIFY(id.ok());
        QCOMPARE(id->version, QStringLiteral("01.005.001"));
    }

    // A crafted tail: runs of '{' before every "contentId" and no closing
    // brace anywhere. The search has a work limit and gives up quickly.
    void hostileTailIsGivenUpQuickly()
    {
        const QString d = dir(QStringLiteral("hostile"));
        QByteArray pkg("\x7F" "FIH", 4);
        pkg += QByteArray(4096, '\0');
        const QByteArray unit("{{{{{{{{{{{{\"contentId\"");
        pkg.reserve(pkg.size() + 4 * 1024 * 1024 + unit.size());
        for (int n = 0; n < 4 * 1024 * 1024; n += int(unit.size()))
            pkg += unit;
        put(d, QStringLiteral("hostile.pkg"), pkg);
        const QString path = QDir(d).filePath(QStringLiteral("hostile.pkg"));

        QElapsedTimer t;
        t.start();
        auto id = inspectPackage(path, CancellationToken());
        QVERIFY(id.ok());
        QCOMPARE(id->kind, PackageKind::Ps5);
        QVERIFY(!id->hasParam);
        QVERIFY2(t.elapsed() < 30000, qPrintable(QStringLiteral("%1 ms").arg(t.elapsed())));

        // Cancel is seen between candidates, not only between 8 MiB blocks.
        CancellationSource cs;
        std::thread canceller([&] {
            QThread::msleep(5);
            cs.cancel(QStringLiteral("test"));
        });
        auto cancelled = findParamJsonInPackage(path, cs.token());
        canceller.join();
        QVERIFY(!cancelled.ok());
        QVERIFY(cancelled.error().isCancelled());

        // A real param.json behind a short run of the pattern is still found.
        QByteArray mixed("\x7F" "FIH", 4);
        mixed += QByteArray(4096, '\0');
        mixed += QByteArray("{\"contentId\":\"UP0000-PPSA01234_00-GOLDENTEST000000\",\"contentVersion\":\"01.000.000\"}");
        mixed += QByteArray(512, '\0');
        for (int n = 0; n < 40; ++n)
            mixed += unit;
        put(d, QStringLiteral("mixed.pkg"), mixed);
        auto found = inspectPackage(QDir(d).filePath(QStringLiteral("mixed.pkg")), CancellationToken());
        QVERIFY(found.ok());
        QCOMPARE(found->version, QStringLiteral("01.000.000"));
    }

    void countAcrossBlocks()
    {
        // The needle straddles the 8 MiB read boundary.
        const QString d = dir(QStringLiteral("blocks"));
        const QByteArray needle("NEEDLE-0123456789-NEEDLE");
        QByteArray data(8 * 1024 * 1024 - 10, 'z');
        data += needle;
        data += QByteArray(1000, 'z');
        data += needle;
        put(d, QStringLiteral("big.bin"), data);
        CancellationSource cs;
        QCOMPARE(countOccurrences(QDir(d).filePath(QStringLiteral("big.bin")), needle, cs.token()).value(), qint64(2));
    }

    void playGoTableScan()
    {
        const QString d = dir(QStringLiteral("plgx"));
        QByteArray table = buildPlgx({{langBit(QStringLiteral("en-US")), "c0"}, {langBit(QStringLiteral("ja-JP")), "c1"}},
                                     {{1, "s", {0, 1}}}, 0, QStringLiteral("en-US"), langBit(QStringLiteral("en-US")));
        QByteArray pkg("\x7F" "FIH", 4);
        pkg += QByteArray(5000, '\0') + table + QByteArray(300, '\0');
        put(d, QStringLiteral("p.pkg"), pkg);
        CancellationSource cs;
        auto found = findPlayGoChunkDatInPackage(QDir(d).filePath(QStringLiteral("p.pkg")), cs.token());
        QVERIFY(found.ok());
        QVERIFY(found->has_value());
        QCOMPARE(**found, table);
    }

    void sdkFixToolchain()
    {
        // Wrong hashes: refused with the Companion's text.
        const QString tk = dir(QStringLiteral("toolkit/toolchain"));
        put(tk, QStringLiteral("prospero-pub-cmd.exe"), "not sony");
        PubToolchain tc;
        tc.exe = QDir(tk).filePath(QStringLiteral("prospero-pub-cmd.exe"));
        tc.binDir = tk;
        SdkFixStatus st = checkSdkFixToolchain(tc);
        QVERIFY(!st.ready());
        QVERIFY(st.text.contains(QStringLiteral("libScePubTools.dll is missing")));
        put(tk, QStringLiteral("libScePubTools.dll"), "not sony either");
        st = checkSdkFixToolchain(tc);
        QVERIFY(st.text.startsWith(QStringLiteral("Not a verified SDK Fix toolchain")));
        QVERIFY(!checkSdkFixToolchain(PubToolchain{}).ready());
        QCOMPARE(sdkFixReleases().size(), 2);
        QCOMPARE(sdkFixExtFiles().size(), 7);
    }

    void staleWorkFoldersAreSwept()
    {
        // As left by a crash: listed in the index with a marker of a process that no longer runs.
        const QString out = dir(QStringLiteral("sweep"));
        const QString dead = QDir(out).filePath(QStringLiteral(".ppr-backport-ppsa01234-0badf00d"));
        const QString mine = QDir(out).filePath(QStringLiteral(".ppr-backport-ppsa01234-12345678"));
        const QString unmarked = QDir(out).filePath(QStringLiteral(".ppr-sdkbase-abc"));
        const QString foreign = QDir(out).filePath(QStringLiteral("My Game"));
        // The marker of a process that has ended.
        QProcess ended;
        ended.start(QStringLiteral("cmd.exe"), {QStringLiteral("/c"), QStringLiteral("exit")});
        QVERIFY(ended.waitForStarted());
        const qint64 deadPid = ended.processId();
        QVERIFY(ended.waitForFinished());
        put(dead, QStringLiteral(".fw-owned"), QByteArray::number(deadPid));
        put(dead, QStringLiteral("update.partial.pkg"), QByteArray(1000, 'x'));
        put(mine, QStringLiteral(".fw-owned"), QByteArray::number(QCoreApplication::applicationPid()));
        put(unmarked, QStringLiteral("package.partial.pkg"), "x");
        put(foreign, QStringLiteral(".fw-owned"), QByteArray::number(deadPid));
        const QString index = QDir(Paths::localDir()).filePath(QStringLiteral("state/backport-work.txt"));
        QVERIFY(fwtest::writeTextFile(index, QStringList{QDir::toNativeSeparators(dead), QDir::toNativeSeparators(mine),
                                                         QDir::toNativeSeparators(unmarked), QDir::toNativeSeparators(foreign),
                                                         QDir::toNativeSeparators(QDir(out).filePath(QStringLiteral(".ppr-backport-gone-1")))}
                                                 .join(u'\n').toUtf8()));
        QVERIFY(!processIsRunning(deadPid));
        QCOMPARE(sweepStaleBackportWork(), 1);
        QVERIFY(!QFileInfo::exists(dead));
        QVERIFY(QFileInfo::exists(mine));
        QVERIFY(QFileInfo::exists(unmarked));
        QVERIFY(QFileInfo::exists(foreign));
        const QByteArray left = readAll(index);
        QVERIFY(!left.contains("0badf00d"));
        QVERIFY(!left.contains("gone-1"));
        QVERIFY(left.contains("12345678"));
    }

    void sweepKeepsUnreachableFoldersAndSeesReusedIds()
    {
        const QString out = dir(QStringLiteral("sweep2"));
        const QString index = QDir(Paths::localDir()).filePath(QStringLiteral("state/backport-work.txt"));
        // A folder made by this run is its own: the marker carries the process id and start time.
        const QString own = QDir(out).filePath(QStringLiteral(".fw-stage-0000aaaa"));
        QVERIFY(createTrackedWorkFolder(own).ok());
        const QByteArray marker = readAll(QDir(own).filePath(QStringLiteral(".fw-owned")));
        QVERIFY(marker.startsWith(QByteArray::number(QCoreApplication::applicationPid())));
        // As left by an earlier process that had the id this one has now: the same id, another start time.
        const QString reused = QDir(out).filePath(QStringLiteral(".fw-stage-0000bbbb"));
        put(reused, QStringLiteral(".fw-owned"), QByteArray::number(QCoreApplication::applicationPid()) + " 12345");
        put(reused, QStringLiteral("big.bin"), "x");
        // On a drive that is not connected right now: the line must survive the sweep.
        QString offline;
        for (char letter = 'Z'; letter >= 'D' && offline.isEmpty(); --letter)
            if (!QFileInfo::exists(QStringLiteral("%1:/").arg(QLatin1Char(letter))))
                offline = QStringLiteral("%1:\\out\\.fw-stage-0000cccc").arg(QLatin1Char(letter));
        QStringList lines = QString::fromUtf8(readAll(index)).split(u'\n', Qt::SkipEmptyParts);
        lines << QDir::toNativeSeparators(reused);
        if (!offline.isEmpty())
            lines << offline;
        QVERIFY(fwtest::writeTextFile(index, lines.join(u'\n').toUtf8()));

        const int removed = sweepStaleBackportWork();
        QVERIFY(QFileInfo::exists(own));
#ifdef Q_OS_WIN
        QCOMPARE(removed, 1);
        QVERIFY(!QFileInfo::exists(reused));
        if (!offline.isEmpty())
            QVERIFY(readAll(index).contains("0000cccc"));
#else
        Q_UNUSED(removed);
#endif
        removeTrackedWorkFolder(own);
        QVERIFY(!QFileInfo::exists(own));
    }

    // An update keeps the base's PlayGo layout whatever the PlayGo settings say.
    void updateSettingsKeepTheBaseLayout()
    {
        FpkgSettings s;
        s.playGoFallback = false;
        s.useSourcePlayGoTable = true;
        s.playGoAllLanguages = true;
        s.outputConflict = OutputConflict::Overwrite;
        const FpkgSettings in = backportUpdateSettings(s, 7, {QStringLiteral("en-US"), QStringLiteral("ja-JP")});
        QCOMPARE(in.playGoChunkCount, 7);
        QCOMPARE(in.playGoForcedLanguages, (QStringList{QStringLiteral("en-US"), QStringLiteral("ja-JP")}));
        QVERIFY(in.playGoFallback && !in.useSourcePlayGoTable && !in.playGoAllLanguages);
        QCOMPARE(in.outputConflict, OutputConflict::Fail);
        QCOMPARE(in.postVerify, PostVerify::HeaderOnly);
        QVERIFY(!in.clearVersionFileUri && !in.lowerRequiredFirmware && !in.clearAttribute3);
        // The base's table could not be read: the user's PlayGo settings stay.
        const FpkgSettings plain = backportUpdateSettings(s, 0, {});
        QVERIFY(!plain.playGoFallback && plain.useSourcePlayGoTable && plain.playGoAllLanguages);
    }

    void digestPlausibility()
    {
        QByteArray digest;
        for (int i = 0; i < 32; ++i)
            digest += char(0xA0 + i);
        QVERIFY(isPlausibleDigest(digest));
        QVERIFY(!isPlausibleDigest(QByteArray(32, '\0')));
        QVERIFY(!isPlausibleDigest(QByteArray(32, 'x')));
        QVERIFY(!isPlausibleDigest(QByteArray("\x01\x02\x03\x04", 4).repeated(8)));
        QVERIFY(!isPlausibleDigest(digest.left(31)));
    }

    void publisherTemp()
    {
        bool aliased = false;
        const QString t = publisherTempPath(m_tmp.path(), &aliased);
        QVERIFY(!t.isEmpty());
        QVERIFY(t.size() <= QDir::toNativeSeparators(QFileInfo(m_tmp.path()).absoluteFilePath()).size());
    }

    // ---- end to end with real tools (skipped without them) -----------------------------
    void endToEnd()
    {
        const QString toolkit = qEnvironmentVariable("FW_BACKPORT_E2E_TOOLKIT");
        const QString gameSrc = qEnvironmentVariable("FW_BACKPORT_E2E_GAME");
        if (toolkit.isEmpty() || gameSrc.isEmpty())
            QSKIP("Set FW_BACKPORT_E2E_TOOLKIT and FW_BACKPORT_E2E_GAME for the real Sony toolkit run.");
        const PubToolchain tc = locatePubTools(toolkit);
        QVERIFY2(tc.ok(), qPrintable(tc.problems.join(u' ')));
        QVERIFY2(checkSdkFixToolchain(tc).ready(), qPrintable(checkSdkFixToolchain(tc).text));

        // A copy of the game with what a package needs (keystone, icon) and two
        // backport folders that replace and add files.
        const QString game = dir(QStringLiteral("e2e/game"));
        QDirIterator it(gameSrc, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString f = it.next();
            const QString rel = QDir(gameSrc).relativeFilePath(f);
            QDir().mkpath(QFileInfo(QDir(game).filePath(rel)).absolutePath());
            QVERIFY(QFile::copy(f, QDir(game).filePath(rel)));
        }
        if (!QFileInfo::exists(QDir(game).filePath(QStringLiteral("sce_sys/keystone"))))
            put(game, QStringLiteral("sce_sys/keystone"), QByteArray(96, 'K'));
        const QString b4 = dir(QStringLiteral("e2e/backport4"));
        QFile::copy(QDir(game).filePath(QStringLiteral("eboot.bin")), QDir(b4).filePath(QStringLiteral("eboot.bin")));
        put(b4, QStringLiteral("fakelib/backport-marker-4xx.txt"), "4xx backport");
        const QString b5 = dir(QStringLiteral("e2e/backport5"));
        QFile::copy(QDir(game).filePath(QStringLiteral("eboot.bin")), QDir(b5).filePath(QStringLiteral("eboot.bin")));
        put(b5, QStringLiteral("fakelib/backport-marker-5xx.txt"), "5xx+ backport");
        const QString out = dir(QStringLiteral("e2e/out"));

        Env env;
        QVariantMap sv{{keys::kPubToolsPath, toolkit}};
        BackportUpdateRequest r;
        r.gameFolder = game;
        r.backport4xx = b4;
        r.backport5xx = b5;
        r.output = out;
        r.buildBase = true;
        FpkgBackportJob job(r);
        const SettingsValues values = env.values(sv);
        const Status valid = job.validate(values);
        QVERIFY2(valid.ok(), valid.ok() ? "" : qPrintable(valid.error().message));
        ProgressReporter reporter([](const ProgressSnapshot&) {}, 0);
        JobContext ctx;
        ctx.jobId = QStringLiteral("E2E");
        ctx.progress = &reporter;
        ctx.log = Log(job.kind(), ctx.jobId);
        ctx.settings = values;
        ctx.tempDir = dir(QStringLiteral("e2e/jobtemp"));
        auto res = job.run(ctx);
        QVERIFY2(res.ok(), res.ok() ? "" : qPrintable(res.error().message + QStringLiteral(" | ") + res.error().detail));
        qInfo() << "outputs" << res->outputs;
        qInfo() << "summary" << res->summary << res->verificationDetail;
        QCOMPARE(res->outputs.size(), 3);
        const QString titleId = QFileInfo(res->outputs[1]).fileName().left(9);
        QCOMPARE(QFileInfo(res->outputs[1]).fileName(), QStringLiteral("%1-backport-4xx.pkg").arg(titleId));
        QCOMPARE(QFileInfo(res->outputs[2]).fileName(), QStringLiteral("%1-backport-5xxplus.pkg").arg(titleId));
        CancellationSource cs;
        const auto baseId = inspectPackage(res->outputs[0], cs.token());
        QVERIFY(baseId.ok());
        for (int i = 1; i <= 2; ++i) {
            const auto d = inspectPackage(res->outputs[i], cs.token());
            QVERIFY(d.ok());
            QCOMPARE(d->kind, PackageKind::Ps5Delta);
            QVERIFY(countOccurrences(res->outputs[i], baseId->digest, cs.token()).value() >= 1);
        }
        // Nothing else left in the output folder but the logs, metrics and packages.
        for (const QFileInfo& fi : QDir(out).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden))
            QVERIFY2(!fi.fileName().startsWith(QStringLiteral(".ppr-backport-")), qPrintable(fi.fileName()));

        // A second run against the existing base: the existing updates are kept, new ones get -2.
        BackportUpdateRequest again;
        again.basePackage = res->outputs[0];
        again.backport4xx = b4;
        again.output = out;
        FpkgBackportJob job2(again);
        ProgressReporter reporter2([](const ProgressSnapshot&) {}, 0);
        ctx.progress = &reporter2;
        ctx.tempDir = dir(QStringLiteral("e2e/jobtemp2"));
        auto res2 = job2.run(ctx);
        QVERIFY2(res2.ok(), res2.ok() ? "" : qPrintable(res2.error().message));
        QCOMPARE(QFileInfo(res2->outputs.value(0)).fileName(), QStringLiteral("%1-backport-4xx-2.pkg").arg(titleId));
    }
};

QTEST_GUILESS_MAIN(TstBackportUpdate)
#include "tst_backport_update.moc"
