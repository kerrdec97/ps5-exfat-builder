#pragma once

// Append-only job history (JSON lines). Used by Home ("Recent jobs") and the
// History view; survives restarts.

#include "core/Result.h"
#include "jobs/JobQueue.h"

#include <QJsonObject>
#include <QString>

#include <mutex>
#include <optional>
#include <vector>

namespace fw {

struct HistoryEntry {
    QString id;
    QString kind;
    QString title;
    QString state;        // jobStateName()
    QString verification; // verificationName()
    QString summary;
    QString error;
    QStringList outputs;
    QDateTime started;
    QDateTime finished;

    QJsonObject toJson() const;
    static HistoryEntry fromJson(const QJsonObject& o);
};

class JobHistory {
public:
    explicit JobHistory(QString path, int maxEntries = 2000);
    Status append(const JobRecord& record);
    // Newest first. The newest entries are kept in memory after the first
    // read (and kept current by append), so Home can ask on every queue
    // change without touching the file.
    std::vector<HistoryEntry> recent(int limit = 50) const;
    Status clear();
    // Writes every entry (oldest first) as RFC 4180 CSV with a header row.
    Status exportCsv(const QString& path) const;
    QString path() const { return m_path; }

private:
    void compactIfNeeded();
    std::vector<HistoryEntry> readRecent(int limit) const; // from the file; m_mutex held
    QString m_path;
    int m_max;
    mutable std::mutex m_mutex;
    static constexpr int kCached = 200;
    mutable std::optional<std::vector<HistoryEntry>> m_cache; // newest first, at most kCached
};

} // namespace fw
