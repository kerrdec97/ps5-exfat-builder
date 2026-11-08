#pragma once

// PS5 Manager (legacy exfat_builder.py _ps5mgr_refresh/_ps5mgr_render,
// ui/tab_ps5_mgr.py): the games on the console next to the local images, and
// a file-by-file compare of a console folder with a local dump or image.
//
// Game list: every configured console folder is listed (LIST/MLSD). Images
// (.exfat .ffpkg .ffpfs .ffpfsc, the legacy _LIBRARY_IMAGE_EXTS) and
// sub-folders (game dumps) are kept; SITE AVAIL gives the free space. Local
// images are the image files directly inside a local folder (non-recursive,
// like the legacy img_scan_dirs scan). Rows are merged by file name: On PS5,
// Size mismatch, Local only, PS5 only (legacy statuses).
//
// Compare: the console folder is walked recursively (RemoteTree.h) and each
// file is matched by its relative path (case-sensitive: the console's own
// storage is): missing on the PS5, only on the PS5, or a different size.
// Contents are not read (that would mean downloading the game).

#include "connect/ps5/FtpClient.h"
#include "connect/ps5/RemoteTree.h"
#include "core/Cancellation.h"
#include "core/Result.h"

#include <QDateTime>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>
#include <vector>

namespace fw::ps5 {

// ---- game list ---------------------------------------------------------------------------

const QStringList& consoleImageSuffixes(); // ".exfat", ".ffpkg", ".ffpfs", ".ffpfsc"
bool isConsoleImageName(const QString& name);

struct ConsoleGame {
    QString folder;    // the configured folder it was found in
    QString name;
    QString path;      // folder + name
    bool isDir = false; // a game dump folder (size unknown)
    qint64 size = -1;
    QDateTime modified;
};

struct ConsoleGameList {
    std::vector<ConsoleGame> games;
    QStringList folderErrors;           // "<folder>: <message>" for folders that could not be listed
    std::optional<qint64> freeBytes;    // ShadowMount+ API available_bytes, else SITE AVAIL
    QString freeText;                   // where it came from / the raw SITE AVAIL reply (empty if unknown)
    qint64 imageBytes = 0;              // sum of the image sizes listed
};

// Free space from a SITE AVAIL reply: the first number, with an optional unit
// (B, KB/KiB, MB/MiB, GB/GiB, TB/TiB; 1024-based) — the legacy app took the
// digits of the whole reply as bytes.
std::optional<qint64> parseSiteAvail(const QString& reply);

// Lists the folders (unusable names skipped). Fails only when no folder could
// be listed and the connection is gone, or on cancel.
// Free space: the ShadowMount+ 1.7 API (`smpApiPort` > 0) for the first
// folder's filesystem, else SITE AVAIL (best effort: no current console
// server implements it).
Result<ConsoleGameList> listConsoleGames(FtpClient& client, const QStringList& folders, const CancellationToken& token = {},
                                         quint16 smpApiPort = 0);

// "/a/; /b" -> {"/a/", "/b"} (';' or newline separated, empty parts dropped,
// each normalised with normalizeRemotePath(), duplicates removed).
QStringList splitConsoleFolders(const QString& text);

struct LocalImage {
    QString name;
    QString path;
    qint64 size = 0;
};
// Image files directly inside `folder` (legacy: not recursive).
std::vector<LocalImage> scanLocalImages(const QString& folder);

enum class GameStatus { OnPs5, SizeMismatch, LocalOnly, Ps5Only };
QString gameStatusLabel(GameStatus status); // "On PS5", "Size mismatch", "Local only", "PS5 only"

struct GameRow {
    QString name;
    GameStatus status = GameStatus::Ps5Only;
    bool isDir = false;
    qint64 localSize = -1;
    qint64 remoteSize = -1;
    QString localPath;
    QString remotePath;
};
// Merged by file name, sorted case-insensitively.
std::vector<GameRow> mergeGames(const std::vector<ConsoleGame>& remote, const std::vector<LocalImage>& local);

// ---- folder compare ----------------------------------------------------------------------

struct CompareFile {
    QString path;      // '/'-separated, relative
    qint64 size = -1;  // -1 = unknown
};

enum class DiffKind { MissingOnConsole, OnlyOnConsole, SizeDiffers };
QString diffKindLabel(DiffKind kind); // "Missing on the PS5", "Only on the PS5", "Different size"

struct FileDiff {
    QString path;
    DiffKind kind = DiffKind::MissingOnConsole;
    qint64 localSize = -1;
    qint64 remoteSize = -1;
};

struct CompareReport {
    QString remoteFolder;
    QString localSource;
    int localFiles = 0;
    int remoteFiles = 0;
    int matching = 0;     // same path and size
    int sizeUnknown = 0;  // on both sides, but the console did not report a size
    qint64 localBytes = 0;
    qint64 remoteBytes = 0;
    std::vector<FileDiff> diffs; // sorted by path
    QStringList warnings;        // unreadable folders, skipped names, links
    int count(DiffKind kind) const;
    bool identical() const { return diffs.empty() && sizeUnknown == 0 && warnings.isEmpty(); }
    QString summary() const;     // one line for the UI and the log
};

CompareReport compareFiles(const std::vector<CompareFile>& local, const std::vector<CompareFile>& remote);

// Every file under a local folder (hidden files included, links skipped).
Result<std::vector<CompareFile>> localFolderFiles(const QString& folder, const CancellationToken& token = {});

using CompareProgress = std::function<void(int folders, int files)>;
// Walks `remoteFolder` and compares it with `local` (from localFolderFiles()
// or an image's file list).
Result<CompareReport> compareConsoleFolder(FtpClient& client, const QString& remoteFolder,
                                           const std::vector<CompareFile>& local, const CancellationToken& token = {},
                                           const CompareProgress& progress = {});

} // namespace fw::ps5
