#pragma once

// fw::Job adapters of the FPKG engine.
//
//  fpkg.build    {source, output?, basePackage?, baseFolder?, exactSource?, updateOutput?,
//                 contentId?, contentVersion?, title?}
//  fpkg.addon    {payloadFolder, output?, contentId?, entitlementKey, volumeType?}
//  fpkg.backport {package?, game?, backport4xx?, backport5xx?, version?, output?, buildBase?}
//                (BackportUpdate.h)
//  fpkg.verify   {package, passcode?, integrity?}
//  fpkg.extract  {package, destination, passcode?}
//
// Settings are read from the job's snapshot (FpkgSettings::from), so per-job
// overrides (fpkg.outputConflict chosen in the UI's conflict dialog, a
// compression level...) apply to that job only.

#include "engines/fpkg/FpkgBuilder.h"
#include "jobs/Job.h"

#include <memory>

namespace fw::fpkg {

inline const QString kJobBuild = QStringLiteral("fpkg.build");
inline const QString kJobAddon = QStringLiteral("fpkg.addon");
inline const QString kJobVerify = QStringLiteral("fpkg.verify");
inline const QString kJobExtract = QStringLiteral("fpkg.extract");

class FpkgBuildJob final : public Job {
public:
    explicit FpkgBuildJob(BuildRequest request);
    QString kind() const override { return kJobBuild; }
    QString title() const override { return m_title; }
    QString subtitle() const override;
    QStringList phases() const override { return buildPhases(); }
    QJsonObject parameters() const override { return m_request.toJson(); }
    // Also the base package and folder of an update, and "<source>-pkg" when no output is named.
    QStringList busyPaths() const override;
    Status validate(const SettingsValues& settings) const override;
    Result<JobOutcome> run(JobContext& ctx) override;
    const BuildRequest& request() const { return m_request; }

private:
    BuildRequest m_request;
    QString m_title;
};

class FpkgAddonJob final : public Job {
public:
    explicit FpkgAddonJob(AddonRequest request);
    QString kind() const override { return kJobAddon; }
    QString title() const override;
    QString subtitle() const override { return QStringLiteral("FPKG add-on (prospero_ac, experimental)"); }
    QStringList phases() const override { return addonPhases(); }
    QJsonObject parameters() const override { return m_request.toJson(); }
    QStringList busyPaths() const override; // the add-on folder and the output folder
    Status validate(const SettingsValues& settings) const override;
    Result<JobOutcome> run(JobContext& ctx) override;

private:
    AddonRequest m_request;
};

class FpkgVerifyJob final : public Job {
public:
    explicit FpkgVerifyJob(VerifyRequest request) : m_request(std::move(request)) {}
    QString kind() const override { return kJobVerify; }
    QString title() const override;
    QString subtitle() const override { return QStringLiteral("Verify a package with img_verify"); }
    QStringList phases() const override;
    QJsonObject parameters() const override { return m_request.toJson(); }
    Status validate(const SettingsValues& settings) const override;
    Result<JobOutcome> run(JobContext& ctx) override;

private:
    VerifyRequest m_request;
};

class FpkgExtractJob final : public Job {
public:
    explicit FpkgExtractJob(ExtractRequest request) : m_request(std::move(request)) {}
    QString kind() const override { return kJobExtract; }
    QString title() const override;
    QString subtitle() const override;
    QStringList phases() const override { return {QStringLiteral("Extracting")}; }
    QJsonObject parameters() const override { return m_request.toJson(); }
    Status validate(const SettingsValues& settings) const override;
    Result<JobOutcome> run(JobContext& ctx) override;

private:
    ExtractRequest m_request;
};

void registerFpkgJobs(JobFactory& jobs);

} // namespace fw::fpkg
