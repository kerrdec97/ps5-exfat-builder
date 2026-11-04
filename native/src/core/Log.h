#pragma once

// Centralised structured logging.
//
// Every record has a level, a source (module: "exfat", "fpkg", "queue"...),
// an optional job id and optional structured fields. Sinks receive every
// record: a rotating text + JSON-lines file sink, an in-memory ring buffer for
// the Logs page, and callback sinks (e.g. a job's own log panel). Thread-safe.

#include <QDateTime>
#include <QJsonObject>
#include <QMetaType>
#include <QString>

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace fw {

enum class LogLevel { Trace = 0, Debug = 1, Info = 2, Warning = 3, Error = 4 };

QString logLevelName(LogLevel level);
LogLevel logLevelFromName(const QString& name, LogLevel fallback = LogLevel::Info);

struct LogRecord {
    quint64 seq = 0;
    QDateTime time;
    LogLevel level = LogLevel::Info;
    QString source;
    QString jobId;
    QString message;
    QJsonObject fields;

    QString toText() const;   // "2026-11-04 04:42:56.123 INFO  [exfat] message"
    QJsonObject toJson() const;
};

class LogSink {
public:
    virtual ~LogSink() = default;
    virtual void write(const LogRecord& record) = 0;
    virtual void flush() {}
};

// Writes `<dir>/<base>.log` (text) and `<dir>/<base>.jsonl` (structured),
// rotating when the text file exceeds `maxBytes` and keeping `keep` old files.
class FileLogSink final : public LogSink {
public:
    FileLogSink(QString dir, QString baseName, qint64 maxBytes = 8 * 1024 * 1024, int keep = 5);
    ~FileLogSink() override;
    void write(const LogRecord& record) override;
    void flush() override;
    QString textPath() const;
    QString jsonPath() const;

private:
    void rotateIfNeeded();
    struct Impl;
    std::unique_ptr<Impl> d;
};

// Keeps the last N records in memory for the Logs page and crash reports.
class RingBufferSink final : public LogSink {
public:
    explicit RingBufferSink(std::size_t capacity = 20000);
    void write(const LogRecord& record) override;
    std::vector<LogRecord> records(quint64 afterSeq = 0) const;
    void clear();

private:
    mutable std::mutex m_mutex;
    std::deque<LogRecord> m_records;
    std::size_t m_capacity;
};

class CallbackSink final : public LogSink {
public:
    explicit CallbackSink(std::function<void(const LogRecord&)> fn) : m_fn(std::move(fn)) {}
    void write(const LogRecord& record) override { if (m_fn) m_fn(record); }

private:
    std::function<void(const LogRecord&)> m_fn;
};

class Logger {
public:
    static Logger& instance();

    void addSink(std::shared_ptr<LogSink> sink);
    void removeSink(const std::shared_ptr<LogSink>& sink);
    void setMinimumLevel(LogLevel level);
    LogLevel minimumLevel() const;

    void log(LogLevel level, const QString& source, const QString& message,
             const QString& jobId = {}, const QJsonObject& fields = {});
    void flush();

    // Route qDebug/qWarning/qCritical into the logger (source "qt").
    static void installQtMessageHandler();

private:
    Logger() = default;
    mutable std::mutex m_mutex;
    std::vector<std::shared_ptr<LogSink>> m_sinks;
    LogLevel m_min = LogLevel::Debug;
    quint64 m_seq = 0;
};

// Convenience logger bound to a source (and optionally a job id).
class Log {
public:
    Log() = default;
    explicit Log(QString source, QString jobId = {}) : m_source(std::move(source)), m_jobId(std::move(jobId)) {}

    void trace(const QString& m, const QJsonObject& f = {}) const { emitRecord(LogLevel::Trace, m, f); }
    void debug(const QString& m, const QJsonObject& f = {}) const { emitRecord(LogLevel::Debug, m, f); }
    void info(const QString& m, const QJsonObject& f = {}) const { emitRecord(LogLevel::Info, m, f); }
    void warn(const QString& m, const QJsonObject& f = {}) const { emitRecord(LogLevel::Warning, m, f); }
    void error(const QString& m, const QJsonObject& f = {}) const { emitRecord(LogLevel::Error, m, f); }

    Log withSource(const QString& source) const { return Log(source, m_jobId); }
    const QString& source() const { return m_source; }
    const QString& jobId() const { return m_jobId; }

private:
    void emitRecord(LogLevel l, const QString& m, const QJsonObject& f) const
    {
        Logger::instance().log(l, m_source, m, m_jobId, f);
    }
    QString m_source = QStringLiteral("app");
    QString m_jobId;
};

} // namespace fw

Q_DECLARE_METATYPE(fw::LogRecord)
