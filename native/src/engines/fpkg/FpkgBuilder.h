#pragma once

// FPKG builds, add-on builds, verification and extraction through the user's
// own prospero-pub-cmd.
//
// Build flow: validate the toolchain -> resolve the source (stage an image
// into the job's temp folder; extract or read the base of an update) -> read
// param.json, keystone, base checks -> scan + rules -> plan PlayGo, payloads,
// fillers, empty folders, image/SELF/UCP repairs -> size class -> write the
// generated inputs, the scenario sidecar and the GP5 -> 01 log -> path-rule
// gate -> pre-scan -> ASCII links -> img_create into <stem>.partial.pkg (live
// CPU limit, progress) -> 02 log -> success rules -> header check -> output
// conflict policy and rename -> optional img_verify and SHA-256 -> cleanup
// (logs are always kept; the project is kept on failure or when asked).

#include "core/OrderedJson.h"
#include "core/Result.h"
#include "engines/fpkg/ContentNaming.h"
#include "engines/fpkg/FpkgSettings.h"
#include "engines/fpkg/Gp5.h"
#include "engines/fpkg/InputRepair.h"
#include "engines/fpkg/ParamJson.h"
#include "engines/fpkg/PlayGo.h"
#include "engines/fpkg/SourceScanner.h"
#include "engines/fpkg/Toolchain.h"
#include "jobs/Job.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>
#include <vector>

namespace fw::fpkg {

// ---- requests (the JSON parameters of the jobs) ------------------------------------
struct BuildRequest {
    QString source;          // folder, .gp5, or an image registered through SourceStager
    QString output;          // "" = storage.defaultOutputFolder, else "<source>-pkg" next to the source
    QString basePackage;     // update builds: the base .pkg (--ref_pkg_path)
    QString baseFolder;      // update builds: the base game's folder (avoids extracting the base)
    bool exactSource = false; // update builds: the source is complete (missing files are deleted)
    UpdateOutput updateOutput = UpdateOutput::Both;
    QString contentId;       // optional; must equal param.json
    QString contentVersion;  // optional; written into the packaged param.json
    QString title;           // optional; written into the packaged param.json
    // Not serialised; set by composite jobs (backport updates): keep Sony's
    // <update>.naps_metric.json next to the update instead of deleting it.
    bool keepNapsMetric = false;

    bool isUpdate() const { return !basePackage.isEmpty(); }
    QJsonObject toJson() const;
    static Result<BuildRequest> fromJson(const QJsonObject& o);
};

struct AddonRequest {
    QString payloadFolder;   // the add-on's content folder with its own sce_sys/param.json
    QString output;
    QString contentId;       // optional; must equal the add-on's param.json
    QString entitlementKey;  // 32 hexadecimal characters
    QString volumeType = QStringLiteral("prospero_ac");

    QJsonObject toJson() const;
    static Result<AddonRequest> fromJson(const QJsonObject& o);
};

struct VerifyRequest {
    QString package;
    QString passcode;        // "" = fpkg.passcode
    bool integrity = true;
    QJsonObject toJson() const;
    static Result<VerifyRequest> fromJson(const QJsonObject& o);
};

struct ExtractRequest {
    QString package;
    QString destination;     // must be empty or absent
    QString passcode;        // "" = fpkg.passcode
    QJsonObject toJson() const;
    static Result<ExtractRequest> fromJson(const QJsonObject& o);
};

// ---- the plan (pure: reads the dump, runs no tool, writes nothing) ---------------------
enum class EntryKind { Sidecar, RecoveredImage, Payload, Filler, GameFile, ParamJson, RepairedSelf, RepairedUcp };

struct PlannedEntry {
    QString dst;
    QString src;               // real path the project will name
    QString origin;            // source data for generated copies ("" otherwise)
    std::optional<int> chunk;
    bool noCompression = false;
    qint64 size = 0;
    EntryKind kind = EntryKind::GameFile;
};

struct BuildPlan {
    QStringList roots;          // [source] or [update, base]
    QString outputDir;
    QString contentId;
    ContentVersion version;
    QString stem;
    bool isUpdate = false;

    QString assetsDir;          // <output>/.gp5-assets/<stem>
    QString gp5Path;            // <output>/<stem>.gp5
    QString scenarioPath;       // <output>/<stem>.playgo-scenario.json
    QString logsDir;            // <output>/<stem>-build-logs

    json::Value sourceParam;    // as read
    json::Value param;          // normalised
    QByteArray paramBytes;
    QStringList paramNotes;
    SizeClassDecision size;

    ScanResult scan;
    PlayGoPlan playgo;
    ImagePlan images;
    std::vector<PlannedEntry> entries; // project order
    QStringList emptyDirs;
    qint64 uncompressedBytes = 0;
    int fileEntries = 0;

    QStringList log;            // 01-create-gp5.log body
    QStringList warnings;       // also in the job outcome
};

struct PlanOptions {
    QStringList roots;          // roots[0] = source folder; roots[1] = base tree (updates)
    QString outputDir;
    bool isUpdate = false;
    QString contentId;          // overrides (see BuildRequest)
    QString contentVersion;
    QString title;
    bool haveDds2png = false;
};

Result<BuildPlan> planBuild(const PlanOptions& options, const FpkgSettings& settings, const CancellationToken& token);

// Re-applies the param.json patch and the size class with the final entry sizes.
Status finalizeParam(BuildPlan& plan, const PlanOptions& options, const FpkgSettings& settings);

// The project model with real paths mapped through `toTool`.
Gp5Project projectFromPlan(const BuildPlan& plan, const FpkgSettings& settings,
                           const std::function<QString(const QString&)>& toTool);

// ---- output naming / conflicts ------------------------------------------------------------
QString defaultOutputFolder(const QString& source, const QString& defaultOutputDir);
// Where an existing file would be moved by "Keep both": "name (n).pkg" with the first free n >= 1.
QString keepBothName(const QString& finalPath);

struct OutputMove {
    QString partial;   // the finished file under its temporary name
    QString finalPath; // where it belongs
};
using RenameFn = std::function<bool(const QString& from, const QString& to)>;

// Puts every finished file in place as one step, following the conflict
// policy: first every existing file at a final name is set aside (renamed
// to "name (n).pkg" for "Keep both", to a temporary name for "Overwrite"),
// then every partial file is renamed. If any step fails, everything done so
// far is undone, so the folder never holds a mix of old and new outputs.
// The new files are never deleted: after a failure each one is kept under a
// free numbered name ("name (n).pkg"), or under its partial name when even
// that rename fails; the error names them and `kept` lists them. Notes for
// the outcome go to `notes`. `renameFn` replaces QFile::rename (tests use it
// to simulate a file another program holds open).
Status placeOutputs(const std::vector<OutputMove>& moves, OutputConflict policy, QStringList& notes,
                    QStringList* kept = nullptr, const RenameFn& renameFn = {});
// One file: placeOutputs({{partial, finalPath}}, ...).
Status placeOutput(const QString& partial, const QString& finalPath, OutputConflict policy, QStringList& notes);

// ---- validation (cheap; runs on the UI thread) ----------------------------------------------
Status validateBuildRequest(const BuildRequest& request, const FpkgSettings& settings);
Status validateAddonRequest(const AddonRequest& request, const FpkgSettings& settings);

// ---- running -----------------------------------------------------------------------------------
QStringList buildPhases();
QStringList addonPhases();

Result<JobOutcome> runBuild(const BuildRequest& request, const FpkgSettings& settings, JobContext& ctx);
Result<JobOutcome> runAddon(const AddonRequest& request, const FpkgSettings& settings, JobContext& ctx);
Result<JobOutcome> runVerify(const VerifyRequest& request, const FpkgSettings& settings, JobContext& ctx);
Result<JobOutcome> runExtract(const ExtractRequest& request, const FpkgSettings& settings, JobContext& ctx);

// ---- helpers for composite jobs (backport updates) ------------------------------------
// The ppr-pkg Companion's publisher check: img_verify with only the integrity
// check (required: an error), then with only the format check (diagnostic: a
// warning in the log). Commands go to the job log with the passcode redacted.
struct PublisherVerifyReport {
    QString package;
    bool integrityPassed = false;
    bool formatPassed = false;
    int formatExitCode = 0;
};
Result<PublisherVerifyReport> verifyPublisherPackage(const QString& package, const PubToolchain& tools,
                                                     const FpkgSettings& settings, JobContext& ctx);

// img_extract of the whole package into `destination` (created; must be
// empty). Returns the app root inside it (the folder holding sce_sys).
Result<QString> extractPackage(const QString& package, const QString& destination, const PubToolchain& tools,
                               const FpkgSettings& settings, JobContext& ctx);

// dlc_emu.ini [PSAC] entries of a game folder, for the add-on dialog.
struct AddonCandidate {
    QString contentId;
    QString section;
    QString payloadFolder;  // "" when the mount folder does not exist locally
    QString entitlementKey;
    bool supported = false;
    QString reason;
};
std::vector<AddonCandidate> addonsFromDlcEmu(const QString& gameFolder);

} // namespace fw::fpkg
