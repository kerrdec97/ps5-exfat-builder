#pragma once

// Logs: one card that fills the page — search, level chips (All / Info /
// Warnings n / Errors n), source and job filters; structured rows; the
// selected line's full text; a footer with the line count and Follow.
// Header: Copy shown, Clear view, Export…, Open log folder.

#include "app/pages/Page.h"
#include "app/ui/LogView.h"

#include <QComboBox>

namespace fw::pages {

class LogsPage : public Page {
    Q_OBJECT
public:
    LogsPage(AppContext& ctx, LogModel& logModel, QWidget* parent = nullptr);
    void navigateWith(const QVariantMap& args) override;
    QString statusContext() const override;
    LogFilterModel* filterModel() { return m_filter; }
    ui::LogTable* table() { return m_table; }
    void setLevelIndex(int index);
    void setSearch(const QString& text);
    void setJobFilter(const QString& jobId);
    void clearFilters();

private:
    void updateFooter();
    void updateDetail();
    void refreshSources();
    void updateCounts();
    QString shownText() const;

    LogModel& m_model;
    LogFilterModel* m_filter;
    ui::Segmented* m_level;
    QComboBox* m_source;
    QComboBox* m_job;
    QLineEdit* m_search;
    ui::Switch* m_follow;
    ui::LogTable* m_table;
    ui::EmptyState* m_noLines;   // filters hide every line
    ui::EmptyState* m_noLog;     // nothing logged (or the view was cleared)
    QWidget* m_detail;
    QWidget* m_detailLine;
    QLabel* m_detailMeta;
    QLabel* m_detailText;
    ui::Link* m_openJob;
    ui::ElideLabel* m_footerLeft;
    QPushButton* m_openFolder;
    QString m_selectedJob;
};

} // namespace fw::pages
