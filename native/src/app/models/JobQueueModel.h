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
        FractionRole,       // overall 0..1 (1 only for a job that succeeded)
        PhaseFractionRole,  // the current stage's share done, 0..1
        IndeterminateRole,
        DoneRole,
        TotalRole,
        FilesDoneRole,
        FilesTotalRole,
        RateTextRole,       // "418 MB/s"
        EtaTextRole,        // "About 3 min left" (the whole job, while it runs)
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

// Human-readable progress detail used by every view: "12.4 GB of 48.2 GB ·
// 1,204 of 18,442 files · 418 MB/s · About 3 min left". The time left is the
// whole job's (ProgressSnapshot::jobEtaMs), or `timeLeftMs` (-1: none).
QString progressDetailText(const ProgressSnapshot& p);
QString progressDetailText(const ProgressSnapshot& p, qint64 timeLeftMs);

// The item a job reports (ProgressSnapshot::currentItem) as the user reads
// it. A composite job (an image conversion, a backport update) reports its
// inner job's stage and item as "<stage> · <item>"; the catalogue holds whole
// texts, so when the whole item is not in it, the stage is looked up alone.
QString currentItemText(const QString& item);

// The overall fraction a view shows for a record: 1 only for a job that
// succeeded (a failed verification can leave the record's fraction at 1).
double shownFraction(const JobRecord& r);
// The stages a record's progress counts in: the reporter's own list once it
// has made the snapshot (a job may declare its phases again while it runs),
// else the phases the job declared when it was queued.
QStringList stageNames(const JobRecord& r);

} // namespace fw
