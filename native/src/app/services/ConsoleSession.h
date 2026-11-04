#pragma once

// A console (FTP) session for the PS5 connect page. One FtpClient lives on a
// Qt worker thread (QThread, never std::thread: Qt sockets need a Qt thread);
// the page posts requests and receives results as queued signals on the UI
// thread. Transfers do not go through here: they are queue jobs.

#include "connect/ps5/FtpListing.h"
#include "connect/ps5/MountConfig.h"
#include "core/Result.h"

#include <QObject>
#include <QThread>

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
    explicit ConsoleSession(QObject* parent = nullptr);
    ~ConsoleSession() override;

    bool isConnected() const { return m_connected; }
    QString host() const { return m_host; }

    // All asynchronous; each answers with the matching signal.
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
    void connected(const QString& welcome);
    void disconnected(const QString& reason);
    void listed(const fw::ui::RemoteListing& listing);
    void operationDone(const QString& what, bool ok, const QString& message);
    void mountConfigLoaded(int tool, bool ok, const QString& text, const QString& message);
    void mountConfigPushed(int tool, bool ok, const QString& message);
    void freeSpace(const QString& text);

private:
    QThread m_thread;
    ConsoleWorker* m_worker = nullptr;
    bool m_connected = false;
    QString m_host;
};

} // namespace fw::ui

Q_DECLARE_METATYPE(fw::ui::RemoteListing)
