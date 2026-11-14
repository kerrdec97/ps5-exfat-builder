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

inline constexpr const char* kBundledRuntimeVersion = "0.3.3";
inline constexpr const char* kBundledRuntimeSha256 = "1c1051f91bc7cba19b15362b2b832114b53a4c3d6193f50c90fdf1ae5d78d268";
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
// Identifies a libSceAmpr.sprx by its contents.
RuntimeInfo identifyRuntime(const QByteArray& sprx);

} // namespace fw::ampr
