#pragma once

// Watches the queue and, when it runs dry after doing work, tells the UI to
// start the "When the queue finishes" countdown (general.afterQueueAction).
// The UI shows the countdown with Cancel and calls fw::power::perform().

#include "core/Power.h"
#include "core/Settings.h"
#include "jobs/JobQueue.h"

#include <QObject>

namespace fw {

class QueueCompletionWatcher : public QObject {
    Q_OBJECT
public:
    QueueCompletionWatcher(JobQueue& queue, SettingsStore& settings, QObject* parent = nullptr);

    int succeededSinceIdle() const { return m_succeeded; }
    int failedSinceIdle() const { return m_failed; }
    // Ids of the jobs of the run that queueFinished() reported last, each once,
    // in the order they finished (notifications list them). Valid until the
    // queue finishes again.
    QStringList lastRunJobIds() const { return m_lastRun; }

signals:
    // The queue finished; `action` should run after `delaySeconds` unless the
    // user cancels. Not emitted for a run in which every job was cancelled,
    // nor, with "Only if every job succeeded" on, when a job failed or was
    // cancelled after it had started.
    void actionDue(fw::power::Action action, int delaySeconds, int succeeded, int failed);
    // The queue finished (always emitted, for toasts/notifications).
    void queueFinished(int succeeded, int failed, int cancelled);

private:
    void onIdle();
    JobQueue& m_queue;
    SettingsStore& m_settings;
    int m_succeeded = 0;
    int m_failed = 0;
    int m_cancelled = 0;
    int m_cancelledAfterStart = 0; // of m_cancelled: the jobs that had started
    QStringList m_runIds;  // jobs finished since the queue was last idle
    QStringList m_lastRun;
};

} // namespace fw
