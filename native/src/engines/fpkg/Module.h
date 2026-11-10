#pragma once

#include "jobs/Module.h"

namespace fw::fpkg {

// Registers the fpkg.* settings, the sony.pubcmd / sony.dds2png dependency
// probes and the fpkg.build / fpkg.addon / fpkg.backport / fpkg.verify /
// fpkg.extract job kinds (see Jobs.h for their JSON parameters).
//
// Image sources (.exfat, .ffpkg, .ffpfs, .ffpfsc) are plugged in separately: the app
// calls fw::fpkg::registerSourceStager() (engines/fpkg/SourceStager.h) once
// the image modules exist.
void registerModule(ModuleContext& ctx);

} // namespace fw::fpkg
