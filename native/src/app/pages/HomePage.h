#pragma once

// Home, the landing page: a hero with the three main actions, status tiles
// (console · queue · free space), a "pick up where you left off" bar for the
// last failed job, quick tools, recent jobs from the history and the PS5
// setup checklist (services/SetupChecklist.h), which folds into a badge on
// the console tile once every step is done.

#include "app/pages/Page.h"
#include "app/services/SetupChecklist.h"

#include <optional>

namespace fw::ui {
class PayloadStore;
class HeroBanner;
class StatTile;
class Disclosure;
class TwoColumnLayout;
} // namespace fw::ui

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
    QString checklistProgress() const { return m_checklistProgress; }

private:
    void refreshChecklist();
    void refreshQueueTile();
    void refreshConsoleTile();
    void refreshRecent();
    void setSetupShown(bool shown);

    JobQueueModel& m_model;
    ui::HeroBanner* m_hero;
    ui::StatTile* m_consoleTile;
    ui::Badge* m_consoleBadge;
    QString m_consoleText;
    ui::Tone m_consoleTone = ui::Tone::Neutral;
    ui::StatTile* m_queueTile;
    ui::ProgressBar* m_queueBar;
    ui::StatTile* m_spaceTile;
    // PS5 setup checklist
    ui::Disclosure* m_setup;
    QWidget* m_setupRows;
    ui::TwoColumnLayout* m_columns;
    bool m_setupShown = true;
    QString m_checklistProgress;
    QList<ui::ChecklistItem> m_checklist;
    std::optional<ui::FtpCheckRecord> m_lastCheck;
    ui::PayloadStore* m_payloads = nullptr;
    bool m_browserConnected = false;
    // Pick up where you left off
    ui::InfoBar* m_continue;
    QString m_continueId;
    QString m_continueDismissed;
    // Recent jobs
    ui::Card* m_recentCard;
    QWidget* m_recentRows;
    ui::EmptyState* m_recentEmpty;
};

} // namespace fw::pages
