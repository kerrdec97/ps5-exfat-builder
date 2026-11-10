#pragma once

// How an SDK version is stored in a process / module param (see SceParam.h):
// a u32 whose top byte is the major and whose second byte is the minor, both
// BCD (0x04500001 = 4.50).

#include <QByteArray>
#include <QString>

#include <array>
#include <optional>

namespace fw::backport {

// Top byte read as BCD: 0x10000040 → 10. nullopt for non-BCD nibbles.
std::optional<int> bcdMajor(quint32 value);
// Second byte read as BCD: 0x09590001 (PS4 9.59) → 59.
std::optional<int> bcdMinor(quint32 value);
// "7.00" style text for a PS5/PS4 SDK value, or "0x…" when not BCD.
QString sdkVersionText(quint32 value);

} // namespace fw::backport
