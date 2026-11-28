#include "core/Power.h"

#include "core/Log.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QPointer>
#include <QProcess>
#include <QStandardPaths>

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
    // Linux: systemd-inhibit holds a sleep lock for as long as its child runs;
    // the child (tail --pid) ends with this app, so a crash cannot leave the
    // lock behind. macOS: caffeinate -i (no idle sleep) -w <pid> does the same.
    static QPointer<QProcess> holder;
    if (!keepAwake) {
        if (holder) {
            holder->kill();
            holder->waitForFinished(2000);
            delete holder;
        }
        g_active = false;
    } else {
        const QString pid = QString::number(QCoreApplication::applicationPid());
        QString program;
        QStringList args;
#ifdef Q_OS_MACOS
        program = QStringLiteral("/usr/bin/caffeinate");
        args = {QStringLiteral("-i"), QStringLiteral("-w"), pid};
#else
        program = QStandardPaths::findExecutable(QStringLiteral("systemd-inhibit"));
        args = {QStringLiteral("--what=sleep:idle"), QStringLiteral("--who=exFAT Image Builder"),
                QStringLiteral("--why=Jobs are running"), QStringLiteral("--mode=block"),
                QStringLiteral("tail"), QStringLiteral("--pid=") + pid, QStringLiteral("-f"), QStringLiteral("/dev/null")};
#endif
        if (program.isEmpty() || !QFileInfo(program).isExecutable() || !QCoreApplication::instance()) {
            Logger::instance().log(LogLevel::Warning, QStringLiteral("power"),
                                   QStringLiteral("Could not keep the computer awake: %1 is not available")
                                       .arg(program.isEmpty() ? QStringLiteral("systemd-inhibit") : program));
            return g_active;
        }
        auto* p = new QProcess(QCoreApplication::instance());
        p->setProgram(program);
        p->setArguments(args);
        p->setStandardInputFile(QProcess::nullDevice());
        p->setStandardOutputFile(QProcess::nullDevice()); // stderr stays readable for the refusal reason
        p->start();
        // A lock holder that ends at once was refused (no logind session, as in
        // a container or WSL): it does not count.
        if (!p->waitForStarted(3000) || p->waitForFinished(300)) {
            Logger::instance().log(LogLevel::Warning, QStringLiteral("power"),
                                   QStringLiteral("Could not keep the computer awake: %1 %2")
                                       .arg(program, p->state() == QProcess::NotRunning && p->error() != QProcess::FailedToStart
                                                         ? QStringLiteral("ended at once (%1)").arg(QString::fromUtf8(p->readAllStandardError()).trimmed())
                                                         : QStringLiteral("did not start")));
            delete p;
            return g_active;
        }
        holder = p;
        g_active = true;
    }
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
    if (!isSupported(action))
        return Error(ErrorCode::Unsupported, QStringLiteral("%1 is not available on this computer.").arg(actionLabel(action)));
    const auto [program, args] = commandFor(action);
    QProcess p;
    p.setProgram(program);
    p.setArguments(args);
    p.setStandardInputFile(QProcess::nullDevice());
    p.start();
    if (!p.waitForStarted(5000))
        return Error(ErrorCode::NotFound, QStringLiteral("Could not start %1.").arg(program));
    if (!p.waitForFinished(60000) || p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) {
        const QString why = QString::fromUtf8(p.readAllStandardError()).trimmed();
        return Error(ErrorCode::PermissionDenied, QStringLiteral("The system did not allow the computer to %1%2")
                                                      .arg(actionVerb(action), why.isEmpty() ? QStringLiteral(".") : QStringLiteral(": ") + why));
    }
    return success();
#endif
}

bool isSupported(Action action)
{
#ifdef Q_OS_MACOS
    return action != Action::Hibernate; // macOS has no hibernate on request (pmset hibernatemode is a system setting)
#else
    Q_UNUSED(action);
    return true;
#endif
}

#ifndef Q_OS_WIN
QString actionVerb(Action action)
{
    switch (action) {
    case Action::Sleep: return QStringLiteral("sleep");
    case Action::Hibernate: return QStringLiteral("hibernate");
    case Action::Shutdown: return QStringLiteral("shut down");
    case Action::Restart: return QStringLiteral("restart");
    case Action::None: break;
    }
    return QString();
}

std::pair<QString, QStringList> commandFor(Action action)
{
#ifdef Q_OS_MACOS
    // pmset needs no administrator rights for sleepnow; shut down and restart go
    // through System Events like the Apple menu (macOS may ask once to allow it).
    switch (action) {
    case Action::Sleep: return {QStringLiteral("/usr/bin/pmset"), {QStringLiteral("sleepnow")}};
    case Action::Shutdown:
        return {QStringLiteral("/usr/bin/osascript"), {QStringLiteral("-e"), QStringLiteral("tell application \"System Events\" to shut down")}};
    case Action::Restart:
        return {QStringLiteral("/usr/bin/osascript"), {QStringLiteral("-e"), QStringLiteral("tell application \"System Events\" to restart")}};
    case Action::Hibernate:
    case Action::None: break;
    }
    return {};
#else
    // systemd-logind lets the user of the active local session do these
    // without a password (polkit); otherwise systemctl says why not.
    switch (action) {
    case Action::Sleep: return {QStringLiteral("systemctl"), {QStringLiteral("suspend")}};
    case Action::Hibernate: return {QStringLiteral("systemctl"), {QStringLiteral("hibernate")}};
    case Action::Shutdown: return {QStringLiteral("systemctl"), {QStringLiteral("poweroff")}};
    case Action::Restart: return {QStringLiteral("systemctl"), {QStringLiteral("reboot")}};
    case Action::None: break;
    }
    return {};
#endif
}
#endif

} // namespace fw::power
