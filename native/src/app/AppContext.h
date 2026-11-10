#pragma once

// Owns the application's backend services for the UI: settings, dependency
// registry, job factory/queue/history and the in-memory log buffer. Created in
// main() after QApplication; lives until exit.

#include "core/Dependencies.h"
#include "core/Log.h"
#include "core/Settings.h"
#include "jobs/Job.h"
#include "jobs/JobHistory.h"
#include "jobs/JobQueue.h"

#include <QFuture>
#include <QList>
#include <QMutex>
#include <QObject>

#include <memory>

namespace fw {
class LogModel;
}

namespace fw {

class AppContext : public QObject {
    Q_OBJECT
public:
    explicit AppContext(QObject* parent = nullptr);
    ~AppContext() override;

    // Loads settings, sets up logging and restores the queue.
    //
    // ReadOnly is for a second process next to the running app (--self-test):
    // the queue is neither restored nor written back, and nothing is swept, so
    // the app's own queue file, history and temporary folders stay as they are.
    enum class Mode { Normal, ReadOnly };
    Status initialize(Mode mode = Mode::Normal);

    SettingsStore& settings() { return *m_settings; }
    const SettingsSchema& schema() const { return *m_schema; }
    DependencyRegistry& dependencies() { return m_deps; }
    const JobFactory& jobFactory() const { return m_factory; }
    // Tests register fake job kinds here; the app never adds kinds after start-up.
    JobFactory& mutableJobFactory() { return m_factory; }
    JobQueue& queue() { return *m_queue; }
    JobHistory& history() { return *m_history; }
    std::shared_ptr<RingBufferSink> logBuffer() const { return m_ring; }
    // The UI-thread log model every log view and job monitor shares (created
    // on first use, after initialize()).
    LogModel& logModel();
    QString logFilePath() const;

    // Runs every dependency probe on a worker thread; emits dependenciesChecked.
    // Runs one at a time (the probes share the registry), and the destructor
    // waits for any that is still running.
    void checkDependenciesAsync();

    static AppContext* instance();

    // Where QSettings keeps the window state, the acknowledged crash and the
    // Games tab's folders. A portable copy (and a run with FW_DATA_DIR) keeps
    // them as an INI file in its data folder, so it carries them along and
    // leaves nothing in the registry; an installed copy keeps the place it
    // always had. Call after Paths::initialize(), before the first QSettings.
    // Returns true when the data folder is used.
    static bool usePortableWindowState();

signals:
    void dependenciesChecked();
    // Emitted on the UI thread for each new log record (batched by the UI).
    void logRecordAdded(const fw::LogRecord& record);

private:
    void applyLogLevel();

    std::shared_ptr<SettingsSchema> m_schema;
    DependencyRegistry m_deps;
    JobFactory m_factory;
    std::unique_ptr<SettingsStore> m_settings;
    std::unique_ptr<JobHistory> m_history;
    std::unique_ptr<JobQueue> m_queue;
    QObject* m_postBuild = nullptr; // integration::PostBuildActions (owned by this)
    std::shared_ptr<RingBufferSink> m_ring;
    std::shared_ptr<FileLogSink> m_file;
    std::shared_ptr<CallbackSink> m_uiSink;
    // Dependency checks in flight (checkDependenciesAsync); m_depMutex
    // serialises them: two probe runs must not use m_deps at once.
    QList<QFuture<void>> m_depChecks;
    QMutex m_depMutex;
    LogModel* m_logModel = nullptr; // owned (QObject child)
    bool m_saveFailureLogged = false; // a settings file that cannot be written is reported once
};

} // namespace fw
