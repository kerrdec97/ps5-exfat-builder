#include "app/models/LogModel.h"
#include "app/i18n/I18n.h"

#include "app/AppContext.h"

namespace fw {

LogModel::LogModel(AppContext& ctx, QObject* parent) : QAbstractTableModel(parent)
{
    if (auto ring = ctx.logBuffer()) {
        m_rows = ring->records();
        for (const auto& r : m_rows)
            if (!m_sources.contains(r.source))
                m_sources << r.source;
    }
    m_timer.setInterval(150);
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, &LogModel::flushPending);
    connect(&ctx, &AppContext::logRecordAdded, this, [this](const LogRecord& r) {
        m_pending.push_back(r);
        if (!m_timer.isActive())
            m_timer.start();
    });
}

void LogModel::flushPending()
{
    if (m_pending.empty())
        return;
    // Drop the oldest rows first if we would exceed the cap.
    const int overflow = int(m_rows.size() + m_pending.size()) - m_maxRows;
    if (overflow > 0) {
        const int drop = std::min<int>(overflow, int(m_rows.size()));
        if (drop > 0) {
            beginRemoveRows({}, 0, drop - 1);
            m_rows.erase(m_rows.begin(), m_rows.begin() + drop);
            endRemoveRows();
        }
    }
    const int first = int(m_rows.size());
    const int count = int(m_pending.size());
    bool newSource = false;
    beginInsertRows({}, first, first + count - 1);
    for (auto& r : m_pending) {
        if (!m_sources.contains(r.source)) {
            m_sources << r.source;
            newSource = true;
        }
        m_rows.push_back(std::move(r));
    }
    m_pending.clear();
    endInsertRows();
    if (newSource)
        emit sourcesChanged();
    emit rowsAppended(count);
}

int LogModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : int(m_rows.size());
}

int LogModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant LogModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= int(m_rows.size()))
        return {};
    const LogRecord& r = m_rows[size_t(index.row())];
    switch (role) {
    case Qt::DisplayRole:
        switch (index.column()) {
        case TimeColumn: return r.time.toString(QStringLiteral("HH:mm:ss.zzz"));
        case LevelColumn: return logLevelName(r.level);
        case SourceColumn: return r.source;
        case JobColumn: return r.jobId;
        case MessageColumn: return r.message;
        default: return {};
        }
    case Qt::ToolTipRole:
        return r.toText();
    case LevelRole: return int(r.level);
    case SourceRole: return r.source;
    case JobRole: return r.jobId;
    case RecordTextRole: return r.toText();
    case SeqRole: return qulonglong(r.seq);
    default: return {};
    }
}

QVariant LogModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case TimeColumn: return tr("Time");
    case LevelColumn: return tr("Level");
    case SourceColumn: return tr("Source");
    case JobColumn: return tr("Job");
    case MessageColumn: return tr("Message");
    default: return {};
    }
}

void LogModel::clearView()
{
    beginResetModel();
    m_rows.clear();
    endResetModel();
}

QStringList LogModel::sources() const
{
    QStringList s = m_sources;
    s.sort();
    return s;
}

QString LogModel::allText() const
{
    QString out;
    for (const auto& r : m_rows) {
        out += r.toText();
        out += u'\n';
    }
    return out;
}

const LogRecord* LogModel::recordAt(int row) const
{
    return (row >= 0 && row < int(m_rows.size())) ? &m_rows[size_t(row)] : nullptr;
}

// ---------------------------------------------------------------------------

LogFilterModel::LogFilterModel(QObject* parent) : QSortFilterProxyModel(parent) {}

void LogFilterModel::setMinimumLevel(LogLevel level)
{
    m_min = level;
    invalidateFilter();
}

void LogFilterModel::setSource(const QString& source)
{
    m_source = source;
    invalidateFilter();
}

void LogFilterModel::setJobId(const QString& jobId)
{
    m_job = jobId;
    invalidateFilter();
}

void LogFilterModel::setText(const QString& text)
{
    m_text = text;
    invalidateFilter();
}

bool LogFilterModel::filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const
{
    const QModelIndex idx = sourceModel()->index(sourceRow, 0, sourceParent);
    if (sourceModel()->data(idx, LogModel::LevelRole).toInt() < int(m_min))
        return false;
    if (!m_source.isEmpty() && sourceModel()->data(idx, LogModel::SourceRole).toString() != m_source)
        return false;
    if (!m_job.isEmpty() && sourceModel()->data(idx, LogModel::JobRole).toString() != m_job)
        return false;
    if (!m_text.isEmpty()
        && !sourceModel()->data(idx, LogModel::RecordTextRole).toString().contains(m_text, Qt::CaseInsensitive))
        return false;
    return true;
}

} // namespace fw
