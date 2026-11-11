#pragma once

// Inputs the Publishing Tools reject or misread unless they are fixed first:
// sce_sys presentation images (PNG colour type), executables (PS4 signature,
// a misplaced .sceversion trailer), and trophy / UDS archives (.ucp) with a
// stale digest or missing altogether. The dump is never modified: corrected
// copies go to .gp5-assets/<stem>/ and the project points at them. The SELF
// and UDS rules follow the reference toolkit (sdk-fpkg279-fix15).

#include "core/Cancellation.h"
#include "core/Result.h"
#include "engines/fpkg/SourceScanner.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <optional>
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
inline constexpr qint64 kSelfTrailerMaxBytes = 64LL * 1024 * 1024;

// What a SELF needs before Sony's publisher reads it:
//  * the PS4 signature replaced by the PS5 one;
//  * its ".sceversion" trailer moved to the offset the SELF header names: a
//    fake-signed file can start the trailer up to 15 bytes early, and the
//    publisher then cannot read the SDK version of the executable (img_verify:
//    "Format of the SELF file is not valid ... sceversion format").
struct SelfRepairPlan {
    bool rewriteMagic = false;
    qint64 trailerOffset = 0; // bytes copied before the padding
    int padding = 0;          // zero bytes inserted at trailerOffset (0-15)
    bool operator==(const SelfRepairPlan&) const = default;
    QString describe() const;
};
// True when `records` is one or more complete .sceversion records.
bool isCompleteSceVersion(const QByteArray& records);
// nullopt: not a SELF, or nothing to correct.
std::optional<SelfRepairPlan> selfRepairPlan(const QString& path);
// The highest SDK version of a SELF's .sceversion trailer, major and minor
// only (0xMMmm000000000000). nullopt: not a SELF or no readable trailer.
std::optional<quint64> executableSdkVersion(const QString& path);
// Copies `src` to `dst` as planned, with the source's timestamp. Fails when
// the file no longer matches the plan.
Status writeSelfRepairCopy(const QString& src, const QString& dst, const SelfRepairPlan& plan, const CancellationToken& token);

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

// ---- UDS archives named by sce_sys/uds/npbind.dat ---------------------------------------
// npbind.dat lists the NPWR id of every udsNN.ucp, by index. A udsNN.ucp
// that is missing or unusable is replaced by a well-formed archive without
// any activity data, so that the package builds and the game finds the file.
struct UcpMember {
    QString name;
    QByteArray data;
};
// The members of an archive, or why it cannot be read.
Result<std::vector<UcpMember>> unpackUcp(const QByteArray& data);
// An archive with a valid digest.
QByteArray packUcp(const std::vector<UcpMember>& members);
// NPWR ids ("NPWR12345_00") in index order.
Result<QStringList> readNpbindIds(const QByteArray& data);
struct UdsUcpVerdict {
    QStringList problems;    // empty: the archive is fine
    bool digestOnly = false; // the stale digest is the only problem
};
UdsUcpVerdict checkUdsUcp(const QByteArray& data, const QString& npwr);
// The empty archive for an NPWR id (four definition files without entries).
QByteArray emptyUdsUcp(const QString& npwr);

} // namespace fw::fpkg
