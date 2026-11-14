#include "app/models/JobQueueModel.h"

#include "core/FileUtil.h"
#include "core/Tr.h"

#include <algorithm>
#include <cmath>

namespace fw {

QString progressDetailText(const ProgressSnapshot& p)
{
    return progressDetailText(p, p.jobEtaMs);
}

QString progressDetailText(const ProgressSnapshot& p, qint64 timeLeftMs)
{
    QStringList parts;
    if (p.total > 0) {
        // Never more than the total, whatever a snapshot made by hand says.
        const qint64 done = std::clamp<qint64>(p.done, 0, p.total);
        if (p.unitIsBytes)
            parts << FW_TR("%1 of %2").arg(formatBytes(done), formatBytes(p.total));
        else
            parts << FW_TR("%1 of %2").arg(formatCount(done), formatCount(p.total));
    } else if (p.unitIsBytes && p.done > 0) {
        parts << formatBytes(p.done); // how much so far, of a size nobody knows yet
    }
    if (p.filesTotal > 0)
        parts << FW_TR("%1 of %2 files").arg(formatCount(std::clamp<qint64>(p.filesDone, 0, p.filesTotal)), formatCount(p.filesTotal));
    else if (p.filesDone > 0)
        parts << (p.filesDone == 1 ? FW_TR("%1 file") : FW_TR("%1 files")).arg(formatCount(p.filesDone));
    if (p.unitIsBytes && p.ratePerSec > 0)
        parts << formatRate(p.ratePerSec);
    // Unqualified, "About 3 min left" is read as the job's: never the stage's.
    if (timeLeftMs >= 0)
        parts << formatTimeLeft(timeLeftMs);
    return parts.join(QStringLiteral(" · "));
}

QString currentItemText(const QString& item)
{
    const QString whole = i18n::translate(item);
    if (whole != item)
        return whole;
    const QString sep = QStringLiteral(" · ");
    const qsizetype at = item.indexOf(sep);
    if (at <= 0)
        return item;
    return i18n::translate(item.left(at)) + item.mid(at);
}

double shownFraction(const JobRecord& r)
{
    if (r.state == JobState::Succeeded)
        return 1.0;
    const double f = r.progress.overallFraction;
    return std::isfinite(f) ? std::clamp(f, 0.0, 0.999) : 0.0;
}

QStringList stageNames(const JobRecord& r)
{
    return r.progress.phaseNames.isEmpty() ? r.phases : r.progress.phaseNames;
}

JobQueueModel::JobQueueModel(JobQueue& queue, QObject* parent) : QAbstractListModel(parent), m_queue(queue)
{
    reload();
    // Membership changes one row at a time: a view keeps its selection and
    // scroll position, and queueing many jobs copies each record once.
    connect(&m_queue, &JobQueue::jobAdded, this, &JobQueueModel::insertJob);
    connect(&m_queue, &JobQueue::jobRemoved, this, &JobQueueModel::removeJob);
    connect(&m_queue, &JobQueue::orderChanged, this, &JobQueueModel::syncOrder);
    connect(&m_queue, &JobQueue::jobChanged, this, &JobQueueModel::refresh);
    connect(&m_queue, &JobQueue::jobFinished, this, [this](const QString& id) {
        // The counts are announced once for a finished job, whether or not its last change already showed the state.
        const int row = rowOf(id);
        const bool known = row >= 0;
        const JobState before = known ? m_rows[size_t(row)].state : JobState::Queued;
        refresh(id);
        if (known && rowOf(id) >= 0 && m_rows[size_t(rowOf(id))].state == before)
            emit countsChanged();
    });
}

void JobQueueModel::reload()
{
    beginResetModel();
    m_rows = m_queue.records();
    endResetModel();
    emit countsChanged();
}

void JobQueueModel::insertJob(const QString& id)
{
    if (rowOf(id) >= 0)
        return;
    const auto rec = m_queue.record(id);
    if (!rec)
        return; // gone again already
    // New jobs go to the end of the queue; "run next" follows as an order change.
    const int row = int(m_rows.size());
    beginInsertRows({}, row, row);
    m_rows.push_back(*rec);
    endInsertRows();
    emit countsChanged();
}

void JobQueueModel::removeJob(const QString& id)
{
    const int row = rowOf(id);
    if (row < 0)
        return;
    beginRemoveRows({}, row, row);
    m_rows.erase(m_rows.begin() + row);
    endRemoveRows();
    emit countsChanged();
}

void JobQueueModel::syncOrder()
{
    std::vector<JobRecord> now = m_queue.records();
    bool same = now.size() == m_rows.size();
    for (size_t i = 0; same && i < now.size(); ++i)
        same = now[i].id == m_rows[i].id;
    if (same)
        return; // the order the views show already
    beginResetModel();
    m_rows = std::move(now);
    endResetModel();
    emit countsChanged();
}

void JobQueueModel::refresh(const QString& id)
{
    const int row = rowOf(id);
    if (row < 0) {
        insertJob(id);
        return;
    }
    auto rec = m_queue.record(id);
    if (!rec)
        return;
    const JobState before = m_rows[size_t(row)].state;
    m_rows[size_t(row)] = *rec;
    emit dataChanged(index(row), index(row));
    if (before != rec->state)
        emit countsChanged();
}

int JobQueueModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : int(m_rows.size());
}

QString JobQueueModel::idAt(int row) const
{
    return (row >= 0 && row < int(m_rows.size())) ? m_rows[size_t(row)].id : QString();
}

int JobQueueModel::rowOf(const QString& id) const
{
    for (size_t i = 0; i < m_rows.size(); ++i)
        if (m_rows[i].id == id)
            return int(i);
    return -1;
}

const JobRecord* JobQueueModel::recordAt(int row) const
{
    return (row >= 0 && row < int(m_rows.size())) ? &m_rows[size_t(row)] : nullptr;
}

int JobQueueModel::countInState(JobState state) const
{
    int n = 0;
    for (const auto& r : m_rows)
        if (r.state == state)
            ++n;
    return n;
}

static QString elapsedText(const JobRecord& r)
{
    if (!r.started.isValid())
        return {};
    const QDateTime end = r.finished.isValid() ? r.finished : QDateTime::currentDateTime();
    const qint64 s = std::max<qint64>(0, r.started.secsTo(end)); // the clock may have been set back
    if (s < 60)
        return FW_TR("%1 s").arg(s);
    if (s < 3600)
        return FW_TR("%1 min %2 s").arg(s / 60).arg(s % 60);
    return FW_TR("%1 h %2 min").arg(s / 3600).arg((s % 3600) / 60);
}

QVariant JobQueueModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= int(m_rows.size()))
        return {};
    const JobRecord& r = m_rows[size_t(index.row())];
    const ProgressSnapshot& p = r.progress;
    switch (role) {
    case Qt::DisplayRole:
    case TitleRole: return r.title;
    case IdRole: return r.id;
    case KindRole: return r.kind;
    case SubtitleRole: return r.subtitle;
    case StateRole: return int(r.state);
    case StateTextRole: return i18n::translate(jobStateName(r.state)); // shown; StateRole is the value
    case PhaseRole: return i18n::translate(p.phase);
    case PhaseIndexRole: return p.phaseIndex;
    case PhaseCountRole: return p.phaseCount;
    case FractionRole: return shownFraction(r);
    case PhaseFractionRole: return p.phaseNames.isEmpty() ? p.phaseFraction() : p.phaseProgress; // the reporter's never falls back
    case IndeterminateRole: return p.indeterminate;
    case DoneRole: return p.done;
    case TotalRole: return p.total;
    case FilesDoneRole: return p.filesDone;
    case FilesTotalRole: return p.filesTotal;
    case RateTextRole: return r.state == JobState::Running && p.unitIsBytes && p.ratePerSec > 0 ? formatRate(p.ratePerSec) : QString();
    case EtaTextRole: return r.state == JobState::Running ? formatTimeLeft(p.jobEtaMs) : QString();
    case DetailTextRole:
        if (r.isActive()) {
            // Paused or stopping: no speed and no time left.
            if (r.state == JobState::Running)
                return progressDetailText(p);
            ProgressSnapshot still = p;
            still.ratePerSec = 0;
            return progressDetailText(still, -1);
        }
        if (r.state == JobState::Succeeded && r.outcome)
            return i18n::translate(r.outcome->summary);
        if (r.error)
            return r.error->message;
        return {};
    case CurrentItemRole: return currentItemText(p.currentItem);
    case VerificationRole: return r.outcome ? int(r.outcome->verification) : int(Verification::NotRun);
    case VerificationTextRole:
        return r.outcome ? i18n::translate(verificationName(r.outcome->verification)) : QString();
    case SummaryRole: return r.outcome ? r.outcome->summary : QString();
    case ErrorTextRole: return r.error ? r.error->message : QString();
    case OutputsRole: return r.outcome ? r.outcome->outputs : QStringList();
    case AddedRole: return r.added;
    case StartedRole: return r.started;
    case FinishedRole: return r.finished;
    case ElapsedTextRole: return elapsedText(r);
    default: return {};
    }
}

QHash<int, QByteArray> JobQueueModel::roleNames() const
{
    return {{IdRole, "id"}, {TitleRole, "title"}, {SubtitleRole, "subtitle"}, {StateRole, "state"},
            {StateTextRole, "stateText"}, {PhaseRole, "phase"}, {FractionRole, "fraction"},
            {DetailTextRole, "detail"}, {ErrorTextRole, "error"}};
}

} // namespace fw
