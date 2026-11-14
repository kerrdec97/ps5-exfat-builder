#include "app/pages/Page.h"

#include "app/AppContext.h"
#include "app/models/JobQueueModel.h"
#include "app/services/JobSubmit.h"
#include "app/ui/Components.h"
#include "app/ui/Icons.h"
#include "app/ui/Motion.h"
#include "core/FileUtil.h"
#include "core/Tr.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QTextLayout>
#include <QPointer>
#include <QScrollBar>
#include <QShowEvent>
#include <QStorageInfo>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>

namespace fw::pages {

using namespace ui;
using namespace theme;

namespace {

// Header to content is 16 px, as between two cards: this much of it belongs
// to the pinned strip (so its hairline has air above it), the rest to the
// scrolling content.
constexpr int kHeaderGap = space::sm;

// The strip that holds the page header above the scroll area. At the top of
// the page it is the page itself; once the content is scrolled it shows a
// hairline, so what passes under it visibly goes somewhere.
class HeaderBar : public QWidget {
public:
    HeaderBar(const QString& title, const QString& caption, QWidget* parent) : QWidget(parent)
    {
        setProperty("fwNoRing", true);
        m_layout = vbox(this, 0, QMargins(space::xxl, space::xl, space::xxl, kHeaderGap));
        m_header = new PageHeader(title, caption, this);
        m_layout->addWidget(m_header);
        // A page may hide its header (Home: the hero is its header).
        m_header->installEventFilter(this);
    }
    PageHeader* header() const { return m_header; }
    // The scroll area under the strip and the layout of its content.
    void follow(QScrollArea* scroll, QVBoxLayout* content)
    {
        m_scroll = scroll;
        m_content = content;
        connect(scroll->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int value) { setLined(value > 0); });
        scroll->viewport()->installEventFilter(this); // the scroll bar comes and goes
        sync();
    }
    void setLined(bool on)
    {
        if (on == m_lined)
            return;
        m_lined = on;
        update();
    }
    bool isLined() const { return m_lined; }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.fillRect(rect(), color::bg_1);
        if (m_lined)
            p.fillRect(QRect(0, height() - 1, width(), 1), color::border_2);
    }
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (m_scroll
            && ((watched == m_header && (event->type() == QEvent::HideToParent || event->type() == QEvent::ShowToParent))
                || (watched == m_scroll->viewport() && event->type() == QEvent::Resize)))
            sync();
        return QWidget::eventFilter(watched, event);
    }

private:
    // Keeps the strip in step with the content: its right edge leaves the
    // room the scroll bar takes from the content (header buttons and cards
    // end on one line with or without a scroll bar), and without a header the
    // content starts where the header would have.
    void sync()
    {
        const bool shown = !m_header->isHidden();
        if (isHidden() == shown)
            setVisible(shown);
        QMargins inner = m_content->contentsMargins();
        inner.setTop(shown ? space::lg - kHeaderGap : space::xl);
        if (inner != m_content->contentsMargins())
            m_content->setContentsMargins(inner);
        QMargins head = m_layout->contentsMargins();
        head.setRight(space::xxl + std::max(0, m_scroll->width() - m_scroll->viewport()->width()));
        if (head != m_layout->contentsMargins())
            m_layout->setContentsMargins(head);
    }
    QVBoxLayout* m_layout;
    PageHeader* m_header;
    QScrollArea* m_scroll = nullptr;
    QVBoxLayout* m_content = nullptr;
    bool m_lined = false;
};

// The dock column of a form page. The two-column layout places this slot,
// which is as big as the column's content; the content itself (body) is a
// sibling placed over the slot and slid down inside the form's height while
// the page scrolls: the jobs stay in view beside a long form, and never go
// past the form's end. Wrapped under the form (narrow window) it stays put.
class AsideSlot : public QWidget {
public:
    AsideSlot(QWidget* columns, TwoColumnLayout* layout, QScrollArea* scroll)
        : QWidget(columns), m_layout(layout), m_scroll(scroll)
    {
        setProperty("fwNoRing", true);
        QSizePolicy sp(QSizePolicy::Preferred, QSizePolicy::Preferred);
        sp.setHeightForWidth(true);
        setSizePolicy(sp);
        m_body = new QWidget(columns);
        m_body->setProperty("fwNoRing", true);
        m_body->installEventFilter(this);       // its content grows or shrinks
        columns->installEventFilter(this);      // the form grows or shrinks
        scroll->viewport()->installEventFilter(this); // the window is resized
        connect(scroll->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] { place(); });
    }
    QWidget* body() const { return m_body; }
    // How far the body is slid down from the slot (tests, screenshots).
    int offset() const { return m_body->y() - y(); }

    QSize sizeHint() const override { return {m_body->sizeHint().width(), bodyHeight(-1)}; }
    QSize minimumSizeHint() const override { return m_body->minimumSizeHint(); }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int w) const override { return bodyHeight(w); }

protected:
    bool event(QEvent* e) override
    {
        // Shown and hidden with the slot (not with the page: the body is in it too).
        if (e->type() == QEvent::ShowToParent || e->type() == QEvent::HideToParent)
            m_body->setVisible(e->type() == QEvent::ShowToParent);
        return QWidget::event(e);
    }
    void moveEvent(QMoveEvent* e) override
    {
        QWidget::moveEvent(e);
        place();
    }
    void resizeEvent(QResizeEvent* e) override
    {
        QWidget::resizeEvent(e);
        place();
    }
    bool eventFilter(QObject* watched, QEvent* e) override
    {
        if (watched == m_body && e->type() == QEvent::LayoutRequest) {
            // The body's layout has just been brought up to date: the slot
            // takes the new height, and the body its new place.
            updateGeometry();
            place();
        } else if ((watched == parentWidget() || watched == m_scroll->viewport()) && e->type() == QEvent::Resize) {
            place();
        }
        return QWidget::eventFilter(watched, e);
    }

private:
    int bodyHeight(int w) const
    {
        int h = w > 0 && m_body->hasHeightForWidth() ? m_body->heightForWidth(w) : -1;
        if (h < 0)
            h = m_body->sizeHint().height();
        return std::max(h, m_body->minimumSizeHint().height());
    }
    void place()
    {
        if (!m_scroll->widget() || !m_scroll->widget()->isAncestorOf(this))
            return; // not in the page's content yet
        const QRect slot = geometry();
        const int h = bodyHeight(slot.width());
        int offset = 0;
        if (!m_layout->isStacked()) {
            // Where the column starts in the scrolled content, how far the
            // content is scrolled and how much of it the window shows.
            const int top = mapTo(m_scroll->widget(), QPoint(0, 0)).y();
            const int scrolled = m_scroll->verticalScrollBar()->value();
            const int view = m_scroll->viewport()->height();
            // At rest the column starts `top` below the view's edge. It keeps
            // that place while the form scrolls; a column taller than the view
            // scrolls with the form until its end is in view, then stays.
            const int overflow = std::max(0, top + h + space::lg - view);
            offset = std::max(0, scrolled - overflow);
            offset = std::min(offset, std::max(0, parentWidget()->height() - slot.top() - h)); // never past the form's end
        }
        const QRect want(slot.x(), slot.y() + offset, slot.width(), h);
        if (m_body->geometry() != want)
            m_body->setGeometry(want);
    }
    TwoColumnLayout* m_layout;
    QScrollArea* m_scroll;
    QWidget* m_body;
};

} // namespace

Page::Page(AppContext& ctx, const QString& key, const QString& title, const QString& caption, QWidget* parent)
    : QWidget(parent), m_ctx(ctx), m_key(key), m_title(title)
{
    qRegisterMetaType<NavRequest>();
    setProperty("fwNoRing", true);
    auto* outer = vbox(this, 0);
    // The header is pinned: it sits above the scroll area, not in it.
    auto* bar = new HeaderBar(title, caption, this);
    m_headerBar = bar;
    m_header = bar->header();
    outer->addWidget(bar);
    m_scroll = new QScrollArea(this);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setFocusPolicy(Qt::NoFocus);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->setProperty("fwNoRing", true);
    m_scroll->viewport()->setProperty("fwNoRing", true);
    auto* inner = new QWidget(m_scroll);
    inner->setProperty("fwNoRing", true);
    auto* il = vbox(inner, space::lg, QMargins(space::xxl, space::lg - kHeaderGap, space::xxl, space::xxl));
    m_content = new QVBoxLayout;
    m_content->setSpacing(space::lg);
    m_content->setContentsMargins(0, 0, 0, 0);
    il->addLayout(m_content);
    il->addStretch(1);
    m_scroll->setWidget(inner);
    outer->addWidget(m_scroll, 1);
    bar->follow(m_scroll, il);
}

bool Page::isHeaderLined() const { return static_cast<const HeaderBar*>(m_headerBar)->isLined(); }

// ---- JobFormPage ---------------------------------------------------------------------

JobFormPage::JobFormPage(AppContext& ctx, JobQueueModel& queueModel, const QString& key, const QString& title,
                         const QString& caption, const QString& primaryText, const QString& featureName, QWidget* parent)
    : Page(ctx, key, title, caption, parent), m_feature(featureName)
{
    m_submitBar = new QWidget(this);
    auto* sb = hbox(m_submitBar, space::sm);
    m_queue = button(FW_TR("Add to queue"), ButtonVariant::Secondary, QStringLiteral("plus"), m_submitBar);
    m_primary = button(primaryText, ButtonVariant::Primary, QStringLiteral("play"), m_submitBar);
    sb->addWidget(m_queue);
    sb->addWidget(m_primary);
    // While the monitor shows, the header keeps nothing of the form: the way
    // back to it ("New build") is the monitor's own footer button, once.
    header()->addAction(m_submitBar);
    connect(m_queue, &QPushButton::clicked, this, [this] { submitClicked(false); });
    connect(m_primary, &QPushButton::clicked, this, [this] { submitClicked(true); });

    auto* columns = new QWidget(this);
    auto* layout = new TwoColumnLayout(columns);
    auto* left = new QWidget(columns);
    auto* leftLayout = vbox(left, 0);
    // The monitor takes the form's place while a job the page started runs.
    m_monitorHost = new QWidget(left);
    auto* mh = vbox(m_monitorHost, space::lg);
    m_monitor = new JobMonitor(ctx, ctx.logModel(), JobMonitor::Variant::Wide, m_monitorHost);
    m_monitor->setFeatures(JobMonitor::ShowOpenQueue | JobMonitor::ShowNewJob);
    m_monitor->newJobButton()->setToolTip(FW_TR("Back to the form. The job keeps running in the queue."));
    mh->addWidget(m_monitor);
    m_monitorHost->hide();
    leftLayout->addWidget(m_monitorHost);
    m_formWidget = new QWidget(left);
    m_form = vbox(m_formWidget, space::lg);
    m_error = new InfoBar(Tone::Err, m_formWidget);
    m_error->setDismissable(true);
    m_error->hide();
    m_form->addWidget(m_error);
    leftLayout->addWidget(m_formWidget);
    auto* right = new AsideSlot(columns, layout, scrollArea());
    m_asideBody = right->body();
    m_aside = vbox(m_asideBody, space::lg);
    m_dock = new JobsDock(ctx, queueModel, m_asideBody);
    m_aside->addWidget(m_dock);
    m_aside->addStretch(1);
    layout->setLeft(left);
    layout->setRight(right);
    content()->addWidget(columns);
    connect(m_dock, &JobsDock::openQueueRequested, this, [this] { emit navigateRequested({QStringLiteral("queue"), {}}); });
    connect(m_dock, &JobsDock::openJobRequested, this, [this](const QString& id) {
        emit navigateRequested({QStringLiteral("queue"), {{QStringLiteral("jobId"), id}}});
    });
    connect(m_dock, &JobsDock::cancelRequested, this, [this](const QString& id) { jobs::cancelWithConfirm(m_ctx, this, id); });
    connect(m_dock, &JobsDock::watchRequested, this, &JobFormPage::showMonitor);
    connect(m_monitor, &JobMonitor::cancelRequested, this, [this](const QString& id) { jobs::cancelWithConfirm(m_ctx, this, id); });
    connect(m_monitor, &JobMonitor::openLogsRequested, this, [this](const QString& id) {
        emit navigateRequested({QStringLiteral("logs"), {{QStringLiteral("jobId"), id}}});
    });
    connect(m_monitor, &JobMonitor::openQueueRequested, this, [this](const QString& id) {
        emit navigateRequested({QStringLiteral("queue"), {{QStringLiteral("jobId"), id}}});
    });
    connect(m_monitor, &JobMonitor::newJobRequested, this, &JobFormPage::showForm);
    connect(m_monitor, &JobMonitor::jobGone, this, &JobFormPage::showForm);
    connect(m_monitor, &JobMonitor::jobSwitched, this, [this](const QString& id) { m_dock->setWatchedJob(id); });
}

void JobFormPage::setNewJobText(const QString& text)
{
    m_monitor->setNewJobText(text);
}

// The one place that moves between the two views; everything visible follows
// from m_view, so no widget can be left in the other view's state.
void JobFormPage::setView(View view)
{
    const bool monitor = view == View::Monitor;
    m_submitBar->setVisible(!monitor);
    m_formWidget->setVisible(!monitor);
    m_monitorHost->setVisible(monitor);
    if (!monitor)
        m_monitor->setJobId({});
    scrollArea()->verticalScrollBar()->setValue(0);
    if (m_view != view) {
        m_view = view;
        emit monitorShownChanged(monitor);
    }
}

void JobFormPage::showMonitor(const QString& id)
{
    if (id.isEmpty())
        return;
    clearFormError();
    m_monitor->setJobId(id);
    m_dock->setWatchedJob(id);
    setView(View::Monitor);
}

void JobFormPage::showForm()
{
    m_dock->setWatchedJob({});
    if (m_view == View::Form)
        return;
    setView(View::Form);
}

void JobFormPage::showEvent(QShowEvent* e)
{
    Page::showEvent(e);
    // Coming back long after the watched job ended: the user wants the form.
    if (m_view == View::Monitor) {
        auto rec = m_ctx.queue().record(m_monitor->jobId());
        if (!rec || (isTerminal(rec->state) && rec->finished.isValid() && rec->finished.secsTo(QDateTime::currentDateTime()) > 600))
            showForm();
    }
}

void JobFormPage::setRequiredKinds(const QStringList& kinds)
{
    m_kinds = kinds;
    refreshAvailability();
}

void JobFormPage::setFeatureName(const QString& name)
{
    if (name == m_feature)
        return;
    m_feature = name;
    // The state card carries the name: it is made again when it is next needed.
    if (m_unavailableCard) {
        m_unavailableCard->hide();
        m_unavailableCard->deleteLater();
        m_unavailableCard = nullptr;
        m_unavailable = nullptr;
    }
}

void JobFormPage::refreshAvailability()
{
    const QStringList missing = jobs::missingKinds(m_ctx, m_kinds);
    m_available = missing.isEmpty();
    if (!m_available) {
        if (!m_unavailableCard) {
            m_unavailableCard = new Card(this);
            m_unavailableCard->body()->setContentsMargins(0, 0, 0, 0);
            m_unavailable = new UnavailableState(m_feature, missing.first(), m_unavailableCard);
            m_unavailableCard->body()->addWidget(m_unavailable);
            m_form->insertWidget(1, m_unavailableCard);
        }
        m_unavailable->setKinds(missing);
        m_unavailableCard->show();
    } else if (m_unavailableCard) {
        m_unavailableCard->hide();
    }
    m_primary->setEnabled(m_available);
    m_queue->setEnabled(m_available && m_queueBlocked.isEmpty());
    applyStartTip();
}

void JobFormPage::setStartHint(const QString& hint)
{
    if (hint == m_startHint)
        return;
    m_startHint = hint;
    applyStartTip();
}

void JobFormPage::setQueueBlocked(const QString& why)
{
    if (why == m_queueBlocked)
        return;
    m_queueBlocked = why;
    m_queue->setEnabled(m_available && m_queueBlocked.isEmpty());
    applyStartTip();
}

// The two buttons say why a click would not start anything; the click itself
// still explains it in the form (the buttons stay enabled for that).
void JobFormPage::applyStartTip()
{
    m_primary->setToolTip(m_available ? m_startHint : FW_TR("Not available in this build"));
    m_queue->setToolTip(m_available && !m_queueBlocked.isEmpty() ? m_queueBlocked : m_primary->toolTip());
}

void JobFormPage::showFormError(const QString& title, const QString& message)
{
    m_error->setTone(Tone::Err);
    m_error->setTitle(title);
    m_error->setMessage(message);
    m_error->clearLinks();
    m_error->show();
    // The start buttons are pinned and can be pressed from anywhere in the
    // form: the answer is at the form's top, so that is brought into view.
    scrollArea()->verticalScrollBar()->setValue(0);
}

void JobFormPage::clearFormError() { m_error->hide(); }

JobOptionsPanel* JobFormPage::addJobOptionsCard(const QString& title, const QStringList& sections,
                                                const QStringList& excludeKeys, const QString& settingsLink,
                                                bool withOpenFolderRow)
{
    // v5: the per-job settings live in a folded "Advanced options" section so a
    // page shows only the decisions most people make; the header says how many
    // settings differ from the defaults for this job.
    auto* card = new Disclosure(FW_TR("Advanced options"), title, this);
    card->body()->setSpacing(0);
    m_options = new JobOptionsPanel(m_ctx, sections, excludeKeys, card);
    m_options->setGroupCards(false);
    m_options->build();
    card->body()->addWidget(m_options);
    auto updateSummary = [card, this] {
        // Rows the page hides (they do not apply to the current action) are not counted.
        int n = 0;
        const QVariantMap changed = m_options->overrides();
        for (auto it = changed.cbegin(); it != changed.cend(); ++it)
            if (SettingRow* row = m_options->row(it.key()); row && !row->isHidden())
                ++n;
        card->setSummary(n > 0 ? FW_TR("%1 changed for this job").arg(n) : FW_TR("Defaults"),
                         n > 0 ? Tone::Info : Tone::Neutral);
    };
    m_refreshOptionsSummary = updateSummary;
    connect(m_options, &JobOptionsPanel::overridesChanged, card, updateSummary);
    updateSummary();
    if (withOpenFolderRow) {
        auto* row = new QWidget(card);
        auto* h = hbox(row, space::lg, QMargins(0, 6, 0, 6));
        auto* lab = label(FW_TR("Open the output folder when done"), TextStyle::Body, color::fg_1, row);
        h->addWidget(lab, 1);
        m_openFolder = new Switch(row);
        m_openFolder->setAccessibleName(lab->text());
        h->addWidget(m_openFolder);
        if (m_options->rowCount() > 0) {
            auto* line = hairline(card);
            card->body()->addWidget(line);
        }
        card->body()->addWidget(row);
    }
    if (!settingsLink.isEmpty()) {
        auto* lrow = new QWidget(card);
        auto* lh = hbox(lrow, 6, QMargins(0, space::md, 0, 0));
        lh->addStretch(1);
        auto* link = new Link(settingsLink, lrow);
        const QString section = sections.isEmpty() ? QString() : sections.first();
        connect(link, &Link::activated, this, [this, section] {
            emit navigateRequested({QStringLiteral("settings"), {{QStringLiteral("section"), section}}});
        });
        lh->addWidget(link);
        card->body()->addWidget(lrow);
    }
    m_form->addWidget(card);
    return m_options;
}

bool JobFormPage::openOutputWhenDone() const { return m_openFolder && m_openFolder->isChecked(); }

namespace {
// None of the user's jobs runs or waits (update checks do not count).
bool queueHasNothingToDo(const JobQueue& q)
{
    for (const JobRecord& r : q.records())
        if (!jobs::isSystemKind(r.kind) && (r.isActive() || r.state == JobState::Queued))
            return false;
    return true;
}
} // namespace

void JobFormPage::submitClicked(bool buildNow)
{
    if (!buildNow && !m_queueBlocked.isEmpty())
        return; // this action is not put on the list
    clearFormError();
    if (!m_available) {
        showFormError(FW_TR("%1 is not available in this build").arg(m_feature),
                      FW_TR("The engine for this workflow is not part of this build, so nothing was queued."));
        return;
    }
    if (m_options) {
        // A row the page hid does not apply to this job and is not sent with
        // it: its error cannot be corrected here and must not block the job.
        QStringList problems;
        for (const QString& key : m_options->errorKeys())
            if (const SettingRow* row = m_options->row(key); !row || !row->isHidden()) {
                // "Label: reason", as shown: the schema's English is translated here.
                const SettingDef* def = m_ctx.schema().find(key);
                const QString reason = i18n::translate(m_options->errorMessage(key).section(QStringLiteral(": "), 1, -1));
                problems << (def && !reason.isEmpty() ? QStringLiteral("%1: %2").arg(shownLabel(*def), reason) : m_options->errorMessage(key));
            }
        if (!problems.isEmpty()) {
            showFormError(FW_TR("Check the job settings"), problems.join(u' '));
            return;
        }
    }
    const QString problem = validateForm();
    if (!problem.isEmpty()) {
        showFormError(FW_TR("Not ready to start"), problem);
        return;
    }
    auto& q = m_ctx.queue();
    // "Add to queue" puts the job on a list: a queue with nothing to do is
    // paused first (the queue starts whatever waits the moment it is added),
    // so the job waits for "Start queue". A queue that is working is left
    // alone and the job joins the line.
    const bool startsList = !buildNow && !q.isHeld() && queueHasNothingToDo(q);
    if (startsList)
        q.holdQueue(true);
    auto r = submit(buildNow);
    if (!r.ok()) {
        if (startsList && queueHasNothingToDo(q))
            q.holdQueue(false); // nothing was added: as it was
        // The user's own "Cancel" in a confirmation is not a failure to report.
        if (r.error().isCancelled())
            return;
        showFormError(r.error().code == ErrorCode::Unsupported ? FW_TR("Not available in this build")
                                                              : FW_TR("The job was not added"),
                      r.error().message);
        return;
    }
    const QString id = r.value();
    if (buildNow)
        q.startNow(id); // ahead of the other waiting jobs; a paused queue stays paused for them
    emit jobSubmitted(id, openOutputWhenDone());
    if (buildNow && m_monitorOnStart) {
        // The monitor is the feedback: stages, clocks and the live log.
        showMonitor(id);
        return;
    }
    if (!buildNow && q.isHeld()) {
        // The job waits: say how the list is started, and offer it.
        emit toastActionRequested(Tone::Info, FW_TR("Added to the queue"),
                                  FW_TR("The queue is waiting. Add more, then press Start queue."),
                                  FW_TR("Start queue"), [ctx = &m_ctx] { ctx->queue().holdQueue(false); });
        return;
    }
    emit toastRequested(Tone::Info, buildNow ? FW_TR("Started") : FW_TR("Added to the queue"),
                        q.record(id) ? q.record(id)->title : QString());
}

void JobFormPage::setDragHighlight(bool on)
{
    if (m_sourceCard)
        m_sourceCard->setDropHighlight(on);
}

void JobFormPage::applyDemoState(const QString& name)
{
    if (name == QLatin1String("error")) // the live texts (Build › space check), so a render in any language reads as the app
        showFormError(FW_TR("Not enough space on %1").arg(QStringLiteral("C:")),
                      FW_TR("This image needs %1; %2 is free. Free %3 or choose another output folder.")
                          .arg(formatBytes(48500000000LL), formatBytes(34100000000LL), formatBytes(14400000000LL)));
}

void JobFormPage::clearDemoState()
{
    clearFormError();
    m_dock->setDemoRunning(JobRecord{});
    showForm();
}

// ---- helpers ------------------------------------------------------------------------

QString humanTime(const QDateTime& t)
{
    return t.isValid() ? t.toString(QStringLiteral("yyyy-MM-dd HH:mm")) : QString();
}

QString relativeTime(const QDateTime& t)
{
    if (!t.isValid())
        return {};
    const qint64 s = t.secsTo(QDateTime::currentDateTime());
    if (s < 60)
        return FW_TR("just now");
    if (s < 3600)
        return FW_TR("%1 min ago").arg(s / 60);
    if (s < 86400)
        return FW_TR("%1 h ago").arg(s / 3600);
    return s < 2 * 86400 ? FW_TR("1 day ago") : FW_TR("%1 days ago").arg(s / 86400);
}

QString driveLabel(const QString& path)
{
    const QStorageInfo si(path.isEmpty() ? QDir::homePath() : path);
    QString root = si.rootPath();
    if (root.size() >= 2 && root[1] == u':')
        return root.left(2);
    return root.isEmpty() ? FW_TR("this drive") : root;
}

DriveSpace driveSpaceOf(const QString& folder)
{
    // A folder that is not created yet: the drive it will be on.
    QString path = folder.isEmpty() ? QDir::homePath() : folder;
    while (!QFileInfo::exists(path)) {
        const QString parent = QFileInfo(path).absolutePath();
        if (parent == path || parent.isEmpty())
            break;
        path = parent;
    }
    const QStorageInfo si(path);
    DriveSpace d;
    d.ready = si.isValid() && si.isReady();
    d.available = d.ready ? si.bytesAvailable() : -1;
    d.label = si.rootPath();
    if (d.label.size() >= 2 && d.label[1] == u':')
        d.label = d.label.left(2);
    else if (d.label.isEmpty())
        d.label = FW_TR("this drive");
    return d;
}

void probeFreeSpace(QObject* context, const QString& folder, std::function<void(bool ready, qint64 available, const QString& label)> done)
{
    if (!context)
        return; // nobody to answer
    QPointer<QObject> guard(context);
    (void)QtConcurrent::run([guard, folder, done = std::move(done)] {
        const DriveSpace d = driveSpaceOf(folder);
        // The context is only touched on the UI thread, where the pointer is tested.
        QMetaObject::invokeMethod(QCoreApplication::instance(), [guard, d, done] {
            if (guard)
                done(d.ready, d.available, d.label);
        }, Qt::QueuedConnection);
    });
}

void outlineIconButton(IconButton* button)
{
    if (!button)
        return;
    // The secondary button's plate and states: the application style sheet's
    // QToolButton[outline="true"] rule (ui/Theme.cpp); the size stays the
    // icon button's (32, 28 or 24 px square).
    button->setProperty("outline", true);
    repolish(button);
}

// ---- option tiles -------------------------------------------------------------------

namespace {

constexpr int kTileGap = 12;
constexpr int kTileH = 132;
constexpr int kTileMinRow = 150; // narrowest tile in one full row
constexpr int kTileMin3 = 170;   // three per row
constexpr int kTileMin2 = 190;   // two per row
constexpr int kTileLines = 3;    // description lines at most (the rest is cut with "…", whole on hover)

QStringList wrapText(const QString& text, const QFont& font, int width, int maxLines)
{
    QStringList out;
    if (text.isEmpty() || width <= 10)
        return out;
    QTextLayout layout(text, font);
    layout.beginLayout();
    while (out.size() < maxLines) {
        QTextLine line = layout.createLine();
        if (!line.isValid())
            break;
        line.setLineWidth(width);
        const int start = line.textStart();
        const int len = line.textLength();
        if (out.size() == maxLines - 1 && start + len < text.size()) {
            out << QFontMetrics(font).elidedText(text.mid(start), Qt::ElideRight, width);
            break;
        }
        out << text.mid(start, len).trimmed();
    }
    layout.endLayout();
    return out;
}

// Whether `text` takes at most `maxLines` lines of `width` (wrapText() cuts the rest with "…").
bool fitsLines(const QString& text, const QFont& font, int width, int maxLines)
{
    if (text.isEmpty())
        return true;
    if (width <= 10)
        return false;
    QTextLayout layout(text, font);
    layout.beginLayout();
    int lines = 0;
    for (QTextLine line = layout.createLine(); line.isValid(); line = layout.createLine()) {
        line.setLineWidth(width);
        ++lines;
    }
    layout.endLayout();
    return lines <= maxLines;
}

} // namespace

OptionTiles::OptionTiles(QWidget* parent) : QWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_Hover);
    setMouseTracking(true);
    QSizePolicy sp(QSizePolicy::Expanding, QSizePolicy::Preferred);
    sp.setHeightForWidth(true);
    setSizePolicy(sp);
    setProperty("fwRadius", radius::md);
    m_hover = new motion::HoverFade(this, [this](int i) {
        if (i >= 0 && i < count())
            update(tileRect(i));
    });
    m_glow = new SelectionGlow(this);
}

void OptionTiles::addTile(const QString& title, const QString& description, const QString& icon, const QString& value,
                          const QString& tag)
{
    m_items.push_back({title, description, icon, tag, value});
    if (m_current < 0) {
        m_current = 0;
        setAccessibleDescription(FW_TR("Selected: %1").arg(title));
    }
    m_hover->reset();
    updateGeometry();
    update();
    syncGlow();
}

QString OptionTiles::currentValue() const { return m_current >= 0 ? m_items[size_t(m_current)].value : QString(); }

QString OptionTiles::titleAt(int i) const { return i >= 0 && i < count() ? m_items[size_t(i)].title : QString(); }

void OptionTiles::setCurrentValue(const QString& v)
{
    for (int i = 0; i < count(); ++i)
        if (m_items[size_t(i)].value == v)
            setCurrentIndex(i);
}

void OptionTiles::setCurrentIndex(int i)
{
    if (i < 0 || i >= count() || i == m_current)
        return;
    m_current = i;
    setAccessibleDescription(FW_TR("Selected: %1").arg(m_items[size_t(i)].title)); // the tiles are painted, not widgets
    update();
    syncGlow();
    if (onChanged)
        onChanged(i);
}

int OptionTiles::heightForWidth(int w) const
{
    const int cols = columns(w);
    const int rows = (count() + cols - 1) / cols;
    return rows * kTileH + std::max(0, rows - 1) * kTileGap;
}

QSize OptionTiles::sizeHint() const
{
    const int w = std::min(count(), 3) * (kTileMin3 + 40);
    return {w, heightForWidth(w)};
}

QSize OptionTiles::minimumSizeHint() const { return {kTileMin2, kTileH}; }

int OptionTiles::columns(int w) const
{
    if (count() <= 1)
        return 1;
    if (w >= count() * kTileMinRow + (count() - 1) * kTileGap)
        return count();
    if (count() >= 5 && w >= 3 * kTileMin3 + 2 * kTileGap)
        return 3;
    return w >= 2 * kTileMin2 + kTileGap ? 2 : 1;
}

QRect OptionTiles::tileRect(int i) const
{
    const int cols = columns(width());
    const int row = i / cols, col = i % cols;
    // A last row with fewer tiles shares the whole width between them.
    const int inRow = std::min(cols, count() - row * cols);
    const int w = (width() - (inRow - 1) * kTileGap) / inRow;
    const int x = col == inRow - 1 ? width() - w : col * (w + kTileGap); // the last one ends at the edge
    return mirrored(this, QRect(x, row * (kTileH + kTileGap), w, kTileH)); // RTL: the first at the right
}

int OptionTiles::at(const QPoint& pt) const
{
    for (int i = 0; i < count(); ++i)
        if (tileRect(i).contains(pt))
            return i;
    return -1;
}

// The selected tile's soft glow lies outside the tile: its own widget draws it.
void OptionTiles::syncGlow()
{
    m_glow->setItemRect(m_current >= 0 ? QRectF(tileRect(m_current)).adjusted(0.5, 0.5, -0.5, -0.5) : QRectF(), radius::md);
}

void OptionTiles::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const qreal dpr = devicePixelRatioF();
    for (int i = 0; i < count(); ++i) {
        const Item& it = m_items[size_t(i)];
        const QRectF tileR = QRectF(tileRect(i)).adjusted(0.5, 0.5, -0.5, -0.5);
        // The inside is worked out left to right and mirrored as it is drawn.
        const QRectF r = mirrored(this, tileR);
        const auto vis = [this](const QRectF& x) { return mirrored(this, x); };
        const bool sel = i == m_current;
        const qreal hov = sel ? 0.0 : m_hover->amount(i); // the hover tint fades
        if (sel) {
            p.setPen(Qt::NoPen);
            p.setBrush(color::bg_2);
            p.drawRoundedRect(tileR, radius::md, radius::md);
            p.setBrush(accentGradient(tileR, 34));
            p.drawRoundedRect(tileR, radius::md, radius::md);
            p.setPen(QPen(QBrush(accentGradient(tileR, 230)), 1.5));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(tileR.adjusted(0.5, 0.5, -0.5, -0.5), radius::md, radius::md);
        } else {
            // A well in the card that lifts on hover.
            p.setPen(QPen(blend(color::border_3, color::border_4, hov), 1));
            p.setBrush(blend(color::field_bg, color::bg_3, hov));
            p.drawRoundedRect(tileR, radius::md, radius::md);
        }
        // Icon square, top left (white only on the fill gradient).
        const QRectF ir(r.left() + 14, r.top() + 14, 32, 32);
        p.setPen(Qt::NoPen);
        p.setBrush(sel ? QBrush(accentFillGradient(vis(ir))) : QBrush(color::bg_4));
        p.drawRoundedRect(vis(ir), 9, 9);
        p.drawPixmap(vis(QRectF(ir.center().x() - 9, ir.center().y() - 9, 18, 18)).toRect(),
                     iconPixmap(it.icon, 18, sel ? color::on_accent : color::fg_2, dpr));
        // Check bubble and tag, top right, on the icon's centre line.
        qreal right = r.right() - 14;
        if (sel) {
            paintCheckBubble(p, vis(QRectF(right - 20, ir.center().y() - 10, 20, 20)), dpr);
            right -= 28;
        }
        if (!it.tag.isEmpty()) {
            const qreal tw = badgeWidth(it.tag);
            if (right - tw > ir.right() + 8)
                paintBadge(p, vis(QRectF(right - tw, ir.center().y() - size::badge / 2.0, tw, size::badge)), it.tag, Tone::Info);
        }
        // Title and the description under it. Right to left, a Latin name that
        // ends in a sign keeps it at its end ("ShadowMount+", ui::bidiSafe).
        const bool rtl = isRightToLeft();
        const qreal tx = r.left() + 14;
        const qreal tw = r.width() - 28;
        p.setFont(font::bodyStrong());
        p.setPen(color::fg_0);
        const QRectF titleR(tx, ir.bottom() + 10, tw, 20);
        const QString title = QFontMetrics(font::bodyStrong()).elidedText(it.title, Qt::ElideRight, int(tw));
        p.drawText(vis(titleR), Qt::AlignLeft | Qt::AlignVCenter, rtl ? bidiSafe(title) : title);
        p.setFont(font::caption());
        p.setPen(color::fg_3);
        qreal y = titleR.bottom() + 2;
        for (const QString& l : wrapText(it.description, font::caption(), int(tw), kTileLines)) {
            p.drawText(vis(QRectF(tx, y, tw, 16)), Qt::AlignLeft | Qt::AlignVCenter, rtl ? bidiSafe(l) : l);
            y += 16;
        }
        if (hasFocus() && sel) {
            p.setPen(QPen(color::focus, 2));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(tileR.adjusted(-3, -3, 3, 3), radius::md + 3, radius::md + 3);
        }
    }
}

void OptionTiles::mousePressEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton)
        return;
    const int i = at(e->pos());
    if (i >= 0)
        setCurrentIndex(i);
}

void OptionTiles::mouseMoveEvent(QMouseEvent* e)
{
    const int i = at(e->pos());
    if (i != m_hover->hovered()) {
        m_hover->setHovered(i);
        setCursor(i >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
        // A description cut with "…" in a narrow tile can be read in full on hover.
        const bool cut = i >= 0 && !fitsLines(m_items[size_t(i)].description, font::caption(), tileRect(i).width() - 29, kTileLines);
        setToolTip(cut ? m_items[size_t(i)].description : QString());
    }
}

void OptionTiles::leaveEvent(QEvent*) { m_hover->setHovered(-1); }

void OptionTiles::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    syncGlow();
}

void OptionTiles::keyPressEvent(QKeyEvent* e)
{
    const int cols = columns(width());
    int next = m_current;
    switch (e->key()) {
    case Qt::Key_Left: next = isRightToLeft() ? m_current + 1 : m_current - 1; break; // as on screen
    case Qt::Key_Right: next = isRightToLeft() ? m_current - 1 : m_current + 1; break;
    case Qt::Key_Up: next = m_current - cols; break;
    case Qt::Key_Down: // into a shorter last row: its last tile
        next = m_current / cols < (count() - 1) / cols ? std::min(m_current + cols, count() - 1) : count();
        break;
    default: QWidget::keyPressEvent(e); return;
    }
    if (next >= 0 && next < count())
        setCurrentIndex(next);
}

void OptionTiles::focusInEvent(QFocusEvent* e)
{
    QWidget::focusInEvent(e);
    update();
}

void OptionTiles::focusOutEvent(QFocusEvent* e)
{
    QWidget::focusOutEvent(e);
    update();
}

} // namespace fw::pages
