#pragma once

// Recursive listing of a console folder over one FTP connection, for the AMPR
// index (legacy ui/build_ampr_index.py collect_ftp_rows) and the PS5 Manager's
// folder compare.
//
// Names from the console are turned into paths only through remoteChild():
// a name with '/', '\\', ':', a control character, "." or ".." is skipped and
// reported, never followed. Symbolic links are never followed. A sub-folder
// that cannot be listed is skipped and reported (legacy: "skipping unreadable
// FTP directory"); the root must be listable.

#include "connect/ps5/FtpClient.h"
#include "core/Cancellation.h"
#include "core/Result.h"

#include <QDateTime>
#include <QString>
#include <QStringList>

#include <functional>
#include <vector>

namespace fw::ps5 {

struct RemoteFile {
    QString relPath;     // '/'-separated, relative to the root
    qint64 size = -1;    // -1 = the server did not say
    QDateTime modified;  // UTC; invalid = unknown
    bool exactTime = false; // seconds from MLSD or MDTM (a LIST time is only minutes or days)
};

struct RemoteTree {
    QString root;                  // cleaned
    std::vector<RemoteFile> files; // walk order: per folder sorted case-insensitively, depth first
    QStringList dirs;              // relative folder paths
    QStringList skippedNames;      // relative paths of unusable names (not followed)
    QStringList unreadableDirs;    // relative folder paths that could not be listed
    int links = 0;                 // symbolic links seen (not followed)
    int folders = 0;               // folders listed, the root included
    qint64 totalBytes = 0;         // known sizes only
};

struct RemoteWalkOptions {
    // Ask SIZE for files the listing gave no size for (MLSD without "size").
    bool fillMissingSizes = false;
    // Ask MDTM for files whose listing has no time to the second (LIST shows
    // minutes or days). Needed when the times are written somewhere (AMPR
    // index); one extra command per file on servers without MLSD.
    bool exactTimes = false;
};

using RemoteWalkProgress = std::function<void(int folders, int files)>;

Result<RemoteTree> walkRemoteTree(FtpClient& client, const QString& root, const RemoteWalkOptions& options = {},
                                  const CancellationToken& token = {}, const RemoteWalkProgress& progress = {});

} // namespace fw::ps5
