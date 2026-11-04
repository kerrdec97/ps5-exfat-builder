#include "connect/ps5/DpiInstaller.h"

#include "connect/ps5/SocketWait.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
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
        if (!ok)
            return false;
        pos = nl + 2;
        if (n == 0) {
            *out = body;
            return true;
        }
        if (raw.size() < pos + n + 2)
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
    const QByteArray low = body.toLower();
    if (status >= 400 || low.contains("fail") || low.contains("error"))
        return false;
    return status == 200 || (status >= 200 && status < 300) || low.contains("success") || low.contains("install");
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
    return "POST /upload HTTP/1.1\r\nHost: " + host.toUtf8() + ':' + QByteArray::number(port)
         + "\r\nAccept-Encoding: identity\r\nContent-Type: multipart/form-data; boundary=" + kBoundary
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
                                 QStringLiteral("Direct Package Installer (enable DPI V2 in etaHEN Services)")));
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
                                                                "upload. Make sure etaHEN is running with Direct "
                                                                "Package Installer V2 enabled in Services."),
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
    while (true) {
        if (auto st = drain(chunk * 2); !st.ok())
            return failSend(st.takeError());
        const QByteArray b = f.read(chunk);
        if (b.isEmpty()) {
            if (!f.atEnd())
                return failSend(Error(ErrorCode::IoError, QStringLiteral("Could not read the package."), f.errorString()));
            break;
        }
        s.write(b);
        s.flush();
        queued += b.size();
    }
    const QByteArray footer = dpiPartFooter();
    s.write(footer);
    queued += footer.size();
    if (auto st = drain(0); !st.ok())
        return failSend(st.takeError());
    if (progress)
        progress(size, size);

    // Wait for the reply.
    QByteArray buf;
    HttpParse head;
    idle.restart();
    while (true) {
        if (s.bytesAvailable() > 0) {
            buf += s.readAll();
            idle.restart();
        }
        head = parseHead(buf);
        if (head.headDone) {
            const QByteArray raw = buf.mid(head.bodyStart);
            QByteArray body;
            const bool complete = head.chunked ? decodeChunked(raw, &body)
                                : head.contentLength >= 0 ? raw.size() >= head.contentLength
                                                          : false;
            if (complete || (s.state() != QAbstractSocket::ConnectedState && s.bytesAvailable() == 0)) {
                DpiResponse r;
                r.status = head.status;
                r.body = head.chunked ? body : head.contentLength >= 0 ? raw.left(head.contentLength) : raw;
                r.accepted = dpiAccepted(r.status, r.body);
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
