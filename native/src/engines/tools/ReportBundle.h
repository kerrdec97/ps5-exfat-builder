#pragma once

// "Report a problem": collects what a maintainer needs into one ZIP the user
// can attach to a GitHub issue — recent log files, settings, job history and a
// system summary. The user's home folder is replaced with "~" in every text
// file unless they turn redaction off. Secrets are always hidden: the value of
// every passcode, password, entitlement key, token or webhook (recognised by
// the name of the setting or job parameter, at any depth) is replaced with
// "<hidden>", and a console address is kept only when it is a local network
// address. Nothing is uploaded.

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
// Hides secrets in one report file: JSON (a document or one object per line)
// is scrubbed by key name, any other text by the usual spellings
// ("key": "value", key=value, --key value). Also hides the token of a Discord
// webhook address (also JSON-escaped "\/") and public IPv4 addresses. Applied
// to every report file, even with path redaction off.
QString redactSecrets(const QString& text);
QJsonObject systemSummary();

Result<ReportResult> buildReportBundle(const ReportOptions& options, const CancellationToken& token,
                                       ProgressReporter* progress);

inline const QString kNewIssueUrl = QStringLiteral("https://github.com/kerrdec97/ps5-exfat-builder/issues/new");

} // namespace fw::tools
