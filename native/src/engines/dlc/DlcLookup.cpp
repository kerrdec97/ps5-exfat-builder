#include "engines/dlc/DlcLookup.h"

#include "core/FileUtil.h"
#include "engines/dlc/DlcIdentity.h"

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSet>
#include <QThread>
#include <QTimeZone>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <iterator>
#include <memory>
#include <optional>

// Port notes. The Companion runs this as async .NET code; here every public
// call blocks its (worker) thread. The Companion's exception flow (an
// HttpRequestException/InvalidDataException/JsonException becomes a warning
// at a specific level, an OperationCanceledException from the 25 s budget
// becomes a "time limit" warning, a user cancel aborts) is kept with two
// private exception types that never leave this file. The five market
// searches run in parallel on their own threads, as in the Companion; the
// (at most four) product pages a market verifies are fetched one after the
// other.

namespace fw::dlc {

// The parsers and URL builders, with external linkage so the unit tests can
// declare and exercise them (tests/engines/dlc/tst_dlc_lookup.cpp repeats
// these declarations). Not part of the module's API.
namespace detail {
void setTimeoutsForTests(int requestMs, int overallMs); // defaults 6000 / 25000
QString htmlDecode(const QString& s);
QString htmlText(const QString& html);
bool isGameName(const QString& value);
bool isDlcDisplayLabel(const QString& value);
QString knownName(const QString& titleId);
QString serialStationUrl(const QString& query);
QString storeSearchUrl(const QString& query);
QString searchApiUrl(const Market& market, const QString& name);
QString searchPageUrl(const Market& market, const QString& name);
bool tryStoreProduct(const QString& input, QString* storeUrl, QString* contentId);
QString cacheFileName(const QString& key);
std::vector<DlcCandidate> parseStoreProduct(const QString& html, const QString& sourceUrl, const QString& expectedContentId);
QString parseStoreGameName(const QString& html, const QString& contentId);
QString parseStoreConceptId(const QString& html, const QString& contentId);
QString parseStoreProductKind(const QString& html, const QString& contentId);
QStringList storeProductSkus(const QString& html);
bool sameGameOrEdition(const QString& item, const QString& query, bool allowDemo);
QString refinedSearch(const QString& name);
int searchCount(const QByteArray& json);
Result<std::vector<DlcCandidate>> parseSearch(const QByteArray& json, const Market& market, const QString& query,
                                              bool conceptRequired);
std::vector<DlcCandidate> parseSerialTitle(const QString& html, const QString& titleId, const QString& sourceUrl, bool related);
std::vector<DlcCandidate> parseSerialSearch(const QString& html, const QString& mappingStatus);
QStringList serialLinks(const QString& html, const QString& pathPrefix);
bool parseTitleIndexHint(const QString& html, const QString& titleId, const QString& sourceContentId, QString* name,
                         QString* contentId);
} // namespace detail

namespace {

constexpr qint64 kMaxResponseBytes = 2LL * 1024 * 1024;
constexpr std::size_t kMaxCandidates = 100;
constexpr qint64 kFreshForMs = 7LL * 24 * 60 * 60 * 1000;
constexpr qsizetype kKeepCacheFiles = 128;

std::atomic<int> g_requestTimeoutMs{6000};
std::atomic<int> g_overallTimeoutMs{25000};

const QRegularExpression::PatternOptions kCi = QRegularExpression::CaseInsensitiveOption;
const QRegularExpression::PatternOptions kSingle = QRegularExpression::DotMatchesEverythingOption;
const QRegularExpression::PatternOptions kSingleCi =
    QRegularExpression::DotMatchesEverythingOption | QRegularExpression::CaseInsensitiveOption;

// A failure the Companion reports as a warning ("HTTP 404 from ...", "Lookup
// response exceeds the 2 MiB limit", a JSON error, ...).
struct LookupError {
    QString message;
};
// The overall budget ran out (user == false) or the user cancelled.
struct LookupInterrupted {
    bool user = false;
};
// An argument the Companion rejects with an ArgumentException: the whole
// lookup ends with only this message.
struct LookupRejected {
    QString message;
};

bool eqCi(const QString& a, const QString& b) { return a.compare(b, Qt::CaseInsensitive) == 0; }

const QRegularExpression& titlePattern()
{
    static const QRegularExpression re(QStringLiteral("(?<![A-Z0-9])(PPSA|CUSA)-?(\\d{5})(?![A-Z0-9])"), kCi);
    return re;
}

// The lookup service's own (case-insensitive) content ID pattern.
const QRegularExpression& contentPattern()
{
    static const QRegularExpression re(QStringLiteral("\\A[A-Z]{2}\\d{4}-(PPSA|CUSA)\\d{5}_\\d{2}-[A-Z0-9_]{16}\\z"), kCi);
    return re;
}

bool isLookupContentId(const QString& s) { return contentPattern().match(s).hasMatch(); }

bool isStrictCid(const QString& s)
{
    static const QRegularExpression re(QStringLiteral("\\A[A-Z]{2}\\d{4}-(PPSA|CUSA)\\d{5}_\\d{2}-[A-Z0-9_]{16}\\z"));
    return re.match(s).hasMatch();
}

bool isStrictTitle(const QString& s)
{
    static const QRegularExpression re(QStringLiteral("\\A(PPSA|CUSA)\\d{5}\\z"));
    return re.match(s).hasMatch();
}

bool isConceptId(const QString& s)
{
    static const QRegularExpression re(QStringLiteral("\\A\\d{1,12}\\z"));
    return re.match(s).hasMatch();
}

bool isSerialHost(const QString& host)
{
    return eqCi(host, QStringLiteral("serialstation.com")) || eqCi(host, QStringLiteral("www.serialstation.com"));
}

bool trustedSource(const QString& url)
{
    if (url.isEmpty())
        return false;
    return DlcLookupService::isTrustedSource(QUrl(url));
}

QString escaped(const QString& s) { return QString::fromLatin1(QUrl::toPercentEncoding(s)); }

QString universalSortable(const QDateTime& t)
{
    return t.toUTC().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")) + QLatin1Char('Z');
}

QString requestTimedOutText(int ms)
{
    QString s = QString::number(ms / 1000.0, 'f', 1);
    if (s.endsWith(QStringLiteral(".0")))
        s.chop(2);
    return QStringLiteral("Request timed out after %1 seconds").arg(s);
}

QString httpErrorText(int status, const QString& host)
{
    return QStringLiteral("HTTP %1 from %2%3")
        .arg(status)
        .arg(host, status == 403 ? QStringLiteral(" (browser challenge or access restriction)") : QString());
}

bool looksLikeChallenge(const QString& body)
{
    return body.contains(QStringLiteral("cf_chl_opt"), Qt::CaseInsensitive)
        || body.contains(QStringLiteral("Enable JavaScript and cookies to continue"), Qt::CaseInsensitive);
}

template <typename T>
void append(std::vector<T>& to, const std::vector<T>& from)
{
    to.insert(to.end(), from.begin(), from.end());
}

QString capture1(const QRegularExpression& re, const QString& s)
{
    const QRegularExpressionMatch m = re.match(s);
    return m.hasMatch() ? m.captured(1) : QString();
}

QString jsonString(const QJsonValue& v, const QString& key)
{
    if (!v.isObject())
        return {};
    const QJsonValue p = v.toObject().value(key);
    return p.isString() ? p.toString() : QString();
}

// ---- HTML helpers --------------------------------------------------------------------

char16_t namedEntity(QStringView name)
{
    static const QHash<QString, char16_t> table = [] {
        QHash<QString, char16_t> t;
        const struct {
            const char* name;
            char16_t ch;
        } entries[] = {
            {"amp", u'&'}, {"lt", u'<'}, {"gt", u'>'}, {"quot", u'"'}, {"apos", u'\''}, {"nbsp", 0x00A0},
            {"iexcl", 0x00A1}, {"cent", 0x00A2}, {"pound", 0x00A3}, {"curren", 0x00A4}, {"yen", 0x00A5},
            {"brvbar", 0x00A6}, {"sect", 0x00A7}, {"uml", 0x00A8}, {"copy", 0x00A9}, {"ordf", 0x00AA},
            {"laquo", 0x00AB}, {"not", 0x00AC}, {"shy", 0x00AD}, {"reg", 0x00AE}, {"macr", 0x00AF},
            {"deg", 0x00B0}, {"plusmn", 0x00B1}, {"sup2", 0x00B2}, {"sup3", 0x00B3}, {"acute", 0x00B4},
            {"micro", 0x00B5}, {"para", 0x00B6}, {"middot", 0x00B7}, {"cedil", 0x00B8}, {"sup1", 0x00B9},
            {"ordm", 0x00BA}, {"raquo", 0x00BB}, {"frac14", 0x00BC}, {"frac12", 0x00BD}, {"frac34", 0x00BE},
            {"iquest", 0x00BF}, {"Agrave", 0x00C0}, {"Aacute", 0x00C1}, {"Acirc", 0x00C2}, {"Atilde", 0x00C3},
            {"Auml", 0x00C4}, {"Aring", 0x00C5}, {"AElig", 0x00C6}, {"Ccedil", 0x00C7}, {"Egrave", 0x00C8},
            {"Eacute", 0x00C9}, {"Ecirc", 0x00CA}, {"Euml", 0x00CB}, {"Igrave", 0x00CC}, {"Iacute", 0x00CD},
            {"Icirc", 0x00CE}, {"Iuml", 0x00CF}, {"ETH", 0x00D0}, {"Ntilde", 0x00D1}, {"Ograve", 0x00D2},
            {"Oacute", 0x00D3}, {"Ocirc", 0x00D4}, {"Otilde", 0x00D5}, {"Ouml", 0x00D6}, {"times", 0x00D7},
            {"Oslash", 0x00D8}, {"Ugrave", 0x00D9}, {"Uacute", 0x00DA}, {"Ucirc", 0x00DB}, {"Uuml", 0x00DC},
            {"Yacute", 0x00DD}, {"THORN", 0x00DE}, {"szlig", 0x00DF}, {"agrave", 0x00E0}, {"aacute", 0x00E1},
            {"acirc", 0x00E2}, {"atilde", 0x00E3}, {"auml", 0x00E4}, {"aring", 0x00E5}, {"aelig", 0x00E6},
            {"ccedil", 0x00E7}, {"egrave", 0x00E8}, {"eacute", 0x00E9}, {"ecirc", 0x00EA}, {"euml", 0x00EB},
            {"igrave", 0x00EC}, {"iacute", 0x00ED}, {"icirc", 0x00EE}, {"iuml", 0x00EF}, {"eth", 0x00F0},
            {"ntilde", 0x00F1}, {"ograve", 0x00F2}, {"oacute", 0x00F3}, {"ocirc", 0x00F4}, {"otilde", 0x00F5},
            {"ouml", 0x00F6}, {"divide", 0x00F7}, {"oslash", 0x00F8}, {"ugrave", 0x00F9}, {"uacute", 0x00FA},
            {"ucirc", 0x00FB}, {"uuml", 0x00FC}, {"yacute", 0x00FD}, {"thorn", 0x00FE}, {"yuml", 0x00FF},
            {"OElig", 0x0152}, {"oelig", 0x0153}, {"Scaron", 0x0160}, {"scaron", 0x0161}, {"Yuml", 0x0178},
            {"fnof", 0x0192}, {"circ", 0x02C6}, {"tilde", 0x02DC}, {"ensp", 0x2002}, {"emsp", 0x2003},
            {"thinsp", 0x2009}, {"zwnj", 0x200C}, {"zwj", 0x200D}, {"lrm", 0x200E}, {"rlm", 0x200F},
            {"ndash", 0x2013}, {"mdash", 0x2014}, {"lsquo", 0x2018}, {"rsquo", 0x2019}, {"sbquo", 0x201A},
            {"ldquo", 0x201C}, {"rdquo", 0x201D}, {"bdquo", 0x201E}, {"dagger", 0x2020}, {"Dagger", 0x2021},
            {"bull", 0x2022}, {"hellip", 0x2026}, {"permil", 0x2030}, {"prime", 0x2032}, {"Prime", 0x2033},
            {"lsaquo", 0x2039}, {"rsaquo", 0x203A}, {"oline", 0x203E}, {"frasl", 0x2044}, {"euro", 0x20AC},
            {"trade", 0x2122}, {"larr", 0x2190}, {"uarr", 0x2191}, {"rarr", 0x2192}, {"darr", 0x2193},
            {"harr", 0x2194}, {"minus", 0x2212}, {"lowast", 0x2217}, {"infin", 0x221E}, {"ne", 0x2260},
            {"le", 0x2264}, {"ge", 0x2265}, {"loz", 0x25CA}, {"spades", 0x2660}, {"clubs", 0x2663},
            {"hearts", 0x2665}, {"diams", 0x2666},
        };
        for (const auto& e : entries)
            t.insert(QString::fromLatin1(e.name), e.ch);
        return t;
    }();
    return table.value(name.toString(), char16_t(0));
}

struct HtmlTable {
    QStringList headers;
    QList<QStringList> rows;
};

// <dt>label</dt><dd>value</dd>
QString detailValue(const QString& html, const QString& label);

// The title index's own text helper: tags to spaces, decoded, collapsed.
QString indexText(QString s)
{
    static const QRegularExpression tags(QStringLiteral("<[^>]*>"), kSingle);
    static const QRegularExpression spaces(QStringLiteral("\\s+"), QRegularExpression::UseUnicodePropertiesOption);
    s.replace(tags, QStringLiteral(" "));
    s = detail::htmlDecode(s);
    s.replace(spaces, QStringLiteral(" "));
    return s.trimmed();
}

QString indexField(const QString& html, const QString& label)
{
    static const QRegularExpression li(QStringLiteral("<li\\b[^>]*>(.*?)</li>"), kSingleCi);
    static const QRegularExpression strong(QStringLiteral("<strong\\b[^>]*>(.*?)</strong>\\s*(.*?)$"), kSingleCi);
    for (auto it = li.globalMatch(html); it.hasNext();) {
        const QRegularExpressionMatch m = strong.match(it.next().captured(1));
        if (indexText(m.captured(1)) == label)
            return indexText(m.captured(2));
    }
    return {};
}

QList<HtmlTable> htmlTables(const QString& html)
{
    static const QRegularExpression tableRe(QStringLiteral("<table\\b[^>]*>(.*?)</table>"), kSingleCi);
    static const QRegularExpression trRe(QStringLiteral("<tr\\b[^>]*>(.*?)</tr>"), kSingleCi);
    static const QRegularExpression thRe(QStringLiteral("<th\\b[^>]*>(.*?)</th>"), kSingleCi);
    static const QRegularExpression tdRe(QStringLiteral("<td\\b[^>]*>(.*?)</td>"), kSingleCi);
    QList<HtmlTable> out;
    for (auto t = tableRe.globalMatch(html); t.hasNext();) {
        HtmlTable table;
        const QString body = t.next().captured(1);
        for (auto r = trRe.globalMatch(body); r.hasNext();) {
            const QString row = r.next().captured(1);
            QStringList headers;
            for (auto h = thRe.globalMatch(row); h.hasNext();)
                headers << detail::htmlText(h.next().captured(1));
            if (!headers.isEmpty()) {
                table.headers = headers;
                continue;
            }
            QStringList cells;
            for (auto d = tdRe.globalMatch(row); d.hasNext();)
                cells << detail::htmlText(d.next().captured(1));
            table.rows << cells;
        }
        out << table;
    }
    return out;
}

QString detailValue(const QString& html, const QString& label)
{
    const QRegularExpression re(QStringLiteral("<dt\\b[^>]*>\\s*") + QRegularExpression::escape(label)
                                    + QStringLiteral("\\s*</dt>\\s*<dd\\b[^>]*>(.*?)</dd>"),
                                kSingleCi);
    return detail::htmlText(capture1(re, html));
}

qsizetype headerIndex(const QStringList& headers, const QString& name)
{
    for (qsizetype i = 0; i < headers.size(); ++i)
        if (eqCi(headers.at(i), name))
            return i;
    return -1;
}

// ---- Store page helpers -----------------------------------------------------------------

// Every `<script type="application/ld+json">` block, parsed (invalid ones skipped).
QList<QJsonValue> ldJsonBlocks(const QString& html)
{
    static const QRegularExpression re(
        QStringLiteral("<script\\b[^>]*type\\s*=\\s*['\"]application/ld\\+json['\"][^>]*>(.*?)</script>"), kSingleCi);
    QList<QJsonValue> out;
    for (auto it = re.globalMatch(html); it.hasNext();) {
        QJsonParseError pe{};
        const QJsonDocument doc = QJsonDocument::fromJson(it.next().captured(1).toUtf8(), &pe);
        if (pe.error != QJsonParseError::NoError)
            continue;
        if (doc.isObject())
            out << QJsonValue(doc.object());
        else if (doc.isArray())
            out << QJsonValue(doc.array());
    }
    return out;
}

// The `cache` objects of the page's Next.js/Apollo `<script type="application/json">` blocks.
QList<QJsonObject> storeCaches(const QString& html)
{
    static const QRegularExpression re(
        QStringLiteral("<script\\b[^>]*type\\s*=\\s*['\"]application/json['\"][^>]*>(.*?)</script>"), kSingleCi);
    QList<QJsonObject> out;
    for (auto it = re.globalMatch(html); it.hasNext();) {
        QJsonParseError pe{};
        const QJsonDocument doc = QJsonDocument::fromJson(it.next().captured(1).toUtf8(), &pe);
        if (pe.error != QJsonParseError::NoError || !doc.isObject())
            continue;
        const QJsonValue cache = doc.object().value(QStringLiteral("cache"));
        if (cache.isObject())
            out << cache.toObject();
    }
    return out;
}

QString gameNameIn(const QList<QJsonObject>& caches, const QString& cid)
{
    for (const QJsonObject& cache : caches) {
        const QJsonValue product = cache.value(QStringLiteral("Product:") + cid);
        if (!product.isObject())
            continue;
        const QJsonValue conceptRef = product.toObject().value(QStringLiteral("concept"));
        if (conceptRef.isUndefined())
            continue;
        const QString ref = jsonString(conceptRef, QStringLiteral("__ref"));
        if (!ref.startsWith(QStringLiteral("Concept:")) || !cache.contains(ref))
            continue;
        const QString name = jsonString(cache.value(ref), QStringLiteral("name"));
        if (detail::isGameName(name))
            return name;
    }
    return {};
}

QString conceptIdIn(const QList<QJsonObject>& caches, const QString& cid)
{
    for (const QJsonObject& cache : caches) {
        const QJsonValue product = cache.value(QStringLiteral("Product:") + cid);
        if (!product.isObject())
            continue;
        const QJsonValue conceptRef = product.toObject().value(QStringLiteral("concept"));
        if (conceptRef.isUndefined())
            continue;
        const QString ref = jsonString(conceptRef, QStringLiteral("__ref"));
        const QString id = ref.startsWith(QStringLiteral("Concept:")) ? ref.mid(8) : jsonString(conceptRef, QStringLiteral("id"));
        if (isConceptId(id))
            return id;
    }
    return {};
}

QString productKindIn(const QList<QJsonObject>& caches, const QString& cid)
{
    static const QStringList dlc = {
        QStringLiteral("ADD_ON"),    QStringLiteral("ADD_ON_PACK"), QStringLiteral("SEASON_PASS"),
        QStringLiteral("EPISODE"),   QStringLiteral("EXPANSION"),   QStringLiteral("LEVEL"),
        QStringLiteral("MAP"),       QStringLiteral("CHARACTER"),   QStringLiteral("COSTUME"),
        QStringLiteral("ITEM"),      QStringLiteral("VEHICLE"),     QStringLiteral("VIRTUAL_CURRENCY"),
        QStringLiteral("UNLOCKABLE"),
    };
    for (const QJsonObject& cache : caches) {
        const QJsonValue product = cache.value(QStringLiteral("Product:") + cid);
        if (product.isUndefined())
            continue;
        const QString cls = jsonString(product, QStringLiteral("storeDisplayClassification")).toUpper();
        if (cls == u"DEMO" || cls == u"TRIAL")
            return QStringLiteral("Demo");
        if (cls == u"FULL_GAME" || cls == u"GAME_BUNDLE" || cls == u"PSN_GAME")
            return QStringLiteral("Game");
        if (dlc.contains(cls))
            return QStringLiteral("DLC");
    }
    return QStringLiteral("Unknown");
}

QString firstPathSegment(const QString& url)
{
    const QStringList parts = QUrl(url).path().split(QLatin1Char('/'), Qt::SkipEmptyParts);
    return parts.isEmpty() ? QString() : parts.first();
}

void readProduct(const QJsonValue& v, const QString& sourceUrl, const QString& expected, const QList<QJsonObject>& caches,
                 std::vector<DlcCandidate>& out)
{
    if (v.isArray()) {
        for (const QJsonValue& e : v.toArray())
            readProduct(e, sourceUrl, expected, caches, out);
        return;
    }
    if (!v.isObject())
        return;
    const QJsonObject o = v.toObject();
    if (o.contains(QStringLiteral("@graph")))
        readProduct(o.value(QStringLiteral("@graph")), sourceUrl, expected, caches, out);
    const QString type = jsonString(v, QStringLiteral("@type"));
    const QString sku = jsonString(v, QStringLiteral("sku")).toUpper();
    const QString name = jsonString(v, QStringLiteral("name"));
    if (!eqCi(type, QStringLiteral("Product")) || !isLookupContentId(sku) || sku != expected.toUpper() || name.isEmpty())
        return;
    const QRegularExpressionMatch tm = titlePattern().match(sku);
    const QString locale = firstPathSegment(sourceUrl);
    const Market* market = nullptr;
    for (const Market& m : markets())
        if (eqCi(m.locale, locale)) {
            market = &m;
            break;
        }
    DlcCandidate c;
    c.titleId = tm.captured(1).toUpper() + tm.captured(2);
    c.contentId = sku;
    c.name = name;
    c.edition = jsonString(v, QStringLiteral("category"));
    c.region = market ? market->displayName : QStringLiteral("Store locale ") + locale;
    c.storeLocale = locale;
    c.marketRegionCode = market ? market->code : QString();
    c.gameName = gameNameIn(caches, sku);
    c.productKind = productKindIn(caches, sku);
    c.conceptId = conceptIdIn(caches, sku);
    c.platform = eqCi(tm.captured(1), QStringLiteral("PPSA")) ? QStringLiteral("PS5") : QStringLiteral("PS4");
    c.sourceUrl = sourceUrl;
    c.mappingStatus = QStringLiteral("Official Store product SKU observed — exact content ID");
    out.push_back(std::move(c));
}

void readSkus(const QJsonValue& v, QStringList& out)
{
    if (v.isArray()) {
        for (const QJsonValue& e : v.toArray())
            readSkus(e, out);
        return;
    }
    if (!v.isObject())
        return;
    const QJsonObject o = v.toObject();
    if (o.contains(QStringLiteral("@graph")))
        readSkus(o.value(QStringLiteral("@graph")), out);
    const QString sku = jsonString(v, QStringLiteral("sku")).toUpper();
    if (jsonString(v, QStringLiteral("@type")) == u"Product" && isStrictCid(sku))
        out << sku;
}

QString metadataValue(const QJsonValue& item, const QString& name)
{
    const QJsonValue metadata = item.toObject().value(QStringLiteral("metadata"));
    if (!metadata.isObject())
        return {};
    const QJsonValue values = metadata.toObject().value(name).toObject().value(QStringLiteral("values"));
    if (!values.isArray() || values.toArray().isEmpty())
        return {};
    const QJsonValue first = values.toArray().first();
    return first.isString() ? first.toString() : QString();
}

QStringList words(const QString& s)
{
    static const QRegularExpression re(QStringLiteral("[\\p{L}\\p{N}]+"));
    QStringList out;
    const QString upper = s.normalized(QString::NormalizationForm_C).toUpper();
    for (auto it = re.globalMatch(upper); it.hasNext();)
        out << it.next().captured(0);
    return out;
}

bool hasWordPrefix(const QStringList& item, const QStringList& query)
{
    if (query.isEmpty() || item.size() < query.size())
        return false;
    for (qsizetype i = 0; i < query.size(); ++i)
        if (item.at(i) != query.at(i))
            return false;
    return true;
}

std::vector<DlcCandidate> distinctCandidates(const std::vector<DlcCandidate>& in)
{
    QSet<QString> seen;
    std::vector<DlcCandidate> out;
    for (const DlcCandidate& c : in) {
        const QString key = QStringList{c.titleId, c.contentId, c.name, c.edition, c.region, c.sourceUrl}
                                .join(QLatin1Char('|'))
                                .toUpper();
        if (seen.contains(key))
            continue;
        seen.insert(key);
        out.push_back(c);
    }
    return out;
}

std::vector<DlcCandidate> firstN(std::vector<DlcCandidate> v, std::size_t n)
{
    if (v.size() > n)
        v.resize(n);
    return v;
}

QStringList distinctCi(const QStringList& in)
{
    QSet<QString> seen;
    QStringList out;
    for (const QString& s : in) {
        const QString key = s.toUpper();
        if (seen.contains(key))
            continue;
        seen.insert(key);
        out << s;
    }
    return out;
}

std::vector<DlcCandidate> searchOrThrow(const QString& json, const Market& market, const QString& query, bool conceptRequired)
{
    auto r = detail::parseSearch(json.toUtf8(), market, query, conceptRequired);
    if (!r.ok())
        throw LookupError{r.error().message};
    return std::move(r).value();
}

bool sourceBaseProbe(const QString& contentId, const QString& titleId, QString* probeId)
{
    static const QRegularExpression re(QStringLiteral("\\A([A-Z]{2}\\d{4})-((?:PPSA|CUSA)\\d{5})_\\d{2}-[A-Z0-9_]{16}\\z"));
    probeId->clear();
    const QRegularExpressionMatch m = re.match(contentId);
    if (!m.hasMatch() || m.captured(2) != titleId)
        return false;
    *probeId = m.captured(1) + QLatin1Char('-') + titleId + QStringLiteral("_00-0000000000000000");
    return *probeId != contentId;
}

// ---- HTTPS fetch -------------------------------------------------------------------------

Result<QByteArray> httpsGet(const QUrl& url, int timeoutMs, const CancellationToken& token)
{
    if (token.isCancelled())
        return Error::cancelled();
    // Automated tests and CI runs set FW_OFFLINE; nothing may reach the Store then.
    if (qEnvironmentVariableIsSet("FW_OFFLINE") && qgetenv("FW_OFFLINE") != "0")
        return Error(ErrorCode::Unsupported, QStringLiteral("Online lookup is disabled (FW_OFFLINE is set)"));
    if (!DlcLookupService::isTrustedSource(url))
        return Error(ErrorCode::PermissionDenied, QStringLiteral("%1 is not a trusted lookup source").arg(url.host()));

    QNetworkAccessManager nam;
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, DlcLookupService::userAgent());
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::UserVerifiedRedirectPolicy);
    req.setMaximumRedirectsAllowed(5);
    QNetworkReply* reply = nam.get(req);

    QByteArray body;
    bool tooLarge = false;
    bool timedOut = false;
    QString refusedRedirect;
    const auto status = [reply] { return reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(); };
    const auto success = [&] { return status() >= 200 && status() <= 299; };

    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::redirected, &loop, [&](const QUrl& target) {
        const QUrl next = reply->url().resolved(target);
        if (DlcLookupService::isTrustedSource(next)) {
            emit reply->redirectAllowed();
        } else {
            refusedRedirect = next.host().isEmpty() ? next.toString() : next.host();
            reply->abort();
        }
    });
    QObject::connect(reply, &QNetworkReply::metaDataChanged, &loop, [&] {
        if (!success())
            return;
        const QVariant len = reply->header(QNetworkRequest::ContentLengthHeader);
        if (len.isValid() && len.toLongLong() > kMaxResponseBytes) {
            tooLarge = true;
            reply->abort();
        }
    });
    QObject::connect(reply, &QNetworkReply::readyRead, &loop, [&] {
        if (!success()) {
            reply->readAll(); // error pages are not used
            return;
        }
        body += reply->readAll();
        if (body.size() > kMaxResponseBytes) {
            tooLarge = true;
            reply->abort();
        }
    });
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &loop, [&] {
        timedOut = true;
        reply->abort();
    });
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (token.isCancelled())
            reply->abort();
    });
    deadline.start(std::max(1, timeoutMs));
    poll.start(50);
    if (!reply->isFinished())
        loop.exec();
    deadline.stop();
    poll.stop();

    const int code = status();
    const QNetworkReply::NetworkError err = reply->error();
    const QString errText = reply->errorString();
    if (err == QNetworkReply::NoError && success() && !tooLarge)
        body += reply->readAll();
    reply->deleteLater();

    if (token.isCancelled())
        return Error::cancelled();
    if (timedOut)
        return Error(ErrorCode::Timeout, requestTimedOutText(timeoutMs));
    if (!refusedRedirect.isEmpty())
        return Error(ErrorCode::PermissionDenied,
                     QStringLiteral("Redirect to %1 was refused; only trusted lookup sources are followed").arg(refusedRedirect));
    if (code != 0 && (code < 200 || code > 299))
        return Error(ErrorCode::ExternalToolFailed, httpErrorText(code, url.host()));
    if (tooLarge || body.size() > kMaxResponseBytes)
        return Error(ErrorCode::CorruptData, QStringLiteral("Lookup response exceeds the 2 MiB limit"));
    if (err != QNetworkReply::NoError)
        return Error(ErrorCode::IoError, errText);
    if (looksLikeChallenge(QString::fromUtf8(body)))
        return Error(ErrorCode::PermissionDenied, QStringLiteral("The lookup service requires a browser challenge"));
    return body;
}

// One lookup's network access: the user's token plus a budget (the
// Companion's linked CancellationTokenSource with CancelAfter).
class Session {
public:
    Session(const DlcLookupService::Fetch& fetch, const CancellationToken& token, qint64 budgetMs)
        : m_fetch(fetch), m_token(token), m_budgetMs(budgetMs)
    {
        m_timer.start();
    }

    qint64 remaining() const { return m_budgetMs - m_timer.elapsed(); }
    bool expired() const { return remaining() <= 0; }

    // The body as text, or throws LookupError / LookupInterrupted.
    QString fetch(const QString& urlText) const
    {
        if (m_token.isCancelled())
            throw LookupInterrupted{true};
        const qint64 left = remaining();
        if (left <= 0)
            throw LookupInterrupted{false};
        const QUrl url(urlText);
        if (!DlcLookupService::isTrustedSource(url))
            throw LookupError{QStringLiteral("%1 is not a trusted lookup source").arg(url.host().isEmpty() ? urlText : url.host())};
        const int requestMs = g_requestTimeoutMs.load();
        const bool budgetLimited = left < requestMs;
        Result<QByteArray> r = m_fetch(url, budgetLimited ? int(left) : requestMs, m_token);
        if (m_token.isCancelled())
            throw LookupInterrupted{true};
        if (!r.ok()) {
            const Error& e = r.error();
            if (e.code == ErrorCode::Cancelled)
                throw LookupInterrupted{true};
            if (e.code == ErrorCode::Timeout) {
                if (budgetLimited || expired())
                    throw LookupInterrupted{false};
                throw LookupError{requestTimedOutText(requestMs)};
            }
            throw LookupError{e.message};
        }
        if (r->size() > kMaxResponseBytes)
            throw LookupError{QStringLiteral("Lookup response exceeds the 2 MiB limit")};
        QString text = QString::fromUtf8(*r);
        if (looksLikeChallenge(text))
            throw LookupError{QStringLiteral("The lookup service requires a browser challenge")};
        return text;
    }

private:
    const DlcLookupService::Fetch& m_fetch;
    CancellationToken m_token;
    qint64 m_budgetMs;
    QElapsedTimer m_timer;
};

// ---- cache --------------------------------------------------------------------------------

struct CacheEntry {
    QDateTime storedAt;
    DlcLookupResult result;
};

const QStringList& candidateKeys()
{
    static const QStringList keys = {
        QStringLiteral("TitleId"),     QStringLiteral("ContentId"),     QStringLiteral("Name"),
        QStringLiteral("Edition"),     QStringLiteral("Region"),        QStringLiteral("Platform"),
        QStringLiteral("Version"),     QStringLiteral("MarketRegionCode"), QStringLiteral("StoreLocale"),
        QStringLiteral("GameName"),    QStringLiteral("ProductKind"),   QStringLiteral("ConceptId"),
        QStringLiteral("SourceUrl"),   QStringLiteral("MappingStatus"), QStringLiteral("CompatibilityStatus"),
    };
    return keys;
}

QList<QString*> candidateFields(DlcCandidate& c)
{
    return {&c.titleId,     &c.contentId,   &c.name,      &c.edition,   &c.region,
            &c.platform,    &c.version,     &c.marketRegionCode, &c.storeLocale, &c.gameName,
            &c.productKind, &c.conceptId,   &c.sourceUrl, &c.mappingStatus, &c.compatibilityStatus};
}

QJsonObject candidateToJson(DlcCandidate c)
{
    QJsonObject o;
    const QList<QString*> fields = candidateFields(c);
    for (qsizetype i = 0; i < fields.size(); ++i)
        o.insert(candidateKeys().at(i), *fields.at(i));
    return o;
}

std::optional<DlcCandidate> candidateFromJson(const QJsonValue& v)
{
    if (!v.isObject())
        return std::nullopt;
    const QJsonObject o = v.toObject();
    DlcCandidate c;
    const QList<QString*> fields = candidateFields(c);
    for (qsizetype i = 0; i < fields.size(); ++i) {
        const QJsonValue f = o.value(candidateKeys().at(i));
        if (f.isString())
            *fields.at(i) = f.toString();
        else if (!f.isUndefined() && !f.isNull())
            return std::nullopt; // a number where a string belongs: the entry is not ours
    }
    return c;
}

QStringList stringList(const QJsonValue& v)
{
    QStringList out;
    for (const QJsonValue& e : v.toArray())
        if (e.isString())
            out << e.toString();
    return out;
}

// Reads a cache file's JSON. `validate` applies ReadCache's per-candidate
// checks; FindCachedGame only needs the envelope checks.
std::optional<CacheEntry> parseCacheEntry(const QByteArray& data, bool validate)
{
    QJsonParseError pe{};
    const QJsonDocument doc = QJsonDocument::fromJson(data, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject())
        return std::nullopt;
    const QJsonObject root = doc.object();
    const QJsonValue schema = root.value(QStringLiteral("Schema"));
    if (!schema.isUndefined() && schema.toInt(-1) != 1)
        return std::nullopt;
    CacheEntry entry;
    const QJsonValue stored = root.value(QStringLiteral("StoredAt"));
    if (stored.isUndefined()) {
        entry.storedAt = QDateTime(QDate(1, 1, 1), QTime(0, 0), QTimeZone::UTC);
    } else {
        entry.storedAt = QDateTime::fromString(stored.toString(), Qt::ISODateWithMs);
        if (!entry.storedAt.isValid())
            return std::nullopt;
    }
    const QJsonValue resultValue = root.value(QStringLiteral("Result"));
    if (!resultValue.isObject())
        return std::nullopt;
    const QJsonObject result = resultValue.toObject();
    const QJsonValue candidates = result.value(QStringLiteral("Candidates"));
    if (!candidates.isArray() || std::size_t(candidates.toArray().size()) > kMaxCandidates)
        return std::nullopt;
    if (entry.storedAt > QDateTime::currentDateTimeUtc().addSecs(5 * 60))
        return std::nullopt;
    for (const QJsonValue& v : candidates.toArray()) {
        std::optional<DlcCandidate> c = candidateFromJson(v);
        if (!c)
            return std::nullopt;
        if (validate) {
            if (!isStrictTitle(c->titleId) || !trustedSource(c->sourceUrl))
                return std::nullopt;
            if (!c->contentId.isEmpty()
                && (!isLookupContentId(c->contentId) || !c->contentId.contains(c->titleId + QLatin1Char('_'))))
                return std::nullopt;
            c->compatibilityStatus = DlcCandidate().compatibilityStatus;
        }
        entry.result.candidates.push_back(*c);
    }
    entry.result.warnings = stringList(result.value(QStringLiteral("Warnings")));
    for (const QString& link : stringList(result.value(QStringLiteral("SourceLinks"))))
        if (!validate || trustedSource(link))
            entry.result.sourceLinks << link;
    entry.result.fromCache = result.value(QStringLiteral("FromCache")).toBool();
    entry.result.isStale = result.value(QStringLiteral("IsStale")).toBool();
    entry.result.resolvedName = result.value(QStringLiteral("ResolvedName")).toString();
    entry.result.storedAt = entry.storedAt;
    return entry;
}

QByteArray cacheJson(const DlcLookupResult& r, const QDateTime& storedAt)
{
    QJsonArray candidates;
    for (const DlcCandidate& c : r.candidates)
        candidates.append(candidateToJson(c));
    QJsonObject result;
    result.insert(QStringLiteral("Candidates"), candidates);
    result.insert(QStringLiteral("Warnings"), QJsonArray::fromStringList(r.warnings));
    result.insert(QStringLiteral("SourceLinks"), QJsonArray::fromStringList(r.sourceLinks));
    result.insert(QStringLiteral("FromCache"), r.fromCache);
    result.insert(QStringLiteral("IsStale"), r.isStale);
    result.insert(QStringLiteral("ResolvedName"), r.resolvedName);
    QJsonObject root;
    root.insert(QStringLiteral("Schema"), 1);
    root.insert(QStringLiteral("StoredAt"), storedAt.toUTC().toString(Qt::ISODateWithMs));
    root.insert(QStringLiteral("Result"), result);
    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

bool isCacheFileName(const QString& name)
{
    static const QRegularExpression re(QStringLiteral("^[A-F0-9]{64}\\.json$"));
    return re.match(name).hasMatch();
}

// The cache files, newest first (stable for equal times).
QFileInfoList cacheFilesNewestFirst(const QString& dir)
{
    QFileInfoList files;
    for (const QFileInfo& fi : QDir(dir).entryInfoList({QStringLiteral("*.json")}, QDir::Files, QDir::Name))
        if (isCacheFileName(fi.fileName()))
            files << fi;
    std::stable_sort(files.begin(), files.end(),
                     [](const QFileInfo& a, const QFileInfo& b) { return a.lastModified() > b.lastModified(); });
    return files;
}

DlcLookupResult messageResult(const QString& message)
{
    DlcLookupResult r;
    r.warnings << message;
    return r;
}

// ---- the lookup service -----------------------------------------------------------------------

class Engine {
public:
    Engine(const QString& cacheDir, const DlcLookupService::Fetch& fetch, const CancellationToken& token)
        : m_cacheDir(cacheDir), m_fetch(fetch), m_token(token)
    {
    }

    DlcLookupResult lookup(const QString& query);
    DlcLookupResult resolveCore(QString titleId, QString contentId, QString gameName, qint64 budgetMs,
                                bool sourceProductChecked, QString conceptId);

private:
    DlcLookupResult lookupProductRegions(const QString& storeUrl, const QString& contentId);
    void lookupSerial(const QString& query, DlcLookupResult& result, const Session& s) const;
    QString resolveTitleIndexName(const QString& titleId, const QString& sourceCid, DlcLookupResult& result,
                                  const Session& s) const;
    DlcLookupResult searchMarket(const Market& market, const QString& name, const QString& conceptId,
                                 const QString& sourceCid, const Session& s, bool* userCancelled) const;
    void verifyRegionalConcept(const std::vector<DlcCandidate>& discovered, DlcLookupResult& part, const Market& market,
                               const QString& name, const QString& conceptId, const QString& sourceCid,
                               const Session& s, bool* userCancelled) const;

    void checkCancelled() const
    {
        if (m_token.isCancelled())
            throw LookupInterrupted{true};
    }

    // cache
    bool cacheEnabled() const { return !m_cacheDir.isEmpty(); }
    QString cachePath(const QString& key) const
    {
        return QDir(m_cacheDir).filePath(detail::cacheFileName(key));
    }
    std::optional<CacheEntry> readCache(const QString& key) const;
    void writeCache(const QString& key, DlcLookupResult& result) const;
    std::optional<DlcCandidate> findCachedGame(const QString& titleId) const;

    static bool isFresh(const CacheEntry& e) { return e.storedAt.msecsTo(QDateTime::currentDateTimeUtc()) < kFreshForMs; }
    static DlcLookupResult freshHit(CacheEntry e, const QString& text)
    {
        e.result.fromCache = true;
        e.result.storedAt = e.storedAt;
        e.result.warnings << text.arg(universalSortable(e.storedAt));
        return e.result;
    }
    static DlcLookupResult staleHit(CacheEntry e, const QStringList& newWarnings, const QString& text)
    {
        e.result.fromCache = true;
        e.result.isStale = true;
        e.result.storedAt = e.storedAt;
        e.result.warnings << newWarnings;
        e.result.warnings << text.arg(universalSortable(e.storedAt));
        return e.result;
    }

    QString m_cacheDir;
    const DlcLookupService::Fetch& m_fetch;
    CancellationToken m_token;
};

std::optional<CacheEntry> Engine::readCache(const QString& key) const
{
    if (!cacheEnabled())
        return std::nullopt;
    const QFileInfo fi(cachePath(key));
    if (!fi.exists() || fi.size() > kMaxResponseBytes)
        return std::nullopt;
    QFile f(fi.filePath());
    if (!f.open(QIODevice::ReadOnly))
        return std::nullopt;
    return parseCacheEntry(f.readAll(), true);
}

void Engine::writeCache(const QString& key, DlcLookupResult& result) const
{
    if (!cacheEnabled())
        return;
    const Status st = writeFileAtomic(cachePath(key), cacheJson(result, QDateTime::currentDateTimeUtc()));
    if (!st.ok()) {
        result.warnings << QStringLiteral("Lookup cache could not be saved: ") + st.error().message;
        return;
    }
    const QFileInfoList files = cacheFilesNewestFirst(m_cacheDir);
    for (qsizetype i = kKeepCacheFiles; i < files.size(); ++i)
        QFile::remove(files.at(i).filePath());
}

std::optional<DlcCandidate> Engine::findCachedGame(const QString& titleId) const
{
    if (!isStrictTitle(titleId) || !cacheEnabled() || !QFileInfo(m_cacheDir).isDir())
        return std::nullopt;
    const QFileInfoList files = cacheFilesNewestFirst(m_cacheDir);
    for (qsizetype i = 0; i < files.size() && i < kKeepCacheFiles; ++i) {
        const QFileInfo& fi = files.at(i);
        if (fi.size() > kMaxResponseBytes)
            continue;
        QFile f(fi.filePath());
        if (!f.open(QIODevice::ReadOnly))
            continue;
        const std::optional<CacheEntry> entry = parseCacheEntry(f.readAll(), false);
        if (!entry)
            continue;
        for (const DlcCandidate& c : entry->result.candidates) {
            if (c.titleId != titleId || c.gameName.isEmpty() || !trustedSource(c.sourceUrl)
                || !eqCi(QUrl(c.sourceUrl).host(), QStringLiteral("store.playstation.com")) || !isLookupContentId(c.contentId)
                || !c.contentId.contains(titleId + QLatin1Char('_')))
                continue;
            if (detail::isGameName(c.gameName))
                return c;
            break; // the Companion looks at the first matching candidate of a file only
        }
    }
    return std::nullopt;
}

DlcLookupResult Engine::lookup(const QString& query)
{
    const QString q = query.trimmed();
    if (q.isEmpty() || q.size() > 240)
        throw LookupRejected{QStringLiteral(
            "Enter a title ID, content ID, game name or PlayStation Store product URL (up to 240 characters).")};
    static const QRegularExpression titleOnly(QStringLiteral("\\A(PPSA|CUSA)-?\\d{5}\\z"), kCi);
    const qint64 budget = g_overallTimeoutMs.load();
    if (titleOnly.match(q).hasMatch())
        return resolveCore(QString(q).remove(QLatin1Char('-')).toUpper(), QString(), QString(), budget, false, QString());
    if (detail::isGameName(q))
        return resolveCore(QString(), QString(), q, budget, false, QString());
    QString storeUrl;
    QString contentId;
    if (detail::tryStoreProduct(q, &storeUrl, &contentId))
        return lookupProductRegions(storeUrl, contentId);

    checkCancelled();
    const std::optional<CacheEntry> cache = readCache(q);
    if (cache && isFresh(*cache))
        return freshHit(*cache, QStringLiteral("Cached database observations from %1. DLC compatibility remains unverified."));
    DlcLookupResult result;
    result.warnings << QStringLiteral(
        "SerialStation is a community database with variable data quality. Check edition, platform and game version. "
        "Every DLC compatibility result is unverified.");
    const Session s(m_fetch, m_token, budget);
    try {
        if (q.contains(QStringLiteral("://")) || q.startsWith(QStringLiteral("http:"), Qt::CaseInsensitive)
            || q.startsWith(QStringLiteral("https:"), Qt::CaseInsensitive))
            throw LookupRejected{QStringLiteral(
                "Only HTTPS PlayStation Store product URLs are accepted. Enter other source IDs or game names manually.")};
        lookupSerial(q, result, s);
    } catch (const LookupInterrupted& i) {
        if (i.user)
            throw;
        result.warnings << QStringLiteral("Online lookup reached its time limit. Local editing and building remain available.");
    } catch (const LookupError& e) {
        result.warnings << QStringLiteral("Online lookup unavailable: %1. Local editing and building remain available.").arg(e.message);
    }
    checkCancelled();
    result.candidates = firstN(distinctCandidates(result.candidates), kMaxCandidates);
    if (result.candidates.empty() && cache)
        return staleHit(*cache, result.warnings,
                        QStringLiteral("Using stale cached observations from %1; recheck identifiers against the source before applying."));
    if (!result.candidates.empty())
        writeCache(q, result);
    if (result.candidates.empty())
        result.warnings << QStringLiteral(
            "No mapping was confirmed. Open the source links or enter target identifiers manually; do not infer a publisher "
            "prefix from a region.");
    result.sourceLinks = distinctCi(result.sourceLinks);
    return result;
}

DlcLookupResult Engine::lookupProductRegions(const QString& storeUrl, const QString& contentId)
{
    checkCancelled();
    const QString key = QStringLiteral("regions-product-v7|") + storeUrl;
    const std::optional<CacheEntry> cache = readCache(key);
    if (cache && isFresh(*cache))
        return freshHit(*cache,
                        QStringLiteral("Cached source product and regional observations from %1. DLC compatibility remains unverified."));
    DlcLookupResult result;
    result.sourceLinks << storeUrl;
    const QString titleId = titlePattern().match(contentId).captured(0).toUpper();
    QString name;
    QString conceptId;
    std::vector<DlcCandidate> observed;
    const Session s(m_fetch, m_token, g_overallTimeoutMs.load());
    try {
        try {
            const QString html = s.fetch(storeUrl);
            observed = detail::parseStoreProduct(html, storeUrl, contentId);
            name = detail::parseStoreGameName(html, contentId);
            conceptId = detail::parseStoreConceptId(html, contentId);
            if (name.isEmpty())
                for (const DlcCandidate& c : observed)
                    if (c.productKind == u"Game") {
                        name = c.name;
                        break;
                    }
            if (observed.empty())
                result.warnings << QStringLiteral(
                    "The supplied Store page did not publish the requested product SKU. Related targets require independent "
                    "Store observations.");
        } catch (const LookupError& e) {
            result.warnings << QStringLiteral("Supplied Store product unavailable: ") + e.message;
        }
        const qint64 remaining = s.remaining();
        if (remaining > 0) {
            const DlcLookupResult inner = !name.isEmpty()
                ? resolveCore(QString(), QString(), name, remaining, true, conceptId)
                : resolveCore(titleId, contentId, QString(), remaining, true, QString());
            result.resolvedName = inner.resolvedName;
            append(result.candidates, inner.candidates);
            result.warnings << inner.warnings;
            result.sourceLinks << inner.sourceLinks;
            result.fromCache = inner.fromCache;
            result.isStale = inner.isStale;
            result.storedAt = inner.storedAt;
        }
    } catch (const LookupInterrupted& i) {
        if (i.user)
            throw;
        result.warnings << QStringLiteral("Store product/regional lookup reached its total time limit. Local editing remains available.");
    }
    checkCancelled();
    std::vector<DlcCandidate> all = observed;
    append(all, result.candidates);
    result.candidates = firstN(distinctCandidates(all), kMaxCandidates);
    result.sourceLinks = distinctCi(result.sourceLinks);
    if (result.candidates.empty() && cache)
        return staleHit(*cache, result.warnings, QStringLiteral("Using stale source product/regional observations from %1."));
    if (!result.candidates.empty() && !result.isStale)
        writeCache(key, result);
    return result;
}

DlcLookupResult Engine::resolveCore(QString titleId, QString contentId, QString gameName, qint64 budgetMs,
                                    bool sourceProductChecked, QString conceptId)
{
    titleId = titleId.trimmed().toUpper();
    contentId = contentId.trimmed().toUpper();
    gameName = gameName.trimmed();
    if (titleId.size() > 64 || contentId.size() > 240 || gameName.size() > 240)
        throw LookupRejected{QStringLiteral("Lookup identifiers and game names must be at most 240 characters.")};
    checkCancelled();
    const QString key = QStringList{QStringLiteral("regions-v6"), titleId, contentId, gameName, conceptId}.join(QLatin1Char('|'));
    const std::optional<CacheEntry> cache = readCache(key);
    if (cache && isFresh(*cache))
        return freshHit(*cache, QStringLiteral("Cached Store observations from %1. DLC compatibility remains unverified."));

    DlcLookupResult result;
    result.warnings << QStringLiteral(
        "Store identifiers are observed release mappings. Check edition and platform; DLC compatibility still needs a "
        "console test.");
    const Session s(m_fetch, m_token, budgetMs);
    try {
        // 1. Hard-coded names, then an earlier Store observation of the title.
        QString name = detail::knownName(titleId);
        std::optional<DlcCandidate> cachedSourceRelease;
        if (name.isEmpty()) {
            if (const std::optional<DlcCandidate> cached = findCachedGame(titleId)) {
                cachedSourceRelease = cached;
                name = cached->gameName;
                if (isConceptId(cached->conceptId))
                    conceptId = cached->conceptId;
            }
        }
        // 2. The DLC's own Store product page.
        QString productUrl;
        QString expectedId;
        if (name.isEmpty() && !sourceProductChecked && detail::tryStoreProduct(contentId, &productUrl, &expectedId)) {
            result.sourceLinks << productUrl;
            try {
                const QString html = s.fetch(productUrl);
                const std::vector<DlcCandidate> found = detail::parseStoreProduct(html, productUrl, expectedId);
                append(result.candidates, found);
                name = detail::parseStoreGameName(html, expectedId);
                conceptId = detail::parseStoreConceptId(html, expectedId);
                if (name.isEmpty() && found.size() == 1 && found.front().productKind == u"Game")
                    name = found.front().name;
            } catch (const LookupError& e) {
                result.warnings << QStringLiteral("Source DLC is absent or unavailable in the Store: ") + e.message;
            }
        }
        // 3. The base release <prefix>-<title>_00-0000000000000000.
        QString probeId;
        QString probeUrl;
        QString probeCid;
        if (name.isEmpty() && sourceBaseProbe(contentId, titleId, &probeId)
            && detail::tryStoreProduct(probeId, &probeUrl, &probeCid)) {
            try {
                const QString html = s.fetch(probeUrl);
                std::vector<DlcCandidate> games;
                for (const DlcCandidate& c : detail::parseStoreProduct(html, probeUrl, probeId))
                    if (c.productKind == u"Game" && c.titleId == titleId)
                        games.push_back(c);
                if (games.size() == 1) {
                    name = detail::parseStoreGameName(html, probeId);
                    if (name.isEmpty())
                        name = games.front().name;
                    result.candidates.push_back(games.front());
                    result.sourceLinks << probeUrl;
                    conceptId = games.front().conceptId;
                    result.warnings << QStringLiteral(
                        "Source game identified from an exact official Store base release observation; the imported DLC "
                        "identifiers remain unchanged until a target is applied.");
                }
            } catch (const LookupError& e) {
                result.warnings << QStringLiteral("Source base-release lookup unavailable: ") + e.message;
            }
        }
        // 4. PPSA only: the ProsperoPatches title index, verified against the Store.
        static const QRegularExpression ppsa(QStringLiteral("\\APPSA\\d{5}\\z"));
        if (name.isEmpty() && ppsa.match(titleId).hasMatch())
            name = resolveTitleIndexName(titleId, contentId, result, s);
        // 5. A name without a concept: learn the concept from the source release's page.
        if (!name.isEmpty() && conceptId.isEmpty()) {
            qsizetype index = -1;
            for (std::size_t i = 0; i < result.candidates.size(); ++i) {
                const DlcCandidate& c = result.candidates[i];
                if (c.titleId == titleId && c.productKind == u"Game" && isLookupContentId(c.contentId)) {
                    index = qsizetype(i);
                    break;
                }
            }
            const std::optional<DlcCandidate> sourceRelease =
                index >= 0 ? std::optional<DlcCandidate>(result.candidates[std::size_t(index)]) : cachedSourceRelease;
            if (sourceRelease) {
                try {
                    const QString html = s.fetch(sourceRelease->sourceUrl);
                    for (const DlcCandidate& c : detail::parseStoreProduct(html, sourceRelease->sourceUrl, sourceRelease->contentId)) {
                        if (c.titleId != titleId)
                            continue;
                        conceptId = c.conceptId;
                        if (index >= 0)
                            result.candidates[std::size_t(index)].conceptId = conceptId;
                        result.candidates.push_back(c);
                        break;
                    }
                } catch (const LookupError& e) {
                    result.warnings << QStringLiteral("Localised-release relationship lookup unavailable: ") + e.message;
                }
            }
        }
        // 6. The DLC entry's own title, unless it is a generic DLC label.
        if (name.isEmpty() && detail::isGameName(gameName) && (titleId.isEmpty() || !detail::isDlcDisplayLabel(gameName)))
            name = gameName;
        // 7. SerialStation.
        if (name.isEmpty()) {
            lookupSerial(titleId.isEmpty() ? gameName : titleId, result, s);
            for (const DlcCandidate& c : result.candidates)
                if (c.titleId == titleId && c.edition != u"DLC" && !c.name.isEmpty()) {
                    name = c.name;
                    break;
                }
        }

        if (!detail::isGameName(name)) {
            result.warnings << QStringLiteral(
                "The source title could not be resolved to a game name. Enter the game name above and select Find regional "
                "releases; no region identifiers will be guessed.");
        } else {
            result.resolvedName = name;
            const QList<Market>& all = markets();
            std::vector<DlcLookupResult> parts(std::size_t(all.size()));
            std::atomic<bool> userCancelled{false};
            std::vector<std::unique_ptr<QThread>> threads;
            for (qsizetype i = 0; i < all.size(); ++i) {
                std::unique_ptr<QThread> t(QThread::create([&, i] {
                    bool cancelled = false;
                    try {
                        parts[std::size_t(i)] = searchMarket(all.at(i), name, conceptId, contentId, s, &cancelled);
                    } catch (...) {
                        parts[std::size_t(i)].warnings << all.at(i).displayName + QStringLiteral(" Store lookup unavailable: internal error");
                    }
                    if (cancelled)
                        userCancelled = true;
                }));
                t->start();
                threads.push_back(std::move(t));
            }
            for (const std::unique_ptr<QThread>& t : threads)
                t->wait();
            if (userCancelled)
                throw LookupInterrupted{true};
            for (const DlcLookupResult& part : parts) {
                append(result.candidates, part.candidates);
                result.warnings << part.warnings;
                result.sourceLinks << part.sourceLinks;
            }
            if (result.candidates.empty() && !s.expired())
                lookupSerial(titleId.isEmpty() ? name : titleId, result, s);
        }
    } catch (const LookupInterrupted& i) {
        if (i.user)
            throw;
        result.warnings << QStringLiteral("Regional lookup reached its time limit. Local editing remains available.");
    }
    checkCancelled();
    result.candidates = firstN(distinctCandidates(result.candidates), kMaxCandidates);
    result.sourceLinks = distinctCi(result.sourceLinks);
    if (result.candidates.empty() && cache)
        return staleHit(*cache, result.warnings, QStringLiteral("Using stale Store observations from %1; recheck the source before applying."));
    if (!result.candidates.empty())
        writeCache(key, result);
    else
        result.warnings << QStringLiteral(
            "No matching Store release was observed. Correct the game name or enter target identifiers manually.");
    return result;
}

DlcLookupResult Engine::searchMarket(const Market& market, const QString& name, const QString& conceptId,
                                     const QString& sourceCid, const Session& s, bool* userCancelled) const
{
    DlcLookupResult part;
    const bool conceptRequired = isConceptId(conceptId);
    std::vector<DlcCandidate> parsed;
    part.sourceLinks << detail::searchPageUrl(market, name);
    try {
        const QString json = s.fetch(detail::searchApiUrl(market, name));
        parsed = searchOrThrow(json, market, name, conceptRequired);
        if (!conceptRequired)
            append(part.candidates, parsed);
        if (detail::searchCount(json.toUtf8()) >= 100) {
            const QString refined = detail::refinedSearch(name);
            if (!refined.isEmpty() && !eqCi(refined, name)) {
                const std::vector<DlcCandidate> more =
                    searchOrThrow(s.fetch(detail::searchApiUrl(market, refined)), market, name, conceptRequired);
                append(parsed, more);
                if (!conceptRequired)
                    append(part.candidates, more);
            }
            part.warnings << market.displayName
                    + QStringLiteral(" search reached its result limit; narrow the game name if the expected edition is missing.");
        }
    } catch (const LookupError& e) {
        part.warnings << market.displayName + QStringLiteral(" Store lookup unavailable: ") + e.message;
    } catch (const LookupInterrupted& i) {
        if (i.user) {
            *userCancelled = true;
            return part;
        }
        part.warnings << market.displayName
                + QStringLiteral(" Store lookup reached the total time limit; completed market results remain available.");
    }
    if (conceptRequired && !s.expired())
        verifyRegionalConcept(parsed, part, market, name, conceptId, sourceCid, s, userCancelled);
    return part;
}

void Engine::verifyRegionalConcept(const std::vector<DlcCandidate>& discovered, DlcLookupResult& part, const Market& market,
                                   const QString& name, const QString& conceptId, const QString& sourceCid,
                                   const Session& s, bool* userCancelled) const
{
    const QString conceptUrl = QStringLiteral("https://store.playstation.com/%1/concept/%2").arg(market.locale, conceptId);
    part.sourceLinks << conceptUrl;
    const auto addVerified = [&](DlcCandidate c) {
        c.conceptId = conceptId;
        c.gameName = name;
        c.mappingStatus = QStringLiteral("Official Store %1 release SKU observed — same game Concept %2").arg(market.country, conceptId);
        part.candidates.push_back(std::move(c));
    };
    QString conceptHtml;
    try {
        conceptHtml = s.fetch(conceptUrl);
        for (const QString& sku : detail::storeProductSkus(conceptHtml))
            for (const DlcCandidate& c : detail::parseStoreProduct(conceptHtml, conceptUrl, sku))
                if (c.conceptId == conceptId)
                    addVerified(c);
    } catch (const LookupError& e) {
        part.warnings << market.displayName + QStringLiteral(" game relationship lookup unavailable: ") + e.message;
    } catch (const LookupInterrupted& i) {
        if (i.user) {
            *userCancelled = true;
            return;
        }
        part.warnings << market.displayName
                + QStringLiteral(" game relationship lookup reached the time limit; unverified search hits were excluded.");
        return;
    }

    const QList<QJsonObject> caches = storeCaches(conceptHtml);
    std::vector<DlcCandidate> pending;
    int excluded = 0;
    QSet<QString> seen;
    for (const DlcCandidate& d : discovered) {
        if (seen.contains(d.contentId))
            continue;
        seen.insert(d.contentId);
        DlcCandidate c = d;
        const QString id = conceptIdIn(caches, c.contentId);
        if (id == conceptId) {
            const QString kind = productKindIn(caches, c.contentId);
            if (kind != u"Unknown")
                c.productKind = kind;
            addVerified(c);
        } else if (!id.isEmpty()) {
            ++excluded;
        } else if (c.productKind == u"Game" || c.productKind == u"DLC") {
            pending.push_back(c);
        }
    }
    const bool sourceValid = isLookupContentId(sourceCid);
    const auto sameSuffix = [&](const DlcCandidate& c) { return sourceValid && c.contentId.mid(16) == sourceCid.mid(16); };
    std::stable_sort(pending.begin(), pending.end(), [&](const DlcCandidate& a, const DlcCandidate& b) {
        const bool sa = sameSuffix(a);
        const bool sb = sameSuffix(b);
        if (sa != sb)
            return sa;
        return (a.productKind == u"Game" ? 0 : 1) < (b.productKind == u"Game" ? 0 : 1);
    });
    const std::size_t selected = std::min<std::size_t>(4, pending.size());
    for (std::size_t i = 0; i < selected; ++i) {
        DlcCandidate c = pending[i];
        try {
            const QList<QJsonObject> productCaches = storeCaches(s.fetch(c.sourceUrl));
            if (conceptIdIn(productCaches, c.contentId) == conceptId) {
                const QString kind = productKindIn(productCaches, c.contentId);
                if (kind != u"Unknown")
                    c.productKind = kind;
                addVerified(c);
            } else {
                ++excluded;
            }
        } catch (const LookupError& e) {
            part.warnings << market.displayName + QStringLiteral(" exact SKU relationship lookup unavailable: ") + e.message;
        } catch (const LookupInterrupted& interrupted) {
            if (interrupted.user) {
                *userCancelled = true;
                return;
            }
            part.warnings << market.displayName
                    + QStringLiteral(" exact SKU relationship lookup reached the time limit; completed verified results remain available.");
        }
    }
    if (excluded > 0 || pending.size() > selected)
        part.warnings << market.displayName
                + QStringLiteral(": search hits without the source game's confirmed Store Concept were excluded; narrow the "
                                 "query or use manual entry if an edition is missing.");
}

QString Engine::resolveTitleIndexName(const QString& titleId, const QString& sourceCid, DlcLookupResult& result,
                                      const Session& s) const
{
    const QString url = QStringLiteral("https://prosperopatches.com/") + titleId;
    result.sourceLinks << url;
    try {
        const QString html = s.fetch(url);
        QString hintName;
        QString hintCid;
        if (!detail::parseTitleIndexHint(html, titleId, sourceCid, &hintName, &hintCid)) {
            result.warnings << QStringLiteral("Title-index discovery did not match the exact source PPSA/publisher; its name was not used.");
            return {};
        }
        const QString prefix = isLookupContentId(sourceCid) ? sourceCid.left(6) : hintCid.left(6);
        const Market& market = marketForPrefix(prefix);
        const auto matches = [&](const DlcCandidate& c) {
            return c.titleId == titleId && c.contentId.startsWith(prefix + QLatin1Char('-')) && c.productKind == u"Game";
        };
        const QString json = s.fetch(detail::searchApiUrl(market, hintName));
        std::vector<DlcCandidate> observed = searchOrThrow(json, market, hintName, false);
        const QString refined = detail::refinedSearch(hintName);
        if (std::none_of(observed.begin(), observed.end(), matches) && detail::searchCount(json.toUtf8()) >= 100
            && !refined.isEmpty())
            append(observed, searchOrThrow(s.fetch(detail::searchApiUrl(market, refined)), market, hintName, false));
        std::vector<DlcCandidate> found;
        std::copy_if(observed.begin(), observed.end(), std::back_inserter(found), matches);
        if (found.empty()) {
            result.warnings << QStringLiteral(
                "Title-index name is an unverified discovery hint: no official source Store release matched the source PPSA "
                "and publisher. Enter the game name manually.");
            return {};
        }
        append(result.candidates, found);
        result.sourceLinks << detail::searchPageUrl(market, hintName);
        result.warnings << QStringLiteral(
                               "Game name discovered in ProsperoPatches and verified against an official %1 Store source "
                               "release with the exact source PPSA/publisher. All regional mappings still use independently "
                               "observed Store SKUs.")
                               .arg(market.country);
        return hintName;
    } catch (const LookupError& e) {
        result.warnings << QStringLiteral("Exact source-title discovery unavailable: %1. Manual game-name lookup remains available.").arg(e.message);
        return {};
    }
}

void Engine::lookupSerial(const QString& query, DlcLookupResult& result, const Session& s) const
{
    const QRegularExpressionMatch tm = titlePattern().match(query);
    const QString normalized = tm.hasMatch() ? tm.captured(1).toUpper() + tm.captured(2) : QString();
    const QString source = normalized.isEmpty() ? QStringLiteral("https://serialstation.com/search/?search=") + escaped(query)
                                                : detail::serialStationUrl(normalized);
    result.sourceLinks << source << detail::storeSearchUrl(query);
    QString html;
    try {
        html = s.fetch(source);
    } catch (const LookupError& e) {
        result.warnings << QStringLiteral("SerialStation unavailable: %1. Open its source link in a browser or use manual entry.").arg(e.message);
        return;
    }
    std::vector<DlcCandidate> titles;
    if (!normalized.isEmpty()) {
        append(result.candidates, detail::parseSerialTitle(html, normalized, source, false));
        const QStringList games = detail::serialLinks(html, QStringLiteral("/games/"));
        for (qsizetype i = 0; i < games.size() && i < 2; ++i) {
            try {
                append(titles, detail::parseSerialSearch(s.fetch(games.at(i)), QStringLiteral("Related game listing — verify edition/category")));
            } catch (const LookupError& e) {
                result.warnings << QStringLiteral("Related release lookup incomplete: ") + e.message;
            }
        }
    } else {
        append(titles, detail::parseSerialSearch(html, QStringLiteral("Name search candidate — ambiguous; verify edition")));
        result.warnings << QStringLiteral(
            "Name matches and related releases are candidates, not a DLC compatibility decision. Similarly named games, "
            "demos and editions may have different identifiers.");
    }
    std::vector<DlcCandidate> others;
    QSet<QString> seen;
    for (const DlcCandidate& c : titles) {
        const QString key = c.titleId.toUpper();
        if (seen.contains(key))
            continue;
        seen.insert(key);
        if (c.titleId != normalized)
            others.push_back(c);
    }
    if (others.size() > 12)
        result.warnings << QStringLiteral("Only the first 12 title matches were resolved. Narrow the query using an exact title ID.");

    // The Companion appends every match, then replaces each of the first 12 by
    // its own release rows (removed in place, rows appended at the end).
    const std::size_t base = result.candidates.size();
    append(result.candidates, others);
    std::vector<bool> removed(others.size(), false);
    std::vector<DlcCandidate> appended;
    const auto rebuild = [&] {
        std::vector<DlcCandidate> rebuilt(result.candidates.begin(), result.candidates.begin() + qsizetype(base));
        for (std::size_t i = 0; i < others.size(); ++i)
            if (!removed[i])
                rebuilt.push_back(others[i]);
        append(rebuilt, appended);
        result.candidates = std::move(rebuilt);
    };
    try {
        for (std::size_t i = 0; i < others.size() && i < 12; ++i) {
            try {
                const std::vector<DlcCandidate> rows =
                    detail::parseSerialTitle(s.fetch(others[i].sourceUrl), others[i].titleId, others[i].sourceUrl, true);
                if (!rows.empty()) {
                    removed[i] = true;
                    append(appended, rows);
                }
            } catch (const LookupError& e) {
                result.warnings << others[i].titleId + QStringLiteral(" release details unavailable: ") + e.message;
            }
        }
    } catch (...) {
        rebuild();
        throw;
    }
    rebuild();
}

} // namespace

// ---- parsers and URL builders (public for the unit tests) -------------------------------------

namespace detail {

void setTimeoutsForTests(int requestMs, int overallMs)
{
    g_requestTimeoutMs = requestMs;
    g_overallTimeoutMs = overallMs;
}

QString htmlDecode(const QString& s)
{
    if (!s.contains(QLatin1Char('&')))
        return s;
    QString out;
    out.reserve(s.size());
    for (qsizetype i = 0; i < s.size(); ++i) {
        const QChar ch = s.at(i);
        if (ch != u'&') {
            out += ch;
            continue;
        }
        // Only as far as an entity can reach (not to the end of the text for every "&").
        const qsizetype rel = QStringView(s).mid(i + 1, 33).indexOf(u';');
        const qsizetype semi = rel < 0 ? -1 : i + 1 + rel;
        if (semi > i + 1 && semi - i <= 33) {
            const QStringView entity = QStringView(s).mid(i + 1, semi - i - 1);
            if (entity.startsWith(u'#')) {
                bool ok = false;
                uint cp = 0;
                if (entity.size() > 2 && (entity.at(1) == u'x' || entity.at(1) == u'X'))
                    cp = entity.mid(2).toUInt(&ok, 16);
                else if (entity.size() > 1)
                    cp = entity.mid(1).toUInt(&ok, 10);
                if (ok && cp > 0 && cp <= 0x10FFFF && (cp < 0xD800 || cp > 0xDFFF)) {
                    const char32_t c32 = char32_t(cp);
                    out += QString::fromUcs4(&c32, 1);
                    i = semi;
                    continue;
                }
            } else if (const char16_t named = namedEntity(entity)) {
                out += QChar(named);
                i = semi;
                continue;
            }
        }
        out += ch;
    }
    return out;
}

QString htmlText(const QString& html)
{
    static const QRegularExpression scripts(QStringLiteral("<(script|style)\\b[^>]*>.*?</\\1>"), kSingleCi);
    static const QRegularExpression tags(QStringLiteral("<[^>]*>"), kSingle);
    static const QRegularExpression spaces(QStringLiteral("\\s+"), QRegularExpression::UseUnicodePropertiesOption);
    QString s = html;
    s.replace(scripts, QString());
    s.replace(tags, QStringLiteral(" "));
    s = htmlDecode(s);
    s.replace(spaces, QStringLiteral(" "));
    return s.trimmed();
}

bool isGameName(const QString& value)
{
    const QString s = value.trimmed();
    if (s.isEmpty() || s.size() > 240)
        return false;
    if (s.contains(QStringLiteral("://")) || s.startsWith(QStringLiteral("http:"), Qt::CaseInsensitive)
        || s.startsWith(QStringLiteral("https:"), Qt::CaseInsensitive))
        return false;
    static const QRegularExpression ids(QStringLiteral("^(PPSA|CUSA)-?\\d{5}$|^[A-Z]{2}\\d{4}-(PPSA|CUSA)\\d{5}_"), kCi);
    static const QRegularExpression token(QStringLiteral("\\A[A-Z0-9_]{12,}\\z"));
    return !ids.match(s).hasMatch() && !token.match(s).hasMatch();
}

bool isDlcDisplayLabel(const QString& value)
{
    static const QRegularExpression re(
        QStringLiteral("^(?:(?:digital\\s+)?deluxe(?:\\s+edition)?(?:\\s+(?:rewards?|bonus(?:es)?|contents?))?"
                       "|pre[-\\s]?order(?:\\s+bonus(?:es)?)?(?:\\s+contents?)?|bonus(?:es)?(?:\\s+contents?)?"
                       "|(?:digital\\s+)?deluxe\\s+upgrade(?:\\s+bundle)?|season\\s+pass|expansion\\s+pass|dlc(?:\\s*\\d+)?)$"),
        kCi);
    return re.match(value.trimmed()).hasMatch();
}

QString knownName(const QString& titleId)
{
    const QString t = titleId.toUpper();
    if (t == u"PPSA24662")
        return QStringLiteral("F1 25");
    if (t == u"PPSA10664" || t == u"PPSA10665" || t == u"PPSA08030" || t == u"PPSA10666")
        return QStringLiteral("FINAL FANTASY XVI");
    return {};
}

QString serialStationUrl(const QString& query)
{
    const QRegularExpressionMatch m = titlePattern().match(query);
    if (!m.hasMatch())
        return QStringLiteral("https://serialstation.com/search/?search=") + escaped(query);
    return QStringLiteral("https://serialstation.com/titles/%1/%2").arg(m.captured(1).toUpper(), m.captured(2));
}

QString storeSearchUrl(const QString& query)
{
    return QStringLiteral("https://store.playstation.com/en-gb/search/") + escaped(query);
}

QString searchApiUrl(const Market& market, const QString& name)
{
    return QStringLiteral("https://store.playstation.com/store/api/chihiro/00_09_000/tumbler/%1/%2/999/%3?size=100")
        .arg(market.country, market.language, escaped(name));
}

QString searchPageUrl(const Market& market, const QString& name)
{
    return QStringLiteral("https://store.playstation.com/%1/search/%2").arg(market.locale, escaped(name));
}

bool tryStoreProduct(const QString& input, QString* storeUrl, QString* contentId)
{
    storeUrl->clear();
    contentId->clear();
    if (isLookupContentId(input)) {
        *contentId = input.toUpper();
        QString locale = QStringLiteral("en-gb");
        if (contentId->startsWith(QStringLiteral("UP")))
            locale = QStringLiteral("en-us");
        else if (contentId->startsWith(QStringLiteral("JP")))
            locale = QStringLiteral("ja-jp");
        else if (contentId->startsWith(QStringLiteral("HP")))
            locale = QStringLiteral("en-hk");
        else if (contentId->startsWith(QStringLiteral("KP")))
            locale = QStringLiteral("ko-kr");
        *storeUrl = QStringLiteral("https://store.playstation.com/%1/product/%2").arg(locale, *contentId);
        return true;
    }
    const QUrl url(input);
    if (!url.isValid() || url.isRelative() || !eqCi(url.scheme(), QStringLiteral("https"))
        || !eqCi(url.host(), QStringLiteral("store.playstation.com")) || !url.userInfo().isEmpty()
        || (url.port() != -1 && url.port() != 443))
        return false;
    static const QRegularExpression path(QStringLiteral("^/([a-z]{2}-[a-z]{2})/product/([^/]+)/?$"), kCi);
    const QRegularExpressionMatch m = path.match(url.path(QUrl::FullyEncoded));
    if (!m.hasMatch() || !isLookupContentId(m.captured(2)))
        return false;
    *contentId = m.captured(2).toUpper();
    *storeUrl = QStringLiteral("https://store.playstation.com/%1/product/%2").arg(m.captured(1).toLower(), *contentId);
    return true;
}

QString cacheFileName(const QString& key)
{
    return QString::fromLatin1(QCryptographicHash::hash(key.toUpper().toUtf8(), QCryptographicHash::Sha256).toHex().toUpper())
        + QStringLiteral(".json");
}

std::vector<DlcCandidate> parseStoreProduct(const QString& html, const QString& sourceUrl, const QString& expectedContentId)
{
    std::vector<DlcCandidate> out;
    const QList<QJsonObject> caches = storeCaches(html);
    for (const QJsonValue& block : ldJsonBlocks(html))
        readProduct(block, sourceUrl, expectedContentId, caches, out);
    return distinctCandidates(out);
}

QString parseStoreGameName(const QString& html, const QString& contentId) { return gameNameIn(storeCaches(html), contentId); }
QString parseStoreConceptId(const QString& html, const QString& contentId) { return conceptIdIn(storeCaches(html), contentId); }
QString parseStoreProductKind(const QString& html, const QString& contentId) { return productKindIn(storeCaches(html), contentId); }

QStringList storeProductSkus(const QString& html)
{
    QStringList all;
    for (const QJsonValue& block : ldJsonBlocks(html))
        readSkus(block, all);
    QStringList out;
    for (const QString& sku : all)
        if (!out.contains(sku) && out.size() < qsizetype(kMaxCandidates))
            out << sku;
    return out;
}

bool sameGameOrEdition(const QString& item, const QString& query, bool allowDemo)
{
    const QStringList itemWords = words(item);
    const QStringList queryWords = words(query);
    if (!hasWordPrefix(itemWords, queryWords))
        return false;
    const QString rest = itemWords.mid(queryWords.size()).join(QLatin1Char(' '));
    if (rest.isEmpty())
        return true;
    static const QRegularExpression edition(QStringLiteral(
        "^(?:(?:PS4|PS5)(?: (?:PS4|PS5))?|(?:STANDARD|DELUXE|DIGITAL DELUXE|ULTIMATE|GOLD|COMPLETE|DEFINITIVE|PREMIUM|"
        "CHAMPIONS|COLLECTOR S)(?: EDITION)?|GAME OF THE YEAR(?: EDITION)?|GOTY(?: EDITION)?|EDITION)$"));
    if (edition.match(rest).hasMatch())
        return true;
    return allowDemo && (rest == u"DEMO" || rest == u"TRIAL");
}

QString refinedSearch(const QString& name)
{
    static const QRegularExpression re(QStringLiteral("[\\p{L}\\p{N}]+"));
    QStringList all;
    for (auto it = re.globalMatch(name); it.hasNext();)
        all << it.next().captured(0);
    if (all.size() <= 2)
        return {};
    return all.mid(all.size() - 2).join(QLatin1Char(' '));
}

int searchCount(const QByteArray& json)
{
    const QJsonValue links = QJsonDocument::fromJson(json).object().value(QStringLiteral("links"));
    return links.isArray() ? int(links.toArray().size()) : 0;
}

Result<std::vector<DlcCandidate>> parseSearch(const QByteArray& json, const Market& market, const QString& query,
                                              bool conceptRequired)
{
    QJsonParseError pe{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &pe);
    if (pe.error != QJsonParseError::NoError)
        return Error(ErrorCode::CorruptData, pe.errorString());
    std::vector<DlcCandidate> out;
    const QJsonValue links = doc.object().value(QStringLiteral("links"));
    if (!links.isArray())
        return out;
    const QStringList queryWords = words(query);
    static const QRegularExpression idRe(QStringLiteral("\\A[A-Z]{2}\\d{4}-((PPSA|CUSA)\\d{5})_\\d{2}-[A-Z0-9_]{16}\\z"));
    static const QRegularExpression demoRe(QStringLiteral("\\b(demo|trial)\\b"),
                                           QRegularExpression::CaseInsensitiveOption | QRegularExpression::UseUnicodePropertiesOption);
    const QJsonArray items = links.toArray();
    for (qsizetype i = 0; i < items.size() && i < 100; ++i) {
        const QJsonValue item = items.at(i);
        const QString id = jsonString(item, QStringLiteral("id")).toUpper();
        const QString name = htmlDecode(jsonString(item, QStringLiteral("name")));
        const QRegularExpressionMatch m = idRe.match(id);
        if (!m.hasMatch() || name.isEmpty() || queryWords.isEmpty())
            continue;
        const QString platform = m.captured(2) == u"PPSA" ? QStringLiteral("PS5") : QStringLiteral("PS4");
        const QJsonValue playable = item.toObject().value(QStringLiteral("playable_platform"));
        if (playable.isArray()) {
            bool listed = false;
            for (const QJsonValue& p : playable.toArray())
                if (p.isString() && p.toString() == platform)
                    listed = true;
            if (!listed)
                continue;
        }
        const QString parentName = jsonString(item, QStringLiteral("parent_name"));
        const QString topCategory = jsonString(item, QStringLiteral("top_category"));
        const QString contentType = jsonString(item, QStringLiteral("game_contentType"));
        const bool isAddOn = eqCi(topCategory, QStringLiteral("add_on"))
            || eqCi(metadataValue(item, QStringLiteral("top_category")), QStringLiteral("ADD_ON"));
        const bool isDemo = eqCi(metadataValue(item, QStringLiteral("secondary_classification")), QStringLiteral("DEMO"))
            || eqCi(contentType, QStringLiteral("Demo")) || demoRe.match(name).hasMatch();
        const bool isGame = eqCi(topCategory, QStringLiteral("downloadable_game"))
            || eqCi(metadataValue(item, QStringLiteral("top_category")), QStringLiteral("GAME"));
        const QString kind = isAddOn ? QStringLiteral("DLC")
            : isDemo                 ? QStringLiteral("Demo")
            : isGame                 ? QStringLiteral("Game")
                                     : QStringLiteral("Unknown");
        bool keep = false;
        if (conceptRequired)
            keep = hasWordPrefix(words(name), queryWords) || (isAddOn && hasWordPrefix(words(parentName), queryWords));
        else
            keep = isAddOn ? sameGameOrEdition(parentName, query, false) : sameGameOrEdition(name, query, isDemo);
        if (!keep)
            continue;
        DlcCandidate c;
        c.titleId = m.captured(1);
        c.contentId = id;
        c.name = name;
        c.edition = !contentType.isEmpty() ? contentType : isAddOn ? QStringLiteral("Add-on") : QStringLiteral("Edition not reported");
        c.region = market.displayName;
        c.marketRegionCode = market.code;
        c.storeLocale = market.locale;
        c.platform = platform;
        c.gameName = isAddOn ? (parentName.isEmpty() ? query : parentName) : name;
        c.productKind = kind;
        c.sourceUrl = QStringLiteral("https://store.playstation.com/%1/product/%2").arg(market.locale, id);
        c.mappingStatus = QStringLiteral("Official Store %1 release SKU observed; %2")
                              .arg(market.country, isAddOn ? QStringLiteral("add-on")
                                                   : isDemo ? QStringLiteral("demo/trial")
                                                            : QStringLiteral("check edition"));
        c.compatibilityStatus = QStringLiteral("Unverified — observed Store identity does not prove DLC compatibility");
        out.push_back(std::move(c));
    }
    return out;
}

std::vector<DlcCandidate> parseSerialTitle(const QString& html, const QString& titleIdIn, const QString& sourceUrl, bool related)
{
    std::vector<DlcCandidate> out;
    const QRegularExpressionMatch tm = titlePattern().match(titleIdIn);
    if (!tm.hasMatch())
        return out;
    const QString titleId = tm.captured(1).toUpper() + tm.captured(2);
    static const QRegularExpression h1(QStringLiteral("<h1\\b[^>]*>(.*?)</h1>"), kSingleCi);
    static const QRegularExpression heading(QStringLiteral("<h[2-6]\\b[^>]*>(.*?)</h[2-6]>"), kSingleCi);
    const QString title = htmlText(capture1(h1, html));
    bool headed = false;
    for (auto it = heading.globalMatch(html); it.hasNext() && !headed;)
        headed = eqCi(htmlText(it.next().captured(1)).remove(QLatin1Char('-')), titleId);
    const bool detailed =
        eqCi(detailValue(html, QStringLiteral("Title ID Type")) + detailValue(html, QStringLiteral("Title ID Number")), titleId);
    if (title.isEmpty() || (!headed && !detailed))
        return out;
    const QString category = detailValue(html, QStringLiteral("Category"));
    const QString status = related ? QStringLiteral("Database title/release mapping — related game; verify edition")
                                   : QStringLiteral("Database title/release mapping — exact title ID");
    const QString platform = titleId.startsWith(QStringLiteral("PPSA")) ? QStringLiteral("PS5") : QStringLiteral("PS4");
    for (const HtmlTable& table : htmlTables(html)) {
        const qsizetype edition = headerIndex(table.headers, QStringLiteral("Edition"));
        const qsizetype regions = headerIndex(table.headers, QStringLiteral("Regions"));
        const qsizetype nameCol = headerIndex(table.headers, QStringLiteral("Name"));
        if (regions < 0 || nameCol < 0 || edition < 0)
            continue;
        for (const QStringList& row : table.rows) {
            if (row.size() <= std::max({regions, edition, nameCol}))
                continue;
            if (row.at(nameCol).startsWith(QStringLiteral("No "), Qt::CaseInsensitive))
                continue;
            DlcCandidate c;
            c.titleId = titleId;
            c.name = row.at(nameCol);
            c.edition = row.at(edition);
            c.region = row.at(regions).isEmpty() ? QStringLiteral("Unknown") : row.at(regions);
            c.platform = platform;
            c.sourceUrl = sourceUrl;
            c.mappingStatus = status;
            out.push_back(std::move(c));
        }
    }
    if (out.empty()) {
        DlcCandidate c;
        c.titleId = titleId;
        c.name = title;
        c.edition = category == u"Game" ? QStringLiteral("Not reported") : category;
        c.platform = platform;
        c.sourceUrl = sourceUrl;
        c.mappingStatus = status + QStringLiteral("; region unknown");
        out.push_back(std::move(c));
    }
    return distinctCandidates(out);
}

std::vector<DlcCandidate> parseSerialSearch(const QString& html, const QString& mappingStatus)
{
    static const QRegularExpression link(
        QStringLiteral("<a\\b[^>]*href\\s*=\\s*['\"](?<url>[^'\"]+)['\"][^>]*>(?<label>.*?)</a>"), kSingleCi);
    static const QRegularExpression relative(QStringLiteral("^/?titles/(PPSA|CUSA)/(\\d{5})/?$"), kCi);
    static const QRegularExpression absolute(QStringLiteral("^/titles/(PPSA|CUSA)/(\\d{5})/?$"), kCi);
    static const QRegularExpression cell(QStringLiteral("<td\\b[^>]*>(.*?)</td>"), kSingleCi);
    std::vector<DlcCandidate> out;
    for (auto it = link.globalMatch(html); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        const QString href = htmlDecode(m.captured(QStringLiteral("url")));
        QRegularExpressionMatch tm = relative.match(href);
        if (!tm.hasMatch()) {
            const QUrl url(href);
            if (url.isValid() && !url.isRelative() && isSerialHost(url.host()))
                tm = absolute.match(url.path(QUrl::FullyEncoded));
        }
        if (!tm.hasMatch())
            continue;
        const QString titleId = tm.captured(1).toUpper() + tm.captured(2);
        QString label = htmlText(m.captured(QStringLiteral("label")));
        const qsizetype at = m.capturedStart(0);
        const qsizetype rowStart = at > 0 ? html.lastIndexOf(QStringLiteral("<tr"), at - 1, Qt::CaseInsensitive) : -1;
        const qsizetype rowEnd = html.indexOf(QStringLiteral("</tr>"), at, Qt::CaseInsensitive);
        if (rowStart >= 0 && rowEnd > rowStart && rowEnd - rowStart < 16000) {
            QStringList cells;
            const QString row = html.mid(rowStart, rowEnd - rowStart);
            for (auto c = cell.globalMatch(row); c.hasNext();)
                cells << htmlText(c.next().captured(1));
            if (cells.size() > 1)
                label = cells.at(1);
        }
        DlcCandidate c;
        c.titleId = titleId;
        c.name = label;
        c.edition = QStringLiteral("Not reported");
        c.platform = titleId.startsWith(QStringLiteral("PPSA")) ? QStringLiteral("PS5") : QStringLiteral("PS4");
        c.sourceUrl = serialStationUrl(titleId);
        c.mappingStatus = mappingStatus;
        out.push_back(std::move(c));
    }
    return firstN(distinctCandidates(out), kMaxCandidates);
}

QStringList serialLinks(const QString& html, const QString& pathPrefix)
{
    static const QRegularExpression link(QStringLiteral("<a\\b[^>]*href\\s*=\\s*['\"]([^'\"]+)['\"]"), kSingleCi);
    const QUrl base(QStringLiteral("https://serialstation.com"));
    QStringList out;
    for (auto it = link.globalMatch(html); it.hasNext();) {
        const QUrl url = base.resolved(QUrl(htmlDecode(it.next().captured(1))));
        if (url.isValid() && url.scheme() == u"https" && isSerialHost(url.host())
            && url.path(QUrl::FullyEncoded).startsWith(pathPrefix))
            out << url.toString(QUrl::FullyEncoded);
    }
    return out;
}

bool parseTitleIndexHint(const QString& html, const QString& titleId, const QString& sourceContentId, QString* name,
                         QString* contentId)
{
    static const QRegularExpression ppsa(QStringLiteral("\\APPSA\\d{5}\\z"));
    if (!ppsa.match(titleId).hasMatch())
        return false;
    static const QRegularExpression h1(QStringLiteral("<h1\\b[^>]*>(.*?)</h1>"), kSingleCi);
    const QString hintName = indexText(capture1(h1, html));
    const QString hintTitle = indexField(html, QStringLiteral("Title ID"));
    const QString hintCid = indexField(html, QStringLiteral("Content ID"));
    if (hintTitle != titleId || !isGameName(hintName))
        return false;
    const QRegularExpression cidRe(QStringLiteral("\\A[A-Z]{2}\\d{4}-") + QRegularExpression::escape(titleId)
                                   + QStringLiteral("_\\d{2}-[A-Z0-9_]{16}\\z"));
    if (!cidRe.match(hintCid).hasMatch())
        return false;
    if (isStrictCid(sourceContentId) && (sourceContentId.left(6) != hintCid.left(6) || sourceContentId.mid(7, 9) != titleId))
        return false;
    *name = hintName;
    *contentId = hintCid;
    return true;
}

} // namespace detail

// ---- public API ---------------------------------------------------------------------------------

const QList<Market>& markets()
{
    static const QList<Market> list = {
        {QStringLiteral("US"), QStringLiteral("en"), QStringLiteral("en-us"), QStringLiteral("UP"), QStringLiteral("Americas (US)")},
        {QStringLiteral("GB"), QStringLiteral("en"), QStringLiteral("en-gb"), QStringLiteral("EP"), QStringLiteral("Europe (UK)")},
        {QStringLiteral("JP"), QStringLiteral("ja"), QStringLiteral("ja-jp"), QStringLiteral("JP"), QStringLiteral("Japan")},
        {QStringLiteral("HK"), QStringLiteral("en"), QStringLiteral("en-hk"), QStringLiteral("HP"), QStringLiteral("Asia (Hong Kong)")},
        {QStringLiteral("KR"), QStringLiteral("ko"), QStringLiteral("ko-kr"), QStringLiteral("KP"), QStringLiteral("Korea")},
    };
    return list;
}

const Market& marketForPrefix(const QString& prefix)
{
    for (const Market& m : markets())
        if (prefix.startsWith(m.code))
            return m;
    return markets().at(1);
}

DlcLookupService::DlcLookupService(QString cacheDir, Fetch fetch)
    : m_cacheDir(std::move(cacheDir)), m_fetch(fetch ? std::move(fetch) : Fetch(&httpsGet))
{
}

QString DlcLookupService::userAgent() { return QStringLiteral("exFAT-Image-Builder/5.0 (+optional-title-metadata-lookup)"); }

bool DlcLookupService::isTrustedSource(const QUrl& url)
{
    if (!url.isValid() || url.isRelative() || !eqCi(url.scheme(), QStringLiteral("https")))
        return false;
    if ((url.port() != -1 && url.port() != 443) || !url.userInfo().isEmpty())
        return false;
    const QString host = url.host();
    return isSerialHost(host) || eqCi(host, QStringLiteral("store.playstation.com"))
        || eqCi(host, QStringLiteral("prosperopatches.com"));
}

DlcLookupResult DlcLookupService::lookup(const QString& query, const CancellationToken& token)
{
    Engine engine(m_cacheDir, m_fetch, token);
    try {
        return engine.lookup(query);
    } catch (const LookupRejected& r) {
        return messageResult(r.message);
    } catch (const LookupInterrupted& i) {
        return messageResult(i.user ? Error::cancelled().message
                                    : QStringLiteral("Online lookup reached its time limit. Local editing and building remain available."));
    } catch (const LookupError& e) {
        return messageResult(QStringLiteral("Online lookup unavailable: %1. Local editing and building remain available.").arg(e.message));
    }
}

DlcLookupResult DlcLookupService::resolveRegions(const QString& titleId, const QString& contentId, const QString& gameName,
                                                 const CancellationToken& token)
{
    Engine engine(m_cacheDir, m_fetch, token);
    try {
        return engine.resolveCore(titleId, contentId, gameName, g_overallTimeoutMs.load(), false, QString());
    } catch (const LookupRejected& r) {
        return messageResult(r.message);
    } catch (const LookupInterrupted& i) {
        return messageResult(i.user ? Error::cancelled().message
                                    : QStringLiteral("Regional lookup reached its time limit. Local editing remains available."));
    } catch (const LookupError& e) {
        return messageResult(QStringLiteral("Online lookup unavailable: %1. Local editing and building remain available.").arg(e.message));
    }
}

// ---- DlcRegionSelection -----------------------------------------------------------------------------

namespace {

bool mappableKind(const QString& kind) { return kind == u"Game" || kind == u"Bundle" || kind == u"DLC"; }

} // namespace

bool canMap(const DlcCandidate& c, const QString& sourceContentId)
{
    if (!isTitleId(c.titleId) || !isContentId(c.contentId) || !mappableKind(c.productKind))
        return false;
    if (titleIdOf(c.contentId) != c.titleId)
        return false;
    return isContentId(sourceContentId) && sourceContentId.mid(7, 4) == c.titleId.left(4);
}

bool isExactDlcSku(const DlcCandidate& c, const QString& sourceContentId)
{
    return c.productKind == u"DLC" && isContentId(c.contentId) && isContentId(sourceContentId)
        && c.contentId.mid(20) == sourceContentId.mid(20);
}

Result<DlcProposal> propose(const DlcCandidate& c, const QString& sourceContentId, const QString& title)
{
    if (!canMap(c, sourceContentId))
        return Error(ErrorCode::InvalidArgument,
                     QStringLiteral("This release has no observed content ID for the same platform. Choose another release or "
                                    "use manual identifiers."));
    QString contentId;
    if (isExactDlcSku(c, sourceContentId)) {
        contentId = c.contentId;
    } else {
        Result<QString> retargeted = retargetContentId(sourceContentId, c.titleId, c.contentId.left(6));
        if (!retargeted.ok())
            return retargeted.takeError();
        contentId = retargeted.value();
    }
    Status valid = validateIdentity(c.titleId, contentId);
    if (!valid.ok())
        return valid.takeError();
    return DlcProposal{c.titleId, contentId, title};
}

std::vector<DlcCandidate> forRegion(const std::vector<DlcCandidate>& candidates, const QString& code,
                                    const QString& sourceContentId)
{
    const QString platform = titleIdOf(sourceContentId);
    std::vector<DlcCandidate> out;
    QHash<QString, std::size_t> index;
    for (const DlcCandidate& c : candidates) {
        if (!mappableKind(c.productKind) || c.titleId.isEmpty())
            continue;
        if (!platform.isEmpty() && !c.titleId.startsWith(platform.left(4)))
            continue;
        if (!code.isEmpty() && (c.marketRegionCode.isEmpty() ? regionCode(c.contentId) : c.marketRegionCode) != code)
            continue;
        const QString key = QStringList{c.titleId, c.contentId, c.name, c.edition, c.productKind, c.marketRegionCode}
                                .join(QLatin1Char('|'))
                                .toUpper();
        const auto it = index.constFind(key);
        if (it == index.constEnd()) {
            index.insert(key, out.size());
            out.push_back(c);
        } else if (!isContentId(out[*it].contentId) && isContentId(c.contentId)) {
            out[*it] = c; // the group keeps its first member with a valid content ID
        }
    }
    return out;
}

std::vector<DlcCandidate> dlcForRegion(const std::vector<DlcCandidate>& candidates, const QString& code,
                                       const QString& sourceContentId)
{
    std::vector<DlcCandidate> out;
    for (const DlcCandidate& c : forRegion(candidates, code, sourceContentId))
        if (c.productKind == u"DLC")
            out.push_back(c);
    return out;
}

std::vector<DlcCandidate> gameMappings(const std::vector<DlcCandidate>& candidates, const QString& code,
                                       const QString& sourceContentId)
{
    std::vector<DlcCandidate> out;
    for (const DlcCandidate& c : forRegion(candidates, code, sourceContentId))
        if (c.productKind != u"DLC" && canMap(c, sourceContentId))
            out.push_back(c);
    return out;
}

bool hasOneIdentity(const std::vector<DlcCandidate>& candidates, const QString& sourceContentId)
{
    if (candidates.empty())
        return false;
    QSet<QString> identities;
    QSet<QString> exact;
    for (const DlcCandidate& c : candidates) {
        if (!canMap(c, sourceContentId))
            return false;
        identities.insert((c.titleId + QLatin1Char('|') + c.contentId.left(6)).toUpper());
        if (isExactDlcSku(c, sourceContentId))
            exact.insert(c.contentId);
    }
    return identities.size() == 1 && exact.size() <= 1;
}

} // namespace fw::dlc
