#include "app/pages/QueuePage.h"

#include "app/AppContext.h"
#include "app/models/JobQueueModel.h"
#include "app/services/JobSubmit.h"
#include "app/ui/Sheet.h"
#include "core/FileUtil.h"
#include "engines/general/Module.h"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonArray>

namespace fw::pages {

using namespace ui;
using namespace theme;

static bool visibleJob(const JobRecord& r) { return !jobs::isSystemKind(r.kind); }

QueuePage::QueuePage(AppContext& ctx, JobQueueModel& queueModel, LogModel& logModel, QWidget* parent)
    : Page(ctx, QStringLiteral("queue"), QStringLiteral("Queue"),
           QStringLiteral("Jobs run one at a time in this order. Drag to reorder; finished jobs stay until you clear them."), parent),
      m_model(queueModel), m_logModel(logModel)
{
    auto* exportCsv = button(QStringLiteral("Export history…"), ButtonVariant::Secondary, QStringLiteral("download"), this);
    m_clearFinished = button(QStringLiteral("Clear finished"), ButtonVariant::Secondary, QStringLiteral("trash"), this);
    m_pauseQueue = button(QStringLiteral("Pause queue"), ButtonVariant::Secondary, QStringLiteral("pause"), this);
    header()->addAction(exportCsv);
    header()->addAction(m_clearFinished);
    header()->addAction(m_pauseQueue);
    connect(exportCsv, &QPushButton::clicked, this, [this] {
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

    auto* columns = new QWidget(this);
    auto* layout = new TwoColumnLayout(columns);
    layout->setLeftMax(100000);
    auto* card = new Card(columns, Card::Variant::Table);
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
    m_empty = new EmptyState(QStringLiteral("queue"), QStringLiteral("No jobs in the queue"),
                             QStringLiteral("Add a dump on Build image, FPKG or Backport. Jobs run one after another and keep going while you work."), card);
    auto* b1 = m_empty->addAction(QStringLiteral("Build image"));
    auto* b2 = m_empty->addAction(QStringLiteral("Build package"));
    connect(b1, &QPushButton::clicked, this, [this] { emit navigateRequested({QStringLiteral("build"), {}}); });
    connect(b2, &QPushButton::clicked, this, [this] { emit navigateRequested({QStringLiteral("fpkg"), {}}); });
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

    // ---- detail panel
    m_detail = new Card(columns);
    m_detail->body()->setSpacing(0);
    m_detailEmpty = new EmptyState(QStringLiteral("list"), QStringLiteral("No job selected"), QStringLiteral("Select a job to see its progress, settings and log."), m_detail);
    m_detailEmpty->setCompact(true);
    m_detail->body()->addWidget(m_detailEmpty);
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
    m_dMeta->setFont(font::mono());
    setTextColor(m_dMeta, color::fg_3);
    tcol->addWidget(m_dTitle);
    tcol->addWidget(m_dMeta);
    top->addLayout(tcol, 1);
    m_dBadge = new Badge({}, Tone::Neutral, m_detailBody);
    top->addWidget(m_dBadge, 0, Qt::AlignTop);
    d->addLayout(top);
    d->addSpacing(space::sm);
    d->addWidget(hairline(m_detailBody));
    auto* progWrap = new QWidget(m_detailBody);
    auto* pl = vbox(progWrap, space::sm, QMargins(0, space::md, 0, space::md));
    m_dProgress = new ProgressBlock(progWrap);
    pl->addWidget(m_dProgress);
    m_dStarted = caption({}, progWrap);
    pl->addWidget(m_dStarted);
    d->addWidget(progWrap);
    d->addWidget(hairline(m_detailBody));
    auto* ov1 = label(QStringLiteral("Job settings"), TextStyle::Overline, color::fg_4, m_detailBody);
    ov1->setContentsMargins(0, space::md, 0, 4);
    d->addWidget(ov1);
    m_dSettings = new QWidget(m_detailBody);
    m_dSettingsLayout = vbox(m_dSettings, 0);
    d->addWidget(m_dSettings);
    m_dResult = new QWidget(m_detailBody);
    m_dResultLayout = vbox(m_dResult, 4, QMargins(0, space::sm, 0, 0));
    d->addWidget(m_dResult);
    auto* ov2 = label(QStringLiteral("Log"), TextStyle::Overline, color::fg_4, m_detailBody);
    ov2->setContentsMargins(0, space::md, 0, 4);
    d->addWidget(ov2);
    m_dLog = new ActivityWell(logModel, m_detailBody);
    m_dLog->setSmall(true);
    m_dLog->setMaxLines(6);
    d->addWidget(m_dLog);
    auto* actions = new QWidget(m_detailBody);
    auto* al = hbox(actions, space::sm, QMargins(0, space::md, 0, 0));
    al->addStretch(1);
    m_dOpenLog = button(QStringLiteral("Open full log"), ButtonVariant::Subtle, {}, actions);
    setButtonSmall(m_dOpenLog, true);
    m_dFolder = button(QStringLiteral("Show in folder"), ButtonVariant::Secondary, QStringLiteral("folder_open"), actions);
    setButtonSmall(m_dFolder, true);
    m_dAction = button(QStringLiteral("Cancel job"), ButtonVariant::Secondary, QStringLiteral("close"), actions);
    setButtonSmall(m_dAction, true);
    al->addWidget(m_dOpenLog);
    al->addWidget(m_dFolder);
    al->addWidget(m_dAction);
    d->addWidget(actions);
    m_detail->body()->addWidget(m_detailBody);
    m_detailBody->hide();
    connect(m_dOpenLog, &QPushButton::clicked, this, [this] {
        emit navigateRequested({QStringLiteral("logs"), {{QStringLiteral("jobId"), m_selected}}});
    });
    connect(m_dFolder, &QPushButton::clicked, this, [this] {
        if (auto rec = m_ctx.queue().record(m_selected); rec && rec->outcome && !rec->outcome->outputs.isEmpty())
            openInFileManager(rec->outcome->outputs.first());
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
            (void)m_ctx.queue().retry(m_selected);
            m_ctx.queue().holdQueue(false);
        }
    });

    layout->setLeft(card);
    layout->setRight(m_detail);
    content()->addWidget(columns);

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
    m_filter->setSegmentText(0, QStringLiteral("All %1").arg(all));
    m_filter->setSegmentText(1, QStringLiteral("Running %1").arg(running));
    m_filter->setSegmentText(2, QStringLiteral("Queued %1").arg(queued));
    m_filter->setSegmentText(3, QStringLiteral("Done %1").arg(done));
    m_filter->setSegmentText(4, QStringLiteral("Failed %1").arg(failed));
    const bool held = m_ctx.queue().isHeld();
    m_pauseQueue->setText(held ? QStringLiteral("Resume queue") : QStringLiteral("Pause queue"));
    setButtonIcon(m_pauseQueue, held ? QStringLiteral("play") : QStringLiteral("pause"));
    m_clearFinished->setEnabled(done + failed > 0);
    if (held)
        m_eta->setText(QStringLiteral("Queue paused · running jobs continue"));
    else if (eta >= 0)
        m_eta->setText(QStringLiteral("Running job: %1").arg(formatTimeLeft(eta).toLower()));
    else if (queued > 0 && running == 0)
        m_eta->setText(QStringLiteral("Waiting to start"));
    else
        m_eta->setText({});
    emit statusContextChanged();
}

static QWidget* detailRow(const QString& name, const QString& value, bool mono, bool overridden, QWidget* parent)
{
    auto* row = new QWidget(parent);
    auto* h = hbox(row, space::md, QMargins(0, 6, 0, 6));
    auto* left = new QWidget(row);
    auto* lh = hbox(left, space::sm);
    lh->addWidget(label(name, TextStyle::Body, color::fg_1, left));
    if (overridden) {
        auto* b = new Badge(QStringLiteral("Overridden"), Tone::Neutral, left);
        b->setIconName(QStringLiteral("advanced"));
        lh->addWidget(b);
    }
    lh->addStretch(1);
    h->addWidget(left, 1);
    auto* v = new ElideLabel(value, mono ? Qt::ElideMiddle : Qt::ElideRight, row);
    v->setFont(mono ? font::mono() : font::body());
    setTextColor(v, color::fg_2);
    v->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    v->setMaximumWidth(220);
    h->addWidget(v, 0, Qt::AlignRight);
    return row;
}

void QueuePage::updateDetail()
{
    auto rec = m_ctx.queue().record(m_selected);
    const bool has = rec.has_value();
    m_detailEmpty->setVisible(!has);
    m_detailBody->setVisible(has);
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
        const SettingDef* d = m_ctx.schema().find(it.key());
        m_dSettingsLayout->addWidget(detailRow(d ? d->label : it.key(), d ? SettingRow::displayValue(*d, it.value()) : it.value().toString(), true, true, m_dSettings));
    }
    if (m_dSettingsLayout->count() == 0)
        m_dSettingsLayout->addWidget(caption(QStringLiteral("Default settings"), m_dSettings));

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

    m_dFolder->setVisible(r.outcome && !r.outcome->outputs.isEmpty());
    if (r.state == JobState::Running || r.state == JobState::Paused) {
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
    const int running = m_model.countInState(JobState::Running), queued = m_model.countInState(JobState::Queued);
    const int done = m_model.countInState(JobState::Succeeded), failed = m_model.countInState(JobState::Failed);
    return QStringLiteral("Queue: %1 running · %2 queued · %3 done · %4 failed").arg(running).arg(queued).arg(done).arg(failed);
}

void QueuePage::applyDemoState(const QString& name)
{
    if (name != QLatin1String("cancel-dialog"))
        return;
    auto* s = new Sheet(window(), QStringLiteral("Cancel the running build?"), Sheet::Icon::Warning);
    s->addText(QStringLiteral("Ghost of Yotei is 63 % copied. Cancelling deletes the partial image %1 and leaves the dump untouched.")
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
