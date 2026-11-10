#include "app/ui/LogView.h"

#include "app/ui/Icons.h"

#include <QHeaderView>
#include <QPainter>
#include <QScrollBar>
#include <QShowEvent>
#include <QTextBlock>
#include <QTextCursor>
#include <QVBoxLayout>

namespace fw::ui {

using namespace theme;

QColor levelColor(LogLevel level)
{
    switch (level) {
    case LogLevel::Trace:
    case LogLevel::Debug: return color::fg_3;
    case LogLevel::Info: return color::fg_1;
    case LogLevel::Warning: return color::warn_hi;
    case LogLevel::Error: return color::danger_hi;
    }
    return color::fg_1;
}

QString levelIcon(LogLevel level)
{
    switch (level) {
    case LogLevel::Trace:
    case LogLevel::Debug: return QStringLiteral("terminal");
    case LogLevel::Info: return QStringLiteral("info");
    case LogLevel::Warning: return QStringLiteral("warning");
    case LogLevel::Error: return QStringLiteral("error");
    }
    return QStringLiteral("info");
}

static QColor levelGlyphColor(LogLevel level)
{
    switch (level) {
    case LogLevel::Info: return color::accent_hi;
    case LogLevel::Warning: return color::warn;
    case LogLevel::Error: return color::danger;
    default: return color::fg_3;
    }
}

LogDelegate::LogDelegate(QObject* parent) : QStyledItemDelegate(parent) {}

QSize LogDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const
{
    return {QStyledItemDelegate::sizeHint(option, index).width(), size::rowCompact};
}

void LogDelegate::paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const
{
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    if (option.state & QStyle::State_Selected)
        p->fillRect(option.rect, color::accent_lo);
    else if (option.state & QStyle::State_MouseOver)
        p->fillRect(option.rect, color::bg_3);
    p->fillRect(QRect(option.rect.left(), option.rect.bottom(), option.rect.width(), 1), color::border_1);
    const auto level = LogLevel(index.data(LogModel::LevelRole).toInt());
    const QRect r = option.rect.adjusted(index.column() == 0 ? 12 : 8, 0, -8, 0);
    const int col = index.column();
    if (col == LogModel::LevelColumn) {
        const int gy = r.center().y() - size::iconSm / 2;
        p->drawPixmap(QRect(r.left(), gy, size::iconSm, size::iconSm),
                      iconPixmap(levelIcon(level), size::iconSm, levelGlyphColor(level), p->device()->devicePixelRatioF()));
        p->setFont(font::caption());
        p->setPen(level == LogLevel::Info || level == LogLevel::Debug || level == LogLevel::Trace ? color::fg_2 : levelColor(level));
        QString word = index.data().toString();
        if (!word.isEmpty())
            word[0] = word[0].toUpper();
        p->drawText(r.adjusted(size::iconSm + 6, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, word);
    } else {
        p->setFont(font::mono());
        QColor c = color::fg_1;
        if (col == LogModel::TimeColumn)
            c = color::fg_4;
        else if (col == LogModel::SourceColumn || col == LogModel::JobColumn)
            c = color::fg_3;
        else if (col == LogModel::MessageColumn)
            c = levelColor(level);
        p->setPen(c);
        const QString text = index.data().toString().simplified();
        p->drawText(r, Qt::AlignVCenter | Qt::AlignLeft, QFontMetrics(font::mono()).elidedText(text, Qt::ElideRight, r.width()));
    }
    p->restore();
}

LogTable::LogTable(QWidget* parent) : QTableView(parent)
{
    setItemDelegate(new LogDelegate(this));
    setShowGrid(false);
    setAlternatingRowColors(false);
    setSelectionBehavior(QAbstractItemView::SelectRows);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    setMouseTracking(true);
    setWordWrap(false);
    setFocusPolicy(Qt::StrongFocus);
    setProperty("fwInput", false);
    verticalHeader()->hide();
    verticalHeader()->setDefaultSectionSize(size::rowCompact);
    verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    horizontalHeader()->setFont(font::caption());
    horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    horizontalHeader()->setHighlightSections(false);
    horizontalHeader()->setStretchLastSection(true);
    horizontalHeader()->setFixedHeight(32);
    horizontalHeader()->setSectionsClickable(false);
    setColumnWidth(LogModel::TimeColumn, 116);
    setColumnWidth(LogModel::LevelColumn, 92);
    setColumnWidth(LogModel::SourceColumn, 120);
    setColumnWidth(LogModel::JobColumn, 90);
}

void LogTable::setFollow(bool follow)
{
    m_follow = follow;
    if (follow)
        scrollToEnd();
}

void LogTable::scrollToEnd() { scrollToBottom(); }

void LogTable::rowsInserted(const QModelIndex& parent, int start, int end)
{
    QTableView::rowsInserted(parent, start, end);
    if (m_follow)
        QMetaObject::invokeMethod(this, &LogTable::scrollToEnd, Qt::QueuedConnection);
}

// ---- activity well ---------------------------------------------------------------

ActivityWell::ActivityWell(LogModel& model, QWidget* parent) : Card(parent, Card::Variant::Inset), m_model(model)
{
    m_lines = body();
    m_lines->setSpacing(0);
    m_emptyLabel = label(QStringLiteral("Nothing yet."), TextStyle::Mono, color::fg_4, this);
    m_lines->addWidget(m_emptyLabel);
    connect(&m_model, &LogModel::rowsAppended, this, [this](int) { refresh(); });
    connect(&m_model, &QAbstractItemModel::modelReset, this, [this] { refresh(); });
    refresh();
}

void ActivityWell::setJobId(const QString& jobId)
{
    m_job = jobId;
    refresh();
}

void ActivityWell::setSource(const QString& source)
{
    m_source = source;
    refresh();
}

void ActivityWell::setSmall(bool small)
{
    m_small = small;
    refresh();
}

void ActivityWell::showEvent(QShowEvent* e)
{
    Card::showEvent(e);
    if (m_dirty)
        refresh();
}

void ActivityWell::refresh()
{
    // One well per Jobs dock, on every job page: only the one on screen
    // rebuilds its lines on each log flush.
    if (!isVisible()) {
        m_dirty = true;
        return;
    }
    m_dirty = false;
    while (m_lines->count() > 0) {
        QLayoutItem* it = m_lines->takeAt(0);
        if (it->widget() != m_emptyLabel)
            delete it->widget();
        delete it;
    }
    std::vector<const LogRecord*> picked;
    for (int r = m_model.rowCount() - 1; r >= 0 && int(picked.size()) < m_max; --r) {
        const LogRecord* rec = m_model.recordAt(r);
        if (!rec || rec->level < LogLevel::Info)
            continue;
        if (!m_job.isEmpty() && rec->jobId != m_job)
            continue;
        if (!m_source.isEmpty() && !rec->source.startsWith(m_source))
            continue;
        picked.push_back(rec);
    }
    if (picked.empty()) {
        m_lines->addWidget(m_emptyLabel);
        m_emptyLabel->show();
        return;
    }
    m_emptyLabel->hide();
    const QFont f = m_small ? font::monoSmall() : font::mono();
    for (auto it = picked.rbegin(); it != picked.rend(); ++it) {
        const LogRecord* rec = *it;
        auto* row = new QWidget(this);
        auto* h = hbox(row, space::sm);
        auto* t = label(rec->time.toString(QStringLiteral("HH:mm:ss")), m_small ? TextStyle::MonoSmall : TextStyle::Mono,
                        rec->level >= LogLevel::Warning ? levelColor(rec->level) : color::fg_4, row);
        auto* m = new ElideLabel(rec->message.simplified(), Qt::ElideRight, row);
        m->setFont(f);
        setTextColor(m, rec->level >= LogLevel::Warning ? levelColor(rec->level) : color::fg_2);
        h->addWidget(t);
        h->addWidget(m, 1);
        m_lines->addWidget(row);
    }
}

// ---- live console ---------------------------------------------------------------

LogConsole::LogConsole(LogModel& model, QWidget* parent) : QPlainTextEdit(parent), m_model(model)
{
    setObjectName(QStringLiteral("fwConsole"));
    setReadOnly(true);
    setUndoRedoEnabled(false);
    setLineWrapMode(QPlainTextEdit::WidgetWidth);
    setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    setMaximumBlockCount(m_max);
    setFont(font::mono());
    document()->setDefaultFont(font::mono());
    document()->setDocumentMargin(2);
    setFrameShape(QFrame::NoFrame);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    setMinimumHeight(120);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setProperty("fwRadius", radius::control); // it takes keyboard focus: it gets the ring
    setAccessibleName(QStringLiteral("Live log"));
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int value) {
        if (m_adjusting)
            return;
        const bool atEnd = value >= verticalScrollBar()->maximum() - 2;
        if (atEnd != m_follow) {
            m_follow = atEnd;
            emit followChanged(m_follow);
        }
    });
    connect(verticalScrollBar(), &QScrollBar::rangeChanged, this, [this](int, int max) {
        if (!m_follow)
            return;
        ++m_adjusting;
        verticalScrollBar()->setValue(max);
        --m_adjusting;
    });
    connect(&m_model, &LogModel::rowsAppended, this, &LogConsole::onAppended);
    connect(&m_model, &QAbstractItemModel::modelReset, this, [this] {
        if (m_live)
            refill();
    });
    refill();
}

void LogConsole::setJobId(const QString& jobId)
{
    if (m_job == jobId && m_live)
        return;
    m_job = jobId;
    m_live = true;
    refill();
}

void LogConsole::setMinimumLevel(LogLevel level)
{
    if (m_min == level)
        return;
    m_min = level;
    if (m_live)
        refill();
}

void LogConsole::setMaxLines(int n)
{
    m_max = std::max(50, n);
    setMaximumBlockCount(m_max);
}

void LogConsole::setLive(bool live)
{
    if (m_live == live)
        return;
    m_live = live;
    if (live)
        refill();
}

void LogConsole::jumpToLatest()
{
    ++m_adjusting;
    verticalScrollBar()->setValue(verticalScrollBar()->maximum());
    --m_adjusting;
    if (!m_follow) {
        m_follow = true;
        emit followChanged(true);
    }
}

bool LogConsole::accepts(const LogRecord& r) const
{
    if (r.level < m_min)
        return false;
    if (!m_job.isEmpty() && r.jobId != m_job)
        return false;
    return true;
}

void LogConsole::clearLines()
{
    clear();
    m_lines = 0;
    m_lastSeq = 0;
    emit lineCountChanged(0);
}

void LogConsole::refill()
{
    const bool wasFollowing = m_follow;
    ++m_adjusting;
    clear();
    m_lines = 0;
    m_lastSeq = 0;
    // The console keeps the last m_max lines: start at the first row that fits.
    int first = 0;
    {
        int kept = 0;
        for (int r = m_model.rowCount() - 1; r >= 0; --r) {
            const LogRecord* rec = m_model.recordAt(r);
            if (rec && accepts(*rec) && ++kept >= m_max) {
                first = r;
                break;
            }
        }
    }
    for (int r = first; r < m_model.rowCount(); ++r)
        if (const LogRecord* rec = m_model.recordAt(r); rec && accepts(*rec))
            appendRecord(*rec);
    if (const LogRecord* last = m_model.recordAt(m_model.rowCount() - 1))
        m_lastSeq = std::max(m_lastSeq, last->seq);
    --m_adjusting;
    m_follow = true;
    jumpToLatest();
    if (!wasFollowing)
        emit followChanged(true);
    emit lineCountChanged(m_lines);
}

void LogConsole::onAppended(int count)
{
    if (!m_live)
        return;
    const int rows = m_model.rowCount();
    const int before = m_lines;
    for (int r = std::max(0, rows - count); r < rows; ++r) {
        const LogRecord* rec = m_model.recordAt(r);
        if (!rec || rec->seq <= m_lastSeq)
            continue;
        m_lastSeq = rec->seq;
        if (accepts(*rec))
            appendRecord(*rec);
    }
    if (m_lines != before)
        emit lineCountChanged(m_lines);
}

void LogConsole::appendRecord(const LogRecord& r)
{
    appendLine(r.time, r.level, r.message, m_job.isEmpty() ? r.source : QString());
}

void LogConsole::appendLine(const QDateTime& time, LogLevel level, const QString& message, const QString& source)
{
    QTextCursor c(document());
    c.movePosition(QTextCursor::End);
    if (m_lines > 0)
        c.insertBlock();
    QTextCharFormat dim;
    dim.setForeground(color::fg_4);
    dim.setFont(font::mono());
    c.insertText(time.toString(QStringLiteral("HH:mm:ss.zzz")) + QStringLiteral("  "), dim);
    if (!source.isEmpty()) {
        QTextCharFormat src;
        src.setForeground(color::fg_3);
        src.setFont(font::mono());
        c.insertText(QStringLiteral("[%1] ").arg(source), src);
    }
    QTextCharFormat msg;
    msg.setFont(font::mono());
    msg.setForeground(level >= LogLevel::Warning ? levelColor(level) : level == LogLevel::Info ? color::fg_1 : color::fg_3);
    if (level >= LogLevel::Warning) {
        QTextCharFormat tag = msg;
        QFont f = font::mono();
        f.setWeight(QFont::DemiBold);
        tag.setFont(f);
        c.insertText(level == LogLevel::Error ? QStringLiteral("ERROR  ") : QStringLiteral("WARN   "), tag);
    }
    QString text = message;
    text.replace(u'\r', u' ');
    text = text.trimmed();
    // One endless line (tool output without line breaks) is laid out again on
    // every resize: the console shows its start, the Logs page has all of it.
    constexpr int kMaxLineChars = 4000;
    if (text.size() > kMaxLineChars)
        text = text.left(kMaxLineChars) + QStringLiteral("…");
    c.insertText(text, msg);
    // Lines as the document counts them: a message may hold several.
    m_lines = document()->blockCount();
}

QString LogConsole::plainLines() const
{
    return toPlainText();
}

} // namespace fw::ui
