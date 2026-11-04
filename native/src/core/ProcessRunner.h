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

} // namespace fw
