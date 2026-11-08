#include "connect/ps5/PayloadSender.h"

#include "connect/ps5/SocketWait.h"

#include <QBuffer>
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

Result<PayloadResult> sendDevice(const QString& host, quint16 port, QIODevice& in, qint64 total,
                                 const PayloadOptions& opt, const CancellationToken& token,
                                 const PayloadProgress& progress)
{
    QElapsedTimer clock;
    clock.start();
    QTcpSocket s;
    if (auto st = detail::connectSocket(s, host, port, opt.connectTimeoutMs, token, QStringLiteral("payload loader")); !st.ok()) {
        Error e = st.takeError();
        if (!e.isCancelled() && port == 9021)
            // itsPLK's elfldr (Y2JB / BD-JB / Lua autoloaders) accepts localhost only (research §5).
            e.message += QStringLiteral(" With the Y2JB, BD-JB or Lua autoloader, add a normal elfldr.elf to autoload.txt "
                                        "followed by !4000 so the PC can send payloads.");
        else if (!e.isCancelled() && port == 9090)
            e.message += QStringLiteral(" Port 9090 is etaHEN's package installer (DPI v1), not a payload loader; use 9021.");
        return e;
    }
    const qint64 chunk = std::max(1024, opt.chunkSize);
    qint64 queued = 0, reported = 0;
    QElapsedTimer idle;
    idle.start();
    auto report = [&] {
        const qint64 sent = queued - s.bytesToWrite();
        if (sent > reported) {
            reported = sent;
            idle.restart();
            if (progress)
                progress(sent, total);
        }
    };
    auto drain = [&](qint64 level) -> Status {
        while (s.bytesToWrite() > level) {
            if (s.state() != QAbstractSocket::ConnectedState)
                return Error(ErrorCode::IoError,
                             QStringLiteral("The PS5 closed the connection before the whole payload was sent."),
                             s.errorString());
            const WaitResult w = waitUntil(
                [&] { return s.bytesToWrite() <= level || s.state() != QAbstractSocket::ConnectedState; }, 200, token,
                {&s});
            report();
            if (w == WaitResult::Cancelled)
                return Error::cancelled();
            if (w == WaitResult::Timeout && idle.elapsed() >= opt.sendTimeoutMs)
                return Error(ErrorCode::Timeout,
                             QStringLiteral("The PS5 stopped accepting the payload (nothing sent for %1 s). Check that "
                                            "it is ready to receive a payload.")
                                 .arg(opt.sendTimeoutMs / 1000));
        }
        return success();
    };
    while (true) {
        if (auto st = drain(chunk * 4); !st.ok()) {
            s.abort();
            return st.takeError();
        }
        const QByteArray b = in.read(chunk);
        if (b.isEmpty()) {
            if (!in.atEnd()) {
                s.abort();
                return Error(ErrorCode::IoError, QStringLiteral("Could not read the payload file."), in.errorString());
            }
            break;
        }
        s.write(b);
        s.flush();
        queued += b.size();
    }
    if (auto st = drain(0); !st.ok()) {
        s.abort();
        return st.takeError();
    }
    report();
    s.disconnectFromHost();
    if (s.state() != QAbstractSocket::UnconnectedState)
        (void)waitUntil([&] { return s.state() == QAbstractSocket::UnconnectedState; }, opt.sendTimeoutMs, token, {&s});
    PayloadResult r;
    r.bytes = queued;
    r.elapsedMs = clock.elapsed();
    return r;
}

} // namespace

Result<PayloadResult> sendPayloadFile(const QString& host, quint16 port, const QString& filePath,
                                      const PayloadOptions& options, const CancellationToken& token,
                                      const PayloadProgress& progress)
{
    QFile f(filePath);
    if (!QFileInfo(filePath).isFile())
        return Error(ErrorCode::NotFound,
                     QStringLiteral("Payload file not found: %1. Edit the payload to update the file path.")
                         .arg(QDir::toNativeSeparators(filePath)));
    if (!f.open(QIODevice::ReadOnly))
        return Error(ErrorCode::IoError, QStringLiteral("The payload file could not be opened."), f.errorString());
    return sendDevice(host, port, f, f.size(), options, token, progress);
}

Result<PayloadResult> sendPayloadData(const QString& host, quint16 port, const QByteArray& data,
                                      const PayloadOptions& options, const CancellationToken& token,
                                      const PayloadProgress& progress)
{
    QByteArray copy = data;
    QBuffer b(&copy);
    b.open(QIODevice::ReadOnly);
    return sendDevice(host, port, b, copy.size(), options, token, progress);
}

Status probeTcpPort(const QString& host, quint16 port, int timeoutMs, const QString& service,
                    const CancellationToken& token)
{
    QTcpSocket s;
    FW_TRY(detail::connectSocket(s, host, port, timeoutMs, token, service));
    s.abort();
    return success();
}

} // namespace fw::ps5
