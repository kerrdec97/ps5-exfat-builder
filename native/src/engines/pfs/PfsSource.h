#pragma once

// The set of files a folder build packs ("source plan"): a scan of the source
// folder with the user's exclusions applied, plus the files the build adds
// itself (empty-folder markers, ampr_emu.index). The same plan drives the
// build and the verification, so verification never reports a file the build
// deliberately left out (audit H2).
//
// File order and directory construction follow Lazy_MkPFS
// lazy_mkpfs/build.py scan_source_tree (GPL-3.0-or-later): files sorted by the
// lower-cased relative path; directories exist only as parents of files.

#include "core/ArchiveSource.h"
#include "core/Cancellation.h"
#include "core/Result.h"
#include "engines/pfs/AmprIndex.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <vector>

namespace fw::pfs {

struct SourceFile {
    QByteArray relPath;   // ASCII, '/' separators
    QString absPath;      // empty for generated files
    QByteArray content;   // generated files only
    qint64 size = 0;
    qint64 mtime = 0;     // whole seconds since the epoch
    bool generated = false;
    // A plan of an archived dump (planArchive): the file's item in
    // GameListing::items (absPath is empty), and once the build has streamed
    // it, the digest of its bytes (archive::ContentDigest), which verification
    // compares the image with in place of the file.
    int archiveItem = -1;
    bool hasDigest = false;
    quint64 digest = 0;

    QByteArray name() const;
};

struct PlanOptions {
    // File-name or relative-path patterns (`*`/`?` wildcards, case-insensitive);
    // a matching folder is skipped with everything inside it.
    QStringList excludes;
    // Adds a zero-byte ".pfskeep" to every folder that would otherwise be empty
    // (PFS keeps folders only as parents of files).
    bool keepEmptyFolders = true;
    AmprMode ampr = AmprMode::Missing;
    qint64 time = 0; // mtime recorded for generated files
};

struct SourcePlan {
    QString root;
    std::vector<SourceFile> files; // Lazy order
    qint64 totalBytes = 0;
    int excludedFiles = 0;
    int excludedFolders = 0;
    int emptyFolderMarkers = 0;
    bool amprGenerated = false; // the plan's ampr_emu.index was built here (added, or put in place of the dump's)
    AmprIndexAction amprAction = AmprIndexAction::Off; // what was decided about it, for the log
    QStringList excludedSamples; // first few excluded paths, for the log
    QStringList amprLeftOut; // amprGenerated: files its index leaves out (AmprIndexDecision::leftOut)
};

Result<SourcePlan> planFolder(const QString& root, const PlanOptions& options, const CancellationToken& token);

// The plan of an archived dump from the archive's list alone (GitHub #37):
// exactly the plan planFolder() makes of the game unpacked to a folder
// (unpackArchiveSource), with the same rules, order, markers and AMPR index
// decision; a file the archive lists no time for gets options.time (unpacking
// would give it the time it was unpacked). The dump's own ampr_emu.index is
// read out of the archive only when the decision needs it. `root` of the plan
// is the archive.
Result<SourcePlan> planArchive(const archive::GameListing& game, const PlanOptions& options, const CancellationToken& token);

// Lazy_MkPFS should_skip_executable_compression().
bool skipExecutableCompression(const QByteArray& name, const QByteArray& relPath);

// Normalised exclude patterns (trimmed, '/' separators, no leading "./").
QStringList normalizeExcludes(const QStringList& patterns);

} // namespace fw::pfs
