#pragma once

// PlayGo layout of a package built from a dump, laid out as the reference
// toolkit does (sdk-fpkg279-fix15, create-gp5-from-folder.py, with the
// PSVIETHOA FPKG Builder's defaults).
//
//  C  the dump's own layout: "Keep the dump's PlayGo chunks" is on and
//     sce_sys/playgo-chunk.dat is readable, which gives the chunk count; with
//     a valid playgo-ficm.dat + playgo-hash-table.dat the chunk names, the
//     languages of each chunk and each file's chunk are kept too.
//  B  generated layout: no usable file map, or the languages are forced. N
//     chunks (the dump's count when its table is readable, else the setting),
//     every game file in chunk 0 and one 1 MiB payload per language: language
//     i (counted from 1) goes to chunk min(i, N - 1).
//  A  minimal: one chunk, one scenario ("Generate PlayGo chunks" is off and
//     the dump has no readable chunk table).
// In B and C the scenarios come from the dump's playgo-scenario.json when it
// passes the rules (else one default scenario); every scenario lists every
// chunk and every chunk is initial, because a package is installed in full;
// declared chunks without data get a 1 MiB placeholder when "Fill empty
// chunks" is on.

#include "core/OrderedJson.h"
#include "core/Result.h"
#include "engines/fpkg/Gp5.h"

#include <QByteArray>
#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>

#include <optional>
#include <vector>

namespace fw::fpkg {

inline constexpr qint64 kPayloadBytes = 1024 * 1024;
inline constexpr int kMaxChunks = 800;        // the reference toolkit's limit
inline constexpr int kMaxScriptScenarios = 5; // SCE_PLAYGO_MAX_SCENARIO

// ---- languages ----------------------------------------------------------------
// The 31 PlayGo language codes in id order (id = index; a mask has bit 63 - id).
// The order is the reference toolkit's PLAYGO_LANGUAGES.
const QStringList& playGoLanguages();
int playGoLanguageId(const QString& code);           // -1 if unknown (case-insensitive)
QString canonicalPlayGoLanguage(const QString& code); // "" if unknown
QStringList languagesInMask(quint64 mask);            // id order
QString sanitizeLanguageForFileName(const QString& code);

// ---- scenario file (layout B) --------------------------------------------------
struct ScenarioFileInfo {
    bool valid = false;
    QString problem;               // why it was not used
    QStringList supportedLanguages; // chunkSupportedLanguages or derived (canonical codes)
    QString chunkDefaultLanguage;
    QString scenarioDefaultLanguage;
    int defaultId = 0;
    struct Scenario {
        int id = 0;
        QString type;
        QString title; // in the scenario default language, "" if none
    };
    std::vector<Scenario> scenarios; // file order
};
ScenarioFileInfo checkScenarioFile(const json::Value& doc);

// Description written into the default scenario object (our own text).
QString defaultScenarioDescription();
json::Value defaultScenarioObject(const QString& language);
QByteArray scenarioFileBytes(const json::Value& doc); // Python json.dumps(indent=2) + "\n", CRLF

// ---- source table (layout C) -----------------------------------------------------
struct PlayGoTable {
    int chunkCount = 0;
    int scenarioCount = 0;
    int defaultScenario = 0;
    int defaultLanguageId = 0;
    quint64 languageMask = 0;
    struct Chunk {
        quint64 languageMask = 0;
        QString label;
    };
    std::vector<Chunk> chunks;
    struct Scenario {
        int type = 0;
        int initialChunkCount = 0;
        QString label;
        std::vector<int> chunks;
    };
    std::vector<Scenario> scenarios;
    bool trivial() const { return chunkCount <= 1 && scenarioCount <= 1; }
};
Result<PlayGoTable> parsePlayGoChunkDat(const QByteArray& data);

struct ChunkMap {
    QHash<quint64, int> chunkByHash;
    bool misnamed = false; // the FLT table is inside playgo-ficm.dat (or vice versa)
    QString problem;       // why the map is empty
    int chunkOf(const QString& rel) const; // 0 when unknown
};
ChunkMap parseChunkMap(const QByteArray& ficm, const QByteArray& hashTable, int chunkCount);

// PS5 flat-path-table hash of a package path (uppercased, leading '/' stripped).
quint64 ps5FlatPathHash(const QString& path);
// The hash playgo-hash-table.dat stores for a package path without a leading
// slash; seed and key come from that file's header.
quint64 playGoPathHash(const QString& rel, quint64 seed, quint64 key);

// ---- the dump's own layout (layout C) ---------------------------------------------
struct SourcePlayGoLayout {
    struct Chunk {
        QString label;         // "" only for a chunk without languages
        QStringList languages; // id order
        bool declared() const { return !label.isEmpty() || !languages.isEmpty(); }
    };
    struct Scenario {
        QString label;
        int initialChunkCount = 0;
        std::vector<int> chunks; // retail order
    };
    std::vector<Chunk> chunks;
    QHash<quint64, int> fileChunks; // path hash -> chunk
    quint64 seed = 0;
    quint64 key = 0;
    std::optional<std::vector<Scenario>> scenarios; // nullopt: the scenario part could not be read

    QStringList languages() const; // every language a chunk names, id order
    quint64 hashOf(const QString& rel) const;
    // The chunk the dump's map gives a package path; a sce_sys/*.png is also
    // looked up as .dds. nullopt when the map does not name it.
    std::optional<int> chunkOf(const QString& rel, quint64* hash = nullptr) const;
};

// The chunk count of a PS5 playgo-chunk.dat ("plgx", version 0x1000, 1-800
// chunks, 1-5 scenarios). The error message says why the file was not used.
Result<int> sourcePlayGoChunkCount(const QByteArray& chunkDat);

// The layout of the three tables. An empty optional: several chunks, but
// every file is in chunk 0 (nothing to keep). An error: a table is damaged
// or the files are misnamed. `warnings` receives notes about ignored parts.
Result<std::optional<SourcePlayGoLayout>> readSourcePlayGoLayout(const QByteArray& chunkDat, const QByteArray& ficm,
                                                                  const QByteArray& hashTable, int chunkCount,
                                                                  QStringList* warnings);

// ---- planning ---------------------------------------------------------------------
enum class PlayGoLayout { Minimal, Script, SourceTable };
QString playGoLayoutName(PlayGoLayout l); // "A (minimal)" ...

struct PlannedPayload {
    QString dst;        // package path
    int chunk = 0;
    QString language;   // "" for fillers
    bool filler = false;
};

struct PlayGoInputs {
    std::optional<QByteArray> sourceScenario;  // sce_sys/playgo-scenario.json of the dump
    std::optional<QByteArray> chunkDat;        // sce_sys/playgo-chunk.dat
    std::optional<QByteArray> ficm;            // sce_sys/playgo-ficm.dat
    std::optional<QByteArray> hashTable;       // sce_sys/playgo-hash-table.dat
    QString paramDefaultLanguage = QStringLiteral("en-US");
    bool useSourceTable = false;
    bool fallback = true;
    bool fillEmptyChunks = true;
    int chunkCount = 100;
    // The ppr-pkg Companion's "All 31 PlayGo languages": the dump's layout is
    // replaced by generated chunks (layout B), the scenario's
    // chunkSupportedLanguages become all 31 codes and the chunk count is
    // raised to at least 32 so every language gets its own chunk.
    bool allLanguages = false;
    // Backport updates: the base package's own language list (its chunk
    // count comes in chunkCount); wins over allLanguages.
    QStringList forcedLanguages;
    // Package paths and sizes of every mapped file (game files, the scenario
    // sidecar, recovered images) and paths already taken (case-insensitive).
    std::vector<std::pair<QString, qint64>> files;
};

struct PlayGoPlan {
    PlayGoLayout layout = PlayGoLayout::Minimal;
    Gp5ChunkInfo chunkInfo;
    QByteArray scenarioBytes;          // the sidecar mapped to sce_sys/playgo-scenario.json
    bool scenarioIsSourceVerbatim = false;
    std::vector<PlannedPayload> payloads; // language payloads, then fillers
    bool chunkOnEveryFile = false;     // layouts B and C: every file names its chunk
    std::optional<SourcePlayGoLayout> source; // layout C
    QStringList notes;
    QStringList warnings;

    // chunk attribute of a mapped (non-payload) file
    std::optional<int> fileChunk(const QString& rel) const;
};

Result<PlayGoPlan> planPlayGo(const PlayGoInputs& inputs);

} // namespace fw::fpkg
