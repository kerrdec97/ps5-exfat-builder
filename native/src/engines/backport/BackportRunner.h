#pragma once

// One complete backport of one game, in the phases the queue shows:
//   Scan    — planBackport(): every check, nothing written (S3-S12, S30-S33, S38)
//   Patch   — buildBackport() in a private staging folder (D1, D4, V1-V5)
//   Package — originals ZIP (required) and backport ZIP written as .part
//   Verify  — both read back (CRC, size, count, SHA-256 per member), then
//             renamed; manifest recorded (S17, S21)
//   Apply   — only when "Apply to game folders" is on: journaled copy into
//             the game with rollback (S35, V7)
// The game folder is not written before Apply (S31). A build with any
// per-file failure produces no backport ZIP and applies nothing (D4, S34).
//
// Every run that passes the preflight writes a run report (#24, RunReport.h)
// next to the game's backup records, whatever its outcome; a failed run names
// it in the error detail (reportFromDetail()).

#include "core/Result.h"
#include "engines/backport/BackportEngine.h"
#include "engines/backport/BackportOptions.h"
#include "jobs/Job.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace fw::backport {

enum class RunStatus { Succeeded, Partial, NothingToDo };
QString runStatusName(RunStatus s);

struct RunResult {
    RunStatus status = RunStatus::Succeeded;
    QString summary;
    QString originalsZip;
    QString backportZip;
    bool applied = false;
    QString manifestPath;
    QString journalPath;
    QStringList warnings;
    QStringList leftUnchanged;
    int filesRewritten = 0;
    bool buildVerified = true; // backport.verifyBuild (V1-V3 per executable)
    QString reportJson;        // #24 run report (RunReport.h)
    QString reportHtml;
    QJsonObject details;
};

QStringList runPhases(); // Scan, Patch, Package, Verify, Apply

Result<RunResult> runBackport(const BackportOptions& options, JobContext& ctx);

JobOutcome toOutcome(const RunResult& r);

} // namespace fw::backport
