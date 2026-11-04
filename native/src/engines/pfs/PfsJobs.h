#pragma once

// Queue jobs of the PFS engine.
//
//   image.pfs.build   {source, output, format?, replaceExisting?, timestamp?, title?}
//                     phases Scanning → Compressing and writing → Verifying
//   image.pfs.verify  {image, source?, depth?}
//   image.pfs.extract {image, output, deep?, overwrite?}
//
// `format` is "ffpfsc" (compressed container) or "ffpfs" (folder image);
// without it the output extension decides (.ffpfs → folder image).
// `timestamp` pins the inode time (seconds) for reproducible builds.

#include "jobs/Job.h"
#include "jobs/Module.h"

#include "engines/pfs/PfsBuilder.h"

namespace fw::pfs {

void registerJobs(ModuleContext& ctx);

class PfsBuildJob final : public Job {
public:
    explicit PfsBuildJob(QJsonObject params);
    QString kind() const override;
    QString title() const override;
    QString subtitle() const override;
    QStringList phases() const override;
    QJsonObject parameters() const override { return m_params; }
    Status validate(const SettingsValues& settings) const override;
    Result<JobOutcome> run(JobContext& ctx) override;

    OutputFormat format() const;

private:
    QJsonObject m_params;
    QString m_source;
    QString m_output;
};

class PfsVerifyJob final : public Job {
public:
    explicit PfsVerifyJob(QJsonObject params);
    QString kind() const override;
    QString title() const override;
    QStringList phases() const override;
    QJsonObject parameters() const override { return m_params; }
    Status validate(const SettingsValues& settings) const override;
    Result<JobOutcome> run(JobContext& ctx) override;

private:
    QJsonObject m_params;
    QString m_image;
    QString m_source;
};

class PfsExtractJob final : public Job {
public:
    explicit PfsExtractJob(QJsonObject params);
    QString kind() const override;
    QString title() const override;
    QStringList phases() const override;
    QJsonObject parameters() const override { return m_params; }
    Status validate(const SettingsValues& settings) const override;
    Result<JobOutcome> run(JobContext& ctx) override;

private:
    QJsonObject m_params;
    QString m_image;
    QString m_output;
};

} // namespace fw::pfs
