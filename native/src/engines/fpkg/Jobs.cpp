#include "engines/fpkg/Jobs.h"

#include "engines/fpkg/BackportUpdate.h"

#include "core/FileUtil.h"
#include "engines/fpkg/PubCmd.h"
#include "engines/fpkg/SourceScanner.h"
#include "engines/fpkg/Toolchain.h"

#include <QDir>
#include <QFileInfo>

namespace fw::fpkg {

namespace {
QString nativePath(const QString& p) { return QDir::toNativeSeparators(p); }
} // namespace

FpkgBuildJob::FpkgBuildJob(BuildRequest request) : m_request(std::move(request))
{
    const QFileInfo fi(m_request.source);
    m_title = fi.isDir() ? fi.fileName() : fi.completeBaseName();
    if (!m_request.title.isEmpty()) {
        m_title = m_request.title;
    } else if (fi.isDir()) {
        // Cheap: one small file.
        const QString pj = findPathCaseInsensitive(fi.absoluteFilePath(), QStringLiteral("sce_sys/param.json"), false);
        if (!pj.isEmpty()) {
            auto prm = readParamJson(pj);
            if (prm.ok()) {
                const QString t = paramTitle(prm.value());
                const QString cid = prm->value(QStringLiteral("contentId")).toString();
                if (!t.isEmpty())
                    m_title = isValidContentId(cid) ? QStringLiteral("%1 (%2)").arg(t, titleIdOf(cid)) : t;
            }
        }
    }
    if (m_title.isEmpty())
        m_title = QStringLiteral("FPKG package");
}

QString FpkgBuildJob::subtitle() const
{
    const QString kind = m_request.isUpdate() ? QStringLiteral("FPKG update") : QStringLiteral("FPKG");
    return m_request.output.isEmpty() ? kind : QStringLiteral("%1 → %2").arg(kind, nativePath(m_request.output));
}

Status FpkgBuildJob::validate(const SettingsValues& settings) const
{
    return validateBuildRequest(m_request, FpkgSettings::from(settings));
}

Result<JobOutcome> FpkgBuildJob::run(JobContext& ctx)
{
    const LiveCpuLimitScope live(ctx.jobId);
    return runBuild(m_request, FpkgSettings::from(ctx.settings), ctx);
}

FpkgAddonJob::FpkgAddonJob(AddonRequest request) : m_request(std::move(request)) {}

QString FpkgAddonJob::title() const
{
    return m_request.contentId.isEmpty() ? QFileInfo(m_request.payloadFolder).fileName() : m_request.contentId;
}

Status FpkgAddonJob::validate(const SettingsValues& settings) const
{
    return validateAddonRequest(m_request, FpkgSettings::from(settings));
}

Result<JobOutcome> FpkgAddonJob::run(JobContext& ctx)
{
    const LiveCpuLimitScope live(ctx.jobId);
    return runAddon(m_request, FpkgSettings::from(ctx.settings), ctx);
}

QString FpkgVerifyJob::title() const { return QFileInfo(m_request.package).fileName(); }

QStringList FpkgVerifyJob::phases() const
{
    return {QStringLiteral("Checking the header"), QStringLiteral("Verifying with img_verify")};
}

Status FpkgVerifyJob::validate(const SettingsValues& settings) const
{
    if (!QFileInfo(m_request.package).isFile())
        return Error(ErrorCode::NotFound, QStringLiteral("%1 was not found.").arg(nativePath(m_request.package)));
    const FpkgSettings s = FpkgSettings::from(settings);
    const QString pc = passcodeProblem(m_request.passcode.isEmpty() ? s.passcode : m_request.passcode);
    if (!pc.isEmpty())
        return Error(ErrorCode::InvalidArgument, QStringLiteral("Package passcode: %1").arg(pc));
    const PubToolchain tc = locatePubTools(s.pubToolsPath);
    if (!tc.ok())
        return Error(ErrorCode::DependencyMissing, tc.problems.join(u' '));
    return success();
}

Result<JobOutcome> FpkgVerifyJob::run(JobContext& ctx)
{
    const LiveCpuLimitScope live(ctx.jobId);
    return runVerify(m_request, FpkgSettings::from(ctx.settings), ctx);
}

QString FpkgExtractJob::title() const { return QFileInfo(m_request.package).fileName(); }

QString FpkgExtractJob::subtitle() const
{
    return QStringLiteral("Extract → %1").arg(nativePath(m_request.destination));
}

Status FpkgExtractJob::validate(const SettingsValues& settings) const
{
    if (!QFileInfo(m_request.package).isFile())
        return Error(ErrorCode::NotFound, QStringLiteral("%1 was not found.").arg(nativePath(m_request.package)));
    const QFileInfo d(m_request.destination);
    if (d.exists() && (!d.isDir() || !QDir(d.absoluteFilePath()).isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System)))
        return Error(ErrorCode::AlreadyExists,
                     QStringLiteral("The destination folder is not empty: %1. Choose an empty or new folder.")
                         .arg(nativePath(m_request.destination)));
    const FpkgSettings s = FpkgSettings::from(settings);
    const QString pc = passcodeProblem(m_request.passcode.isEmpty() ? s.passcode : m_request.passcode);
    if (!pc.isEmpty())
        return Error(ErrorCode::InvalidArgument, QStringLiteral("Package passcode: %1").arg(pc));
    const PubToolchain tc = locatePubTools(s.pubToolsPath);
    if (!tc.ok())
        return Error(ErrorCode::DependencyMissing, tc.problems.join(u' '));
    return success();
}

Result<JobOutcome> FpkgExtractJob::run(JobContext& ctx)
{
    const LiveCpuLimitScope live(ctx.jobId);
    return runExtract(m_request, FpkgSettings::from(ctx.settings), ctx);
}

void registerFpkgJobs(JobFactory& jobs)
{
    jobs.add(kJobBuild, QStringLiteral("Build FPKG package"), [](const QJsonObject& p) -> Result<std::unique_ptr<Job>> {
        auto r = BuildRequest::fromJson(p);
        if (!r.ok())
            return r.takeError();
        return std::unique_ptr<Job>(new FpkgBuildJob(std::move(r).value()));
    });
    jobs.add(kJobAddon, QStringLiteral("Build FPKG add-on"), [](const QJsonObject& p) -> Result<std::unique_ptr<Job>> {
        auto r = AddonRequest::fromJson(p);
        if (!r.ok())
            return r.takeError();
        return std::unique_ptr<Job>(new FpkgAddonJob(std::move(r).value()));
    });
    jobs.add(kJobBackport, QStringLiteral("Build backport update packages"),
             [](const QJsonObject& p) -> Result<std::unique_ptr<Job>> {
                 auto r = BackportUpdateRequest::fromJson(p);
                 if (!r.ok())
                     return r.takeError();
                 return std::unique_ptr<Job>(new FpkgBackportJob(std::move(r).value()));
             });
    jobs.add(kJobVerify, QStringLiteral("Verify FPKG package"), [](const QJsonObject& p) -> Result<std::unique_ptr<Job>> {
        auto r = VerifyRequest::fromJson(p);
        if (!r.ok())
            return r.takeError();
        return std::unique_ptr<Job>(new FpkgVerifyJob(std::move(r).value()));
    });
    jobs.add(kJobExtract, QStringLiteral("Extract FPKG package"), [](const QJsonObject& p) -> Result<std::unique_ptr<Job>> {
        auto r = ExtractRequest::fromJson(p);
        if (!r.ok())
            return r.takeError();
        return std::unique_ptr<Job>(new FpkgExtractJob(std::move(r).value()));
    });
}

} // namespace fw::fpkg
