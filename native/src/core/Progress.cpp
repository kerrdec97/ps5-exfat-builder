#include "core/Progress.h"

#include <QStringList>

#include <algorithm>

namespace fw {

using Clock = std::chrono::steady_clock;

ProgressReporter::ProgressReporter(Sink sink, int throttleMs)
    : m_sink(std::move(sink)), m_throttle(throttleMs)
{
    m_phaseNames = {QStringLiteral("Working")};
    m_weights = {1.0};
}

void ProgressReporter::setPhases(const QStringList& names, const QList<double>& weights)
{
    std::lock_guard lock(m_mutex);
    m_phaseNames = names.isEmpty() ? QStringList{QStringLiteral("Working")} : names;
    m_weights.clear();
    for (int i = 0; i < m_phaseNames.size(); ++i)
        m_weights.push_back(i < weights.size() && weights[i] > 0 ? weights[i] : 1.0);
}

void ProgressReporter::beginPhase(int index, qint64 total, qint64 filesTotal, bool unitIsBytes)
{
    {
        std::lock_guard lock(m_mutex);
        m_phaseIndex = std::clamp(index, 0, int(m_phaseNames.size()) - 1);
        m_total = std::max<qint64>(0, total);
        m_filesTotal = std::max<qint64>(0, filesTotal);
        m_unitIsBytes = unitIsBytes;
        m_currentItem.clear();
        m_done.store(0);
        m_filesDone.store(0);
        m_rate = 0.0;
        m_rateT0 = Clock::now();
        m_rateDone0 = 0;
    }
    flush();
}

void ProgressReporter::begin(const QString& phase, qint64 total, qint64 filesTotal, bool unitIsBytes)
{
    {
        std::lock_guard lock(m_mutex);
        const int idx = m_phaseNames.indexOf(phase);
        if (idx < 0) {
            m_phaseNames = {phase};
            m_weights = {1.0};
        }
    }
    const int idx = [&] {
        std::lock_guard lock(m_mutex);
        return int(m_phaseNames.indexOf(phase));
    }();
    beginPhase(std::max(0, idx), total, filesTotal, unitIsBytes);
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

void ProgressReporter::setTotal(qint64 total)
{
    {
        std::lock_guard lock(m_mutex);
        m_total = std::max<qint64>(0, total);
    }
    maybeEmit(false);
}

void ProgressReporter::flush()
{
    maybeEmit(true);
}

ProgressSnapshot ProgressReporter::snapshot() const
{
    std::lock_guard lock(m_mutex);
    ProgressSnapshot s;
    s.phase = m_phaseNames.value(m_phaseIndex);
    s.phaseIndex = m_phaseIndex;
    s.phaseCount = int(m_phaseNames.size());
    s.done = m_done.load(std::memory_order_relaxed);
    s.total = m_total;
    if (s.total > 0)
        s.done = std::min(s.done, s.total);
    s.filesDone = m_filesDone.load(std::memory_order_relaxed);
    s.filesTotal = m_filesTotal;
    s.unitIsBytes = m_unitIsBytes;
    s.indeterminate = (m_total == 0);
    s.ratePerSec = m_rate;
    if (m_rate > 1.0 && s.total > 0)
        s.etaMs = qint64(double(s.total - s.done) / m_rate * 1000.0);
    s.currentItem = m_currentItem;

    double sumW = 0, before = 0;
    for (int i = 0; i < m_weights.size(); ++i) {
        sumW += m_weights[i];
        if (i < m_phaseIndex)
            before += m_weights[i];
    }
    const double cur = m_weights.value(m_phaseIndex, 1.0) * (s.indeterminate ? 0.0 : s.phaseFraction());
    s.overallFraction = sumW > 0 ? std::clamp((before + cur) / sumW, 0.0, 1.0) : 0.0;
    return s;
}

void ProgressReporter::maybeEmit(bool force)
{
    if (!m_sink)
        return;
    const auto now = Clock::now();
    {
        std::lock_guard lock(m_mutex);
        if (!force && now - m_lastEmit < m_throttle)
            return;
        m_lastEmit = now;
        // Exponential moving average of the rate over ~0.5 s windows.
        const auto dt = std::chrono::duration<double>(now - m_rateT0).count();
        if (dt >= 0.5) {
            const qint64 d = m_done.load(std::memory_order_relaxed);
            const double inst = double(d - m_rateDone0) / dt;
            m_rate = (m_rate <= 0.0) ? inst : (0.7 * m_rate + 0.3 * inst);
            m_rateT0 = now;
            m_rateDone0 = d;
        }
    }
    m_sink(snapshot());
}

} // namespace fw
