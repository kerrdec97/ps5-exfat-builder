#include "connect/ps5/SmpApi.h"

#include "connect/ps5/SocketWait.h"

#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTcpSocket>

#include <algorithm>

namespace fw::ps5::smp {

using detail::WaitResult;
using detail::waitUntil;

namespace {

struct Head {
    bool done = false;
    int status = 0;
    qint64 contentLength = -1;
    bool chunked = false;
    qsizetype bodyStart = 0;
};

Head parseHead(const QByteArray& buf)
{
    Head h;
    const qsizetype end = buf.indexOf("\r\n\r\n");
    if (end < 0)
        return h;
    h.done = true;
    h.bodyStart = end + 4;
    const QList<QByteArray> lines = buf.left(end).split('\n');
    if (!lines.isEmpty()) {
        const QList<QByteArray> parts = lines.front().trimmed().split(' ');
        if (parts.size() >= 2)
            h.status = parts.at(1).toInt();
    }
    for (qsizetype i = 1; i < lines.size(); ++i) {
        const QByteArray l = lines.at(i).trimmed();
        const qsizetype colon = l.indexOf(':');
        if (colon < 0)
            continue;
        const QByteArray k = l.left(colon).trimmed().toLower();
        const QByteArray v = l.mid(colon + 1).trimmed();
        if (k == "content-length")
            h.contentLength = v.toLongLong();
        else if (k == "transfer-encoding" && v.toLower().contains("chunked"))
            h.chunked = true;
    }
    return h;
}

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

qint64 number(const QJsonObject& o, const char* key, qint64 fallback = -1)
{
    const QJsonValue v = o.value(QLatin1String(key));
    if (v.isDouble())
        return qint64(v.toDouble());
    if (v.isString()) {
        bool ok = false;
        const qint64 n = v.toString().toLongLong(&ok);
        return ok ? n : fallback;
    }
    return fallback;
}

QString text(const QJsonObject& o, const char* key)
{
    const QJsonValue v = o.value(QLatin1String(key));
    if (v.isString())
        return v.toString();
    if (v.isDouble())
        return QString::number(qint64(v.toDouble()));
    return {};
}

} // namespace

Result<ApiReply> call(const QString& host, quint16 port, const QString& path, const QJsonObject& request,
                      const ApiOptions& opt, const CancellationToken& token)
{
    if (host.trimmed().isEmpty())
        return Error(ErrorCode::InvalidArgument, QStringLiteral("No console address."));
    const QByteArray body = QJsonDocument(request).toJson(QJsonDocument::Compact);
    if (body.size() > 4096)
        return Error(ErrorCode::InvalidArgument, QStringLiteral("The ShadowMount+ API accepts at most 4096 bytes per request."));
    QTcpSocket s;
    FW_TRY(detail::connectSocket(s, host.trimmed(), port, opt.connectTimeoutMs, token, QStringLiteral("ShadowMount+ API")));
    const QByteArray req = "POST " + path.toUtf8() + " HTTP/1.1\r\nHost: " + host.trimmed().toUtf8() + ':' + QByteArray::number(port)
                         + "\r\nContent-Type: application/json\r\nAccept: application/json\r\nConnection: close\r\nContent-Length: "
                         + QByteArray::number(body.size()) + "\r\n\r\n" + body;
    s.write(req);
    s.flush();
    QByteArray buf;
    QElapsedTimer idle;
    idle.start();
    while (true) {
        if (s.bytesAvailable() > 0) {
            buf += s.readAll();
            idle.restart();
        }
        const Head h = parseHead(buf);
        const bool closed = s.state() != QAbstractSocket::ConnectedState && s.bytesAvailable() == 0;
        if (h.done) {
            const QByteArray raw = buf.mid(h.bodyStart);
            QByteArray content;
            bool complete = false;
            if (h.chunked)
                complete = decodeChunked(raw, &content);
            else if (h.contentLength >= 0) {
                complete = raw.size() >= h.contentLength;
                content = raw.left(h.contentLength);
            } else if (closed) {
                complete = true;
                content = raw;
            }
            if (complete) {
                s.abort();
                ApiReply r;
                r.httpStatus = h.status;
                QJsonParseError perr{};
                const QJsonDocument doc = QJsonDocument::fromJson(content, &perr);
                if (perr.error != QJsonParseError::NoError || !doc.isObject())
                    return Error(ErrorCode::CorruptData, QStringLiteral("The ShadowMount+ API sent a reply that is not JSON (HTTP %1).").arg(h.status),
                                 QString::fromUtf8(content.left(200)));
                r.body = doc.object();
                if (h.status < 200 || h.status >= 300 || r.status() != 0) {
                    const QString why = r.error().isEmpty() ? QStringLiteral("status %1").arg(r.status()) : r.error();
                    const ErrorCode code = h.status == 404 ? ErrorCode::NotFound
                                         : h.status == 409 ? ErrorCode::AlreadyExists
                                         : h.status == 400 ? ErrorCode::InvalidArgument
                                                           : ErrorCode::ExternalToolFailed;
                    return Error(code, QStringLiteral("ShadowMount+ refused %1: %2").arg(path, why),
                                 QStringLiteral("HTTP %1 %2").arg(h.status).arg(QString::fromUtf8(content.left(300))));
                }
                return r;
            }
        }
        if (closed)
            return Error(ErrorCode::IoError, QStringLiteral("The ShadowMount+ API closed the connection without an answer."));
        const WaitResult w = waitUntil(
            [&] { return s.bytesAvailable() > 0 || s.state() != QAbstractSocket::ConnectedState; }, 200, token, {&s});
        if (w == WaitResult::Cancelled) {
            s.abort();
            return Error::cancelled();
        }
        if (w == WaitResult::Timeout && idle.elapsed() >= opt.replyTimeoutMs) {
            s.abort();
            return Error(ErrorCode::Timeout, QStringLiteral("The ShadowMount+ API did not answer within %1 s.").arg(opt.replyTimeoutMs / 1000));
        }
    }
}

Result<VersionInfo> version(const QString& host, quint16 port, const ApiOptions& opt, const CancellationToken& token)
{
    FW_TRY_ASSIGN(ApiReply r, call(host, port, QStringLiteral("/api/v1/version"), {}, opt, token));
    VersionInfo v;
    v.version = text(r.body, "shadowmount_version");
    v.apiVersion = int(number(r.body, "api_version", 0));
    for (const QJsonValue& c : r.body.value(QStringLiteral("capabilities")).toArray())
        v.capabilities << c.toString();
    if (v.version.isEmpty())
        return Error(ErrorCode::CorruptData, QStringLiteral("The ShadowMount+ API did not say its version."));
    return v;
}

Result<std::vector<StorageItem>> storage(const QString& host, quint16 port, const ApiOptions& opt, const CancellationToken& token)
{
    FW_TRY_ASSIGN(ApiReply r, call(host, port, QStringLiteral("/api/v1/storage"), {}, opt, token));
    std::vector<StorageItem> out;
    // The mounted filesystems: the array whose items carry "mount_point"
    // (the docs do not name it; "destinations" is the copy-target list).
    QJsonArray items;
    for (auto it = r.body.begin(); it != r.body.end(); ++it) {
        if (!it.value().isArray() || it.key() == u"destinations")
            continue;
        const QJsonArray a = it.value().toArray();
        if (!a.isEmpty() && a.first().toObject().contains(QStringLiteral("mount_point"))) {
            items = a;
            break;
        }
    }
    for (const QJsonValue& v : items) {
        const QJsonObject o = v.toObject();
        StorageItem i;
        i.mountPoint = text(o, "mount_point");
        if (i.mountPoint.isEmpty())
            i.mountPoint = text(o, "path");
        i.source = text(o, "source");
        i.filesystem = text(o, "filesystem");
        i.mountId = text(o, "mount_id");
        i.totalBytes = number(o, "total_bytes");
        i.availableBytes = number(o, "available_bytes");
        if (i.availableBytes < 0)
            i.availableBytes = number(o, "free_bytes");
        i.ejectable = o.value(QStringLiteral("ejectable")).toBool(false);
        i.readOnly = o.value(QStringLiteral("read_only")).toBool(false);
        if (!i.mountPoint.isEmpty())
            out.push_back(i);
    }
    return out;
}

const StorageItem* storageFor(const std::vector<StorageItem>& items, const QString& remotePath)
{
    const StorageItem* best = nullptr;
    for (const StorageItem& i : items) {
        const QString mp = i.mountPoint.endsWith(u'/') && i.mountPoint.size() > 1 ? i.mountPoint.chopped(1) : i.mountPoint;
        const bool inside = remotePath == mp || remotePath.startsWith(mp == u"/" ? mp : mp + u'/');
        if (inside && (!best || mp.size() > best->mountPoint.size()))
            best = &i;
    }
    return best;
}

Result<qint64> eject(const QString& host, quint16 port, const StorageItem& item, const ApiOptions& opt, const CancellationToken& token)
{
    if (!item.ejectable)
        return Error(ErrorCode::InvalidArgument, QStringLiteral("%1 cannot be ejected (only USB drives can).").arg(item.mountPoint));
    QJsonObject req{{QStringLiteral("path"), item.mountPoint}, {QStringLiteral("source"), item.source}, {QStringLiteral("mount_id"), item.mountId}};
    FW_TRY_ASSIGN(ApiReply r, call(host, port, QStringLiteral("/api/v1/storage/eject"), req, opt, token));
    const qint64 id = number(r.body, "job_id", -1);
    if (id < 0)
        return Error(ErrorCode::CorruptData, QStringLiteral("ShadowMount+ accepted the eject but sent no job id."));
    return id;
}

JobStatus parseJobStatus(const QJsonObject& o)
{
    JobStatus s;
    QString state = text(o, "state");
    if (state.isEmpty())
        state = text(o, "phase");
    if (state.isEmpty())
        state = text(o, "result");
    s.state = state;
    const QString reason = text(o, "result_error_reason").isEmpty() ? text(o, "error_reason") : text(o, "result_error_reason");
    QString err = text(o, "result_error");
    if (!reason.isEmpty())
        err = err.isEmpty() ? reason : QStringLiteral("%1 (%2)").arg(err, reason);
    s.error = err;
    const QString src = text(o, "blocking_source"), dst = text(o, "blocking_target");
    if (!src.isEmpty() || !dst.isEmpty())
        s.blocking = dst.isEmpty() ? src : QStringLiteral("%1 → %2").arg(src, dst);
    const QString low = (state + u' ' + reason).toLower();
    s.unconfirmed = low.contains(QLatin1String("usb_eject_unconfirmed"));
    const bool failed = low.contains(QLatin1String("fail")) || low.contains(QLatin1String("error")) || low.contains(QLatin1String("cancel"))
                      || low.contains(QLatin1String("busy")) || s.unconfirmed;
    s.succeeded = !failed && (low.contains(QLatin1String("completed")) || low.contains(QLatin1String("done")) || low.contains(QLatin1String("success")));
    s.finished = s.succeeded || failed || o.value(QStringLiteral("finished")).toBool(false) || o.value(QStringLiteral("active")) == QJsonValue(false);
    return s;
}

Result<JobStatus> jobStatus(const QString& host, quint16 port, qint64 jobId, const ApiOptions& opt, const CancellationToken& token)
{
    FW_TRY_ASSIGN(ApiReply r, call(host, port, QStringLiteral("/api/v1/games/storage/status"), {{QStringLiteral("job_id"), jobId}}, opt, token));
    return parseJobStatus(r.body);
}

QString logText(const QJsonObject& o)
{
    for (const char* k : {"log", "text", "content", "data", "tail", "lines"}) {
        const QJsonValue v = o.value(QLatin1String(k));
        if (v.isString())
            return v.toString();
        if (v.isArray()) {
            QStringList lines;
            for (const QJsonValue& l : v.toArray())
                lines << l.toString();
            return lines.join(u'\n');
        }
    }
    // Unknown shape: the longest string value.
    QString best;
    for (auto it = o.begin(); it != o.end(); ++it)
        if (it.value().isString() && it.value().toString().size() > best.size())
            best = it.value().toString();
    return best;
}

Result<QString> debugLog(const QString& host, quint16 port, int maxBytes, const ApiOptions& opt, const CancellationToken& token)
{
    FW_TRY_ASSIGN(ApiReply r, call(host, port, QStringLiteral("/api/v1/debug-log"),
                                   {{QStringLiteral("max_bytes"), std::clamp(maxBytes, 1024, 256 * 1024)}}, opt, token));
    return logText(r.body);
}

Result<QString> kernelLog(const QString& host, quint16 port, int maxBytes, const ApiOptions& opt, const CancellationToken& token)
{
    FW_TRY_ASSIGN(ApiReply r, call(host, port, QStringLiteral("/api/v1/kernel-log"),
                                   {{QStringLiteral("max_bytes"), std::clamp(maxBytes, 1024, 256 * 1024)}}, opt, token));
    return logText(r.body);
}

Result<QJsonObject> settings(const QString& host, quint16 port, const ApiOptions& opt, const CancellationToken& token)
{
    FW_TRY_ASSIGN(ApiReply r, call(host, port, QStringLiteral("/api/v1/settings"), {}, opt, token));
    return r.body;
}

} // namespace fw::ps5::smp
