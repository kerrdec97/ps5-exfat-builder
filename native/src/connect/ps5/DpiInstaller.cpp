#include "connect/ps5/DpiInstaller.h"

#include "connect/ps5/SocketWait.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTcpSocket>

#include <algorithm>

namespace fw::ps5 {

using detail::WaitResult;
using detail::waitUntil;

namespace {

const QByteArray kBoundary = QByteArrayLiteral("X-INSOMNIA-BOUNDARY");

// Parses "HTTP/1.1 200 OK\r\nHeaders...\r\n\r\nbody". Returns false while incomplete.
struct HttpParse {
    bool headDone = false;
    int status = 0;
    qint64 contentLength = -1;
    bool chunked = false;
    qsizetype bodyStart = 0;
};

HttpParse parseHead(const QByteArray& buf)
{
    HttpParse p;
    const qsizetype end = buf.indexOf("\r\n\r\n");
    if (end < 0)
        return p;
    p.headDone = true;
    p.bodyStart = end + 4;
    const QList<QByteArray> lines = buf.left(end).split('\n');
    if (!lines.isEmpty()) {
        const QList<QByteArray> parts = lines.front().trimmed().split(' ');
        if (parts.size() >= 2)
            p.status = parts.at(1).toInt();
    }
    for (qsizetype i = 1; i < lines.size(); ++i) {
        const QByteArray l = lines.at(i).trimmed();
        const qsizetype colon = l.indexOf(':');
        if (colon < 0)
            continue;
        const QByteArray k = l.left(colon).trimmed().toLower();
        const QByteArray v = l.mid(colon + 1).trimmed();
        if (k == "content-length")
            p.contentLength = v.toLongLong();
        else if (k == "transfer-encoding" && v.toLower().contains("chunked"))
            p.chunked = true;
    }
    return p;
}

// Returns true and the decoded body once the final chunk has arrived.
bool decodeChunked(const QByteArray& raw, QByteArray* out)
{
    QByteArray body;
    qsizetype pos = 0;
    while (true) {
        const qsizetype nl = raw.indexOf("\r\n", pos);
        if (nl < 0)
            return false;
        bool ok = false;
        const qint64 n = raw.mid(pos, nl - pos).split(';').front().trimmed().toLongLong(&ok, 16);
        // A negative or absurd size (a hostile or broken peer) must not loop or allocate without bound.
        if (!ok || n < 0 || n > 64LL * 1024 * 1024)
            return false;
        pos = nl + 2;
        if (n == 0) {
            *out = body;
            return true;
        }
        if (qint64(raw.size()) - qint64(pos) < n + 2)
            return false;
        body += raw.mid(pos, n);
        pos += n + 2;
    }
}

QString safeFileName(const QString& name)
{
    QString n = name;
    n.replace(u'"', QStringLiteral("%22"));
    n.replace(u'\r', u'_').replace(u'\n', u'_');
    return n;
}

} // namespace

bool dpiAccepted(int status, const QByteArray& body)
{
    if (status < 200 || status >= 300)
        return false;
    const QByteArray low = body.trimmed().toLower();
    if (low.startsWith("success"))
        return true;
    if (low.isEmpty() || low.contains("fail") || low.contains("error") || low.contains("no data"))
        return false;
    return low.contains("success");
}

QString dpiFailureReason(int status, const QByteArray& body)
{
    const QString text = QString::fromUtf8(body).trimmed();
    if (text.isEmpty())
        return status >= 200 && status < 300 ? QStringLiteral("the installer gave an empty answer.")
                                             : QStringLiteral("HTTP %1 without an explanation.").arg(status);
    if (text.contains(QLatin1String("No data received"), Qt::CaseInsensitive))
        return QStringLiteral("the installer received no package data.");
    // "FAILED: Install failed with error <name>, code <n> (0x...) for file: ..."
    static const QRegularExpression code(QStringLiteral("error\\s+(\\S+?),?\\s+code\\s+(-?\\d+)\\s*\\((0x[0-9a-fA-F]+)\\)"),
                                         QRegularExpression::CaseInsensitiveOption);
    if (const auto m = code.match(text); m.hasMatch())
        return QStringLiteral("%1 (code %2, %3).").arg(m.captured(1), m.captured(2), m.captured(3));
    QString first = text.section(u'\n', 0, 0).left(160);
    if (status >= 300 || status < 200)
        first = QStringLiteral("HTTP %1: %2").arg(status).arg(first);
    return first.endsWith(u'.') ? first : first + u'.';
}

Status clearDpiTemp(const QString& host, quint16 port, const DpiOptions& opt)
{
    QTcpSocket s;
    FW_TRY(detail::connectSocket(s, host, port, opt.connectTimeoutMs, {}, QStringLiteral("Direct Package Installer")));
    s.write("POST /cleartmp HTTP/1.1\r\nHost: " + host.toUtf8() + ':' + QByteArray::number(port)
            + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
    s.flush();
    QByteArray buf;
    const WaitResult w = waitUntil(
        [&] {
            buf += s.readAll();
            return buf.contains("\r\n\r\n") || s.state() != QAbstractSocket::ConnectedState;
        },
        5000, {}, {&s});
    s.abort();
    if (w != WaitResult::Ready || !buf.startsWith("HTTP/"))
        return Error(ErrorCode::IoError, QStringLiteral("The installer did not answer /cleartmp."));
    return success();
}

QByteArray dpiPartHeader(const QString& fileName)
{
    return "--" + kBoundary + "\r\nContent-Disposition: form-data; name=\"file\"; filename=\""
         + safeFileName(fileName).toUtf8() + "\"\r\nContent-Type: application/octet-stream\r\n\r\n";
}

QByteArray dpiPartFooter()
{
    return "\r\n--" + kBoundary + "--\r\n";
}

QByteArray dpiRequestHead(const QString& host, quint16 port, const QString& fileName, qint64 fileSize)
{
    const qint64 total = dpiPartHeader(fileName).size() + fileSize + dpiPartFooter().size();
    // Connection: close, so a reply without Content-Length ends when the
    // server closes instead of after the idle timeout.
    return "POST /upload HTTP/1.1\r\nHost: " + host.toUtf8() + ':' + QByteArray::number(port)
         + "\r\nConnection: close\r\nAccept-Encoding: identity\r\nContent-Type: multipart/form-data; boundary=" + kBoundary
         + "\r\nContent-Length: " + QByteArray::number(total) + "\r\n\r\n";
}

Result<DpiResponse> installPackage(const QString& host, quint16 port, const QString& pkgPath, const DpiOptions& opt,
                                   const CancellationToken& token,
                                   const std::function<void(qint64, qint64)>& progress)
{
    QFile f(pkgPath);
    if (!QFileInfo(pkgPath).isFile())
        return Error(ErrorCode::NotFound,
                     QStringLiteral("The package %1 was not found.").arg(QDir::toNativeSeparators(pkgPath)));
    if (!f.open(QIODevice::ReadOnly))
        return Error(ErrorCode::IoError, QStringLiteral("The package could not be opened."), f.errorString());
    const qint64 size = f.size();
    const QString name = QFileInfo(pkgPath).fileName();

    QTcpSocket s;
    FW_TRY(detail::connectSocket(s, host, port, opt.connectTimeoutMs, token,
                                 QStringLiteral("Direct Package Installer (turn on DPI_v2=1 in /data/etaHEN/config.ini)")));
    const QByteArray prefix = dpiRequestHead(host, port, name, size) + dpiPartHeader(name);
    s.write(prefix);

    const qint64 chunk = std::max(4096, opt.chunkSize);
    qint64 queued = prefix.size(), reported = 0; // every byte queued on the socket
    QElapsedTimer idle;
    idle.start();
    auto failSend = [&](Error e) -> Result<DpiResponse> {
        s.abort();
        return e;
    };
    auto drain = [&](qint64 level) -> Status {
        while (s.bytesToWrite() > level) {
            if (s.state() != QAbstractSocket::ConnectedState)
                return Error(ErrorCode::IoError, QStringLiteral("The PS5 closed the connection during the package "
                                                                "upload. Make sure etaHEN runs with Direct Package "
                                                                "Installer V2 on (DPI_v2=1 in /data/etaHEN/config.ini, "
                                                                "or the etaHEN toolbox)."),
                             s.errorString());
            const WaitResult w = waitUntil(
                [&] { return s.bytesToWrite() <= level || s.state() != QAbstractSocket::ConnectedState; }, 200, token,
                {&s});
            const qint64 sent = std::clamp<qint64>(queued - s.bytesToWrite() - prefix.size(), 0, size);
            if (sent > reported) {
                reported = sent;
                idle.restart();
                if (progress)
                    progress(sent, size);
            }
            if (w == WaitResult::Cancelled)
                return Error::cancelled();
            if (w == WaitResult::Timeout && idle.elapsed() >= opt.idleTimeoutMs)
                return Error(ErrorCode::Timeout, QStringLiteral("The package upload stalled (nothing sent for %1 s).")
                                                     .arg(opt.idleTimeoutMs / 1000));
        }
        return success();
    };
    // Exactly the `size` bytes the request announced, never to the end of a
    // file that is still growing.
    qint64 left = size;
    while (left > 0) {
        if (auto st = drain(chunk * 2); !st.ok())
            return failSend(st.takeError());
        // A closed socket has an empty write buffer, so drain() returns at
        // once: without these checks the rest of the package would be read
        // from disk for nothing, with no way to stop it.
        if (token.isCancelled())
            return failSend(Error::cancelled());
        if (s.state() != QAbstractSocket::ConnectedState)
            break; // the reply (or the lack of one) below says why
        const QByteArray b = f.read(std::min(chunk, left));
        if (b.isEmpty()) {
            if (!f.atEnd())
                return failSend(Error(ErrorCode::IoError, QStringLiteral("Could not read the package."), f.errorString()));
            return failSend(Error(ErrorCode::IoError, QStringLiteral("The package file changed while it was being sent. "
                                                                     "Wait until it is complete, then try again.")));
        }
        s.write(b);
        s.flush();
        queued += b.size();
        left -= b.size();
    }
    const QByteArray footer = dpiPartFooter();
    s.write(footer);
    queued += footer.size();
    if (auto st = drain(0); !st.ok())
        return failSend(st.takeError());
    const bool sentAll = left == 0; // false: the console closed the connection part-way
    if (progress && sentAll)
        progress(size, size);

    // Wait for the reply. It is a line or two of text: whatever sends more is
    // not the installer and must not grow the buffer (or reset the idle
    // timer) for ever.
    constexpr qsizetype kMaxReplyBytes = 1024 * 1024;
    QByteArray buf;
    HttpParse head;
    idle.restart();
    while (true) {
        if (s.bytesAvailable() > 0) {
            buf += s.readAll();
            idle.restart();
            if (buf.size() > kMaxReplyBytes)
                return failSend(Error(ErrorCode::CorruptData,
                                      QStringLiteral("The PS5 sent an answer that is not the package installer's. Check "
                                                     "the Direct Package Installer port in Settings.")));
        }
        head = parseHead(buf);
        if (head.headDone) {
            const QByteArray raw = buf.mid(head.bodyStart);
            QByteArray body;
            bool complete = head.chunked ? decodeChunked(raw, &body)
                          : head.contentLength >= 0 ? raw.size() >= head.contentLength
                                                    : false;
            // No length and no closing (a kept-alive server): an answer that
            // already reads as accepted, quiet for a second, is the answer.
            if (!complete && !head.chunked && head.contentLength < 0 && idle.elapsed() >= 1000 && dpiAccepted(head.status, raw))
                complete = true;
            if (complete || (s.state() != QAbstractSocket::ConnectedState && s.bytesAvailable() == 0)) {
                DpiResponse r;
                r.status = head.status;
                r.body = head.chunked ? body : head.contentLength >= 0 ? raw.left(head.contentLength) : raw;
                // An answer to a package that was not sent whole is never an acceptance.
                r.accepted = sentAll && dpiAccepted(r.status, r.body);
                s.abort();
                return r;
            }
        }
        if (s.state() != QAbstractSocket::ConnectedState && s.bytesAvailable() == 0)
            return Error(ErrorCode::IoError, QStringLiteral("The PS5 closed the connection without answering the "
                                                            "package upload."));
        const WaitResult w = waitUntil(
            [&] { return s.bytesAvailable() > 0 || s.state() != QAbstractSocket::ConnectedState; }, 200, token, {&s});
        if (w == WaitResult::Cancelled)
            return failSend(Error::cancelled());
        if (w == WaitResult::Timeout && idle.elapsed() >= opt.idleTimeoutMs)
            return failSend(Error(ErrorCode::Timeout, QStringLiteral("The PS5 did not confirm the package within %1 s.")
                                                          .arg(opt.idleTimeoutMs / 1000)));
    }
}

} // namespace fw::ps5
