#pragma once

// Queue: one inviting empty state while nothing is queued; otherwise the job
// list (filter, 56 px rows with drag reorder and per-job actions) and the
// 400 px details panel (state, the job's actions, progress, result, log tail
// and its folded settings).

#include "app/pages/Page.h"
#include "app/ui/LogView.h"

namespace fw::pages {

class QueuePage : public Page {
    Q_OBJECT
public:
    QueuePage(AppContext& ctx, JobQueueModel& queueModel, LogModel& logModel, QWidget* parent = nullptr);
    void navigateWith(const QVariantMap& args) override;
    void refresh() override { updateCounts(); }
    QString statusContext() const override;
    QStringList demoStates() const override { return {QStringLiteral("cancel-dialog")}; }
    void applyDemoState(const QString& name) override;
    void clearDemoState() override;
    // Shows the confirm sheet, then cancels (used by Esc and the dock too).
    void cancelWithConfirm(const QString& id);
    void selectJob(const QString& id);
    ui::JobList* list() { return m_list; }

private:
    void updateCounts();
    void updateDetail();
    void applyFilter(int index);
    void ensureSelection();

    JobQueueModel& m_model;
    LogModel& m_logModel;
    QPushButton* m_pauseQueue;
    QPushButton* m_clearFinished;
    QWidget* m_columns;
    ui::Card* m_emptyCard;
    ui::Segmented* m_filter;
    QLabel* m_eta;
    ui::JobList* m_list;
    ui::EmptyState* m_empty;
    // detail panel
    ui::Card* m_detail;
    QWidget* m_detailBody;
    ui::ElideLabel* m_dTitle;
    ui::ElideLabel* m_dMeta;
    ui::Badge* m_dBadge;
    ui::ProgressBlock* m_dProgress;
    QLabel* m_dStarted;
    QAbstractButton* m_dSettingsToggle;
    QWidget* m_dSettings;
    QVBoxLayout* m_dSettingsLayout;
    QWidget* m_dResult;
    QVBoxLayout* m_dResultLayout;
    ui::ActivityWell* m_dLog;
    QPushButton* m_dPause;       // Pause / Resume (running jobs)
    QPushButton* m_dAction;      // Cancel job / Retry / Run again / Remove
    QPushButton* m_dFolder;      // Show in folder
    ui::IconButton* m_dRemove;   // remove a finished job
    QString m_selected;
    QPointer<QWidget> m_demoSheet;
};

} // namespace fw::pages
