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
    // Each row with its key, made once (a dump has 100,000 rows and more).
    std::vector<std::pair<QByteArray, const AmprRow*>> keyed;
    std::set<QByteArray> seen;
    for (const AmprRow& r : input) {
        QByteArray k = keyFor(r.path);
        if (!seen.insert(k).second)
            continue;
        keyed.emplace_back(std::move(k), &r);
    }
    if (keyed.empty())
        return {};
    std::stable_sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) {
        return std::lexicographical_compare(a.first.begin(), a.first.end(), b.first.begin(), b.first.end(),
                                            [](char x, char y) { return quint8(x) < quint8(y); });
    });
    std::vector<AmprRow> rows;
    rows.reserve(keyed.size());
    for (const auto& k : keyed)
        rows.push_back(*k.second);

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

bool amprRuntimeAccepts(const QByteArray& indexedPath)
{
    if (indexedPath.size() > 1023 || !indexedPath.startsWith("/app0/") || indexedPath.contains('\\') || indexedPath.contains('\0'))
        return false;
    for (const QByteArray& c : indexedPath.mid(6).split('/'))
        if (c.isEmpty() || c == "." || c == ".." || c.size() > 255)
            return false;
    return true;
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
        // One row the runtime refuses would make it set the whole index aside.
        if (!amprRuntimeAccepts(indexed)) {
            out.skippedRuntime << QString::fromUtf8(f.relPath);
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

// ---- the index a folder build puts into its image --------------------------------------

AmprMode amprModeFromSetting(const QString& value, AmprMode fallback)
{
    if (value == u"auto")
        return AmprMode::Auto;
    if (value == u"missing")
        return AmprMode::Missing;
    if (value == u"refresh")
        return AmprMode::Refresh;
    if (value == u"off")
        return AmprMode::Off;
    return fallback;
}

bool amprActionChangesImage(AmprIndexAction action)
{
    return action == AmprIndexAction::Added || action == AmprIndexAction::Rebuilt || action == AmprIndexAction::RebuiltAlways;
}

QString amprActionLogLine(AmprIndexAction action)
{
    switch (action) {
    case AmprIndexAction::Off: return {};
    case AmprIndexAction::NotNeeded: return QStringLiteral("ampr_emu.index: not needed (no fakelib/libSceAmpr.sprx)");
    case AmprIndexAction::Added: return QStringLiteral("ampr_emu.index: added (the dump has none)");
    case AmprIndexAction::Rebuilt: return QStringLiteral("ampr_emu.index: rebuilt (it did not match the game's files)");
    case AmprIndexAction::RebuiltAlways: return QStringLiteral("ampr_emu.index: rebuilt (set to always rebuild it)");
    case AmprIndexAction::KeptUpToDate: return QStringLiteral("ampr_emu.index: kept (up to date)");
    case AmprIndexAction::KeptExisting: return QStringLiteral("ampr_emu.index: kept (the dump has one)");
    case AmprIndexAction::KeptForPacks:
        return QStringLiteral("ampr_emu.index: kept (the game's asset packs need the index they were made with)");
    }
    return {};
}

QString amprLeftOutLogLine(const QStringList& leftOut)
{
    if (leftOut.isEmpty())
        return {};
    QString shown = leftOut.mid(0, 3).join(QStringLiteral("; "));
    if (leftOut.size() > 3)
        shown += QStringLiteral("; and %1 more").arg(leftOut.size() - 3);
    return QStringLiteral("ampr_emu.index: %1 file(s) left out (the AMPR runtime cannot list a name longer than 255 bytes, a path "
                          "longer than 1023 bytes or a backslash): %2")
        .arg(leftOut.size())
        .arg(shown);
}

AmprIndexDecision decideAmprIndex(AmprMode mode, const AmprIndexState& state, const std::function<QByteArray()>& fresh,
                                  const std::function<Result<QByteArray>()>& existing)
{
    AmprIndexDecision d;
    if (mode == AmprMode::Off)
        return d;
    if (!state.hasLibrary) {
        d.action = AmprIndexAction::NotNeeded;
        return d;
    }
    const AmprIndexAction kept = mode == AmprMode::Auto ? AmprIndexAction::KeptUpToDate : AmprIndexAction::KeptExisting;
    if (state.hasIndex && mode == AmprMode::Missing) {
        d.action = kept;
        return d;
    }
    if (mode == AmprMode::Auto && state.hasAssetPacks) {
        d.action = AmprIndexAction::KeptForPacks;
        return d;
    }
    QByteArray index = fresh();
    if (index.isEmpty()) { // nothing to list: the image stays as the dump is
        d.action = state.hasIndex ? kept : AmprIndexAction::NotNeeded;
        return d;
    }
    if (!state.hasIndex) {
        d.action = AmprIndexAction::Added;
    } else if (mode == AmprMode::Refresh) {
        d.action = AmprIndexAction::RebuiltAlways;
    } else {
        // Up to date means the bytes this build would write: the runtime reads
        // every field of every row, the times too (fstat of a file opened
        // through the emulator reports them). An index of another size cannot
        // match; one that cannot be read is replaced like one that does not.
        bool same = false;
        if (state.indexSize == index.size()) {
            const Result<QByteArray> old = existing();
            same = old.ok() && old.value() == index;
        }
        d.action = same ? AmprIndexAction::KeptUpToDate : AmprIndexAction::Rebuilt;
    }
    if (amprActionChangesImage(d.action))
        d.index = std::move(index);
    return d;
}

bool isAmprPackIndexName(const QString& fileName)
{
    for (const QLatin1String name : {QLatin1String("ampr_zstd.index"), QLatin1String("ampr_assets.index"),
                                     QLatin1String("ampr_assets.index.runtime")})
        if (fileName.compare(name, Qt::CaseInsensitive) == 0)
            return true;
    return false;
}

} // namespace fw::pfs
