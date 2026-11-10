#pragma once

// Language Stripper: finds per-language data in a game dump (locale folders,
// language containers, Unreal .pak/.sig, PSARC, videos, audio banks) and
// removes the languages the user does not keep — after backing every removed
// item up into a verified ZIP, so the change can be undone.
//
// Detection rules follow the legacy app (exfat_builder.py _ls_walk_for_langs);
// the legacy table of language names was never defined there (its scan failed
// on the first match), so names come from languageName() below.

#include "core/Cancellation.h"
#include "core/Progress.h"
#include "core/Result.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <vector>

namespace fw::tools {

struct LanguageItem {
    QString relativePath; // inside the game root, '/' separators
    bool isFolder = false;
    QString code;         // "en", "ja", "zh"...
    QString language;     // "English", "Japanese (audio)"...
    qint64 bytes = 0;
    qint64 files = 0;
};

struct LanguageScan {
    QString gameRoot;
    std::vector<LanguageItem> items;
    qint64 totalBytes = 0;
    QStringList codes() const; // distinct, sorted
    QJsonObject toJson() const;
    static LanguageScan fromJson(const QJsonObject& o);
};

QString languageName(const QString& codeOrLocale);

// True if `folder` looks like a game root (has eboot.bin).
bool isGameRoot(const QString& folder);

// Game roots directly in `folder` or one level below (batch mode).
QStringList findGameRoots(const QString& folder);

Result<LanguageScan> scanLanguages(const QString& gameRoot, const CancellationToken& token);

struct StripPlan {
    QString gameRoot;
    QStringList remove;      // relative paths from a scan
    QString backupFolder;    // where the backup ZIP goes (required, not inside the game)
};

struct StripResult {
    QString backupZip;
    int removed = 0;
    qint64 bytesFreed = 0;
    QStringList failed;      // relative paths that could not be removed (completely)
    QStringList leftInPlace; // files kept because they changed, or appeared, after they were backed up
};

// Backs up every item to "<backupFolder>/<game>_languages_<time>.zip"
// (verified by reading it back), then deletes exactly the files that are in
// the verified backup and have not changed since they were read; a file that
// changed or appeared in the meantime stays where it is. Nothing is deleted if
// the backup fails. A manifest inside the ZIP records what was removed.
Result<StripResult> stripLanguages(const StripPlan& plan, const CancellationToken& token, ProgressReporter* progress);

struct RestoreResult {
    int restored = 0;
    QStringList skippedExisting; // relative paths that are in the game already; left as they are
    QString backupGame;          // the game folder name the backup was made from
    bool otherGame = false;      // `backupGame` is not the name of the folder restored into
};

// Puts everything from a strip backup back into `gameRoot` (never overwrites).
// The ZIP must carry a valid manifest and hold nothing but the items the
// manifest lists; anything else is refused before a file is written.
Result<RestoreResult> restoreLanguageBackup(const QString& backupZip, const QString& gameRoot,
                                            const CancellationToken& token, ProgressReporter* progress);
// The same, returning only the number of restored files.
Result<int> restoreLanguages(const QString& backupZip, const QString& gameRoot, const CancellationToken& token,
                             ProgressReporter* progress);

} // namespace fw::tools
