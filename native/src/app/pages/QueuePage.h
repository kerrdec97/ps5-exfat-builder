#pragma once

// Queue: "When the queue finishes" (what the computer does after the run, and
// a note while an action is armed); one inviting empty state while nothing is
// queued; otherwise the job list (filter, 56 px rows with drag reorder and
// per-job actions) and the details column: the job monitor (state, hero
// progress, clocks, stages, live log, result, actions) and the job's folded
// settings.

#include "app/pages/Page.h"
#include "app/ui/JobMonitor.h"
#include "app/ui/LogView.h"

#include <optional>
#include <vector>

namespace fw::ui {
class SettingEditor;
}

namespace fw::pages {

class QueuePage : public Page {
    Q_OBJECT
public:
    QueuePage(AppContext& ctx, JobQueueModel& queueModel, LogModel& logModel, QWidget* parent = nullptr);
    void navigateWith(const QVariantMap& args) override;
    void refresh() override { updateCounts(); }
    QString statusContext() const override;
    QStringList demoStates() const override { return {QStringLiteral("job-running"), QStringLiteral("job-done"), QStringLiteral("cancel-dialog")}; }
    void applyDemoState(const QString& name) override;
    void clearDemoState() override;
    // Shows the confirm sheet, then cancels (used by Esc and the dock too).
    void cancelWithConfirm(const QString& id);
    void selectJob(const QString& id);
    ui::JobList* list() { return m_list; }
    ui::JobMonitor* monitor() { return m_monitor; }
    QPushButton* queueSwitchButton() { return m_pauseQueue; } // "Start queue" / "Resume queue" / "Pause queue"

private:
    void updateCounts();
    void updateAfterQueue(); // "When the queue finishes": the choice and the note while one is armed
    void updateEta();   // the time-left caption (every progress tick)
    void updateDetail();
    void applyFilter(int index);
    void ensureSelection();
    // The user's jobs of the queue (system jobs left out), and in screenshot
    // mode the made-up job first.
    std::vector<const JobRecord*> listedJobs() const;

    JobQueueModel& m_model;
    LogModel& m_logModel;
    QPushButton* m_pauseQueue;
    QPushButton* m_clearFinished;
    ui::SettingEditor* m_afterAction = nullptr; // general.afterQueueAction, for the next run
    ui::MessageRow* m_afterNote = nullptr;
    QWidget* m_columns;
    ui::Card* m_emptyCard;
    ui::Segmented* m_filter;
    QLabel* m_eta;
    ui::JobList* m_list;
    ui::EmptyState* m_empty;
    // details column
    QWidget* m_detail;
    ui::JobMonitor* m_monitor;
    ui::Card* m_settingsCard;
    QAbstractButton* m_dSettingsToggle;
    QWidget* m_dSettings;
    QVBoxLayout* m_dSettingsLayout;
    QString m_settingsFor; // the job whose settings rows are built
    QString m_selected;
    bool m_demo = false;   // a made-up job in the details column (screenshots)
    std::optional<JobRecord> m_demoRecord; // that job, also listed and counted
    QPointer<QWidget> m_demoSheet;
};

} // namespace fw::pages
