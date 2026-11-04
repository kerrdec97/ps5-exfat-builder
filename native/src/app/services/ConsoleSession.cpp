#include "app/services/ConsoleSession.h"

#include "connect/ps5/ConsoleBackup.h"
#include "connect/ps5/FtpClient.h"
#include "connect/ps5/RemotePath.h"

#include <memory>

namespace fw::ui {

class ConsoleWorker : public QObject {
    Q_OBJECT
public:
    explicit ConsoleWorker(ConsoleSession* session) : m_session(session) {}

public slots:
    void doConnect(const QString& host, quint16 port, int connectTimeoutMs, bool passive)
    {
        ps5::FtpOptions opt;
        opt.connectTimeoutMs = connectTimeoutMs;
        opt.replyTimeoutMs = std::max(connectTimeoutMs, 5000);
        opt.passive = passive;
        m_client = std::make_unique<ps5::FtpClient>(opt);
        auto st = m_client->open(host, port);
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
            out.error = QStringLiteral("Not connected.");
            emit m_session->listed(out);
            return;
        }
        auto r = m_client->list(out.path);
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
            emit m_session->operationDone(QStringLiteral("delete"), false, QStringLiteral("Not connected."));
            return;
        }
        Status st = isDir ? m_client->removeTree(path) : m_client->dele(path);
        emit m_session->operationDone(QStringLiteral("delete"), st.ok(), st.ok() ? QStringLiteral("Deleted %1").arg(path) : st.error().message);
    }
    void doRename(const QString& from, const QString& to)
    {
        if (!m_client) {
            emit m_session->operationDone(QStringLiteral("rename"), false, QStringLiteral("Not connected."));
            return;
        }
        auto st = m_client->rename(from, to);
        emit m_session->operationDone(QStringLiteral("rename"), st.ok(), st.ok() ? QStringLiteral("Renamed to %1").arg(ps5::remoteFileName(to)) : st.error().message);
    }
    void doMkdir(const QString& path)
    {
        if (!m_client) {
            emit m_session->operationDone(QStringLiteral("mkdir"), false, QStringLiteral("Not connected."));
            return;
        }
        auto st = m_client->mkd(path);
        emit m_session->operationDone(QStringLiteral("mkdir"), st.ok(), st.ok() ? QStringLiteral("Created %1").arg(path) : st.error().message);
    }
    void doLoadMount(int tool)
    {
        if (!m_client) {
            emit m_session->mountConfigLoaded(tool, false, {}, QStringLiteral("Not connected."));
            return;
        }
        auto r = ps5::loadMountConfig(*m_client, ps5::MountTool(tool));
        if (!r.ok()) {
            const bool fresh = r.error().code == ErrorCode::NotFound;
            emit m_session->mountConfigLoaded(tool, fresh, {}, fresh ? QStringLiteral("No config.ini on the console yet; the defaults are shown.") : r.error().message);
            return;
        }
        emit m_session->mountConfigLoaded(tool, true, r.value(), QStringLiteral("Loaded from the console."));
    }
    void doPushMount(int tool, const QString& text)
    {
        if (!m_client) {
            emit m_session->mountConfigPushed(tool, false, QStringLiteral("Not connected."));
            return;
        }
        auto r = ps5::pushMountConfig(*m_client, ps5::MountTool(tool), text, ps5::newConsoleBackupDir());
        if (!r.ok()) {
            emit m_session->mountConfigPushed(tool, false, r.error().message);
            return;
        }
        emit m_session->mountConfigPushed(tool, true,
                                          r->backupPath.isEmpty() ? QStringLiteral("Written to the console (no earlier config existed).")
                                                                  : QStringLiteral("Written to the console. The previous config was saved to %1").arg(r->backupPath));
    }
    void doSiteAvail()
    {
        if (!m_client)
            return;
        auto r = m_client->siteAvail();
        emit m_session->freeSpace(r.ok() ? r.value().simplified() : QString());
    }

private:
    ConsoleSession* m_session;
    std::unique_ptr<ps5::FtpClient> m_client;
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
    QMetaObject::invokeMethod(m_worker, &ConsoleWorker::doDisconnect, Qt::BlockingQueuedConnection);
    m_thread.quit();
    m_thread.wait(5000);
    delete m_worker;
}

void ConsoleSession::connectTo(const QString& host, quint16 port, int connectTimeoutMs, bool passive)
{
    m_host = host;
    QMetaObject::invokeMethod(m_worker, [this, host, port, connectTimeoutMs, passive] { m_worker->doConnect(host, port, connectTimeoutMs, passive); }, Qt::QueuedConnection);
}

void ConsoleSession::disconnect()
{
    QMetaObject::invokeMethod(m_worker, &ConsoleWorker::doDisconnect, Qt::QueuedConnection);
}

void ConsoleSession::list(const QString& path)
{
    QMetaObject::invokeMethod(m_worker, [this, path] { m_worker->doList(path); }, Qt::QueuedConnection);
}

void ConsoleSession::remove(const QString& path, bool isDir)
{
    QMetaObject::invokeMethod(m_worker, [this, path, isDir] { m_worker->doRemove(path, isDir); }, Qt::QueuedConnection);
}

void ConsoleSession::rename(const QString& from, const QString& to)
{
    QMetaObject::invokeMethod(m_worker, [this, from, to] { m_worker->doRename(from, to); }, Qt::QueuedConnection);
}

void ConsoleSession::mkdir(const QString& path)
{
    QMetaObject::invokeMethod(m_worker, [this, path] { m_worker->doMkdir(path); }, Qt::QueuedConnection);
}

void ConsoleSession::loadMountConfig(ps5::MountTool tool)
{
    QMetaObject::invokeMethod(m_worker, [this, tool] { m_worker->doLoadMount(int(tool)); }, Qt::QueuedConnection);
}

void ConsoleSession::pushMountConfig(ps5::MountTool tool, const QString& text)
{
    QMetaObject::invokeMethod(m_worker, [this, tool, text] { m_worker->doPushMount(int(tool), text); }, Qt::QueuedConnection);
}

void ConsoleSession::siteAvail()
{
    QMetaObject::invokeMethod(m_worker, &ConsoleWorker::doSiteAvail, Qt::QueuedConnection);
}

} // namespace fw::ui

#include "ConsoleSession.moc"
