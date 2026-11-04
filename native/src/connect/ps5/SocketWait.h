#pragma once

// Blocking socket helpers for worker threads.
//
// Qt documents QAbstractSocket::waitForReadyRead()/waitForBytesWritten() as
// unreliable on Windows, so every blocking wait in this module runs a short
// local QEventLoop instead: it wakes on the watched sockets' signals and on a
// 25 ms tick (cancellation and timeouts), on the thread that owns the sockets.
// Sockets must be created and used on that same thread, and that thread must
// be a Qt thread (QThread / QThreadPool worker, or the main thread): Qt's
// socket notifiers refuse threads started with std::thread.

#include "core/Cancellation.h"
#include "core/Result.h"

#include <QAbstractSocket>
#include <QString>

#include <functional>
#include <initializer_list>

class QObject;
class QTcpSocket;

namespace fw::ps5::detail {

enum class WaitResult { Ready, Timeout, Cancelled };

// Runs a local event loop until `ready()` returns true, `timeoutMs` elapses
// (< 0: no limit) or `token` is cancelled.
WaitResult waitUntil(const std::function<bool()>& ready, int timeoutMs, const CancellationToken& token,
                     std::initializer_list<QObject*> watch);

// "Connecting to the PS5 (192.168.1.20:2121) failed: ..." style message for a
// socket error. `service` is e.g. "FTP server", "payload loader".
QString socketErrorMessage(QAbstractSocket::SocketError error, const QString& host, quint16 port,
                           const QString& service);

// Connects `socket` with a timeout; maps failures to user-facing errors.
Status connectSocket(QTcpSocket& socket, const QString& host, quint16 port, int timeoutMs,
                     const CancellationToken& token, const QString& service);

} // namespace fw::ps5::detail
