#include "connect/ps5/KlogClient.h"

#include "connect/ps5/SocketWait.h"

#include <QElapsedTimer>
#include <QTcpSocket>
#include <QThread>

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
    case KlogState::Connecting: return QStringLiteral("Connecting");
    case KlogState::Connected: return QStringLiteral("Connected");
    case KlogState::Disconnected: return QStringLiteral("Disconnected");
    case KlogState::Reconnecting: return QStringLiteral("Reconnecting");
    case KlogState::Stopped: return QStringLiteral("Stopped");
    }
    return {};
}

QStringList KlogLineSplitter::feed(const QByteArray& bytes)
{
    m_pending += bytes;
    QStringList out;
    qsizetype start = 0;
    while (true) {
        const qsizetype nl = m_pending.indexOf('\n', start);
        if (nl < 0)
            break;
        QByteArray line = m_pending.mid(start, nl - start);
        while (line.endsWith('\r'))
            line.chop(1);
        if (!line.isEmpty())
            out << QString::fromUtf8(line);
        start = nl + 1;
    }
    m_pending.remove(0, start);
    // A stream that never sends '\n' must not grow without bound.
    if (m_pending.size() > 1024 * 1024) {
        out << QString::fromUtf8(m_pending);
        m_pending.clear();
    }
    return out;
}

QString KlogLineSplitter::flush()
{
    QByteArray rest = m_pending;
    m_pending.clear();
    while (rest.endsWith('\r'))
        rest.chop(1);
    return QString::fromUtf8(rest);
}

// ---- KlogClient ----------------------------------------------------------------------

KlogClient::KlogClient(Options options, LineCallback onLine, StateCallback onState)
    : m_opt(std::move(options)), m_onLine(std::move(onLine)), m_onState(std::move(onState))
{
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
            state(KlogState::Reconnecting, QStringLiteral("retrying in %1 s").arg(m_opt.reconnectDelayMs / 1000.0));
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
        state(KlogState::Connected, QStringLiteral("Connected to %1:%2").arg(m_opt.host).arg(m_opt.port));
        KlogLineSplitter split;
        while (!token.isCancelled()) {
            if (s.bytesAvailable() > 0) {
                for (const QString& line : split.feed(s.readAll())) {
                    m_lines.fetch_add(1);
                    if (m_onLine)
                        m_onLine(KlogLine{QDateTime::currentDateTime(), line, klogTag(line)});
                }
                continue;
            }
            if (s.state() != QAbstractSocket::ConnectedState)
                break;
            (void)waitUntil([&] { return s.bytesAvailable() > 0 || s.state() != QAbstractSocket::ConnectedState; }, 250,
                            token, {&s});
        }
        const QString rest = split.flush();
        if (!rest.isEmpty() && m_onLine) {
            m_lines.fetch_add(1);
            m_onLine(KlogLine{QDateTime::currentDateTime(), rest, klogTag(rest)});
        }
        s.abort();
        if (token.isCancelled())
            break;
        state(KlogState::Disconnected, QStringLiteral("The kernel log stream closed."));
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
