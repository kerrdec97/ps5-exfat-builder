#include "core/Paths.h"

#include "core/FileUtil.h"

#include "core/Log.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QStandardPaths>

#include <mutex>

namespace fw::Paths {

namespace {
struct State {
    std::mutex mutex;
    bool initialized = false;
    bool portable = false;
    QString data;
    QString local;
};

State& state()
{
    static State s;
    return s;
}

void ensure()
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    if (s.initialized)
        return;
    s.initialized = true;

    const QString env = qEnvironmentVariable("FW_DATA_DIR");
    if (!env.isEmpty()) {
        s.data = QDir(env).filePath(QStringLiteral("data"));
        s.local = QDir(env).filePath(QStringLiteral("local"));
    } else {
        const QString exeDir = QCoreApplication::instance() ? QCoreApplication::applicationDirPath() : QString();
        if (!exeDir.isEmpty() && QFileInfo::exists(QDir(exeDir).filePath(QStringLiteral("portable.txt")))) {
            s.portable = true;
            s.data = QDir(exeDir).filePath(QStringLiteral("data"));
            s.local = s.data;
        } else {
            s.data = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
            s.local = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
            if (s.data.isEmpty())
                s.data = QDir::home().filePath(QStringLiteral(".farewell-edition"));
            if (s.local.isEmpty())
                s.local = s.data;
        }
    }
    QDir().mkpath(s.data);
    QDir().mkpath(s.local);
}

QString sub(const QString& base, const QString& name)
{
    const QString p = QDir(base).filePath(name);
    QDir().mkpath(p);
    return p;
}
} // namespace

void initialize()
{
    if (QCoreApplication::organizationName().isEmpty())
        QCoreApplication::setOrganizationName(QStringLiteral("kerrdec97"));
    if (QCoreApplication::applicationName().isEmpty())
        QCoreApplication::setApplicationName(QStringLiteral("exFAT Image Builder Farewell"));
    ensure();
}

void overrideRoot(const QString& root)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    s.initialized = true;
    s.portable = false;
    s.data = QDir(root).filePath(QStringLiteral("data"));
    s.local = QDir(root).filePath(QStringLiteral("local"));
    QDir().mkpath(s.data);
    QDir().mkpath(s.local);
}

bool isPortable()
{
    ensure();
    return state().portable;
}

QString dataDir()
{
    ensure();
    return state().data;
}

QString localDir()
{
    ensure();
    return state().local;
}

QString settingsFile() { return QDir(dataDir()).filePath(QStringLiteral("settings.json")); }
QString historyFile() { return QDir(dataDir()).filePath(QStringLiteral("history.jsonl")); }
QString queueFile() { return QDir(dataDir()).filePath(QStringLiteral("queue.json")); }
QString logsDir() { return sub(localDir(), QStringLiteral("logs")); }
QString cacheDir() { return sub(localDir(), QStringLiteral("cache")); }
QString tempRoot() { return sub(localDir(), QStringLiteral("temp")); }
QString toolsDir() { return sub(localDir(), QStringLiteral("tools")); }

int sweepStaleTemp(int maxAgeHours)
{
    QDir root(tempRoot());
    const QDateTime cutoff = QDateTime::currentDateTime().addSecs(-qint64(maxAgeHours) * 3600);
    int removed = 0;
    const auto entries = root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden);
    for (const QFileInfo& fi : entries) {
        // Only folders this app created ("fw-" prefix) are ever swept.
        if (!fi.fileName().startsWith(QStringLiteral("fw-")))
            continue;
        if (fi.lastModified() > cutoff)
            continue;
        if (removeTreeNoFollow(fi.absoluteFilePath())) // a crashed job may have left a junction inside
            ++removed;
    }
    if (removed > 0)
        Logger::instance().log(LogLevel::Info, QStringLiteral("paths"),
                               QStringLiteral("Removed %1 stale temporary folder(s)").arg(removed));
    return removed;
}

qint64 tempUsageBytes()
{
    qint64 total = 0;
    QDirIterator it(tempRoot(), QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        total += it.fileInfo().size();
    }
    return total;
}

} // namespace fw::Paths
