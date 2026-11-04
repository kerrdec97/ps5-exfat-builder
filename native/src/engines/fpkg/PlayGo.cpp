#include "engines/fpkg/PlayGo.h"

#include "engines/fpkg/ParamJson.h"

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
    return toCrlf(json::write(doc, {2, false, true}));
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
        return bad(QStringLiteral("chunk count %1 is outside 1-255").arg(t.chunkCount));
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
// GPL-3.0-or-later, github.com/drakmor/LibProsperoPKG). Whether the PlayGo
// hash table of real dumps uses this hash is UNVERIFIED.
namespace {
constexpr quint64 kSeed0 = 0x92ca8aab26a24f51ULL;
constexpr quint64 kSeed1 = 0x09bbb761a41bc44dULL;
constexpr quint64 kRound = 0x8000000080008081ULL;
inline quint64 rotl(quint64 x, int n) { return (x << n) | (x >> (64 - n)); }
inline quint64 rotr(quint64 x, int n) { return (x >> n) | (x << (64 - n)); }

quint64 flatHashBytes(const QByteArray& str)
{
    const qsizetype len = str.size();
    quint64 s0 = kSeed0, s1 = rotl(kSeed0, 11), s2 = rotl(kSeed0, 23);
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
    const quint64 u6 = tail ^ s0 ^ kSeed1;
    const quint64 u17 = rotl(u11 ^ u16v, 5) ^ u6;
    const quint64 u18 = rotl(u11 ^ u6, 17) ^ u16v;
    u11 = rotl(u16v ^ u6, 1) ^ u11;
    return (~rotl(u18, 11) & rotr(u11, 5)) ^ rotr(u17, 11) ^ kRound;
}
} // namespace

quint64 ps5FlatPathHash(const QString& path)
{
    QString p = path;
    if (p.startsWith(u'/'))
        p.remove(0, 1);
    return flatHashBytes(p.toUpper().toLatin1());
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
    const auto hasFlt = [](const QByteArray& d) { return d.size() >= 28 && d.mid(24, 4) == QByteArray("\x7F" "FLT", 4); };
    if (hasFlt(ficm) && !hasFlt(ht)) {
        m.misnamed = true;
        m.problem = QStringLiteral("playgo-ficm.dat holds the path hash table and playgo-hash-table.dat the chunk map "
                                   "(the files are swapped)");
        return m;
    }
    if (ficm.size() < 16 || u32(ficm, 0) != 1 || u32(ficm, 8) != 16) {
        m.problem = QStringLiteral("playgo-ficm.dat has an unknown header");
        return m;
    }
    if (ht.size() < 56 || u32(ht, 0) != 1 || u32(ht, 8) != 56 || !hasFlt(ht)) {
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
        const int chunk = quint8(ficm[16 + i * 2]);
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

// ---- planning ------------------------------------------------------------------------

QString playGoLayoutName(PlayGoLayout l)
{
    switch (l) {
    case PlayGoLayout::Minimal: return QStringLiteral("A (one chunk, one scenario)");
    case PlayGoLayout::Script: return QStringLiteral("B (generated chunks)");
    case PlayGoLayout::SourceTable: return QStringLiteral("C (the dump's chunk table)");
    }
    return {};
}

std::optional<int> PlayGoPlan::fileChunk(const QString& rel) const
{
    switch (layout) {
    case PlayGoLayout::Minimal: return std::nullopt;
    case PlayGoLayout::Script: return 0;
    case PlayGoLayout::SourceTable: {
        const int c = chunkMap.chunkOf(rel);
        return c > 0 ? std::optional<int>(c) : std::nullopt;
    }
    }
    return std::nullopt;
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

void addFillers(PlayGoPlan& plan, int chunkCount, const std::vector<bool>& hasData, NameAllocator& names)
{
    for (int c = 0; c < chunkCount; ++c) {
        if (hasData[size_t(c)])
            continue;
        PlannedPayload p;
        p.chunk = c;
        p.filler = true;
        p.dst = names.take(QStringLiteral("playgo-chunks/%1.bin").arg(c, 3, 10, QLatin1Char('0')));
        plan.payloads.push_back(p);
    }
}

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

Status planScript(PlayGoPlan& plan, const PlayGoInputs& in)
{
    plan.layout = PlayGoLayout::Script;
    plan.chunkOnEveryFile = true;
    const int n = in.chunkCount;
    if (n < 1 || n > kMaxChunks)
        return Error(ErrorCode::InvalidArgument, QStringLiteral("The PlayGo chunk count must be from 1 to 255."));

    ScenarioFileInfo info;
    json::Value doc;
    if (in.sourceScenario) {
        auto parsed = json::parse(*in.sourceScenario, {true, false, false, 64});
        if (parsed.ok()) {
            info = checkScenarioFile(parsed.value());
            if (info.valid)
                doc = std::move(parsed).value();
        } else {
            info.problem = QStringLiteral("it is not valid JSON");
        }
        if (!info.valid)
            plan.warnings << QStringLiteral("sce_sys/playgo-scenario.json was not used (%1); the package gets one default "
                                            "scenario.").arg(info.problem);
    }
    if (!info.valid) {
        const QString lang = defaultLanguageFor(in);
        doc = defaultScenarioObject(lang);
        info = checkScenarioFile(doc);
    } else {
        plan.notes << QStringLiteral("scenarios taken from sce_sys/playgo-scenario.json (%1 scenario(s), %2 language(s))")
                          .arg(info.scenarios.size())
                          .arg(info.supportedLanguages.size());
    }
    plan.scenarioBytes = scenarioFileBytes(doc);

    NameAllocator names(in.files);
    std::vector<QStringList> chunkLangs(static_cast<size_t>(n));
    std::vector<bool> hasData(size_t(n), false);
    for (const auto& f : in.files)
        if (f.second > 0)
            hasData[0] = true;
    for (int i = 0; i < info.supportedLanguages.size(); ++i) {
        const QString& lang = info.supportedLanguages[i];
        PlannedPayload p;
        p.chunk = std::min(i, n - 1);
        p.language = lang;
        p.dst = names.take(QStringLiteral("playgo-languages/%1-%2.bin")
                               .arg(i + 1, 2, 10, QLatin1Char('0'))
                               .arg(sanitizeLanguageForFileName(lang)));
        chunkLangs[size_t(p.chunk)] << lang;
        hasData[size_t(p.chunk)] = true;
        plan.payloads.push_back(p);
    }
    if (in.fillEmptyChunks)
        addFillers(plan, n, hasData, names);

    Gp5ChunkInfo& ci = plan.chunkInfo;
    ci.attrs = {{QStringLiteral("chunk_count"), QString::number(n)},
                {QStringLiteral("scenario_count"), QString::number(info.scenarios.size())}};
    ci.chunksAttrs = {{QStringLiteral("supported_languages"), info.supportedLanguages.join(u' ')},
                      {QStringLiteral("default_language"), info.chunkDefaultLanguage}};
    for (int c = 0; c < n; ++c) {
        const QStringList& langs = chunkLangs[size_t(c)].isEmpty() ? info.supportedLanguages : chunkLangs[size_t(c)];
        ci.chunks.push_back({{{QStringLiteral("id"), QString::number(c)},
                              {QStringLiteral("label"), QStringLiteral("Chunk #%1").arg(c)},
                              {QStringLiteral("layer_no"), QStringLiteral("0")},
                              {QStringLiteral("languages"), langs.join(u' ')}}});
    }
    ci.scenariosAttrs = {{QStringLiteral("default_id"), QString::number(info.defaultId)}};
    auto scenarios = info.scenarios;
    std::stable_sort(scenarios.begin(), scenarios.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    for (const auto& s : scenarios) {
        ci.scenarios.push_back({{{QStringLiteral("id"), QString::number(s.id)},
                                 {QStringLiteral("type"), s.type},
                                 {QStringLiteral("initial_chunk_count"), QString::number(n)},
                                 {QStringLiteral("label"), s.title.isEmpty() ? QStringLiteral("Scenario #%1").arg(s.id) : s.title}},
                                chunkRangeText(n)});
    }
    return success();
}

bool sourceScenarioMatchesTable(const QByteArray& bytes, const PlayGoTable& t)
{
    auto parsed = json::parse(bytes, {true, false, false, 64});
    if (!parsed.ok() || !parsed->isObject())
        return false;
    const json::Value& d = parsed.value();
    if (integerOf(d.value(QStringLiteral("scenarioCount"))) != std::optional<qint64>(t.scenarioCount))
        return false;
    if (integerOf(d.value(QStringLiteral("scenarioDefaultId"))) != std::optional<qint64>(t.defaultScenario))
        return false;
    const json::Value& scs = d.value(QStringLiteral("scenarios"));
    if (!scs.isArray() || scs.size() != t.scenarioCount)
        return false;
    for (int i = 0; i < scs.size(); ++i) {
        if (integerOf(scs.at(i).value(QStringLiteral("id"))) != std::optional<qint64>(i))
            return false;
        if (scs.at(i).value(QStringLiteral("type")).toString() != u"playmode")
            return false;
    }
    return true;
}

void planSourceTable(PlayGoPlan& plan, const PlayGoInputs& in, const PlayGoTable& t)
{
    plan.layout = PlayGoLayout::SourceTable;
    if (in.ficm && in.hashTable) {
        plan.chunkMap = parseChunkMap(*in.ficm, *in.hashTable, t.chunkCount);
        if (!plan.chunkMap.problem.isEmpty())
            plan.warnings << QStringLiteral("PlayGo file map: %1%2.")
                                 .arg(plan.chunkMap.problem,
                                      plan.chunkMap.chunkByHash.isEmpty() ? QStringLiteral("; every file is in chunk 0") : QString());
    } else {
        plan.warnings << QStringLiteral("playgo-ficm.dat or playgo-hash-table.dat is missing; every file is in chunk 0.");
    }

    const QString defLang = playGoLanguages().value(t.defaultLanguageId);
    const QStringList headerLangs = languagesInMask(t.languageMask);
    Gp5ChunkInfo& ci = plan.chunkInfo;
    ci.attrs = {{QStringLiteral("chunk_count"), QString::number(t.chunkCount)},
                {QStringLiteral("scenario_count"), QString::number(t.scenarioCount)}};
    if (t.languageMask != 0)
        ci.chunksAttrs = {{QStringLiteral("supported_languages"), headerLangs.join(u' ')},
                          {QStringLiteral("default_language"), defLang}};
    for (int c = 0; c < t.chunkCount; ++c) {
        const auto& ch = t.chunks[size_t(c)];
        Gp5Chunk gc;
        gc.attrs.emplace_back(QStringLiteral("id"), QString::number(c));
        if (ch.languageMask != t.languageMask)
            gc.attrs.emplace_back(QStringLiteral("languages"), languagesInMask(ch.languageMask).join(u' '));
        gc.attrs.emplace_back(QStringLiteral("label"), ch.label.isEmpty() ? QStringLiteral("Chunk #%1").arg(c) : ch.label);
        ci.chunks.push_back(gc);
    }
    ci.scenariosAttrs = {{QStringLiteral("default_id"), QString::number(t.defaultScenario)}};
    for (int s = 0; s < t.scenarioCount; ++s) {
        const auto& sc = t.scenarios[size_t(s)];
        std::vector<int> order;
        std::vector<bool> seen(size_t(t.chunkCount), false);
        for (int id : sc.chunks)
            if (!seen[size_t(id)]) {
                seen[size_t(id)] = true;
                order.push_back(id);
            }
        for (int id = 0; id < t.chunkCount; ++id)
            if (!seen[size_t(id)])
                order.push_back(id);
        QStringList ids;
        for (int id : order)
            ids << QString::number(id);
        ci.scenarios.push_back({{{QStringLiteral("id"), QString::number(s)},
                                 {QStringLiteral("type"), QStringLiteral("playmode")},
                                 {QStringLiteral("initial_chunk_count"), QString::number(t.chunkCount)},
                                 {QStringLiteral("label"), sc.label.isEmpty() ? QStringLiteral("Scenario #%1").arg(s) : sc.label}},
                                ids.join(u' ')});
    }

    if (in.sourceScenario && sourceScenarioMatchesTable(*in.sourceScenario, t)) {
        plan.scenarioBytes = *in.sourceScenario;
        plan.scenarioIsSourceVerbatim = true;
    } else {
        const QString lang = defLang.isEmpty() ? defaultLanguageFor(in) : defLang;
        json::Value o = json::Value::object();
        o.insert(QStringLiteral("chunkDefaultLanguage"), json::Value(lang));
        json::Value langs = json::Value::array();
        for (const QString& l : headerLangs.isEmpty() ? QStringList{lang} : headerLangs)
            langs.append(json::Value(l));
        o.insert(QStringLiteral("chunkSupportedLanguages"), langs);
        o.insert(QStringLiteral("scenarioCount"), json::Value(t.scenarioCount));
        o.insert(QStringLiteral("scenarioDefaultId"), json::Value(t.defaultScenario));
        o.insert(QStringLiteral("scenarioDefaultLanguage"), json::Value(lang));
        json::Value arr = json::Value::array();
        for (int s = 0; s < t.scenarioCount; ++s) {
            json::Value sc = json::Value::object();
            sc.insert(QStringLiteral("id"), json::Value(s));
            sc.insert(QStringLiteral("type"), json::Value(QStringLiteral("playmode")));
            json::Value text = json::Value::object();
            const QString label = t.scenarios[size_t(s)].label;
            text.insert(QStringLiteral("title"), json::Value(label.isEmpty() ? QStringLiteral("Scenario #%1").arg(s) : label));
            text.insert(QStringLiteral("description"), json::Value(defaultScenarioDescription()));
            sc.insert(lang, text);
            arr.append(sc);
        }
        o.insert(QStringLiteral("scenarios"), arr);
        plan.scenarioBytes = scenarioFileBytes(o);
        if (in.sourceScenario)
            plan.notes << QStringLiteral("sce_sys/playgo-scenario.json does not match the chunk table; a scenario file "
                                         "was generated from the table's labels");
    }

    if (in.fillEmptyChunks) {
        std::vector<bool> hasData(size_t(t.chunkCount), false);
        for (const auto& f : in.files)
            if (f.second > 0)
                hasData[size_t(plan.chunkMap.chunkOf(f.first))] = true;
        NameAllocator names(in.files);
        addFillers(plan, t.chunkCount, hasData, names);
    }
}

} // namespace

Result<PlayGoPlan> planPlayGo(const PlayGoInputs& in)
{
    PlayGoPlan plan;
    std::optional<PlayGoTable> table;
    if (in.useSourceTable && in.chunkDat) {
        auto t = parsePlayGoChunkDat(*in.chunkDat);
        if (t.ok())
            table = std::move(t).value();
        else
            plan.warnings << t.error().message;
    }
    if (table && !table->trivial()) {
        planSourceTable(plan, in, *table);
    } else if (table) {
        plan.notes << QStringLiteral("the dump's chunk table has one chunk and one scenario");
        planMinimal(plan, in);
    } else if (in.fallback) {
        FW_TRY(planScript(plan, in));
    } else {
        planMinimal(plan, in);
    }
    plan.notes.prepend(QStringLiteral("PlayGo layout %1").arg(playGoLayoutName(plan.layout)));
    return plan;
}

} // namespace fw::fpkg
