#pragma once

// What a feature module registers at start-up. Each module under
// src/engines/<name>/ or src/connect/<name>/ exposes
//
//     namespace fw::<name> { void registerModule(fw::ModuleContext& ctx); }
//
// in its Module.h, and src/registration/Registration.cpp calls it. A module
// declares its settings (with their consumers in the same module), its
// dependency probes and its job kinds.

#include "core/Dependencies.h"
#include "core/Settings.h"
#include "jobs/Job.h"

namespace fw {

struct ModuleContext {
    SettingsSchema& settings;
    DependencyRegistry& dependencies;
    JobFactory& jobs;
};

} // namespace fw
