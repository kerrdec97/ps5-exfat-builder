// AMPRIDX3 builder. Port of Lazy_MkPFS lazy_mkpfs/ampr_index.py
// (Nazky; format from the AMPR resolver's build_ampr_index.py). GPL-3.0-or-later.
// The stand-alone tool's row rules follow drakmor ampr_emu
// tools/build_ampr_index.py (current upstream, GPL-3.0).

#include "engines/pfs/AmprIndex.h"

#include "core/FileUtil.h"
#include "engines/pfs/PfsFormat.h"

#include <algorithm>
#include <map>
#include <set>

namespace fw::pfs {

namespace {
QByteArray keyFor(const QByteArray& path)
{
    QByteArray k = path;
    k.replace('\\', '/');
    return asciiLower(k);
}

quint64 slotCountFor(qint64 rows)
{
    if (rows <= 0)
        return 0;
    quint64 hashSlots = 2;
    const quint64 target = quint64(rows) * 2;
    while (hashSlots < target)
        hashSlots <<= 1;
    return hashSlots;
}
} // namespace

quint64 amprPathHash(const QByteArray& path)
{
    quint64 h = 1469598103934665603ULL;
    for (char ch : keyFor(path)) {
        h ^= quint64(quint8(ch));
        h *= 1099511628211ULL;
    }
    return h ? h : 1;
}

QByteArray buildAmprIndex(const std::vector<AmprRow>& input)
{
    // Keep the first of case-insensitively equal paths (walk order).
    std::vector<AmprRow> rows;
    std::set<QByteArray> seen;
    for (const AmprRow& r : input) {
        const QByteArray k = keyFor(r.path);
        if (!seen.insert(k).second)
            continue;
        rows.push_back(r);
    }
    if (rows.empty())
        return {};
    std::stable_sort(rows.begin(), rows.end(), [](const AmprRow& a, const AmprRow& b) {
        const QByteArray ka = keyFor(a.path), kb = keyFor(b.path);
        return std::lexicographical_compare(ka.begin(), ka.end(), kb.begin(), kb.end(),
                                            [](char x, char y) { return quint8(x) < quint8(y); });
    });

    QByteArray records;
    QByteArray blob;
    records.reserve(qsizetype(rows.size()) * 24);
    for (const AmprRow& r : rows) {
        char rec[24];
        putU32(rec + 0, quint32(blob.size()));
        putU32(rec + 4, quint32(r.path.size()));
        putU64(rec + 8, quint64(r.size));
        putI64(rec + 16, r.mtime);
        records.append(rec, 24);
        blob.append(r.path);
        blob.append('\0');
    }

    // Open-addressed hash slots, linear probing (Lazy marks only the earlier slot).
    const quint64 nslots = slotCountFor(qint64(rows.size()));
    struct Slot {
        quint64 hash = 0;
        quint32 indexPlusOne = 0;
        quint32 flags = 0;
    };
    std::vector<Slot> hashSlots(static_cast<size_t>(nslots));
    const quint64 mask = nslots - 1;
    for (size_t i = 0; i < rows.size(); ++i) {
        const quint64 h = amprPathHash(rows[i].path);
        quint64 pos = h & mask;
        while (hashSlots[size_t(pos)].indexPlusOne != 0) {
            if (hashSlots[size_t(pos)].hash == h)
                hashSlots[size_t(pos)].flags |= 1;
            pos = (pos + 1) & mask;
        }
        hashSlots[size_t(pos)] = {h, quint32(i + 1), 0};
    }

    const qint64 headerSize = 48;
    const qint64 pathEnd = headerSize + records.size() + blob.size();
    const qint64 hashOffset = (pathEnd + 15) & ~qint64(15);

    QByteArray out(qsizetype(hashOffset + qint64(nslots) * 16), '\0');
    char* o = out.data();
    std::memcpy(o, "AMPRIDX3", 8);
    putU32(o + 8, 3);
    putU32(o + 12, 24);
    putU64(o + 16, quint64(rows.size()));
    putU64(o + 24, quint64(blob.size()));
    putU64(o + 32, quint64(hashOffset));
    putU32(o + 40, 16);
    putU32(o + 44, quint32(nslots));
    std::memcpy(o + headerSize, records.constData(), size_t(records.size()));
    std::memcpy(o + headerSize + records.size(), blob.constData(), size_t(blob.size()));
    char* s = o + hashOffset;
    for (const Slot& slot : hashSlots) {
        putU64(s, slot.hash);
        putU32(s + 8, slot.indexPlusOne);
        putU32(s + 12, slot.flags);
        s += 16;
    }
    return out;
}

bool amprIndexesDifferOnlyInTimes(const QByteArray& a, const QByteArray& b)
{
    const qint64 headerSize = 48;
    if (a.size() != b.size() || a.size() < headerSize || std::memcmp(a.constData(), "AMPRIDX3", 8) != 0
        || std::memcmp(a.constData(), b.constData(), size_t(headerSize)) != 0)
        return false;
    const quint64 rows = getU64(a.constData() + 16);
    if (getU32(a.constData() + 12) != 24 || rows > quint64(a.size() - headerSize) / 24)
        return false;
    const qint64 recordsEnd = headerSize + qint64(rows) * 24;
    for (qint64 r = headerSize; r < recordsEnd; r += 24)
        if (std::memcmp(a.constData() + r, b.constData() + r, 16) != 0) // path offset, length, size
            return false;
    return std::memcmp(a.constData() + recordsEnd, b.constData() + recordsEnd, size_t(a.size() - recordsEnd)) == 0;
}

// ---- the stand-alone tool --------------------------------------------------------------

bool amprToolSkips(const QByteArray& indexedPath)
{
    static const std::set<QByteArray> own = {
        QByteArrayLiteral("/app0/ampr_emu.index"),     QByteArrayLiteral("/app0/ampr_emu.index.tmp"),
        QByteArrayLiteral("/app0/ampr_emu.index.bak"), QByteArrayLiteral("/app0/ampr_commands.bin"),
        QByteArrayLiteral("/app0/apr_emu.log"),
    };
    return own.count(keyFor(indexedPath)) > 0;
}

AmprToolRows amprToolRows(std::vector<AmprFile> files)
{
    // Byte order of the relative paths decides which of two case-only
    // variants is kept, whatever order the folder was walked in.
    std::stable_sort(files.begin(), files.end(), [](const AmprFile& a, const AmprFile& b) {
        return std::lexicographical_compare(a.relPath.begin(), a.relPath.end(), b.relPath.begin(), b.relPath.end(),
                                            [](char x, char y) { return quint8(x) < quint8(y); });
    });
    AmprToolRows out;
    std::map<QByteArray, QByteArray> seen; // key -> kept path
    for (const AmprFile& f : files) {
        const QByteArray indexed = QByteArrayLiteral("/app0/") + f.relPath;
        if (amprToolSkips(indexed)) {
            out.skippedOwn << QString::fromUtf8(f.relPath);
            continue;
        }
        if (indexed.contains('\t') || indexed.contains('\n') || indexed.contains('\r')) {
            out.skippedControl << QString::fromUtf8(f.relPath);
            continue;
        }
        const QByteArray key = keyFor(indexed);
        const auto it = seen.find(key);
        if (it != seen.end()) {
            out.collisions << QStringLiteral("%1 <- %2").arg(QString::fromUtf8(it->second), QString::fromUtf8(indexed));
            continue;
        }
        seen.emplace(key, indexed);
        out.rows.push_back({f.size, f.mtime, indexed});
    }
    return out;
}

bool hasAmprLibrary(const std::vector<AmprFile>& files)
{
    return std::any_of(files.begin(), files.end(),
                       [](const AmprFile& f) { return asciiLower(f.relPath) == "fakelib/libsceampr.sprx"; });
}

Result<AmprLocalScan> scanAmprFolder(const QString& folder, const CancellationToken& token)
{
    FW_TRY_ASSIGN(FileTree tree, scanTree(folder, token));
    AmprLocalScan out;
    out.folders = int(tree.dirCount) + 1;
    out.files.reserve(size_t(tree.fileCount));
    for (const FileEntry& e : tree.entries) {
        if (e.isDir)
            continue;
        out.files.push_back({e.relativePath.toUtf8(), e.size, e.modified.toSecsSinceEpoch()});
    }
    out.hasAmprLibrary = hasAmprLibrary(out.files);
    return out;
}

} // namespace fw::pfs
