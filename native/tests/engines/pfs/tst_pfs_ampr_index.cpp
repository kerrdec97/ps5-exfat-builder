// ampr_emu.index (AMPRIDX3) as the runtime sees it, and when a build keeps the
// dump's own index (pfs::decideAmprIndex, the rule behind pfs.amprIndex,
// exfat.amprIndex and ufs2.amprIndex).
//
// runtimeLoad() is the loader of the bundled runtime (ampr-emu-ng 0.3.3,
// libSceAmpr.sprx) as read from the binary: the header, row and slot checks
// and the lookup the game's resolve calls go through. Two indexes the game
// cannot tell apart give the same RuntimeView.
//
// The kV402* bytes were written by version 4.0.2's ui/build_ampr_index.py
// (build_index_local) for folders with the files of kAsciiGame / kOtherGame,
// their sizes and times set as listed; v402Index() is that script in C++.

#include "PfsTestUtil.h"

#include "engines/pfs/AmprIndex.h"
#include "engines/pfs/PfsFormat.h"

#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <bit>
#include <functional>
#include <map>
#include <optional>

using namespace fw;
using namespace fw::pfs;

namespace {

struct File {
    const char* rel;
    qint64 size;
    qint64 mtime;
};

const std::vector<File> kAsciiGame = {
    {"eboot.bin", 16, 1700000000},          {"sce_sys/param.json", 5, 1700000001}, {"fakelib/libSceAmpr.sprx", 3, 1700000002},
    {"data/Big.BIN", 1000, 1700000003},     {"data/sub/a.txt", 0, 1700000004},
};
// kAsciiGame plus a file of the emulator's own and a name outside ASCII.
const std::vector<File> kOtherGame = {
    {"eboot.bin", 16, 1700000000},      {"sce_sys/param.json", 5, 1700000001}, {"fakelib/libSceAmpr.sprx", 3, 1700000002},
    {"data/Big.BIN", 1000, 1700000003}, {"data/sub/a.txt", 0, 1700000004},     {"apr_emu.log", 7, 1700000005},
    {"data/caf\xc3\xa9.bin", 2, 1700000006},
};

// CRC32 AA24EB66, 544 bytes.
const QByteArray kV402Ascii = QByteArray::fromHex(
    "414d505249445833030000001800000005000000000000006f00000000000000200100000000000010000000100000000000000012000000"
    "e80300000000000003f15365000000001300000014000000000000000000000004f1536500000000280000000f0000001000000000000000"
    "00f1536500000000380000001d000000030000000000000002f15365000000005600000018000000050000000000000001f1536500000000"
    "2f617070302f646174612f4269672e42494e002f617070302f646174612f7375622f612e747874002f617070302f65626f6f742e62696e00"
    "2f617070302f66616b656c69622f6c6962536365416d70722e73707278002f617070302f7363655f7379732f706172616d2e6a736f6e0000"
    "000000000000000000000000000000000000000000000000000000000000000000000000000000008297494fa90080a30400000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "00000000000000000000000000000000000000000000000000000000000000000000000000000000f997457c00cecd5d0500000000000000"
    "00000000000000000000000000000000000000000000000000000000000000000ccace4c763fcb3401000000000000005dc857a677ac7508"
    "0200000000000000aef1f48c231f64a8030000000000000000000000000000000000000000000000");
// CRC32 D06C6851, 624 bytes.
const QByteArray kV402Other = QByteArray::fromHex(
    "414d505249445833030000001800000007000000000000009600000000000000700100000000000010000000100000000000000011000000"
    "070000000000000005f15365000000001200000012000000e80300000000000003f153650000000025000000140000000200000000000000"
    "06f15365000000003a00000014000000000000000000000004f15365000000004f0000000f000000100000000000000000f1536500000000"
    "5f0000001d000000030000000000000002f15365000000007d00000018000000050000000000000001f15365000000002f617070302f6170"
    "725f656d752e6c6f67002f617070302f646174612f4269672e42494e002f617070302f646174612f636166c3a92e62696e002f617070302f"
    "646174612f7375622f612e747874002f617070302f65626f6f742e62696e002f617070302f66616b656c69622f6c6962536365416d70722e"
    "73707278002f617070302f7363655f7379732f706172616d2e6a736f6e000000000000000000000000000000000000009168114edf320590"
    "01000000000000008297494fa90080a306000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000f997457c00cecd5d07000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0ccace4c763fcb3402000000000000008d357cad3ee2c4f403000000000000005dc857a677ac75080400000000000000aef1f48c231f64a8"
    "0500000000000000");

std::vector<AmprFile> filesOf(const std::vector<File>& game)
{
    std::vector<AmprFile> out;
    for (const File& f : game)
        out.push_back({QByteArray(f.rel), f.size, f.mtime});
    return out;
}

QByteArray indexOf(const std::vector<AmprFile>& files) { return buildAmprIndex(amprToolRows(files).rows); }

// ---- the runtime's loader ----------------------------------------------------------------

char fold(char c)
{
    if (c >= 'A' && c <= 'Z')
        return char(c + 32);
    return c == '\\' ? '/' : c;
}

quint64 runtimeHash(const QByteArray& path)
{
    quint64 h = 0x14650FB0739D0383ULL;
    for (char c : path) {
        h ^= quint8(fold(c));
        h *= 0x100000001B3ULL;
    }
    return h ? h : 1;
}

// "/app0/" (any case), then components that are not empty, "." or ".." and
// hold no backslash or NUL.
bool runtimeValidPath(const QByteArray& p)
{
    if (p.size() < 7 || p[0] != '/' || p[5] != '/')
        return false;
    for (int i = 1; i <= 4; ++i)
        if (p[i] == '\\' || fold(p[i]) != "app0"[i - 1])
            return false;
    const QList<QByteArray> parts = p.mid(6).split('/');
    for (const QByteArray& c : parts)
        if (c.isEmpty() || c == "." || c == ".." || c.contains('\\') || c.contains('\0'))
            return false;
    return true;
}

struct RuntimeRow {
    QByteArray path;
    quint64 size = 0;
    qint64 mtime = 0;
    bool operator==(const RuntimeRow&) const = default;
};

struct RuntimeView {
    bool loaded = false;
    QString why; // the first check that failed
    bool sorted = false;
    std::vector<RuntimeRow> rows; // by file ID (row number) - 1
    bool operator==(const RuntimeView& o) const { return loaded == o.loaded && sorted == o.sorted && rows == o.rows; }
};

struct Slot {
    quint64 hash = 0;
    quint32 rowPlusOne = 0;
    quint32 flags = 0;
};

// The row the game's lookup finds for `path` (1-based), 0 for none.
quint32 runtimeLookup(const std::vector<Slot>& table, const std::vector<RuntimeRow>& rows, const QByteArray& path)
{
    if (!runtimeValidPath(path))
        return 0;
    const quint64 h = runtimeHash(path);
    const quint64 mask = table.size() - 1;
    quint64 pos = h & mask;
    for (size_t probe = 0; probe < table.size(); ++probe, pos = (pos + 1) & mask) {
        const Slot& s = table[size_t(pos)];
        if (s.rowPlusOne == 0)
            return 0;
        if (s.hash != h || s.rowPlusOne > rows.size())
            continue;
        const QByteArray& other = rows[s.rowPlusOne - 1].path;
        bool same = other.size() == path.size();
        for (qsizetype i = 0; same && i < path.size(); ++i)
            same = fold(other[i]) == fold(path[i]);
        if (same)
            return s.rowPlusOne;
    }
    return 0;
}

RuntimeView runtimeLoad(const QByteArray& f)
{
    RuntimeView v;
    const auto fail = [&v](const char* why) {
        v = {};
        v.why = QString::fromLatin1(why);
        return v;
    };
    if (f.size() < 48)
        return fail("shorter than the header");
    const char* d = f.constData();
    const quint64 rows = getU64(d + 16), blobLen = getU64(d + 24), hashOffset = getU64(d + 32);
    const quint32 slotCount = getU32(d + 44);
    if (std::memcmp(d, "AMPRIDX3", 8) != 0 || getU32(d + 8) != 3 || getU32(d + 12) != 24 || getU32(d + 40) != 16)
        return fail("header");
    if (rows - 1 > 1999999 || rows > slotCount || quint64(blobLen - 0x10000001) < 0xF0000000 || slotCount < 2
        || std::popcount(slotCount) != 1)
        return fail("header counts");
    const quint64 blobAt = 48 + rows * 24;
    if (hashOffset < blobAt + blobLen || hashOffset % 8 != 0 || hashOffset > quint64(f.size())
        || quint64(f.size()) - hashOffset < quint64(slotCount) * 16)
        return fail("hash table offset");
    const QByteArray blob = f.mid(qsizetype(blobAt), qsizetype(blobLen));
    for (quint64 i = 0; i < rows; ++i) {
        const char* r = d + 48 + i * 24;
        const quint64 off = getU32(r), len = getU32(r + 4);
        if (off + len >= blobLen || blob[qsizetype(off + len)] != '\0')
            return fail("path outside the blob");
        const QByteArray path = blob.mid(qsizetype(off), qsizetype(len));
        if (path.contains('\0') || !runtimeValidPath(path) || len > 1023)
            return fail("path");
        for (const QByteArray& c : path.split('/'))
            if (c.size() > 255)
                return fail("path component");
        v.rows.push_back({path, getU64(r + 8), getI64(r + 16)});
    }
    std::vector<Slot> table(slotCount);
    quint64 used = 0;
    for (quint32 s = 0; s < slotCount; ++s) {
        const char* p = d + hashOffset + quint64(s) * 16;
        table[s] = {getU64(p), getU32(p + 8), getU32(p + 12)};
        if (table[s].rowPlusOne == 0) {
            if (table[s].hash != 0 || table[s].flags != 0)
                return fail("empty slot not zero");
            continue;
        }
        if (table[s].rowPlusOne > rows || table[s].flags >= 2)
            return fail("slot");
        if (runtimeHash(v.rows[table[s].rowPlusOne - 1].path) != table[s].hash)
            return fail("slot hash");
        ++used;
    }
    if (used != rows)
        return fail("slot count");
    for (quint64 i = 0; i < rows; ++i)
        if (runtimeLookup(table, v.rows, v.rows[i].path) != i + 1)
            return fail("row not found at its slot");
    v.loaded = true;
    v.sorted = true;
    for (size_t i = 1; i < v.rows.size() && v.sorted; ++i) {
        QByteArray a = v.rows[i - 1].path, b = v.rows[i].path;
        std::transform(a.begin(), a.end(), a.begin(), fold);
        std::transform(b.begin(), b.end(), b.begin(), fold);
        v.sorted = !std::lexicographical_compare(b.begin(), b.end(), a.begin(), a.end(),
                                                 [](char x, char y) { return quint8(x) < quint8(y); });
    }
    return v;
}

// ---- version 4.0.2's script ---------------------------------------------------------------

// key_for(): Python's str.lower() of the path, compared and hashed by code point.
std::vector<char32_t> v402Key(const QByteArray& path)
{
    QByteArray p = path;
    p.replace('\\', '/');
    const QList<uint> ucs = QString::fromUtf8(p).toLower().toUcs4();
    return std::vector<char32_t>(ucs.begin(), ucs.end());
}

// build_index_local() + write_index(): every file but the index and its .tmp
// (no case-only duplicates in these games), sorted by key, hashed by code
// point, and a later row with the same 64-bit hash is flagged too.
QByteArray v402Index(const std::vector<File>& game)
{
    std::vector<std::pair<std::vector<char32_t>, File>> rows;
    for (const File& f : game) {
        const QByteArray rel = QByteArray(f.rel).toLower();
        if (rel != "ampr_emu.index" && rel != "ampr_emu.index.tmp")
            rows.emplace_back(v402Key(QByteArrayLiteral("/app0/") + f.rel), f);
    }
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    QByteArray records, blob;
    for (const auto& [key, f] : rows) {
        char rec[24];
        putU32(rec, quint32(blob.size()));
        const QByteArray path = QByteArrayLiteral("/app0/") + f.rel;
        putU32(rec + 4, quint32(path.size()));
        putU64(rec + 8, quint64(f.size));
        putI64(rec + 16, f.mtime);
        records.append(rec, 24);
        blob.append(path).append('\0');
    }
    quint32 nslots = 2;
    while (nslots < rows.size() * 2)
        nslots <<= 1;
    std::vector<Slot> table(nslots);
    for (size_t i = 0; i < rows.size(); ++i) {
        quint64 h = 1469598103934665603ULL;
        for (char32_t ch : rows[i].first) {
            h ^= quint64(ch);
            h *= 1099511628211ULL;
        }
        h = h ? h : 1;
        quint64 pos = h & (nslots - 1);
        bool duplicate = false;
        while (table[size_t(pos)].rowPlusOne != 0) {
            if (table[size_t(pos)].hash == h) {
                table[size_t(pos)].flags |= 1;
                duplicate = true;
            }
            pos = (pos + 1) & (nslots - 1);
        }
        table[size_t(pos)] = {h, quint32(i + 1), duplicate ? 1u : 0u};
    }
    const qint64 pathEnd = 48 + records.size() + blob.size();
    const qint64 hashOffset = (pathEnd + 15) & ~qint64(15);
    QByteArray out(qsizetype(hashOffset + nslots * 16), '\0');
    char* o = out.data();
    std::memcpy(o, "AMPRIDX3", 8);
    putU32(o + 8, 3);
    putU32(o + 12, 24);
    putU64(o + 16, rows.size());
    putU64(o + 24, quint64(blob.size()));
    putU64(o + 32, quint64(hashOffset));
    putU32(o + 40, 16);
    putU32(o + 44, nslots);
    std::memcpy(o + 48, records.constData(), size_t(records.size()));
    std::memcpy(o + 48 + records.size(), blob.constData(), size_t(blob.size()));
    for (quint32 s = 0; s < nslots; ++s) {
        putU64(o + hashOffset + s * 16, table[s].hash);
        putU32(o + hashOffset + s * 16 + 8, table[s].rowPlusOne);
        putU32(o + hashOffset + s * 16 + 12, table[s].flags);
    }
    return out;
}

// ---- the decision -------------------------------------------------------------------------

const AmprMode kModes[] = {AmprMode::Auto, AmprMode::Missing, AmprMode::Refresh, AmprMode::Off};

const char* modeName(AmprMode m)
{
    switch (m) {
    case AmprMode::Auto: return "auto";
    case AmprMode::Missing: return "missing";
    case AmprMode::Refresh: return "refresh";
    case AmprMode::Off: return "off";
    }
    return "?";
}

struct Decided {
    AmprIndexDecision d;
    int freshCalls = 0;
    int existingCalls = 0;
};

// `dump`: the dump's ampr_emu.index (none when empty), or an unreadable one.
Decided decide(AmprMode mode, const std::optional<QByteArray>& dump, const QByteArray& fresh, bool packs = false,
               bool library = true, bool unreadable = false)
{
    Decided out;
    AmprIndexState state;
    state.hasLibrary = library;
    state.hasIndex = dump.has_value();
    state.indexSize = dump ? dump->size() : 0;
    state.hasAssetPacks = packs;
    out.d = decideAmprIndex(
        mode, state,
        [&] {
            ++out.freshCalls;
            return fresh;
        },
        [&]() -> Result<QByteArray> {
            ++out.existingCalls;
            if (unreadable)
                return Error(ErrorCode::IoError, QStringLiteral("unreadable"));
            return *dump;
        });
    return out;
}

// With the dump holding `dump` and the game's files giving `fresh`, every
// mode does what it says; `upToDate`: Auto keeps the dump's index.
void checkEveryMode(const QByteArray& dump, const QByteArray& fresh, bool upToDate)
{
    for (const AmprMode mode : kModes) {
        const Decided r = decide(mode, dump, fresh);
        const char* what = modeName(mode);
        switch (mode) {
        case AmprMode::Auto:
            QVERIFY2(r.d.action == (upToDate ? AmprIndexAction::KeptUpToDate : AmprIndexAction::Rebuilt), what);
            QCOMPARE(r.d.index, upToDate ? QByteArray() : fresh);
            break;
        case AmprMode::Missing:
            QVERIFY2(r.d.action == AmprIndexAction::KeptExisting, what);
            QVERIFY(r.d.index.isEmpty());
            QCOMPARE(r.freshCalls + r.existingCalls, 0);
            break;
        case AmprMode::Refresh:
            QVERIFY2(r.d.action == AmprIndexAction::RebuiltAlways, what);
            QCOMPARE(r.d.index, fresh);
            QCOMPARE(r.existingCalls, 0);
            break;
        case AmprMode::Off:
            QVERIFY2(r.d.action == AmprIndexAction::Off, what);
            QCOMPARE(r.freshCalls + r.existingCalls, 0);
            break;
        }
    }
}

} // namespace

class TestPfsAmprIndex : public QObject {
    Q_OBJECT

private slots:
    void settingValues()
    {
        QCOMPARE(amprModeFromSetting(QStringLiteral("auto"), AmprMode::Off), AmprMode::Auto);
        QCOMPARE(amprModeFromSetting(QStringLiteral("missing"), AmprMode::Off), AmprMode::Missing);
        QCOMPARE(amprModeFromSetting(QStringLiteral("refresh"), AmprMode::Off), AmprMode::Refresh);
        QCOMPARE(amprModeFromSetting(QStringLiteral("off"), AmprMode::Auto), AmprMode::Off);
        QCOMPARE(amprModeFromSetting(QStringLiteral("sometimes"), AmprMode::Missing), AmprMode::Missing);
    }

    // Every index the builder writes passes the runtime's checks, every row is
    // found where it is, and the rows are what the game's files are.
    void runtimeAcceptsWhatTheBuilderWrites()
    {
        std::vector<std::vector<AmprFile>> games = {filesOf(kAsciiGame), filesOf(kOtherGame), {{"eboot.bin", 1, 2}}};
        // Case-only variants (the first in byte order stays), deep folders and a long name.
        games.push_back({{"b/Z.bin", 1, 1}, {"b/z.bin", 2, 2}, {"a.bin", 3, 3}, {"A.BIN", 4, 4},
                         {QByteArray("deep/") + QByteArray("x/").repeated(60) + "leaf.bin", 5, 5},
                         {QByteArray("long/") + QByteArray(255, 'n'), 6, 6}});
        std::vector<AmprFile> many;
        for (int i = 0; i < 5000; ++i)
            many.push_back({QStringLiteral("dir%1/Sub%2/file_%3.dat").arg(i % 37).arg(i % 5).arg(i).toUtf8(), i, 1700000000 + i});
        games.push_back(many);
        for (const auto& files : games) {
            const AmprToolRows rows = amprToolRows(files);
            const RuntimeView v = runtimeLoad(buildAmprIndex(rows.rows));
            QVERIFY2(v.loaded, qPrintable(v.why));
            QVERIFY(v.sorted);
            QCOMPARE(v.rows.size(), rows.rows.size());
            std::map<QByteArray, AmprRow> want;
            for (const AmprRow& r : rows.rows)
                want.emplace(r.path, r);
            for (const RuntimeRow& r : v.rows) {
                const auto it = want.find(r.path);
                QVERIFY2(it != want.end(), r.path.constData());
                QCOMPARE(qint64(r.size), it->second.size);
                QCOMPARE(r.mtime, it->second.mtime);
            }
        }
        // Damage the runtime notices sets the whole index aside.
        const QByteArray good = indexOf(filesOf(kAsciiGame));
        QByteArray bad = good;
        bad[48 + 24 + 4] = char(bad[48 + 24 + 4] + 1); // a path length
        QVERIFY(!runtimeLoad(bad).loaded);
        bad = good;
        putU64(bad.data() + 32, getU64(good.constData() + 32) - 16); // the hash table inside the blob
        QVERIFY(!runtimeLoad(bad).loaded);
        QVERIFY(!runtimeLoad(good.left(good.size() - 1)).loaded);
        QVERIFY(!runtimeLoad(QByteArray(good.size(), 'x')).loaded);
    }

    // A name the runtime refuses (a component over 255 bytes: 100 CJK letters
    // are a legal exFAT or NTFS name of 300 UTF-8 bytes; a path over 1023
    // bytes; a backslash, which UFS2 and PFS names may hold) is left out of
    // the index and named: one such row would make the runtime set the whole
    // index aside, and every other file of the game with it.
    void rowsTheRuntimeRefusesAreLeftOut()
    {
        const QByteArray cjk = QString(100, QChar(0x4E00)).toUtf8();
        QCOMPARE(cjk.size(), 300);
        QByteArray far;
        for (int i = 0; i < 5; ++i)
            far += QByteArray(210, char('a' + i)) + '/';
        far += "far.bin";
        std::vector<AmprFile> files = filesOf(kAsciiGame);
        files.push_back({"data/" + cjk, 1, 1700000100});
        files.push_back({far, 2, 1700000101});
        files.push_back({"data/back\\slash.bin", 3, 1700000102});
        files.push_back({"data/" + QString(85, QChar(0x4E00)).toUtf8(), 4, 1700000103}); // 255 bytes: allowed
        const AmprToolRows rows = amprToolRows(files);
        QCOMPARE(rows.skippedRuntime.size(), 3);
        QVERIFY(rows.skippedRuntime.contains(QString::fromUtf8("data/" + cjk)));
        QVERIFY(rows.skippedRuntime.contains(QString::fromUtf8(far)));
        QVERIFY(rows.skippedRuntime.contains(QStringLiteral("data/back\\slash.bin")));
        QCOMPARE(rows.rows.size(), kAsciiGame.size() + 1);
        const RuntimeView v = runtimeLoad(buildAmprIndex(rows.rows));
        QVERIFY2(v.loaded, qPrintable(v.why));
        QCOMPARE(v.rows.size(), rows.rows.size());

        // The checks themselves, as the runtime makes them.
        QVERIFY(amprRuntimeAccepts("/app0/eboot.bin"));
        QVERIFY(amprRuntimeAccepts("/app0/" + QByteArray(255, 'n')));
        QVERIFY(!amprRuntimeAccepts("/app0/" + QByteArray(256, 'n')));
        QVERIFY(amprRuntimeAccepts("/app0/" + QByteArray(1017, 'n').replace(200, 1, "/").replace(400, 1, "/").replace(600, 1, "/")
                                                  .replace(800, 1, "/")));
        QVERIFY(!amprRuntimeAccepts("/app0/" + QByteArray(1018, 'n').replace(200, 1, "/").replace(400, 1, "/").replace(600, 1, "/")
                                                   .replace(800, 1, "/")));
        for (const QByteArray bad : {QByteArray("/app0/a//b"), QByteArray("/app0/a/./b"), QByteArray("/app0/../b"), QByteArray("/app0/a\\b"),
                                     QByteArray("/app0/a/"), QByteArray("/app0/a\0b", 9)})
            QVERIFY2(!amprRuntimeAccepts(bad), bad.constData());
        for (const QByteArray& p : {QByteArray("/app0/data/") + cjk, QByteArray("/app0/") + far})
            QVERIFY(!runtimeLoad(buildAmprIndex({{1, 1, p}})).loaded); // what the loader makes of such a row

        QCOMPARE(amprLeftOutLogLine({}), QString());
        QVERIFY(amprLeftOutLogLine(rows.skippedRuntime).startsWith(QStringLiteral("ampr_emu.index: 3 file(s) left out")));
    }

    // For plain names version 4.0.2 wrote the bytes this version writes, so
    // its index of an unchanged game is up to date.
    void v402IndexOfPlainNamesIsUpToDate()
    {
        QCOMPARE(v402Index(kAsciiGame), kV402Ascii);
        QCOMPARE(indexOf(filesOf(kAsciiGame)), kV402Ascii);
        const RuntimeView v = runtimeLoad(kV402Ascii);
        QVERIFY2(v.loaded, qPrintable(v.why));
        checkEveryMode(kV402Ascii, indexOf(filesOf(kAsciiGame)), true);

        // The same from a folder on disk: the scan's sizes and times are the ones 4.0.2 read.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        for (const File& f : kAsciiGame)
            QVERIFY(pfstest::writeFile(dir.filePath(QString::fromUtf8(f.rel)), QByteArray(qsizetype(f.size), '\0'), f.mtime));
        auto scan = scanAmprFolder(dir.path());
        QVERIFY(scan.ok());
        QVERIFY(scan->hasAmprLibrary);
        QCOMPARE(buildAmprIndex(amprToolRows(scan->files).rows), kV402Ascii);
    }

    // 4.0.2 listed the emulator's own files and hashed names outside ASCII by
    // code point, not by the UTF-8 bytes the runtime hashes: the runtime sets
    // such an index aside, and Auto replaces it.
    void v402IndexOfOtherNamesIsRebuilt()
    {
        QCOMPARE(v402Index(kOtherGame), kV402Other);
        const RuntimeView old = runtimeLoad(kV402Other);
        QVERIFY(!old.loaded);
        QCOMPARE(old.why, QStringLiteral("slot hash"));

        const QByteArray fresh = indexOf(filesOf(kOtherGame));
        const RuntimeView now = runtimeLoad(fresh);
        QVERIFY2(now.loaded, qPrintable(now.why));
        QCOMPARE(now.rows.size(), size_t(6)); // no apr_emu.log
        QVERIFY(std::any_of(now.rows.begin(), now.rows.end(), [](const RuntimeRow& r) { return r.path == "/app0/data/caf\xc3\xa9.bin"; }));
        checkEveryMode(kV402Other, fresh, false);
    }

    // The dump's index no longer describes the game: something the runtime
    // reads differs, and Auto rebuilds it in every such case.
    void staleIndexIsRebuilt_data()
    {
        QTest::addColumn<QByteArray>("dump");
        QTest::addColumn<QByteArray>("fresh");
        QTest::addColumn<bool>("onlyTimes"); // what 5.0.2 still called up to date

        const std::vector<AmprFile> game = filesOf(kAsciiGame);
        const auto changed = [&game](const std::function<void(std::vector<AmprFile>&)>& edit) {
            std::vector<AmprFile> files = game;
            edit(files);
            return indexOf(files);
        };
        const QByteArray now = indexOf(game);
        QTest::newRow("file added") << now << changed([](auto& f) { f.push_back({"data/new.bin", 9, 1700000100}); }) << false;
        QTest::newRow("file removed") << now << changed([](auto& f) { f.erase(f.begin() + 1); }) << false;
        QTest::newRow("renamed, same length") << now << changed([](auto& f) { f[3].relPath = "data/Big.DAT"; }) << false;
        QTest::newRow("renamed, case only") << now << changed([](auto& f) { f[3].relPath = "data/big.bin"; }) << false;
        QTest::newRow("resized") << now << changed([](auto& f) { f[3].size += 1; }) << false;
        QTest::newRow("content changed, same size") << now << changed([](auto& f) { f[0].mtime += 86400; }) << true;
        QTest::newRow("dump copied, every time later") << now << changed([](auto& f) {
            for (AmprFile& x : f)
                x.mtime += 3600;
        }) << true;
        QTest::newRow("one second off") << now << changed([](auto& f) { f[4].mtime -= 1; }) << true;
    }

    void staleIndexIsRebuilt()
    {
        QFETCH(QByteArray, dump);
        QFETCH(QByteArray, fresh);
        QFETCH(bool, onlyTimes);
        QVERIFY(dump != fresh);
        const RuntimeView a = runtimeLoad(dump), b = runtimeLoad(fresh);
        QVERIFY2(a.loaded && b.loaded, qPrintable(a.why + b.why));
        QVERIFY(!(a == b)); // the game can tell them apart
        QCOMPARE(amprIndexesDifferOnlyInTimes(dump, fresh), onlyTimes);
        checkEveryMode(dump, fresh, false);
    }

    // The dump's index is the one this build would write: kept by Auto.
    void currentIndexIsKept()
    {
        const QByteArray fresh = indexOf(filesOf(kAsciiGame));
        checkEveryMode(fresh, fresh, true);
        const Decided r = decide(AmprMode::Auto, fresh, fresh);
        QCOMPARE(r.existingCalls, 1);
    }

    void missingIndexIsAdded()
    {
        const QByteArray fresh = indexOf(filesOf(kAsciiGame));
        for (const AmprMode mode : kModes) {
            const Decided r = decide(mode, std::nullopt, fresh);
            QCOMPARE(r.existingCalls, 0);
            if (mode == AmprMode::Off) {
                QCOMPARE(r.d.action, AmprIndexAction::Off);
                QCOMPARE(r.freshCalls, 0);
            } else {
                QCOMPARE(r.d.action, AmprIndexAction::Added);
                QCOMPARE(r.d.index, fresh);
                QVERIFY(amprActionChangesImage(r.d.action));
            }
        }
        // Nothing to list: nothing is added.
        QCOMPARE(decide(AmprMode::Auto, std::nullopt, QByteArray()).d.action, AmprIndexAction::NotNeeded);
        QCOMPARE(decide(AmprMode::Auto, fresh, QByteArray()).d.action, AmprIndexAction::KeptUpToDate);
    }

    // Other bytes, same rows for the game (the CRC32 differs, the runtime sees
    // the same index): bytes the runtime never reads. Auto still rebuilds,
    // which costs nothing: the image gets an index the game cannot tell apart,
    // and the dump is not written.
    void equivalentIndexWithOtherBytesIsRebuilt()
    {
        const QByteArray fresh = indexOf(filesOf(kAsciiGame));
        const qint64 hashOffset = qint64(getU64(fresh.constData() + 32));
        const qint64 pathEnd = 48 + 5 * 24 + qint64(getU64(fresh.constData() + 24));
        QVERIFY(hashOffset > pathEnd); // the fixture has padding

        QByteArray morePadding = fresh;
        morePadding.insert(qsizetype(hashOffset), QByteArray(8, '\0'));
        putU64(morePadding.data() + 32, quint64(hashOffset + 8));
        QByteArray dirtyPadding = fresh;
        dirtyPadding[qsizetype(pathEnd)] = 'p';
        const QByteArray tail = fresh + QByteArray(16, 't');

        const RuntimeView want = runtimeLoad(fresh);
        for (const QByteArray& dump : {morePadding, dirtyPadding, tail}) {
            QVERIFY(dump != fresh);
            QVERIFY(runtimeLoad(dump) == want);
            checkEveryMode(dump, fresh, false);
        }
        // Same size, other bytes (the size check alone does not decide).
        const Decided r = decide(AmprMode::Auto, dirtyPadding, fresh);
        QCOMPARE(r.existingCalls, 1);
        QCOMPARE(r.d.action, AmprIndexAction::Rebuilt);
        // Another size is not even read.
        QCOMPARE(decide(AmprMode::Auto, tail, fresh).existingCalls, 0);
    }

    void unusableIndexIsRebuilt()
    {
        const QByteArray fresh = indexOf(filesOf(kAsciiGame));
        const QByteArray garbage(fresh.size(), 'x');
        QVERIFY(!runtimeLoad(garbage).loaded);
        checkEveryMode(garbage, fresh, false);
        const Decided r = decide(AmprMode::Auto, fresh, fresh, false, true, true);
        QCOMPARE(r.d.action, AmprIndexAction::Rebuilt);
        QCOMPARE(r.d.index, fresh);
    }

    // A game with asset packs keeps the index its packs were made with (Auto);
    // a game without the library gets none.
    void packsAndLibrary()
    {
        const QByteArray fresh = indexOf(filesOf(kAsciiGame));
        const QByteArray packer("the packer's index");
        QCOMPARE(decide(AmprMode::Auto, packer, fresh, true).d.action, AmprIndexAction::KeptForPacks);
        QCOMPARE(decide(AmprMode::Auto, packer, fresh, true).freshCalls, 0);
        QCOMPARE(decide(AmprMode::Missing, packer, fresh, true).d.action, AmprIndexAction::KeptExisting);
        QCOMPARE(decide(AmprMode::Refresh, packer, fresh, true).d.action, AmprIndexAction::RebuiltAlways);
        QCOMPARE(decide(AmprMode::Off, packer, fresh, true).d.action, AmprIndexAction::Off);
        for (const AmprMode mode : kModes) {
            const Decided r = decide(mode, std::nullopt, fresh, false, false);
            QCOMPARE(r.d.action, mode == AmprMode::Off ? AmprIndexAction::Off : AmprIndexAction::NotNeeded);
            QCOMPARE(r.freshCalls, 0);
        }
    }

    // Verifying against a folder still accepts an image index that differs
    // only in times (amprIndexesDifferOnlyInTimes), and nothing more.
    void verificationToleratesOnlyTimes()
    {
        std::vector<AmprFile> files = filesOf(kAsciiGame);
        const QByteArray built = indexOf(files);
        for (AmprFile& f : files)
            f.mtime += 7200;
        QVERIFY(amprIndexesDifferOnlyInTimes(built, indexOf(files)));
        files[2].size += 1;
        QVERIFY(!amprIndexesDifferOnlyInTimes(built, indexOf(files)));
        files = filesOf(kAsciiGame);
        files[3].relPath = "data/big.bin";
        QVERIFY(!amprIndexesDifferOnlyInTimes(built, indexOf(files)));
        QVERIFY(!amprIndexesDifferOnlyInTimes(built, built + "x"));
    }
};

QTEST_GUILESS_MAIN(TestPfsAmprIndex)
#include "tst_pfs_ampr_index.moc"
