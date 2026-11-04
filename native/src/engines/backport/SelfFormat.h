#pragma once

// Fake-signed SELF format constants and helpers.
//
// Derived from make_fself.py by john-tornblom (ps5-payload-dev/sdk,
// samples/install_app/make_fself.py, GNU GPL version 3), as modified by
// Nazky (@NazkyYT) in Auto-Backpork. C++ port for the exFAT Image Builder
// Farewell Edition. This file is distributed under the terms of the GNU
// General Public License version 3, like the code it derives from; see
// docs/BACKPORT_PARITY.md §2.1 and native/docs/engines/BACKPORT.md.

#include <QtGlobal>

namespace fw::backport::self {

inline constexpr quint32 kMagicFake = 0x1D3D154F; // bytes 4F 15 3D 1D (PS4 / fake-signed)
inline constexpr quint32 kMagicPs5 = 0xEEF51454;  // bytes 54 14 F5 EE (PS5)

inline constexpr int kCommonHeaderSize = 0x20;
inline constexpr int kEntrySize = 0x20;
inline constexpr int kExInfoSize = 0x40;       // paid, ptype, app_version, fw_version, digest[32]
inline constexpr int kNpdrmBlockSize = 0x30;   // u16 type, 14 pad, content_id[19], random_pad[13]
inline constexpr int kMetaBlockSize = 0x50;
inline constexpr int kMetaFooterSize = 0x50;   // u32 unk1 at +0x30
inline constexpr int kSignatureSize = 0x100;
inline constexpr int kDigestSize = 0x20;
inline constexpr quint64 kBlockSize = 0x4000;

inline constexpr quint8 kVersion = 0x00;
inline constexpr quint8 kMode = 0x01;
inline constexpr quint8 kEndian = 0x01;
inline constexpr quint8 kAttributes = 0x12;
inline constexpr quint32 kKeyType = 0x101;
inline constexpr quint16 kFlags = 0x2 | (2 << 4); // signed block count 2
inline constexpr quint16 kNpdrmControlType = 0x3;
inline constexpr quint32 kMetaFooterUnk1 = 0x10000;

inline constexpr quint64 kDefaultPaid = 0x3100000000000002ULL;
inline constexpr quint64 kPtypeFake = 0x1;

// Entry property bits (§2.7).
namespace props {
inline constexpr quint64 kOrder = 1ULL << 0;
inline constexpr quint64 kEncrypted = 1ULL << 1;
inline constexpr quint64 kSigned = 1ULL << 2;
inline constexpr quint64 kCompressed = 1ULL << 3;
inline constexpr int kWindowBitsShift = 8;
inline constexpr quint64 kHasBlocks = 1ULL << 11;
inline constexpr int kBlockSizeShift = 12;
inline constexpr quint64 kHasDigests = 1ULL << 16;
inline constexpr quint64 kHasExtents = 1ULL << 17;
inline constexpr int kSegmentIndexShift = 20;
inline constexpr quint64 kSegmentIndexMask = 0xFFFF;

inline constexpr int segmentIndex(quint64 p) { return int((p >> kSegmentIndexShift) & kSegmentIndexMask); }
inline constexpr quint64 withSegmentIndex(quint64 p, quint64 index)
{
    return (p & ~(kSegmentIndexMask << kSegmentIndexShift)) | ((index & kSegmentIndexMask) << kSegmentIndexShift);
}
inline constexpr quint64 blockSizeFor(quint64 p) { return 1ULL << (12 + ((p >> kBlockSizeShift) & 0xF)); }
} // namespace props

inline constexpr quint64 alignUp(quint64 x, quint64 a) { return (x + (a - 1)) & ~(a - 1); }

} // namespace fw::backport::self
