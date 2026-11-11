#include "app/ui/JobMonitor.h"

#include "app/AppContext.h"
#include "app/models/JobQueueModel.h"
#include "app/ui/Icons.h"
#include "app/ui/JobRows.h"
#include "app/ui/Motion.h"
#include "app/ui/Sheet.h"
#include "core/FileUtil.h"
#include "core/Tr.h"
#include "engines/general/Module.h"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QMetaMethod>
#include <QPainter>
#include <QShowEvent>
#include <QPainterPath>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace fw::ui {

using namespace theme;

// ---- formatters -----------------------------------------------------------------

QString clockText(qint64 ms)
{
    const qint64 s = std::max<qint64>(0, ms) / 1000;
    const qint64 h = s / 3600, m = (s % 3600) / 60, sec = s % 60;
    if (h > 0)
        return QStringLiteral("%1:%2:%3").arg(h).arg(m, 2, 10, QLatin1Char('0')).arg(sec, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1:%2").arg(m, 2, 10, QLatin1Char('0')).arg(sec, 2, 10, QLatin1Char('0'));
}

QString shortDuration(qint64 ms)
{
    if (ms < 1000)
        return QStringLiteral("<1 s");
    const qint64 s = ms / 1000;
    if (s < 60)
        return QStringLiteral("%1 s").arg(s);
    if (s < 3600)
        return s % 60 == 0 ? QStringLiteral("%1 min").arg(s / 60) : QStringLiteral("%1 min %2 s").arg(s / 60).arg(s % 60);
    return QStringLiteral("%1 h %2 min").arg(s / 3600).arg((s % 3600) / 60, 2, 10, QLatin1Char('0'));
}

QString shortTimeLeft(qint64 ms)
{
    if (ms < 0)
        return {};
    const qint64 s = (ms + 500) / 1000;
    if (s < 60)
        return QStringLiteral("~%1 s").arg(std::max<qint64>(1, s));
    const qint64 min = (s + 30) / 60;
    if (min < 60)
        return QStringLiteral("~%1 min").arg(min);
    return QStringLiteral("~%1 h %2 min").arg(min / 60).arg(min % 60, 2, 10, QLatin1Char('0'));
}

namespace {

const QString kDash = QStringLiteral("—");

QFont metricFont()
{
    QFont f = font::stat();
    f.setPixelSize(17);
    return f;
}

// The hero figure: the display font (tabular digits, so it does not shake as
// it counts), sized for the card.
QFont percentFont(JobMonitor::Variant v)
{
    QFont f = font::display();
    f.setPixelSize(v == JobMonitor::Variant::Wide ? 28 : 18);
    return f;
}

bool active(JobState s) { return isActive(s); }

QString stateIconFor(JobState s)
{
    switch (s) {
    case JobState::Queued: return QStringLiteral("clock");
    case JobState::Running: return QStringLiteral("play");
    case JobState::Paused: return QStringLiteral("pause");
    case JobState::Cancelling: return QStringLiteral("stop_circle");
    case JobState::Succeeded: return QStringLiteral("check");
    case JobState::Failed: return QStringLiteral("error");
    case JobState::Cancelled: return QStringLiteral("ban");
    case JobState::Interrupted: return QStringLiteral("warning");
    }
    return QStringLiteral("clock");
}

qint64 elapsedMsOf(const JobRecord& r)
{
    if (r.started.isValid()) {
        const QDateTime end = r.finished.isValid() ? r.finished : QDateTime::currentDateTime();
        return std::max<qint64>(0, r.started.msecsTo(end));
    }
    return r.progress.elapsedMs;
}

} // namespace

// ---- sparkline --------------------------------------------------------------------

RateSparkline::RateSparkline(QWidget* parent) : QWidget(parent)
{
    setFixedHeight(font::lineCaption);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setAttribute(Qt::WA_TransparentForMouseEvents);
}

void RateSparkline::addSample(double value)
{
    m_samples.push_back(std::max(0.0, value));
    while (m_samples.size() > m_capacity)
        m_samples.pop_front();
    update();
}

void RateSparkline::clear()
{
    m_samples.clear();
    update();
}

void RateSparkline::paintEvent(QPaintEvent*)
{
    if (m_samples.size() < 2)
        return;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const double peak = std::max(1.0, *std::max_element(m_samples.begin(), m_samples.end()));
    const QRectF r = QRectF(rect()).adjusted(1, 2, -1, -1);
    const double step = r.width() / double(m_capacity - 1);
    const double x0 = r.right() - step * double(m_samples.size() - 1);
    QPainterPath line;
    for (std::size_t i = 0; i < m_samples.size(); ++i) {
        const QPointF pt(x0 + step * double(i), r.bottom() - r.height() * (m_samples[i] / peak));
        if (i == 0)
            line.moveTo(pt);
        else
            line.lineTo(pt);
    }
    QPainterPath fill = line;
    fill.lineTo(r.right(), r.bottom());
    fill.lineTo(x0, r.bottom());
    fill.closeSubpath();
    QLinearGradient g(r.topLeft(), r.bottomLeft());
    QColor a = color::accent_hi, b = color::accent;
    a.setAlpha(90);
    b.setAlpha(0);
    g.setColorAt(0, a);
    g.setColorAt(1, b);
    p.fillPath(fill, g);
    p.setPen(QPen(color::accent_hi, 1.5));
    p.setBrush(Qt::NoBrush);
    p.drawPath(line);
}

// ---- metric tile ------------------------------------------------------------------

MetricTile::MetricTile(const QString& overline, QWidget* parent) : Card(parent, Card::Variant::Inset)
{
    body()->setSpacing(2);
    body()->setContentsMargins(0, 2, 0, 2);
    body()->addWidget(label(overline, TextStyle::Overline, color::fg_4, this));
    auto* row = new QWidget(this);
    m_row = hbox(row, space::sm);
    m_value = label(kDash, TextStyle::Body, color::fg_0, row);
    m_value->setFont(metricFont());
    m_value->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_row->addWidget(m_value, 1);
    body()->addWidget(row);
    m_hint = new ElideLabel({}, Qt::ElideRight, this);
    m_hint->setFont(font::caption());
    setTextColor(m_hint, color::fg_4);
    m_hint->setFixedHeight(font::lineCaption);
    body()->addWidget(m_hint);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setMinimumWidth(96);
}

void MetricTile::setValue(const QString& value, const QColor& c)
{
    m_value->setText(value.isEmpty() ? kDash : value);
    setTextColor(m_value, c);
}

void MetricTile::setHint(const QString& hint)
{
    // The line keeps its height so the tiles of a row stay level.
    m_hint->setFullText(hint);
    m_hint->setToolTip(plainTip(hint));
}

QString MetricTile::value() const { return m_value->text(); }

RateSparkline* MetricTile::sparkline()
{
    if (!m_spark) {
        // Under the value, as wide as the tile: the value never has to make room.
        m_spark = new RateSparkline(this);
        m_hint->hide();
        body()->addWidget(m_spark);
    }
    return m_spark;
}

// ---- stage list ---------------------------------------------------------------------

// The state mark of one stage row: an icon, or, for the stage that is running,
// an accent dot whose halo swings slowly. It is the one thing that moves in
// the list, and it rests while the row is hidden or motion is off.
class StageMark : public QWidget {
public:
    explicit StageMark(QWidget* parent) : QWidget(parent)
    {
        setFixedSize(14, 14);
        setAttribute(Qt::WA_TransparentForMouseEvents);
    }
    void set(const QString& icon, const QColor& color, bool running)
    {
        m_icon = icon;
        m_color = color;
        m_running = running;
        if (running && !m_pulse)
            m_pulse = new motion::Pulse(this);
        if (m_pulse)
            m_pulse->setActive(running);
        update();
    }
    bool isPulsing() const { return m_pulse && m_pulse->isRunning(); }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        if (!m_running) {
            p.drawPixmap(rect(), iconPixmap(m_icon, 14, m_color, devicePixelRatioF()));
            return;
        }
        p.setRenderHint(QPainter::Antialiasing);
        const QPointF c = QRectF(rect()).center();
        const qreal s = m_pulse ? m_pulse->value() : 0.0;
        // The halo grows and thins out, then comes back.
        p.setPen(Qt::NoPen);
        p.setBrush(withAlpha(color::accent, int(96 - 72 * s)));
        p.drawEllipse(c, 4.5 + 2.5 * s, 4.5 + 2.5 * s);
        p.setBrush(m_color);
        p.drawEllipse(c, 3.5, 3.5);
    }

private:
    QString m_icon;
    QColor m_color;
    bool m_running = false;
    motion::Pulse* m_pulse = nullptr;
};

StageList::StageList(QWidget* parent) : QWidget(parent)
{
    m_layout = vbox(this, 0);
    setProperty("fwNoRing", true);
}

bool StageList::isPulsing() const
{
    return std::any_of(m_rows.begin(), m_rows.end(), [](const Row& r) { return r.icon->isPulsing(); });
}

void StageList::setStages(const QStringList& names)
{
    if (names == m_names && !m_rows.empty())
        return;
    m_names = names;
    m_rows.clear();
    while (QLayoutItem* it = m_layout->takeAt(0)) {
        delete it->widget(); // rows and the "No stages" label alike
        delete it;
    }
    for (const QString& n : names) {
        Row row;
        row.widget = new QWidget(this);
        row.widget->setFixedHeight(m_compact ? 22 : 26);
        auto* h = hbox(row.widget, space::sm);
        row.icon = new StageMark(row.widget);
        const QString shownName = i18n::translate(n); // stage names are the engines' English
        row.name = new ElideLabel(shownName, Qt::ElideRight, row.widget);
        row.name->setFont(font::body());
        row.name->setToolTip(plainTip(shownName));
        row.time = label({}, TextStyle::MonoSmall, color::fg_3, row.widget);
        row.time->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        row.time->setMinimumWidth(QFontMetrics(font::monoSmall()).horizontalAdvance(QStringLiteral("12 min 40 s")));
        h->addWidget(row.icon);
        h->addWidget(row.name, 1);
        h->addWidget(row.time);
        m_layout->addWidget(row.widget);
        m_rows.push_back(row);
    }
    if (names.isEmpty())
        m_layout->addWidget(label(FW_TR("No stages"), TextStyle::Caption, color::fg_4, this));
}

void StageList::update(const ProgressSnapshot& p, JobState state)
{
    const bool started = p.elapsedMs > 0 || active(state) || (isTerminal(state) && state != JobState::Interrupted && !p.phaseMs.isEmpty() && p.phaseMs.value(0) > 0);
    const int current = state == JobState::Succeeded ? int(m_rows.size()) : p.phaseIndex;
    const bool failed = state == JobState::Failed || state == JobState::Interrupted;
    const bool stopped = state == JobState::Cancelled;
    for (int i = 0; i < int(m_rows.size()); ++i) {
        Row& row = m_rows[size_t(i)];
        const qint64 ms = p.phaseMs.value(i, 0);
        QString icon = QStringLiteral("circle");
        QColor c = color::fg_5;
        QColor nameColor = color::fg_4;
        QFont f = font::body();
        QString time;
        QColor timeColor = color::fg_3;
        if (started && i < current) {
            icon = QStringLiteral("check");
            c = color::success_hi;
            nameColor = color::fg_2;
            time = ms > 0 ? shortDuration(ms) : FW_TR("skipped");
            if (ms <= 0)
                timeColor = color::fg_4; // a real caption, not a disabled one
            row.status = QStringLiteral("done");
        } else if (started && i == current && (active(state) || failed || stopped)) {
            if (failed) {
                icon = QStringLiteral("error");
                c = color::danger;
                nameColor = color::danger_hi;
                row.status = QStringLiteral("failed");
            } else if (stopped) {
                icon = QStringLiteral("ban");
                c = color::fg_3;
                nameColor = color::fg_2;
                row.status = QStringLiteral("cancelled");
            } else if (state == JobState::Paused) {
                icon = QStringLiteral("pause_circle");
                c = color::fg_3;
                nameColor = color::fg_0;
                row.status = QStringLiteral("paused");
            } else if (state == JobState::Cancelling) {
                icon = QStringLiteral("stop_circle");
                c = color::fg_3;
                nameColor = color::fg_1;
                row.status = QStringLiteral("cancelling");
            } else {
                icon = QStringLiteral("circle_filled");
                c = color::accent_hi;
                nameColor = color::fg_0;
                timeColor = color::accent_hi;
                row.status = QStringLiteral("running");
            }
            f = font::bodyStrong();
            time = shortDuration(ms);
        } else {
            row.status = QStringLiteral("pending");
        }
        // Ten updates a second while a job runs: repaint a row only when it changed.
        const QString shown = icon + u'|' + c.name() + u'|' + nameColor.name() + u'|' + time + u'|' + timeColor.name() + u'|' + (f.weight() == QFont::Normal ? u'n' : u'b');
        if (shown == row.shown)
            continue;
        row.shown = shown;
        row.icon->set(icon, c, row.status == QLatin1String("running"));
        row.name->setFont(f);
        setTextColor(row.name, nameColor);
        row.time->setText(time);
        setTextColor(row.time, timeColor);
    }
}

QStringList StageList::texts() const
{
    QStringList out;
    for (const auto& r : m_rows)
        out << QStringLiteral("%1 · %2 · %3").arg(r.name->fullText(), r.time->text().isEmpty() ? kDash : r.time->text(), r.status.isEmpty() ? QStringLiteral("pending") : r.status);
    return out;
}

// ---- monitor ------------------------------------------------------------------------

namespace {

// The result block. For a job that succeeded it is a success-tinted plate
// around what the job made and what the verification found; otherwise it is
// not painted at all (a failure brings its own danger-tinted bar).
class ResultPlate : public QWidget {
public:
    using QWidget::QWidget;
    void setTinted(bool on)
    {
        if (on == m_tinted)
            return;
        m_tinted = on;
        setProperty("fwTinted", on);
        if (layout())
            layout()->setContentsMargins(on ? QMargins(space::md, space::sm, space::md, space::sm) : QMargins());
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        if (!m_tinted)
            return;
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(toneLine(Tone::Ok), 1));
        p.setBrush(toneTint(Tone::Ok));
        p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), radius::md, radius::md);
    }

private:
    bool m_tinted = false;
};

} // namespace

JobMonitor::JobMonitor(AppContext& ctx, LogModel& logs, Variant variant, QWidget* parent)
    : Card(parent, Card::Variant::Regular), m_ctx(ctx), m_logs(logs), m_variant(variant)
{
    setObjectName(QStringLiteral("job-monitor"));
    build();
    m_tick.setInterval(1000);
    connect(&m_tick, &QTimer::timeout, this, &JobMonitor::refresh);
    connect(&m_ctx.queue(), &JobQueue::jobChanged, this, [this](const QString& id) {
        if (!m_demo && id == m_id)
            refresh();
    });
    connect(&m_ctx.queue(), &JobQueue::jobFinished, this, [this](const QString& id) {
        if (!m_demo && id == m_id)
            refresh();
    });
    connect(&m_ctx.queue(), &JobQueue::jobRemoved, this, [this](const QString& id) {
        if (!m_demo && id == m_id) {
            setJobId({});
            emit jobGone();
        }
    });
    // Nothing is followed yet: an empty console, not every job's lines.
    m_console->setLive(false);
    m_console->clearLines();
    applyEmpty();
}

void JobMonitor::build()
{
    const bool wide = m_variant == Variant::Wide;
    auto* l = body();
    l->setSpacing(space::md);

    // ---- header: tile, title/meta, state badge
    auto* head = new QWidget(this);
    auto* hl = hbox(head, space::md);
    m_tile = new IconTile(QStringLiteral("play"), wide ? 40 : 32, head);
    hl->addWidget(m_tile, 0, Qt::AlignTop);
    auto* tcol = new QVBoxLayout;
    tcol->setSpacing(2);
    tcol->setContentsMargins(0, 0, 0, 0);
    m_title = new ElideLabel({}, Qt::ElideRight, head);
    m_title->setFont(wide ? font::title() : font::subtitle());
    setTextColor(m_title, color::fg_0);
    m_meta = new ElideLabel({}, Qt::ElideMiddle, head);
    m_meta->setFont(font::mono());
    setTextColor(m_meta, color::fg_3);
    tcol->addWidget(m_title);
    tcol->addWidget(m_meta);
    hl->addLayout(tcol, 1);
    m_badge = new Badge({}, Tone::Neutral, head);
    hl->addWidget(m_badge, 0, Qt::AlignTop);
    l->addWidget(head);

    // ---- hero progress
    auto* prog = new QWidget(this);
    auto* pl = vbox(prog, 6);
    auto* barRow = new QHBoxLayout;
    barRow->setSpacing(space::md);
    barRow->setContentsMargins(0, 0, 0, 0);
    m_bar = new ProgressBar(prog);
    m_bar->setFixedHeight(wide ? 8 : size::progress);
    barRow->addWidget(m_bar, 1);
    m_percent = label(thinPercent(0.0), TextStyle::Body, color::fg_0, prog);
    m_percent->setFont(percentFont(m_variant));
    m_percent->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_percent->setMinimumWidth(QFontMetrics(percentFont(m_variant)).horizontalAdvance(thinPercent(1.0)));
    barRow->addWidget(m_percent);
    pl->addLayout(barRow);
    m_detail = label({}, TextStyle::Body, color::fg_1, prog);
    m_detail->setWordWrap(true);
    pl->addWidget(m_detail);
    m_item = new ElideLabel({}, Qt::ElideMiddle, prog);
    m_item->setFont(font::monoSmall());
    setTextColor(m_item, color::fg_3);
    pl->addWidget(m_item);
    l->addWidget(prog);

    // ---- metrics
    auto* metrics = new QWidget(this);
    m_mElapsed = new MetricTile(FW_TR("Elapsed"), metrics);
    m_mLeft = new MetricTile(FW_TR("Remaining"), metrics);
    m_mSpeed = new MetricTile(FW_TR("Speed"), metrics);
    m_mDone = new MetricTile(FW_TR("Done"), metrics);
    m_mStage = new MetricTile(FW_TR("Stage"), metrics);
    m_mSpeed->sparkline();
    if (wide) {
        auto* ml = hbox(metrics, space::sm);
        for (MetricTile* t : {m_mElapsed, m_mLeft, m_mSpeed, m_mDone, m_mStage})
            ml->addWidget(t, 1);
    } else {
        auto* g = new QGridLayout(metrics);
        g->setSpacing(space::sm);
        g->setContentsMargins(0, 0, 0, 0);
        g->addWidget(m_mElapsed, 0, 0);
        g->addWidget(m_mLeft, 0, 1);
        g->addWidget(m_mSpeed, 1, 0);
        g->addWidget(m_mDone, 1, 1);
        m_mStage->hide(); // the stage list right below says it
    }
    l->addWidget(metrics);

    // ---- stages | live log
    auto* stagesPanel = new QWidget(this);
    auto* sl = vbox(stagesPanel, space::sm);
    sl->addWidget(label(FW_TR("Stages"), TextStyle::Overline, color::fg_4, stagesPanel));
    m_stages = new StageList(stagesPanel);
    m_stages->setCompact(!wide);
    sl->addWidget(m_stages);
    sl->addStretch(1);

    auto* logPanel = new QWidget(this);
    auto* ll = vbox(logPanel, space::sm);
    auto* logHead = new QWidget(logPanel);
    auto* lh = hbox(logHead, space::sm);
    lh->addWidget(label(FW_TR("Live log"), TextStyle::Overline, color::fg_4, logHead));
    m_lineCount = label({}, TextStyle::Caption, color::fg_4, logHead);
    lh->addWidget(m_lineCount);
    lh->addStretch(1);
    auto* verboseLabel = label(FW_TR("Verbose"), TextStyle::Caption, color::fg_3, logHead);
    verboseLabel->setToolTip(FW_TR("Also show the tools' own output and debug lines."));
    m_verbose = new Switch(logHead);
    m_verbose->setAccessibleName(FW_TR("Verbose log"));
    m_verbose->setToolTip(verboseLabel->toolTip());
    lh->addWidget(verboseLabel);
    lh->addWidget(m_verbose);
    m_copy = new IconButton(QStringLiteral("copy"), FW_TR("Copy these lines"), IconButton::Size::Small, logHead);
    lh->addWidget(m_copy);
    m_openLogs = new Link(FW_TR("Open in Logs"), logHead);
    lh->addWidget(m_openLogs);
    ll->addWidget(logHead);
    m_console = new LogConsole(m_logs, logPanel);
    m_console->setMinimumHeight(wide ? 240 : 170);
    ll->addWidget(m_console, 1);
    auto* jumpRow = new QWidget(logPanel);
    auto* jl = hbox(jumpRow, space::sm);
    jl->addStretch(1);
    m_jump = new Link(FW_TR("Jump to the latest line ↓"), jumpRow);
    m_jump->setFont(font::caption());
    jl->addWidget(m_jump);
    jumpRow->setVisible(false);
    ll->addWidget(jumpRow);
    connect(m_console, &LogConsole::followChanged, jumpRow, [jumpRow](bool follow) { jumpRow->setVisible(!follow); });
    connect(m_jump, &Link::activated, m_console, &LogConsole::jumpToLatest);
    connect(m_console, &LogConsole::lineCountChanged, this, [this](int n) {
        m_lineCount->setText(n <= 0 ? QString() : n == 1 ? FW_TR("%1 line").arg(formatCount(n)) : FW_TR("%1 lines").arg(formatCount(n)));
    });
    connect(m_verbose, &QAbstractButton::toggled, this, [this](bool on) { m_console->setMinimumLevel(on ? LogLevel::Debug : LogLevel::Info); });
    connect(m_copy, &QToolButton::clicked, this, [this] {
        QApplication::clipboard()->setText(m_console->plainLines());
        m_copy->setToolTip(FW_TR("Copied"));
        QTimer::singleShot(2000, m_copy, [this] { m_copy->setToolTip(FW_TR("Copy these lines")); });
    });
    connect(m_openLogs, &Link::activated, this, [this] { emit openLogsRequested(m_id); });

    if (wide) {
        auto* bodyRow = new QWidget(this);
        auto* bl = hbox(bodyRow, space::xl);
        bl->setAlignment(Qt::AlignTop);
        stagesPanel->setFixedWidth(272);
        bl->addWidget(stagesPanel);
        bl->addWidget(logPanel, 1);
        l->addWidget(bodyRow);
    } else {
        l->addWidget(stagesPanel);
        l->addWidget(logPanel);
    }

    // ---- result
    m_result = new ResultPlate(this);
    m_resultLayout = vbox(m_result, 4);
    m_result->hide();
    l->addWidget(m_result);

    // ---- actions
    l->addWidget(hairline(this));
    auto* foot = new QWidget(this);
    auto* fl = hbox(foot, space::sm);
    m_pause = button(FW_TR("Pause"), ButtonVariant::Secondary, QStringLiteral("pause"), foot);
    m_cancel = button(FW_TR("Cancel"), ButtonVariant::Secondary, QStringLiteral("close"), foot);
    m_folder = button(FW_TR("Show in folder"), ButtonVariant::Primary, QStringLiteral("folder_open"), foot);
    m_retry = button(FW_TR("Retry"), ButtonVariant::Primary, QStringLiteral("undo"), foot);
    m_newJob = button(FW_TR("Start another"), ButtonVariant::Secondary, QStringLiteral("plus"), foot);
    m_remove = new IconButton(QStringLiteral("trash"), FW_TR("Remove from queue"), IconButton::Size::Regular, foot);
    m_openQueue = new Link(FW_TR("Open queue"), foot);
    for (QPushButton* b : {m_pause, m_cancel, m_folder, m_retry, m_newJob})
        setButtonSmall(b, !wide);
    fl->addWidget(m_pause);
    fl->addWidget(m_cancel);
    fl->addWidget(m_folder);
    fl->addWidget(m_retry);
    fl->addWidget(m_newJob);
    fl->addWidget(m_openQueue);
    fl->addStretch(1);
    m_note = label({}, TextStyle::Caption, color::fg_4, foot);
    m_note->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    fl->addWidget(m_note);
    fl->addWidget(m_remove);
    l->addWidget(foot);

    connect(m_pause, &QPushButton::clicked, this, [this] {
        auto rec = m_ctx.queue().record(m_id);
        if (!rec)
            return;
        if (rec->state == JobState::Paused)
            m_ctx.queue().resumeJob(m_id);
        else if (rec->state == JobState::Running)
            m_ctx.queue().pauseJob(m_id);
    });
    connect(m_cancel, &QPushButton::clicked, this, [this] { emit cancelRequested(m_id); });
    connect(m_folder, &QPushButton::clicked, this, [this] {
        if (auto rec = m_ctx.queue().record(m_id); rec && rec->outcome && !rec->outcome->outputs.isEmpty()) {
            const QString path = rec->outcome->outputs.first();
            if (!openInFileManager(path))
                Sheet::notify(window(), FW_TR("The output is not there any more"),
                              FW_TR("%1 was moved or deleted, and so was its folder.").arg(nativePath(path)), Sheet::Icon::Warning);
        }
    });
    connect(m_retry, &QPushButton::clicked, this, &JobMonitor::retry);
    connect(m_newJob, &QPushButton::clicked, this, [this] { emit newJobRequested(); });
    connect(m_remove, &QToolButton::clicked, this, [this] {
        auto rec = m_ctx.queue().record(m_id);
        if (rec && !active(rec->state))
            m_ctx.queue().remove(m_id);
    });
    connect(m_openQueue, &Link::activated, this, [this] { emit openQueueRequested(m_id); });
}

void JobMonitor::setFeatures(Features features)
{
    m_features = features;
    refresh();
}

void JobMonitor::setNewJobText(const QString& text)
{
    m_newJob->setText(text);
}

void JobMonitor::setConsoleMinimumHeight(int px)
{
    m_console->setMinimumHeight(px);
}

QAbstractButton* JobMonitor::verboseSwitch() const { return m_verbose; }

void JobMonitor::setJobId(const QString& id)
{
    if (m_id == id && !m_demo && !id.isEmpty())
        return; // already following it (the queue's signals keep it current)
    m_demo.reset();
    if (m_id != id) {
        m_id = id;
        m_phasesKey.clear();
        m_stagesSet = false;
        m_hadOutcome = false;
        m_hadError = false;
        m_mSpeed->sparkline()->clear();
        m_lastSampleMs = 0;
        m_copy->setToolTip(FW_TR("Copy these lines"));
    }
    if (id.isEmpty()) {
        m_console->setJobId({});
        m_console->setLive(false);
        m_console->clearLines();
        applyEmpty();
        return;
    }
    m_console->setJobId(id);
    refresh();
}

void JobMonitor::setDemoRecord(const JobRecord& record, const QStringList& logLines)
{
    m_demo = record;
    m_id = record.id;
    m_phasesKey.clear();
    m_stagesSet = false;
    m_console->setLive(false);
    m_console->clearLines();
    QDateTime t = record.started.isValid() ? record.started : QDateTime::currentDateTime();
    for (const QString& line : logLines) {
        LogLevel level = LogLevel::Info;
        QString text = line;
        if (text.startsWith(QLatin1String("W "))) {
            level = LogLevel::Warning;
            text = text.mid(2);
        } else if (text.startsWith(QLatin1String("E "))) {
            level = LogLevel::Error;
            text = text.mid(2);
        } else if (text.startsWith(QLatin1String("D "))) {
            level = LogLevel::Debug;
            text = text.mid(2);
        }
        t = t.addMSecs(1200 + (text.size() * 37) % 2600);
        m_console->appendLine(t, level, text);
    }
    m_console->jumpToLatest();
    auto* spark = m_mSpeed->sparkline();
    spark->clear();
    for (int i = 0; i < 40; ++i)
        spark->addSample(300.0 + 120.0 * std::sin(i / 4.0) + (i % 7) * 9.0);
    apply(record);
    m_tick.stop();
}

JobRecord JobMonitor::demoRecord(const QString& state, QStringList* logLines)
{
    const QDateTime now = QDateTime::currentDateTime();
    JobRecord rec;
    rec.id = QStringLiteral("J160801-042");
    rec.kind = QStringLiteral("fpkg.build");
    rec.title = QStringLiteral("Demon's Souls (PPSA01342)");
    rec.subtitle = QStringLiteral("FPKG · 31.6 GB → D:\\PS5\\Packages");
    // The FPKG job's stage names (engines/fpkg), English as a job has them.
    rec.phases = {FW_TR_NOOP("Preparing the source"), FW_TR_NOOP("Planning"), FW_TR_NOOP("Writing project files"),
                  FW_TR_NOOP("Pre-reading files"), FW_TR_NOOP("Packing"), FW_TR_NOOP("Finishing"),
                  FW_TR_NOOP("Checking the package"), FW_TR_NOOP("Verifying with img_verify"), FW_TR_NOOP("Computing SHA-256")};
    rec.added = now.addSecs(-240);
    rec.started = now.addSecs(-218);
    ProgressSnapshot& p = rec.progress;
    p.phaseCount = rec.phases.size();
    p.unitIsBytes = true;
    p.total = 31600000000LL;
    p.filesTotal = 18442;
    QStringList lines{
        QStringLiteral("Queued: Demon's Souls (PPSA01342)"),
        QStringLiteral("Started: Demon's Souls (PPSA01342)"),
        QStringLiteral("Publishing Tools: prospero-pub-cmd 2.6.0 (sdk-fpkg279-fix13)"),
        QStringLiteral("Source: D:\\PS5\\Dumps\\PPSA01342 Demon's Souls (18,442 files, 31.6 GB)"),
        QStringLiteral("Left out: 212 SDK files and dump leftovers (AMPR index, journal, logs)"),
        QStringLiteral("Project written: PPSA01342.gp5 (18,230 files, 4 PlayGo chunks)"),
        QStringLiteral("Pre-reading 18,230 files so the publisher reads from the cache"),
        QStringLiteral("Packing with D:\\Tools\\PubTools\\bin\\prospero-pub-cmd.exe"),
        QStringLiteral("Sony publisher: priority Below Normal, 12 of 16 CPUs."),
        QStringLiteral("D img_create: \"D:\\Tools\\PubTools\\bin\\prospero-pub-cmd.exe\" img_create --oformat pkg PPSA01342.gp5 …"),
        QStringLiteral("SDK: Creating an image for UP9000-PPSA01342_00-DEMONSSOULS00000"),
        QStringLiteral("W SDK: Image chunk 3 has no label; the default label is used"),
        QStringLiteral("SDK: |____________________________________________________|"),
    };
    if (state == QLatin1String("running")) {
        rec.state = JobState::Running;
        p.phase = QStringLiteral("Packing");
        p.phaseIndex = 4;
        p.done = 19900000000LL;
        p.filesDone = 11204;
        p.ratePerSec = 418.0 * 1024 * 1024;
        p.etaMs = qint64(double(p.total - p.done) / p.ratePerSec * 1000.0);
        p.currentItem = QStringLiteral("19.9 GB written · Data\\chunk_0007.pak");
        p.overallFraction = 0.63;
        p.phaseMs = {4100, 900, 1300, 22000, 190000, 0, 0, 0, 0};
        p.elapsedMs = 218300;
        p.phaseElapsedMs = 190000;
    } else {
        rec.finished = now.addSecs(-20);
        p.finished = true;
        lines << QStringLiteral("Header check passed: FIH header, plaintext debug marker.")
              << QStringLiteral("Package written: D:\\PS5\\Packages\\UP9000-PPSA01342_00-DEMONSSOULS00000-A0104-V0104.pkg (31.2 GB)")
              << QStringLiteral("img_verify passed.");
        if (state == QLatin1String("done")) {
            rec.state = JobState::Succeeded;
            p.phase = QStringLiteral("Computing SHA-256");
            p.phaseIndex = 8;
            p.done = p.total;
            p.filesDone = p.filesTotal;
            p.overallFraction = 1.0;
            p.phaseMs = {4100, 900, 1300, 22000, 301000, 400, 2100, 96000, 48000};
            p.elapsedMs = 475800;
            JobOutcome o;
            o.outputs = {QStringLiteral("D:/PS5/Packages/UP9000-PPSA01342_00-DEMONSSOULS00000-A0104-V0104.pkg")};
            o.verification = Verification::Passed;
            o.verificationDetail = QStringLiteral("Header checked; img_verify passed (format and integrity).");
            o.summary = QStringLiteral("UP9000-PPSA01342_00-DEMONSSOULS00000-A0104-V0104.pkg · 31.2 GB");
            o.warnings = {QStringLiteral("Publishing Tools: Image chunk 3 has no label; the default label is used.")};
            rec.outcome = o;
            lines << QStringLiteral("SHA-256 of UP9000-PPSA01342_00-DEMONSSOULS00000-A0104-V0104.pkg: 7f3c0a9e…b41d")
                  << QStringLiteral("Finished: Demon's Souls (PPSA01342) — UP9000-PPSA01342_00-DEMONSSOULS00000-A0104-V0104.pkg · 31.2 GB");
        } else {
            rec.state = JobState::Failed;
            p.phase = QStringLiteral("Verifying with img_verify");
            p.phaseIndex = 7;
            p.done = p.total;
            p.overallFraction = 0.91;
            p.phaseMs = {4100, 900, 1300, 22000, 301000, 400, 2100, 61000, 0};
            p.elapsedMs = 392800;
            rec.error = Error(ErrorCode::VerificationFailed,
                              QStringLiteral("Verification failed. img_verify reported: [Error] The PFS image digest does not match (chunk 3)."));
            lines.removeLast();
            lines << QStringLiteral("E SDK: [Error] The PFS image digest does not match (chunk 3)")
                  << QStringLiteral("E Failed: Demon's Souls (PPSA01342) — Verification failed. img_verify reported: [Error] The PFS image digest does not match (chunk 3).");
        }
    }
    if (logLines)
        *logLines = lines;
    return rec;
}

void JobMonitor::refresh()
{
    if (m_demo) {
        apply(*m_demo);
        return;
    }
    if (m_id.isEmpty()) {
        applyEmpty();
        return;
    }
    auto rec = m_ctx.queue().record(m_id);
    if (!rec) {
        applyEmpty();
        updateTick(nullptr);
        return;
    }
    apply(*rec);
}

void JobMonitor::applyEmpty()
{
    m_tile->setIconName(QStringLiteral("queue"));
    m_tile->setTone(Tone::Info);
    m_title->setFullText(FW_TR("No job selected"));
    m_meta->setFullText({});
    m_badge->hide();
    m_bar->setFraction(0);
    m_bar->setState(ProgressBar::State::Determinate);
    m_percent->setText({});
    m_detail->setText({});
    m_detail->hide();
    m_item->hide();
    for (MetricTile* t : {m_mElapsed, m_mLeft, m_mSpeed, m_mDone, m_mStage}) {
        t->setValue({});
        t->setHint({});
    }
    m_stages->setStages({});
    m_result->hide();
    for (QWidget* w : {static_cast<QWidget*>(m_pause), static_cast<QWidget*>(m_cancel), static_cast<QWidget*>(m_folder),
                       static_cast<QWidget*>(m_retry), static_cast<QWidget*>(m_remove), static_cast<QWidget*>(m_openQueue)})
        w->hide();
    m_newJob->setVisible(m_features.testFlag(ShowNewJob));
    m_note->setText({});
    m_tick.stop();
}

void JobMonitor::updateTick(const JobRecord* r)
{
    // The clocks tick only while something can see them.
    const bool run = r && active(r->state) && !m_demo && isVisible();
    if (run && !m_tick.isActive())
        m_tick.start();
    else if (!run && m_tick.isActive())
        m_tick.stop();
}

void JobMonitor::showEvent(QShowEvent* e)
{
    Card::showEvent(e);
    if (!m_id.isEmpty())
        refresh();
}

void JobMonitor::hideEvent(QHideEvent* e)
{
    Card::hideEvent(e);
    m_tick.stop();
}

void JobMonitor::sample(const JobRecord& r)
{
    const auto& p = r.progress;
    if (r.state != JobState::Running || !p.unitIsBytes || p.ratePerSec <= 0)
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - m_lastSampleMs < 500)
        return;
    m_lastSampleMs = now;
    m_mSpeed->sparkline()->addSample(p.ratePerSec);
}

void JobMonitor::apply(const JobRecord& r)
{
    const auto& p = r.progress;
    const bool isActive = active(r.state);
    const bool finished = isTerminal(r.state);
    const bool started = r.started.isValid();

    // Header
    m_tile->setIconName(stateIconFor(r.state));
    m_tile->setTone(toneForState(r.state));
    m_title->setFullText(r.title);
    QString meta = i18n::translate(m_ctx.jobFactory().displayName(r.kind));
    if (!r.subtitle.isEmpty())
        meta += QStringLiteral(" · ") + r.subtitle;
    m_meta->setFullText(meta);
    m_meta->setToolTip(plainTip(meta));
    QString badge = i18n::translate(jobStateName(r.state));
    if (r.state == JobState::Running)
        badge = p.indeterminate ? FW_TR("Running") : FW_TR("Running · %1").arg(thinPercent(p.overallFraction));
    else if (r.state == JobState::Paused)
        badge = FW_TR("Paused");
    else if (r.state == JobState::Cancelling)
        badge = FW_TR("Cancelling…");
    else if (r.state == JobState::Succeeded)
        badge = FW_TR("Done"); // what the verification found is in the result below
    const Tone tone = toneForState(r.state);
    m_badge->set(badge, tone);
    m_badge->setIconName(r.state == JobState::Queued || r.state == JobState::Paused || r.state == JobState::Cancelled || r.state == JobState::Cancelling
                             ? QString() : defaultBadgeIcon(tone));
    m_badge->show();

    // Hero progress
    m_bar->setFraction(p.overallFraction);
    QString pct;
    QColor pctColor = color::fg_0;
    switch (r.state) {
    case JobState::Running:
        m_bar->setState(p.indeterminate ? ProgressBar::State::Indeterminate : ProgressBar::State::Determinate);
        pct = p.indeterminate ? QString() : thinPercent(p.overallFraction);
        break;
    case JobState::Paused:
        m_bar->setState(ProgressBar::State::Paused);
        pct = p.indeterminate ? FW_TR("Paused") : FW_TR("Paused · %1").arg(thinPercent(p.overallFraction));
        pctColor = color::fg_3;
        break;
    case JobState::Cancelling:
        m_bar->setState(ProgressBar::State::Paused);
        pct = FW_TR("Cancelling…");
        pctColor = color::fg_3;
        break;
    case JobState::Succeeded:
        m_bar->setState(ProgressBar::State::Done);
        pct = thinPercent(1.0);
        pctColor = color::success_hi;
        break;
    case JobState::Failed:
        m_bar->setState(ProgressBar::State::Failed);
        pct = (!started || p.indeterminate) ? FW_TR("Failed") : FW_TR("Failed at %1").arg(thinPercent(p.overallFraction));
        pctColor = color::danger_hi;
        break;
    case JobState::Interrupted:
        // A job restored after the app closed has no start time any more.
        m_bar->setState(ProgressBar::State::Failed);
        pct = (!started || p.indeterminate) ? FW_TR("Interrupted") : FW_TR("Interrupted at %1").arg(thinPercent(p.overallFraction));
        pctColor = color::warn_hi;
        break;
    case JobState::Cancelled:
        m_bar->setState(ProgressBar::State::Paused);
        pct = (!started || p.indeterminate) ? FW_TR("Cancelled") : FW_TR("Cancelled at %1").arg(thinPercent(p.overallFraction));
        pctColor = color::fg_3;
        break;
    case JobState::Queued:
        m_bar->setFraction(0);
        m_bar->setState(ProgressBar::State::Determinate);
        pct = QString();
        break;
    }
    m_percent->setText(pct);
    m_percent->setMinimumWidth(pct.size() <= 5 ? QFontMetrics(percentFont(m_variant)).horizontalAdvance(thinPercent(1.0)) : 0);
    setTextColor(m_percent, pctColor);

    // The error itself is in the result block below (once, not twice).
    const bool errorShownBelow = r.error.has_value() && !r.error->isCancelled();
    QString detail;
    QColor detailColor = color::fg_1;
    const QString phase = i18n::translate(p.phase);
    switch (r.state) {
    case JobState::Running: {
        QStringList parts;
        if (!phase.isEmpty())
            parts << phase;
        if (const QString d = progressDetailText(p); !d.isEmpty())
            parts << d;
        detail = parts.join(QStringLiteral(" · "));
        break;
    }
    case JobState::Paused:
        detail = phase.isEmpty() ? FW_TR("Paused. Resume to continue.")
                                 : FW_TR("Paused in “%1”. Resume to continue.").arg(phase);
        break;
    case JobState::Cancelling:
        detail = FW_TR("Stopping at the next safe point…");
        detailColor = color::fg_2;
        break;
    case JobState::Queued: {
        const int maxAttempts = m_ctx.settings().value(general::keys::kAutoRetryCount).toInt() + 1;
        const QString w = waitingText(r, maxAttempts);
        detail = !w.isEmpty() ? w : m_ctx.queue().isHeld() ? (m_ctx.queue().isBusy() ? FW_TR("Waiting · the queue is paused") : FW_TR("Waiting · press Start queue")) : FW_TR("Waiting for its turn");
        detailColor = color::fg_2;
        break;
    }
    case JobState::Succeeded:
        detail = r.outcome && !r.outcome->summary.isEmpty() ? i18n::translate(r.outcome->summary) : FW_TR("Finished");
        break;
    case JobState::Failed:
        if (!errorShownBelow)
            detail = FW_TR("Failed");
        else if (started && !phase.isEmpty())
            detail = FW_TR("Failed in “%1”.").arg(phase);
        detailColor = color::danger_hi;
        break;
    case JobState::Interrupted:
        if (!errorShownBelow)
            detail = FW_TR("Interrupted when the app closed. Retry to run it again.");
        detailColor = color::warn_hi;
        break;
    case JobState::Cancelled:
        detail = started ? FW_TR("Cancelled before it finished.") : FW_TR("Cancelled before it started.");
        detailColor = color::fg_2;
        break;
    }
    m_detail->setText(detail);
    setTextColor(m_detail, detailColor);
    m_detail->setVisible(!detail.isEmpty());
    const bool showItem = r.state == JobState::Running && !p.currentItem.isEmpty();
    // A file name, or a step text some engines give as data (FTP diagnostics): shown translated.
    const QString item = showItem ? i18n::translate(p.currentItem) : QString();
    m_item->setFullText(item);
    m_item->setToolTip(showItem ? plainTip(item) : QString());
    m_item->setVisible(showItem);

    // Metrics
    if (started || isActive) {
        m_mElapsed->setValue(clockText(elapsedMsOf(r)));
        m_mElapsed->setHint(finished ? FW_TR("total") : FW_TR("since %1").arg(r.started.toString(QStringLiteral("HH:mm"))));
    } else {
        m_mElapsed->setValue({});
        m_mElapsed->setHint(r.state == JobState::Queued ? FW_TR("not started") : QString());
    }
    if (r.state == JobState::Running) {
        const QString left = shortTimeLeft(p.etaMs);
        m_mLeft->setValue(left.isEmpty() ? kDash : left);
        m_mLeft->setHint(left.isEmpty() ? (p.indeterminate ? FW_TR("this stage has no size") : FW_TR("measuring")) : FW_TR("this stage"));
    } else {
        m_mLeft->setValue(r.state == JobState::Succeeded ? clockText(0) : kDash);
        m_mLeft->setHint({});
    }
    if (r.state == JobState::Running && p.unitIsBytes && p.ratePerSec > 0) {
        m_mSpeed->setValue(formatRate(p.ratePerSec));
        m_mSpeed->setHint({});
    } else {
        m_mSpeed->setValue({});
        m_mSpeed->setHint({});
    }
    if (isActive && p.total > 0) {
        m_mDone->setValue(p.unitIsBytes ? formatBytes(p.done) : formatCount(p.done));
        m_mDone->setHint(FW_TR("of %1").arg(p.unitIsBytes ? formatBytes(p.total) : formatCount(p.total)));
    } else if (isActive && p.filesTotal > 0) {
        m_mDone->setValue(formatCount(p.filesDone));
        m_mDone->setHint(FW_TR("of %1 files").arg(formatCount(p.filesTotal)));
    } else if (r.state == JobState::Succeeded && r.outcome) {
        m_mDone->setValue(FW_TR("All"));
        const qsizetype outputs = r.outcome->outputs.size();
        m_mDone->setHint(outputs == 0 ? QString() : outputs == 1 ? FW_TR("%1 output").arg(outputs) : FW_TR("%1 outputs").arg(outputs));
    } else {
        m_mDone->setValue({});
        m_mDone->setHint(isActive ? FW_TR("this stage has no size") : QString());
    }
    if (started || isActive) {
        const int cur = r.state == JobState::Succeeded ? std::max(1, p.phaseCount) : std::min(p.phaseIndex + 1, std::max(1, p.phaseCount));
        m_mStage->setValue(FW_TR("%1 of %2").arg(cur).arg(std::max(1, p.phaseCount)));
        m_mStage->setHint(r.state == JobState::Succeeded ? FW_TR("all done") : phase);
    } else {
        m_mStage->setValue(FW_TR("%1 of %2").arg(0).arg(std::max<qsizetype>(1, r.phases.size())));
        m_mStage->setHint({});
    }
    sample(r);

    // Stages
    const QString key = r.phases.join(u'\x1f');
    if (!m_stagesSet || key != m_phasesKey) {
        m_stagesSet = true;
        m_phasesKey = key;
        m_stages->setStages(r.phases);
    }
    m_stages->update(p, r.state);

    // Console follows the job (no-op while it already does).
    if (!m_demo)
        m_console->setJobId(r.id);

    // Result and actions (rebuilt on state changes only).
    const bool hasOutcome = r.outcome.has_value();
    const bool hasError = r.error.has_value() && !r.error->isCancelled();
    if (r.state != m_lastState || hasOutcome != m_hadOutcome || hasError != m_hadError || m_result->isHidden() == (hasOutcome || hasError)) {
        rebuildResult(r);
        m_lastState = r.state;
        m_hadOutcome = hasOutcome;
        m_hadError = hasError;
    }
    updateActions(r);

    QStringList note;
    if (r.state == JobState::Queued)
        note << FW_TR("Added %1").arg(r.added.toString(QStringLiteral("HH:mm")));
    else if (started)
        note << FW_TR("Started %1").arg(r.started.toString(QStringLiteral("HH:mm")));
    if (r.attempt > 1)
        note << FW_TR("Attempt %1").arg(r.attempt);
    if (r.finished.isValid())
        note << FW_TR("Finished %1").arg(r.finished.toString(QStringLiteral("HH:mm")));
    if (!started && finished && r.state != JobState::Interrupted)
        note << FW_TR("Not started");
    m_note->setText(note.join(QStringLiteral(" · ")));
    updateTick(&r);
}

void JobMonitor::rebuildResult(const JobRecord& r)
{
    while (QLayoutItem* it = m_resultLayout->takeAt(0)) {
        delete it->widget();
        delete it;
    }
    // A failure leads with its error, in the danger-tinted bar; what a
    // successful job made sits on a success-tinted plate.
    const bool hasError = r.error && !r.error->isCancelled();
    if (hasError) {
        auto* bar = new InfoBar(Tone::Err, m_result);
        bar->setTitle(r.state == JobState::Interrupted ? FW_TR("Interrupted") : FW_TR("Failed"));
        bar->setMessage(r.error->message);
        auto* link = bar->addLink(FW_TR("Open in Logs"));
        connect(link, &Link::activated, this, [this] { emit openLogsRequested(m_id); });
        m_resultLayout->addWidget(bar);
    }
    if (r.outcome) {
        if (r.outcome->verification != Verification::NotRun) {
            auto* row = new MessageRow(m_result);
            const Verification v = r.outcome->verification;
            // The row's word for it ("Not verified"), not the engine's ("Skipped").
            row->set(QStringLiteral("%1%2").arg(v == Verification::Skipped ? FW_TR("Not verified") : i18n::translate(verificationName(v)), r.outcome->verificationDetail.isEmpty() ? QString() : QStringLiteral(" · %1").arg(r.outcome->verificationDetail)),
                     v == Verification::Passed ? Tone::Ok : v == Verification::Failed ? Tone::Err : Tone::Warn);
            m_resultLayout->addWidget(row);
        }
        for (const QString& w : r.outcome->warnings) {
            auto* row = new MessageRow(m_result);
            row->set(w, Tone::Warn);
            m_resultLayout->addWidget(row);
        }
        for (const QString& o : r.outcome->outputs) {
            auto* row = new QWidget(m_result);
            auto* h = hbox(row, space::sm, QMargins(0, 2, 0, 2));
            auto* ic = new QLabel(row);
            ic->setFixedSize(size::iconSm, size::iconSm);
            ic->setPixmap(iconPixmap(QStringLiteral("file_check"), size::iconSm, color::fg_3, devicePixelRatioF()));
            h->addWidget(ic);
            auto* path = new ElideLabel(nativePath(o), Qt::ElideMiddle, row);
            path->setFont(font::mono());
            setTextColor(path, color::fg_1);
            path->setToolTip(plainTip(nativePath(o)));
            path->setTextInteractionFlags(Qt::TextSelectableByMouse);
            h->addWidget(path, 1);
            m_resultLayout->addWidget(row);
        }
    }
    static_cast<ResultPlate*>(m_result)->setTinted(r.state == JobState::Succeeded && !hasError && m_resultLayout->count() > 0);
    m_result->setVisible(m_resultLayout->count() > 0);
}

void JobMonitor::updateActions(const JobRecord& r)
{
    const bool isActive = active(r.state) && r.state != JobState::Cancelling; // cancelling: nothing left to do
    const bool finished = isTerminal(r.state);
    m_pause->setVisible(isActive);
    m_pause->setText(r.state == JobState::Paused ? FW_TR("Resume") : FW_TR("Pause"));
    setButtonIcon(m_pause, r.state == JobState::Paused ? QStringLiteral("play") : QStringLiteral("pause"));
    m_cancel->setVisible(isActive || r.state == JobState::Queued);
    m_cancel->setText(r.state == JobState::Queued ? FW_TR("Remove") : FW_TR("Cancel"));
    setButtonIcon(m_cancel, r.state == JobState::Queued ? QStringLiteral("trash") : QStringLiteral("close"));
    const bool outputs = r.outcome && !r.outcome->outputs.isEmpty();
    m_folder->setVisible(outputs && finished);
    setButtonVariant(m_folder, r.state == JobState::Succeeded ? ButtonVariant::Primary : ButtonVariant::Secondary);
    m_retry->setVisible(finished);
    m_retry->setText(r.state == JobState::Succeeded ? FW_TR("Run again") : FW_TR("Retry"));
    setButtonVariant(m_retry, r.state == JobState::Succeeded ? ButtonVariant::Secondary : ButtonVariant::Primary);
    m_newJob->setVisible(m_features.testFlag(ShowNewJob));
    setButtonVariant(m_newJob, finished && r.state == JobState::Succeeded && !outputs ? ButtonVariant::Primary : ButtonVariant::Secondary);
    m_remove->setVisible(m_features.testFlag(ShowRemove) && finished);
    m_openQueue->setVisible(m_features.testFlag(ShowOpenQueue));
}

void JobMonitor::retry()
{
    auto r = m_ctx.queue().retry(m_id);
    if (!r.ok()) {
        if (isSignalConnected(QMetaMethod::fromSignal(&JobMonitor::retryFailed)))
            emit retryFailed(m_id, r.error().message);
        else
            Sheet::notify(window(), FW_TR("This job cannot run again"), r.error().message, Sheet::Icon::Error);
        return;
    }
    const QString id = r.value();
    m_ctx.queue().startNow(id); // this job only; a paused queue stays paused
    setJobId(id);
    emit jobSwitched(id);
}

QString JobMonitor::stateText() const { return m_badge->text(); }
QString JobMonitor::percentText() const { return m_percent->text(); }
QString JobMonitor::detailText() const { return m_detail->text(); }

QStringList JobMonitor::metricValues() const
{
    return {m_mElapsed->value(), m_mLeft->value(), m_mSpeed->value(), m_mDone->value(), m_mStage->value()};
}

} // namespace fw::ui
