#pragma once

// Queue (Queue artboard): filters + 56 px job rows (drag reorder, per-job
// actions) and the 400 px job details panel (progress, stepper, settings
// with override markers, log tail, outputs, verification, error).

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

    JobQueueModel& m_model;
    LogModel& m_logModel;
    QPushButton* m_pauseQueue;
    QPushButton* m_clearFinished;
    ui::Segmented* m_filter;
    QLabel* m_eta;
    ui::JobList* m_list;
    ui::EmptyState* m_empty;
    // detail panel
    ui::Card* m_detail;
    ui::EmptyState* m_detailEmpty;
    QWidget* m_detailBody;
    ui::ElideLabel* m_dTitle;
    ui::ElideLabel* m_dMeta;
    ui::Badge* m_dBadge;
    ui::ProgressBlock* m_dProgress;
    QLabel* m_dStarted;
    QWidget* m_dSettings;
    QVBoxLayout* m_dSettingsLayout;
    QWidget* m_dResult;
    QVBoxLayout* m_dResultLayout;
    ui::ActivityWell* m_dLog;
    QPushButton* m_dOpenLog;
    QPushButton* m_dAction;      // Cancel job / Retry / Remove
    QPushButton* m_dFolder;      // Show in folder
    QString m_selected;
    QPointer<QWidget> m_demoSheet;
};

} // namespace fw::pages
