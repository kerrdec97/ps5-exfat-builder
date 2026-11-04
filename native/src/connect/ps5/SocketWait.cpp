#include "connect/ps5/SocketWait.h"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

namespace fw::ps5::detail {

WaitResult waitUntil(const std::function<bool()>& ready, int timeoutMs, const CancellationToken& token,
                     std::initializer_list<QObject*> watch)
{
    if (ready())
        return WaitResult::Ready;
    if (token.isCancelled())
        return WaitResult::Cancelled;
    QEventLoop loop;
    QElapsedTimer clock;
    clock.start();
    WaitResult result = WaitResult::Timeout;
    auto check = [&] {
        if (ready()) {
            result = WaitResult::Ready;
            loop.quit();
        } else if (token.isCancelled()) {
            result = WaitResult::Cancelled;
            loop.quit();
        } else if (timeoutMs >= 0 && clock.elapsed() >= timeoutMs) {
            result = WaitResult::Timeout;
            loop.quit();
        }
    };
    QTimer tick;
    tick.setInterval(25);
    QObject::connect(&tick, &QTimer::timeout, &loop, check);
    for (QObject* o : watch) {
        if (auto* s = qobject_cast<QAbstractSocket*>(o)) {
            QObject::connect(s, &QIODevice::readyRead, &loop, check);
            QObject::connect(s, &QIODevice::bytesWritten, &loop, check);
            QObject::connect(s, &QAbstractSocket::stateChanged, &loop, check);
            QObject::connect(s, &QAbstractSocket::errorOccurred, &loop, check);
        } else if (auto* srv = qobject_cast<QTcpServer*>(o)) {
            QObject::connect(srv, &QTcpServer::newConnection, &loop, check);
        }
    }
    tick.start();
    loop.exec(QEventLoop::ExcludeUserInputEvents);
    return result;
}

QString socketErrorMessage(QAbstractSocket::SocketError error, const QString& host, quint16 port,
                           const QString& service)
{
    const QString where = QStringLiteral("%1:%2").arg(host).arg(port);
    switch (error) {
    case QAbstractSocket::ConnectionRefusedError:
        return QStringLiteral("The PS5 at %1 refused the connection: no %2 is listening on that port. "
                              "Start it on the console (most jailbreaks need this after every boot), "
                              "check the port, then try again.")
            .arg(where, service);
    case QAbstractSocket::HostNotFoundError:
        return QStringLiteral("The address %1 could not be found. Check the PS5 IP address in Settings.").arg(host);
    case QAbstractSocket::NetworkError:
        return QStringLiteral("No network route to the PS5 at %1. Check the IP address and that the PS5 and "
                              "this PC are on the same network.")
            .arg(where);
    case QAbstractSocket::SocketTimeoutError:
        return QStringLiteral("The PS5 at %1 did not answer in time. Check that it is on (not in rest mode), "
                              "that the IP address is right and that both are on the same network.")
            .arg(where);
    case QAbstractSocket::RemoteHostClosedError:
        return QStringLiteral("The PS5 (%1) closed the connection.").arg(service);
    default:
        return QStringLiteral("The connection to the PS5 %1 at %2 failed.").arg(service, where);
    }
}

Status connectSocket(QTcpSocket& socket, const QString& host, quint16 port, int timeoutMs,
                     const CancellationToken& token, const QString& service)
{
    if (host.trimmed().isEmpty())
        return Error(ErrorCode::InvalidArgument, QStringLiteral("No PS5 IP address set. Enter it in Settings."));
    socket.connectToHost(host.trimmed(), port);
    const WaitResult w = waitUntil(
        [&] {
            return socket.state() == QAbstractSocket::ConnectedState
                || socket.state() == QAbstractSocket::UnconnectedState;
        },
        timeoutMs, token, {&socket});
    if (w == WaitResult::Cancelled) {
        socket.abort();
        return Error::cancelled();
    }
    if (socket.state() == QAbstractSocket::ConnectedState)
        return success();
    const QAbstractSocket::SocketError err =
        w == WaitResult::Timeout ? QAbstractSocket::SocketTimeoutError : socket.error();
    const QString detail = w == WaitResult::Timeout ? QStringLiteral("timeout after %1 ms").arg(timeoutMs)
                                                    : socket.errorString();
    socket.abort();
    return Error(err == QAbstractSocket::SocketTimeoutError ? ErrorCode::Timeout : ErrorCode::IoError,
                 socketErrorMessage(err, host, port, service), detail);
}

} // namespace fw::ps5::detail
