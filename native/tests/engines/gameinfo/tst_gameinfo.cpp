// gameinfo: param.sfo / param.json / pfs-version.dat / nptitle.dat chain,
// detection, naming, covers, library scan, Dump Rename and the module's jobs.
// Every dump is synthetic and built in a temporary folder.

#include "core/FsLinks.h"
#include "engines/gameinfo/Cover.h"
#include "engines/gameinfo/DumpRename.h"
#include "engines/gameinfo/GameInfo.h"
#include "engines/gameinfo/JsonText.h"
#include "engines/gameinfo/Library.h"
#include "engines/gameinfo/Module.h"
#include "engines/gameinfo/Naming.h"
#include "engines/gameinfo/ParamJson.h"
#include "engines/gameinfo/Sfo.h"
#include "engines/gameinfo/Version.h"
#include "support/TestSupport.h"

#include <QDir>
#include <QFile>
#include <QHash>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>

#include <memory>
#include <zlib.h>

using namespace fw;
using namespace fw::gameinfo;

namespace {

// Independent param.sfo writer (not the code under test).
struct SfoField {
    QByteArray key;
    quint16 format;
    QByteArray value; // already encoded
    quint32 maxLen;
};

SfoField sfoString(const char* key, const QString& v, quint32 maxLen = 0)
{
    QByteArray b = v.toUtf8();
    b.append('\0');
    return {key, 0x0204, b, maxLen ? maxLen : quint32((b.size() + 3) & ~3)};
}

SfoField sfoInt(const char* key, quint32 v)
{
    QByteArray b(4, '\0');
    qToLittleEndian<quint32>(v, b.data());
    return {key, 0x0404, b, 4};
}

QByteArray buildSfo(const std::vector<SfoField>& fields)
{
    QByteArray keys, data;
    QByteArray table;
    for (const auto& f : fields) {
        QByteArray e(16, '\0');
        qToLittleEndian<quint16>(quint16(keys.size()), e.data());
        qToLittleEndian<quint16>(f.format, e.data() + 2);
        qToLittleEndian<quint32>(quint32(f.value.size()), e.data() + 4);
        qToLittleEndian<quint32>(f.maxLen, e.data() + 8);
        qToLittleEndian<quint32>(quint32(data.size()), e.data() + 12);
        table += e;
        keys += f.key + '\0';
        QByteArray v = f.value;
        v.resize(qMax<qsizetype>(v.size(), f.maxLen), '\0');
        data += v;
    }
    while (keys.size() % 4)
        keys.append('\0');
    QByteArray h(20, '\0');
    qToLittleEndian<quint32>(0x46535000, h.data());
    qToLittleEndian<quint32>(0x101, h.data() + 4);
    const quint32 keyOff = 20 + quint32(table.size());
    qToLittleEndian<quint32>(keyOff, h.data() + 8);
    qToLittleEndian<quint32>(keyOff + quint32(keys.size()), h.data() + 12);
    qToLittleEndian<quint32>(quint32(fields.size()), h.data() + 16);
    return h + table + keys + data;
}

bool write(const QString& path, const QByteArray& bytes) { return fwtest::writeTextFile(path, bytes); }

// The error of a call that must have failed (a marker error, not undefined
// behaviour, when it succeeded).
template <typename R>
Error errorOf(const R& result)
{
    return result.ok() ? Error(ErrorCode::Internal, QStringLiteral("<the call succeeded>")) : result.error();
}

QByteArray ps5Param(const QString& titleId, const QString& title, const QString& lang = QStringLiteral("en-US"),
                    const QString& extra = {})
{
    return QStringLiteral("{\n"
                          "  \"applicationCategoryType\": 0,\n"
                          "  \"applicationDrmType\": \"standard\",\n"
                          "  \"contentId\": \"UP9000-%1_00-ABCDEFGHIJKLMNOP\",\n"
                          "  \"contentVersion\": \"01.002.000\",\n"
                          "  \"masterVersion\": \"01.02\",\n"
                          "  \"titleId\": \"%1\",\n"
                          "  \"sdkVersion\": \"0x0450000000000000\",\n"
                          "  \"requiredSystemSoftwareVersion\": \"0x0700000000000000\",\n"
                          "  \"versionFileUri\": \"http://example.invalid/version.xml\",\n"
                          "  \"attribute\": 4,\n"
                          "  \"localizedParameters\": {\n"
                          "    \"defaultLanguage\": \"%3\",\n"
                          "    \"%3\": { \"titleName\": \"%2\" }\n"
                          "  }%4\n"
                          "}")
        .arg(titleId, title, lang, extra)
        .toUtf8();
}

// A PS5 dump: <root>/eboot.bin + sce_sys/param.json (+ icon0.png). False when
// a file could not be written.
[[nodiscard]] bool makePs5Dump(const QString& root, const QString& titleId, const QString& title, qint64 ebootSize = 1000,
                               bool icon = false)
{
    if (!fwtest::writeRandomFile(QDir(root).filePath(QStringLiteral("eboot.bin")), ebootSize, 7)
        || !write(QDir(root).filePath(QStringLiteral("sce_sys/param.json")), ps5Param(titleId, title)))
        return false;
    if (!icon)
        return true;
    QImage img(512, 512, QImage::Format_RGB32);
    img.fill(QColor(200, 30, 30));
    return img.save(QDir(root).filePath(QStringLiteral("sce_sys/icon0.png")), "PNG");
}

JobContext makeContext(ProgressReporter& progress, const SettingsValues& settings, const QString& temp)
{
    JobContext ctx;
    ctx.jobId = QStringLiteral("t");
    ctx.progress = &progress;
    ctx.log = Log(QStringLiteral("test"));
    ctx.settings = settings;
    ctx.tempDir = temp;
    return ctx;
}

struct Module {
    std::shared_ptr<SettingsSchema> schema = std::make_shared<SettingsSchema>();
    DependencyRegistry deps;
    JobFactory jobs;
    Module()
    {
        ModuleContext ctx{*schema, deps, jobs};
        registerModule(ctx);
    }
    SettingsValues settings(const QVariantMap& values = {}) const { return SettingsValues(schema, values, {}); }
};

} // namespace

class TstGameInfo : public QObject {
    Q_OBJECT
    QTemporaryDir m_root;
    int m_n = 0;

    QString fresh(const QString& name)
    {
        const QString p = m_root.filePath(QStringLiteral("%1-%2").arg(++m_n).arg(name));
        QDir().mkpath(p);
        return p;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_root.isValid());
        fwtest::isolateAppData(m_root);
    }

    // ---- versions, IDs, names ---------------------------------------------------
    void systemVersionDecoding()
    {
        QCOMPARE(decodeSystemVersion(0x04508001).toString(), QStringLiteral("4.50"));
        QCOMPARE(decodeSystemVersion(0x10000000).toString(), QStringLiteral("10.00"));
        QCOMPARE(decodeSystemVersion(0x05050000).toString(), QStringLiteral("5.05"));
        QCOMPARE(decodeSystemVersion(0x00090000).toString(), QStringLiteral("9.00")); // shift-16 rule
        QVERIFY(!decodeSystemVersion(0).isValid());
        QCOMPARE(decodeSystemVersion(0x04508001).majorLabel(), QStringLiteral("4.xx"));
        QCOMPARE(parseVersionField(QJsonValue(QStringLiteral("0x1270000000000000"))).toString(), QStringLiteral("12.70"));
        QCOMPARE(parseVersionField(QJsonValue(QStringLiteral("0x0450000000000000"))).toString(), QStringLiteral("4.50"));
        QCOMPARE(parseVersionField(QJsonValue(QStringLiteral("0x0A00000000000000"))).toString(), QStringLiteral("10.00"));
        QCOMPARE(parseVersionField(QJsonValue(QStringLiteral("7"))).toString(), QStringLiteral("7.00"));
        QCOMPARE(parseVersionField(QJsonValue(9)).toString(), QStringLiteral("9.00"));
        QVERIFY(!parseVersionField(QJsonValue(QStringLiteral("banana"))).isValid());
        QVERIFY(!parseVersionField(QJsonValue(QStringLiteral("0x0000000000000000"))).isValid());
        QVERIFY(!parseVersionField(QJsonValue()).isValid());
    }

    void contentVersionNormalisation()
    {
        QCOMPARE(normalizeContentVersion(QStringLiteral("01.00")), QStringLiteral("01.000.000"));
        QCOMPARE(normalizeContentVersion(QStringLiteral("1.007.000")), QStringLiteral("01.007.000"));
        QCOMPARE(normalizeContentVersion(QStringLiteral("01.00.000")), QStringLiteral("01.000.000"));
        QCOMPARE(normalizeContentVersion(QStringLiteral("01.000.000.5")), QStringLiteral("01.000.000"));
        QCOMPARE(normalizeContentVersion(QStringLiteral("2")), QStringLiteral("2"));
        QCOMPARE(normalizeContentVersion(QString()), QString());
    }

    void contentAndTitleIds()
    {
        const QString cid = QStringLiteral("UP9000-PPSA01325_00-ASTROBOTPS5GAME0");
        QCOMPARE(cid.size(), 36);
        QVERIFY(isValidContentId(cid));
        QCOMPARE(titleIdFromContentId(cid), QStringLiteral("PPSA01325"));
        QVERIFY(!isValidContentId(QStringLiteral("up9000-PPSA01325_00-ASTROBOTPS5GAME0")));
        QVERIFY(!isValidContentId(QStringLiteral("UP9000-PPSA01325_00-ASTROBOTPS5GAME")));
        QVERIFY(!isValidContentId(QStringLiteral("UP9000-PPSA01325-00-ASTROBOTPS5GAME0")));
        QVERIFY(titleIdFromContentId(QStringLiteral("nope")).isEmpty());
        QVERIFY(isValidTitleId(QStringLiteral("CUSA00001")));
        QVERIFY(!isValidTitleId(QStringLiteral("CUSA0001")));
        QCOMPARE(findTitleIdIn(QStringLiteral("[DLPSGAME] ppsa01325 Astro")), QStringLiteral("PPSA01325"));
        QVERIFY(findTitleIdIn(QStringLiteral("PPSB01325")).isEmpty());
        QCOMPARE(normalizedTitleId(QStringLiteral("ppsa-01325")), QStringLiteral("PPSA01325"));
    }

    void sanitizerMatchesLegacyRules()
    {
        QCOMPARE(sanitizeTitle(QString::fromUtf8("Marvel\u2019s Spider-Man\u2122 \u2013 Miles Morales\u2026")),
                 QStringLiteral("Marvel's Spider-Man - Miles Morales")); // trailing dots are stripped (legacy)
        QCOMPARE(sanitizeTitle(QString::fromUtf8("Wait\u2026 What")), QStringLiteral("Wait... What"));
        QCOMPARE(sanitizeTitle(QStringLiteral("Ratchet & Clank: Rift Apart")), QStringLiteral("Ratchet Clank Rift Apart"));
        QCOMPARE(sanitizeTitle(QStringLiteral("  .A\tB\x01 C?*.  ")), QStringLiteral("A B C"));
        QCOMPARE(sanitizeTitle(QStringLiteral("100% Orange Juice!")), QStringLiteral("100 Orange Juice"));
        QCOMPARE(sanitizeTitle(QStringLiteral("\u2022 Bullet\u00b7Point")), QStringLiteral("Bullet Point"));
        QCOMPARE(sanitizeTitle(QStringLiteral("***")), QStringLiteral("game"));
        QCOMPARE(sanitizeTitle(QString()), QString());
        QCOMPARE(sanitizeTitle(QString::fromUtf8("\u30c6\u30b9\u30c8 \u30b2\u30fc\u30e0")),
                 QString::fromUtf8("\u30c6\u30b9\u30c8 \u30b2\u30fc\u30e0"));
        // 80 code points, never splitting a surrogate pair.
        QString emoji;
        for (int i = 0; i < 100; ++i)
            emoji += QString::fromUtf8("\U0001F3AE");
        const QString cut = sanitizeTitle(emoji);
        QCOMPARE(cut.size(), 160);
        QVERIFY(!cut.back().isHighSurrogate());
        QCOMPARE(sanitizeTitle(QString(120, u'x')).size(), 80);
    }

    void dumpRenamePresetsAndImageNames()
    {
        const NameParts p{QStringLiteral("ppsa01234"), QStringLiteral("Spider-Man"), QStringLiteral("01.050.000")};
        QCOMPARE(buildDumpName(p, NamePreset::TitleId), QStringLiteral("PPSA01234"));
        QCOMPARE(buildDumpName(p, NamePreset::TitleIdTitle), QStringLiteral("PPSA01234 Spider-Man"));
        QCOMPARE(buildDumpName(p, NamePreset::TitleIdTitleVersion), QStringLiteral("PPSA01234 Spider-Man (01.050.000)"));
        NameOptions keepCase;
        keepCase.uppercaseTitleId = false;
        QCOMPARE(buildDumpName(p, NamePreset::TitleId, keepCase), QStringLiteral("ppsa01234"));
        QCOMPARE(buildDumpName({QStringLiteral("PPSA01234"), QStringLiteral("PPSA01234"), {}}, NamePreset::TitleIdTitle),
                 QStringLiteral("PPSA01234"));
        QVERIFY(buildDumpName({{}, QStringLiteral("No ID"), {}}, NamePreset::TitleIdTitle).isEmpty());
        QCOMPARE(buildDumpName({QStringLiteral("PPSA01234"), QStringLiteral("A: B?"), {}}, NamePreset::TitleIdTitle),
                 QStringLiteral("PPSA01234 A B"));
        QCOMPARE(namePresetKey(NamePreset::TitleIdTitle), QStringLiteral("ppsa_title"));
        QCOMPARE(*namePresetFromKey(QStringLiteral("ppsa_title_v")), NamePreset::TitleIdTitleVersion);
        QVERIFY(!namePresetFromKey(QStringLiteral("other")));

        QCOMPARE(buildImageBaseName(p), QStringLiteral("ppsa01234 Spider-Man (01.050.000)"));
        QCOMPARE(buildImageBaseName({}), QStringLiteral("game"));
        QCOMPARE(buildImageBaseName({{}, QStringLiteral("Only Title"), {}}), QStringLiteral("Only Title"));

        QVERIFY(folderNameProblem(QStringLiteral("PPSA01234 Fine")).isEmpty());
        QVERIFY(!folderNameProblem(QStringLiteral("a/b")).isEmpty());
        QVERIFY(!folderNameProblem(QStringLiteral("CON")).isEmpty());
        QVERIFY(!folderNameProblem(QStringLiteral("trailing.")).isEmpty());
    }

    // Windows opens these names as devices, with an extension too, also with
    // spaces before it ("CON .txt"), and reads ¹ ² ³ as digits: an image or
    // folder name never becomes one of them.
    void deviceNamesAreNeverImageOrFolderNames_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<QString>("image"); // what sanitizeImageName() makes of it
        QTest::newRow("CON") << QStringLiteral("CON") << QStringLiteral("CON_");
        QTest::newRow("COM9 with extension") << QStringLiteral("COM9.img") << QStringLiteral("COM9_.img");
        QTest::newRow("console in") << QStringLiteral("CONIN$") << QStringLiteral("CONIN$_");
        QTest::newRow("console out, lower case, extension") << QStringLiteral("conout$.txt") << QStringLiteral("conout$_.txt");
        QTest::newRow("superscript COM") << QStringLiteral("COM¹") << QStringLiteral("COM¹_");
        QTest::newRow("superscript LPT, extension") << QStringLiteral("lpt³.log") << QStringLiteral("lpt³_.log");
        QTest::newRow("spaces before the extension") << QStringLiteral("NUL .backup") << QStringLiteral("NUL_ .backup");
    }

    void deviceNamesAreNeverImageOrFolderNames()
    {
        QFETCH(QString, name);
        QFETCH(QString, image);
        QVERIFY2(folderNameProblem(name).contains(QStringLiteral("reserved")), qPrintable(name));
        QCOMPARE(sanitizeImageName(name), image);
        QVERIFY2(folderNameProblem(image).isEmpty(), qPrintable(image));
    }

    void namesLikeDeviceNamesStayAsTheyAre()
    {
        for (const QString& name : {QStringLiteral("COM10"), QStringLiteral("COM0"), QStringLiteral("CONIN"), QStringLiteral("CON$"),
                                    QStringLiteral("CONSOLE"), QStringLiteral("NULL.backup"), QStringLiteral("COM¹0")}) {
            QVERIFY2(folderNameProblem(name).isEmpty(), qPrintable(name));
            QCOMPARE(sanitizeImageName(name), name);
        }
    }

    void nameTemplatesMatchLegacy()
    {
        // Legacy Build tab _gen_name (#60, #43): presets, template tokens,
        // bracket clean-up, firmware, Unicode titles and the folder fallback.
        ImageNameFields f;
        f.titleId = QStringLiteral("PPSA01342");
        f.title = QStringLiteral("Demon's Souls");
        f.version = QStringLiteral("01.000.000");
        f.firmware = QStringLiteral("2");
        f.sdk = QStringLiteral("2.00");
        f.type = QStringLiteral("exFAT");
        f.folderName = QStringLiteral("Demons Souls dump");
        f.date = QStringLiteral("2026-11-08");
        QCOMPARE(renderImageName(ImageNamePreset::TitleIdTitleVersion, {}, f), QStringLiteral("PPSA01342 Demon's Souls (01.000.000)"));
        QCOMPARE(renderImageName(ImageNamePreset::TitleIdTitleVersionFirmware, {}, f),
                 QStringLiteral("PPSA01342 Demon's Souls (01.000.000) (FW2)")); // the v4.0.2 name in #76
        QCOMPARE(renderImageName(ImageNamePreset::TitleIdTitle, {}, f), QStringLiteral("PPSA01342 Demon's Souls"));
        QCOMPARE(renderImageName(ImageNamePreset::TitleId, {}, f), QStringLiteral("PPSA01342"));
        QCOMPARE(renderImageName(ImageNamePreset::Folder, {}, f), QStringLiteral("Demons Souls dump"));
        // Every token, case-insensitive; unknown tokens stay as typed.
        QCOMPARE(renderImageName(ImageNamePreset::Custom, kDefaultImageNameTemplate, f),
                 QStringLiteral("Demon's Souls (01.000.000) (FW2) (PPSA01342)"));
        QCOMPARE(renderImageName(ImageNamePreset::Custom, QStringLiteral("%ppsa%_%Type%_%date%_SDK %sdk% %folder% %NOPE%"), f),
                 QStringLiteral("PPSA01342_exFAT_2026-11-08_SDK 2.00 Demons Souls dump %NOPE%"));
        // Missing values: empty brackets go, spaces inside brackets are trimmed, ends are stripped.
        ImageNameFields bare = f;
        bare.firmware.clear();
        bare.version.clear();
        QCOMPARE(renderImageName(ImageNamePreset::Custom, kDefaultImageNameTemplate, bare), QStringLiteral("Demon's Souls (PPSA01342)"));
        QCOMPARE(renderImageName(ImageNamePreset::Custom, QStringLiteral("- [ %VERSION% ] %TITLE% ( %PPSA% ) {%FIRMWARE%} _"), bare),
                 QStringLiteral("Demon's Souls (PPSA01342)"));
        QCOMPARE(renderImageName(ImageNamePreset::Custom, QStringLiteral("((%FIRMWARE%)) %TITLE%"), bare), QStringLiteral("Demon's Souls"));
        QCOMPARE(renderImageName(ImageNamePreset::TitleIdTitleVersionFirmware, {}, bare), QStringLiteral("PPSA01342 Demon's Souls"));
        // Unicode survives; only characters Windows forbids are removed (no more "Y_tei").
        ImageNameFields yotei;
        yotei.titleId = QStringLiteral("PPSA26344");
        yotei.title = QStringLiteral("Ghost of Yōtei: Director’s Cut");
        yotei.version = QStringLiteral("01.512.000");
        QCOMPARE(renderImageName(ImageNamePreset::TitleIdTitleVersion, {}, yotei),
                 QStringLiteral("PPSA26344 Ghost of Yōtei Director’s Cut (01.512.000)"));
        ImageNameFields jp;
        jp.titleId = QStringLiteral("PPSA00001");
        jp.title = QStringLiteral("ファイナルファンタジー 🎮 <Remake>?");
        QCOMPARE(renderImageName(ImageNamePreset::TitleIdTitle, {}, jp), QStringLiteral("PPSA00001 ファイナルファンタジー 🎮 Remake"));
        // Fallbacks: the folder name, then "game"; reserved names and the length cap.
        ImageNameFields none;
        none.folderName = QStringLiteral("My Dump.");
        QCOMPARE(renderImageName(ImageNamePreset::TitleIdTitleVersion, {}, none), QStringLiteral("My Dump"));
        QCOMPARE(renderImageName(ImageNamePreset::Custom, QStringLiteral("(%VERSION%)"), none), QStringLiteral("My Dump"));
        QCOMPARE(renderImageName(ImageNamePreset::TitleId, {}, {}), QStringLiteral("game"));
        QCOMPARE(sanitizeImageName(QStringLiteral("con")), QStringLiteral("con_"));
        QCOMPARE(sanitizeImageName(QStringLiteral(" a\tb\u0001c | d ")), QStringLiteral("a bc d"));
        const QString longName = sanitizeImageName(QString(100, u'x') + QStringLiteral("🎮").repeated(80));
        QVERIFY(longName.size() <= 300);
        QCOMPARE(longName.left(100), QString(100, u'x'));
        QVERIFY(!longName.back().isHighSurrogate());
        QVERIFY(folderNameProblem(renderImageName(ImageNamePreset::TitleIdTitleVersion, {}, yotei)).isEmpty());
        // Preset keys round-trip (legacy name_preset values).
        for (auto p : {ImageNamePreset::TitleId, ImageNamePreset::TitleIdTitle, ImageNamePreset::TitleIdTitleVersion,
                       ImageNamePreset::TitleIdTitleVersionFirmware, ImageNamePreset::Folder, ImageNamePreset::Custom})
            QCOMPARE(*imageNamePresetFromKey(imageNamePresetKey(p)), p);
        QVERIFY(!imageNamePresetFromKey(QStringLiteral("nope")));
        QCOMPARE(firmwareMajor(QStringLiteral("4.50")), QStringLiteral("4"));
        QCOMPARE(firmwareMajor(QStringLiteral("10.00")), QStringLiteral("10"));
        QVERIFY(firmwareMajor(QStringLiteral("x")).isEmpty());
    }

    // ---- param.sfo ------------------------------------------------------------------
    void sfoParsesStringsAndIntegers()
    {
        const QByteArray sfo = buildSfo({sfoString("APP_VER", QStringLiteral("01.05")),
                                         sfoString("CATEGORY", QStringLiteral("gd")),
                                         sfoString("TITLE", QString::fromUtf8("\u30a2\u30b9\u30c8\u30ed")),
                                         sfoString("TITLE_ID", QStringLiteral("CUSA01234"), 12),
                                         sfoInt("SYSTEM_VER", 0x05050000)});
        auto r = parseSfo(sfo);
        QVERIFY(r.ok());
        QCOMPARE(int(r->entries.size()), 5);
        QCOMPARE(r->string(QStringLiteral("TITLE")), QString::fromUtf8("\u30a2\u30b9\u30c8\u30ed"));
        QCOMPARE(r->string(QStringLiteral("TITLE_ID")), QStringLiteral("CUSA01234"));
        QCOMPARE(*r->integer(QStringLiteral("SYSTEM_VER")), quint32(0x05050000));
        QVERIFY(!r->integer(QStringLiteral("TITLE")));
        QVERIFY(r->string(QStringLiteral("MISSING")).isEmpty());
    }

    void sfoRejectsOrSkipsDamage()
    {
        QVERIFY(!parseSfo(QByteArray(10, 'x')).ok());
        QByteArray bad = buildSfo({sfoString("TITLE", QStringLiteral("x"))});
        bad[1] = 'X';
        QVERIFY(!parseSfo(bad).ok());
        // An entry whose value lies outside the file is skipped, the rest is kept.
        QByteArray sfo = buildSfo({sfoString("TITLE", QStringLiteral("Good")), sfoString("TITLE_ID", QStringLiteral("CUSA00001"))});
        qToLittleEndian<quint32>(0x00FFFFFF, sfo.data() + 20 + 16 + 12); // second entry's data offset
        auto r = parseSfo(sfo);
        QVERIFY(r.ok());
        QCOMPARE(r->skippedEntries, 1);
        QCOMPARE(r->string(QStringLiteral("TITLE")), QStringLiteral("Good"));
        // Truncated entry table: parse what is there.
        QByteArray cut = buildSfo({sfoString("TITLE", QStringLiteral("T"))});
        qToLittleEndian<quint32>(50, cut.data() + 16);
        QVERIFY(parseSfo(cut).ok());
    }

    // ---- param.json ---------------------------------------------------------------
    void paramJsonFieldsAndTitleOrder()
    {
        auto r = parseParamJson(ps5Param(QStringLiteral("PPSA01325"), QString::fromUtf8("\u30a2\u30b9\u30c8\u30ed\u30dc\u30c3\u30c8"),
                                         QStringLiteral("ja-JP")));
        QVERIFY2(r.ok(), r.ok() ? "" : qPrintable(r.error().toLogString()));
        QCOMPARE(r->titleId, QStringLiteral("PPSA01325"));
        QCOMPARE(r->contentId, QStringLiteral("UP9000-PPSA01325_00-ABCDEFGHIJKLMNOP"));
        QCOMPARE(r->title, QString::fromUtf8("\u30a2\u30b9\u30c8\u30ed\u30dc\u30c3\u30c8"));
        QCOMPARE(r->version, QStringLiteral("01.02")); // legacy: version or masterVersion
        QCOMPARE(r->contentVersion, QStringLiteral("01.002.000"));
        QCOMPARE(r->sdkVersion.toString(), QStringLiteral("4.50"));
        QCOMPARE(r->requiredSystemVersion.toString(), QStringLiteral("7.00"));
        QCOMPARE(*r->applicationCategoryType, 0);
        QCOMPARE(*r->attribute, 4);
        QCOMPARE(r->applicationDrmType, QStringLiteral("standard"));
        QCOMPARE(r->versionFileUri, QStringLiteral("http://example.invalid/version.xml"));
        QVERIFY(!r->repaired);
        QCOMPARE(r->raw.value(QStringLiteral("attribute")).toInt(), 4);

        // default language without a title -> en-US -> en-GB -> first in FILE order.
        auto order = parseParamJson(R"({"titleId":"PPSA00001","localizedParameters":{
            "defaultLanguage":"ko-KR","ko-KR":{},"fr-FR":{"titleName":"Le Jeu"},"de-DE":{"titleName":"Das Spiel"}}})");
        QVERIFY(order.ok());
        QCOMPARE(order->title, QStringLiteral("Le Jeu"));
        QCOMPARE(order->localizedTitles.size(), 3);
        QCOMPARE(order->localizedTitles.at(1).first, QStringLiteral("fr-FR"));
        auto gb = parseParamJson(R"({"localizedParameters":{"defaultLanguage":"ko-KR","en-GB":{"titleName":"Colour"},"de-DE":{"titleName":"x"}}})");
        QVERIFY(gb.ok());
        QCOMPARE(gb->title, QStringLiteral("Colour"));
        auto top = parseParamJson(R"({"titleName":"  Top Level  ","titleId":12345})");
        QVERIFY(top.ok());
        QCOMPARE(top->title, QStringLiteral("Top Level"));
        QCOMPARE(top->titleId, QStringLiteral("12345"));
        auto name = parseParamJson(R"({"name":"By Name"})");
        QVERIFY(name.ok());
        QCOMPARE(name->title, QStringLiteral("By Name"));
    }

    void paramJsonToleratesMalformedText()
    {
        // Built with an explicit length: a QByteArray from a literal stops at the first NUL.
        static const char messyText[] = "\xEF\xBB\xBF{\n // dumped by a tool\n \"titleId\": \"PPSA02000\", /* note */\n"
                                        " \"localizedParameters\": {\"en-US\": {\"titleName\": \"Tr\xC3\xA4iling\",},},\n}\n\0\0\0";
        auto r = parseParamJson(QByteArray(messyText, int(sizeof(messyText) - 1)));
        QVERIFY2(r.ok(), r.ok() ? "" : qPrintable(r.error().toLogString()));
        QVERIFY(r->repaired);
        QCOMPARE(r->titleId, QStringLiteral("PPSA02000"));
        QCOMPARE(r->title, QString::fromUtf8("Tr\u00e4iling"));
        // Invalid UTF-8 is replaced, not fatal.
        auto u = parseParamJson("{\"titleName\":\"bad \xFF byte\"}");
        QVERIFY(u.ok());
        QVERIFY(u->title.contains(QChar(0xFFFD)));
        // UTF-16 with BOM.
        QByteArray utf16("\xFF\xFE", 2);
        const QString t = QStringLiteral("{\"titleId\":\"PPSA03000\"}");
        utf16.append(reinterpret_cast<const char*>(t.utf16()), t.size() * 2);
        QCOMPARE(parseParamJson(utf16)->titleId, QStringLiteral("PPSA03000"));
        // Garbage is an error.
        QVERIFY(!parseParamJson("not json at all").ok());
        QVERIFY(!parseParamJson("[1,2,3]").ok());
    }

    void jsonTextKeepsOrderAndBytes()
    {
        const QByteArray text = "{\"b\": 1, \"a\": {\"z\": \"x\\\"y\", \"y\": [1, {\"q\": 2}]}, \"big\": 12345678901234567890}";
        auto top = json::topLevelMembers(text);
        QVERIFY(top.ok());
        QCOMPARE(top->size(), std::size_t(3));
        QCOMPARE(top->at(0).key, QStringLiteral("b"));
        QCOMPARE(top->at(2).valueType, 'n');
        auto inner = json::membersAt(text, {QStringLiteral("a")});
        QVERIFY(inner.ok());
        QCOMPARE(inner->at(0).key, QStringLiteral("z"));
        QCOMPARE(json::stringValue(text, inner->at(0)), QStringLiteral("x\"y"));
        const QByteArray edited = json::replaceValues(text, {{top->at(0), json::quote(QStringLiteral("é"))}});
        QCOMPARE(edited, QByteArray("{\"b\": \"\xC3\xA9\", \"a\": {\"z\": \"x\\\"y\", \"y\": [1, {\"q\": 2}]}, \"big\": 12345678901234567890}"));
        QVERIFY(!json::topLevelMembers("{\"a\": }").ok());
    }

    // ---- the metadata chain -----------------------------------------------------------
    void chainPrefersSfoThenPfsVersionThenParamJsonThenNptitle()
    {
        // SFO title + ID win over param.json; pfs-version.dat overrides the version.
        const QString a = fresh(QStringLiteral("chainA"));
        QVERIFY(fwtest::writeRandomFile(QDir(a).filePath(QStringLiteral("eboot.bin")), 64, 1));
        QVERIFY(write(QDir(a).filePath(QStringLiteral("sce_sys/param.sfo")),
                      buildSfo({sfoString("TITLE", QStringLiteral("From SFO")), sfoString("TITLE_ID", QStringLiteral("CUSA11111")),
                                sfoString("APP_VER", QStringLiteral("01.00")), sfoInt("SYSTEM_VER", 0x04508001)})));
        QVERIFY(write(QDir(a).filePath(QStringLiteral("sce_sys/param.json")), ps5Param(QStringLiteral("PPSA22222"), QStringLiteral("From JSON"))));
        QVERIFY(write(QDir(a).filePath(QStringLiteral("sce_sys/pfs-version.dat")), "1.007.000\nignored\n"));
        auto m = readGameMetadata(a);
        QVERIFY(m.ok());
        QCOMPARE(m->title, QStringLiteral("From SFO"));
        QCOMPARE(m->titleId, QStringLiteral("CUSA11111"));
        QCOMPARE(m->version, QStringLiteral("01.007.000"));
        QCOMPARE(m->titleSource, QStringLiteral("param.sfo"));
        QCOMPARE(m->platform, Platform::PS5);          // a param.json means PS5
        QCOMPARE(m->sdkVersion.toString(), QStringLiteral("4.50")); // param.json sdkVersion first
        QCOMPARE(m->contentId, QStringLiteral("UP9000-PPSA22222_00-ABCDEFGHIJKLMNOP"));

        // pfs-version.dat that does not start with a digit is ignored.
        QVERIFY(write(QDir(a).filePath(QStringLiteral("sce_sys/pfs-version.dat")), "v2\n"));
        QCOMPARE(readGameMetadata(a)->version, QStringLiteral("01.000.000"));

        // No SFO: param.json gives title, ID and version (masterVersion).
        const QString b = fresh(QStringLiteral("chainB"));
        QVERIFY(makePs5Dump(b, QStringLiteral("PPSA33333"), QString::fromUtf8("Caf\u00e9 \u2013 Deluxe\u2122")));
        auto mb = readGameMetadata(b);
        QVERIFY(mb.ok());
        QCOMPARE(mb->title, QString::fromUtf8("Caf\u00e9 - Deluxe"));
        QCOMPARE(mb->rawTitle, QString::fromUtf8("Caf\u00e9 \u2013 Deluxe\u2122"));
        QCOMPARE(mb->titleId, QStringLiteral("PPSA33333"));
        QCOMPARE(mb->version, QStringLiteral("01.002.000")); // legacy zfill: "01.02" -> "01.002.000"
        QCOMPARE(mb->titleSource, QStringLiteral("param.json"));
        QCOMPARE(mb->kind, ContentKind::Application);
        QCOMPARE(mb->requiredSystemVersion.toString(), QStringLiteral("7.00"));

        // SFO with an ID but no title: param.json is NOT consulted (legacy), nptitle.dat gives the title.
        const QString c = fresh(QStringLiteral("chainC"));
        QVERIFY(write(QDir(c).filePath(QStringLiteral("sce_sys/param.sfo")),
                      buildSfo({sfoString("TITLE_ID", QStringLiteral("CUSA44444")), sfoString("CATEGORY", QStringLiteral("gp"))})));
        QVERIFY(write(QDir(c).filePath(QStringLiteral("sce_sys/param.json")), ps5Param(QStringLiteral("PPSA55555"), QStringLiteral("Ignored"))));
        QVERIFY(write(QDir(c).filePath(QStringLiteral("sce_sys/nptitle.dat")), "Np Title\r\nsecond\n"));
        auto mc = readGameMetadata(c);
        QVERIFY(mc.ok());
        QCOMPARE(mc->titleId, QStringLiteral("CUSA44444"));
        QCOMPARE(mc->title, QStringLiteral("Np Title"));
        QCOMPARE(mc->titleSource, QStringLiteral("nptitle.dat"));

        // Binary nptitle.dat is not used as a title.
        const QString d = fresh(QStringLiteral("chainD"));
        QVERIFY(write(QDir(d).filePath(QStringLiteral("sce_sys/param.sfo")), buildSfo({sfoString("TITLE_ID", QStringLiteral("CUSA66666")),
                                                                                         sfoString("CATEGORY", QStringLiteral("ac"))})));
        QVERIFY(write(QDir(d).filePath(QStringLiteral("sce_sys/nptitle.dat")), QByteArray("NPTD\0\x01\x02", 7)));
        auto md = readGameMetadata(d);
        QVERIFY(md.ok());
        QVERIFY(md->title.isEmpty());
        QCOMPARE(md->platform, Platform::PS4);
        QCOMPARE(md->kind, ContentKind::AddOn);
        QVERIFY(!md->notes.isEmpty());

        // TITLE_ID stored as an integer is ignored (legacy).
        const QString e = fresh(QStringLiteral("chainE"));
        QVERIFY(write(QDir(e).filePath(QStringLiteral("param.sfo")), buildSfo({sfoString("TITLE", QStringLiteral("T")), sfoInt("TITLE_ID", 5)})));
        QVERIFY(readGameMetadata(e)->titleId.isEmpty());
        QCOMPARE(readGameMetadata(e)->title, QStringLiteral("T"));
    }

    void metaFilesAreFoundDeepAndCaseInsensitively()
    {
        const QString r = fresh(QStringLiteral("deep"));
        QVERIFY(write(QDir(r).filePath(QStringLiteral("a/b/c/d/PARAM.JSON")), ps5Param(QStringLiteral("PPSA77777"), QStringLiteral("Deep"))));
        QCOMPARE(QFileInfo(findMetaFile(r, {QStringLiteral("param.json")})).fileName(), QStringLiteral("PARAM.JSON"));
        QVERIFY(write(QDir(r).filePath(QStringLiteral("1/2/3/4/5/6/param.sfo")), "x"));
        QVERIFY(findMetaFile(r, {QStringLiteral("param.sfo")}).isEmpty()); // deeper than 5 levels
        QCOMPARE(readGameMetadata(r)->titleId, QStringLiteral("PPSA77777"));
        QVERIFY(!readGameMetadata(m_root.filePath(QStringLiteral("missing"))).ok());
    }

    void malformedFilesBecomeNotesNotFailures()
    {
        const QString r = fresh(QStringLiteral("broken"));
        QVERIFY(write(QDir(r).filePath(QStringLiteral("eboot.bin")), "x"));
        QVERIFY(write(QDir(r).filePath(QStringLiteral("sce_sys/param.sfo")), "garbage garbage garbage"));
        QVERIFY(write(QDir(r).filePath(QStringLiteral("sce_sys/param.json")), "{ \"titleId\": "));
        auto m = readGameMetadata(r);
        QVERIFY(m.ok());
        QVERIFY(m->titleId.isEmpty());
        QCOMPARE(m->notes.size(), 2);
        QCOMPARE(m->kind, ContentKind::Application);
        QCOMPARE(m->platform, Platform::PS5); // a damaged param.json still means PS5
        const QJsonObject j = m->toJson();
        QCOMPARE(j.value(QStringLiteral("notes")).toArray().size(), 2);
    }

    void platformAndKindDetection()
    {
        const QString addon = fresh(QStringLiteral("addon"));
        QVERIFY(write(QDir(addon).filePath(QStringLiteral("sce_sys/param.json")), ps5Param(QStringLiteral("PPSA01325"), QStringLiteral("DLC"))));
        auto a = readGameMetadata(addon);
        QVERIFY(a.ok());
        QCOMPARE(a->platform, Platform::PS5);
        QCOMPARE(a->kind, ContentKind::AddOn);

        const QString app = fresh(QStringLiteral("ps5selfonly"));
        QVERIFY(write(QDir(app).filePath(QStringLiteral("eboot.bin")), QByteArray("\x54\x14\xF5\xEE....", 8)));
        auto s = readGameMetadata(app);
        QVERIFY(s.ok());
        QCOMPARE(s->platform, Platform::PS5);
        QCOMPARE(s->kind, ContentKind::Application);

        const QString patch = fresh(QStringLiteral("ps4patch"));
        QVERIFY(write(QDir(patch).filePath(QStringLiteral("eboot.bin")), "x"));
        QVERIFY(write(QDir(patch).filePath(QStringLiteral("sce_sys/param.sfo")),
                      buildSfo({sfoString("CATEGORY", QStringLiteral("gp")), sfoString("TITLE_ID", QStringLiteral("CUSA00002")),
                                sfoInt("SYSTEM_VER", 0x09000000)})));
        auto p = readGameMetadata(patch);
        QVERIFY(p.ok());
        QCOMPARE(p->platform, Platform::PS4);
        QCOMPARE(p->kind, ContentKind::Patch);
        QCOMPARE(p->requiredSystemVersion.toString(), QStringLiteral("9.00"));

        const QString other = fresh(QStringLiteral("cat"));
        QVERIFY(write(QDir(other).filePath(QStringLiteral("eboot.bin")), "x"));
        QVERIFY(write(QDir(other).filePath(QStringLiteral("sce_sys/param.json")),
                      R"({"titleId":"PPSA09999","applicationCategoryType":65536})"));
        QCOMPARE(readGameMetadata(other)->kind, ContentKind::Unknown);
    }

    // ---- roots, sizes, covers --------------------------------------------------------
    void gameRootSearch()
    {
        const QString r = fresh(QStringLiteral("roots"));
        QVERIFY(write(QDir(r).filePath(QStringLiteral("wrap/inner/eboot.bin")), "x"));
        QVERIFY(write(QDir(r).filePath(QStringLiteral("wrap/inner/deeper/eboot.bin")), "x")); // not descended into
        QVERIFY(write(QDir(r).filePath(QStringLiteral("wrap/sce_sys/eboot.bin")), "x"));      // skipped folder
        QVERIFY(write(QDir(r).filePath(QStringLiteral("wrap/.hidden/eboot.bin")), "x"));      // dot folder
        auto one = findGameRoots(r);
        QCOMPARE(one.candidates.size(), 1);
        QCOMPARE(QFileInfo(one.root).fileName(), QStringLiteral("inner"));
        QVERIFY(write(QDir(r).filePath(QStringLiteral("other/eboot.bin")), "x"));
        auto two = findGameRoots(r);
        QCOMPARE(two.candidates.size(), 2);
        QVERIFY(two.root.isEmpty());
        auto self = findGameRoots(QDir(r).filePath(QStringLiteral("other")));
        QCOMPARE(QFileInfo(self.root).fileName(), QStringLiteral("other"));
        auto capped = findGameRoots(r, {}, 1);
        QVERIFY(capped.depthCapped);
        QCOMPARE(capped.candidates.size(), 1); // only "other" at depth 1
        auto limited = findGameRoots(r, {}, 5, 2);
        QVERIFY(limited.truncated);

        QVERIFY(write(QDir(r).filePath(QStringLiteral("lw/inner/eboot.bin")), "x"));
        QCOMPARE(QFileInfo(findLibraryGameRoot(QDir(r).filePath(QStringLiteral("lw")))).fileName(), QStringLiteral("inner"));
        QVERIFY(write(QDir(r).filePath(QStringLiteral("g/a/b/eboot.bin")), "x"));
        QCOMPARE(QFileInfo(findLibraryGameRoot(QDir(r).filePath(QStringLiteral("g")))).fileName(), QStringLiteral("b"));
        QVERIFY(write(QDir(r).filePath(QStringLiteral("toodeep/a/b/c/eboot.bin")), "x"));
        QVERIFY(findLibraryGameRoot(QDir(r).filePath(QStringLiteral("toodeep"))).isEmpty());

        QDir().mkpath(QDir(r).filePath(QStringLiteral("PPSA01234-noeboot")));
        QVERIFY(!findRenameGameRoot(QDir(r).filePath(QStringLiteral("PPSA01234-noeboot"))).isEmpty());
        QDir().mkpath(QDir(r).filePath(QStringLiteral("random")));
        QVERIFY(findRenameGameRoot(QDir(r).filePath(QStringLiteral("random"))).isEmpty());
    }

    void dumpSizeSumsFilesWithoutFollowingLinks()
    {
        const QString r = fresh(QStringLiteral("size"));
        QVERIFY(fwtest::writeRandomFile(QDir(r).filePath(QStringLiteral("a.bin")), 1000, 1));
        QVERIFY(fwtest::writeRandomFile(QDir(r).filePath(QStringLiteral("sub/b.bin")), 2345, 2));
        QVERIFY(fwtest::writeRandomFile(QDir(r).filePath(QStringLiteral("sub/deeper/.c")), 5, 3));
        QCOMPARE(dumpSize(r), qint64(3350));
        // A link to a folder elsewhere and a link back to the dump itself (junctions
        // on Windows): neither is followed. The links are removed again, so that
        // nothing is ever deleted through them.
        const QString outside = fresh(QStringLiteral("outside"));
        QVERIFY(fwtest::writeRandomFile(QDir(outside).filePath(QStringLiteral("big.bin")), 50000, 4));
        JunctionGuard away(QDir(r).filePath(QStringLiteral("link"))), loop(QDir(r).filePath(QStringLiteral("sub/back")));
        const Status linked = createDirectoryJunction(away.path(), outside);
        if (linked.ok()) {
            QVERIFY(createDirectoryJunction(loop.path(), r).ok());
            QCOMPARE(dumpSize(r), qint64(3350));
        }
        away.release();
        loop.release();
        CancellationSource cs;
        cs.cancel();
        for (int i = 0; i < 600; ++i)
            QVERIFY(write(QDir(r).filePath(QStringLiteral("many/%1").arg(i)), "y"));
        QCOMPARE(dumpSize(r, cs.token()), qint64(-1));
        if (!linked.ok())
            QSKIP(qPrintable(QStringLiteral("No folder link can be made in the temporary folder (%1): links were not checked.")
                                 .arg(linked.error().message)));
    }

    void coverIsTrimmedFilledAndCached()
    {
        const QString r = fresh(QStringLiteral("cover"));
        // 512x512: a 64 px white frame around a red square with a blue top half.
        QImage img(512, 512, QImage::Format_RGB32);
        img.fill(Qt::white);
        {
            QPainter pt(&img);
            pt.fillRect(64, 64, 384, 384, QColor(220, 0, 0));
            pt.fillRect(64, 64, 384, 192, QColor(0, 0, 220));
        }
        const QString src = QDir(r).filePath(QStringLiteral("sce_sys/icon0.png"));
        QDir().mkpath(QFileInfo(src).absolutePath());
        QVERIFY(img.save(src, "PNG"));
        QCOMPARE(findCoverSource(r), src);

        const QString cache = fresh(QStringLiteral("cache"));
        auto out = normalizedCover(src, cache);
        QVERIFY2(out.ok(), out.ok() ? "" : qPrintable(out.error().toLogString()));
        QImage n(out.value());
        QCOMPARE(n.size(), QSize(kCoverWidth, kCoverHeight));
        // The white frame was trimmed: edges are red/blue, not white.
        QCOMPARE(QColor(n.pixel(2, 2)).blue() > 150, true);
        QCOMPARE(QColor(n.pixel(kCoverWidth - 3, kCoverHeight - 3)).red() > 150, true);
        QVERIFY(QColor(n.pixel(2, kCoverHeight / 2 + 20)).green() < 60);
        // Cached: same path, file not rewritten. The cached file gets a time of
        // its own first: a rewrite within one tick of the clock would not show.
        {
            QFile cached(out.value());
            QVERIFY(cached.open(QIODevice::ReadWrite));
            QVERIFY(cached.setFileTime(QDateTime(QDate(2020, 2, 3), QTime(4, 5, 6)), QFileDevice::FileModificationTime));
        }
        const QDateTime t = QFileInfo(out.value()).lastModified();
        QVERIFY(t.date().year() == 2020);
        auto second = normalizedCover(src, cache);
        QVERIFY(second.ok());
        QCOMPARE(second.value(), out.value());
        QCOMPARE(QFileInfo(out.value()).lastModified(), t);
        QCOMPARE(QDir(cache).entryList(QDir::Files).size(), 1);

        // Transparent borders are trimmed before filling.
        QImage alpha(300, 300, QImage::Format_ARGB32);
        alpha.fill(Qt::transparent);
        {
            QPainter pt(&alpha);
            pt.fillRect(100, 50, 100, 200, QColor(0, 200, 0));
        }
        const QImage trimmed = trimCoverBorders(alpha);
        QCOMPARE(trimmed.size(), QSize(100, 200));
    }

    void coverRefusesUnsupportedAndHugeImages()
    {
        const QString r = fresh(QStringLiteral("badcover"));
        const QString txt = QDir(r).filePath(QStringLiteral("icon0.png"));
        QVERIFY(write(txt, "this is not an image"));
        QVERIFY(!normalizedCover(txt, r).ok());
        // A PNG header claiming 9000x9000 pixels is refused before decoding.
        QByteArray png("\x89PNG\r\n\x1a\n\0\0\0\x0DIHDR", 16);
        QByteArray dims(8, '\0');
        qToBigEndian<quint32>(9000, dims.data());
        qToBigEndian<quint32>(9000, dims.data() + 4);
        const QByteArray ihdr = QByteArray("IHDR") + dims + QByteArray("\x08\x02\x00\x00\x00", 5);
        QByteArray crc(4, '\0');
        qToBigEndian<quint32>(quint32(crc32(0, reinterpret_cast<const Bytef*>(ihdr.constData()), uInt(ihdr.size()))), crc.data());
        png += dims + QByteArray("\x08\x02\x00\x00\x00", 5) + crc;
        for (const char* type : {"IDAT", "IEND"}) { // empty chunks with valid CRCs
            QByteArray c(4, '\0');
            qToBigEndian<quint32>(quint32(crc32(0, reinterpret_cast<const Bytef*>(type), 4)), c.data());
            png += QByteArray(4, '\0') + QByteArray(type) + c;
        }
        const QString huge = QDir(r).filePath(QStringLiteral("huge.png"));
        QVERIFY(write(huge, png));
        auto h = normalizedCover(huge, r);
        QVERIFY(!h.ok());
        QCOMPARE(errorOf(h).code, ErrorCode::Unsupported);
        QVERIFY(!normalizedCover(QDir(r).filePath(QStringLiteral("none.png")), r).ok());
    }

    // ---- library scan ---------------------------------------------------------------
    void libraryScanFindsDedupesAndEnriches()
    {
        const QString lib1 = fresh(QStringLiteral("lib1"));
        const QString lib2 = fresh(QStringLiteral("lib2"));
        QVERIFY(makePs5Dump(QDir(lib1).filePath(QStringLiteral("Astro")), QStringLiteral("PPSA01325"), QStringLiteral("Astro Bot"), 5000, true));
        QVERIFY(makePs5Dump(QDir(lib1).filePath(QStringLiteral("Wrapper/PPSA02222-app")), QStringLiteral("PPSA02222"), QString::fromUtf8("\u00c9t\u00e9")));
        // Duplicate of Astro, smaller: the bigger one wins.
        QVERIFY(makePs5Dump(QDir(lib2).filePath(QStringLiteral("AstroCopy")), QStringLiteral("PPSA01325"), QStringLiteral("Astro Bot"), 100));
        // A PS4 dump with param.sfo only.
        const QString ps4 = QDir(lib2).filePath(QStringLiteral("PS4Game"));
        QVERIFY(write(QDir(ps4).filePath(QStringLiteral("eboot.bin")), "x"));
        QVERIFY(write(QDir(ps4).filePath(QStringLiteral("sce_sys/param.sfo")),
                      buildSfo({sfoString("TITLE", QStringLiteral("Bloodborne")), sfoString("TITLE_ID", QStringLiteral("CUSA00900")),
                                sfoString("CATEGORY", QStringLiteral("gd")), sfoInt("SYSTEM_VER", 0x02500000)})));
        // No metadata: the title ID comes from the folder name.
        QVERIFY(write(QDir(lib2).filePath(QStringLiteral("[X] ppsa04444 Something/eboot.bin")), "x"));
        // Not games / skipped.
        QVERIFY(write(QDir(lib1).filePath(QStringLiteral("NotAGame/readme.txt")), "x"));
        QVERIFY(write(QDir(lib1).filePath(QStringLiteral("$RECYCLE.BIN/eboot.bin")), "x"));
        QVERIFY(write(QDir(lib1).filePath(QStringLiteral(".git/eboot.bin")), "x"));

        const QString cache = fresh(QStringLiteral("libcache"));
        LibraryScanOptions opt;
        opt.coverCacheDir = cache;
        ProgressReporter progress;
        progress.setPhases({QStringLiteral("a"), QStringLiteral("b")});
        auto r = scanLibrary({lib1, lib2, m_root.filePath(QStringLiteral("nope"))}, opt, {}, &progress);
        QVERIFY(r.ok());
        const auto& games = r->games;
        QCOMPARE(int(games.size()), 4);
        QCOMPARE(r->missingFolders.size(), 1);
        QCOMPARE(r->foldersChecked, 6);
        // Sorted by title, case-insensitively.
        QCOMPARE(games[0].title, QStringLiteral("Astro Bot"));
        QCOMPARE(games[1].title, QStringLiteral("Bloodborne"));
        QCOMPARE(games[2].title, QStringLiteral("PPSA04444"));
        QCOMPARE(games[3].title, QString::fromUtf8("\u00c9t\u00e9"));
        QVERIFY(games[0].wrapper.endsWith(QStringLiteral("/Astro")));
        QVERIFY(games[0].sizeBytes > 5000);
        QVERIFY(!games[0].coverPath.isEmpty());
        QCOMPARE(QImage(games[0].coverPath).size(), QSize(260, 390));
        QCOMPARE(games[0].sdkVersion.toString(), QStringLiteral("4.50"));
        QCOMPARE(games[1].platform, Platform::PS4);
        QCOMPARE(games[1].sdkVersion.toString(), QStringLiteral("2.50"));
        QCOMPARE(games[2].titleId, QStringLiteral("PPSA04444"));
        QVERIFY(games[3].folder.endsWith(QStringLiteral("/PPSA02222-app")));

        // JSON round trip.
        const LibraryGame back = LibraryGame::fromJson(games[0].toJson());
        QCOMPARE(back.titleId, games[0].titleId);
        QCOMPARE(back.sdkVersion, games[0].sdkVersion);
        QCOMPARE(back.sizeBytes, games[0].sizeBytes);
        QCOMPARE(back.platform, Platform::PS5);

        CancellationSource cs;
        cs.cancel();
        QVERIFY(errorOf(scanLibrary({lib1}, opt, cs.token())).isCancelled());
    }

    void libraryScanJobUsesSettings()
    {
        Module mod;
        const QString lib = fresh(QStringLiteral("joblib"));
        QVERIFY(makePs5Dump(QDir(lib).filePath(QStringLiteral("G1")), QStringLiteral("PPSA05555"), QStringLiteral("Job Game"), 10, true));
        const QString cache = fresh(QStringLiteral("jobcache"));
        auto job = mod.jobs.create(jobkinds::kLibraryScan, {});
        QVERIFY(job.ok());
        // No folders anywhere: refused.
        QVERIFY(!job.value()->validate(mod.settings()).ok());
        const SettingsValues s = mod.settings({{keys::kLibraryFolders, QStringList{lib}}, {keys::kCoverCacheFolder, cache}});
        QVERIFY(job.value()->validate(s).ok());
        ProgressReporter progress;
        progress.setPhases(job.value()->phases());
        JobContext ctx = makeContext(progress, s, fresh(QStringLiteral("tmp")));
        auto out = job.value()->run(ctx);
        QVERIFY(out.ok());
        const QJsonArray games = out->details.value(QStringLiteral("games")).toArray();
        QCOMPARE(games.size(), 1);
        QCOMPARE(games[0].toObject().value(QStringLiteral("titleId")).toString(), QStringLiteral("PPSA05555"));
        QCOMPARE(out->outputs.size(), 1);
        QVERIFY(QFileInfo(out->outputs[0]).absolutePath() == QFileInfo(cache).absoluteFilePath());
        QCOMPARE(out->summary, QStringLiteral("1 game(s) found across 1 folder(s)"));
    }

    // ---- Dump Rename -------------------------------------------------------------------
    void renamePlanApplyNeverOverwritesAndUndoes()
    {
        const QString src = fresh(QStringLiteral("rename"));
        QVERIFY(makePs5Dump(QDir(src).filePath(QStringLiteral("dump1")), QStringLiteral("PPSA01111"), QStringLiteral("First: Game")));
        QVERIFY(makePs5Dump(QDir(src).filePath(QStringLiteral("dump2")), QStringLiteral("PPSA02222"), QStringLiteral("Second")));
        QVERIFY(makePs5Dump(QDir(src).filePath(QStringLiteral("dump3")), QStringLiteral("PPSA03333"), QStringLiteral("Third")));
        // Same game twice -> the second proposal is a duplicate.
        QVERIFY(makePs5Dump(QDir(src).filePath(QStringLiteral("dump4")), QStringLiteral("PPSA03333"), QStringLiteral("Third")));
        // Already named.
        QVERIFY(makePs5Dump(QDir(src).filePath(QStringLiteral("PPSA04444 Fourth (01.002.000)")), QStringLiteral("PPSA04444"), QStringLiteral("Fourth")));
        // Not a dump (no eboot, no ID-looking name): ignored.
        QDir().mkpath(QDir(src).filePath(QStringLiteral("photos")));
        // A dump whose name looks like an ID but has no metadata.
        QDir().mkpath(QDir(src).filePath(QStringLiteral("ppsa05555")));
        // Something (a file) already uses dump2's proposed name.
        QVERIFY(write(QDir(src).filePath(QStringLiteral("PPSA02222 Second (01.002.000)")), "occupied"));

        RenamePlanOptions opt;
        auto plan = planDumpRenames(src, opt, {});
        QVERIFY(plan.ok());
        // A name that is not in the list gives an item no check below accepts
        // (a default item would read as "Ready").
        auto byName = [&](const std::vector<RenameItem>& items, const QString& n) -> const RenameItem& {
            for (const auto& it : items)
                if (it.currentName == n)
                    return it;
            static const RenameItem none = [] {
                RenameItem missing;
                missing.status = RenameStatus::Failed;
                missing.proposedName = QStringLiteral("<not in the list>");
                missing.confidence = QStringLiteral("<not in the list>");
                return missing;
            }();
            return none;
        };
        QCOMPARE(byName(*plan, QStringLiteral("no such dump")).status, RenameStatus::Failed);
        QCOMPARE(int(plan->size()), 6); // dump1-4, PPSA04444..., ppsa05555
        QCOMPARE(byName(*plan, QStringLiteral("dump1")).proposedName, QStringLiteral("PPSA01111 First Game (01.002.000)"));
        QCOMPARE(byName(*plan, QStringLiteral("dump1")).status, RenameStatus::Ready);
        QCOMPARE(byName(*plan, QStringLiteral("dump1")).confidence, QStringLiteral("green"));
        QCOMPARE(byName(*plan, QStringLiteral("dump2")).status, RenameStatus::TargetExists);
        QCOMPARE(byName(*plan, QStringLiteral("dump3")).status, RenameStatus::Ready);
        QCOMPARE(byName(*plan, QStringLiteral("dump4")).status, RenameStatus::Duplicate);
        QCOMPARE(byName(*plan, QStringLiteral("PPSA04444 Fourth (01.002.000)")).status, RenameStatus::AlreadyNamed);
        QCOMPARE(byName(*plan, QStringLiteral("ppsa05555")).proposedName, QStringLiteral("PPSA05555"));
        QCOMPARE(byName(*plan, QStringLiteral("ppsa05555")).confidence, QStringLiteral("yellow"));
        // Only the case differs: where paths ignore case that is the folder itself, not a taken name.
#ifdef Q_OS_WIN
        QCOMPARE(byName(*plan, QStringLiteral("ppsa05555")).status, RenameStatus::AlreadyNamed);
#else
        QCOMPARE(byName(*plan, QStringLiteral("ppsa05555")).status, RenameStatus::Ready);
#endif
        // The plan changed nothing on disk.
        QVERIFY(QFileInfo(QDir(src).filePath(QStringLiteral("dump1"))).isDir());

        // Edit one name to something invalid and deselect another.
        std::vector<RenameItem> edited = plan.value();
        for (auto& it : edited) {
            if (it.currentName == u"dump3")
                it.proposedName = QStringLiteral("bad/name");
            if (it.currentName == u"ppsa05555")
                it.selected = false;
        }
        const QString log = QDir(fresh(QStringLiteral("undo"))).filePath(QStringLiteral("log.json"));
        auto run = applyDumpRenames(edited, {}, log, {});
        QVERIFY(run.ok());
        // dump3's invalid edit frees the name for dump4, which is no longer a duplicate.
        QCOMPARE(run->renamed, 2);
        QCOMPARE(byName(run->items, QStringLiteral("dump3")).status, RenameStatus::InvalidName);
        QCOMPARE(byName(run->items, QStringLiteral("dump4")).status, RenameStatus::Renamed);
        QCOMPARE(byName(run->items, QStringLiteral("ppsa05555")).status, RenameStatus::NotSelected);
        QVERIFY(QFileInfo(QDir(src).filePath(QStringLiteral("PPSA01111 First Game (01.002.000)/eboot.bin"))).isFile());
        QVERIFY(!QFileInfo::exists(QDir(src).filePath(QStringLiteral("dump1"))));
        QVERIFY(QFileInfo(QDir(src).filePath(QStringLiteral("dump2"))).isDir()); // never overwritten
        QCOMPARE(run->undoLogPath, log);

        // Undo puts it back; a second undo changes nothing.
        auto undo = undoDumpRenames(log, {});
        QVERIFY(undo.ok());
        QCOMPARE(undo->renamed, 2);
        QVERIFY(QFileInfo(QDir(src).filePath(QStringLiteral("dump1/eboot.bin"))).isFile());
        auto again = undoDumpRenames(log, {});
        QVERIFY(again.ok());
        QCOMPARE(again->renamed, 0);
        QCOMPARE(again->skipped, 2);

        // A target created after planning is still never overwritten.
        auto plan2 = planDumpRenames(src, opt, {});
        QVERIFY(plan2.ok());
        QDir().mkpath(QDir(src).filePath(QStringLiteral("PPSA01111 First Game (01.002.000)")));
        auto run2 = applyDumpRenames(plan2.value(), {}, QDir(fresh(QStringLiteral("undo2"))).filePath(QStringLiteral("l.json")), {});
        QVERIFY(run2.ok());
        QCOMPARE(byName(run2->items, QStringLiteral("dump1")).status, RenameStatus::TargetExists);
        QVERIFY(QFileInfo(QDir(src).filePath(QStringLiteral("dump1"))).isDir());
    }

    // A proposal that differs from the folder's name in case only. Where paths
    // ignore case (Windows) the folder counts as named already and keeps its
    // spelling; elsewhere it is an ordinary rename. Either way nothing is lost
    // and the result says what happened.
    void renameWithACaseOnlyDifference()
    {
        const QString src = fresh(QStringLiteral("rename-case"));
        QVERIFY(write(QDir(src).filePath(QStringLiteral("ppsa05555/eboot.bin")), "game"));
        const auto spelling = [&] { return QDir(src).entryList(QDir::Dirs | QDir::NoDotAndDotDot); };
        auto plan = planDumpRenames(src, RenamePlanOptions{}, {});
        QVERIFY(plan.ok());
        QCOMPARE(int(plan->size()), 1);
        QCOMPARE(plan->front().currentName, QStringLiteral("ppsa05555"));
        QCOMPARE(plan->front().proposedName, QStringLiteral("PPSA05555"));
        const QString log = QDir(fresh(QStringLiteral("rename-case-undo"))).filePath(QStringLiteral("log.json"));
        auto run = applyDumpRenames(plan.value(), {}, log, {});
        QVERIFY(run.ok());
        QCOMPARE(run->failed, 0);
        QCOMPARE(int(run->items.size()), 1);
#ifdef Q_OS_WIN
        QCOMPARE(plan->front().status, RenameStatus::AlreadyNamed);
        QCOMPARE(run->items.front().status, RenameStatus::AlreadyNamed);
        QCOMPARE(run->renamed, 0);
        QCOMPARE(spelling(), QStringList{QStringLiteral("ppsa05555")});
#else
        QCOMPARE(plan->front().status, RenameStatus::Ready);
        QCOMPARE(run->items.front().status, RenameStatus::Renamed);
        QCOMPARE(run->renamed, 1);
        QCOMPARE(spelling(), QStringList{QStringLiteral("PPSA05555")});
#endif
        QVERIFY(QFileInfo(QDir(src).filePath(spelling().value(0) + QStringLiteral("/eboot.bin"))).isFile());
    }

    void renameIntoDestinationAndPresets()
    {
        const QString src = fresh(QStringLiteral("rename-dst"));
        QVERIFY(makePs5Dump(QDir(src).filePath(QStringLiteral("x")), QStringLiteral("PPSA06666"), QStringLiteral("Sixth")));
        RenamePlanOptions opt;
        opt.preset = NamePreset::TitleId;
        opt.destinationRoot = QDir(src).filePath(QStringLiteral("sorted/new"));
        auto plan = planDumpRenames(src, opt, {});
        QVERIFY(plan.ok());
        QCOMPARE(plan->front().proposedName, QStringLiteral("PPSA06666"));
        auto run = applyDumpRenames(plan.value(), opt.destinationRoot, {}, {});
        QVERIFY(run.ok());
        QCOMPARE(run->renamed, 1);
        QVERIFY(QFileInfo(QDir(src).filePath(QStringLiteral("sorted/new/PPSA06666/eboot.bin"))).isFile());
        QVERIFY(run->undoLogPath.startsWith(defaultRenameUndoDir()));
        QVERIFY(QFileInfo(run->undoLogPath).isFile());
    }

    void renameJobPreviewApplyUndo()
    {
        Module mod;
        const QString src = fresh(QStringLiteral("rename-job"));
        QVERIFY(makePs5Dump(QDir(src).filePath(QStringLiteral("a")), QStringLiteral("PPSA07777"), QStringLiteral("Seventh")));
        const SettingsValues s = mod.settings({{keys::kRenamePreset, QStringLiteral("ppsa_title")}});
        ProgressReporter progress;

        auto preview = mod.jobs.create(jobkinds::kLibraryRename, {{QStringLiteral("mode"), QStringLiteral("preview")},
                                                                  {QStringLiteral("source"), src}});
        QVERIFY(preview.value()->validate(s).ok());
        JobContext ctx = makeContext(progress, s, fresh(QStringLiteral("t1")));
        auto pv = preview.value()->run(ctx);
        QVERIFY(pv.ok());
        const QJsonObject item = pv->details.value(QStringLiteral("items")).toArray().at(0).toObject();
        QCOMPARE(item.value(QStringLiteral("proposedName")).toString(), QStringLiteral("PPSA07777 Seventh")); // setting preset
        QVERIFY(QFileInfo(QDir(src).filePath(QStringLiteral("a"))).isDir());

        auto apply = mod.jobs.create(jobkinds::kLibraryRename,
                                     {{QStringLiteral("mode"), QStringLiteral("apply")},
                                      {QStringLiteral("source"), src},
                                      {QStringLiteral("preset"), QStringLiteral("ppsa")},
                                      {QStringLiteral("items"), QJsonArray{QJsonObject{{QStringLiteral("source"), QDir(src).filePath(QStringLiteral("a"))},
                                                                                       {QStringLiteral("name"), QStringLiteral("Custom Name.")}}}}});
        JobContext ctx2 = makeContext(progress, s, fresh(QStringLiteral("t2")));
        auto ap = apply.value()->run(ctx2);
        QVERIFY(ap.ok());
        QCOMPARE(ap->verification, Verification::Passed);
        QVERIFY(QFileInfo(QDir(src).filePath(QStringLiteral("Custom Name"))).isDir());
        const QString log = ap->details.value(QStringLiteral("undoLog")).toString();
        QVERIFY(QFileInfo(log).isFile());

        auto undo = mod.jobs.create(jobkinds::kLibraryRename, {{QStringLiteral("mode"), QStringLiteral("undo")},
                                                               {QStringLiteral("undoLog"), log}});
        QVERIFY(undo.value()->validate(s).ok());
        JobContext ctx3 = makeContext(progress, s, fresh(QStringLiteral("t3")));
        auto un = undo.value()->run(ctx3);
        QVERIFY(un.ok());
        QCOMPARE(un->verification, Verification::Passed);
        QVERIFY(QFileInfo(QDir(src).filePath(QStringLiteral("a"))).isDir());

        auto bad = mod.jobs.create(jobkinds::kLibraryRename, {{QStringLiteral("mode"), QStringLiteral("explode")}});
        QVERIFY(!bad.value()->validate(s).ok());
        auto badPreset = mod.jobs.create(jobkinds::kLibraryRename, {{QStringLiteral("source"), src}, {QStringLiteral("preset"), QStringLiteral("zzz")}});
        QVERIFY(!badPreset.value()->validate(s).ok());
    }

    // A partly reversed or cancelled undo is not "Passed", and still says what it did.
    void undoIsTruthfulWhenBlockedOrCancelled()
    {
        Module mod;
        const SettingsValues s = mod.settings();
        const QString src = fresh(QStringLiteral("undo-truth"));
        for (const char* n : {"a", "b", "c"})
            QVERIFY(makePs5Dump(QDir(src).filePath(QLatin1String(n)), QStringLiteral("PPSA1000%1").arg(int(n[0] - 'a')),
                        QStringLiteral("Game %1").arg(QLatin1String(n))));
        RenamePlanOptions opt;
        opt.preset = NamePreset::TitleId;
        const QString log = QDir(fresh(QStringLiteral("undo-truth-log"))).filePath(QStringLiteral("log.json"));
        auto run = applyDumpRenames(planDumpRenames(src, opt, {}).value(), {}, log, {});
        QVERIFY(run.ok());
        QCOMPARE(run->renamed, 3);
        QVERIFY(!run->cancelled);

        // Something new took the old name "b": that entry cannot be reversed.
        QVERIFY(QDir().mkpath(QDir(src).filePath(QStringLiteral("b"))));
        ProgressReporter progress;
        auto undo = mod.jobs.create(jobkinds::kLibraryRename,
                                    {{QStringLiteral("mode"), QStringLiteral("undo")}, {QStringLiteral("undoLog"), log}});
        JobContext ctx = makeContext(progress, s, fresh(QStringLiteral("ut1")));
        auto un = undo.value()->run(ctx);
        QVERIFY(un.ok());
        QCOMPARE(un->verification, Verification::Inconclusive);
        QVERIFY2(un->verificationDetail.contains(QStringLiteral("original name is taken")), qPrintable(un->verificationDetail));
        QVERIFY2(un->summary.contains(QStringLiteral("1 not restored")), qPrintable(un->summary));
        QCOMPARE(un->details.value(QStringLiteral("renamed")).toInt(), 2);
        QCOMPARE(QDir::fromNativeSeparators(un->details.value(QStringLiteral("undoLog")).toString()), log);
        QCOMPARE(int(un->warnings.size()), 1);
        QVERIFY(QFileInfo(QDir(src).filePath(QStringLiteral("PPSA10001/eboot.bin"))).isFile()); // untouched
        QVERIFY(QFileInfo(QDir(src).filePath(QStringLiteral("a/eboot.bin"))).isFile());

        // Cancel after the first restored name: the rest is listed, nothing claims success.
        const QString src2 = fresh(QStringLiteral("undo-cancel"));
        for (const char* n : {"a", "b", "c"})
            QVERIFY(makePs5Dump(QDir(src2).filePath(QLatin1String(n)), QStringLiteral("PPSA2000%1").arg(int(n[0] - 'a')),
                        QStringLiteral("Game %1").arg(QLatin1String(n))));
        const QString log2 = QDir(fresh(QStringLiteral("undo-cancel-log"))).filePath(QStringLiteral("log.json"));
        QVERIFY(applyDumpRenames(planDumpRenames(src2, opt, {}).value(), {}, log2, {}).ok());
        {
            CancellationSource cancel;
            ProgressReporter stopAfterOne([&](const ProgressSnapshot& p) {
                if (p.done >= 1)
                    cancel.cancel();
            }, 0);
            auto r = undoDumpRenames(log2, cancel.token(), &stopAfterOne);
            QVERIFY(r.ok());
            QVERIFY(r->cancelled);
            QCOMPARE(r->renamed, 1);
            QCOMPARE(int(r->items.size()), 3);
            QCOMPARE(r->items.at(1).status, RenameStatus::NotSelected);
            QVERIFY(!r->items.at(1).message.isEmpty());
            QCOMPARE(r->undoLogPath, log2);
        }
        {
            // Through the job: the entry restored above is passed over, one more
            // is restored, then the cancel; not "Passed".
            CancellationSource cancel;
            ProgressReporter stopAfterOne([&](const ProgressSnapshot& p) {
                if (p.done >= 2)
                    cancel.cancel();
            }, 0);
            auto job = mod.jobs.create(jobkinds::kLibraryRename,
                                       {{QStringLiteral("mode"), QStringLiteral("undo")}, {QStringLiteral("undoLog"), log2}});
            JobContext c = makeContext(stopAfterOne, s, fresh(QStringLiteral("ut2")));
            c.token = cancel.token();
            auto o = job.value()->run(c);
            QVERIFY(o.ok());
            QCOMPARE(o->verification, Verification::Inconclusive);
            QVERIFY2(o->summary.startsWith(QStringLiteral("Cancelled after restoring 1 ")), qPrintable(o->summary));
            QVERIFY(!o->verificationDetail.contains(QStringLiteral("every logged rename")));
        }
        {
            // Cancelled before anything was restored: plain Cancelled.
            CancellationSource cancel;
            cancel.cancel();
            auto r = undoDumpRenames(log2, cancel.token());
            QVERIFY(!r.ok());
            QVERIFY(errorOf(r).isCancelled());
        }
    }

    // The undo log learns of a rename before it happens. A crash right after
    // a rename (the log as it stood while that dump was renamed) still lets
    // Undo put the old name back; an entry whose rename never happened (a
    // crash just before it) is passed over.
    void aRenameIsLoggedBeforeItHappens()
    {
        const auto readLog = [](const QString& path) {
            QFile f(path);
            return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray("");
        };
        const auto writeLog = [](const QString& path, const QByteArray& bytes) {
            QFile f(path);
            return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(bytes) == bytes.size();
        };
        for (const bool crashAfter : {true, false}) {
            const QString src = fresh(crashAfter ? QStringLiteral("wal-after") : QStringLiteral("wal-before"));
            for (const char* n : {"a", "b"})
                QVERIFY(makePs5Dump(QDir(src).filePath(QLatin1String(n)), QStringLiteral("PPSA3000%1").arg(int(n[0] - 'a')),
                                    QStringLiteral("Game %1").arg(QLatin1String(n))));
            RenamePlanOptions opt;
            opt.preset = NamePreset::TitleId;
            const QString log = QDir(fresh(crashAfter ? QStringLiteral("wal-after-log") : QStringLiteral("wal-before-log")))
                                    .filePath(QStringLiteral("log.json"));
            // The log as it stands while each dump is renamed (what a crash then leaves).
            QHash<QString, QByteArray> whileRenaming;
            ProgressReporter watch([&](const ProgressSnapshot& p) {
                if (!p.currentItem.isEmpty() && !whileRenaming.contains(p.currentItem))
                    whileRenaming.insert(p.currentItem, readLog(log));
            }, 0);
            auto run = applyDumpRenames(planDumpRenames(src, opt, {}).value(), {}, log, {}, &watch);
            QVERIFY(run.ok());
            QCOMPARE(run->renamed, 2);
            QVERIFY(!readLog(log).contains("pending"));
            const QString crashAt = crashAfter ? QStringLiteral("a") : QStringLiteral("b");
            QVERIFY2(!whileRenaming.value(crashAt).isEmpty(), "nothing was logged before the rename");
            QVERIFY(writeLog(log, whileRenaming.value(crashAt)));
            // What the disk holds after either crash: "a" renamed, "b" not yet.
            QVERIFY(QDir().rename(QDir(src).filePath(QStringLiteral("PPSA30001")), QDir(src).filePath(QStringLiteral("b"))));
            auto undo = undoDumpRenames(log, {});
            QVERIFY2(undo.ok(), undo.ok() ? "" : qPrintable(undo.error().toLogString()));
            QCOMPARE(undo->renamed, 1);
            QCOMPARE(undo->failed, 0);
            QVERIFY(QFileInfo(QDir(src).filePath(QStringLiteral("a/eboot.bin"))).isFile());
            QVERIFY(QFileInfo(QDir(src).filePath(QStringLiteral("b/eboot.bin"))).isFile());
            QCOMPARE(QDir(src).entryList(QDir::Dirs | QDir::NoDotAndDotDot), (QStringList{QStringLiteral("a"), QStringLiteral("b")}));
        }
    }

    // A cancelled apply keeps its outcome (what was renamed, the undo log) instead of an error.
    void cancelledApplyKeepsOutcomeAndUndoLog()
    {
        Module mod;
        const SettingsValues s = mod.settings();
        const QString src = fresh(QStringLiteral("apply-cancel"));
        for (const char* n : {"a", "b", "c"})
            QVERIFY(makePs5Dump(QDir(src).filePath(QLatin1String(n)), QStringLiteral("PPSA3000%1").arg(int(n[0] - 'a')),
                        QStringLiteral("Game %1").arg(QLatin1String(n))));
        const QString log = QDir(fresh(QStringLiteral("apply-cancel-log"))).filePath(QStringLiteral("log.json"));
        CancellationSource cancel;
        ProgressReporter stopAfterOne([&](const ProgressSnapshot& p) {
            if (p.done >= 1)
                cancel.cancel();
        }, 0);
        auto job = mod.jobs.create(jobkinds::kLibraryRename, {{QStringLiteral("mode"), QStringLiteral("apply")},
                                                              {QStringLiteral("source"), src},
                                                              {QStringLiteral("preset"), QStringLiteral("ppsa")},
                                                              {QStringLiteral("undoLog"), log}});
        JobContext ctx = makeContext(stopAfterOne, s, fresh(QStringLiteral("ac1")));
        ctx.token = cancel.token();
        auto o = job.value()->run(ctx);
        QVERIFY2(o.ok(), o.ok() ? "" : qPrintable(o.error().toLogString()));
        QCOMPARE(o->verification, Verification::Inconclusive);
        QCOMPARE(o->details.value(QStringLiteral("renamed")).toInt(), 1);
        QVERIFY2(o->summary.startsWith(QStringLiteral("Cancelled after renaming 1 dump(s).")), qPrintable(o->summary));
        QCOMPARE(QDir::fromNativeSeparators(o->details.value(QStringLiteral("undoLog")).toString()), log);
        QVERIFY(QFileInfo(log).isFile());
        QCOMPARE(int(o->details.value(QStringLiteral("items")).toArray().size()), 3);
        QCOMPARE(int(o->warnings.size()), 2); // the two dumps the cancel left untouched
        QVERIFY(QFileInfo(QDir(src).filePath(QStringLiteral("PPSA30000"))).isDir());
        QVERIFY(QFileInfo(QDir(src).filePath(QStringLiteral("b"))).isDir());
        QVERIFY(QFileInfo(QDir(src).filePath(QStringLiteral("c"))).isDir());
    }

    // "apply" touches only the dumps its edits name, whatever the letter case of the path.
    void applyRenamesOnlyWhatTheCallerSent()
    {
        Module mod;
        const SettingsValues s = mod.settings();
        const QString src = fresh(QStringLiteral("apply-only"));
        QVERIFY(makePs5Dump(QDir(src).filePath(QStringLiteral("Seen")), QStringLiteral("PPSA40001"), QStringLiteral("Seen")));
        QVERIFY(makePs5Dump(QDir(src).filePath(QStringLiteral("Unticked")), QStringLiteral("PPSA40002"), QStringLiteral("Unticked")));
        // Copied in after the preview: no edit names it.
        QVERIFY(makePs5Dump(QDir(src).filePath(QStringLiteral("Late")), QStringLiteral("PPSA40003"), QStringLiteral("Late")));
        QString untickedPath = QDir::toNativeSeparators(QDir(src).filePath(QStringLiteral("Unticked")));
#ifdef Q_OS_WIN
        untickedPath = untickedPath.toUpper(); // another spelling of the same folder
#endif
        ProgressReporter progress;
        auto job = mod.jobs.create(
            jobkinds::kLibraryRename,
            {{QStringLiteral("mode"), QStringLiteral("apply")},
             {QStringLiteral("source"), src},
             {QStringLiteral("preset"), QStringLiteral("ppsa")},
             {QStringLiteral("undoLog"), QDir(fresh(QStringLiteral("apply-only-log"))).filePath(QStringLiteral("l.json"))},
             {QStringLiteral("items"),
              QJsonArray{QJsonObject{{QStringLiteral("source"), QDir(src).filePath(QStringLiteral("Seen"))},
                                     {QStringLiteral("selected"), true}},
                         QJsonObject{{QStringLiteral("source"), untickedPath}, {QStringLiteral("selected"), false}}}}});
        JobContext ctx = makeContext(progress, s, fresh(QStringLiteral("ao1")));
        auto o = job.value()->run(ctx);
        QVERIFY2(o.ok(), o.ok() ? "" : qPrintable(o.error().toLogString()));
        QCOMPARE(o->details.value(QStringLiteral("renamed")).toInt(), 1);
        QVERIFY(QFileInfo(QDir(src).filePath(QStringLiteral("PPSA40001/eboot.bin"))).isFile());
        QVERIFY(QFileInfo(QDir(src).filePath(QStringLiteral("Unticked/eboot.bin"))).isFile());
        QVERIFY(QFileInfo(QDir(src).filePath(QStringLiteral("Late/eboot.bin"))).isFile());
        QVERIFY(!QFileInfo::exists(QDir(src).filePath(QStringLiteral("PPSA40002"))));
        QVERIFY(!QFileInfo::exists(QDir(src).filePath(QStringLiteral("PPSA40003"))));
    }

    // One walk finds every metadata file the per-file walks found, and can be cancelled.
    void metadataFilesComeFromOneCancellableWalk()
    {
        const QString r = fresh(QStringLiteral("onewalk"));
        QVERIFY(write(QDir(r).filePath(QStringLiteral("eboot.bin")), "x"));
        // Nothing at the root or in sce_sys: all four are found deeper, in walk order.
        QVERIFY(write(QDir(r).filePath(QStringLiteral("b/deep/PARAM.SFO")),
                      buildSfo({sfoString("TITLE_ID", QStringLiteral("CUSA07777"))}))); // no title: nptitle.dat is used
        QVERIFY(write(QDir(r).filePath(QStringLiteral("a/pfs-version.dat")), "02.10\n"));
        QVERIFY(write(QDir(r).filePath(QStringLiteral("c/param.json")), ps5Param(QStringLiteral("PPSA01234"), QStringLiteral("Json Title"))));
        QVERIFY(write(QDir(r).filePath(QStringLiteral("a/z/nptitle.dat")), "Np Title\n"));
        QVERIFY(write(QDir(r).filePath(QStringLiteral("d/nptitle.dat")), "Later Title\n"));
        auto m = readGameMetadata(r);
        QVERIFY(m.ok());
        for (const char* name : {"param.sfo", "pfs-version.dat", "param.json", "nptitle.dat"})
            QCOMPARE(QStringList({m->paramSfoPath, m->pfsVersionPath, m->paramJsonPath, m->npTitlePath})
                         .filter(QString::fromLatin1(name), Qt::CaseInsensitive)
                         .value(0),
                     findMetaFile(r, {QString::fromLatin1(name)}));
        QCOMPARE(m->titleId, QStringLiteral("CUSA07777"));
        QCOMPARE(m->version, QStringLiteral("02.010.000"));
        QCOMPARE(m->title, QStringLiteral("Np Title"));
        QCOMPARE(m->titleSource, QStringLiteral("nptitle.dat"));

        // The walk stops early once the three main files are found; nptitle.dat is still found.
        const QString e = fresh(QStringLiteral("early"));
        QVERIFY(write(QDir(e).filePath(QStringLiteral("a/param.sfo")), buildSfo({sfoString("TITLE_ID", QStringLiteral("CUSA08888"))})));
        QVERIFY(write(QDir(e).filePath(QStringLiteral("a/pfs-version.dat")), "01.00\n"));
        QVERIFY(write(QDir(e).filePath(QStringLiteral("a/param.json")), "{}"));
        QVERIFY(write(QDir(e).filePath(QStringLiteral("z/nptitle.dat")), "Found After The Stop\n"));
        auto em = readGameMetadata(e);
        QVERIFY(em.ok());
        QCOMPARE(em->title, QStringLiteral("Found After The Stop"));

        // Cancelled while searching: Cancelled, not a half-read result.
        CancellationSource cancel;
        cancel.cancel();
        auto c = readGameMetadata(r, cancel.token());
        QVERIFY(!c.ok());
        QVERIFY(errorOf(c).isCancelled());
        // Files at the usual places need no walk, so there is nothing to cancel.
        const QString direct = fresh(QStringLiteral("direct"));
        QVERIFY(makePs5Dump(direct, QStringLiteral("PPSA05678"), QStringLiteral("Direct")));
        QVERIFY(write(QDir(direct).filePath(QStringLiteral("sce_sys/param.sfo")), buildSfo({sfoString("TITLE", QStringLiteral("T"))})));
        QVERIFY(write(QDir(direct).filePath(QStringLiteral("pfs-version.dat")), "01.00\n"));
        QVERIFY(readGameMetadata(direct, cancel.token()).ok());
    }

    // A param.json somewhere in a PS4 game's data does not make the dump PS5.
    void strayParamJsonDoesNotMakeAPs4DumpPs5()
    {
        const QString r = fresh(QStringLiteral("ps4-json"));
        QVERIFY(write(QDir(r).filePath(QStringLiteral("eboot.bin")), "x"));
        QVERIFY(write(QDir(r).filePath(QStringLiteral("sce_sys/param.sfo")),
                      buildSfo({sfoString("CATEGORY", QStringLiteral("gd")), sfoString("TITLE", QStringLiteral("Old Game")),
                                sfoString("TITLE_ID", QStringLiteral("PCAS00123"))})));
        QVERIFY(write(QDir(r).filePath(QStringLiteral("data/config/param.json")), "{\"quality\": 3}"));
        auto m = readGameMetadata(r);
        QVERIFY(m.ok());
        QCOMPARE(m->platform, Platform::PS4);
        QCOMPARE(m->kind, ContentKind::Application);
        QVERIFY(m->kindReason.contains(QStringLiteral("CATEGORY")));

        // Without the dump's own param.sfo or a PS4 title ID, a deep param.json still means PS5.
        const QString w = fresh(QStringLiteral("ps5-wrapper"));
        QVERIFY(makePs5Dump(QDir(w).filePath(QStringLiteral("inner")), QStringLiteral("PPSA04321"), QStringLiteral("Wrapped")));
        auto wm = readGameMetadata(w);
        QVERIFY(wm.ok());
        QCOMPARE(wm->platform, Platform::PS5);
        // And the dump's own damaged param.json still means PS5.
        const QString d = fresh(QStringLiteral("ps5-damaged"));
        QVERIFY(write(QDir(d).filePath(QStringLiteral("eboot.bin")), "x"));
        QVERIFY(write(QDir(d).filePath(QStringLiteral("sce_sys/param.json")), "{ not json"));
        QCOMPARE(readGameMetadata(d)->platform, Platform::PS5);
    }

    void longNamesKeepTheirVersionAndOddTemplatesWork()
    {
        const QString longTitle = QStringLiteral("An Extremely Long Game Title That Goes On And On And On Until It Is Far Too Long. Really");
        const QString name = buildDumpName({QStringLiteral("PPSA01234"), longTitle, QStringLiteral("01.050.000")},
                                           NamePreset::TitleIdTitleVersion);
        QVERIFY2(name.endsWith(QStringLiteral(" (01.050.000)")), qPrintable(name));
        QVERIFY(name.startsWith(QStringLiteral("PPSA01234 An Extremely Long")));
        QCOMPARE(int(name.size()), 80);
        // Two versions of the same long-titled game no longer collide.
        QVERIFY(name != buildDumpName({QStringLiteral("PPSA01234"), longTitle, QStringLiteral("01.060.000")},
                                      NamePreset::TitleIdTitleVersion));
        // Short names are exactly as before.
        QCOMPARE(buildDumpName({QStringLiteral("PPSA01234"), QStringLiteral("Short"), QStringLiteral("01.000.000")},
                               NamePreset::TitleIdTitleVersion),
                 QStringLiteral("PPSA01234 Short (01.000.000)"));

        // An unknown token keeps its text and does not swallow the next token's "%".
        ImageNameFields f;
        f.title = QStringLiteral("Game");
        f.titleId = QStringLiteral("PPSA01234");
        f.folderName = QStringLiteral("folder");
        QCOMPARE(renderImageName(ImageNamePreset::Custom, QStringLiteral("%X%TITLE% %PPSA%"), f), QStringLiteral("%XGame PPSA01234"));
        // A reserved device name is reserved with an extension too.
        QCOMPARE(sanitizeImageName(QStringLiteral("NUL")), QStringLiteral("NUL_"));
        QCOMPARE(sanitizeImageName(QStringLiteral("nul.backup")), QStringLiteral("nul_.backup"));
        QCOMPARE(sanitizeImageName(QStringLiteral("NULL.backup")), QStringLiteral("NULL.backup"));
        // IDs with a trailing line feed are not valid IDs.
        QVERIFY(!isValidTitleId(QStringLiteral("PPSA01234\n")));
        QVERIFY(!isValidContentId(QStringLiteral("UP9000-PPSA01234_00-ABCDEFGHIJKLMNOP\n")));
        QVERIFY(isValidTitleId(QStringLiteral("PPSA01234")));
    }

    // ---- hostile input ---------------------------------------------------------------
    void sfoBoundsEntryCountAndValueSizes()
    {
        // A crafted header: a million entries, each claiming nearly the whole file as its value.
        const quint32 count = 0xFFFFF;
        QByteArray sfo(20 + qsizetype(count) * 16, '\0');
        qToLittleEndian<quint32>(0x46535000, sfo.data());
        qToLittleEndian<quint32>(0x101, sfo.data() + 4);
        qToLittleEndian<quint32>(0, sfo.data() + 8);
        qToLittleEndian<quint32>(0, sfo.data() + 12);
        qToLittleEndian<quint32>(count, sfo.data() + 16);
        for (quint32 i = 0; i < count; ++i) {
            char* e = sfo.data() + 20 + qsizetype(i) * 16;
            qToLittleEndian<quint16>(0x0204, e + 2);
            qToLittleEndian<quint32>(quint32(sfo.size() - 1), e + 4);
        }
        auto r = parseSfo(sfo);
        QVERIFY(!r.ok());
        QCOMPARE(errorOf(r).code, ErrorCode::CorruptData);

        // Within the entry limit, oversized and overlapping values are skipped, not copied.
        const quint32 few = 4096;
        QByteArray big(20 + qsizetype(few) * 16 + 8 * 1024 * 1024, '\0');
        qToLittleEndian<quint32>(0x46535000, big.data());
        qToLittleEndian<quint32>(0, big.data() + 8);
        qToLittleEndian<quint32>(0, big.data() + 12);
        qToLittleEndian<quint32>(few, big.data() + 16);
        for (quint32 i = 0; i < few; ++i) {
            char* e = big.data() + 20 + qsizetype(i) * 16;
            qToLittleEndian<quint16>(0x0204, e + 2);
            qToLittleEndian<quint32>(i == 0 ? 4u : quint32(big.size() - 1), e + 4); // only the first is sane
        }
        auto b = parseSfo(big);
        QVERIFY(b.ok());
        QCOMPARE(int(b->entries.size()), 1);
        QCOMPARE(b->skippedEntries, int(few) - 1);

        // 4,096 entries that each fit, but overlap: together they may not exceed the file.
        QByteArray overlap(20 + qsizetype(few) * 16, '\0');
        qToLittleEndian<quint32>(0x46535000, overlap.data());
        qToLittleEndian<quint32>(few, overlap.data() + 16);
        for (quint32 i = 0; i < few; ++i) {
            char* e = overlap.data() + 20 + qsizetype(i) * 16;
            qToLittleEndian<quint16>(0x0204, e + 2);
            qToLittleEndian<quint32>(60000, e + 4);
        }
        auto ov = parseSfo(overlap);
        QVERIFY(ov.ok());
        qint64 copied = 0;
        for (const auto& e : ov->entries)
            copied += e.raw.size();
        QVERIFY(copied <= overlap.size());
        QVERIFY(ov->skippedEntries > 0);
    }

    void coverWithAnExtremeShapeStaysSmall()
    {
        const QString r = fresh(QStringLiteral("thin-cover"));
        for (const QSize size : {QSize(1, 8192), QSize(8192, 1), QSize(3, 20000)}) {
            QImage img(size, QImage::Format_RGB32);
            img.fill(QColor(10, 120, 200));
            const QString src = QDir(r).filePath(QStringLiteral("%1x%2.png").arg(size.width()).arg(size.height()));
            QVERIFY(img.save(src, "PNG"));
            auto out = normalizeCoverImage(src);
            QVERIFY2(out.ok(), out.ok() ? "" : qPrintable(out.error().toLogString()));
            QCOMPARE(out->size(), QSize(kCoverWidth, kCoverHeight));
            QVERIFY(QColor(out->pixel(kCoverWidth / 2, kCoverHeight / 2)).blue() > 150);
        }
    }

    void outOfRangeJsonNumbersAreIgnored()
    {
        auto p = parseParamJson("{\"titleId\":\"PPSA01234\",\"attribute\":1e300,\"attribute2\":-1e300,"
                                "\"attribute3\":7,\"applicationCategoryType\":1e19,\"sdkVersion\":1e40}");
        QVERIFY(p.ok());
        QVERIFY(!p->attribute);
        QVERIFY(!p->attribute2);
        QVERIFY(!p->applicationCategoryType);
        QCOMPARE(*p->attribute3, qint64(7));
        QVERIFY(!p->sdkVersion.isValid());
        QVERIFY(!parseVersionField(QJsonValue(1e40)).isValid());
        QVERIFY(!parseVersionField(QJsonValue(1.9e19)).isValid());
        // A packed quad word written as a number still decodes.
        QCOMPARE(parseVersionField(QJsonValue(double(0x0450000000000000ULL))).toString(), QStringLiteral("4.50"));
    }

    // ---- module registration -----------------------------------------------------------
    void settingsAreWellFormedAndLegacyKeysImport()
    {
        Module mod;
        int n = 0;
        for (const auto* d : mod.schema->all()) {
            QVERIFY2(d->key.startsWith(QStringLiteral("library.")) || d->key.startsWith(QStringLiteral("build.")), qPrintable(d->key));
            QVERIFY(mod.schema->normalize(d->key, d->defaultValue).ok());
            QVERIFY(!d->description.isEmpty());
            ++n;
        }
        QCOMPARE(n, 6); // library.folders / renamePreset / coverCacheFolder, build.namePreset / nameTemplate / firmwareOverride
        QVERIFY(mod.jobs.contains(jobkinds::kLibraryScan));
        QVERIFY(mod.jobs.contains(jobkinds::kLibraryRename));
        QVERIFY(!mod.schema->normalize(keys::kRenamePreset, QStringLiteral("nope")).ok());
        // Build page naming: the legacy default preset and template.
        QCOMPARE(mod.schema->find(keys::kNamePreset)->defaultValue.toString(), QStringLiteral("ppsa_title_ver"));
        QCOMPARE(mod.schema->find(keys::kNameTemplate)->defaultValue.toString(), kDefaultImageNameTemplate);
        QVERIFY(!mod.schema->normalize(keys::kNamePreset, QStringLiteral("nope")).ok());
        QVERIFY(mod.schema->normalize(keys::kFirmwareOverride, QStringLiteral("13")).ok());

        const QString legacy = QDir(fresh(QStringLiteral("legacy"))).filePath(QStringLiteral("settings.json"));
        QVERIFY(write(legacy, R"({"lib_folders": ["D:\\Games", "E:\\More"], "unrelated": 1,
            "name_preset": "custom", "name_custom_template": "%PPSA% %TITLE%"})"));
        SettingsStore store(mod.schema, QDir(fresh(QStringLiteral("store"))).filePath(QStringLiteral("s.json")));
        QVERIFY(store.load().ok());
        auto imported = store.importLegacy(legacy);
        QVERIFY(imported.ok());
        QCOMPARE(imported.value(), 3);
        QCOMPARE(store.value(keys::kLibraryFolders).toStringList().size(), 2);
        QCOMPARE(store.value(keys::kNamePreset).toString(), QStringLiteral("custom"));
        QCOMPARE(store.value(keys::kNameTemplate).toString(), QStringLiteral("%PPSA% %TITLE%"));
    }
};

QTEST_GUILESS_MAIN(TstGameInfo)
#include "tst_gameinfo.moc"
