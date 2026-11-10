#pragma once

// The few file-system operations Qt does not cover portably: flushing a file
// to the device, setting folder timestamps and Windows file attributes, and
// opening source files that another program briefly holds locked.

#include "core/Cancellation.h"
#include "core/Result.h"

#include <QDateTime>
#include <QFile>

#include <memory>

namespace fw::exfat {

// What fw::commitOrKeep() does for a .part file, for a finished staging
// folder that could not be renamed to `output`: moves it to "<output>.new"
// (numbered when taken) and returns where the files are now (the staging
// folder itself when that failed too).
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

} // namespace fw::exfat
