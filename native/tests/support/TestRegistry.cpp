#include "support/TestRegistry.h"

#include "registration/Registration.h"

namespace fwtest {

Registry::Registry()
{
    fw::ModuleContext ctx{*schema, deps, jobs};
    fw::registerAllModules(ctx);
}

} // namespace fwtest
