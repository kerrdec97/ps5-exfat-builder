#pragma once

// Log view (§4.18): LogTable over LogFilterModel with a column delegate
// (time / level glyph + word / source / message), plus ActivityWell, the
// 4–6 line inset log tail that pages show with "Open in Logs".

#include "app/models/LogModel.h"
#include "app/ui/Widgets.h"

#include <QPlainTextEdit>
#include <QStyledItemDelegate>
#include <QTableView>

namespace fw::ui {

QColor levelColor(LogLevel level);
QString levelIcon(LogLevel level);

class LogDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    explicit LogDelegate(QObject* parent = nullptr);
    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;
};

class LogTable : public QTableView {
    Q_OBJECT
public:
    explicit LogTable(QWidget* parent = nullptr);
    void setFollow(bool follow);
    bool follow() const { return m_follow; }
    void scrollToEnd();

protected:
    void rowsInserted(const QModelIndex& parent, int start, int end) override;

private:
    bool m_follow = true;
};

class ActivityWell : public Card {
    Q_OBJECT
public:
    ActivityWell(LogModel& model, QWidget* parent = nullptr);
    void setJobId(const QString& jobId);     // "" = any job
    QString jobId() const { return m_job; }
    void setSource(const QString& source);   // "" = any source
    void setMaxLines(int n) { m_max = n; refresh(); }
    void setSmall(bool small);               // mono-sm (dock / detail panels)
    void refresh();

protected:
    void showEvent(QShowEvent* e) override;

private:
    LogModel& m_model;
    QString m_job, m_source;
    int m_max = 5;
    bool m_small = false;
    bool m_dirty = false; // changed while hidden: refreshed when shown
    QVBoxLayout* m_lines;
    QWidget* m_emptyLabel; // the compact empty state: a glyph and "Nothing yet."
};

// The live log console of the job monitor: a terminal-like well (bg_0, mono)
// that shows one job's records as they arrive — time, a level tag for
// warnings and errors, the message — follows the newest line unless the user
// scrolled up, and keeps the last `maxLines` lines. Text is selectable.
class LogConsole : public QPlainTextEdit {
    Q_OBJECT
public:
    explicit LogConsole(LogModel& model, QWidget* parent = nullptr);
    void setJobId(const QString& jobId);          // "" = every job (with its source)
    QString jobId() const { return m_job; }
    void setMinimumLevel(LogLevel level);         // default Info; Debug shows the tools' own output
    LogLevel minimumLevel() const { return m_min; }
    void setMaxLines(int n);
    void setLive(bool live);                      // false: lines come from appendLine() only (screenshots)
    bool follow() const { return m_follow; }
    void jumpToLatest();
    int lineCount() const { return m_lines; }
    QString plainLines() const;                   // the shown lines, for Copy
    void appendLine(const QDateTime& time, LogLevel level, const QString& message, const QString& source = {});
    void clearLines();
signals:
    void followChanged(bool follow);
    void lineCountChanged(int count);

private:
    void refill();
    void onAppended(int count);
    bool accepts(const LogRecord& r) const;
    void appendRecord(const LogRecord& r);
    LogModel& m_model;
    QString m_job;
    LogLevel m_min = LogLevel::Info;
    quint64 m_lastSeq = 0;
    bool m_follow = true;
    int m_adjusting = 0; // depth: scroll changes the console makes itself do not end "follow"
    bool m_live = true;
    int m_lines = 0;
    int m_max = 4000;
};

} // namespace fw::ui
