#pragma once

// Fake-signed SELF format constants and helpers, as far as the reader needs
// them.
//
// Derived from make_fself.py by john-tornblom (ps5-payload-dev/sdk,
// samples/install_app/make_fself.py, GNU GPL version 3), as modified by
// Nazky (@NazkyYT) in Auto-Backpork. C++ port for the exFAT Image Builder.
// This file is distributed under the terms of the GNU General Public License
// version 3, like the code it derives from; see docs/BACKPORT_PARITY.md §2.1
// and native/docs/engines/BACKPORT.md.

#include <QtGlobal>

namespace fw::backport::self {

inline constexpr quint32 kMagicFake = 0x1D3D154F; // bytes 4F 15 3D 1D (PS4 / fake-signed)
inline constexpr quint32 kMagicPs5 = 0xEEF51454;  // bytes 54 14 F5 EE (PS5)

inline constexpr int kCommonHeaderSize = 0x20;
inline constexpr int kEntrySize = 0x20;
inline constexpr int kExInfoSize = 0x40;       // paid, ptype, app_version, fw_version, digest[32]
inline constexpr int kDigestSize = 0x20;

// Entry property bits (§2.7).
namespace props {
inline constexpr quint64 kEncrypted = 1ULL << 1;
inline constexpr quint64 kCompressed = 1ULL << 3;
inline constexpr quint64 kHasBlocks = 1ULL << 11;
inline constexpr int kSegmentIndexShift = 20;
inline constexpr quint64 kSegmentIndexMask = 0xFFFF;

inline constexpr int segmentIndex(quint64 p) { return int((p >> kSegmentIndexShift) & kSegmentIndexMask); }
} // namespace props

inline constexpr quint64 alignUp(quint64 x, quint64 a) { return (x + (a - 1)) & ~(a - 1); }

} // namespace fw::backport::self
