#pragma once

// The small amount of game metadata the backport engine needs: title ID,
// title and version for ZIP names (§2.13) and the manifest folder (§2.14),
// plus the param.json SDK / required firmware for display only (D7: the
// game's SDK for decisions comes from eboot.bin). Kept inside this module on
// purpose (the gameinfo module is separate).

#include <QString>
#include <QVariant>

#include <optional>
#include <utility>

namespace fw::backport {

struct GameMetadata {
    QString titleId;      // upper case, e.g. PPSA01234
    QString title;
    QString version;      // e.g. 01.000.000
    QString source;       // absolute path of the param.json / param.sfo used
    std::optional<int> paramSdk;          // param.json sdkVersion (display only)
    std::optional<int> requiredFwMajor;   // requiredSystemSoftwareVersion
    std::optional<int> requiredFwMinor;
    QString requiredFwText() const;       // "12.70" or empty
};

// param.json in <game>/sce_sys, <game>, then the parent folder (as the
// legacy app); then param.sfo in the same places. Never throws, never writes.
GameMetadata readGameMetadata(const QString& game);

// "<PPSA> <Title> (<version>)", "<PPSA> <Title>", "<PPSA>", the folder name
// or "Unknown Game" (legacy _sfo_zip_prefix rules, including the folder-name
// fallbacks).
QString zipPrefix(const GameMetadata& meta, const QString& folderName);

// The manifest folder name: title ID, else the folder name, else UNKNOWN;
// only letters, digits, '-' and '_' kept (legacy backup_root_for).
QString manifestTitleId(const GameMetadata& meta, const QString& game);

// PS5 version fields in param.json: hex digits read as BCD
// ("0x1270000000000000" → 12.70); non-BCD hex → top byte; plain decimal.
std::optional<std::pair<int, int>> parsePs5VersionField(const QVariant& raw);

} // namespace fw::backport
