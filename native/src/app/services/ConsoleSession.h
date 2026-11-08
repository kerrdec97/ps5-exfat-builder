#pragma once

// A console (FTP) session for the PS5 connect page. One FtpClient lives on a
// Qt worker thread (QThread, never std::thread: Qt sockets need a Qt thread);
// the page posts requests and receives results as queued signals on the UI
// thread. Transfers do not go through here: they are queue jobs.
//
// Requests run strictly one after another. The FTP client waits in a nested
// event loop, which also delivers newly posted requests; those are queued and
// run after the current one instead of re-entering the client (a nested
// disconnect would otherwise destroy the client under the running command).

#include "connect/ps5/FtpListing.h"
#include "connect/ps5/MountConfig.h"
#include "core/Result.h"

#include <QObject>
#include <QThread>

#include <functional>
#include <vector>

namespace fw::ui {

struct RemoteListing {
    QString path;
    std::vector<ps5::FtpEntry> entries;
    QString error; // empty on success
};

class ConsoleWorker; // lives on the worker thread

class ConsoleSession : public QObject {
    Q_OBJECT
public:
    // The one connection lifecycle every view (hero card, header chip, Home
    // tile, file manager) is a projection of:
    //   Offline ─▶ Connecting ─▶ Connected ─▶ Offline   (disconnect())
    //                 │             └──────▶ Failed    (the link dropped)
    //                 └──────────────────▶ Failed    (could not connect)
    //   Failed ─▶ Connecting                          (connectTo() again)
    enum class ConnectionState { Offline, Connecting, Connected, Failed };
    Q_ENUM(ConnectionState)

    explicit ConsoleSession(QObject* parent = nullptr);
    ~ConsoleSession() override;

    ConnectionState state() const { return m_state; }
    bool isConnected() const { return m_state == ConnectionState::Connected; }
    bool isConnecting() const { return m_state == ConnectionState::Connecting; }
    QString host() const { return m_host; }

    // All asynchronous; each answers with the matching signal. connectTo()
    // while connecting or connected is ignored (one connection at a time).
    void connectTo(const QString& host, quint16 port, int connectTimeoutMs, bool passive);
    void disconnect();
    void list(const QString& path);
    void remove(const QString& path, bool isDir);
    void rename(const QString& from, const QString& to);
    void mkdir(const QString& path);
    void loadMountConfig(ps5::MountTool tool);
    void pushMountConfig(ps5::MountTool tool, const QString& text);
    void siteAvail();

signals:
    // Every transition, with the welcome line (Connected) or the reason (Failed).
    void stateChanged(fw::ui::ConsoleSession::ConnectionState state, const QString& detail);
    void connected(const QString& welcome);
    void disconnected(const QString& reason);
    void listed(const fw::ui::RemoteListing& listing);
    void operationDone(const QString& what, bool ok, const QString& message);
    void mountConfigLoaded(int tool, bool ok, const QString& text, const QString& message);
    void mountConfigPushed(int tool, bool ok, const QString& message);
    void freeSpace(const QString& text);

private:
    void post(std::function<void()> request);
    void setState(ConnectionState state, const QString& detail);

    QThread m_thread;
    ConsoleWorker* m_worker = nullptr;
    ConnectionState m_state = ConnectionState::Offline;
    QString m_host;
};

} // namespace fw::ui

Q_DECLARE_METATYPE(fw::ui::RemoteListing)
