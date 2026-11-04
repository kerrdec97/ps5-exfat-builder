#pragma once

#include "jobs/Module.h"

namespace fw::ufs2 {

// Registers this module's settings, dependency probes and job kinds.
void registerModule(ModuleContext& ctx);

} // namespace fw::ufs2
