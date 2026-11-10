#pragma once

// Game dump detection and title metadata.
//
// readGameMetadata() follows the legacy get_game_info() chain exactly:
//   1. param.sfo (root, sce_sys, then a walk <= 5 levels): TITLE or TITLE_00,
//      TITLE_ID (ignored when it is a number), VERSION or APP_VER;
//   2. pfs-version.dat: a first line starting with a digit or '.' overrides
//      the version (the real patch version);
//   3. param.json, only when the SFO gave neither title nor ID: titleId,
//      version/masterVersion, title (see ParamJson);
//   4. nptitle.dat, title only, first line, when there is still no title;
//   5. version normalised to XX.XXX.XXX, title sanitised.
// On top of the legacy result it reports the platform, the content kind, SDK
// and required firmware, the content ID and the parsed files.

#include "core/Cancellation.h"
#include "core/Result.h"
#include "engines/gameinfo/ParamJson.h"
#include "engines/gameinfo/Sfo.h"
#include "engines/gameinfo/Version.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <optional>

namespace fw::gameinfo {

enum class Platform { Unknown, PS4, PS5 };
enum class ContentKind { Unknown, Application, Patch, AddOn };

QString platformName(Platform p);       // "PS4", "PS5", "Unknown"
QString contentKindName(ContentKind k); // "Game", "Patch", "Add-on", "Unknown"

struct GameMetadata {
    QString folder;          // the folder that was read
    bool hasEboot = false;   // eboot.bin directly in `folder`

    // Legacy get_game_info() results (empty when unknown).
    QString title;           // sanitised
    QString titleId;
    QString version;         // normalised XX.XXX.XXX
    QString titleSource;     // "param.sfo", "param.json", "nptitle.dat" or ""

    QString rawTitle;        // before sanitising
    QString contentId;       // param.json contentId or SFO CONTENT_ID
    QString contentVersion;  // param.json contentVersion
    Platform platform = Platform::Unknown;
    ContentKind kind = ContentKind::Unknown;
    QString kindReason;      // why `kind` was chosen
    SystemVersion sdkVersion;           // build SDK (legacy Library "SDK N.xx")
    SystemVersion requiredSystemVersion; // firmware needed to run

    QString paramSfoPath, paramJsonPath, pfsVersionPath, npTitlePath;
    std::optional<SfoFile> paramSfo;
    std::optional<ParamJson> paramJson;
    QStringList notes;       // non-fatal problems (damaged files, repairs)

    QJsonObject toJson() const; // summary for job details / the UI (raw JSON included)
};

// Never fails for an existing folder: missing or damaged files leave fields
// empty and add a note. The only other error is Cancelled, when `token` is
// cancelled while the folder is searched for its metadata files.
Result<GameMetadata> readGameMetadata(const QString& folder, const CancellationToken& token = {});

// Legacy find_meta_file(): `folder/<name>`, `folder/sce_sys/<name>`, then a
// walk up to 5 levels deep (case-insensitive names). Empty when not found.
QString findMetaFile(const QString& folder, const QStringList& names);

// ---- dump roots -------------------------------------------------------------------
bool hasEbootBin(const QString& folder);

struct RootSearch {
    QString root;           // set when exactly one game root was found
    QStringList candidates; // every folder holding eboot.bin (not descended into)
    int scanned = 0;
    bool truncated = false;   // stopped at maxDirs
    bool depthCapped = false; // some folder was not looked into (maxDepth)
    bool cancelled = false;
};

// Legacy resolve_game_root_deep(): breadth-first search for folders holding
// eboot.bin at or below `folder`, skipping dot folders and sce_sys,
// sce_module, fakelib, image, $recycle.bin, system volume information,
// __macosx. `folder` itself wins if it holds eboot.bin.
RootSearch findGameRoots(const QString& folder, const CancellationToken& token = {}, int maxDepth = 5,
                         int maxDirs = 4000);

// Library discovery rule: eboot.bin in `wrapper`, else in a child, else in a
// grandchild (children visited in name order). Empty when none.
QString findLibraryGameRoot(const QString& wrapper);

// Dump Rename rule (_dumps_find_eboot_root): `path` itself, else the first
// child with eboot.bin, else `path` when its name looks like a title ID.
QString findRenameGameRoot(const QString& path);

// Total size of the files under `folder` (symlinks and junctions are not
// followed, unreadable entries are skipped) — legacy _get_folder_size().
// Returns -1 when cancelled.
qint64 dumpSize(const QString& folder, const CancellationToken& token = {});

// Legacy _load_cover_art(): sce_sys/icon0.png, icon0.png, then the first
// icon0.png up to 4 levels deep, skipping "decrypted" and "fakelib".
QString findCoverSource(const QString& folder);

} // namespace fw::gameinfo
