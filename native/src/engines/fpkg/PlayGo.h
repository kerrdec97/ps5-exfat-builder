#pragma once

// PlayGo layout of a package built from a dump.
//
// Three layouts, chosen in this order:
//  C  source table: the dump's sce_sys/playgo-chunk.dat parses, is not
//     trivial (more than one chunk or scenario) and "Reuse the dump's PlayGo
//     chunk table" is on. Chunks, languages and scenarios come from the table;
//     each file's chunk from playgo-ficm.dat + playgo-hash-table.dat.
//  B  generated ("script") layout: no usable table and the fallback is on.
//     N chunks, one 1 MiB payload per language, scenarios from the dump's
//     playgo-scenario.json when it passes the rules, else a default scenario.
//  A  minimal: one chunk, one scenario (fallback off, or a trivial table).
// In B and C every scenario lists every chunk and every chunk is initial;
// chunks without data get a 1 MiB placeholder when "Fill empty chunks" is on.

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
inline constexpr int kMaxChunks = 255;

// ---- languages ----------------------------------------------------------------
// The 31 PlayGo language codes in id order (id = index; a mask has bit 63 - id).
// The id order follows the system-parameter language numbering and is
// UNVERIFIED against Sony documentation; it only matters for layout C.
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
    // The ppr-pkg Companion's "All 31 PlayGo languages" (layout B): the
    // scenario's chunkSupportedLanguages become all 31 codes and the chunk
    // count is raised to at least 32 so every language gets its own chunk.
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
    bool chunkOnEveryFile = false;     // layout B: chunk="0" on game files
    ChunkMap chunkMap;                 // layout C
    QStringList notes;
    QStringList warnings;

    // chunk attribute of a mapped (non-payload) file
    std::optional<int> fileChunk(const QString& rel) const;
};

Result<PlayGoPlan> planPlayGo(const PlayGoInputs& inputs);

} // namespace fw::fpkg
