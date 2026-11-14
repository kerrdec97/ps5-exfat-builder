#pragma once

// The integrated build queue.
//
// Lives on the UI thread; runs each job on its own worker thread (dedicated
// pool, `performance.maxConcurrentJobs` at a time). Progress is coalesced so a
// fast engine cannot flood the event loop. Pending jobs are persisted to
// queue.json; a job that was running when the app closed is restored as
// Interrupted so the user can retry it. Finished jobs are appended to the
// history file.

#include "core/Settings.h"
#include "jobs/Job.h"

#include <QDateTime>
#include <QJsonArray>
#include <QObject>
#include <QThreadPool>

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace fw {

struct JobRecord {
    QString id;
    QString kind;
    QString title;
    QString subtitle;
    QStringList phases;
    JobState state = JobState::Queued;
    ProgressSnapshot progress;
    QDateTime added, started, finished;
    std::optional<JobOutcome> outcome;
    std::optional<Error> error;
    QJsonObject parameters;
    QVariantMap overrides;
    int attempt = 1;          // automatic retries increase this (general.autoRetryCount)
    QDateTime notBefore;      // a retry waits until then (general.autoRetryDelaySeconds)
    bool runsWhileHeld = false; // Job::runsWhileHeld(): a system job (update check, problem report)

    bool isActive() const { return fw::isActive(state); }
    bool isTerminal() const { return fw::isTerminal(state); }
    // Queued after an automatic retry, until notBefore has passed.
    bool isWaitingToRetry() const { return state == JobState::Queued && notBefore.isValid() && notBefore > QDateTime::currentDateTime(); }
};

class JobHistory;

class JobQueue : public QObject {
    Q_OBJECT
public:
    JobQueue(SettingsStore& settings, const JobFactory& factory, QObject* parent = nullptr);
    ~JobQueue() override;

    // Validates and enqueues. Returns the job id.
    Result<QString> enqueue(std::unique_ptr<Job> job);
    Result<QString> enqueue(const QString& kind, const QJsonObject& params, const QVariantMap& overrides = {});

    void setHistory(JobHistory* history) { m_history = history; }
    void setPersistencePath(const QString& path) { m_persistPath = path; }

    // Queue-level hold: running jobs continue, no new job starts.
    void holdQueue(bool hold);
    bool isHeld() const { return m_held; }

    void pauseJob(const QString& id);
    void resumeJob(const QString& id);
    // Runs this queued job next (ahead of the other waiting jobs, and even
    // while the queue is paused) without resuming the rest of the queue:
    // "Build now", Retry.
    void startNow(const QString& id);
    // Several at once, in this order ("Build 3 images", a batch of
    // transfers): the first may start at once, the others follow it in turn.
    void startNow(const QStringList& ids);
    void cancel(const QString& id);
    void cancelAll();
    Result<QString> retry(const QString& id); // new job from the same parameters
    bool remove(const QString& id);           // not while running
    bool move(const QString& id, int newIndex);
    void clearFinished();

    std::vector<JobRecord> records() const;
    std::optional<JobRecord> record(const QString& id) const;
    int runningCount() const;
    int pendingCount() const;
    bool isBusy() const { return runningCount() > 0; }
    // True when a job that is running, paused or being cancelled reads or
    // writes `path`, a folder holding it or something inside it, by its
    // busyPaths() (busyPathsOverlap): a page does not rename what a job has open.
    // A file already in the folder a job saves into (its "output" or
    // "destination" parameter) is not the job's, so it is not in use.
    bool isPathInUse(const QString& path) const;

    // Persists pending jobs; restores them (and marks interrupted ones). A
    // saved job this build has no job type for is not shown; it stays in the
    // file for the build that knows it. A file that cannot be read is set
    // aside as "<name>.damaged" (or "<name> (2).damaged"...: one set aside
    // earlier is never replaced) and reported as the error.
    Status saveQueue() const;
    Status restoreQueue();

    // Cancels everything and waits up to `msecs` for workers to stop.
    bool shutdown(int msecs = 15000);

signals:
    void jobAdded(const QString& id);
    void jobChanged(const QString& id);
    void jobFinished(const QString& id);
    void jobRemoved(const QString& id);
    void orderChanged();
    void heldChanged(bool held); // holdQueue(), restoreQueue()
    void idle();

private:
    struct Entry;
    void schedule();
    void startEntry(const std::shared_ptr<Entry>& e);
    void onWorkerProgress(const QString& id);
    void onWorkerFinished(const QString& id, Result<JobOutcome> result);
    std::shared_ptr<Entry> find(const QString& id) const;
    QString newId();
    bool tryAutoRetry(const std::shared_ptr<Entry>& e, const Error& error);

    SettingsStore& m_settings;
    const JobFactory& m_factory;
    JobHistory* m_history = nullptr;
    QString m_persistPath;
    QThreadPool m_pool;
    mutable std::mutex m_mutex;
    std::vector<std::shared_ptr<Entry>> m_entries;
    QJsonArray m_foreignJobs;     // saved jobs of another build, written back unchanged (m_mutex)
    bool m_restoring = false;     // restoreQueue() is running: saveQueue() waits for its end
    bool m_held = false;
    bool m_shuttingDown = false;
    quint64 m_counter = 0;
};

} // namespace fw
