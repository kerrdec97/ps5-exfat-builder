#pragma once

// Editing an existing image: the legacy Edit > exFAT "File Manager" (add
// files or folders, new folder, replace, delete), without mounting anything.
//
// Safety model (audit M2): the original image is never lost before the edited
// image has verified.
//  * In place, only when that is provably safe: deleting entries, and
//    replacing a file with one that fits its existing contiguous clusters.
//    Every byte about to be overwritten is first saved to an undo journal
//    ("<image>.edit-journal", flushed to disk before the image is touched).
//    A failed or cancelled edit, or one whose verification fails, is rolled
//    back from the journal, leaving the image byte-identical. A journal left
//    by a crash is rolled back by the next edit (or recoverInterruptedEdit()).
//  * Everything else (adding files or folders, a replacement that needs more
//    clusters, "always rebuild"): a new image is written to "<image>.part",
//    verified against the expected tree, then swapped in (the old image is
//    set aside and restored if the swap fails). The image grows when the
//    edited contents no longer fit.

#include "core/Cancellation.h"
#include "core/Log.h"
#include "core/Progress.h"
#include "core/Result.h"
#include "engines/exfat/ExfatVerify.h"
#include "engines/exfat/ExfatWriter.h"
#include "engines/exfat/SourceScan.h"

#include <QJsonArray>

#include <functional>
#include <vector>

namespace fw::exfat {

struct EditOp {
    enum class Kind { Add, Replace, Delete, Mkdir };
    Kind kind = Kind::Add;
    QString imagePath; // path inside the image ('/' or '\' separators)
    QString localPath; // add / replace: the file or folder on disk
};

// Parses [{"op": "add|replace|delete|mkdir", "imagePath": ..., "localPath": ...}].
Result<std::vector<EditOp>> parseEditOps(const QJsonArray& ops);
QJsonArray editOpsToJson(const std::vector<EditOp>& ops);

enum class EditStrategy {
    Auto,    // in place when provably safe, otherwise rebuild
    Rebuild, // always write a new image
};

struct EditOptions {
    EditStrategy strategy = EditStrategy::Auto;
    ScanRules rules;     // applied to folders added from disk
    WriterOptions writer; // buffers, retries, and the sizing rule when the image must grow
    VerifyOptions verify;
    qint64 freeSpaceMarginBytes = 0;
    // Phase changes: 0 planning, 1 writing, 2 verifying (total, files, unit is bytes).
    std::function<void(int, qint64, qint64, bool)> onPhase;
    // Tests only: stop right after an in-place edit was applied, as if the
    // app had crashed (the journal stays behind).
    bool testCrashAfterApply = false;
};

struct EditResult {
    bool inPlace = false;
    qint64 added = 0;          // files added
    qint64 foldersAdded = 0;   // folders created (add of a folder, mkdir, parents)
    qint64 replaced = 0;
    qint64 deleted = 0;        // files and folders removed (top level of each delete)
    qint64 oldVolumeBytes = 0;
    qint64 newVolumeBytes = 0;
    bool recoveredJournal = false; // an earlier interrupted edit was rolled back first
    bool restored = false;         // verification failed; the original image was kept or put back
    VerifyReport verification;
    QStringList warnings;

    QString summary() const;
};

Result<EditResult> editImage(const QString& image, const std::vector<EditOp>& ops, const EditOptions& options,
                             const CancellationToken& token, ProgressReporter* progress = nullptr,
                             const Log* log = nullptr);

QString editJournalPath(const QString& image);

// Rolls back an in-place edit that was interrupted. Returns true when the
// image was restored from its journal, false when there was nothing to undo.
Result<bool> recoverInterruptedEdit(const QString& image, const Log* log = nullptr);

} // namespace fw::exfat
