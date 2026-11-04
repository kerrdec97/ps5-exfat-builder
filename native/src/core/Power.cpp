#include "core/Power.h"

#include "core/Log.h"

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace fw::power {

namespace {
bool g_active = false;
}

bool setKeepAwake(bool keepAwake)
{
    if (keepAwake == g_active)
        return g_active;
#ifdef Q_OS_WIN
    const EXECUTION_STATE flags = keepAwake ? (ES_CONTINUOUS | ES_SYSTEM_REQUIRED) : ES_CONTINUOUS;
    if (SetThreadExecutionState(flags) == 0) {
        Logger::instance().log(LogLevel::Warning, QStringLiteral("power"),
                               QStringLiteral("Could not change the sleep setting"));
        return g_active;
    }
    g_active = keepAwake;
#else
    g_active = false;
    Q_UNUSED(keepAwake);
    return false;
#endif
    Logger::instance().log(LogLevel::Debug, QStringLiteral("power"),
                           g_active ? QStringLiteral("Keeping the computer awake while jobs run")
                                    : QStringLiteral("Sleep allowed again"));
    return g_active;
}

bool isKeepingAwake()
{
    return g_active;
}

} // namespace fw::power
