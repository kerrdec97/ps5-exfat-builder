#pragma once

// Watches a job's progress and checks what the views rely on (Progress.h):
//  * the bar stays below 100 % while the job runs and never falls back, not
//    even when a job runs other jobs;
//  * within a stage the stage's own share never falls back, and the stage
//    index never goes back;
//  * no stage stands full ("99 %") while the part of it that runs has most of
//    its work still to do (a stage entered again that found no room left);
//  * at the end of a successful job the bar has reached the end of the work,
//    so the queue's 100 % is no jump.
// Header only, so a test includes it without a change to the build lists.

#include "core/Progress.h"

#include <QString>
#include <QStringList>

#include <mutex>
#include <vector>

namespace fwtest {

class ProgressAudit {
public:
    // The reporter is read again at every report, under the audit's lock:
    // a report's own snapshot can arrive after a newer one from another
    // worker thread, so only a fresh read is in the order the work went.
    void watch(const fw::ProgressReporter* reporter)
    {
        std::lock_guard lock(m_mutex);
        m_reporter = reporter;
    }

    void report(const fw::ProgressSnapshot& reported)
    {
        std::lock_guard lock(m_mutex);
        check(m_reporter ? m_reporter->snapshot() : reported);
    }

    // The job ended; `succeeded`: it returned an outcome that the queue
    // would show as Done. A job that ends in a stage without a size has no
    // bar to fill there.
    void finish(const fw::ProgressSnapshot& last, bool succeeded)
    {
        std::lock_guard lock(m_mutex);
        check(last);
        if (succeeded && !last.indeterminate && last.overallFraction < 0.99)
            m_problems << QStringLiteral("the bar ended at %1 %, short of the end of the work")
                              .arg(last.overallFraction * 100.0, 0, 'f', 1);
    }

    QStringList problems() const
    {
        std::lock_guard lock(m_mutex);
        return m_problems;
    }
    // Every snapshot read, in order.
    std::vector<fw::ProgressSnapshot> seen() const
    {
        std::lock_guard lock(m_mutex);
        return m_seen;
    }
    // The bar when `pred` first held, or -1.
    template <typename Pred>
    double fractionWhen(Pred pred) const
    {
        std::lock_guard lock(m_mutex);
        for (const fw::ProgressSnapshot& s : m_seen)
            if (pred(s))
                return s.overallFraction;
        return -1.0;
    }

private:
    void problem(const QString& what, const fw::ProgressSnapshot& s)
    {
        if (m_problems.size() < 20)
            m_problems << QStringLiteral("%1 (stage %2 \"%3\", %4 of %5, bar %6 %, stage %7 %)")
                              .arg(what)
                              .arg(s.phaseIndex)
                              .arg(s.phase)
                              .arg(s.done)
                              .arg(s.total)
                              .arg(s.overallFraction * 100.0, 0, 'f', 1)
                              .arg(s.phaseProgress * 100.0, 0, 'f', 1);
    }

    void check(const fw::ProgressSnapshot& s)
    {
        if (!(s.overallFraction >= 0.0 && s.overallFraction < 1.0))
            problem(QStringLiteral("the bar is out of 0..99.9 %"), s);
        if (!m_seen.empty()) {
            const fw::ProgressSnapshot& p = m_seen.back();
            if (s.overallFraction < p.overallFraction)
                problem(QStringLiteral("the bar fell back from %1 %").arg(p.overallFraction * 100.0, 0, 'f', 1), s);
            if (s.phaseNames == p.phaseNames) {
                if (s.phaseIndex < p.phaseIndex)
                    problem(QStringLiteral("the stage went back from %1").arg(p.phaseIndex), s);
                else if (s.phaseIndex == p.phaseIndex && s.phaseProgress < p.phaseProgress)
                    problem(QStringLiteral("the stage fell back from %1 %").arg(p.phaseProgress * 100.0, 0, 'f', 1), s);
            }
        }
        if (s.total > 0 && s.done * 2 < s.total && s.phaseProgress >= 0.99)
            problem(QStringLiteral("the stage stands full with most of its work to do"), s);
        m_seen.push_back(s);
    }

    mutable std::mutex m_mutex;
    const fw::ProgressReporter* m_reporter = nullptr;
    std::vector<fw::ProgressSnapshot> m_seen;
    QStringList m_problems;
};

} // namespace fwtest
