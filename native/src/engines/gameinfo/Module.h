#pragma once

#include "jobs/Module.h"

#include <QString>

namespace fw::gameinfo {

// Registers this module's settings and job kinds (library.scan, library.rename).
void registerModule(ModuleContext& ctx);

namespace keys {
// Folders scanned by the Library (legacy lib_folders).
inline const QString kLibraryFolders = QStringLiteral("library.folders");
// Default Dump Rename pattern: ppsa / ppsa_title / ppsa_title_v.
inline const QString kRenamePreset = QStringLiteral("library.renamePreset");
// Where normalised covers are kept; empty = the app's cache folder.
inline const QString kCoverCacheFolder = QStringLiteral("library.coverCacheFolder");
} // namespace keys

namespace jobkinds {
inline const QString kLibraryScan = QStringLiteral("library.scan");
inline const QString kLibraryRename = QStringLiteral("library.rename");
} // namespace jobkinds

} // namespace fw::gameinfo
