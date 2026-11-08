#pragma once

// Table model over the in-memory log (RingBufferSink) for the Logs page and
// per-job log panels. New records arrive batched (every 150 ms) so heavy
// logging never stalls the UI. LogFilterModel filters by level, source, job
// and free text.

#include "core/Log.h"

#include <QAbstractTableModel>
#include <QSortFilterProxyModel>
#include <QTimer>

#include <memory>
#include <vector>

namespace fw {

class AppContext;

class LogModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column { TimeColumn, LevelColumn, SourceColumn, JobColumn, MessageColumn, ColumnCount };
    enum Role { LevelRole = Qt::UserRole + 1, SourceRole, JobRole, RecordTextRole, SeqRole };

    explicit LogModel(AppContext& ctx, QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

    void clearView();               // hides existing rows (does not delete log files)
    QStringList sources() const;    // distinct sources seen
    QString allText() const;        // for copy/export
    const LogRecord* recordAt(int row) const;
    int maxRows() const { return m_maxRows; }

signals:
    void sourcesChanged();
    void rowsAppended(int count);

private:
    void flushPending();

    std::vector<LogRecord> m_rows;
    std::vector<LogRecord> m_pending;
    QTimer m_timer;
    QStringList m_sources;
    int m_maxRows = 20000;
    quint64 m_lastSeq = 0; // the newest record this model holds or has queued
};

class LogFilterModel : public QSortFilterProxyModel {
    Q_OBJECT
public:
    explicit LogFilterModel(QObject* parent = nullptr);
    void setMinimumLevel(LogLevel level);
    void setSource(const QString& source);   // empty = all
    void setJobId(const QString& jobId);     // empty = all
    void setText(const QString& text);

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override;

private:
    LogLevel m_min = LogLevel::Debug;
    QString m_source, m_job, m_text;
};

} // namespace fw
