#include "app/services/ConsoleSession.h"
#include "app/i18n/I18n.h"

#include "connect/ps5/ConsoleBackup.h"
#include "connect/ps5/FtpClient.h"
#include "connect/ps5/RemotePath.h"

#include "core/Cancellation.h"

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
    // from inside it (the client's nested event loop delivered it).
    void run(std::function<void()> request)
    {
        m_pending.push_back(std::move(request));
        if (m_busy)
            return;
        m_busy = true;
        while (!m_pending.empty()) {
            auto next = std::move(m_pending.front());
            m_pending.pop_front();
            next();
        }
        m_busy = false;
    }

    // Any thread: stops the running operation (where the client can be
    // cancelled); later requests get a fresh token.
    void cancelRunning()
    {
        std::lock_guard lock(m_cancelMutex);
        m_cancel.cancel(tr("Disconnected"));
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
            const QString why = st.error().message;
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
            out.error = tr("Not connected.");
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
            emit m_session->operationDone(QStringLiteral("delete"), false, tr("Not connected."));
            return;
        }
        Status st = isDir ? m_client->removeTree(path, token()) : m_client->dele(path);
        emit m_session->operationDone(QStringLiteral("delete"), st.ok(), st.ok() ? tr("Deleted %1").arg(path) : st.error().message);
    }
    void doRename(const QString& from, const QString& to)
    {
        if (!m_client) {
            emit m_session->operationDone(QStringLiteral("rename"), false, tr("Not connected."));
            return;
        }
        auto st = m_client->rename(from, to);
        emit m_session->operationDone(QStringLiteral("rename"), st.ok(), st.ok() ? tr("Renamed to %1").arg(ps5::remoteFileName(to)) : st.error().message);
    }
    void doMkdir(const QString& path)
    {
        if (!m_client) {
            emit m_session->operationDone(QStringLiteral("mkdir"), false, tr("Not connected."));
            return;
        }
        auto st = m_client->mkd(path);
        emit m_session->operationDone(QStringLiteral("mkdir"), st.ok(), st.ok() ? tr("Created %1").arg(path) : st.error().message);
    }
    void doLoadMount(int tool)
    {
        if (!m_client) {
            emit m_session->mountConfigLoaded(tool, false, {}, tr("Not connected."));
            return;
        }
        auto r = ps5::loadMountConfig(*m_client, ps5::MountTool(tool));
        if (!r.ok()) {
            const bool fresh = r.error().code == ErrorCode::NotFound;
            emit m_session->mountConfigLoaded(tool, fresh, {}, fresh ? tr("No config.ini on the console yet; the defaults are shown.") : r.error().message);
            return;
        }
        emit m_session->mountConfigLoaded(tool, true, r.value(), tr("Loaded from the console."));
    }
    void doPushMount(int tool, const QString& text)
    {
        if (!m_client) {
            emit m_session->mountConfigPushed(tool, false, tr("Not connected."));
            return;
        }
        auto r = ps5::pushMountConfig(*m_client, ps5::MountTool(tool), text, ps5::newConsoleBackupDir());
        if (!r.ok()) {
            emit m_session->mountConfigPushed(tool, false, r.error().message);
            return;
        }
        emit m_session->mountConfigPushed(tool, true,
                                          r->backupPath.isEmpty() ? tr("Written to the console (no earlier config existed).")
                                                                  : tr("Written to the console. The previous config was saved to %1").arg(r->backupPath));
    }
    void doSiteAvail()
    {
        if (!m_client)
            return;
        auto r = m_client->siteAvail();
        emit m_session->freeSpace(r.ok() ? r.value().simplified() : QString());
    }

private:
    CancellationToken token()
    {
        std::lock_guard lock(m_cancelMutex);
        return m_cancel.token();
    }

    ConsoleSession* m_session;
    std::unique_ptr<ps5::FtpClient> m_client;
    std::deque<std::function<void()>> m_pending; // worker thread only
    bool m_busy = false;                          // worker thread only
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
    connect(this, &ConsoleSession::connected, this, [this] { m_connected = true; });
    connect(this, &ConsoleSession::disconnected, this, [this] { m_connected = false; });
}

ConsoleSession::~ConsoleSession()
{
    // Stop what is running, close the connection (after it, if the request
    // arrives inside it), then let the thread finish before the worker goes.
    m_worker->cancelRunning();
    QMetaObject::invokeMethod(m_worker, [w = m_worker] { w->run([w] { w->doDisconnect(); }); }, Qt::BlockingQueuedConnection);
    m_thread.quit();
    m_thread.wait(); // every client wait has a timeout; never delete a running worker
    delete m_worker;
}

void ConsoleSession::post(std::function<void()> request)
{
    QMetaObject::invokeMethod(m_worker, [w = m_worker, r = std::move(request)]() mutable { w->run(std::move(r)); },
                              Qt::QueuedConnection);
}

void ConsoleSession::connectTo(const QString& host, quint16 port, int connectTimeoutMs, bool passive)
{
    m_host = host;
    post([w = m_worker, host, port, connectTimeoutMs, passive] { w->doConnect(host, port, connectTimeoutMs, passive); });
}

void ConsoleSession::disconnect()
{
    m_worker->cancelRunning(); // a long listing or delete stops now
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

void ConsoleSession::loadMountConfig(ps5::MountTool tool)
{
    post([w = m_worker, tool] { w->doLoadMount(int(tool)); });
}

void ConsoleSession::pushMountConfig(ps5::MountTool tool, const QString& text)
{
    post([w = m_worker, tool, text] { w->doPushMount(int(tool), text); });
}

void ConsoleSession::siteAvail()
{
    post([w = m_worker] { w->doSiteAvail(); });
}

} // namespace fw::ui

#include "ConsoleSession.moc"
