#pragma once

// Append-only job history (JSON lines). Used by Home ("Recent jobs") and the
// History view; survives restarts.

#include "core/Result.h"
#include "jobs/JobQueue.h"

#include <QJsonObject>
#include <QString>

#include <mutex>
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
    std::vector<HistoryEntry> recent(int limit = 50) const; // newest first
    Status clear();
    QString path() const { return m_path; }

private:
    void compactIfNeeded();
    QString m_path;
    int m_max;
    mutable std::mutex m_mutex;
};

} // namespace fw
