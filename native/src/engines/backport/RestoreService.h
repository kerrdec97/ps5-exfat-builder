#pragma once

// Restore a game folder from its originals ZIP (docs/BACKPORT_PARITY.md
// §2.15, S20, S36, D6).
//
//  * Backport records are kept per game folder (BackportManifest.h). Without
//    a chosen ZIP, only this folder's own record is used: its ZIP is restored
//    (another dump of the same title ID never matches). A chosen ZIP is
//    matched to the record that names it (path or SHA-256): this folder's
//    record, or another folder's (then nothing from that record is changed).
//    A chosen ZIP no record names is refused unless `allowUnrecorded`.
//  * The ZIP the record names must still have the recorded SHA-256, and every
//    member must be the recorded original.
//  * Every entry name is checked first; an entry that would land outside the
//    game ("zip slip": absolute, drive letter, "..", ':' anywhere) makes the
//    whole restore refuse before anything is written.
//  * A file is only overwritten when it holds what the record says the
//    backport wrote there; a file that already holds the original is left
//    untouched; any other content is left as it is and reported (skipped),
//    which fails the restore. A missing file is recreated only from this
//    folder's own record.
//  * Files the backport added (manifest added_files, schema 2; for a schema-1
//    manifest the recorded fakelib/ post hashes not in the ZIP) are removed
//    when they still hold what the backport wrote, and only with this folder's
//    own record, which is then updated. Other folders' records are never written.

#include "core/Cancellation.h"
#include "core/Progress.h"
#include "core/Result.h"

#include <QString>
#include <QStringList>

#include <optional>

namespace fw::backport {

struct RestoreOptions {
    QString game;
    QString zip;          // empty: this folder's recorded originals ZIP
    QString originalsDir; // where the records (BackportBackup/) and ZIPs are
    // A chosen ZIP that no backport record names is restored without hash
    // checks only when this is set (the user confirmed it belongs to the game).
    bool allowUnrecorded = false;
};

struct RestoreReport {
    QString zip;
    QString manifestPath;
    bool ownRecord = false;    // the record used belongs to this game folder
    int restored = 0;
    int unchanged = 0;
    QStringList refused;       // zip-slip entries
    QStringList errors;        // files that could not be restored
    QStringList skipped;       // files holding content no record explains, left as they are
    QStringList removedAdded;  // files the backport added, removed
    QStringList keptAdded;     // added files left because their content changed
    QStringList warnings;
    bool manifestUpdated = false;
    bool ok() const { return refused.isEmpty() && errors.isEmpty() && skipped.isEmpty(); }
};

// The game-relative path for an entry, or nullopt when it would escape `root`.
std::optional<QString> safeMemberTarget(const QString& root, const QString& entryName);

// The originals ZIP recorded for this game folder in `originalsDir`, or
// empty. Never guessed from file names: another dump of the same title ID
// would match.
QString findOriginalsZip(const QString& game, const QString& originalsDir);

// Restores; an error for problems found before anything was written
// (missing ZIP or record, hash mismatch, cancellation before the first file).
// Zip-slip refusals and partial restores come back as a report with
// ok() == false.
Result<RestoreReport> restoreOriginals(const RestoreOptions& options, const CancellationToken& token,
                                       ProgressReporter* progress = nullptr);

} // namespace fw::backport
