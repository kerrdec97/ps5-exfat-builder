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

#include <QObject>

#include <memory>

namespace fw {

class AppContext : public QObject {
    Q_OBJECT
public:
    explicit AppContext(QObject* parent = nullptr);
    ~AppContext() override;

    // Loads settings, sets up logging and restores the queue.
    Status initialize();

    SettingsStore& settings() { return *m_settings; }
    const SettingsSchema& schema() const { return *m_schema; }
    DependencyRegistry& dependencies() { return m_deps; }
    const JobFactory& jobFactory() const { return m_factory; }
    JobQueue& queue() { return *m_queue; }
    JobHistory& history() { return *m_history; }
    std::shared_ptr<RingBufferSink> logBuffer() const { return m_ring; }
    QString logFilePath() const;

    // Runs every dependency probe on a worker thread; emits dependenciesChecked.
    void checkDependenciesAsync();

    static AppContext* instance();

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
    std::shared_ptr<RingBufferSink> m_ring;
    std::shared_ptr<FileLogSink> m_file;
    std::shared_ptr<CallbackSink> m_uiSink;
};

} // namespace fw
