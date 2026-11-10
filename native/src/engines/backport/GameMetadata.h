#pragma once

// PS5 version fields of a param.json, as the Backport tab shows them for the
// games in its list (display only). Kept inside this module on purpose (the
// gameinfo module is separate).

#include <QString>
#include <QVariant>

#include <optional>
#include <utility>

namespace fw::backport {

// PS5 version fields in param.json: hex digits read as BCD
// ("0x1270000000000000" → 12.70); non-BCD hex → top byte; plain decimal.
std::optional<std::pair<int, int>> parsePs5VersionField(const QVariant& raw);

} // namespace fw::backport
