#include "app/pages/HomePage.h"

#include "app/AppContext.h"
#include "app/models/JobQueueModel.h"
#include "app/ui/Icons.h"
#include "core/FileUtil.h"
#include "engines/general/Module.h"

#include <QDir>
#include <QHeaderView>
#include <QStorageInfo>

namespace fw::pages {

using namespace ui;
using namespace theme;

static QWidget* statCell(const QString& title, QLabel*& value, QLabel*& captionLabel, QWidget* parent)
{
    auto* w = new QWidget(parent);
    auto* l = vbox(w, 2);
    l->addWidget(caption(title, w));
    value = label(QStringLiteral("—"), TextStyle::Stat, color::fg_0, w);
    l->addWidget(value);
    captionLabel = caption({}, w);
    l->addWidget(captionLabel);
    return w;
}

HomePage::HomePage(AppContext& ctx, JobQueueModel& queueModel, QWidget* parent)
    : Page(ctx, QStringLiteral("home"), QStringLiteral("Home"),
           QStringLiteral("What the app is doing right now, and where you left off."), parent),
      m_model(queueModel)
{
    header()->setTitle(QStringLiteral("exFAT Image Builder"));
    // ---- Status strip
    auto* strip = new Card(this);
    auto* row = new QHBoxLayout;
    row->setSpacing(space::xxl);
    QLabel* dummy = nullptr;
    auto* console = new QWidget(strip);
    {
        auto* l = vbox(console, 2);
        l->addWidget(caption(QStringLiteral("PS5 console"), console));
        m_consoleValue = label(QStringLiteral("Not connected"), TextStyle::Stat, color::fg_0, console);
        l->addWidget(m_consoleValue);
        auto* badgeRow = new QHBoxLayout;
        m_consoleBadge = new Badge(QStringLiteral("Open PS5 connect"), Tone::Neutral, console);
        m_consoleBadge->setIconName(QStringLiteral("plug"));
        badgeRow->addWidget(m_consoleBadge);
        badgeRow->addStretch(1);
        l->addLayout(badgeRow);
    }
    row->addWidget(console, 1);
    row->addWidget(statCell(QStringLiteral("Queue"), m_queueValue, m_queueCaption, strip), 1);
    row->addWidget(statCell(QStringLiteral("Free space"), m_spaceValue, m_spaceCaption, strip), 1);
    Q_UNUSED(dummy);
    strip->body()->addLayout(row);
    content()->addWidget(strip);

    // ---- Continue
    m_continue = new Card(this);
    m_continueButton = button(QStringLiteral("Retry"), ButtonVariant::Primary, QStringLiteral("undo"), m_continue);
    m_continue->setTitle(QStringLiteral("Continue"), m_continueButton);
    m_continueTitle = label({}, TextStyle::BodyStrong, color::fg_0, m_continue);
    m_continueMeta = label({}, TextStyle::Mono, color::fg_3, m_continue);
    m_continue->body()->addWidget(m_continueTitle);
    m_continue->body()->addWidget(m_continueMeta);
    m_continue->hide();
    content()->addWidget(m_continue);
    connect(m_continueButton, &QPushButton::clicked, this, [this] {
        if (m_continueId.isEmpty())
            return;
        auto r = m_ctx.queue().retry(m_continueId);
        m_ctx.queue().holdQueue(false);
        if (r.ok())
            emit navigateRequested({QStringLiteral("queue"), {{QStringLiteral("jobId"), r.value()}}});
    });

    // ---- Recent jobs
    m_recentCard = new Card(this, Card::Variant::Table);
    auto* h = m_recentCard->header();
    h->addWidget(heading(QStringLiteral("Recent jobs"), TextStyle::Subtitle, m_recentCard));
    h->addStretch(1);
    auto* openQueue = new Link(QStringLiteral("Open queue"), m_recentCard);
    connect(openQueue, &Link::activated, this, [this] { emit navigateRequested({QStringLiteral("queue"), {}}); });
    h->addWidget(openQueue);
    m_recent = new QTableWidget(0, 5, m_recentCard);
    m_recent->setHorizontalHeaderLabels({QStringLiteral("Job"), QStringLiteral("Kind"), QStringLiteral("Finished"), QStringLiteral("Result"), QStringLiteral("Output")});
    m_recent->horizontalHeader()->setStretchLastSection(true);
    m_recent->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_recent->horizontalHeader()->setHighlightSections(false);
    m_recent->horizontalHeader()->setFixedHeight(32);
    m_recent->verticalHeader()->hide();
    m_recent->verticalHeader()->setDefaultSectionSize(size::row);
    m_recent->setShowGrid(false);
    m_recent->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_recent->setSelectionMode(QAbstractItemView::SingleSelection);
    m_recent->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_recent->setFocusPolicy(Qt::StrongFocus);
    m_recent->setMinimumHeight(5 * size::row + 40);
    m_recent->setSizeAdjustPolicy(QAbstractScrollArea::AdjustToContents);
    m_recentCard->body()->addWidget(m_recent);
    m_recentEmpty = new EmptyState(QStringLiteral("history"), QStringLiteral("No jobs yet"),
                                   QStringLiteral("Finished jobs are listed here. Start on Build image, FPKG or Backport."), m_recentCard);
    auto* b = m_recentEmpty->addAction(QStringLiteral("Build image"), ButtonVariant::Secondary);
    connect(b, &QPushButton::clicked, this, [this] { emit navigateRequested({QStringLiteral("build"), {}}); });
    m_recentCard->body()->addWidget(m_recentEmpty);
    m_recentFooter = caption({}, m_recentCard);
    m_recentFooter->setWordWrap(false);
    m_recentFooter->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_recentCard->footer()->addWidget(m_recentFooter);
    content()->addWidget(m_recentCard);
    connect(m_recent, &QTableWidget::cellDoubleClicked, this, [this](int r, int) {
        const QString out = m_recent->item(r, 4) ? m_recent->item(r, 4)->data(Qt::UserRole).toString() : QString();
        if (!out.isEmpty())
            openInFileManager(out);
    });

    connect(&m_model, &JobQueueModel::countsChanged, this, &HomePage::refresh);
    connect(&m_model, &QAbstractItemModel::dataChanged, this, [this] {
        const int running = m_model.countInState(JobState::Running);
        const int queued = m_model.countInState(JobState::Queued);
        m_queueValue->setText(running > 0 ? QStringLiteral("%1 running").arg(running) : queued > 0 ? QStringLiteral("%1 queued").arg(queued) : QStringLiteral("Idle"));
    });
    refresh();
}

void HomePage::setConsoleStatus(const QString& text, Tone tone)
{
    m_consoleValue->setText(text);
    m_consoleBadge->set(tone == Tone::Ok ? QStringLiteral("Connected") : QStringLiteral("Open PS5 connect"), tone);
}

void HomePage::refresh()
{
    const int running = m_model.countInState(JobState::Running) + m_model.countInState(JobState::Paused);
    const int queued = m_model.countInState(JobState::Queued);
    const int failed = m_model.countInState(JobState::Failed) + m_model.countInState(JobState::Interrupted);
    m_queueValue->setText(running > 0 ? QStringLiteral("%1 running").arg(running) : queued > 0 ? QStringLiteral("%1 queued").arg(queued) : QStringLiteral("Idle"));
    QStringList qc;
    if (queued > 0 && running > 0)
        qc << QStringLiteral("%1 queued").arg(queued);
    if (failed > 0)
        qc << QStringLiteral("%1 failed").arg(failed);
    if (m_ctx.queue().isHeld())
        qc << QStringLiteral("queue paused");
    m_queueCaption->setText(qc.isEmpty() ? QStringLiteral("Jobs run one after another") : qc.join(QStringLiteral(" · ")));

    QString folder = m_ctx.settings().value(general::keys::kDefaultOutputDir).toString();
    if (folder.isEmpty())
        folder = QDir::homePath();
    const QStorageInfo si(folder);
    if (si.isValid() && si.isReady()) {
        m_spaceValue->setText(formatBytes(si.bytesAvailable()));
        m_spaceCaption->setText(QStringLiteral("on %1 · %2").arg(driveLabel(folder), si.fileSystemType().isEmpty() ? QStringLiteral("output drive") : QString::fromLatin1(si.fileSystemType())));
    } else {
        m_spaceValue->setText(QStringLiteral("—"));
        m_spaceCaption->setText(QStringLiteral("Output drive not available"));
    }

    // Continue: the most recent failed/interrupted job in the queue.
    m_continueId.clear();
    for (int r = m_model.rowCount() - 1; r >= 0; --r) {
        const JobRecord* rec = m_model.recordAt(r);
        if (rec && (rec->state == JobState::Failed || rec->state == JobState::Interrupted || rec->state == JobState::Cancelled)) {
            m_continueId = rec->id;
            m_continueTitle->setText(rec->title);
            const QString why = rec->state == JobState::Interrupted ? QStringLiteral("Interrupted when the app closed")
                              : rec->error ? rec->error->message : jobStateName(rec->state);
            m_continueMeta->setText(QStringLiteral("%1 · %2").arg(m_ctx.jobFactory().displayName(rec->kind), why));
            break;
        }
    }
    m_continue->setVisible(!m_continueId.isEmpty());

    // Recent jobs
    const auto recent = m_ctx.history().recent(10);
    m_recent->setRowCount(0);
    for (const auto& e : recent) {
        const int r = m_recent->rowCount();
        m_recent->insertRow(r);
        auto* title = new QTableWidgetItem(e.title);
        title->setFont(font::bodyStrong());
        title->setForeground(color::fg_0);
        m_recent->setItem(r, 0, title);
        m_recent->setItem(r, 1, new QTableWidgetItem(m_ctx.jobFactory().displayName(e.kind)));
        auto* when = new QTableWidgetItem(humanTime(e.finished));
        when->setFont(font::mono());
        when->setForeground(color::fg_3);
        m_recent->setItem(r, 2, when);
        QString result = e.state;
        if (e.state == QLatin1String("Done") && e.verification == QLatin1String("Verified"))
            result = QStringLiteral("Done · verified");
        else if (!e.error.isEmpty())
            result = QStringLiteral("%1 · %2").arg(e.state, e.error);
        auto* res = new QTableWidgetItem(result);
        res->setForeground(e.state == QLatin1String("Done") ? color::success_hi : e.state == QLatin1String("Failed") ? color::danger_hi : color::fg_2);
        m_recent->setItem(r, 3, res);
        auto* out = new QTableWidgetItem(e.outputs.isEmpty() ? QString() : nativePath(e.outputs.first()));
        out->setFont(font::mono());
        out->setForeground(color::fg_3);
        out->setData(Qt::UserRole, e.outputs.isEmpty() ? QString() : e.outputs.first());
        out->setToolTip(e.outputs.isEmpty() ? QString() : QStringLiteral("Double-click to show in folder"));
        m_recent->setItem(r, 4, out);
    }
    m_recent->setVisible(!recent.empty());
    m_recentEmpty->setVisible(recent.empty());
    m_recentFooter->setText(recent.empty() ? QStringLiteral("History is kept in %1").arg(nativePath(m_ctx.history().path()))
                                           : QStringLiteral("Last %1 jobs · double-click a row to show its output").arg(recent.size()));
}

} // namespace fw::pages
