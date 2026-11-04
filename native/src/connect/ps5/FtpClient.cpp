#include "connect/ps5/FtpClient.h"

#include "connect/ps5/RemotePath.h"
#include "connect/ps5/SocketWait.h"
#include "core/Log.h"

#include <QBuffer>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QRegularExpression>
#include <QTcpServer>
#include <QTcpSocket>

#include <optional>

namespace fw::ps5 {

using detail::WaitResult;
using detail::waitUntil;

namespace {

constexpr qint64 kChunk = 256 * 1024;
constexpr qint64 kHighWater = 1024 * 1024;
constexpr int kSliceMs = 200;
constexpr qint64 kCheckpointBytes = 4 * 1024 * 1024;

const Log& ftpLog()
{
    static const Log log(QStringLiteral("ps5.ftp"));
    return log;
}

bool isDigits3(const QString& s)
{
    return s.size() >= 3 && s.at(0).isDigit() && s.at(1).isDigit() && s.at(2).isDigit();
}

} // namespace

Error ftpReplyError(const FtpReply& r, const QString& what)
{
    const QString detail = QStringLiteral("%1 %2").arg(r.code).arg(r.text.trimmed());
    switch (r.code) {
    case 530:
    case 331:
    case 332:
        return Error(ErrorCode::PermissionDenied, QStringLiteral("%1: the PS5 FTP server refused the login.").arg(what), detail);
    case 550:
        return Error(ErrorCode::NotFound,
                     QStringLiteral("%1: the PS5 FTP server rejected the path. The file or folder may not exist, or "
                                    "the server may not allow this there.")
                         .arg(what),
                     detail);
    case 552:
    case 452:
        return Error(ErrorCode::InsufficientSpace, QStringLiteral("%1: the console is out of space.").arg(what), detail);
    case 553:
        return Error(ErrorCode::PermissionDenied, QStringLiteral("%1: the PS5 FTP server does not allow that name or "
                                                                 "location.").arg(what), detail);
    case 500:
    case 501:
    case 502:
    case 504:
        return Error(ErrorCode::Unsupported, QStringLiteral("%1: the PS5 FTP server does not support this.").arg(what), detail);
    default:
        return Error(ErrorCode::IoError, QStringLiteral("%1: the PS5 FTP server answered \"%2\".").arg(what, detail), detail);
    }
}

struct FtpClient::Impl {
    std::unique_ptr<QTcpSocket> ctrl;
    std::optional<FtpFeatures> features;
    char type = 0;
    bool useEpsv = false;
};

FtpClient::FtpClient(FtpOptions options) : d(std::make_unique<Impl>()), m_opt(std::move(options)) {}

FtpClient::~FtpClient()
{
    if (isConnected())
        quit();
}

bool FtpClient::isConnected() const
{
    return d->ctrl && d->ctrl->state() == QAbstractSocket::ConnectedState;
}

void FtpClient::abort()
{
    if (d->ctrl)
        d->ctrl->abort();
    d->ctrl.reset();
    d->features.reset();
    d->type = 0;
}

namespace {

// Reads one complete reply from the control connection.
Result<FtpReply> readReplyOn(QTcpSocket& s, int timeoutMs, const CancellationToken& token)
{
    QStringList lines;
    int code = 0;
    bool done = false;
    while (!done) {
        while (!done && s.canReadLine()) {
            QString line = QString::fromUtf8(s.readLine());
            while (line.endsWith(u'\n') || line.endsWith(u'\r'))
                line.chop(1);
            if (code == 0) {
                if (!isDigits3(line))
                    continue; // stray text before a reply
                code = line.left(3).toInt();
                lines << line.mid(4);
                done = !(line.size() > 3 && line.at(3) == u'-');
            } else if (line.startsWith(QString::number(code) + u' ')) {
                lines << line.mid(4);
                done = true;
            } else {
                lines << (line.startsWith(QString::number(code) + u'-') ? line.mid(4) : line);
            }
        }
        if (done)
            break;
        const WaitResult w = waitUntil(
            [&] { return s.canReadLine() || s.state() != QAbstractSocket::ConnectedState; }, timeoutMs, token, {&s});
        if (w == WaitResult::Cancelled)
            return Error::cancelled();
        if (w == WaitResult::Timeout)
            return Error(ErrorCode::Timeout, QStringLiteral("The PS5 FTP server did not answer in time."),
                         QStringLiteral("no reply within %1 ms").arg(timeoutMs));
        if (!s.canReadLine() && s.state() != QAbstractSocket::ConnectedState)
            return Error(ErrorCode::IoError, QStringLiteral("The PS5 closed the FTP connection."), s.errorString());
    }
    FtpReply r;
    r.code = code;
    r.text = lines.join(u'\n');
    return r;
}

} // namespace

Status FtpClient::connectToHost(const QString& host, quint16 port, const CancellationToken& token)
{
    abort();
    m_host = host.trimmed();
    m_port = port;
    d->ctrl = std::make_unique<QTcpSocket>();
    d->ctrl->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    FW_TRY(detail::connectSocket(*d->ctrl, m_host, port, m_opt.connectTimeoutMs, token, QStringLiteral("FTP server")));
    // Greeting: 220 (possibly after 120 "ready in n minutes").
    while (true) {
        auto r = readReplyOn(*d->ctrl, m_opt.replyTimeoutMs, token);
        if (!r.ok()) {
            abort();
            return r.takeError();
        }
        m_last = r.value();
        if (m_last.code == 120)
            continue;
        if (m_last.code != 220) {
            abort();
            return ftpReplyError(m_last, QStringLiteral("Connecting to the PS5 FTP server"));
        }
        m_welcome = m_last.text;
        break;
    }
    ftpLog().debug(QStringLiteral("Connected to %1:%2 — %3").arg(m_host).arg(port).arg(m_welcome));
    return success();
}

Result<FtpReply> FtpClient::command(const QString& line)
{
    if (!isConnected())
        return Error(ErrorCode::IoError, QStringLiteral("Not connected to the PS5 FTP server."));
    ftpLog().debug(QStringLiteral("> %1").arg(line.startsWith(QLatin1String("PASS ")) ? QStringLiteral("PASS ****") : line));
    d->ctrl->write(line.toUtf8() + "\r\n");
    d->ctrl->flush();
    while (true) {
        auto r = readReplyOn(*d->ctrl, m_opt.replyTimeoutMs, {});
        if (!r.ok()) {
            abort();
            return r.takeError();
        }
        m_last = r.value();
        if (!m_last.isPreliminary())
            break;
    }
    ftpLog().trace(QStringLiteral("< %1 %2").arg(m_last.code).arg(m_last.text));
    return m_last;
}

Status FtpClient::login()
{
    const bool anon = m_opt.user.isEmpty();
    const QString user = anon ? QStringLiteral("anonymous") : m_opt.user;
    const QString pass = anon ? QStringLiteral("anonymous@") : m_opt.password;
    FW_TRY_ASSIGN(FtpReply r, command(QStringLiteral("USER ") + user));
    if (r.code == 331 || r.code == 332) {
        FW_TRY_ASSIGN(FtpReply p, command(QStringLiteral("PASS ") + pass));
        r = p;
    }
    if (r.code != 230 && r.code != 202)
        return ftpReplyError(r, QStringLiteral("Logging in"));
    return success();
}

Status FtpClient::open(const QString& host, quint16 port, const CancellationToken& token)
{
    FW_TRY(connectToHost(host, port, token));
    return login();
}

void FtpClient::quit()
{
    if (isConnected()) {
        d->ctrl->write("QUIT\r\n");
        d->ctrl->flush();
        (void)readReplyOn(*d->ctrl, 2000, {});
    }
    abort();
}

Result<FtpFeatures> FtpClient::features()
{
    if (d->features)
        return *d->features;
    FtpFeatures f;
    FW_TRY_ASSIGN(FtpReply r, command(QStringLiteral("FEAT")));
    if (r.code == 211) {
        f.raw = r.text;
        const QString up = r.text.toUpper();
        f.mlsd = up.contains(QLatin1String(" MLSD")) || up.contains(QLatin1String("\nMLSD"))
              || up.trimmed().endsWith(QLatin1String("MLSD")) || up.contains(QLatin1String("MLST"));
        f.rest = up.contains(QLatin1String("REST STREAM")) || up.contains(QLatin1String(" REST"));
        f.size = up.contains(QLatin1String(" SIZE")) || up.contains(QLatin1String("\nSIZE"));
        f.mdtm = up.contains(QLatin1String(" MDTM")) || up.contains(QLatin1String("\nMDTM"));
        f.utf8 = up.contains(QLatin1String("UTF8"));
        f.xcrypt = up.contains(QLatin1String("XCRYPT"));
        f.copy = up.contains(QLatin1String("CPFR")) || up.contains(QLatin1String("CPTO")) || up.contains(QLatin1String(" COPY"));
    }
    d->features = f;
    return f;
}

Result<QString> FtpClient::pwd()
{
    FW_TRY_ASSIGN(FtpReply r, command(QStringLiteral("PWD")));
    if (r.code != 257)
        return ftpReplyError(r, QStringLiteral("Reading the current folder"));
    static const QRegularExpression quoted(QStringLiteral("\"((?:[^\"]|\"\")*)\""));
    const auto m = quoted.match(r.text);
    return m.hasMatch() ? m.captured(1).replace(QLatin1String("\"\""), QLatin1String("\"")) : r.text.trimmed();
}

Status FtpClient::cwd(const QString& path)
{
    FW_TRY_ASSIGN(FtpReply r, command(QStringLiteral("CWD ") + path));
    if (r.code != 250 && r.code != 200)
        return ftpReplyError(r, QStringLiteral("Opening %1").arg(path));
    return success();
}

Status FtpClient::mkd(const QString& path)
{
    FW_TRY_ASSIGN(FtpReply r, command(QStringLiteral("MKD ") + path));
    if (r.code != 257 && r.code != 250)
        return ftpReplyError(r, QStringLiteral("Creating %1").arg(path));
    return success();
}

Status FtpClient::mkdirs(const QString& pathIn)
{
    const QString path = normalizeRemotePath(pathIn);
    if (path == u"/")
        return success();
    QString cur;
    Status last = success();
    for (const QString& part : path.split(u'/', Qt::SkipEmptyParts)) {
        cur += u'/' + part;
        last = mkd(cur);
        if (!last.ok() && !isConnected())
            return last;
    }
    if (last.ok() || isDirectory(path))
        return success();
    return last;
}

Status FtpClient::rmd(const QString& path)
{
    FW_TRY_ASSIGN(FtpReply r, command(QStringLiteral("RMD ") + path));
    if (r.code != 250 && r.code != 200)
        return ftpReplyError(r, QStringLiteral("Removing %1").arg(path));
    return success();
}

Status FtpClient::dele(const QString& path)
{
    FW_TRY_ASSIGN(FtpReply r, command(QStringLiteral("DELE ") + path));
    if (r.code != 250 && r.code != 200)
        return ftpReplyError(r, QStringLiteral("Deleting %1").arg(path));
    return success();
}

Status FtpClient::rename(const QString& from, const QString& to)
{
    FW_TRY_ASSIGN(FtpReply r, command(QStringLiteral("RNFR ") + from));
    if (r.code != 350)
        return ftpReplyError(r, QStringLiteral("Renaming %1").arg(from));
    FW_TRY_ASSIGN(FtpReply t, command(QStringLiteral("RNTO ") + to));
    if (t.code != 250 && t.code != 200)
        return ftpReplyError(t, QStringLiteral("Renaming %1").arg(from));
    return success();
}

namespace {

Status setTypeOn(FtpClient& c, char& current, char wanted)
{
    if (current == wanted)
        return success();
    FW_TRY_ASSIGN(FtpReply r, c.command(QStringLiteral("TYPE %1").arg(QChar::fromLatin1(wanted))));
    if (r.code != 200)
        return ftpReplyError(r, QStringLiteral("Switching transfer mode"));
    current = wanted;
    return success();
}

} // namespace

Result<qint64> FtpClient::size(const QString& path)
{
    FW_TRY(setTypeOn(*this, d->type, 'I'));
    FW_TRY_ASSIGN(FtpReply r, command(QStringLiteral("SIZE ") + path));
    if (r.code != 213)
        return ftpReplyError(r, QStringLiteral("Reading the size of %1").arg(path));
    bool ok = false;
    const qint64 n = r.text.trimmed().toLongLong(&ok);
    if (!ok)
        return Error(ErrorCode::CorruptData, QStringLiteral("The PS5 FTP server sent an unreadable size."), r.text);
    return n;
}

Result<QDateTime> FtpClient::mdtm(const QString& path)
{
    FW_TRY_ASSIGN(FtpReply r, command(QStringLiteral("MDTM ") + path));
    if (r.code != 213)
        return ftpReplyError(r, QStringLiteral("Reading the date of %1").arg(path));
    const QDateTime t = parseFtpTimestamp(r.text);
    if (!t.isValid())
        return Error(ErrorCode::CorruptData, QStringLiteral("The PS5 FTP server sent an unreadable date."), r.text);
    return t;
}

Status FtpClient::noop()
{
    FW_TRY_ASSIGN(FtpReply r, command(QStringLiteral("NOOP")));
    if (r.code != 200)
        return ftpReplyError(r, QStringLiteral("Checking the connection"));
    return success();
}

Result<QString> FtpClient::siteAvail()
{
    FW_TRY_ASSIGN(FtpReply r, command(QStringLiteral("SITE AVAIL")));
    if (!r.isPositive())
        return ftpReplyError(r, QStringLiteral("Reading free space"));
    return r.text.trimmed();
}

bool FtpClient::isDirectory(const QString& path)
{
    auto here = pwd();
    if (!here.ok())
        return false;
    if (!cwd(path).ok())
        return false;
    (void)cwd(here.value());
    return true;
}

bool FtpClient::fileExists(const QString& path)
{
    if (size(path).ok())
        return true;
    if (!isConnected())
        return false;
    auto names = nameList(remoteParent(path));
    if (!names.ok())
        return false;
    const QString base = remoteFileName(path);
    for (const QString& n : names.value())
        if (n.mid(n.lastIndexOf(u'/') + 1) == base)
            return true;
    return false;
}

// ---- data connections ----------------------------------------------------------------

namespace {

struct Transfer {
    std::unique_ptr<QTcpServer> server;
    std::unique_ptr<QTcpSocket> owned;
    QTcpSocket* sock = nullptr;
};

} // namespace

// Opens the data connection, sends `cmd` and waits for the 1xx reply.
static Result<Transfer> startTransfer(FtpClient& c, QTcpSocket& ctrl, bool& useEpsv, char& type, char wantType,
                                      const QString& cmd, qint64 offset, const FtpOptions& opt,
                                      const CancellationToken& token, bool* started = nullptr)
{
    if (started)
        *started = false;
    FW_TRY(setTypeOn(c, type, wantType));
    Transfer t;
    if (opt.passive) {
        quint16 port = 0;
        if (!useEpsv) {
            FW_TRY_ASSIGN(FtpReply r, c.command(QStringLiteral("PASV")));
            if (r.code == 227) {
                static const QRegularExpression re(QStringLiteral("(\\d+),(\\d+),(\\d+),(\\d+),(\\d+),(\\d+)"));
                const auto m = re.match(r.text);
                if (!m.hasMatch())
                    return Error(ErrorCode::CorruptData, QStringLiteral("The PS5 FTP server sent an unreadable "
                                                                        "passive-mode reply."), r.text);
                port = quint16(m.captured(5).toInt() * 256 + m.captured(6).toInt());
            } else {
                useEpsv = true;
            }
        }
        if (useEpsv) {
            FW_TRY_ASSIGN(FtpReply r, c.command(QStringLiteral("EPSV")));
            static const QRegularExpression re(QStringLiteral("\\((.)\\1\\1(\\d+)\\1\\)"));
            const auto m = re.match(r.text);
            if (r.code != 229 || !m.hasMatch())
                return ftpReplyError(r, QStringLiteral("Opening a data connection (passive mode)"));
            port = quint16(m.captured(2).toInt());
        }
        // Like ftplib: connect to the control connection's host, not the
        // address in the PASV reply (servers behind NAT often send 0.0.0.0).
        t.owned = std::make_unique<QTcpSocket>();
        FW_TRY(detail::connectSocket(*t.owned, ctrl.peerAddress().toString(), port, opt.connectTimeoutMs, token,
                                     QStringLiteral("FTP data connection")));
        t.sock = t.owned.get();
    } else {
        t.server = std::make_unique<QTcpServer>();
        if (!t.server->listen(ctrl.localAddress(), 0))
            return Error(ErrorCode::IoError, QStringLiteral("Could not open a port for an active-mode FTP transfer. "
                                                            "Turn on passive mode in Settings."),
                         t.server->errorString());
        const QHostAddress a = ctrl.localAddress();
        const quint16 p = t.server->serverPort();
        bool ok4 = false;
        const quint32 v4 = a.toIPv4Address(&ok4);
        const QString portCmd = ok4 ? QStringLiteral("PORT %1,%2,%3,%4,%5,%6")
                                          .arg((v4 >> 24) & 0xFF).arg((v4 >> 16) & 0xFF).arg((v4 >> 8) & 0xFF)
                                          .arg(v4 & 0xFF).arg(p >> 8).arg(p & 0xFF)
                                    : QStringLiteral("EPRT |2|%1|%2|").arg(a.toString()).arg(p);
        FW_TRY_ASSIGN(FtpReply r, c.command(portCmd));
        if (r.code != 200)
            return ftpReplyError(r, QStringLiteral("Opening a data connection (active mode)"));
    }
    if (offset > 0) {
        FW_TRY_ASSIGN(FtpReply r, c.command(QStringLiteral("REST %1").arg(offset)));
        if (r.code != 350)
            return Error(ErrorCode::Unsupported, QStringLiteral("The PS5 FTP server cannot resume transfers."),
                         QStringLiteral("%1 %2").arg(r.code).arg(r.text));
    }
    ftpLog().debug(QStringLiteral("> %1").arg(cmd));
    ctrl.write(cmd.toUtf8() + "\r\n");
    ctrl.flush();
    // From here on the server may have created or truncated the target.
    if (started)
        *started = true;
    FW_TRY_ASSIGN(FtpReply r, readReplyOn(ctrl, opt.replyTimeoutMs, token));
    if (r.code >= 200 && r.code < 300) { // ftplib: a 2xx first, then the 1xx
        FW_TRY_ASSIGN(FtpReply r2, readReplyOn(ctrl, opt.replyTimeoutMs, token));
        r = r2;
    }
    if (!r.isPreliminary() && started)
        *started = false; // refused: nothing changed on the console
    if (!r.isPreliminary())
        return ftpReplyError(r, cmd.section(u' ', 0, 0) == u"STOR" || cmd.startsWith(QLatin1String("APPE"))
                                    ? QStringLiteral("Uploading %1").arg(cmd.section(u' ', 1))
                                : cmd.startsWith(QLatin1String("RETR")) ? QStringLiteral("Downloading %1").arg(cmd.section(u' ', 1))
                                                                       : QStringLiteral("Listing"));
    if (!opt.passive) {
        QTcpServer* srv = t.server.get();
        const WaitResult w = waitUntil([&] { return srv->hasPendingConnections(); }, opt.connectTimeoutMs, token, {srv});
        if (w == WaitResult::Cancelled)
            return Error::cancelled();
        if (w != WaitResult::Ready)
            return Error(ErrorCode::Timeout, QStringLiteral("The PS5 did not open the active-mode data connection. "
                                                            "Turn on passive mode in Settings."));
        t.sock = srv->nextPendingConnection();
    }
    return t;
}

Status FtpClient::retrieve(const QString& remote, QIODevice& out, qint64 offset, const FtpProgress& progress,
                           const CancellationToken& token)
{
    if (!isConnected())
        return Error(ErrorCode::IoError, QStringLiteral("Not connected to the PS5 FTP server."));
    auto t = startTransfer(*this, *d->ctrl, d->useEpsv, d->type, 'I', QStringLiteral("RETR ") + remote, offset, m_opt,
                           token, &m_transferStarted);
    if (!t.ok()) {
        if (t.error().isCancelled() || t.error().code == ErrorCode::Timeout)
            abort();
        return t.takeError();
    }
    QTcpSocket* s = t->sock;
    QElapsedTimer idle;
    idle.start();
    qint64 sinceCheckpoint = 0;
    while (true) {
        while (s->bytesAvailable() > 0) {
            const QByteArray chunk = s->read(std::min<qint64>(s->bytesAvailable(), kHighWater * 4));
            if (out.write(chunk) != chunk.size()) {
                abort();
                return Error(ErrorCode::IoError, QStringLiteral("Could not write the downloaded data to disk."),
                             out.errorString());
            }
            if (progress)
                progress(chunk.size());
            sinceCheckpoint += chunk.size();
            idle.restart();
        }
        if (sinceCheckpoint >= kCheckpointBytes) {
            sinceCheckpoint = 0;
            if (!token.checkpoint()) {
                abort();
                return Error::cancelled();
            }
        }
        if (s->state() != QAbstractSocket::ConnectedState && s->bytesAvailable() == 0)
            break;
        const WaitResult w = waitUntil(
            [&] { return s->bytesAvailable() > 0 || s->state() != QAbstractSocket::ConnectedState; }, kSliceMs, token,
            {s});
        if (w == WaitResult::Cancelled) {
            abort();
            return Error::cancelled();
        }
        if (w == WaitResult::Timeout && idle.elapsed() >= m_opt.idleTimeoutMs) {
            abort();
            return Error(ErrorCode::Timeout,
                         QStringLiteral("The download stalled: nothing arrived for %1 s.").arg(m_opt.idleTimeoutMs / 1000));
        }
    }
    auto r = readReplyOn(*d->ctrl, m_opt.idleTimeoutMs, token);
    if (!r.ok()) {
        abort();
        return r.takeError();
    }
    m_last = r.value();
    if (m_last.code < 200 || m_last.code >= 300)
        return ftpReplyError(m_last, QStringLiteral("Downloading %1").arg(remote));
    return success();
}

Status FtpClient::store(const QString& remote, QIODevice& in, qint64 offset, const FtpProgress& progress,
                        const CancellationToken& token, bool append)
{
    if (!isConnected())
        return Error(ErrorCode::IoError, QStringLiteral("Not connected to the PS5 FTP server."));
    if (offset > 0 && !in.isSequential() && !in.seek(offset))
        return Error(ErrorCode::IoError, QStringLiteral("Could not read the local file."), in.errorString());
    const QString cmd = (append ? QStringLiteral("APPE ") : QStringLiteral("STOR ")) + remote;
    auto t = startTransfer(*this, *d->ctrl, d->useEpsv, d->type, 'I', cmd, append ? 0 : offset, m_opt, token,
                           &m_transferStarted);
    if (!t.ok()) {
        if (t.error().isCancelled() || t.error().code == ErrorCode::Timeout)
            abort();
        return t.takeError();
    }
    QTcpSocket* s = t->sock;
    qint64 queued = 0, reported = 0, sinceCheckpoint = 0;
    QElapsedTimer idle;
    idle.start();
    auto report = [&] {
        const qint64 sent = queued - s->bytesToWrite();
        if (sent > reported) {
            if (progress)
                progress(sent - reported);
            reported = sent;
            idle.restart();
        }
    };
    // Waits until the socket buffer drops below `level` (or the peer goes away).
    auto drain = [&](qint64 level) -> Status {
        while (s->bytesToWrite() > level && s->state() == QAbstractSocket::ConnectedState) {
            const WaitResult w = waitUntil(
                [&] { return s->bytesToWrite() <= level || s->state() != QAbstractSocket::ConnectedState; }, kSliceMs,
                token, {s});
            report();
            if (w == WaitResult::Cancelled)
                return Error::cancelled();
            if (w == WaitResult::Timeout && idle.elapsed() >= m_opt.idleTimeoutMs)
                return Error(ErrorCode::Timeout,
                             QStringLiteral("The upload stalled: the PS5 accepted nothing for %1 s.").arg(m_opt.idleTimeoutMs / 1000));
        }
        return success();
    };
    bool peerClosed = false;
    while (true) {
        if (s->state() != QAbstractSocket::ConnectedState) {
            peerClosed = true;
            break;
        }
        if (auto st = drain(kHighWater); !st.ok()) {
            abort();
            return st;
        }
        const QByteArray chunk = in.read(kChunk);
        if (chunk.isEmpty()) {
            if (!in.atEnd()) {
                abort();
                return Error(ErrorCode::IoError, QStringLiteral("Could not read the local file."), in.errorString());
            }
            break;
        }
        s->write(chunk);
        s->flush();
        queued += chunk.size();
        sinceCheckpoint += chunk.size();
        if (sinceCheckpoint >= kCheckpointBytes) {
            sinceCheckpoint = 0;
            if (!token.checkpoint()) {
                abort();
                return Error::cancelled();
            }
        }
    }
    if (!peerClosed) {
        if (auto st = drain(0); !st.ok()) {
            abort();
            return st;
        }
        report();
        s->disconnectFromHost();
        if (s->state() != QAbstractSocket::UnconnectedState) {
            const WaitResult w = waitUntil([&] { return s->state() == QAbstractSocket::UnconnectedState; },
                                           m_opt.idleTimeoutMs, token, {s});
            if (w == WaitResult::Cancelled) {
                abort();
                return Error::cancelled();
            }
        }
    }
    auto r = readReplyOn(*d->ctrl, m_opt.idleTimeoutMs, token);
    if (!r.ok()) {
        abort();
        return r.takeError();
    }
    m_last = r.value();
    if (m_last.code < 200 || m_last.code >= 300)
        return ftpReplyError(m_last, QStringLiteral("Uploading %1").arg(remote));
    return success();
}

namespace {

Result<QByteArray> transferText(FtpClient& c, QTcpSocket& ctrl, bool& useEpsv, char& type, const QString& cmd,
                                const FtpOptions& opt, const CancellationToken& token, FtpReply& last)
{
    auto t = startTransfer(c, ctrl, useEpsv, type, 'A', cmd, 0, opt, token);
    if (!t.ok())
        return t.takeError();
    QTcpSocket* s = t->sock;
    QByteArray data;
    QElapsedTimer idle;
    idle.start();
    while (true) {
        if (s->bytesAvailable() > 0) {
            data += s->readAll();
            idle.restart();
        }
        if (s->state() != QAbstractSocket::ConnectedState && s->bytesAvailable() == 0)
            break;
        const WaitResult w = waitUntil(
            [&] { return s->bytesAvailable() > 0 || s->state() != QAbstractSocket::ConnectedState; }, kSliceMs, token, {s});
        if (w == WaitResult::Cancelled)
            return Error::cancelled();
        if (w == WaitResult::Timeout && idle.elapsed() >= opt.idleTimeoutMs)
            return Error(ErrorCode::Timeout, QStringLiteral("The folder listing stalled."));
    }
    FW_TRY_ASSIGN(FtpReply r, readReplyOn(ctrl, opt.idleTimeoutMs, token));
    last = r;
    if (r.code < 200 || r.code >= 300)
        return ftpReplyError(r, QStringLiteral("Listing"));
    return data;
}

} // namespace

Result<std::vector<FtpEntry>> FtpClient::list(const QString& path, FtpListMode mode, const CancellationToken& token)
{
    if (!path.isEmpty())
        FW_TRY(cwd(path));
    bool mlsd = mode == FtpListMode::Mlsd;
    if (mode == FtpListMode::Auto) {
        auto f = features();
        if (!f.ok())
            return f.takeError();
        mlsd = f.value().mlsd;
    }
    if (mlsd) {
        auto data = transferText(*this, *d->ctrl, d->useEpsv, d->type, QStringLiteral("MLSD"), m_opt, token, m_last);
        if (data.ok())
            return parseListing(data.value(), true);
        if (mode == FtpListMode::Mlsd || !isConnected() || data.error().isCancelled())
            return data.takeError();
        // Legacy: MLSD failed, try LIST.
    }
    auto data = transferText(*this, *d->ctrl, d->useEpsv, d->type, QStringLiteral("LIST"), m_opt, token, m_last);
    if (!data.ok()) {
        if (data.error().isCancelled() || data.error().code == ErrorCode::Timeout)
            abort();
        return data.takeError();
    }
    return parseListing(data.value(), false);
}

Result<QStringList> FtpClient::nameList(const QString& path)
{
    const QString cmd = path.isEmpty() ? QStringLiteral("NLST") : QStringLiteral("NLST ") + path;
    FW_TRY_ASSIGN(QByteArray data, transferText(*this, *d->ctrl, d->useEpsv, d->type, cmd, m_opt, {}, m_last));
    QStringList out;
    for (const QByteArray& l : data.split('\n')) {
        const QString s = QString::fromUtf8(l).trimmed();
        if (!s.isEmpty())
            out << s;
    }
    return out;
}

Result<QByteArray> FtpClient::download(const QString& remote, const CancellationToken& token)
{
    QByteArray bytes;
    QBuffer buf(&bytes);
    buf.open(QIODevice::WriteOnly);
    FW_TRY(retrieve(remote, buf, 0, {}, token));
    return bytes;
}

Status FtpClient::upload(const QString& remote, const QByteArray& data, const CancellationToken& token)
{
    QByteArray copy = data;
    QBuffer buf(&copy);
    buf.open(QIODevice::ReadOnly);
    return store(remote, buf, 0, {}, token);
}

Status FtpClient::uploadReplace(const QString& remoteFinal, const QByteArray& data, const QString& tempSuffix,
                                const CancellationToken& token)
{
    const QString finalPath = normalizeRemotePath(remoteFinal);
    const QString temp = finalPath + tempSuffix;
    const QString bak = finalPath + QStringLiteral(".replacing.bak");
    (void)mkdirs(remoteParent(finalPath));
    if (auto st = upload(temp, data, token); !st.ok()) {
        if (isConnected())
            (void)dele(temp);
        return st;
    }
    bool movedAside = false;
    if (fileExists(finalPath)) {
        (void)dele(bak);
        if (auto st = rename(finalPath, bak); !st.ok()) {
            (void)dele(temp);
            return Error(ErrorCode::Unsupported, QStringLiteral("The PS5 FTP server refused to rename the old file, "
                                                                "so it was left unchanged."),
                         st.error().detail);
        }
        movedAside = true;
    }
    if (auto st = rename(temp, finalPath); !st.ok()) {
        if (movedAside)
            (void)rename(bak, finalPath);
        (void)dele(temp);
        return Error(ErrorCode::Unsupported, QStringLiteral("The PS5 FTP server refused to rename the uploaded file; "
                                                            "the old file was kept."),
                     st.error().detail);
    }
    if (movedAside)
        (void)dele(bak);
    return success();
}

Status FtpClient::removeTree(const QString& pathIn, const CancellationToken& token)
{
    const QString path = normalizeRemotePath(pathIn);
    if (path == u"/")
        return Error(ErrorCode::InvalidArgument, QStringLiteral("Refusing to delete the console's root folder."));
    if (!isDirectory(path))
        return dele(path);
    FW_TRY_ASSIGN(std::vector<FtpEntry> entries, list(path, FtpListMode::Auto, token));
    for (const FtpEntry& e : entries) {
        if (!token.checkpoint())
            return Error::cancelled();
        const QString child = joinRemote(path, e.name);
        if (e.isDir && !e.isLink)
            FW_TRY(removeTree(child, token));
        else
            FW_TRY(dele(child));
    }
    FW_TRY(cwd(QStringLiteral("/")));
    return rmd(path);
}

} // namespace fw::ps5
