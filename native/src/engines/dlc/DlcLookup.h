#pragma once

// Optional online lookup of a DLC's game and its regional releases (the
// ppr-pkg Companion's DlcLookupService, DlcRegionalCatalog and
// DlcRegionSelection, ported). Sources: the PlayStation Store (product
// pages, concept pages, the chihiro search API), SerialStation and
// ProsperoPatches. Results are identifier observations: they never prove
// that a DLC works with another region's game.
//
// Blocking API: call from a worker thread. Network access goes through a
// replaceable fetch function (tests use fixtures). Responses are cached for
// 7 days in `cacheDir` (newest 128 files kept).

#include "core/Cancellation.h"
#include "core/Result.h"

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <functional>
#include <vector>

namespace fw::dlc {

struct Market {
    QString country;     // "US"
    QString language;    // "en"
    QString locale;      // "en-us"
    QString code;        // publisher prefix region: "UP"
    QString displayName; // "Americas (US)"
};
const QList<Market>& markets(); // US/UP, GB/EP, JP/JP, HK/HP, KR/KP
const Market& marketForPrefix(const QString& prefix); // default GB

struct DlcCandidate {
    QString titleId;
    QString contentId;
    QString name;
    QString edition;
    QString region = QStringLiteral("Unknown");
    QString marketRegionCode;   // "UP" ... or ""
    QString storeLocale;
    QString platform;           // "PS5" / "PS4"
    QString productKind;        // "Game", "Bundle", "DLC", "Demo", "Unknown"
    QString gameName;
    QString conceptId;
    QString version = QStringLiteral("Not reported");
    QString sourceUrl;
    QString mappingStatus = QStringLiteral("Unverified candidate");
    QString compatibilityStatus = QStringLiteral("Unverified — identifier mapping does not prove DLC compatibility");
};

struct DlcLookupResult {
    std::vector<DlcCandidate> candidates; // at most 100
    QStringList warnings;
    QStringList sourceLinks;
    bool fromCache = false;
    bool isStale = false;
    QString resolvedName;
    QDateTime storedAt;
};

class DlcLookupService {
public:
    // Returns the body, or an error with the Companion's texts ("HTTP 403 from
    // <host> (browser challenge or access restriction)", "Request timed out
    // after 6 seconds", "Lookup response exceeds the 2 MiB limit", ...).
    using Fetch = std::function<Result<QByteArray>(const QUrl& url, int timeoutMs, const CancellationToken& token)>;

    explicit DlcLookupService(QString cacheDir, Fetch fetch = {}); // empty fetch = HTTPS with QNetworkAccessManager
    // A title ID, content ID, game name or Store product URL typed by the user.
    DlcLookupResult lookup(const QString& query, const CancellationToken& token);
    // The main resolver: the game name for (titleId, contentId, gameName) and
    // its releases in the five markets.
    DlcLookupResult resolveRegions(const QString& titleId, const QString& contentId, const QString& gameName,
                                   const CancellationToken& token);

    static QString userAgent(); // "PprCompanion/1.5.1 (+optional-title-metadata-lookup)" equivalent for this app
    static bool isTrustedSource(const QUrl& url);

private:
    QString m_cacheDir;
    Fetch m_fetch;
};

// ---- choosing a target from candidates (DlcRegionSelection) -------------------------
bool canMap(const DlcCandidate& c, const QString& sourceContentId);
bool isExactDlcSku(const DlcCandidate& c, const QString& sourceContentId);
struct DlcProposal {
    QString titleId;
    QString contentId;
    QString title;
};
Result<DlcProposal> propose(const DlcCandidate& c, const QString& sourceContentId, const QString& title);
std::vector<DlcCandidate> forRegion(const std::vector<DlcCandidate>& candidates, const QString& regionCode,
                                    const QString& sourceContentId);
std::vector<DlcCandidate> dlcForRegion(const std::vector<DlcCandidate>& candidates, const QString& regionCode,
                                       const QString& sourceContentId);
std::vector<DlcCandidate> gameMappings(const std::vector<DlcCandidate>& candidates, const QString& regionCode,
                                       const QString& sourceContentId);
bool hasOneIdentity(const std::vector<DlcCandidate>& candidates, const QString& sourceContentId);

} // namespace fw::dlc
