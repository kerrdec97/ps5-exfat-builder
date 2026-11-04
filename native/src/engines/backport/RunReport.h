#pragma once

// Per-run reports (docs/BACKPORT_PARITY.md matrix #24). Every backport run
// that passed its preflight, every SDK fix and every audio scan writes a JSON
// document and a small self-contained HTML page (inline CSS, no scripts, no
// external resources, a Content-Security-Policy that loads nothing, every
// value escaped) next to the game's backup records:
//
//   <originalsZipFolder>/BackportBackup/<TitleID>[/folders/<name>-<hash>]/reports/
//
// The BackportBackup layout and manifest.json are unchanged (D8). Without a
// usable originals ZIP folder the report goes to <dataDir>/backport/reports.
// Report files are never overwritten: a free name is chosen.
//
// A failed job still names its report: the HTML path is appended to the
// error detail ("\nRun report: <path>"; reportFromDetail() reads it back).

#include "core/Result.h"

#include <QJsonObject>
#include <QString>

namespace fw::backport {

inline constexpr const char* kReportFormat = "fw-backport-report";
inline constexpr int kReportSchema = 1;

// <dataDir>/backport/reports
QString defaultReportDir();
// "reports" next to a manifest path (the game folder's own record).
QString reportDirForManifest(const QString& manifestPath);
// The reports folder of `game`'s own record in `originalsDir`, or
// defaultReportDir() when the originals folder is not set or does not exist.
QString reportDirForGame(const QString& originalsDir, const QString& game);

struct ReportFiles {
    QString json;
    QString html;
};

// The common header of every report: format, schema, kind ("backport",
// "sdkFix", "audioScan"), engine identity and the creation time.
QJsonObject newReport(const QString& kind);

// Writes <dir>/<baseName>.json and .html (atomic writes; when either name is
// taken, "<baseName> (N)" is used for both).
Result<ReportFiles> writeReport(const QString& dir, const QString& baseName, const QJsonObject& report);

// "<yyyyMMdd-HHmmss> <what>" with characters Windows forbids replaced by "_".
QString reportBaseName(const QString& what);

// The self-contained HTML page for a report.
QString renderReportHtml(const QJsonObject& report);

// The report marker in an error detail.
QString appendReportToDetail(const QString& detail, const QString& htmlPath);
QString reportFromDetail(const QString& detail);

// {"json": …, "html": …} with native separators, for JobOutcome::details.
QJsonObject reportFilesJson(const ReportFiles& files);

} // namespace fw::backport
