#include "connect/ps5/Module.h"

#include "connect/ps5/ConsoleBackup.h"
#include "connect/ps5/CrashDumps.h"
#include "connect/ps5/DpiInstaller.h"
#include "connect/ps5/Endpoint.h"
#include "connect/ps5/FtpDiagnostics.h"
#include "connect/ps5/FtpTransfer.h"
#include "connect/ps5/MountConfig.h"
#include "connect/ps5/PayloadSender.h"
#include "connect/ps5/RemotePath.h"
#include "connect/ps5/SmpApi.h"
#include "connect/ps5/Y2jb.h"
#include "core/FileUtil.h"
#include "core/Settings.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUuid>

#include <algorithm>
#include <memory>

namespace fw::ps5 {

namespace {

QStringList stringArray(const QJsonValue& v)
{
    // Blank entries are dropped; a path keeps its exact characters (a file
    // name may begin or end with a space).
    QStringList out;
    for (const auto& x : v.toArray())
        if (!x.toString().trimmed().isEmpty())
            out << x.toString();
    return out;
}

// Settings, then per-job "host" / "port" overrides from the parameters.
ConsoleEndpoint endpointFor(const QJsonObject& p, const SettingsValues& s)
{
    ConsoleEndpoint ep = endpointFromSettings(s);
    const QString host = p.value(QStringLiteral("host")).toString().trimmed();
    if (!host.isEmpty())
        ep.host = host;
    if (p.contains(QStringLiteral("parallel")))
        ep.parallelTransfers = std::clamp(p.value(QStringLiteral("parallel")).toInt(1), 1, 8);
    return ep;
}

quint16 portParam(const QJsonObject& p, quint16 fallback)
{
    const int v = p.value(QStringLiteral("port")).toInt(0);
    return v >= 1 && v <= 65535 ? quint16(v) : fallback;
}

Status needHost(const ConsoleEndpoint& ep)
{
    if (ep.host.isEmpty())
        return Error(ErrorCode::InvalidArgument, QStringLiteral("Enter your PS5 IP address in Settings › PS5 connection."));
    return success();
}

QJsonArray toJsonArray(const QStringList& l)
{
    return QJsonArray::fromStringList(l);
}

void fillTransferOutcome(JobOutcome& o, const TransferReport& r)
{
    o.details.insert(QStringLiteral("files"), r.files);
    o.details.insert(QStringLiteral("transferred"), r.transferred);
    o.details.insert(QStringLiteral("skipped"), r.skipped);
    o.details.insert(QStringLiteral("failed"), r.failed);
    o.details.insert(QStringLiteral("bytes"), r.bytes);
    o.details.insert(QStringLiteral("errors"), toJsonArray(r.errors));
    o.details.insert(QStringLiteral("removedPartials"), toJsonArray(r.removedPartials));
    o.details.insert(QStringLiteral("leftoverPartials"), toJsonArray(r.leftoverPartials));
    o.outputs = r.outputs;
    o.warnings = r.warnings;
    for (const QString& p : r.leftoverPartials)
        o.warnings << QStringLiteral("A partial file may remain on the PS5: %1").arg(p);
    if (r.failed > 0) {
        o.verification = Verification::Failed;
        o.verificationDetail = QStringLiteral("%1 of %2 file(s) failed: %3").arg(r.failed).arg(r.files).arg(r.errors.join(QStringLiteral("; ")));
    } else if (r.unverified > 0) {
        o.verification = Verification::Inconclusive;
        o.verificationDetail = QStringLiteral("%1 file(s) could not be size-checked (the server did not report sizes)").arg(r.unverified);
    } else {
        o.verification = Verification::Passed;
        o.verificationDetail = QStringLiteral("%1 file(s) size-checked").arg(r.verified);
    }
}

// Entries the plan left out (unsafe names, two sources for one target): each
// is a warning, and a run that left something out is not a clean "Passed".
void notePlanSkips(JobOutcome& o, const QStringList& skipped, const QString& notDone)
{
    for (const QString& s : skipped)
        o.warnings << QStringLiteral("%1: %2").arg(notDone, s);
    if (!skipped.isEmpty() && o.verification == Verification::Passed) {
        o.verification = Verification::Inconclusive;
        o.verificationDetail += QStringLiteral("; %1 %2 left out").arg(skipped.size()).arg(
            skipped.size() == 1 ? QStringLiteral("entry was") : QStringLiteral("entries were"));
    }
}

// Nothing to transfer because every source was left out.
Error nothingPlanned(const QStringList& skipped, const QString& notDone)
{
    QString list = skipped.mid(0, 3).join(QStringLiteral("; "));
    if (skipped.size() > 3)
        list += QStringLiteral("; and %1 more").arg(skipped.size() - 3);
    return Error(ErrorCode::InvalidArgument, QStringLiteral("%1: %2").arg(notDone, list));
}

Error cancelledWithCleanup(const TransferReport& r)
{
    QString msg = QStringLiteral("Cancelled after %1 of %2 file(s).").arg(r.transferred).arg(r.files);
    if (!r.removedPartials.isEmpty())
        msg += QStringLiteral(" Removed the partial file(s): %1.").arg(r.removedPartials.join(QStringLiteral(", ")));
    if (!r.leftoverPartials.isEmpty())
        msg += QStringLiteral(" Could not remove: %1.").arg(r.leftoverPartials.join(QStringLiteral(", ")));
    return Error(ErrorCode::Cancelled, msg);
}

// ---- ps5.ftp.upload --------------------------------------------------------------------

class UploadJob final : public Job {
public:
    explicit UploadJob(QJsonObject p) : m_p(std::move(p)) {}
    QString kind() const override { return jobkinds::kUpload; }
    QString title() const override
    {
        const QStringList s = stringArray(m_p.value(QStringLiteral("sources")));
        return s.size() == 1 ? QStringLiteral("Upload %1 to PS5").arg(QFileInfo(s.front()).fileName())
                             : QStringLiteral("Upload %1 items to PS5").arg(s.size());
    }
    QString subtitle() const override { return m_p.value(QStringLiteral("remoteDir")).toString(); }
    QStringList phases() const override { return {QStringLiteral("Preparing"), QStringLiteral("Uploading")}; }
    // The local files and folders being read: no other job may rewrite them meanwhile.
    QStringList busyPaths() const override
    {
        QStringList out;
        for (const QString& s : stringArray(m_p.value(QStringLiteral("sources"))))
            out << QDir::cleanPath(QFileInfo(s).absoluteFilePath());
        return out;
    }
    QJsonObject parameters() const override { return m_p; }

    Status validate(const SettingsValues& s) const override
    {
        const ConsoleEndpoint ep = endpointFor(m_p, s);
        FW_TRY(needHost(ep));
        const QStringList src = stringArray(m_p.value(QStringLiteral("sources")));
        if (src.isEmpty())
            return Error(ErrorCode::InvalidArgument, QStringLiteral("Choose a file or folder to upload."));
        for (const QString& f : src)
            if (!QFileInfo::exists(f))
                return Error(ErrorCode::NotFound, QStringLiteral("Path not found: %1").arg(QDir::toNativeSeparators(f)));
        // The folder itself and what each source becomes in it: a folder
        // called "system_ex" sent to "/" writes into /system_ex.
        const QString dir = remoteDir(ep);
        if (!m_p.value(QStringLiteral("allowProtectedPaths")).toBool()) {
            if (const QString prot = protectedUploadTarget(dir, src); !prot.isEmpty())
                return Error(ErrorCode::PermissionDenied,
                             protectedPathWarning(prot, prot == dir ? QStringLiteral("upload into") : QStringLiteral("upload to")));
        }
        return success();
    }

    Result<JobOutcome> run(JobContext& ctx) override
    {
        const ConsoleEndpoint ep = endpointFor(m_p, ctx.settings);
        TransferOptions opt;
        opt.endpoint = ep;
        opt.endpoint.ftpPort = portParam(m_p, ep.ftpPort);
        opt.parallel = ep.parallelTransfers;
        opt.resume = m_p.value(QStringLiteral("resume")).toBool(false);
        opt.retries = std::clamp(m_p.value(QStringLiteral("retries")).toInt(2), 0, 10);
        const QString dir = remoteDir(ep);
        ctx.progress->beginPhase(0, 0);
        auto plan = planUpload(stringArray(m_p.value(QStringLiteral("sources"))), dir, ctx.token);
        if (!plan.ok())
            return plan.takeError();
        if (plan->files.empty() && !plan->skipped.isEmpty())
            return nothingPlanned(plan->skipped, QStringLiteral("Not uploaded"));
        const DestinationInfo dest = classifyDestination(dir);
        ctx.log.info(QStringLiteral("[FTP] Uploading %1 file(s), %2, to %3:%4%5 (%6) over %7 connection(s)")
                         .arg(plan->files.size())
                         .arg(formatBytes(plan->totalBytes))
                         .arg(ep.host)
                         .arg(opt.endpoint.ftpPort)
                         .arg(dir, dest.label)
                         .arg(opt.parallel));
        ctx.progress->beginPhase(1, plan->totalBytes, qint64(plan->files.size()));
        auto rep = runUpload(plan.value(), opt, ctx.token, ctx.progress, ctx.log);
        if (!rep.ok())
            return rep.takeError();
        if (rep->cancelled || ctx.token.isCancelled())
            return cancelledWithCleanup(rep.value());
        JobOutcome o;
        fillTransferOutcome(o, rep.value());
        notePlanSkips(o, plan->skipped, QStringLiteral("Not uploaded"));
        o.details.insert(QStringLiteral("skippedNames"), toJsonArray(plan->skipped));
        o.details.insert(QStringLiteral("remoteDir"), dir);
        o.details.insert(QStringLiteral("destination"), dest.label);
        o.summary = QStringLiteral("Uploaded %1 file(s) (%2) to %3").arg(rep->transferred).arg(formatBytes(rep->bytes)).arg(dir);
        if (rep->skipped)
            o.summary += QStringLiteral(", %1 skipped").arg(rep->skipped);
        return o;
    }

private:
    QString remoteDir(const ConsoleEndpoint& ep) const
    {
        const QString d = m_p.value(QStringLiteral("remoteDir")).toString().trimmed();
        return normalizeRemotePath(d.isEmpty() ? ep.gamesFolder : d);
    }
    QJsonObject m_p;
};

// ---- ps5.ftp.download ------------------------------------------------------------------

class DownloadJob final : public Job {
public:
    explicit DownloadJob(QJsonObject p) : m_p(std::move(p)) {}
    QString kind() const override { return jobkinds::kDownload; }
    QString title() const override
    {
        const QStringList s = stringArray(m_p.value(QStringLiteral("sources")));
        return s.size() == 1 ? QStringLiteral("Download %1 from PS5").arg(remoteFileName(s.front()))
                             : QStringLiteral("Download %1 items from PS5").arg(s.size());
    }
    QString subtitle() const override { return QDir::toNativeSeparators(m_p.value(QStringLiteral("localDir")).toString()); }
    QStringList phases() const override { return {QStringLiteral("Listing"), QStringLiteral("Downloading")}; }
    QJsonObject parameters() const override { return m_p; }

    Status validate(const SettingsValues& s) const override
    {
        FW_TRY(needHost(endpointFor(m_p, s)));
        if (stringArray(m_p.value(QStringLiteral("sources"))).isEmpty())
            return Error(ErrorCode::InvalidArgument, QStringLiteral("Select files or folders on the PS5 to download."));
        if (m_p.value(QStringLiteral("localDir")).toString().trimmed().isEmpty())
            return Error(ErrorCode::InvalidArgument, QStringLiteral("Choose where to save the downloaded files."));
        return success();
    }

    Result<JobOutcome> run(JobContext& ctx) override
    {
        const ConsoleEndpoint ep = endpointFor(m_p, ctx.settings);
        TransferOptions opt;
        opt.endpoint = ep;
        opt.endpoint.ftpPort = portParam(m_p, ep.ftpPort);
        opt.parallel = ep.parallelTransfers;
        opt.resume = m_p.value(QStringLiteral("resume")).toBool(false);
        opt.overwriteLocal = m_p.value(QStringLiteral("overwrite")).toBool(false);
        opt.retries = std::clamp(m_p.value(QStringLiteral("retries")).toInt(2), 0, 10);
        const QString localDir = QDir::fromNativeSeparators(m_p.value(QStringLiteral("localDir")).toString().trimmed());
        ctx.progress->beginPhase(0, 0);
        TransferPlan plan;
        {
            FtpClient c(ftpOptionsFor(opt.endpoint));
            FW_TRY(c.open(ep.host, opt.endpoint.ftpPort, ctx.token));
            auto p = planDownload(c, stringArray(m_p.value(QStringLiteral("sources"))), localDir, ctx.token);
            if (!p.ok())
                return p.takeError();
            plan = std::move(p.value());
            c.quit();
        }
        if (plan.files.empty() && !plan.skipped.isEmpty())
            return nothingPlanned(plan.skipped, QStringLiteral("Not downloaded"));
        QDir().mkpath(localDir);
        FW_TRY(ensureFreeSpace(localDir, plan.totalBytes));
        ctx.progress->beginPhase(1, plan.totalBytes, qint64(plan.files.size()));
        auto rep = runDownload(plan, opt, ctx.token, ctx.progress, ctx.log);
        if (!rep.ok())
            return rep.takeError();
        if (rep->cancelled || ctx.token.isCancelled())
            return cancelledWithCleanup(rep.value());
        JobOutcome o;
        fillTransferOutcome(o, rep.value());
        notePlanSkips(o, plan.skipped, QStringLiteral("Not downloaded"));
        o.details.insert(QStringLiteral("skippedNames"), toJsonArray(plan.skipped));
        o.summary = QStringLiteral("%1 file(s) downloaded to %2").arg(rep->transferred).arg(QDir::toNativeSeparators(localDir));
        if (rep->skipped)
            o.summary += QStringLiteral(", %1 skipped").arg(rep->skipped);
        return o;
    }

private:
    QJsonObject m_p;
};

// ---- ps5.payload.send ------------------------------------------------------------------

class PayloadJob final : public Job {
public:
    explicit PayloadJob(QJsonObject p) : m_p(std::move(p)) {}
    QString kind() const override { return jobkinds::kPayloadSend; }
    QString title() const override
    {
        return QStringLiteral("Send %1").arg(QFileInfo(m_p.value(QStringLiteral("file")).toString()).fileName());
    }
    QJsonObject parameters() const override { return m_p; }
    QStringList phases() const override { return {QStringLiteral("Sending")}; }

    Status validate(const SettingsValues& s) const override
    {
        FW_TRY(needHost(endpointFor(m_p, s)));
        const QString t = target();
        if (t != u"payload" && t != u"shadowmount" && t != u"micromount")
            return Error(ErrorCode::InvalidArgument, QStringLiteral("Unknown payload target \"%1\".").arg(t));
        if (!QFileInfo(m_p.value(QStringLiteral("file")).toString()).isFile())
            return Error(ErrorCode::NotFound, QStringLiteral("Payload file not found: %1").arg(
                QDir::toNativeSeparators(m_p.value(QStringLiteral("file")).toString())));
        return success();
    }

    Result<JobOutcome> run(JobContext& ctx) override
    {
        const ConsoleEndpoint ep = endpointFor(m_p, ctx.settings);
        const QString t = target();
        const quint16 defPort = t == u"shadowmount" ? ep.shadowMountPort : t == u"micromount" ? ep.microMountPort : ep.payloadPort;
        const quint16 port = portParam(m_p, defPort);
        const QString file = m_p.value(QStringLiteral("file")).toString();
        PayloadOptions opt;
        opt.connectTimeoutMs = ep.connectTimeoutMs;
        opt.sendTimeoutMs = ep.idleTimeoutMs;
        ctx.progress->beginPhase(0, QFileInfo(file).size(), 1);
        qint64 last = 0;
        auto r = sendPayloadFile(ep.host, port, file, opt, ctx.token, [&](qint64 sent, qint64) {
            ctx.progress->advance(sent - last);
            last = sent;
        });
        if (!r.ok())
            return r.takeError();
        JobOutcome o;
        o.summary = QStringLiteral("Sent \"%1\" to %2:%3 in %4 s")
                        .arg(QFileInfo(file).fileName(), ep.host)
                        .arg(port)
                        .arg(double(r->elapsedMs) / 1000.0, 0, 'f', 1);
        o.details.insert(QStringLiteral("bytes"), r->bytes);
        o.details.insert(QStringLiteral("port"), port);
        o.verification = r->bytes == QFileInfo(file).size() ? Verification::Passed : Verification::Failed;
        o.verificationDetail = QStringLiteral("%1 of %2 bytes sent").arg(r->bytes).arg(QFileInfo(file).size());
        o.warnings << QStringLiteral("Sending a payload while another is resident can crash the kernel. Reboot between "
                                     "unrelated payloads.");
        ctx.log.info(o.summary);
        return o;
    }

private:
    QString target() const { return m_p.value(QStringLiteral("target")).toString(QStringLiteral("payload")); }
    QJsonObject m_p;
};

// ---- ps5.dpi.install ---------------------------------------------------------------------

class DpiJob final : public Job {
public:
    explicit DpiJob(QJsonObject p) : m_p(std::move(p)) {}
    QString kind() const override { return jobkinds::kDpiInstall; }
    QString title() const override
    {
        return QStringLiteral("Install %1").arg(QFileInfo(m_p.value(QStringLiteral("package")).toString()).fileName());
    }
    QJsonObject parameters() const override { return m_p; }
    QStringList phases() const override { return {QStringLiteral("Sending package"), QStringLiteral("Waiting for the PS5")}; }

    Status validate(const SettingsValues& s) const override
    {
        FW_TRY(needHost(endpointFor(m_p, s)));
        if (!QFileInfo(m_p.value(QStringLiteral("package")).toString()).isFile())
            return Error(ErrorCode::NotFound, QStringLiteral("No PKG file selected. Browse for the PKG file first."));
        return success();
    }

    Result<JobOutcome> run(JobContext& ctx) override
    {
        const ConsoleEndpoint ep = endpointFor(m_p, ctx.settings);
        const quint16 port = portParam(m_p, ep.dpiPort);
        const QString pkg = m_p.value(QStringLiteral("package")).toString();
        DpiOptions opt;
        opt.connectTimeoutMs = ep.connectTimeoutMs;
        opt.idleTimeoutMs = std::max(ep.idleTimeoutMs, 30000);
        ctx.log.info(QStringLiteral("[Y2JB] Target: http://%1:%2/upload").arg(ep.host).arg(port));
        ctx.progress->beginPhase(0, QFileInfo(pkg).size(), 1);
        qint64 last = 0;
        bool waiting = false;
        auto r = installPackage(ep.host, port, pkg, opt, ctx.token, [&](qint64 sent, qint64 total) {
            ctx.progress->advance(sent - last);
            last = sent;
            if (sent == total && !waiting) {
                waiting = true;
                ctx.progress->beginPhase(1, 0);
            }
        });
        if (!r.ok()) {
            // A half-received upload stays in /user/data/tmp: after a failure,
            // and after a cancel once package data was on its way. The cancel
            // path does not wait long for a console that is gone.
            const bool cancelled = r.error().isCancelled();
            if (!cancelled || last > 0) {
                DpiOptions clear = opt;
                if (cancelled)
                    clear.connectTimeoutMs = std::min(clear.connectTimeoutMs, 3000);
                if (clearDpiTemp(ep.host, port, clear).ok())
                    ctx.log.info(QStringLiteral("[Y2JB] Cleared the installer's temporary folder (/user/data/tmp)."));
                else
                    ctx.log.warn(QStringLiteral("[Y2JB] Could not clear the installer's temporary folder; part of the "
                                                "package may remain in /user/data/tmp on the PS5."));
            }
            return r.takeError();
        }
        if (!r->accepted) {
            // DPIv2 streams the upload to /user/data/tmp first; free it again.
            if (clearDpiTemp(ep.host, port, opt).ok())
                ctx.log.info(QStringLiteral("[Y2JB] Cleared the installer's temporary folder (/user/data/tmp)."));
            return Error(ErrorCode::ExternalToolFailed,
                         QStringLiteral("The PS5 did not accept the package: %1 Make sure etaHEN runs with Direct Package "
                                        "Installer V2 on (DPI_v2=1 in /data/etaHEN/config.ini, or the etaHEN toolbox).")
                             .arg(dpiFailureReason(r->status, r->body)),
                         QString::fromUtf8(r->body.left(200)));
        }
        JobOutcome o;
        o.summary = QStringLiteral("%1 is now installing on the PS5 — check the console's download queue.")
                        .arg(QFileInfo(pkg).fileName());
        o.details.insert(QStringLiteral("httpStatus"), r->status);
        o.details.insert(QStringLiteral("response"), QString::fromUtf8(r->body.left(200)));
        o.verification = Verification::Skipped;
        return o;
    }

private:
    QJsonObject m_p;
};

// ---- ps5.y2jb.autoBlock --------------------------------------------------------------------

class AutoBlockJob final : public Job {
public:
    explicit AutoBlockJob(QJsonObject p) : m_p(std::move(p)) {}
    QString kind() const override { return jobkinds::kY2jbAutoBlock; }
    QString title() const override { return QStringLiteral("Y2JB: block YouTube updates (%1)").arg(regionName()); }
    QJsonObject parameters() const override { return m_p; }

    Status validate(const SettingsValues& s) const override
    {
        FW_TRY(needHost(endpointFor(m_p, s)));
        if (!y2jbRegionFromName(regionName()))
            return Error(ErrorCode::InvalidArgument, QStringLiteral("Choose the region: USA, EU or JP."));
        if (!m_p.value(QStringLiteral("confirmed")).toBool())
            return Error(ErrorCode::InvalidArgument,
                         QStringLiteral("This patches param.json, appinfo.db and app.db for %1 (%2) directly on your "
                                        "PS5. Make sure you are in the PS5 Settings menu before proceeding, then confirm.")
                             .arg(regionName(), y2jbTitleId(*y2jbRegionFromName(regionName()))));
        return success();
    }

    Result<JobOutcome> run(JobContext& ctx) override
    {
        const ConsoleEndpoint ep = endpointFor(m_p, ctx.settings);
        const Y2jbRegion region = *y2jbRegionFromName(regionName());
        QString backupDir = m_p.value(QStringLiteral("backupDir")).toString();
        if (backupDir.isEmpty())
            backupDir = newConsoleBackupDir();
        FtpClient c(ftpOptionsFor(ep));
        FW_TRY(c.open(ep.host, portParam(m_p, ep.ftpPort), ctx.token));
        auto rep = autoBlockOverFtp(c, region, backupDir, ctx.token, ctx.log);
        c.quit();
        if (!rep.ok())
            return rep.takeError();
        JobOutcome o;
        QJsonArray files;
        int verified = 0;
        for (const auto& f : rep->files) {
            files.append(QJsonObject{{QStringLiteral("name"), f.name}, {QStringLiteral("path"), f.path},
                                     {QStringLiteral("status"), f.status}, {QStringLiteral("detail"), f.detail},
                                     {QStringLiteral("backup"), QDir::toNativeSeparators(f.backup)}});
            if (!f.backup.isEmpty())
                o.outputs << f.backup;
            if (f.status == u"✓")
                ++verified;
        }
        o.details.insert(QStringLiteral("files"), files);
        o.details.insert(QStringLiteral("backupDir"), QDir::toNativeSeparators(backupDir));
        o.summary = QStringLiteral("%1 file(s) patched and verified for %2").arg(verified).arg(regionName());
        if (!rep->ok()) {
            o.verification = Verification::Failed;
            o.verificationDetail = (rep->verifyFailures + rep->errors).join(QStringLiteral("; ")) +
                                   QStringLiteral(". The originals are saved in %1.").arg(QDir::toNativeSeparators(backupDir));
        } else if (verified == 0) {
            o.verification = Verification::Inconclusive;
            o.verificationDetail = QStringLiteral("none of the files was found on the PS5");
        } else {
            o.verification = Verification::Passed;
            o.verificationDetail = QStringLiteral("%1 file(s) read back with the new values").arg(verified);
        }
        return o;
    }

private:
    QString regionName() const { return m_p.value(QStringLiteral("region")).toString(QStringLiteral("USA")); }
    QJsonObject m_p;
};

// ---- ps5.shadowmount.stop ------------------------------------------------------------------

class StopJob final : public Job {
public:
    explicit StopJob(QJsonObject p) : m_p(std::move(p)) {}
    QString kind() const override { return jobkinds::kShadowMountStop; }
    QString title() const override { return QStringLiteral("ShadowMount+: safe to unplug"); }
    QJsonObject parameters() const override { return m_p; }

    Status validate(const SettingsValues& s) const override { return needHost(endpointFor(m_p, s)); }

    Result<JobOutcome> run(JobContext& ctx) override
    {
        const ConsoleEndpoint ep = endpointFor(m_p, ctx.settings);
        ctx.progress->beginPhase(0, 0);
        auto r = stopShadowMount(ep, ctx.token, [&](const QString& s) {
            ctx.progress->setCurrentItem(s);
            ctx.log.info(s);
        });
        if (!r.ok())
            return r.takeError();
        JobOutcome o;
        o.summary = stopOutcomeMessage(r->outcome);
        o.details.insert(QStringLiteral("unmountsStarted"), r->unmountsStarted);
        o.details.insert(QStringLiteral("unmountsCompleted"), r->unmountsCompleted);
        o.details.insert(QStringLiteral("source"), r->source);
        o.details.insert(QStringLiteral("problems"), toJsonArray(r->problems));
        o.verification = r->outcome == StopOutcome::Confirmed ? Verification::Passed : Verification::Inconclusive;
        o.verificationDetail = r->outcome == StopOutcome::Confirmed
                                   ? (r->unmountsStarted > 0 ? QStringLiteral("cleanup confirmed (%1), released %2/%3 mounts")
                                                                   .arg(r->source).arg(r->unmountsCompleted).arg(r->unmountsStarted)
                                                             : QStringLiteral("cleanup confirmed (%1)").arg(r->source))
                                   : stopOutcomeMessage(r->outcome);
        if (r->outcome != StopOutcome::Confirmed)
            o.warnings << stopOutcomeMessage(r->outcome);
        for (const QString& p : r->problems)
            o.warnings << QStringLiteral("ShadowMount+: %1").arg(p);
        return o;
    }

private:
    QJsonObject m_p;
};

// ---- ps5.shadowmount.eject (ShadowMount+ 1.7 API) -------------------------------------------

class EjectJob final : public Job {
public:
    explicit EjectJob(QJsonObject p) : m_p(std::move(p)) {}
    QString kind() const override { return jobkinds::kShadowMountEject; }
    QString title() const override
    {
        const QString path = m_p.value(QStringLiteral("path")).toString();
        return path.isEmpty() ? QStringLiteral("Eject the USB drive") : QStringLiteral("Eject %1").arg(path);
    }
    QStringList phases() const override { return {QStringLiteral("Asking ShadowMount+"), QStringLiteral("Ejecting")}; }
    QJsonObject parameters() const override { return m_p; }
    Status validate(const SettingsValues& s) const override { return needHost(endpointFor(m_p, s)); }

    Result<JobOutcome> run(JobContext& ctx) override
    {
        const ConsoleEndpoint ep = endpointFor(m_p, ctx.settings);
        const quint16 port = quint16(std::clamp(m_p.value(QStringLiteral("apiPort")).toInt(ep.smpApiPort), 1, 65535));
        const int pollMs = std::clamp(m_p.value(QStringLiteral("pollMs")).toInt(1000), 50, 10000);
        smp::ApiOptions api;
        api.connectTimeoutMs = ep.connectTimeoutMs;
        ctx.progress->beginPhase(0, 0);
        FW_TRY_ASSIGN(std::vector<smp::StorageItem> items, smp::storage(ep.host, port, api, ctx.token));
        const QString wanted = m_p.value(QStringLiteral("path")).toString().trimmed();
        const smp::StorageItem* target = nullptr;
        QStringList ejectable;
        for (const smp::StorageItem& i : items) {
            if (!i.ejectable)
                continue;
            ejectable << i.mountPoint;
            if (wanted.isEmpty() ? !target : i.mountPoint == wanted)
                target = &i;
        }
        if (!target)
            return Error(ErrorCode::NotFound, wanted.isEmpty() ? QStringLiteral("ShadowMount+ reports no USB drive that can be ejected.")
                                                               : QStringLiteral("%1 is not an ejectable USB drive (ejectable: %2).")
                                                                     .arg(wanted, ejectable.isEmpty() ? QStringLiteral("none") : ejectable.join(QStringLiteral(", "))));
        if (wanted.isEmpty() && ejectable.size() > 1)
            return Error(ErrorCode::InvalidArgument, QStringLiteral("More than one USB drive is connected (%1); choose which one to eject.")
                                                         .arg(ejectable.join(QStringLiteral(", "))));
        const smp::StorageItem drive = *target;
        ctx.log.info(QStringLiteral("[ShadowMount+] Ejecting %1 (%2)").arg(drive.mountPoint, drive.source));
        FW_TRY_ASSIGN(qint64 jobId, smp::eject(ep.host, port, drive, api, ctx.token));
        ctx.progress->beginPhase(1, 0);
        ctx.progress->setCurrentItem(drive.mountPoint);
        QElapsedTimer clock;
        clock.start();
        const int timeoutMs = std::clamp(m_p.value(QStringLiteral("timeoutMs")).toInt(120000), 1000, 600000);
        smp::JobStatus st;
        while (true) {
            if (!ctx.token.sleepFor(pollMs))
                return Error::cancelled();
            auto r = smp::jobStatus(ep.host, port, jobId, api, ctx.token);
            if (!r.ok()) {
                if (r.error().isCancelled())
                    return Error::cancelled();
                if (clock.elapsed() > timeoutMs)
                    return r.takeError();
                continue; // the API may be busy for a moment
            }
            st = r.value();
            if (!st.state.isEmpty())
                ctx.progress->setCurrentItem(QStringLiteral("%1 · %2").arg(drive.mountPoint, st.state));
            if (st.finished)
                break;
            if (clock.elapsed() > timeoutMs)
                return Error(ErrorCode::Timeout, QStringLiteral("ShadowMount+ did not finish ejecting %1 in time. Keep the drive "
                                                                "connected and check the console.").arg(drive.mountPoint));
        }
        JobOutcome o;
        o.details.insert(QStringLiteral("path"), drive.mountPoint);
        o.details.insert(QStringLiteral("state"), st.state);
        if (!st.succeeded) {
            QString why = st.unconfirmed ? QStringLiteral("the console could not confirm the unmount (usb_eject_unconfirmed)")
                                         : st.error.isEmpty() ? st.state : st.error;
            if (!st.blocking.isEmpty())
                why += QStringLiteral("; in use by %1").arg(st.blocking);
            return Error(ErrorCode::ExternalToolFailed,
                         QStringLiteral("%1 was not ejected: %2. Keep the drive connected.").arg(drive.mountPoint, why));
        }
        o.summary = QStringLiteral("%1 was ejected — safe to unplug.").arg(drive.mountPoint);
        o.verification = Verification::Passed;
        o.verificationDetail = QStringLiteral("ShadowMount+ reported the eject as completed");
        ctx.log.info(o.summary);
        return o;
    }

private:
    QJsonObject m_p;
};

// ---- ps5.crashdumps.capture ------------------------------------------------------------------

QString defaultCrashDumpFolder()
{
    const QString desktop = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    return QDir(desktop.isEmpty() ? QDir::homePath() : desktop).filePath(QStringLiteral("PS5 Logs"));
}

class CrashDumpsJob final : public Job {
public:
    explicit CrashDumpsJob(QJsonObject p) : m_p(std::move(p)) {}
    QString kind() const override { return jobkinds::kCrashDumps; }
    QString title() const override { return QStringLiteral("Capture PS5 crash dumps"); }
    QString subtitle() const override { return QDir::toNativeSeparators(outputDir()); }
    QStringList phases() const override { return {QStringLiteral("Watching")}; }
    QJsonObject parameters() const override { return m_p; }
    Status validate(const SettingsValues& s) const override { return needHost(endpointFor(m_p, s)); }

    Result<JobOutcome> run(JobContext& ctx) override
    {
        ConsoleEndpoint ep = endpointFor(m_p, ctx.settings);
        ep.ftpPort = portParam(m_p, ep.ftpPort);
        CrashCaptureOptions opt;
        opt.outputDir = outputDir();
        opt.runForMs = qint64(std::clamp(m_p.value(QStringLiteral("minutes")).toInt(0), 0, 24 * 60)) * 60 * 1000;
        if (m_p.contains(QStringLiteral("root")))
            opt.root = m_p.value(QStringLiteral("root")).toString();
        if (m_p.contains(QStringLiteral("settleMs"))) // tests
            opt.settleMs = std::clamp(m_p.value(QStringLiteral("settleMs")).toInt(), 100, 60000);
        ctx.progress->beginPhase(0, 0);
        auto r = captureCrashDumps(ep, opt, ctx.token, [&](const QString& text) {
            ctx.progress->setCurrentItem(text);
            ctx.log.info(QStringLiteral("[Crash dumps] %1").arg(text));
        });
        if (!r.ok())
            return r.takeError();
        const CrashCaptureReport& rep = r.value();
        JobOutcome o;
        QJsonArray crashes;
        int withCore = 0;
        for (const CapturedCrash& c : rep.crashes) {
            QJsonObject j;
            j.insert(QStringLiteral("folder"), c.folder);
            j.insert(QStringLiteral("localDir"), QDir::toNativeSeparators(c.localDir));
            j.insert(QStringLiteral("files"), toJsonArray(c.files));
            j.insert(QStringLiteral("core"), c.hasCore);
            j.insert(QStringLiteral("zip"), QDir::toNativeSeparators(c.zipPath));
            crashes.append(j);
            if (c.hasCore)
                ++withCore;
            if (!c.zipPath.isEmpty())
                o.outputs << c.zipPath;
        }
        o.details.insert(QStringLiteral("crashes"), crashes);
        o.details.insert(QStringLiteral("baseline"), toJsonArray(rep.baseline));
        o.details.insert(QStringLiteral("outputDir"), QDir::toNativeSeparators(opt.outputDir));
        o.warnings = rep.warnings;
        o.summary = rep.crashes.empty()
            ? QStringLiteral("No new crash on the console%1").arg(rep.stoppedByUser ? QStringLiteral(" (stopped)") : QString())
            : QStringLiteral("%1 crash dump(s) saved, %2 with a core, in %3")
                  .arg(rep.crashes.size())
                  .arg(withCore)
                  .arg(QDir::toNativeSeparators(opt.outputDir));
        o.verification = Verification::Skipped;
        return o;
    }

private:
    QString outputDir() const
    {
        const QString d = m_p.value(QStringLiteral("outputDir")).toString().trimmed();
        return d.isEmpty() ? defaultCrashDumpFolder() : d;
    }
    QJsonObject m_p;
};

// ---- ps5.ftp.diagnose ----------------------------------------------------------------------

QString mbps(double v)
{
    return v < 0 ? QStringLiteral("—") : QStringLiteral("%1 MB/s").arg(v, 0, 'f', v < 10 ? 1 : 0);
}

class DiagnoseJob final : public Job {
public:
    explicit DiagnoseJob(QJsonObject p) : m_p(std::move(p)) {}
    QString kind() const override { return jobkinds::kFtpDiagnose; }
    QString title() const override
    {
        return speedTest() ? QStringLiteral("PS5 FTP speed test") : QStringLiteral("PS5 FTP diagnostics");
    }
    QString subtitle() const override
    {
        const QString folder = m_p.value(QStringLiteral("folder")).toString().trimmed();
        return folder.isEmpty() ? QStringLiteral("Games folder from Settings") : folder;
    }
    QStringList phases() const override
    {
        QStringList p{QStringLiteral("Connecting"), QStringLiteral("Checking")};
        if (speedTest())
            p << QStringLiteral("Speed test");
        return p;
    }
    QJsonObject parameters() const override { return m_p; }

    // The folder is normalised like typed input ("games" → "/games"). The
    // checks write there, so a system folder needs the same confirmation as an
    // upload, also when it is the games folder from Settings.
    Status validate(const SettingsValues& s) const override
    {
        const ConsoleEndpoint ep = endpointFor(m_p, s);
        FW_TRY(needHost(ep));
        const QString folder = m_p.value(QStringLiteral("folder")).toString().trimmed();
        const QString dir = normalizeRemotePath(folder.isEmpty() ? ep.gamesFolder : folder);
        if (isProtectedRemotePath(dir) && !m_p.value(QStringLiteral("allowProtectedPaths")).toBool())
            return Error(ErrorCode::PermissionDenied, protectedPathWarning(dir, QStringLiteral("write test files into")));
        return success();
    }

    Result<JobOutcome> run(JobContext& ctx) override
    {
        ConsoleEndpoint ep = endpointFor(m_p, ctx.settings);
        ep.ftpPort = portParam(m_p, ep.ftpPort);
        DiagnoseOptions opt;
        opt.folder = m_p.value(QStringLiteral("folder")).toString();
        opt.speedTest = speedTest();
        if (m_p.contains(QStringLiteral("speedTestBytes")))
            opt.speedTestBytes = std::clamp<qint64>(m_p.value(QStringLiteral("speedTestBytes")).toInteger(), 4096, kSpeedTestBytes * 4);
        int phase = 0;
        ctx.progress->beginPhase(0, 0);
        auto r = diagnoseFtp(
            ep, opt, ctx.token,
            [&](DiagnoseStage stage, const QString& text) {
                const int want = stage == DiagnoseStage::Connecting ? 0 : stage == DiagnoseStage::Checking ? 1 : 2;
                if (want != phase && want < int(phases().size())) {
                    phase = want;
                    ctx.progress->beginPhase(phase, phase == 2 ? 2 * opt.speedTestBytes : 0);
                }
                ctx.progress->setCurrentItem(text);
                ctx.log.debug(QStringLiteral("[FTP] Diagnostics: %1").arg(text));
            },
            [&](qint64 n) { ctx.progress->advance(n); });
        if (!r.ok())
            return r.takeError();
        const FtpDiagnosis& d = r.value();
        logDiagnosis(ctx.log, d);
        JobOutcome o;
        o.details = d.toJson();
        QStringList parts;
        if (!d.reachable) {
            o.summary = QStringLiteral("Not reachable at %1:%2").arg(d.host).arg(d.port);
        } else {
            parts << QStringLiteral("Reachable in %1 ms").arg(d.rttMs);
            if (d.loggedIn)
                parts << QStringLiteral("logged in");
            if (d.writable)
                parts << (*d.writable ? QStringLiteral("%1 writable").arg(d.folder) : QStringLiteral("%1 not writable").arg(d.folder));
            if (d.speed.ran && d.speed.uploadMBps >= 0)
                parts << QStringLiteral("upload %1, download %2").arg(mbps(d.speed.uploadMBps), mbps(d.speed.downloadMBps));
            o.summary = parts.join(QStringLiteral(" · "));
        }
        // Warnings: everything that is not right but does not stop transfers outright.
        if (d.passive == false)
            o.warnings << QStringLiteral("The FTP server refused passive mode (PASV and EPSV).");
        if (d.binary == false)
            o.warnings << QStringLiteral("The FTP server refused binary mode (TYPE I).");
        if (d.writable == false)
            o.warnings << QStringLiteral("Cannot write to %1. %2").arg(d.folder, d.writableDetail);
        if (!d.probeCleaned)
            o.warnings << QStringLiteral("The probe file %1 could not be removed from the PS5; delete it by hand.").arg(d.probePath);
        if (opt.speedTest && !d.speed.error.isEmpty())
            o.warnings << QStringLiteral("Speed test: %1").arg(d.speed.error);
        if (d.speed.ran && !d.speed.cleaned)
            o.warnings << QStringLiteral("The speed-test file %1 (%2) could not be removed from the PS5; delete it by hand.")
                              .arg(d.speed.path, formatBytes(d.speed.bytes));
        for (const QString& w : o.warnings)
            ctx.log.warn(QStringLiteral("[FTP] %1").arg(w));
        if (!d.usable()) {
            o.verification = Verification::Failed;
            o.verificationDetail = d.error.isEmpty() ? QStringLiteral("the FTP server could not be used") : d.error;
        } else if (d.speed.readBackMatches == false) {
            o.verification = Verification::Failed;
            o.verificationDetail = QStringLiteral("the speed-test file read back from the PS5 differs from what was sent");
        } else if (!o.warnings.isEmpty() || !d.error.isEmpty()) {
            o.verification = Verification::Inconclusive;
            o.verificationDetail = !d.error.isEmpty() ? d.error : o.warnings.join(u' ');
        } else {
            o.verification = Verification::Passed;
            o.verificationDetail = opt.speedTest ? QStringLiteral("every check passed; the speed-test file read back identical")
                                                 : QStringLiteral("every check passed");
        }
        return o;
    }

private:
    bool speedTest() const { return m_p.value(QStringLiteral("speedTest")).toBool(false); }

    static void logDiagnosis(const Log& log, const FtpDiagnosis& d)
    {
        auto yn = [](const std::optional<bool>& b) {
            return !b ? QStringLiteral("not checked") : *b ? QStringLiteral("yes") : QStringLiteral("no");
        };
        if (!d.reachable) {
            log.info(QStringLiteral("[FTP] Diagnostics %1:%2: not reachable — %3").arg(d.host).arg(d.port).arg(d.error));
            return;
        }
        log.info(QStringLiteral("[FTP] Diagnostics %1:%2: reachable (%3 ms), banner \"%4\", login %5, FEAT %6, passive %7%8, "
                                "binary %9")
                     .arg(d.host)
                     .arg(d.port)
                     .arg(d.rttMs)
                     .arg(d.banner, d.loggedIn ? QStringLiteral("ok") : QStringLiteral("refused"),
                          d.featOk ? d.features.raw.simplified() : QStringLiteral("not supported"), yn(d.passive),
                          d.passiveMode.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(d.passiveMode), yn(d.binary)));
        if (d.writable)
            log.info(QStringLiteral("[FTP] Writable %1: %2 — %3").arg(d.folder, yn(d.writable), d.writableDetail));
        log.info(QStringLiteral("[FTP] Server: %1 (%2); destination %3").arg(d.server.label, d.server.confidence, d.destination));
        if (d.speed.ran)
            log.info(QStringLiteral("[FTP] Speed test (%1): upload %2, download %3, read-back %4, removed %5")
                         .arg(formatBytes(d.speed.bytes), mbps(d.speed.uploadMBps), mbps(d.speed.downloadMBps),
                              yn(d.speed.readBackMatches), d.speed.cleaned ? QStringLiteral("yes") : QStringLiteral("NO")));
        if (!d.error.isEmpty())
            log.warn(QStringLiteral("[FTP] Diagnostics stopped: %1").arg(d.error));
    }

    QJsonObject m_p;
};

// ---- settings ------------------------------------------------------------------------------

QString hostProblem(const QVariant& v)
{
    const QString s = v.toString().trimmed();
    if (s.isEmpty())
        return {};
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9.:\\-\\[\\]]+$"));
    return re.match(s).hasMatch() ? QString() : QStringLiteral("enter an IP address such as 192.168.1.20.");
}

QString remoteFolderProblem(const QVariant& v)
{
    return v.toString().trimmed().startsWith(u'/') ? QString() : QStringLiteral("the folder must start with \"/\".");
}

std::optional<QVariant> nonEmpty(const QVariant& v)
{
    if (v.toString().trimmed().isEmpty())
        return std::nullopt;
    return v.toString().trimmed();
}

void addPort(SettingsSchema& s, const QString& key, const QString& group, const QString& label, const QString& desc,
             int def, int order)
{
    SettingDef d;
    d.key = key;
    d.section = QStringLiteral("PS5 connection");
    d.group = group;
    d.label = label;
    d.description = desc;
    d.type = SettingType::Int;
    d.defaultValue = def;
    d.minimum = 1;
    d.maximum = 65535;
    d.order = order;
    s.add(d);
}

DependencyInfo probeSqlite(const SettingsValues&)
{
    DependencyInfo i;
    i.id = QStringLiteral("qt.sqlite");
    i.name = QStringLiteral("SQLite database support (Qt SQL)");
    i.purpose = QStringLiteral("Y2JB update blocking edits app.db and appinfo.db.");
    if (!QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE"))) {
        i.status = DependencyStatus::Missing;
        i.detail = QStringLiteral("The Qt SQL plugin qsqlite was not found.");
        i.howToFix = QStringLiteral("Reinstall the app; the sqldrivers folder next to it is missing.");
        return i;
    }
    const QString name = QStringLiteral("fw-probe-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    bool json = false;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        db.setDatabaseName(QStringLiteral(":memory:"));
        if (db.open()) {
            QSqlQuery q(db);
            json = q.exec(QStringLiteral("SELECT json('{}')"));
            if (q.exec(QStringLiteral("SELECT sqlite_version()")) && q.next())
                i.version = q.value(0).toString();
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(name);
    i.status = json ? DependencyStatus::Ready : DependencyStatus::Outdated;
    i.detail = json ? QStringLiteral("SQLite with JSON functions") : QStringLiteral("SQLite has no JSON functions; app.db cannot be patched.");
    i.howToFix = json ? QString() : QStringLiteral("Use a build whose Qt SQLite includes JSON1 (SQLite 3.38 or later).");
    return i;
}

} // namespace

void registerModule(ModuleContext& ctx)
{
    auto& s = ctx.settings;
    int order = 0;
    {
        SettingDef d;
        d.key = keys::kConsoleIp;
        d.section = QStringLiteral("PS5 connection");
        d.group = QStringLiteral("Console");
        d.label = QStringLiteral("Console IP address");
        d.description = QStringLiteral("The PS5's address on your network (Settings › Network › Connection Status › View "
                                       "Connection Status on the console). Used for FTP, payloads, the kernel log and package installs.");
        d.type = SettingType::String;
        d.defaultValue = QString();
        d.validator = hostProblem;
        d.order = order++;
        s.add(d);
    }
    addPort(s, keys::kFtpPort, QStringLiteral("Console"), QStringLiteral("FTP port"),
            QStringLiteral("Port of the FTP server running on the PS5: ftpsrv 2121, etaHEN 1337, zftpd 2120."), 2121, order++);
    {
        SettingDef d;
        d.key = keys::kGamesFolder;
        d.section = QStringLiteral("PS5 connection");
        d.group = QStringLiteral("Console");
        d.label = QStringLiteral("PS5 games folder");
        d.description = QStringLiteral("Where uploads go when you do not pick a folder on the console.");
        d.type = SettingType::String;
        d.defaultValue = QStringLiteral("/data/etaHEN/games/");
        d.validator = remoteFolderProblem;
        d.order = order++;
        s.add(d);
    }
    addPort(s, keys::kPayloadPort, QStringLiteral("Payloads"), QStringLiteral("Payload port"),
            QStringLiteral("Port of the ELF loader for the Payloads page: elfldr 9021 (9020 is the exploit's own loader). 9090 is etaHEN's package installer, not a loader."), 9021, order++);
    addPort(s, keys::kShadowMountPort, QStringLiteral("Payloads"), QStringLiteral("ShadowMount+ ELF port"),
            QStringLiteral("Port used to send shadowmountplus.elf (the ELF loader, usually 9021)."), 9021, order++);
    addPort(s, keys::kMicroMountPort, QStringLiteral("Payloads"), QStringLiteral("MicroMount ELF port"),
            QStringLiteral("Port used to send micromount.elf (the ELF loader, usually 9021)."), 9021, order++);
    addPort(s, keys::kKlogPort, QStringLiteral("Services"), QStringLiteral("Kernel log port"),
            QStringLiteral("Port of the kernel log streamer: klogsrv 3232, etaHEN 9081 (turn on Klog=1). ShadowMount+ \"Safe to unplug\" "
                           "watches it."), 3232, order++);
    addPort(s, keys::kDpiPort, QStringLiteral("Services"), QStringLiteral("Package installer port"),
            QStringLiteral("Port of etaHEN's Direct Package Installer V2 (12800; off by default: set DPI_v2=1 in /data/etaHEN/config.ini), used by Y2JB installs."), 12800, order++);
    addPort(s, keys::kSmpApiPort, QStringLiteral("Services"), QStringLiteral("ShadowMount+ API port"),
            QStringLiteral("Port of the ShadowMount+ 1.7 HTTP API (10101): version, free space, USB eject and logs. Optional; FTP and the kernel log are used when it does not answer."), 10101, order++);
    {
        SettingDef d;
        d.key = keys::kConnectTimeout;
        d.section = QStringLiteral("PS5 connection");
        d.group = QStringLiteral("Transfers");
        d.label = QStringLiteral("Connection timeout");
        d.description = QStringLiteral("How long to wait for the PS5 to accept a connection or answer a command.");
        d.type = SettingType::Int;
        d.defaultValue = 10;
        d.minimum = 1;
        d.maximum = 120;
        d.unit = QStringLiteral("s");
        d.order = order++;
        s.add(d);
    }
    {
        SettingDef d;
        d.key = keys::kIdleTimeout;
        d.section = QStringLiteral("PS5 connection");
        d.group = QStringLiteral("Transfers");
        d.label = QStringLiteral("Stalled transfer timeout");
        d.description = QStringLiteral("A transfer that moves no data for this long fails (and is retried). Raise it for "
                                       "slow USB drives on the console.");
        d.type = SettingType::Int;
        d.defaultValue = 30;
        d.minimum = 5;
        d.maximum = 600;
        d.unit = QStringLiteral("s");
        d.order = order++;
        s.add(d);
    }
    {
        SettingDef d;
        d.key = keys::kParallelTransfers;
        d.section = QStringLiteral("PS5 connection");
        d.group = QStringLiteral("Transfers");
        d.label = QStringLiteral("Files at the same time");
        d.description = QStringLiteral("FTP connections used for folder uploads and downloads. More helps with many small "
                                       "files; set 1 if the console's FTP server drops connections.");
        d.type = SettingType::Int;
        d.defaultValue = 2;
        d.minimum = 1;
        d.maximum = 8;
        d.unit = QStringLiteral("connections");
        d.perJob = true;
        d.order = order++;
        s.add(d);
    }
    {
        SettingDef d;
        d.key = keys::kPassiveMode;
        d.section = QStringLiteral("PS5 connection");
        d.group = QStringLiteral("Transfers");
        d.label = QStringLiteral("Passive FTP mode");
        d.description = QStringLiteral("The PC opens the data connections (PASV, then EPSV). Turn off only if the "
                                       "console's FTP server needs active mode; a PC firewall usually blocks that.");
        d.type = SettingType::Bool;
        d.defaultValue = true;
        d.advanced = true;
        d.order = order++;
        s.add(d);
    }

    {
        SettingDef d;
        d.key = keys::kAutoloaderFolder;
        d.section = QStringLiteral("PS5 connection");
        d.group = QStringLiteral("Autoloader");
        d.label = QStringLiteral("ps5_autoloader folder");
        d.description = QStringLiteral("The local ps5_autoloader folder the Autoloader tab edits (autoload.txt and the "
                                       "payloads next to it). Copy the folder to a USB drive or /data on the console "
                                       "yourself; nothing is sent from here.");
        d.type = SettingType::DirPath;
        d.defaultValue = QString();
        d.order = order++;
        s.add(d);
    }

    {
        SettingDef d;
        d.key = keys::kYoutubePackages;
        d.section = QStringLiteral("PS5 connection");
        d.group = QStringLiteral("Y2JB");
        d.label = QStringLiteral("YouTube packages folder");
        d.description = QStringLiteral("A folder with your own YouTube app .pkg files (for example v01.000.003 and "
                                       "v01.000.030). The Y2JB tab lists them with the firmware each one is for and "
                                       "installs the right one. Nothing is downloaded.");
        d.type = SettingType::DirPath;
        d.defaultValue = QString();
        d.order = order++;
        s.add(d);
    }

    s.addLegacyKey(QStringLiteral("ftp_ip"), keys::kConsoleIp, nonEmpty);
    s.addLegacyKey(QStringLiteral("ps5_ip"), keys::kConsoleIp, nonEmpty); // never written by the legacy app
    s.addLegacyKey(QStringLiteral("ftp_port"), keys::kFtpPort, nonEmpty);
    s.addLegacyKey(QStringLiteral("ftp_path"), keys::kGamesFolder, nonEmpty);
    s.addLegacyKey(QStringLiteral("pl_port"), keys::kPayloadPort, [](const QVariant& v) -> std::optional<QVariant> {
        // The legacy default 9090 is etaHEN DPI v1, never a payload loader.
        if (v.toString().trimmed().isEmpty())
            return std::nullopt;
        return v.toInt() == ports::kEtaHenDpiV1 ? QVariant(int(ports::kElfLoader)) : QVariant(v.toString().trimmed());
    });
    s.addLegacyKey(QStringLiteral("smp_payload_port"), keys::kShadowMountPort, nonEmpty);
    s.addLegacyKey(QStringLiteral("mm_payload_port"), keys::kMicroMountPort, nonEmpty);
    s.addLegacyKey(QStringLiteral("klog_port"), keys::kKlogPort, nonEmpty);
    s.addLegacyKey(QStringLiteral("y2jb_dpi_port"), keys::kDpiPort, nonEmpty);
    s.addLegacyKey(QStringLiteral("al_dir"), keys::kAutoloaderFolder, nonEmpty);

    ctx.dependencies.add(QStringLiteral("qt.sqlite"), QStringLiteral("SQLite database support (Qt SQL)"),
                         QStringLiteral("Y2JB update blocking edits app.db and appinfo.db."), probeSqlite);

    auto add = [&](const QString& kind, const QString& name, auto make) {
        ctx.jobs.add(kind, name, [make](const QJsonObject& p) -> Result<std::unique_ptr<Job>> { return make(p); });
    };
    add(jobkinds::kUpload, QStringLiteral("Upload to PS5"), [](const QJsonObject& p) { return std::unique_ptr<Job>(new UploadJob(p)); });
    add(jobkinds::kDownload, QStringLiteral("Download from PS5"), [](const QJsonObject& p) { return std::unique_ptr<Job>(new DownloadJob(p)); });
    add(jobkinds::kPayloadSend, QStringLiteral("Send payload"), [](const QJsonObject& p) { return std::unique_ptr<Job>(new PayloadJob(p)); });
    add(jobkinds::kDpiInstall, QStringLiteral("Install package (DPI)"), [](const QJsonObject& p) { return std::unique_ptr<Job>(new DpiJob(p)); });
    add(jobkinds::kY2jbAutoBlock, QStringLiteral("Y2JB auto-block"), [](const QJsonObject& p) { return std::unique_ptr<Job>(new AutoBlockJob(p)); });
    add(jobkinds::kShadowMountStop, QStringLiteral("ShadowMount+ safe to unplug"), [](const QJsonObject& p) { return std::unique_ptr<Job>(new StopJob(p)); });
    add(jobkinds::kShadowMountEject, QStringLiteral("ShadowMount+ eject USB"), [](const QJsonObject& p) { return std::unique_ptr<Job>(new EjectJob(p)); });
    add(jobkinds::kFtpDiagnose, QStringLiteral("FTP diagnostics"), [](const QJsonObject& p) { return std::unique_ptr<Job>(new DiagnoseJob(p)); });
    add(jobkinds::kCrashDumps, QStringLiteral("Capture crash dumps"), [](const QJsonObject& p) { return std::unique_ptr<Job>(new CrashDumpsJob(p)); });
}

bool migrateLegacyPayloadPort(SettingsStore& settings, const QString& markerPath)
{
    if (markerPath.isEmpty() || QFileInfo::exists(markerPath))
        return false;
    bool changed = false;
    if (settings.value(keys::kPayloadPort).toInt() == ports::kEtaHenDpiV1)
        changed = settings.setValue(keys::kPayloadPort, int(ports::kElfLoader)).ok();
    QDir().mkpath(QFileInfo(markerPath).absolutePath());
    (void)writeFileAtomic(markerPath, QByteArrayLiteral("ps5.payloadPort: 9090 -> 9021 checked\n"));
    return changed;
}

} // namespace fw::ps5
