#include "engines/backport/Module.h"

#include "engines/backport/ApplyJournal.h"
#include "engines/backport/BackportJobs.h"
#include "engines/backport/BackportSettings.h"

namespace fw::backport {

void registerModule(ModuleContext& ctx)
{
    registerBackportSettings(ctx.settings);
    registerBackportJobs(ctx.jobs);
}

std::vector<PendingJournal> interruptedApplies()
{
    return ApplyJournal::findInterrupted(defaultJournalDir());
}

} // namespace fw::backport
