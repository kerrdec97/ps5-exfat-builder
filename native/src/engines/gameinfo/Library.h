#pragma once

// Library scan (legacy Library > Source Dumps, exfat_builder.py _lib_scan).
//
// For every library folder, every immediate sub-folder is a candidate
// ("wrapper"); OS folders ($sysreset, found.000, lost+found, $windows.~bt,
// $windows.~ws, recovery, recycler, $recycle.bin, system volume information)
// and dot folders are skipped. A wrapper is a game when eboot.bin sits in it,
// a child or a grandchild. Metadata comes from readGameMetadata() on the game
// root; a missing title ID falls back to the game root's then the wrapper's
// folder name. Then size, cover and SDK are added and duplicates (same title
// ID) are collapsed: bigger dump, then shallower wrapper, then shallower
// eboot.bin wins. Games are sorted by title.

#include "core/Cancellation.h"
#include "core/Progress.h"
#include "core/Result.h"
#include "engines/gameinfo/GameInfo.h"

#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <vector>

namespace fw::gameinfo {

struct LibraryGame {
    QString folder;        // game root (holds eboot.bin)
    QString wrapper;       // immediate sub-folder of the library folder
    QString libraryFolder; // the scanned library folder
    QString title;         // title, else title ID, else wrapper name
    QString titleId;
    QString version;
    QString contentId;
    Platform platform = Platform::Unknown;
    ContentKind kind = ContentKind::Unknown;
    SystemVersion sdkVersion;
    SystemVersion requiredSystemVersion;
    qint64 sizeBytes = -1;  // -1 = not measured
    QString coverSource;    // icon0.png in the dump
    QString coverPath;      // normalised 260x390 PNG in the cache
    QDateTime modified;     // wrapper modification time
    QStringList notes;

    QJsonObject toJson() const;
    static LibraryGame fromJson(const QJsonObject& o);
};

struct LibraryScanOptions {
    bool measureSizes = true;
    bool covers = true;
    QString coverCacheDir; // empty = defaultCoverCacheDir()
};

struct LibraryScanResult {
    std::vector<LibraryGame> games;
    int foldersChecked = 0;      // wrappers looked at
    QStringList missingFolders;  // library folders that do not exist
    QStringList warnings;
};

Result<LibraryScanResult> scanLibrary(const QStringList& libraryFolders, const LibraryScanOptions& options,
                                      const CancellationToken& token, ProgressReporter* progress = nullptr);

// Legacy _lib_dedupe(): keeps the best entry per normalised title ID; games
// without an ID are all kept; result sorted by title (case-insensitive).
std::vector<LibraryGame> dedupeLibrary(std::vector<LibraryGame> games);

// True for the OS folder names the scan skips.
bool isSkippedLibraryEntry(const QString& name);

} // namespace fw::gameinfo
