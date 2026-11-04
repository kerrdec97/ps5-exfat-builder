// FPKG engine: naming, param.json, size class, scanner and dump rules, PlayGo
// layouts and tables, GP5 bytes (goldens written by Python's ElementTree, see
// tests/data/fpkg/make_goldens.py), tool output parsing, header check,
// conflict policy, input repairs, settings, toolchain lookup, source stagers.

#include "FpkgTestUtil.h"

#include "core/FsLinks.h"
#include "engines/fpkg/ContentNaming.h"
#include "engines/fpkg/DumpRules.h"
#include "engines/fpkg/FpkgBuilder.h"
#include "engines/fpkg/Gp5.h"
#include "engines/fpkg/InputRepair.h"
#include "engines/fpkg/PackageHeader.h"
#include "engines/fpkg/ParamJson.h"
#include "engines/fpkg/PlayGo.h"
#include "engines/fpkg/PubCmd.h"
#include "engines/fpkg/SourceScanner.h"
#include "engines/fpkg/SourceStager.h"
#include "engines/fpkg/Toolchain.h"

#include <QCryptographicHash>
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

using namespace fw;
using namespace fw::fpkg;
using namespace fpkgtest;

namespace {

json::Value parsed(const char* text)
{
    auto r = json::parse(QByteArray(text));
    return r.ok() ? r.value() : json::Value();
}

QByteArray fakePackage(bool fih, bool marker, int mode = 13, char signedByte = 0, quint64 sb = 0x1000, int size = 0x2000)
{
    QByteArray d(size, '\0');
    if (fih) {
        d.replace(0, 4, QByteArray("\x7F" "FIH", 4));
        d[5] = signedByte;
        putLe64(d, 0x20, sb);
        if (sb + 0x380 <= quint64(size)) {
            putLe16(d, int(sb) + 0x1C, quint16(mode));
            if (marker)
                d.replace(int(sb) + 0x370, 16, "PPRPLAIN-NOAUTH!");
        }
    }
    return d;
}

QByteArray makeUcp(const QList<QPair<QByteArray, QByteArray>>& entries)
{
    const int header = 0x60, rec = 0x40;
    qint64 cursor = header + qint64(entries.size()) * rec;
    std::vector<qint64> offsets;
    for (const auto& e : entries) {
        offsets.push_back(cursor);
        cursor = ((cursor + e.second.size()) / 16 + 1) * 16;
    }
    const qint64 total = entries.isEmpty() ? header : cursor;
    QByteArray d(int(total), '\0');
    qToBigEndian<quint32>(0xB228C60A, d.data());
    qToBigEndian<quint32>(1, d.data() + 4);
    qToBigEndian<quint64>(quint64(total), d.data() + 8);
    qToBigEndian<quint32>(quint32(entries.size()), d.data() + 0x10);
    qToBigEndian<quint32>(0x40, d.data() + 0x14);
    for (int i = 0; i < entries.size(); ++i) {
        const int r = header + i * rec;
        d.replace(r, entries[i].first.size(), entries[i].first);
        qToBigEndian<quint64>(quint64(offsets[size_t(i)]), d.data() + r + 32);
        qToBigEndian<quint64>(quint64(entries[i].second.size()), d.data() + r + 40);
        d.replace(int(offsets[size_t(i)]), entries[i].second.size(), entries[i].second);
    }
    const QByteArray digest = QCryptographicHash::hash(d, QCryptographicHash::Sha1);
    d.replace(0x1C, 20, digest);
    return d;
}

class FakeStager : public SourceStager {
public:
    QString displayName() const override { return QStringLiteral("test image"); }
    QStringList extensions() const override { return {QStringLiteral("exfat")}; }
    Status stage(const QString&, const QString&, const CancellationToken&, ProgressReporter*) override { return success(); }
};

} // namespace

class TstFpkgRules : public QObject {
    Q_OBJECT
    QTemporaryDir m_root;

    QString dir(const QString& name)
    {
        const QString p = m_root.filePath(name);
        QDir(p).removeRecursively();
        QDir().mkpath(p);
        return p;
    }

    // Golden plan: real paths mapped to C:\dump and C:\out.
    QByteArray goldenProject(const QString& dump, const QString& out, const QVariantMap& settings, BuildPlan* planOut = nullptr)
    {
        Env env;
        const FpkgSettings s = FpkgSettings::from(env.values(settings));
        PlanOptions po;
        po.roots = {dump};
        po.outputDir = out;
        auto plan = planBuild(po, s, CancellationToken());
        if (!plan.ok()) {
            qWarning() << plan.error().toLogString();
            return {};
        }
        const auto toTool = [&](const QString& real) {
            const QString r = QDir::cleanPath(real);
            QString mapped = r;
            if (r.startsWith(dump + u'/'))
                mapped = QStringLiteral("C:/dump/") + r.mid(dump.size() + 1);
            else if (r.startsWith(out + u'/'))
                mapped = QStringLiteral("C:/out/") + r.mid(out.size() + 1);
            return mapped.replace(u'/', u'\\');
        };
        const QByteArray bytes = writeGp5(projectFromPlan(plan.value(), s, toTool));
        if (planOut)
            *planOut = plan.value();
        return bytes;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_root.isValid());
        fwtest::isolateAppData(m_root);
    }

    // ---- naming --------------------------------------------------------------------
    void contentVersionAndNames()
    {
        QVERIFY(!ContentVersion::parse(QStringLiteral("1.00")));
        QVERIFY(!ContentVersion::parse(QStringLiteral("01.00.000")));
        const auto v = ContentVersion::parse(QStringLiteral("01.512.000"));
        QVERIFY(v);
        QCOMPARE(v->text(), QStringLiteral("01.512.000"));
        QCOMPARE(v->packageComponent(), QStringLiteral("01512"));
        QCOMPARE(ContentVersion::parse(QStringLiteral("01.000.000"))->packageComponent(), QStringLiteral("0100"));
        QCOMPARE(ContentVersion::parse(QStringLiteral("01.020.000"))->packageComponent(), QStringLiteral("0120"));
        QCOMPARE(ContentVersion::parse(QStringLiteral("01.000.000"))->next().text(), QStringLiteral("01.001.000"));
        QCOMPARE(ContentVersion::parse(QStringLiteral("01.999.005"))->next().text(), QStringLiteral("02.000.000"));
        QCOMPARE(ContentVersion::parse(QStringLiteral("01.002.000"))->compare(*ContentVersion::parse(QStringLiteral("01.001.999"))), 1);
        QCOMPARE(ContentVersion::parse(QStringLiteral("01.001.000"))->compare(*ContentVersion::parse(QStringLiteral("01.001.000"))), 0);

        const QString stem = packageStem(kCid, *ContentVersion::parse(QStringLiteral("01.020.000")));
        QCOMPARE(stem, kCid + QStringLiteral("-A0120-V0120"));
        QCOMPARE(fullPackageName(stem), stem + QStringLiteral(".pkg"));
        QCOMPARE(updatePackageName(stem), QStringLiteral("UPDATE_") + stem + QStringLiteral(".pkg"));
        QCOMPARE(companionName(stem), stem + QStringLiteral(".remastered.pkg"));
        QCOMPARE(numberedName(QStringLiteral("a.pkg"), 1), QStringLiteral("a (1).pkg"));
        QCOMPARE(numberedName(QStringLiteral("a.remastered.pkg"), 2), QStringLiteral("a (2).remastered.pkg"));

        QVERIFY(isValidContentId(kCid));
        QVERIFY(isValidContentId(QStringLiteral("UP0000-PPSA01234_01-ABCDEFGHIJKLMNOP")));
        QVERIFY(!hasStandardContentIdSuffix(QStringLiteral("UP0000-PPSA01234_01-ABCDEFGHIJKLMNOP")));
        QVERIFY(!isValidContentId(QStringLiteral("PPSA01234")));
        QVERIFY(!isValidContentId(QStringLiteral("up0000-PPSA01234_00-ABCDEFGHIJKLMNOP")));
        QCOMPARE(titleIdOf(kCid), QStringLiteral("PPSA01234"));
    }

    // ---- param.json -----------------------------------------------------------------
    void paramPatch()
    {
        ParamPatchOptions o;
        // DRM: missing, non-string, "free" -> "standard"; "standard" untouched.
        for (const char* text : {"{}", "{\"applicationDrmType\":5}", "{\"applicationDrmType\":\"free\"}"}) {
            json::Value p = parsed(text);
            QVERIFY(!applyParamPatch(p, o).isEmpty());
            QCOMPARE(p.value(QStringLiteral("applicationDrmType")).toString(), QStringLiteral("standard"));
        }
        {
            json::Value p = parsed("{\"applicationDrmType\":\"standard\",\"versionFileUri\":\"\",\"attribute3\":0}");
            o.clearAttribute3 = true;
            QVERIFY(applyParamPatch(p, o).isEmpty());
        }
        {
            json::Value p = parsed("{\"versionFileUri\":\"http://x\",\"attribute3\":8388736}");
            ParamPatchOptions off;
            off.clearVersionFileUri = false;
            applyParamPatch(p, off);
            QCOMPARE(p.value(QStringLiteral("versionFileUri")).toString(), QStringLiteral("http://x"));
            QCOMPARE(p.value(QStringLiteral("attribute3")).toInt(), qint64(8388736));
            ParamPatchOptions on;
            on.clearAttribute3 = true;
            applyParamPatch(p, on);
            QCOMPARE(p.value(QStringLiteral("versionFileUri")).toString(), QString());
            QCOMPARE(p.value(QStringLiteral("attribute3")).toInt(), qint64(0));
        }
        {
            json::Value p = parsed("{\"versionFileUri\":7}");
            applyParamPatch(p, o);
            QCOMPARE(p.value(QStringLiteral("versionFileUri")).numberText(), QStringLiteral("7")); // non-string kept
        }
        // Firmware lowering only when higher than the SDK's firmware.
        {
            json::Value p = parsed("{\"sdkVersion\":\"0x0114000000080001\",\"requiredSystemSoftwareVersion\":\"0x0200000000000000\"}");
            applyParamPatch(p, o);
            QCOMPARE(p.value(QStringLiteral("requiredSystemSoftwareVersion")).toString(), QStringLiteral("0x0114000000000000"));
            json::Value q = parsed("{\"sdkVersion\":\"0x0114000000080001\",\"requiredSystemSoftwareVersion\":\"0x0100000000000000\"}");
            applyParamPatch(q, o);
            QCOMPARE(q.value(QStringLiteral("requiredSystemSoftwareVersion")).toString(), QStringLiteral("0x0100000000000000"));
            json::Value z = parsed("{\"sdkVersion\":\"0x0000000000080001\",\"requiredSystemSoftwareVersion\":\"0x0200000000000000\"}");
            applyParamPatch(z, o);
            QCOMPARE(z.value(QStringLiteral("requiredSystemSoftwareVersion")).toString(), QStringLiteral("0x0200000000000000"));
        }
        // Version and title only when they differ; title goes to the default language.
        {
            json::Value p = parsed("{\"contentVersion\":\"01.000.000\",\"localizedParameters\":{\"defaultLanguage\":\"fr-FR\",\"fr-FR\":{\"titleName\":\"A\"}}}");
            ParamPatchOptions t;
            t.contentVersion = QStringLiteral("01.000.000");
            t.title = QStringLiteral("A");
            const QStringList none = applyParamPatch(p, t);
            QCOMPARE(none.size(), 1); // DRM only
            t.contentVersion = QStringLiteral("01.001.000");
            t.title = QStringLiteral("B");
            applyParamPatch(p, t);
            QCOMPARE(p.value(QStringLiteral("contentVersion")).toString(), QStringLiteral("01.001.000"));
            QCOMPARE(p.path(QStringLiteral("localizedParameters.fr-FR.titleName")).toString(), QStringLiteral("B"));
            json::Value bare = parsed("{}");
            applyParamPatch(bare, t);
            QCOMPARE(bare.path(QStringLiteral("localizedParameters.en-US.titleName")).toString(), QStringLiteral("B"));
        }
        // Serialisation: Python indent=2 + "\n", CRLF, raw UTF-8.
        QCOMPARE(serializeParamJson(parsed("{\"a\":[1,{\"b\":\"\\u00e9\"}],\"c\":{}}")),
                 QByteArray("{\r\n  \"a\": [\r\n    1,\r\n    {\r\n      \"b\": \"\xC3\xA9\"\r\n    }\r\n  ],\r\n  \"c\": {}\r\n}\r\n"));
    }

    void sizeClassThresholds()
    {
        QCOMPARE(chooseSizeClass(kSizeClass0Max, 0).attributePub, 0);
        QCOMPARE(chooseSizeClass(kSizeClass0Max + 1, 0).attributePub, 1);
        QCOMPARE(chooseSizeClass(kSizeClass1Max, 0).attributePub, 1);
        QCOMPARE(chooseSizeClass(kSizeClass1Max + 1, 0).attributePub, 2);
        QCOMPARE(chooseSizeClass(kSizeClass2Max + 1, 0).attributePub, 2);
        // Mount level transitions.
        QCOMPARE(chooseSizeClass(kMountLevel2Max, 2).mountLevel, 2);
        QCOMPARE(chooseSizeClass(kMountLevel2Max, 2).attributePub, 0);
        QCOMPARE(chooseSizeClass(kMountLevel2Max + 1, 2).mountLevel, 1);
        QCOMPARE(chooseSizeClass(kSizeClass1Max, 2).mountLevel, 1);
        QCOMPARE(chooseSizeClass(kSizeClass1Max, 1).attributePub, 1);
        QCOMPARE(chooseSizeClass(kSizeClass1Max + 1, 1).mountLevel, 0);
        QCOMPARE(chooseSizeClass(kSizeClass1Max + 1, 2).mountLevel, 0);
        QCOMPARE(chooseSizeClass(kSizeClass0Max + 1, 1).mountLevel, 1);
        // Estimate formula.
        QCOMPARE(estimatePackageBytes(0, 0), 512LL * 1024 * 1024);
        QCOMPARE(estimatePackageBytes(100LL * 1000 * 1000 * 1000, 10), 101LL * 1000 * 1000 * 1000 + 40960);
        QCOMPARE(estimatePackageBytes(101, 0), 101 + 512LL * 1024 * 1024);
    }

    void sizeClassParamRules()
    {
        {
            json::Value p = parsed("{}");
            QCOMPARE(applyPackageSizeClass(p, 1, 1).error().code, ErrorCode::CorruptData); // category missing
        }
        {
            json::Value p = parsed("{\"applicationCategoryType\":1}");
            auto d = applyPackageSizeClass(p, 1, 1);
            QVERIFY(d.ok());
            QVERIFY(!d->applied);
            QVERIFY(!d->warnings.isEmpty());
            QVERIFY(!p.contains(QStringLiteral("attributePub")));
        }
        for (const char* bad : {"{\"applicationCategoryType\":0,\"kernel\":{\"addcontMountLevel\":1.0}}",
                                "{\"applicationCategoryType\":0,\"kernel\":{\"addcontMountLevel\":true}}",
                                "{\"applicationCategoryType\":0,\"kernel\":{\"addcontMountLevel\":\"1\"}}",
                                "{\"applicationCategoryType\":0,\"kernel\":{\"addcontMountLevel\":3}}",
                                "{\"applicationCategoryType\":0,\"kernel\":[]}",
                                "{\"applicationCategoryType\":0.5}"}) {
            json::Value p = parsed(bad);
            QVERIFY2(!applyPackageSizeClass(p, 1, 1).ok(), bad);
        }
        {
            json::Value p = parsed("{\"applicationCategoryType\":0,\"kernel\":{\"addcontMountLevel\":2,\"appSizeInGib\":3}}");
            auto d = applyPackageSizeClass(p, 170LL * 1000 * 1000 * 1000, 100);
            QVERIFY(d.ok());
            QCOMPARE(d->attributePub, 1);
            QCOMPARE(p.value(QStringLiteral("attributePub")).toInt(), qint64(1));
            QCOMPARE(p.path(QStringLiteral("kernel.addcontMountLevel")).toInt(), qint64(1));
            QVERIFY(!p.value(QStringLiteral("kernel")).contains(QStringLiteral("appSizeInGib")));
            QVERIFY(!d->warnings.isEmpty());
        }
        {
            json::Value p = parsed("{\"applicationCategoryType\":0}");
            QCOMPARE(applyPackageSizeClass(p, 200LL * 1000 * 1000 * 1000, kLv2MaxFiles + 1).error().code, ErrorCode::Unsupported);
            json::Value q = parsed("{\"applicationCategoryType\":0}");
            auto big = applyPackageSizeClass(q, 300LL * 1000 * 1000 * 1000, 10);
            QVERIFY(big.ok()); // above the largest class: a warning, img_create decides
            QCOMPARE(big->attributePub, 2);
            QVERIFY(!big->warnings.isEmpty());
        }
    }

    // ---- dump rules and scanner -----------------------------------------------------------
    void exclusionRules()
    {
        const QStringList root;
        const QStringList sceSys{QStringLiteral("sce_sys")};
        QVERIFY(!baseExclusionReason(sceSys, QStringLiteral("keystone"), false).size());
        QVERIFY(baseExclusionReason(sceSys, QStringLiteral("PARAM.SFO"), false).size());
        QVERIFY(baseExclusionReason(sceSys, QStringLiteral("playgo-chunk.dat"), false).size());
        QVERIFY(baseExclusionReason(sceSys, QStringLiteral("about"), true).size());
        QVERIFY(baseExclusionReason({QStringLiteral("sce_sys"), QStringLiteral("trophy2")}, QStringLiteral("x.dds"), false).size());
        QVERIFY(baseExclusionReason({QStringLiteral("SCE_SYS"), QStringLiteral("a"), QStringLiteral("b")}, QStringLiteral("x.auth_info"), false).size());
        QVERIFY(!baseExclusionReason({QStringLiteral("Data")}, QStringLiteral("x.dds"), false).size());
        QVERIFY(baseExclusionReason({QStringLiteral("Data")}, QStringLiteral("a.GP5"), false).size());
        QVERIFY(baseExclusionReason({QStringLiteral("Data")}, QStringLiteral("x.playgo-scenario.json"), false).size());
        QVERIFY(baseExclusionReason(root, QStringLiteral("sce_suppl"), true).size());
        QVERIFY(baseExclusionReason(root, QStringLiteral(".gp5-assets"), true).size());
        QVERIFY(!baseExclusionReason({QStringLiteral("Data")}, QStringLiteral("sce_sc"), true).size());
        QVERIFY(baseExclusionReason(root, QStringLiteral("ampr_emu.index"), false).size());
        QVERIFY(baseExclusionReason(root, QStringLiteral("game.ffpkg"), false).size());
        QVERIFY(!baseExclusionReason({QStringLiteral("Data")}, QStringLiteral("game.pkg"), false).size());
        QVERIFY(baseExclusionReason({QStringLiteral("fakelib")}, QStringLiteral("libScePlayGo.sprx"), false).size());
        QVERIFY(baseExclusionReason({QStringLiteral("Data")}, QStringLiteral("Thumbs.db"), false).size());
        QVERIFY(baseExclusionReason({QStringLiteral("Data")}, QStringLiteral("__MACOSX"), true).size());
        QVERIFY(isJunkFileName(QStringLiteral("._x")));
        QVERIFY(isJunkFileName(QStringLiteral("ehthumbs_vista.db")));
        QVERIFY(isJunkFileName(QStringLiteral("link.URL")));
        QVERIFY(!isJunkFileName(QStringLiteral("thumbs.dbx")));

        LeftoverOptions on;
        LeftoverOptions off{true, false, false};
        LeftoverContext none;
        QVERIFY(leftoverReason(root, QStringLiteral("_DUBLEX_"), true, on, none).size());
        QVERIFY(!leftoverReason(root, QStringLiteral("_DUBLEX_"), true, off, none).size());
        LeftoverContext onlySaved;
        onlySaved.childNames = {QStringLiteral("Saved")};
        QVERIFY(leftoverReason(root, QStringLiteral("Engine"), true, on, onlySaved).size());
        LeftoverContext more;
        more.childNames = {QStringLiteral("Saved"), QStringLiteral("Binaries")};
        QVERIFY(!leftoverReason(root, QStringLiteral("Engine"), true, on, more).size());
        QVERIFY(leftoverReason({QStringLiteral("Engine")}, QStringLiteral("Saved"), true, on, none).size());
        LeftoverContext paks;
        paks.rootFolderHasContentPaks = true;
        QVERIFY(leftoverReason({QStringLiteral("MyGame")}, QStringLiteral("Saved"), true, on, paks).size());
        QVERIFY(!leftoverReason({QStringLiteral("MyGame")}, QStringLiteral("Saved"), true, on, none).size());
        QVERIFY(leftoverReason({QStringLiteral("fakelib")}, QStringLiteral("ampr_emu.index"), false, on, none).size());
        QVERIFY(!leftoverReason({QStringLiteral("fakelib")}, QStringLiteral("ampr_emu.index"), false, off, none).size());
        LeftoverOptions noDlc{false, true, true};
        QVERIFY(leftoverReason(root, QStringLiteral("dlc_emu.ini"), false, noDlc, none).size());
        QVERIFY(leftoverReason({QStringLiteral("fakelib2")}, QStringLiteral("libSceAppContent.prx"), false, noDlc, none).size());
        QVERIFY(!leftoverReason(root, QStringLiteral("dlc_emu.ini"), false, on, none).size());

        QVERIFY(pathRuleProblem(QStringLiteral("Data/ok_name-1.pak")).isEmpty());
        QVERIFY(pathRuleProblem(QStringLiteral("Data/space ")).isEmpty()); // only a trailing dot is refused
        QVERIFY(!pathRuleProblem(QStringLiteral("Data/trail.")).isEmpty());
        QVERIFY(!pathRuleProblem(QStringLiteral("Data/50%.pak")).isEmpty());
        QVERIFY(!pathRuleProblem(QStringLiteral("Data/a;b")).isEmpty());
        QVERIFY(!pathRuleProblem(QStringLiteral("D\u00e4ta/x.pak")).isEmpty());
        QVERIFY(!pathRuleProblem(QStringLiteral("Data//x")).isEmpty());
        QVERIFY(!pathRuleProblem(QStringLiteral("Data/\tx")).isEmpty());
    }

    void scannerOrderAndEmptyFolders()
    {
        const QString d = dir(QStringLiteral("scan"));
        for (const char* n : {"b.bin", "_x.bin", "A.bin", "1.bin", "Zeta/z.bin", "alpha/a.bin"})
            put(d, QString::fromLatin1(n), "x");
        put(d, QStringLiteral("sce_sys/keystone"), QByteArray(96, 'K'));
        QDir().mkpath(QDir(d).filePath(QStringLiteral("sce_sys/trophy2")));       // never an empty dir
        QDir().mkpath(QDir(d).filePath(QStringLiteral("Empty/Inner")));
        put(d, QStringLiteral("JunkOnly/Thumbs.db"), "x");
        put(d, QStringLiteral("Emptied/x.gp4"), "x");
        put(d, QStringLiteral("Bad/50%.bin"), "x");
        auto r = scanSource({d}, {}, CancellationToken());
        QVERIFY(r.ok());
        QStringList rels;
        for (const auto& f : r->files)
            rels << f.rel;
        QCOMPARE(rels, (QStringList{QStringLiteral("1.bin"), QStringLiteral("_x.bin"), QStringLiteral("A.bin"),
                                    QStringLiteral("alpha/a.bin"), QStringLiteral("b.bin"), QStringLiteral("Bad/50%.bin"),
                                    QStringLiteral("sce_sys/keystone"), QStringLiteral("Zeta/z.bin")}));
        QCOMPARE(r->emptyDirs, (QStringList{QStringLiteral("Empty/Inner"), QStringLiteral("JunkOnly")}));
        QCOMPARE(int(r->badPaths.size()), 1);
        QCOMPARE(r->badPaths.front().rel, QStringLiteral("Bad/50%.bin"));
        QCOMPARE(r->keystoneSize, qint64(96));
        QVERIFY(r->hasFileUnder(QStringLiteral("zeta")));
        QVERIFY(!r->hasFileUnder(QStringLiteral("Empty")));
        bool emptiedListed = false;
        for (const auto& e : r->excluded)
            if (e.rel == u"Emptied/x.gp4")
                emptiedListed = !e.reason.isEmpty();
        QVERIFY(emptiedListed);
    }

    void scannerOverlayKeepsBaseSpelling()
    {
        const QString upd = dir(QStringLiteral("ovl/upd"));
        const QString base = dir(QStringLiteral("ovl/base"));
        put(upd, QStringLiteral("data/new.pak"), "new");
        put(upd, QStringLiteral("data/SHARED.PAK"), "updated");
        put(base, QStringLiteral("Data/Shared.pak"), "old");
        put(base, QStringLiteral("Data/base.pak"), "base");
        put(base, QStringLiteral("playgo-chunks/001.bin"), "payload of an earlier build");
        auto r = scanSource({upd, base}, {}, CancellationToken());
        QVERIFY(r.ok());
        QCOMPARE(int(r->files.size()), 3);
        QCOMPARE(r->files[0].rel, QStringLiteral("Data/base.pak"));
        QCOMPARE(r->files[0].root, 1);
        QCOMPARE(r->files[1].rel, QStringLiteral("Data/new.pak"));
        QCOMPARE(r->files[2].rel, QStringLiteral("Data/Shared.pak")); // base spelling ...
        QCOMPARE(r->files[2].root, 0);                                // ... update data
        QCOMPARE(readAll(r->files[2].absPath), QByteArray("updated"));
        QCOMPARE(r->filesFromRoot[0], 2);
    }

    void scannerRejectsLinksAndCancels()
    {
        const QString d = dir(QStringLiteral("links"));
        put(d, QStringLiteral("real/a.bin"), "x");
        QVERIFY(createDirectoryJunction(QDir(d).filePath(QStringLiteral("link")), QDir(d).filePath(QStringLiteral("real"))).ok());
        auto r = scanSource({d}, {}, CancellationToken());
        QVERIFY(!r.ok());
        QCOMPARE(r.error().code, ErrorCode::Unsupported);
        QVERIFY(r.error().message.contains(QStringLiteral("link")));

        const QString many = dir(QStringLiteral("many"));
        for (int i = 0; i < 2100; ++i)
            put(many, QStringLiteral("f%1.bin").arg(i), "");
        CancellationSource src;
        src.cancel();
        auto c = scanSource({many}, {}, src.token());
        QVERIFY(!c.ok());
        QVERIFY(c.error().isCancelled());
        ScanOptions o;
        o.checkpointEvery = 5000;
        QVERIFY(scanSource({many}, o, src.token()).ok()); // no checkpoint reached
    }

    void dlcEmuIni()
    {
        const DlcEmuIni ini = parseDlcEmuIni(
            "; comment\r\n[PSAC]\r\ncontent_id=UP0000-PPSA01234_00-DLC0000000000001\r\nmount_point=/app0/addcont0\r\n"
            "entitlement_key=00112233445566778899aabbccddeeff\r\n[PSAL]\r\ncontent_id=UP0000-PPSA01234_00-DLC0000000000002\r\n"
            "[PSAC]\r\nlabel=nocid\r\n[PSAC]\r\ncontent_id=UP0000-PPSA01234_00-DLC0000000000001\r\n"
            "[psac]\r\ncontent_id=UP0000-PPSA01234_00-DLC0000000000003\r\nmount_point=/data/x\r\n");
        QCOMPARE(int(ini.entries.size()), 2);
        QCOMPARE(int(ini.allEntries.size()), 4);
        QCOMPARE(ini.entries[0].mountFolder(), QStringLiteral("addcont0"));
        QCOMPARE(ini.entries[0].entitlementKey, QStringLiteral("00112233445566778899aabbccddeeff"));
        QCOMPARE(ini.entries[1].mountFolder(), QString()); // not under /app0
        QCOMPARE(ini.warnings.size(), 1);                  // the duplicate
        QVERIFY(!parseDlcEmuIni(QByteArray(70 * 1024, 'x')).warnings.isEmpty());
        QVERIFY(isAddcontFolderName(QStringLiteral("AddCont12")));
        QVERIFY(!isAddcontFolderName(QStringLiteral("addcont")));
        QVERIFY(bytesImportAmpr(QByteArray("....libSceAmpr.sprx....")));
    }

    // ---- PlayGo -------------------------------------------------------------------------------
    void scenarioFileRules()
    {
        const auto ok = checkScenarioFile(parsed(golden(QStringLiteral("source_scenario.json")).constData()));
        QVERIFY2(ok.valid, qPrintable(ok.problem));
        QCOMPARE(ok.supportedLanguages, (QStringList{QStringLiteral("en-US"), QStringLiteral("fr-FR")}));
        QCOMPARE(int(ok.scenarios.size()), 2);
        QCOMPARE(ok.scenarios[1].title, QStringLiteral("Multiplayer"));

        // Derived language list (not written back): chunk default + scenario keys, canonicalised.
        const auto derived = checkScenarioFile(parsed(
            "{\"scenarioCount\":1,\"scenarioDefaultId\":0,\"scenarioDefaultLanguage\":\"EN-us\",\"scenarios\":[{\"id\":0,"
            "\"type\":\"playmode\",\"en-US\":{\"title\":\"T\"},\"ja-jp\":{},\"xx\":{}}]}"));
        QVERIFY2(derived.valid, qPrintable(derived.problem));
        QCOMPARE(derived.supportedLanguages, (QStringList{QStringLiteral("en-US"), QStringLiteral("ja-JP")}));
        QCOMPARE(derived.chunkDefaultLanguage, QStringLiteral("en-US"));

        const char* bad[] = {
            "[]",
            "{\"scenarioCount\":0,\"scenarioDefaultId\":0,\"scenarioDefaultLanguage\":\"en-US\",\"scenarios\":[]}",
            "{\"scenarioCount\":6,\"scenarioDefaultId\":0,\"scenarioDefaultLanguage\":\"en-US\",\"scenarios\":[]}",
            "{\"scenarioCount\":1,\"scenarioDefaultId\":1,\"scenarioDefaultLanguage\":\"en-US\",\"scenarios\":[{\"id\":0,\"type\":\"p\",\"en-US\":{}}]}",
            "{\"scenarioCount\":1,\"scenarioDefaultId\":0,\"scenarioDefaultLanguage\":\"xx-XX\",\"scenarios\":[{\"id\":0,\"type\":\"p\",\"en-US\":{}}]}",
            "{\"scenarioCount\":2,\"scenarioDefaultId\":0,\"scenarioDefaultLanguage\":\"en-US\",\"scenarios\":[{\"id\":0,\"type\":\"p\",\"en-US\":{}}]}",
            "{\"scenarioCount\":2,\"scenarioDefaultId\":0,\"scenarioDefaultLanguage\":\"en-US\",\"scenarios\":[{\"id\":0,\"type\":\"p\",\"en-US\":{}},{\"id\":0,\"type\":\"p\",\"en-US\":{}}]}",
            "{\"scenarioCount\":1,\"scenarioDefaultId\":0,\"scenarioDefaultLanguage\":\"en-US\",\"scenarios\":[{\"id\":0,\"type\":\"\",\"en-US\":{}}]}",
            "{\"scenarioCount\":1,\"scenarioDefaultId\":0,\"scenarioDefaultLanguage\":\"en-US\",\"scenarios\":[{\"id\":0,\"type\":\"p\",\"fr-FR\":{}}]}",
            "{\"scenarioCount\":1,\"scenarioDefaultId\":0,\"scenarioDefaultLanguage\":\"en-US\",\"chunkDefaultLanguage\":\"de-DE\",\"chunkSupportedLanguages\":[\"en-US\"],\"scenarios\":[{\"id\":0,\"type\":\"p\",\"en-US\":{}}]}",
            "{\"scenarioCount\":1,\"scenarioDefaultId\":0,\"scenarioDefaultLanguage\":\"en-US\",\"chunkSupportedLanguages\":[],\"scenarios\":[{\"id\":0,\"type\":\"p\",\"en-US\":{}}]}",
            "{\"scenarioCount\":1,\"scenarioDefaultId\":0,\"scenarioDefaultLanguage\":\"en-US\",\"chunkSupportedLanguages\":[\"en-US\",\"en-US\"],\"scenarios\":[{\"id\":0,\"type\":\"p\",\"en-US\":{}}]}",
            "{\"scenarioCount\":1,\"scenarioDefaultId\":0,\"scenarioDefaultLanguage\":\"en-US\",\"chunkSupportedLanguages\":[\"en-US\",\"zz\"],\"scenarios\":[{\"id\":0,\"type\":\"p\",\"en-US\":{}}]}",
            "{\"scenarioCount\":true,\"scenarioDefaultId\":0,\"scenarioDefaultLanguage\":\"en-US\",\"scenarios\":[{\"id\":0,\"type\":\"p\",\"en-US\":{}}]}",
        };
        for (const char* b : bad) {
            const auto r = checkScenarioFile(parsed(b));
            QVERIFY2(!r.valid, b);
            QVERIFY2(!r.problem.isEmpty(), b);
        }
        QCOMPARE(scenarioFileBytes(defaultScenarioObject(QStringLiteral("en-US"))), golden(QStringLiteral("scenario_default_en-US.json")));
    }

    void payloadsAndFillers()
    {
        PlayGoInputs in;
        in.sourceScenario = golden(QStringLiteral("source_scenario.json"));
        in.files = {{QStringLiteral("eboot.bin"), 10}};
        for (const int n : {1, 2, 100}) {
            in.chunkCount = n;
            auto p = planPlayGo(in);
            QVERIFY(p.ok());
            QCOMPARE(p->layout, PlayGoLayout::Script);
            int languages = 0, fillers = 0;
            for (const auto& pl : p->payloads) {
                if (pl.filler)
                    ++fillers;
                else {
                    ++languages;
                    QCOMPARE(pl.chunk, std::min(languages - 1, n - 1));
                }
            }
            QCOMPARE(languages, 2);
            QCOMPARE(fillers, std::max(0, n - 2));
            QCOMPARE(p->chunkInfo.scenarios.front().text, n == 1 ? QStringLiteral("0") : QStringLiteral("0-%1").arg(n - 1));
            QCOMPARE(p->fileChunk(QStringLiteral("eboot.bin")), std::optional<int>(0));
        }
        // More languages than chunks: the last chunk holds the rest.
        in.sourceScenario = QByteArray(
            "{\"chunkSupportedLanguages\":[\"en-US\",\"fr-FR\",\"de-DE\"],\"scenarioCount\":1,\"scenarioDefaultId\":0,"
            "\"scenarioDefaultLanguage\":\"en-US\",\"scenarios\":[{\"id\":0,\"type\":\"playmode\",\"en-US\":{\"title\":\"x\"}}]}");
        in.chunkCount = 2;
        auto p = planPlayGo(in);
        QVERIFY(p.ok());
        QCOMPARE(p->chunkInfo.chunks[1].attrs.back().second, QStringLiteral("fr-FR de-DE"));
        QCOMPARE(p->payloads[2].dst, QStringLiteral("playgo-languages/03-de-DE.bin"));
        // Name collisions with game files; fillers off.
        in.chunkCount = 4;
        in.files.push_back({QStringLiteral("PLAYGO-CHUNKS/003.bin"), 5});
        p = planPlayGo(in);
        QVERIFY(p.ok());
        QCOMPARE(p->payloads.back().dst, QStringLiteral("playgo-chunks/003-2.bin"));
        in.fillEmptyChunks = false;
        p = planPlayGo(in);
        QCOMPARE(int(p->payloads.size()), 3);
        // Invalid scenario file: warning and a default scenario in the param.json language.
        in.sourceScenario = QByteArray("{\"scenarioCount\":9}");
        in.paramDefaultLanguage = QStringLiteral("ja-jp");
        p = planPlayGo(in);
        QVERIFY(!p->warnings.isEmpty());
        QCOMPARE(p->scenarioBytes, scenarioFileBytes(defaultScenarioObject(QStringLiteral("ja-JP"))));
        // Fallback off: minimal layout.
        in.fallback = false;
        p = planPlayGo(in);
        QCOMPARE(p->layout, PlayGoLayout::Minimal);
        QVERIFY(p->payloads.empty());
        QCOMPARE(p->fileChunk(QStringLiteral("eboot.bin")), std::optional<int>());
        in.chunkCount = 0;
        in.fallback = true;
        QVERIFY(!planPlayGo(in).ok());
    }

    void sourceTableParsing()
    {
        const quint64 header = langBit(QStringLiteral("en-US")) | langBit(QStringLiteral("fr-FR"));
        const QByteArray good = buildPlgx({{header, "A"}, {header, "B"}}, {{1, "S", {1}}}, 0, QStringLiteral("en-US"), header);
        auto t = parsePlayGoChunkDat(good);
        QVERIFY2(t.ok(), t.ok() ? "" : qPrintable(t.error().message));
        QCOMPARE(t->chunkCount, 2);
        QCOMPARE(t->scenarios.front().chunks, (std::vector<int>{1}));
        QCOMPARE(t->chunks[1].label, QStringLiteral("B"));
        QCOMPARE(languagesInMask(header), (QStringList{QStringLiteral("en-US"), QStringLiteral("fr-FR")}));

        QByteArray badMagic = good;
        badMagic[0] = 'x';
        QVERIFY(!parsePlayGoChunkDat(badMagic).ok());
        QByteArray badLen = good;
        putLe32(badLen, 0x10, quint32(good.size() + 1));
        QVERIFY(!parsePlayGoChunkDat(badLen).ok());
        QByteArray badVersion = good;
        putLe16(badVersion, 4, 0x0100);
        QVERIFY(!parsePlayGoChunkDat(badVersion).ok());
        QVERIFY(!parsePlayGoChunkDat(buildPlgx({}, {{1, "S", {}}}, 0, QStringLiteral("en-US"), header)).ok());
        std::vector<PlgxScenario> tooMany(65, PlgxScenario{1, "S", {0}});
        QVERIFY(!parsePlayGoChunkDat(buildPlgx({{header, "A"}}, tooMany, 0, QStringLiteral("en-US"), header)).ok());
        QVERIFY(!parsePlayGoChunkDat(buildPlgx({{header, "A"}}, {{1, "S", {0}}}, 1, QStringLiteral("en-US"), header)).ok());
        QVERIFY(!parsePlayGoChunkDat(buildPlgx({{header, "A"}}, {{1, "S", {0}}}, 0, QStringLiteral("de-DE"), header)).ok());
        QVERIFY(!parsePlayGoChunkDat(buildPlgx({{header, "A"}}, {{1, "S", {3}}}, 0, QStringLiteral("en-US"), header)).ok());
        QVERIFY(parsePlayGoChunkDat(buildPlgx({{0, "A"}}, {{1, "S", {0}}}, 0, QStringLiteral("en-US"), 0)).ok()); // no mask
        QVERIFY(parsePlayGoChunkDat(buildPlgx({{0, "A"}}, {{1, "S", {0}}}, 0, QStringLiteral("en-US"), 0))->trivial());

        // File map: FICM entry i <-> hash i.
        const QByteArray ficm = buildFicm({1, 0, 7});
        const QByteArray flt = buildFlt({ps5FlatPathHash(QStringLiteral("/Data/A.pak")), ps5FlatPathHash(QStringLiteral("/eboot.bin")),
                                         ps5FlatPathHash(QStringLiteral("/x.bin"))});
        const ChunkMap m = parseChunkMap(ficm, flt, 2);
        QCOMPARE(m.chunkOf(QStringLiteral("Data/A.pak")), 1);
        QCOMPARE(m.chunkOf(QStringLiteral("data/a.PAK")), 1); // the hash is over the upper-cased path
        QCOMPARE(m.chunkOf(QStringLiteral("x.bin")), 0);      // chunk 7 does not exist
        QVERIFY(!m.problem.isEmpty());
        const ChunkMap swapped = parseChunkMap(flt, ficm, 2);
        QVERIFY(swapped.misnamed);
        QCOMPARE(swapped.chunkOf(QStringLiteral("Data/A.pak")), 0);
    }

    void flatPathHashMatchesSecondPort()
    {
        const QList<QByteArray> lines = golden(QStringLiteral("flat_path_hash.txt")).split('\n');
        int checked = 0;
        for (const QByteArray& line : lines) {
            if (line.isEmpty())
                continue;
            const QList<QByteArray> parts = line.split('\t');
            QCOMPARE(parts.size(), 2);
            bool ok = false;
            const quint64 expected = parts[1].toULongLong(&ok, 16);
            QVERIFY(ok);
            QCOMPARE(ps5FlatPathHash(QString::fromLatin1(parts[0])), expected);
            ++checked;
        }
        QCOMPARE(checked, 7);
    }

    // ---- GP5 bytes ----------------------------------------------------------------------------------
    void gp5EscapingAndLayout()
    {
        Gp5Project p;
        p.passcode = kDefaultPasscode;
        p.contentId = kCid;
        p.files.push_back({QStringLiteral("a&b<c>\"d\"\r\n\t.bin"), QStringLiteral("C:\\x\\a&b.bin"), 0, true});
        const QByteArray text = writeGp5(p);
        QVERIFY(text.startsWith("<?xml version='1.0' encoding='utf-8'?>\r\n<psproject fmt=\"gp5\">\r\n  <volume>\r\n"));
        QVERIFY(text.contains("dst_path=\"a&amp;b&lt;c&gt;&quot;d&quot;&#13;&#10;&#09;.bin\" src_path=\"C:\\x\\a&amp;b.bin\" chunk=\"0\" pfs_compression=\"disable\" />"));
        QVERIFY(text.endsWith("</psproject>"));
        QVERIFY(!text.startsWith("\xEF\xBB\xBF"));
        QVERIFY(!QByteArray(text).replace("\r\n", "").contains('\n'));
        // Add-on volume: entitlement key, no chunk_info.
        p.volumeType = QStringLiteral("prospero_ac");
        p.entitlementKey = QStringLiteral("00112233445566778899aabbccddeeff");
        const QByteArray ac = writeGp5(p);
        QVERIFY(ac.contains("<package passcode=\"00000000000000000000000000000000\" content_id=\"" + kCid.toLatin1()
                            + "\" entitlement_key=\"00112233445566778899aabbccddeeff\" />"));
        QVERIFY(!ac.contains("chunk_info"));
        // Element text escaping and an empty <files />.
        XmlElement e{QStringLiteral("r"), {}, {}, {{QStringLiteral("t"), {}, QStringLiteral("a<&>\"b"), {}}, {QStringLiteral("files"), {}, {}, {}}}};
        QCOMPARE(writeEtreeDocument(e), QByteArray("<?xml version='1.0' encoding='utf-8'?>\n<r>\n  <t>a&lt;&amp;&gt;\"b</t>\n  <files />\n</r>"));
    }

    void goldenLayoutB()
    {
        const QString dump = dir(QStringLiteral("golden-b/dump"));
        const QString out = dir(QStringLiteral("golden-b/out"));
        makeGoldenDump(dump);
        BuildPlan plan;
        const QByteArray gp5 = goldenProject(dump, out, {{keys::kPlayGoChunkCount, 3}}, &plan);
        QCOMPARE(gp5, golden(QStringLiteral("gp5_layout_b.gp5")));
        QCOMPARE(plan.playgo.scenarioBytes, golden(QStringLiteral("scenario_layout_b.json")));
        QCOMPARE(plan.paramBytes, golden(QStringLiteral("param_normalised.json")));
        QCOMPARE(plan.stem, kStem);
        QCOMPARE(plan.size.attributePub, 0);
        QVERIFY(plan.size.applied);
        bool sfoExcluded = false;
        for (const auto& e : plan.scan.excluded)
            if (e.rel == u"sce_sys/param.sfo")
                sfoExcluded = true;
        QVERIFY(sfoExcluded);
    }

    void goldenLayoutA()
    {
        const QString dump = dir(QStringLiteral("golden-a/dump"));
        const QString out = dir(QStringLiteral("golden-a/out"));
        makeGoldenDump(dump);
        BuildPlan plan;
        const QByteArray gp5 = goldenProject(dump, out, {{keys::kPlayGoFallback, false}}, &plan);
        QCOMPARE(gp5, golden(QStringLiteral("gp5_layout_a.gp5")));
        QCOMPARE(plan.playgo.scenarioBytes, golden(QStringLiteral("scenario_default_en-US.json")));
    }

    void goldenLayoutC()
    {
        const QString dump = dir(QStringLiteral("golden-c/dump"));
        const QString out = dir(QStringLiteral("golden-c/out"));
        makeGoldenDump(dump);
        const quint64 header = langBit(QStringLiteral("en-US")) | langBit(QStringLiteral("fr-FR")) | langBit(QStringLiteral("de-DE"));
        put(dump, QStringLiteral("sce_sys/playgo-chunk.dat"),
            buildPlgx({{header, "Base"}, {langBit(QStringLiteral("fr-FR")), "French voice"}, {header, "Extra"}},
                      {{1, "Main", {0}}, {1, "Second", {2, 0}}}, 0, QStringLiteral("en-US"), header));
        put(dump, QStringLiteral("sce_sys/playgo-ficm.dat"), buildFicm({1, 2, 0}));
        put(dump, QStringLiteral("sce_sys/playgo-hash-table.dat"),
            buildFlt({ps5FlatPathHash(QStringLiteral("/Data/A.pak")), ps5FlatPathHash(QStringLiteral("/Data/_x.bin")),
                      ps5FlatPathHash(QStringLiteral("/eboot.bin"))}));
        BuildPlan plan;
        const QByteArray gp5 = goldenProject(dump, out, {{keys::kUseSourcePlayGoTable, true}}, &plan);
        QCOMPARE(gp5, golden(QStringLiteral("gp5_layout_c.gp5")));
        QCOMPARE(plan.playgo.layout, PlayGoLayout::SourceTable);
        QCOMPARE(plan.playgo.scenarioBytes, golden(QStringLiteral("scenario_layout_c.json")));

        // The same table is ignored while the setting is off (layout B).
        BuildPlan off;
        goldenProject(dump, out, {{keys::kPlayGoChunkCount, 3}}, &off);
        QCOMPARE(off.playgo.layout, PlayGoLayout::Script);
    }

    void planRulesAndErrors()
    {
        Env env;
        const QString out = dir(QStringLiteral("plan/out"));
        const auto plan = [&](const QString& d, const QVariantMap& s = {}, const PlanOptions& base = {}) {
            PlanOptions po = base;
            po.roots = {d};
            po.outputDir = out;
            return planBuild(po, FpkgSettings::from(env.values(s)), CancellationToken());
        };
        {
            const QString d = dir(QStringLiteral("plan/nokeystone"));
            makeSimpleDump(d);
            QFile::remove(QDir(d).filePath(QStringLiteral("sce_sys/keystone")));
            auto r = plan(d);
            QCOMPARE(r.error().code, ErrorCode::CorruptData);
            QVERIFY(r.error().message.contains(QStringLiteral("keystone")));
            put(d, QStringLiteral("sce_sys/keystone"), QByteArray(95, 'K'));
            QVERIFY(plan(d).error().message.contains(QStringLiteral("95 bytes")));
        }
        {
            const QString d = dir(QStringLiteral("plan/badnames"));
            makeSimpleDump(d);
            put(d, QStringLiteral("Data/50%.pak"), "x");
            put(d, QStringLiteral("Data/a;b"), "x");
            auto r = plan(d);
            QCOMPARE(r.error().code, ErrorCode::Unsupported);
            QVERIFY(r.error().message.startsWith(QStringLiteral("2 file name(s)")));
        }
        {
            const QString d = dir(QStringLiteral("plan/badid"));
            makeSimpleDump(d, QStringLiteral("XX"));
            QCOMPARE(plan(d).error().code, ErrorCode::CorruptData);
            const QString e = dir(QStringLiteral("plan/otherid"));
            makeSimpleDump(e);
            PlanOptions po;
            po.contentId = QStringLiteral("UP0000-PPSA01234_00-ABCDEFGHIJKLMNOP");
            QCOMPARE(plan(e, {}, po).error().code, ErrorCode::InvalidArgument);
            po = {};
            po.contentVersion = QStringLiteral("01.002.000");
            po.title = QStringLiteral("Renamed");
            auto ok = plan(e, {}, po);
            QVERIFY(ok.ok());
            QCOMPARE(ok->stem, kCid + QStringLiteral("-A0102-V0102"));
            QVERIFY(ok->paramBytes.contains("\"titleName\": \"Renamed\""));
            QVERIFY(ok->paramBytes.contains("\"contentVersion\": \"01.002.000\""));
        }
        {
            // DLC emulator off: dlc_emu.ini left out, but an empty addcont0 is still an empty folder.
            const QString d = dir(QStringLiteral("plan/dlcoff"));
            makeGoldenDump(d);
            put(d, QStringLiteral("fakelib/libSceAppContent.sprx"), "x");
            auto r = plan(d, {{keys::kKeepDlcEmulator, false}, {keys::kKeepEmptyFolders, false}});
            QVERIFY(r.ok());
            QVERIFY(!r->scan.findFile(QStringLiteral("dlc_emu.ini")));
            QVERIFY(!r->scan.findFile(QStringLiteral("fakelib/libSceAppContent.sprx")));
            QVERIFY(r->emptyDirs.isEmpty());
            auto on = plan(d, {{keys::kKeepEmptyFolders, false}});
            QCOMPARE(on->emptyDirs, QStringList{QStringLiteral("addcont0")}); // DLC mounts always declared
            put(d, QStringLiteral("dlc_emu.ini"), "[PSAC]\ncontent_id=UP0000-PPSA01234_00-GOLDENDLC0000001\nmount_point=/app0/addcont7\n");
            auto missing = plan(d);
            QVERIFY(std::any_of(missing->warnings.begin(), missing->warnings.end(),
                                [](const QString& w) { return w.contains(QStringLiteral("addcont7")); }));
        }
        {
            // Size class off and on.
            const QString d = dir(QStringLiteral("plan/size"));
            makeSimpleDump(d);
            auto off = plan(d, {{keys::kAutoSizeClass, false}});
            QVERIFY(!off->paramBytes.contains("attributePub"));
            auto on = plan(d);
            QVERIFY(on->paramBytes.contains("\"attributePub\": 0"));
        }
        {
            // PS4 SELF: repaired copy when on, warning when off; AMPR import warning.
            const QString d = dir(QStringLiteral("plan/self"));
            makeSimpleDump(d);
            put(d, QStringLiteral("eboot.bin"), QByteArray("\x4F\x15\x3D\x1D", 4) + QByteArray(60, '\0') + "libSceAmpr");
            auto on = plan(d);
            QVERIFY(on.ok());
            bool repaired = false;
            for (const auto& e : on->entries)
                if (e.dst == u"eboot.bin")
                    repaired = e.kind == EntryKind::RepairedSelf && e.src.contains(QStringLiteral("normalized-self"));
            QVERIFY(repaired);
            QVERIFY(std::any_of(on->warnings.begin(), on->warnings.end(), [](const QString& w) { return w.contains(QStringLiteral("libSceAmpr")); }));
            auto off = plan(d, {{keys::kRepairSelf, false}});
            QVERIFY(std::any_of(off->warnings.begin(), off->warnings.end(), [](const QString& w) { return w.contains(QStringLiteral("PS4 SELF")); }));
        }
        {
            // UCP: checked only when the setting is on.
            const QString d = dir(QStringLiteral("plan/ucp"));
            makeSimpleDump(d);
            QByteArray stale = makeUcp({{"a.png", "hello"}});
            stale[stale.size() - 1] = 'Z';
            put(d, QStringLiteral("sce_sys/trophy2/trophy00.ucp"), stale);
            auto off = plan(d);
            QVERIFY(off.ok());
            auto on = plan(d, {{keys::kRepairUcp, true}});
            QVERIFY(on.ok());
            bool repaired = false;
            for (const auto& e : on->entries)
                if (e.dst == u"sce_sys/trophy2/trophy00.ucp")
                    repaired = e.kind == EntryKind::RepairedUcp;
            QVERIFY(repaired);
            put(d, QStringLiteral("sce_sys/trophy2/trophy00.ucp"), "garbage");
            QCOMPARE(plan(d, {{keys::kRepairUcp, true}}).error().code, ErrorCode::CorruptData);
        }
    }

    // ---- images, SELF, UCP -------------------------------------------------------------------------
    void imagesAndRepairs()
    {
        QCOMPARE(pngInfoFromBytes(makePng(2)).colorType, 2);
        QVERIFY(!pngInfoFromBytes("GIF89a").valid);
        QCOMPARE(presentationRequirement(QStringLiteral("icon0.png")), PngRequirement::Rgb);
        QCOMPARE(presentationRequirement(QStringLiteral("PIC1_03.PNG")), PngRequirement::Rgb);
        QCOMPARE(presentationRequirement(QStringLiteral("pic2_12.png")), PngRequirement::Rgba);
        QCOMPARE(presentationRequirement(QStringLiteral("pic3.png")), PngRequirement::None);
        QVERIFY(!pngMeets(pngInfoFromBytes(makePng(2, 16)), PngRequirement::Rgb));

        const QString d = dir(QStringLiteral("img"));
        put(d, QStringLiteral("sce_sys/icon0.png"), makePng(6));   // wrong: must be RGB
        put(d, QStringLiteral("sce_sys/icon0.dds"), "DDS ");
        put(d, QStringLiteral("sce_sys/pic2.dds"), "DDS ");        // PNG missing
        put(d, QStringLiteral("sce_sys/pic0.png"), makePng(2));
        put(d, QStringLiteral("sce_sys/pic0.dds"), "DDS ");        // fine already
        auto scan = scanSource({d}, {}, CancellationToken());
        QVERIFY(scan.ok());
        auto plan = planImageRecovery(scan.value(), true, true);
        QVERIFY(plan.ok());
        QCOMPARE(int(plan->conversions.size()), 2);
        QCOMPARE(plan->conversions[0].dst, QStringLiteral("sce_sys/icon0.png"));
        QCOMPARE(plan->conversions[0].replacedPngRel, QStringLiteral("sce_sys/icon0.png"));
        QCOMPARE(plan->conversions[1].dst, QStringLiteral("sce_sys/pic2.png"));
        QCOMPARE(plan->conversions[1].requirement, PngRequirement::Rgba);
        QCOMPARE(planImageRecovery(scan.value(), true, false).error().code, ErrorCode::DependencyMissing);
        QCOMPARE(planImageRecovery(scan.value(), false, true).error().code, ErrorCode::CorruptData);

        put(d, QStringLiteral("sce_sys/pic1.png"), makePng(6)); // wrong mode, no .dds
        scan = scanSource({d}, {}, CancellationToken());
        QCOMPARE(planImageRecovery(scan.value(), true, true).error().code, ErrorCode::CorruptData);

        const QString d2 = dir(QStringLiteral("img2"));
        put(d2, QStringLiteral("sce_sys/Pic2.dds"), "DDS ");
        put(d2, QStringLiteral("sce_sys/pic2.DDS"), "DDS ");
        scan = scanSource({d2}, {}, CancellationToken());
        if (scan->sceSysDds.size() == 2) // case-sensitive file systems only
            QVERIFY(planImageRecovery(scan.value(), true, true).error().message.contains(QStringLiteral("both")));

        // SELF copy with the PS5 signature; the source is untouched.
        const QString self = QDir(d).filePath(QStringLiteral("old.sprx"));
        put(d, QStringLiteral("old.sprx"), QByteArray("\x4F\x15\x3D\x1D", 4) + QByteArray(60, 'x'));
        QVERIFY(startsWithPs4SelfMagic(self));
        const QString copy = QDir(d).filePath(QStringLiteral("fixed/old.sprx"));
        QVERIFY(writeSelfRepairCopy(self, copy, CancellationToken()).ok());
        QCOMPARE(readAll(copy).left(4), kPs5SelfMagic);
        QCOMPARE(readAll(copy).mid(4), QByteArray(60, 'x'));
        QCOMPARE(readAll(self).left(4), kPs4SelfMagic);

        // UCP digest.
        QVERIFY(isUcpPath(QStringLiteral("sce_sys/uds/uds00.ucp")));
        QVERIFY(!isUcpPath(QStringLiteral("sce_sys/uds/sub/uds00.ucp")));
        const QByteArray ucp = makeUcp({{"b.json", "{}"}, {"a.png", "png"}});
        QVERIFY(checkUcp(ucp).structureOk);
        QVERIFY(checkUcp(ucp).digestOk);
        QByteArray stale = ucp;
        stale[stale.size() - 20] = 'Q';
        QVERIFY(checkUcp(stale).structureOk);
        QVERIFY(!checkUcp(stale).digestOk);
        QVERIFY(checkUcp(withRepairedUcpDigest(stale)).digestOk);
        QByteArray truncated = ucp.left(ucp.size() - 16);
        QVERIFY(!checkUcp(truncated).structureOk);
        QVERIFY(!checkUcp("short").structureOk);
    }

    // ---- tool output ----------------------------------------------------------------------------------
    void outputParserCountsBarAcrossSplits()
    {
        const QByteArray stream = QByteArray("[Info] Creating an image...\r\n|____|____|____|\r\n0        20        40\r\n")
            + QByteArray(51, '=')
            + "\r\n[Error] boom\n[Warn] The version of the SDK toolchain is invalid or missing\n[Warn] real problem\n"
              "random text\n[Info] key=value done\r[Error] second\n[Error] third\n[Error] fourth";
        for (int seed = 0; seed < 20; ++seed) {
            QList<ToolLine> lines;
            int steps = 0, calls = 0;
            PubCmdOutputParser p([&](const ToolLine& l) { lines << l; },
                                 [&](int s) {
                                     QVERIFY(s == steps + 1);
                                     steps = s;
                                     ++calls;
                                 });
            QRandomGenerator rng{quint32(seed)};
            qsizetype i = 0;
            while (i < stream.size()) {
                const qsizetype n = seed == 0 ? 1 : qsizetype(rng.bounded(1, 40));
                p.feed(stream.mid(i, n));
                i += n;
            }
            p.finish();
            QCOMPARE(steps, 51);
            QCOMPARE(calls, 51);
            QVERIFY(p.sawBar());
            QStringList texts;
            for (const auto& l : lines)
                if (l.kind != LineKind::Dropped)
                    texts << l.text;
            QCOMPARE(texts, (QStringList{QStringLiteral("Creating an image..."), QStringLiteral("boom"),
                                         QStringLiteral("The version of the SDK toolchain is invalid or missing"),
                                         QStringLiteral("real problem"), QStringLiteral("key=value done"),
                                         QStringLiteral("second"), QStringLiteral("third"), QStringLiteral("fourth")}));
            QCOMPARE(lines[2].kind, LineKind::Info);    // routine warning
            QCOMPARE(lines[3].kind, LineKind::Warning);
        }
        QCOMPARE(classifyToolLine(QStringLiteral("wine: something")).kind, LineKind::Warning);
        QCOMPARE(classifyToolLine(QStringLiteral("0009:err:module")).kind, LineKind::Warning);
        QCOMPARE(classifyToolLine(QStringLiteral("[Debug] x")).kind, LineKind::Info);
        QCOMPARE(classifyToolLine(QStringLiteral("hello")).kind, LineKind::Dropped);
        QVERIFY(isRoutineWarning(QStringLiteral("Process finished with warning(s)")));

        ToolRunResult r;
        r.exitCode = 0;
        r.errors = {QStringLiteral("Format of the png file is not valid (sce_sys/pic2.png)"), QStringLiteral("b"),
                    QStringLiteral("c"), QStringLiteral("d")};
        QVERIFY(!r.ok());
        QVERIFY(r.failureText().contains(QStringLiteral("8-bit RGBA")));
        QVERIFY(!r.failureText().contains(QStringLiteral(" d")));
        ToolRunResult crash;
        crash.exitCode = int(0xC0000135u);
        crash.crashed = true;
        QVERIFY(crash.failureText().contains(QStringLiteral("Visual C++")));
        QVERIFY(exitCodeMeansMissingRuntime(int(0xC000007Bu)));
        QVERIFY(exitCodeHint(int(0xC000007Bu)).contains(QStringLiteral("architecture")));
        QVERIFY(exitCodeHint(3).isEmpty());
        QCOMPARE(formatElapsed(3723456), QStringLiteral("01:02:03.4560000"));

        QCOMPARE(imgCreateArgs(QStringLiteral("p.gp5"), QStringLiteral("o.pkg"), 4, QStringLiteral("b.pkg")),
                 (QStringList{QStringLiteral("img_create"), QStringLiteral("--oformat"), QStringLiteral("nwonly"),
                              QStringLiteral("--compression_level"), QStringLiteral("4"), QStringLiteral("--ref_pkg_path"),
                              QStringLiteral("b.pkg"), QStringLiteral("p.gp5"), QStringLiteral("o.pkg")}));
        QCOMPARE(imgVerifyArgs(kDefaultPasscode, false, QStringLiteral("x.pkg")).join(u' '),
                 QStringLiteral("img_verify --passcode %1 --format_check on --integrity_check off --no_progress_bar x.pkg").arg(kDefaultPasscode));
        QCOMPARE(imgExtractArgs(kDefaultPasscode, QStringLiteral("x.pkg"), QStringLiteral("d")).join(u' '),
                 QStringLiteral("img_extract --passcode %1 --no_progress_bar x.pkg d").arg(kDefaultPasscode));
        QCOMPARE(dds2pngArgs(QStringLiteral("a.dds"), QStringLiteral("a.png"), true).last(), QStringLiteral("--preserve-alpha"));
    }

    void headerCheck()
    {
        const QString d = dir(QStringLiteral("hdr"));
        const auto check = [&](const QByteArray& bytes) {
            put(d, QStringLiteral("p.pkg"), bytes);
            return checkPackageHeader(QDir(d).filePath(QStringLiteral("p.pkg")));
        };
        QVERIFY(check(fakePackage(true, true)).ok);
        HeaderCheck stock = check(fakePackage(true, false));
        QVERIFY(!stock.ok);
        QVERIFY(stock.hasFih);
        QVERIFY(stock.summary().contains(QStringLiteral("marker")));
        QVERIFY(check(fakePackage(true, true, 13, 1)).summary().contains(QStringLiteral("signed")));
        QVERIFY(check(fakePackage(true, true, 12)).summary().contains(QStringLiteral("mode 12")));
        QVERIFY(check(fakePackage(true, true, 13, 0, 0x1F00)).summary().contains(QStringLiteral("outside")));
        QVERIFY(check(fakePackage(true, true, 13, 0, 0x1000, 4000)).summary().contains(QStringLiteral("4000 bytes")));
        QVERIFY(!check(fakePackage(false, false)).hasFih);
        QVERIFY(!checkPackageHeader(QDir(d).filePath(QStringLiteral("missing.pkg"))).ok);
    }

    void conflictPolicies()
    {
        const QString d = dir(QStringLiteral("conflict"));
        const QString final = QDir(d).filePath(QStringLiteral("x.pkg"));
        const auto partial = [&](const QByteArray& data) {
            put(d, QStringLiteral("x.partial.pkg"), data);
            return QDir(d).filePath(QStringLiteral("x.partial.pkg"));
        };
        QStringList notes;
        QVERIFY(placeOutput(partial("1"), final, OutputConflict::KeepBoth, notes).ok());
        QVERIFY(notes.isEmpty());
        QVERIFY(placeOutput(partial("2"), final, OutputConflict::KeepBoth, notes).ok());
        QCOMPARE(readAll(QDir(d).filePath(QStringLiteral("x (1).pkg"))), QByteArray("1"));
        QCOMPARE(readAll(final), QByteArray("2"));
        QVERIFY(placeOutput(partial("3"), final, OutputConflict::KeepBoth, notes).ok());
        QCOMPARE(readAll(QDir(d).filePath(QStringLiteral("x (2).pkg"))), QByteArray("2"));
        QCOMPARE(notes.size(), 2);
        QVERIFY(placeOutput(partial("4"), final, OutputConflict::Overwrite, notes).ok());
        QCOMPARE(readAll(final), QByteArray("4"));
        QCOMPARE(QDir(d).entryList(QDir::Files).size(), 3); // x.pkg, x (1).pkg, x (2).pkg: nothing set aside is left
        const QString p5 = partial("5");
        const Status fail = placeOutput(p5, final, OutputConflict::Fail, notes);
        QCOMPARE(fail.error().code, ErrorCode::AlreadyExists);
        QCOMPARE(readAll(final), QByteArray("4"));
        QCOMPARE(keepBothName(QDir(d).filePath(QStringLiteral("y.remastered.pkg"))), QDir(d).filePath(QStringLiteral("y (1).remastered.pkg")));
        // A failed rename restores the file that was set aside.
        QVERIFY(!placeOutput(QDir(d).filePath(QStringLiteral("missing.partial.pkg")), final, OutputConflict::Overwrite, notes).ok());
        QCOMPARE(readAll(final), QByteArray("4"));
    }

    // An update and its .remastered.pkg are placed as one step: every existing
    // file is set aside first, a failure anywhere undoes all of it, and the new
    // packages (hours of packing) are kept under another name, never deleted.
    void placementIsAllOrNothing()
    {
        const QString d = dir(QStringLiteral("placeall"));
        const auto at = [&](const char* name) { return QDir(d).filePath(QString::fromLatin1(name)); };
        const QString upd = at("UPDATE_x.pkg"), full = at("x.remastered.pkg");
        const QString pUpd = at("x.partial.pkg"), pFull = at("x.partial.pkg.remastered.pkg");
        const QString keptUpd = at("UPDATE_x (1).pkg"), keptFull = at("x (1).remastered.pkg");
        const std::vector<OutputMove> moves{{pUpd, upd}, {pFull, full}};
        const auto reset = [&] {
            QDir(d).removeRecursively();
            QDir().mkpath(d);
            put(d, QStringLiteral("UPDATE_x.pkg"), "old update");
            put(d, QStringLiteral("x.remastered.pkg"), "old full");
            put(d, QStringLiteral("x.partial.pkg"), "new update");
            put(d, QStringLiteral("x.partial.pkg.remastered.pkg"), "new full");
        };
        // A rename that fails for one source (and target, when given), as for
        // a file another program holds open on Windows.
        const auto failing = [](const QString& badFrom, const QString& badTo) {
            return RenameFn([=](const QString& from, const QString& to) {
                if (from == badFrom && (badTo.isEmpty() || to == badTo))
                    return false;
                return QFile::rename(from, to);
            });
        };
        const auto files = [&] { return QDir(d).entryList(QDir::Files, QDir::Name); };

        // The existing .remastered.pkg cannot be set aside (the update's could):
        // both old files are back, both new packages are kept and named.
        for (const OutputConflict policy : {OutputConflict::Overwrite, OutputConflict::KeepBoth}) {
            reset();
            QStringList notes, kept;
            const Status st = placeOutputs(moves, policy, notes, &kept, failing(full, {}));
            QVERIFY(!st.ok());
            QCOMPARE(readAll(upd), QByteArray("old update"));
            QCOMPARE(readAll(full), QByteArray("old full"));
            QCOMPARE(kept, (QStringList{keptUpd, keptFull}));
            QCOMPARE(readAll(keptUpd), QByteArray("new update"));
            QCOMPARE(readAll(keptFull), QByteArray("new full"));
            QVERIFY2(st.error().message.contains(QDir::toNativeSeparators(keptUpd)), qPrintable(st.error().message));
            QVERIFY(st.error().message.contains(QDir::toNativeSeparators(keptFull)));
            QVERIFY(notes.isEmpty());
            QCOMPARE(files().size(), 4); // nothing set aside is left over
        }

        // The update took its name, then the .remastered.pkg could not: the
        // update is rolled back too, so old and new are never mixed.
        reset();
        {
            QStringList notes, kept;
            const Status st = placeOutputs(moves, OutputConflict::Overwrite, notes, &kept, failing(pFull, full));
            QVERIFY(!st.ok());
            QCOMPARE(st.error().code, ErrorCode::IoError);
            QCOMPARE(readAll(upd), QByteArray("old update"));
            QCOMPARE(readAll(full), QByteArray("old full"));
            QCOMPARE(kept, (QStringList{keptUpd, keptFull}));
            QCOMPARE(readAll(keptUpd), QByteArray("new update"));
            QCOMPARE(readAll(keptFull), QByteArray("new full"));
            QCOMPARE(files().size(), 4);
        }

        // Nothing can be renamed at all: the packages stay under their partial names.
        reset();
        {
            QStringList notes, kept;
            const Status st = placeOutputs(moves, OutputConflict::Overwrite, notes, &kept,
                                           [](const QString&, const QString&) { return false; });
            QVERIFY(!st.ok());
            QCOMPARE(kept, (QStringList{pUpd, pFull}));
            QCOMPARE(readAll(pUpd), QByteArray("new update"));
            QCOMPARE(readAll(pFull), QByteArray("new full"));
            QCOMPARE(readAll(upd), QByteArray("old update"));
            QVERIFY(st.error().message.contains(QDir::toNativeSeparators(pUpd)));
        }

        // A new file appearing at a final name under "Fail" stops the step,
        // keeps the existing file and still keeps the new packages.
        reset();
        {
            QStringList notes, kept;
            const Status st = placeOutputs(moves, OutputConflict::Fail, notes, &kept);
            QCOMPARE(st.error().code, ErrorCode::AlreadyExists);
            QCOMPARE(readAll(upd), QByteArray("old update"));
            QCOMPARE(kept, (QStringList{keptUpd, keptFull}));
        }

        // Success: both replaced, or both old ones renamed.
        reset();
        {
            QStringList notes;
            QVERIFY(placeOutputs(moves, OutputConflict::Overwrite, notes).ok());
            QCOMPARE(readAll(upd), QByteArray("new update"));
            QCOMPARE(readAll(full), QByteArray("new full"));
            QCOMPARE(files(), (QStringList{QStringLiteral("UPDATE_x.pkg"), QStringLiteral("x.remastered.pkg")}));
            QCOMPARE(notes.size(), 2);
        }
        reset();
        {
            QStringList notes;
            QVERIFY(placeOutputs(moves, OutputConflict::KeepBoth, notes).ok());
            QCOMPARE(readAll(upd), QByteArray("new update"));
            QCOMPARE(readAll(keptUpd), QByteArray("old update"));
            QCOMPARE(readAll(keptFull), QByteArray("old full"));
            QCOMPARE(files().size(), 4);
            QCOMPARE(notes.size(), 2);
        }
    }

    // ---- settings, toolchain, stagers ---------------------------------------------------------------
    void settingsAndLegacyKeys()
    {
        Env env;
        for (const auto* d : env.schema->all()) {
            if (!d->key.startsWith(QStringLiteral("fpkg.")))
                continue;
            QVERIFY2(env.schema->normalize(d->key, d->defaultValue).ok(), qPrintable(d->key));
            QVERIFY2(!d->description.isEmpty(), qPrintable(d->key));
            if (!d->enabledWhenKey.isEmpty())
                QVERIFY2(env.schema->contains(d->enabledWhenKey), qPrintable(d->key));
            if (!d->dependencyId.isEmpty()) {
                const auto ids = env.deps.ids();
                QVERIFY2(std::find(ids.begin(), ids.end(), d->dependencyId) != ids.end(), qPrintable(d->key));
            }
        }
        QVERIFY(!env.schema->normalize(keys::kPasscode, QStringLiteral("short")).ok());
        QVERIFY(!env.schema->normalize(keys::kPasscode, QString(31, u'0') + QChar(0xE9)).ok());
        QVERIFY(!env.schema->normalize(keys::kPlayGoChunkCount, 256).ok());
        QVERIFY(!env.schema->normalize(keys::kCompressionLevel, QStringLiteral("10")).ok());

        const auto& legacy = env.schema->legacyKeys();
        const auto conv = legacy.at(QStringLiteral("fpkg_compression")).convert;
        QCOMPARE(conv(QStringLiteral("Tool default"))->toString(), QStringLiteral("default"));
        QCOMPARE(conv(QStringLiteral("Fast (2)"))->toString(), QStringLiteral("2"));
        QCOMPARE(conv(QStringLiteral("Standard (4)"))->toString(), QStringLiteral("4"));
        QCOMPARE(conv(QStringLiteral("Smallest (7)"))->toString(), QStringLiteral("7"));
        QCOMPARE(conv(QStringLiteral("Maximum (9)"))->toString(), QStringLiteral("9"));
        QVERIFY(!conv(QStringLiteral("Ultra (12)")));
        QCOMPARE(legacy.at(QStringLiteral("fpkg_tool_verify")).convert(false)->toString(), QStringLiteral("headerOnly"));
        QCOMPARE(legacy.at(QStringLiteral("fpkg_pubtools_path")).key, keys::kPubToolsPath);
        QCOMPARE(legacy.at(QStringLiteral("fpkg_chunk_count")).key, keys::kPlayGoChunkCount);
        QCOMPARE(legacy.at(QStringLiteral("fpkg_keep_project")).key, keys::kKeepIntermediate);

        const FpkgSettings def = FpkgSettings::from(env.values());
        QVERIFY(!def.compressionLevel);
        QCOMPARE(def.passcode, kDefaultPasscode);
        QCOMPARE(def.outputConflict, OutputConflict::KeepBoth);
        QCOMPARE(def.postVerify, PostVerify::ToolFull);
        QCOMPARE(def.playGoChunkCount, 100);
        QVERIFY(!def.repairUcp && !def.useSourcePlayGoTable && def.repairSelf && def.fillEmptyChunks);
        QCOMPARE(def.cpuCoreLimit, 0);
        const FpkgSettings custom = FpkgSettings::from(env.values({{keys::kCompressionLevel, QStringLiteral("-4")},
                                                                   {general::keys::kVerifyAfterBuild, false},
                                                                   {general::keys::kWorkerThreads, 1},
                                                                   {keys::kOutputConflict, QStringLiteral("overwrite")}}));
        QCOMPARE(custom.compressionLevel, std::optional<int>(-4));
        QCOMPARE(custom.postVerify, PostVerify::HeaderOnly);
        QCOMPARE(custom.outputConflict, OutputConflict::Overwrite);
        if (QThread::idealThreadCount() > 1)
            QCOMPARE(custom.cpuCoreLimit, 1);
        QCOMPARE(env.jobs.kinds(), (QStringList{QStringLiteral("fpkg.addon"), QStringLiteral("fpkg.build"),
                                                QStringLiteral("fpkg.extract"), QStringLiteral("fpkg.verify")}));
    }

    void toolchainLookup()
    {
        const QString root = dir(QStringLiteral("tk"));
        QVERIFY(!locatePubTools(QString(), QString()).ok());
        QVERIFY(!locatePubTools(QDir(root).filePath(QStringLiteral("nope")), QString()).ok());
        put(root, QStringLiteral("toolchain/prospero-pub-cmd.exe"), "exe");
        PubToolchain t = locatePubTools(root, QString());
        QVERIFY(t.ok());
        QCOMPARE(QFileInfo(t.exe).fileName(), kPubCmdExeName);
        QCOMPARE(QDir::cleanPath(t.toolkitRoot), QDir::cleanPath(root));
        QCOMPARE(t.warnings.size(), 2); // no libScePubTools.dll, no ext folder
        put(root, QStringLiteral("toolchain/libScePubTools.dll"), "");
        QDir().mkpath(QDir(root).filePath(QStringLiteral("toolchain/ext")));
        put(root, QStringLiteral("toolchain/patch-manifest.json"), "{\"profile\":\"test-profile\"}");
        t = locatePubTools(QDir(root).filePath(QStringLiteral("toolchain/prospero-pub-cmd.exe")), QString());
        QVERIFY(t.warnings.isEmpty());
        QCOMPARE(t.profile, QStringLiteral("test-profile"));
        QVERIFY(locatePubTools(QString(), QDir(root).filePath(QStringLiteral("toolchain"))).ok()); // environment fallback
        put(root, QStringLiteral("bin2/other.exe"), "exe");
        QVERIFY(!locatePubTools(QDir(root).filePath(QStringLiteral("bin2/other.exe")), QString()).ok());
        QVERIFY(locateDds2png(t, QString()).isEmpty());
        put(root, QStringLiteral("prospero-dds2png.exe"), "exe");
        QCOMPARE(QFileInfo(locateDds2png(t, QString())).fileName(), kDds2pngExeName);
        QVERIFY(locateDds2png(t, QDir(root).filePath(QStringLiteral("missing.exe"))).isEmpty());
#ifndef Q_OS_WIN
        QVERIFY(missingVcRuntimeDlls(QString()).isEmpty());
#endif
        Env env;
        const auto ready = env.deps.probe(QStringLiteral("sony.pubcmd"), env.values({{keys::kPubToolsPath, root}}));
        QCOMPARE(ready.status, DependencyStatus::Ready);
        QCOMPARE(env.deps.probe(QStringLiteral("sony.dds2png"), env.values({{keys::kPubToolsPath, root}})).status, DependencyStatus::Ready);
        const auto missing = env.deps.probe(QStringLiteral("sony.pubcmd"), env.values({{keys::kPubToolsPath, QDir(root).filePath(QStringLiteral("x"))}}));
        QVERIFY(missing.status != DependencyStatus::Ready);
        QVERIFY(!missing.howToFix.isEmpty());
    }

    void sourceStagersAndAppRoot()
    {
        unregisterSourceStagers();
        QVERIFY(!sourceStagerFor(QStringLiteral("game.exfat")));
        registerSourceStager(std::make_shared<FakeStager>());
        QVERIFY(sourceStagerFor(QStringLiteral("C:/x/GAME.EXFAT")));
        QVERIFY(!sourceStagerFor(QStringLiteral("game.ffpkg")));
        unregisterSourceStagers();

        const QString d = dir(QStringLiteral("approot"));
        QDir().mkpath(QDir(d).filePath(QStringLiteral("a/b/PPSA01234-app/sce_sys")));
        QDir().mkpath(QDir(d).filePath(QStringLiteral("z/sce_sys_not")));
        QCOMPARE(QDir::cleanPath(findAppRoot(d, 3)), QDir::cleanPath(QDir(d).filePath(QStringLiteral("a/b/PPSA01234-app"))));
        QVERIFY(findAppRoot(d, 2).isEmpty());
        const QString g = dir(QStringLiteral("g"));
        QDir().mkpath(QDir(g).filePath(QStringLiteral("Game")));
        QCOMPARE(defaultOutputFolder(QDir(g).filePath(QStringLiteral("Game")), QString()), QDir(g).filePath(QStringLiteral("Game-pkg")));
        QCOMPARE(defaultOutputFolder(QDir(g).filePath(QStringLiteral("img.exfat")), QString()), QDir(g).filePath(QStringLiteral("img-pkg")));
        QCOMPARE(defaultOutputFolder(QDir(g).filePath(QStringLiteral("Game")), d), QFileInfo(d).absoluteFilePath());
    }
};

QTEST_GUILESS_MAIN(TstFpkgRules)
#include "tst_fpkg_rules.moc"
