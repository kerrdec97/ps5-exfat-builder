#pragma once

// Logs (Logs artboard): level segmented, source and job filters, search,
// Follow switch, structured rows, selected-line detail, footer; Copy shown,
// Export…, Clear, Open log folder.

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
    QWidget* m_detail;
    QLabel* m_detailMeta;
    QLabel* m_detailText;
    ui::Link* m_openJob;
    ui::ElideLabel* m_footerLeft;
    ui::ElideLabel* m_footerRight;
    QString m_selectedJob;
};

} // namespace fw::pages
