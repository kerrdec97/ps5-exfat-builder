#pragma once

// PS5/PS4 DLC identifiers (the ppr-pkg Companion's DlcIdentity): Title ID and
// Content ID rules, region inference from the publisher prefix, and the
// region retarget that changes the publisher prefix and the title ID while
// keeping "_NN" and the 16-character suffix.
//
// Content ID layout (0-based): [0..5] publisher prefix (UP9000), [6] '-',
// [7..15] title ID, [16] '_', [17..18] NN, [19] '-', [20..35] suffix.

#include "core/Result.h"

#include <QString>

namespace fw::dlc {

bool isTitleId(const QString& titleId);       // ^(?:PPSA|CUSA)[0-9]{5}$
bool isContentId(const QString& contentId);   // ^[A-Z]{2}[0-9]{4}-(?:PPSA|CUSA)[0-9]{5}_[0-9]{2}-[A-Z0-9_]{16}$
bool isPublisherPrefix(const QString& prefix); // ^[A-Z]{2}[0-9]{4}$
QString titleIdOf(const QString& contentId);  // [7..15], "" when invalid
QString suffixOf(const QString& contentId);   // [20..35], "" when invalid
bool isPs5TitleId(const QString& titleId);    // starts with PPSA

// "Title ID must be PPSA or CUSA ...", "Content ID must use ...", "Title ID must match ...".
Status validateIdentity(const QString& titleId, const QString& contentId);

// "UP" / "EP" / "JP" / "HP" / "KP" / other two letters; "" for an invalid ID.
QString regionCode(const QString& contentId);
// "Americas (inferred from UP)", ..., "Unknown (prefix XX)", "Unknown".
QString regionText(const QString& contentId);

// prefix (default: the original's) + "-" + targetTitleId + original[16..].
Result<QString> retargetContentId(const QString& original, const QString& targetTitleId, const QString& prefix = {});

} // namespace fw::dlc
