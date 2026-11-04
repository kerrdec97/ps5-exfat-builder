#pragma once

// etaHEN Direct Package Installer V2 (Y2JB "Install <region>"): an HTTP/1.1
// POST of the .pkg as multipart/form-data to http://<ip>:12800/upload.
//
// Exactly the request the legacy app built: boundary "X-INSOMNIA-BOUNDARY",
// one part (name="file", filename=<pkg name>, application/octet-stream), a
// manual Content-Length, the file streamed in 512 KiB blocks. Raw HTTP on a
// QTcpSocket so the whole package is never held in memory.

#include "core/Cancellation.h"
#include "core/Result.h"

#include <QByteArray>
#include <QString>

#include <functional>

namespace fw::ps5 {

struct DpiResponse {
    int status = 0;     // HTTP status
    QByteArray body;
    bool accepted = false;
};

// Legacy success rule was "HTTP 200, or the body mentions success/install".
// Kept, except that a 4xx/5xx status or a body that mentions a failure or an
// error is never taken as success.
bool dpiAccepted(int status, const QByteArray& body);

struct DpiOptions {
    int connectTimeoutMs = 10000;
    int idleTimeoutMs = 30000;  // legacy: 30 s HTTP timeout
    int chunkSize = 512 * 1024;
};

// progress(sent, total) counts package bytes only.
Result<DpiResponse> installPackage(const QString& host, quint16 port, const QString& pkgPath,
                                   const DpiOptions& options = {}, const CancellationToken& token = {},
                                   const std::function<void(qint64 sent, qint64 total)>& progress = {});

// The request head and multipart framing for `fileName` and `fileSize` (tests).
QByteArray dpiRequestHead(const QString& host, quint16 port, const QString& fileName, qint64 fileSize);
QByteArray dpiPartHeader(const QString& fileName);
QByteArray dpiPartFooter();

} // namespace fw::ps5
