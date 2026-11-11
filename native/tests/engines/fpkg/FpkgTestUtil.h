#pragma once

// Shared helpers of the FPKG tests: synthetic dumps, small binary fixtures
// written by the tests themselves, and settings snapshots.

#include "core/Settings.h"
#include "engines/fpkg/FpkgSettings.h"
#include "engines/fpkg/Module.h"
#include "engines/fpkg/PlayGo.h"
#include "engines/general/Module.h"
#include "jobs/Module.h"
#include "support/TestSupport.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QtEndian>

#include <cstdlib>
#include <memory>
#include <vector>

namespace fpkgtest {

inline const QString kCid = QStringLiteral("UP0000-PPSA01234_00-GOLDENTEST000000");
inline const QString kStem = QStringLiteral("UP0000-PPSA01234_00-GOLDENTEST000000-A0100-V0100");

inline QByteArray readAll(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

inline QByteArray golden(const QString& name)
{
    return readAll(QStringLiteral(FW_FPKG_TEST_DATA) + u'/' + name);
}

inline bool put(const QString& root, const QString& rel, const QByteArray& data)
{
    return fwtest::writeTextFile(QDir(root).filePath(rel), data);
}

// The error of a call that must have failed. error() of a success is
// undefined (it ends the test program); this gives a marker error instead,
// so the check that follows fails in the ordinary way.
template <typename R>
fw::Error errorOf(const R& result)
{
    return result.ok() ? fw::Error(fw::ErrorCode::Internal, QStringLiteral("<the call succeeded>")) : result.error();
}

// Sets an environment variable of this process (and of the tools it starts)
// from a QString. qputenv() would send the UTF-8 bytes through the ANSI code
// page on Windows and damage a path with other characters; the stand-in tool
// reads the wide environment. An empty value removes the variable.
inline void setEnv(const char* name, const QString& value)
{
#ifdef Q_OS_WIN
    _wputenv_s(reinterpret_cast<const wchar_t*>(QString::fromLatin1(name).utf16()),
               reinterpret_cast<const wchar_t*>(value.utf16()));
#else
    if (value.isEmpty())
        qunsetenv(name);
    else
        qputenv(name, value.toLocal8Bit());
#endif
}

// Sets a variable for a scope and puts the earlier state back afterwards.
class EnvVar {
public:
    EnvVar(const char* name, const QByteArray& utf8Value)
        : m_name(name), m_wasSet(qEnvironmentVariableIsSet(name)), m_before(qEnvironmentVariable(name))
    {
        setEnv(name, QString::fromUtf8(utf8Value));
    }
    ~EnvVar() { setEnv(m_name, m_wasSet ? m_before : QString()); }
    EnvVar(const EnvVar&) = delete;
    EnvVar& operator=(const EnvVar&) = delete;

private:
    const char* m_name;
    bool m_wasSet;
    QString m_before;
};

// Keeps the FPKG engine inside `root` and away from the developer's own
// tools: no toolkit from SCE_PROSPERO_PUBTOOLS, and the system temporary
// folder and %ProgramData% (where the engine puts its ASCII links and a work
// folder) are folders under `root`. False when a folder could not be made or
// the temporary folder did not follow.
inline bool confineEngineTo(const QString& root)
{
    qunsetenv("SCE_PROSPERO_PUBTOOLS");
    const QString temp = QDir(root).filePath(QStringLiteral("sys-temp"));
    const QString programData = QDir(root).filePath(QStringLiteral("program-data"));
    if (!QDir().mkpath(temp) || !QDir().mkpath(programData))
        return false;
    setEnv("TMP", QDir::toNativeSeparators(temp));
    setEnv("TEMP", QDir::toNativeSeparators(temp));
    setEnv("TMPDIR", QDir::toNativeSeparators(temp));
    setEnv("ProgramData", QDir::toNativeSeparators(programData));
    return QFileInfo(QDir::tempPath()) == QFileInfo(temp);
}

inline QByteArray be32(quint32 v)
{
    QByteArray b(4, '\0');
    qToBigEndian(v, b.data());
    return b;
}

// 1x1 PNG with the given colour type (2 = RGB, 6 = RGBA).
inline QByteArray makePng(int colorType, int bitDepth = 8)
{
    QByteArray ihdr = be32(1) + be32(1);
    ihdr += char(bitDepth);
    ihdr += char(colorType);
    ihdr += QByteArray(3, '\0');
    QByteArray png("\x89PNG\r\n\x1A\n", 8);
    png += be32(13) + QByteArray("IHDR") + ihdr + be32(0);
    png += be32(0) + QByteArray("IEND") + be32(0);
    return png;
}

// The synthetic dump of the golden tests (see tests/data/fpkg/make_goldens.py).
// False when a golden source file is missing or a file could not be written.
[[nodiscard]] inline bool makeGoldenDump(const QString& d)
{
    const QByteArray param = golden(QStringLiteral("source_param.json"));
    const QByteArray scenario = golden(QStringLiteral("source_scenario.json"));
    return !param.isEmpty() && !scenario.isEmpty()
        && put(d, QStringLiteral("eboot.bin"), QByteArray("\x7F" "ELF", 4) + QByteArray(1000, '\0'))
        && put(d, QStringLiteral("sce_sys/param.json"), param)
        && put(d, QStringLiteral("sce_sys/keystone"), QByteArray(96, 'K'))
        && put(d, QStringLiteral("sce_sys/icon0.png"), makePng(2))
        && put(d, QStringLiteral("sce_sys/param.sfo"), "sfo")
        && put(d, QStringLiteral("sce_sys/about/right.sprx"), "x")
        && put(d, QStringLiteral("sce_sys/playgo-scenario.json"), scenario)
        && put(d, QStringLiteral("sce_module/libc.prx"), QByteArray("\x54\x14\xF5\xEE", 4) + QByteArray(100, '\0'))
        && put(d, QStringLiteral("Data/b.pak"), QByteArray(300, 'b'))
        && put(d, QStringLiteral("Data/A.pak"), QByteArray(200, 'a'))
        && put(d, QStringLiteral("Data/_x.bin"), QByteArray())
        && put(d, QStringLiteral("Data/1.bin"), "1")
        && put(d, QStringLiteral("Data/a&b.bin"), "&")
        && QDir().mkpath(QDir(d).filePath(QStringLiteral("Data/Empty")))
        && put(d, QStringLiteral("Data/JunkOnly/.DS_Store"), "junk")
        && put(d, QStringLiteral("Data/Emptied/old.gp5"), "<x/>")
        && QDir().mkpath(QDir(d).filePath(QStringLiteral("addcont0")))
        && put(d, QStringLiteral("dlc_emu.ini"),
               "[PSAC]\r\ncontent_id=UP0000-PPSA01234_00-GOLDENDLC0000001\r\nmount_point=/app0/addcont0\r\n")
        && put(d, QStringLiteral("._eboot.bin"), "ad")
        && put(d, QStringLiteral("Thumbs.db"), "x")
        && put(d, QStringLiteral("old.exfat"), "img")
        && put(d, QStringLiteral("fakelib/libSceAmpr.sprx"), "a");
}

// A small game dump for end-to-end builds. False when a file could not be written.
[[nodiscard]] inline bool makeSimpleDump(const QString& d, const QString& cid = kCid,
                                         const QString& version = QStringLiteral("01.000.000"))
{
    return put(d, QStringLiteral("eboot.bin"), QByteArray("\x7F" "ELF", 4) + QByteArray(1000, '\0'))
        && put(d, QStringLiteral("sce_sys/param.json"),
               QStringLiteral("{\"contentId\":\"%1\",\"contentVersion\":\"%2\",\"applicationCategoryType\":0,"
                              "\"applicationDrmType\":\"free\",\"localizedParameters\":{\"defaultLanguage\":\"en-US\","
                              "\"en-US\":{\"titleName\":\"Test Game\"}}}").arg(cid, version).toUtf8())
        && put(d, QStringLiteral("sce_sys/keystone"), QByteArray(96, 'K'))
        && put(d, QStringLiteral("sce_sys/icon0.png"), makePng(2))
        && put(d, QStringLiteral("sce_sys/param.sfo"), "sfo")
        && put(d, QStringLiteral("Data/A.pak"), QByteArray(2000, 'a'))
        && put(d, QStringLiteral("Data/shared.pak"), QByteArray(200, 'n'))
        && QDir().mkpath(QDir(d).filePath(QStringLiteral("addcont0")));
}

// ---- PlayGo tables (hand-built, our own layout description) --------------------------
struct PlgxChunk {
    quint64 mask;
    QByteArray label;
};
struct PlgxScenario {
    int type;
    QByteArray label;
    std::vector<int> chunks;
};

inline void putLe16(QByteArray& d, int off, quint16 v) { qToLittleEndian(v, d.data() + off); }
inline void putLe32(QByteArray& d, int off, quint32 v) { qToLittleEndian(v, d.data() + off); }
inline void putLe64(QByteArray& d, int off, quint64 v) { qToLittleEndian(v, d.data() + off); }

inline quint64 langBit(const QString& code) { return quint64(1) << (63 - fw::fpkg::playGoLanguageId(code)); }

inline QByteArray buildPlgx(const std::vector<PlgxChunk>& chunks, const std::vector<PlgxScenario>& scenarios,
                            int defaultScenario, const QString& defaultLanguage, quint64 headerMask)
{
    QByteArray chunkAttrs(int(chunks.size()) * 0x20, '\0'), chunkLabels, scAttrs(int(scenarios.size()) * 0x20, '\0'),
        scChunks, scLabels;
    for (size_t i = 0; i < chunks.size(); ++i) {
        putLe64(chunkAttrs, int(i) * 0x20 + 0x10, chunks[i].mask);
        putLe32(chunkAttrs, int(i) * 0x20 + 0x1C, quint32(chunkLabels.size()));
        chunkLabels += chunks[i].label + '\0';
    }
    for (size_t i = 0; i < scenarios.size(); ++i) {
        const int a = int(i) * 0x20;
        scAttrs[a] = char(scenarios[i].type);
        putLe16(scAttrs, a + 0x14, quint16(scenarios[i].chunks.size()));
        putLe16(scAttrs, a + 0x16, quint16(scenarios[i].chunks.size()));
        putLe32(scAttrs, a + 0x18, quint32(scChunks.size()));
        putLe32(scAttrs, a + 0x1C, quint32(scLabels.size()));
        for (int c : scenarios[i].chunks) {
            QByteArray two(2, '\0');
            qToLittleEndian(quint16(c), two.data());
            scChunks += two;
        }
        scLabels += scenarios[i].label + '\0';
    }
    QByteArray d(0x100, '\0');
    d.replace(0, 4, "plgx");
    putLe16(d, 0x04, 0x1000);
    putLe16(d, 0x0A, quint16(chunks.size()));
    putLe16(d, 0x0E, quint16(scenarios.size()));
    putLe16(d, 0x14, quint16(defaultScenario));
    putLe16(d, 0x24, quint16(fw::fpkg::playGoLanguageId(defaultLanguage)));
    putLe64(d, 0x38, headerMask);
    const auto section = [&](int at, const QByteArray& body) {
        putLe32(d, at, quint32(d.size()));
        putLe32(d, at + 4, quint32(body.size()));
        d += body;
    };
    section(0xC0, chunkAttrs);
    section(0xD0, chunkLabels);
    section(0xE0, scAttrs);
    section(0xE8, scChunks);
    section(0xF0, scLabels);
    putLe32(d, 0x10, quint32(d.size()));
    return d;
}

// playgo-ficm.dat: header (1, 0, 16, byte size of the entries), then one
// 16-bit chunk id per file.
inline QByteArray buildFicm(const std::vector<int>& chunks)
{
    QByteArray d(16, '\0');
    putLe32(d, 0, 1);
    putLe32(d, 8, 16);
    putLe32(d, 12, quint32(chunks.size() * 2));
    for (int c : chunks) {
        QByteArray two(2, '\0');
        qToLittleEndian(quint16(c), two.data());
        d += two;
    }
    return d;
}

inline constexpr quint64 kTestPlayGoSeed = 0x92ca8aab26a24f51ULL;
inline constexpr quint64 kTestPlayGoKey = 0x09bbb761a41bc44dULL;

// playgo-hash-table.dat: header (1, 0, 56, byte size of the entries, "\x7FFLT"
// at 24, the entry count at 36, seed and key at 40 and 48), then one 64-bit
// path hash per file.
inline QByteArray buildFlt(const std::vector<quint64>& hashes, quint64 seed = kTestPlayGoSeed, quint64 key = kTestPlayGoKey)
{
    QByteArray d(56, '\0');
    putLe32(d, 0, 1);
    putLe32(d, 8, 56);
    putLe32(d, 12, quint32(hashes.size() * 8));
    d.replace(24, 4, QByteArray("\x7F" "FLT", 4));
    putLe32(d, 36, quint32(hashes.size()));
    putLe64(d, 40, seed);
    putLe64(d, 48, key);
    for (quint64 h : hashes) {
        QByteArray e(8, '\0');
        qToLittleEndian(h, e.data());
        d += e;
    }
    return d;
}

// The two tables for (package path, chunk) pairs as a package stores them:
// rows sorted by path hash.
struct PlayGoFileMap {
    QByteArray ficm;
    QByteArray hashTable;
};
inline PlayGoFileMap buildFileMap(const std::vector<std::pair<QString, int>>& files, quint64 seed = kTestPlayGoSeed,
                                  quint64 key = kTestPlayGoKey)
{
    std::vector<std::pair<quint64, int>> rows;
    for (const auto& f : files)
        rows.emplace_back(fw::fpkg::playGoPathHash(f.first, seed, key), f.second);
    std::sort(rows.begin(), rows.end());
    std::vector<int> chunks;
    std::vector<quint64> hashes;
    for (const auto& r : rows) {
        hashes.push_back(r.first);
        chunks.push_back(r.second);
    }
    return {buildFicm(chunks), buildFlt(hashes, seed, key)};
}

// ---- settings ---------------------------------------------------------------------------
struct Env {
    std::shared_ptr<fw::SettingsSchema> schema = std::make_shared<fw::SettingsSchema>();
    fw::DependencyRegistry deps;
    fw::JobFactory jobs;
    Env();
    fw::SettingsValues values(const QVariantMap& v = {}) const { return fw::SettingsValues(schema, v, {}); }
};

inline Env::Env()
{
    fw::ModuleContext ctx{*schema, deps, jobs};
    fw::general::registerModule(ctx);
    fw::fpkg::registerModule(ctx);
}

} // namespace fpkgtest
