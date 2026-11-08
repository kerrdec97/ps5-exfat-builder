#include "app/pages/HomePage.h"

#include "app/AppContext.h"
#include "app/models/JobQueueModel.h"
#include "app/services/AppStores.h"
#include "app/ui/Components.h"
#include "app/ui/HomeKit.h"
#include "app/ui/Icons.h"
#include "app/services/JobSubmit.h"
#include "connect/ps5/Module.h"
#include "core/BuildInfo.h"
#include "core/FileUtil.h"
#include "engines/general/Module.h"

#include <QDir>
#include <QFileInfo>
#include <QMouseEvent>
#include <QPainter>
#include <QSettings>
#include <QStorageInfo>

#include <algorithm>

namespace fw::pages {

using namespace ui;
using namespace theme;

namespace {

constexpr int kRecentRows = 6;
constexpr int kSetupColumn = 380;
const char* const kSetupExpandedKey = "home/setupExpanded";

// The sidebar icon of the page a job kind belongs to.
QString kindIcon(const QString& kind)
{
    if (kind.contains(QLatin1String("backport")))
        return QStringLiteral("backports");
    if (kind.contains(QLatin1String("fpkg")) || kind.contains(QLatin1String("package")))
        return QStringLiteral("package");
    if (kind.endsWith(QLatin1String(".edit")))
        return QStringLiteral("file_edit");
    if (kind.contains(QLatin1String("extract")) || kind.contains(QLatin1String("verify")) || kind.contains(QLatin1String("compare")))
        return QStringLiteral("extract");
    if (kind.startsWith(QLatin1String("ps5.")))
        return QStringLiteral("ps5");
    if (kind.endsWith(QLatin1String(".build")) || kind == QLatin1String("ampr.pack"))
        return QStringLiteral("build"); // ampr.pack: the Build page's packed folder
    return QStringLiteral("layers");
}

// Five rounded segments, the done ones in the signature gradient.
class StepMeter : public QWidget {
public:
    explicit StepMeter(QWidget* parent) : QWidget(parent) { setFixedHeight(6); }
    void set(int done, int total)
    {
        m_done = done;
        m_total = std::max(1, total);
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        const qreal gap = 4;
        const qreal w = (width() - gap * (m_total - 1)) / m_total;
        const QRectF all(0, 0, width(), height());
        for (int i = 0; i < m_total; ++i) {
            const QRectF r(i * (w + gap), 0, w, height());
            p.setBrush(i < m_done ? QBrush(accentGradient(all)) : QBrush(color::bg_5));
            p.drawRoundedRect(r, height() / 2.0, height() / 2.0);
        }
    }

private:
    int m_done = 0, m_total = 5;
};

// One history entry: kind tile · title / kind · when · result · folder button.
class RecentRow : public QWidget {
public:
    RecentRow(const HistoryEntry& e, const QString& kindName, bool divider, QWidget* parent)
        : QWidget(parent), m_divider(divider)
    {
        setAttribute(Qt::WA_Hover);
        setProperty("fwNoRing", true);
        m_output = e.outputs.isEmpty() ? QString() : e.outputs.first();
        auto* h = hbox(this, space::md, QMargins(space::sm, 8, space::sm, 8));
        const bool done = e.state == QLatin1String("Done");
        const bool failed = e.state == QLatin1String("Failed") || e.state == QLatin1String("Interrupted");
        auto* tile = new IconTile(kindIcon(e.kind), 36, this);
        if (failed)
            tile->setTone(Tone::Err);
        h->addWidget(tile);
        auto* col = new QVBoxLayout;
        col->setSpacing(1);
        auto* title = new ElideLabel(e.title, Qt::ElideRight, this);
        title->setFont(font::bodyStrong());
        setTextColor(title, color::fg_0);
        col->addWidget(title);
        QStringList meta{kindName, relativeTime(e.finished)};
        if (!e.error.isEmpty())
            meta << e.error;
        auto* metaLabel = new ElideLabel(meta.join(QStringLiteral(" · ")), Qt::ElideRight, this);
        metaLabel->setFont(font::caption());
        setTextColor(metaLabel, color::fg_3);
        col->addWidget(metaLabel);
        h->addLayout(col, 1);
        QString result = e.state;
        if (done && e.verification == QLatin1String("Verified"))
            result = QStringLiteral("Verified");
        auto* badge = new Badge(result, done ? Tone::Ok : failed ? Tone::Err : Tone::Neutral, this);
        if (!done && !failed)
            badge->setIconName({});
        h->addWidget(badge, 0, Qt::AlignVCenter);
        auto* folder = new IconButton(QStringLiteral("folder_open"), QStringLiteral("Show output in folder"), IconButton::Size::Small, this);
        folder->setToolTip(m_output.isEmpty() ? QStringLiteral("No output") : nativePath(m_output));
        folder->setEnabled(!m_output.isEmpty());
        QObject::connect(folder, &QToolButton::clicked, this, [this] { openInFileManager(m_output); });
        h->addWidget(folder, 0, Qt::AlignVCenter);
        setToolTip(e.started.isValid() ? QStringLiteral("Started %1 · finished %2").arg(humanTime(e.started), humanTime(e.finished))
                                       : QStringLiteral("Finished %1").arg(humanTime(e.finished)));
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        if (underMouse()) {
            p.setPen(Qt::NoPen);
            p.setBrush(color::bg_3);
            p.drawRoundedRect(QRectF(rect()).adjusted(0, 1, 0, -1), radius::control, radius::control);
        } else if (m_divider) {
            p.fillRect(QRect(space::sm, height() - 1, width() - 2 * space::sm, 1), color::border_1);
        }
    }
    void mouseDoubleClickEvent(QMouseEvent* e) override
    {
        if (!m_output.isEmpty())
            openInFileManager(m_output);
        QWidget::mouseDoubleClickEvent(e);
    }

private:
    QString m_output;
    bool m_divider;
};

} // namespace

HomePage::HomePage(AppContext& ctx, JobQueueModel& queueModel, QWidget* parent)
    : Page(ctx, QStringLiteral("home"), QStringLiteral("Home"), QStringLiteral("Start a task or see what the app is doing."), parent),
      m_model(queueModel)
{
    // The hero is this page's header.
    header()->setTitle(QStringLiteral("exFAT Image Builder"));
    header()->hide();

    // ---- Hero: what this app is for and the three things people come to do.
    const QString major = QString::fromLatin1(build::kVersion).section(u'.', 0, 0);
    m_hero = new HeroBanner(QStringLiteral("exFAT Image Builder %1").arg(major),
                            QStringLiteral("Turn PS5 game dumps into images that mount"),
                            QStringLiteral("Drop a game dump folder anywhere on this window to start, or pick a task."), this);
    m_hero->setObjectName(QStringLiteral("home-hero"));
    auto* buildBtn = button(QStringLiteral("Build an image"), ButtonVariant::Primary, QStringLiteral("build"), m_hero);
    buildBtn->setObjectName(QStringLiteral("home-build"));
    auto* backportBtn = button(QStringLiteral("Backport a game"), ButtonVariant::Secondary, QStringLiteral("backports"), m_hero);
    backportBtn->setObjectName(QStringLiteral("home-backport"));
    auto* connectBtn = button(QStringLiteral("Connect to PS5"), ButtonVariant::Secondary, QStringLiteral("plug"), m_hero);
    connectBtn->setObjectName(QStringLiteral("home-connect"));
    m_hero->actions()->addWidget(buildBtn);
    m_hero->actions()->addWidget(backportBtn);
    m_hero->actions()->addWidget(connectBtn);
    connect(buildBtn, &QPushButton::clicked, this, [this] { emit navigateRequested({QStringLiteral("build"), {}}); });
    connect(backportBtn, &QPushButton::clicked, this, [this] { emit navigateRequested({QStringLiteral("backport"), {}}); });
    connect(connectBtn, &QPushButton::clicked, this, [this] { emit navigateRequested({QStringLiteral("ps5"), {}}); });
    content()->addWidget(m_hero);

    // ---- Status tiles
    auto* stats = new QWidget(this);
    auto* sg = new TileGrid(stats, 220, 3);
    m_consoleTile = new StatTile(QStringLiteral("ps5"), QStringLiteral("PS5 console"), stats);
    m_consoleTile->setObjectName(QStringLiteral("home-console"));
    m_consoleBadge = new Badge(QStringLiteral("Setup complete"), Tone::Ok, m_consoleTile);
    m_consoleBadge->hide();
    m_consoleTile->footer()->addWidget(m_consoleBadge);
    m_consoleTile->footer()->addStretch(1);
    m_queueTile = new StatTile(QStringLiteral("queue"), QStringLiteral("Queue"), stats);
    m_queueTile->setObjectName(QStringLiteral("home-queue"));
    m_queueBar = new ProgressBar(m_queueTile);
    m_queueBar->hide();
    m_queueTile->footer()->addWidget(m_queueBar, 1);
    m_spaceTile = new StatTile(QStringLiteral("hard_drive"), QStringLiteral("Free space"), stats);
    m_spaceTile->setObjectName(QStringLiteral("home-space"));
    for (StatTile* t : {m_consoleTile, m_queueTile, m_spaceTile}) {
        t->setCursor(Qt::PointingHandCursor);
        sg->addWidget(t);
    }
    m_consoleTile->setToolTip(QStringLiteral("Open PS5 connect"));
    m_queueTile->setToolTip(QStringLiteral("Open the queue"));
    m_spaceTile->setToolTip(QStringLiteral("Change the output folder"));
    connect(m_consoleTile, &StatTile::clicked, this, [this] { emit navigateRequested({QStringLiteral("ps5"), {}}); });
    connect(m_queueTile, &StatTile::clicked, this, [this] { emit navigateRequested({QStringLiteral("queue"), {}}); });
    connect(m_spaceTile, &StatTile::clicked, this, [this] {
        emit navigateRequested({QStringLiteral("settings"), {{QStringLiteral("key"), QString(general::keys::kDefaultOutputDir)}}});
    });
    content()->addWidget(stats);

    // ---- Pick up where you left off: the most recent failed / interrupted job.
    m_continue = new InfoBar(Tone::Warn, this);
    m_continue->setObjectName(QStringLiteral("home-continue"));
    m_continue->setDismissable(true);
    auto* retry = m_continue->addLink(QStringLiteral("Retry"));
    auto* openJob = m_continue->addLink(QStringLiteral("Open in queue"));
    m_continue->hide();
    content()->addWidget(m_continue);
    connect(retry, &Link::activated, this, [this] {
        if (m_continueId.isEmpty())
            return;
        auto r = m_ctx.queue().retry(m_continueId);
        if (r.ok()) {
            m_ctx.queue().startNow(r.value()); // this job only; a paused queue stays paused
            emit navigateRequested({QStringLiteral("queue"), {{QStringLiteral("jobId"), r.value()}}});
        }
    });
    connect(openJob, &Link::activated, this, [this] {
        emit navigateRequested({QStringLiteral("queue"), {{QStringLiteral("jobId"), m_continueId}}});
    });
    connect(m_continue, &InfoBar::dismissed, this, [this] {
        m_continueDismissed = m_continueId;
        m_continue->hide();
    });

    // ---- More tools
    auto* toolsHead = label(QStringLiteral("More tools"), TextStyle::Overline, color::fg_4, this);
    toolsHead->setContentsMargins(2, space::xs, 0, 0);
    content()->addWidget(toolsHead);
    auto* tools = new QWidget(this);
    auto* tg = new TileGrid(tools, 220, 3);
    struct Tool {
        const char* icon;
        const char* title;
        const char* text;
        const char* page;
    };
    const Tool toolList[] = {
        {"extract", "Extract & verify", "Check an image or copy its files back out.", "extract"},
        {"file_edit", "Edit an image", "Add, replace or delete files without a rebuild.", "edit"},
        {"library", "Game library", "Your dumps, images and DLC in one place.", "library"},
    };
    for (const Tool& t : toolList) {
        auto* card = new ActionCard(QString::fromLatin1(t.icon), QString::fromLatin1(t.title), QString::fromLatin1(t.text), tools);
        const QString page = QString::fromLatin1(t.page);
        connect(card, &QAbstractButton::clicked, this, [this, page] { emit navigateRequested({page, {}}); });
        tg->addWidget(card);
    }
    content()->addWidget(tools);

    // ---- Recent jobs | PS5 setup
    auto* columns = new QWidget(this);
    m_columns = new TwoColumnLayout(columns, kSetupColumn, 100000);

    m_recentCard = new Card(columns);
    m_recentCard->setObjectName(QStringLiteral("home-recent"));
    auto* openQueue = new Link(QStringLiteral("Open queue"), m_recentCard);
    m_recentCard->setTitle(QStringLiteral("Recent jobs"), openQueue);
    connect(openQueue, &Link::activated, this, [this] { emit navigateRequested({QStringLiteral("queue"), {}}); });
    m_recentCard->body()->setSpacing(0);
    m_recentRows = new QWidget(m_recentCard);
    vbox(m_recentRows, 0);
    m_recentCard->body()->addWidget(m_recentRows);
    m_recentEmpty = new EmptyState(QStringLiteral("history"), QStringLiteral("No jobs yet"),
                                   QStringLiteral("Finished jobs show up here."), m_recentCard);
    m_recentEmpty->setCompact(true);
    auto* first = m_recentEmpty->addAction(QStringLiteral("Build your first image"), ButtonVariant::Secondary);
    connect(first, &QPushButton::clicked, this, [this] { emit navigateRequested({QStringLiteral("build"), {}}); });
    m_recentCard->body()->addWidget(m_recentEmpty);
    m_columns->setLeft(m_recentCard);

    m_setup = new Disclosure(QStringLiteral("Set up your PS5"), QStringLiteral("Optional, for network transfers"), columns);
    m_setup->setObjectName(QStringLiteral("home-setup"));
    auto* meter = new StepMeter(m_setup);
    meter->setObjectName(QStringLiteral("home-setup-meter"));
    m_setup->body()->addWidget(meter);
    m_setupRows = new QWidget(m_setup);
    vbox(m_setupRows, 0);
    m_setup->body()->addWidget(m_setupRows);
    {
        QSettings s;
        m_setup->setExpanded(s.value(QLatin1String(kSetupExpandedKey), true).toBool());
    }
    connect(m_setup, &Disclosure::toggled, this, [](bool on) { QSettings().setValue(QLatin1String(kSetupExpandedKey), on); });
    m_columns->setRight(m_setup);
    content()->addWidget(columns);

    m_lastCheck = loadLastFtpCheck();
    connect(&ctx.settings(), &SettingsStore::changed, this, [this](const QString& key, const QVariant&) {
        if (key.startsWith(QLatin1String("ps5."))) {
            refreshChecklist();
            refreshConsoleTile();
        }
    });
    connect(&ctx.settings(), &SettingsStore::resetDone, this, [this] {
        refreshChecklist();
        refreshConsoleTile();
    });
    // The last FTP diagnosis feeds the "FTP reachable" row, also after a restart.
    connect(&ctx.queue(), &JobQueue::jobFinished, this, [this](const QString& id) {
        const auto rec = m_ctx.queue().record(id);
        if (!rec)
            return;
        if (const auto check = ftpCheckFromJob(*rec)) {
            m_lastCheck = check;
            (void)saveLastFtpCheck(*check);
            refreshChecklist();
        }
    });

    connect(&m_model, &JobQueueModel::countsChanged, this, &HomePage::refresh);
    connect(&m_model, &QAbstractItemModel::dataChanged, this, &HomePage::refreshQueueTile);
    refresh();
}

void HomePage::setConsoleStatus(const QString& text, Tone tone)
{
    m_consoleText = text;
    m_consoleTone = tone;
    m_browserConnected = tone == Tone::Ok;
    refreshConsoleTile();
    refreshChecklist();
}

void HomePage::refreshConsoleTile()
{
    const QString ip = m_ctx.settings().value(ps5::keys::kConsoleIp).toString().trimmed();
    if (m_consoleTone == Tone::Ok) {
        m_consoleTile->setValue(m_consoleText, Tone::Ok);
        m_consoleTile->setCaption(QStringLiteral("Connected"));
    } else if (!m_consoleText.isEmpty()) {
        m_consoleTile->setValue(m_consoleText);
        m_consoleTile->setCaption(ip.isEmpty() ? QStringLiteral("Find it on PS5 connect") : QStringLiteral("Address %1").arg(ip));
    } else if (ip.isEmpty()) {
        m_consoleTile->setValue(QStringLiteral("Not set up"));
        m_consoleTile->setCaption(QStringLiteral("Find it on PS5 connect"));
    } else {
        m_consoleTile->setValue(QStringLiteral("Not connected"));
        m_consoleTile->setCaption(QStringLiteral("Address %1").arg(ip));
    }
}

void HomePage::setPayloadStore(PayloadStore* store)
{
    if (m_payloads)
        disconnect(m_payloads, nullptr, this, nullptr);
    m_payloads = store;
    if (m_payloads)
        connect(m_payloads, &PayloadStore::changed, this, &HomePage::refreshChecklist);
    refreshChecklist();
}

void HomePage::setSetupShown(bool shown)
{
    if (shown == m_setupShown)
        return;
    m_setupShown = shown;
    m_columns->setRight(shown ? m_setup : nullptr);
    m_setup->setVisible(shown);
}

void HomePage::refreshChecklist()
{
    ChecklistInputs in = checklistInputs(m_ctx.settings().snapshot());
    in.lastCheck = m_lastCheck;
    in.browserConnected = m_browserConnected;
    if (m_payloads)
        for (const auto& e : m_payloads->entries())
            if (QFileInfo(e.path).isFile())
                ++in.payloadsReady;
    m_checklist = setupChecklist(in);
    const int done = completedCount(m_checklist);
    const bool all = done == m_checklist.size();
    m_checklistProgress = QStringLiteral("%1 / %2 complete").arg(done).arg(m_checklist.size());
    m_setup->setSummary(m_checklistProgress, all ? Tone::Ok : Tone::Neutral);
    if (auto* meter = m_setup->findChild<QWidget*>(QStringLiteral("home-setup-meter")))
        static_cast<StepMeter*>(meter)->set(done, int(m_checklist.size()));

    auto* rows = m_setupRows->layout();
    while (QLayoutItem* it = rows->takeAt(0)) {
        delete it->widget();
        delete it;
    }
    bool nextShown = false;
    for (const ChecklistItem& item : m_checklist) {
        auto* row = new QWidget(m_setupRows);
        row->setObjectName(QStringLiteral("checklist-") + item.id);
        const bool next = !item.done && !nextShown;
        nextShown = nextShown || next;
        auto* h = hbox(row, space::md, QMargins(0, 7, 0, 7));
        auto* glyph = new QLabel(row);
        glyph->setFixedSize(size::icon, size::icon);
        glyph->setPixmap(iconPixmap(item.done ? QStringLiteral("success") : next ? QStringLiteral("circle_dashed") : QStringLiteral("circle"), size::icon,
                                    item.done ? color::success : next ? color::accent_hi : color::fg_5, devicePixelRatioF()));
        h->addWidget(glyph, 0, Qt::AlignTop);
        auto* text = new QWidget(row);
        auto* tv = vbox(text, 1);
        tv->addWidget(label(item.label, next ? TextStyle::BodyStrong : TextStyle::Body, item.done ? color::fg_2 : color::fg_0, text));
        auto* detail = new ElideLabel(item.detail, Qt::ElideRight, text);
        detail->setFont(font::caption());
        setTextColor(detail, next ? color::fg_2 : color::fg_4);
        detail->setToolTip(item.detail);
        tv->addWidget(detail);
        h->addWidget(text, 1);
        if (!item.fixText.isEmpty()) {
            auto* fix = new Link(item.fixText, row);
            const NavRequest nav{item.fixPage, item.fixArgs};
            // Queued: the rows are rebuilt on many signals; never inside the link's own click.
            connect(fix, &Link::activated, this, [this, nav] { emit navigateRequested(nav); }, Qt::QueuedConnection);
            h->addWidget(fix, 0, Qt::AlignTop);
        }
        rows->addWidget(row);
    }
    setSetupShown(!all);
    m_consoleBadge->setVisible(all);
}

void HomePage::refreshQueueTile()
{
    int running = 0, queued = 0, failed = 0;
    double fraction = -1;
    bool indeterminate = false;
    QString stage; // "Packing · About 3 min left" of the first running job
    for (int r = 0; r < m_model.rowCount(); ++r) {
        const JobRecord* rec = m_model.recordAt(r);
        if (!rec || jobs::isSystemKind(rec->kind))
            continue;
        switch (rec->state) {
        case JobState::Running:
        case JobState::Paused:
        case JobState::Cancelling:
            ++running;
            if (fraction < 0) {
                fraction = rec->progress.overallFraction;
                indeterminate = rec->progress.indeterminate && rec->state == JobState::Running;
                stage = rec->state == JobState::Paused ? QStringLiteral("Paused")
                        : rec->state == JobState::Cancelling ? QStringLiteral("Cancelling") : rec->progress.phase;
                if (rec->state == JobState::Running && rec->progress.etaMs >= 0)
                    stage += QStringLiteral(" · ") + formatTimeLeft(rec->progress.etaMs).toLower();
            }
            break;
        case JobState::Queued: ++queued; break;
        case JobState::Failed:
        case JobState::Interrupted: ++failed; break;
        default: break;
        }
    }
    if (running > 0)
        m_queueTile->setValue(QStringLiteral("%1 running").arg(running), Tone::Info);
    else if (queued > 0)
        m_queueTile->setValue(QStringLiteral("%1 waiting").arg(queued));
    else
        m_queueTile->setValue(QStringLiteral("Idle"));
    QStringList qc;
    if (running > 0 && !stage.isEmpty())
        qc << stage;
    if (queued > 0 && running > 0)
        qc << QStringLiteral("%1 waiting").arg(queued);
    if (failed > 0)
        qc << QStringLiteral("%1 failed").arg(failed);
    if (m_ctx.queue().isHeld())
        qc << QStringLiteral("queue paused");
    m_queueTile->setCaption(qc.isEmpty() ? (running > 0 ? QString() : QStringLiteral("Jobs run one after another")) : qc.join(QStringLiteral(" · ")));
    m_queueBar->setVisible(running > 0);
    if (running > 0) {
        m_queueBar->setState(indeterminate ? ProgressBar::State::Indeterminate : ProgressBar::State::Determinate);
        m_queueBar->setFraction(std::max(0.0, fraction));
    }
}

void HomePage::refreshRecent()
{
    // The user's own jobs only: update checks and other system jobs stay out.
    std::vector<HistoryEntry> recent;
    for (const HistoryEntry& e : m_ctx.history().recent(kRecentRows * 4)) {
        if (jobs::isSystemKind(e.kind))
            continue;
        recent.push_back(e);
        if (int(recent.size()) == kRecentRows)
            break;
    }
    auto* rows = m_recentRows->layout();
    while (QLayoutItem* it = rows->takeAt(0)) {
        delete it->widget();
        delete it;
    }
    for (size_t i = 0; i < recent.size(); ++i)
        rows->addWidget(new RecentRow(recent[i], m_ctx.jobFactory().displayName(recent[i].kind), i + 1 < recent.size(), m_recentRows));
    m_recentRows->setVisible(!recent.empty());
    m_recentEmpty->setVisible(recent.empty());
    m_recentCard->setToolTip(QStringLiteral("History is kept in %1").arg(nativePath(m_ctx.history().path())));
}

void HomePage::refresh()
{
    refreshChecklist();
    refreshConsoleTile();
    refreshQueueTile();

    QString folder = m_ctx.settings().value(general::keys::kDefaultOutputDir).toString();
    const bool nextToSource = folder.isEmpty();
    if (nextToSource)
        folder = QDir::homePath();
    // Off the UI thread (refresh() runs on every queue change; a network drive answers slowly).
    const quint64 gen = ++m_spaceGen;
    probeFreeSpace(this, folder, [this, gen, nextToSource](bool ready, qint64 available, const QString& label) {
        if (gen != m_spaceGen)
            return;
        if (ready) {
            m_spaceTile->setValue(formatBytes(available));
            m_spaceTile->setCaption(QStringLiteral("on %1 · %2").arg(label, nextToSource ? QStringLiteral("home drive") : QStringLiteral("output folder")));
        } else {
            m_spaceTile->setValue(QStringLiteral("—"));
            m_spaceTile->setCaption(QStringLiteral("Output drive not available"));
        }
    });

    // Pick up where you left off: the most recent failed / interrupted / cancelled job.
    m_continueId.clear();
    for (int r = m_model.rowCount() - 1; r >= 0; --r) {
        const JobRecord* rec = m_model.recordAt(r);
        if (rec && !jobs::isSystemKind(rec->kind)
            && (rec->state == JobState::Failed || rec->state == JobState::Interrupted || rec->state == JobState::Cancelled)) {
            m_continueId = rec->id;
            const QString why = rec->error && !rec->error->isCancelled() ? rec->error->message
                              : rec->state == JobState::Interrupted      ? QStringLiteral("Interrupted when the app closed.")
                                                                         : QStringLiteral("Cancelled.");
            m_continue->setTone(rec->state == JobState::Failed ? Tone::Err : Tone::Warn);
            m_continue->setTitle(QStringLiteral("%1 · %2").arg(rec->title, m_ctx.jobFactory().displayName(rec->kind)));
            // One calm line here; the queue shows the whole message.
            m_continue->setMessage(elideMiddle(why, 110));
            m_continue->setToolTip(why.size() > 110 ? why : QString());
            break;
        }
    }
    m_continue->setVisible(!m_continueId.isEmpty() && m_continueId != m_continueDismissed);

    refreshRecent();
}

} // namespace fw::pages
