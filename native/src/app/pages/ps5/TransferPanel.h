#pragma once

// PS5 connect › Files › Transfers: the FTP uploads and downloads of the queue
// (ps5.ftp.upload / ps5.ftp.download) as a transfer list, newest first. Each
// row shows the file being moved, a progress bar, bytes done of total, speed,
// time left and files done; Pause / Resume and Cancel while it runs, Retry
// and the error when it failed, Remove when it ended. "Clear finished"
// removes the ended transfers (only these kinds; other jobs stay).

#include "app/ui/Widgets.h"
#include "jobs/JobQueue.h"

#include <QHash>

#include <vector>

namespace fw {
class AppContext;
}

namespace fw::pages {

class TransferRow : public QWidget {
    Q_OBJECT
public:
    explicit TransferRow(QWidget* parent = nullptr);
    void setRecord(const JobRecord& r);
    QString jobId() const { return m_id; }
    QString statsText() const;
    QString stateText() const;
signals:
    void pauseRequested(const QString& id);
    void resumeRequested(const QString& id);
    void cancelRequested(const QString& id);
    void retryRequested(const QString& id);
    void removeRequested(const QString& id);

private:
    QString m_id;
    ui::IconTile* m_tile;
    ui::ElideLabel* m_title;
    ui::ElideLabel* m_item;
    ui::ProgressBar* m_bar;
    QLabel* m_stats;
    ui::Badge* m_badge;
    ui::IconButton* m_pause;
    ui::IconButton* m_cancel;
    ui::IconButton* m_retry;
    ui::IconButton* m_remove;
};

class TransferPanel : public ui::Card {
    Q_OBJECT
public:
    TransferPanel(AppContext& ctx, QWidget* parent = nullptr);
    int rowCount() const { return int(m_order.size()); }
    int activeCount() const;
    TransferRow* row(int index) const;
    void clearFinished();
    // Screenshots: example rows instead of the queue (labelled by the caller).
    void setSpecimen(const std::vector<JobRecord>& records);
    void clearSpecimen();
    static bool isTransferKind(const QString& kind);

private:
    void sync(const QString& id);
    void drop(const QString& id);
    void updateHeader();
    void reloadAll();

    AppContext& m_ctx;
    QWidget* m_list;
    QVBoxLayout* m_rows;
    QWidget* m_empty;
    ui::Badge* m_summary;
    ui::Link* m_clear;
    QHash<QString, TransferRow*> m_byId;
    std::vector<QString> m_order; // newest first
    std::vector<JobRecord> m_specimen;
    bool m_specimenMode = false;
};

} // namespace fw::pages
