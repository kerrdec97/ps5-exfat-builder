#include "app/pages/Page.h"

#include "app/AppContext.h"
#include "app/models/JobQueueModel.h"
#include "app/services/JobSubmit.h"
#include "app/ui/Components.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
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

Page::Page(AppContext& ctx, const QString& key, const QString& title, const QString& caption, QWidget* parent)
    : QWidget(parent), m_ctx(ctx), m_key(key), m_title(title)
{
    qRegisterMetaType<NavRequest>();
    setProperty("fwNoRing", true);
    auto* outer = vbox(this, 0);
    m_scroll = new QScrollArea(this);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setFocusPolicy(Qt::NoFocus);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->setProperty("fwNoRing", true);
    m_scroll->viewport()->setProperty("fwNoRing", true);
    auto* inner = new QWidget(m_scroll);
    inner->setProperty("fwNoRing", true);
    auto* il = vbox(inner, space::lg, QMargins(space::xxl, space::xl, space::xxl, space::xxl));
    m_header = new PageHeader(title, caption, inner);
    il->addWidget(m_header);
    m_content = new QVBoxLayout;
    m_content->setSpacing(space::lg);
    m_content->setContentsMargins(0, 0, 0, 0);
    il->addLayout(m_content);
    il->addStretch(1);
    m_scroll->setWidget(inner);
    outer->addWidget(m_scroll);
}

// ---- JobFormPage ---------------------------------------------------------------------

JobFormPage::JobFormPage(AppContext& ctx, JobQueueModel& queueModel, const QString& key, const QString& title,
                         const QString& caption, const QString& primaryText, const QString& featureName, QWidget* parent)
    : Page(ctx, key, title, caption, parent), m_feature(featureName)
{
    m_submitBar = new QWidget(this);
    auto* sb = hbox(m_submitBar, space::sm);
    m_queue = button(QStringLiteral("Add to queue"), ButtonVariant::Secondary, QStringLiteral("plus"), m_submitBar);
    m_primary = button(primaryText, ButtonVariant::Primary, QStringLiteral("play"), m_submitBar);
    sb->addWidget(m_queue);
    sb->addWidget(m_primary);
    m_newJob = button(QStringLiteral("Start another"), ButtonVariant::Secondary, QStringLiteral("plus"), this);
    m_newJob->setToolTip(QStringLiteral("Back to the form. The job keeps running in the queue."));
    m_newJob->hide();
    header()->addAction(m_newJob);
    header()->addAction(m_submitBar);
    connect(m_queue, &QPushButton::clicked, this, [this] { submitClicked(false); });
    connect(m_primary, &QPushButton::clicked, this, [this] { submitClicked(true); });
    connect(m_newJob, &QPushButton::clicked, this, &JobFormPage::showForm);

    auto* columns = new QWidget(this);
    auto* layout = new TwoColumnLayout(columns);
    auto* left = new QWidget(columns);
    auto* leftLayout = vbox(left, 0);
    // The monitor takes the form's place while a job the page started runs.
    m_monitorHost = new QWidget(left);
    auto* mh = vbox(m_monitorHost, space::lg);
    m_monitor = new JobMonitor(ctx, ctx.logModel(), JobMonitor::Variant::Wide, m_monitorHost);
    m_monitor->setFeatures(JobMonitor::ShowOpenQueue | JobMonitor::ShowNewJob);
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
    auto* right = new QWidget(columns);
    m_aside = vbox(right, space::lg);
    m_dock = new JobsDock(ctx, queueModel, right);
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
    m_newJob->setText(text);
    m_monitor->setNewJobText(text);
}

// The one place that moves between the two views; everything visible follows
// from m_view, so no widget can be left in the other view's state.
void JobFormPage::setView(View view)
{
    const bool monitor = view == View::Monitor;
    m_submitBar->setVisible(!monitor);
    m_newJob->setVisible(monitor);
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
    m_queue->setEnabled(m_available);
    m_primary->setToolTip(m_available ? QString() : QStringLiteral("Not available in this build"));
    m_queue->setToolTip(m_primary->toolTip());
}

void JobFormPage::showFormError(const QString& title, const QString& message)
{
    m_error->setTone(Tone::Err);
    m_error->setTitle(title);
    m_error->setMessage(message);
    m_error->clearLinks();
    m_error->show();
}

void JobFormPage::clearFormError() { m_error->hide(); }

JobOptionsPanel* JobFormPage::addJobOptionsCard(const QString& title, const QStringList& sections,
                                                const QStringList& excludeKeys, const QString& settingsLink,
                                                bool withOpenFolderRow)
{
    // v5: the per-job settings live in a folded "Advanced options" section so a
    // page shows only the decisions most people make; the header says how many
    // settings differ from the defaults for this job.
    auto* card = new Disclosure(QStringLiteral("Advanced options"), title, this);
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
        card->setSummary(n > 0 ? QStringLiteral("%1 changed for this job").arg(n) : QStringLiteral("Defaults"),
                         n > 0 ? Tone::Info : Tone::Neutral);
    };
    m_refreshOptionsSummary = updateSummary;
    connect(m_options, &JobOptionsPanel::overridesChanged, card, updateSummary);
    updateSummary();
    if (withOpenFolderRow) {
        auto* row = new QWidget(card);
        auto* h = hbox(row, space::lg, QMargins(0, 6, 0, 6));
        auto* lab = label(QStringLiteral("Open the output folder when done"), TextStyle::Body, color::fg_1, row);
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

void JobFormPage::submitClicked(bool buildNow)
{
    clearFormError();
    if (!m_available) {
        showFormError(QStringLiteral("%1 is not available in this build").arg(m_feature),
                      QStringLiteral("The engine for this workflow is not part of this build, so nothing was queued."));
        return;
    }
    if (m_options && m_options->hasErrors()) {
        showFormError(QStringLiteral("Check the job settings"), m_options->errorMessages().join(u' '));
        return;
    }
    const QString problem = validateForm();
    if (!problem.isEmpty()) {
        showFormError(QStringLiteral("Not ready to start"), problem);
        return;
    }
    auto r = submit(buildNow);
    if (!r.ok()) {
        // The user's own "Cancel" in a confirmation is not a failure to report.
        if (r.error().isCancelled())
            return;
        showFormError(r.error().code == ErrorCode::Unsupported ? QStringLiteral("Not available in this build")
                                                              : QStringLiteral("The job was not added"),
                      r.error().message);
        return;
    }
    const QString id = r.value();
    auto& q = m_ctx.queue();
    if (buildNow)
        q.startNow(id); // ahead of the other waiting jobs; a paused queue stays paused for them
    emit jobSubmitted(id, openOutputWhenDone());
    if (buildNow && m_monitorOnStart) {
        // The monitor is the feedback: stages, clocks and the live log.
        showMonitor(id);
        return;
    }
    emit toastRequested(Tone::Info, buildNow ? QStringLiteral("Started") : QStringLiteral("Added to the queue"),
                        q.record(id) ? q.record(id)->title : QString());
}

void JobFormPage::setDragHighlight(bool on)
{
    if (m_sourceCard)
        m_sourceCard->setDropHighlight(on);
}

void JobFormPage::applyDemoState(const QString& name)
{
    if (name == QLatin1String("error"))
        showFormError(QStringLiteral("Not enough space on C:"),
                      QStringLiteral("This image needs 48.5 GB; 34.1 GB is free. Free 14.4 GB or choose another output folder."));
}

void JobFormPage::clearDemoState()
{
    clearFormError();
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
        return QStringLiteral("just now");
    if (s < 3600)
        return QStringLiteral("%1 min ago").arg(s / 60);
    if (s < 86400)
        return QStringLiteral("%1 h ago").arg(s / 3600);
    return s < 2 * 86400 ? QStringLiteral("1 day ago") : QStringLiteral("%1 days ago").arg(s / 86400);
}

QString driveLabel(const QString& path)
{
    const QStorageInfo si(path.isEmpty() ? QDir::homePath() : path);
    QString root = si.rootPath();
    if (root.size() >= 2 && root[1] == u':')
        return root.left(2);
    return root.isEmpty() ? QStringLiteral("this drive") : root;
}

void probeFreeSpace(QObject* context, const QString& folder, std::function<void(bool ready, qint64 available, const QString& label)> done)
{
    if (!context)
        return; // nobody to answer
    QPointer<QObject> guard(context);
    (void)QtConcurrent::run([guard, folder, done = std::move(done)] {
        // A folder that is not created yet: the drive it will be on.
        QString path = folder.isEmpty() ? QDir::homePath() : folder;
        while (!QFileInfo::exists(path)) {
            const QString parent = QFileInfo(path).absolutePath();
            if (parent == path || parent.isEmpty())
                break;
            path = parent;
        }
        const QStorageInfo si(path);
        const bool ready = si.isValid() && si.isReady();
        const qint64 available = ready ? si.bytesAvailable() : -1;
        QString label = si.rootPath();
        if (label.size() >= 2 && label[1] == u':')
            label = label.left(2);
        else if (label.isEmpty())
            label = QStringLiteral("this drive");
        // The context is only touched on the UI thread, where the pointer is tested.
        QMetaObject::invokeMethod(QCoreApplication::instance(), [guard, ready, available, label, done] {
            if (guard)
                done(ready, available, label);
        }, Qt::QueuedConnection);
    });
}

} // namespace fw::pages
