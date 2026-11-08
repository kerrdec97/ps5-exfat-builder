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
    // ps5-payload-dev ftpsrv: FEAT lists only MLSD KILL MTRW SELF, although
    // REST and SIZE work (srv.c/cmd.c), so those are set too.
    bool ftpsrv = false;
    QString raw;
};

// Which console FTP server answered, from the greeting (and FEAT when known):
//   ftpsrv  "220-Welcome to ftpsrv.elf running on pid N" / "220-Version: v0.21.1 (built ...)"
//   etaHEN  "220 Service ready for new user." (port 1337)
//   zftpd   a banner naming zftpd, or FEAT with XCRYPT / server-side copy
struct FtpServerIdentity {
    QString name;    // "ftpsrv", "etaHEN", "zftpd"; empty = unknown
    QString version; // "v0.21.1" when the greeting says it
    QString label() const; // "ftpsrv v0.21.1", "etaHEN FTP", "FTP server"
};
FtpServerIdentity identifyFtpServer(const QString& welcome, const FtpFeatures* features = nullptr);

// ftpsrv's per-connection SELF->ELF decryption (on by default): SIZE and RETR
// of a file that starts with SELF magic (4F 15 3D 1D / 54 14 F5 EE: fself
// eboot.bin, .sprx, .prx) return the decrypted ELF, so a size check after an
// upload fails and a backup gets other bytes than the console holds. The
// client sends SELF after every ftpsrv login to turn it off.
enum class SelfMode { NotApplicable, Disabled, StillEnabled };

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
    // connectToHost() + login(), then (ftpsrv only) SELF to turn off the
    // SELF decryption mode, checked from the reply (see SelfMode).
    Status open(const QString& host, quint16 port, const CancellationToken& token = {});
    SelfMode selfMode() const { return m_selfMode; }
    FtpServerIdentity serverIdentity() const { return identifyFtpServer(m_welcome); }
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
    // True only when the server accepted the last retrieve()/store() command
    // (a 1xx reply arrived), i.e. it really opened the target. Resume offsets
    // must only be trusted after this; lastTransferStarted() may also be true
    // when the connection broke right after the command was written.
    bool lastTransferAccepted() const { return m_transferAccepted; }

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
    // Raw reply text of SITE AVAIL. No current console server implements it
    // (ftpsrv 502, etaHEN 504, zftpd: not listed); best effort only.
    Result<QString> siteAvail();
    // True when `path` is an existing file (SIZE, then a LIST/MLSD of the
    // parent: ftpsrv answers 502 to NLST; NLST is the last resort).
    bool fileExists(const QString& path);
    // The size of `path` as its parent folder's listing reports it (stat
    // based on every console server, also when SIZE is not). NotFound when
    // the listing has no such file.
    Result<qint64> listedSize(const QString& path);
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
    SelfMode m_selfMode = SelfMode::NotApplicable;
    FtpReply m_last;
    bool m_transferStarted = false;
    bool m_transferAccepted = false;
};

// User-facing error for a negative FTP reply to `what` ("Uploading x").
Error ftpReplyError(const FtpReply& reply, const QString& what);

} // namespace fw::ps5
