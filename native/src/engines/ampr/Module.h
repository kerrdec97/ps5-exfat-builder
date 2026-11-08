#pragma once

// zstd asset packs (see ZstdPack.h, GamePack.h).
//
// Settings (Build > zstd packs, per job):
//   ampr.zstdPack       pack the game's files into zstd packs before an image build
//   ampr.zstdLevel      5 (fast, default), 9 (medium), 19 (smallest, slow)
//   ampr.packStrategy   everything (default) | compatible
//   ampr.verifyPacks    read every volume back after packing (default on)
//
// Dependency: ampr.runtime (the bundled ampr-emu-ng libSceAmpr.sprx).
//
// Jobs:
//   ampr.pack   {source (folder), output (new folder), level?, strategy?}
//               a packed copy of a game folder
//   ampr.unpack {source (folder), output (new folder)}
//               a packed game folder (zstd or LZ4) as plain files
// Image builds with packs and image sources are jobs of the integration module.

#include "core/Settings.h"
#include "engines/ampr/PackRules.h"
#include "jobs/Module.h"

namespace fw::ampr {

namespace keys {
inline const QString kZstdPack = QStringLiteral("ampr.zstdPack");
inline const QString kZstdLevel = QStringLiteral("ampr.zstdLevel");
inline const QString kPackStrategy = QStringLiteral("ampr.packStrategy");
inline const QString kVerifyPacks = QStringLiteral("ampr.verifyPacks");
} // namespace keys

inline const QString kPackJob = QStringLiteral("ampr.pack");
inline const QString kUnpackJob = QStringLiteral("ampr.unpack");

struct PackSettings {
    bool enabled = false;
    int level = 5;
    PackStrategy strategy = PackStrategy::Everything;
    bool verify = true;
    int threads = 0; // performance.workerThreads (resolved)
};
// The job's parameters ("level", "strategy") win over the settings.
PackSettings readPackSettings(const SettingsValues& s, const QJsonObject& params = {});

void registerModule(ModuleContext& ctx);

} // namespace fw::ampr
