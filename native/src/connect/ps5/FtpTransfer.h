#pragma once

// File and folder transfers to and from the console over N parallel FTP
// connections (the legacy app uploaded one file at a time).
//
// Layout (legacy): a file goes to <remoteDir>/<name>; a folder goes to
// <remoteDir>/<folder name>/<relative path>. Downloads mirror that locally.
//
// Safety:
//  * every remote folder is created first, on one connection;
//  * a cancelled or finally failed upload deletes its half-written remote file
//    over a fresh connection, like the legacy transfer queue ("Removed the
//    partial remote file ..."); a file that could not be removed is reported;
//  * downloads write <name>.part and rename it only when complete; existing
//    local files are skipped unless overwrite is set;
//  * a dropped connection is retried `retries` times, resuming with REST (or
//    APPE) from what already arrived — but only when this run's STOR for that
//    file was accepted; otherwise the file is rewritten from the start, so an
//    older remote file is never extended; `resume` also continues partial
//    files left by an earlier run (and skips remote files that already have
//    the full size);
//  * every transferred file's size is checked afterwards (SIZE / local size).

#include "connect/ps5/Endpoint.h"
#include "connect/ps5/FtpClient.h"
#include "core/Cancellation.h"
#include "core/Log.h"
#include "core/Progress.h"
#include "core/Result.h"

#include <QString>
#include <QStringList>

#include <vector>

namespace fw::ps5 {

struct TransferFile {
    QString local;
    QString remote;
    qint64 size = -1; // -1 = unknown (download whose SIZE failed)
};

struct TransferPlan {
    std::vector<TransferFile> files;
    QStringList directories; // remote (upload) or local (download) folders to create
    qint64 totalBytes = 0;
    // Entries left out of the plan ("path: reason"). Download: console entries
    // whose names are unsafe on this PC (containing '/', '\\' or ':', see
    // localNameProblem) or collide with another entry here. Both directions: a
    // source that would be written to the same target as an earlier one. They
    // are reported, never written.
    QStringList skipped;
};

struct TransferOptions {
    ConsoleEndpoint endpoint;
    int parallel = 1;          // connections; clamped to 1..8 and to the file count
    bool resume = false;
    int retries = 2;           // per file, after a lost connection or a timeout
    bool overwriteLocal = false;
};

struct TransferReport {
    int files = 0;
    int transferred = 0;
    int skipped = 0;          // already complete (resume) or existing local file
    int failed = 0;
    int verified = 0;         // size confirmed after the transfer
    int unverified = 0;       // the server could not report a size
    qint64 bytes = 0;         // bytes moved in this run
    bool cancelled = false;
    QStringList outputs;      // remote (upload) or local (download) paths written
    QStringList errors;       // one line per failed file
    QStringList warnings;
    QStringList removedPartials;
    QStringList leftoverPartials; // partial remote files that could not be removed
};

FtpOptions ftpOptionsFor(const ConsoleEndpoint& endpoint);

// Builds the upload plan (local files and folders, symlinks skipped).
Result<TransferPlan> planUpload(const QStringList& localSources, const QString& remoteDir,
                                const CancellationToken& token = {});
// Builds the download plan by listing remote folders recursively over `client`.
Result<TransferPlan> planDownload(FtpClient& client, const QStringList& remoteSources, const QString& localDir,
                                  const CancellationToken& token = {});

// Runs the plan. `progress` (optional) is advanced in bytes in its current
// phase; the caller begins the phase. Per-file failures are in the report.
// runUpload() fails as a whole only when nothing reached the console (no
// connection, a folder that cannot be created); runDownload() only when a
// local folder cannot be created.
Result<TransferReport> runUpload(const TransferPlan& plan, const TransferOptions& options,
                                 const CancellationToken& token, ProgressReporter* progress = nullptr,
                                 const Log& log = Log(QStringLiteral("ps5.ftp")));
Result<TransferReport> runDownload(const TransferPlan& plan, const TransferOptions& options,
                                   const CancellationToken& token, ProgressReporter* progress = nullptr,
                                   const Log& log = Log(QStringLiteral("ps5.ftp")));

// Renames a finished `<name>.part` over `finalPath`. An existing final file is
// set aside first and put back if the rename fails; on any failure the .part
// is kept and the error says where.
Status commitDownloadedFile(const QString& part, const QString& finalPath);

} // namespace fw::ps5
