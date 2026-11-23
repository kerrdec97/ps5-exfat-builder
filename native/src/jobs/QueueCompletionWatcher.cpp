#include "jobs/QueueCompletionWatcher.h"

#include "core/Log.h"

namespace {
const QString kAction = QStringLiteral("general.afterQueueAction");
const QString kThisRunOnly = QStringLiteral("general.afterQueueThisRunOnly");
} // namespace

namespace fw {

QueueCompletionWatcher::QueueCompletionWatcher(JobQueue& queue, SettingsStore& settings, QObject* parent)
    : QObject(parent), m_queue(queue), m_settings(settings)
{
    connect(&m_queue, &JobQueue::jobFinished, this, [this](const QString& id) {
        const auto rec = m_queue.record(id);
        if (!rec)
            return;
        // Update checks and problem reports run beside the queue (even while
        // it is paused): they are not part of the user's run, and one of
        // them ending alone ends no run (GitHub #96: the check a few seconds
        // after start started the countdown at every start).
        if (rec->runsWhileHeld)
            return;
        // Ran in this session: the queue started it (a restored job never
        // finishes by itself), now or in an attempt before an automatic retry.
        const bool ran = rec->started.isValid() || rec->attempt > 1;
        switch (rec->state) {
        case JobState::Succeeded:
            if (!ran)
                return;
            ++m_succeeded;
            break;
        case JobState::Cancelled:
            ++m_cancelled;
            if (ran)
                ++m_cancelledAfterStart; // it had begun: for the after-queue action that is not a clean run
            break;
        case JobState::Failed:
        case JobState::Interrupted:
            if (!ran)
                return;
            ++m_failed;
            break;
        default: return;
        }
        if (!m_runIds.contains(id))
            m_runIds << id;
    });
    connect(&m_queue, &JobQueue::idle, this, &QueueCompletionWatcher::onIdle);
    // A choice the user makes after a "this run only" action was due wins
    // over it, also during that run's countdown.
    connect(&m_settings, &SettingsStore::changed, this, [this](const QString& key, const QVariant&) {
        if (!m_resetting && (key == kAction || key == kThisRunOnly))
            m_thisRunAction = power::Action::None;
    });
    connect(&m_settings, &SettingsStore::resetDone, this, [this] {
        if (!m_resetting)
            m_thisRunAction = power::Action::None;
    });
}

power::Action QueueCompletionWatcher::dueAction() const
{
    if (m_thisRunAction != power::Action::None)
        return m_thisRunAction;
    return power::actionFromName(m_settings.value(kAction).toString());
}

void QueueCompletionWatcher::endThisRunOnly(power::Action ranFor)
{
    m_resetting = true;
    const Status a = m_settings.setValue(kAction, QStringLiteral("none"));
    const Status b = m_settings.setValue(kThisRunOnly, false);
    m_resetting = false;
    if (!a.ok() || !b.ok()) {
        Log(QStringLiteral("power")).warn(QStringLiteral("Could not set \"When the queue finishes\" back to \"Do nothing\": %1")
                                              .arg((a.ok() ? b : a).error().message));
        return;
    }
    Log(QStringLiteral("power")).info(QStringLiteral("After-queue action was for this run only (%1): set back to \"Do nothing\"")
                                          .arg(power::actionLabel(ranFor)));
    // (The app saves the settings on every change, before a sleep or shut down.)
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

    m_thisRunAction = power::Action::None; // another run ended: the earlier run's action is over
    const SettingsValues snap = m_settings.snapshot();
    const auto action = power::actionFromName(snap.string(kAction));
    if (action == power::Action::None)
        return;
    const bool thisRunOnly = snap.boolean(kThisRunOnly);
    // "Only if every job succeeded": a job that failed, or was cancelled after
    // it had started, keeps the computer on. One cancelled while it was still
    // waiting never ran and does not count. With the option off the action
    // runs whatever the jobs did, also when every one of them failed.
    if (snap.boolean(QStringLiteral("general.afterQueueOnlyIfAllSucceeded")) && (failed > 0 || cancelledAfterStart > 0)) {
        if (thisRunOnly)
            endThisRunOnly(action); // the run is over: Sleep is not left armed for the next one
        return;
    }
    if (thisRunOnly) {
        endThisRunOnly(action);
        m_thisRunAction = action; // for this run's countdown (MainWindow asks dueAction())
    }
    emit actionDue(action, int(snap.integer(QStringLiteral("general.afterQueueDelaySeconds"))), ok, failed);
}

} // namespace fw
