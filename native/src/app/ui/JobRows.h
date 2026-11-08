#pragma once

// Queue rows (§4.19): JobRow in its 56 px Queue and 36 px dock variants,
// JobList (a filtered stack of rows bound to JobQueueModel with drag reorder)
// and JobsDock (the 400 px aside on job-creating pages).

#include "app/models/JobQueueModel.h"
#include "app/ui/Widgets.h"

#include <QPointer>
#include <QTimer>
#include <QWidget>

#include <functional>

namespace fw {
class AppContext;
}

namespace fw::ui {

Tone toneForState(JobState state);
QString stateIcon(JobState state);           // row state glyph
QString waitingText(const JobRecord& r, int maxAttempts); // "Retrying in 25 s · attempt 2 of 3" or ""
QString finishedNote(const JobRecord& r);    // "Verified · 12 min 40 s", "2 warnings"

class JobRow : public QWidget {
    Q_OBJECT
public:
    enum class Variant { Queue, Dock };
    JobRow(Variant variant, QWidget* parent = nullptr);
    void setRecord(const JobRecord& r, int queuePosition, int maxAttempts, bool reorderable);
    QString id() const { return m_id; }
    JobState state() const { return m_state; }
    void setSelected(bool on);
    bool isSelected() const { return m_selected; }
    void setDivider(bool on) { m_divider = on; update(); }
    void setCanMoveUp(bool on);
    void setCanMoveDown(bool on);
    QSize sizeHint() const override;
    // A drag of this row is running (its nested loop): the list must not
    // delete the row until it ends.
    bool isBusy() const { return m_dragging; }
signals:
    void clicked(const QString& id);
    void activated(const QString& id);
    void focusNeighbourRequested(const QString& id, int delta); // Up / Down
    void pauseRequested(const QString& id);
    void resumeRequested(const QString& id);
    void cancelRequested(const QString& id);
    void retryRequested(const QString& id);
    void removeRequested(const QString& id);
    void moveUpRequested(const QString& id);
    void moveDownRequested(const QString& id);
    void showInFolderRequested(const QString& id);
    void dragStarted(const QString& id);

protected:
    void paintEvent(QPaintEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void contextMenuEvent(QContextMenuEvent* e) override;

private:
    void showMenu(const QPoint& globalPos);
    void rebuildActions();
    Variant m_variant;
    QString m_id;
    JobState m_state = JobState::Queued;
    JobRecord m_record;
    bool m_selected = false;
    bool m_divider = true;
    bool m_reorderable = false;
    bool m_canUp = true, m_canDown = true;
    bool m_dragging = false;
    QPoint m_pressPos;
    QWidget* m_grip;
    QLabel* m_stateIcon;
    ElideLabel* m_title;
    ElideLabel* m_meta;
    QWidget* m_middle;
    ProgressBar* m_bar;
    QLabel* m_note;
    Badge* m_badge;
    QWidget* m_actions;
    QHBoxLayout* m_actionsLayout;
};

class JobList : public QWidget {
    Q_OBJECT
public:
    JobList(AppContext& ctx, JobQueueModel& model, JobRow::Variant variant, QWidget* parent = nullptr);
    void setFilter(std::function<bool(const JobRecord&)> filter);
    void setSelectedId(const QString& id);
    QString selectedId() const { return m_selected; }
    int visibleCount() const { return int(m_rows.size()); }
    void setMaxRows(int n) { m_maxRows = n; rebuild(); } // dock: show only the first n
    JobQueueModel& model() { return m_model; }
    void setEmptyWidget(QWidget* w);
    QStringList visibleIds() const;
signals:
    void selectionChanged(const QString& id);
    void activated(const QString& id);
    void cancelRequested(const QString& id); // the page confirms, then cancels
    void countChanged();

protected:
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dragMoveEvent(QDragMoveEvent* e) override;
    void dragLeaveEvent(QDragLeaveEvent* e) override;
    void dropEvent(QDropEvent* e) override;
    void paintEvent(QPaintEvent* e) override;

private:
    // Model changes arrive inside the rows' own handlers (a context menu, a
    // drag, a confirm sheet): rebuild() only schedules; rebuildNow() runs on
    // the next event-loop turn, and waits while a row is busy.
    void rebuild();
    void rebuildNow();
    void refreshRow(int modelRow);
    void wire(JobRow* row);
    int dropIndexAt(const QPoint& p) const;
    AppContext& m_ctx;
    JobQueueModel& m_model;
    JobRow::Variant m_variant;
    std::function<bool(const JobRecord&)> m_filter;
    std::vector<JobRow*> m_rows;
    QVBoxLayout* m_layout;
    QString m_selected;
    int m_maxRows = -1;
    int m_dropIndex = -1;
    QWidget* m_empty = nullptr;
    QTimer m_tick;
    bool m_rebuildPending = false;
};

class ActivityWell;

class JobsDock : public Card {
    Q_OBJECT
public:
    JobsDock(AppContext& ctx, JobQueueModel& model, QWidget* parent = nullptr);
    void refresh();
    // The job the page's monitor shows: the dock hides its own running block
    // for it (nothing is shown twice). "" = none.
    void setWatchedJob(const QString& id);
    QString watchedJob() const { return m_watched; }
    QString runningJob() const { return m_runningId; }
signals:
    void openQueueRequested();
    void openJobRequested(const QString& id);
    void cancelRequested(const QString& id); // the page confirms, then cancels
    void watchRequested(const QString& id); // "Watch" on the running block

private:
    AppContext& m_ctx;
    JobQueueModel& m_model;
    Badge* m_count;
    IconButton* m_pause;
    Card* m_running;
    ElideLabel* m_runTitle;
    ElideLabel* m_runMeta;
    ProgressBlock* m_progress;
    ActivityWell* m_tail;
    IconButton* m_runWatch;
    IconButton* m_runPause;
    IconButton* m_runCancel;
    JobList* m_list;
    QLabel* m_footer;
    EmptyState* m_empty;
    QString m_runningId;
    QString m_watched;
};

} // namespace fw::ui
