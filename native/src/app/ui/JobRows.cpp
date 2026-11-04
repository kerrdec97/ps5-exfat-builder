#include "app/ui/JobRows.h"

#include "app/AppContext.h"
#include "app/ui/Icons.h"
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
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QVBoxLayout>

namespace fw::ui {

using namespace theme;

static const char* kJobMime = "application/x-fw-job-id";

Tone toneForState(JobState state)
{
    switch (state) {
    case JobState::Running: return Tone::Info;
    case JobState::Paused: return Tone::Neutral;
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
    case JobState::Running: return QStringLiteral("dot");
    case JobState::Paused: return QStringLiteral("pause_circle");
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

static QString durationText(const JobRecord& r)
{
    if (!r.started.isValid() || !r.finished.isValid())
        return {};
    const qint64 s = r.started.secsTo(r.finished);
    if (s < 60)
        return QStringLiteral("%1 s").arg(s);
    if (s < 3600)
        return QStringLiteral("%1 min %2 s").arg(s / 60).arg(s % 60);
    return QStringLiteral("%1 h %2 min").arg(s / 3600).arg((s % 3600) / 60);
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
    m_grip->setCursor(Qt::SizeAllCursor);
    m_grip->setToolTip(QStringLiteral("Drag to reorder"));
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
    h->addWidget(m_badge);
    m_actions = new QWidget(this);
    m_actionsLayout = hbox(m_actions, 2);
    m_actions->setFixedWidth(queue ? 100 : 28);
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
    rebuildActions();
}

void JobRow::setCanMoveDown(bool on)
{
    m_canDown = on;
    rebuildActions();
}

void JobRow::setRecord(const JobRecord& r, int queuePosition, int maxAttempts, bool reorderable)
{
    const bool changedState = m_record.id != r.id || m_record.state != r.state;
    m_record = r;
    m_id = r.id;
    m_state = r.state;
    m_reorderable = reorderable && r.state == JobState::Queued;
    m_grip->setVisible(m_reorderable || m_variant == Variant::Queue);
    m_grip->setEnabled(m_reorderable);
    if (m_variant == Variant::Queue) {
        m_grip->setVisible(true);
        static_cast<QLabel*>(m_grip)->setPixmap(m_reorderable ? iconPixmap(QStringLiteral("grip"), size::icon, color::fg_5, devicePixelRatioF()) : QPixmap());
        m_grip->setCursor(m_reorderable ? Qt::SizeAllCursor : Qt::ArrowCursor);
    } else {
        m_grip->setVisible(true);
        static_cast<QLabel*>(m_grip)->setPixmap(m_reorderable ? iconPixmap(QStringLiteral("grip"), size::icon, color::fg_5, devicePixelRatioF())
                                                              : iconPixmap(stateIcon(r.state), size::icon, stateColor(r.state), devicePixelRatioF()));
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
        showBar = true;
        m_bar->setFraction(r.progress.overallFraction);
        m_bar->setState(r.state == JobState::Paused ? ProgressBar::State::Paused
                        : r.progress.indeterminate ? ProgressBar::State::Indeterminate : ProgressBar::State::Determinate);
        {
            QStringList parts;
            if (!r.progress.phase.isEmpty())
                parts << r.progress.phase;
            if (r.progress.unitIsBytes && r.progress.ratePerSec > 0)
                parts << formatRate(r.progress.ratePerSec);
            const QString eta = formatTimeLeft(r.progress.etaMs);
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
        else
            note = queuePosition <= 1 ? QStringLiteral("Starts next") : QStringLiteral("Waits for the running job");
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
    m_note->setToolTip(note);
    setTextColor(m_note, noteColor);

    // Badge
    QString badgeText = jobStateName(r.state);
    Tone tone = toneForState(r.state);
    if (r.state == JobState::Running)
        badgeText = r.progress.indeterminate ? QStringLiteral("Running") : QStringLiteral("Running · %1").arg(thinPercent(r.progress.overallFraction));
    else if (r.state == JobState::Queued && queuePosition > 0 && m_variant == Variant::Queue) {
        static const char* suffix[] = {"th", "st", "nd", "rd"};
        const int n = queuePosition;
        const int s = (n % 100 >= 11 && n % 100 <= 13) ? 0 : (n % 10 <= 3 ? n % 10 : 0);
        badgeText = QStringLiteral("Queued · %1%2").arg(n).arg(QLatin1String(suffix[s]));
    } else if (r.state == JobState::Failed && r.error && m_variant == Variant::Queue) {
        badgeText = QStringLiteral("Failed");
    } else if (r.state == JobState::Succeeded && r.outcome && r.outcome->verification == Verification::Passed) {
        badgeText = QStringLiteral("Done");
    }
    m_badge->set(badgeText, tone);
    m_badge->setIconName(r.state == JobState::Queued || r.state == JobState::Cancelled || r.state == JobState::Paused ? QString() : defaultBadgeIcon(tone));
    if (changedState)
        rebuildActions();
    update();
}

void JobRow::rebuildActions()
{
    while (QLayoutItem* it = m_actionsLayout->takeAt(0)) {
        delete it->widget();
        delete it;
    }
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
    case JobState::Queued:
        add(QStringLiteral("arrow_up"), QStringLiteral("Move up"), &JobRow::moveUpRequested, m_canUp);
        add(QStringLiteral("arrow_down"), QStringLiteral("Move down"), &JobRow::moveDownRequested, m_canDown);
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
    QMenu menu(this);
    menu.addAction(QStringLiteral("Open details"), this, [this] { emit activated(m_id); });
    switch (m_state) {
    case JobState::Running:
        menu.addAction(QStringLiteral("Pause"), this, [this] { emit pauseRequested(m_id); });
        menu.addAction(QStringLiteral("Cancel job…"), this, [this] { emit cancelRequested(m_id); });
        break;
    case JobState::Paused:
        menu.addAction(QStringLiteral("Resume"), this, [this] { emit resumeRequested(m_id); });
        menu.addAction(QStringLiteral("Cancel job…"), this, [this] { emit cancelRequested(m_id); });
        break;
    case JobState::Queued:
        menu.addAction(QStringLiteral("Move up"), this, [this] { emit moveUpRequested(m_id); })->setEnabled(m_canUp);
        menu.addAction(QStringLiteral("Move down"), this, [this] { emit moveDownRequested(m_id); })->setEnabled(m_canDown);
        menu.addSeparator();
        menu.addAction(QStringLiteral("Remove from queue"), this, [this] { emit removeRequested(m_id); });
        break;
    case JobState::Succeeded:
        if (m_record.outcome && !m_record.outcome->outputs.isEmpty())
            menu.addAction(QStringLiteral("Show in folder"), this, [this] { emit showInFolderRequested(m_id); });
        menu.addAction(QStringLiteral("Run again"), this, [this] { emit retryRequested(m_id); });
        menu.addSeparator();
        menu.addAction(QStringLiteral("Remove"), this, [this] { emit removeRequested(m_id); });
        break;
    default:
        menu.addAction(QStringLiteral("Retry"), this, [this] { emit retryRequested(m_id); });
        menu.addSeparator();
        menu.addAction(QStringLiteral("Remove"), this, [this] { emit removeRequested(m_id); });
        break;
    }
    menu.exec(globalPos);
}

void JobRow::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    if (m_selected)
        p.fillRect(rect(), color::accent_lo);
    else if (underMouse())
        p.fillRect(rect(), color::bg_3);
    if (m_divider)
        p.fillRect(QRect(0, height() - 1, width(), 1), color::border_1);
}

void JobRow::mousePressEvent(QMouseEvent* e)
{
    m_pressPos = e->pos();
    if (e->button() == Qt::LeftButton)
        emit clicked(m_id);
    QWidget::mousePressEvent(e);
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
    drag->setPixmap(grab().scaledToWidth(std::min(width(), 320), Qt::SmoothTransformation));
    drag->setHotSpot(QPoint(16, height() / 2));
    emit dragStarted(m_id);
    drag->exec(Qt::MoveAction);
}

void JobRow::mouseDoubleClickEvent(QMouseEvent*) { emit activated(m_id); }

void JobRow::keyPressEvent(QKeyEvent* e)
{
    switch (e->key()) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Space:
        emit clicked(m_id);
        emit activated(m_id);
        return;
    case Qt::Key_Up:
        if (e->modifiers() & Qt::ControlModifier) { if (m_canUp) emit moveUpRequested(m_id); return; }
        focusPreviousChild();
        return;
    case Qt::Key_Down:
        if (e->modifiers() & Qt::ControlModifier) { if (m_canDown) emit moveDownRequested(m_id); return; }
        focusNextChild();
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
    connect(row, &JobRow::moveUpRequested, this, [this, &q](const QString& id) {
        const int r = m_model.rowOf(id);
        if (r > 0)
            q.move(id, r - 1);
    });
    connect(row, &JobRow::moveDownRequested, this, [this, &q](const QString& id) {
        const int r = m_model.rowOf(id);
        if (r >= 0 && r + 1 < m_model.rowCount())
            q.move(id, r + 1);
    });
    connect(row, &JobRow::removeRequested, this, [&q](const QString& id) { q.remove(id); });
    connect(row, &JobRow::retryRequested, this, [&q](const QString& id) {
        (void)q.retry(id);
        q.holdQueue(false);
    });
    connect(row, &JobRow::showInFolderRequested, this, [this](const QString& id) {
        if (auto rec = m_ctx.queue().record(id); rec && rec->outcome && !rec->outcome->outputs.isEmpty())
            openInFileManager(rec->outcome->outputs.first());
    });
    connect(row, &JobRow::cancelRequested, this, [this](const QString& id) {
        // The confirm dialog is the page's job (it knows the window); fall back to direct cancel.
        emit activated(QStringLiteral("cancel:") + id);
    });
}

void JobList::rebuild()
{
    for (auto* r : m_rows)
        r->deleteLater();
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
        auto* row = new JobRow(m_variant, this);
        row->setRecord(*rec, queuedPos, maxAttempts, true);
        row->setSelected(rec->id == m_selected);
        wire(row);
        m_rows.push_back(row);
        m_layout->addWidget(row);
        if (rec->notBefore.isValid() && rec->state == JobState::Queued)
            anyWaiting = true;
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
    return m_model.rowCount();
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
    if (target > from)
        --target;
    // Never move ahead of running / finished rows that precede the queued block.
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
    connect(m_pause, &QToolButton::clicked, this, [this] { m_ctx.queue().holdQueue(!m_ctx.queue().isHeld()); refresh(); });
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
    m_runPause = new IconButton(QStringLiteral("pause"), QStringLiteral("Pause job"), IconButton::Size::Small, m_running);
    m_runCancel = new IconButton(QStringLiteral("close"), QStringLiteral("Cancel job"), IconButton::Size::Small, m_running);
    connect(m_runPause, &QToolButton::clicked, this, [this] {
        auto rec = m_ctx.queue().record(m_runningId);
        if (!rec)
            return;
        if (rec->state == JobState::Paused)
            m_ctx.queue().resumeJob(m_runningId);
        else
            m_ctx.queue().pauseJob(m_runningId);
    });
    connect(m_runCancel, &QToolButton::clicked, this, [this] { emit openJobRequested(QStringLiteral("cancel:") + m_runningId); });
    top->addWidget(m_runPause);
    top->addWidget(m_runCancel);
    rl->addLayout(top);
    m_progress = new ProgressBlock(m_running);
    rl->addWidget(m_progress);
    body()->addWidget(m_running);
    body()->setSpacing(0);
    m_running->hide();

    m_list = new JobList(ctx, model, JobRow::Variant::Dock, this);
    m_list->setFilter([](const JobRecord& r) { return r.state != JobState::Running && r.state != JobState::Paused; });
    m_list->setMaxRows(6);
    connect(m_list, &JobList::activated, this, &JobsDock::openJobRequested);
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
    refresh();
}

void JobsDock::refresh()
{
    const int total = m_model.rowCount();
    m_count->setText(QString::number(total));
    m_count->setVisible(total > 0);
    const bool held = m_ctx.queue().isHeld();
    m_pause->setIconName(held ? QStringLiteral("play") : QStringLiteral("pause"));
    m_pause->setToolTip(held ? QStringLiteral("Resume queue") : QStringLiteral("Pause queue"));
    m_pause->setAccessibleName(m_pause->toolTip());

    const JobRecord* running = nullptr;
    int queued = 0;
    qint64 etaMs = -1;
    for (int r = 0; r < total; ++r) {
        const JobRecord* rec = m_model.recordAt(r);
        if (!rec)
            continue;
        if ((rec->state == JobState::Running || rec->state == JobState::Paused) && !running)
            running = rec;
        if (rec->state == JobState::Queued)
            ++queued;
        if (rec->state == JobState::Running && rec->progress.etaMs >= 0)
            etaMs = std::max<qint64>(etaMs, 0) + rec->progress.etaMs;
    }
    if (running) {
        m_runningId = running->id;
        m_runTitle->setFullText(running->title);
        m_runMeta->setFullText(running->subtitle);
        const auto& p = running->progress;
        QString detail = progressDetailText(p);
        if (!p.phase.isEmpty())
            detail = detail.isEmpty() ? p.phase : QStringLiteral("%1 · %2").arg(p.phase, detail);
        m_progress->setProgress(p.overallFraction, p.indeterminate, detail);
        m_progress->setState(running->state == JobState::Paused ? ProgressBar::State::Paused
                             : p.indeterminate ? ProgressBar::State::Indeterminate : ProgressBar::State::Determinate);
        m_progress->setPhases(running->phases, p.phaseIndex);
        m_runPause->setIconName(running->state == JobState::Paused ? QStringLiteral("play") : QStringLiteral("pause"));
        m_runPause->setToolTip(running->state == JobState::Paused ? QStringLiteral("Resume job") : QStringLiteral("Pause job"));
        m_running->show();
    } else {
        m_runningId.clear();
        m_running->hide();
    }
    m_empty->setVisible(total == 0);
    QString foot;
    if (held)
        foot = QStringLiteral("Queue paused · %1 waiting").arg(queued);
    else if (etaMs >= 0 && queued > 0)
        foot = QStringLiteral("%1 · %2 more queued").arg(formatTimeLeft(etaMs), QString::number(queued));
    else if (etaMs >= 0)
        foot = formatTimeLeft(etaMs);
    else if (queued > 0)
        foot = QStringLiteral("%1 queued").arg(queued);
    else if (total > 0)
        foot = QStringLiteral("Nothing running");
    else
        foot = QStringLiteral("Queue is empty");
    m_footer->setText(foot);
}

} // namespace fw::ui
