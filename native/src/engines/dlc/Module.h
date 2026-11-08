#pragma once

#include "jobs/Module.h"

namespace fw::dlc {

// Registers the dlc.* settings, the dlc.fpkgcli dependency probe (Settings >
// Tools) and the dlc.convert job kind (see DlcConvert.h).
void registerModule(ModuleContext& ctx);

} // namespace fw::dlc
