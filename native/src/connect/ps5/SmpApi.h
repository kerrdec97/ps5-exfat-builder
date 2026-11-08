#pragma once

// ShadowMount+ 1.7 HTTP/JSON API v1 (drakmor/ShadowMountPlus docs/api.md;
// research §1.2-8). On by default since 1.7beta8 at 0.0.0.0:10101, no
// authentication. Every call is POST with Content-Type: application/json and
// one JSON object (at most 4096 bytes); every reply carries a numeric
// "status" (0 = OK, otherwise an errno) and, on errors, "error".
//
// Only the routes the app uses:
//   /api/v1/version             shadowmount_version, capabilities
//   /api/v1/settings            API-managed runtime settings
//   /api/v1/storage             mounted filesystems: source, mount_point,
//                               available_bytes, total_bytes, ejectable, mount_id
//   /api/v1/storage/eject       {"path","source","mount_id"} -> HTTP 202 + job_id
//   /api/v1/games/storage/status {"job_id"}: poll until completed / failed;
//                               usb_eject_unconfirmed = keep the drive connected
//   /api/v1/debug-log           {"max_bytes"} bounded tail of debug.log (<= 256 KiB)
//   /api/v1/kernel-log          {"max_bytes"} kernel-log snapshot
//
// Everything is optional: callers fall back to FTP/klog when the API does not
// answer (older ShadowMount+, api_enabled=0, api_bind_address=127.0.0.1).
// Blocking; use from a Qt-started thread (queue worker, QtConcurrent).

#include "core/Cancellation.h"
#include "core/Result.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <vector>

namespace fw::ps5::smp {

inline constexpr quint16 kDefaultApiPort = 10101;

struct ApiOptions {
    int connectTimeoutMs = 3000;
    int replyTimeoutMs = 8000;
};

struct ApiReply {
    int httpStatus = 0;
    QJsonObject body;
    int status() const { return body.value(QStringLiteral("status")).toInt(-1); }
    QString error() const { return body.value(QStringLiteral("error")).toString(); }
};

// One POST <path> with `request`; the reply as JSON. HTTP 2xx with status 0
// is success; anything else is an error naming the API's own message.
Result<ApiReply> call(const QString& host, quint16 port, const QString& path, const QJsonObject& request = {},
                      const ApiOptions& options = {}, const CancellationToken& token = {});

struct VersionInfo {
    QString version;          // "1.7", "1.7beta11"
    int apiVersion = 0;
    QStringList capabilities; // "storage_eject", "read_debug_log", "pkg_install", ...
    bool has(const QString& capability) const { return capabilities.contains(capability); }
};
Result<VersionInfo> version(const QString& host, quint16 port, const ApiOptions& options = {}, const CancellationToken& token = {});

struct StorageItem {
    QString mountPoint;  // "/mnt/usb0", "/data"
    QString source;      // "/dev/da1p1"
    QString filesystem;  // "exfatfs", "ufs"
    QString mountId;     // opaque 16-hex id (eject)
    qint64 totalBytes = -1;
    qint64 availableBytes = -1; // space for new files
    bool ejectable = false;
    bool readOnly = false;
};
Result<std::vector<StorageItem>> storage(const QString& host, quint16 port, const ApiOptions& options = {},
                                         const CancellationToken& token = {});
// The item whose mount point holds `remotePath` (longest prefix), if any.
const StorageItem* storageFor(const std::vector<StorageItem>& items, const QString& remotePath);

// Starts the eject of `item` (must be ejectable). Returns the storage job id.
Result<qint64> eject(const QString& host, quint16 port, const StorageItem& item, const ApiOptions& options = {},
                     const CancellationToken& token = {});

struct JobStatus {
    QString state;   // "measuring", "running", "completed", "failed", "usb_eject_unconfirmed", ...
    QString error;   // result_error / result_error_reason when it failed
    QString blocking; // blocking_source -> blocking_target when a mount blocks the eject
    bool finished = false;
    bool succeeded = false;
    bool unconfirmed = false; // usb_eject_unconfirmed: keep the drive connected
};
// Interprets a /games/storage/status reply (tolerant of the field names).
JobStatus parseJobStatus(const QJsonObject& reply);
Result<JobStatus> jobStatus(const QString& host, quint16 port, qint64 jobId, const ApiOptions& options = {},
                            const CancellationToken& token = {});

// The log text of a /debug-log or /kernel-log reply (tolerant of the field name).
QString logText(const QJsonObject& reply);
Result<QString> debugLog(const QString& host, quint16 port, int maxBytes = 131072, const ApiOptions& options = {},
                         const CancellationToken& token = {});
Result<QString> kernelLog(const QString& host, quint16 port, int maxBytes = 131072, const ApiOptions& options = {},
                          const CancellationToken& token = {});

Result<QJsonObject> settings(const QString& host, quint16 port, const ApiOptions& options = {}, const CancellationToken& token = {});

} // namespace fw::ps5::smp
