#include "core/Log.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QtGlobal>

#include <algorithm>

namespace fw {

QString logLevelName(LogLevel level)
{
    switch (level) {
    case LogLevel::Trace: return QStringLiteral("trace");
    case LogLevel::Debug: return QStringLiteral("debug");
    case LogLevel::Info: return QStringLiteral("info");
    case LogLevel::Warning: return QStringLiteral("warning");
    case LogLevel::Error: return QStringLiteral("error");
    }
    return QStringLiteral("info");
}

LogLevel logLevelFromName(const QString& name, LogLevel fallback)
{
    const QString n = name.trimmed().toLower();
    if (n == u"trace") return LogLevel::Trace;
    if (n == u"debug") return LogLevel::Debug;
    if (n == u"info") return LogLevel::Info;
    if (n == u"warning" || n == u"warn") return LogLevel::Warning;
    if (n == u"error") return LogLevel::Error;
    return fallback;
}

QString LogRecord::toText() const
{
    static const char* tags[] = {"TRACE", "DEBUG", "INFO ", "WARN ", "ERROR"};
    QString line = time.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
    line += u' ';
    line += QString::fromLatin1(tags[int(level)]);
    line += QStringLiteral(" [%1]").arg(source);
    if (!jobId.isEmpty())
        line += QStringLiteral(" {%1}").arg(jobId);
    line += u' ';
    line += message;
    if (!fields.isEmpty())
        line += u' ' + QString::fromUtf8(QJsonDocument(fields).toJson(QJsonDocument::Compact));
    return line;
}

QJsonObject LogRecord::toJson() const
{
    QJsonObject o;
    o.insert(QStringLiteral("seq"), qint64(seq));
    o.insert(QStringLiteral("time"), time.toString(Qt::ISODateWithMs));
    o.insert(QStringLiteral("level"), logLevelName(level));
    o.insert(QStringLiteral("source"), source);
    if (!jobId.isEmpty())
        o.insert(QStringLiteral("job"), jobId);
    o.insert(QStringLiteral("message"), message);
    if (!fields.isEmpty())
        o.insert(QStringLiteral("fields"), fields);
    return o;
}

// ---------------------------------------------------------------------------

struct FileLogSink::Impl {
    QString dir, base;
    qint64 maxBytes;
    int keep;
    QFile text, json;
    std::mutex mutex;
    qint64 rotateAt = 0; // size that triggers the next rotation

    void flushFiles()
    {
        text.flush();
        json.flush();
    }
};

FileLogSink::FileLogSink(QString dir, QString baseName, qint64 maxBytes, int keep)
    : d(std::make_unique<Impl>())
{
    d->dir = std::move(dir);
    d->base = std::move(baseName);
    d->maxBytes = maxBytes;
    d->keep = std::max(1, keep);
    QDir().mkpath(d->dir);
    d->text.setFileName(textPath());
    d->json.setFileName(jsonPath());
    (void)d->text.open(QIODevice::Append | QIODevice::Text);
    (void)d->json.open(QIODevice::Append | QIODevice::Text);
}

FileLogSink::~FileLogSink()
{
    flush();
}

QString FileLogSink::textPath() const { return QDir(d->dir).filePath(d->base + QStringLiteral(".log")); }
QString FileLogSink::jsonPath() const { return QDir(d->dir).filePath(d->base + QStringLiteral(".jsonl")); }

void FileLogSink::rotateIfNeeded()
{
    if (d->text.size() < std::max(d->maxBytes, d->rotateAt))
        return;
    d->text.close();
    d->json.close();
    const QDir dir(d->dir);
    for (int i = d->keep - 1; i >= 1; --i) {
        for (const auto* ext : {".log", ".jsonl"}) {
            const QString from = dir.filePath(QStringLiteral("%1.%2%3").arg(d->base).arg(i).arg(QLatin1String(ext)));
            const QString to = dir.filePath(QStringLiteral("%1.%2%3").arg(d->base).arg(i + 1).arg(QLatin1String(ext)));
            QFile::remove(to);
            QFile::rename(from, to);
        }
    }
    bool rotated = true;
    for (const auto* ext : {".log", ".jsonl"}) {
        const QString cur = dir.filePath(d->base + QLatin1String(ext));
        const QString to = dir.filePath(QStringLiteral("%1.1%2").arg(d->base, QLatin1String(ext)));
        QFile::remove(to);
        if (!QFile::rename(cur, to))
            rotated = false;
    }
    (void)d->text.open(QIODevice::Append | QIODevice::Text);
    (void)d->json.open(QIODevice::Append | QIODevice::Text);
    // The current log could not be renamed (another program holds it): keep
    // writing to it and try again only after another `maxBytes`, instead of
    // shifting — and deleting — the older logs on every write.
    d->rotateAt = rotated ? 0 : d->text.size() + d->maxBytes;
}

void FileLogSink::write(const LogRecord& r)
{
    std::lock_guard lock(d->mutex);
    rotateIfNeeded();
    if (d->text.isOpen()) {
        d->text.write(r.toText().toUtf8());
        d->text.write("\n");
    }
    if (d->json.isOpen()) {
        d->json.write(QJsonDocument(r.toJson()).toJson(QJsonDocument::Compact));
        d->json.write("\n");
    }
    if (r.level >= LogLevel::Warning)
        d->flushFiles();
}

void FileLogSink::flush()
{
    std::lock_guard lock(d->mutex); // never while write() or a rotation uses the files
    d->flushFiles();
}

// ---------------------------------------------------------------------------

RingBufferSink::RingBufferSink(std::size_t capacity) : m_capacity(std::max<std::size_t>(16, capacity)) {}

void RingBufferSink::write(const LogRecord& r)
{
    std::lock_guard lock(m_mutex);
    m_records.push_back(r);
    while (m_records.size() > m_capacity)
        m_records.pop_front();
}

std::vector<LogRecord> RingBufferSink::records(quint64 afterSeq) const
{
    std::lock_guard lock(m_mutex);
    std::vector<LogRecord> out;
    for (const auto& r : m_records)
        if (r.seq > afterSeq)
            out.push_back(r);
    return out;
}

void RingBufferSink::clear()
{
    std::lock_guard lock(m_mutex);
    m_records.clear();
}

// ---------------------------------------------------------------------------

Logger& Logger::instance()
{
    static Logger logger;
    return logger;
}

void Logger::addSink(std::shared_ptr<LogSink> sink)
{
    std::lock_guard lock(m_mutex);
    m_sinks.push_back(std::move(sink));
}

void Logger::removeSink(const std::shared_ptr<LogSink>& sink)
{
    std::lock_guard lock(m_mutex);
    m_sinks.erase(std::remove(m_sinks.begin(), m_sinks.end(), sink), m_sinks.end());
}

void Logger::setMinimumLevel(LogLevel level)
{
    std::lock_guard lock(m_mutex);
    m_min = level;
}

LogLevel Logger::minimumLevel() const
{
    std::lock_guard lock(m_mutex);
    return m_min;
}

void Logger::log(LogLevel level, const QString& source, const QString& message, const QString& jobId,
                 const QJsonObject& fields)
{
    std::vector<std::shared_ptr<LogSink>> sinks;
    LogRecord r;
    {
        std::lock_guard lock(m_mutex);
        if (int(level) < int(m_min))
            return;
        r.seq = ++m_seq;
        sinks = m_sinks;
    }
    r.time = QDateTime::currentDateTime();
    r.level = level;
    r.source = source;
    r.jobId = jobId;
    r.message = message;
    r.fields = fields;
    for (const auto& s : sinks)
        s->write(r);
}

void Logger::flush()
{
    std::vector<std::shared_ptr<LogSink>> sinks;
    {
        std::lock_guard lock(m_mutex);
        sinks = m_sinks;
    }
    for (const auto& s : sinks)
        s->flush();
}

static void fwQtMessageHandler(QtMsgType type, const QMessageLogContext& context, const QString& msg)
{
    LogLevel level = LogLevel::Debug;
    switch (type) {
    case QtDebugMsg: level = LogLevel::Debug; break;
    case QtInfoMsg: level = LogLevel::Info; break;
    case QtWarningMsg: level = LogLevel::Warning; break;
    case QtCriticalMsg:
    case QtFatalMsg: level = LogLevel::Error; break;
    }
    // Qt's own notes (category "qt.*": "OpenType support missing for ..." from
    // qt.text.font.db, a dozen per start) say nothing the user can act on: at
    // Info they filled the Logs page and every problem report. They are kept
    // as detail (Debug); Qt's warnings and errors keep their level.
    const QLatin1StringView category(context.category ? context.category : "");
    QJsonObject fields;
    if (!category.isEmpty() && category != QLatin1StringView("default")) {
        fields.insert(QStringLiteral("category"), QString(category));
        if (level == LogLevel::Info && category.startsWith(QLatin1StringView("qt.")))
            level = LogLevel::Debug;
    }
    Logger::instance().log(level, QStringLiteral("qt"), msg, {}, fields);
}

void Logger::installQtMessageHandler()
{
    qInstallMessageHandler(fwQtMessageHandler);
}

} // namespace fw
