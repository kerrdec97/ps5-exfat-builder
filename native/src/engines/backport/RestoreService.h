#pragma once

// Restore a game folder from its originals ZIP (docs/BACKPORT_PARITY.md
// §2.15, S20, S36, D6).
//
//  * The ZIP is checked against the manifest when there is one: the ZIP the
//    manifest recorded must still have the recorded SHA-256; another ZIP
//    must hold the same originals (member hashes) as the manifest records.
//  * Every entry name is checked first; an entry that would land outside
//    the game ("zip slip": absolute, drive letter, "..", ':' anywhere) makes
//    the whole restore refuse before anything is written.
//  * Files are written through "<target>.fwpart" and renamed over the target;
//    files that already hold the original bytes are left untouched.
//  * Files the backport added (manifest added_files, schema 2; for a schema-1
//    manifest the recorded fakelib/ post hashes not in the ZIP) are removed
//    when they still hold what the backport wrote.
//  * Any file that could not be restored makes the result a failure.

#include "core/Cancellation.h"
#include "core/Progress.h"
#include "core/Result.h"

#include <QString>
#include <QStringList>

#include <optional>

namespace fw::backport {

struct RestoreOptions {
    QString game;
    QString zip;          // empty: find it (manifest first, then the newest matching name)
    QString originalsDir; // where to look for the ZIP and the manifest
};

struct RestoreReport {
    QString zip;
    QString manifestPath;
    int restored = 0;
    int unchanged = 0;
    QStringList refused;       // zip-slip entries
    QStringList errors;        // files that could not be restored
    QStringList removedAdded;  // files the backport added, removed
    QStringList keptAdded;     // added files left because their content changed
    QStringList warnings;
    bool manifestUpdated = false;
    bool ok() const { return refused.isEmpty() && errors.isEmpty(); }
};

// The game-relative path for an entry, or nullopt when it would escape `root`.
std::optional<QString> safeMemberTarget(const QString& root, const QString& entryName);

// The originals ZIP for a game: the manifest's recorded ZIP when it exists,
// else the newest "* original files*.zip" in `originalsDir` whose name holds
// the game's title ID.
QString findOriginalsZip(const QString& game, const QString& originalsDir);

// Restores; an error only for problems found before anything was written
// (missing ZIP, hash mismatch, zip slip, cancellation before the first file).
// Partial restores come back as a report with errors (ok() == false).
Result<RestoreReport> restoreOriginals(const RestoreOptions& options, const CancellationToken& token,
                                       ProgressReporter* progress = nullptr);

} // namespace fw::backport
