#pragma once

// Inputs the Publishing Tools reject unless they are fixed first: sce_sys
// presentation images (PNG colour type), PS4-format executables, and trophy /
// UDS archives (.ucp) with a stale digest. The dump is never modified:
// corrected copies go to .gp5-assets/<stem>/ and the project points at them.

#include "core/Cancellation.h"
#include "core/Result.h"
#include "engines/fpkg/SourceScanner.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <vector>

namespace fw::fpkg {

// ---- PNG -------------------------------------------------------------------------
struct PngInfo {
    bool valid = false;  // signature + IHDR
    int bitDepth = 0;
    int colorType = -1;  // 2 = RGB, 6 = RGBA
    int interlace = 0;
    QString problem;
};
PngInfo pngInfoFromBytes(const QByteArray& head);
PngInfo readPngInfo(const QString& path);

enum class PngRequirement { None, Rgb, Rgba };
// sce_sys/icon0|pic0|pic1[_NN].png must be 8-bit RGB, pic2[_NN].png 8-bit RGBA.
PngRequirement presentationRequirement(const QString& fileName);
bool pngMeets(const PngInfo& info, PngRequirement req);
QString requirementText(PngRequirement req);

struct ImageConversion {
    QString dst;          // package path, e.g. "sce_sys/pic2.png"
    QString ddsRel;
    QString ddsAbs;
    QString replacedPngRel; // the dump's own PNG that is left out, or ""
    PngRequirement requirement = PngRequirement::None;
    QString reason;       // for the log
};

struct ImagePlan {
    std::vector<ImageConversion> conversions;
    QStringList notes;
};

// `recoverEnabled`: fpkg.recoverPngFromDds; `haveConverter`: prospero-dds2png found.
Result<ImagePlan> planImageRecovery(const ScanResult& scan, bool recoverEnabled, bool haveConverter);

// ---- SELF --------------------------------------------------------------------------
inline const QByteArray kPs4SelfMagic = QByteArray("\x4F\x15\x3D\x1D", 4);
inline const QByteArray kPs5SelfMagic = QByteArray("\x54\x14\xF5\xEE", 4);
// Copies `src` to `dst` with the PS5 SELF signature and the source's timestamp.
Status writeSelfRepairCopy(const QString& src, const QString& dst, const CancellationToken& token);

// ---- UCP ---------------------------------------------------------------------------
// Layout (big-endian): magic 0xB228C60A, version 1, u64 file size at 0x08,
// u32 entry count at 0x10, u32 record size 0x40 at 0x14, SHA-1 at 0x1C over
// the whole file with the digest field zeroed; 0x40-byte records from 0x60
// (32-byte name, u64 offset, u64 size). Format facts from LibProsperoPkg
// (ProsperoUcp, GPL-3.0-or-later); UNVERIFIED on real dumps.
struct UcpCheck {
    bool structureOk = false;
    bool digestOk = false;
    QString problem;
};
inline constexpr qint64 kUcpMaxBytes = 128LL * 1024 * 1024;
bool isUcpPath(const QString& rel); // sce_sys/trophy2/*.ucp, sce_sys/uds/*.ucp
UcpCheck checkUcp(const QByteArray& data);
QByteArray withRepairedUcpDigest(const QByteArray& data);

} // namespace fw::fpkg
