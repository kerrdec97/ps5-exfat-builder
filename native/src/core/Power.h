#pragma once

// Keeps the computer from sleeping while jobs run (not the display).
// Windows: SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED) from the
// calling (UI) thread; other platforms: no-op that reports false.

namespace fw::power {

// Returns true if the request is active after the call.
bool setKeepAwake(bool keepAwake);
bool isKeepingAwake();

} // namespace fw::power
