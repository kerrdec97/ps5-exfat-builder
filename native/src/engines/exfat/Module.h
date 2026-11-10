#pragma once

#include "jobs/Module.h"

namespace fw::exfat {

// Registers this module's settings, dependency probes and job kinds.
void registerModule(ModuleContext& ctx);

// Job kinds.
inline const QString kJobBuild = QStringLiteral("image.exfat.build");
inline const QString kJobVerify = QStringLiteral("image.exfat.verify");
inline const QString kJobExtract = QStringLiteral("image.exfat.extract");
inline const QString kJobEdit = QStringLiteral("image.exfat.edit");

// Settings declared here (consumers: ExfatJobs.cpp).
namespace keys {
inline const QString kClusterSize = QStringLiteral("exfat.clusterSize");
inline const QString kSectorSize = QStringLiteral("exfat.sectorSize");
inline const QString kImageSizeGiB = QStringLiteral("exfat.imageSizeGiB");
inline const QString kSpareSpace = QStringLiteral("exfat.spareSpace");
inline const QString kSpareSpaceMiB = QStringLiteral("exfat.spareSpaceMiB");
inline const QString kLabelPolicy = QStringLiteral("exfat.labelPolicy");
inline const QString kLabel = QStringLiteral("exfat.label");
inline const QString kRequireEboot = QStringLiteral("exfat.requireEboot");
inline const QString kExcludePatterns = QStringLiteral("exfat.excludePatterns");
inline const QString kExcludeHidden = QStringLiteral("exfat.excludeHidden");
inline const QString kAmprIndex = QStringLiteral("exfat.amprIndex");
inline const QString kOverwriteExisting = QStringLiteral("exfat.overwriteExisting");
inline const QString kAllocation = QStringLiteral("exfat.allocation");
inline const QString kOpenRetries = QStringLiteral("exfat.openRetries");
inline const QString kRetryWaitSeconds = QStringLiteral("exfat.retryWaitSeconds");
inline const QString kEditStrategy = QStringLiteral("exfat.editStrategy");
inline const QString kExtractParallel = QStringLiteral("exfat.extractParallel");
inline const QString kExtractThreads = QStringLiteral("exfat.extractThreads");
inline const QString kDeleteSourceAfterVerify = QStringLiteral("exfat.deleteSourceAfterVerify");
} // namespace keys

// Default exclusions: images nested in a dump (the legacy robocopy /XF
// *.exfat *.ffpkg, plus the PFS images the legacy verifier skipped),
// Windows/macOS folder clutter, and drive-root system folders.
QStringList defaultExcludePatterns();

} // namespace fw::exfat
