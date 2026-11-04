#pragma once

// sce_sys/param.json: tolerant reading, the normalisation written into the
// package (.gp5-assets/<stem>/sce_sys/param.json), and the package size class
// (attributePub / kernel.addcontMountLevel) of full builds.
//
// The document is kept in an order-preserving model (core/OrderedJson.h) and
// written like Python's json.dumps(indent=2, ensure_ascii=False) + "\n" with
// CRLF line endings, so only the fields we change differ from the source.

#include "core/OrderedJson.h"
#include "core/Result.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <optional>

namespace fw::fpkg {

// UTF-8 with or without BOM; trailing commas and comments are tolerated. The
// top level must be an object.
Result<json::Value> parseParamJson(const QByteArray& bytes);
Result<json::Value> readParamJson(const QString& path);

// "en-US" when localizedParameters.defaultLanguage is missing or not a string.
QString paramDefaultLanguage(const json::Value& param);
QString paramTitle(const json::Value& param); // titleName in the default language, or ""

struct ParamPatchOptions {
    bool clearVersionFileUri = true;
    bool clearAttribute3 = false;
    bool lowerRequiredFirmware = true;
    QString contentVersion; // "" = keep
    QString title;          // "" = keep
};

// applicationDrmType is always set to "standard" (a "free" DRM type shows a
// lock and the game refuses to start). Returns one note per change.
QStringList applyParamPatch(json::Value& param, const ParamPatchOptions& options);

// ---- package size class -------------------------------------------------------
inline constexpr qint64 kSizeClass0Max = 162514599936LL;
inline constexpr qint64 kSizeClass1Max = 195878191104LL;
inline constexpr qint64 kSizeClass2Max = 260436918272LL; // largest class of SDK 2.79 (warning above)
inline constexpr qint64 kMountLevel2Max = 97945387008LL;
inline constexpr qint64 kLv2MaxFiles = 500000;

// E = U + max(512 MiB, ceil(U / 100)) + 4096 * F
qint64 estimatePackageBytes(qint64 uncompressedBytes, qint64 fileEntries);

// Mount level: 2 -> 1 above kMountLevel2Max, then 1 -> 0 above kSizeClass1Max.
// Class: 0 when the level is still 2 or E <= kSizeClass0Max, 1 up to
// kSizeClass1Max, else 2.
struct SizeClassChoice {
    int attributePub = 0;
    int mountLevel = 0;
};
SizeClassChoice chooseSizeClass(qint64 estimate, int mountLevel);

struct SizeClassDecision {
    bool applied = false;     // false: skipped (see note)
    qint64 estimate = 0;
    int attributePub = 0;
    int mountLevelBefore = 0;
    int mountLevelAfter = 0;
    QStringList notes;        // for 01-create-gp5.log
    QStringList warnings;     // shown to the user
};

// U = uncompressed bytes of every mapped file except the source param.json,
// F = number of <file> entries in the project. Errors: missing or non-integer
// applicationCategoryType, a kernel that is not an object, an invalid
// addcontMountLevel, more than 500,000 files in size class 2. A category other
// than 0 is skipped with a warning.
Result<SizeClassDecision> applyPackageSizeClass(json::Value& param, qint64 uncompressedBytes, qint64 fileEntries);

// Python json.dumps(indent=2, ensure_ascii=False) + "\n", then LF -> CRLF.
QByteArray serializeParamJson(const json::Value& param);
QByteArray toCrlf(const QByteArray& lf);

// "0x0114000000000000" or a number; nullopt when neither.
std::optional<quint64> parseHexOrNumber(const json::Value& v);

} // namespace fw::fpkg
