#pragma once

// Home: status strip (console · queue · free space), the PS5 setup checklist
// (services/SetupChecklist.h), Continue card for the last unfinished job,
// Recent jobs from the history.

#include "app/pages/Page.h"
#include "app/services/SetupChecklist.h"

#include <QTableWidget>

#include <optional>

namespace fw::ui {
class PayloadStore;
}

namespace fw::pages {

class HomePage : public Page {
    Q_OBJECT
public:
    HomePage(AppContext& ctx, JobQueueModel& queueModel, QWidget* parent = nullptr);
    void refresh() override;
    void onShown() override { refresh(); }
    void setConsoleStatus(const QString& text, ui::Tone tone);
    // The payload list counts for the checklist's last row.
    void setPayloadStore(ui::PayloadStore* store);
    // Test hooks
    QList<ui::ChecklistItem> checklist() const { return m_checklist; }
    QString checklistProgress() const;

private:
    void refreshChecklist();

    JobQueueModel& m_model;
    QLabel* m_consoleValue;
    ui::Badge* m_consoleBadge;
    QLabel* m_queueValue;
    QLabel* m_queueCaption;
    QLabel* m_spaceValue;
    QLabel* m_spaceCaption;
    ui::Card* m_setup;
    ui::Badge* m_setupProgress;
    QWidget* m_setupRows;
    QList<ui::ChecklistItem> m_checklist;
    std::optional<ui::FtpCheckRecord> m_lastCheck;
    ui::PayloadStore* m_payloads = nullptr;
    bool m_browserConnected = false;
    ui::Card* m_continue;
    QLabel* m_continueTitle;
    QLabel* m_continueMeta;
    QPushButton* m_continueButton;
    QString m_continueId;
    ui::Card* m_recentCard;
    QTableWidget* m_recent;
    ui::EmptyState* m_recentEmpty;
    QLabel* m_recentFooter;
};

} // namespace fw::pages
