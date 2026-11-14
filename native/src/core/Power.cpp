#include "core/Power.h"

#include "core/Log.h"

#include <QCoreApplication>
#include <QFileInfo>

#include <mutex>

#ifdef Q_OS_WIN
#include <windows.h>
#include <powrprof.h>
#endif

namespace fw::power {

namespace {
bool g_active = false;

std::mutex g_hookMutex;
PerformHook g_hook; // g_hookMutex

// Every test program is called tst_<name> (tests/CMakeLists.txt): none of
// them may put the computer to sleep or shut it down, whatever a test does.
bool isTestProgram()
{
    if (!QCoreApplication::instance())
        return true; // no application to ask: the safe answer
    return QFileInfo(QCoreApplication::applicationFilePath()).fileName().startsWith(QLatin1String("tst_"), Qt::CaseInsensitive);
}
} // namespace

void setPerformHook(PerformHook hook)
{
    std::lock_guard lock(g_hookMutex);
    g_hook = std::move(hook);
}

bool performBlocked()
{
    {
        std::lock_guard lock(g_hookMutex);
        if (g_hook)
            return false;
    }
    return qEnvironmentVariableIsSet("FW_NO_POWER_ACTIONS") || isTestProgram();
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

Action actionFromName(const QString& name)
{
    const QString n = name.trimmed().toLower();
    if (n == u"sleep") return Action::Sleep;
    if (n == u"hibernate") return Action::Hibernate;
    if (n == u"shutdown") return Action::Shutdown;
    if (n == u"restart") return Action::Restart;
    return Action::None;
}

QString actionName(Action action)
{
    switch (action) {
    case Action::None: return QStringLiteral("none");
    case Action::Sleep: return QStringLiteral("sleep");
    case Action::Hibernate: return QStringLiteral("hibernate");
    case Action::Shutdown: return QStringLiteral("shutdown");
    case Action::Restart: return QStringLiteral("restart");
    }
    return QStringLiteral("none");
}

QString actionLabel(Action action)
{
    switch (action) {
    case Action::None: return QStringLiteral("Do nothing");
    case Action::Sleep: return QStringLiteral("Sleep");
    case Action::Hibernate: return QStringLiteral("Hibernate");
    case Action::Shutdown: return QStringLiteral("Shut down");
    case Action::Restart: return QStringLiteral("Restart");
    }
    return QString();
}

#ifdef Q_OS_WIN
static bool enableShutdownPrivilege()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        return false;
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    bool ok = LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid)
        && AdjustTokenPrivileges(token, FALSE, &tp, 0, nullptr, nullptr) && GetLastError() == ERROR_SUCCESS;
    CloseHandle(token);
    return ok;
}
#endif

Status perform(Action action)
{
    if (action == Action::None)
        return success();
    PerformHook hook;
    {
        std::lock_guard lock(g_hookMutex);
        hook = g_hook;
    }
    if (hook)
        return hook(action);
    if (performBlocked()) {
        Logger::instance().log(LogLevel::Warning, QStringLiteral("power"),
                               QStringLiteral("Queue finished: %1 refused (power actions are switched off here)").arg(actionLabel(action)));
        return Error(ErrorCode::PermissionDenied, QStringLiteral("Power actions are switched off in this run of the app."));
    }
    Logger::instance().log(LogLevel::Info, QStringLiteral("power"),
                           QStringLiteral("Queue finished: %1").arg(actionLabel(action)));
    Logger::instance().flush();
#ifdef Q_OS_WIN
    switch (action) {
    case Action::Sleep:
    case Action::Hibernate:
        if (!SetSuspendState(action == Action::Hibernate ? TRUE : FALSE, FALSE, FALSE))
            return Error(ErrorCode::PermissionDenied, QStringLiteral("Windows did not allow the computer to %1.")
                                                          .arg(action == Action::Hibernate ? QStringLiteral("hibernate") : QStringLiteral("sleep")));
        return success();
    case Action::Shutdown:
    case Action::Restart: {
        if (!enableShutdownPrivilege())
            return Error(ErrorCode::PermissionDenied,
                         QStringLiteral("Windows did not allow this app to %1 the computer.")
                             .arg(action == Action::Restart ? QStringLiteral("restart") : QStringLiteral("shut down")));
        const UINT flags = action == Action::Restart ? EWX_REBOOT : (EWX_SHUTDOWN | EWX_POWEROFF);
        if (!ExitWindowsEx(flags, SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_MINOR_OTHER | SHTDN_REASON_FLAG_PLANNED))
            return Error(ErrorCode::PermissionDenied, QStringLiteral("Windows did not allow the computer to %1.")
                                                          .arg(action == Action::Restart ? QStringLiteral("restart") : QStringLiteral("shut down")));
        return success();
    }
    case Action::None: break;
    }
    return success();
#else
    return Error(ErrorCode::Unsupported, QStringLiteral("Power actions are only available on Windows."));
#endif
}

} // namespace fw::power
