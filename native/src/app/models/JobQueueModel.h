#pragma once

// List model over fw::JobQueue for the Queue page, the build panel and Home.
// One row per job, in queue order; updates are incremental (dataChanged for
// progress, row insert/remove for membership; a changed order resets once).

#include "jobs/JobQueue.h"

#include <QAbstractListModel>
#include <QStringList>

namespace fw {

class JobQueueModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        KindRole,
        TitleRole,
        SubtitleRole,
        StateRole,          // int(JobState)
        StateTextRole,      // "Running", "Done", ...
        PhaseRole,          // current phase name
        PhaseIndexRole,
        PhaseCountRole,
        FractionRole,       // overall 0..1
        PhaseFractionRole,
        IndeterminateRole,
        DoneRole,
        TotalRole,
        FilesDoneRole,
        FilesTotalRole,
        RateTextRole,       // "418 MB/s"
        EtaTextRole,        // "About 3 min left"
        DetailTextRole,     // "12.4 GB of 48.2 GB · 418 MB/s · About 3 min left"
        CurrentItemRole,
        VerificationRole,   // int(Verification)
        VerificationTextRole,
        SummaryRole,
        ErrorTextRole,
        OutputsRole,        // QStringList
        AddedRole,
        StartedRole,
        FinishedRole,
        ElapsedTextRole,
    };

    explicit JobQueueModel(JobQueue& queue, QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QString idAt(int row) const;
    int rowOf(const QString& id) const;
    const JobRecord* recordAt(int row) const;

    // Counts for headers/badges.
    int countInState(JobState state) const;

signals:
    void countsChanged();

private:
    void reload();
    void insertJob(const QString& id);
    void removeJob(const QString& id);
    void syncOrder(); // after "run next" or a move: one reset, and only when the order differs
    void refresh(const QString& id);

    JobQueue& m_queue;
    std::vector<JobRecord> m_rows;
};

// Human-readable progress detail used by every view.
QString progressDetailText(const ProgressSnapshot& p);

} // namespace fw
