#pragma once

// Games as the pack engine sees them (ampr::GameFiles), from any source the
// app reads: a folder, an .exfat, .ffpkg, .ffpfs or .ffpfsc image (an image
// inside a PFS container is opened through), plus the jobs that need both the
// image readers and the pack engine:
//
//   image.build.zstd {format, source, output, ...build parameters}
//       Packs the game (folder or ZIP) into zstd packs in a work folder next to
//       the output, then builds the image (image.exfat.build,
//       image.ffpkg.build or image.pfs.build) from it. Phases: Preparing,
//       Packing, Checking packs, then the image build's phases.
//
//   game.extract {source, output, unpackPacks?}
//       Writes the files of a game (folder or image) into a new folder; zstd
//       and LZ4 packs are unpacked unless unpackPacks is false.
//
// FPKG builds from images and from packed folders stage only the files the
// package keeps (FpkgGameStager).

#include "core/Cancellation.h"
#include "core/Result.h"
#include "engines/ampr/GameFiles.h"
#include "engines/integration/ImageFiles.h"
#include "jobs/Job.h"
#include "jobs/Module.h"

#include <memory>

namespace fw::integration {

inline const QString kZstdBuildJob = QStringLiteral("image.build.zstd");
inline const QString kGameExtractJob = QStringLiteral("game.extract");

std::shared_ptr<ampr::GameFiles> gameFilesOf(std::unique_ptr<FileTree> tree);
Result<std::shared_ptr<ampr::GameFiles>> openGameFiles(const QString& path, const CancellationToken& token);

// The folder inside `files` that holds sce_sys/param.json (or sce_sys), "" for
// the root, at most three levels deep; null when there is none.
std::optional<QString> findGameRoot(const ampr::GameFiles& files);

void registerGameJobs(ModuleContext& ctx);
void registerFpkgGameStager();

} // namespace fw::integration
