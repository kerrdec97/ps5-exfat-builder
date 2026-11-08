#pragma once

// Raw TCP payload push (legacy _pl_send and the ShadowMount+/MicroMount ELF
// senders): connect (10 s), send the file's bytes in 64 KiB chunks (30 s
// without progress = failure), close. No framing, no handshake, nothing is
// read back. Default port: 9021 (elfldr) for every target; 9090 is etaHEN's
// DPI v1 service and discards an ELF.

#include "core/Cancellation.h"
#include "core/Result.h"

#include <QByteArray>
#include <QString>

#include <functional>

namespace fw::ps5 {

struct PayloadOptions {
    int connectTimeoutMs = 10000;
    int sendTimeoutMs = 30000;
    int chunkSize = 64 * 1024;
};

struct PayloadResult {
    qint64 bytes = 0;
    qint64 elapsedMs = 0;
};

using PayloadProgress = std::function<void(qint64 sent, qint64 total)>;

Result<PayloadResult> sendPayloadFile(const QString& host, quint16 port, const QString& filePath,
                                      const PayloadOptions& options = {}, const CancellationToken& token = {},
                                      const PayloadProgress& progress = {});
Result<PayloadResult> sendPayloadData(const QString& host, quint16 port, const QByteArray& data,
                                      const PayloadOptions& options = {}, const CancellationToken& token = {},
                                      const PayloadProgress& progress = {});

// TCP connect probe (Y2JB "Test DPI", reachability checks). Closes at once.
Status probeTcpPort(const QString& host, quint16 port, int timeoutMs, const QString& service,
                    const CancellationToken& token = {});

} // namespace fw::ps5
