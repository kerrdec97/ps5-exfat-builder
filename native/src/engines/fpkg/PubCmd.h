#pragma once

// Running prospero-pub-cmd (and prospero-dds2png) and reading their output.
//
// img_create prints a ruler line starting with "|____", then up to 51 '='
// characters without line breaks while it writes the image. The parser counts
// them from the raw output (one step = 1/51) and classifies every other line:
// "[Error] x" fails the run even with exit code 0; "[Warn] x" is a warning
// unless it is one of the routine notices the tools always print; "[Info]",
// "[Debug]" and "Creating an image" are info; everything else is dropped.
// The size of the file being written is polled every 500 ms as a second
// progress signal, and the CPU-core limit can be changed while it runs.
// While the job's token is paused the tool and every process it started are
// suspended (within 250 ms) and resumed when the job continues.

#include "core/Cancellation.h"
#include "core/Log.h"
#include "core/Result.h"

#include <QByteArray>
#include <QDateTime>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>

namespace fw::fpkg {

inline constexpr int kBarSteps = 51;

// ---- command lines ----------------------------------------------------------------
QStringList imgCreateArgs(const QString& gp5, const QString& output, std::optional<int> compressionLevel,
                          const QString& refPackage = {});
QStringList imgVerifyArgs(const QString& passcode, bool integrity, const QString& package);
// Explicit on/off for both checks (the backport flow runs them as two passes).
QStringList imgVerifyArgs(const QString& passcode, bool formatCheck, bool integrity, const QString& package);
QStringList imgExtractArgs(const QString& passcode, const QString& package, const QString& destination);
QStringList dds2pngArgs(const QString& dds, const QString& png, bool preserveAlpha);

// ---- output classification -----------------------------------------------------------
enum class LineKind { Error, Warning, Info, Dropped };
struct ToolLine {
    LineKind kind = LineKind::Dropped;
    QString text;    // message without the [Error]/[Warn]/[Info] prefix
    QString raw;     // the whole line
};
ToolLine classifyToolLine(const QString& line);
bool isRoutineWarning(const QString& text);
// Plain-language explanations for known Publishing Tools errors (deduplicated).
QStringList explainToolErrors(const QStringList& errors);

class PubCmdOutputParser {
public:
    using LineSink = std::function<void(const ToolLine&)>;
    using BarSink = std::function<void(int steps)>;
    PubCmdOutputParser(LineSink onLine, BarSink onBar, bool withBar = true);
    void feed(const QByteArray& chunk);
    void finish();
    int barSteps() const { return m_steps; }
    bool sawBar() const { return m_sawRuler; }

private:
    void endLine();
    LineSink m_onLine;
    BarSink m_onBar;
    bool m_withBar;
    QByteArray m_line;
    bool m_lineHasText = false;
    bool m_lineHasBar = false;
    bool m_armed = false;
    bool m_sawRuler = false;
    int m_steps = 0;
};

// ---- running ------------------------------------------------------------------------------
struct ToolRunOptions {
    QString program;
    QStringList arguments;
    QString workingDirectory;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    QString watchFile;            // size polled every 500 ms (img_create output)
    std::function<qint64()> pollBytes; // used instead of watchFile when set
    int cpuCoreLimit = 0;         // 0 = no limit
    int priorityMode = -1;        // applyProcessPriority() mode; -1 = leave as started
    QString liveLimitKey;         // job id: setLiveCpuLimit() changes the limit while it runs
    bool withBar = true;
    std::function<void(int steps)> onBar;
    std::function<void(qint64 bytes)> onBytesWritten;
    std::function<void(const ToolLine&)> onLine;
    Log log{QStringLiteral("fpkg")}; // "Paused ..." / "Resumed ..." notices (the job's log when set)
};

struct ToolRunResult {
    int exitCode = -1;
    bool cancelled = false;
    bool crashed = false;
    QStringList errors;     // "[Error]" messages
    QStringList warnings;   // non-routine "[Warn]" messages
    int barSteps = 0;
    bool sawBar = false;
    qint64 elapsedMs = 0;
    QDateTime startedUtc;
    QDateTime finishedUtc;
    QString commandLine;    // for logs

    // exit code 0 and no [Error] line (the caller also checks the output file)
    bool ok() const { return !cancelled && !crashed && exitCode == 0 && errors.isEmpty(); }
    // First three error lines, or "exit code N" and a hint, plus explanations.
    QString failureText() const;
};

// Errors only when the program cannot be started.
Result<ToolRunResult> runPubTool(const ToolRunOptions& options, const CancellationToken& token);

// "hh:mm:ss.fffffff"
QString formatElapsed(qint64 ms);

// ---- CPU-core limit -----------------------------------------------------------------------------
// The UI calls this while a job runs; the publisher run in progress follows
// within 250 ms and every later run of the job starts with it. 0 = all cores.
// clearLiveCpuLimit() is called once, when the job ends.
void setLiveCpuLimit(const QString& jobId, int cores);
std::optional<int> liveCpuLimit(const QString& jobId);
void clearLiveCpuLimit(const QString& jobId);
// For a job's run(): forgets the job's live limit when the job ends, however
// many publisher runs it made.
struct LiveCpuLimitScope {
    explicit LiveCpuLimitScope(QString id) : jobId(std::move(id)) {}
    ~LiveCpuLimitScope() { clearLiveCpuLimit(jobId); }
    LiveCpuLimitScope(const LiveCpuLimitScope&) = delete;
    LiveCpuLimitScope& operator=(const LiveCpuLimitScope&) = delete;
    QString jobId;
};

// Tests: observe (or replace) how the limit is applied. Default: fw::setProcessCpuLimit.
using CpuLimitApplier = std::function<bool(qint64 pid, int cores)>;
void setCpuLimitApplier(CpuLimitApplier applier); // empty = default
bool applyCpuLimit(qint64 pid, int cores);

} // namespace fw::fpkg
