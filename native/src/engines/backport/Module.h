#pragma once

// Small PS5 executable readers kept from the native backport port (v5.0
// previews): param.json version fields (GameMetadata), the SDK in an ELF's
// process param (SceParam, SdkTable, NidPatch), SELF unwrapping (SelfReader,
// SelfFormat, ElfImage) and the canonical fakelib trio pins (CanonicalTrio).
// The Backport tab uses them for its game list (src/app/backport). Backports
// themselves are made by the bundled PS5 Backport engine, so this module
// registers no settings, dependencies or job kinds.

#include "jobs/Module.h"

namespace fw::backport {

void registerModule(ModuleContext& ctx);

} // namespace fw::backport
