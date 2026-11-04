#pragma once

// Home: status strip (console · queue · free space), Continue card for the
// last unfinished job, Recent jobs from the history.

#include "app/pages/Page.h"

#include <QTableWidget>

namespace fw::pages {

class HomePage : public Page {
    Q_OBJECT
public:
    HomePage(AppContext& ctx, JobQueueModel& queueModel, QWidget* parent = nullptr);
    void refresh() override;
    void onShown() override { refresh(); }
    void setConsoleStatus(const QString& text, ui::Tone tone);

private:
    JobQueueModel& m_model;
    QLabel* m_consoleValue;
    ui::Badge* m_consoleBadge;
    QLabel* m_queueValue;
    QLabel* m_queueCaption;
    QLabel* m_spaceValue;
    QLabel* m_spaceCaption;
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
