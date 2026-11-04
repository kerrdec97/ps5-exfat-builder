#include "engines/fpkg/Module.h"

#include "engines/fpkg/FpkgSettings.h"
#include "engines/fpkg/Jobs.h"
#include "engines/fpkg/Toolchain.h"

namespace fw::fpkg {

void registerModule(ModuleContext& ctx)
{
    registerFpkgSettings(ctx.settings);
    ctx.dependencies.add(QStringLiteral("sony.pubcmd"), QStringLiteral("Sony Publishing Tools (prospero-pub-cmd)"),
                         QStringLiteral("Builds, verifies and extracts PS5 FPKG packages. You supply your own copy."),
                         probePubCmd);
    ctx.dependencies.add(QStringLiteral("sony.dds2png"), QStringLiteral("prospero-dds2png"),
                         QStringLiteral("Optional: rebuilds sce_sys images from their .dds copies."), probeDds2png);
    registerFpkgJobs(ctx.jobs);
}

} // namespace fw::fpkg
