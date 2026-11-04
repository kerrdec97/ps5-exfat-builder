#include "jobs/Job.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonValue>

namespace fw {

QString jobStateName(JobState state)
{
    switch (state) {
    case JobState::Queued: return QStringLiteral("Queued");
    case JobState::Running: return QStringLiteral("Running");
    case JobState::Paused: return QStringLiteral("Paused");
    case JobState::Succeeded: return QStringLiteral("Done");
    case JobState::Failed: return QStringLiteral("Failed");
    case JobState::Cancelled: return QStringLiteral("Cancelled");
    case JobState::Interrupted: return QStringLiteral("Interrupted");
    }
    return QStringLiteral("Unknown");
}

bool isTerminal(JobState state)
{
    return state == JobState::Succeeded || state == JobState::Failed || state == JobState::Cancelled
        || state == JobState::Interrupted;
}

QString verificationName(Verification v)
{
    switch (v) {
    case Verification::NotRun: return QStringLiteral("Not run");
    case Verification::Passed: return QStringLiteral("Verified");
    case Verification::Failed: return QStringLiteral("Verification failed");
    case Verification::Skipped: return QStringLiteral("Skipped");
    case Verification::Inconclusive: return QStringLiteral("Inconclusive");
    }
    return QStringLiteral("Unknown");
}

QStringList Job::busyPaths() const
{
    static const char* const keys[] = {"image", "output", "source", "game", "package", "destination", "folder", "file"};
    const QJsonObject p = parameters();
    QStringList out;
    for (const char* k : keys) {
        const QString v = p.value(QLatin1String(k)).toString();
        if (!v.isEmpty())
            out << QDir::cleanPath(QFileInfo(v).absoluteFilePath());
    }
    return out;
}

namespace {
#ifdef Q_OS_WIN
constexpr Qt::CaseSensitivity kPathCase = Qt::CaseInsensitive;
#else
constexpr Qt::CaseSensitivity kPathCase = Qt::CaseSensitive;
#endif
} // namespace

bool busyPathsOverlap(const QStringList& a, const QStringList& b)
{
    const Qt::CaseSensitivity cs = kPathCase;
    const auto inside = [cs](const QString& x, const QString& dir) {
        return x.size() > dir.size() && x.startsWith(dir, cs)
            && (dir.endsWith(u'/') || x.at(dir.size()) == u'/');
    };
    for (const QString& x : a)
        for (const QString& y : b)
            if (x.compare(y, kPathCase) == 0 || inside(x, y) || inside(y, x))
                return true;
    return false;
}

void JobFactory::add(const QString& kind, const QString& displayName, Creator creator)
{
    m_creators[kind] = Entry{displayName, std::move(creator)};
}

QString JobFactory::displayName(const QString& kind) const
{
    auto it = m_creators.find(kind);
    return it == m_creators.end() ? kind : it->second.displayName;
}

Result<std::unique_ptr<Job>> JobFactory::create(const QString& kind, const QJsonObject& params,
                                                const QVariantMap& overrides) const
{
    auto it = m_creators.find(kind);
    if (it == m_creators.end())
        return Error(ErrorCode::Unsupported, QStringLiteral("This job type (%1) is not available in this build.").arg(kind));
    auto job = it->second.creator(params);
    if (job.ok())
        job.value()->settingsOverrides = overrides;
    return job;
}

QStringList JobFactory::kinds() const
{
    QStringList out;
    for (const auto& [k, e] : m_creators)
        out << k;
    return out;
}

} // namespace fw
