#pragma once

// Progress reporting shared by every engine.
//
// Engines describe work as a sequence of named phases ("Scanning",
// "Writing image", "Verifying"). Within a phase they `advance()` a byte (or
// unit) counter from any number of worker threads; the reporter computes the
// speed and the time left and pushes throttled snapshots to a sink (the job
// queue, which forwards them to the UI thread with a queued signal).
//
// What a snapshot promises the views:
//  * overallFraction stays within 0..1, never falls back while the job runs
//    (a phase entered again, a total that grew, weights declared anew) and
//    stays below 1: only the queue says 100 %, when the job has succeeded.
//    Only a different list of phases (setPhases with other names, begin with
//    a new name) starts a new measure.
//  * phaseProgress, the current stage's own share, never falls within a stage.
//  * a stage without a size (total 0) is indeterminate: no percentage, no
//    time left.
//  * speed and time left come from moving averages over the running time
//    (pauses excluded). The time left counts down between measurements, moves
//    towards a new estimate gradually, and is -1 until there is enough to go
//    on (a few seconds of the stage and some units done).

#include <QMetaType>
#include <QList>
#include <QString>
#include <QStringList>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>

namespace fw {

struct ProgressSnapshot {
    QString phase;           // user-facing phase name (a step's name while one runs)
    int phaseIndex = 0;      // 0-based, into phaseNames
    int phaseCount = 1;
    QStringList phaseNames;  // the phases the reporter counts in (empty: no reporter made this snapshot)
    qint64 done = 0;         // units done in this phase (bytes unless unitIsBytes=false), 0..total
    qint64 total = 0;        // 0 = unknown
    qint64 filesDone = 0;
    qint64 filesTotal = 0;
    bool unitIsBytes = true;
    bool indeterminate = false;
    double ratePerSec = 0.0;        // current speed, units/s (falls towards 0 while nothing moves; 0 while paused)
    double averageRatePerSec = 0.0; // units/s over this phase's running time so far
    qint64 etaMs = -1;              // this phase's time left, smoothed; -1 = unknown
    qint64 jobEtaMs = -1;           // the whole job's time left, smoothed; -1 = unknown
    QString currentItem;            // file being processed (relative path)
    double overallFraction = 0.0;   // 0..1 across all phases (weighted)
    double phaseProgress = 0.0;     // 0..1: the share of the current phase done

    // Timing (for the job monitor's stage list and clocks). elapsedMs counts
    // from the first phase; phaseMs holds the time spent in each phase so far,
    // index-aligned with the phases (0 = not entered yet). Both freeze when
    // the reporter is finished.
    qint64 elapsedMs = 0;
    qint64 phaseElapsedMs = 0;
    QList<qint64> phaseMs;
    QList<bool> phaseEntered; // index-aligned: the phase ran (a phase can take under 1 ms)
    bool finished = false;
    // Counts the snapshots of one reporter in the order they were taken (0:
    // made by hand). Its sink can get them out of that order from worker
    // threads; a higher number is the newer state.
    quint64 sequence = 0;

    double phaseFraction() const { return total > 0 ? double(done) / double(total) : 0.0; }
};

class ProgressReporter {
public:
    using Sink = std::function<void(const ProgressSnapshot&)>;
    using Clock = std::chrono::steady_clock;

    explicit ProgressReporter(Sink sink = {}, int throttleMs = 100);

    // Declare the phases up front (weights are relative; default equal; a
    // weight of 0 is a phase that takes no share of the bar).
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
    // current phase and takes the first half of what is left of that phase's
    // share of the overall bar; the phase's own progress then takes the rest,
    // so the bar neither runs to the end nor falls back.
    // Likewise beginPhase() for the phase that runs already goes on from
    // where that phase stood: the new part fills the rest of its share.
    void beginStep(const QString& name, qint64 total, qint64 filesTotal = 0, bool unitIsBytes = true);

    void advance(qint64 units);      // thread-safe
    void advanceFiles(qint64 files); // thread-safe
    void setDone(qint64 units);      // thread-safe
    void setCurrentItem(const QString& item);
    void setTotal(qint64 total);

    // The figures the views show for the running stage (done and total,
    // files, speed) when the stage follows other work than its own count: a
    // job that maps another job's whole bar onto one of its stages shows that
    // job's figures there, while its own count keeps moving the bar and the
    // time left. Until the next phase or step is entered.
    void showFiguresOf(const ProgressSnapshot& other);
    // beginPhase() for such a stage: `units` move the bar only, and no
    // figures show until the first showFiguresOf().
    void beginMappedPhase(int index, qint64 units);

    // The job was paused / resumed (the queue says so). While paused the
    // clocks stand still, and the pause counts neither as elapsed time nor
    // into the speed, so the time left does not jump after Resume.
    void setPaused(bool paused);

    // Push a snapshot now, ignoring the throttle (phase change, completion).
    void flush();

    // The job ended (whatever the outcome): closes the current phase so the
    // clocks stop. Snapshots taken afterwards keep the final times.
    void finish();

    // Thread-safe. Taking a snapshot also brings the speed and the time left
    // up to date, so a stage that stalls shows it without new progress calls.
    ProgressSnapshot snapshot() const;

    // Tests: the clock the reporter reads instead of steady_clock. Set it
    // before the first phase and keep it alive as long as the reporter.
    void setClockForTesting(std::function<Clock::time_point()> now);

private:
    // A time-weighted moving average of units per millisecond. Units are
    // taken to arrive evenly through each window, so the result does not
    // depend on how often it is sampled; while it is younger than its time
    // constant it is the plain average of what it has seen.
    struct RateAverage {
        double units = 0.0;
        double ms = 0.0;
        void add(double du, double dt, double tauMs);
        double perSecond() const { return ms > 0.0 ? units / ms * 1000.0 : 0.0; }
    };
    // The bar shares around the work in progress: what is before it, the
    // current phase (or the part of it a step or the rest after a step
    // takes) and what follows. Fractions of `sum`.
    struct Shares {
        double before = 0.0, current = 0.0, after = 0.0, sum = 0.0;
    };

    void maybeEmit(bool force);
    ProgressSnapshot snapshotLocked() const; // m_mutex held
    // m_mutex held. `step`: a step begins; `stepEnds`: one was running.
    void enterPhaseLocked(int index, qint64 total, qint64 filesTotal, bool unitIsBytes, bool step, bool stepEnds);
    Clock::time_point now() const { return m_clock ? m_clock() : Clock::now(); }
    Clock::time_point clockNowLocked() const;  // m_mutex held; stands still while paused
    qint64 runningMsLocked() const;            // m_mutex held; time since the first phase, pauses excluded
    qint64 doneLocked() const;                 // m_mutex held; 0..total
    Shares sharesLocked() const;               // m_mutex held
    double withinPhaseLocked(double fraction) const; // m_mutex held; the running part's slice applied
    void updateHighsLocked() const;            // m_mutex held; the never-backwards marks
    void sampleLocked(qint64 runMs) const;     // m_mutex held; updates the speed and the time left
    qint64 rawPhaseEtaLocked(qint64 runMs) const;
    qint64 rawJobEtaLocked(qint64 runMs) const;

    Sink m_sink;
    std::chrono::milliseconds m_throttle;
    std::function<Clock::time_point()> m_clock;

    mutable std::mutex m_mutex;
    QStringList m_phaseNames;
    QList<double> m_weights;
    int m_phaseIndex = 0;
    bool m_declared = false;      // setPhases() was called
    bool m_planChanged = false;   // other phases were declared while running; the next phase starts them
    QString m_stepName;           // beginStep(): shown instead of the phase name while it runs
    // The slice of the current phase the running part fills: from m_partBase
    // (what the phase had done when the part began) over m_partSpan.
    double m_partBase = 0.0;
    double m_partSpan = 1.0;
    // timing
    Clock::time_point m_startedAt{};
    Clock::time_point m_phaseStartedAt{};
    QList<qint64> m_phaseMs;      // closed time per phase
    QList<bool> m_phaseEntered;
    qint64 m_totalMs = 0;         // valid once finished
    bool m_started = false;
    bool m_finished = false;
    bool m_paused = false;
    Clock::time_point m_pausedAt{}; // while paused, this is "now" for the clocks
    qint64 m_total = 0;
    qint64 m_filesTotal = 0;
    bool m_unitIsBytes = true;
    QString m_currentItem;
    // showFiguresOf(): what the views show instead of the stage's own count.
    struct ShownFigures {
        qint64 done = 0, total = 0, filesDone = 0, filesTotal = 0;
        bool unitIsBytes = true;
        double ratePerSec = 0.0, averageRatePerSec = 0.0;
    };
    std::optional<ShownFigures> m_shown;
    mutable quint64 m_sequence = 0;

    std::atomic<qint64> m_done{0};
    std::atomic<qint64> m_filesDone{0};
    std::atomic<qint64> m_nextEmitNs{0}; // the throttle, checked before the lock

    // Speed and time left (running ms). Brought up to date by snapshots,
    // which are const to their callers: mutable, under m_mutex.
    mutable qint64 m_measureFromMs = 0;  // this work's measurement began here
    mutable qint64 m_measuredUnits = 0;  // units counted since then (the average speed)
    mutable qint64 m_sampleAtMs = 0;
    mutable qint64 m_sampleDone = 0;
    mutable RateAverage m_speed;         // short: the speed shown
    mutable RateAverage m_etaRate;       // long: the speed behind the time left
    mutable qint64 m_phaseEtaMs = -1, m_phaseEtaAtMs = 0;
    mutable qint64 m_jobEtaMs = -1, m_jobEtaAtMs = 0;
    // Never backwards: the highest values shown so far.
    mutable double m_overallHigh = 0.0;
    mutable double m_phaseHigh = 0.0;
};

} // namespace fw

Q_DECLARE_METATYPE(fw::ProgressSnapshot)
