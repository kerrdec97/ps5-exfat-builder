#include "core/Progress.h"

#include <QStringList>

#include <algorithm>
#include <cmath>

namespace fw {

using Clock = std::chrono::steady_clock;

namespace {

// The shown speed follows changes within a few seconds; the speed behind the
// time left looks further back, so a burst or a short stall moves it little.
constexpr double kSpeedTauMs = 3000.0;
constexpr double kEtaRateTauMs = 20000.0;
// A new time-left estimate is reached gradually (about 2/3 of the way in this time).
constexpr double kEtaSmoothTauMs = 4000.0;
// The speed is measured over windows of at least this much running time.
constexpr qint64 kSampleMs = 500;
// A phase's time left is shown after this much of its running time (and once
// some units are done); the job's after this much in all.
constexpr qint64 kPhaseEtaAfterMs = 3000;
constexpr qint64 kJobEtaAfterMs = 5000;
// Beyond 1000 days it is no estimate.
constexpr double kMaxEtaMs = 8.64e10;
// While the job runs the bar stops short of 100 %: only the queue, when the
// job has succeeded, says it is complete.
constexpr double kBelowDone = 0.999;
// The part of a phase a step (beginStep) takes.
constexpr double kStepShare = 0.5;

qint64 msBetween(Clock::time_point from, Clock::time_point to)
{
    return std::max<qint64>(0, std::chrono::duration_cast<std::chrono::milliseconds>(to - from).count());
}

qint64 nsOf(Clock::time_point t)
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
}

// The time left `shown` was set at running time `at` and counts down since.
qint64 shownEta(qint64 shown, qint64 at, qint64 runMs)
{
    return shown < 0 ? -1 : std::max<qint64>(0, shown - (runMs - at));
}

// Moves the shown time left towards a new estimate: it keeps counting down
// and takes a share of the difference that grows with the time since the
// last estimate. An unknown estimate hides it; the first one is taken as it is.
void smoothEta(qint64& shown, qint64& at, qint64 raw, qint64 runMs)
{
    if (raw < 0) {
        shown = -1;
    } else if (shown < 0) {
        shown = raw;
    } else {
        const double predicted = double(shownEta(shown, at, runMs));
        const double k = 1.0 - std::exp(-double(std::max<qint64>(0, runMs - at)) / kEtaSmoothTauMs);
        shown = std::max<qint64>(0, std::llround(predicted + (double(raw) - predicted) * k));
    }
    at = runMs;
}

} // namespace

void ProgressReporter::RateAverage::add(double du, double dt, double tauMs)
{
    if (dt <= 0.0) {
        units += du;
        return;
    }
    const double decay = std::exp(-dt / tauMs);
    const double span = tauMs * (1.0 - decay); // the window's weight, in ms
    units = units * decay + du * (span / dt);
    ms = ms * decay + span;
}

ProgressReporter::ProgressReporter(Sink sink, int throttleMs)
    : m_sink(std::move(sink)), m_throttle(std::max(0, throttleMs))
{
    m_phaseNames = {QStringLiteral("Working")};
    m_weights = {1.0};
    m_phaseMs = {0};
    m_phaseEntered = {false};
}

void ProgressReporter::setClockForTesting(std::function<Clock::time_point()> now)
{
    std::lock_guard lock(m_mutex);
    m_clock = std::move(now);
}

void ProgressReporter::setPhases(const QStringList& names, const QList<double>& weights)
{
    std::lock_guard lock(m_mutex);
    const QStringList next = names.isEmpty() ? QStringList{QStringLiteral("Working")} : names;
    const bool newPlan = m_started && !m_finished && next != m_phaseNames;
    if (newPlan) {
        // Other phases altogether (a job that runs other jobs on its own
        // reporter): a new measure, which the old marks, counts and stage
        // times say nothing about. Its first beginPhase starts it.
        m_overallHigh = 0.0;
        m_phaseHigh = 0.0;
        m_partBase = 0.0;
        m_partSpan = 1.0;
        m_stepName.clear();
        m_total = 0;
        m_filesTotal = 0;
        m_done.store(0);
        m_filesDone.store(0);
        m_currentItem.clear();
        m_planChanged = true;
    } else if (m_started) {
        updateHighsLocked(); // weights declared anew do not take the bar back
    }
    m_declared = true;
    m_phaseNames = next;
    m_weights.clear();
    bool anyShare = false;
    for (int i = 0; i < m_phaseNames.size(); ++i) {
        // 0 is a phase that takes no share; what is not a weight counts as 1.
        const double w = i < weights.size() ? weights[i] : 1.0;
        m_weights.push_back(std::isfinite(w) && w >= 0.0 ? w : 1.0);
        anyShare = anyShare || m_weights.back() > 0.0;
    }
    if (!anyShare)
        std::fill(m_weights.begin(), m_weights.end(), 1.0);
    // Keep the time already spent in phases that keep their index (a job may
    // declare its phases after a first "Preparing" phase); new phases start at 0.
    m_phaseMs.resize(m_phaseNames.size());
    for (int i = 0; i < m_phaseMs.size(); ++i)
        if (m_phaseMs[i] < 0 || newPlan)
            m_phaseMs[i] = 0;
    m_phaseEntered.resize(m_phaseNames.size());
    if (newPlan)
        std::fill(m_phaseEntered.begin(), m_phaseEntered.end(), false);
    m_phaseIndex = std::clamp(m_phaseIndex, 0, int(m_phaseNames.size()) - 1);
}

void ProgressReporter::enterPhaseLocked(int index, qint64 total, qint64 filesTotal, bool unitIsBytes, bool step, bool stepEnds)
{
    const int next = std::clamp(index, 0, std::max(0, int(m_phaseNames.size()) - 1));
    const auto t = clockNowLocked();
    const bool wasStarted = m_started;
    const bool newPlan = m_planChanged;
    m_planChanged = false;
    if (wasStarted && !newPlan && m_phaseIndex >= 0)
        updateHighsLocked(); // where the phase stands now, shown or not: the next part starts there
    if (!m_started) {
        m_started = true;
        m_startedAt = t;
        m_phaseStartedAt = t;
    } else if (newPlan) {
        m_phaseStartedAt = t; // the new phases' first one starts here
    } else if (next != m_phaseIndex && !m_finished) {
        // Close the phase that ends; a phase entered again (a second
        // beginPhase with the real total) keeps its clock running.
        if (m_phaseIndex >= 0 && m_phaseIndex < m_phaseMs.size())
            m_phaseMs[m_phaseIndex] += msBetween(m_phaseStartedAt, t);
        m_phaseStartedAt = t;
    }
    if (next < m_phaseEntered.size())
        m_phaseEntered[next] = true;
    const bool samePhase = wasStarted && !newPlan && next == m_phaseIndex;
    // The same work going on (its phase entered again with the real total)
    // keeps its speed; other work is measured afresh, since a scan before a
    // compare or an unpacking step says nothing about the speed of what follows.
    const bool sameWork = samePhase && unitIsBytes == m_unitIsBytes && m_total > 0 && !step && !stepEnds;
    // A phase entered again goes on from where it stood: this part fills the
    // rest of it (a step the first half of that), so the stage neither falls
    // back nor runs to its end before its last part.
    if (!samePhase)
        m_phaseHigh = 0.0;
    m_partBase = m_phaseHigh;
    m_partSpan = (1.0 - m_partBase) * (step ? kStepShare : 1.0);
    m_phaseIndex = next;
    m_total = std::max<qint64>(0, total);
    m_filesTotal = std::max<qint64>(0, filesTotal);
    m_unitIsBytes = unitIsBytes;
    m_currentItem.clear();
    m_shown.reset();
    m_done.store(0);
    m_filesDone.store(0);
    const qint64 run = runningMsLocked();
    m_sampleAtMs = run;
    m_sampleDone = 0;
    if (!sameWork) {
        m_speed = {};
        m_etaRate = {};
        m_measureFromMs = run;
        m_measuredUnits = 0;
        m_phaseEtaMs = -1;
        m_phaseEtaAtMs = run;
    }
}

void ProgressReporter::beginPhase(int index, qint64 total, qint64 filesTotal, bool unitIsBytes)
{
    {
        std::lock_guard lock(m_mutex);
        const bool stepEnds = !m_stepName.isEmpty();
        m_stepName.clear(); // a step ends where the job's next phase begins
        enterPhaseLocked(index, total, filesTotal, unitIsBytes, false, stepEnds);
    }
    flush();
}

void ProgressReporter::begin(const QString& phase, qint64 total, qint64 filesTotal, bool unitIsBytes)
{
    {
        std::lock_guard lock(m_mutex);
        int idx = int(m_phaseNames.indexOf(phase));
        if (idx < 0) {
            m_phaseNames = {phase};
            m_weights = {1.0};
            m_phaseMs = {0};
            m_phaseEntered = {false};
            m_phaseIndex = -1; // a new phase, not the old first one entered again
            m_phaseHigh = 0.0;
            m_overallHigh = 0.0; // a new measure (see setPhases)
            idx = 0;
        }
        const bool stepEnds = !m_stepName.isEmpty();
        m_stepName.clear();
        enterPhaseLocked(idx, total, filesTotal, unitIsBytes, false, stepEnds);
    }
    flush();
}

void ProgressReporter::beginStep(const QString& name, qint64 total, qint64 filesTotal, bool unitIsBytes)
{
    bool insidePhase = false;
    {
        std::lock_guard lock(m_mutex);
        insidePhase = m_declared && !m_phaseNames.contains(name);
        if (insidePhase) {
            const bool stepEnds = !m_stepName.isEmpty();
            m_stepName = name;
            enterPhaseLocked(m_phaseIndex, total, filesTotal, unitIsBytes, true, stepEnds); // the phase's clock keeps running
        }
    }
    if (insidePhase)
        flush();
    else
        begin(name, total, filesTotal, unitIsBytes);
}

void ProgressReporter::advance(qint64 units)
{
    m_done.fetch_add(units, std::memory_order_relaxed);
    maybeEmit(false);
}

void ProgressReporter::advanceFiles(qint64 files)
{
    m_filesDone.fetch_add(files, std::memory_order_relaxed);
    maybeEmit(false);
}

void ProgressReporter::setDone(qint64 units)
{
    m_done.store(units, std::memory_order_relaxed);
    maybeEmit(false);
}

void ProgressReporter::setCurrentItem(const QString& item)
{
    {
        std::lock_guard lock(m_mutex);
        m_currentItem = item;
    }
    maybeEmit(false);
}

void ProgressReporter::beginMappedPhase(int index, qint64 units)
{
    {
        std::lock_guard lock(m_mutex);
        const bool stepEnds = !m_stepName.isEmpty();
        m_stepName.clear();
        enterPhaseLocked(index, units, 0, false, false, stepEnds);
        ShownFigures none;
        none.unitIsBytes = false;
        m_shown = none;
    }
    flush();
}

void ProgressReporter::showFiguresOf(const ProgressSnapshot& other)
{
    {
        std::lock_guard lock(m_mutex);
        ShownFigures f;
        f.total = std::max<qint64>(0, other.total);
        f.done = std::max<qint64>(0, other.done);
        f.filesTotal = std::max<qint64>(0, other.filesTotal);
        f.filesDone = std::max<qint64>(0, other.filesDone);
        f.unitIsBytes = other.unitIsBytes;
        f.ratePerSec = other.ratePerSec;
        f.averageRatePerSec = other.averageRatePerSec;
        m_shown = f;
    }
    maybeEmit(false);
}

void ProgressReporter::setTotal(qint64 total)
{
    {
        std::lock_guard lock(m_mutex);
        if (m_started)
            updateHighsLocked(); // a total that grows does not take back what was done
        m_total = std::max<qint64>(0, total);
    }
    maybeEmit(false);
}

void ProgressReporter::flush()
{
    maybeEmit(true);
}

void ProgressReporter::finish()
{
    {
        std::lock_guard lock(m_mutex);
        if (m_finished)
            return;
        m_finished = true;
        if (!m_started)
            return;
        const auto t = clockNowLocked();
        if (m_phaseIndex >= 0 && m_phaseIndex < m_phaseMs.size())
            m_phaseMs[m_phaseIndex] += msBetween(m_phaseStartedAt, t);
        m_totalMs = msBetween(m_startedAt, t);
    }
    maybeEmit(true);
}

Clock::time_point ProgressReporter::clockNowLocked() const
{
    return m_paused ? m_pausedAt : now();
}

qint64 ProgressReporter::runningMsLocked() const
{
    return m_started ? msBetween(m_startedAt, clockNowLocked()) : 0;
}

qint64 ProgressReporter::doneLocked() const
{
    const qint64 d = std::max<qint64>(0, m_done.load(std::memory_order_relaxed));
    return m_total > 0 ? std::min(d, m_total) : d;
}

void ProgressReporter::setPaused(bool paused)
{
    {
        std::lock_guard lock(m_mutex);
        if (paused == m_paused || m_finished)
            return;
        const auto t = now();
        if (paused) {
            m_pausedAt = t; // the running time stands still from here
            m_paused = true;
        } else {
            // Move the start marks by the length of the pause: the running
            // time, and with it the speed and the time left, go on from
            // where they stood.
            const auto pause = t - m_pausedAt;
            m_startedAt += pause;
            m_phaseStartedAt += pause;
            m_paused = false;
        }
    }
    maybeEmit(true);
}

ProgressReporter::Shares ProgressReporter::sharesLocked() const
{
    Shares s;
    for (int i = 0; i < m_weights.size(); ++i) {
        const double w = m_weights[i];
        s.sum += w;
        if (i < m_phaseIndex)
            s.before += w;
        else if (i == m_phaseIndex)
            s.current = w;
        else
            s.after += w;
    }
    // The part of the phase that runs now (a step, or the phase entered
    // again) fills its own slice of the phase's share.
    s.before += s.current * m_partBase;
    s.after += s.current * std::max(0.0, 1.0 - m_partBase - m_partSpan);
    s.current *= m_partSpan;
    return s;
}

double ProgressReporter::withinPhaseLocked(double fraction) const
{
    return m_partBase + m_partSpan * fraction;
}

qint64 ProgressReporter::rawPhaseEtaLocked(qint64 runMs) const
{
    // Nothing to count down to, too early, or nothing measured yet.
    if (m_total <= 0 || runMs - m_measureFromMs < kPhaseEtaAfterMs || m_measuredUnits <= 0)
        return -1;
    // Bytes below 1 B/s are noise; a phase counted in files or steps can be
    // far slower than one unit a second and still has a time left.
    const double rate = m_etaRate.perSecond();
    if (rate <= (m_unitIsBytes ? 1.0 : 0.0))
        return -1;
    const double eta = double(m_total - doneLocked()) / rate * 1000.0;
    return eta < kMaxEtaMs ? qint64(eta) : -1;
}

qint64 ProgressReporter::rawJobEtaLocked(qint64 runMs) const
{
    // A phase without a size: nobody can say how long the job still takes.
    if (m_total <= 0 || runMs < kJobEtaAfterMs)
        return -1;
    const Shares sh = sharesLocked();
    if (sh.sum <= 0.0)
        return -1;
    const qint64 phase = m_phaseEtaMs; // smoothed just before, at runMs
    if (phase < 0) {
        // The phase has just begun: the earlier estimate counts on for the
        // few seconds its measurement takes. Longer without an estimate (it
        // has not moved, or hardly): nobody can say.
        const bool justBegun = runMs - m_measureFromMs < kPhaseEtaAfterMs;
        return justBegun ? shownEta(m_jobEtaMs, m_jobEtaAtMs, runMs) : -1;
    }
    // The phases still to come take as long per share as the job up to the
    // end of this phase is about to have taken.
    const double upTo = sh.before + sh.current;
    double eta = double(phase);
    if (sh.after > 0.0) {
        if (upTo <= 0.0)
            return -1;
        eta += sh.after * (double(runMs) + double(phase)) / upTo;
    }
    return eta < kMaxEtaMs ? qint64(eta) : -1;
}

void ProgressReporter::sampleLocked(qint64 runMs) const
{
    if (!m_started || m_finished)
        return;
    const qint64 dt = runMs - m_sampleAtMs;
    if (dt < kSampleMs)
        return; // also while paused: the running time stands still
    // The speed counts what was done, also beyond a total that was too small;
    // a count set back is measured from its new value.
    const qint64 raw = std::max<qint64>(0, m_done.load(std::memory_order_relaxed));
    const qint64 d = std::max<qint64>(0, raw - m_sampleDone);
    m_speed.add(double(d), double(dt), kSpeedTauMs);
    m_etaRate.add(double(d), double(dt), kEtaRateTauMs);
    m_measuredUnits += d;
    m_sampleAtMs = runMs;
    m_sampleDone = raw;
    smoothEta(m_phaseEtaMs, m_phaseEtaAtMs, rawPhaseEtaLocked(runMs), runMs);
    smoothEta(m_jobEtaMs, m_jobEtaAtMs, rawJobEtaLocked(runMs), runMs);
}

void ProgressReporter::updateHighsLocked() const
{
    if (m_planChanged)
        return; // the new phases have not begun: nothing to measure yet
    // The bar: never back, never at 100 % from here (see kBelowDone). A
    // phase without a size adds nothing until it ends.
    const Shares sh = sharesLocked();
    const double f = m_total > 0 ? double(doneLocked()) / double(m_total) : 0.0;
    double overall = sh.sum > 0.0 ? (sh.before + sh.current * f) / sh.sum : 0.0;
    if (!std::isfinite(overall))
        overall = 0.0;
    m_overallHigh = std::max(m_overallHigh, std::clamp(overall, 0.0, kBelowDone));
    double within = withinPhaseLocked(f);
    if (!std::isfinite(within))
        within = 0.0;
    m_phaseHigh = std::max(m_phaseHigh, std::clamp(within, 0.0, kBelowDone));
}

ProgressSnapshot ProgressReporter::snapshot() const
{
    std::lock_guard lock(m_mutex);
    return snapshotLocked();
}

ProgressSnapshot ProgressReporter::snapshotLocked() const
{
    const qint64 run = runningMsLocked();
    sampleLocked(run);

    ProgressSnapshot s;
    s.phase = m_stepName.isEmpty() ? m_phaseNames.value(m_phaseIndex) : m_stepName;
    s.phaseIndex = m_phaseIndex;
    s.phaseCount = int(m_phaseNames.size());
    s.phaseNames = m_phaseNames;
    s.done = doneLocked();
    s.total = m_total;
    s.filesDone = std::max<qint64>(0, m_filesDone.load(std::memory_order_relaxed));
    if (m_filesTotal > 0)
        s.filesDone = std::min(s.filesDone, m_filesTotal);
    s.filesTotal = m_filesTotal;
    s.unitIsBytes = m_unitIsBytes;
    s.indeterminate = (m_total == 0);
    s.currentItem = m_currentItem;
    if (m_started && !m_finished) {
        s.ratePerSec = m_paused ? 0.0 : m_speed.perSecond(); // paused, nothing moves; the time left waits
        if (const qint64 span = run - m_measureFromMs; span >= kSampleMs) {
            const qint64 raw = std::max<qint64>(0, m_done.load(std::memory_order_relaxed));
            s.averageRatePerSec = double(m_measuredUnits + std::max<qint64>(0, raw - m_sampleDone)) * 1000.0 / double(span);
        }
        s.etaMs = shownEta(m_phaseEtaMs, m_phaseEtaAtMs, run);
        s.jobEtaMs = shownEta(m_jobEtaMs, m_jobEtaAtMs, run);
    }

    updateHighsLocked();
    s.overallFraction = m_overallHigh;
    s.phaseProgress = m_phaseHigh;

    // Figures of other work shown for this stage (the bar, its share and the
    // time left stay this reporter's).
    if (m_shown) {
        s.total = m_shown->total;
        s.done = s.total > 0 ? std::min(m_shown->done, s.total) : m_shown->done;
        s.filesTotal = m_shown->filesTotal;
        s.filesDone = s.filesTotal > 0 ? std::min(m_shown->filesDone, s.filesTotal) : m_shown->filesDone;
        s.unitIsBytes = m_shown->unitIsBytes;
        if (m_started && !m_finished) {
            s.ratePerSec = m_paused ? 0.0 : m_shown->ratePerSec;
            s.averageRatePerSec = m_shown->averageRatePerSec;
        }
    }
    s.sequence = ++m_sequence;

    // Clocks
    s.finished = m_finished;
    s.phaseMs = m_phaseMs;
    s.phaseEntered = m_phaseEntered;
    if (m_started) {
        if (m_finished) {
            s.elapsedMs = m_totalMs;
            s.phaseElapsedMs = s.phaseMs.value(m_phaseIndex, 0);
        } else {
            const auto t = clockNowLocked();
            s.elapsedMs = msBetween(m_startedAt, t);
            s.phaseElapsedMs = msBetween(m_phaseStartedAt, t);
            if (m_phaseIndex >= 0 && m_phaseIndex < s.phaseMs.size())
                s.phaseMs[m_phaseIndex] += s.phaseElapsedMs;
        }
    }
    return s;
}

void ProgressReporter::maybeEmit(bool force)
{
    if (!m_sink)
        return;
    // The workers call this for every buffer: the throttle is checked
    // before the lock is taken.
    const qint64 t = nsOf(now());
    if (!force && t < m_nextEmitNs.load(std::memory_order_relaxed))
        return;
    ProgressSnapshot snap;
    {
        std::lock_guard lock(m_mutex);
        if (!force && t < m_nextEmitNs.load(std::memory_order_relaxed))
            return;
        m_nextEmitNs.store(t + std::chrono::duration_cast<std::chrono::nanoseconds>(m_throttle).count(), std::memory_order_relaxed);
        snap = snapshotLocked();
    }
    m_sink(snap);
}

} // namespace fw
