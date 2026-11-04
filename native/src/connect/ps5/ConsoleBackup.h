#pragma once

// Local backups taken before the app overwrites anything on the console.
//
// The legacy app wrote best-effort backups next to the file on the PS5
// (".bak", "param_backup.json") or none at all. Every console-side write in
// this module first copies the current remote file to the PC; if the file
// exists but cannot be read, the write is refused.

#include "connect/ps5/FtpClient.h"
#include "core/Cancellation.h"
#include "core/Result.h"

#include <QString>

namespace fw::ps5 {

// Paths::dataDir()/ps5-backups/<yyyyMMdd-HHmmss-zzz>
QString newConsoleBackupDir();

// Copies `remotePath` to `backupDir/<remote path without the leading '/'>`
// (a numbered suffix is added if that name is taken). Returns the local path,
// or an empty string when the remote file does not exist (nothing to save).
Result<QString> backupRemoteFile(FtpClient& client, const QString& remotePath, const QString& backupDir,
                                 const CancellationToken& token = {});

// Saves bytes already downloaded from `remotePath` the same way.
Result<QString> saveRemoteCopy(const QByteArray& bytes, const QString& remotePath, const QString& backupDir);

} // namespace fw::ps5
