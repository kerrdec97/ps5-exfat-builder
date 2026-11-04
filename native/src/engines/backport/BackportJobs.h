#pragma once

// Queue job kinds of the backport module:
//   backport.run      {game, confirmDiffers?}     Scan → Patch → Package → Verify → Apply
//   backport.restore  {game, zip?} | {journal}    restore originals / roll back an interrupted apply
//                     {game, undoSdkFix: true}    undo the newest in-place SDK fix of the folder
//   backport.scan     {game?}                     report only (one game, or the library folders)
//   backport.sdkFix   {folder, mode?, targetSdk?, outputDir?}  SDK marker only (SdkFixer.h);
//                     mode "copy" (default) or "inPlace"
//   backport.audioScan {game, allExecutables?}    read-only audio report (AudioScan.h)
//   backport.pinProviders {folder?}               pin the AMPR/PlayGo libraries (ProviderSet.h)

#include "engines/backport/SdkFixer.h"
#include "jobs/Job.h"

#include <memory>

namespace fw::backport {

inline const QString kJobRun = QStringLiteral("backport.run");
inline const QString kJobRestore = QStringLiteral("backport.restore");
inline const QString kJobScan = QStringLiteral("backport.scan");
inline const QString kJobSdkFix = QStringLiteral("backport.sdkFix");
inline const QString kJobAudioScan = QStringLiteral("backport.audioScan");
inline const QString kJobPinProviders = QStringLiteral("backport.pinProviders");

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

class BackportSdkFixJob : public Job {
public:
    explicit BackportSdkFixJob(QJsonObject params);
    QString kind() const override { return kJobSdkFix; }
    QString title() const override;
    QString subtitle() const override;
    QStringList phases() const override;
    QJsonObject parameters() const override { return m_params; }
    Status validate(const SettingsValues& settings) const override;
    Result<JobOutcome> run(JobContext& ctx) override;

private:
    QJsonObject m_params;
};

class BackportAudioScanJob : public Job {
public:
    explicit BackportAudioScanJob(QJsonObject params);
    QString kind() const override { return kJobAudioScan; }
    QString title() const override;
    QStringList phases() const override { return {QStringLiteral("Scan")}; }
    QJsonObject parameters() const override { return m_params; }
    Status validate(const SettingsValues& settings) const override;
    Result<JobOutcome> run(JobContext& ctx) override;

private:
    QJsonObject m_params;
};

class BackportPinProvidersJob : public Job {
public:
    explicit BackportPinProvidersJob(QJsonObject params);
    QString kind() const override { return kJobPinProviders; }
    QString title() const override { return QStringLiteral("Pin the AMPR/PlayGo libraries"); }
    QStringList phases() const override { return {QStringLiteral("Pin")}; }
    QJsonObject parameters() const override { return m_params; }
    Status validate(const SettingsValues& settings) const override;
    Result<JobOutcome> run(JobContext& ctx) override;

private:
    QJsonObject m_params;
};

// The SDK fix options for a job: its parameters, then the settings snapshot
// (backport.targetSdk, originals/backport ZIP folders, encrypted policy,
// build verification, journal retention, free-space reserve).
SdkFixOptions sdkFixOptionsFrom(const SettingsValues& settings, const QJsonObject& params);

void registerBackportJobs(JobFactory& jobs);

} // namespace fw::backport
