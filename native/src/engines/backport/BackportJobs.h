#pragma once

// Queue job kinds of the backport module:
//   backport.run      {game, confirmDiffers?}     Scan → Patch → Package → Verify → Apply
//   backport.restore  {game, zip?} | {journal}    restore originals / roll back an interrupted apply
//   backport.scan     {game?}                     report only (one game, or the library folders)

#include "jobs/Job.h"

#include <memory>

namespace fw::backport {

inline const QString kJobRun = QStringLiteral("backport.run");
inline const QString kJobRestore = QStringLiteral("backport.restore");
inline const QString kJobScan = QStringLiteral("backport.scan");

class BackportRunJob : public Job {
public:
    explicit BackportRunJob(QJsonObject params);
    QString kind() const override { return kJobRun; }
    QString title() const override;
    QString subtitle() const override;
    QStringList phases() const override;
    QJsonObject parameters() const override { return m_params; }
    Status validate(const SettingsValues& settings) const override;
    Result<JobOutcome> run(JobContext& ctx) override;

private:
    QJsonObject m_params;
};

class BackportRestoreJob : public Job {
public:
    explicit BackportRestoreJob(QJsonObject params);
    QString kind() const override { return kJobRestore; }
    QString title() const override;
    QStringList phases() const override;
    QJsonObject parameters() const override { return m_params; }
    Status validate(const SettingsValues& settings) const override;
    Result<JobOutcome> run(JobContext& ctx) override;

private:
    QJsonObject m_params;
};

class BackportScanJob : public Job {
public:
    explicit BackportScanJob(QJsonObject params);
    QString kind() const override { return kJobScan; }
    QString title() const override;
    QStringList phases() const override { return {QStringLiteral("Scan")}; }
    QJsonObject parameters() const override { return m_params; }
    Result<JobOutcome> run(JobContext& ctx) override;

private:
    QJsonObject m_params;
};

void registerBackportJobs(JobFactory& jobs);

} // namespace fw::backport
