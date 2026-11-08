#pragma once

#include "jobs/Module.h"

namespace fw::general {

// App-wide settings (General, Performance, Storage, Logging, Advanced) and the
// dependency probes every workflow shares.
void registerModule(ModuleContext& ctx);

// Keys declared here (consumers: JobQueue, main.cpp, FileUtil callers).
namespace keys {
inline const QString kConfirmOnClose = QStringLiteral("general.confirmCloseWhileBusy");
inline const QString kNotifyOnFinish = QStringLiteral("general.notifyOnFinish");
inline const QString kRestoreQueue = QStringLiteral("general.restoreQueueOnStart");
inline const QString kLastSourceDir = QStringLiteral("general.lastSourceFolder");
inline const QString kDefaultOutputDir = QStringLiteral("storage.defaultOutputFolder");
inline const QString kMaxConcurrentJobs = QStringLiteral("performance.maxConcurrentJobs");
inline const QString kWorkerThreads = QStringLiteral("performance.workerThreads");
inline const QString kPerformanceMode = QStringLiteral("performance.mode");
inline const QString kIoBufferMiB = QStringLiteral("performance.ioBufferMiB");
inline const QString kKeepAwake = QStringLiteral("performance.keepComputerAwake");
inline const QString kFreeSpaceMarginMiB = QStringLiteral("storage.freeSpaceMarginMiB");
inline const QString kTempFolder = QStringLiteral("storage.tempFolder");
inline const QString kLogLevel = QStringLiteral("logging.level");
inline const QString kLogKeepFiles = QStringLiteral("logging.keepFiles");
inline const QString kLogMaxMiB = QStringLiteral("logging.maxFileMiB");
inline const QString kKeepJobTemp = QStringLiteral("advanced.keepJobTempFolders");
inline const QString kVerifyAfterBuild = QStringLiteral("verification.verifyAfterBuild");
inline const QString kVerifyMode = QStringLiteral("verification.mode");
inline const QString kAfterQueueAction = QStringLiteral("general.afterQueueAction");
inline const QString kAfterQueueDelay = QStringLiteral("general.afterQueueDelaySeconds");
inline const QString kAfterQueueOnlyIfOk = QStringLiteral("general.afterQueueOnlyIfAllSucceeded");
inline const QString kAutoRetryCount = QStringLiteral("general.autoRetryCount");
inline const QString kAutoRetryDelay = QStringLiteral("general.autoRetryDelaySeconds");
inline const QString kLogFolder = QStringLiteral("logging.folder");
inline const QString kDeleteSource = QStringLiteral("storage.deleteSourceAfterVerifiedBuild");
} // namespace keys

// Effective worker-thread count for engines. performance.workerThreads 0 =
// automatic from the performance mode: low impact half the logical cores,
// balanced three quarters, maximum all of them.
int workerThreads(const SettingsValues& settings);
// performance.mode as the int applyProcessPriority() takes (0 low impact,
// 1 balanced, 2 maximum).
int performanceMode(const SettingsValues& settings);
// Read/write buffer size for streaming engines (performance.ioBufferMiB).
qint64 ioBufferBytes(const SettingsValues& settings);
// Free-space reserve every writer must keep (storage.freeSpaceMarginMiB);
// pass it as the margin to fw::ensureFreeSpace().
qint64 freeSpaceMarginBytes(const SettingsValues& settings);
// The user's temporary folder (storage.tempFolder) or empty for the default.
QString tempParentDir(const SettingsValues& settings);

// For build jobs, after the outcome is final: deletes the source folder when
// the user enabled storage.deleteSourceAfterVerifiedBuild AND the outcome is
// content-verified (verification Passed in "full" mode) AND no output lies
// inside the source (audit C1/C2). Returns true if the source was deleted;
// a refusal is added to outcome.warnings and logged, never an error.
bool deleteSourceAfterVerifiedBuild(const JobContext& ctx, const QString& sourceFolder, JobOutcome& outcome);

} // namespace fw::general
