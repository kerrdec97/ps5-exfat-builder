#include "app/pages/QueuePage.h"

#include "app/AppContext.h"
#include "app/models/JobQueueModel.h"
#include "app/services/JobSubmit.h"
#include "app/ui/Icons.h"
#include "app/ui/Sheet.h"
#include "core/FileUtil.h"
#include "engines/general/Module.h"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonArray>
#include <QMenu>
#include <QPainter>

#include <algorithm>

namespace fw::pages {

using namespace ui;
using namespace theme;

namespace {

bool visibleJob(const JobRecord& r) { return !jobs::isSystemKind(r.kind); }

// "▸ JOB SETTINGS ............ 4" — folds a block of the details panel.
class FoldHeader : public QAbstractButton {
public:
    FoldHeader(const QString& text, QWidget* parent) : QAbstractButton(parent)
    {
        setText(text);
        setAccessibleName(text);
        setCheckable(true);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::StrongFocus);
        setAttribute(Qt::WA_Hover);
        setFixedHeight(32);
        setProperty("fwNoRing", true);
    }
    void setCount(const QString& count)
    {
        m_count = count;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QColor c = underMouse() ? color::fg_2 : color::fg_4;
        p.drawPixmap(QRect(0, (height() - 14) / 2, 14, 14),
                     iconPixmap(isChecked() ? QStringLiteral("chevron_down") : QStringLiteral("chevron_right"), 14, c, devicePixelRatioF()));
        p.setFont(font::overline());
        p.setPen(c);
        p.drawText(rect().adjusted(20, 0, -40, 0), Qt::AlignVCenter | Qt::AlignLeft, text());
        p.setFont(font::caption());
        p.setPen(color::fg_4);
        p.drawText(rect(), Qt::AlignVCenter | Qt::AlignRight, m_count);
        if (hasFocus()) {
            p.setPen(QPen(color::focus, 2));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(QRectF(rect()).adjusted(-3, 2, 3, -2), radius::sm, radius::sm);
        }
    }

private:
    QString m_count;
};

QWidget* detailRow(const QString& name, const QString& value, bool mono, bool overridden, QWidget* parent)
{
    auto* row = new QWidget(parent);
    auto* h = hbox(row, space::md, QMargins(0, 5, 0, 5));
    auto* left = new QWidget(row);
    auto* lh = hbox(left, space::sm);
    lh->addWidget(label(name, TextStyle::Body, color::fg_2, left));
    if (overridden) {
        auto* b = new Badge(QStringLiteral("Overridden"), Tone::Neutral, left);
        b->setIconName(QStringLiteral("advanced"));
        lh->addWidget(b);
    }
    lh->addStretch(1);
    h->addWidget(left, 1);
    auto* v = new ElideLabel(value, mono ? Qt::ElideMiddle : Qt::ElideRight, row);
    v->setFont(mono ? font::monoSmall() : font::body());
    setTextColor(v, color::fg_1);
    v->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    v->setMaximumWidth(220);
    v->setToolTip(value);
    h->addWidget(v, 0, Qt::AlignRight);
    return row;
}

} // namespace

QueuePage::QueuePage(AppContext& ctx, JobQueueModel& queueModel, LogModel& logModel, QWidget* parent)
    : Page(ctx, QStringLiteral("queue"), QStringLiteral("Queue"), QStringLiteral("Jobs run one at a time, in this order."), parent),
      m_model(queueModel), m_logModel(logModel)
{
    m_clearFinished = button(QStringLiteral("Clear finished"), ButtonVariant::Secondary, QStringLiteral("trash"), this);
    m_clearFinished->setToolTip(QStringLiteral("Remove done, failed and cancelled jobs from the list. History keeps them."));
    m_pauseQueue = button(QStringLiteral("Pause queue"), ButtonVariant::Secondary, QStringLiteral("pause"), this);
    auto* more = new IconButton(QStringLiteral("more"), QStringLiteral("More queue actions"), IconButton::Size::Regular, this);
    more->setToolTip(QStringLiteral("More queue actions"));
    // One right-aligned bar: buttons keep their size when one of them hides.
    auto* bar = new QWidget(this);
    auto* bl = hbox(bar, space::sm);
    bl->addStretch(1);
    for (QWidget* w : {static_cast<QWidget*>(m_clearFinished), static_cast<QWidget*>(m_pauseQueue), static_cast<QWidget*>(more)}) {
        w->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        bl->addWidget(w);
    }
    header()->addAction(bar);
    auto* menu = new QMenu(this);
    auto* exportCsv = menu->addAction(icon(QStringLiteral("download"), color::fg_2), QStringLiteral("Export job history…"));
    connect(more, &QToolButton::clicked, this, [more, menu] { menu->popup(more->mapToGlobal(QPoint(0, more->height() + 4))); });
    connect(exportCsv, &QAction::triggered, this, [this] {
        const QString path = QFileDialog::getSaveFileName(window(), QStringLiteral("Export job history"), QDir::homePath() + QStringLiteral("/job-history.csv"), QStringLiteral("CSV files (*.csv)"));
        if (path.isEmpty())
            return;
        auto st = m_ctx.history().exportCsv(path);
        if (st.ok())
            emit toastRequested(Tone::Ok, QStringLiteral("History exported"), nativePath(path));
        else
            emit toastRequested(Tone::Err, QStringLiteral("Export failed"), st.error().message);
    });
    connect(m_clearFinished, &QPushButton::clicked, this, [this] { m_ctx.queue().clearFinished(); });
    connect(m_pauseQueue, &QPushButton::clicked, this, [this] {
        m_ctx.queue().holdQueue(!m_ctx.queue().isHeld());
        updateCounts();
    });

    // ---- Nothing queued: one inviting state instead of two empty panels.
    m_emptyCard = new Card(this);
    m_emptyCard->setObjectName(QStringLiteral("queue-empty"));
    auto* hero = new EmptyState(QStringLiteral("queue"), QStringLiteral("Nothing in the queue"),
                                QStringLiteral("Jobs you start wait here and run one at a time while you keep working."), m_emptyCard);
    auto* b1 = hero->addAction(QStringLiteral("Build an image"), ButtonVariant::Primary);
    auto* b2 = hero->addAction(QStringLiteral("Backport a game"));
    connect(b1, &QPushButton::clicked, this, [this] { emit navigateRequested({QStringLiteral("build"), {}}); });
    connect(b2, &QPushButton::clicked, this, [this] { emit navigateRequested({QStringLiteral("backport"), {}}); });
    m_emptyCard->body()->addWidget(hero);
    content()->addWidget(m_emptyCard);

    // ---- Jobs | details
    m_columns = new QWidget(this);
    auto* layout = new TwoColumnLayout(m_columns);
    layout->setLeftMax(100000);
    auto* card = new Card(m_columns, Card::Variant::Table);
    auto* h = card->header();
    m_filter = new Segmented(card);
    for (const auto& s : {QStringLiteral("All"), QStringLiteral("Running"), QStringLiteral("Queued"), QStringLiteral("Done"), QStringLiteral("Failed")})
        m_filter->addSegment(s);
    m_filter->setAccessibleName(QStringLiteral("Filter jobs"));
    h->addWidget(m_filter);
    h->addStretch(1);
    m_eta = caption({}, card);
    h->addWidget(m_eta);
    connect(m_filter, &Segmented::currentChanged, this, &QueuePage::applyFilter);
    m_list = new JobList(ctx, queueModel, JobRow::Variant::Queue, card);
    m_list->setFilter(visibleJob);
    m_list->setToolTip(QStringLiteral("Drag a waiting job to change the order."));
    m_empty = new EmptyState(QStringLiteral("filter"), QStringLiteral("No jobs here"), QStringLiteral("Nothing matches this filter."), card);
    m_empty->setCompact(true);
    auto* showAll = m_empty->addAction(QStringLiteral("Show all jobs"));
    connect(showAll, &QPushButton::clicked, this, [this] { m_filter->setCurrentIndex(0); });
    m_list->setEmptyWidget(m_empty);
    card->body()->addWidget(m_list);
    connect(m_list, &JobList::selectionChanged, this, [this](const QString& id) {
        m_selected = id;
        updateDetail();
    });
    connect(m_list, &JobList::activated, this, [this](const QString& id) {
        if (id.startsWith(QLatin1String("cancel:")))
            cancelWithConfirm(id.mid(7));
        else
            selectJob(id);
    });
    connect(m_list, &JobList::countChanged, this, &QueuePage::updateCounts);

    // ---- details panel
    m_detail = new Card(m_columns);
    m_detail->setObjectName(QStringLiteral("queue-detail"));
    m_detail->body()->setSpacing(0);
    m_detailBody = new QWidget(m_detail);
    auto* d = vbox(m_detailBody, 0);
    auto* top = new QHBoxLayout;
    top->setSpacing(space::sm);
    auto* tcol = new QVBoxLayout;
    tcol->setSpacing(2);
    m_dTitle = new ElideLabel({}, Qt::ElideRight, m_detailBody);
    m_dTitle->setFont(font::subtitle());
    setTextColor(m_dTitle, color::fg_0);
    m_dMeta = new ElideLabel({}, Qt::ElideMiddle, m_detailBody);
    m_dMeta->setFont(font::caption());
    setTextColor(m_dMeta, color::fg_3);
    tcol->addWidget(m_dTitle);
    tcol->addWidget(m_dMeta);
    top->addLayout(tcol, 1);
    m_dBadge = new Badge({}, Tone::Neutral, m_detailBody);
    top->addWidget(m_dBadge, 0, Qt::AlignTop);
    d->addLayout(top);

    // The job's actions, right under its name.
    auto* actions = new QWidget(m_detailBody);
    auto* al = hbox(actions, space::sm, QMargins(0, space::md, 0, 0));
    m_dAction = button(QStringLiteral("Cancel job"), ButtonVariant::Secondary, QStringLiteral("close"), actions);
    setButtonSmall(m_dAction, true);
    m_dPause = button(QStringLiteral("Pause"), ButtonVariant::Secondary, QStringLiteral("pause"), actions);
    setButtonSmall(m_dPause, true);
    m_dFolder = button(QStringLiteral("Show in folder"), ButtonVariant::Secondary, QStringLiteral("folder_open"), actions);
    setButtonSmall(m_dFolder, true);
    m_dRemove = new IconButton(QStringLiteral("trash"), QStringLiteral("Remove from the queue"), IconButton::Size::Small, actions);
    m_dRemove->setToolTip(QStringLiteral("Remove from the queue"));
    al->addWidget(m_dAction);
    al->addWidget(m_dPause);
    al->addWidget(m_dFolder);
    al->addStretch(1);
    al->addWidget(m_dRemove);
    d->addWidget(actions);

    auto* progWrap = new QWidget(m_detailBody);
    auto* pl = vbox(progWrap, space::sm, QMargins(0, space::lg, 0, space::md));
    m_dProgress = new ProgressBlock(progWrap);
    pl->addWidget(m_dProgress);
    m_dStarted = caption({}, progWrap);
    pl->addWidget(m_dStarted);
    d->addWidget(progWrap);
    m_dResult = new QWidget(m_detailBody);
    m_dResultLayout = vbox(m_dResult, 4, QMargins(0, 0, 0, space::md));
    d->addWidget(m_dResult);
    d->addWidget(hairline(m_detailBody));

    auto* logHead = new QWidget(m_detailBody);
    auto* lh = hbox(logHead, space::sm, QMargins(0, space::md, 0, 6));
    lh->addWidget(label(QStringLiteral("Log"), TextStyle::Overline, color::fg_4, logHead));
    lh->addStretch(1);
    auto* openLog = new Link(QStringLiteral("Open full log"), logHead);
    lh->addWidget(openLog);
    d->addWidget(logHead);
    m_dLog = new ActivityWell(logModel, m_detailBody);
    m_dLog->setSmall(true);
    m_dLog->setMaxLines(6);
    d->addWidget(m_dLog);
    connect(openLog, &Link::activated, this, [this] {
        emit navigateRequested({QStringLiteral("logs"), {{QStringLiteral("jobId"), m_selected}}});
    });

    auto* fold = new FoldHeader(QStringLiteral("Job settings"), m_detailBody);
    m_dSettingsToggle = fold;
    d->addSpacing(space::sm);
    d->addWidget(fold);
    m_dSettings = new QWidget(m_detailBody);
    m_dSettingsLayout = vbox(m_dSettings, 0);
    m_dSettings->hide();
    d->addWidget(m_dSettings);
    connect(fold, &QAbstractButton::toggled, m_dSettings, &QWidget::setVisible);
    m_detail->body()->addWidget(m_detailBody);

    connect(m_dFolder, &QPushButton::clicked, this, [this] {
        if (auto rec = m_ctx.queue().record(m_selected); rec && rec->outcome && !rec->outcome->outputs.isEmpty())
            openInFileManager(rec->outcome->outputs.first());
    });
    connect(m_dPause, &QPushButton::clicked, this, [this] {
        auto rec = m_ctx.queue().record(m_selected);
        if (!rec)
            return;
        if (rec->state == JobState::Paused)
            m_ctx.queue().resumeJob(m_selected);
        else if (rec->state == JobState::Running)
            m_ctx.queue().pauseJob(m_selected);
    });
    connect(m_dRemove, &QToolButton::clicked, this, [this] {
        auto rec = m_ctx.queue().record(m_selected);
        if (rec && rec->state != JobState::Running && rec->state != JobState::Paused)
            m_ctx.queue().remove(m_selected);
    });
    connect(m_dAction, &QPushButton::clicked, this, [this] {
        auto rec = m_ctx.queue().record(m_selected);
        if (!rec)
            return;
        if (rec->state == JobState::Running || rec->state == JobState::Paused)
            cancelWithConfirm(m_selected);
        else if (rec->state == JobState::Queued)
            m_ctx.queue().remove(m_selected);
        else {
            auto r = m_ctx.queue().retry(m_selected);
            m_ctx.queue().holdQueue(false);
            if (r.ok())
                selectJob(r.value());
        }
    });

    layout->setLeft(card);
    layout->setRight(m_detail);
    content()->addWidget(m_columns);

    connect(&m_model, &QAbstractItemModel::dataChanged, this, [this](const QModelIndex& tl, const QModelIndex& br) {
        for (int r = tl.row(); r <= br.row(); ++r)
            if (m_model.idAt(r) == m_selected)
                updateDetail();
        updateCounts();
    });
    connect(&m_model, &JobQueueModel::countsChanged, this, &QueuePage::updateCounts);
    updateCounts();
    updateDetail();
}

void QueuePage::applyFilter(int index)
{
    switch (index) {
    case 1: m_list->setFilter([](const JobRecord& r) { return visibleJob(r) && (r.state == JobState::Running || r.state == JobState::Paused); }); break;
    case 2: m_list->setFilter([](const JobRecord& r) { return visibleJob(r) && r.state == JobState::Queued; }); break;
    case 3: m_list->setFilter([](const JobRecord& r) { return visibleJob(r) && r.state == JobState::Succeeded; }); break;
    case 4: m_list->setFilter([](const JobRecord& r) { return visibleJob(r) && (r.state == JobState::Failed || r.state == JobState::Interrupted || r.state == JobState::Cancelled); }); break;
    default: m_list->setFilter(visibleJob); break;
    }
}

void QueuePage::ensureSelection()
{
    if (!m_selected.isEmpty() && m_ctx.queue().record(m_selected))
        return;
    const QStringList ids = m_list->visibleIds();
    QString pick;
    for (const QString& id : ids)
        if (auto rec = m_ctx.queue().record(id); rec && (rec->state == JobState::Running || rec->state == JobState::Paused)) {
            pick = id;
            break;
        }
    if (pick.isEmpty() && !ids.isEmpty())
        pick = ids.first();
    // The filter hides every job: keep the details panel on any job of the queue.
    for (int r = 0; pick.isEmpty() && r < m_model.rowCount(); ++r)
        if (const JobRecord* rec = m_model.recordAt(r); rec && visibleJob(*rec))
            pick = rec->id;
    if (pick != m_selected) {
        m_selected = pick;
        if (!pick.isEmpty())
            m_list->setSelectedId(pick);
        updateDetail();
    }
}

void QueuePage::updateCounts()
{
    int all = 0, running = 0, queued = 0, done = 0, failed = 0;
    qint64 eta = -1;
    for (int r = 0; r < m_model.rowCount(); ++r) {
        const JobRecord* rec = m_model.recordAt(r);
        if (!rec || !visibleJob(*rec))
            continue;
        ++all;
        switch (rec->state) {
        case JobState::Running:
        case JobState::Paused: ++running; if (rec->progress.etaMs >= 0) eta = std::max<qint64>(eta, 0) + rec->progress.etaMs; break;
        case JobState::Queued: ++queued; break;
        case JobState::Succeeded: ++done; break;
        default: ++failed; break;
        }
    }
    auto seg = [](const QString& name, int n) { return n > 0 ? QStringLiteral("%1 %2").arg(name).arg(n) : name; };
    m_filter->setSegmentText(0, seg(QStringLiteral("All"), all));
    m_filter->setSegmentText(1, seg(QStringLiteral("Running"), running));
    m_filter->setSegmentText(2, seg(QStringLiteral("Queued"), queued));
    m_filter->setSegmentText(3, seg(QStringLiteral("Done"), done));
    m_filter->setSegmentText(4, seg(QStringLiteral("Failed"), failed));
    const bool held = m_ctx.queue().isHeld();
    m_pauseQueue->setText(held ? QStringLiteral("Resume queue") : QStringLiteral("Pause queue"));
    setButtonIcon(m_pauseQueue, held ? QStringLiteral("play") : QStringLiteral("pause"));
    setButtonVariant(m_pauseQueue, held ? ButtonVariant::Primary : ButtonVariant::Secondary);
    m_pauseQueue->setToolTip(held ? QStringLiteral("Start the next waiting job when the current one ends.")
                                  : QStringLiteral("Finish the running job, then wait. Nothing is cancelled."));
    m_clearFinished->setEnabled(done + failed > 0);
    m_clearFinished->setVisible(all > 0);
    if (held)
        m_eta->setText(QStringLiteral("Queue paused · running jobs continue"));
    else if (eta >= 0)
        m_eta->setText(QStringLiteral("Running job: %1").arg(formatTimeLeft(eta).toLower()));
    else if (queued > 0 && running == 0)
        m_eta->setText(QStringLiteral("Waiting to start"));
    else
        m_eta->setText({});
    m_columns->setVisible(all > 0);
    m_emptyCard->setVisible(all == 0);
    ensureSelection();
    emit statusContextChanged();
}

void QueuePage::updateDetail()
{
    auto rec = m_ctx.queue().record(m_selected);
    const bool has = rec.has_value();
    m_detail->setVisible(has);
    if (!has)
        return;
    const JobRecord& r = *rec;
    m_dTitle->setFullText(r.title);
    QString meta = m_ctx.jobFactory().displayName(r.kind);
    if (!r.subtitle.isEmpty())
        meta += QStringLiteral(" · ") + r.subtitle;
    m_dMeta->setFullText(meta);
    m_dBadge->set(jobStateName(r.state), toneForState(r.state));
    m_dBadge->setIconName(r.state == JobState::Queued || r.state == JobState::Paused || r.state == JobState::Cancelled ? QString() : defaultBadgeIcon(toneForState(r.state)));
    const auto& p = r.progress;
    QString detail = progressDetailText(p);
    if (r.started.isValid())
        detail += (detail.isEmpty() ? QString() : QStringLiteral(" · ")) + QStringLiteral("Started %1").arg(r.started.toString(QStringLiteral("HH:mm")));
    m_dProgress->setProgress(p.overallFraction, p.indeterminate && r.state == JobState::Running, r.state == JobState::Running || r.state == JobState::Paused ? detail : QString());
    switch (r.state) {
    case JobState::Succeeded: m_dProgress->setState(ProgressBar::State::Done); break;
    case JobState::Failed:
    case JobState::Interrupted:
    case JobState::Cancelled: m_dProgress->setState(ProgressBar::State::Failed); break;
    case JobState::Paused: m_dProgress->setState(ProgressBar::State::Paused); break;
    default: m_dProgress->setState(p.indeterminate ? ProgressBar::State::Indeterminate : ProgressBar::State::Determinate); break;
    }
    const int phaseIndex = r.state == JobState::Succeeded ? r.phases.size() : p.phaseIndex;
    m_dProgress->setPhases(r.phases, phaseIndex);
    // A job that never ran has no progress to show; its caption says why.
    m_dProgress->setVisible(r.started.isValid() || r.state == JobState::Running || r.state == JobState::Paused);
    QStringList started;
    if (r.state == JobState::Queued) {
        const int maxAttempts = m_ctx.settings().value(general::keys::kAutoRetryCount).toInt() + 1;
        const QString w = waitingText(r, maxAttempts);
        started << (w.isEmpty() ? QStringLiteral("Added %1").arg(r.added.toString(QStringLiteral("HH:mm"))) : w);
    }
    if (r.attempt > 1)
        started << QStringLiteral("Attempt %1").arg(r.attempt);
    if (r.finished.isValid())
        started << QStringLiteral("Finished %1").arg(r.finished.toString(QStringLiteral("HH:mm")));
    if (!r.started.isValid() && r.state != JobState::Queued)
        started << QStringLiteral("Not started");
    m_dStarted->setText(started.join(QStringLiteral(" · ")));
    m_dStarted->setVisible(!started.isEmpty());

    // Settings rows: path-like parameters first, then overrides.
    while (QLayoutItem* it = m_dSettingsLayout->takeAt(0)) {
        delete it->widget();
        delete it;
    }
    int overrides = 0;
    for (auto it = r.parameters.constBegin(); it != r.parameters.constEnd(); ++it) {
        if (!it.value().isString())
            continue;
        const QString v = it.value().toString();
        if (v.isEmpty() || v.size() > 260)
            continue;
        QString name = it.key();
        name[0] = name[0].toUpper();
        const bool pathLike = v.contains(u'/') || v.contains(u'\\');
        m_dSettingsLayout->addWidget(detailRow(name, pathLike ? nativePath(v) : v, pathLike || name.contains(QLatin1String("Id")) || name == QLatin1String("Version"), false, m_dSettings));
    }
    for (auto it = r.overrides.cbegin(); it != r.overrides.cend(); ++it) {
        const SettingDef* def = m_ctx.schema().find(it.key());
        m_dSettingsLayout->addWidget(detailRow(def ? def->label : it.key(), def ? SettingRow::displayValue(*def, it.value()) : it.value().toString(), true, true, m_dSettings));
        ++overrides;
    }
    if (m_dSettingsLayout->count() == 0)
        m_dSettingsLayout->addWidget(caption(QStringLiteral("Default settings"), m_dSettings));
    static_cast<FoldHeader*>(m_dSettingsToggle)->setCount(overrides > 0 ? QStringLiteral("%1 overridden").arg(overrides) : QString());

    // Result block
    while (QLayoutItem* it = m_dResultLayout->takeAt(0)) {
        delete it->widget();
        delete it;
    }
    if (r.outcome) {
        if (r.outcome->verification != Verification::NotRun) {
            auto* row = new MessageRow(m_dResult);
            const Verification v = r.outcome->verification;
            row->set(QStringLiteral("%1%2").arg(verificationName(v), r.outcome->verificationDetail.isEmpty() ? QString() : QStringLiteral(" · %1").arg(r.outcome->verificationDetail)),
                     v == Verification::Passed ? Tone::Ok : v == Verification::Failed ? Tone::Err : Tone::Warn);
            m_dResultLayout->addWidget(row);
        }
        for (const QString& w : r.outcome->warnings) {
            auto* row = new MessageRow(m_dResult);
            row->set(w, Tone::Warn);
            m_dResultLayout->addWidget(row);
        }
        for (const QString& o : r.outcome->outputs)
            m_dResultLayout->addWidget(detailRow(QStringLiteral("Output"), nativePath(o), true, false, m_dResult));
    }
    if (r.error && !r.error->isCancelled()) {
        auto* bar = new InfoBar(Tone::Err, m_dResult);
        bar->setTitle(QStringLiteral("Failed"));
        bar->setMessage(r.error->message);
        auto* l = bar->addLink(QStringLiteral("Show in log"));
        connect(l, &Link::activated, this, [this] {
            emit navigateRequested({QStringLiteral("logs"), {{QStringLiteral("jobId"), m_selected}, {QStringLiteral("level"), QStringLiteral("error")}}});
        });
        m_dResultLayout->addWidget(bar);
    }
    m_dResult->setVisible(m_dResultLayout->count() > 0);
    m_dLog->setJobId(r.id);

    // Actions for this state.
    const bool active = r.state == JobState::Running || r.state == JobState::Paused;
    m_dFolder->setVisible(r.outcome && !r.outcome->outputs.isEmpty());
    m_dPause->setVisible(active);
    m_dPause->setText(r.state == JobState::Paused ? QStringLiteral("Resume") : QStringLiteral("Pause"));
    setButtonIcon(m_dPause, r.state == JobState::Paused ? QStringLiteral("play") : QStringLiteral("pause"));
    m_dRemove->setVisible(!active && r.state != JobState::Queued);
    if (active) {
        m_dAction->setText(QStringLiteral("Cancel job"));
        setButtonIcon(m_dAction, QStringLiteral("close"));
        setButtonVariant(m_dAction, ButtonVariant::Secondary);
    } else if (r.state == JobState::Queued) {
        m_dAction->setText(QStringLiteral("Remove"));
        setButtonIcon(m_dAction, QStringLiteral("trash"));
        setButtonVariant(m_dAction, ButtonVariant::Secondary);
    } else {
        m_dAction->setText(r.state == JobState::Succeeded ? QStringLiteral("Run again") : QStringLiteral("Retry"));
        setButtonIcon(m_dAction, QStringLiteral("undo"));
        setButtonVariant(m_dAction, r.state == JobState::Succeeded ? ButtonVariant::Secondary : ButtonVariant::Primary);
    }
    setButtonSmall(m_dAction, true);
}

void QueuePage::selectJob(const QString& id)
{
    m_list->setSelectedId(id);
    m_selected = id;
    updateDetail();
}

void QueuePage::navigateWith(const QVariantMap& args)
{
    if (args.contains(QStringLiteral("jobId"))) {
        const QString id = args.value(QStringLiteral("jobId")).toString();
        if (id.startsWith(QLatin1String("cancel:")))
            cancelWithConfirm(id.mid(7));
        else {
            m_filter->setCurrentIndex(0);
            selectJob(id);
        }
    }
}

void QueuePage::cancelWithConfirm(const QString& id)
{
    auto rec = m_ctx.queue().record(id);
    if (!rec || (rec->state != JobState::Running && rec->state != JobState::Paused))
        return;
    const QString pct = rec->progress.indeterminate ? QString() : QStringLiteral(" is %1 done").arg(thinPercent(rec->progress.overallFraction));
    bool remove = false;
    const bool ok = Sheet::confirm(window(), QStringLiteral("Cancel the running job?"),
                                   QStringLiteral("%1%2. Cancelling removes its partial output and leaves the source untouched.").arg(rec->title.toHtmlEscaped(), pct),
                                   QStringLiteral("Keep running"), QStringLiteral("Cancel job"), true, Sheet::Icon::Warning,
                                   QStringLiteral("Also remove the job from the queue"), &remove);
    if (!ok)
        return;
    m_ctx.queue().cancel(id);
    if (remove) {
        // The job leaves the queue once the worker reports Cancelled.
        auto* conn = new QMetaObject::Connection;
        *conn = connect(&m_ctx.queue(), &JobQueue::jobFinished, this, [this, id, conn](const QString& fin) {
            if (fin != id)
                return;
            m_ctx.queue().remove(id);
            QObject::disconnect(*conn);
            delete conn;
        });
    }
}

QString QueuePage::statusContext() const
{
    // The jobs this page lists (update checks and other system jobs stay out).
    int running = 0, queued = 0, done = 0, failed = 0;
    for (int r = 0; r < m_model.rowCount(); ++r) {
        const JobRecord* rec = m_model.recordAt(r);
        if (!rec || !visibleJob(*rec))
            continue;
        switch (rec->state) {
        case JobState::Running:
        case JobState::Paused: ++running; break;
        case JobState::Queued: ++queued; break;
        case JobState::Succeeded: ++done; break;
        default: ++failed; break;
        }
    }
    return QStringLiteral("Queue: %1 running · %2 queued · %3 done · %4 failed").arg(running).arg(queued).arg(done).arg(failed);
}

void QueuePage::applyDemoState(const QString& name)
{
    if (name != QLatin1String("cancel-dialog"))
        return;
    auto* s = new Sheet(window(), QStringLiteral("Cancel the running build?"), Sheet::Icon::Warning);
    s->addText(QStringLiteral("Ghost of Yotei is 63 % copied. Cancelling deletes the partial image %1 and leaves the dump untouched.")
                   .arg(Sheet::monoSpan(QStringLiteral("PPSA01325_Ghost_of_Yotei_v01.000.000.exfat"))));
    s->addCheckbox(QStringLiteral("Also remove the job from the queue"));
    s->addButton(QStringLiteral("Keep building"), Sheet::Role::Safe);
    s->addButton(QStringLiteral("Cancel build"), Sheet::Role::Destructive);
    s->open();
    m_demoSheet = s;
}

void QueuePage::clearDemoState()
{
    if (auto* s = qobject_cast<Sheet*>(m_demoSheet.data()))
        s->close(0);
}

} // namespace fw::pages
