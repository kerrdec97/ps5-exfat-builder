#include "core/ProcessRunner.h"

#include "core/Log.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>

#include <algorithm>
#include <atomic>

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
        return QFileInfo(name).isExecutable() || QFileInfo::exists(name) ? name : QString();
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

QString describeCommandLine(const ProcessSpec& spec)
{
    QStringList parts;
    const auto quote = [](const QString& s) {
        return (s.contains(u' ') || s.isEmpty()) ? QStringLiteral("\"%1\"").arg(s) : s;
    };
    parts << quote(QDir::toNativeSeparators(spec.program));
    for (const auto& a : spec.arguments)
        parts << quote(a);
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
    // Fallback for children that escaped the job object.
    QProcess::execute(QStringLiteral("taskkill"),
                      {QStringLiteral("/PID"), QString::number(pid), QStringLiteral("/T"), QStringLiteral("/F")});
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
        }
#endif
    };
    CancelRegistration reg = token.onCancel([&] {
        cancelRequested = true;
#ifdef Q_OS_WIN
        jobObject.terminate();
#else
        if (pid > 0)
            ::kill(-pid_t(pid), SIGTERM);
#endif
    });

    ProcessResult result;
    LineSplitter outSplit, errSplit;
    const auto keep = [&](QString& target, const QString& line) {
        if (!spec.keepOutput || target.size() > spec.maxKeptOutputChars)
            return;
        target += line;
        target += u'\n';
    };
    const auto handleOut = [&](const QByteArray& bytes) {
        const QString line = decodeConsoleOutput(bytes, spec.encoding);
        keep(result.output, line);
        if (onLine)
            onLine(line, false);
    };
    const auto handleErr = [&](const QByteArray& bytes) {
        const QString line = decodeConsoleOutput(bytes, spec.encoding);
        keep(result.errorOutput, line);
        if (onLine)
            onLine(line, true);
    };

    bool killed = false;
    while (proc.state() != QProcess::NotRunning) {
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
        if (!killed && spec.timeout.count() > 0 && timer.elapsed() > spec.timeout.count()) {
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
    if (cancelRequested.load())
        result.cancelled = true;
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
