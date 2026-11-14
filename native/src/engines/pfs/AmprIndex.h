#pragma once

// ampr_emu.index (AMPRIDX3) — the case-insensitive /app0 file index used by
// the AMPR APR file resolver (fakelib/libSceAmpr.sprx).
//
// Port of Lazy_MkPFS lazy_mkpfs/ampr_index.py (build_index_local,
// build_hash_slots, fnv1a64_path_hash). GPL-3.0-or-later.
//
// Layout (little-endian):
//   header  "<8sIIQQQII": "AMPRIDX3", version 3, record size 24, row count,
//           path blob length, hash table offset, slot size 16, slot count
//   records "<IIQq" per row: path offset, path length, size, mtime (seconds)
//   path blob: "/app0/<rel>" NUL-terminated, rows sorted by lower-cased path
//   padding to a 16-byte boundary
//   hash slots "<QII": FNV-1a 64 of the lower-cased path (0 → 1), row+1, flags
//           (open addressing, linear probing, 2^n >= 2 * rows slots; bit 0
//           marks a slot whose 64-bit hash is shared by a later row)
//
// What the bundled runtime (ampr-emu-ng 0.3.3, read from its loader) does
// with it. It checks the header (magic, version, record and slot sizes, 1 to
// 2,000,000 rows, a power-of-two slot count >= rows, a path blob of at most
// 256 MiB, the hash table at an 8-byte boundary after the blob and inside the
// file), every row (path inside the blob and NUL-terminated, "/app0/" then
// components that are not empty, "." or "..", without a backslash, at most
// 255 bytes each and 1023 in all) and every slot (an empty one all zero, flags
// 0 or 1, the hash of its row's path, each row found at its own slot); one
// failed check sets the whole index aside. A file's ID for the game is its
// row number, its size is what the resolve-with-sizes calls and fstat
// report, and its time is st_atime, st_mtime and st_ctime in the fstat of a
// file opened through the emulator. The case-insensitive lookup compares the
// full path and ignores the flags. Never read: the padding before the hash
// table and anything after the table.

#include "core/Cancellation.h"
#include "core/Result.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <functional>
#include <vector>

namespace fw::pfs {

struct AmprRow {
    qint64 size = 0;
    qint64 mtime = 0;    // whole seconds (Python int(st_mtime))
    QByteArray path;     // "/app0/..." (UTF-8)
};

quint64 amprPathHash(const QByteArray& path);

// Builds the index for `rows` given in walk order: rows whose lower-cased path
// repeats an earlier one are dropped (first kept), the rest are sorted by the
// lower-cased path. Returns an empty array when there are no rows (Lazy then
// writes no index).
QByteArray buildAmprIndex(const std::vector<AmprRow>& rows);

// True when two indexes list the same paths with the same sizes and differ
// at most in the files' modification times. For verifying an image against a
// folder whose files were copied or touched since the build; a build keeps
// only an identical index (decideAmprIndex).
bool amprIndexesDifferOnlyInTimes(const QByteArray& a, const QByteArray& b);

// ---- the stand-alone AMPR index tool (local folder or console folder) -------------------
//
// Rules of drakmor ampr_emu tools/build_ampr_index.py (current upstream,
// GPL-3.0; the legacy ui/build_ampr_index.py is an older revision): the files
// below /app0 are indexed except the index itself and its temporaries
// (ampr_emu.index, .tmp, .bak) and the emulator's own runtime files at the top
// (ampr_commands.bin, apr_emu.log); paths containing a tab, CR or LF are
// skipped with a warning; of paths that differ only in ASCII case the first
// (in byte order of the relative path) is kept with a warning — the legacy
// tool ran with collisions allowed. The same rules apply to a local folder
// and to a folder on the console, so both give the same bytes. Paths the
// bundled runtime refuses (amprRuntimeAccepts) are left out with a warning
// too: one such row would make it set the whole index aside.

inline const QString kAmprIndexName = QStringLiteral("ampr_emu.index");

struct AmprFile {
    QByteArray relPath; // UTF-8, '/'-separated, relative to app0
    qint64 size = 0;
    qint64 mtime = 0;   // whole seconds since the epoch (UTC)
};

struct AmprToolRows {
    std::vector<AmprRow> rows;
    QStringList skippedOwn;      // the index, its temporaries, the emulator's files
    QStringList skippedControl;  // paths with a tab, CR or LF
    QStringList skippedRuntime;  // paths the runtime cannot list (amprRuntimeAccepts)
    QStringList collisions;      // "kept <- dropped" for case-only differences
};

// True for the /app0 paths the tool never indexes (ASCII case-insensitive).
bool amprToolSkips(const QByteArray& indexedPath);
// True when the bundled runtime accepts `indexedPath` ("/app0/...") as a row:
// components that are not empty, "." or "..", of at most 255 bytes each, no
// backslash or NUL, at most 1023 bytes in all (the loader's checks above).
bool amprRuntimeAccepts(const QByteArray& indexedPath);
AmprToolRows amprToolRows(std::vector<AmprFile> files);
// The job-log warning for the files a build's index leaves out because the
// runtime cannot list them (AmprToolRows::skippedRuntime). Empty when none.
QString amprLeftOutLogLine(const QStringList& leftOut);

struct AmprLocalScan {
    std::vector<AmprFile> files;
    int folders = 0;
    bool hasAmprLibrary = false; // fakelib/libSceAmpr.sprx exists
};
// Every file under `folder` (hidden files included, symbolic links skipped).
Result<AmprLocalScan> scanAmprFolder(const QString& folder, const CancellationToken& token = {});
// fakelib/libSceAmpr.sprx among the relative paths (case-insensitive).
bool hasAmprLibrary(const std::vector<AmprFile>& files);

// ---- the index a folder build puts into its image ----------------------------------------
//
// pfs.amprIndex, exfat.amprIndex and ufs2.amprIndex: what a build does about
// ampr_emu.index when the game has fakelib/libSceAmpr.sprx. The dump folder
// is never written; a new or rebuilt index exists in the image only.

enum class AmprMode {
    Missing, // add one when the dump has none
    Refresh, // always rebuild it
    Off,     // never add or change it
    Auto,    // add one when the dump has none, rebuild one that no longer matches the game's files
};
// "auto", "missing", "refresh", "off"; anything else gives `fallback`.
AmprMode amprModeFromSetting(const QString& value, AmprMode fallback);

enum class AmprIndexAction {
    Off,           // the setting is off: nothing looked at
    NotNeeded,     // no fakelib/libSceAmpr.sprx
    Added,         // the dump has none
    Rebuilt,       // Auto: the dump's index did not match the game's files
    RebuiltAlways, // Refresh
    KeptUpToDate,  // Auto: the dump's index matches
    KeptExisting,  // Missing: the dump has one
    KeptForPacks,  // Auto: the game has asset packs, which need the index they were made with
};
// Added and the two Rebuilt: the image gets AmprIndexDecision::index.
bool amprActionChangesImage(AmprIndexAction action);
// The job-log line, e.g. "ampr_emu.index: added (the dump has none)". Empty for Off.
QString amprActionLogLine(AmprIndexAction action);

// What the scan of the game found.
struct AmprIndexState {
    bool hasLibrary = false;    // fakelib/libSceAmpr.sprx is among the image's files
    bool hasIndex = false;      // ampr_emu.index is at the top of the dump
    qint64 indexSize = 0;       // its size
    bool hasAssetPacks = false; // an asset-pack index is at the top (isAmprPackIndexName)
};

struct AmprIndexDecision {
    AmprIndexAction action = AmprIndexAction::Off;
    QByteArray index; // amprActionChangesImage(action): the bytes of the image's ampr_emu.index
    QStringList leftOut; // the same: files it does not list because the runtime cannot (amprLeftOutLogLine)
};

// `fresh` builds the index for the files the image will hold; `existing`
// reads the dump's own index (asked for only when it could match: Auto, same
// size as the fresh one). The dump's index is up to date only when it is byte
// for byte the fresh one: other file times are a difference the game sees
// (fstat), and the builder writes the same bytes for the same files. An
// index that differs only in bytes the runtime never reads (padding, a tail)
// is rebuilt as well; the image then gets an equivalent one.
AmprIndexDecision decideAmprIndex(AmprMode mode, const AmprIndexState& state, const std::function<QByteArray()>& fresh,
                                  const std::function<Result<QByteArray>()>& existing);

// The index of a game's asset packs at its top folder: ampr_zstd.index (zstd)
// or ampr_assets.index[.runtime] (LZ4). Such a game's ampr_emu.index lists the
// files inside the packs, which an index rebuilt from the folder would drop.
bool isAmprPackIndexName(const QString& fileName);

} // namespace fw::pfs
