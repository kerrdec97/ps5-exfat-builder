#include "app/pages/QueuePage.h"

#include "app/AppContext.h"
#include "app/models/JobQueueModel.h"
#include "app/services/JobSubmit.h"
#include "app/ui/Icons.h"
#include "app/ui/Sheet.h"
#include "core/FileUtil.h"
#include "core/Tr.h"
#include "engines/general/Module.h"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QMenu>
#include <QPainter>

#include <algorithm>

namespace fw::pages {

using namespace ui;
using namespace theme;

namespace {

bool visibleJob(const JobRecord& r) { return !jobs::isSystemKind(r.kind); }

// "▸ JOB SETTINGS ............ 4" — folds a block of the details column.
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
        const QString closed = isRightToLeft() ? QStringLiteral("chevron_left") : QStringLiteral("chevron_right");
        p.drawPixmap(mirrored(this, QRect(0, (height() - 14) / 2, 14, 14)),
                     iconPixmap(isChecked() ? QStringLiteral("chevron_down") : closed, 14, c, devicePixelRatioF()));
        p.setFont(font::overline());
        p.setPen(c);
        p.drawText(mirrored(this, rect().adjusted(20, 0, -40, 0)), Qt::AlignVCenter | Qt::AlignLeft, text());
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

// The label of a job parameter in the details ("contentId" → "Content ID");
// unknown names are shown as words ("remoteDir" → "Remote dir").
QString parameterLabel(const QString& key)
{
    static const QHash<QString, QString> known = {
        {QStringLiteral("source"), FW_TR_NOOP("Source")},
        {QStringLiteral("output"), FW_TR_NOOP("Output")},
        {QStringLiteral("outputDir"), FW_TR_NOOP("Output folder")},
        {QStringLiteral("destination"), FW_TR_NOOP("Destination")},
        {QStringLiteral("image"), FW_TR_NOOP("Image")},
        {QStringLiteral("package"), FW_TR_NOOP("Package")},
        {QStringLiteral("basePackage"), FW_TR_NOOP("Base package")},
        {QStringLiteral("game"), FW_TR_NOOP("Game folder")},
        {QStringLiteral("fileName"), FW_TR_NOOP("File name")},
        {QStringLiteral("format"), FW_TR_NOOP("Format")},
        {QStringLiteral("title"), FW_TR_NOOP("Title")},
        {QStringLiteral("titleId"), FW_TR_NOOP("Title ID")},
        {QStringLiteral("contentId"), FW_TR_NOOP("Content ID")},
        {QStringLiteral("contentVersion"), FW_TR_NOOP("Version")},
        {QStringLiteral("version"), FW_TR_NOOP("Version")},
        {QStringLiteral("packageType"), FW_TR_NOOP("Package type")},
        {QStringLiteral("backport4xx"), FW_TR_NOOP("Backport 4.xx")},
        {QStringLiteral("backport5xx"), FW_TR_NOOP("Backport 5.xx+")},
        {QStringLiteral("entitlementKey"), FW_TR_NOOP("Entitlement key")},
        {QStringLiteral("volumeType"), FW_TR_NOOP("Volume type")},
        {QStringLiteral("remoteDir"), FW_TR_NOOP("Folder on the console")},
        {QStringLiteral("host"), FW_TR_NOOP("Console")},
        {QStringLiteral("a"), QStringLiteral("A")},
        {QStringLiteral("b"), QStringLiteral("B")},
        {QStringLiteral("depth"), FW_TR_NOOP("Depth")},
        {QStringLiteral("method"), FW_TR_NOOP("Method")},
    };
    if (const auto it = known.constFind(key); it != known.constEnd())
        return i18n::translate(it.value());
    QString out;
    for (const QChar c : key) {
        if (c.isUpper() && !out.isEmpty() && !out.back().isSpace())
            out += u' ';
        out += out.isEmpty() ? c.toUpper() : c.toLower();
    }
    return out;
}

QWidget* detailRow(const QString& name, const QString& value, bool mono, bool overridden, QWidget* parent)
{
    auto* row = new QWidget(parent);
    auto* h = hbox(row, space::md, QMargins(0, 5, 0, 5));
    auto* left = new QWidget(row);
    auto* lh = hbox(left, space::sm);
    // Names and values come from the stored job (paths, IDs): shown literally.
    lh->addWidget(label(name, TextStyle::Body, color::fg_2, left));
    if (overridden) {
        auto* b = new Badge(FW_TR("Overridden"), Tone::Neutral, left);
        b->setIconName(QStringLiteral("advanced"));
        lh->addWidget(b);
    }
    lh->addStretch(1);
    h->addWidget(left, 1);
    auto* v = new ElideLabel(value, mono ? Qt::ElideMiddle : Qt::ElideRight, row);
    v->setFont(mono ? font::monoSmall() : font::body());
    if (mono)
        keepLeftToRight(v);
    setTextColor(v, color::fg_1);
    v->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    v->setMaximumWidth(240);
    v->setToolTip(value);
    h->addWidget(v, 0, Qt::AlignRight);
    return row;
}

} // namespace

QueuePage::QueuePage(AppContext& ctx, JobQueueModel& queueModel, LogModel& logModel, QWidget* parent)
    : Page(ctx, QStringLiteral("queue"), FW_TR("Queue"), FW_TR("Jobs run in this order."), parent),
      m_model(queueModel), m_logModel(logModel)
{
    m_clearFinished = button(FW_TR("Clear finished"), ButtonVariant::Secondary, QStringLiteral("trash"), this);
    m_clearFinished->setToolTip(FW_TR("Remove done, failed and cancelled jobs from the list. History keeps them."));
    m_pauseQueue = button(FW_TR("Pause queue"), ButtonVariant::Secondary, QStringLiteral("pause"), this);
    auto* more = new IconButton(QStringLiteral("more"), FW_TR("More queue actions"), IconButton::Size::Regular, this);
    more->setToolTip(FW_TR("More queue actions"));
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
    auto* exportCsv = menu->addAction(icon(QStringLiteral("download"), color::fg_2), FW_TR("Export job history…"));
    connect(more, &QToolButton::clicked, this, [more, menu] { menu->popup(more->mapToGlobal(QPoint(0, more->height() + 4))); });
    connect(exportCsv, &QAction::triggered, this, [this] {
        const QString path = QFileDialog::getSaveFileName(window(), FW_TR("Export job history"), QDir::homePath() + QStringLiteral("/job-history.csv"), FW_TR("CSV files") + QStringLiteral(" (*.csv)"));
        if (path.isEmpty())
            return;
        auto st = m_ctx.history().exportCsv(path);
        if (st.ok())
            emit toastRequested(Tone::Ok, FW_TR("History exported"), nativePath(path));
        else
            emit toastRequested(Tone::Err, FW_TR("Export failed"), st.error().message);
    });
    connect(m_clearFinished, &QPushButton::clicked, this, [this] { m_ctx.queue().clearFinished(); });
    connect(m_pauseQueue, &QPushButton::clicked, this, [this] { m_ctx.queue().holdQueue(!m_ctx.queue().isHeld()); });

    // ---- Nothing queued: one inviting state instead of two empty panels.
    m_emptyCard = new Card(this);
    m_emptyCard->setObjectName(QStringLiteral("queue-empty"));
    auto* hero = new EmptyState(QStringLiteral("queue"), FW_TR("Nothing in the queue"),
                                FW_TR("Jobs you start wait here and run in order while you keep working."), m_emptyCard);
    auto* b1 = hero->addAction(FW_TR("Build an image"), ButtonVariant::Primary);
    auto* b2 = hero->addAction(FW_TR("Backport a game"));
    connect(b1, &QPushButton::clicked, this, [this] { emit navigateRequested({QStringLiteral("build"), {}}); });
    connect(b2, &QPushButton::clicked, this, [this] { emit navigateRequested({QStringLiteral("backport"), {}}); });
    m_emptyCard->body()->addWidget(hero);
    content()->addWidget(m_emptyCard);

    // ---- Jobs | details
    m_columns = new QWidget(this);
    auto* layout = new TwoColumnLayout(m_columns, 460);
    layout->setLeftMax(100000);
    auto* card = new Card(m_columns, Card::Variant::Table);
    auto* h = card->header();
    m_filter = new Segmented(card);
    for (const auto& s : {FW_TR("All"), FW_TR("Running"), FW_TR("Queued"), FW_TR("Done"), FW_TR("Not finished")})
        m_filter->addSegment(s);
    m_filter->setAccessibleName(QStringLiteral("Filter jobs"));
    h->addWidget(m_filter);
    h->addStretch(1);
    m_eta = caption({}, card);
    h->addWidget(m_eta);
    connect(m_filter, &Segmented::currentChanged, this, &QueuePage::applyFilter);
    m_list = new JobList(ctx, queueModel, JobRow::Variant::Queue, card);
    m_list->setFilter(visibleJob);
    m_list->setToolTip(FW_TR("Drag a waiting job to change the order."));
    m_empty = new EmptyState(QStringLiteral("filter"), FW_TR("No jobs here"), FW_TR("Nothing matches this filter."), card);
    m_empty->setCompact(true);
    auto* showAll = m_empty->addAction(FW_TR("Show all jobs"));
    connect(showAll, &QPushButton::clicked, this, [this] { m_filter->setCurrentIndex(0); });
    m_list->setEmptyWidget(m_empty);
    card->body()->addWidget(m_list);
    connect(m_list, &JobList::selectionChanged, this, [this](const QString& id) {
        m_selected = id;
        updateDetail();
    });
    connect(m_list, &JobList::activated, this, &QueuePage::selectJob);
    connect(m_list, &JobList::cancelRequested, this, &QueuePage::cancelWithConfirm);
    connect(m_list, &JobList::countChanged, this, &QueuePage::updateCounts);

    // ---- details column: the monitor and the job's settings
    m_detail = new QWidget(m_columns);
    m_detail->setObjectName(QStringLiteral("queue-detail"));
    auto* d = vbox(m_detail, space::lg);
    m_monitor = new JobMonitor(ctx, logModel, JobMonitor::Variant::Compact, m_detail);
    m_monitor->setFeatures(JobMonitor::ShowRemove);
    d->addWidget(m_monitor);
    connect(m_monitor, &JobMonitor::cancelRequested, this, &QueuePage::cancelWithConfirm);
    connect(m_monitor, &JobMonitor::openLogsRequested, this, [this](const QString& id) {
        emit navigateRequested({QStringLiteral("logs"), {{QStringLiteral("jobId"), id}}});
    });
    connect(m_monitor, &JobMonitor::jobSwitched, this, &QueuePage::selectJob);

    m_settingsCard = new Card(m_detail);
    m_settingsCard->body()->setSpacing(0);
    auto* fold = new FoldHeader(FW_TR("Job settings"), m_settingsCard);
    m_dSettingsToggle = fold;
    m_settingsCard->body()->addWidget(fold);
    m_dSettings = new QWidget(m_settingsCard);
    m_dSettingsLayout = vbox(m_dSettings, 0);
    m_dSettings->hide();
    m_settingsCard->body()->addWidget(m_dSettings);
    connect(fold, &QAbstractButton::toggled, m_dSettings, &QWidget::setVisible);
    d->addWidget(m_settingsCard);
    d->addStretch(1);

    layout->setLeft(card);
    layout->setRight(m_detail);
    content()->addWidget(m_columns);

    // Progress ticks (ten a second while a job runs) touch only the time-left
    // caption; the counts, filters and buttons follow state changes.
    connect(&m_model, &QAbstractItemModel::dataChanged, this, [this](const QModelIndex&, const QModelIndex&) { updateEta(); });
    connect(&m_model, &JobQueueModel::countsChanged, this, &QueuePage::updateCounts);
    connect(&m_ctx.queue(), &JobQueue::heldChanged, this, &QueuePage::updateCounts);
    updateCounts();
    updateDetail();
}

void QueuePage::updateEta()
{
    int running = 0, queued = 0;
    qint64 eta = -1;
    for (int r = 0; r < m_model.rowCount(); ++r) {
        const JobRecord* rec = m_model.recordAt(r);
        if (!rec || !visibleJob(*rec))
            continue;
        if (rec->isActive()) {
            ++running;
            // Jobs that run at the same time end together: the longest one, not the sum.
            if (rec->progress.etaMs >= 0)
                eta = std::max<qint64>(eta, rec->progress.etaMs);
        } else if (rec->state == JobState::Queued) {
            ++queued;
        }
    }
    if (m_ctx.queue().isHeld())
        m_eta->setText(running == 0 && queued > 0 ? FW_TR("Waiting · press Start queue")
                                                  : FW_TR("Queue paused · running jobs continue"));
    else if (eta >= 0)
        m_eta->setText((running > 1 ? FW_TR("Running jobs: %1") : FW_TR("Running job: %1")).arg(i18n::currentCode() == u"en" ? formatTimeLeft(eta).toLower() : formatTimeLeft(eta)));
    else if (queued > 0 && running == 0)
        m_eta->setText(FW_TR("Waiting to start"));
    else
        m_eta->setText({});
}

void QueuePage::applyFilter(int index)
{
    switch (index) {
    case 1: m_list->setFilter([](const JobRecord& r) { return visibleJob(r) && r.isActive(); }); break;
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
        if (auto rec = m_ctx.queue().record(id); rec && rec->isActive()) {
            pick = id;
            break;
        }
    if (pick.isEmpty() && !ids.isEmpty())
        pick = ids.first();
    // The filter hides every job: keep the details column on any job of the queue.
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
    // The "Not finished" tab lists failed, interrupted and cancelled jobs
    // (applyFilter), and its number counts what it lists.
    int all = 0, running = 0, queued = 0, done = 0, failed = 0, cancelled = 0;
    for (int r = 0; r < m_model.rowCount(); ++r) {
        const JobRecord* rec = m_model.recordAt(r);
        if (!rec || !visibleJob(*rec))
            continue;
        ++all;
        switch (rec->state) {
        case JobState::Running:
        case JobState::Paused:
        case JobState::Cancelling: ++running; break;
        case JobState::Queued: ++queued; break;
        case JobState::Succeeded: ++done; break;
        case JobState::Cancelled: ++cancelled; break;
        default: ++failed; break;
        }
    }
    auto seg = [](const QString& name, int n) { return n > 0 ? QStringLiteral("%1 %2").arg(name).arg(n) : name; };
    m_filter->setSegmentText(0, seg(FW_TR("All"), all));
    m_filter->setSegmentText(1, seg(FW_TR("Running"), running));
    m_filter->setSegmentText(2, seg(FW_TR("Queued"), queued));
    m_filter->setSegmentText(3, seg(FW_TR("Done"), done));
    m_filter->setSegmentText(4, seg(FW_TR("Not finished"), failed + cancelled));
    // One button: it starts a list that waits (made with "Add to queue", or
    // restored when the app started), resumes a queue paused while it ran,
    // and pauses a queue that runs.
    const bool held = m_ctx.queue().isHeld();
    const QueueSwitch next = queueSwitch(held, running, queued);
    m_pauseQueue->setText(next == QueueSwitch::Start ? startQueueText(queued)
                          : next == QueueSwitch::Resume ? FW_TR("Resume queue") : FW_TR("Pause queue"));
    setButtonIcon(m_pauseQueue, held ? QStringLiteral("play") : QStringLiteral("pause"));
    setButtonVariant(m_pauseQueue, held ? ButtonVariant::Primary : ButtonVariant::Secondary);
    m_pauseQueue->setToolTip(next == QueueSwitch::Start ? FW_TR("Run the waiting jobs, one after another.")
                             : next == QueueSwitch::Resume ? FW_TR("Start the next waiting job when the current one ends.")
                                                           : FW_TR("Finish the running job, then wait. Nothing is cancelled."));
    m_clearFinished->setEnabled(done + failed + cancelled > 0);
    m_clearFinished->setVisible(all > 0);
    updateEta();
    m_columns->setVisible(all > 0 || m_demo);
    m_emptyCard->setVisible(all == 0 && !m_demo);
    ensureSelection();
    emit statusContextChanged();
}

void QueuePage::updateDetail()
{
    if (m_demo)
        return;
    auto rec = m_ctx.queue().record(m_selected);
    const bool has = rec.has_value();
    m_detail->setVisible(has);
    if (!has) {
        m_monitor->setJobId({});
        m_settingsFor.clear();
        return;
    }
    const JobRecord& r = *rec;
    m_monitor->setJobId(r.id); // a no-op while it already follows this job

    // Settings rows: path-like parameters first, then overrides (built once per job).
    if (m_settingsFor == r.id)
        return;
    m_settingsFor = r.id;
    while (QLayoutItem* it = m_dSettingsLayout->takeAt(0)) {
        delete it->widget();
        delete it;
    }
    int overrides = 0;
    for (auto it = r.parameters.constBegin(); it != r.parameters.constEnd(); ++it) {
        // The queue file is the user's to damage: a parameter without a name is skipped.
        if (!it.value().isString() || it.key().isEmpty())
            continue;
        const QString v = it.value().toString();
        if (v.isEmpty())
            continue;
        const QString key = it.key();
        const bool pathLike = v.contains(u'/') || v.contains(u'\\');
        const bool mono = pathLike || key.endsWith(QLatin1String("Id")) || key.contains(QLatin1String("version"), Qt::CaseInsensitive);
        // A long value (a deep path) is elided by the row, with the whole text in its tooltip.
        m_dSettingsLayout->addWidget(detailRow(parameterLabel(key), pathLike ? nativePath(v) : v, mono, false, m_dSettings));
    }
    for (auto it = r.overrides.cbegin(); it != r.overrides.cend(); ++it) {
        const SettingDef* def = m_ctx.schema().find(it.key());
        m_dSettingsLayout->addWidget(detailRow(def ? shownLabel(*def) : it.key(), def ? SettingRow::displayValue(*def, it.value()) : it.value().toString(), true, true, m_dSettings));
        ++overrides;
    }
    if (m_dSettingsLayout->count() == 0)
        m_dSettingsLayout->addWidget(caption(FW_TR("Default settings"), m_dSettings));
    static_cast<FoldHeader*>(m_dSettingsToggle)->setCount(overrides > 0 ? FW_TR("%1 overridden").arg(overrides) : QString());
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
    jobs::cancelWithConfirm(m_ctx, this, id);
}

QString QueuePage::statusContext() const
{
    // The jobs this page lists (update checks and other system jobs stay out).
    int running = 0, queued = 0, done = 0, failed = 0, cancelled = 0;
    for (int r = 0; r < m_model.rowCount(); ++r) {
        const JobRecord* rec = m_model.recordAt(r);
        if (!rec || !visibleJob(*rec))
            continue;
        switch (rec->state) {
        case JobState::Running:
        case JobState::Paused:
        case JobState::Cancelling: ++running; break;
        case JobState::Queued: ++queued; break;
        case JobState::Succeeded: ++done; break;
        case JobState::Cancelled: ++cancelled; break;
        default: ++failed; break;
        }
    }
    if (cancelled > 0)
        return FW_TR("Queue: %1 running · %2 queued · %3 done · %4 failed · %5 cancelled").arg(running).arg(queued).arg(done).arg(failed).arg(cancelled);
    return FW_TR("Queue: %1 running · %2 queued · %3 done · %4 failed").arg(running).arg(queued).arg(done).arg(failed);
}

void QueuePage::applyDemoState(const QString& name)
{
    if (name == QLatin1String("job-running") || name == QLatin1String("job-done")) {
        // The details column with a made-up job (the list stays as it is).
        QStringList lines;
        const JobRecord rec = JobMonitor::demoRecord(name == QLatin1String("job-running") ? QStringLiteral("running") : QStringLiteral("done"), &lines);
        m_demo = true;
        m_columns->show();
        m_emptyCard->hide();
        m_detail->show();
        m_monitor->setDemoRecord(rec, lines);
        return;
    }
    if (name != QLatin1String("cancel-dialog"))
        return;
    auto* s = new Sheet(window(), QStringLiteral("Cancel the running build?"), Sheet::Icon::Warning);
    s->addText(QStringLiteral("Ghost of Yotei is 63 % copied. Cancelling deletes the partial image %1 and leaves the dump untouched.")
                   .arg(Sheet::monoSpan(QStringLiteral("PPSA01325_Ghost_of_Yotei_v01.000.000.exfat"))));
    s->addCheckbox(FW_TR("Also remove the job from the queue"));
    s->addButton(QStringLiteral("Keep building"), Sheet::Role::Safe);
    s->addButton(QStringLiteral("Cancel build"), Sheet::Role::Destructive);
    s->open();
    m_demoSheet = s;
}

void QueuePage::clearDemoState()
{
    if (auto* s = qobject_cast<Sheet*>(m_demoSheet.data()))
        s->close(0);
    if (m_demo) {
        m_demo = false;
        m_monitor->setJobId({});
        m_settingsFor.clear();
        updateCounts();
        updateDetail();
    }
}

} // namespace fw::pages
