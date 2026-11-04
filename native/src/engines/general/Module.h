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
} // namespace keys

// Effective worker-thread count for engines (0 = all logical cores).
int workerThreads(const SettingsValues& settings);
// Read/write buffer size for streaming engines (performance.ioBufferMiB).
qint64 ioBufferBytes(const SettingsValues& settings);
// Free-space reserve every writer must keep (storage.freeSpaceMarginMiB);
// pass it as the margin to fw::ensureFreeSpace().
qint64 freeSpaceMarginBytes(const SettingsValues& settings);
// The user's temporary folder (storage.tempFolder) or empty for the default.
QString tempParentDir(const SettingsValues& settings);

} // namespace fw::general
