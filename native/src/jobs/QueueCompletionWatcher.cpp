#include "jobs/QueueCompletionWatcher.h"

namespace fw {

QueueCompletionWatcher::QueueCompletionWatcher(JobQueue& queue, SettingsStore& settings, QObject* parent)
    : QObject(parent), m_queue(queue), m_settings(settings)
{
    connect(&m_queue, &JobQueue::jobFinished, this, [this](const QString& id) {
        const auto rec = m_queue.record(id);
        if (!rec)
            return;
        if (!m_runIds.contains(id))
            m_runIds << id;
        switch (rec->state) {
        case JobState::Succeeded: ++m_succeeded; break;
        case JobState::Cancelled:
            ++m_cancelled;
            if (rec->started.isValid())
                ++m_cancelledAfterStart; // it had begun: for the after-queue action that is not a clean run
            break;
        case JobState::Failed:
        case JobState::Interrupted: ++m_failed; break;
        default: break;
        }
    });
    connect(&m_queue, &JobQueue::idle, this, &QueueCompletionWatcher::onIdle);
}

void QueueCompletionWatcher::onIdle()
{
    const int total = m_succeeded + m_failed + m_cancelled;
    if (total == 0)
        return;
    const int ok = m_succeeded, failed = m_failed, cancelled = m_cancelled, cancelledAfterStart = m_cancelledAfterStart;
    m_succeeded = m_failed = m_cancelled = m_cancelledAfterStart = 0;
    m_lastRun = m_runIds;
    m_runIds.clear();
    // A run the user cancelled in full did nothing worth a notification or a
    // shutdown; the counters are still reset so the next run starts clean.
    if (ok + failed == 0)
        return;
    emit queueFinished(ok, failed, cancelled);

    const SettingsValues snap = m_settings.snapshot();
    const auto action = power::actionFromName(snap.string(QStringLiteral("general.afterQueueAction")));
    if (action == power::Action::None)
        return;
    // "Only if every job succeeded": a job that failed, or was cancelled after
    // it had started, keeps the computer on. One cancelled while it was still
    // waiting never ran and does not count. With the option off the action
    // runs whatever the jobs did, also when every one of them failed.
    if (snap.boolean(QStringLiteral("general.afterQueueOnlyIfAllSucceeded")) && (failed > 0 || cancelledAfterStart > 0))
        return;
    emit actionDue(action, int(snap.integer(QStringLiteral("general.afterQueueDelaySeconds"))), ok, failed);
}

} // namespace fw
