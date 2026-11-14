// DLC INI parsing and byte-preserving rewriting (the Companion's
// DlcIniDocument): encodings and BOMs, the line model, entries in any section,
// title sources, field edits with their quoting rules, re-parse validation and
// the create-new file writes.

#include "engines/dlc/DlcIni.h"

#include <QDir>
#include <QFile>
#include <QStringEncoder>
#include <QTemporaryDir>
#include <QTest>

using namespace fw;
using namespace fw::dlc;

namespace fw::dlc {
struct DlcIniTestAccess {
    static std::vector<DlcIniEntry>& entries(DlcIniDocument& d) { return d.m_entries; }
    static QStringList lineTexts(const DlcIniDocument& d)
    {
        QStringList out;
        for (const auto& l : d.m_lines)
            out << l.text;
        return out;
    }
    static QStringList lineEnds(const DlcIniDocument& d)
    {
        QStringList out;
        for (const auto& l : d.m_lines)
            out << l.end;
        return out;
    }
};
} // namespace fw::dlc

namespace {

const QString kCidA = QStringLiteral("UP0000-PPSA01234_00-AAAAAAAAAAAAAAAA");
const QString kCidB = QStringLiteral("UP0000-PPSA01234_00-BBBBBBBBBBBBBBBB");
const QString kCidC = QStringLiteral("UP0000-PPSA01234_00-CCCCCCCCCCCCCCCC");
const QString kTid = QStringLiteral("PPSA01234");

const QString kBadBomText = QStringLiteral("DLC INI has invalid text for its declared BOM encoding.");
const QString kNulText = QStringLiteral("DLC INI contains NUL characters or an unsupported encoding.");
const QString kTooLarge = QStringLiteral("DLC INI exceeds the 16 MiB safety limit.");
const QString kLatin1Warning = QStringLiteral(
    "Non-UTF-8 text is preserved as single-byte text. Non-ASCII title edits must be representable in that encoding.");
const QString kNoEntries = QStringLiteral("No content_id or contentId fields were found.");
const QString kMalformed = QStringLiteral("Malformed Content ID; correct it before writing or building.");
const QString kMultiple = QStringLiteral(
    "Multiple Content IDs in one section: only identity/name fields in this entry's following block are updated; "
    "shared fields before the first Content ID remain unchanged.");
const QString kTitleIdDiffers = QStringLiteral("INI Title ID differs from the Content ID's embedded Title ID.");
const QString kConflict =
    QStringLiteral("Two target entries would change the same INI field differently. Write separate target files.");
const QString kNotRepresentable = QStringLiteral("The edited title is not representable in the original INI encoding.");

QByteArray u8(const QString& s) { return s.toUtf8(); }

template <typename T>
QString errorText(const Result<T>& r)
{
    return r.ok() ? QStringLiteral("<ok>") : r.error().message;
}

// The error of a call that must have failed (a marker error, not undefined
// behaviour, when it succeeded).
template <typename R>
Error errorOf(const R& result)
{
    return result.ok() ? Error(ErrorCode::Internal, QStringLiteral("<the call succeeded>")) : result.error();
}

DlcTarget target(const QString& titleId, const QString& contentId, std::optional<QString> title = std::nullopt)
{
    return DlcTarget{titleId, contentId, std::move(title)};
}

QByteArray encodeWith(const QString& codec, const QString& text)
{
    if (codec == u"utf8")
        return text.toUtf8();
    QStringConverter::Encoding e = QStringConverter::Utf16LE;
    if (codec == u"utf16be")
        e = QStringConverter::Utf16BE;
    else if (codec == u"utf32le")
        e = QStringConverter::Utf32LE;
    else if (codec == u"utf32be")
        e = QStringConverter::Utf32BE;
    QStringEncoder enc(e);
    QByteArray out = enc(text);
    return out;
}

QByteArray readAll(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QByteArray("<unreadable>");
    return f.readAll();
}

bool writeAll(const QString& path, const QByteArray& data)
{
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(data) == data.size();
}

// The sample used by the field and rewrite tests (CRLF throughout).
QByteArray sampleIni()
{
    return QByteArray("; DLC list\r\n"                                                  // 0
                      "\r\n"                                                            // 1
                      "[Global]\r\n"                                                    // 2
                      "version=2\r\n"                                                   // 3
                      "\r\n"                                                            // 4
                      "[DLC1]\r\n"                                                      // 5
                      "# Season Pass\r\n"                                               // 6
                      "content_id=UP0000-PPSA01234_00-SEASONPASS000001\r\n"             // 7
                      "title_id=PPSA01234\r\n"                                          // 8
                      "primary_id=UP0000-PPSA01234_00-SEASONPASS000001\r\n"             // 9
                      "unknown_key = keep me ; note\r\n"                                // 10
                      "\r\n"                                                            // 11
                      "[DLC2]\r\n"                                                      // 12
                      "contentId = \"UP0000-PPSA01234_00-COSTUMEPACK00002\" ; quoted\r\n" // 13
                      "name=Costume Pack\r\n"                                           // 14
                      "[Broken]\r\n"                                                    // 15
                      "content_id=NOT-A-CID\r\n");                                      // 16
}

} // namespace

#define PARSE_OK(var, bytes)                                                                       \
    auto var##_result = DlcIniDocument::parse(bytes);                                              \
    QVERIFY2(var##_result.ok(), qPrintable(errorText(var##_result)));                              \
    DlcIniDocument& var = var##_result.value()

#define UPDATE_OK(var, doc, changes)                                                               \
    auto var##_result = (doc).updatedBytes(changes);                                               \
    QVERIFY2(var##_result.ok(), qPrintable(errorText(var##_result)));                              \
    const QByteArray var = var##_result.value()

class TstDlcIni : public QObject {
    Q_OBJECT

private slots:
    // ---- encodings ----
    void bomVariants_data()
    {
        QTest::addColumn<QString>("codec");
        QTest::addColumn<QByteArray>("bom");
        QTest::addColumn<QString>("encodingName");
        QTest::newRow("utf-8 no BOM") << "utf8" << QByteArray() << "utf-8 (no BOM)";
        QTest::newRow("utf-8 BOM") << "utf8" << QByteArray::fromHex("EFBBBF") << "utf-8 (BOM)";
        QTest::newRow("utf-16 LE") << "utf16le" << QByteArray::fromHex("FFFE") << "utf-16LE (BOM)";
        QTest::newRow("utf-16 BE") << "utf16be" << QByteArray::fromHex("FEFF") << "utf-16BE (BOM)";
        QTest::newRow("utf-32 LE") << "utf32le" << QByteArray::fromHex("FFFE0000") << "utf-32LE (BOM)";
        QTest::newRow("utf-32 BE") << "utf32be" << QByteArray::fromHex("0000FEFF") << "utf-32BE (BOM)";
    }

    void bomVariants()
    {
        QFETCH(QString, codec);
        QFETCH(QByteArray, bom);
        QFETCH(QString, encodingName);
        const QString text = QStringLiteral("; ünïcode \U0001D11E\r\n[DLC]\r\n# Old Ü\r\ncontent_id=") + kCidA
            + QStringLiteral("\r\n");
        const QByteArray bytes = bom + encodeWith(codec, text);
        PARSE_OK(doc, bytes);
        QCOMPARE(doc.encodingName(), encodingName);
        QVERIFY(doc.warnings().isEmpty());
        QCOMPARE(doc.originalBytes(), bytes);
        QCOMPARE(doc.entries().size(), size_t(1));
        QCOMPARE(doc.entries()[0].title, QStringLiteral("Old Ü"));
        QCOMPARE(doc.entries()[0].contentId, kCidA);

        UPDATE_OK(same, doc, {});
        QCOMPARE(same, bytes);

        const QString newTitle = QStringLiteral("Neu ✓ \U0001D11E");
        UPDATE_OK(out, doc, (std::map<int, DlcTarget>{{0, target(kTid, kCidA, newTitle)}}));
        QString expected = text;
        expected.replace(QStringLiteral("# Old Ü"), QStringLiteral("# ") + newTitle);
        QCOMPARE(out, bom + encodeWith(codec, expected));
    }

    void invalidBomText_data()
    {
        QTest::addColumn<QByteArray>("bytes");
        QTest::newRow("utf-8 invalid byte") << QByteArray::fromHex("EFBBBF" "613DFF0A");
        QTest::newRow("utf-8 overlong") << QByteArray::fromHex("EFBBBF" "61C0AF");
        QTest::newRow("utf-8 surrogate") << QByteArray::fromHex("EFBBBF" "61EDA080");
        QTest::newRow("utf-8 above U+10FFFF") << QByteArray::fromHex("EFBBBF" "F4908080");
        QTest::newRow("utf-8 truncated") << QByteArray::fromHex("EFBBBF" "61E282");
        QTest::newRow("utf-16 LE odd length") << QByteArray::fromHex("FFFE" "610062");
        QTest::newRow("utf-16 LE lone high surrogate") << QByteArray::fromHex("FFFE" "00D86100");
        QTest::newRow("utf-16 BE lone low surrogate") << QByteArray::fromHex("FEFF" "0061DC00");
        QTest::newRow("utf-32 LE above U+10FFFF") << QByteArray::fromHex("FFFE0000" "61000000" "00001100");
        QTest::newRow("utf-32 LE surrogate") << QByteArray::fromHex("FFFE0000" "00D80000");
        QTest::newRow("utf-32 BE truncated") << QByteArray::fromHex("0000FEFF" "000000");
    }

    void invalidBomText()
    {
        QFETCH(QByteArray, bytes);
        const auto r = DlcIniDocument::parse(bytes);
        QCOMPARE(errorText(r), kBadBomText);
    }

    void latin1Fallback()
    {
        const QByteArray bytes = QByteArray("# Caf\xE9\r\ncontent_id=") + kCidA.toLatin1() + "\r\n";
        PARSE_OK(doc, bytes);
        QCOMPARE(doc.encodingName(), QStringLiteral("iso-8859-1 (no BOM)"));
        QCOMPARE(doc.warnings(), QStringList{kLatin1Warning});
        QCOMPARE(doc.entries()[0].title, QStringLiteral("Café"));

        UPDATE_OK(same, doc, {});
        QCOMPARE(same, bytes);
        UPDATE_OK(out, doc, (std::map<int, DlcTarget>{{0, target(kTid, kCidA, QStringLiteral("Crème"))}}));
        QCOMPARE(out, QByteArray("# Cr\xE8me\r\ncontent_id=") + kCidA.toLatin1() + "\r\n");

        const auto snow = doc.updatedBytes({{0, target(kTid, kCidA, QStringLiteral("Snow ☃"))}});
        QCOMPARE(errorText(snow), kNotRepresentable);

        // Malformed UTF-8 without a BOM is not an error: it is kept as single-byte text.
        PARSE_OK(overlong, QByteArray::fromHex("613DC0AF0A"));
        QCOMPARE(overlong.encodingName(), QStringLiteral("iso-8859-1 (no BOM)"));
        QVERIFY(overlong.warnings().contains(kLatin1Warning));
    }

    void unpairedSurrogateTitleIsNotRepresentable()
    {
        PARSE_OK(doc, u8(QStringLiteral("content_id=") + kCidA + QStringLiteral("\n")));
        QString broken = QStringLiteral("x");
        broken.append(QChar(char16_t(0xD800)));
        QCOMPARE(errorText(doc.updatedBytes({{0, target(kTid, kCidA, broken)}})), kNotRepresentable);
    }

    void nulRejected_data()
    {
        QTest::addColumn<QByteArray>("bytes");
        QTest::newRow("utf-8") << QByteArray("content_id=A\0B\n", 15);
        QTest::newRow("utf-16 LE without BOM") << encodeWith(QStringLiteral("utf16le"), QStringLiteral("content_id=X\n"));
        QTest::newRow("utf-16 LE with BOM") << QByteArray::fromHex("FFFE" "610000006200");
        QTest::newRow("latin-1 fallback") << QByteArray::fromHex("61FF0062");
    }

    void nulRejected()
    {
        QFETCH(QByteArray, bytes);
        QCOMPARE(errorText(DlcIniDocument::parse(bytes)), kNulText);
    }

    void sizeLimit()
    {
        QCOMPARE(errorText(DlcIniDocument::parse(QByteArray(kMaxIniBytes + 1, 'a'))), kTooLarge);
        const auto atLimit = DlcIniDocument::parse(QByteArray(kMaxIniBytes, 'a'));
        QVERIFY2(atLimit.ok(), qPrintable(errorText(atLimit)));
        QCOMPARE(atLimit->warnings(), QStringList{kNoEntries});

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString big = dir.filePath(QStringLiteral("big.ini"));
        {
            QFile f(big);
            QVERIFY(f.open(QIODevice::WriteOnly));
            QVERIFY(f.resize(kMaxIniBytes + 1));
        }
        QCOMPARE(errorText(DlcIniDocument::load(big)), kTooLarge);
    }

    void loadMissing()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto missing = DlcIniDocument::load(dir.filePath(QStringLiteral("nope.ini")));
        QCOMPARE(errorText(missing), QStringLiteral("DLC INI does not exist."));
        QCOMPARE(errorOf(missing).code, ErrorCode::NotFound);
        QCOMPARE(errorText(DlcIniDocument::load(dir.path())), QStringLiteral("DLC INI does not exist."));
        QCOMPARE(errorText(DlcIniDocument::load(QString())), QStringLiteral("DLC INI does not exist."));
    }

    // ---- line model ----
    void lineModel_data()
    {
        QTest::addColumn<QByteArray>("bytes");
        QTest::addColumn<QStringList>("texts");
        QTest::addColumn<QStringList>("ends");
        QTest::newRow("empty") << QByteArray() << QStringList{QString()} << QStringList{QString()};
        QTest::newRow("no terminator") << QByteArray("a") << QStringList{"a"} << QStringList{QString()};
        QTest::newRow("trailing LF") << QByteArray("a\n") << QStringList{"a"} << QStringList{"\n"};
        QTest::newRow("blank last line") << QByteArray("a\n\n") << QStringList{"a", ""} << QStringList{"\n", "\n"};
        QTest::newRow("only CRLF") << QByteArray("\r\n") << QStringList{QString()} << QStringList{"\r\n"};
        QTest::newRow("LF then CR") << QByteArray("\n\r") << QStringList{"", ""} << QStringList{"\n", "\r"};
        QTest::newRow("mixed") << QByteArray("a\r\nb\rc\nd") << QStringList{"a", "b", "c", "d"}
                               << QStringList{"\r\n", "\r", "\n", ""};
    }

    void lineModel()
    {
        QFETCH(QByteArray, bytes);
        QFETCH(QStringList, texts);
        QFETCH(QStringList, ends);
        PARSE_OK(doc, bytes);
        QCOMPARE(DlcIniTestAccess::lineTexts(doc), texts);
        QCOMPARE(DlcIniTestAccess::lineEnds(doc), ends);
    }

    void emptyFile()
    {
        PARSE_OK(doc, QByteArray());
        QVERIFY(doc.entries().empty());
        QCOMPARE(doc.warnings(), QStringList{kNoEntries});
        QCOMPARE(doc.encodingName(), QStringLiteral("utf-8 (no BOM)"));
        UPDATE_OK(same, doc, {});
        QCOMPARE(same, QByteArray());

        PARSE_OK(bomOnly, QByteArray::fromHex("EFBBBF"));
        QCOMPARE(bomOnly.encodingName(), QStringLiteral("utf-8 (BOM)"));
        UPDATE_OK(sameBom, bomOnly, {});
        QCOMPARE(sameBom, QByteArray::fromHex("EFBBBF"));
    }

    // ---- entries ----
    void entryFields()
    {
        PARSE_OK(doc, sampleIni());
        QVERIFY(doc.warnings().isEmpty());
        QCOMPARE(doc.entries().size(), size_t(3));

        const DlcIniEntry& a = doc.entries()[0];
        QCOMPARE(a.index, 0);
        QCOMPARE(a.contentId, QStringLiteral("UP0000-PPSA01234_00-SEASONPASS000001"));
        QCOMPARE(a.titleId, kTid);
        QCOMPARE(a.title, QStringLiteral("Season Pass"));
        QCOMPARE(a.section, QStringLiteral("DLC1"));
        QCOMPARE(a.region, QStringLiteral("Americas (inferred from UP)"));
        QCOMPARE(a.startLine, 5);
        QCOMPARE(a.contentLine, 7);
        QCOMPARE(a.endLine, 12);
        QCOMPARE(a.titleCommentLine, 6);
        QVERIFY(a.warnings.isEmpty());
        const std::vector<std::pair<QString, QString>> aMeta{
            {"content_id", "UP0000-PPSA01234_00-SEASONPASS000001"},
            {"title_id", "PPSA01234"},
            {"primary_id", "UP0000-PPSA01234_00-SEASONPASS000001"},
            {"unknown_key", "keep me"}};
        QVERIFY(a.metadata == aMeta);

        const DlcIniEntry& b = doc.entries()[1];
        QCOMPARE(b.index, 1);
        QCOMPARE(b.contentId, QStringLiteral("UP0000-PPSA01234_00-COSTUMEPACK00002"));
        QCOMPARE(b.title, QStringLiteral("Costume Pack"));
        QCOMPARE(b.section, QStringLiteral("DLC2"));
        QCOMPARE(b.startLine, 12);
        QCOMPARE(b.contentLine, 13);
        QCOMPARE(b.endLine, 15);
        QCOMPARE(b.titleCommentLine, -1);
        const std::vector<std::pair<QString, QString>> bMeta{
            {"contentId", "UP0000-PPSA01234_00-COSTUMEPACK00002"}, {"name", "Costume Pack"}};
        QVERIFY(b.metadata == bMeta);

        const DlcIniEntry& c = doc.entries()[2];
        QCOMPARE(c.contentId, QStringLiteral("NOT-A-CID"));
        QCOMPARE(c.titleId, QString());
        QCOMPARE(c.title, QStringLiteral("CID"));
        QCOMPARE(c.region, QStringLiteral("Unknown"));
        QCOMPARE(c.section, QStringLiteral("Broken"));
        QCOMPARE(c.startLine, 15);
        QCOMPARE(c.endLine, 17);
        QCOMPARE(c.warnings, QStringList{kMalformed});
    }

    void entriesInAnySection()
    {
        const QByteArray bytes = u8(QStringLiteral("content_id=%1\n"     // 0
                                                   "[Weird Name]\n"      // 1
                                                   "CONTENTID=%2\n"      // 2
                                                   "[DLC]\n"             // 3
                                                   "[Other]\n"           // 4
                                                   "  ContentId  =  %3  \n" // 5
                                                   "[broken header\n"    // 6
                                                   "x=1\n")              // 7
                                        .arg(kCidA, kCidB, kCidC));
        PARSE_OK(doc, bytes);
        QCOMPARE(doc.entries().size(), size_t(3));
        QCOMPARE(doc.entries()[0].section, QString());
        QCOMPARE(doc.entries()[0].startLine, 0);
        QCOMPARE(doc.entries()[0].endLine, 1);
        QCOMPARE(doc.entries()[1].section, QStringLiteral("Weird Name"));
        QCOMPARE(doc.entries()[1].contentId, kCidB);
        QCOMPARE(doc.entries()[1].startLine, 1);
        QCOMPARE(doc.entries()[1].endLine, 3);
        QCOMPARE(doc.entries()[2].section, QStringLiteral("Other"));
        QCOMPARE(doc.entries()[2].contentId, kCidC);
        QCOMPARE(doc.entries()[2].startLine, 4);
        QCOMPARE(doc.entries()[2].endLine, 8);
        const std::vector<std::pair<QString, QString>> meta{{"ContentId", kCidC}, {"x", "1"}};
        QVERIFY(doc.entries()[2].metadata == meta);
        for (const DlcIniEntry& e : doc.entries())
            QVERIFY2(e.warnings.isEmpty(), qPrintable(e.warnings.join(u'|')));
    }

    void multipleInSection()
    {
        const QString text = QStringLiteral("[DLC]\n"             // 0
                                            "title_id=PPSA01234\n" // 1 shared, before the first Content ID
                                            "# First\n"           // 2
                                            "content_id=%1\n"     // 3
                                            "name=First Name\n"   // 4
                                            "# Second\n"          // 5
                                            "content_id=%2\n"     // 6
                                            "title_id=PPSA01234\n" // 7
                                            "[Next]\n"            // 8
                                            "content_id=%3\n")    // 9
                                 .arg(kCidA, kCidB, kCidC);
        PARSE_OK(doc, u8(text));
        QCOMPARE(doc.entries().size(), size_t(3));
        const DlcIniEntry& a = doc.entries()[0];
        QCOMPARE(a.warnings, QStringList{kMultiple});
        QCOMPARE(a.startLine, 0);
        QCOMPARE(a.contentLine, 3);
        QCOMPARE(a.endLine, 6);
        QCOMPARE(a.titleCommentLine, 2);
        QCOMPARE(a.title, QStringLiteral("First Name"));
        const std::vector<std::pair<QString, QString>> aMeta{{"content_id", kCidA}, {"name", "First Name"}};
        QVERIFY(a.metadata == aMeta);
        const DlcIniEntry& b = doc.entries()[1];
        QCOMPARE(b.warnings, QStringList{kMultiple});
        QCOMPARE(b.endLine, 8);
        QCOMPARE(b.titleCommentLine, 5);
        QCOMPARE(b.title, QStringLiteral("Second"));
        const std::vector<std::pair<QString, QString>> bMeta{{"content_id", kCidB}, {"title_id", kTid}};
        QVERIFY(b.metadata == bMeta);
        QVERIFY(doc.entries()[2].warnings.isEmpty());
        QCOMPARE(doc.entries()[2].endLine, 10);

        // Only the entry's own block changes; the shared title_id (line 1) stays.
        const QString cidA2 = QStringLiteral("UP0000-PPSA09999_00-AAAAAAAAAAAAAAAA");
        UPDATE_OK(outA, doc,
                  (std::map<int, DlcTarget>{{0, target(QStringLiteral("PPSA09999"), cidA2, QStringLiteral("Renamed"))}}));
        QString expectedA = text;
        expectedA.replace(QStringLiteral("content_id=%1\n").arg(kCidA), QStringLiteral("content_id=%1\n").arg(cidA2));
        expectedA.replace(QStringLiteral("name=First Name"), QStringLiteral("name=Renamed"));
        QCOMPARE(QString::fromUtf8(outA), expectedA);

        const QString cidB2 = QStringLiteral("UP0000-PPSA09999_00-BBBBBBBBBBBBBBBB");
        UPDATE_OK(outB, doc,
                  (std::map<int, DlcTarget>{{1, target(QStringLiteral("PPSA09999"), cidB2, QStringLiteral("Renamed B"))}}));
        const QString expectedB = QStringLiteral("[DLC]\ntitle_id=PPSA01234\n# First\ncontent_id=%1\nname=First Name\n"
                                                 "# Renamed B\ncontent_id=%2\ntitle_id=PPSA09999\n[Next]\ncontent_id=%3\n")
                                      .arg(kCidA, cidB2, kCidC);
        QCOMPARE(QString::fromUtf8(outB), expectedB);
    }

    void titleSources_data()
    {
        QTest::addColumn<QString>("text");
        QTest::addColumn<int>("entry");
        QTest::addColumn<QString>("title");
        QTest::addColumn<int>("commentLine");
        QTest::newRow("comment") << "# My DLC\ncontent_id=%1\n" << 0 << "My DLC" << 0;
        QTest::newRow("semicolon comment") << "  ;  Semi  \ncontent_id=%1" << 0 << "Semi" << 0;
        QTest::newRow("name key beats comment") << "# My DLC\ncontent_id=%1\ntitle=Real Name\n" << 0 << "Real Name" << 0;
        QTest::newRow("empty name key") << "# My DLC\ncontent_id=%1\nname=\n" << 0 << "My DLC" << 0;
        QTest::newRow("empty comment") << "#   \ncontent_id=%1\n" << 0 << "AAAAAAAAAAAAAAAA" << 0;
        QTest::newRow("no comment") << "content_id=%1\n" << 0 << "AAAAAAAAAAAAAAAA" << -1;
        QTest::newRow("comment before section") << "# File\n[DLC]\ncontent_id=%1\n" << 0 << "AAAAAAAAAAAAAAAA" << -1;
        QTest::newRow("latest comment") << "[DLC]\n# One\nx=1\n# Two\ncontent_id=%1\n" << 0 << "Two" << 3;
        QTest::newRow("previous entry's comment") << "# One\ncontent_id=%1\ncontent_id=%2\n" << 1 << "BBBBBBBBBBBBBBBB"
                                                  << -1;
        QTest::newRow("last name key wins") << "content_id=%1\ntitle=First\nname=Second\n" << 0 << "Second" << -1;
        QTest::newRow("titleName") << "content_id=%1\ntitleName=TN\n" << 0 << "TN" << -1;
        QTest::newRow("title_name") << "content_id=%1\nTITLE_NAME = \"Quoted  \"\n" << 0 << "Quoted  " << -1;
        QTest::newRow("suffix of malformed") << "content_id=BAD-ID-xyz\n" << 0 << "xyz" << -1;
    }

    void titleSources()
    {
        QFETCH(QString, text);
        QFETCH(int, entry);
        QFETCH(QString, title);
        QFETCH(int, commentLine);
        PARSE_OK(doc, u8(QString(text).replace(QStringLiteral("%1"), kCidA).replace(QStringLiteral("%2"), kCidB)));
        QVERIFY(size_t(entry) < doc.entries().size());
        QCOMPARE(doc.entries()[size_t(entry)].title, title);
        QCOMPARE(doc.entries()[size_t(entry)].titleCommentLine, commentLine);
    }

    void titleIdMismatch()
    {
        PARSE_OK(differs, u8(QStringLiteral("content_id=%1\ntitle_id=PPSA99999\n").arg(kCidA)));
        QCOMPARE(differs.entries()[0].titleId, QStringLiteral("PPSA99999"));
        QCOMPARE(differs.entries()[0].warnings, QStringList{kTitleIdDiffers});

        PARSE_OK(same, u8(QStringLiteral("content_id=%1\ntitleId=PPSA01234\n").arg(kCidA)));
        QCOMPARE(same.entries()[0].titleId, kTid);
        QVERIFY(same.entries()[0].warnings.isEmpty());

        // A malformed Content ID embeds no Title ID, so the INI value is used without a mismatch warning.
        PARSE_OK(malformed, QByteArray("content_id=BAD\ntitle_id=PPSA01234\n"));
        QCOMPARE(malformed.entries()[0].titleId, kTid);
        QCOMPARE(malformed.entries()[0].warnings, QStringList{kMalformed});
    }

    // ---- rewriting ----
    void rewriteKeepsUntouchedBytes()
    {
        PARSE_OK(doc, sampleIni());
        const QString tid = QStringLiteral("PPSA05678");
        const QString cid1 = QStringLiteral("EP1111-PPSA05678_00-SEASONPASS000001");
        const QString cid2 = QStringLiteral("EP1111-PPSA05678_00-COSTUMEPACK00002");
        UPDATE_OK(out, doc,
                  (std::map<int, DlcTarget>{{0, target(tid, cid1, QStringLiteral("Saison Pass"))},
                                            {1, target(tid, cid2, QStringLiteral("Costume Pack 2"))}}));
        const QByteArray expected("; DLC list\r\n"
                                  "\r\n"
                                  "[Global]\r\n"
                                  "version=2\r\n"
                                  "\r\n"
                                  "[DLC1]\r\n"
                                  "# Saison Pass\r\n"
                                  "content_id=EP1111-PPSA05678_00-SEASONPASS000001\r\n"
                                  "title_id=PPSA05678\r\n"
                                  "primary_id=EP1111-PPSA05678_00-SEASONPASS000001\r\n"
                                  "unknown_key = keep me ; note\r\n"
                                  "\r\n"
                                  "[DLC2]\r\n"
                                  "contentId = \"EP1111-PPSA05678_00-COSTUMEPACK00002\" ; quoted\r\n"
                                  "name=Costume Pack 2\r\n"
                                  "[Broken]\r\n"
                                  "content_id=NOT-A-CID\r\n");
        QCOMPARE(out, expected);

        // Identifiers only: the title stays and nothing else moves.
        UPDATE_OK(idOnly, doc, (std::map<int, DlcTarget>{{1, target(tid, cid2)}}));
        QByteArray expectedIdOnly = sampleIni();
        expectedIdOnly.replace("\"UP0000-PPSA01234_00-COSTUMEPACK00002\"", "\"EP1111-PPSA05678_00-COSTUMEPACK00002\"");
        QCOMPARE(idOnly, expectedIdOnly);

        PARSE_OK(again, out);
        QCOMPARE(again.entries()[0].titleId, tid);
        QCOMPARE(again.entries()[0].title, QStringLiteral("Saison Pass"));
        QCOMPARE(again.entries()[1].contentId, cid2);
        QCOMPARE(again.entries()[2].warnings, QStringList{kMalformed});
    }

    void rewritePrimaryId()
    {
        const QString other = QStringLiteral("UP0000-PPSA01234_00-OTHERCONTENT0001");
        PARSE_OK(doc, u8(QStringLiteral("content_id=%1\nprimary_id=%1\nprimaryId=%2\n").arg(kCidA, other)));
        const QString cid = QStringLiteral("UP0000-PPSA05678_00-AAAAAAAAAAAAAAAA");
        UPDATE_OK(out, doc, (std::map<int, DlcTarget>{{0, target(QStringLiteral("PPSA05678"), cid)}}));
        QCOMPARE(QString::fromUtf8(out), QStringLiteral("content_id=%1\nprimary_id=%1\nprimaryId=%2\n").arg(cid, other));
    }

    void rewriteNameKeys()
    {
        PARSE_OK(doc, u8(QStringLiteral("content_id=%1\ntitle=Old\ntitle_name=Old\nTitleName = Old\nNAME=\"Old\"\n"
                                        "other=Old\ntitle_id=%2\n")
                             .arg(kCidA, kTid)));
        UPDATE_OK(out, doc, (std::map<int, DlcTarget>{{0, target(kTid, kCidA, QStringLiteral("New"))}}));
        QCOMPARE(QString::fromUtf8(out),
                 QStringLiteral("content_id=%1\ntitle=New\ntitle_name=New\nTitleName = New\nNAME=\"New\"\nother=Old\n"
                                "title_id=%2\n")
                     .arg(kCidA, kTid));
    }

    // Several name-like keys with different values: a change that leaves the
    // title alone leaves all of them alone; an edited title is still written.
    void untouchedTitleLeavesOtherNameKeys()
    {
        const QString before = QStringLiteral("content_id=%1\nname=Costume Pack\ntitle=My Game\ntitle_name=\ntitle_id=%2\n");
        PARSE_OK(doc, u8(before.arg(kCidA, kTid)));
        QCOMPARE(doc.entries().front().title, QStringLiteral("My Game"));
        const QString cid = QStringLiteral("EP0000-PPSA01234_00-AAAAAAAAAAAAAAAA"); // region change only
        UPDATE_OK(out, doc, (std::map<int, DlcTarget>{{0, target(kTid, cid, QStringLiteral("My Game"))}}));
        QCOMPARE(QString::fromUtf8(out), before.arg(cid, kTid));
        UPDATE_OK(edited, doc, (std::map<int, DlcTarget>{{0, target(kTid, cid, QStringLiteral("New"))}}));
        QCOMPARE(QString::fromUtf8(edited),
                 QStringLiteral("content_id=%1\nname=New\ntitle=New\ntitle_name=New\ntitle_id=%2\n").arg(cid, kTid));
    }

    // "key = ; note": the value goes in front of the comment, which stays a comment.
    void emptyValueWithInlineComment()
    {
        PARSE_OK(doc, u8(QStringLiteral("content_id=%1\ntitle_id =   ; fill in\nname = # todo\n").arg(kCidA)));
        const QString cid = QStringLiteral("UP0000-PPSA05678_00-AAAAAAAAAAAAAAAA");
        UPDATE_OK(out, doc, (std::map<int, DlcTarget>{{0, target(QStringLiteral("PPSA05678"), cid, QStringLiteral("New"))}}));
        QCOMPARE(QString::fromUtf8(out), QStringLiteral("content_id=%1\ntitle_id =   PPSA05678 ; fill in\nname = New # todo\n").arg(cid));
        PARSE_OK(back, out);
        QCOMPARE(back.entries().front().titleId, QStringLiteral("PPSA05678"));
        QCOMPARE(back.entries().front().title, QStringLiteral("New"));
    }

    void insertsCommentTitle_data()
    {
        QTest::addColumn<QString>("before");
        QTest::addColumn<QString>("after");
        QTest::newRow("content line terminator") << "content_id=%1\r\nfoo=1\n" << "# New DLC\r\ncontent_id=%1\r\nfoo=1\n";
        QTest::newRow("last line uses CRLF") << "[DLC]\ncontent_id=%1" << "[DLC]\n# New DLC\r\ncontent_id=%1";
        QTest::newRow("CR file") << "content_id=%1\rx=1\r" << "# New DLC\rcontent_id=%1\rx=1\r";
        QTest::newRow("file comment kept") << "# File\n[DLC]\ncontent_id=%1\n" << "# File\n[DLC]\n# New DLC\ncontent_id=%1\n";
    }

    void insertsCommentTitle()
    {
        QFETCH(QString, before);
        QFETCH(QString, after);
        PARSE_OK(doc, u8(before.arg(kCidA)));
        UPDATE_OK(out, doc, (std::map<int, DlcTarget>{{0, target(kTid, kCidA, QStringLiteral("New DLC"))}}));
        QCOMPARE(QString::fromUtf8(out), after.arg(kCidA));
    }

    void replacesCommentTitle_data()
    {
        QTest::addColumn<QString>("before");
        QTest::addColumn<QString>("after");
        QTest::newRow("marker and spacing kept") << "  ;  Old Title  \ncontent_id=%1\n" << "  ;  New\ncontent_id=%1\n";
        QTest::newRow("hash without space") << "#Old\r\ncontent_id=%1\r\n" << "#New\r\ncontent_id=%1\r\n";
        QTest::newRow("comment above other keys") << "# Old\nfoo=1\ncontent_id=%1\n" << "# New\nfoo=1\ncontent_id=%1\n";
        QTest::newRow("empty comment") << "#\ncontent_id=%1\n" << "#New\ncontent_id=%1\n";
    }

    void replacesCommentTitle()
    {
        QFETCH(QString, before);
        QFETCH(QString, after);
        PARSE_OK(doc, u8(before.arg(kCidA)));
        UPDATE_OK(out, doc, (std::map<int, DlcTarget>{{0, target(kTid, kCidA, QStringLiteral("New"))}}));
        QCOMPARE(QString::fromUtf8(out), after.arg(kCidA));
    }

    void unchangedTitleKeepsBytes()
    {
        const QByteArray bytes = u8(QStringLiteral("# Old\ncontent_id=%1\n").arg(kCidA));
        PARSE_OK(doc, bytes);
        UPDATE_OK(same, doc, (std::map<int, DlcTarget>{{0, target(kTid, kCidA, QStringLiteral("Old"))}}));
        QCOMPARE(same, bytes);
        UPDATE_OK(keep, doc, (std::map<int, DlcTarget>{{0, target(kTid, kCidA)}}));
        QCOMPARE(keep, bytes);
    }

    void quoting_data()
    {
        QTest::addColumn<QString>("line");
        QTest::addColumn<QString>("title");
        QTest::addColumn<QString>("expected");
        QTest::newRow("leading/trailing spaces") << "name=Old" << " Spaced " << "name=\" Spaced \"";
        QTest::newRow("space hash") << "name=Old" << "A #1" << "name=\"A #1\"";
        QTest::newRow("space semicolon") << "name=Old" << "A ;1" << "name=\"A ;1\"";
        QTest::newRow("tab hash") << "name=Old" << "A\t#1" << "name=\"A\t#1\"";
        QTest::newRow("hash inside a word") << "name=Old" << "A#1;2" << "name=A#1;2";
        QTest::newRow("inline comment kept") << "name=Old ; note" << "New" << "name=New ; note";
        QTest::newRow("tab comment kept") << "name=Old\t# note" << "New" << "name=New\t# note";
        QTest::newRow("spacing kept") << "name = Old" << "New" << "name = New";
        QTest::newRow("quoted with comment") << "name=\"Old\" ; note" << " New " << "name=\" New \" ; note";
        QTest::newRow("quoted keeps quotes") << "name=\"Old\"" << "A #1" << "name=\"A #1\"";
        QTest::newRow("hash glued to value") << "name=Old#x" << "New" << "name=New";
        QTest::newRow("empty value") << "name=" << "New" << "name=New";
        QTest::newRow("quote inside unquoted") << "name=Old" << "Say \"hi\"" << "name=Say \"hi\"";
    }

    void quoting()
    {
        QFETCH(QString, line);
        QFETCH(QString, title);
        QFETCH(QString, expected);
        const QString head = QStringLiteral("content_id=%1\n").arg(kCidA);
        PARSE_OK(doc, u8(head + line + u'\n'));
        UPDATE_OK(out, doc, (std::map<int, DlcTarget>{{0, target(kTid, kCidA, title)}}));
        QCOMPARE(QString::fromUtf8(out), head + expected + u'\n');
        PARSE_OK(again, out);
        QCOMPARE(again.entries()[0].title, title);
    }

    void quoteErrors()
    {
        const QString head = QStringLiteral("content_id=%1\n").arg(kCidA);
        PARSE_OK(quoted, u8(head + QStringLiteral("name=\"Old\"\n")));
        QCOMPARE(errorText(quoted.updatedBytes({{0, target(kTid, kCidA, QStringLiteral("Say \"hi\""))}})),
                 QStringLiteral("A quoted INI title cannot contain a double quote."));

        PARSE_OK(plain, u8(head + QStringLiteral("name=Old\n")));
        const QString needs = QStringLiteral("This INI title needs quotes but contains an unsupported double quote.");
        QCOMPARE(errorText(plain.updatedBytes({{0, target(kTid, kCidA, QStringLiteral(" \"hi\" "))}})), needs);
        QCOMPARE(errorText(plain.updatedBytes({{0, target(kTid, kCidA, QStringLiteral("x #\"y\""))}})), needs);
    }

    void lineBreakErrors()
    {
        const QString head = QStringLiteral("content_id=%1\n").arg(kCidA);
        PARSE_OK(withName, u8(head + QStringLiteral("name=Old\n")));
        QCOMPARE(errorText(withName.updatedBytes({{0, target(kTid, kCidA, QStringLiteral("a\nb"))}})),
                 QStringLiteral("INI values cannot contain line breaks or NUL characters."));

        PARSE_OK(noName, u8(head));
        const QString titles = QStringLiteral("INI titles cannot contain line breaks or NUL characters.");
        QCOMPARE(errorText(noName.updatedBytes({{0, target(kTid, kCidA, QStringLiteral("a\r\nb"))}})), titles);
        QString nul = QStringLiteral("a");
        nul.append(QChar(u'\0'));
        QCOMPARE(errorText(noName.updatedBytes({{0, target(kTid, kCidA, nul)}})), titles);
    }

    void identityErrors()
    {
        PARSE_OK(doc, u8(QStringLiteral("content_id=%1\n").arg(kCidA)));
        QCOMPARE(errorText(doc.updatedBytes({{1, target(kTid, kCidA)}})), QStringLiteral("Unknown DLC entry index."));
        QCOMPARE(errorText(doc.updatedBytes({{-1, target(kTid, kCidA)}})), QStringLiteral("Unknown DLC entry index."));
        QCOMPARE(errorText(doc.updatedBytes({{0, target(QStringLiteral("PPSA1234"), kCidA)}})),
                 QStringLiteral("Title ID must be PPSA or CUSA followed by exactly five digits."));
        QCOMPARE(errorText(doc.updatedBytes({{0, target(kTid, QStringLiteral("UP0000-PPSA01234_00-short"))}})),
                 QStringLiteral("Content ID must use XX0000-PPSA00000_00-XXXXXXXXXXXXXXXX (or CUSA), with a "
                                "16-character uppercase alphanumeric/underscore suffix."));
        QCOMPARE(errorText(doc.updatedBytes({{0, target(QStringLiteral("PPSA05678"), kCidA)}})),
                 QStringLiteral("Title ID must match the game identifier embedded in the Content ID."));
    }

    void platformSwapRefused()
    {
        const QString refused = QStringLiteral("PS4 and PS5 identifiers cannot be interchanged.");
        PARSE_OK(ps5, u8(QStringLiteral("content_id=%1\n").arg(kCidA)));
        QCOMPARE(errorText(ps5.updatedBytes(
                     {{0, target(QStringLiteral("CUSA01234"), QStringLiteral("UP0000-CUSA01234_00-AAAAAAAAAAAAAAAA"))}})),
                 refused);
        PARSE_OK(ps4, QByteArray("content_id=EP0000-CUSA01234_00-AAAAAAAAAAAAAAAA\n"));
        QCOMPARE(errorText(ps4.updatedBytes({{0, target(kTid, kCidA)}})), refused);
    }

    void correctsMalformedEntry()
    {
        // A malformed original has no platform to protect; it can be corrected.
        PARSE_OK(doc, QByteArray("content_id=BROKEN\n"));
        const QString cid = QStringLiteral("EP0000-CUSA01234_00-AAAAAAAAAAAAAAAA");
        UPDATE_OK(out, doc, (std::map<int, DlcTarget>{{0, target(QStringLiteral("CUSA01234"), cid)}}));
        QCOMPARE(out, QByteArray("content_id=EP0000-CUSA01234_00-AAAAAAAAAAAAAAAA\n"));
    }

    void duplicateEditConflict()
    {
        // Blocks never overlap in a parsed file; point two entries at one title
        // comment to reach the guard.
        PARSE_OK(doc, u8(QStringLiteral("# Shared\ncontent_id=%1\ncontent_id=%2\n").arg(kCidA, kCidB)));
        DlcIniTestAccess::entries(doc)[1].titleCommentLine = 0;
        const auto r = doc.updatedBytes(
            {{0, target(kTid, kCidA, QStringLiteral("One"))}, {1, target(kTid, kCidB, QStringLiteral("Two"))}});
        QCOMPARE(errorText(r), kConflict);
    }

    void identityReadBackFailure()
    {
        PARSE_OK(doc, u8(QStringLiteral("content_id=%1\nfoo=1\ntitle_id=PPSA01234\n").arg(kCidA)));
        DlcIniTestAccess::entries(doc)[0].endLine = 2; // the title_id line falls outside the edited block
        const auto r = doc.updatedBytes(
            {{0, target(QStringLiteral("PPSA05678"), QStringLiteral("UP0000-PPSA05678_00-AAAAAAAAAAAAAAAA"))}});
        QCOMPARE(errorText(r), QStringLiteral("Updated INI failed identity validation; no output was saved."));
    }

    void titleReadBackFailure()
    {
        // Comment titles are read back trimmed, so trailing spaces cannot survive.
        PARSE_OK(doc, u8(QStringLiteral("content_id=%1\n").arg(kCidA)));
        const auto r = doc.updatedBytes({{0, target(kTid, kCidA, QStringLiteral("Trailing  "))}});
        QCOMPARE(errorText(r), QStringLiteral("Updated INI failed title validation; no output was saved."));
    }

    void lineEndingsPreserved()
    {
        const QString before = QStringLiteral("a=1\r\nb=2\ncontent_id=%1\rname=Old\r\n\n\rlast=1").arg(kCidA);
        PARSE_OK(doc, u8(before));
        const QString cid = QStringLiteral("UP0000-PPSA05678_00-AAAAAAAAAAAAAAAA");
        UPDATE_OK(out, doc,
                  (std::map<int, DlcTarget>{{0, target(QStringLiteral("PPSA05678"), cid, QStringLiteral("New"))}}));
        QCOMPARE(QString::fromUtf8(out), QStringLiteral("a=1\r\nb=2\ncontent_id=%1\rname=New\r\n\n\rlast=1").arg(cid));
    }

    // ---- files ----
    void saveUpdated()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString src = dir.filePath(QStringLiteral("dlc.ini"));
        QVERIFY(writeAll(src, sampleIni()));
        auto loaded = DlcIniDocument::load(src);
        QVERIFY2(loaded.ok(), qPrintable(errorText(loaded)));
        const DlcIniDocument& doc = loaded.value();
        QVERIFY(QFileInfo(doc.path()).isAbsolute());
        QCOMPARE(doc.sourceSize(), qint64(sampleIni().size()));

        const QString tid = QStringLiteral("PPSA05678");
        const QString cid = QStringLiteral("EP1111-PPSA05678_00-SEASONPASS000001");
        const std::map<int, DlcTarget> changes{{0, target(tid, cid, QStringLiteral("Saison Pass"))}};
        const QString overwrite = QStringLiteral("Original INI inputs cannot be overwritten. Choose an updated output name.");
        QCOMPARE(errorText(doc.saveUpdated(src, changes)), overwrite);
        QCOMPARE(errorText(doc.saveUpdated(dir.filePath(QStringLiteral("DLC.INI")), changes)), overwrite);
        QCOMPARE(errorText(doc.saveUpdated(dir.filePath(QStringLiteral("sub/../dlc.ini")), changes)), overwrite);

        const QString taken = dir.filePath(QStringLiteral("taken.ini"));
        QVERIFY(writeAll(taken, "keep"));
        const Status existing = doc.saveUpdated(taken, changes);
        QVERIFY(!existing.ok());
        QCOMPARE(errorOf(existing).code, ErrorCode::AlreadyExists);
        QCOMPARE(readAll(taken), QByteArray("keep"));

        const QString out = dir.filePath(QStringLiteral("dlc-PPSA05678-updated.ini"));
        const Status saved = doc.saveUpdated(out, changes);
        QVERIFY2(saved.ok(), qPrintable(errorText(saved)));
        UPDATE_OK(expected, doc, changes);
        QCOMPARE(readAll(out), expected);
        QCOMPARE(readAll(src), sampleIni());
        auto back = DlcIniDocument::load(out);
        QVERIFY2(back.ok(), qPrintable(errorText(back)));
        QCOMPARE(back->entries()[0].contentId, cid);
        QCOMPARE(back->entries()[0].titleId, tid);

        // A second save to the same name never overwrites.
        QCOMPARE(errorOf(doc.saveUpdated(out, changes)).code, ErrorCode::AlreadyExists);

        // A document parsed from bytes has no source file to protect.
        PARSE_OK(fromBytes, sampleIni());
        const QString out2 = dir.filePath(QStringLiteral("from-bytes.ini"));
        QVERIFY(fromBytes.saveUpdated(out2, changes).ok());
        QCOMPARE(readAll(out2), expected);
    }

    void saveUpdatedSourceChanged()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString src = dir.filePath(QStringLiteral("dlc.ini"));
        QVERIFY(writeAll(src, sampleIni()));
        auto loaded = DlcIniDocument::load(src);
        QVERIFY(loaded.ok());
        const std::map<int, DlcTarget> changes{{1, target(kTid, QStringLiteral("UP0000-PPSA01234_00-COSTUMEPACK00009"))}};
        const QString changed = QStringLiteral("The source INI changed after import. Reload it before applying edits.");

        QByteArray edited = sampleIni();
        edited.replace("version=2", "version=3"); // same size
        QVERIFY(writeAll(src, edited));
        {
            // The two writes can fall into one tick of the file system's clock
            // (two seconds on FAT): give the second one a later time for certain.
            QFile rewritten(src);
            QVERIFY(rewritten.open(QIODevice::ReadWrite));
            QVERIFY(rewritten.setFileTime(loaded->sourceModifiedUtc().addSecs(4), QFileDevice::FileModificationTime));
        }
        const QString out = dir.filePath(QStringLiteral("out.ini"));
        QCOMPARE(errorText(loaded->saveUpdated(out, changes)), changed);
        QVERIFY(!QFileInfo::exists(out));

        QVERIFY(QFile::remove(src));
        QCOMPARE(errorText(loaded->saveUpdated(out, changes)), changed);
        QVERIFY(!QFileInfo::exists(out));
    }

    void sourceUnchanged()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString src = dir.filePath(QStringLiteral("dlc.ini"));
        QVERIFY(writeAll(src, sampleIni()));
        auto loaded = DlcIniDocument::load(src);
        QVERIFY(loaded.ok());
        QVERIFY(loaded->sourceUnchanged());
        QVERIFY(loaded->sourceModifiedUtc().isValid());
        QCOMPARE(loaded->sourceModifiedUtc().timeSpec(), Qt::UTC);

        auto parsed = DlcIniDocument::parse(sampleIni(), src);
        QVERIFY(parsed.ok());
        QVERIFY(!parsed->sourceUnchanged()); // no snapshot of the file was taken

        QVERIFY(writeAll(src, sampleIni() + "x=1\r\n"));
        QVERIFY(!loaded->sourceUnchanged());
        QVERIFY(QFile::remove(src));
        QVERIFY(!loaded->sourceUnchanged());
    }

    void newIniForPackage()
    {
        const auto r = DlcIniDocument::newIniForPackage(QStringLiteral("My\r\nDLC"), kCidA);
        QVERIFY2(r.ok(), qPrintable(errorText(r)));
        QCOMPARE(r.value(), u8(QStringLiteral("#My  DLC\r\ncontent_id=%1\r\n").arg(kCidA)));
        PARSE_OK(doc, r.value());
        QCOMPARE(doc.encodingName(), QStringLiteral("utf-8 (no BOM)"));
        QCOMPARE(doc.entries().size(), size_t(1));
        QCOMPARE(doc.entries()[0].title, QStringLiteral("My  DLC"));

        const auto unicode = DlcIniDocument::newIniForPackage(QStringLiteral("Café ✓"), kCidA);
        QVERIFY(unicode.ok());
        QCOMPARE(unicode.value(), u8(QStringLiteral("#Café ✓\r\ncontent_id=%1\r\n").arg(kCidA)));
        QVERIFY(!unicode.value().startsWith("\xEF\xBB\xBF"));

        const QString failed = QStringLiteral("Generated INI failed validation.");
        QCOMPARE(errorText(DlcIniDocument::newIniForPackage(QStringLiteral("x"), u' ' + kCidA)), failed);
        QCOMPARE(errorText(DlcIniDocument::newIniForPackage(QStringLiteral("x"), kCidA + QStringLiteral("\ncontent_id=")
                                                                                     + kCidB)),
                 failed);
        QString nul = QStringLiteral("x");
        nul.append(QChar(u'\0'));
        QCOMPARE(errorText(DlcIniDocument::newIniForPackage(nul, kCidA)), failed);
    }

    void writeNewFileRules()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("new.ini"));
        QVERIFY(writeNewFile(path, "abc").ok());
        QCOMPARE(readAll(path), QByteArray("abc"));

        const Status again = writeNewFile(path, "other");
        QVERIFY(!again.ok());
        QCOMPARE(errorOf(again).code, ErrorCode::AlreadyExists);
        QCOMPARE(readAll(path), QByteArray("abc"));

        const QString noDir = dir.filePath(QStringLiteral("missing/new.ini"));
        QVERIFY(!writeNewFile(noDir, "abc").ok());
        QVERIFY(!QFileInfo::exists(noDir));

        const QString empty = dir.filePath(QStringLiteral("empty.ini"));
        QVERIFY(writeNewFile(empty, QByteArray()).ok());
        QCOMPARE(QFileInfo(empty).size(), qint64(0));

        QVERIFY(!writeNewFile(QString(), "abc").ok());

        // Written through a temporary name: none is left, and a file of the
        // user's that has that name is not touched.
        const QString other = dir.filePath(QStringLiteral("other.ini"));
        QFile theirs(other + QStringLiteral(".part"));
        QVERIFY(theirs.open(QIODevice::WriteOnly));
        theirs.write("the user's");
        theirs.close();
        QVERIFY(writeNewFile(other, "new").ok());
        QCOMPARE(readAll(other), QByteArray("new"));
        QCOMPARE(readAll(other + QStringLiteral(".part")), QByteArray("the user's"));
        QCOMPARE(QDir(dir.path()).entryList(QDir::Files, QDir::Name),
                 (QStringList{QStringLiteral("empty.ini"), QStringLiteral("new.ini"), QStringLiteral("other.ini"),
                              QStringLiteral("other.ini.part")}));
    }
};

QTEST_GUILESS_MAIN(TstDlcIni)
#include "tst_dlc_ini.moc"
