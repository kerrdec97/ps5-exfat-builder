#include "engines/fpkg/PubCmd.h"

#include "core/Log.h"
#include "core/ProcessRunner.h"
#include "engines/fpkg/Toolchain.h"

#include <QFileInfo>
#include <QHash>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace fw::fpkg {

// ---- command lines ----------------------------------------------------------------

QStringList imgCreateArgs(const QString& gp5, const QString& output, std::optional<int> level, const QString& ref)
{
    QStringList a{QStringLiteral("img_create"), QStringLiteral("--oformat"), QStringLiteral("nwonly")};
    if (level)
        a << QStringLiteral("--compression_level") << QString::number(*level);
    if (!ref.isEmpty())
        a << QStringLiteral("--ref_pkg_path") << ref;
    a << gp5 << output;
    return a;
}

QStringList imgVerifyArgs(const QString& passcode, bool integrity, const QString& package)
{
    return {QStringLiteral("img_verify"), QStringLiteral("--passcode"), passcode, QStringLiteral("--format_check"),
            QStringLiteral("on"), QStringLiteral("--integrity_check"), integrity ? QStringLiteral("on") : QStringLiteral("off"),
            QStringLiteral("--no_progress_bar"), package};
}

QStringList imgVerifyArgs(const QString& passcode, bool formatCheck, bool integrity, const QString& package)
{
    return {QStringLiteral("img_verify"), QStringLiteral("--passcode"), passcode, QStringLiteral("--format_check"),
            formatCheck ? QStringLiteral("on") : QStringLiteral("off"), QStringLiteral("--integrity_check"),
            integrity ? QStringLiteral("on") : QStringLiteral("off"), QStringLiteral("--no_progress_bar"), package};
}

QStringList imgExtractArgs(const QString& passcode, const QString& package, const QString& destination)
{
    return {QStringLiteral("img_extract"), QStringLiteral("--passcode"), passcode, QStringLiteral("--no_progress_bar"),
            package, destination};
}

QStringList dds2pngArgs(const QString& dds, const QString& png, bool preserveAlpha)
{
    QStringList a{dds, png};
    if (preserveAlpha)
        a << QStringLiteral("--preserve-alpha");
    return a;
}

// ---- classification ----------------------------------------------------------------

bool isRoutineWarning(const QString& text)
{
    static const char* routine[] = {
        "toolchain is invalid or missing",
        "(online check)",
        "has not been built for submission",
        "- Perform package verification",
        "- Rebuild the package for submission",
        "Process finished with warning(s)",
    };
    for (const char* r : routine)
        if (text.contains(QLatin1String(r), Qt::CaseInsensitive))
            return true;
    return false;
}

ToolLine classifyToolLine(const QString& line)
{
    ToolLine l;
    l.raw = line;
    const QString s = line.trimmed();
    if (s.isEmpty())
        return l;
    if (s.startsWith(QStringLiteral("[Error]"))) {
        l.kind = LineKind::Error;
        l.text = s.mid(7).trimmed();
    } else if (s.startsWith(QStringLiteral("[Warn]"))) {
        l.text = s.mid(6).trimmed();
        l.kind = isRoutineWarning(l.text) ? LineKind::Info : LineKind::Warning;
    } else if (s.startsWith(QStringLiteral("[Info]"))) {
        l.kind = LineKind::Info;
        l.text = s.mid(6).trimmed();
    } else if (s.startsWith(QStringLiteral("[Debug]"))) {
        l.kind = LineKind::Info;
        l.text = s.mid(7).trimmed();
    } else if (s.startsWith(QStringLiteral("Creating an image"))) {
        l.kind = LineKind::Info;
        l.text = s;
    } else if (s.startsWith(QStringLiteral("wine:")) || s.contains(QStringLiteral(":err:"))) {
        l.kind = LineKind::Warning;
        l.text = s;
    }
    return l;
}

QStringList explainToolErrors(const QStringList& errors)
{
    struct Known {
        const char* needle;
        const char* text;
    };
    static const Known known[] = {
        {"reserved node found", "A file or folder name in the game is reserved by the package format. Rename it in the dump."},
        {"invalid attribute value dst_path", "A package path is not accepted by the Publishing Tools (check for unusual characters)."},
        {"invalid attribute value src_path", "A source path is not accepted by the Publishing Tools (non-ASCII folder names are a common cause)."},
        {"Format of the GP5 file is not valid", "The Publishing Tools rejected the project file. Report this with the build logs."},
        {"File not found", "A file listed in the project was not found. Was the dump changed while it was being packed?"},
        {"ucp_digest is invalid", "A trophy or UDS .ucp archive in sce_sys has a stale digest. Turn on \"Fix trophy and UDS archive digests\"."},
        {"Format of the UCP file is not valid", "A trophy or UDS .ucp archive in sce_sys is damaged."},
        {"Format of the png file is not valid", "A PNG in sce_sys has the wrong format (icon0/pic0/pic1 must be 8-bit RGB, pic2 8-bit RGBA)."},
        {"NAPS metadata missing", "The base package was not built by the Publishing Tools, so an update cannot be made against it."},
        {"Compression level should be", "The compression level must be from -4 to 9."},
        {"keystone", "The dump's sce_sys/keystone is missing or not accepted. The debug Publishing Tools need the dump's own 96-byte keystone."},
    };
    QStringList out;
    for (const QString& e : errors)
        for (const Known& k : known)
            if (e.contains(QLatin1String(k.needle), Qt::CaseInsensitive)) {
                const QString t = QString::fromUtf8(k.text);
                if (!out.contains(t))
                    out << t;
            }
    return out;
}

// ---- parser --------------------------------------------------------------------------

PubCmdOutputParser::PubCmdOutputParser(LineSink onLine, BarSink onBar, bool withBar)
    : m_onLine(std::move(onLine)), m_onBar(std::move(onBar)), m_withBar(withBar)
{
}

void PubCmdOutputParser::feed(const QByteArray& chunk)
{
    for (const char c : chunk) {
        if (c == '\r' || c == '\n') {
            endLine();
            continue;
        }
        if (c == '=' && m_withBar && m_armed && !m_lineHasText) {
            m_lineHasBar = true;
            if (m_steps < kBarSteps) {
                ++m_steps;
                if (m_onBar)
                    m_onBar(m_steps);
            }
            continue;
        }
        m_line += c;
        if (c != ' ' && c != '\t')
            m_lineHasText = true;
    }
}

void PubCmdOutputParser::endLine()
{
    if (m_armed && m_lineHasBar && m_steps >= 1)
        m_armed = false; // the bar ends at the first line break after it started
    const QString line = QString::fromUtf8(m_line);
    m_line.clear();
    m_lineHasText = false;
    m_lineHasBar = false;
    const QString s = line.trimmed();
    if (s.isEmpty())
        return;
    if (s.startsWith(QStringLiteral("|____"))) {
        if (m_withBar) {
            m_armed = true;
            m_sawRuler = true;
            m_steps = 0;
        }
        return;
    }
    // The scale line under the ruler ("0        20 ...") and stray bar remnants.
    bool scale = true;
    for (const QChar ch : s)
        if (!(ch.isDigit() || ch == u' ' || ch == u'%' || ch == u'|' || ch == u'='))
            scale = false;
    if (scale)
        return;
    if (m_onLine)
        m_onLine(classifyToolLine(line));
}

void PubCmdOutputParser::finish()
{
    if (!m_line.isEmpty())
        endLine();
}

// ---- results ---------------------------------------------------------------------------

QString ToolRunResult::failureText() const
{
    QStringList parts;
    if (cancelled)
        return QStringLiteral("cancelled");
    if (!errors.isEmpty()) {
        parts << errors.mid(0, 3);
    } else if (crashed) {
        parts << QStringLiteral("the tool crashed (exit code %1)").arg(exitCode);
    } else if (exitCode != 0) {
        parts << QStringLiteral("exit code %1 (0x%2)").arg(exitCode).arg(quint32(exitCode), 8, 16, QLatin1Char('0'));
    }
    const QString hint = exitCodeHint(exitCode);
    if (!hint.isEmpty())
        parts << hint;
    parts << explainToolErrors(errors);
    if (parts.isEmpty())
        parts << QStringLiteral("unknown error");
    return parts.join(QStringLiteral(" "));
}

QString formatElapsed(qint64 ms)
{
    if (ms < 0)
        ms = 0;
    const qint64 h = ms / 3600000, m = (ms / 60000) % 60, s = (ms / 1000) % 60, frac = (ms % 1000) * 10000;
    return QStringLiteral("%1:%2:%3.%4")
        .arg(h, 2, 10, QLatin1Char('0'))
        .arg(m, 2, 10, QLatin1Char('0'))
        .arg(s, 2, 10, QLatin1Char('0'))
        .arg(frac, 7, 10, QLatin1Char('0'));
}

// ---- CPU limit -----------------------------------------------------------------------------

namespace {
std::mutex& limitMutex()
{
    static std::mutex m;
    return m;
}
QHash<QString, int>& liveLimits()
{
    static QHash<QString, int> h;
    return h;
}
CpuLimitApplier& applierSlot()
{
    static CpuLimitApplier a;
    return a;
}
} // namespace

void setLiveCpuLimit(const QString& jobId, int cores)
{
    std::lock_guard lock(limitMutex());
    liveLimits().insert(jobId, std::max(0, cores));
}

std::optional<int> liveCpuLimit(const QString& jobId)
{
    std::lock_guard lock(limitMutex());
    const auto it = liveLimits().constFind(jobId);
    if (it == liveLimits().cend())
        return std::nullopt;
    return *it;
}

void clearLiveCpuLimit(const QString& jobId)
{
    std::lock_guard lock(limitMutex());
    liveLimits().remove(jobId);
}

void setCpuLimitApplier(CpuLimitApplier applier)
{
    std::lock_guard lock(limitMutex());
    applierSlot() = std::move(applier);
}

bool applyCpuLimit(qint64 pid, int cores)
{
    CpuLimitApplier a;
    {
        std::lock_guard lock(limitMutex());
        a = applierSlot();
    }
    return a ? a(pid, cores) : setProcessCpuLimit(pid, cores);
}

// ---- running ---------------------------------------------------------------------------------

namespace {

class Watcher {
public:
    Watcher(const ToolRunOptions& o) : m_opt(o) {}
    ~Watcher() { stop(); }

    void start(qint64 pid, int appliedLimit)
    {
        m_pid = pid;
        m_applied = appliedLimit;
        m_thread = std::thread([this] { loop(); });
    }
    void stop()
    {
        {
            std::lock_guard lock(m_mutex);
            m_stop = true;
        }
        m_cv.notify_all();
        if (m_thread.joinable())
            m_thread.join();
    }

private:
    void loop()
    {
        qint64 lastSize = -1;
        int tick = 0;
        std::unique_lock lock(m_mutex);
        while (!m_stop) {
            m_cv.wait_for(lock, std::chrono::milliseconds(250));
            if (m_stop)
                break;
            lock.unlock();
            if (!m_opt.liveLimitKey.isEmpty()) {
                if (const auto live = liveCpuLimit(m_opt.liveLimitKey); live && *live != m_applied) {
                    applyCpuLimit(m_pid, *live);
                    m_applied = *live;
                }
            }
            if (++tick % 2 == 0 && (m_opt.pollBytes || !m_opt.watchFile.isEmpty()) && m_opt.onBytesWritten) {
                qint64 size = 0;
                if (m_opt.pollBytes) {
                    size = m_opt.pollBytes();
                } else {
                    const QFileInfo fi(m_opt.watchFile);
                    size = fi.exists() ? fi.size() : 0;
                }
                if (size != lastSize) {
                    lastSize = size;
                    m_opt.onBytesWritten(size);
                }
            }
            lock.lock();
        }
    }

    const ToolRunOptions& m_opt;
    qint64 m_pid = 0;
    int m_applied = 0;
    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_stop = false;
};

} // namespace

Result<ToolRunResult> runPubTool(const ToolRunOptions& o, const CancellationToken& token)
{
    ToolRunResult r;
    PubCmdOutputParser out(
        [&](const ToolLine& l) {
            if (l.kind == LineKind::Error)
                r.errors << l.text;
            else if (l.kind == LineKind::Warning)
                r.warnings << l.text;
            if (o.onLine && l.kind != LineKind::Dropped)
                o.onLine(l);
        },
        [&](int steps) {
            r.barSteps = steps;
            if (o.onBar)
                o.onBar(steps);
        },
        o.withBar);
    PubCmdOutputParser err(
        [&](const ToolLine& l) {
            if (l.kind == LineKind::Error)
                r.errors << l.text;
            else if (l.kind == LineKind::Warning)
                r.warnings << l.text;
            if (o.onLine && l.kind != LineKind::Dropped)
                o.onLine(l);
        },
        {}, false);

    Watcher watcher(o);
    ProcessSpec spec;
    spec.program = o.program;
    spec.arguments = o.arguments;
    spec.workingDirectory = o.workingDirectory;
    spec.environment = o.environment;
    spec.mergeStderr = false;
    spec.keepOutput = false;
    spec.encoding = OutputEncoding::Utf8;
    spec.onRawOutput = [&](const QByteArray& chunk, bool isStderr) { (isStderr ? err : out).feed(chunk); };
    spec.onStarted = [&](qint64 pid) {
        if (o.priorityMode >= 0)
            (void)applyProcessPriority(pid, o.priorityMode);
        int applied = 0;
        if (o.liveLimitKey.isEmpty() || !liveCpuLimit(o.liveLimitKey)) {
            if (o.cpuCoreLimit > 0 && applyCpuLimit(pid, o.cpuCoreLimit))
                applied = o.cpuCoreLimit;
        } else if (const auto live = liveCpuLimit(o.liveLimitKey); live && *live > 0 && applyCpuLimit(pid, *live)) {
            applied = *live;
        }
        watcher.start(pid, applied);
    };
    r.commandLine = describeCommandLine(spec);
    r.startedUtc = QDateTime::currentDateTimeUtc();
    auto pr = runProcess(spec, token);
    watcher.stop();
    r.finishedUtc = QDateTime::currentDateTimeUtc();
    if (!pr.ok())
        return pr.takeError();
    out.finish();
    err.finish();
    r.exitCode = pr->exitCode;
    r.cancelled = pr->cancelled;
    r.crashed = pr->crashed;
    r.elapsedMs = pr->elapsedMs;
    r.sawBar = out.sawBar();
    return r;
}

} // namespace fw::fpkg
