#pragma once

// Backport manifest: <originalsZipFolder>/BackportBackup/<TitleID>/manifest.json
// (docs/BACKPORT_PARITY.md §2.14). Reads schema 1 (the Python app) unchanged
// and writes schema 2, a superset: unknown members and member order are kept
// (core/OrderedJson), new members are engine, added_files, applied, notes,
// files_after and journal. Hash-based already-backported detection (S7).

#include "core/OrderedJson.h"
#include "core/Result.h"
#include "engines/backport/ExecutableScanner.h"

#include <QMap>
#include <QString>
#include <QStringList>

#include <optional>
#include <utility>

namespace fw::backport {

inline constexpr int kManifestSchemaWritten = 2;

struct Manifest {
    QString path;
    fw::json::Value doc = fw::json::Value::object();

    int schemaVersion() const;
    QString titleId() const;
    std::optional<QString> currentTarget() const;
    QString originalsZipAbs() const;
    QString originalsZipSha256() const;
    QMap<QString, QString> originalsFiles() const;   // arcname → sha256
    QMap<QString, QString> postHashes() const;       // union over targets (keys as stored)
    QStringList addedFiles() const;                  // schema 2
    QMap<QString, QString> addedFileHashes() const;  // schema 2: rel → sha256 at apply time
    bool applied() const;
};

QString manifestPathFor(const QString& originalsDir, const QString& titleId);

// nullopt when there is no manifest; an error when it exists but cannot be
// parsed (the caller decides: the planner warns and treats it as unknown).
Result<std::optional<Manifest>> readManifest(const QString& path);
Status writeManifest(const Manifest& manifest);

Manifest newManifest(const QString& path, const QString& game, const QString& titleId, const QString& title);

struct OriginalsRecord {
    QString zipAbs;
    QString zipSha256;
    QMap<QString, QString> files; // read back from the written ZIP
};

struct TargetRecord {
    int sdk = 0;
    QMap<QString, QString> postHashes;              // staged executables (rel → sha256)
    QMap<QString, QPair<QString, qint64>> filesAfter; // every staged file: sha256, size
    bool applied = false;
    QStringList addedFiles;                         // created in the game by Apply
    QString journal;
    QString notes;
};

void recordOriginals(Manifest& m, const OriginalsRecord& o);
void recordTarget(Manifest& m, const TargetRecord& t);
void recordRestore(Manifest& m, const QString& zip);
void addHistory(Manifest& m, const QString& action, const std::optional<QString>& target);

enum class Verdict { Unknown, Patched, Differs, Original };
QString verdictName(Verdict v);

// Python backported_by_manifest(): any recorded post hash (outside fakelib/)
// matching the current file → Patched; else any recorded original executable
// (not decrypted/ or fakelib/) differing → Differs; else any match →
// Original; else Unknown. Keys compare case-insensitively; files are
// resolved through the scan's index so the result is the same on every OS.
std::pair<Verdict, QStringList> manifestVerdict(const ScanResult& scan, const Manifest* manifest,
                                                const CancellationToken& token = {});

} // namespace fw::backport
