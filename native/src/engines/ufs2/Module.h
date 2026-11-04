#pragma once

#include "engines/ufs2/Ufs2Format.h"
#include "jobs/Module.h"

namespace fw::ufs2 {

// Registers the FFPKG (UFS2) settings and the image.ffpkg.* job kinds.
void registerModule(ModuleContext& ctx);

namespace keys {
inline const QString kBlockSize = QStringLiteral("ufs2.blockSize");             // newfs -b
inline const QString kFragmentSize = QStringLiteral("ufs2.fragmentSize");       // newfs -f
inline const QString kMinFreePercent = QStringLiteral("ufs2.minFreePercent");   // newfs -m
inline const QString kBytesPerInode = QStringLiteral("ufs2.bytesPerInode");     // newfs -i
inline const QString kDeleteSourceAfterBuild = QStringLiteral("ufs2.deleteSourceAfterBuild");
} // namespace keys

namespace kinds {
inline const QString kBuild = QStringLiteral("image.ffpkg.build");
inline const QString kVerify = QStringLiteral("image.ffpkg.verify");
inline const QString kExtract = QStringLiteral("image.ffpkg.extract");
inline const QString kEdit = QStringLiteral("image.ffpkg.edit");
} // namespace kinds

// newfs parameters from the ufs2.* settings. A fragment larger than the block
// is reduced to the block size, as the legacy app did; `notes` says so.
FormatParams formatParams(const SettingsValues& settings, QStringList* notes = nullptr);

} // namespace fw::ufs2
