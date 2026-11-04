#pragma once

// Dump Rename (legacy ui/dump_rename.py + exfat_builder.py _dumps_*).
//
// plan:  every immediate sub-folder of the source folder whose game root is
//        found (itself, a child with eboot.bin, or a title-ID-looking name)
//        gets a proposed name from the chosen preset. Nothing on disk changes.
// apply: renames each selected item in place (or into a destination folder on
//        the same drive). Never overwrites: an existing target is skipped
//        ("Skipped — target exists"), as are two items proposing the same name.
//        Every completed rename is appended to an undo log right away.
// undo:  reverses a log, newest first, again never overwriting.

#include "core/Cancellation.h"
#include "core/Progress.h"
#include "core/Result.h"
#include "engines/gameinfo/Naming.h"

#include <QJsonObject>
#include <QString>

#include <vector>

namespace fw::gameinfo {

enum class RenameStatus {
    Ready,        // will be renamed
    AlreadyNamed, // proposed == current name
    NoTitleId,    // no proposal (legacy "No PPSA found")
    TargetExists, // something already has the proposed name
    Duplicate,    // another item in the plan proposes the same name
    InvalidName,  // an edited name Windows would not accept
    NotSelected,
    Renamed,
    Failed,
    Restored,     // undo put it back
};
QString renameStatusName(RenameStatus s);

struct RenameItem {
    QString sourcePath;   // the dump folder (child of the source folder)
    QString gameRoot;     // folder used for metadata
    QString currentName;
    QString proposedName; // may be edited before apply
    QString targetPath;
    QString title, titleId, version;
    QString confidence;   // legacy badge: "green" (ID + title), "yellow" (ID only), "red" (nothing)
    bool selected = true;
    RenameStatus status = RenameStatus::Ready;
    QString message;

    QJsonObject toJson() const;
};

struct RenamePlanOptions {
    NamePreset preset = NamePreset::TitleIdTitleVersion;
    NameOptions naming;
    QString destinationRoot; // empty = rename in place
};

Result<std::vector<RenameItem>> planDumpRenames(const QString& sourceFolder, const RenamePlanOptions& options,
                                                const CancellationToken& token);

// Re-checks names, collisions and targets after edits (cheap, no metadata).
void refreshRenamePlan(std::vector<RenameItem>& plan, const QString& destinationRoot);

struct RenameRunResult {
    std::vector<RenameItem> items;
    int renamed = 0;
    int skipped = 0;
    int failed = 0;
    QString undoLogPath; // empty when nothing was renamed
};

// `undoLogPath` empty = Paths::dataDir()/rename-undo/<timestamp>.json.
Result<RenameRunResult> applyDumpRenames(std::vector<RenameItem> plan, const QString& destinationRoot,
                                         const QString& undoLogPath, const CancellationToken& token,
                                         ProgressReporter* progress = nullptr);

Result<RenameRunResult> undoDumpRenames(const QString& undoLogPath, const CancellationToken& token,
                                        ProgressReporter* progress = nullptr);

QString defaultRenameUndoDir();

} // namespace fw::gameinfo
