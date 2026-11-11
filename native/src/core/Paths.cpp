#include "core/Paths.h"

#include "core/FileUtil.h"

#include "core/Log.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QThreadPool>

#include <mutex>

namespace fw::Paths {

namespace {
struct State {
    std::mutex mutex;
    bool initialized = false;
    bool portable = false;
    bool overridden = false;
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
    s.overridden = !env.isEmpty();
    if (!env.isEmpty()) {
        s.data = QDir(env).filePath(QStringLiteral("data"));
        s.local = QDir(env).filePath(QStringLiteral("local"));
    } else {
        const QString exeDir = appDir();
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

QString appDir()
{
    const QString launcher = qEnvironmentVariable("FW_LAUNCHER_DIR");
    if (!launcher.isEmpty() && QFileInfo(launcher).isDir())
        return QDir::cleanPath(QDir::fromNativeSeparators(launcher));
    return QCoreApplication::instance() ? QCoreApplication::applicationDirPath() : QString();
}

QString launcherExe()
{
    const QString exe = qEnvironmentVariable("FW_LAUNCHER_EXE");
    if (exe.isEmpty() || !QFileInfo(exe).isFile())
        return {};
    return QDir::cleanPath(QDir::fromNativeSeparators(exe));
}

QString programDir()
{
    return QCoreApplication::instance() ? QCoreApplication::applicationDirPath() : QString();
}

QString portableMarkerFile()
{
    const QString dir = appDir();
    return dir.isEmpty() ? QString() : QDir(dir).filePath(QStringLiteral("portable.txt"));
}

bool isOverridden()
{
    ensure();
    return state().overridden;
}

void resetForTests()
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    s.initialized = false;
    s.portable = false;
    s.overridden = false;
    s.data.clear();
    s.local.clear();
}

void overrideRoot(const QString& root)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    s.initialized = true;
    s.portable = false;
    s.overridden = true;
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

namespace {
bool isOwnTempRoot(const QString& folder)
{
    const QString root = tempRoot();
    return isSameOrInside(folder, root) && isSameOrInside(root, folder);
}

// "fw-job-J<HHmmss>-<nnn>-<yyyyMMddHHmmss>-<6 hex>": the name the job queue
// gives a job's folder (TempDir with the prefix "job-<job id>").
bool isJobTempName(const QString& name)
{
    static const QRegularExpression shape(QStringLiteral("^fw-job-J[0-9]{6}-[0-9]{3,}-[0-9]{14}-[0-9a-f]{6}$"));
    return shape.match(name).hasMatch();
}

// What the sweep may delete in `root`. The app's own temp root holds nothing
// but its folders, so the "fw-" prefix is enough there. Any other folder is
// the user's (they may keep a "fw-11.00" of their own in it): only a job
// folder that also carries the app's marker file is the app's.
bool isSweepable(const QFileInfo& fi, bool ownRoot)
{
    if (ownRoot)
        return fi.fileName().startsWith(QStringLiteral("fw-"));
    return isJobTempName(fi.fileName()) && TempDir::isOwnedFolder(fi.absoluteFilePath());
}

QFileInfoList subfolders(const QString& folder)
{
    return QDir(folder).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
}

// Size of the files below `dir`; links and junctions are not followed.
qint64 treeBytesNoFollow(const QString& dir)
{
    qint64 total = 0;
    QStringList pending{dir};
    while (!pending.isEmpty()) {
        QDirIterator it(pending.takeLast(), QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
        while (it.hasNext()) {
            const QFileInfo fi = it.nextFileInfo();
            if (fi.isSymbolicLink() || fi.isJunction())
                continue;
            if (fi.isDir())
                pending << fi.absoluteFilePath();
            else
                total += fi.size();
        }
    }
    return total;
}
} // namespace

int sweepStaleTemp(int maxAgeHours) { return sweepStaleTempIn(tempRoot(), maxAgeHours); }

int sweepStaleTempIn(const QString& folder, int maxAgeHours)
{
    if (folder.isEmpty() || !QDir(folder).exists())
        return 0;
    const bool ownRoot = isOwnTempRoot(folder);
    const QDateTime cutoff = QDateTime::currentDateTime().addSecs(-qint64(maxAgeHours) * 3600);
    int removed = 0;
    for (const QFileInfo& fi : subfolders(folder)) {
        if (!isSweepable(fi, ownRoot))
            continue;
        if (fi.lastModified() > cutoff)
            continue;
        if (removeTreeNoFollow(fi.absoluteFilePath())) // a crashed job may have left a junction inside
            ++removed;
    }
    if (removed > 0)
        Logger::instance().log(LogLevel::Info, QStringLiteral("paths"),
                               QStringLiteral("Removed %1 stale temporary folder(s) from %2")
                                   .arg(QString::number(removed), QDir::toNativeSeparators(folder)));
    return removed;
}

void sweepStaleTempInBackground(const QStringList& otherFolders, int maxAgeHours)
{
    QThreadPool::globalInstance()->start([otherFolders, maxAgeHours] {
        sweepStaleTemp(maxAgeHours);
        for (const QString& folder : otherFolders)
            if (!folder.trimmed().isEmpty() && !isOwnTempRoot(folder.trimmed()))
                sweepStaleTempIn(folder.trimmed(), maxAgeHours);
    });
}

qint64 tempUsageBytes()
{
    return treeBytesNoFollow(tempRoot());
}

qint64 tempUsageBytesIn(const QString& folder)
{
    if (folder.isEmpty() || !QDir(folder).exists())
        return 0;
    if (isOwnTempRoot(folder))
        return tempUsageBytes();
    qint64 total = 0;
    for (const QFileInfo& fi : subfolders(folder))
        if (isSweepable(fi, false))
            total += treeBytesNoFollow(fi.absoluteFilePath());
    return total;
}

} // namespace fw::Paths
