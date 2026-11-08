#include "app/pages/ps5/TransferPanel.h"

#include "app/AppContext.h"
#include "app/ui/Icons.h"
#include "connect/ps5/Module.h"
#include "core/FileUtil.h"

#include <QHBoxLayout>
#include <QVBoxLayout>

#include <algorithm>

namespace fw::pages {

using namespace ui;
using namespace theme;

// ---- row ------------------------------------------------------------------------------

TransferRow::TransferRow(QWidget* parent) : QWidget(parent)
{
    auto* h = hbox(this, space::md, QMargins(0, space::sm, 0, space::sm));
    m_tile = new IconTile(QStringLiteral("upload"), 36, this);
    h->addWidget(m_tile, 0, Qt::AlignTop);
    auto* mid = new QWidget(this);
    auto* ml = vbox(mid, 3);
    auto* top = new QWidget(mid);
    auto* th = hbox(top, space::sm);
    m_title = new ElideLabel({}, Qt::ElideRight, top);
    m_title->setFont(font::bodyStrong());
    setTextColor(m_title, color::fg_0);
    th->addWidget(m_title, 1);
    m_badge = new Badge({}, Tone::Neutral, top);
    th->addWidget(m_badge);
    ml->addWidget(top);
    m_item = new ElideLabel({}, Qt::ElideMiddle, mid);
    m_item->setFont(font::monoSmall());
    setTextColor(m_item, color::fg_3);
    ml->addWidget(m_item);
    m_bar = new ProgressBar(mid);
    ml->addWidget(m_bar);
    m_stats = caption({}, mid);
    m_stats->setWordWrap(true);
    ml->addWidget(m_stats);
    h->addWidget(mid, 1);
    auto* acts = new QWidget(this);
    auto* ah = hbox(acts, 2);
    m_pause = new IconButton(QStringLiteral("pause"), QStringLiteral("Pause"), IconButton::Size::Small, acts);
    m_cancel = new IconButton(QStringLiteral("close"), QStringLiteral("Cancel"), IconButton::Size::Small, acts);
    m_retry = new IconButton(QStringLiteral("rotate_ccw"), QStringLiteral("Retry"), IconButton::Size::Small, acts);
    m_remove = new IconButton(QStringLiteral("trash"), QStringLiteral("Remove from the list"), IconButton::Size::Small, acts);
    for (auto* b : {m_pause, m_cancel, m_retry, m_remove})
        ah->addWidget(b);
    h->addWidget(acts, 0, Qt::AlignTop);
    connect(m_pause, &QToolButton::clicked, this, [this] {
        if (m_pause->iconName() == u"play")
            emit resumeRequested(m_id);
        else
            emit pauseRequested(m_id);
    });
    connect(m_cancel, &QToolButton::clicked, this, [this] { emit cancelRequested(m_id); });
    connect(m_retry, &QToolButton::clicked, this, [this] { emit retryRequested(m_id); });
    connect(m_remove, &QToolButton::clicked, this, [this] { emit removeRequested(m_id); });
}

void TransferRow::setRecord(const JobRecord& r)
{
    m_id = r.id;
    const bool upload = r.kind == ps5::jobkinds::kUpload;
    m_tile->setIconName(upload ? QStringLiteral("upload") : QStringLiteral("download"));
    m_title->setFullText(r.title);
    const ProgressSnapshot& p = r.progress;
    QString item = p.currentItem;
    if (item.isEmpty())
        item = r.subtitle;
    m_item->setFullText(item);
    m_item->setToolTip(item);
    const bool running = r.state == JobState::Running;
    const bool paused = r.state == JobState::Paused;
    const bool ended = isTerminal(r.state);
    // Bar
    if (r.state == JobState::Succeeded) {
        m_bar->setState(ProgressBar::State::Done);
        m_bar->setFraction(1);
    } else if (r.state == JobState::Failed || r.state == JobState::Cancelled || r.state == JobState::Interrupted) {
        m_bar->setState(ProgressBar::State::Failed);
        m_bar->setFraction(std::max(0.02, p.overallFraction));
    } else if (paused) {
        m_bar->setState(ProgressBar::State::Paused);
        m_bar->setFraction(p.overallFraction);
    } else if (running && (p.indeterminate || p.total <= 0)) {
        m_bar->setState(ProgressBar::State::Indeterminate);
    } else {
        m_bar->setState(ProgressBar::State::Determinate);
        m_bar->setFraction(p.overallFraction);
    }
    // Stats line: bytes, speed, time left, files.
    QStringList parts;
    if (p.unitIsBytes && p.total > 0)
        parts << QStringLiteral("%1 of %2").arg(formatBytes(p.done), formatBytes(p.total));
    if (running && p.ratePerSec > 0)
        parts << formatRate(p.ratePerSec);
    if (running && p.etaMs >= 0)
        parts << formatTimeLeft(p.etaMs);
    if (p.filesTotal > 1)
        parts << QStringLiteral("file %1 of %2").arg(std::min(p.filesDone + (ended ? 0 : 1), p.filesTotal)).arg(p.filesTotal);
    QString stats = parts.join(QStringLiteral(" · "));
    Tone tone = Tone::Info;
    QString word;
    switch (r.state) {
    case JobState::Queued: word = QStringLiteral("Waiting"); tone = Tone::Neutral; stats = QStringLiteral("Waits for the queue"); break;
    case JobState::Running: word = thinPercent(p.overallFraction); tone = Tone::Info; break;
    case JobState::Paused: word = QStringLiteral("Paused"); tone = Tone::Warn; break;
    case JobState::Succeeded:
        word = QStringLiteral("Done");
        tone = r.outcome && !r.outcome->warnings.isEmpty() ? Tone::Warn : Tone::Ok;
        stats = r.outcome ? r.outcome->summary : stats;
        break;
    case JobState::Failed:
        word = QStringLiteral("Failed");
        tone = Tone::Err;
        stats = r.error ? r.error->message : QStringLiteral("The transfer failed.");
        break;
    case JobState::Cancelled: word = QStringLiteral("Cancelled"); tone = Tone::Neutral; stats = r.error ? r.error->message : stats; break;
    case JobState::Interrupted: word = QStringLiteral("Interrupted"); tone = Tone::Warn; stats = QStringLiteral("The app closed during the transfer; retry to continue."); break;
    }
    m_badge->set(word, tone);
    if (r.state == JobState::Running)
        m_badge->setIconName({});
    m_stats->setText(stats);
    setTextColor(m_stats, r.state == JobState::Failed ? color::danger_hi : color::fg_3);
    m_tile->setTone(r.state == JobState::Failed ? Tone::Err : r.state == JobState::Succeeded ? Tone::Ok : Tone::Info);
    m_pause->setVisible(running || paused);
    m_pause->setIconName(paused ? QStringLiteral("play") : QStringLiteral("pause"));
    m_pause->setToolTip(paused ? QStringLiteral("Resume") : QStringLiteral("Pause"));
    m_pause->setAccessibleName(paused ? QStringLiteral("Resume") : QStringLiteral("Pause"));
    m_cancel->setVisible(!ended);
    m_retry->setVisible(r.state == JobState::Failed || r.state == JobState::Cancelled || r.state == JobState::Interrupted);
    m_remove->setVisible(ended);
}

QString TransferRow::statsText() const
{
    return m_stats->text();
}

QString TransferRow::stateText() const
{
    return m_badge->text();
}

// ---- panel ------------------------------------------------------------------------------

bool TransferPanel::isTransferKind(const QString& kind)
{
    return kind == ps5::jobkinds::kUpload || kind == ps5::jobkinds::kDownload;
}

TransferPanel::TransferPanel(AppContext& ctx, QWidget* parent) : Card(parent), m_ctx(ctx)
{
    setObjectName(QStringLiteral("ps5Transfers"));
    auto* right = new QWidget(this);
    auto* rh = hbox(right, space::md);
    m_summary = new Badge({}, Tone::Neutral, right);
    m_summary->setIconName({});
    rh->addWidget(m_summary);
    m_clear = new Link(QStringLiteral("Clear finished"), right);
    rh->addWidget(m_clear);
    setTitle(QStringLiteral("Transfers"), right);
    m_list = new QWidget(this);
    m_rows = vbox(m_list, 0);
    body()->addWidget(m_list);
    m_empty = new QWidget(this);
    auto* eh = hbox(m_empty, space::sm);
    auto* glyph = new QLabel(m_empty);
    glyph->setPixmap(iconPixmap(QStringLiteral("cloud_upload"), size::icon, color::fg_4, devicePixelRatioF()));
    eh->addWidget(glyph);
    eh->addWidget(caption(QStringLiteral("Uploads and downloads show here with their speed and time left. Drop files on the list above to upload them."), m_empty), 1);
    body()->addWidget(m_empty);
    connect(m_clear, &Link::activated, this, &TransferPanel::clearFinished);
    auto& q = m_ctx.queue();
    connect(&q, &JobQueue::jobAdded, this, &TransferPanel::sync);
    connect(&q, &JobQueue::jobChanged, this, &TransferPanel::sync);
    connect(&q, &JobQueue::jobFinished, this, &TransferPanel::sync);
    connect(&q, &JobQueue::jobRemoved, this, &TransferPanel::drop);
    reloadAll();
}

void TransferPanel::reloadAll()
{
    for (const JobRecord& r : m_ctx.queue().records())
        if (isTransferKind(r.kind))
            sync(r.id);
    updateHeader();
}

int TransferPanel::activeCount() const
{
    int n = 0;
    if (m_specimenMode) {
        for (const JobRecord& r : m_specimen)
            n += !isTerminal(r.state);
        return n;
    }
    for (const QString& id : m_order)
        if (const auto r = m_ctx.queue().record(id); r && !isTerminal(r->state))
            ++n;
    return n;
}

TransferRow* TransferPanel::row(int index) const
{
    return index >= 0 && index < int(m_order.size()) ? m_byId.value(m_order[size_t(index)]) : nullptr;
}

void TransferPanel::sync(const QString& id)
{
    if (m_specimenMode)
        return;
    const auto r = m_ctx.queue().record(id);
    if (!r || !isTransferKind(r->kind))
        return;
    TransferRow* row = m_byId.value(id);
    if (!row) {
        row = new TransferRow(m_list);
        if (!m_order.empty())
            m_rows->insertWidget(0, hairline(m_list));
        m_rows->insertWidget(0, row);
        m_byId.insert(id, row);
        m_order.insert(m_order.begin(), id);
        auto& q = m_ctx.queue();
        connect(row, &TransferRow::pauseRequested, &q, &JobQueue::pauseJob);
        connect(row, &TransferRow::resumeRequested, &q, &JobQueue::resumeJob);
        connect(row, &TransferRow::cancelRequested, &q, &JobQueue::cancel);
        connect(row, &TransferRow::retryRequested, this, [this](const QString& jid) {
            if (auto r2 = m_ctx.queue().retry(jid); r2.ok()) {
                m_ctx.queue().remove(jid);
                m_ctx.queue().holdQueue(false);
            }
        });
        connect(row, &TransferRow::removeRequested, &q, [&q](const QString& jid) { q.remove(jid); });
    }
    row->setRecord(*r);
    updateHeader();
}

void TransferPanel::drop(const QString& id)
{
    TransferRow* row = m_byId.take(id);
    if (!row)
        return;
    const int at = m_rows->indexOf(row);
    // The divider after (or before, for the last row) goes with it.
    QLayoutItem* sep = nullptr;
    if (at + 1 < m_rows->count())
        sep = m_rows->takeAt(at + 1);
    else if (at > 0)
        sep = m_rows->takeAt(at - 1);
    if (sep) {
        delete sep->widget();
        delete sep;
    }
    m_rows->removeWidget(row);
    row->deleteLater();
    m_order.erase(std::remove(m_order.begin(), m_order.end(), id), m_order.end());
    updateHeader();
}

void TransferPanel::clearFinished()
{
    if (m_specimenMode)
        return;
    const std::vector<QString> ids = m_order;
    for (const QString& id : ids)
        if (const auto r = m_ctx.queue().record(id); r && isTerminal(r->state))
            m_ctx.queue().remove(id);
}

void TransferPanel::updateHeader()
{
    const int total = int(m_order.size());
    const int active = activeCount();
    int failed = 0;
    if (m_specimenMode) {
        for (const JobRecord& r : m_specimen)
            failed += r.state == JobState::Failed;
    } else {
        for (const QString& id : m_order)
            if (const auto r = m_ctx.queue().record(id); r && r->state == JobState::Failed)
                ++failed;
    }
    m_empty->setVisible(total == 0);
    m_list->setVisible(total > 0);
    m_clear->setVisible(total > active);
    if (total == 0)
        m_summary->set(QStringLiteral("Idle"), Tone::Neutral);
    else if (failed > 0)
        m_summary->set(QStringLiteral("%1 active · %2 failed").arg(active).arg(failed), Tone::Err);
    else if (active > 0)
        m_summary->set(QStringLiteral("%1 active").arg(active), Tone::Info);
    else
        m_summary->set(QStringLiteral("All done"), Tone::Ok);
    if (active > 0 && failed == 0)
        m_summary->setIconName(QStringLiteral("loader"));
}

void TransferPanel::setSpecimen(const std::vector<JobRecord>& records)
{
    // Rows of the real queue are hidden, not destroyed.
    m_specimenMode = true;
    m_specimen = records;
    for (TransferRow* row : m_byId)
        row->hide();
    int i = 0;
    for (const JobRecord& r : records) {
        auto* row = new TransferRow(m_list);
        row->setProperty("fwSpecimen", true);
        row->setRecord(r);
        m_rows->insertWidget(i++, row);
        if (i < int(records.size())) {
            auto* line = hairline(m_list);
            line->setProperty("fwSpecimen", true);
            m_rows->insertWidget(i++, line);
        }
    }
    m_empty->setVisible(records.empty());
    m_list->setVisible(!records.empty());
    const int active = activeCount();
    m_clear->setVisible(int(records.size()) > active);
    int failed = 0;
    for (const JobRecord& r : records)
        failed += r.state == JobState::Failed;
    if (failed > 0)
        m_summary->set(QStringLiteral("%1 active · %2 failed").arg(active).arg(failed), Tone::Err);
    else
        m_summary->set(QStringLiteral("%1 active").arg(active), Tone::Info);
}

void TransferPanel::clearSpecimen()
{
    if (!m_specimenMode)
        return;
    for (QWidget* w : m_list->findChildren<QWidget*>(Qt::FindDirectChildrenOnly))
        if (w->property("fwSpecimen").toBool()) {
            m_rows->removeWidget(w);
            w->deleteLater();
        }
    for (TransferRow* row : m_byId)
        row->show();
    m_specimen.clear();
    m_specimenMode = false;
    updateHeader();
}

} // namespace fw::pages
