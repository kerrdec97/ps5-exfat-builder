#include "app/models/JobQueueModel.h"
#include "app/i18n/I18n.h"

#include "core/FileUtil.h"

namespace fw {

QString progressDetailText(const ProgressSnapshot& p)
{
    QStringList parts;
    if (p.total > 0) {
        if (p.unitIsBytes)
            parts << tr("%1 of %2").arg(formatBytes(p.done), formatBytes(p.total));
        else
            parts << tr("%1 of %2").arg(formatCount(p.done), formatCount(p.total));
    }
    if (p.filesTotal > 0)
        parts << tr("%1 of %2 files").arg(formatCount(p.filesDone), formatCount(p.filesTotal));
    if (p.unitIsBytes && p.ratePerSec > 0)
        parts << formatRate(p.ratePerSec);
    if (p.etaMs >= 0)
        parts << formatTimeLeft(p.etaMs);
    return parts.join(QStringLiteral(" · "));
}

JobQueueModel::JobQueueModel(JobQueue& queue, QObject* parent) : QAbstractListModel(parent), m_queue(queue)
{
    reload();
    connect(&m_queue, &JobQueue::jobAdded, this, [this](const QString&) { reload(); });
    connect(&m_queue, &JobQueue::jobRemoved, this, [this](const QString&) { reload(); });
    connect(&m_queue, &JobQueue::orderChanged, this, [this] { reload(); });
    connect(&m_queue, &JobQueue::jobChanged, this, &JobQueueModel::refresh);
    connect(&m_queue, &JobQueue::jobFinished, this, [this](const QString& id) {
        refresh(id);
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

void JobQueueModel::refresh(const QString& id)
{
    const int row = rowOf(id);
    if (row < 0) {
        reload();
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
    const qint64 s = r.started.secsTo(end);
    if (s < 60)
        return tr("%1 s").arg(s);
    if (s < 3600)
        return tr("%1 min %2 s").arg(s / 60).arg(s % 60);
    return tr("%1 h %2 min").arg(s / 3600).arg((s % 3600) / 60);
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
    case StateTextRole: return jobStateName(r.state);
    case PhaseRole: return p.phase;
    case PhaseIndexRole: return p.phaseIndex;
    case PhaseCountRole: return p.phaseCount;
    case FractionRole: return p.overallFraction;
    case PhaseFractionRole: return p.phaseFraction();
    case IndeterminateRole: return p.indeterminate;
    case DoneRole: return p.done;
    case TotalRole: return p.total;
    case FilesDoneRole: return p.filesDone;
    case FilesTotalRole: return p.filesTotal;
    case RateTextRole: return p.unitIsBytes && p.ratePerSec > 0 ? formatRate(p.ratePerSec) : QString();
    case EtaTextRole: return formatTimeLeft(p.etaMs);
    case DetailTextRole:
        if (r.state == JobState::Running || r.state == JobState::Paused)
            return progressDetailText(p);
        if (r.state == JobState::Succeeded && r.outcome)
            return r.outcome->summary;
        if (r.error)
            return r.error->message;
        return {};
    case CurrentItemRole: return p.currentItem;
    case VerificationRole: return r.outcome ? int(r.outcome->verification) : int(Verification::NotRun);
    case VerificationTextRole:
        return r.outcome ? verificationName(r.outcome->verification) : QString();
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
