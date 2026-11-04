#pragma once

// "Report a problem": collects what a maintainer needs into one ZIP the user
// can attach to a GitHub issue — recent log files, settings, job history and a
// system summary. The user's home folder is replaced with "~" in every text
// file unless they turn redaction off; secrets (a Discord webhook token) are
// always hidden. Nothing is uploaded.

#include "core/Cancellation.h"
#include "core/Progress.h"
#include "core/Result.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace fw::tools {

struct ReportOptions {
    QString destinationFolder;
    QString logsFolder;             // the app's current log folder
    QStringList extraFiles;         // e.g. settings.json, history.jsonl, queue.json
    QJsonObject diagnostics;        // dependency statuses, self-test, etc. from the UI
    bool redactUserPaths = true;
    qint64 maxLogBytes = 64LL * 1024 * 1024; // newest log files first, up to this much
};

struct ReportResult {
    QString zipPath;
    QStringList contents; // archive entry names
};

QString redactUserPaths(const QString& text);
// Hides secrets that settings may hold (the token of a Discord webhook
// address, also JSON-escaped "\/"). Applied to every report file, even with
// path redaction off.
QString redactSecrets(const QString& text);
QJsonObject systemSummary();

Result<ReportResult> buildReportBundle(const ReportOptions& options, const CancellationToken& token,
                                       ProgressReporter* progress);

inline const QString kNewIssueUrl = QStringLiteral("https://github.com/kerrdec97/ps5-exfat-builder/issues/new");

} // namespace fw::tools
