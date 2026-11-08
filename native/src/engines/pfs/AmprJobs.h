#pragma once

// The stand-alone AMPR index tool for a local folder (legacy Advanced › AMPR
// "Generate From Folder", ui/tab_advanced.py _generate_from_folder).
//
//   ampr.index.local {folder, force?}
//       Writes <folder>/ampr_emu.index (through a .part file; an existing
//       index is replaced only once the new one is complete) and reads it
//       back. Without force, a folder that has no fakelib/libSceAmpr.sprx is
//       refused with the legacy "Not an APR title?" question as the message;
//       the UI asks it and resubmits with force: true.
//
// The console variant (ampr.index.ftp) lives in the integration module: it
// needs the FTP client.

#include "engines/pfs/AmprIndex.h"
#include "jobs/Job.h"
#include "jobs/Module.h"

namespace fw::pfs {

inline const QString kAmprLocalJob = QStringLiteral("ampr.index.local");

void registerAmprJobs(ModuleContext& ctx);

// ampr_emu 0.4.2.1 asset packs (ampr_assets.index[.runtime] + *.pak) are
// bound to the exact ampr_emu.index they were built from; an index rebuilt
// over a packed /app0 lists the .pak files and lacks the packed originals,
// which breaks the set. Both AMPR jobs refuse such a folder unless the
// parameter "allowAssetPacks" is true.
bool isAmprAssetPackMarker(const QString& fileName); // ampr_assets.index, ampr_assets.index.runtime (any case)
QString folderAmprAssetPack(const QString& folder);  // the marker found in `folder`, empty when none
QString amprAssetPackProblem(const QString& folder, const QString& marker);

// True when <folder>/fakelib/libSceAmpr.sprx exists (any letter case).
bool folderHasAmprLibrary(const QString& folder);

// Outcome fields shared by the local and the console tool.
void describeAmprRows(const AmprToolRows& rows, JobOutcome& outcome);

} // namespace fw::pfs
