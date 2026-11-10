#pragma once

// Last-resort crash reporting. Windows: an unhandled exception writes a
// minidump and a short text report into the log folder before the process
// ends (the logger is not touched there: the crash may have happened while a
// log sink held its lock). Everywhere: std::terminate is logged and flushed.
// The next start can offer to include the dump in a problem report.

#include <QString>
#include <QStringList>

namespace fw::crash {

void install(const QString& crashFolder);

// Crash dumps/reports left by earlier runs (newest first).
QStringList previousCrashFiles(const QString& crashFolder);

} // namespace fw::crash
