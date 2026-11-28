#pragma once

// Keeps the computer from sleeping while jobs run (not the display), from the
// calling (UI) thread. Windows: SetThreadExecutionState(ES_CONTINUOUS |
// ES_SYSTEM_REQUIRED). Linux: a systemd-inhibit sleep lock. macOS: caffeinate -i.
// Reports false when the platform tool is missing.

#include "core/Result.h"

#include <QString>

#include <QStringList>

#include <functional>
#include <utility>

namespace fw::power {

// Returns true if the request is active after the call.
bool setKeepAwake(bool keepAwake);
bool isKeepingAwake();

enum class Action { None, Sleep, Hibernate, Shutdown, Restart };

Action actionFromName(const QString& name); // "none", "sleep", "hibernate", "shutdown", "restart"
QString actionName(Action action);
QString actionLabel(Action action);         // "Shut down", ...

// Performs the action now. Windows: the system calls. Linux: systemctl
// suspend / hibernate / poweroff / reboot. macOS: pmset sleepnow, or System
// Events for shut down and restart; Hibernate is Unsupported there.
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

// False for an action this platform cannot perform (Hibernate on macOS).
bool isSupported(Action action);

#ifndef Q_OS_WIN
// The program and arguments perform() runs for `action` (tests read them).
std::pair<QString, QStringList> commandFor(Action action);
QString actionVerb(Action action); // "sleep", "shut down", ...
#endif

} // namespace fw::power

#include <QMetaType>
Q_DECLARE_METATYPE(fw::power::Action)
