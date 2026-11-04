#pragma once

// Log view (§4.18): LogTable over LogFilterModel with a column delegate
// (time / level glyph + word / source / message), plus ActivityWell, the
// 4–6 line inset log tail that pages show with "Open in Logs".

#include "app/models/LogModel.h"
#include "app/ui/Widgets.h"

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
    void setSource(const QString& source);   // "" = any source
    void setMaxLines(int n) { m_max = n; refresh(); }
    void setSmall(bool small);               // mono-sm (dock / detail panels)
    void refresh();

private:
    LogModel& m_model;
    QString m_job, m_source;
    int m_max = 5;
    bool m_small = false;
    QVBoxLayout* m_lines;
    QLabel* m_emptyLabel;
};

} // namespace fw::ui
