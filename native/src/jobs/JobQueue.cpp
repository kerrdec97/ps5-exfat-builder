#include "jobs/JobQueue.h"

#include "core/FileUtil.h"
#include "core/Paths.h"
#include "core/Power.h"
#include "jobs/JobHistory.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QRunnable>
#include <QTimer>

#include <algorithm>
#include <exception>

namespace fw {

struct JobQueue::Entry {
    JobRecord rec;                     // guarded by JobQueue::m_mutex
    std::unique_ptr<Job> job;          // owned until the worker takes it
    CancellationSource cancel;
    std::atomic<bool> progressPending{false};
    std::unique_ptr<ProgressReporter> reporter;
    QStringList busy;                  // busyPaths() taken when the job starts
    bool startDespiteHold = false;     // startNow(): may start while the queue is held
};

namespace {
QJsonObject variantMapToJson(const QVariantMap& m) { return QJsonObject::fromVariantMap(m); }
} // namespace

JobQueue::JobQueue(SettingsStore& settings, const JobFactory& factory, QObject* parent)
    : QObject(parent), m_settings(settings), m_factory(factory)
{
    qRegisterMetaType<ProgressSnapshot>();
    qRegisterMetaType<JobState>();
    m_pool.setMaxThreadCount(16);
    m_pool.setExpiryTimeout(30000);
}

JobQueue::~JobQueue()
{
    shutdown(15000);
    // Workers hold raw pointers into m_entries (their progress reporters), so
    // they must all be gone before the members are destroyed. Cancellation has
    // been requested and tool processes killed; this returns promptly.
    m_pool.waitForDone(-1);
}

QString JobQueue::newId()
{
    return QStringLiteral("J%1-%2")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("HHmmss")))
        .arg(++m_counter, 3, 10, QLatin1Char('0'));
}

Result<QString> JobQueue::enqueue(std::unique_ptr<Job> job)
{
    if (!job)
        return Error(ErrorCode::InvalidArgument, QStringLiteral("No job to add."));
    if (m_shuttingDown)
        return Error(ErrorCode::Unsupported, QStringLiteral("The app is closing."));

    auto overrides = m_settings.normalizeOverrides(job->settingsOverrides);
    if (!overrides.ok())
        return overrides.takeError();
    job->settingsOverrides = overrides.value();
    FW_TRY(job->validate(m_settings.snapshot(job->settingsOverrides)));

    auto e = std::make_shared<Entry>();
    e->rec.id = newId();
    e->rec.kind = job->kind();
    e->rec.title = job->title();
    e->rec.subtitle = job->subtitle();
    e->rec.phases = job->phases();
    e->rec.parameters = job->parameters();
    e->rec.overrides = job->settingsOverrides;
    e->rec.added = QDateTime::currentDateTime();
    e->rec.progress.phase = QStringLiteral("Waiting");
    e->rec.progress.indeterminate = true;
    e->job = std::move(job);
    const QString id = e->rec.id;
    {
        std::lock_guard lock(m_mutex);
        m_entries.push_back(e);
    }
    Log(QStringLiteral("queue"), id).info(QStringLiteral("Queued: %1").arg(e->rec.title),
                                          QJsonObject{{QStringLiteral("kind"), e->rec.kind}});
    emit jobAdded(id);
    (void)saveQueue();
    schedule();
    return id;
}

Result<QString> JobQueue::enqueue(const QString& kind, const QJsonObject& params, const QVariantMap& overrides)
{
    auto job = m_factory.create(kind, params, overrides);
    if (!job.ok())
        return job.takeError();
    return enqueue(std::move(job).value());
}

void JobQueue::holdQueue(bool hold)
{
    if (m_held == hold)
        return;
    m_held = hold;
    emit heldChanged(hold);
    if (!hold)
        schedule();
}

std::shared_ptr<JobQueue::Entry> JobQueue::find(const QString& id) const
{
    std::lock_guard lock(m_mutex);
    for (const auto& e : m_entries)
        if (e->rec.id == id)
            return e;
    return nullptr;
}

void JobQueue::pauseJob(const QString& id)
{
    auto e = find(id);
    if (!e)
        return;
    {
        std::lock_guard lock(m_mutex);
        if (e->rec.state != JobState::Running)
            return;
        e->rec.state = JobState::Paused;
    }
    e->cancel.pause();
    emit jobChanged(id);
}

void JobQueue::resumeJob(const QString& id)
{
    auto e = find(id);
    if (!e)
        return;
    {
        std::lock_guard lock(m_mutex);
        if (e->rec.state != JobState::Paused)
            return;
        e->rec.state = JobState::Running;
    }
    e->cancel.resume();
    emit jobChanged(id);
}

void JobQueue::startNow(const QString& id)
{
    auto e = find(id);
    if (!e)
        return;
    {
        std::lock_guard lock(m_mutex);
        if (e->rec.state != JobState::Queued)
            return;
        e->startDespiteHold = true;
        e->rec.notBefore = QDateTime();
        // Ahead of every other waiting job, behind the running and finished ones.
        auto it = std::find(m_entries.begin(), m_entries.end(), e);
        if (it != m_entries.end())
            m_entries.erase(it);
        auto firstQueued = std::find_if(m_entries.begin(), m_entries.end(), [](const auto& x) { return x->rec.state == JobState::Queued; });
        m_entries.insert(firstQueued, e);
    }
    emit orderChanged();
    (void)saveQueue();
    schedule();
}

void JobQueue::cancel(const QString& id)
{
    auto e = find(id);
    if (!e)
        return;
    bool wasQueued = false;
    {
        std::lock_guard lock(m_mutex);
        if (isTerminal(e->rec.state) || e->rec.state == JobState::Cancelling)
            return;
        if (e->rec.state == JobState::Queued) {
            wasQueued = true;
            e->rec.state = JobState::Cancelled;
            e->rec.finished = QDateTime::currentDateTime();
            e->rec.error = Error::cancelled();
        } else {
            // The worker winds down (it checks the token at its next checkpoint
            // and removes its partial output); Cancelled follows from it.
            e->rec.state = JobState::Cancelling;
        }
    }
    Log(QStringLiteral("queue"), id).info(QStringLiteral("Cancel requested"));
    e->cancel.cancel(QStringLiteral("user"));
    e->cancel.resume();
    emit jobChanged(id);
    if (wasQueued) {
        e->job.reset();
        if (m_history) {
            JobRecord snapshot;
            {
                std::lock_guard lock(m_mutex);
                snapshot = e->rec;
            }
            (void)m_history->append(snapshot); // every ending is in the history
        }
        emit jobFinished(id);
        (void)saveQueue();
        schedule();
    }
}

void JobQueue::cancelAll()
{
    for (const auto& r : records())
        if (!isTerminal(r.state))
            cancel(r.id);
}

Result<QString> JobQueue::retry(const QString& id)
{
    auto rec = record(id);
    if (!rec)
        return Error(ErrorCode::NotFound, QStringLiteral("That job is no longer in the queue."));
    if (!isTerminal(rec->state))
        return Error(ErrorCode::InvalidArgument, QStringLiteral("Only finished jobs can be retried."));
    return enqueue(rec->kind, rec->parameters, rec->overrides);
}

bool JobQueue::remove(const QString& id)
{
    std::shared_ptr<Entry> removed;
    {
        std::lock_guard lock(m_mutex);
        auto it = std::find_if(m_entries.begin(), m_entries.end(), [&](const auto& e) { return e->rec.id == id; });
        if (it == m_entries.end())
            return false;
        if ((*it)->rec.isActive())
            return false;
        removed = *it;
        m_entries.erase(it);
    }
    emit jobRemoved(id);
    (void)saveQueue();
    // The last waiting job is gone: the queue may have just run dry, and only
    // schedule() tells anybody (idle()).
    if (removed->rec.state == JobState::Queued)
        schedule();
    return true;
}

bool JobQueue::move(const QString& id, int newIndex)
{
    {
        std::lock_guard lock(m_mutex);
        auto it = std::find_if(m_entries.begin(), m_entries.end(), [&](const auto& e) { return e->rec.id == id; });
        if (it == m_entries.end())
            return false;
        auto e = *it;
        m_entries.erase(it);
        newIndex = std::clamp(newIndex, 0, int(m_entries.size()));
        m_entries.insert(m_entries.begin() + newIndex, e);
    }
    emit orderChanged();
    (void)saveQueue();
    return true;
}

void JobQueue::clearFinished()
{
    QStringList ids;
    {
        std::lock_guard lock(m_mutex);
        for (auto it = m_entries.begin(); it != m_entries.end();) {
            if (isTerminal((*it)->rec.state)) {
                ids << (*it)->rec.id;
                it = m_entries.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (const auto& id : ids)
        emit jobRemoved(id);
    (void)saveQueue();
}

std::vector<JobRecord> JobQueue::records() const
{
    std::lock_guard lock(m_mutex);
    std::vector<JobRecord> out;
    out.reserve(m_entries.size());
    for (const auto& e : m_entries) {
        JobRecord r = e->rec;
        if (e->reporter && r.isActive())
            r.progress = e->reporter->snapshot();
        out.push_back(std::move(r));
    }
    return out;
}

std::optional<JobRecord> JobQueue::record(const QString& id) const
{
    std::lock_guard lock(m_mutex);
    for (const auto& e : m_entries)
        if (e->rec.id == id) {
            JobRecord r = e->rec;
            if (e->reporter && r.isActive())
                r.progress = e->reporter->snapshot();
            return r;
        }
    return std::nullopt;
}

int JobQueue::runningCount() const
{
    std::lock_guard lock(m_mutex);
    return int(std::count_if(m_entries.begin(), m_entries.end(), [](const auto& e) { return e->rec.isActive(); }));
}

int JobQueue::pendingCount() const
{
    std::lock_guard lock(m_mutex);
    return int(std::count_if(m_entries.begin(), m_entries.end(),
                             [](const auto& e) { return e->rec.state == JobState::Queued; }));
}

void JobQueue::schedule()
{
    if (m_shuttingDown)
        return;
    const int maxConcurrent = std::max<int>(1, int(m_settings.snapshot().integer(QStringLiteral("performance.maxConcurrentJobs"))));
    std::vector<std::shared_ptr<Entry>> toStart;
    {
        std::lock_guard lock(m_mutex);
        int running = 0;
        QList<QStringList> busy; // paths in use by running jobs
        for (const auto& e : m_entries)
            if (e->rec.isActive()) {
                ++running;
                busy << e->busy;
            }
        for (const auto& e : m_entries) {
            if (running >= maxConcurrent)
                break;
            if (e->rec.state == JobState::Queued && e->job
                && (!e->rec.notBefore.isValid() || e->rec.notBefore <= QDateTime::currentDateTime())) {
                // A paused queue holds the user's jobs; system jobs and a job
                // the user started by hand (startNow) go through.
                if (m_held && !e->startDespiteHold && !e->job->runsWhileHeld())
                    continue;
                // Never two jobs on the same image or game at once (a verify
                // must not roll back an edit that is still being applied).
                const QStringList paths = e->job->busyPaths();
                if (std::any_of(busy.begin(), busy.end(), [&](const QStringList& b) { return busyPathsOverlap(paths, b); }))
                    continue;
                e->busy = paths;
                busy << paths;
                e->rec.state = JobState::Running;
                e->rec.started = QDateTime::currentDateTime();
                e->rec.notBefore = QDateTime(); // the retry wait is over
                toStart.push_back(e);
                ++running;
            }
        }
    }
    for (const auto& e : toStart)
        startEntry(e);
    const bool busy = runningCount() > 0;
    power::setKeepAwake(busy && m_settings.snapshot().boolean(QStringLiteral("performance.keepComputerAwake")));
    if (toStart.empty() && !busy && pendingCount() == 0)
        emit idle();
    // A delayed retry is waiting: come back when it is due.
    QDateTime nextDue;
    {
        std::lock_guard lock(m_mutex);
        for (const auto& e : m_entries)
            if (e->rec.state == JobState::Queued && e->rec.notBefore.isValid()
                && e->rec.notBefore > QDateTime::currentDateTime()
                && (!nextDue.isValid() || e->rec.notBefore < nextDue))
                nextDue = e->rec.notBefore;
    }
    if (nextDue.isValid())
        QTimer::singleShot(int(std::max<qint64>(50, QDateTime::currentDateTime().msecsTo(nextDue) + 20)), this,
                           [this] { schedule(); });
}

void JobQueue::startEntry(const std::shared_ptr<Entry>& e)
{
    const QString id = e->rec.id;
    QPointer<JobQueue> self(this);

    std::weak_ptr<Entry> weak = e; // the entry owns the reporter: no cycle
    e->reporter = std::make_unique<ProgressReporter>(
        [self, weak, id](const ProgressSnapshot&) {
            auto entry = weak.lock();
            // Coalesce: post at most one pending UI update per job.
            if (!entry || entry->progressPending.exchange(true))
                return;
            if (self)
                QMetaObject::invokeMethod(self.data(), [self, id] {
                    if (self)
                        self->onWorkerProgress(id);
                }, Qt::QueuedConnection);
        },
        100);
    e->reporter->setPhases(e->rec.phases, e->job ? e->job->phaseWeights() : QList<double>{});

    std::unique_ptr<Job> job = std::move(e->job);
    SettingsValues settings = m_settings.snapshot(e->rec.overrides);
    CancellationToken token = e->cancel.token();
    ProgressReporter* reporter = e->reporter.get();
    const QString title = e->rec.title;

    emit jobChanged(id);
    Log(QStringLiteral("queue"), id).info(QStringLiteral("Started: %1").arg(title));

    m_pool.start([self, id, job = std::shared_ptr<Job>(std::move(job)), settings, token, reporter]() mutable {
        Result<JobOutcome> result = Error(ErrorCode::Internal, QStringLiteral("The job did not run."));
        {
            const QString tempParent = settings.string(QStringLiteral("storage.tempFolder")).trimmed();
            TempDir temp(QStringLiteral("job-") + id,
                         (!tempParent.isEmpty() && QFileInfo(tempParent).isDir()) ? tempParent : QString());
            JobContext ctx;
            ctx.jobId = id;
            ctx.token = token;
            ctx.progress = reporter;
            ctx.log = Log(job->kind(), id);
            ctx.settings = settings;
            ctx.tempDir = temp.path();
            try {
                if (!temp.isValid())
                    result = Error(ErrorCode::IoError, QStringLiteral("Could not create a temporary folder for this job."));
                else
                    result = job->run(ctx);
            } catch (const std::exception& ex) {
                result = Error(ErrorCode::Internal, QStringLiteral("The job stopped because of an internal error."),
                               QString::fromUtf8(ex.what()));
            } catch (...) {
                result = Error(ErrorCode::Internal, QStringLiteral("The job stopped because of an internal error."));
            }
            if (!result.ok() && token.isCancelled() && !result.error().isCancelled())
                result = Error::cancelled();
            if (settings.boolean(QStringLiteral("advanced.keepJobTempFolders")))
                temp.keep();
        }
        if (self)
            QMetaObject::invokeMethod(self.data(), [self, id, result = std::move(result)]() mutable {
                if (self)
                    self->onWorkerFinished(id, std::move(result));
            }, Qt::QueuedConnection);
    });
}

void JobQueue::onWorkerProgress(const QString& id)
{
    auto e = find(id);
    if (!e)
        return;
    e->progressPending.store(false);
    emit jobChanged(id);
}

bool JobQueue::tryAutoRetry(const std::shared_ptr<Entry>& e, const Error& error)
{
    if (m_shuttingDown)
        return false;
    switch (error.code) {
    case ErrorCode::IoError:
    case ErrorCode::ExternalToolFailed:
    case ErrorCode::Timeout:
        break;
    default:
        return false; // cancelled, invalid input, missing tool, full disk, verification...
    }
    const SettingsValues snap = m_settings.snapshot();
    const int maxRetries = int(snap.integer(QStringLiteral("general.autoRetryCount")));
    if (e->rec.attempt > maxRetries)
        return false;
    auto job = m_factory.create(e->rec.kind, e->rec.parameters, e->rec.overrides);
    if (!job.ok())
        return false;
    const int delay = int(snap.integer(QStringLiteral("general.autoRetryDelaySeconds")));
    {
        std::lock_guard lock(m_mutex);
        e->job = std::move(job).value();
        e->cancel = CancellationSource();
        e->rec.attempt += 1;
        e->rec.state = JobState::Queued;
        e->rec.error.reset();
        e->rec.outcome.reset();
        e->rec.started = QDateTime();
        e->rec.finished = QDateTime();
        e->rec.notBefore = QDateTime::currentDateTime().addSecs(delay);
        e->rec.progress = ProgressSnapshot();
        e->rec.progress.phase = QStringLiteral("Waiting to retry");
        e->rec.progress.indeterminate = true;
    }
    Log(QStringLiteral("queue"), e->rec.id)
        .warn(QStringLiteral("Failed (%1); retrying in %2 s, attempt %3 of %4")
                  .arg(error.message).arg(delay).arg(e->rec.attempt).arg(maxRetries + 1));
    return true;
}

void JobQueue::onWorkerFinished(const QString& id, Result<JobOutcome> result)
{
    auto e = find(id);
    if (!e)
        return;
    // Stop the job's clocks before the final snapshot is taken (only this
    // thread creates and resets the reporter; the reporter itself is thread-safe).
    if (e->reporter)
        e->reporter->finish();
    if (!result.ok() && !result.error().isCancelled() && !e->cancel.isCancelled()) {
        {
            std::lock_guard lock(m_mutex);
            if (e->reporter) // keep where it stopped for the failed row
                e->rec.progress = e->reporter->snapshot();
            e->reporter.reset();
        }
        if (tryAutoRetry(e, result.error())) {
            emit jobChanged(id);
            (void)saveQueue();
            schedule();
            return;
        }
    }
    JobRecord snapshot;
    {
        std::lock_guard lock(m_mutex);
        e->rec.finished = QDateTime::currentDateTime();
        if (e->reporter)
            e->rec.progress = e->reporter->snapshot();
        if (result.ok()) {
            const JobOutcome& o = result.value();
            // A job whose verification failed is a failed job (audit M1).
            if (o.verification == Verification::Failed) {
                e->rec.state = JobState::Failed;
                e->rec.error = Error(ErrorCode::VerificationFailed,
                                     QStringLiteral("Verification failed. %1").arg(o.verificationDetail));
            } else {
                e->rec.state = JobState::Succeeded;
            }
            e->rec.outcome = o;
            e->rec.progress.overallFraction = 1.0;
        } else {
            e->rec.state = result.error().isCancelled() ? JobState::Cancelled : JobState::Failed;
            e->rec.error = result.error();
        }
        e->reporter.reset();
        snapshot = e->rec;
    }
    Log log(QStringLiteral("queue"), id);
    if (snapshot.state == JobState::Succeeded)
        log.info(QStringLiteral("Finished: %1 — %2").arg(snapshot.title, snapshot.outcome ? snapshot.outcome->summary : QString()));
    else if (snapshot.state == JobState::Cancelled)
        log.warn(QStringLiteral("Cancelled: %1").arg(snapshot.title));
    else
        log.error(QStringLiteral("Failed: %1 — %2").arg(snapshot.title, snapshot.error ? snapshot.error->toLogString() : QString()));
    if (m_history)
        (void)m_history->append(snapshot);
    emit jobChanged(id);
    emit jobFinished(id);
    if (m_shuttingDown)
        return; // keep queue.json as saved at shutdown so interrupted jobs can be retried
    (void)saveQueue();
    schedule();
}

Status JobQueue::saveQueue() const
{
    if (m_persistPath.isEmpty() || m_restoring)
        return success();
    QJsonArray arr;
    {
        std::lock_guard lock(m_mutex);
        for (const auto& e : m_entries) {
            // Interrupted records stay until the user retries or removes them.
            if (isTerminal(e->rec.state) && e->rec.state != JobState::Interrupted)
                continue;
            QJsonObject o;
            o.insert(QStringLiteral("kind"), e->rec.kind);
            o.insert(QStringLiteral("title"), e->rec.title);
            o.insert(QStringLiteral("params"), e->rec.parameters);
            o.insert(QStringLiteral("overrides"), variantMapToJson(e->rec.overrides));
            o.insert(QStringLiteral("state"), jobStateName(e->rec.state));
            o.insert(QStringLiteral("added"), e->rec.added.toString(Qt::ISODate));
            if (e->rec.state == JobState::Interrupted && e->rec.error)
                o.insert(QStringLiteral("error"), e->rec.error->message);
            arr.append(o);
        }
        // Jobs of another build of the app, exactly as they were read.
        for (const auto& v : m_foreignJobs)
            arr.append(v);
    }
    QJsonObject root;
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("jobs"), arr);
    return writeFileAtomic(m_persistPath, QJsonDocument(root).toJson(QJsonDocument::Indented));
}

Status JobQueue::restoreQueue()
{
    if (m_persistPath.isEmpty() || !QFile::exists(m_persistPath))
        return success();
    QFile f(m_persistPath);
    if (!f.open(QIODevice::ReadOnly))
        return Error(ErrorCode::IoError, QStringLiteral("Could not read the saved queue."), f.errorString());
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &parseError);
    f.close();
    const QJsonValue saved = doc.object().value(QStringLiteral("jobs"));
    if (parseError.error != QJsonParseError::NoError || !doc.isObject() || (!saved.isArray() && !saved.isUndefined())) {
        // Set aside: the next save would otherwise replace it with an empty queue.
        const QString aside = m_persistPath + QStringLiteral(".damaged");
        QFile::remove(aside);
        const bool moved = QFile::rename(m_persistPath, aside);
        return Error(ErrorCode::CorruptData,
                     moved ? QStringLiteral("The saved queue was damaged and has been set aside as %1. The queue starts empty.")
                                 .arg(QFileInfo(aside).fileName())
                           : QStringLiteral("The saved queue was damaged and could not be read. The queue starts empty."),
                     parseError.error != QJsonParseError::NoError ? parseError.errorString()
                                                                  : QStringLiteral("not a queue file"));
    }
    const QJsonArray arr = saved.toArray();
    const bool previousHold = m_held;
    m_held = true; // restore without starting; the user resumes
    // The file is written once, after the loop: until then it still holds
    // every saved job, whatever happens while they are restored.
    m_restoring = true;
    QJsonArray foreign;
    for (const auto& v : arr) {
        const QJsonObject o = v.toObject();
        const QString kind = o.value(QStringLiteral("kind")).toString();
        const QJsonObject params = o.value(QStringLiteral("params")).toObject();
        const QVariantMap overrides = o.value(QStringLiteral("overrides")).toObject().toVariantMap();
        if (!v.isObject() || !m_factory.contains(kind)) {
            // Saved by another build of the app (one with more job types):
            // kept in the file as it is, so that build finds it again.
            foreign.append(v);
            Log(QStringLiteral("queue"))
                .warn(QStringLiteral("A saved job of the type \"%1\" is not available in this build. It stays in the queue "
                                     "file for the build that knows it.")
                          .arg(kind));
            continue;
        }
        // A record the user can retry, but that never starts by itself.
        const QString savedState = o.value(QStringLiteral("state")).toString();
        const bool wasRunning = savedState != jobStateName(JobState::Queued);
        const auto keepRecord = [&](JobRecord rec, const QString& why, bool fresh) {
            auto e = std::make_shared<Entry>();
            e->rec = std::move(rec);
            e->rec.id = newId();
            e->rec.kind = kind;
            e->rec.added = QDateTime::fromString(o.value(QStringLiteral("added")).toString(), Qt::ISODate);
            e->rec.state = JobState::Interrupted;
            e->rec.error = Error(ErrorCode::Internal, why);
            {
                std::lock_guard lock(m_mutex);
                m_entries.push_back(e);
            }
            // A job that ended this way is part of the history too (once).
            if (fresh && m_history)
                (void)m_history->append(e->rec);
            emit jobAdded(e->rec.id);
        };
        const auto keepInterrupted = [&](std::unique_ptr<Job>& j, const QString& why, bool fresh) {
            JobRecord rec;
            rec.title = j->title();
            rec.subtitle = j->subtitle();
            rec.phases = j->phases();
            rec.parameters = j->parameters();
            rec.overrides = j->settingsOverrides;
            keepRecord(std::move(rec), why, fresh);
        };
        auto job = m_factory.create(kind, params, overrides);
        if (!job.ok()) {
            // The job type exists but refuses what was saved: shown with the
            // reason instead of vanishing; the user removes it.
            Log(QStringLiteral("queue")).warn(QStringLiteral("Could not restore a %1 job: %2").arg(kind, job.error().message));
            JobRecord rec;
            rec.title = o.value(QStringLiteral("title")).toString();
            if (rec.title.isEmpty())
                rec.title = m_factory.displayName(kind);
            rec.parameters = params;
            rec.overrides = overrides;
            const QString before = o.value(QStringLiteral("error")).toString();
            const bool known = savedState == jobStateName(JobState::Interrupted) && !before.isEmpty();
            keepRecord(std::move(rec),
                       known ? before : QStringLiteral("This saved job could not be restored: %1").arg(job.error().message),
                       !known);
            continue;
        }
        if (savedState == jobStateName(JobState::Interrupted)) {
            // Kept from an earlier restart: shown again with the same explanation.
            const QString why = o.value(QStringLiteral("error")).toString();
            keepInterrupted(job.value(), why.isEmpty() ? QStringLiteral("The app closed while this job was running. Retry to run it again.") : why, false);
            continue;
        }
        if (wasRunning) {
            // After a crash or a power cut nobody removed the job's .part file.
            keepInterrupted(job.value(),
                            QStringLiteral("The app closed while this job was running. Its unfinished output (a .part file) "
                                           "may still be in the output folder. Retry to run it again."),
                            true);
            continue;
        }
        // enqueue() validates the job's inputs (a USB drive may not be plugged
        // in yet) and takes the job; a second copy keeps the record if refused.
        auto again = m_factory.create(kind, params, overrides);
        auto added = enqueue(std::move(job).value());
        if (!added.ok()) {
            Log(QStringLiteral("queue")).warn(QStringLiteral("Could not restore a job: %1").arg(added.error().message));
            if (again.ok())
                keepInterrupted(again.value(), QStringLiteral("This job was waiting when the app closed, but it cannot run now: %1 "
                                                              "Retry it when that is fixed.")
                                                   .arg(added.error().message), true);
        }
    }
    {
        std::lock_guard lock(m_mutex);
        m_foreignJobs = foreign;
    }
    m_restoring = false;
    (void)saveQueue();
    // Restored jobs wait for the user to resume the queue.
    m_held = previousHold || pendingCount() > 0;
    if (m_held != previousHold)
        emit heldChanged(m_held);
    schedule();
    return success();
}

bool JobQueue::shutdown(int msecs)
{
    if (m_shuttingDown)
        return m_pool.waitForDone(0);
    (void)saveQueue();
    m_shuttingDown = true;
    power::setKeepAwake(false);
    {
        std::lock_guard lock(m_mutex);
        for (const auto& e : m_entries)
            if (!isTerminal(e->rec.state)) {
                e->cancel.cancel(QStringLiteral("shutdown"));
                e->cancel.resume();
            }
    }
    return m_pool.waitForDone(msecs);
}

} // namespace fw
