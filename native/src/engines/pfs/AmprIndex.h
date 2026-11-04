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

#include "core/Cancellation.h"
#include "core/Result.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

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
// and to a folder on the console, so both give the same bytes.

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
    QStringList collisions;      // "kept <- dropped" for case-only differences
};

// True for the /app0 paths the tool never indexes (ASCII case-insensitive).
bool amprToolSkips(const QByteArray& indexedPath);
AmprToolRows amprToolRows(std::vector<AmprFile> files);

struct AmprLocalScan {
    std::vector<AmprFile> files;
    int folders = 0;
    bool hasAmprLibrary = false; // fakelib/libSceAmpr.sprx exists
};
// Every file under `folder` (hidden files included, symbolic links skipped).
Result<AmprLocalScan> scanAmprFolder(const QString& folder, const CancellationToken& token = {});
// fakelib/libSceAmpr.sprx among the relative paths (case-insensitive).
bool hasAmprLibrary(const std::vector<AmprFile>& files);

} // namespace fw::pfs
