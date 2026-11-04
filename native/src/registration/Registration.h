#pragma once

#include "jobs/Module.h"

namespace fw {

// Registers every module's settings, dependency probes and job kinds.
void registerAllModules(ModuleContext& ctx);

// Names of the registered modules, for About and diagnostics.
QStringList registeredModuleNames();

} // namespace fw
