#pragma once

#include "engines/backport/ApplyJournal.h"
#include "jobs/Module.h"

#include <vector>

namespace fw::backport {

// Registers the backport settings (sections "Backport" and "Firmware and
// SDK"), and the job kinds backport.run, backport.restore and backport.scan.
// The module has no external tool, so it registers no dependency probe.
void registerModule(ModuleContext& ctx);

// Apply journals left pending by an interrupted backport (or a failed
// rollback). The app offers "Roll back the interrupted backport of <game>"
// for each at start-up by queueing backport.restore {journal: path}.
std::vector<PendingJournal> interruptedApplies();

} // namespace fw::backport
