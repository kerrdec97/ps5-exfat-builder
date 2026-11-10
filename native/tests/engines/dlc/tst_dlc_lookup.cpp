// DLC online lookup (the ppr-pkg Companion's DlcLookupService, DlcRegionalCatalog
// and DlcRegionSelection): Store / SerialStation / ProsperoPatches parsers, query
// routing, the regional resolver, fetch failures, the 7-day cache and target
// selection. Offline only: every service gets a fake fetch that serves the
// hand-written fixtures in tests/data/dlc/lookup (unknown URLs answer
// "HTTP 404 from <host>"), so the real network is never touched.

#include "support/TestSupport.h"

#include "core/Cancellation.h"
#include "engines/dlc/DlcLookup.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTest>

#include <memory>
#include <optional>

// Parser and URL helpers defined in DlcLookup.cpp (external linkage, not in the header).
namespace fw::dlc::detail {
void setTimeoutsForTests(int requestMs, int overallMs);
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
} // namespace fw::dlc::detail

using namespace fw;
using namespace fw::dlc;
namespace lk = fw::dlc::detail;

namespace {

QString q(const char* s) { return QString::fromUtf8(s); }

// The error of a call that must have failed (a marker error, not undefined
// behaviour, when it succeeded).
template <typename R>
Error errorOf(const R& result)
{
    return result.ok() ? Error(ErrorCode::Internal, QStringLiteral("<the call succeeded>")) : result.error();
}

const QString kSourceCid = QStringLiteral("EP0700-PPSA01234_00-DLCSUFFIX0000001");
const QString kSourceProductUrl = QStringLiteral("https://store.playstation.com/en-gb/product/EP0700-PPSA01234_00-DLCSUFFIX0000001");
const QString kGameName = QStringLiteral("Example Quest");

const char* kAlwaysWarning =
    "Store identifiers are observed release mappings. Check edition and platform; DLC compatibility still needs a console test.";
const char* kNoStoreRelease =
    "No matching Store release was observed. Correct the game name or enter target identifiers manually.";

QByteArray fixture(const QString& name)
{
    QFile f(fwtest::dataDir() + QStringLiteral("/dlc/lookup/") + name);
    if (!f.open(QIODevice::ReadOnly))
        qWarning("missing fixture %s", qPrintable(name));
    return f.readAll();
}

QString fixtureText(const QString& name) { return QString::fromUtf8(fixture(name)); }

const Market& market(const QString& country)
{
    for (const Market& m : markets())
        if (m.country == country)
            return m;
    return markets().at(0);
}

QString universal(const QDateTime& t) { return t.toUTC().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")) + QLatin1Char('Z'); }

// A tiny offline web: URL -> body or error. Thread-safe (markets run in parallel).
class FakeWeb : public std::enable_shared_from_this<FakeWeb> {
public:
    void body(const QString& url, const QByteArray& data)
    {
        QMutexLocker lock(&m_mutex);
        m_routes.insert(norm(url), Route{data, std::nullopt});
    }
    void fail(const QString& url, ErrorCode code, const QString& message)
    {
        QMutexLocker lock(&m_mutex);
        m_routes.insert(norm(url), Route{{}, Error(code, message)});
    }
    void remove(const QString& url)
    {
        QMutexLocker lock(&m_mutex);
        m_routes.remove(norm(url));
    }
    void setAllTimeOut(bool on) { m_allTimeOut = on; }
    QStringList requested() const
    {
        QMutexLocker lock(&m_mutex);
        return m_requested;
    }
    QList<int> timeouts() const
    {
        QMutexLocker lock(&m_mutex);
        return m_timeouts;
    }
    int count() const { return int(requested().size()); }

    DlcLookupService::Fetch fetch()
    {
        std::shared_ptr<FakeWeb> self = shared_from_this();
        return [self](const QUrl& url, int timeoutMs, const CancellationToken&) -> Result<QByteArray> {
            QMutexLocker lock(&self->m_mutex);
            self->m_requested << url.toString(QUrl::FullyEncoded);
            self->m_timeouts << timeoutMs;
            if (self->m_allTimeOut)
                return Error(ErrorCode::Timeout, QStringLiteral("timed out"));
            const auto it = self->m_routes.constFind(url.toString(QUrl::FullyEncoded));
            if (it == self->m_routes.constEnd())
                return Error(ErrorCode::ExternalToolFailed, QStringLiteral("HTTP 404 from %1").arg(url.host()));
            if (it->error)
                return *it->error;
            return it->data;
        };
    }

private:
    struct Route {
        QByteArray data;
        std::optional<Error> error;
    };
    static QString norm(const QString& url) { return QUrl(url).toString(QUrl::FullyEncoded); }

    mutable QMutex m_mutex;
    QHash<QString, Route> m_routes;
    QStringList m_requested;
    QList<int> m_timeouts;
    bool m_allTimeOut = false;
};

// The Store as the fixtures describe it: the source DLC's product page (EP),
// US and UK searches and pages for "Example Quest" (Concept 10001234), a 403
// in Japan, a browser challenge in Hong Kong and a timeout in Korea.
std::shared_ptr<FakeWeb> exampleWeb()
{
    auto web = std::make_shared<FakeWeb>();
    web->body(kSourceProductUrl, fixture(QStringLiteral("store_product_dlc_gb.html")));
    web->body(lk::searchApiUrl(market(QStringLiteral("US")), kGameName), fixture(QStringLiteral("search_us.json")));
    web->body(lk::searchApiUrl(market(QStringLiteral("GB")), kGameName), fixture(QStringLiteral("search_gb.json")));
    web->fail(lk::searchApiUrl(market(QStringLiteral("JP")), kGameName), ErrorCode::ExternalToolFailed,
              q("HTTP 403 from store.playstation.com (browser challenge or access restriction)"));
    web->body(lk::searchApiUrl(market(QStringLiteral("HK")), kGameName), fixture(QStringLiteral("challenge.html")));
    web->fail(lk::searchApiUrl(market(QStringLiteral("KR")), kGameName), ErrorCode::Timeout, q("timed out"));
    web->body(q("https://store.playstation.com/en-us/concept/10001234"), fixture(QStringLiteral("store_concept_us.html")));
    web->body(q("https://store.playstation.com/en-us/product/UP0700-PPSA05678_00-EXAMPLEQUESTDLX0"),
              fixture(QStringLiteral("store_product_deluxe_us.html")));
    web->body(q("https://store.playstation.com/en-gb/product/EP0700-PPSA01234_00-EXAMPLEQUEST0000"),
              fixture(QStringLiteral("store_product_game_gb.html")));
    web->body(q("https://store.playstation.com/en-gb/product/EP0700-PPSA01234_01-DLCSUFFIX0000001"),
              fixture(QStringLiteral("store_product_dlc01_gb.html")));
    web->body(q("https://prosperopatches.com/PPSA01234"), fixture(QStringLiteral("prospero_title.html")));
    web->body(q("https://serialstation.com/titles/PPSA/01234"), fixture(QStringLiteral("serial_title.html")));
    web->body(q("https://serialstation.com/search/?search=ABCDEFGHIJKLMNOP"), fixture(QStringLiteral("serial_search.html")));
    return web;
}

const DlcCandidate* find(const std::vector<DlcCandidate>& list, const QString& contentId)
{
    for (const DlcCandidate& c : list)
        if (c.contentId == contentId)
            return &c;
    return nullptr;
}

bool anyWarningStartsWith(const DlcLookupResult& r, const QString& prefix)
{
    for (const QString& w : r.warnings)
        if (w.startsWith(prefix))
            return true;
    return false;
}

DlcCandidate cand(const QString& titleId, const QString& contentId, const QString& kind, const QString& marketCode)
{
    DlcCandidate c;
    c.titleId = titleId;
    c.contentId = contentId;
    c.productKind = kind;
    c.marketRegionCode = marketCode;
    c.name = QStringLiteral("Example Quest");
    c.edition = QStringLiteral("Standard");
    return c;
}

QJsonObject readJson(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

bool writeJson(const QString& path, const QJsonObject& o)
{
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(QJsonDocument(o).toJson()) > 0;
}

} // namespace

#define VERIFY_WARNING(result, text) QVERIFY2((result).warnings.contains(text), qPrintable((result).warnings.join(u'\n')))

class TstDlcLookup : public QObject {
    Q_OBJECT

private slots:
    void cleanup() { lk::setTimeoutsForTests(6000, 25000); }

    void basics()
    {
        QCOMPARE(int(markets().size()), 5);
        QStringList names;
        for (const Market& m : markets())
            names << QStringList{m.country, m.language, m.locale, m.code, m.displayName}.join(u'/');
        QCOMPARE(names, QStringList({q("US/en/en-us/UP/Americas (US)"), q("GB/en/en-gb/EP/Europe (UK)"), q("JP/ja/ja-jp/JP/Japan"),
                                     q("HK/en/en-hk/HP/Asia (Hong Kong)"), q("KR/ko/ko-kr/KP/Korea")}));
        QCOMPARE(marketForPrefix(q("UP0700")).country, q("US"));
        QCOMPARE(marketForPrefix(q("KP1234")).country, q("KR"));
        QCOMPARE(marketForPrefix(q("XX0000")).country, q("GB"));
        QCOMPARE(marketForPrefix(QString()).country, q("GB"));
        QCOMPARE(DlcLookupService::userAgent(), q("exFAT-Image-Builder/5.0 (+optional-title-metadata-lookup)"));

        QVERIFY(DlcLookupService::isTrustedSource(QUrl(q("https://store.playstation.com/en-us/product/X"))));
        QVERIFY(DlcLookupService::isTrustedSource(QUrl(q("https://WWW.SerialStation.com/titles/PPSA/01234"))));
        QVERIFY(DlcLookupService::isTrustedSource(QUrl(q("https://serialstation.com/"))));
        QVERIFY(DlcLookupService::isTrustedSource(QUrl(q("https://prosperopatches.com:443/PPSA01234"))));
        QVERIFY(!DlcLookupService::isTrustedSource(QUrl(q("http://store.playstation.com/"))));
        QVERIFY(!DlcLookupService::isTrustedSource(QUrl(q("https://store.playstation.com:8443/"))));
        QVERIFY(!DlcLookupService::isTrustedSource(QUrl(q("https://user:pw@store.playstation.com/"))));
        QVERIFY(!DlcLookupService::isTrustedSource(QUrl(q("https://store.playstation.com.evil.example/"))));
        QVERIFY(!DlcLookupService::isTrustedSource(QUrl(q("/titles/PPSA/01234"))));

        QCOMPARE(lk::searchApiUrl(market(q("US")), q("Example Quest: Rise/Fall")),
                 q("https://store.playstation.com/store/api/chihiro/00_09_000/tumbler/US/en/999/Example%20Quest%3A%20Rise%2FFall?size=100"));
        QCOMPARE(lk::searchPageUrl(market(q("JP")), kGameName), q("https://store.playstation.com/ja-jp/search/Example%20Quest"));
        QCOMPARE(lk::serialStationUrl(q("cusa-00001")), q("https://serialstation.com/titles/CUSA/00001"));
        QCOMPARE(lk::serialStationUrl(kGameName), q("https://serialstation.com/search/?search=Example%20Quest"));
        QCOMPARE(lk::storeSearchUrl(q("Ö")), q("https://store.playstation.com/en-gb/search/%C3%96"));

        QString url;
        QString cid;
        QVERIFY(lk::tryStoreProduct(q("jp0001-ppsa01234_00-abcdefghijklmnop"), &url, &cid));
        QCOMPARE(cid, q("JP0001-PPSA01234_00-ABCDEFGHIJKLMNOP"));
        QCOMPARE(url, q("https://store.playstation.com/ja-jp/product/JP0001-PPSA01234_00-ABCDEFGHIJKLMNOP"));
        const QList<QPair<QString, QString>> locales = {
            {q("UP"), q("en-us")}, {q("EP"), q("en-gb")}, {q("HP"), q("en-hk")}, {q("KP"), q("ko-kr")}, {q("XX"), q("en-gb")}};
        for (const auto& [prefix, locale] : locales) {
            QVERIFY(lk::tryStoreProduct(prefix + q("0001-PPSA01234_00-ABCDEFGHIJKLMNOP"), &url, &cid));
            QVERIFY2(url.startsWith(q("https://store.playstation.com/") + locale + q("/product/")), qPrintable(url));
        }
        QVERIFY(lk::tryStoreProduct(q("https://store.playstation.com/EN-US/product/up0700-ppsa05678_00-examplequest0000/?smcid=psapp"),
                                        &url, &cid));
        QCOMPARE(url, q("https://store.playstation.com/en-us/product/UP0700-PPSA05678_00-EXAMPLEQUEST0000"));
        QCOMPARE(cid, q("UP0700-PPSA05678_00-EXAMPLEQUEST0000"));
        QVERIFY(!lk::tryStoreProduct(q("https://store.playstation.com/en-us/concept/10001234"), &url, &cid));
        QVERIFY(!lk::tryStoreProduct(q("http://store.playstation.com/en-us/product/UP0700-PPSA05678_00-EXAMPLEQUEST0000"), &url, &cid));
        QVERIFY(!lk::tryStoreProduct(q("https://store.playstation.com:8443/en-us/product/UP0700-PPSA05678_00-EXAMPLEQUEST0000"), &url, &cid));
        QVERIFY(!lk::tryStoreProduct(q("https://store.playstation.com/en-us/product/NOTACID"), &url, &cid));
        QVERIFY(url.isEmpty() && cid.isEmpty());

        QCOMPARE(lk::cacheFileName(q("regions-v6|ppsa01234")),
                 QString::fromLatin1(QCryptographicHash::hash("REGIONS-V6|PPSA01234", QCryptographicHash::Sha256).toHex().toUpper())
                     + q(".json"));

        QCOMPARE(lk::htmlDecode(q("&lt;b&gt; &amp;amp; &#65;&#x42; &copy; &unknown; &#xD800; &#0; & ;")),
                 q("<b> &amp; AB © &unknown; &#xD800; &#0; & ;"));
        QCOMPARE(lk::htmlText(q("<p>Hi <SCRIPT type=x>var a = '<b>';</script><b>there</b>&nbsp;!<style>p{}</style>\n</p>")),
                 q("Hi there !"));

        QVERIFY(lk::isGameName(q("  Example Quest  ")));
        QVERIFY(lk::isGameName(q("F1 25")));
        QVERIFY(lk::isGameName(q("abcdefghijkl")));
        QVERIFY(!lk::isGameName(QString()));
        QVERIFY(!lk::isGameName(QString(241, u'a')));
        QVERIFY(!lk::isGameName(q("https://store.playstation.com/x")));
        QVERIFY(!lk::isGameName(q("HTTP:example")));
        QVERIFY(!lk::isGameName(q("ppsa-01234")));
        QVERIFY(!lk::isGameName(q("ep0700-ppsa01234_00-anything")));
        QVERIFY(!lk::isGameName(q("ABCDEFGHIJKL")));
        QVERIFY(lk::isDlcDisplayLabel(q("Digital Deluxe Edition Bonus")));
        QVERIFY(lk::isDlcDisplayLabel(q(" Season Pass ")));
        QVERIFY(lk::isDlcDisplayLabel(q("Pre-order bonus content")));
        QVERIFY(lk::isDlcDisplayLabel(q("DLC 2")));
        QVERIFY(!lk::isDlcDisplayLabel(q("Bonus Pack")));
        QVERIFY(!lk::isDlcDisplayLabel(q("Example Quest Deluxe Edition")));
        QCOMPARE(lk::knownName(q("ppsa24662")), q("F1 25"));
        QCOMPARE(lk::knownName(q("PPSA08030")), q("FINAL FANTASY XVI"));
        QCOMPARE(lk::knownName(q("PPSA00001")), QString());
    }

    void storeProduct()
    {
        const QString html = fixtureText(q("store_product_dlc_gb.html"));
        const std::vector<DlcCandidate> found = lk::parseStoreProduct(html, kSourceProductUrl, kSourceCid.toLower());
        QCOMPARE(int(found.size()), 1);
        const DlcCandidate& c = found.front();
        QCOMPARE(c.titleId, q("PPSA01234"));
        QCOMPARE(c.contentId, kSourceCid);
        QCOMPARE(c.name, q("Bonus Pack"));
        QCOMPARE(c.edition, q("Add-on"));
        QCOMPARE(c.region, q("Europe (UK)"));
        QCOMPARE(c.marketRegionCode, q("EP"));
        QCOMPARE(c.storeLocale, q("en-gb"));
        QCOMPARE(c.platform, q("PS5"));
        QCOMPARE(c.productKind, q("DLC"));
        QCOMPARE(c.gameName, kGameName);
        QCOMPARE(c.conceptId, q("10001234"));
        QCOMPARE(c.sourceUrl, kSourceProductUrl);
        QCOMPARE(c.version, q("Not reported"));
        QCOMPARE(c.mappingStatus, q("Official Store product SKU observed — exact content ID"));
        QCOMPARE(c.compatibilityStatus, q("Unverified — identifier mapping does not prove DLC compatibility"));

        // Only the expected SKU counts; the page's other product has no cache data.
        QVERIFY(lk::parseStoreProduct(html, kSourceProductUrl, q("EP0700-PPSA01234_00-NOTONTHISPAGE000")).empty());
        const auto other = lk::parseStoreProduct(html, kSourceProductUrl, q("EP0700-PPSA01234_00-OTHERPRODUCT0000"));
        QCOMPARE(int(other.size()), 1);
        QCOMPARE(other.front().productKind, q("Unknown"));
        QVERIFY(other.front().gameName.isEmpty() && other.front().conceptId.isEmpty() && other.front().edition.isEmpty());

        // A locale outside the five markets.
        const auto au = lk::parseStoreProduct(html, q("https://store.playstation.com/en-au/product/x"), kSourceCid);
        QCOMPARE(int(au.size()), 1);
        QCOMPARE(au.front().region, q("Store locale en-au"));
        QCOMPARE(au.front().storeLocale, q("en-au"));
        QVERIFY(au.front().marketRegionCode.isEmpty());

        // PS4 SKU; a nameless product is ignored.
        const QString ps4 = q(R"(<script type="application/ld+json">{"@type":"Product","name":"Old Game","sku":"UP0001-CUSA00001_00-OLDGAME000000000"}</script>
<script type="application/ld+json">{"@type":"Product","name":"","sku":"UP0001-CUSA00001_00-NONAME0000000000"}</script>)");
        const auto old = lk::parseStoreProduct(ps4, q("https://store.playstation.com/en-us/product/x"), q("UP0001-CUSA00001_00-OLDGAME000000000"));
        QCOMPARE(int(old.size()), 1);
        QCOMPARE(old.front().platform, q("PS4"));
        QCOMPARE(old.front().titleId, q("CUSA00001"));
        QCOMPARE(old.front().region, q("Americas (US)"));
        QVERIFY(lk::parseStoreProduct(ps4, q("https://store.playstation.com/en-us/product/x"), q("UP0001-CUSA00001_00-NONAME0000000000")).empty());
    }

    void storeNextCache()
    {
        const QString html = fixtureText(q("store_product_dlc_gb.html"));
        QCOMPARE(lk::parseStoreGameName(html, kSourceCid), kGameName);
        QCOMPARE(lk::parseStoreConceptId(html, kSourceCid), q("10001234"));
        QCOMPARE(lk::parseStoreProductKind(html, kSourceCid), q("DLC"));
        QCOMPARE(lk::parseStoreProductKind(html, q("EP0700-PPSA01234_00-OTHERPRODUCT0000")), q("Unknown"));

        const QString conceptPage = fixtureText(q("store_concept_us.html"));
        QCOMPARE(lk::parseStoreConceptId(conceptPage, q("UP0700-PPSA09999_00-EXAMPLEQUEST2000")), q("99999")); // `.concept.id`
        QCOMPARE(lk::parseStoreGameName(conceptPage, q("UP0700-PPSA09999_00-EXAMPLEQUEST2000")), QString());
        QCOMPARE(lk::parseStoreProductKind(conceptPage, q("UP0700-PPSA05678_00-EXAMPLEQUEST0000")), q("Game"));

        const auto page = [](const QString& cls, const QString& conceptJson) {
            return q(R"(<script type="application/json">{"cache":{"Product:%1":{"storeDisplayClassification":"%2","concept":%3},"Concept:42":{"name":"PPSA01234"}}}</script>)")
                .arg(kSourceCid, cls, conceptJson);
        };
        const QString ref = q(R"({"__ref":"Concept:42"})");
        QCOMPARE(lk::parseStoreProductKind(page(q("trial"), ref), kSourceCid), q("Demo"));
        QCOMPARE(lk::parseStoreProductKind(page(q("DEMO"), ref), kSourceCid), q("Demo"));
        QCOMPARE(lk::parseStoreProductKind(page(q("GAME_BUNDLE"), ref), kSourceCid), q("Game"));
        QCOMPARE(lk::parseStoreProductKind(page(q("PSN_GAME"), ref), kSourceCid), q("Game"));
        QCOMPARE(lk::parseStoreProductKind(page(q("SEASON_PASS"), ref), kSourceCid), q("DLC"));
        QCOMPARE(lk::parseStoreProductKind(page(q("VIRTUAL_CURRENCY"), ref), kSourceCid), q("DLC"));
        QCOMPARE(lk::parseStoreProductKind(page(q("BUNDLE"), ref), kSourceCid), q("Unknown"));
        // An identifier is never a game name; the concept ID still counts.
        QCOMPARE(lk::parseStoreGameName(page(q("ADD_ON"), ref), kSourceCid), QString());
        QCOMPARE(lk::parseStoreConceptId(page(q("ADD_ON"), ref), kSourceCid), q("42"));
        QCOMPARE(lk::parseStoreConceptId(page(q("ADD_ON"), q(R"({"id":"1234567890123"})")), kSourceCid), QString());
        QCOMPARE(lk::parseStoreConceptId(page(q("ADD_ON"), q(R"({"id":"12a"})")), kSourceCid), QString());

        // Concept page SKUs: valid, distinct, @type exactly "Product".
        QCOMPARE(lk::storeProductSkus(conceptPage),
                 QStringList({q("UP0700-PPSA05678_00-EXAMPLEQUEST0000"), q("UP0700-PPSA05678_00-DLCSUFFIX0000001")}));
        const QString conceptUrl = q("https://store.playstation.com/en-us/concept/10001234");
        QCOMPARE(int(lk::parseStoreProduct(conceptPage, conceptUrl, q("UP0700-PPSA05678_00-LOWERCASETYPE000")).size()), 1);
        QCOMPARE(int(lk::parseStoreProduct(conceptPage, conceptUrl, q("UP0700-PPSA05678_00-EXAMPLEQUEST0000")).size()), 1);
    }

    void storeSearch()
    {
        const QByteArray us = fixture(q("search_us.json"));
        const auto plain = lk::parseSearch(us, market(q("US")), kGameName, false);
        QVERIFY(plain.ok());
        QStringList ids;
        for (const DlcCandidate& c : plain.value())
            ids << c.contentId;
        QCOMPARE(ids, QStringList({q("UP0700-PPSA05678_00-EXAMPLEQUEST0000"), q("UP0700-PPSA05678_00-DLCSUFFIX0000001"),
                                   q("UP0700-PPSA05678_00-EXAMPLEQUESTDLX0"), q("UP0700-PPSA05678_00-EXAMPLEQUESTDEMO")}));
        const DlcCandidate& game = plain.value()[0];
        QCOMPARE(game.titleId, q("PPSA05678"));
        QCOMPARE(game.productKind, q("Game"));
        QCOMPARE(game.edition, q("Full Game"));
        QCOMPARE(game.gameName, kGameName);
        QCOMPARE(game.region, q("Americas (US)"));
        QCOMPARE(game.marketRegionCode, q("UP"));
        QCOMPARE(game.storeLocale, q("en-us"));
        QCOMPARE(game.platform, q("PS5"));
        QCOMPARE(game.sourceUrl, q("https://store.playstation.com/en-us/product/UP0700-PPSA05678_00-EXAMPLEQUEST0000"));
        QCOMPARE(game.mappingStatus, q("Official Store US release SKU observed; check edition"));
        QCOMPARE(game.compatibilityStatus, q("Unverified — observed Store identity does not prove DLC compatibility"));
        const DlcCandidate& addOn = plain.value()[1];
        QCOMPARE(addOn.productKind, q("DLC"));
        QCOMPARE(addOn.edition, q("Add-on"));
        QCOMPARE(addOn.gameName, kGameName);
        QCOMPARE(addOn.mappingStatus, q("Official Store US release SKU observed; add-on"));
        const DlcCandidate& deluxe = plain.value()[2];
        QCOMPARE(deluxe.name, q("Example Quest™ Deluxe Edition"));
        QCOMPARE(deluxe.edition, q("Edition not reported"));
        const DlcCandidate& demo = plain.value()[3];
        QCOMPARE(demo.productKind, q("Demo"));
        QCOMPARE(demo.mappingStatus, q("Official Store US release SKU observed; demo/trial"));

        // With a concept to verify against, any name starting with the query is kept.
        const auto conceptHits = lk::parseSearch(us, market(q("US")), kGameName, true);
        QVERIFY(conceptHits.ok());
        ids.clear();
        for (const DlcCandidate& c : conceptHits.value())
            ids << c.contentId.mid(20);
        QCOMPARE(ids, QStringList({q("EXAMPLEQUEST0000"), q("DLCSUFFIX0000001"), q("EXAMPLEQUEST2000"), q("EXAMPLEQUESTDLX0"),
                                   q("EXAMPLEQUESTDEMO"), q("OTHERADDON000000")}));

        // Add-ons are matched on their parent game; without one the query is the game name.
        const QByteArray parentOnly = R"({"links":[{"id":"UP0700-PPSA05678_00-ADDONNAMEONLY000","name":"Bonus Pack","top_category":"add_on","parent_name":"Example Quest"},
{"id":"UP0700-PPSA05678_00-ORPHANADDON00000","name":"Example Quest Bonus","metadata":{"top_category":{"values":["ADD_ON"]}}}]})";
        const auto parents = lk::parseSearch(parentOnly, market(q("US")), kGameName, true);
        QVERIFY(parents.ok());
        QCOMPARE(int(parents.value().size()), 2);
        QCOMPARE(int(lk::parseSearch(parentOnly, market(q("US")), kGameName, false).value().size()), 1);
        QCOMPARE(parents.value()[1].gameName, kGameName);
        QCOMPARE(parents.value()[1].productKind, q("DLC"));

        QVERIFY(!lk::parseSearch("{oops", market(q("US")), kGameName, false).ok());
        QVERIFY(lk::parseSearch("{}", market(q("US")), kGameName, false).value().empty());
        QVERIFY(lk::parseSearch(us, market(q("US")), q("  "), false).value().empty());

        QVERIFY(lk::sameGameOrEdition(kGameName, q("example quest"), false));
        QVERIFY(lk::sameGameOrEdition(q("EXAMPLE QUEST – Game of the Year Edition"), kGameName, false));
        QVERIFY(lk::sameGameOrEdition(q("Example Quest PS4 & PS5"), kGameName, false));
        QVERIFY(lk::sameGameOrEdition(q("Example Quest Collector's Edition"), kGameName, false));
        QVERIFY(lk::sameGameOrEdition(q("Example Quest GOTY"), kGameName, false));
        QVERIFY(lk::sameGameOrEdition(q("Example Quest: Digital Deluxe"), kGameName, false));
        QVERIFY(!lk::sameGameOrEdition(q("Example Quest II"), kGameName, false));
        QVERIFY(!lk::sameGameOrEdition(q("Example Quest Demo"), kGameName, false));
        QVERIFY(lk::sameGameOrEdition(q("Example Quest Demo"), kGameName, true));
        QVERIFY(lk::sameGameOrEdition(q("Example Quest Trial"), kGameName, true));
        QVERIFY(!lk::sameGameOrEdition(q("Example"), kGameName, false));
        QVERIFY(!lk::sameGameOrEdition(kGameName, QString(), false));
        QVERIFY(lk::sameGameOrEdition(q("Pokémon Quest"), q("Pokémon Quest"), false)); // NFC

        QCOMPARE(lk::refinedSearch(q("The Example Quest Saga")), q("Quest Saga"));
        QCOMPARE(lk::refinedSearch(kGameName), QString());
        QCOMPARE(lk::searchCount(us), 10);
        QCOMPARE(lk::searchCount("{}"), 0);
    }

    void serialStation()
    {
        const QString title = fixtureText(q("serial_title.html"));
        const QString url = q("https://serialstation.com/titles/PPSA/01234");
        const auto rows = lk::parseSerialTitle(title, q("ppsa-01234"), url, false);
        QCOMPARE(int(rows.size()), 2);
        QCOMPARE(rows[0].titleId, q("PPSA01234"));
        QCOMPARE(rows[0].name, kGameName);
        QCOMPARE(rows[0].edition, q("Standard"));
        QCOMPARE(rows[0].region, q("Europe, Australia"));
        QCOMPARE(rows[0].platform, q("PS5"));
        QCOMPARE(rows[0].sourceUrl, url);
        QCOMPARE(rows[0].mappingStatus, q("Database title/release mapping — exact title ID"));
        QVERIFY(rows[0].contentId.isEmpty() && rows[0].productKind.isEmpty());
        QCOMPARE(rows[1].name, q("Example Quest & Friends"));
        QCOMPARE(rows[1].region, q("Unknown"));
        QCOMPARE(lk::parseSerialTitle(title, q("PPSA01234"), url, true)[0].mappingStatus,
                 q("Database title/release mapping — related game; verify edition"));
        QVERIFY(lk::parseSerialTitle(title, q("PPSA09999"), url, false).empty());
        QVERIFY(lk::parseSerialTitle(title, q("not a title"), url, false).empty());

        // No release table: one candidate from the heading; validated by the details alone.
        const QString fallback = q("<h1>Old Game</h1><dl><dt>Title ID Type</dt><dd>CUSA</dd><dt>Title ID Number</dt><dd>00001</dd>"
                                   "<dt>Category</dt><dd>Game</dd></dl>");
        const auto one = lk::parseSerialTitle(fallback, q("CUSA00001"), q("https://serialstation.com/titles/CUSA/00001"), false);
        QCOMPARE(int(one.size()), 1);
        QCOMPARE(one[0].name, q("Old Game"));
        QCOMPARE(one[0].edition, q("Not reported"));
        QCOMPARE(one[0].platform, q("PS4"));
        QCOMPARE(one[0].region, q("Unknown"));
        QCOMPARE(one[0].mappingStatus, q("Database title/release mapping — exact title ID; region unknown"));
        QString addOn = fallback;
        addOn.replace(q("<dd>Game</dd>"), q("<dd>Add-on</dd>"));
        QCOMPARE(lk::parseSerialTitle(addOn, q("CUSA00001"), url, false)[0].edition, q("Add-on"));
        QVERIFY(lk::parseSerialTitle(q("<h2>CUSA00001</h2>"), q("CUSA00001"), url, false).empty());

        const auto hits = lk::parseSerialSearch(fixtureText(q("serial_search.html")), q("Name search candidate — ambiguous; verify edition"));
        QStringList got;
        for (const DlcCandidate& c : hits)
            got << c.titleId + u'=' + c.name;
        QCOMPARE(got, QStringList({q("PPSA01234=Example Quest"), q("CUSA00001=Example Quest (PS4)"), q("PPSA02222=PPSA02222"),
                                   q("PPSA03333=Loose & Link")}));
        QCOMPARE(hits[0].sourceUrl, url);
        QCOMPARE(hits[0].edition, q("Not reported"));
        QCOMPARE(hits[0].mappingStatus, q("Name search candidate — ambiguous; verify edition"));
        QCOMPARE(hits[1].platform, q("PS4"));

        QCOMPARE(lk::serialLinks(title, q("/games/")),
                 QStringList({q("https://serialstation.com/games/example-quest"), q("https://serialstation.com/games/example-quest-2/"),
                              q("https://serialstation.com/games/third")}));
    }

    void prosperoHint()
    {
        const QString html = fixtureText(q("prospero_title.html"));
        QString name;
        QString cid;
        QVERIFY(lk::parseTitleIndexHint(html, q("PPSA01234"), kSourceCid, &name, &cid));
        QCOMPARE(name, kGameName);
        QCOMPARE(cid, q("EP0700-PPSA01234_00-EXAMPLEQUEST0000"));
        QVERIFY(lk::parseTitleIndexHint(html, q("PPSA01234"), QString(), &name, &cid)); // no source CID: no publisher check
        QVERIFY(!lk::parseTitleIndexHint(html, q("PPSA01234"), q("UP0700-PPSA01234_00-DLCSUFFIX0000001"), &name, &cid));
        QVERIFY(!lk::parseTitleIndexHint(html, q("PPSA05678"), QString(), &name, &cid));
        QVERIFY(!lk::parseTitleIndexHint(html, q("CUSA01234"), QString(), &name, &cid));
        QString idName = html;
        idName.replace(q("Example <em>Quest</em>"), q("PPSA01234"));
        QVERIFY(!lk::parseTitleIndexHint(idName, q("PPSA01234"), QString(), &name, &cid));
        QString otherCid = html;
        otherCid.replace(q("EP0700-PPSA01234_00-EXAMPLEQUEST0000"), q("EP0700-PPSA05678_00-EXAMPLEQUEST0000"));
        QVERIFY(!lk::parseTitleIndexHint(otherCid, q("PPSA01234"), QString(), &name, &cid));
    }

    void lookupRouting()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        {
            auto web = exampleWeb();
            DlcLookupService svc(tmp.filePath(q("a")), web->fetch());
            const QString enter =
                q("Enter a title ID, content ID, game name or PlayStation Store product URL (up to 240 characters).");
            QCOMPARE(svc.lookup(q("   "), {}).warnings, QStringList{enter});
            QCOMPARE(svc.lookup(QString(241, u'x'), {}).warnings, QStringList{enter});
            const QString https =
                q("Only HTTPS PlayStation Store product URLs are accepted. Enter other source IDs or game names manually.");
            for (const QString& url : {q("http://store.playstation.com/en-gb/product/") + kSourceCid,
                                       q("https://store.playstation.com.evil.example/en-gb/product/") + kSourceCid,
                                       q("https://store.playstation.com:8443/en-gb/product/") + kSourceCid, q("https://example.com/x")}) {
                const DlcLookupResult r = svc.lookup(url, {});
                QCOMPARE(r.warnings, QStringList{https});
                QVERIFY(r.candidates.empty());
            }
            QCOMPARE(web->count(), 0);
            QCOMPARE(svc.resolveRegions(QString(), QString(), QString(241, u'x'), {}).warnings,
                     QStringList{q("Lookup identifiers and game names must be at most 240 characters.")});
        }
        {
            // A title ID goes to the resolver (a PPSA without a game name: the title index first).
            auto web = exampleWeb();
            DlcLookupService svc(tmp.filePath(q("b")), web->fetch());
            const DlcLookupResult r = svc.lookup(q("ppsa-01234"), {});
            QCOMPARE(web->requested().first(), q("https://prosperopatches.com/PPSA01234"));
            QCOMPARE(r.resolvedName, kGameName);
            VERIFY_WARNING(r, q(kAlwaysWarning));
        }
        {
            // A content ID: its own Store product page, then the regions of its game.
            auto web = exampleWeb();
            DlcLookupService svc(tmp.filePath(q("c")), web->fetch());
            const DlcLookupResult r = svc.lookup(kSourceCid.toLower(), {});
            QCOMPARE(web->requested().first(), kSourceProductUrl);
            QCOMPARE(r.sourceLinks.first(), kSourceProductUrl);
            QCOMPARE(r.resolvedName, kGameName);
            QVERIFY(find(r.candidates, kSourceCid));
            QVERIFY(find(r.candidates, q("UP0700-PPSA05678_00-EXAMPLEQUEST0000")));
            QVERIFY(web->requested().contains(q("https://store.playstation.com/en-us/concept/10001234")));
            QCOMPARE(int(QDir(tmp.filePath(q("c"))).entryList({q("*.json")}).size()), 2); // product key + regional key
        }
        {
            // A Store product URL is normalised to locale + upper-case SKU.
            auto web = exampleWeb();
            DlcLookupService svc(tmp.filePath(q("d")), web->fetch());
            const DlcLookupResult r =
                svc.lookup(q("https://store.playstation.com/EN-GB/product/") + kSourceCid.toLower() + q("/?smcid=psapp"), {});
            QCOMPARE(web->requested().first(), kSourceProductUrl);
            QCOMPARE(r.resolvedName, kGameName);
        }
        {
            // A game name: straight to the five market searches.
            auto web = exampleWeb();
            DlcLookupService svc(tmp.filePath(q("e")), web->fetch());
            const DlcLookupResult r = svc.lookup(q("  Example Quest "), {});
            QCOMPARE(r.resolvedName, kGameName);
            QVERIFY(web->requested().contains(lk::searchApiUrl(market(q("US")), kGameName)));
            QVERIFY(!web->requested().contains(q("https://prosperopatches.com/PPSA01234")));
            QVERIFY(find(r.candidates, q("UP0700-PPSA05678_00-EXAMPLEQUESTDEMO"))); // no concept: the name filter decides
        }
        {
            // Anything else: SerialStation, then the cache.
            auto web = exampleWeb();
            DlcLookupService svc(tmp.filePath(q("f")), web->fetch());
            const DlcLookupResult r = svc.lookup(q("ABCDEFGHIJKLMNOP"), {});
            QCOMPARE(web->requested().first(), q("https://serialstation.com/search/?search=ABCDEFGHIJKLMNOP"));
            QCOMPARE(r.warnings.first(),
                     q("SerialStation is a community database with variable data quality. Check edition, platform and game "
                       "version. Every DLC compatibility result is unverified."));
            VERIFY_WARNING(r, q("Name matches and related releases are candidates, not a DLC compatibility decision. Similarly "
                                "named games, demos and editions may have different identifiers."));
            VERIFY_WARNING(r, q("CUSA00001 release details unavailable: HTTP 404 from serialstation.com"));
            QStringList got;
            for (const DlcCandidate& c : r.candidates)
                got << c.titleId + u'/' + c.edition;
            QCOMPARE(got, QStringList({q("CUSA00001/Not reported"), q("PPSA02222/Not reported"), q("PPSA03333/Not reported"),
                                       q("PPSA01234/Standard"), q("PPSA01234/Deluxe")}));
            QCOMPARE(r.candidates.back().mappingStatus, q("Database title/release mapping — related game; verify edition"));
            QCOMPARE(r.sourceLinks, QStringList({q("https://serialstation.com/search/?search=ABCDEFGHIJKLMNOP"),
                                                 q("https://store.playstation.com/en-gb/search/ABCDEFGHIJKLMNOP")}));

            auto offline = std::make_shared<FakeWeb>();
            DlcLookupService again(tmp.filePath(q("f")), offline->fetch());
            const DlcLookupResult cached = again.lookup(q("ABCDEFGHIJKLMNOP"), {});
            QVERIFY(cached.fromCache);
            QCOMPARE(offline->count(), 0);
            QCOMPARE(cached.warnings.last(),
                     q("Cached database observations from ") + universal(cached.storedAt) + q(". DLC compatibility remains unverified."));
            QCOMPARE(int(cached.candidates.size()), 5);
        }
        {
            // A truncated content ID: SerialStation by its title ID; nothing found there either.
            auto web = std::make_shared<FakeWeb>();
            DlcLookupService svc(tmp.filePath(q("g")), web->fetch());
            const DlcLookupResult r = svc.lookup(q("UP0700-PPSA01234_0"), {});
            QCOMPARE(web->requested(), QStringList{q("https://serialstation.com/titles/PPSA/01234")});
            VERIFY_WARNING(r, q("SerialStation unavailable: HTTP 404 from serialstation.com. Open its source link in a browser or use manual entry."));
            QCOMPARE(r.warnings.last(), q("No mapping was confirmed. Open the source links or enter target identifiers manually; "
                                          "do not infer a publisher prefix from a region."));
        }
    }

    void resolveRegionsAcrossMarkets()
    {
        QTemporaryDir tmp;
        auto web = exampleWeb();
        DlcLookupService svc(tmp.path(), web->fetch());
        const DlcLookupResult r = svc.resolveRegions(q("ppsa01234"), kSourceCid.toLower(), q("Bonus Pack"), {});
        QCOMPARE(r.resolvedName, kGameName);
        QVERIFY(!r.fromCache && !r.isStale);
        QCOMPARE(r.warnings.first(), q(kAlwaysWarning));
        for (const char* w : {
                 "Americas (US) exact SKU relationship lookup unavailable: HTTP 404 from store.playstation.com",
                 "Americas (US): search hits without the source game's confirmed Store Concept were excluded; narrow the query or "
                 "use manual entry if an edition is missing.",
                 "Europe (UK) game relationship lookup unavailable: HTTP 404 from store.playstation.com",
                 "Japan Store lookup unavailable: HTTP 403 from store.playstation.com (browser challenge or access restriction)",
                 "Japan game relationship lookup unavailable: HTTP 404 from store.playstation.com",
                 "Asia (Hong Kong) Store lookup unavailable: The lookup service requires a browser challenge",
                 "Korea Store lookup unavailable: Request timed out after 6 seconds",
             })
            VERIFY_WARNING(r, q(w));
        QVERIFY(web->timeouts().contains(6000));
        QVERIFY(r.sourceLinks.contains(kSourceProductUrl));
        QVERIFY(r.sourceLinks.contains(q("https://store.playstation.com/en-us/concept/10001234")));
        QVERIFY(r.sourceLinks.contains(lk::searchPageUrl(market(q("KR")), kGameName)));

        const DlcCandidate* source = find(r.candidates, kSourceCid);
        QVERIFY(source);
        QCOMPARE(source->productKind, q("DLC"));
        const DlcCandidate* usGame = find(r.candidates, q("UP0700-PPSA05678_00-EXAMPLEQUEST0000"));
        QVERIFY(usGame);
        QCOMPARE(usGame->productKind, q("Game"));
        QCOMPARE(usGame->conceptId, q("10001234"));
        QCOMPARE(usGame->gameName, kGameName);
        QCOMPARE(usGame->mappingStatus, q("Official Store US release SKU observed — same game Concept 10001234"));
        QVERIFY(find(r.candidates, q("UP0700-PPSA05678_00-EXAMPLEQUESTDLX0"))); // verified through its product page
        QVERIFY(!find(r.candidates, q("UP0700-PPSA09999_00-EXAMPLEQUEST2000"))); // another concept
        QVERIFY(!find(r.candidates, q("UP0700-PPSA05678_00-EXAMPLEQUESTDEMO"))); // unverifiable demo
        QVERIFY(!find(r.candidates, q("UP0700-PPSA07777_00-OTHERADDON000000"))); // its page failed
        const DlcCandidate* gbDlc = find(r.candidates, q("EP0700-PPSA01234_01-DLCSUFFIX0000001"));
        QVERIFY(gbDlc);
        QCOMPARE(gbDlc->productKind, q("DLC"));
        QCOMPARE(gbDlc->conceptId, q("10001234"));
        QCOMPARE(int(r.candidates.size()), 8);

        const QString key = q("regions-v6|PPSA01234|") + kSourceCid + q("|Bonus Pack|");
        QVERIFY(QFile::exists(QDir(tmp.path()).filePath(lk::cacheFileName(key))));
        QCOMPARE(int(QDir(tmp.path()).entryList({q("*.json")}).size()), 1);

        // Choosing targets from these observations.
        const auto usDlc = dlcForRegion(r.candidates, q("UP"), kSourceCid);
        QCOMPARE(int(usDlc.size()), 1); // concept page and search hit merge
        QCOMPARE(usDlc[0].contentId, q("UP0700-PPSA05678_00-DLCSUFFIX0000001"));
        QVERIFY(isExactDlcSku(usDlc[0], kSourceCid));
        QVERIFY(hasOneIdentity(usDlc, kSourceCid));
        const auto usGames = gameMappings(r.candidates, q("UP"), kSourceCid);
        QCOMPARE(int(usGames.size()), 2); // the game and its Deluxe Edition
        QVERIFY(hasOneIdentity(usGames, kSourceCid));
        const auto proposal = propose(usGames[0], kSourceCid, q("Bonus Pack"));
        QVERIFY(proposal.ok());
        QCOMPARE(proposal->titleId, q("PPSA05678"));
        QCOMPARE(proposal->contentId, q("UP0700-PPSA05678_00-DLCSUFFIX0000001"));
        QCOMPARE(proposal->title, q("Bonus Pack"));
        const auto gbDlcs = dlcForRegion(r.candidates, q("EP"), kSourceCid);
        QCOMPARE(int(gbDlcs.size()), 2);
        QVERIFY(!hasOneIdentity(gbDlcs, kSourceCid)); // two exact SKUs with different _NN
        QCOMPARE(propose(*gbDlc, kSourceCid, q("Bonus Pack"))->contentId, q("EP0700-PPSA01234_01-DLCSUFFIX0000001"));
        QVERIFY(dlcForRegion(r.candidates, q("JP"), kSourceCid).empty());
    }

    void resolveViaTitleIndex()
    {
        QTemporaryDir tmp;
        {
            auto web = exampleWeb();
            web->remove(kSourceProductUrl);
            DlcLookupService svc(tmp.filePath(q("a")), web->fetch());
            const DlcLookupResult r = svc.resolveRegions(q("PPSA01234"), kSourceCid, q("Season Pass"), {});
            VERIFY_WARNING(r, q("Source DLC is absent or unavailable in the Store: HTTP 404 from store.playstation.com"));
            VERIFY_WARNING(r, q("Source base-release lookup unavailable: HTTP 404 from store.playstation.com"));
            VERIFY_WARNING(r, q("Game name discovered in ProsperoPatches and verified against an official GB Store source release "
                                "with the exact source PPSA/publisher. All regional mappings still use independently observed "
                                "Store SKUs."));
            QCOMPARE(r.resolvedName, kGameName);
            QVERIFY(web->requested().contains(q("https://store.playstation.com/en-gb/product/EP0700-PPSA01234_00-0000000000000000")));
            // The concept came from the verified UK source release's product page.
            QVERIFY(web->requested().contains(q("https://store.playstation.com/en-gb/product/EP0700-PPSA01234_00-EXAMPLEQUEST0000")));
            QVERIFY(web->requested().contains(q("https://store.playstation.com/en-us/concept/10001234")));
            QVERIFY(r.sourceLinks.contains(q("https://prosperopatches.com/PPSA01234")));
            QVERIFY(r.sourceLinks.contains(lk::searchPageUrl(market(q("GB")), kGameName)));
            QVERIFY(find(r.candidates, q("UP0700-PPSA05678_00-EXAMPLEQUEST0000")));
        }
        {
            // Another publisher: the hint is refused; a DLC label is no game name; SerialStation names the game.
            auto web = exampleWeb();
            DlcLookupService svc(tmp.filePath(q("b")), web->fetch());
            const DlcLookupResult r = svc.resolveRegions(q("PPSA01234"), q("UP9999-PPSA01234_00-DLCSUFFIX0000001"), q("Season Pass"), {});
            VERIFY_WARNING(r, q("Title-index discovery did not match the exact source PPSA/publisher; its name was not used."));
            VERIFY_WARNING(r, q("Related release lookup incomplete: HTTP 404 from serialstation.com"));
            QVERIFY(web->requested().contains(q("https://serialstation.com/titles/PPSA/01234")));
            QCOMPARE(r.resolvedName, kGameName);
            QVERIFY(find(r.candidates, q("UP0700-PPSA05678_00-EXAMPLEQUESTDEMO"))); // no concept: name filter only
        }
        {
            // The title index names a game the source market does not list.
            auto web = exampleWeb();
            web->remove(kSourceProductUrl);
            web->body(lk::searchApiUrl(market(q("GB")), kGameName),
                      R"({"links":[{"id":"EP0700-PPSA01234_01-DLCSUFFIX0000001","name":"Example Quest - Bonus Pack","top_category":"add_on","parent_name":"Example Quest"}]})");
            DlcLookupService svc(tmp.filePath(q("c")), web->fetch());
            const DlcLookupResult r = svc.resolveRegions(q("PPSA01234"), kSourceCid, q("Season Pass"), {});
            VERIFY_WARNING(r, q("Title-index name is an unverified discovery hint: no official source Store release matched the "
                                "source PPSA and publisher. Enter the game name manually."));
        }
        {
            auto web = exampleWeb();
            web->remove(kSourceProductUrl);
            web->remove(lk::searchApiUrl(market(q("GB")), kGameName));
            DlcLookupService svc(tmp.filePath(q("d")), web->fetch());
            const DlcLookupResult r = svc.resolveRegions(q("PPSA01234"), kSourceCid, q("Season Pass"), {});
            VERIFY_WARNING(r, q("Exact source-title discovery unavailable: HTTP 404 from store.playstation.com. Manual game-name "
                                "lookup remains available."));
        }
        {
            // No source of a name at all.
            auto web = std::make_shared<FakeWeb>();
            DlcLookupService svc(tmp.filePath(q("e")), web->fetch());
            const DlcLookupResult r = svc.resolveRegions(q("CUSA00001"), QString(), QString(), {});
            VERIFY_WARNING(r, q("The source title could not be resolved to a game name. Enter the game name above and select Find "
                                "regional releases; no region identifiers will be guessed."));
            QCOMPARE(r.warnings.last(), q(kNoStoreRelease));
            QVERIFY(r.resolvedName.isEmpty());
        }
    }

    void resolveViaBaseRelease()
    {
        QTemporaryDir tmp;
        auto web = exampleWeb();
        web->remove(kSourceProductUrl);
        const QString baseUrl = q("https://store.playstation.com/en-gb/product/EP0700-PPSA01234_00-0000000000000000");
        web->body(baseUrl, fixture(q("store_base_gb.html")));
        DlcLookupService svc(tmp.path(), web->fetch());
        const DlcLookupResult r = svc.resolveRegions(q("PPSA01234"), kSourceCid, q("Bonus Pack"), {});
        VERIFY_WARNING(r, q("Source game identified from an exact official Store base release observation; the imported DLC "
                            "identifiers remain unchanged until a target is applied."));
        QCOMPARE(r.resolvedName, kGameName);
        QVERIFY(!web->requested().contains(q("https://prosperopatches.com/PPSA01234")));
        QVERIFY(web->requested().contains(q("https://store.playstation.com/en-us/concept/10001234")));
        QVERIFY(find(r.candidates, q("EP0700-PPSA01234_00-0000000000000000")));
        QVERIFY(r.sourceLinks.contains(baseUrl));
    }

    void resultLimitAndRefinedSearch()
    {
        QTemporaryDir tmp;
        auto web = std::make_shared<FakeWeb>();
        const QString name = q("The Example Quest Saga");
        QJsonArray links;
        for (int i = 0; i < 100; ++i)
            links.append(QJsonObject{{q("id"), q("UP0700-PPSA05678_00-FILLER%1").arg(i, 10, 10, QLatin1Char('0'))},
                                     {q("name"), q("Unrelated %1").arg(i)}});
        web->body(lk::searchApiUrl(market(q("US")), name), QJsonDocument(QJsonObject{{q("links"), links}}).toJson());
        web->body(lk::searchApiUrl(market(q("US")), q("Quest Saga")),
                  R"({"links":[{"id":"UP0700-PPSA05678_00-QUESTSAGA0000000","name":"The Example Quest Saga","top_category":"downloadable_game"}]})");
        DlcLookupService svc(tmp.path(), web->fetch());
        const DlcLookupResult r = svc.lookup(name, {});
        VERIFY_WARNING(r, q("Americas (US) search reached its result limit; narrow the game name if the expected edition is missing."));
        VERIFY_WARNING(r, q("Europe (UK) Store lookup unavailable: HTTP 404 from store.playstation.com"));
        QVERIFY(web->requested().contains(lk::searchApiUrl(market(q("US")), q("Quest Saga"))));
        QCOMPARE(int(r.candidates.size()), 1);
        QCOMPARE(r.candidates[0].contentId, q("UP0700-PPSA05678_00-QUESTSAGA0000000"));
    }

    void fetchFailures()
    {
        QTemporaryDir tmp;
        {
            auto web = std::make_shared<FakeWeb>();
            const QString search = q("https://serialstation.com/search/?search=ABCDEFGHIJKLMNOP");
            web->body(search, QByteArray(2 * 1024 * 1024 + 1, ' '));
            DlcLookupService svc(tmp.filePath(q("a")), web->fetch());
            DlcLookupResult r = svc.lookup(q("ABCDEFGHIJKLMNOP"), {});
            VERIFY_WARNING(r, q("SerialStation unavailable: Lookup response exceeds the 2 MiB limit. Open its source link in a "
                                "browser or use manual entry."));
            web->body(search, QByteArray(2 * 1024 * 1024, ' ')); // exactly the limit is accepted
            r = svc.lookup(q("ABCDEFGHIJKLMNOP"), {});
            QVERIFY(!anyWarningStartsWith(r, q("SerialStation unavailable")));
            web->body(search, fixture(q("challenge.html")));
            r = svc.lookup(q("ABCDEFGHIJKLMNOP"), {});
            VERIFY_WARNING(r, q("SerialStation unavailable: The lookup service requires a browser challenge. Open its source link in "
                                "a browser or use manual entry."));
        }

        // The overall budget: every request is cut by the remaining budget and runs out.
        lk::setTimeoutsForTests(6000, 3000);
        {
            auto web = exampleWeb();
            web->setAllTimeOut(true);
            DlcLookupService svc(tmp.filePath(q("b")), web->fetch());
            const DlcLookupResult r = svc.resolveRegions(q("PPSA01234"), kSourceCid, q("Bonus Pack"), {});
            VERIFY_WARNING(r, q("Regional lookup reached its time limit. Local editing remains available."));
            QCOMPARE(r.warnings.last(), q(kNoStoreRelease));
            QVERIFY(r.candidates.empty());
            QVERIFY(!web->timeouts().isEmpty());
            for (int t : web->timeouts())
                QVERIFY(t <= 3000);
            const DlcLookupResult free = svc.lookup(q("ABCDEFGHIJKLMNOP"), {});
            VERIFY_WARNING(free, q("Online lookup reached its time limit. Local editing and building remain available."));
            const DlcLookupResult product = svc.lookup(kSourceProductUrl, {});
            VERIFY_WARNING(product, q("Store product/regional lookup reached its total time limit. Local editing remains available."));
        }
        {
            // One market running into the budget keeps the other markets' results.
            auto web = exampleWeb();
            web->fail(lk::searchApiUrl(market(q("US")), kGameName), ErrorCode::Timeout, q("timed out"));
            DlcLookupService svc(tmp.filePath(q("c")), web->fetch());
            const DlcLookupResult r = svc.lookup(kGameName, {});
            VERIFY_WARNING(r, q("Americas (US) Store lookup reached the total time limit; completed market results remain available."));
            QVERIFY(find(r.candidates, q("EP0700-PPSA01234_00-EXAMPLEQUEST0000")));
        }
        lk::setTimeoutsForTests(6000, 25000);
        {
            // A supplied product page that does not publish the SKU.
            auto web = exampleWeb();
            const QString url = q("https://store.playstation.com/en-gb/product/EP0700-PPSA01234_00-NOTONTHISPAGE000");
            web->body(url, fixture(q("store_product_dlc_gb.html")));
            DlcLookupService svc(tmp.filePath(q("d")), web->fetch());
            DlcLookupResult r = svc.lookup(url, {});
            VERIFY_WARNING(r, q("The supplied Store page did not publish the requested product SKU. Related targets require "
                                "independent Store observations."));
            web->remove(url);
            r = svc.lookup(q("https://store.playstation.com/en-gb/product/EP0700-PPSA01234_00-NOTONTHISPAGE001"), {});
            VERIFY_WARNING(r, q("Supplied Store product unavailable: HTTP 404 from store.playstation.com"));
        }
    }

    void cancellation()
    {
        QTemporaryDir tmp;
        {
            CancellationSource source;
            DlcLookupService svc(tmp.path(), [&](const QUrl&, int, const CancellationToken&) -> Result<QByteArray> {
                source.cancel();
                return Error::cancelled();
            });
            QCOMPARE(svc.resolveRegions(q("PPSA01234"), kSourceCid, q("Bonus Pack"), source.token()).warnings,
                     QStringList{q("Cancelled.")});
        }
        {
            // Cancelled while the market searches run.
            CancellationSource source;
            auto web = exampleWeb();
            const DlcLookupService::Fetch inner = web->fetch();
            DlcLookupService svc(tmp.path(), [&](const QUrl& url, int ms, const CancellationToken& token) -> Result<QByteArray> {
                if (url.path().contains(q("/tumbler/")))
                    source.cancel();
                return inner(url, ms, token);
            });
            QCOMPARE(svc.lookup(kGameName, source.token()).warnings, QStringList{q("Cancelled.")});
        }
        {
            CancellationSource source;
            source.cancel();
            auto web = exampleWeb();
            DlcLookupService svc(tmp.path(), web->fetch());
            QCOMPARE(svc.lookup(kGameName, source.token()).warnings, QStringList{q("Cancelled.")});
            QCOMPARE(svc.lookup(q("ABCDEFGHIJKLMNOP"), source.token()).warnings, QStringList{q("Cancelled.")});
            QCOMPARE(web->count(), 0);
        }
        QVERIFY(QDir(tmp.path()).entryList({q("*.json")}).isEmpty());
    }

    void cacheReadWrite()
    {
        QTemporaryDir tmp;
        const QString key = q("regions-v6|PPSA01234|") + kSourceCid + q("|Bonus Pack|");
        const QString file = QDir(tmp.path()).filePath(lk::cacheFileName(key));
        auto web = exampleWeb();
        DlcLookupService svc(tmp.path(), web->fetch());
        const DlcLookupResult first = svc.resolveRegions(q("PPSA01234"), kSourceCid, q("Bonus Pack"), {});
        QVERIFY(!first.candidates.empty());
        QVERIFY(QFile::exists(file));

        const QJsonObject stored = readJson(file);
        QCOMPARE(stored.value(q("Schema")).toInt(), 1);
        QVERIFY(QDateTime::fromString(stored.value(q("StoredAt")).toString(), Qt::ISODateWithMs).isValid());
        const QJsonObject storedResult = stored.value(q("Result")).toObject();
        QCOMPARE(storedResult.value(q("ResolvedName")).toString(), kGameName);
        QCOMPARE(int(storedResult.value(q("Candidates")).toArray().size()), int(first.candidates.size()));
        const QJsonObject firstCandidate = storedResult.value(q("Candidates")).toArray().first().toObject();
        for (const char* field : {"TitleId", "ContentId", "Name", "Edition", "Region", "Platform", "Version", "MarketRegionCode",
                                  "StoreLocale", "GameName", "ProductKind", "ConceptId", "SourceUrl", "MappingStatus",
                                  "CompatibilityStatus"})
            QVERIFY2(firstCandidate.contains(q(field)), field);
        QVERIFY(storedResult.value(q("Warnings")).isArray() && storedResult.value(q("SourceLinks")).isArray());
        QVERIFY(storedResult.contains(q("FromCache")) && storedResult.contains(q("IsStale")));

        // Fresh: answered from the cache (same key after trimming / upper-casing), no request.
        auto offline = std::make_shared<FakeWeb>();
        DlcLookupService cached(tmp.path(), offline->fetch());
        const auto again = [&] { return cached.resolveRegions(q("ppsa01234 "), q(" ") + kSourceCid.toLower(), q("Bonus Pack "), {}); };
        const DlcLookupResult fresh = again();
        QVERIFY(fresh.fromCache);
        QVERIFY(!fresh.isStale);
        QCOMPARE(offline->count(), 0);
        QCOMPARE(fresh.warnings.last(),
                 q("Cached Store observations from ") + universal(fresh.storedAt) + q(". DLC compatibility remains unverified."));
        QVERIFY(QRegularExpression(q("^\\d{4}-\\d{2}-\\d{2} \\d{2}:\\d{2}:\\d{2}Z$")).match(universal(fresh.storedAt)).hasMatch());
        QCOMPARE(int(fresh.candidates.size()), int(first.candidates.size()));
        QCOMPARE(fresh.resolvedName, kGameName);

        // Reading resets the compatibility status and drops untrusted source links.
        QJsonObject edited = stored;
        QJsonObject result = storedResult;
        QJsonArray candidates = result.value(q("Candidates")).toArray();
        QJsonObject c0 = candidates.first().toObject();
        c0.insert(q("CompatibilityStatus"), q("Compatible"));
        candidates.replace(0, c0);
        result.insert(q("Candidates"), candidates);
        QJsonArray links = result.value(q("SourceLinks")).toArray();
        links.append(q("https://evil.example/x"));
        result.insert(q("SourceLinks"), links);
        edited.insert(q("Result"), result);
        QVERIFY(writeJson(file, edited));
        const DlcLookupResult reread = again();
        QVERIFY(reread.fromCache);
        QCOMPARE(reread.candidates.front().compatibilityStatus, DlcCandidate().compatibilityStatus);
        QVERIFY(!reread.sourceLinks.contains(q("https://evil.example/x")));
        QVERIFY(!reread.sourceLinks.isEmpty());

        // Older than 7 days: looked up again; nothing observed, so the old entry comes back as stale.
        QJsonObject old = stored;
        old.insert(q("StoredAt"), QDateTime::currentDateTimeUtc().addDays(-8).toString(Qt::ISODateWithMs));
        QVERIFY(writeJson(file, old));
        const DlcLookupResult stale = again();
        QVERIFY(stale.fromCache && stale.isStale);
        QVERIFY(offline->count() > 0);
        QCOMPARE(stale.warnings.last(), q("Using stale Store observations from ") + universal(stale.storedAt)
                                            + q("; recheck the source before applying."));
        // (The stale file itself still names the game - FindCachedGame - so the markets were searched again.)
        VERIFY_WARNING(stale, q("Americas (US) Store lookup unavailable: HTTP 404 from store.playstation.com"));
        QCOMPARE(int(stale.candidates.size()), int(first.candidates.size()));
        QVERIFY(stale.storedAt < QDateTime::currentDateTimeUtc().addDays(-7));

        // Entries that fail validation are ignored (not even used as stale observations).
        const auto withFirstCandidate = [&](const char* field, const QString& value) {
            QJsonObject root = old;
            QJsonObject res = root.value(q("Result")).toObject();
            QJsonArray list = res.value(q("Candidates")).toArray();
            QJsonObject c = list.first().toObject();
            c.insert(q(field), value);
            list.replace(0, c);
            res.insert(q("Candidates"), list);
            root.insert(q("Result"), res);
            return root;
        };
        QJsonObject future = old;
        future.insert(q("StoredAt"), QDateTime::currentDateTimeUtc().addSecs(3600).toString(Qt::ISODateWithMs));
        QJsonObject schema = old;
        schema.insert(q("Schema"), 2);
        QJsonObject noResult = old;
        noResult.remove(q("Result"));
        const QList<QPair<const char*, QJsonObject>> invalid = {
            {"future", future},
            {"schema", schema},
            {"no result", noResult},
            {"untrusted source", withFirstCandidate("SourceUrl", q("https://evil.example/product"))},
            {"cid of another title", withFirstCandidate("ContentId", q("EP0700-PPSA99999_00-DLCSUFFIX0000001"))},
            {"bad title", withFirstCandidate("TitleId", q("PPSA1234"))},
        };
        for (const auto& [what, json] : invalid) {
            QVERIFY(writeJson(file, json));
            const DlcLookupResult r = again();
            QVERIFY2(!r.fromCache, what);
            QVERIFY2(r.candidates.empty(), what);
            QCOMPARE(r.warnings.last(), q(kNoStoreRelease));
        }
        // Too many candidates, and a file over 2 MiB.
        {
            QJsonObject root = old;
            QJsonObject res = root.value(q("Result")).toObject();
            QJsonArray many;
            const QJsonObject one = res.value(q("Candidates")).toArray().first().toObject();
            for (int i = 0; i < 101; ++i)
                many.append(one);
            res.insert(q("Candidates"), many);
            root.insert(q("Result"), res);
            QVERIFY(writeJson(file, root));
            QVERIFY(!again().fromCache);
            QFile f(file);
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write(QJsonDocument(old).toJson());
            f.write(QByteArray(2 * 1024 * 1024, ' '));
            f.close();
            QVERIFY(!again().fromCache);
        }
    }

    void cachePruneAndWriteFailure()
    {
        QTemporaryDir tmp;
        const QDateTime old = QDateTime::currentDateTimeUtc().addDays(-30);
        const auto name = [](int i) { return q("%1.json").arg(QString::number(i, 16).rightJustified(64, u'0').toUpper()); };
        for (int i = 0; i < 130; ++i) {
            QFile f(QDir(tmp.path()).filePath(name(i)));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("{}");
            QVERIFY(f.flush()); // a write after the time was set would make it "now" on some systems
            QVERIFY(f.setFileTime(old.addSecs(i * 60), QFileDevice::FileModificationTime));
        }
        QVERIFY(fwtest::writeTextFile(QDir(tmp.path()).filePath(q("notes.json")), "{}"));
        QVERIFY(fwtest::writeTextFile(QDir(tmp.path()).filePath(q("readme.txt")), "x"));

        auto web = exampleWeb();
        DlcLookupService svc(tmp.path(), web->fetch());
        QVERIFY(!svc.lookup(kGameName, {}).candidates.empty());
        QStringList cacheFiles;
        for (const QString& f : QDir(tmp.path()).entryList({q("*.json")}, QDir::Files))
            if (QRegularExpression(q("^[A-F0-9]{64}\\.json$")).match(f).hasMatch())
                cacheFiles << f;
        QCOMPARE(int(cacheFiles.size()), 128);
        QVERIFY(cacheFiles.contains(lk::cacheFileName(q("regions-v6|||Example Quest|"))));
        for (int i = 0; i < 3; ++i)
            QVERIFY(!cacheFiles.contains(name(i)));
        QVERIFY(cacheFiles.contains(name(3)));
        QVERIFY(QFile::exists(QDir(tmp.path()).filePath(q("notes.json"))));
        QVERIFY(QFile::exists(QDir(tmp.path()).filePath(q("readme.txt"))));

        // The cache folder is a file: results still arrive, with a warning.
        const QString notADir = QDir(tmp.path()).filePath(q("readme.txt"));
        DlcLookupService broken(notADir, exampleWeb()->fetch());
        const DlcLookupResult r = broken.lookup(kGameName, {});
        QVERIFY(!r.candidates.empty());
        QVERIFY2(anyWarningStartsWith(r, q("Lookup cache could not be saved: ")), qPrintable(r.warnings.join(u'\n')));

        // No cache folder at all: nothing is read or written.
        DlcLookupService uncached(QString(), exampleWeb()->fetch());
        QVERIFY(!uncached.lookup(kGameName, {}).candidates.empty());
        QVERIFY(!uncached.lookup(kGameName, {}).fromCache);
    }

    void cachedGameName()
    {
        QTemporaryDir tmp;
        {
            auto web = exampleWeb();
            DlcLookupService svc(tmp.path(), web->fetch());
            QVERIFY(!svc.resolveRegions(q("PPSA01234"), kSourceCid, q("Bonus Pack"), {}).candidates.empty());
        }
        // A later lookup of the US title finds the game (and its concept) among the cached Store observations.
        auto web = exampleWeb();
        DlcLookupService svc(tmp.path(), web->fetch());
        const DlcLookupResult r = svc.resolveRegions(q("PPSA05678"), QString(), QString(), {});
        QCOMPARE(r.resolvedName, kGameName);
        QVERIFY(!web->requested().contains(q("https://prosperopatches.com/PPSA05678")));
        QVERIFY(web->requested().contains(q("https://store.playstation.com/en-us/concept/10001234")));

        // Hard-coded names come first.
        auto empty = std::make_shared<FakeWeb>();
        DlcLookupService known(tmp.filePath(q("known")), empty->fetch());
        const DlcLookupResult f1 = known.resolveRegions(q("PPSA24662"), QString(), QString(), {});
        QCOMPARE(f1.resolvedName, q("F1 25"));
        QVERIFY(empty->requested().contains(lk::searchApiUrl(market(q("US")), q("F1 25"))));
        QVERIFY(!empty->requested().contains(q("https://prosperopatches.com/PPSA24662")));
        QVERIFY(empty->requested().contains(q("https://serialstation.com/titles/PPSA/24662"))); // nothing found: SerialStation fallback
    }

    void selection()
    {
        const QString src = kSourceCid;
        const DlcCandidate game = cand(q("PPSA05678"), q("UP0700-PPSA05678_00-EXAMPLEQUEST0000"), q("Game"), q("UP"));
        QVERIFY(canMap(game, src));
        QVERIFY(!canMap(game, q("not a content id")));
        QVERIFY(!canMap(game, q("EP0700-CUSA01234_00-DLCSUFFIX0000001")));
        DlcCandidate demo = game;
        demo.productKind = q("Demo");
        QVERIFY(!canMap(demo, src));
        DlcCandidate bundle = game;
        bundle.productKind = q("Bundle");
        QVERIFY(canMap(bundle, src));
        DlcCandidate mismatch = game;
        mismatch.titleId = q("PPSA05679");
        QVERIFY(!canMap(mismatch, src));
        DlcCandidate noCid = game;
        noCid.contentId.clear();
        QVERIFY(!canMap(noCid, src));

        // Retargeting keeps the source's _NN and suffix; the publisher prefix and title come from the release.
        auto p = propose(game, q("EP0700-PPSA01234_03-DLCSUFFIX0000001"), q("Bonus Pack"));
        QVERIFY(p.ok());
        QCOMPARE(p->titleId, q("PPSA05678"));
        QCOMPARE(p->contentId, q("UP0700-PPSA05678_03-DLCSUFFIX0000001"));
        QCOMPARE(p->title, q("Bonus Pack"));
        // An observed DLC SKU with the same suffix is used as is, with its own _NN.
        const DlcCandidate sku = cand(q("PPSA05678"), q("UP0700-PPSA05678_01-DLCSUFFIX0000001"), q("DLC"), q("UP"));
        QVERIFY(isExactDlcSku(sku, src));
        QVERIFY(!isExactDlcSku(game, src));
        QCOMPARE(propose(sku, src, q("x"))->contentId, q("UP0700-PPSA05678_01-DLCSUFFIX0000001"));
        const DlcCandidate otherDlc = cand(q("PPSA05678"), q("UP0700-PPSA05678_02-OTHERSUFFIX00000"), q("DLC"), q("UP"));
        QVERIFY(!isExactDlcSku(otherDlc, src));
        QCOMPARE(propose(otherDlc, src, q("x"))->contentId, q("UP0700-PPSA05678_00-DLCSUFFIX0000001"));
        const QString noMapping =
            q("This release has no observed content ID for the same platform. Choose another release or use manual identifiers.");
        QCOMPARE(errorOf(propose(demo, src, q("x"))).message, noMapping);
        QCOMPARE(errorOf(propose(cand(q("CUSA00001"), q("UP0001-CUSA00001_00-OLDGAME000000000"), q("Game"), q("UP")), src, q("x"))).message,
                 noMapping);

        const DlcCandidate gameNoMarket = cand(q("PPSA05678"), q("UP0700-PPSA05678_00-EXAMPLEQUEST0000"), q("Game"), QString());
        const DlcCandidate ep = cand(q("PPSA01234"), q("EP0700-PPSA01234_00-EXAMPLEQUEST0000"), q("Game"), q("EP"));
        const DlcCandidate jpInUs = cand(q("PPSA01234"), q("JP0700-PPSA01234_00-EXAMPLEQUEST0000"), q("Game"), q("UP"));
        const DlcCandidate ps4 = cand(q("CUSA00001"), q("UP0001-CUSA00001_00-OLDGAME000000000"), q("Game"), q("UP"));
        const DlcCandidate noTitle = cand(QString(), QString(), q("Game"), q("UP"));
        const DlcCandidate lowerCid = cand(q("PPSA05678"), q("up0700-ppsa05678_00-dlcsuffix0000001"), q("DLC"), q("UP"));
        const DlcCandidate upperCid = cand(q("PPSA05678"), q("UP0700-PPSA05678_00-DLCSUFFIX0000001"), q("DLC"), q("UP"));
        const std::vector<DlcCandidate> list = {game, gameNoMarket, ep, jpInUs, ps4, demo, noTitle, lowerCid, upperCid, game};
        QStringList ids;
        for (const DlcCandidate& c : forRegion(list, q("UP"), src))
            ids << c.contentId + u'/' + c.marketRegionCode;
        QCOMPARE(ids, QStringList({q("UP0700-PPSA05678_00-EXAMPLEQUEST0000/UP"), q("UP0700-PPSA05678_00-EXAMPLEQUEST0000/"),
                                   q("JP0700-PPSA01234_00-EXAMPLEQUEST0000/UP"), q("UP0700-PPSA05678_00-DLCSUFFIX0000001/UP")}));
        QCOMPARE(int(forRegion(list, q("EP"), src).size()), 1);
        QCOMPARE(int(forRegion(list, QString(), src).size()), 5);
        QCOMPARE(int(forRegion(list, QString(), QString()).size()), 6); // no source: no platform filter
        QCOMPARE(int(dlcForRegion(list, q("UP"), src).size()), 1);
        QCOMPARE(int(gameMappings(list, q("UP"), src).size()), 3);

        QVERIFY(!hasOneIdentity({}, src));
        QVERIFY(hasOneIdentity({game, gameNoMarket}, src));
        QVERIFY(!hasOneIdentity({game, ep}, src));
        QVERIFY(!hasOneIdentity({game, demo}, src));
        QVERIFY(hasOneIdentity({game, sku}, src));
        DlcCandidate sku2 = sku;
        sku2.contentId = q("UP0700-PPSA05678_02-DLCSUFFIX0000001");
        QVERIFY(!hasOneIdentity({game, sku, sku2}, src));
    }
};

QTEST_GUILESS_MAIN(TstDlcLookup)
#include "tst_dlc_lookup.moc"
