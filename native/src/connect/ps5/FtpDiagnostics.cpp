#include "connect/ps5/FtpDiagnostics.h"

#include "connect/ps5/FtpTransfer.h"
#include "connect/ps5/RemotePath.h"
#include "connect/ps5/SocketWait.h"
#include "core/FileUtil.h"

#include <QBuffer>
#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QRandomGenerator>
#include <QTcpSocket>
#include <QThread>

#include <algorithm>
#include <cstring>

namespace fw::ps5 {

ServerGuess classifyFtpServer(const FtpFeatures& f, const QString& banner)
{
    ServerGuess g;
    // ps5-payload-dev ftpsrv: its greeting names it; FEAT lists KILL/MTRW/SELF.
    const bool ftpsrvBanner = banner.contains(QLatin1String("ftpsrv"), Qt::CaseInsensitive);
    if (ftpsrvBanner || f.ftpsrv) {
        const FtpServerIdentity id = identifyFtpServer(banner, &f);
        g.label = id.version.isEmpty() ? QStringLiteral("ftpsrv") : QStringLiteral("ftpsrv %1").arg(id.version);
        g.confidence = ftpsrvBanner && f.ftpsrv ? QStringLiteral("very likely") : QStringLiteral("likely");
        if (ftpsrvBanner)
            g.evidence << QStringLiteral("Welcome banner names ftpsrv");
        if (f.ftpsrv)
            g.evidence << QStringLiteral("FEAT lists ftpsrv's own commands (KILL/MTRW/SELF)");
        return g;
    }
    if (banner.contains(QLatin1String("Service ready for new user"), Qt::CaseInsensitive)) {
        g.label = QStringLiteral("etaHEN FTP");
        g.confidence = QStringLiteral("likely");
        g.evidence << QStringLiteral("Welcome banner matches etaHEN's FTP server (port 1337)");
        return g;
    }
    int score = 0;
    if (f.xcrypt) {
        score += 2;
        g.evidence << QStringLiteral("FEAT advertises XCRYPT (zftpd extension)");
    }
    if (f.copy) {
        score += 2;
        g.evidence << QStringLiteral("Server-side copy (CPFR/CPTO/COPY) advertised");
    }
    if (f.mlsd && f.utf8 && f.rest) {
        score += 1;
        g.evidence << QStringLiteral("MLSD + UTF8 + REST all present (consistent with zftpd)");
    }
    if (banner.contains(QLatin1String("zftpd"), Qt::CaseInsensitive)) {
        score += 3;
        g.evidence << QStringLiteral("Welcome banner names zftpd");
    }
    if (score >= 2) {
        g.label = QStringLiteral("zftpd");
        g.confidence = score >= 4 ? QStringLiteral("very likely") : QStringLiteral("likely");
        return g;
    }
    if (g.evidence.isEmpty())
        g.evidence << QStringLiteral("No zftpd-specific markers (no XCRYPT, no server-side copy)");
    return g;
}

namespace {

QJsonObject featuresJson(const FtpFeatures& f)
{
    return {{QStringLiteral("mlsd"), f.mlsd}, {QStringLiteral("rest"), f.rest},   {QStringLiteral("size"), f.size},
            {QStringLiteral("mdtm"), f.mdtm}, {QStringLiteral("utf8"), f.utf8},   {QStringLiteral("xcrypt"), f.xcrypt},
            {QStringLiteral("copy"), f.copy}, {QStringLiteral("ftpsrv"), f.ftpsrv}, {QStringLiteral("raw"), f.raw}};
}

FtpFeatures featuresFrom(const QJsonObject& o)
{
    FtpFeatures f;
    f.mlsd = o.value(QStringLiteral("mlsd")).toBool();
    f.rest = o.value(QStringLiteral("rest")).toBool();
    f.size = o.value(QStringLiteral("size")).toBool();
    f.mdtm = o.value(QStringLiteral("mdtm")).toBool();
    f.utf8 = o.value(QStringLiteral("utf8")).toBool();
    f.xcrypt = o.value(QStringLiteral("xcrypt")).toBool();
    f.copy = o.value(QStringLiteral("copy")).toBool();
    f.ftpsrv = o.value(QStringLiteral("ftpsrv")).toBool();
    f.raw = o.value(QStringLiteral("raw")).toString();
    return f;
}

QJsonValue optBool(const std::optional<bool>& b)
{
    return b ? QJsonValue(*b) : QJsonValue(QJsonValue::Null);
}

std::optional<bool> optBoolFrom(const QJsonValue& v)
{
    if (v.isBool())
        return v.toBool();
    return std::nullopt;
}

QJsonValue optDouble(double v)
{
    return v >= 0 ? QJsonValue(v) : QJsonValue(QJsonValue::Null);
}

} // namespace

QJsonObject FtpDiagnosis::toJson() const
{
    QJsonObject o;
    o.insert(QStringLiteral("host"), host);
    o.insert(QStringLiteral("port"), int(port));
    o.insert(QStringLiteral("folder"), folder);
    o.insert(QStringLiteral("reachable"), reachable);
    o.insert(QStringLiteral("rttMs"), rttMs);
    o.insert(QStringLiteral("greeted"), greeted);
    o.insert(QStringLiteral("banner"), banner);
    o.insert(QStringLiteral("loggedIn"), loggedIn);
    o.insert(QStringLiteral("featOk"), featOk);
    o.insert(QStringLiteral("features"), featuresJson(features));
    o.insert(QStringLiteral("passive"), optBool(passive));
    o.insert(QStringLiteral("passiveMode"), passiveMode);
    o.insert(QStringLiteral("binary"), optBool(binary));
    o.insert(QStringLiteral("writable"), optBool(writable));
    o.insert(QStringLiteral("probeCleaned"), probeCleaned);
    o.insert(QStringLiteral("probePath"), probePath);
    o.insert(QStringLiteral("writableDetail"), writableDetail);
    o.insert(QStringLiteral("destination"), destination);
    o.insert(QStringLiteral("server"), QJsonObject{{QStringLiteral("label"), server.label},
                                                   {QStringLiteral("confidence"), server.confidence},
                                                   {QStringLiteral("evidence"), QJsonArray::fromStringList(server.evidence)}});
    o.insert(QStringLiteral("speedTest"), QJsonObject{{QStringLiteral("ran"), speed.ran},
                                                      {QStringLiteral("bytes"), speed.bytes},
                                                      {QStringLiteral("uploadMBps"), optDouble(speed.uploadMBps)},
                                                      {QStringLiteral("downloadMBps"), optDouble(speed.downloadMBps)},
                                                      {QStringLiteral("readBackMatches"), optBool(speed.readBackMatches)},
                                                      {QStringLiteral("path"), speed.path},
                                                      {QStringLiteral("cleaned"), speed.cleaned},
                                                      {QStringLiteral("error"), speed.error}});
    o.insert(QStringLiteral("error"), error);
    o.insert(QStringLiteral("notes"), QJsonArray::fromStringList(notes));
    return o;
}

FtpDiagnosis FtpDiagnosis::fromJson(const QJsonObject& o)
{
    FtpDiagnosis d;
    d.host = o.value(QStringLiteral("host")).toString();
    d.port = quint16(std::clamp(o.value(QStringLiteral("port")).toInt(), 0, 65535));
    d.folder = o.value(QStringLiteral("folder")).toString();
    d.reachable = o.value(QStringLiteral("reachable")).toBool();
    d.rttMs = o.value(QStringLiteral("rttMs")).toInt(-1);
    d.greeted = o.value(QStringLiteral("greeted")).toBool();
    d.banner = o.value(QStringLiteral("banner")).toString();
    d.loggedIn = o.value(QStringLiteral("loggedIn")).toBool();
    d.featOk = o.value(QStringLiteral("featOk")).toBool();
    d.features = featuresFrom(o.value(QStringLiteral("features")).toObject());
    d.passive = optBoolFrom(o.value(QStringLiteral("passive")));
    d.passiveMode = o.value(QStringLiteral("passiveMode")).toString();
    d.binary = optBoolFrom(o.value(QStringLiteral("binary")));
    d.writable = optBoolFrom(o.value(QStringLiteral("writable")));
    d.probeCleaned = o.value(QStringLiteral("probeCleaned")).toBool(true);
    d.probePath = o.value(QStringLiteral("probePath")).toString();
    d.writableDetail = o.value(QStringLiteral("writableDetail")).toString();
    d.destination = o.value(QStringLiteral("destination")).toString();
    const QJsonObject s = o.value(QStringLiteral("server")).toObject();
    if (!s.isEmpty()) {
        d.server.label = s.value(QStringLiteral("label")).toString(d.server.label);
        d.server.confidence = s.value(QStringLiteral("confidence")).toString(d.server.confidence);
        d.server.evidence.clear();
        for (const auto& v : s.value(QStringLiteral("evidence")).toArray())
            d.server.evidence << v.toString();
    }
    const QJsonObject sp = o.value(QStringLiteral("speedTest")).toObject();
    d.speed.ran = sp.value(QStringLiteral("ran")).toBool();
    d.speed.bytes = sp.value(QStringLiteral("bytes")).toInteger();
    d.speed.uploadMBps = sp.value(QStringLiteral("uploadMBps")).toDouble(-1);
    d.speed.downloadMBps = sp.value(QStringLiteral("downloadMBps")).toDouble(-1);
    d.speed.readBackMatches = optBoolFrom(sp.value(QStringLiteral("readBackMatches")));
    d.speed.path = sp.value(QStringLiteral("path")).toString();
    d.speed.cleaned = sp.value(QStringLiteral("cleaned")).toBool(true);
    d.speed.error = sp.value(QStringLiteral("error")).toString();
    d.error = o.value(QStringLiteral("error")).toString();
    for (const auto& v : o.value(QStringLiteral("notes")).toArray())
        d.notes << v.toString();
    return d;
}

namespace {

// Deletes a file the diagnostics wrote. Tries the open connection first, then
// fresh ones for about two seconds (a server may still hold the file of an
// aborted STOR). Never cancellable: this is the clean-up.
bool removeProbe(FtpClient& c, const ConsoleEndpoint& ep, const QString& path, QString* why)
{
    Status st = c.isConnected() ? c.dele(path) : Status(Error(ErrorCode::IoError, QStringLiteral("not connected")));
    if (st.ok())
        return true;
    FtpOptions quick = ftpOptionsFor(ep);
    quick.connectTimeoutMs = std::min(quick.connectTimeoutMs, 3000);
    FtpClient fresh(quick);
    QElapsedTimer clock;
    clock.start();
    for (int attempt = 0; attempt < 6 && !st.ok() && clock.elapsed() < 4000; ++attempt) {
        if (attempt > 0)
            QThread::msleep(350);
        if (!fresh.isConnected() && !fresh.open(ep.host, ep.ftpPort, CancellationToken()).ok())
            break; // the console does not answer: nothing more can be done now
        st = fresh.dele(path);
        if (!st.ok() && st.error().code == ErrorCode::NotFound && !fresh.fileExists(path))
            return true; // already gone
    }
    if (fresh.isConnected())
        fresh.quit();
    if (why)
        *why = st.ok() ? QString() : st.error().message;
    return st.ok();
}

// The legacy payload: 1 MiB of random bytes repeated.
QByteArray randomPayload(qint64 bytes)
{
    QByteArray block(int(std::min<qint64>(bytes, 1024 * 1024)), Qt::Uninitialized);
    QRandomGenerator* rng = QRandomGenerator::global();
    for (qsizetype i = 0; i + 4 <= block.size(); i += 4) {
        const quint32 v = rng->generate();
        std::memcpy(block.data() + i, &v, 4);
    }
    for (qsizetype i = block.size() & ~qsizetype(3); i < block.size(); ++i)
        block[i] = char(rng->generate() & 0xFF);
    QByteArray out;
    out.reserve(bytes);
    while (out.size() < bytes)
        out.append(block.constData(), std::min<qsizetype>(block.size(), bytes - out.size()));
    return out;
}

double mibPerSecond(qint64 bytes, qint64 nanoseconds)
{
    const double secs = std::max(double(nanoseconds) / 1e9, 1e-6);
    return double(bytes) / 1048576.0 / secs;
}

} // namespace

Result<FtpDiagnosis> diagnoseFtp(const ConsoleEndpoint& endpoint, const DiagnoseOptions& opt,
                                 const CancellationToken& token, const DiagnoseStep& step, const FtpProgress& bytesMoved)
{
    ConsoleEndpoint ep = endpoint;
    ep.host = ep.host.trimmed();
    if (ep.host.isEmpty())
        return Error(ErrorCode::InvalidArgument, QStringLiteral("Enter your PS5 IP address in Settings › PS5 connection."));
    FtpDiagnosis d;
    d.host = ep.host;
    d.port = ep.ftpPort;
    d.folder = normalizeRemotePath(opt.folder.trimmed().isEmpty() ? ep.gamesFolder : opt.folder);
    d.destination = classifyDestination(d.folder).label;
    const qint64 epoch = opt.epochSeconds > 0 ? opt.epochSeconds : QDateTime::currentSecsSinceEpoch();
    auto say = [&](DiagnoseStage stage, const QString& text) {
        if (step)
            step(stage, text);
    };
    if (token.isCancelled())
        return Error::cancelled();

    // 1. Reachable: a plain TCP connect, timed.
    say(DiagnoseStage::Connecting, QStringLiteral("Connecting to %1:%2").arg(d.host).arg(d.port));
    {
        QTcpSocket s;
        QElapsedTimer t;
        t.start();
        const Status st = detail::connectSocket(s, d.host, d.port, ep.connectTimeoutMs, token, QStringLiteral("FTP server"));
        if (!st.ok()) {
            if (st.error().isCancelled())
                return Error::cancelled();
            d.error = st.error().message;
            return d;
        }
        d.rttMs = int(t.elapsed());
        d.reachable = true;
        s.disconnectFromHost();
        if (s.state() != QAbstractSocket::UnconnectedState)
            s.abort();
    }

    // 2. Greeting and login.
    FtpClient c(ftpOptionsFor(ep));
    say(DiagnoseStage::Connecting, QStringLiteral("Reading the server greeting"));
    if (const Status st = c.connectToHost(d.host, d.port, token); !st.ok()) {
        if (st.error().isCancelled())
            return Error::cancelled();
        d.error = st.error().message;
        return d;
    }
    d.greeted = true;
    d.banner = c.welcome().trimmed();
    say(DiagnoseStage::Connecting, QStringLiteral("Logging in"));
    if (const Status st = c.login(); !st.ok()) {
        d.error = st.error().message;
        d.server = classifyFtpServer(d.features, d.banner);
        return d;
    }
    d.loggedIn = true;
    auto lost = [&]() -> bool {
        if (c.isConnected())
            return false;
        d.error = QStringLiteral("The PS5 closed the FTP connection during the checks.");
        return true;
    };

    // 3. FEAT.
    say(DiagnoseStage::Checking, QStringLiteral("Reading the server features (FEAT)"));
    if (auto f = c.features(); f.ok()) {
        d.features = f.value();
        d.featOk = !d.features.raw.isEmpty();
    }
    d.server = classifyFtpServer(d.features, d.banner);
    if (lost())
        return d;
    if (!token.checkpoint())
        return Error::cancelled();

    // 4. Passive mode: PASV, then EPSV (legacy _ftp_test_passive).
    say(DiagnoseStage::Checking, QStringLiteral("Checking passive mode"));
    if (auto p = c.command(QStringLiteral("PASV")); p.ok() && p->code == 227) {
        d.passive = true;
        d.passiveMode = QStringLiteral("PASV");
    } else if (c.isConnected()) {
        auto e = c.command(QStringLiteral("EPSV"));
        d.passive = e.ok() && e->code == 229;
        if (*d.passive)
            d.passiveMode = QStringLiteral("EPSV");
    }
    if (lost())
        return d;
    if (!ep.passive)
        d.notes << QStringLiteral("Passive mode is off in Settings › PS5 connection: the probes used active mode (PORT).");
    else if (d.passive == false)
        d.notes << QStringLiteral("The server refused PASV and EPSV. Transfers need active mode; a PC firewall usually blocks it.");

    // 5. Binary mode.
    say(DiagnoseStage::Checking, QStringLiteral("Checking binary mode (TYPE I)"));
    if (auto t = c.command(QStringLiteral("TYPE I")); t.ok())
        d.binary = t->code >= 200 && t->code < 300;
    if (lost())
        return d;
    if (!token.checkpoint())
        return Error::cancelled();

    // 6. Writable: a small probe file, removed again (legacy _ftp_test_writable).
    d.probePath = joinRemote(d.folder, QStringLiteral(".exfatbuilder_wtest_%1.tmp").arg(epoch));
    say(DiagnoseStage::Checking, QStringLiteral("Writing a probe file to %1").arg(d.folder));
    const Status up = c.upload(d.probePath, QByteArrayLiteral("exfat-builder writable test\n"), token);
    if (up.ok()) {
        d.writable = true;
        QString why;
        d.probeCleaned = removeProbe(c, ep, d.probePath, &why);
        d.writableDetail = d.probeCleaned ? QStringLiteral("ok")
                                          : QStringLiteral("Wrote the probe but could NOT delete it: %1 (%2)").arg(d.probePath, why);
    } else {
        const bool started = c.lastTransferStarted();
        if (started) {
            QString why;
            d.probeCleaned = removeProbe(c, ep, d.probePath, &why);
        }
        if (up.error().isCancelled()) {
            if (!d.probeCleaned)
                return Error(ErrorCode::Cancelled, QStringLiteral("Cancelled. The probe file %1 may remain on the PS5.").arg(d.probePath));
            return Error::cancelled();
        }
        d.writable = false;
        d.writableDetail = QStringLiteral("Write denied: %1").arg(up.error().message);
    }
    if (!c.isConnected()) {
        if (auto st = c.open(d.host, d.port, token); !st.ok()) {
            if (st.error().isCancelled())
                return Error::cancelled();
            d.error = QStringLiteral("The PS5 closed the FTP connection during the checks.");
            return d;
        }
    }

    // 7. Speed test (legacy _ftpspeed_run).
    if (opt.speedTest) {
        SpeedTestResult& r = d.speed;
        r.ran = true;
        r.bytes = std::clamp<qint64>(opt.speedTestBytes, 1, 1024LL * 1024 * 1024);
        r.path = joinRemote(d.folder, QStringLiteral(".exfatbuilder_speedtest_%1.bin").arg(epoch));
        if (d.writable != true) {
            r.ran = false;
            r.path.clear();
            r.error = QStringLiteral("Not run: the folder is not writable.");
        } else {
            const QByteArray payload = randomPayload(r.bytes);
            say(DiagnoseStage::SpeedTest, QStringLiteral("Uploading %1").arg(formatBytes(r.bytes)));
            QBuffer in;
            in.setData(payload);
            in.open(QIODevice::ReadOnly);
            QElapsedTimer t;
            t.start();
            Status st = c.store(r.path, in, 0, bytesMoved, token);
            const bool wrote = st.ok() || c.lastTransferStarted();
            if (st.ok()) {
                r.uploadMBps = mibPerSecond(r.bytes, t.nsecsElapsed());
                say(DiagnoseStage::SpeedTest, QStringLiteral("Downloading %1").arg(formatBytes(r.bytes)));
                QByteArray back;
                back.reserve(r.bytes);
                QBuffer out(&back);
                out.open(QIODevice::WriteOnly);
                t.restart();
                st = c.retrieve(r.path, out, 0, bytesMoved, token);
                if (st.ok()) {
                    r.downloadMBps = mibPerSecond(back.size(), t.nsecsElapsed());
                    r.readBackMatches = back == payload;
                }
            }
            if (!st.ok())
                r.error = st.error().message;
            if (wrote) {
                say(DiagnoseStage::SpeedTest, QStringLiteral("Removing the test file"));
                QString why;
                r.cleaned = removeProbe(c, ep, r.path, &why);
            }
            if (st.ok() == false && st.error().isCancelled()) {
                if (!r.cleaned)
                    return Error(ErrorCode::Cancelled,
                                 QStringLiteral("Cancelled. The speed-test file %1 could not be removed from the PS5; delete it by hand.").arg(r.path));
                return Error::cancelled();
            }
        }
    }
    if (c.isConnected())
        c.quit();
    return d;
}

} // namespace fw::ps5
