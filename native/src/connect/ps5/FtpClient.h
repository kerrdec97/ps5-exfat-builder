#pragma once

// A small blocking FTP client on QTcpSocket (Qt 6 has no QFtp).
//
// One FtpClient owns one control connection and is used from ONE thread: the
// one that calls connectToHost(). Waits run a local event loop (SocketWait.h),
// so it works on queue worker threads without an event loop of their own.
// That thread must have been started by Qt (QThreadPool, QThread): Qt's
// socket notifiers do not work on std::thread. Parallel transfers use one
// FtpClient per worker thread.
//
// Supported: anonymous or USER/PASS login, FEAT, PASV with EPSV fallback (or
// active PORT/EPRT), TYPE A/I, LIST and MLSD (FtpListing.h), NLST, CWD, PWD,
// MKD, RMD, DELE, RNFR/RNTO, SIZE, MDTM, SITE, NOOP, QUIT, RETR/STOR/APPE with
// REST offsets, streaming progress, cancellation and timeouts. After a
// cancelled or timed-out transfer the client disconnects (the server state is
// unknown); reconnect to continue.

#include "connect/ps5/FtpListing.h"
#include "core/Cancellation.h"
#include "core/Result.h"

#include <QString>
#include <QStringList>

#include <functional>
#include <memory>
#include <vector>

class QIODevice;

namespace fw::ps5 {

struct FtpOptions {
    QString user;              // empty = anonymous
    QString password;
    int connectTimeoutMs = 10000;
    int replyTimeoutMs = 10000; // waiting for a command reply
    int idleTimeoutMs = 30000;  // a transfer that moves no byte for this long fails
    bool passive = true;        // false = active mode (PORT/EPRT)
};

struct FtpReply {
    int code = 0;
    QString text;      // all lines joined with '\n', codes stripped from the first/last line
    bool isPositive() const { return code >= 100 && code < 400; }
    bool isPreliminary() const { return code >= 100 && code < 200; }
};

struct FtpFeatures {
    bool mlsd = false, rest = false, size = false, mdtm = false, utf8 = false, xcrypt = false, copy = false;
    QString raw;
};

enum class FtpListMode { Auto, List, Mlsd };

// Called with the number of bytes moved since the last call.
using FtpProgress = std::function<void(qint64 delta)>;

class FtpClient {
public:
    explicit FtpClient(FtpOptions options = {});
    ~FtpClient(); // QUIT (best effort) and close
    FtpClient(const FtpClient&) = delete;
    FtpClient& operator=(const FtpClient&) = delete;

    const FtpOptions& options() const { return m_opt; }

    // Connects and reads the greeting.
    Status connectToHost(const QString& host, quint16 port, const CancellationToken& token = {});
    // USER/PASS; anonymous ("anonymous" / "anonymous@") when options().user is empty.
    Status login();
    // connectToHost() + login().
    Status open(const QString& host, quint16 port, const CancellationToken& token = {});
    bool isConnected() const;
    void quit();  // QUIT then close
    void abort(); // close at once
    QString welcome() const { return m_welcome; }
    QString host() const { return m_host; }
    quint16 port() const { return m_port; }
    const FtpReply& lastReply() const { return m_last; }
    // True when the last retrieve()/store() reached the server: the command
    // was accepted (1xx) or the connection broke before any reply. False when
    // the server refused it, so nothing on the console changed.
    bool lastTransferStarted() const { return m_transferStarted; }

    // One command, one reply (1xx replies are read through).
    Result<FtpReply> command(const QString& line);

    Result<FtpFeatures> features(); // FEAT, cached per connection
    Result<QString> pwd();
    Status cwd(const QString& path);
    Status mkd(const QString& path);
    // Creates every missing segment (errors on existing ones are ignored, like
    // the legacy ensure_dir); fails only when the final folder is not there.
    Status mkdirs(const QString& path);
    Status rmd(const QString& path);
    Status dele(const QString& path);
    Status rename(const QString& from, const QString& to);
    Result<qint64> size(const QString& path);
    Result<QDateTime> mdtm(const QString& path);
    Status noop();
    Result<QString> siteAvail(); // raw reply text of SITE AVAIL
    // True when `path` is an existing file (SIZE, then NLST of the parent).
    bool fileExists(const QString& path);
    // True when `path` is a folder (CWD into it and back).
    bool isDirectory(const QString& path);

    // Lists `path` (CWD into it first, like the legacy browser). Auto uses
    // MLSD when FEAT advertises it and falls back to LIST.
    Result<std::vector<FtpEntry>> list(const QString& path, FtpListMode mode = FtpListMode::Auto,
                                       const CancellationToken& token = {});
    Result<QStringList> nameList(const QString& path);

    // Downloads `remote` into `out` (already open for writing), starting at
    // byte `offset` (REST) when > 0.
    Status retrieve(const QString& remote, QIODevice& out, qint64 offset = 0, const FtpProgress& progress = {},
                    const CancellationToken& token = {});
    // Uploads `in` (already open; read from its current position) to
    // `remote`. offset > 0 resumes with REST + STOR; append = APPE.
    Status store(const QString& remote, QIODevice& in, qint64 offset = 0, const FtpProgress& progress = {},
                 const CancellationToken& token = {}, bool append = false);
    // Small helpers for whole files in memory.
    Result<QByteArray> download(const QString& remote, const CancellationToken& token = {});
    Status upload(const QString& remote, const QByteArray& data, const CancellationToken& token = {});
    // Legacy _ftp_upload_to safe swap: STOR to "<final><tempSuffix>", move an
    // existing final aside to "<final>.replacing.bak", rename the temp over the
    // final, delete the backup; on failure the temp is removed and the old
    // final restored.
    Status uploadReplace(const QString& remoteFinal, const QByteArray& data,
                         const QString& tempSuffix = QStringLiteral(".uploading"), const CancellationToken& token = {});
    // Removes a file or a whole folder tree (children first).
    Status removeTree(const QString& path, const CancellationToken& token = {});

private:
    struct Impl;
    std::unique_ptr<Impl> d;
    FtpOptions m_opt;
    QString m_host;
    quint16 m_port = 0;
    QString m_welcome;
    FtpReply m_last;
    bool m_transferStarted = false;
};

// User-facing error for a negative FTP reply to `what` ("Uploading x").
Error ftpReplyError(const FtpReply& reply, const QString& what);

} // namespace fw::ps5
