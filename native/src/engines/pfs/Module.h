#pragma once

// PFS / FFPFSC engine (native port of Lazy_MkPFS, GPL-3.0-or-later).
// See native/docs/engines/PFS.md.

#include "jobs/Module.h"

#include "engines/pfs/PfsBuilder.h"
#include "engines/pfs/PfsSource.h"
#include "engines/pfs/PfsVerify.h"

#include <optional>

namespace fw::pfs {

// Registers this module's settings and job kinds
// (image.pfs.build, image.pfs.verify, image.pfs.extract).
void registerModule(ModuleContext& ctx);

namespace keys {
inline const QString kCompressionLevel = QStringLiteral("pfs.compressionLevel");
inline const QString kContainerMinCompressMiB = QStringLiteral("pfs.containerMinCompressMiB");
inline const QString kContainerBlockGain = QStringLiteral("pfs.containerBlockGain");
inline const QString kCompressFiles = QStringLiteral("pfs.compressFiles");
inline const QString kFileMinCompressMiB = QStringLiteral("pfs.fileMinCompressMiB");
inline const QString kFileBlockGain = QStringLiteral("pfs.fileBlockGain");
inline const QString kFileMinGain = QStringLiteral("pfs.fileMinGain");
inline const QString kSkipExecutables = QStringLiteral("pfs.skipExecutables");
inline const QString kBlockSize = QStringLiteral("pfs.blockSize");
inline const QString kCaseInsensitive = QStringLiteral("pfs.caseInsensitive");
inline const QString kImageVersion = QStringLiteral("pfs.imageVersion");
inline const QString kExcludes = QStringLiteral("pfs.excludes");
inline const QString kKeepEmptyFolders = QStringLiteral("pfs.keepEmptyFolders");
inline const QString kAmprIndex = QStringLiteral("pfs.amprIndex");
inline const QString kThreads = QStringLiteral("pfs.threads");
inline const QString kDeleteSourceAfterConvert = QStringLiteral("pfs.deleteSourceAfterConvert");
} // namespace keys

namespace kinds {
inline const QString kBuild = QStringLiteral("image.pfs.build");
inline const QString kVerify = QStringLiteral("image.pfs.verify");
inline const QString kExtract = QStringLiteral("image.pfs.extract");
} // namespace kinds

// Engine options from a settings snapshot (every pfs.* setting is read here or
// by the jobs). `time` is the inode timestamp.
BuildOptions buildOptionsFrom(const SettingsValues& settings, qint64 time);
PlanOptions planOptionsFrom(const SettingsValues& settings, qint64 time);
// Worker threads: pfs.threads, or performance.workerThreads when it is 0.
int threadsFrom(const SettingsValues& settings);
// VerifyDepth from the general module's verification.verifyAfterBuild and
// verification.mode; std::nullopt when verification is off.
std::optional<VerifyDepth> verifyDepthFrom(const SettingsValues& settings);
// storage.freeSpaceMarginMiB in bytes (the app-wide reserve).
qint64 freeSpaceMarginFrom(const SettingsValues& settings);

} // namespace fw::pfs
