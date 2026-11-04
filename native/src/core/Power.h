#pragma once

// Keeps the computer from sleeping while jobs run (not the display).
// Windows: SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED) from the
// calling (UI) thread; other platforms: no-op that reports false.

#include "core/Result.h"

#include <QString>

namespace fw::power {

// Returns true if the request is active after the call.
bool setKeepAwake(bool keepAwake);
bool isKeepingAwake();

enum class Action { None, Sleep, Hibernate, Shutdown, Restart };

Action actionFromName(const QString& name); // "none", "sleep", "hibernate", "shutdown", "restart"
QString actionName(Action action);
QString actionLabel(Action action);         // "Shut down", ...

// Performs the action now (Windows). Elsewhere returns Unsupported.
Status perform(Action action);

} // namespace fw::power

#include <QMetaType>
Q_DECLARE_METATYPE(fw::power::Action)
