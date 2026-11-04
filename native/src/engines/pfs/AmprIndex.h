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

#include <QByteArray>

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

} // namespace fw::pfs
