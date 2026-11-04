#pragma once

// Progress reporting shared by every engine.
//
// Engines describe work as a sequence of named phases ("Scanning",
// "Writing image", "Verifying"). Within a phase they `advance()` a byte (or
// unit) counter from any number of worker threads; the reporter computes rate
// and time-left and pushes throttled snapshots to a sink (the job queue, which
// forwards them to the UI thread with a queued signal).

#include <QMetaType>
#include <QList>
#include <QString>
#include <QStringList>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>

namespace fw {

struct ProgressSnapshot {
    QString phase;           // user-facing phase name
    int phaseIndex = 0;      // 0-based
    int phaseCount = 1;
    qint64 done = 0;         // units done in this phase (bytes unless unitIsBytes=false)
    qint64 total = 0;        // 0 = unknown
    qint64 filesDone = 0;
    qint64 filesTotal = 0;
    bool unitIsBytes = true;
    bool indeterminate = false;
    double ratePerSec = 0.0; // smoothed units/s
    qint64 etaMs = -1;       // -1 = unknown
    QString currentItem;     // file being processed (relative path)
    double overallFraction = 0.0; // 0..1 across all phases (weighted)

    double phaseFraction() const { return total > 0 ? double(done) / double(total) : 0.0; }
};

class ProgressReporter {
public:
    using Sink = std::function<void(const ProgressSnapshot&)>;

    explicit ProgressReporter(Sink sink = {}, int throttleMs = 100);

    // Declare the phases up front (weights are relative; default equal).
    void setPhases(const QStringList& names, const QList<double>& weights = {});
    // Enter phase `index` with a total (0 = indeterminate).
    void beginPhase(int index, qint64 total, qint64 filesTotal = 0, bool unitIsBytes = true);
    // Convenience for single-phase work.
    void begin(const QString& phase, qint64 total, qint64 filesTotal = 0, bool unitIsBytes = true);

    void advance(qint64 units);      // thread-safe
    void advanceFiles(qint64 files); // thread-safe
    void setDone(qint64 units);      // thread-safe
    void setCurrentItem(const QString& item);
    void setTotal(qint64 total);

    // Push a snapshot now, ignoring the throttle (phase change, completion).
    void flush();

    ProgressSnapshot snapshot() const;

private:
    void maybeEmit(bool force);

    Sink m_sink;
    std::chrono::milliseconds m_throttle;

    mutable std::mutex m_mutex;
    QStringList m_phaseNames;
    QList<double> m_weights;
    int m_phaseIndex = 0;
    qint64 m_total = 0;
    qint64 m_filesTotal = 0;
    bool m_unitIsBytes = true;
    QString m_currentItem;

    std::atomic<qint64> m_done{0};
    std::atomic<qint64> m_filesDone{0};

    // rate estimation
    std::chrono::steady_clock::time_point m_lastEmit{};
    std::chrono::steady_clock::time_point m_rateT0{};
    qint64 m_rateDone0 = 0;
    double m_rate = 0.0;
};

} // namespace fw

Q_DECLARE_METATYPE(fw::ProgressSnapshot)
