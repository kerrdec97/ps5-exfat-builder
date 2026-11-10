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

    // Timing (for the job monitor's stage list and clocks). elapsedMs counts
    // from the first phase; phaseMs holds the time spent in each phase so far,
    // index-aligned with the phases (0 = not entered yet). Both freeze when
    // the reporter is finished.
    qint64 elapsedMs = 0;
    qint64 phaseElapsedMs = 0;
    QList<qint64> phaseMs;
    bool finished = false;

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
    // Convenience for single-phase work. A name that is not a declared phase
    // replaces the phase list by that one phase.
    void begin(const QString& phase, qint64 total, qint64 filesTotal = 0, bool unitIsBytes = true);
    // Work a helper does on behalf of a job that has its own phases (unpacking
    // a zipped source before the job's first phase). If `name` is one of the
    // phases, or none were declared with setPhases(), this is begin(). Else the
    // phases stay as they are: the step is shown under its own name inside the
    // current phase and takes the first half of that phase's share of the
    // overall bar; the phase's own progress then takes the other half, so the
    // bar neither runs to the end nor falls back.
    void beginStep(const QString& name, qint64 total, qint64 filesTotal = 0, bool unitIsBytes = true);

    void advance(qint64 units);      // thread-safe
    void advanceFiles(qint64 files); // thread-safe
    void setDone(qint64 units);      // thread-safe
    void setCurrentItem(const QString& item);
    void setTotal(qint64 total);

    // The job was paused / resumed (the queue says so). While paused the
    // clocks stand still, and the pause counts neither as elapsed time nor
    // into the rate, so the time left does not jump after Resume.
    void setPaused(bool paused);

    // Push a snapshot now, ignoring the throttle (phase change, completion).
    void flush();

    // The job ended (whatever the outcome): closes the current phase so the
    // clocks stop. Snapshots taken afterwards keep the final times.
    void finish();

    ProgressSnapshot snapshot() const;

private:
    void maybeEmit(bool force);
    ProgressSnapshot snapshotLocked() const; // m_mutex held
    void enterPhaseLocked(int index, qint64 total, qint64 filesTotal, bool unitIsBytes); // m_mutex held
    std::chrono::steady_clock::time_point clockNowLocked() const;                        // m_mutex held

    Sink m_sink;
    std::chrono::milliseconds m_throttle;

    mutable std::mutex m_mutex;
    QStringList m_phaseNames;
    QList<double> m_weights;
    int m_phaseIndex = 0;
    bool m_declared = false;      // setPhases() was called
    QString m_stepName;           // beginStep(): shown instead of the phase name while it runs
    int m_stepPhase = -1;         // the phase whose share of the bar a step took part of
    double m_stepShare = 0.0;
    // timing
    std::chrono::steady_clock::time_point m_startedAt{};
    std::chrono::steady_clock::time_point m_phaseStartedAt{};
    QList<qint64> m_phaseMs;      // closed time per phase
    qint64 m_totalMs = 0;         // valid once finished
    bool m_started = false;
    bool m_finished = false;
    bool m_paused = false;
    std::chrono::steady_clock::time_point m_pausedAt{}; // while paused, this is "now" for the clocks
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
