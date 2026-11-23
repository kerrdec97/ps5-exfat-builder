#pragma once

// The console side of zstd packs: ampr-emu-ng's release libSceAmpr.sprx
// (quer3q/ampr-emu-ng, GPL-3.0-or-later), bundled next to the app as
// engines/ampr-emu-ng/libSceAmpr.sprx and pinned by SHA-256
// (cmake/AmprRuntime.cmake). A packed game gets it at fakelib/libSceAmpr.sprx;
// nothing else in fakelib/ is touched (backported games keep their libraries).

#include "core/Result.h"

#include <QByteArray>
#include <QString>

namespace fw::ampr {

inline constexpr const char* kBundledRuntimeVersion = "0.5.0";
inline constexpr const char* kBundledRuntimeSha256 = "7379e49953d8f14f26b45ff06a863ecea56aa90a60ba3d5a375f61c52618a174";
inline const QString kRuntimeRelPath = QStringLiteral("fakelib/libSceAmpr.sprx");

// FW_AMPR_RUNTIME (tests, development), else <app>/engines/ampr-emu-ng/libSceAmpr.sprx.
QString bundledRuntimePath();
// The bundled runtime's bytes, checked against the pinned SHA-256.
Result<QByteArray> loadBundledRuntime();
// For the jobs and the dependency check: "" when the runtime is usable, else
// why not (English, like every engine error).
QString bundledRuntimeProblem();
// The same text in the interface's language, for the list of tools.
QString bundledRuntimeProblemShown();

enum class RuntimeKind { None, AmprEmuNg, AmprEmu, Unknown };

struct RuntimeInfo {
    RuntimeKind kind = RuntimeKind::None;
    QString version;  // "0.3.2", "0.4.2.1", "" when unknown
    bool bundled = false; // byte-for-byte the bundled build
    QString describe() const; // "ampr-emu-ng 0.3.2", "ampr_emu 0.4.2.1", "an unknown libSceAmpr.sprx"
};
// Identifies a libSceAmpr.sprx by its contents: the 0.3.x string
// "ampr-emu-ng 0.3.3" and the 0.4.x/0.5.x notice "ampr-emu-ng: v0.5.0 (zstd)
// running" both give the version.
RuntimeInfo identifyRuntime(const QByteArray& sprx);

// Dotted version numbers compared part by part ("0.10.0" > "0.9.9", "0.4" ==
// "0.4.0"): <0, 0 or >0. An empty or malformed version is unknown and
// compares as 0 with everything.
int compareVersions(const QString& a, const QString& b);

// What a packed game needs from the mounter (English, for job warnings;
// the setting's description says the same in the interface's language).
QString packedGameMounterNote();

} // namespace fw::ampr
