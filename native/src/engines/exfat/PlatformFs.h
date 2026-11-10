#pragma once

// The few file-system operations Qt does not cover portably: flushing a file
// to the device, setting folder timestamps and Windows file attributes, and
// opening source files that another program briefly holds locked.

#include "core/Cancellation.h"
#include "core/Result.h"

#include <QDateTime>
#include <QFile>

#include <memory>

namespace fw {
class PartFile;
}

namespace fw::exfat {

// Gives a finished .part file its final name. When that fails (the file it
// replaces is mounted or open in another program) the finished work is not
// thrown away: it is moved to a free name next to it, "<name> (new).<ext>",
// and the returned error says so. `what` starts that message, for example
// "The image was built and verified".
Status commitOrKeep(PartFile& part, bool replaceExisting, const QString& what);

// The same for a finished staging folder that could not be renamed to
// `output`: moves it to "<output>.new" (numbered when taken) and returns
// where the files are now (the staging folder itself when that failed too).
QString keepStagedFolder(const QString& stage, const QString& output);

// Flushes the OS cache of an open file to the device (FlushFileBuffers/fsync).
Status syncFile(QFile& file, const QString& displayName);

// Sets created (Windows only), modified and accessed times of a file or
// folder. Invalid times are left unchanged. Returns false on failure.
bool setPathTimes(const QString& path, const QDateTime& created, const QDateTime& modified,
                  const QDateTime& accessed);

// Applies read-only/hidden/system/archive bits (Windows). Elsewhere only
// read-only is applied, as the owner write permission of files.
bool applyAttributes(const QString& path, quint16 attributes, bool isDir);

// Opens `path` read-only. A failure is retried `retries` times, `waitMs`
// apart (files held by antivirus or Windows Search, like robocopy /R /W).
Result<std::unique_ptr<QFile>> openForRead(const QString& path, int retries, int waitMs,
                                           const CancellationToken& token);

// An I/O error for `file`, mapped to InsufficientSpace when the disk is full.
Error fileError(const QString& userMessage, const QFileDevice& file);

// True for the file systems that cannot hold a file of 4 GiB or more (FAT32
// and older), by the name diskSpace() reports.
bool limitsFilesTo4GiB(const QString& fileSystemName);
// True when the drive holding `folder` is such a file system and `bytes`
// is over its limit: checked before hours of writing fail at 4 GiB.
bool tooLargeForDrive(const QString& folder, qint64 bytes);

} // namespace fw::exfat
