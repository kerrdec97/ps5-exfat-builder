#include "jobs/QueueCompletionWatcher.h"

namespace fw {

QueueCompletionWatcher::QueueCompletionWatcher(JobQueue& queue, SettingsStore& settings, QObject* parent)
    : QObject(parent), m_queue(queue), m_settings(settings)
{
    connect(&m_queue, &JobQueue::jobFinished, this, [this](const QString& id) {
        const auto rec = m_queue.record(id);
        if (!rec)
            return;
        switch (rec->state) {
        case JobState::Succeeded: ++m_succeeded; break;
        case JobState::Cancelled: ++m_cancelled; break;
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
    const int ok = m_succeeded, failed = m_failed, cancelled = m_cancelled;
    m_succeeded = m_failed = m_cancelled = 0;
    emit queueFinished(ok, failed, cancelled);

    const SettingsValues snap = m_settings.snapshot();
    const auto action = power::actionFromName(snap.string(QStringLiteral("general.afterQueueAction")));
    if (action == power::Action::None)
        return;
    if (snap.boolean(QStringLiteral("general.afterQueueOnlyIfAllSucceeded")) && (failed > 0 || cancelled > 0))
        return;
    if (ok == 0)
        return;
    emit actionDue(action, int(snap.integer(QStringLiteral("general.afterQueueDelaySeconds"))), ok, failed);
}

} // namespace fw
