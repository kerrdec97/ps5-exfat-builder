#pragma once

// Watches the queue and, when it runs dry after doing work, tells the UI to
// start the "When the queue finishes" countdown (general.afterQueueAction).
// The UI shows the countdown with Cancel and calls fw::power::perform().
//
// A run is made of the user's jobs that the queue ran in this session. Jobs
// that run beside the queue (JobRecord::runsWhileHeld: update checks, problem
// reports) are not part of it, and a job restored from an earlier session
// counts only once the queue has run it (a restored queue, an interrupted job
// or a waiting job that is removed or cancelled never arms the action).
//
// "Only after this run" (general.afterQueueThisRunOnly, GitHub #96): at the
// end of a run that did something, the watcher sets the choice back to "Do
// nothing" and the switch off, whether the action is due or "Only if every
// job succeeded" kept the computer on. dueAction() still names the action of
// that run for its countdown, until the user changes the choice.

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
    // What a countdown that actionDue() started may do when it ends: the
    // action chosen now, or the action of the run that just ended when "Only
    // after this run" set the choice back to "Do nothing" (until the user
    // changes the choice or the switch, or another run ends).
    power::Action dueAction() const;

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
    void endThisRunOnly(power::Action ranFor);
    JobQueue& m_queue;
    SettingsStore& m_settings;
    int m_succeeded = 0;
    int m_failed = 0;
    int m_cancelled = 0;
    int m_cancelledAfterStart = 0; // of m_cancelled: the jobs that had started
    QStringList m_runIds;  // jobs finished since the queue was last idle
    QStringList m_lastRun;
    power::Action m_thisRunAction = power::Action::None; // see dueAction()
    bool m_resetting = false;                             // our own reset of the choice
};

} // namespace fw
