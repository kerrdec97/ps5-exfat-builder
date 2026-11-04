#include "connect/ps5/Module.h"

#include "connect/ps5/ConsoleBackup.h"
#include "connect/ps5/DpiInstaller.h"
#include "connect/ps5/Endpoint.h"
#include "connect/ps5/FtpTransfer.h"
#include "connect/ps5/MountConfig.h"
#include "connect/ps5/PayloadSender.h"
#include "connect/ps5/RemotePath.h"
#include "connect/ps5/Y2jb.h"
#include "core/FileUtil.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUuid>

#include <algorithm>
#include <memory>

namespace fw::ps5 {

namespace {

QStringList stringArray(const QJsonValue& v)
{
    QStringList out;
    for (const auto& x : v.toArray())
        if (!x.toString().trimmed().isEmpty())
            out << x.toString().trimmed();
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
        return Error(ErrorCode::InvalidArgument, QStringLiteral("Enter your PS5 IP address in Settings > PS5 connection."));
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
        const QString dir = remoteDir(ep);
        if (!dir.startsWith(u'/'))
            return Error(ErrorCode::InvalidArgument, QStringLiteral("The PS5 folder must start with \"/\"."));
        if (isProtectedRemotePath(dir) && !m_p.value(QStringLiteral("allowProtectedPaths")).toBool())
            return Error(ErrorCode::PermissionDenied, protectedPathWarning(dir, QStringLiteral("upload into")));
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
        for (const QString& s : plan.skipped)
            o.warnings << QStringLiteral("Not downloaded (unsafe name): %1").arg(s);
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
        if (!r.ok())
            return r.takeError();
        if (!r->accepted)
            return Error(ErrorCode::ExternalToolFailed,
                         QStringLiteral("The PS5 did not accept the package (HTTP %1). Make sure etaHEN is running with "
                                        "Direct Package Installer V2 enabled in Services.")
                             .arg(r->status),
                         QString::fromUtf8(r->body.left(200)));
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
        o.verification = r->outcome == StopOutcome::Confirmed ? Verification::Passed : Verification::Inconclusive;
        o.verificationDetail = r->outcome == StopOutcome::Confirmed
                                   ? QStringLiteral("released %1/%2 mounts").arg(r->unmountsCompleted).arg(r->unmountsStarted)
                                   : stopOutcomeMessage(r->outcome);
        if (r->outcome != StopOutcome::Confirmed)
            o.warnings << stopOutcomeMessage(r->outcome);
        return o;
    }

private:
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
        d.description = QStringLiteral("The PS5's address on your network (Settings > Network > View connection status on "
                                       "the console). Used for FTP, payloads, the kernel log and package installs.");
        d.type = SettingType::String;
        d.defaultValue = QString();
        d.validator = hostProblem;
        d.order = order++;
        s.add(d);
    }
    addPort(s, keys::kFtpPort, QStringLiteral("Console"), QStringLiteral("FTP port"),
            QStringLiteral("Port of the FTP server running on the PS5. ftpsrv uses 2121; etaHEN's FTP uses 1337."), 2121, order++);
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
            QStringLiteral("Port the payload loader listens on for the Payloads page (usually 9090)."), 9090, order++);
    addPort(s, keys::kShadowMountPort, QStringLiteral("Payloads"), QStringLiteral("ShadowMount+ ELF port"),
            QStringLiteral("Port used to send shadowmountplus.elf (the ELF loader, usually 9021)."), 9021, order++);
    addPort(s, keys::kMicroMountPort, QStringLiteral("Payloads"), QStringLiteral("MicroMount ELF port"),
            QStringLiteral("Port used to send micromount.elf (the ELF loader, usually 9021)."), 9021, order++);
    addPort(s, keys::kKlogPort, QStringLiteral("Services"), QStringLiteral("Kernel log port"),
            QStringLiteral("Port of klogsrv, the kernel log streamer (usually 3232). ShadowMount+ \"Safe to unplug\" "
                           "watches it."), 3232, order++);
    addPort(s, keys::kDpiPort, QStringLiteral("Services"), QStringLiteral("Package installer port"),
            QStringLiteral("Port of etaHEN's Direct Package Installer V2 (usually 12800), used by Y2JB installs."), 12800, order++);
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

    s.addLegacyKey(QStringLiteral("ftp_ip"), keys::kConsoleIp, nonEmpty);
    s.addLegacyKey(QStringLiteral("ps5_ip"), keys::kConsoleIp, nonEmpty); // never written by the legacy app
    s.addLegacyKey(QStringLiteral("ftp_port"), keys::kFtpPort, nonEmpty);
    s.addLegacyKey(QStringLiteral("ftp_path"), keys::kGamesFolder, nonEmpty);
    s.addLegacyKey(QStringLiteral("pl_port"), keys::kPayloadPort, nonEmpty);
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
}

} // namespace fw::ps5
