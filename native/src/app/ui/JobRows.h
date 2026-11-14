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
#include <optional>

namespace fw {
class AppContext;
}

namespace fw::ui {

Tone toneForState(JobState state);
QString stateIcon(JobState state);           // row state glyph
QString waitingText(const JobRecord& r, int maxAttempts); // "Retrying in 25 s · attempt 2 of 3" or ""
QString finishedNote(const JobRecord& r);    // "Verified · 12 min 40 s", "2 warnings"
// The whole running job's time left, in ms; -1 when unknown or not running.
// The reporter's smoothed estimate (ProgressSnapshot::jobEtaMs; etaMs is the
// current stage only); for a record no reporter made (a demo) a rough one
// from the elapsed time and the share done.
qint64 jobTimeLeftMs(const JobRecord& r);
// What a running job's row says in full (its tooltip): the stage, the units
// and files done, the speed now and on average, and the time left.
QString runningDetailText(const JobRecord& r);

// What the queue's one switch does next, from the queue's state (the user's
// jobs; system jobs do not count):
//   Pause   the queue is not held: "Pause queue"
//   Start   held, jobs wait and nothing runs: "Start queue". That is the list
//           made with "Add to queue", a queue restored when the app starts,
//           and a paused queue whose running job has ended.
//   Resume  held while a job still runs, or with nothing waiting: "Resume queue"
enum class QueueSwitch { Pause, Start, Resume };
QueueSwitch queueSwitch(bool held, int running, int waiting);
// "Start queue (3)"; without the number for less than two waiting jobs.
QString startQueueText(int waiting);
// The end of the header chip while an end-of-queue action is chosen
// (general.afterQueueAction): "then sleep"; "" for none or an unknown name.
QString afterQueueText(const QString& actionName);

// A job's meta line, "Build FPKG package · FPKG · 31.6 GB → D:\PS5\Packages":
// when it does not fit, only the destination is cut, in its middle; the kind,
// the format and the size stay whole. The tooltip has the whole line.
class MetaLine : public QLabel {
    Q_OBJECT
public:
    explicit MetaLine(QWidget* parent = nullptr);
    void setFullText(const QString& text);
    QString fullText() const { return m_text; }
    // Where the destination starts: after the last "→ ", else after the last
    // " · " when what follows is a path; 0 when the whole line is a path.
    static int pathStart(const QString& text);
    QSize minimumSizeHint() const override;

protected:
    void resizeEvent(QResizeEvent* e) override;
    void changeEvent(QEvent* e) override;

private:
    void relayout();
    QString m_text;
};

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
    void enterEvent(QEnterEvent* e) override;
    void leaveEvent(QEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;

private:
    void showMenu(const QPoint& globalPos);
    void rebuildActions();
    void fitMiddle(); // Queue rows: the note column gives way to the title in a narrow list
    QString m_noteText; // the note in full (shown cut to its column)
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
    QPointer<IconButton> m_upButton, m_downButton;
    motion::Value* m_hover; // the hover tint fades
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
    // Screenshot mode only: a made-up job listed first, as if the queue held
    // it (the filter applies); std::nullopt takes it away.
    void setDemoRecord(const std::optional<JobRecord>& record);
signals:
    void selectionChanged(const QString& id);
    void activated(const QString& id);
    void cancelRequested(const QString& id); // the page confirms, then cancels
    // Retry / Run again could not queue the job (source gone, drive unplugged).
    // While nothing is connected the list shows the message itself.
    void retryFailed(const QString& id, const QString& message);
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
    std::optional<JobRecord> m_demo; // screenshot mode
};

class ActivityWell;

class JobsDock : public Card {
    Q_OBJECT
public:
    JobsDock(AppContext& ctx, JobQueueModel& model, QWidget* parent = nullptr);
    void refresh();
    JobList* list() const { return m_list; } // the rows under the running block (tests)
    QPushButton* startQueueButton() const { return m_start; }
    QString footerText() const;    // "3 waiting · then sleep" (tests)
    // The job the page's monitor shows: the dock hides its own running block
    // for it (nothing is shown twice). "" = none.
    void setWatchedJob(const QString& id);
    QString watchedJob() const { return m_watched; }
    QString runningJob() const { return m_runningId; }
    // Screenshot mode only: the made-up job a page's monitor shows (running
    // or finished) counts as a job of the queue, so the dock says what it
    // would say for it: the count, the time left, its row once it has ended.
    // A record with an empty id takes it away.
    void setDemoRunning(const JobRecord& record);
signals:
    void openQueueRequested();
    void openJobRequested(const QString& id);
    void cancelRequested(const QString& id); // the page confirms, then cancels
    void watchRequested(const QString& id); // "Watch" on the running block

private:
    void applyListFilter();
    AppContext& m_ctx;
    JobQueueModel& m_model;
    Badge* m_count;
    IconButton* m_pause;
    QPushButton* m_start; // "Start queue (n)", while a list waits to be started
    Card* m_running;
    ElideLabel* m_runTitle;
    MetaLine* m_runMeta;
    ProgressBlock* m_progress;
    ActivityWell* m_tail;
    IconButton* m_runWatch;
    IconButton* m_runPause;
    IconButton* m_runCancel;
    JobList* m_list;
    QLabel* m_footer; // ends in "· then sleep" while an end-of-queue action is chosen
    EmptyState* m_empty;
    QString m_runningId;
    QString m_watched;
    std::optional<JobRecord> m_demo; // screenshot mode
};

} // namespace fw::ui
