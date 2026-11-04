#include "jobs/Job.h"

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
