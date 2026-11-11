#include "app/services/ConsoleSession.h"

#include "connect/ps5/FtpClient.h"
#include "connect/ps5/RemotePath.h"

#include "core/Cancellation.h"
#include "core/Tr.h"

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>

namespace fw::ui {

class ConsoleWorker : public QObject {
    Q_OBJECT
public:
    explicit ConsoleWorker(ConsoleSession* session) : m_session(session) {}

    // Worker thread: runs `request` now, or after the running one when called
    // from inside it (the client's nested event loop delivered it). A request
    // posted before the last cancelRunning() is dropped: a disconnect also
    // stops what was still waiting (the other deletes of a selection).
    void run(std::function<void()> request, quint64 epoch)
    {
        m_pending.push_back({std::move(request), epoch});
        if (m_busy)
            return;
        m_busy = true;
        while (!m_pending.empty()) {
            auto next = std::move(m_pending.front());
            m_pending.pop_front();
            if (next.epoch == m_epoch.load())
                next.fn();
        }
        m_busy = false;
    }

    // Any thread: the value a request posted now has to carry.
    quint64 epoch() const { return m_epoch.load(); }

    // Any thread: stops the running operation (where the client can be
    // cancelled) and drops the requests that wait behind it; later requests
    // get a fresh token.
    void cancelRunning()
    {
        std::lock_guard lock(m_cancelMutex);
        ++m_epoch;
        m_cancel.cancel(QStringLiteral("Disconnected"));
        m_cancel = CancellationSource();
    }

public slots:
    void doConnect(const QString& host, quint16 port, int connectTimeoutMs, bool passive)
    {
        ps5::FtpOptions opt;
        opt.connectTimeoutMs = connectTimeoutMs;
        opt.replyTimeoutMs = std::max(connectTimeoutMs, 5000);
        opt.passive = passive;
        m_client = std::make_unique<ps5::FtpClient>(opt);
        auto st = m_client->open(host, port, token());
        if (!st.ok()) {
            // A cancelled attempt (Disconnect while connecting) is not a failure to report.
            const QString why = st.error().isCancelled() ? QString() : st.error().message;
            m_client.reset();
            emit m_session->disconnected(why);
            return;
        }
        emit m_session->connected(m_client->welcome());
    }
    void doDisconnect()
    {
        if (m_client)
            m_client->quit();
        m_client.reset();
        emit m_session->disconnected(QString());
    }
    void doList(const QString& path)
    {
        RemoteListing out;
        out.path = ps5::normalizeRemotePath(path);
        if (!m_client) {
            out.error = FW_TR("Not connected.");
            emit m_session->listed(out);
            return;
        }
        auto r = m_client->list(out.path, ps5::FtpListMode::Auto, token());
        if (!r.ok()) {
            out.error = r.error().message;
            if (!m_client->isConnected()) {
                m_client.reset();
                emit m_session->disconnected(out.error);
            }
        } else {
            out.entries = std::move(r.value());
        }
        emit m_session->listed(out);
    }
    void doRemove(const QString& path, bool isDir)
    {
        if (!m_client) {
            emit m_session->operationDone(QStringLiteral("delete"), false, FW_TR("Not connected."));
            return;
        }
        Status st = isDir ? m_client->removeTree(path, token()) : m_client->dele(path);
        noteDroppedLink(st);
        emit m_session->operationDone(QStringLiteral("delete"), st.ok(), st.ok() ? FW_TR("Deleted %1").arg(path) : st.error().message);
    }
    void doRename(const QString& from, const QString& to)
    {
        if (!m_client) {
            emit m_session->operationDone(QStringLiteral("rename"), false, FW_TR("Not connected."));
            return;
        }
        auto st = m_client->rename(from, to);
        noteDroppedLink(st);
        emit m_session->operationDone(QStringLiteral("rename"), st.ok(), st.ok() ? FW_TR("Renamed to %1").arg(ps5::remoteFileName(to)) : st.error().message);
    }
    void doMkdir(const QString& path)
    {
        if (!m_client) {
            emit m_session->operationDone(QStringLiteral("mkdir"), false, FW_TR("Not connected."));
            return;
        }
        auto st = m_client->mkd(path);
        noteDroppedLink(st);
        emit m_session->operationDone(QStringLiteral("mkdir"), st.ok(), st.ok() ? FW_TR("Created %1").arg(path) : st.error().message);
    }
    void doSiteAvail()
    {
        if (!m_client)
            return;
        auto r = m_client->siteAvail();
        emit m_session->freeSpace(r.ok() ? r.value().simplified() : QString());
    }

private:
    // A command that failed because the link is gone ends the session (like a listing does).
    void noteDroppedLink(const Status& st)
    {
        if (!st.ok() && m_client && !m_client->isConnected()) {
            m_client.reset();
            emit m_session->disconnected(st.error().message);
        }
    }
    CancellationToken token()
    {
        std::lock_guard lock(m_cancelMutex);
        return m_cancel.token();
    }

    struct Request {
        std::function<void()> fn;
        quint64 epoch = 0;
    };

    ConsoleSession* m_session;
    std::unique_ptr<ps5::FtpClient> m_client;
    std::deque<Request> m_pending; // worker thread only
    bool m_busy = false;           // worker thread only
    std::atomic<quint64> m_epoch{0};
    std::mutex m_cancelMutex;
    CancellationSource m_cancel;
};

ConsoleSession::ConsoleSession(QObject* parent) : QObject(parent)
{
    qRegisterMetaType<RemoteListing>();
    m_worker = new ConsoleWorker(this);
    m_worker->moveToThread(&m_thread);
    m_thread.setObjectName(QStringLiteral("ps5-session"));
    m_thread.start();
    // The worker's signals arrive here on the UI thread; the state follows them.
    connect(this, &ConsoleSession::connected, this, [this](const QString& welcome) { setState(ConnectionState::Connected, welcome); });
    connect(this, &ConsoleSession::disconnected, this, [this](const QString& reason) {
        setState(reason.isEmpty() ? ConnectionState::Offline : ConnectionState::Failed, reason);
    });
}

void ConsoleSession::setState(ConnectionState state, const QString& detail)
{
    if (m_state == state && detail.isEmpty())
        return;
    m_state = state;
    emit stateChanged(state, detail);
}

ConsoleSession::~ConsoleSession()
{
    // Stop what is running, close the connection (after it, if the request
    // arrives inside it), then let the thread finish before the worker goes.
    m_worker->cancelRunning();
    QMetaObject::invokeMethod(m_worker, [w = m_worker] { w->run([w] { w->doDisconnect(); }, w->epoch()); }, Qt::BlockingQueuedConnection);
    m_thread.quit();
    m_thread.wait(); // every client wait has a timeout; never delete a running worker
    delete m_worker;
}

void ConsoleSession::post(std::function<void()> request)
{
    QMetaObject::invokeMethod(m_worker, [w = m_worker, r = std::move(request), epoch = m_worker->epoch()]() mutable { w->run(std::move(r), epoch); },
                              Qt::QueuedConnection);
}

void ConsoleSession::connectTo(const QString& host, quint16 port, int connectTimeoutMs, bool passive)
{
    if (m_state == ConnectionState::Connecting || m_state == ConnectionState::Connected)
        return; // one connection at a time (Return in the host field while connecting)
    m_host = host;
    m_port = port;
    setState(ConnectionState::Connecting, {});
    post([w = m_worker, host, port, connectTimeoutMs, passive] { w->doConnect(host, port, connectTimeoutMs, passive); });
}

void ConsoleSession::disconnect()
{
    if (m_state == ConnectionState::Offline)
        return;
    m_worker->cancelRunning(); // a long listing, delete or connect attempt stops now
    post([w = m_worker] { w->doDisconnect(); });
}

void ConsoleSession::list(const QString& path)
{
    post([w = m_worker, path] { w->doList(path); });
}

void ConsoleSession::remove(const QString& path, bool isDir)
{
    post([w = m_worker, path, isDir] { w->doRemove(path, isDir); });
}

void ConsoleSession::rename(const QString& from, const QString& to)
{
    post([w = m_worker, from, to] { w->doRename(from, to); });
}

void ConsoleSession::mkdir(const QString& path)
{
    post([w = m_worker, path] { w->doMkdir(path); });
}

void ConsoleSession::siteAvail()
{
    post([w = m_worker] { w->doSiteAvail(); });
}

} // namespace fw::ui

#include "ConsoleSession.moc"
