#include "engines/fpkg/PlayGo.h"

#include "engines/fpkg/ParamJson.h"

#include <QStringDecoder>
#include <QtEndian>

#include <algorithm>
#include <set>

namespace fw::fpkg {

// ---- languages ----------------------------------------------------------------

const QStringList& playGoLanguages()
{
    static const QStringList codes = {
        QStringLiteral("ja-JP"),  QStringLiteral("en-US"),   QStringLiteral("fr-FR"),   QStringLiteral("es-ES"),
        QStringLiteral("de-DE"),  QStringLiteral("it-IT"),   QStringLiteral("nl-NL"),   QStringLiteral("pt-PT"),
        QStringLiteral("ru-RU"),  QStringLiteral("ko-KR"),   QStringLiteral("zh-Hant"), QStringLiteral("zh-Hans"),
        QStringLiteral("fi-FI"),  QStringLiteral("sv-SE"),   QStringLiteral("da-DK"),   QStringLiteral("no-NO"),
        QStringLiteral("pl-PL"),  QStringLiteral("pt-BR"),   QStringLiteral("en-GB"),   QStringLiteral("tr-TR"),
        QStringLiteral("es-419"), QStringLiteral("ar-AE"),   QStringLiteral("fr-CA"),   QStringLiteral("cs-CZ"),
        QStringLiteral("hu-HU"),  QStringLiteral("el-GR"),   QStringLiteral("ro-RO"),   QStringLiteral("th-TH"),
        QStringLiteral("vi-VN"),  QStringLiteral("id-ID"),   QStringLiteral("uk-UA"),
    };
    return codes;
}

int playGoLanguageId(const QString& code)
{
    const QStringList& l = playGoLanguages();
    for (int i = 0; i < l.size(); ++i)
        if (l[i].compare(code.trimmed(), Qt::CaseInsensitive) == 0)
            return i;
    return -1;
}

QString canonicalPlayGoLanguage(const QString& code)
{
    const int id = playGoLanguageId(code);
    return id < 0 ? QString() : playGoLanguages()[id];
}

QStringList languagesInMask(quint64 mask)
{
    QStringList out;
    const QStringList& l = playGoLanguages();
    for (int id = 0; id < l.size(); ++id)
        if (mask & (quint64(1) << (63 - id)))
            out << l[id];
    return out;
}

QString sanitizeLanguageForFileName(const QString& code)
{
    QString out;
    for (const QChar c : code) {
        const char16_t u = c.unicode();
        const bool ok = (u >= u'A' && u <= u'Z') || (u >= u'a' && u <= u'z') || (u >= u'0' && u <= u'9') || u == u'.'
            || u == u'_' || u == u'-';
        out += ok ? c : QChar(u'_');
    }
    return out;
}

// ---- scenario file ---------------------------------------------------------------

namespace {

std::optional<qint64> integerOf(const json::Value& v)
{
    if (!v.isNumber())
        return std::nullopt;
    const QString t = v.numberText();
    bool ok = false;
    const qint64 n = t.toLongLong(&ok);
    if (!ok)
        return std::nullopt;
    return n;
}

} // namespace

ScenarioFileInfo checkScenarioFile(const json::Value& doc)
{
    ScenarioFileInfo info;
    auto fail = [&](const QString& why) {
        info.valid = false;
        info.problem = why;
        return info;
    };
    if (!doc.isObject())
        return fail(QStringLiteral("it is not a JSON object"));

    const auto count = integerOf(doc.value(QStringLiteral("scenarioCount")));
    if (!count || *count < 1 || *count > 5)
        return fail(QStringLiteral("scenarioCount must be a whole number from 1 to 5"));
    const auto defId = integerOf(doc.value(QStringLiteral("scenarioDefaultId")));
    if (!defId || *defId < 0 || *defId >= *count)
        return fail(QStringLiteral("scenarioDefaultId must name one of the scenarios"));
    info.defaultId = int(*defId);

    const json::Value& sdl = doc.value(QStringLiteral("scenarioDefaultLanguage"));
    info.scenarioDefaultLanguage = canonicalPlayGoLanguage(sdl.toString());
    if (!sdl.isString() || sdl.toString().isEmpty() || info.scenarioDefaultLanguage.isEmpty())
        return fail(QStringLiteral("scenarioDefaultLanguage is not a PlayGo language code"));

    const json::Value& scs = doc.value(QStringLiteral("scenarios"));
    if (!scs.isArray() || scs.size() != *count)
        return fail(QStringLiteral("scenarios must list exactly scenarioCount entries"));
    QStringList derived;
    std::set<qint64> ids;
    for (int i = 0; i < scs.size(); ++i) {
        const json::Value& sc = scs.at(i);
        if (!sc.isObject())
            return fail(QStringLiteral("scenario %1 is not an object").arg(i));
        const auto id = integerOf(sc.value(QStringLiteral("id")));
        if (!id || *id < 0 || *id >= *count || ids.count(*id))
            return fail(QStringLiteral("scenario ids must be unique and from 0 to scenarioCount - 1"));
        ids.insert(*id);
        const json::Value& type = sc.value(QStringLiteral("type"));
        if (!type.isString() || type.toString().isEmpty())
            return fail(QStringLiteral("scenario %1 has no type").arg(*id));
        ScenarioFileInfo::Scenario s;
        s.id = int(*id);
        s.type = type.toString();
        bool hasDefault = false;
        for (const QString& key : sc.keys()) {
            const QString canon = canonicalPlayGoLanguage(key);
            if (canon.isEmpty())
                continue;
            if (!derived.contains(canon))
                derived << canon;
            if (canon == info.scenarioDefaultLanguage && sc.value(key).isObject()) {
                hasDefault = true;
                s.title = sc.value(key).value(QStringLiteral("title")).toString();
            }
        }
        if (!hasDefault)
            return fail(QStringLiteral("scenario %1 has no text in %2").arg(*id).arg(info.scenarioDefaultLanguage));
        info.scenarios.push_back(s);
    }

    const json::Value& cdl = doc.value(QStringLiteral("chunkDefaultLanguage"));
    if (doc.contains(QStringLiteral("chunkDefaultLanguage"))) {
        info.chunkDefaultLanguage = canonicalPlayGoLanguage(cdl.toString());
        if (!cdl.isString() || info.chunkDefaultLanguage.isEmpty())
            return fail(QStringLiteral("chunkDefaultLanguage is not a PlayGo language code"));
    } else {
        info.chunkDefaultLanguage = info.scenarioDefaultLanguage;
    }

    if (doc.contains(QStringLiteral("chunkSupportedLanguages"))) {
        const json::Value& csl = doc.value(QStringLiteral("chunkSupportedLanguages"));
        if (!csl.isArray() || csl.size() == 0)
            return fail(QStringLiteral("chunkSupportedLanguages must be a non-empty list"));
        for (int i = 0; i < csl.size(); ++i) {
            const QString canon = canonicalPlayGoLanguage(csl.at(i).toString());
            if (!csl.at(i).isString() || canon.isEmpty())
                return fail(QStringLiteral("chunkSupportedLanguages contains an unknown language"));
            if (info.supportedLanguages.contains(canon))
                return fail(QStringLiteral("chunkSupportedLanguages lists %1 twice").arg(canon));
            info.supportedLanguages << canon;
        }
    } else {
        info.supportedLanguages << info.chunkDefaultLanguage;
        for (const QString& l : derived)
            if (!info.supportedLanguages.contains(l))
                info.supportedLanguages << l;
    }
    if (!info.supportedLanguages.contains(info.chunkDefaultLanguage))
        return fail(QStringLiteral("chunkDefaultLanguage is not one of the supported languages"));
    info.valid = true;
    return info;
}

QString defaultScenarioDescription()
{
    return QStringLiteral("All game data is installed before the game starts.");
}

json::Value defaultScenarioObject(const QString& language)
{
    json::Value o = json::Value::object();
    o.insert(QStringLiteral("chunkDefaultLanguage"), json::Value(language));
    json::Value langs = json::Value::array();
    langs.append(json::Value(language));
    o.insert(QStringLiteral("chunkSupportedLanguages"), langs);
    o.insert(QStringLiteral("scenarioCount"), json::Value(1));
    o.insert(QStringLiteral("scenarioDefaultId"), json::Value(0));
    o.insert(QStringLiteral("scenarioDefaultLanguage"), json::Value(language));
    json::Value sc = json::Value::object();
    sc.insert(QStringLiteral("id"), json::Value(0));
    sc.insert(QStringLiteral("type"), json::Value(QStringLiteral("playmode")));
    json::Value text = json::Value::object();
    text.insert(QStringLiteral("title"), json::Value(QStringLiteral("Scenario #0")));
    text.insert(QStringLiteral("description"), json::Value(defaultScenarioDescription()));
    sc.insert(language, text);
    json::Value arr = json::Value::array();
    arr.append(sc);
    o.insert(QStringLiteral("scenarios"), arr);
    return o;
}

QByteArray scenarioFileBytes(const json::Value& doc)
{
    return toCrlf(json::write(doc, {2, false, true, /*pythonFloats=*/true}));
}

// ---- source table ------------------------------------------------------------------

namespace {

quint16 u16(const QByteArray& d, qsizetype off) { return qFromLittleEndian<quint16>(d.constData() + off); }
quint32 u32(const QByteArray& d, qsizetype off) { return qFromLittleEndian<quint32>(d.constData() + off); }
quint64 u64(const QByteArray& d, qsizetype off) { return qFromLittleEndian<quint64>(d.constData() + off); }

struct Section {
    quint32 offset = 0;
    quint32 size = 0;
};

QString cString(const QByteArray& d, const Section& s, quint32 rel)
{
    if (rel >= s.size)
        return {};
    const qsizetype start = qsizetype(s.offset) + rel;
    const qsizetype end = qsizetype(s.offset) + s.size;
    qsizetype i = start;
    while (i < end && d[i] != '\0')
        ++i;
    return QString::fromLatin1(d.constData() + start, int(i - start));
}

// A label from the dump for a project attribute: a control character (other
// than tab and line breaks, which are escaped) would make the project
// invalid XML.
QString projectLabel(QString label)
{
    for (QChar& c : label)
        if (c.unicode() < 0x20 && c != u'\t' && c != u'\r' && c != u'\n')
            c = u'_';
    return label;
}

} // namespace

Result<PlayGoTable> parsePlayGoChunkDat(const QByteArray& d)
{
    auto bad = [](const QString& why) {
        return Error(ErrorCode::CorruptData, QStringLiteral("sce_sys/playgo-chunk.dat could not be used: %1.").arg(why));
    };
    if (d.size() < 0x100)
        return bad(QStringLiteral("the file is too small"));
    if (!d.startsWith("plgx"))
        return bad(QStringLiteral("it does not start with \"plgx\""));
    if (u16(d, 0x04) != 0x1000)
        return bad(QStringLiteral("unknown version 0x%1").arg(u16(d, 0x04), 4, 16, QLatin1Char('0')));
    if (u32(d, 0x10) != quint32(d.size()))
        return bad(QStringLiteral("the stored length does not match the file size"));
    PlayGoTable t;
    t.chunkCount = u16(d, 0x0A);
    t.scenarioCount = u16(d, 0x0E);
    t.defaultScenario = u16(d, 0x14);
    t.defaultLanguageId = u16(d, 0x24);
    t.languageMask = u64(d, 0x38);
    if (t.chunkCount < 1 || t.chunkCount > kMaxChunks)
        return bad(QStringLiteral("chunk count %1 is outside 1-%2").arg(t.chunkCount).arg(kMaxChunks));
    if (t.scenarioCount < 1 || t.scenarioCount > 64)
        return bad(QStringLiteral("scenario count %1 is outside 1-64").arg(t.scenarioCount));
    if (t.defaultScenario >= t.scenarioCount)
        return bad(QStringLiteral("the default scenario does not exist"));
    if (t.defaultLanguageId >= playGoLanguages().size())
        return bad(QStringLiteral("unknown default language %1").arg(t.defaultLanguageId));
    if (t.languageMask != 0 && !(t.languageMask & (quint64(1) << (63 - t.defaultLanguageId))))
        return bad(QStringLiteral("the language mask does not contain the default language"));

    auto section = [&](qsizetype at, Section& s) {
        s.offset = u32(d, at);
        s.size = u32(d, at + 4);
        return qsizetype(s.offset) + qsizetype(s.size) <= d.size();
    };
    Section chunkAttrs, chunkLabels, scenarioAttrs, scenarioChunks, scenarioLabels;
    if (!section(0xC0, chunkAttrs) || !section(0xD0, chunkLabels) || !section(0xE0, scenarioAttrs)
        || !section(0xE8, scenarioChunks) || !section(0xF0, scenarioLabels))
        return bad(QStringLiteral("a section lies outside the file"));
    if (chunkAttrs.size < quint32(t.chunkCount) * 0x20)
        return bad(QStringLiteral("the chunk table is too small"));
    if (scenarioAttrs.size < quint32(t.scenarioCount) * 0x20)
        return bad(QStringLiteral("the scenario table is too small"));

    for (int i = 0; i < t.chunkCount; ++i) {
        const qsizetype a = qsizetype(chunkAttrs.offset) + i * 0x20;
        PlayGoTable::Chunk c;
        c.languageMask = u64(d, a + 0x10);
        c.label = cString(d, chunkLabels, u32(d, a + 0x1C));
        t.chunks.push_back(c);
    }
    for (int i = 0; i < t.scenarioCount; ++i) {
        const qsizetype a = qsizetype(scenarioAttrs.offset) + i * 0x20;
        PlayGoTable::Scenario s;
        s.type = quint8(d[a]);
        s.initialChunkCount = u16(d, a + 0x14);
        const int n = u16(d, a + 0x16);
        const quint32 off = u32(d, a + 0x18);
        s.label = cString(d, scenarioLabels, u32(d, a + 0x1C));
        if (quint64(off) + quint64(n) * 2 > scenarioChunks.size)
            return bad(QStringLiteral("scenario %1 lists chunks outside its table").arg(i));
        for (int k = 0; k < n; ++k) {
            const int id = u16(d, qsizetype(scenarioChunks.offset) + off + k * 2);
            if (id >= t.chunkCount)
                return bad(QStringLiteral("scenario %1 names chunk %2, which does not exist").arg(i).arg(id));
            s.chunks.push_back(id);
        }
        t.scenarios.push_back(s);
    }
    return t;
}

// The PS5 flat-path-table path hash. Ported from LibProsperoPkg
// (ProsperoPs5FlatPathTable.HashPath, Copyright (C) 2026 SvenGDK,
// GPL-3.0-or-later, github.com/drakmor/LibProsperoPKG). playgo-hash-table.dat
// stores this hash of each package path, with the seed and key of its header
// (the reference toolkit's playgo_path_hash).
namespace {
constexpr quint64 kSeed0 = 0x92ca8aab26a24f51ULL;
constexpr quint64 kSeed1 = 0x09bbb761a41bc44dULL;
constexpr quint64 kRound = 0x8000000080008081ULL;
inline quint64 rotl(quint64 x, int n) { return (x << n) | (x >> (64 - n)); }
inline quint64 rotr(quint64 x, int n) { return (x >> n) | (x << (64 - n)); }

quint64 flatHashBytes(const QByteArray& str, quint64 seed, quint64 key)
{
    const qsizetype len = str.size();
    quint64 s0 = seed, s1 = rotl(seed, 11), s2 = rotl(seed, 23);
    quint64 tail = 0;
    if (len != 0) {
        const qsizetype nwords = (len - 1) >> 3;
        quint64 a0 = s0, a1 = s1, a2 = s2;
        qsizetype off = 0;
        for (qsizetype i = 0; i < nwords; ++i) {
            const quint64 w = qFromLittleEndian<quint64>(str.constData() + off);
            off += 8;
            a0 ^= w;
            const quint64 t18 = rotr(rotl(a2 ^ a1, 5) ^ a0, 11);
            const quint64 t12 = rotl(rotl(a2 ^ a0, 17) ^ a1, 11);
            a2 = rotr(rotl(a1 ^ a0, 1) ^ a2, 5);
            a0 = (~t12 & a2) ^ t18 ^ kRound;
            a1 = (~a2 & t18) ^ t12;
            a2 = (~t18 & t12) ^ a2;
        }
        s0 = a0;
        s1 = a1;
        s2 = a2;
        const qsizetype tlen = ((len - 1) & 7) + 1;
        for (qsizetype j = 0; j < tlen; ++j)
            tail |= quint64(quint8(str[off + j])) << (8 * j);
    }
    const quint64 u16v = s1;
    quint64 u11 = s2;
    const quint64 u6 = tail ^ s0 ^ key;
    const quint64 u17 = rotl(u11 ^ u16v, 5) ^ u6;
    const quint64 u18 = rotl(u11 ^ u6, 17) ^ u16v;
    u11 = rotl(u16v ^ u6, 1) ^ u11;
    return (~rotl(u18, 11) & rotr(u11, 5)) ^ rotr(u17, 11) ^ kRound;
}

bool hasFltMagic(const QByteArray& d) { return d.size() >= 28 && d.mid(24, 4) == QByteArray("\x7F" "FLT", 4); }

// Characters a project attribute cannot carry.
bool hasInvalidXmlChar(const QString& s)
{
    for (const QChar c : s) {
        const char16_t u = c.unicode();
        if ((u < 0x20 && u != u'\t' && u != u'\n' && u != u'\r') || u == 0xFFFE || u == 0xFFFF)
            return true;
    }
    return false;
}

// A NUL-terminated UTF-8 string at `at` inside the section [off, off + size).
std::optional<QString> sectionString(const QByteArray& d, qsizetype off, qsizetype size, quint32 at)
{
    if (qsizetype(at) >= size)
        return std::nullopt;
    const qsizetype start = off + at;
    const qsizetype end = off + size;
    qsizetype i = start;
    while (i < end && d[i] != '\0')
        ++i;
    if (i >= end)
        return std::nullopt;
    QStringDecoder dec(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
    const QString s = dec.decode(QByteArrayView(d.constData() + start, i - start));
    if (dec.hasError() || hasInvalidXmlChar(s))
        return std::nullopt;
    return s;
}
} // namespace

quint64 ps5FlatPathHash(const QString& path)
{
    QString p = path;
    if (p.startsWith(u'/'))
        p.remove(0, 1);
    return flatHashBytes(p.toUpper().toLatin1(), kSeed0, kSeed1);
}

quint64 playGoPathHash(const QString& rel, quint64 seed, quint64 key)
{
    // The path as UTF-8 with only a-z upper-cased, no leading slash.
    return flatHashBytes(rel.toUtf8().toUpper(), seed, key);
}

int ChunkMap::chunkOf(const QString& rel) const
{
    if (chunkByHash.isEmpty())
        return 0;
    auto it = chunkByHash.constFind(ps5FlatPathHash(u'/' + rel));
    if (it != chunkByHash.cend())
        return *it;
    it = chunkByHash.constFind(ps5FlatPathHash(rel));
    return it != chunkByHash.cend() ? *it : 0;
}

ChunkMap parseChunkMap(const QByteArray& ficm, const QByteArray& ht, int chunkCount)
{
    ChunkMap m;
    if (hasFltMagic(ficm) && !hasFltMagic(ht)) {
        m.misnamed = true;
        m.problem = QStringLiteral("playgo-ficm.dat holds the path hash table and playgo-hash-table.dat the chunk map "
                                   "(the files are swapped)");
        return m;
    }
    if (ficm.size() < 16 || u32(ficm, 0) != 1 || u32(ficm, 8) != 16) {
        m.problem = QStringLiteral("playgo-ficm.dat has an unknown header");
        return m;
    }
    if (ht.size() < 56 || u32(ht, 0) != 1 || u32(ht, 8) != 56 || !hasFltMagic(ht)) {
        m.problem = QStringLiteral("playgo-hash-table.dat has an unknown header");
        return m;
    }
    const quint32 n = u32(ficm, 12);
    qsizetype entries;
    if (16 + qsizetype(n) * 2 <= ficm.size())
        entries = n;
    else if (16 + qsizetype(n) <= ficm.size() && n % 2 == 0)
        entries = n / 2;
    else
        entries = (ficm.size() - 16) / 2;
    const qsizetype hashes = std::min<qsizetype>(u32(ht, 36), (ht.size() - 56) / 8);
    const qsizetype count = std::min(entries, hashes);
    int outOfRange = 0;
    for (qsizetype i = 0; i < count; ++i) {
        const int chunk = u16(ficm, 16 + i * 2);
        const quint64 h = u64(ht, 56 + i * 8);
        if (chunk >= chunkCount) {
            ++outOfRange;
            continue;
        }
        m.chunkByHash.insert(h, chunk);
    }
    if (outOfRange > 0)
        m.problem = QStringLiteral("%1 file(s) name a chunk that does not exist and were put in chunk 0").arg(outOfRange);
    return m;
}

// ---- the dump's own layout ---------------------------------------------------------------

Result<int> sourcePlayGoChunkCount(const QByteArray& d)
{
    if (d.size() < 0x100 || !d.startsWith("plgx") || u16(d, 0x04) != 0x1000 || u16(d, 0x06) != 0
        || u32(d, 0x10) != quint32(d.size()))
        return Error(ErrorCode::CorruptData, QStringLiteral("it is not a PS5 PlayGo chunk table"));
    const int chunks = u16(d, 0x0A);
    const int scenarios = u16(d, 0x0E);
    if (chunks < 1 || chunks > kMaxChunks || scenarios < 1 || scenarios > kMaxScriptScenarios)
        return Error(ErrorCode::CorruptData,
                     QStringLiteral("it names %1 chunk(s) and %2 scenario(s); 1-%3 chunks and 1-%4 scenarios are supported")
                         .arg(chunks).arg(scenarios).arg(kMaxChunks).arg(kMaxScriptScenarios));
    return chunks;
}

QStringList SourcePlayGoLayout::languages() const
{
    quint64 present = 0;
    for (const Chunk& c : chunks)
        for (const QString& l : c.languages) {
            const int id = playGoLanguageId(l);
            if (id >= 0)
                present |= quint64(1) << (63 - id);
        }
    return languagesInMask(present);
}

quint64 SourcePlayGoLayout::hashOf(const QString& rel) const { return playGoPathHash(rel, seed, key); }

std::optional<int> SourcePlayGoLayout::chunkOf(const QString& rel, quint64* hash) const
{
    const auto find = [&](const QString& candidate) -> std::optional<int> {
        const quint64 h = hashOf(candidate);
        const auto it = fileChunks.constFind(h);
        if (it == fileChunks.cend())
            return std::nullopt;
        if (hash)
            *hash = h;
        return *it;
    };
    if (const auto c = find(rel))
        return c;
    // Sony's publisher turns the presentation PNGs into .dds entries, so the
    // retail file map usually names the .dds.
    if (rel.startsWith(QStringLiteral("sce_sys/"), Qt::CaseInsensitive) && rel.endsWith(QStringLiteral(".png"), Qt::CaseInsensitive))
        return find(rel.left(rel.size() - 4) + QStringLiteral(".dds"));
    return std::nullopt;
}

namespace {

// Scenario labels, initial counts and ordered chunk lists of the table, or
// nullopt (with a warning) when its scenario sections cannot be read.
std::optional<std::vector<SourcePlayGoLayout::Scenario>> readSourceScenarios(const QByteArray& d, int chunkCount, QStringList* warnings)
{
    const auto fail = [&](const QString& why) -> std::optional<std::vector<SourcePlayGoLayout::Scenario>> {
        if (warnings)
            *warnings << QStringLiteral("PlayGo: the scenario part of sce_sys/playgo-chunk.dat was not used (%1); the scenario "
                                        "names come from playgo-scenario.json.").arg(why);
        return std::nullopt;
    };
    const qsizetype size = d.size();
    const int scenarioCount = u16(d, 0x0E);
    const qsizetype tableAt = u32(d, 0xE0), tableSize = u32(d, 0xE4);
    const qsizetype listsAt = u32(d, 0xE8), listsSize = u32(d, 0xEC);
    const qsizetype namesAt = u32(d, 0xF0), namesSize = u32(d, 0xF4);
    if (tableSize != qsizetype(scenarioCount) * 0x20 || std::min({tableAt, listsAt, namesAt}) < 0x100
        || std::max({tableAt + tableSize, listsAt + listsSize, namesAt + namesSize}) > size)
        return fail(QStringLiteral("its sections do not match the header"));
    std::vector<SourcePlayGoLayout::Scenario> out;
    for (int i = 0; i < scenarioCount; ++i) {
        const qsizetype e = tableAt + qsizetype(i) * 0x20;
        const int initial = u16(d, e + 0x14);
        const int total = u16(d, e + 0x16);
        const qsizetype listAt = u32(d, e + 0x18);
        const quint32 nameAt = u32(d, e + 0x1C);
        if (listAt + qsizetype(total) * 2 > listsSize || initial > total)
            return fail(QStringLiteral("scenario %1 points outside its section").arg(i));
        const auto label = sectionString(d, namesAt, namesSize, nameAt);
        if (!label)
            return fail(QStringLiteral("the name of scenario %1 cannot be read").arg(i));
        SourcePlayGoLayout::Scenario s;
        s.label = *label;
        s.initialChunkCount = initial;
        std::vector<bool> seen(size_t(chunkCount), false);
        for (int k = 0; k < total; ++k) {
            const int id = u16(d, listsAt + listAt + qsizetype(k) * 2);
            if (id >= chunkCount || seen[size_t(id)])
                return fail(QStringLiteral("scenario %1 has an invalid chunk list").arg(i));
            seen[size_t(id)] = true;
            s.chunks.push_back(id);
        }
        out.push_back(s);
    }
    return out;
}

} // namespace

Result<std::optional<SourcePlayGoLayout>> readSourcePlayGoLayout(const QByteArray& chunkDat, const QByteArray& ficm,
                                                                  const QByteArray& ht, int chunkCount, QStringList* warnings)
{
    const auto bad = [](const QString& why) { return Error(ErrorCode::CorruptData, why); };
    if (hasFltMagic(ficm) && !hasFltMagic(ht))
        return bad(QStringLiteral("playgo-ficm.dat holds the path hash table instead of the chunk map (the files are "
                                  "swapped or misnamed)"));
    if (ht.size() < 0x38 || u32(ht, 0) != 1 || !hasFltMagic(ht) || u32(ht, 0x08) != 0x38)
        return bad(QStringLiteral("playgo-hash-table.dat has an unknown header"));
    const quint64 fileCount = u32(ht, 0x24);
    if (quint64(u32(ht, 0x0C)) != fileCount * 8 || quint64(ht.size()) != 0x38 + fileCount * 8)
        return bad(QStringLiteral("playgo-hash-table.dat does not hold the %1 entries it names").arg(fileCount));
    if (ficm.size() < 0x10 || u32(ficm, 0) != 1 || u32(ficm, 0x08) != 0x10 || quint64(u32(ficm, 0x0C)) != fileCount * 2
        || quint64(ficm.size()) != 0x10 + fileCount * 2)
        return bad(QStringLiteral("playgo-ficm.dat does not describe the %1 files of the hash table").arg(fileCount));

    SourcePlayGoLayout l;
    l.seed = u64(ht, 0x28);
    l.key = u64(ht, 0x30);
    l.fileChunks.reserve(qsizetype(fileCount));
    bool anyOutsideChunk0 = false;
    quint64 previous = 0;
    for (quint64 i = 0; i < fileCount; ++i) {
        const quint64 h = u64(ht, 0x38 + qsizetype(i) * 8);
        const int chunk = u16(ficm, 0x10 + qsizetype(i) * 2);
        if (i > 0 && h <= previous)
            return bad(QStringLiteral("the hashes of playgo-hash-table.dat are not in rising order"));
        if (chunk >= chunkCount)
            return bad(QStringLiteral("playgo-ficm.dat names chunk %1, but the table has %2 chunk(s)").arg(chunk).arg(chunkCount));
        previous = h;
        l.fileChunks.insert(h, chunk);
        anyOutsideChunk0 = anyOutsideChunk0 || chunk != 0;
    }
    // Several chunks but every file in chunk 0: there is no layout to keep.
    if (chunkCount > 1 && !anyOutsideChunk0)
        return std::optional<SourcePlayGoLayout>();

    const QByteArray& d = chunkDat;
    const qsizetype size = d.size();
    if (size < 0x100 || !d.startsWith("plgx") || u16(d, 0x04) != 0x1000 || u16(d, 0x06) != 0 || u32(d, 0x10) != quint32(size)
        || u16(d, 0x0A) != chunkCount)
        return bad(QStringLiteral("playgo-chunk.dat is cut short or does not match its header"));
    const qsizetype attrsAt = u32(d, 0xC0), attrsSize = u32(d, 0xC4);
    const qsizetype labelsAt = u32(d, 0xD0), labelsSize = u32(d, 0xD4);
    if (attrsAt < 0x100 || attrsSize != qsizetype(chunkCount) * 0x20 || attrsAt + attrsSize > size)
        return bad(QStringLiteral("the chunk table of playgo-chunk.dat does not match its %1 chunk(s)").arg(chunkCount));
    if (labelsAt < 0x100 || labelsAt + labelsSize > size
        || std::max(attrsAt, labelsAt) < std::min(attrsAt + attrsSize, labelsAt + labelsSize))
        return bad(QStringLiteral("the chunk names of playgo-chunk.dat lie outside the file or overlap the chunk table"));

    quint64 knownMask = 0;
    for (int id = 0; id < playGoLanguages().size(); ++id)
        knownMask |= quint64(1) << (63 - id);
    for (int c = 0; c < chunkCount; ++c) {
        const qsizetype e = attrsAt + qsizetype(c) * 0x20;
        const quint64 mask = u64(d, e + 0x10);
        const auto label = sectionString(d, labelsAt, labelsSize, u32(d, e + 0x1C));
        if (!label)
            return bad(QStringLiteral("the name of chunk %1 in playgo-chunk.dat cannot be read").arg(c));
        SourcePlayGoLayout::Chunk chunk;
        chunk.languages = languagesInMask(mask);
        if ((mask & ~knownMask) != 0 && mask != ~quint64(0) && warnings)
            *warnings << QStringLiteral("PlayGo chunk %1 names languages this toolkit does not know (mask 0x%2); it is "
                                        "written for the known ones only.").arg(c).arg(mask, 16, 16, QLatin1Char('0'));
        chunk.label = !label->isEmpty() ? *label : chunk.languages.isEmpty() ? QString() : QStringLiteral("Chunk #%1").arg(c);
        l.chunks.push_back(chunk);
    }
    l.scenarios = readSourceScenarios(d, chunkCount, warnings);
    return std::optional<SourcePlayGoLayout>(std::move(l));
}

// ---- planning ------------------------------------------------------------------------

QString playGoLayoutName(PlayGoLayout l)
{
    switch (l) {
    case PlayGoLayout::Minimal: return QStringLiteral("A (one chunk, one scenario)");
    case PlayGoLayout::Script: return QStringLiteral("B (generated chunks)");
    case PlayGoLayout::SourceTable: return QStringLiteral("C (the dump's own chunks)");
    }
    return {};
}

std::optional<int> PlayGoPlan::fileChunk(const QString& rel) const
{
    if (layout == PlayGoLayout::Minimal)
        return std::nullopt;
    if (source)
        return source->chunkOf(rel).value_or(0);
    return 0;
}

namespace {

QString chunkRangeText(int n) { return n == 1 ? QStringLiteral("0") : QStringLiteral("0-%1").arg(n - 1); }

class NameAllocator {
public:
    explicit NameAllocator(const std::vector<std::pair<QString, qint64>>& files)
    {
        for (const auto& f : files)
            m_taken.insert(f.first.toLower());
    }
    QString take(const QString& dst)
    {
        QString cand = dst;
        const qsizetype dot = dst.lastIndexOf(u'.');
        for (int n = 2; m_taken.contains(cand.toLower()); ++n)
            cand = dst.left(dot) + QStringLiteral("-%1").arg(n) + dst.mid(dot);
        m_taken.insert(cand.toLower());
        return cand;
    }

private:
    QSet<QString> m_taken;
};

QString defaultLanguageFor(const PlayGoInputs& in)
{
    const QString l = canonicalPlayGoLanguage(in.paramDefaultLanguage);
    return l.isEmpty() ? QStringLiteral("en-US") : l;
}

void planMinimal(PlayGoPlan& plan, const PlayGoInputs& in)
{
    plan.layout = PlayGoLayout::Minimal;
    Gp5ChunkInfo& ci = plan.chunkInfo;
    ci.attrs = {{QStringLiteral("chunk_count"), QStringLiteral("1")}, {QStringLiteral("scenario_count"), QStringLiteral("1")}};
    ci.chunks.push_back({{{QStringLiteral("id"), QStringLiteral("0")}, {QStringLiteral("label"), QStringLiteral("Chunk #0")}}});
    ci.scenariosAttrs = {{QStringLiteral("default_id"), QStringLiteral("0")}};
    ci.scenarios.push_back({{{QStringLiteral("id"), QStringLiteral("0")},
                             {QStringLiteral("type"), QStringLiteral("playmode")},
                             {QStringLiteral("initial_chunk_count"), QStringLiteral("1")},
                             {QStringLiteral("label"), QStringLiteral("Scenario #0")}},
                            QStringLiteral("0")});
    plan.scenarioBytes = scenarioFileBytes(defaultScenarioObject(defaultLanguageFor(in)));
}

// The reference toolkit's --playgo-languages on a scenario document: the
// chunk languages become `selected`; the chunk default language stays when it
// is one of them, else en-US, else the first.
void applyLanguageSelection(json::Value& doc, const QStringList& selected)
{
    if (!doc.isObject() || selected.isEmpty())
        return;
    const QString chunkKey = QStringLiteral("chunkDefaultLanguage");
    const json::Value& original = doc.contains(chunkKey) ? doc.value(chunkKey) : doc.value(QStringLiteral("scenarioDefaultLanguage"));
    const QString canonical = original.isString() ? canonicalPlayGoLanguage(original.toString()) : QString();
    json::Value langs = json::Value::array();
    for (const QString& l : selected)
        langs.append(json::Value(l));
    doc.insert(QStringLiteral("chunkSupportedLanguages"), langs);
    doc.insert(chunkKey, json::Value(!canonical.isEmpty() && selected.contains(canonical) ? canonical
                                     : selected.contains(QStringLiteral("en-US"))        ? QStringLiteral("en-US")
                                                                                         : selected.first()));
}

// Package paths Sony's publisher writes itself or the builder always leaves
// out: a retail file map names them, the project never does.
const QStringList& publisherOwnPaths()
{
    static const QStringList paths = [] {
        QStringList p{QStringLiteral("sce_sys/about/right.sprx")};
        for (const char* n : {"keystone", "pfs-version.dat", "disc_info.dat", "ext_info.dat", "imagedigs.dat", "license.info",
                              "license.dat", "lem_notification.dat", "playgo-chunk.dat", "playgo-hash-table.dat",
                              "playgo-ficm.dat", "playgo-manifest.xml", "playgo-chunk.crc", "pfsimage.xml",
                              "pfs-region-hints.json", "param.sfo", "param_cp_values.json", "pronunciation.sig",
                              "origin-deltainfo.dat", "target-deltainfo.dat", "origin-param.json", "target-param.json",
                              "origin-relocinfo.dat", "target-relocinfo.dat"})
            p << QStringLiteral("sce_sys/") + QLatin1String(n);
        p << QStringLiteral("ampr_emu.index") << QStringLiteral("entitlements.txt") << QStringLiteral("entitlement_key.dat")
          << QStringLiteral("fakelib/libsceampr.sprx") << QStringLiteral("fakelib/libsceplaygo.sprx");
        return p;
    }();
    return paths;
}

// Layouts B and C: the scenario file, the chunk list, the scenarios, the
// language payloads (B) and the placeholders of empty chunks.
Status planToolkit(PlayGoPlan& plan, const PlayGoInputs& in, int n, std::optional<SourcePlayGoLayout> layout)
{
    const bool forced = !in.forcedLanguages.isEmpty();
    if (!layout && !forced && in.allLanguages && n < int(playGoLanguages().size()) + 1) {
        plan.notes << QStringLiteral("PlayGo: raising the chunk count from %1 to %2 so every language gets its own chunk.")
                          .arg(n).arg(playGoLanguages().size() + 1);
        n = int(playGoLanguages().size()) + 1;
    }
    if (n < 1 || n > kMaxChunks)
        return Error(ErrorCode::InvalidArgument, QStringLiteral("The PlayGo chunk count must be from 1 to %1.").arg(kMaxChunks));

    QStringList selected;
    if (layout) {
        selected = layout->languages();
    } else if (forced) {
        for (const QString& code : in.forcedLanguages)
            if (!canonicalPlayGoLanguage(code).isEmpty() && !selected.contains(canonicalPlayGoLanguage(code)))
                selected << canonicalPlayGoLanguage(code);
    } else if (in.allLanguages) {
        selected = playGoLanguages();
    }

    // The scenario file: the dump's own when it passes the rules, else one default scenario.
    ScenarioFileInfo info;
    json::Value doc;
    if (in.sourceScenario) {
        auto parsed = json::parse(*in.sourceScenario, {true, false, false, 64});
        if (parsed.ok()) {
            json::Value candidate = std::move(parsed).value();
            applyLanguageSelection(candidate, selected);
            info = checkScenarioFile(candidate);
            if (info.valid)
                doc = std::move(candidate);
        } else {
            info.problem = QStringLiteral("it is not valid JSON");
        }
        if (!info.valid)
            plan.warnings << QStringLiteral("sce_sys/playgo-scenario.json was not used (%1); the package gets one default "
                                            "scenario.").arg(info.problem);
    }
    if (!info.valid) {
        doc = defaultScenarioObject(defaultLanguageFor(in));
        applyLanguageSelection(doc, selected);
        info = checkScenarioFile(doc);
        if (!info.valid)
            return Error(ErrorCode::Internal,
                         QStringLiteral("The default PlayGo scenario failed validation (%1).").arg(info.problem));
    } else {
        plan.notes << QStringLiteral("scenarios taken from sce_sys/playgo-scenario.json (%1 scenario(s), %2 language(s))")
                          .arg(info.scenarios.size())
                          .arg(info.supportedLanguages.size());
    }
    if (!layout && !selected.isEmpty())
        plan.notes << QStringLiteral("PlayGo: %1 language(s) forced (%2)")
                          .arg(selected.size())
                          .arg(forced ? QStringLiteral("the base package's own set") : QStringLiteral("all PlayGo languages"));
    plan.scenarioBytes = scenarioFileBytes(doc);

    const QStringList& languages = info.supportedLanguages;
    const QString all = languages.join(u' ');
    plan.layout = layout ? PlayGoLayout::SourceTable : PlayGoLayout::Script;
    plan.chunkOnEveryFile = true;

    Gp5ChunkInfo& ci = plan.chunkInfo;
    ci.attrs = {{QStringLiteral("chunk_count"), QString::number(n)},
                {QStringLiteral("scenario_count"), QString::number(info.scenarios.size())}};
    ci.chunksAttrs = {{QStringLiteral("supported_languages"), all}, {QStringLiteral("default_language"), info.chunkDefaultLanguage}};

    std::vector<bool> declared(size_t(n), true);
    std::vector<bool> occupied(size_t(n), false);
    occupied[0] = true; // param.json, the scenario file and every file the map does not name
    NameAllocator names(in.files);
    const auto addChunk = [&](int id, const QString& label, const QStringList& assigned) {
        ci.chunks.push_back({{{QStringLiteral("id"), QString::number(id)},
                              {QStringLiteral("label"), label},
                              {QStringLiteral("layer_no"), QStringLiteral("0")},
                              {QStringLiteral("languages"), assigned.isEmpty() ? all : assigned.join(u' ')}}});
    };
    if (layout) {
        for (int c = 0; c < n; ++c) {
            const SourcePlayGoLayout::Chunk& ch = layout->chunks[size_t(c)];
            // A chunk the table neither names nor gives a language is not declared.
            declared[size_t(c)] = ch.declared();
            if (ch.declared())
                addChunk(c, ch.label, ch.languages);
        }
    } else {
        // Language i (counted from 1) goes to chunk min(i, N - 1): chunk 0 keeps
        // the game data for every language; the last chunk takes the rest.
        const auto chunkFor = [n](int languageNumber) { return std::min(languageNumber, n - 1); };
        for (int c = 0; c < n; ++c) {
            QStringList assigned;
            for (int i = 0; i < languages.size(); ++i)
                if (chunkFor(i + 1) == c)
                    assigned << languages[i];
            addChunk(c, QStringLiteral("Chunk #%1").arg(c), assigned);
        }
        for (int i = 0; i < languages.size(); ++i) {
            PlannedPayload p;
            p.chunk = chunkFor(i + 1);
            p.language = languages[i];
            p.dst = names.take(QStringLiteral("playgo-languages/%1-%2.bin")
                                   .arg(i + 1, 2, 10, QLatin1Char('0'))
                                   .arg(sanitizeLanguageForFileName(languages[i])));
            occupied[size_t(p.chunk)] = true;
            plan.payloads.push_back(p);
        }
    }

    // Every scenario lists every chunk and every chunk is initial: a package is
    // installed in full, and a chunk outside the running scenario would count
    // as not installed. The scenario names come from the dump's table when it
    // has as many scenarios as playgo-scenario.json.
    const std::vector<SourcePlayGoLayout::Scenario>* tableScenarios =
        layout && layout->scenarios ? &*layout->scenarios : nullptr;
    if (tableScenarios && tableScenarios->size() != info.scenarios.size()) {
        plan.warnings << QStringLiteral("PlayGo: the dump's chunk table has %1 scenario(s) but playgo-scenario.json declares %2; "
                                        "the names of playgo-scenario.json are used.")
                             .arg(tableScenarios->size())
                             .arg(info.scenarios.size());
        tableScenarios = nullptr;
    }
    ci.scenariosAttrs = {{QStringLiteral("default_id"), QString::number(info.defaultId)}};
    auto scenarios = info.scenarios;
    std::stable_sort(scenarios.begin(), scenarios.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    for (const auto& s : scenarios) {
        QString label = tableScenarios ? (*tableScenarios)[size_t(s.id)].label : QString();
        if (label.isEmpty())
            label = s.title.trimmed();
        if (label.isEmpty())
            label = QStringLiteral("Scenario #%1").arg(s.id);
        ci.scenarios.push_back({{{QStringLiteral("id"), QString::number(s.id)},
                                 {QStringLiteral("type"), s.type},
                                 {QStringLiteral("initial_chunk_count"), QString::number(n)},
                                 {QStringLiteral("label"), projectLabel(label)}},
                                chunkRangeText(n)});
    }

    if (layout) {
        // Where the dump's map puts each mapped file.
        QSet<quint64> placed;
        int unmapped = 0;
        QStringList sample, unexpected;
        for (const auto& f : in.files) {
            quint64 h = 0;
            if (const auto c = layout->chunkOf(f.first, &h)) {
                placed.insert(h);
                if (f.second > 0)
                    occupied[size_t(*c)] = true;
                continue;
            }
            ++unmapped;
            if (sample.size() < 5)
                sample << f.first;
            if (!f.first.startsWith(QStringLiteral("sce_sys/"), Qt::CaseInsensitive))
                unexpected << f.first;
        }
        plan.notes << QStringLiteral("PlayGo: kept the dump's layout (%1 chunk(s), languages %2); %3 of %4 files placed from "
                                     "playgo-ficm.dat")
                          .arg(n).arg(all).arg(qsizetype(in.files.size()) - unmapped).arg(in.files.size());
        if (unmapped > 0)
            plan.notes << QStringLiteral("PlayGo: %1 file(s) the dump's map does not name went to chunk 0: %2%3")
                              .arg(unmapped).arg(sample.join(QStringLiteral(", ")), unmapped > 5 ? QStringLiteral(", ...") : QString());
        if (!unexpected.isEmpty()) {
            plan.warnings << QStringLiteral("PlayGo: %1 game file(s) are not in the dump's own file map and were put in chunk 0 "
                                            "(for example %2). Files added to a dump are expected here.")
                                 .arg(unexpected.size()).arg(unexpected.first());
            for (qsizetype i = 0; i < std::min<qsizetype>(unexpected.size(), 25); ++i)
                plan.notes << QStringLiteral("PlayGo: not in the dump's map: %1").arg(unexpected[i]);
            if (unexpected.size() > 25)
                plan.notes << QStringLiteral("PlayGo: ... and %1 more").arg(unexpected.size() - 25);
        }
        // Chunks whose files are not all in the dump (a language that was not dumped).
        QSet<quint64> own;
        for (const QString& p : publisherOwnPaths())
            own.insert(layout->hashOf(p));
        std::vector<int> expected(size_t(n), 0), missing(size_t(n), 0);
        for (auto it = layout->fileChunks.cbegin(); it != layout->fileChunks.cend(); ++it) {
            if (own.contains(it.key()))
                continue;
            ++expected[size_t(it.value())];
            if (!placed.contains(it.key()))
                ++missing[size_t(it.value())];
        }
        QStringList incomplete;
        for (int c = 0; c < n; ++c)
            if (missing[size_t(c)] > 0) {
                const SourcePlayGoLayout::Chunk& ch = layout->chunks[size_t(c)];
                const QString line = QStringLiteral("chunk %1 (%2%3) lacks %4 of its %5 file(s)")
                                         .arg(c)
                                         .arg(ch.label, ch.languages.isEmpty() ? QString() : QStringLiteral("; ") + ch.languages.join(u' '))
                                         .arg(missing[size_t(c)])
                                         .arg(expected[size_t(c)]);
                plan.notes << QStringLiteral("PlayGo: %1").arg(line);
                if (incomplete.size() < 3)
                    incomplete << line;
            }
        if (!incomplete.isEmpty())
            plan.warnings << QStringLiteral("PlayGo: the dump does not hold every file of the original package: %1%2. The "
                                            "package still declares these chunks; a language that needs them gets "
                                            "incomplete data.")
                                 .arg(incomplete.join(QStringLiteral("; ")),
                                      incomplete.size() < int(std::count_if(missing.begin(), missing.end(), [](int m) { return m > 0; }))
                                          ? QStringLiteral(" (and more, see the build log)")
                                          : QString());
    }

    // A chunk without data counts as not installed on the console.
    if (in.fillEmptyChunks && n >= 2)
        for (int c = 0; c < n; ++c) {
            if (occupied[size_t(c)] || !declared[size_t(c)])
                continue;
            PlannedPayload p;
            p.chunk = c;
            p.filler = true;
            p.dst = names.take(QStringLiteral("playgo-chunks/%1.bin").arg(c, 3, 10, QLatin1Char('0')));
            plan.payloads.push_back(p);
        }

    plan.source = std::move(layout);
    return success();
}

} // namespace

Result<PlayGoPlan> planPlayGo(const PlayGoInputs& in)
{
    PlayGoPlan plan;
    const bool generatedLanguages = !in.forcedLanguages.isEmpty() || in.allLanguages;
    int n = in.chunkCount;
    bool sourceCount = false;
    // A backport update keeps the base package's chunk count (in.chunkCount).
    if (in.useSourceTable && in.forcedLanguages.isEmpty() && in.chunkDat) {
        auto count = sourcePlayGoChunkCount(*in.chunkDat);
        if (count.ok()) {
            n = *count;
            sourceCount = true;
            plan.notes << QStringLiteral("PlayGo: %1 chunk(s), the count of the dump's sce_sys/playgo-chunk.dat").arg(n);
        } else {
            plan.warnings << QStringLiteral("sce_sys/playgo-chunk.dat was not used (%1); the package gets generated PlayGo "
                                            "chunks.").arg(count.error().message);
        }
    }
    std::optional<SourcePlayGoLayout> layout;
    if (sourceCount && generatedLanguages) {
        plan.notes << QStringLiteral("PlayGo: languages are forced, so the dump's chunk layout is replaced by generated chunks");
    } else if (sourceCount && in.ficm && in.hashTable) {
        auto l = readSourcePlayGoLayout(*in.chunkDat, *in.ficm, *in.hashTable, n, &plan.warnings);
        if (!l.ok()) {
            plan.warnings << QStringLiteral("PlayGo: the dump's file map was not used (%1). The package keeps the %2 chunk(s) "
                                            "of the dump with every file in chunk 0.").arg(l.error().message).arg(n);
        } else if (!l.value()) {
            plan.notes << QStringLiteral("PlayGo: the dump's file map puts every file in chunk 0; the chunks are generated");
        } else if (l.value()->languages().isEmpty()) {
            plan.warnings << QStringLiteral("PlayGo: the dump's chunks name no known language; the package keeps the %1 "
                                            "chunk(s) of the dump with every file in chunk 0.").arg(n);
        } else {
            layout = std::move(l).value();
        }
    } else if (sourceCount) {
        plan.notes << QStringLiteral("PlayGo: playgo-ficm.dat or playgo-hash-table.dat is missing; every file goes into chunk 0");
    }

    if (!sourceCount && !in.fallback)
        planMinimal(plan, in);
    else
        FW_TRY(planToolkit(plan, in, n, std::move(layout)));
    plan.notes.prepend(QStringLiteral("PlayGo layout %1").arg(playGoLayoutName(plan.layout)));
    return plan;
}

} // namespace fw::fpkg
