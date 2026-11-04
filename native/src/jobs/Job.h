#pragma once

// A unit of work in the queue (build an image, build an FPKG, backport a game,
// verify, extract...). Jobs are created by modules, described by plain JSON
// parameters (so they can be persisted, retried and restored after a restart)
// and run on a worker thread with a JobContext.

#include "core/Cancellation.h"
#include "core/Log.h"
#include "core/Progress.h"
#include "core/Result.h"
#include "core/Settings.h"

#include <QJsonObject>
#include <QMetaType>
#include <QStringList>
#include <QVariantMap>

#include <functional>
#include <map>
#include <memory>

namespace fw {

enum class JobState { Queued, Running, Paused, Succeeded, Failed, Cancelled, Interrupted };

QString jobStateName(JobState state);
bool isTerminal(JobState state);

enum class Verification { NotRun, Passed, Failed, Skipped, Inconclusive };

QString verificationName(Verification v);

// True when two busyPaths() lists share a path or one contains the other.
bool busyPathsOverlap(const QStringList& a, const QStringList& b);

struct JobOutcome {
    QStringList outputs;          // files/folders produced
    Verification verification = Verification::NotRun;
    QString verificationDetail;   // "18,442 files compared, all identical"
    QString summary;              // one line for the queue row and history
    QStringList warnings;         // non-fatal issues the user should see
    QJsonObject details;          // engine-specific facts for the log/history
};

struct JobContext {
    QString jobId;
    CancellationToken token;
    ProgressReporter* progress = nullptr; // never null while running
    Log log;
    SettingsValues settings;
    QString tempDir; // private, created and removed by the queue
};

class Job {
public:
    virtual ~Job() = default;

    virtual QString kind() const = 0;      // stable id: "image.exfat", "fpkg.build", ...
    virtual QString title() const = 0;     // "Astro Bot (PPSA01325)"
    virtual QString subtitle() const { return {}; } // "exFAT · 48.2 GB → D:\Images"
    virtual QStringList phases() const { return {QStringLiteral("Working")}; }
    // Relative weight of each phase for the overall progress (default equal).
    virtual QList<double> phaseWeights() const { return {}; }
    virtual QJsonObject parameters() const = 0; // enough to re-create the job

    // Files and folders this job reads or changes that no other job may use at
    // the same time (an image being edited, verified or written, a game being
    // backported). The queue does not start a job whose paths overlap — equal,
    // or one inside the other — a running job's. Default: the string values of
    // the parameters "image", "output", "source", "game", "package",
    // "destination", "folder" and "file", made absolute.
    virtual QStringList busyPaths() const;

    // Cheap pre-flight check before the job is accepted into the queue.
    virtual Status validate(const SettingsValues& settings) const
    {
        Q_UNUSED(settings);
        return success();
    }

    // Runs on a worker thread. Must honour ctx.token (checkpoint regularly),
    // write outputs through .part files and never leave a partial output under
    // its final name.
    virtual Result<JobOutcome> run(JobContext& ctx) = 0;

    QVariantMap settingsOverrides; // per-job overrides (validated by the queue)
};

class JobFactory {
public:
    using Creator = std::function<Result<std::unique_ptr<Job>>(const QJsonObject& params)>;

    void add(const QString& kind, const QString& displayName, Creator creator);
    bool contains(const QString& kind) const { return m_creators.count(kind) > 0; }
    QString displayName(const QString& kind) const;
    Result<std::unique_ptr<Job>> create(const QString& kind, const QJsonObject& params,
                                        const QVariantMap& overrides = {}) const;
    QStringList kinds() const;

private:
    struct Entry {
        QString displayName;
        Creator creator;
    };
    std::map<QString, Entry> m_creators;
};

} // namespace fw

Q_DECLARE_METATYPE(fw::JobState)
