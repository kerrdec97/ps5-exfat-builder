#include "app/ui/JobRows.h"

#include "app/AppContext.h"
#include "app/ui/Icons.h"
#include "app/ui/JobMonitor.h"
#include "app/ui/LogView.h"
#include "app/ui/Sheet.h"
#include "app/services/JobSubmit.h"
#include "core/FileUtil.h"
#include "engines/general/Module.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMenu>
#include <QMetaMethod>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QVBoxLayout>

#include <algorithm>

namespace fw::ui {

using namespace theme;

static const char* kJobMime = "application/x-fw-job-id";

Tone toneForState(JobState state)
{
    switch (state) {
    case JobState::Running: return Tone::Info;
    case JobState::Paused: return Tone::Neutral;
    case JobState::Cancelling: return Tone::Neutral;
    case JobState::Succeeded: return Tone::Ok;
    case JobState::Failed: return Tone::Err;
    case JobState::Cancelled: return Tone::Neutral;
    case JobState::Interrupted: return Tone::Warn;
    case JobState::Queued: break;
    }
    return Tone::Neutral;
}

QString stateIcon(JobState state)
{
    switch (state) {
    case JobState::Running: return QStringLiteral("circle_filled");
    case JobState::Paused: return QStringLiteral("pause_circle");
    case JobState::Cancelling: return QStringLiteral("stop_circle");
    case JobState::Succeeded: return QStringLiteral("success");
    case JobState::Failed: return QStringLiteral("error");
    case JobState::Cancelled: return QStringLiteral("ban");
    case JobState::Interrupted: return QStringLiteral("warning");
    case JobState::Queued: break;
    }
    return QStringLiteral("circle");
}

static QColor stateColor(JobState state)
{
    switch (state) {
    case JobState::Running: return color::accent_hi;
    case JobState::Succeeded: return color::success;
    case JobState::Failed: return color::danger;
    case JobState::Interrupted: return color::warn;
    default: return color::fg_3;
    }
}

QString waitingText(const JobRecord& r, int maxAttempts)
{
    if (r.state != JobState::Queued || !r.notBefore.isValid())
        return {};
    const qint64 s = QDateTime::currentDateTime().secsTo(r.notBefore);
    if (s <= 0)
        return {};
    return QStringLiteral("Retrying in %1 s · attempt %2 of %3").arg(s).arg(r.attempt).arg(std::max(maxAttempts, r.attempt));
}

// The same wording as the monitor's stage times ("2 min", "1 h 05 min").
static QString durationText(const JobRecord& r)
{
    if (!r.started.isValid() || !r.finished.isValid())
        return {};
    // Never negative: the clock may have been set back while the job ran.
    return shortDuration(std::max<qint64>(0, r.started.msecsTo(r.finished)));
}

QString finishedNote(const JobRecord& r)
{
    QStringList parts;
    if (r.outcome) {
        if (r.outcome->verification == Verification::Passed)
            parts << QStringLiteral("Verified");
        else if (r.outcome->verification == Verification::Skipped)
            parts << QStringLiteral("Not verified");
        if (!r.outcome->warnings.isEmpty())
            parts << QStringLiteral("%1 warning%2").arg(r.outcome->warnings.size()).arg(r.outcome->warnings.size() == 1 ? QString() : QStringLiteral("s"));
    }
    const QString d = durationText(r);
    if (!d.isEmpty())
        parts << d;
    if (parts.isEmpty() && r.outcome)
        parts << r.outcome->summary;
    return parts.join(QStringLiteral(" · "));
}

qint64 jobTimeLeftMs(const JobRecord& r)
{
    if (r.state != JobState::Running)
        return -1;
    const auto& p = r.progress;
    // In the last stage the stage's own estimate is the job's.
    if (p.etaMs >= 0 && p.phaseIndex >= p.phaseCount - 1)
        return p.etaMs;
    // Earlier: the time spent so far, scaled by the weighted share still to do.
    const double f = p.overallFraction;
    if (f < 0.02 || f >= 1.0 || p.elapsedMs < 5000)
        return -1;
    const qint64 ms = qint64(double(p.elapsedMs) * (1.0 - f) / f);
    return std::max(ms, p.etaMs); // never less than what this stage still needs
}

// ---- row ---------------------------------------------------------------------------

JobRow::JobRow(Variant variant, QWidget* parent) : QWidget(parent), m_variant(variant)
{
    setFocusPolicy(Qt::TabFocus);
    setAttribute(Qt::WA_Hover);
    setProperty("fwRadius", radius::xs);
    const bool queue = variant == Variant::Queue;
    setFixedHeight(queue ? size::rowJob : size::row);
    auto* h = hbox(this, queue ? space::sm : space::sm,
                   queue ? QMargins(space::sm, space::sm, space::md, space::sm) : QMargins(space::md, 0, space::sm, 0));
    m_grip = new QLabel(this);
    m_grip->setFixedSize(size::icon, size::icon);
    static_cast<QLabel*>(m_grip)->setPixmap(iconPixmap(QStringLiteral("grip"), size::icon, color::fg_5, devicePixelRatioF()));
    h->addWidget(m_grip);
    m_stateIcon = new QLabel(this);
    m_stateIcon->setFixedSize(size::icon, size::icon);
    if (queue)
        h->addWidget(m_stateIcon);
    else
        m_stateIcon->hide();
    auto* text = new QWidget(this);
    auto* tl = vbox(text, 2);
    m_title = new ElideLabel({}, Qt::ElideRight, text);
    m_title->setFont(queue ? font::bodyStrong() : font::body());
    setTextColor(m_title, queue ? color::fg_0 : color::fg_1);
    m_meta = new ElideLabel({}, Qt::ElideMiddle, text);
    m_meta->setFont(font::mono());
    setTextColor(m_meta, color::fg_3);
    tl->addWidget(m_title);
    if (queue)
        tl->addWidget(m_meta);
    else {
        m_meta->hide();
    }
    h->addWidget(text, 1);
    m_middle = new QWidget(this);
    auto* ml = vbox(m_middle, 4);
    m_bar = new ProgressBar(m_middle);
    m_note = label({}, TextStyle::Caption, color::fg_3, m_middle);
    m_note->setWordWrap(false);
    ml->addWidget(m_bar);
    ml->addWidget(m_note);
    m_middle->setFixedWidth(220);
    if (queue)
        h->addWidget(m_middle);
    else
        m_middle->hide();
    m_badge = new Badge({}, Tone::Neutral, this);
    if (queue) {
        // A column as wide as the widest state, so the rows line up.
        auto* cell = new QWidget(this);
        auto* cl = hbox(cell, 0);
        cl->addWidget(m_badge);
        cl->addStretch(1);
        static const int widest = Badge(QStringLiteral("Running · %1").arg(thinPercent(1.0)), Tone::Info).sizeHint().width();
        cell->setFixedWidth(widest);
        h->addWidget(cell);
    } else {
        h->addWidget(m_badge);
    }
    m_actions = new QWidget(this);
    m_actionsLayout = hbox(m_actions, 2);
    m_actions->setFixedWidth(queue ? 4 * 28 + 3 * 2 : 28); // up, down, remove, more
    m_actionsLayout->setAlignment(Qt::AlignRight);
    h->addWidget(m_actions);
}

QSize JobRow::sizeHint() const
{
    return {m_variant == Variant::Queue ? 560 : 360, m_variant == Variant::Queue ? size::rowJob : size::row};
}

void JobRow::setSelected(bool on)
{
    m_selected = on;
    update();
}

void JobRow::setCanMoveUp(bool on)
{
    m_canUp = on;
    if (m_upButton)
        m_upButton->setEnabled(on);
}

void JobRow::setCanMoveDown(bool on)
{
    m_canDown = on;
    if (m_downButton)
        m_downButton->setEnabled(on);
}

void JobRow::setRecord(const JobRecord& r, int queuePosition, int maxAttempts, bool reorderable)
{
    const bool changedState = m_record.id != r.id || m_record.state != r.state;
    m_record = r;
    m_id = r.id;
    m_state = r.state;
    m_reorderable = reorderable && r.state == JobState::Queued;
    // The first cell: the grip of a queued row; in the dock the state glyph of
    // the others (the label stays enabled: a disabled one greys its pixmap).
    auto* grip = static_cast<QLabel*>(m_grip);
    if (m_reorderable) {
        grip->setPixmap(iconPixmap(QStringLiteral("grip"), size::icon, color::fg_5, devicePixelRatioF()));
        grip->setCursor(Qt::SizeAllCursor);
        grip->setToolTip(QStringLiteral("Drag to reorder"));
    } else {
        grip->setPixmap(m_variant == Variant::Queue ? QPixmap() : iconPixmap(stateIcon(r.state), size::icon, stateColor(r.state), devicePixelRatioF()));
        grip->setCursor(Qt::ArrowCursor);
        grip->setToolTip(m_variant == Variant::Queue ? QString() : jobStateName(r.state));
    }
    m_stateIcon->setPixmap(iconPixmap(stateIcon(r.state), size::icon, stateColor(r.state), devicePixelRatioF()));
    setAccessibleName(QStringLiteral("%1, %2").arg(r.title, jobStateName(r.state)));

    const QString kindName = AppContext::instance() ? AppContext::instance()->jobFactory().displayName(r.kind) : r.kind;
    if (m_variant == Variant::Queue) {
        m_title->setFullText(r.title);
        QString meta = r.subtitle.isEmpty() ? kindName : QStringLiteral("%1 · %2").arg(kindName, r.subtitle);
        m_meta->setFullText(meta);
    } else {
        m_title->setFullText(r.subtitle.isEmpty() ? r.title : QStringLiteral("%1 · %2").arg(r.title, r.subtitle));
    }

    // Middle column
    const QString waiting = waitingText(r, maxAttempts);
    QString note;
    QColor noteColor = color::fg_3;
    bool showBar = false;
    switch (r.state) {
    case JobState::Running:
    case JobState::Paused:
    case JobState::Cancelling:
        showBar = true;
        m_bar->setFraction(r.progress.overallFraction);
        m_bar->setState(r.state != JobState::Running ? ProgressBar::State::Paused
                        : r.progress.indeterminate ? ProgressBar::State::Indeterminate : ProgressBar::State::Determinate);
        if (r.state == JobState::Cancelling) {
            note = QStringLiteral("Stopping at the next safe point…");
        } else if (r.state == JobState::Paused) {
            note = r.progress.phase.isEmpty() ? QStringLiteral("Paused") : QStringLiteral("Paused in %1").arg(r.progress.phase);
        } else {
            QStringList parts;
            if (!r.progress.phase.isEmpty())
                parts << r.progress.phase;
            if (r.progress.unitIsBytes && r.progress.ratePerSec > 0)
                parts << formatRate(r.progress.ratePerSec);
            const QString eta = formatTimeLeft(jobTimeLeftMs(r));
            if (!eta.isEmpty())
                parts << eta;
            note = parts.join(QStringLiteral(" · "));
        }
        break;
    case JobState::Queued:
        if (!waiting.isEmpty())
            note = waiting;
        else if (!r.overrides.isEmpty())
            note = QStringLiteral("%1 setting%2 overridden").arg(r.overrides.size()).arg(r.overrides.size() == 1 ? QString() : QStringLiteral("s"));
        else if (AppContext::instance() && AppContext::instance()->queue().isHeld())
            note = QStringLiteral("Waits for the queue to resume");
        else if (queuePosition <= 1)
            note = QStringLiteral("Starts next");
        else
            note = QStringLiteral("Waits for %1 job%2 ahead").arg(queuePosition - 1).arg(queuePosition == 2 ? QString() : QStringLiteral("s"));
        break;
    case JobState::Failed:
        note = r.error ? r.error->message : QStringLiteral("Failed");
        noteColor = color::danger_hi;
        break;
    case JobState::Interrupted:
        note = QStringLiteral("Interrupted when the app closed. Retry to run it again.");
        noteColor = color::warn_hi;
        break;
    case JobState::Cancelled:
        note = QStringLiteral("Cancelled");
        break;
    case JobState::Succeeded:
        note = finishedNote(r);
        break;
    }
    m_bar->setVisible(showBar);
    m_note->setText(QFontMetrics(m_note->font()).elidedText(note, Qt::ElideRight, 216));
    m_note->setToolTip(plainTip(note));
    setTextColor(m_note, noteColor);

    // Badge
    QString badgeText = jobStateName(r.state);
    Tone tone = toneForState(r.state);
    if (r.state == JobState::Running)
        badgeText = r.progress.indeterminate ? QStringLiteral("Running") : QStringLiteral("Running · %1").arg(thinPercent(r.progress.overallFraction));
    else if (r.state == JobState::Cancelling)
        badgeText = QStringLiteral("Cancelling…");
    else if (r.state == JobState::Queued && queuePosition > 0 && m_variant == Variant::Queue) {
        static const char* suffix[] = {"th", "st", "nd", "rd"};
        const int n = queuePosition;
        const int s = (n % 100 >= 11 && n % 100 <= 13) ? 0 : (n % 10 <= 3 ? n % 10 : 0);
        badgeText = QStringLiteral("Queued · %1%2").arg(n).arg(QLatin1String(suffix[s]));
    }
    m_badge->set(badgeText, tone);
    m_badge->setIconName(r.state == JobState::Queued || r.state == JobState::Cancelled || r.state == JobState::Paused || r.state == JobState::Cancelling
                             ? QString() : defaultBadgeIcon(tone));
    if (changedState)
        rebuildActions();
    update();
}

void JobRow::rebuildActions()
{
    // deleteLater: this runs inside the click of one of these buttons (Pause
    // changes the job's state before the click handler has returned).
    while (QLayoutItem* it = m_actionsLayout->takeAt(0)) {
        if (QWidget* w = it->widget()) {
            w->hide();
            w->deleteLater();
        }
        delete it;
    }
    m_upButton = nullptr;
    m_downButton = nullptr;
    auto add = [this](const QString& iconName, const QString& name, auto signal, bool enabled = true) {
        auto* b = new IconButton(iconName, name, IconButton::Size::Small, m_actions);
        b->setEnabled(enabled);
        connect(b, &QToolButton::clicked, this, [this, signal] { emit(this->*signal)(m_id); });
        m_actionsLayout->addWidget(b);
        return b;
    };
    if (m_variant == Variant::Dock) {
        auto* more = new IconButton(QStringLiteral("more"), QStringLiteral("More"), IconButton::Size::Small, m_actions);
        connect(more, &QToolButton::clicked, this, [this, more] { showMenu(more->mapToGlobal(QPoint(0, more->height()))); });
        m_actionsLayout->addWidget(more);
        return;
    }
    switch (m_state) {
    case JobState::Running:
        add(QStringLiteral("pause"), QStringLiteral("Pause"), &JobRow::pauseRequested);
        add(QStringLiteral("close"), QStringLiteral("Cancel"), &JobRow::cancelRequested);
        break;
    case JobState::Paused:
        add(QStringLiteral("play"), QStringLiteral("Resume"), &JobRow::resumeRequested);
        add(QStringLiteral("close"), QStringLiteral("Cancel"), &JobRow::cancelRequested);
        break;
    case JobState::Cancelling:
        break; // on its way out: nothing to do but wait
    case JobState::Queued:
        m_upButton = add(QStringLiteral("arrow_up"), QStringLiteral("Move up"), &JobRow::moveUpRequested, m_canUp);
        m_downButton = add(QStringLiteral("arrow_down"), QStringLiteral("Move down"), &JobRow::moveDownRequested, m_canDown);
        add(QStringLiteral("trash"), QStringLiteral("Remove"), &JobRow::removeRequested);
        break;
    case JobState::Failed:
    case JobState::Interrupted:
    case JobState::Cancelled: {
        auto* b = button(QStringLiteral("Retry"), ButtonVariant::Secondary, QStringLiteral("undo"), m_actions);
        setButtonSmall(b, true);
        connect(b, &QPushButton::clicked, this, [this] { emit retryRequested(m_id); });
        m_actionsLayout->addWidget(b);
        break;
    }
    case JobState::Succeeded:
        add(QStringLiteral("folder_open"), QStringLiteral("Show in folder"), &JobRow::showInFolderRequested,
            m_record.outcome && !m_record.outcome->outputs.isEmpty());
        break;
    }
    auto* more = new IconButton(QStringLiteral("more"), QStringLiteral("More"), IconButton::Size::Small, m_actions);
    connect(more, &QToolButton::clicked, this, [this, more] { showMenu(more->mapToGlobal(QPoint(0, more->height()))); });
    m_actionsLayout->addWidget(more);
}

void JobRow::showMenu(const QPoint& globalPos)
{
    // The row can be rebuilt while the menu is up (the job finishes, the next
    // one starts): the menu belongs to the window and every action checks
    // that the row still exists.
    auto* menu = new QMenu(window());
    menu->setAttribute(Qt::WA_DeleteOnClose);
    QPointer<JobRow> self(this);
    const QString id = m_id;
    auto act = [menu, self, id](const QString& text, auto signal) {
        return menu->addAction(text, menu, [self, id, signal] {
            if (self)
                emit(self.data()->*signal)(id);
        });
    };
    act(QStringLiteral("Open details"), &JobRow::activated);
    switch (m_state) {
    case JobState::Running:
        act(QStringLiteral("Pause"), &JobRow::pauseRequested);
        act(QStringLiteral("Cancel job…"), &JobRow::cancelRequested);
        break;
    case JobState::Paused:
        act(QStringLiteral("Resume"), &JobRow::resumeRequested);
        act(QStringLiteral("Cancel job…"), &JobRow::cancelRequested);
        break;
    case JobState::Cancelling:
        break;
    case JobState::Queued:
        act(QStringLiteral("Move up"), &JobRow::moveUpRequested)->setEnabled(m_canUp);
        act(QStringLiteral("Move down"), &JobRow::moveDownRequested)->setEnabled(m_canDown);
        menu->addSeparator();
        act(QStringLiteral("Remove from queue"), &JobRow::removeRequested);
        break;
    case JobState::Succeeded:
        if (m_record.outcome && !m_record.outcome->outputs.isEmpty())
            act(QStringLiteral("Show in folder"), &JobRow::showInFolderRequested);
        act(QStringLiteral("Run again"), &JobRow::retryRequested);
        menu->addSeparator();
        act(QStringLiteral("Remove"), &JobRow::removeRequested);
        break;
    default:
        act(QStringLiteral("Retry"), &JobRow::retryRequested);
        menu->addSeparator();
        act(QStringLiteral("Remove"), &JobRow::removeRequested);
        break;
    }
    menu->popup(globalPos);
}

void JobRow::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    if (m_selected) {
        // Like a selected table row: accent_lo and the accent bar at its left edge.
        p.fillRect(rect(), color::accent_lo);
        p.fillRect(QRect(0, 0, 2, height()), color::accent);
    } else if (underMouse()) {
        p.fillRect(rect(), color::bg_3);
    }
    if (m_divider)
        p.fillRect(QRect(0, height() - 1, width(), 1), color::border_1);
}

void JobRow::mousePressEvent(QMouseEvent* e)
{
    m_pressPos = e->pos();
    const bool left = e->button() == Qt::LeftButton;
    QWidget::mousePressEvent(e);
    if (left) {
        e->accept();
        emit clicked(m_id); // last: a slot may rebuild the list and delete this row
    }
}

void JobRow::mouseMoveEvent(QMouseEvent* e)
{
    if (!(e->buttons() & Qt::LeftButton) || !m_reorderable)
        return;
    if ((e->pos() - m_pressPos).manhattanLength() < QApplication::startDragDistance())
        return;
    if (!m_grip->geometry().adjusted(-8, -8, 8, 8).contains(m_pressPos))
        return;
    auto* drag = new QDrag(this);
    auto* mime = new QMimeData;
    mime->setData(QLatin1String(kJobMime), m_id.toUtf8());
    drag->setMimeData(mime);
    // scaledToWidth() counts device pixels: 320 logical px at any scaling.
    drag->setPixmap(grab().scaledToWidth(qRound(std::min(width(), 320) * devicePixelRatioF()), Qt::SmoothTransformation));
    drag->setHotSpot(QPoint(16, height() / 2));
    // The list waits for the drag to end before it rebuilds its rows.
    QPointer<JobRow> self(this);
    m_dragging = true;
    emit dragStarted(m_id);
    if (!self)
        return; // the drag was this row's child
    drag->exec(Qt::MoveAction);
    if (self)
        m_dragging = false;
}

void JobRow::mouseDoubleClickEvent(QMouseEvent*) { emit activated(m_id); }

void JobRow::keyPressEvent(QKeyEvent* e)
{
    switch (e->key()) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Space: {
        QPointer<JobRow> self(this);
        const QString id = m_id;
        emit clicked(id);
        if (self)
            emit activated(id);
        return;
    }
    case Qt::Key_Up:
        if (e->modifiers() & Qt::ControlModifier) { if (m_canUp) emit moveUpRequested(m_id); return; }
        emit focusNeighbourRequested(m_id, -1);
        return;
    case Qt::Key_Down:
        if (e->modifiers() & Qt::ControlModifier) { if (m_canDown) emit moveDownRequested(m_id); return; }
        emit focusNeighbourRequested(m_id, +1);
        return;
    case Qt::Key_Delete:
        if (m_state == JobState::Queued || isTerminal(m_state))
            emit removeRequested(m_id);
        return;
    case Qt::Key_Menu:
        showMenu(mapToGlobal(QPoint(width() - 40, height())));
        return;
    default: break;
    }
    QWidget::keyPressEvent(e);
}

void JobRow::contextMenuEvent(QContextMenuEvent* e) { showMenu(e->globalPos()); }

// ---- list -------------------------------------------------------------------------

JobList::JobList(AppContext& ctx, JobQueueModel& model, JobRow::Variant variant, QWidget* parent)
    : QWidget(parent), m_ctx(ctx), m_model(model), m_variant(variant)
{
    setProperty("fwNoRing", true);
    setAcceptDrops(true);
    m_layout = vbox(this, 0);
    connect(&m_model, &QAbstractItemModel::modelReset, this, &JobList::rebuild);
    connect(&m_model, &QAbstractItemModel::dataChanged, this, [this](const QModelIndex& tl, const QModelIndex& br) {
        for (int r = tl.row(); r <= br.row(); ++r)
            refreshRow(r);
    });
    connect(&m_model, &QAbstractItemModel::rowsInserted, this, &JobList::rebuild);
    connect(&m_model, &QAbstractItemModel::rowsRemoved, this, &JobList::rebuild);
    connect(&m_ctx.queue(), &JobQueue::heldChanged, this, &JobList::rebuild); // "Starts next" / "Waits for the queue to resume"
    m_tick.setInterval(1000);
    connect(&m_tick, &QTimer::timeout, this, [this] {
        bool waiting = false;
        for (int r = 0; r < m_model.rowCount(); ++r)
            if (const JobRecord* rec = m_model.recordAt(r); rec && rec->state == JobState::Queued && rec->notBefore.isValid()) {
                refreshRow(r);
                waiting = true;
            }
        if (!waiting)
            m_tick.stop();
    });
    rebuild();
}

void JobList::setFilter(std::function<bool(const JobRecord&)> filter)
{
    m_filter = std::move(filter);
    rebuild();
}

void JobList::setEmptyWidget(QWidget* w)
{
    m_empty = w;
    w->setParent(this);
    if (auto* state = qobject_cast<EmptyState*>(w))
        state->setCompact(true); // inside a list: never a tall block
    rebuild();
}

QStringList JobList::visibleIds() const
{
    QStringList out;
    for (auto* r : m_rows)
        out << r->id();
    return out;
}

void JobList::wire(JobRow* row)
{
    auto& q = m_ctx.queue();
    connect(row, &JobRow::clicked, this, &JobList::setSelectedId);
    connect(row, &JobRow::activated, this, &JobList::activated);
    connect(row, &JobRow::pauseRequested, this, [&q](const QString& id) { q.pauseJob(id); });
    connect(row, &JobRow::resumeRequested, this, [&q](const QString& id) { q.resumeJob(id); });
    // Up / down change places with the neighbouring QUEUED job: running and
    // finished jobs in between (and rows a filter hides) are stepped over.
    connect(row, &JobRow::moveUpRequested, this, [this, &q](const QString& id) {
        for (int k = m_model.rowOf(id) - 1; k >= 0; --k)
            if (const JobRecord* o = m_model.recordAt(k); o && o->state == JobState::Queued) {
                q.move(id, k);
                return;
            }
    });
    connect(row, &JobRow::moveDownRequested, this, [this, &q](const QString& id) {
        const int r = m_model.rowOf(id);
        for (int k = r + 1; r >= 0 && k < m_model.rowCount(); ++k)
            if (const JobRecord* o = m_model.recordAt(k); o && o->state == JobState::Queued) {
                q.move(id, k);
                return;
            }
    });
    connect(row, &JobRow::removeRequested, this, [&q](const QString& id) { q.remove(id); });
    connect(row, &JobRow::retryRequested, this, [this, &q](const QString& id) {
        auto r = q.retry(id);
        if (r.ok()) {
            q.startNow(r.value()); // this job only; a paused queue stays paused
            return;
        }
        if (isSignalConnected(QMetaMethod::fromSignal(&JobList::retryFailed)))
            emit retryFailed(id, r.error().message);
        else
            Sheet::notify(window(), QStringLiteral("This job cannot run again"), r.error().message, Sheet::Icon::Error);
    });
    connect(row, &JobRow::showInFolderRequested, this, [this](const QString& id) {
        if (auto rec = m_ctx.queue().record(id); rec && rec->outcome && !rec->outcome->outputs.isEmpty()) {
            const QString path = rec->outcome->outputs.first();
            if (!openInFileManager(path))
                Sheet::notify(window(), QStringLiteral("The output is not there any more"),
                              QStringLiteral("%1 was moved or deleted, and so was its folder.").arg(nativePath(path)), Sheet::Icon::Warning);
        }
    });
    // The confirm dialog is the page's job (it knows the window). Queued: the
    // row's button handler has returned before the sheet's nested loop runs.
    connect(row, &JobRow::cancelRequested, this, &JobList::cancelRequested, Qt::QueuedConnection);
    connect(row, &JobRow::focusNeighbourRequested, this, [this](const QString& id, int delta) {
        for (size_t i = 0; i < m_rows.size(); ++i)
            if (m_rows[i]->id() == id) {
                const int j = int(i) + delta;
                if (j >= 0 && j < int(m_rows.size()))
                    m_rows[size_t(j)]->setFocus(Qt::TabFocusReason);
                return;
            }
    });
}

void JobList::rebuild()
{
    if (m_rebuildPending)
        return;
    m_rebuildPending = true;
    QTimer::singleShot(0, this, &JobList::rebuildNow);
}

void JobList::rebuildNow()
{
    for (auto* r : m_rows)
        if (r->isBusy()) { // a drag is running: come back when it has ended
            QTimer::singleShot(100, this, &JobList::rebuildNow);
            return;
        }
    m_rebuildPending = false;
    // Rows are kept for the jobs that stay: a row that has the keyboard focus
    // or an open menu survives a start, a finish or a move of any job.
    QHash<QString, JobRow*> old;
    for (auto* r : m_rows)
        old.insert(r->id(), r);
    m_rows.clear();
    while (QLayoutItem* it = m_layout->takeAt(0))
        delete it;
    const int maxAttempts = int(m_ctx.settings().value(general::keys::kAutoRetryCount).toInt()) + 1;
    int queuedPos = 0;
    bool anyWaiting = false;
    for (int r = 0; r < m_model.rowCount(); ++r) {
        const JobRecord* rec = m_model.recordAt(r);
        if (!rec)
            continue;
        if (rec->state == JobState::Queued)
            ++queuedPos;
        if (m_filter && !m_filter(*rec))
            continue;
        if (m_maxRows >= 0 && int(m_rows.size()) >= m_maxRows)
            continue;
        JobRow* row = old.take(rec->id);
        if (!row) {
            row = new JobRow(m_variant, this);
            wire(row);
        }
        row->setRecord(*rec, queuedPos, maxAttempts, true);
        row->setSelected(rec->id == m_selected);
        if (!m_rows.empty())
            QWidget::setTabOrder(m_rows.back(), row); // Tab follows the order on screen
        m_rows.push_back(row);
        m_layout->addWidget(row);
        if (rec->notBefore.isValid() && rec->state == JobState::Queued)
            anyWaiting = true;
    }
    for (JobRow* gone : std::as_const(old)) {
        gone->hide();
        gone->deleteLater();
    }
    for (size_t i = 0; i < m_rows.size(); ++i) {
        m_rows[i]->setDivider(i + 1 < m_rows.size());
        const int mr = m_model.rowOf(m_rows[i]->id());
        bool canUp = false, canDown = false;
        if (const JobRecord* rec = m_model.recordAt(mr); rec && rec->state == JobState::Queued) {
            for (int k = mr - 1; k >= 0; --k)
                if (const JobRecord* o = m_model.recordAt(k); o && o->state == JobState::Queued) { canUp = true; break; }
            for (int k = mr + 1; k < m_model.rowCount(); ++k)
                if (const JobRecord* o = m_model.recordAt(k); o && o->state == JobState::Queued) { canDown = true; break; }
        }
        m_rows[i]->setCanMoveUp(canUp);
        m_rows[i]->setCanMoveDown(canDown);
    }
    if (m_empty) {
        m_empty->setVisible(m_rows.empty());
        if (m_rows.empty())
            m_layout->addWidget(m_empty);
    }
    if (anyWaiting && !m_tick.isActive())
        m_tick.start();
    if (!m_selected.isEmpty() && m_model.rowOf(m_selected) < 0) {
        m_selected.clear();
        emit selectionChanged({});
    }
    emit countChanged();
}

void JobList::refreshRow(int modelRow)
{
    const JobRecord* rec = m_model.recordAt(modelRow);
    if (!rec)
        return;
    if (m_filter && !m_filter(*rec)) {
        for (auto* r : m_rows)
            if (r->id() == rec->id) {
                rebuild();
                return;
            }
        return;
    }
    const int maxAttempts = int(m_ctx.settings().value(general::keys::kAutoRetryCount).toInt()) + 1;
    int queuedPos = 0;
    for (int r = 0; r <= modelRow; ++r)
        if (const JobRecord* o = m_model.recordAt(r); o && o->state == JobState::Queued)
            ++queuedPos;
    for (auto* r : m_rows)
        if (r->id() == rec->id) {
            const JobState before = r->state();
            r->setRecord(*rec, queuedPos, maxAttempts, true);
            if (before != rec->state)
                rebuild(); // filters and neighbours may change
            if (rec->notBefore.isValid() && !m_tick.isActive())
                m_tick.start();
            return;
        }
    rebuild(); // newly visible
}

void JobList::setSelectedId(const QString& id)
{
    if (m_selected == id)
        return;
    m_selected = id;
    for (auto* r : m_rows)
        r->setSelected(r->id() == id);
    emit selectionChanged(id);
}

int JobList::dropIndexAt(const QPoint& p) const
{
    // Returns the model row before which the dragged job should be inserted.
    for (size_t i = 0; i < m_rows.size(); ++i) {
        const QRect g = m_rows[i]->geometry();
        if (p.y() < g.center().y())
            return m_model.rowOf(m_rows[i]->id());
    }
    // Below the last row shown: right after it, not at the end of everything
    // (the dock shows the first few rows only).
    return m_rows.empty() ? m_model.rowCount() : m_model.rowOf(m_rows.back()->id()) + 1;
}

void JobList::dragEnterEvent(QDragEnterEvent* e)
{
    if (e->mimeData()->hasFormat(QLatin1String(kJobMime)))
        e->acceptProposedAction();
}

void JobList::dragMoveEvent(QDragMoveEvent* e)
{
    m_dropIndex = dropIndexAt(e->position().toPoint());
    e->acceptProposedAction();
    update();
}

void JobList::dragLeaveEvent(QDragLeaveEvent*)
{
    m_dropIndex = -1;
    update();
}

void JobList::dropEvent(QDropEvent* e)
{
    const QString id = QString::fromUtf8(e->mimeData()->data(QLatin1String(kJobMime)));
    int target = dropIndexAt(e->position().toPoint());
    const int from = m_model.rowOf(id);
    m_dropIndex = -1;
    update();
    if (from < 0)
        return;
    // A queued job stays among the queued ones: never ahead of the first, never
    // behind the last of them (running and finished rows keep their places).
    int firstQueued = -1, lastQueued = -1;
    for (int r = 0; r < m_model.rowCount(); ++r)
        if (const JobRecord* o = m_model.recordAt(r); o && o->state == JobState::Queued) {
            if (firstQueued < 0)
                firstQueued = r;
            lastQueued = r;
        }
    if (firstQueued >= 0)
        target = std::clamp(target, firstQueued, lastQueued + 1);
    if (target > from)
        --target;
    if (target != from)
        m_ctx.queue().move(id, target);
    e->acceptProposedAction();
}

void JobList::paintEvent(QPaintEvent*)
{
    if (m_dropIndex < 0)
        return;
    QPainter p(this);
    int y = 0;
    bool found = false;
    for (auto* r : m_rows) {
        if (m_model.rowOf(r->id()) >= m_dropIndex) {
            y = r->geometry().top();
            found = true;
            break;
        }
    }
    if (!found && !m_rows.empty())
        y = m_rows.back()->geometry().bottom();
    p.fillRect(QRect(0, std::max(0, y - 1), width(), 2), color::accent);
}

// ---- dock -----------------------------------------------------------------------------

JobsDock::JobsDock(AppContext& ctx, JobQueueModel& model, QWidget* parent)
    : Card(parent, Card::Variant::Table), m_ctx(ctx), m_model(model)
{
    setMinimumWidth(size::dock);
    auto* h = header();
    h->addWidget(heading(QStringLiteral("Jobs"), TextStyle::Subtitle, this));
    m_count = new Badge(QStringLiteral("0"), Tone::Neutral, this);
    m_count->setCount(true);
    h->addWidget(m_count);
    h->addStretch(1);
    m_pause = new IconButton(QStringLiteral("pause"), QStringLiteral("Pause queue"), IconButton::Size::Small, this);
    connect(m_pause, &QToolButton::clicked, this, [this] { m_ctx.queue().holdQueue(!m_ctx.queue().isHeld()); });
    h->addWidget(m_pause);
    auto* more = new IconButton(QStringLiteral("more"), QStringLiteral("More"), IconButton::Size::Small, this);
    connect(more, &QToolButton::clicked, this, [this, more] {
        QMenu menu(this);
        menu.addAction(QStringLiteral("Open queue"), this, [this] { emit openQueueRequested(); });
        menu.addAction(QStringLiteral("Clear finished"), this, [this] { m_ctx.queue().clearFinished(); });
        menu.exec(more->mapToGlobal(QPoint(0, more->height())));
    });
    h->addWidget(more);

    // Running block
    m_running = new Card(this, Card::Variant::Regular);
    m_running->setSelected(true);
    auto* rl = m_running->body();
    rl->setSpacing(space::sm);
    rl->setContentsMargins(0, 0, 0, 0);
    auto* top = new QHBoxLayout;
    top->setSpacing(space::sm);
    auto* tcol = new QVBoxLayout;
    tcol->setSpacing(2);
    m_runTitle = new ElideLabel({}, Qt::ElideRight, m_running);
    m_runTitle->setFont(font::bodyStrong());
    setTextColor(m_runTitle, color::fg_0);
    m_runMeta = new ElideLabel({}, Qt::ElideMiddle, m_running);
    m_runMeta->setFont(font::mono());
    setTextColor(m_runMeta, color::fg_3);
    tcol->addWidget(m_runTitle);
    tcol->addWidget(m_runMeta);
    top->addLayout(tcol, 1);
    m_runWatch = new IconButton(QStringLiteral("maximize"), QStringLiteral("Watch this job"), IconButton::Size::Small, m_running);
    m_runWatch->setToolTip(QStringLiteral("Watch this job: stages, clocks and the live log"));
    m_runPause = new IconButton(QStringLiteral("pause"), QStringLiteral("Pause job"), IconButton::Size::Small, m_running);
    m_runCancel = new IconButton(QStringLiteral("close"), QStringLiteral("Cancel job"), IconButton::Size::Small, m_running);
    connect(m_runWatch, &QToolButton::clicked, this, [this] { emit watchRequested(m_runningId); });
    connect(m_runPause, &QToolButton::clicked, this, [this] {
        auto rec = m_ctx.queue().record(m_runningId);
        if (!rec)
            return;
        if (rec->state == JobState::Paused)
            m_ctx.queue().resumeJob(m_runningId);
        else
            m_ctx.queue().pauseJob(m_runningId);
    });
    connect(m_runCancel, &QToolButton::clicked, this, [this] {
        // After this click handler: the page's confirm sheet must not run inside it.
        QMetaObject::invokeMethod(this, [this, id = m_runningId] { emit cancelRequested(id); }, Qt::QueuedConnection);
    });
    top->addWidget(m_runWatch);
    top->addWidget(m_runPause);
    top->addWidget(m_runCancel);
    rl->addLayout(top);
    m_progress = new ProgressBlock(m_running);
    rl->addWidget(m_progress);
    // The last lines of the running job's log, so the dock says what is
    // happening even when the page shows a form.
    m_tail = new ActivityWell(ctx.logModel(), m_running);
    m_tail->setSmall(true);
    m_tail->setMaxLines(3);
    rl->addWidget(m_tail);
    body()->addWidget(m_running);
    body()->setSpacing(0);
    m_running->hide();

    m_list = new JobList(ctx, model, JobRow::Variant::Dock, this);
    applyListFilter();
    m_list->setMaxRows(6);
    connect(m_list, &JobList::activated, this, &JobsDock::openJobRequested);
    connect(m_list, &JobList::cancelRequested, this, &JobsDock::cancelRequested);
    body()->addWidget(m_list);

    m_empty = new EmptyState(QStringLiteral("queue"), QStringLiteral("No jobs yet"),
                             QStringLiteral("Jobs you add appear here and run one after another while you keep working."), this);
    m_empty->setCompact(true);
    body()->addWidget(m_empty);

    auto* f = footer();
    m_footer = caption({}, this);
    f->addWidget(m_footer, 1);
    auto* open = new Link(QStringLiteral("Open queue"), this);
    connect(open, &Link::activated, this, &JobsDock::openQueueRequested);
    f->addWidget(open);

    connect(&m_model, &QAbstractItemModel::dataChanged, this, [this] { refresh(); });
    connect(&m_model, &QAbstractItemModel::modelReset, this, [this] { refresh(); });
    connect(&m_model, &JobQueueModel::countsChanged, this, [this] { refresh(); });
    connect(&m_ctx.queue(), &JobQueue::heldChanged, this, [this] { refresh(); });
    refresh();
}

// System jobs (update checks) never show in the dock, like in the Queue page
// and the header chip. The job of the running block is not listed again; a
// second job that runs at the same time is, so it can be paused and cancelled.
void JobsDock::applyListFilter()
{
    m_list->setFilter([shown = m_runningId](const JobRecord& r) { return !jobs::isSystemKind(r.kind) && r.id != shown; });
}

void JobsDock::refresh()
{
    const int rows = m_model.rowCount();
    const bool held = m_ctx.queue().isHeld();
    m_pause->setIconName(held ? QStringLiteral("play") : QStringLiteral("pause"));
    m_pause->setToolTip(held ? QStringLiteral("Resume queue") : QStringLiteral("Pause queue"));
    m_pause->setAccessibleName(m_pause->toolTip());

    const JobRecord* running = nullptr;
    int total = 0; // user jobs only; system jobs (update.*) are hidden everywhere in the UI
    int queued = 0;
    int runningCount = 0;
    qint64 etaMs = -1;
    for (int r = 0; r < rows; ++r) {
        const JobRecord* rec = m_model.recordAt(r);
        if (!rec || jobs::isSystemKind(rec->kind))
            continue;
        ++total;
        if (rec->isActive() && !running)
            running = rec;
        if (rec->state == JobState::Queued)
            ++queued;
        if (rec->state == JobState::Running) {
            ++runningCount;
            etaMs = std::max(etaMs, jobTimeLeftMs(*rec)); // jobs run side by side: the longest one
        }
    }
    m_count->setText(QString::number(total));
    m_count->setVisible(total > 0);
    const QString shownBefore = m_runningId;
    if (running) {
        m_runningId = running->id;
        m_runTitle->setFullText(running->title);
        m_runMeta->setFullText(running->subtitle);
        const auto& p = running->progress;
        QString detail = progressDetailText(p);
        if (!p.phase.isEmpty())
            detail = detail.isEmpty() ? p.phase : QStringLiteral("%1 · %2").arg(p.phase, detail);
        m_progress->setProgress(p.overallFraction, p.indeterminate, detail);
        m_progress->setState(running->state != JobState::Running ? ProgressBar::State::Paused
                             : p.indeterminate ? ProgressBar::State::Indeterminate : ProgressBar::State::Determinate);
        m_progress->setPhases(running->phases, p.phaseIndex);
        const bool cancelling = running->state == JobState::Cancelling;
        m_runPause->setVisible(!cancelling);
        m_runCancel->setVisible(!cancelling);
        m_runPause->setIconName(running->state == JobState::Paused ? QStringLiteral("play") : QStringLiteral("pause"));
        m_runPause->setToolTip(running->state == JobState::Paused ? QStringLiteral("Resume job") : QStringLiteral("Pause job"));
        m_runPause->setAccessibleName(m_runPause->toolTip());
        if (m_tail->jobId() != running->id)
            m_tail->setJobId(running->id);
        // The page's monitor shows this job already: nothing twice.
        m_running->setVisible(running->id != m_watched);
    } else {
        m_runningId.clear();
        m_running->hide();
    }
    if (m_runningId != shownBefore)
        applyListFilter();
    m_empty->setVisible(total == 0);
    QString foot;
    if (held)
        foot = QStringLiteral("Queue paused · %1 waiting").arg(queued);
    else if (etaMs >= 0 && queued > 0)
        foot = QStringLiteral("%1 · %2 more queued").arg(formatTimeLeft(etaMs), QString::number(queued));
    else if (etaMs >= 0)
        foot = formatTimeLeft(etaMs);
    else if (runningCount > 0 && queued > 0)
        foot = QStringLiteral("Running · %1 more queued").arg(queued);
    else if (runningCount > 0)
        foot = QStringLiteral("Running");
    else if (queued > 0)
        foot = QStringLiteral("%1 queued").arg(queued);
    else if (total > 0)
        foot = QStringLiteral("Nothing running");
    else
        foot = QStringLiteral("Nothing in the queue");
    m_footer->setText(foot);
}

void JobsDock::setWatchedJob(const QString& id)
{
    if (m_watched == id)
        return;
    m_watched = id;
    refresh();
}

} // namespace fw::ui
