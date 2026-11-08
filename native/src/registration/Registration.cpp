#include "registration/Registration.h"

#include "connect/notify/Module.h"
#include "connect/ps5/Module.h"
#include "connect/update/Module.h"
#include "engines/ampr/Module.h"
#include "engines/backport/Module.h"
#include "engines/dlc/Module.h"
#include "engines/exfat/Module.h"
#include "engines/fpkg/Module.h"
#include "engines/gameinfo/Module.h"
#include "engines/integration/Module.h"
#include "engines/general/Module.h"
#include "engines/pfs/Module.h"
#include "engines/tools/Module.h"
#include "engines/ufs2/Module.h"

namespace fw {

void registerAllModules(ModuleContext& ctx)
{
    // Order matters only for the order of dependency rows in Settings > Tools.
    general::registerModule(ctx);
    gameinfo::registerModule(ctx);
    exfat::registerModule(ctx);
    pfs::registerModule(ctx);
    ufs2::registerModule(ctx);
    fpkg::registerModule(ctx);
    backport::registerModule(ctx);
    dlc::registerModule(ctx);
    ampr::registerModule(ctx);
    ps5::registerModule(ctx);
    update::registerModule(ctx);
    notify::registerModule(ctx);
    tools::registerModule(ctx);
    integration::registerModule(ctx); // last: wires engines together
}

QStringList registeredModuleNames()
{
    return {QStringLiteral("general"), QStringLiteral("gameinfo"), QStringLiteral("exfat"), QStringLiteral("pfs"),
            QStringLiteral("ufs2"),    QStringLiteral("fpkg"),     QStringLiteral("backport"), QStringLiteral("dlc"), QStringLiteral("ampr"), QStringLiteral("ps5"), QStringLiteral("update"), QStringLiteral("notify"), QStringLiteral("tools"), QStringLiteral("integration")};
}

} // namespace fw
