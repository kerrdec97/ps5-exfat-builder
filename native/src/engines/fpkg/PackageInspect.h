#pragma once

// Reading a built PS5 package without keys (the ppr-pkg Companion's
// SdkFix.Inspect, ported):
//  * kind from the first four bytes: 7F 'F' 'I' 'H' a full package, 7F 'L' 'I'
//    'H' an update delta, 7F 'C' 'N' 'T' a PS4 package;
//  * the digest of a full package: the 32 bytes at 0x30..0x4F. An update
//    built against that package carries them; one that does not fails on the
//    console with CE-107891-6;
//  * its param.json: the last JSON object holding "contentId" and
//    "contentVersion" found in the last 1 GiB of the file (debug packages
//    keep their metadata entries in plaintext). Update and remastered images
//    also carry the base's copies (origin/target param.json, found on real
//    Sony output): of the copies within 1 MiB, the newest contentVersion wins.

#include "core/Cancellation.h"
#include "core/OrderedJson.h"
#include "core/Result.h"

#include <QByteArray>
#include <QString>

#include <optional>

namespace fw::fpkg {

enum class PackageKind { Ps5, Ps5Delta, Ps4, Unknown };
QString packageKindName(PackageKind kind); // "ps5" | "ps5-delta" | "ps4" | "unknown"

struct PackageIdentity {
    PackageKind kind = PackageKind::Unknown;
    QByteArray digest;      // 32 bytes (full packages only)
    QString digestHex;      // upper-case
    bool hasParam = false;
    json::Value param;
    QString contentId;
    QString titleId;        // titleId, else from the Content ID
    QString version;        // contentVersion
    QString title;          // titleName in the default language
};

// Errors only when the file cannot be read or is shorter than 80 bytes.
Result<PackageIdentity> inspectPackage(const QString& path, const CancellationToken& token);

// A digest that can identify a package: 32 bytes with at least 8 different
// values (not zeros or a repeated pattern, which any file contains).
bool isPlausibleDigest(const QByteArray& digest);

// The bytes of the param.json object (see above), or nullopt. The search
// gives up (nullopt) after a fixed amount of work, so a crafted file cannot
// keep it busy.
Result<std::optional<QByteArray>> findParamJsonInPackage(const QString& path, const CancellationToken& token);

// The package's own sce_sys/playgo-chunk.dat ("plgx" header, version 0x1000,
// at least 256 bytes, its size field equal to its length), searched like
// param.json. nullopt when not found.
Result<std::optional<QByteArray>> findPlayGoChunkDatInPackage(const QString& path, const CancellationToken& token);

// How often `needle` occurs in the file (overlapping matches counted).
Result<qint64> countOccurrences(const QString& path, const QByteArray& needle, const CancellationToken& token);

// The ppr-pkg Companion's Content ID / Title ID rules.
bool isStrictContentId(const QString& id); // ^[A-Z]{2}[0-9]{4}-(PPSA|CUSA)[0-9]{5}_[0-9]{2}-[A-Z0-9_]{16}$
QString titleIdFromContentId(const QString& contentId); // "" unless strict

} // namespace fw::fpkg
