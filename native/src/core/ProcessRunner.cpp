#include "core/ProcessRunner.h"

#include "core/Log.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>

#include <algorithm>
#include <atomic>
#include <vector>

#ifdef Q_OS_WIN
#include <windows.h>
#include <tlhelp32.h>
#else
#include <csignal>
#include <sched.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include <QThread>

namespace fw {

QString decodeConsoleOutput(const QByteArray& bytes, OutputEncoding encoding)
{
#ifdef Q_OS_WIN
    if (encoding != OutputEncoding::Utf8 && !bytes.isEmpty()) {
        const UINT cp = encoding == OutputEncoding::OemCodePage ? GetOEMCP() : GetACP();
        const int n = MultiByteToWideChar(cp, 0, bytes.constData(), int(bytes.size()), nullptr, 0);
        if (n > 0) {
            std::wstring w(size_t(n), L'\0');
            MultiByteToWideChar(cp, 0, bytes.constData(), int(bytes.size()), w.data(), n);
            return QString::fromStdWString(w);
        }
    }
#else
    Q_UNUSED(encoding);
#endif
    return QString::fromUtf8(bytes);
}

QString findExecutable(const QString& name, const QStringList& extraDirs)
{
    if (QFileInfo(name).isAbsolute())
        return QFileInfo(name).isFile() ? name : QString(); // a folder of that name is no program
    QString found;
    if (!extraDirs.isEmpty())
        found = QStandardPaths::findExecutable(name, extraDirs);
    if (found.isEmpty())
        found = QStandardPaths::findExecutable(name);
    return found;
}

bool setProcessCpuLimit(qint64 pid, int cores)
{
    if (pid <= 0)
        return false;
    const int total = std::max(1, QThread::idealThreadCount());
    const int n = (cores <= 0 || cores >= total) ? total : cores;
#ifdef Q_OS_WIN
    HANDLE h = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid));
    if (!h)
        return false;
    DWORD_PTR processMask = 0, systemMask = 0;
    BOOL ok = GetProcessAffinityMask(h, &processMask, &systemMask);
    if (ok) {
        DWORD_PTR mask = 0;
        int picked = 0;
        for (int bit = 0; bit < int(sizeof(DWORD_PTR) * 8) && picked < n; ++bit) {
            const DWORD_PTR b = DWORD_PTR(1) << bit;
            if (systemMask & b) {
                mask |= b;
                ++picked;
            }
        }
        ok = mask != 0 && SetProcessAffinityMask(h, mask);
    }
    CloseHandle(h);
    return ok != FALSE;
#else
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int i = 0; i < n && i < CPU_SETSIZE; ++i)
        CPU_SET(i, &set);
    return sched_setaffinity(pid_t(pid), sizeof(set), &set) == 0;
#endif
}

QString processPriorityName(int mode)
{
    switch (mode) {
    case 0: return QStringLiteral("Idle");
    case 1: return QStringLiteral("Below Normal");
    default: break;
    }
    return QStringLiteral("Normal");
}

QString applyProcessPriority(qint64 pid, int mode)
{
    if (pid <= 0 || mode < 0)
        return {};
#ifdef Q_OS_WIN
    HANDLE h = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid));
    if (!h)
        return {};
    const DWORD cls = mode == 0 ? IDLE_PRIORITY_CLASS : mode == 1 ? BELOW_NORMAL_PRIORITY_CLASS : NORMAL_PRIORITY_CLASS;
    const bool cpu = SetPriorityClass(h, cls) != FALSE;
    // I/O priority (ProcessIoPriority = 33): 1 = low, 2 = normal.
    using NtSetInformationProcessFn = LONG(WINAPI*)(HANDLE, ULONG, PVOID, ULONG);
    if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
        if (auto fn = reinterpret_cast<NtSetInformationProcessFn>(
                reinterpret_cast<void*>(GetProcAddress(ntdll, "NtSetInformationProcess")))) {
            ULONG io = mode >= 2 ? 2 : 1;
            (void)fn(h, 33, &io, sizeof(io));
        }
    }
    // Memory priority: 2 = low, 4 = below normal, 5 = normal.
    MEMORY_PRIORITY_INFORMATION mp{};
    mp.MemoryPriority = mode == 0 ? MEMORY_PRIORITY_LOW : mode == 1 ? MEMORY_PRIORITY_BELOW_NORMAL : MEMORY_PRIORITY_NORMAL;
    (void)SetProcessInformation(h, ProcessMemoryPriority, &mp, sizeof(mp));
    CloseHandle(h);
    return cpu ? processPriorityName(mode) : QString();
#else
    Q_UNUSED(mode);
    return {};
#endif
}

QString describeCommandLine(const ProcessSpec& spec)
{
    QStringList parts;
    const auto quote = [](const QString& s) {
        return (s.contains(u' ') || s.isEmpty()) ? QStringLiteral("\"%1\"").arg(s) : s;
    };
    parts << quote(QDir::toNativeSeparators(spec.program));
    // Secrets stay out of logs and problem reports: the value after a
    // --passcode (Sony's publisher, fpkg-cli) is masked.
    bool maskNext = false;
    for (const auto& a : spec.arguments) {
        if (maskNext) {
            parts << QStringLiteral("********");
            maskNext = false;
        } else if (a.compare(QLatin1String("--passcode"), Qt::CaseInsensitive) == 0) {
            parts << a;
            maskNext = true;
        } else if (a.startsWith(QLatin1String("--passcode="), Qt::CaseInsensitive)) {
            parts << QStringLiteral("--passcode=********");
        } else {
            parts << quote(a);
        }
    }
    return parts.join(u' ');
}

namespace {

// Splits a byte stream into lines on \n or \r (progress bars use \r).
class LineSplitter {
public:
    template <typename Fn>
    void feed(const QByteArray& data, Fn&& onLine)
    {
        m_buf += data;
        qsizetype start = 0;
        for (qsizetype i = 0; i < m_buf.size(); ++i) {
            const char c = m_buf[i];
            if (c == '\n' || c == '\r') {
                if (i > start)
                    onLine(m_buf.mid(start, i - start));
                start = i + 1;
            }
        }
        m_buf.remove(0, start);
        if (m_buf.size() > 1024 * 1024) { // pathological: no newline for 1 MiB
            onLine(m_buf);
            m_buf.clear();
        }
    }
    template <typename Fn>
    void finish(Fn&& onLine)
    {
        if (!m_buf.isEmpty())
            onLine(m_buf);
        m_buf.clear();
    }

private:
    QByteArray m_buf;
};

#ifdef Q_OS_WIN
struct JobHandle {
    HANDLE job = nullptr;
    JobHandle()
    {
        job = CreateJobObjectW(nullptr, nullptr);
        if (job) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
            info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info));
        }
    }
    ~JobHandle()
    {
        if (job)
            CloseHandle(job);
    }
    bool assign(qint64 pid)
    {
        if (!job)
            return false;
        HANDLE h = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, DWORD(pid));
        if (!h)
            return false;
        const BOOL ok = AssignProcessToJobObject(job, h);
        CloseHandle(h);
        return ok != FALSE;
    }
    void terminate()
    {
        if (job)
            TerminateJobObject(job, 1);
    }
};

// Holds a tool and everything it started still while its job is paused:
// every process of the job object (or only the tool when it could not join
// one) is suspended as a whole, and resumed when the pause ends. The handles
// stay open while suspended, so a process id cannot pass to another program
// before it is resumed. Whatever is still suspended is resumed on destruction.
class TreeSuspender {
public:
    TreeSuspender(HANDLE job, qint64 onlyPid) : m_job(job), m_onlyPid(DWORD(onlyPid)) {}
    TreeSuspender(const TreeSuspender&) = delete;
    TreeSuspender& operator=(const TreeSuspender&) = delete;
    ~TreeSuspender() { resume(); }

    bool isSuspended() const { return !m_held.empty(); }

    // Several rounds: a process that was starting a child when it was
    // suspended may have added that child to the job in between.
    bool suspend()
    {
        const Ntdll& nt = ntdll();
        if (!nt.suspend || !nt.resume)
            return false;
        for (int round = 0; round < 8; ++round) {
            bool added = false;
            for (const DWORD id : members()) {
                if (std::find(m_ids.cbegin(), m_ids.cend(), id) != m_ids.cend())
                    continue;
                const HANDLE h = OpenProcess(PROCESS_SUSPEND_RESUME | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, id);
                if (!h)
                    continue;
                BOOL inJob = m_job == nullptr;
                if (m_job && !IsProcessInJob(h, m_job, &inJob))
                    inJob = FALSE;
                if (inJob && nt.suspend(h) >= 0) {
                    m_held.push_back(h);
                    m_ids.push_back(id);
                    added = true;
                } else {
                    CloseHandle(h);
                }
            }
            if (!added)
                break;
        }
        return isSuspended();
    }

    void resume()
    {
        const Ntdll& nt = ntdll();
        for (const HANDLE h : m_held) {
            if (nt.resume)
                (void)nt.resume(h);
            CloseHandle(h);
        }
        m_held.clear();
        m_ids.clear();
    }

private:
    using NtProcessFn = LONG(NTAPI*)(HANDLE);
    struct Ntdll {
        NtProcessFn suspend = nullptr;
        NtProcessFn resume = nullptr;
    };
    static const Ntdll& ntdll()
    {
        static const Ntdll fns = [] {
            Ntdll n;
            if (HMODULE m = GetModuleHandleW(L"ntdll.dll")) {
                n.suspend = reinterpret_cast<NtProcessFn>(reinterpret_cast<void*>(GetProcAddress(m, "NtSuspendProcess")));
                n.resume = reinterpret_cast<NtProcessFn>(reinterpret_cast<void*>(GetProcAddress(m, "NtResumeProcess")));
            }
            return n;
        }();
        return fns;
    }

    std::vector<DWORD> members() const
    {
        if (!m_job)
            return {m_onlyPid};
        // The list: two counts, then one ULONG_PTR per process id.
        std::vector<ULONG_PTR> buf(2 + 256);
        for (int attempt = 0; attempt < 4; ++attempt) {
            auto* list = reinterpret_cast<JOBOBJECT_BASIC_PROCESS_ID_LIST*>(buf.data());
            const DWORD bytes = DWORD(buf.size() * sizeof(ULONG_PTR));
            const bool ok = QueryInformationJobObject(m_job, JobObjectBasicProcessIdList, list, bytes, nullptr) != FALSE;
            if (!ok && GetLastError() != ERROR_MORE_DATA)
                return {};
            if (ok || attempt == 3) {
                std::vector<DWORD> ids;
                const DWORD n = std::min<DWORD>(list->NumberOfProcessIdsInList, DWORD(buf.size() - 2));
                for (DWORD i = 0; i < n; ++i)
                    ids.push_back(DWORD(list->ProcessIdList[i]));
                return ids;
            }
            buf.resize(2 + size_t(list->NumberOfAssignedProcesses) + 64);
        }
        return {};
    }

    HANDLE m_job = nullptr;
    DWORD m_onlyPid = 0;
    std::vector<HANDLE> m_held;
    std::vector<DWORD> m_ids;
};

// Resumes every thread of a process started with CREATE_SUSPENDED.
bool resumeProcessThreads(qint64 pid)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return false;
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    bool resumed = false;
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID == DWORD(pid)) {
                HANDLE t = OpenThread(THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
                if (t) {
                    if (ResumeThread(t) != DWORD(-1))
                        resumed = true;
                    CloseHandle(t);
                }
            }
            te.dwSize = sizeof(te);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    return resumed;
}

void taskkillTree(qint64 pid)
{
    // Fallback for children that escaped the job object. By its full path: a
    // bare "taskkill" would be looked up in the app folder and the current
    // folder before System32.
    wchar_t system[MAX_PATH] = {};
    const UINT n = GetSystemDirectoryW(system, MAX_PATH);
    const QString taskkill = n > 0 && n < MAX_PATH ? QDir(QString::fromWCharArray(system, int(n))).filePath(QStringLiteral("taskkill.exe"))
                                                   : QStringLiteral("taskkill");
    QProcess::execute(taskkill, {QStringLiteral("/PID"), QString::number(pid), QStringLiteral("/T"), QStringLiteral("/F")});
}
#endif

} // namespace

Result<ProcessResult> runProcess(const ProcessSpec& spec, const CancellationToken& token, const LineCallback& onLine)
{
    if (spec.program.isEmpty())
        return Error(ErrorCode::InvalidArgument, QStringLiteral("No program to run."));
    if (token.isCancelled()) {
        ProcessResult r;
        r.cancelled = true;
        return r;
    }

    Log log(QStringLiteral("process"));
    log.debug(QStringLiteral("Starting: %1").arg(describeCommandLine(spec)));

    QProcess proc;
    proc.setProgram(spec.program);
    proc.setArguments(spec.arguments);
    if (!spec.workingDirectory.isEmpty())
        proc.setWorkingDirectory(spec.workingDirectory);
    proc.setProcessEnvironment(spec.environment);
    proc.setProcessChannelMode(spec.mergeStderr ? QProcess::MergedChannels : QProcess::SeparateChannels);
    proc.setInputChannelMode(QProcess::ManagedInputChannel);
#ifdef Q_OS_WIN
    // Started suspended: it joins the kill-on-close job object (and gets its
    // CPU limit) before it can run a single instruction or spawn a child.
    proc.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) {
        args->flags |= CREATE_NO_WINDOW | CREATE_SUSPENDED;
    });
    JobHandle jobObject;
#else
    proc.setChildProcessModifier([] { ::setpgid(0, 0); });
#endif

    QElapsedTimer timer;
    timer.start();
    proc.start();
    if (!proc.waitForStarted(30000)) {
        return Error(ErrorCode::ExternalToolFailed,
                     QStringLiteral("Could not start %1.").arg(QFileInfo(spec.program).fileName()),
                     proc.errorString());
    }
    proc.closeWriteChannel();
    const qint64 pid = proc.processId();

#ifdef Q_OS_WIN
    const bool inJobObject = jobObject.assign(pid);
    if (!inJobObject)
        log.warn(QStringLiteral("Could not attach %1 to the stop-on-close group; taskkill is the fallback.")
                     .arg(QFileInfo(spec.program).fileName()));
#endif
    if (spec.onStarted)
        spec.onStarted(pid);
#ifdef Q_OS_WIN
    if (!resumeProcessThreads(pid)) {
        proc.kill();
        proc.waitForFinished(5000);
        return Error(ErrorCode::ExternalToolFailed,
                     QStringLiteral("Could not start %1.").arg(QFileInfo(spec.program).fileName()),
                     QStringLiteral("the suspended process could not be resumed"));
    }
#endif

    std::atomic<bool> cancelRequested{false};
    auto killTree = [&, pid] {
#ifdef Q_OS_WIN
        jobObject.terminate();
        // taskkill only as the fallback, and only while the process is still
        // ours: once it has exited its PID can belong to someone else.
        if (!inJobObject && proc.state() != QProcess::NotRunning)
            taskkillTree(pid);
#else
        if (pid > 0) {
            ::kill(-pid_t(pid), SIGTERM);
            ::kill(pid_t(pid), SIGTERM);
            ::kill(-pid_t(pid), SIGCONT); // a stopped (paused) process only acts on SIGTERM once it runs
        }
#endif
    };
    CancelRegistration reg = token.onCancel([&] {
        cancelRequested = true;
#ifdef Q_OS_WIN
        jobObject.terminate();
#else
        if (pid > 0) {
            ::kill(-pid_t(pid), SIGTERM);
            ::kill(-pid_t(pid), SIGCONT);
        }
#endif
    });

    // A paused job holds its tool and everything the tool started; the
    // timeout counts only the time it may run.
#ifdef Q_OS_WIN
    TreeSuspender suspender(inJobObject ? jobObject.job : nullptr, pid);
#endif
    bool held = false;
    bool killed = false;
    bool ended = false;
    qint64 heldMs = 0;
    QElapsedTimer heldFor;
    const auto followPause = [&] {
        bool hold = token.isPaused();
        if (ended)
            hold = false;
        else if (killed || cancelRequested.load())
            hold = held; // being ended: a held tool is ended as it is, never run again first
        if (hold == held)
            return;
        if (hold) {
#ifdef Q_OS_WIN
            if (!suspender.suspend())
                log.warn(QStringLiteral("Could not pause %1; it keeps running while the job is paused.")
                             .arg(QFileInfo(spec.program).fileName()));
#else
            if (pid > 0)
                ::kill(-pid_t(pid), SIGSTOP);
#endif
            heldFor.start();
        } else {
#ifdef Q_OS_WIN
            suspender.resume();
#else
            if (pid > 0)
                ::kill(-pid_t(pid), SIGCONT);
#endif
            heldMs += heldFor.elapsed();
        }
        held = hold;
    };
    const auto runningMs = [&] { return timer.elapsed() - heldMs - (held ? heldFor.elapsed() : 0); };

    ProcessResult result;
    LineSplitter outSplit, errSplit;
    const auto keep = [&](QString& target, const QString& line) {
        if (!spec.keepOutput || target.size() > spec.maxKeptOutputChars)
            return;
        target += line;
        target += u'\n';
    };
    // Before each line: a callback that waits in checkpoint() during a pause
    // must find the tool already held.
    const auto handleOut = [&](const QByteArray& bytes) {
        const QString line = decodeConsoleOutput(bytes, spec.encoding);
        keep(result.output, line);
        if (onLine) {
            followPause();
            onLine(line, false);
        }
    };
    const auto handleErr = [&](const QByteArray& bytes) {
        const QString line = decodeConsoleOutput(bytes, spec.encoding);
        keep(result.errorOutput, line);
        if (onLine) {
            followPause();
            onLine(line, true);
        }
    };

    while (proc.state() != QProcess::NotRunning) {
        followPause();
        proc.waitForReadyRead(100);
        {
            const QByteArray out = proc.readAllStandardOutput();
            if (spec.onRawOutput && !out.isEmpty())
                spec.onRawOutput(out, false);
            outSplit.feed(out, handleOut);
        }
        if (!spec.mergeStderr) {
            const QByteArray err = proc.readAllStandardError();
            if (spec.onRawOutput && !err.isEmpty())
                spec.onRawOutput(err, true);
            errSplit.feed(err, handleErr);
        }

        if (!killed && cancelRequested.load()) {
            killed = true;
            result.cancelled = true;
            killTree();
            proc.kill();
        }
        if (!killed && spec.timeout.count() > 0 && runningMs() > spec.timeout.count()) {
            killed = true;
            result.timedOut = true;
            log.warn(QStringLiteral("Timed out after %1 s: %2")
                         .arg(spec.timeout.count() / 1000)
                         .arg(QFileInfo(spec.program).fileName()));
            killTree();
            proc.kill();
        }
        if (killed)
            proc.waitForFinished(5000);
    }
    // Whatever of the tree is still held (the tool ended as it was being
    // paused) runs on and ends with the job object.
    ended = true;
    followPause();
    {
        const QByteArray out = proc.readAllStandardOutput();
        if (spec.onRawOutput && !out.isEmpty())
            spec.onRawOutput(out, false);
        outSplit.feed(out, handleOut);
        outSplit.finish(handleOut);
    }
    if (!spec.mergeStderr) {
        const QByteArray err = proc.readAllStandardError();
        if (spec.onRawOutput && !err.isEmpty())
            spec.onRawOutput(err, true);
        errSplit.feed(err, handleErr);
        errSplit.finish(handleErr);
    }

    result.elapsedMs = timer.elapsed();
    result.crashed = !killed && proc.exitStatus() == QProcess::CrashExit;
    result.exitCode = proc.exitCode();
    if (cancelRequested.load()) {
        // One outcome: a cancel that arrived while the timeout was killing the
        // process is still a cancel, never "timed out" as well.
        result.cancelled = true;
        result.timedOut = false;
    }
    log.debug(QStringLiteral("Finished %1: exit %2%3 in %4 ms")
                  .arg(QFileInfo(spec.program).fileName())
                  .arg(result.exitCode)
                  .arg(result.cancelled ? QStringLiteral(" (cancelled)")
                       : result.timedOut ? QStringLiteral(" (timed out)")
                       : result.crashed  ? QStringLiteral(" (crashed)")
                                         : QString())
                  .arg(result.elapsedMs));
    return result;
}

} // namespace fw
