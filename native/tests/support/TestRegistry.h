#pragma once

// Full registry (every module) for app-level tests. Module tests should build
// their own ModuleContext and call only their module's registerModule().

#include "core/Dependencies.h"
#include "core/Settings.h"
#include "jobs/Job.h"

#include <memory>

namespace fwtest {

struct Registry {
    std::shared_ptr<fw::SettingsSchema> schema = std::make_shared<fw::SettingsSchema>();
    fw::DependencyRegistry deps;
    fw::JobFactory jobs;
    Registry();
};

} // namespace fwtest
