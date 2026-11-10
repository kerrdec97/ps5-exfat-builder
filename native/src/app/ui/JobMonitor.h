#pragma once

// The job monitor: what the user watches while a job runs.
//
// One card that answers "what is it doing, how far is it, how fast, what is
// left, what happened": a state header, the hero progress bar with its
// percentage, the detail line and the file being processed, five live
// metrics (elapsed, remaining, speed with a sparkline, done, stage), the stage
// timeline with the time each stage took, the live log console (every record
// of the job as it is written, warnings and errors tagged, verbose shows the
// tools' own output), the result (verification, warnings, outputs, error) and
// the job's actions.
//
// Wide: the Build / Image tools / DLC pages while the job they started runs
// (the form column gives way to it). Compact: the Queue page's details panel.
// It follows one job id through the queue's signals and a 1 s tick for the
// clocks; setDemoRecord() shows a made-up job for screenshots.

#include "app/models/LogModel.h"
#include "app/ui/LogView.h"
#include "app/ui/Widgets.h"
#include "jobs/JobQueue.h"

#include <QTimer>

#include <deque>
#include <optional>
#include <vector>

class QGridLayout;

namespace fw {
class AppContext;
}

namespace fw::ui {

// "04:12" / "1:04:12"
QString clockText(qint64 ms);
// "<1 s" / "4 s" / "2 min 10 s" / "1 h 05 min"
QString shortDuration(qint64 ms);
// "~45 s" / "~3 min" / "~1 h 05 min"; "" when unknown (ms < 0)
QString shortTimeLeft(qint64 ms);

// A small rate history drawn as a filled line.
class RateSparkline : public QWidget {
    Q_OBJECT
public:
    explicit RateSparkline(QWidget* parent = nullptr);
    void addSample(double value);
    void clear();
    int sampleCount() const { return int(m_samples.size()); }
    QSize sizeHint() const override { return {72, theme::font::lineCaption}; }

protected:
    void paintEvent(QPaintEvent* e) override;

private:
    std::deque<double> m_samples;
    std::size_t m_capacity = 60;
};

// overline + value (+ hint); the Speed tile also holds the sparkline.
class MetricTile : public Card {
    Q_OBJECT
public:
    MetricTile(const QString& overline, QWidget* parent = nullptr);
    void setValue(const QString& value, const QColor& color = theme::color::fg_0);
    void setHint(const QString& hint); // "" hides the line
    QString value() const;
    RateSparkline* sparkline(); // created on first use, right of the value

private:
    QLabel* m_value;
    ElideLabel* m_hint;
    QHBoxLayout* m_row;
    RateSparkline* m_spark = nullptr;
};

class StageMark; // a row's state glyph (JobMonitor.cpp)

// The stage timeline: one row per phase with its state glyph and time.
class StageList : public QWidget {
    Q_OBJECT
public:
    explicit StageList(QWidget* parent = nullptr);
    void setCompact(bool compact) { m_compact = compact; }
    void setStages(const QStringList& names);
    QStringList stages() const { return m_names; }
    // state: the job state; p.phaseIndex is the current stage.
    void update(const ProgressSnapshot& p, JobState state);
    QStringList texts() const; // "Packing · 2 min 10 s · running" per row (tests)
    // The running stage's dot is moving right now (tests): only while the
    // list is on screen, a stage runs and motion is on.
    bool isPulsing() const;

private:
    struct Row {
        QWidget* widget;
        StageMark* icon;
        ElideLabel* name;
        QLabel* time;
        QString status;
        QString shown; // what the row currently paints (icon, colours, time): skip unchanged updates
    };
    QStringList m_names;
    std::vector<Row> m_rows;
    QVBoxLayout* m_layout;
    bool m_compact = false;
};

class JobMonitor : public Card {
    Q_OBJECT
public:
    enum class Variant { Wide, Compact };
    enum Feature { None = 0, ShowRemove = 1, ShowOpenQueue = 2, ShowNewJob = 4 };
    Q_DECLARE_FLAGS(Features, Feature)

    JobMonitor(AppContext& ctx, LogModel& logs, Variant variant, QWidget* parent = nullptr);
    void setFeatures(Features features);
    void setNewJobText(const QString& text); // "New build", "Start another"...
    void setConsoleMinimumHeight(int px);

    void setJobId(const QString& id); // "" clears the monitor
    QString jobId() const { return m_id; }
    // A made-up job for screenshots: the record and the console lines.
    void setDemoRecord(const JobRecord& record, const QStringList& logLines);
    // A made-up FPKG build for demos: state "running" (63 %, packing),
    // "done" (verified) or "failed" (img_verify); its console lines in *logLines.
    static JobRecord demoRecord(const QString& state, QStringList* logLines);
    void refresh();

    // Introspection (tests)
    QString stateText() const;
    QString percentText() const;
    QString detailText() const;
    QStringList stageTexts() const { return m_stages->texts(); }
    bool isStagePulsing() const { return m_stages->isPulsing(); }
    // The result block; its property "fwTinted" says whether it is the
    // success-tinted plate of a job that succeeded.
    QWidget* resultBlock() const { return m_result; }
    QStringList metricValues() const; // elapsed, remaining, speed, done, stage
    LogConsole* console() const { return m_console; }
    QPushButton* cancelButton() const { return m_cancel; }
    QPushButton* pauseButton() const { return m_pause; }
    QPushButton* retryButton() const { return m_retry; }
    QPushButton* folderButton() const { return m_folder; }
    QPushButton* newJobButton() const { return m_newJob; }
    QAbstractButton* verboseSwitch() const;

signals:
    void cancelRequested(const QString& id);
    void openLogsRequested(const QString& id);
    void openQueueRequested(const QString& id);
    void newJobRequested();
    void jobSwitched(const QString& id); // Retry / Run again: the monitor now follows the new job
    // Retry / Run again could not queue the job (source gone, drive unplugged).
    // While nothing is connected the monitor shows the message itself.
    void retryFailed(const QString& id, const QString& message);
    void jobGone();                      // the job left the queue

protected:
    void showEvent(QShowEvent* e) override;
    void hideEvent(QHideEvent* e) override;

private:
    void build();
    void apply(const JobRecord& r);
    void applyEmpty();
    void rebuildResult(const JobRecord& r);
    void updateActions(const JobRecord& r);
    void updateTick(const JobRecord* r);
    void sample(const JobRecord& r);
    void retry();

    AppContext& m_ctx;
    LogModel& m_logs;
    Variant m_variant;
    Features m_features = ShowOpenQueue;
    QString m_id;
    std::optional<JobRecord> m_demo;
    JobState m_lastState = JobState::Queued;
    bool m_hadOutcome = false;
    bool m_hadError = false;
    QString m_phasesKey;
    bool m_stagesSet = false; // the stage list shows the phases of m_id
    qint64 m_lastSampleMs = 0;
    QTimer m_tick;

    IconTile* m_tile;
    ElideLabel* m_title;
    ElideLabel* m_meta;
    Badge* m_badge;
    ProgressBar* m_bar;
    QLabel* m_percent;
    QLabel* m_detail;
    ElideLabel* m_item;
    MetricTile* m_mElapsed;
    MetricTile* m_mLeft;
    MetricTile* m_mSpeed;
    MetricTile* m_mDone;
    MetricTile* m_mStage;
    StageList* m_stages;
    LogConsole* m_console;
    Switch* m_verbose;
    IconButton* m_copy;
    Link* m_openLogs;
    Link* m_jump;
    QLabel* m_lineCount;
    QWidget* m_result;
    QVBoxLayout* m_resultLayout;
    QPushButton* m_pause;
    QPushButton* m_cancel;
    QPushButton* m_folder;
    QPushButton* m_retry;
    QPushButton* m_newJob;
    IconButton* m_remove;
    Link* m_openQueue;
    QLabel* m_note;
};

Q_DECLARE_OPERATORS_FOR_FLAGS(JobMonitor::Features)

} // namespace fw::ui
