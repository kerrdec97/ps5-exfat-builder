#pragma once

// Backport update packages (the ppr-pkg Companion's "Backport" tab and its
// "dual backports" on the Create tab, ported).
//
// A backport is shipped as its own small update package that installs over
// the game's base package, so the base stays the original game. Up to two
// backports (4.xx and 5.xx+, game-root layout: eboot.bin, sce_module/,
// fakelib/ ...) become two alternative updates against the same base:
// <TITLEID>-backport-4xx.pkg and <TITLEID>-backport-5xxplus.pkg.
//
// Flow per update: the base is inspected (kind, digest, param.json) and
// verified (img_verify integrity pass required), the backport files overlay
// the game folder (or the extracted base), contentVersion = base + 1 in the
// last group, img_create builds the delta against the base, the delta must
// be a LIH container that references the base's digest (else the console
// refuses it with CE-107891-6), the .remastered.pkg must carry the base's
// identity and the new version and pass the integrity check, and only then
// is the delta given its final name (never replacing an existing file: -2,
// -3 ...). With buildBase the base package is built from the game folder
// first ("dual backports").
//
// Needs Sony's publisher from a verified SDK Fix release (Toolchain.h).

#include "engines/fpkg/FpkgSettings.h"
#include "jobs/Job.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <optional>

namespace fw::fpkg {

inline const QString kJobBackport = QStringLiteral("fpkg.backport");

struct BackportUpdateRequest {
    QString basePackage;  // the base installed on the console ("" with buildBase)
    QString gameFolder;   // optional; required with buildBase
    QString backport4xx;  // backport files: folder or .zip ("" = none)
    QString backport5xx;
    QString version;      // "" = base + 1 in the last group
    QString output;       // "" = the base package's folder (buildBase: the game's FPKG output folder)
    bool buildBase = false;

    bool hasAny() const { return !backport4xx.trimmed().isEmpty() || !backport5xx.trimmed().isEmpty(); }
    QJsonObject toJson() const;
    static Result<BackportUpdateRequest> fromJson(const QJsonObject& o);
};

// "01.000.000" -> "01.000.001" ("" when the base is not NN.NNN.NNN or at the maximum).
QString nextBackportVersion(const QString& baseVersion);
// The Companion's UpdateVersion: blank request = auto; must be NN.NNN.NNN and
// greater than the base.
Result<QString> backportUpdateVersion(const QString& baseVersion, const QString& requested);

// "<TITLEID>-backport-<tag>.pkg"
QString backportPackageName(const QString& titleId, const QString& tag);
// `wanted`, else "<stem>-2<ext>", "-3" ... (a name is taken when the file, a
// folder of that name or "<name>.naps_metric.json" exists).
QString availableOutputPath(const QString& wanted);

// One wrapper folder around the backport files is skipped: while the folder
// holds exactly one entry and it is a folder not named sce_sys, sce_module,
// fakelib, sce_suppl, sce_sc or playgo-languages, go into it.
QString resolveBackportFolder(const QString& folder);

// "4xx" for names like "FW 4xx ..." / "4.xx" / "SDK 4", "5xxplus" for 5+,
// "" when the name says nothing (the Backport tab's ZIP names).
QString firmwareGroupFromName(const QString& fileName);

// Cheap checks for the UI thread and the queue (paths exist, at least one
// backport, overlap rules, the version text).
Status validateBackportRequest(const BackportUpdateRequest& request, const FpkgSettings& settings);

QStringList backportPhases(const BackportUpdateRequest& request);

// The settings one update is built with: the job's, with the checks and the
// clean-up this flow does itself switched off, param.json left as shipped
// and, when the base's PlayGo table was read (baseChunks > 0 and languages),
// a generated layout with exactly that chunk count and those languages,
// whatever the PlayGo settings say.
FpkgSettings backportUpdateSettings(const FpkgSettings& settings, int baseChunks, const QStringList& baseLanguages);

// Work folders (<output>\.ppr-backport-*, .ppr-sdkbase-*, and the zstd and
// FPKG staging folders .fw-zstd-*, .fw-stage-*) are listed in
// <local data>\state\backport-work.txt with an owner marker (process id and
// start time); a folder whose owner process no longer runs (a crash or power
// loss) is removed by this sweep, which runs when the app starts and before
// every backport job. A folder on a drive that is not connected stays listed
// for a later sweep. Returns how many were removed.
int sweepStaleBackportWork();

// Creates `dir` (which must not exist; its name must start with one of the
// prefixes above), records it and writes the owner marker.
Status createTrackedWorkFolder(const QString& dir);
// Removes it and its index line (kept for the next sweep if it cannot be removed).
void removeTrackedWorkFolder(const QString& dir);
struct TrackedWorkFolder {
    QString dir;
    TrackedWorkFolder() = default;
    explicit TrackedWorkFolder(QString d) : dir(std::move(d)) {}
    TrackedWorkFolder(const TrackedWorkFolder&) = delete;
    TrackedWorkFolder& operator=(const TrackedWorkFolder&) = delete;
    ~TrackedWorkFolder()
    {
        if (!dir.isEmpty())
            removeTrackedWorkFolder(dir);
    }
};


Result<JobOutcome> runBackportUpdates(const BackportUpdateRequest& request, const FpkgSettings& settings, JobContext& ctx);

class FpkgBackportJob final : public Job {
public:
    explicit FpkgBackportJob(BackportUpdateRequest request);
    QString kind() const override { return kJobBackport; }
    QString title() const override { return m_title; }
    QString subtitle() const override;
    QStringList phases() const override { return backportPhases(m_request); }
    QJsonObject parameters() const override { return m_request.toJson(); }
    QStringList busyPaths() const override;
    Status validate(const SettingsValues& settings) const override;
    Result<JobOutcome> run(JobContext& ctx) override;

private:
    BackportUpdateRequest m_request;
    QString m_title;
};

} // namespace fw::fpkg
