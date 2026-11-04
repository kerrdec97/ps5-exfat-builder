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

signals:
    // The queue finished; `action` should run after `delaySeconds` unless the
    // user cancels.
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
};

} // namespace fw
