#pragma once

// SDK pair table (docs/BACKPORT_PARITY.md §2.6). Facts only: the PS5 SDK
// value written at param +0x14 and the PS4 compatibility value at +0x10 for
// each supported target. Pairs 1-10 are the pinned SDK patcher's table;
// pairs 11-13 are the process-param values of retail eboots recorded by the
// owner's Backport engine (see SdkTable.cpp). An SDK N build only loads on
// firmware N.00 and later; SDK 5 is the only target above 4 with a reported
// hardware result.

#include <QByteArray>
#include <QString>

#include <array>
#include <optional>

namespace fw::backport {

struct SdkPair {
    int n = 0;          // target SDK major (1..13)
    quint32 ps5 = 0;    // u32 LE at param +0x14
    quint32 ps4 = 0;    // u32 LE at param +0x10
};

inline constexpr int kMinTargetSdk = 1;
inline constexpr int kMaxTargetSdk = 13;
inline constexpr int kDefaultTargetSdk = 4;
// The legacy Python app offered targets up to 11 but wrote pair 10 for 11.
inline constexpr int kMaxLegacyTargetSdk = 10;
inline constexpr std::size_t kPairCount = 13;

const std::array<SdkPair, kPairCount>& sdkPairs();
// nullptr when `n` is not a supported pair (S3: never clamp).
const SdkPair* sdkPair(int n);

// Top byte read as BCD: 0x10000040 → 10. nullopt for non-BCD nibbles.
std::optional<int> bcdMajor(quint32 value);
// Second byte read as BCD: 0x09590001 (PS4 9.59) → 59.
std::optional<int> bcdMinor(quint32 value);
// "7.00" style text for a PS5/PS4 SDK value, or "0x…" when not BCD.
QString sdkVersionText(quint32 value);

// "SDK 4 · loads on FW 4.00+"
QString firmwareLabel(int n);
// "4xx" (manifest target key, Python normalize_fw)
QString targetKey(int n);

// SHA-256 over the pair table and the NID substitution table, identifying
// the engine's data in manifests and the status bar.
QByteArray tableHash();

} // namespace fw::backport
