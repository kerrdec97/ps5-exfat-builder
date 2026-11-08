#pragma once

// Runs external tools (user-supplied Sony Publishing Tools, UFS2Tool, ...)
// from a worker thread with line-by-line output, timeouts and cancellation that
// stops the whole process tree (Windows job object + taskkill fallback; POSIX
// process group). Output from Windows console tools is decoded with the OEM
// code page (audit M10).

#include "core/Cancellation.h"
#include "core/Result.h"

#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

#include <chrono>
#include <functional>

namespace fw {

enum class OutputEncoding { Utf8, OemCodePage, AnsiCodePage };

struct ProcessSpec {
    QString program;              // absolute path preferred
    QStringList arguments;
    QString workingDirectory;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    std::chrono::milliseconds timeout{0}; // 0 = none
    bool mergeStderr = true;
    OutputEncoding encoding = OutputEncoding::Utf8;
    bool keepOutput = true;       // collect output into ProcessResult
    qint64 maxKeptOutputChars = 4 * 1024 * 1024;
    // Raw bytes as they arrive, before line splitting (tools that draw
    // progress bars without newlines). Called on the running thread.
    std::function<void(const QByteArray& chunk, bool isStderr)> onRawOutput;
    // Called once right after the process started (and joined the kill-tree
    // job object), with its process id — e.g. to apply a CPU affinity limit.
    std::function<void(qint64 pid)> onStarted;
};

struct ProcessResult {
    int exitCode = -1;
    bool crashed = false;
    bool timedOut = false;
    bool cancelled = false;
    QString output;      // stdout (+stderr when merged), possibly truncated
    QString errorOutput; // stderr when not merged
    qint64 elapsedMs = 0;

    bool succeeded() const { return !crashed && !timedOut && !cancelled && exitCode == 0; }
};

using LineCallback = std::function<void(const QString& line, bool isStderr)>;

// Blocking: run from a worker thread, never from the UI thread.
// Returns an Error only when the process could not be started; a non-zero exit
// code is reported in ProcessResult and is the caller's decision.
Result<ProcessResult> runProcess(const ProcessSpec& spec, const CancellationToken& token,
                                 const LineCallback& onLine = {});

// Decodes console bytes with the given encoding.
QString decodeConsoleOutput(const QByteArray& bytes, OutputEncoding encoding);

// Finds an executable on PATH (and, on Windows, with .exe appended).
QString findExecutable(const QString& name, const QStringList& extraDirs = {});

// Quotes a command line for logs only (never for execution).
QString describeCommandLine(const ProcessSpec& spec);

// Restricts a running process to the first `cores` logical processors
// (0 = all). Processes it starts afterwards inherit the limit. Can be called
// again while it runs. Returns false if the limit could not be applied.
bool setProcessCpuLimit(qint64 pid, int cores);

// The ppr-pkg Companion's performance modes for external tools (Sony's
// publisher, fpkg-cli): 0 = low impact (idle CPU, low disk, low memory
// priority), 1 = balanced (below-normal CPU, low disk, below-normal memory),
// 2 = maximum (normal everywhere). Returns a short description of what was
// applied ("Below Normal") or "" when nothing could be applied. Windows only.
QString applyProcessPriority(qint64 pid, int mode);
QString processPriorityName(int mode); // "Idle" / "Below Normal" / "Normal"

} // namespace fw
