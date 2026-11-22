#include "connect/ps5/KlogClient.h"

#include "connect/ps5/SocketWait.h"
#include "core/Tr.h"

#include <QElapsedTimer>
#include <QTcpSocket>
#include <QThread>

#include <algorithm>
#include <memory>

namespace fw::ps5 {

using detail::waitUntil;

KlogTag klogTag(const QString& line)
{
    const QString l = line.toLower();
    auto any = [&](std::initializer_list<const char*> words) {
        for (const char* w : words)
            if (l.contains(QLatin1String(w)))
                return true;
        return false;
    };
    if (any({"error", "fatal", "panic", "assert", "crash"}))
        return KlogTag::Error;
    if (any({"warn", "caution"}))
        return KlogTag::Warning;
    if (any({"debug", "trace", "verbose"}))
        return KlogTag::Debug;
    return KlogTag::Info;
}

QString klogTagName(KlogTag tag)
{
    switch (tag) {
    case KlogTag::Error: return QStringLiteral("error");
    case KlogTag::Warning: return QStringLiteral("warning");
    case KlogTag::Debug: return QStringLiteral("debug");
    case KlogTag::Info: break;
    }
    return QStringLiteral("info");
}

QString klogStateName(KlogState s)
{
    switch (s) {
    case KlogState::Connecting: return FW_TR("Connecting");
    case KlogState::Connected: return FW_TR("Connected");
    case KlogState::Disconnected: return FW_TR("Disconnected");
    case KlogState::Reconnecting: return FW_TR("Reconnecting");
    case KlogState::Stopped: return FW_TR("Stopped");
    }
    return {};
}

namespace {

// The first kMaxLineBytes of an over-long line, without a character cut in
// half at the end, and "…".
QString cutLine(QByteArray bytes)
{
    bytes.truncate(KlogLineSplitter::kMaxLineBytes);
    // Drop a trailing incomplete UTF-8 sequence (at most 3 bytes).
    qsizetype lead = bytes.size();
    for (int back = 0; back < 4 && lead > 0; ++back) {
        const uchar b = uchar(bytes.at(lead - 1));
        if ((b & 0xC0) != 0x80) { // an ASCII byte or a lead byte
            const int need = b < 0x80 ? 1 : (b >> 5) == 0x6 ? 2 : (b >> 4) == 0xE ? 3 : (b >> 3) == 0x1E ? 4 : 1;
            if (bytes.size() - (lead - 1) < need)
                bytes.truncate(lead - 1);
            break;
        }
        --lead;
    }
    return QString::fromUtf8(bytes) + QChar(0x2026);
}

} // namespace

QStringList KlogLineSplitter::feed(const QByteArray& bytes)
{
    QStringList out;
    qsizetype pos = 0;
    const qsizetype n = bytes.size();
    while (pos < n) {
        const qsizetype nl = bytes.indexOf('\n', pos);
        const qsizetype end = nl < 0 ? n : nl;
        if (m_skipping) { // the dropped rest of a line already cut
            if (nl < 0)
                break;
            m_skipping = false;
            pos = nl + 1;
            continue;
        }
        const qsizetype len = end - pos;
        if (m_pending.size() + len > kMaxLineBytes) {
            // Too long to be a log line (or no '\n' for this long): keep its start only.
            m_pending.append(bytes.constData() + pos, kMaxLineBytes - m_pending.size());
            out << cutLine(std::move(m_pending));
            m_pending.clear();
            if (nl < 0) {
                m_skipping = true;
                break;
            }
            pos = nl + 1;
            continue;
        }
        m_pending.append(bytes.constData() + pos, len);
        if (nl < 0)
            break;
        while (m_pending.endsWith('\r'))
            m_pending.chop(1);
        if (!m_pending.isEmpty())
            out << QString::fromUtf8(m_pending);
        m_pending.clear();
        pos = nl + 1;
    }
    return out;
}

QString KlogLineSplitter::flush()
{
    m_skipping = false;
    QByteArray rest = m_pending;
    m_pending.clear();
    while (rest.endsWith('\r'))
        rest.chop(1);
    return QString::fromUtf8(rest);
}

// ---- KlogClient ----------------------------------------------------------------------

KlogClient::KlogClient(Options options, LineCallback onLine, StateCallback onState)
    : m_opt(std::move(options)), m_onState(std::move(onState))
{
    if (onLine)
        m_onBatch = [onLine = std::move(onLine)](const std::vector<KlogLine>& lines) {
            for (const KlogLine& l : lines)
                onLine(l);
        };
}

KlogClient::KlogClient(Options options, BatchCallback onBatch, StateCallback onState, int)
    : m_opt(std::move(options)), m_onBatch(std::move(onBatch)), m_onState(std::move(onState))
{
}

std::unique_ptr<KlogClient> KlogClient::withBatches(Options options, BatchCallback onBatch, StateCallback onState)
{
    return std::unique_ptr<KlogClient>(new KlogClient(std::move(options), std::move(onBatch), std::move(onState), 0));
}

KlogClient::~KlogClient()
{
    stop();
}

bool KlogClient::isRunning() const
{
    return m_thread && m_thread->isRunning();
}

void KlogClient::start()
{
    if (isRunning())
        return;
    stop();
    m_stop = std::make_unique<CancellationSource>();
    m_thread = QThread::create([this] { run(); });
    m_thread->start();
}

void KlogClient::stop()
{
    if (m_stop)
        m_stop->cancel(QStringLiteral("stopped"));
    if (m_thread) {
        m_thread->wait();
        delete m_thread;
        m_thread = nullptr;
    }
}

void KlogClient::run()
{
    const CancellationToken token = m_stop->token();
    auto state = [&](KlogState s, const QString& detail) {
        if (m_onState)
            m_onState(s, detail);
    };
    bool first = true;
    while (!token.isCancelled()) {
        if (!first) {
            state(KlogState::Reconnecting, FW_TR("retrying in %1 s").arg(m_opt.reconnectDelayMs / 1000.0));
            if (!token.sleepFor(m_opt.reconnectDelayMs))
                break;
        }
        first = false;
        state(KlogState::Connecting, QStringLiteral("%1:%2").arg(m_opt.host).arg(m_opt.port));
        QTcpSocket s;
        const Status st = detail::connectSocket(s, m_opt.host, m_opt.port, m_opt.connectTimeoutMs, token,
                                                QStringLiteral("kernel log server"));
        if (!st.ok()) {
            if (st.error().isCancelled())
                break;
            state(KlogState::Disconnected, st.error().message);
            if (!m_opt.reconnect)
                break;
            continue;
        }
        m_connections.fetch_add(1);
        state(KlogState::Connected, FW_TR("Connected to %1:%2").arg(m_opt.host).arg(m_opt.port));
        KlogLineSplitter split;
        std::vector<KlogLine> batch;
        QElapsedTimer sinceBatch; // since the last hand-over
        sinceBatch.start();
        const auto add = [&](const QString& line) {
            m_lines.fetch_add(1);
            if (m_onBatch)
                batch.push_back(KlogLine{QDateTime::currentDateTime(), line, klogTag(line)});
        };
        const auto handOver = [&] {
            if (batch.empty())
                return;
            std::vector<KlogLine> lines;
            lines.swap(batch);
            m_onBatch(lines);
            sinceBatch.restart();
        };
        while (!token.isCancelled()) {
            if (s.bytesAvailable() > 0)
                for (const QString& line : split.feed(s.read(256 * 1024)))
                    add(line);
            if (!batch.empty() && sinceBatch.elapsed() >= kBatchMs)
                handOver();
            if (s.bytesAvailable() > 0)
                continue;
            if (s.state() != QAbstractSocket::ConnectedState)
                break;
            // With lines waiting, wake up when their interval is over.
            const int wait = batch.empty() ? 250 : int(std::max<qint64>(1, kBatchMs - sinceBatch.elapsed()));
            (void)waitUntil([&] { return s.bytesAvailable() > 0 || s.state() != QAbstractSocket::ConnectedState; }, wait,
                            token, {&s});
        }
        if (const QString rest = split.flush(); !rest.isEmpty())
            add(rest);
        handOver();
        s.abort();
        if (token.isCancelled())
            break;
        state(KlogState::Disconnected, FW_TR("The kernel log stream closed."));
        if (!m_opt.reconnect)
            break;
    }
    state(KlogState::Stopped, {});
}

// ---- KlogSession ---------------------------------------------------------------------

Result<std::unique_ptr<KlogSession>> KlogSession::open(const QString& host, quint16 port, int connectTimeoutMs,
                                                       const CancellationToken& token)
{
    std::unique_ptr<KlogSession> s(new KlogSession);
    s->m_socket = std::make_unique<QTcpSocket>();
    FW_TRY(detail::connectSocket(*s->m_socket, host, port, connectTimeoutMs, token, QStringLiteral("kernel log server")));
    return s;
}

KlogSession::~KlogSession()
{
    if (m_socket)
        m_socket->abort();
}

KlogSession::WaitResult KlogSession::waitFor(const std::function<bool(const QString&)>& matcher, int timeoutMs,
                                             const CancellationToken& token)
{
    WaitResult r;
    QElapsedTimer clock;
    clock.start();
    QTcpSocket& s = *m_socket;
    while (true) {
        if (s.bytesAvailable() > 0) {
            for (const QString& line : m_split.feed(s.readAll())) {
                r.lines << line;
                if (matcher(line)) {
                    r.outcome = Outcome::Matched;
                    r.matchedLine = line;
                    return r;
                }
            }
            continue;
        }
        if (s.state() != QAbstractSocket::ConnectedState) {
            r.outcome = Outcome::ConnectionLost;
            return r;
        }
        const qint64 left = timeoutMs - clock.elapsed();
        if (left <= 0) {
            r.outcome = Outcome::Timeout;
            return r;
        }
        const auto w = waitUntil([&] { return s.bytesAvailable() > 0 || s.state() != QAbstractSocket::ConnectedState; },
                                 int(left), token, {&s});
        if (w == detail::WaitResult::Cancelled) {
            r.outcome = Outcome::Cancelled;
            return r;
        }
    }
}

KlogSession::ReadResult KlogSession::read(int waitMs, const CancellationToken& token)
{
    ReadResult r;
    QTcpSocket& s = *m_socket;
    if (s.bytesAvailable() == 0 && s.state() == QAbstractSocket::ConnectedState) {
        const auto w = waitUntil([&] { return s.bytesAvailable() > 0 || s.state() != QAbstractSocket::ConnectedState; },
                                 waitMs, token, {&s});
        if (w == detail::WaitResult::Cancelled) {
            r.cancelled = true;
            return r;
        }
    }
    if (s.bytesAvailable() > 0)
        r.lines = m_split.feed(s.readAll());
    r.closed = s.state() != QAbstractSocket::ConnectedState && s.bytesAvailable() == 0;
    return r;
}

KlogSession::WaitResult waitForKlogLine(const QString& host, quint16 port,
                                        const std::function<bool(const QString&)>& matcher, int timeoutMs,
                                        int connectTimeoutMs, const CancellationToken& token, Error* connectError)
{
    auto s = KlogSession::open(host, port, connectTimeoutMs, token);
    if (!s.ok()) {
        if (connectError)
            *connectError = s.error();
        KlogSession::WaitResult r;
        r.outcome = s.error().isCancelled() ? KlogSession::Outcome::Cancelled : KlogSession::Outcome::ConnectionLost;
        return r;
    }
    return s.value()->waitFor(matcher, timeoutMs, token);
}

} // namespace fw::ps5
