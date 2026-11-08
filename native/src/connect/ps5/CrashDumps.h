#pragma once

// Crash-dump capture over FTP (docs/BACKPORT_PARITY.md matrix #46). Ported
// 2026-11-04 from the owner's PS5 Log Sender (log_sender/app/pslog/
// coredump.py, the Backport GUI's Console Logs tab).
//
// When a game crashes, the console writes a folder such as
// "PPSA34008_1792103554" in /user/devlog/system/sce_coredumps.0: report.log
// first and the .prosperodmp core shortly after. The folder is cleared when
// the crash-report screen closes, so it is fetched while it exists:
//  * the first successful listing is the baseline: folders already there are
//    old crashes and are left alone;
//  * the root is listed every 250 ms; every new folder is captured once;
//  * a capture lists the folder, downloads every new or grown file (the
//    .prosperodmp first) through "<name>.partial", up to 8 tries each, and is
//    complete once a core was saved and nothing changed for 3 s (at most
//    180 s per folder); a ".prosperodmp.tmp" is dropped when the core itself
//    was saved;
//  * each capture is zipped as PS5_logs_<ip>_<yyyyMMdd_HHmmss>_crash_<folder>.zip
//    (crash_dumps/<folder>/...) next to crash_dumps/ in the output folder.
// The console side needs an FTP server on the console (the Log Sender sends
// ftpsrv, port 2121, anonymous). The klog wake-up of the Log Sender is not
// used: the 250 ms poll finds the folder as soon as the console lists it.
//
// Blocking; run on a Qt-started thread (queue worker).

#include "connect/ps5/Endpoint.h"
#include "core/Cancellation.h"
#include "core/Result.h"

#include <QString>
#include <QStringList>

#include <functional>
#include <vector>

namespace fw::ps5 {

inline constexpr const char* kCrashDumpRoot = "/user/devlog/system/sce_coredumps.0";

struct CrashCaptureOptions {
    QString root = QString::fromLatin1(kCrashDumpRoot);
    QString outputDir;            // local; crash_dumps/<folder>/ and the ZIPs
    int pollMs = 250;
    int settleMs = 3000;
    int folderTimeoutMs = 180000;
    int retries = 8;
    qint64 runForMs = 0;          // 0: until cancelled
    QString ipTag;                // ZIP names; empty: from the endpoint host
};

struct CapturedCrash {
    QString folder;               // remote folder name
    QString localDir;
    QStringList files;            // saved file names
    bool hasCore = false;
    QString zipPath;              // empty when nothing was saved
    QString warning;
};

struct CrashCaptureReport {
    QStringList baseline;         // folders that were there before (left alone)
    std::vector<CapturedCrash> crashes;
    QStringList warnings;
    bool stoppedByUser = false;
};

// What the watcher is doing ("Watching for new crashes", "Saving crash dump
// PPSA..."), for the job's current-item line and the log.
using CrashCaptureStatus = std::function<void(const QString& text)>;

// "PPSA34008_1792103554" → a safe local name (Windows-reserved characters
// and control characters become '_', trailing dots and spaces removed).
QString safeCrashFolderName(const QString& name);

// "192.168.1.20" → "192-168-1-20" (the Log Sender's ip_tag; ':' → '_').
QString crashIpTag(const QString& host);

// Watches until cancelled (then returns what was captured, not an error) or
// until runForMs. Errors: InvalidArgument (no host or output folder) and
// local I/O failures; an unreachable console is retried every 2 s and
// reported as a warning.
Result<CrashCaptureReport> captureCrashDumps(const ConsoleEndpoint& endpoint, const CrashCaptureOptions& options,
                                             const CancellationToken& token, const CrashCaptureStatus& status = {});

} // namespace fw::ps5
