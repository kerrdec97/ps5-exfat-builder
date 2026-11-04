#include "app/pages/Page.h"

#include "app/AppContext.h"
#include "app/models/JobQueueModel.h"
#include "app/services/JobSubmit.h"

#include <QDateTime>
#include <QDir>
#include <QScrollBar>
#include <QStorageInfo>
#include <QVBoxLayout>

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
    m_queue = button(QStringLiteral("Add to queue"), ButtonVariant::Secondary, QStringLiteral("plus"), this);
    m_primary = button(primaryText, ButtonVariant::Primary, QStringLiteral("play"), this);
    header()->addAction(m_queue);
    header()->addAction(m_primary);
    connect(m_queue, &QPushButton::clicked, this, [this] { submitClicked(false); });
    connect(m_primary, &QPushButton::clicked, this, [this] { submitClicked(true); });

    auto* columns = new QWidget(this);
    auto* layout = new TwoColumnLayout(columns);
    auto* left = new QWidget(columns);
    m_form = vbox(left, space::lg);
    m_error = new InfoBar(Tone::Err, left);
    m_error->setDismissable(true);
    m_error->hide();
    m_form->addWidget(m_error);
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
}

void JobFormPage::setRequiredKinds(const QStringList& kinds)
{
    m_kinds = kinds;
    refreshAvailability();
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
    auto* card = new Card(this);
    card->body()->setSpacing(0);
    card->setTitle(title);
    m_options = new JobOptionsPanel(m_ctx, sections, excludeKeys, card);
    m_options->setGroupCards(false);
    m_options->build();
    card->body()->addWidget(m_options);
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
        showFormError(r.error().code == ErrorCode::Unsupported ? QStringLiteral("Not available in this build")
                                                              : QStringLiteral("The job was not added"),
                      r.error().message);
        return;
    }
    const QString id = r.value();
    auto& q = m_ctx.queue();
    if (buildNow) {
        // Put it right after the running jobs and make sure the queue runs.
        int firstQueued = 0;
        const auto recs = q.records();
        for (const auto& rec : recs) {
            if (rec.state == JobState::Running || rec.state == JobState::Paused)
                ++firstQueued;
            else
                break;
        }
        q.move(id, firstQueued);
        q.holdQueue(false);
    }
    emit jobSubmitted(id, openOutputWhenDone());
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

void JobFormPage::clearDemoState() { clearFormError(); }

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
    return QStringLiteral("%1 days ago").arg(s / 86400);
}

QString driveLabel(const QString& path)
{
    const QStorageInfo si(path.isEmpty() ? QDir::homePath() : path);
    QString root = si.rootPath();
    if (root.size() >= 2 && root[1] == u':')
        return root.left(2);
    return root.isEmpty() ? QStringLiteral("this drive") : root;
}

} // namespace fw::pages
