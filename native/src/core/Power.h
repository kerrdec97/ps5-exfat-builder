#pragma once

// Keeps the computer from sleeping while jobs run (not the display).
// Windows: SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED) from the
// calling (UI) thread; other platforms: no-op that reports false.

#include "core/Result.h"

#include <QString>

#include <functional>

namespace fw::power {

// Returns true if the request is active after the call.
bool setKeepAwake(bool keepAwake);
bool isKeepingAwake();

enum class Action { None, Sleep, Hibernate, Shutdown, Restart };

Action actionFromName(const QString& name); // "none", "sleep", "hibernate", "shutdown", "restart"
QString actionName(Action action);
QString actionLabel(Action action);         // "Shut down", ...

// Performs the action now (Windows). Elsewhere returns Unsupported.
// A test never reaches Windows: while a hook is set perform() returns what
// the hook returns and does nothing else; without one it refuses (PermissionDenied)
// in a test program (an executable named tst_*) and whenever the environment
// variable FW_NO_POWER_ACTIONS is set (smoke runs of the real app).
Status perform(Action action);

using PerformHook = std::function<Status(Action)>;
void setPerformHook(PerformHook hook); // an empty hook puts perform() back to normal
// No hook is set and this is a test program or FW_NO_POWER_ACTIONS is set:
// perform() refuses.
bool performBlocked();

} // namespace fw::power

#include <QMetaType>
Q_DECLARE_METATYPE(fw::power::Action)
