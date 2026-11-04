#pragma once

// Preflight for one game (docs/BACKPORT_PARITY.md §2.2 step 0, §3): every
// safety rule that can be checked before anything is written is checked
// here, and every finding is collected (Refuse / Confirm / Warn / Info), so
// a scan can report all of them at once. Nothing is written except the S10
// probe file in each ZIP folder (created and removed).

#include "core/Cancellation.h"
#include "core/Result.h"
#include "engines/backport/BackportManifest.h"
#include "engines/backport/BackportOptions.h"
#include "engines/backport/ExecutableScanner.h"
#include "engines/backport/FakelibSet.h"
#include "engines/backport/GameMetadata.h"

#include <QList>
#include <QString>
#include <QStringList>

#include <optional>
#include <vector>

namespace fw::backport {

enum class Severity { Refuse, Confirm, Warn, Info };
QString severityName(Severity s);

struct Issue {
    Code code = Code::Ok;
    Severity severity = Severity::Info;
    QString message;  // user-facing, names the file or folder
    QStringList files;
};

struct SpaceNeed {
    QString volume;      // e.g. "D:/"
    QStringList uses;    // "staging", "originals ZIP", ...
    qint64 needBytes = 0;
    qint64 availableBytes = 0;
};

struct BackportPlan {
    BackportOptions options;
    ScanResult scan;
    GameMetadata meta;
    QString folderName;
    QString prefix;        // ZIP name prefix
    QString titleId;       // manifest folder
    std::optional<int> gameSdk;
    const SdkPair* pair = nullptr;
    std::optional<FakelibSource> fakelib;
    FakelibState fakelibState = FakelibState::None;
    QStringList fakelibExtras;          // the dump's own fakelib files the set does not replace
    QStringList stagedFakelibTargets;   // game-relative targets of the staged set
    QStringList replacedFakelibFiles;   // existing game files the set replaces (S14)
    QStringList leaveUnchanged;         // encrypted files carried over (policy)
    QString manifestPath;
    std::optional<Manifest> manifest;
    Verdict verdict = Verdict::Unknown;
    QStringList verdictFiles;
    QList<Issue> issues;
    std::vector<SpaceNeed> space;
    int longestStagingPath = 0;
    QString originalsZipPath; // proposed (the final name is chosen again when writing)
    QString backportZipPath;

    bool canRun() const;
    // The first Refuse issue, or an unconfirmed Confirm issue.
    const Issue* blocking() const;
    Error blockingError() const;
    QList<Issue> issuesOf(Severity s) const;
    // "Next run: backport + originals ZIPs · game folder patched"
    QString summary() const;
};

Result<BackportPlan> planBackport(const BackportOptions& options, const CancellationToken& token);

} // namespace fw::backport
