#include "app/MainWindow.h"

#include "app/AppContext.h"
#include "app/models/JobQueueModel.h"
#include "app/models/LogModel.h"
#include "app/pages/AboutPage.h"
#include "app/pages/BackportPage.h"
#include "app/pages/BuildImagePage.h"
#include "app/pages/DlcPage.h"
#include "app/pages/HomePage.h"
#include "app/pages/ImageToolsPage.h"
#include "app/pages/LibraryPage.h"
#include "app/pages/LogsPage.h"
#include "app/pages/Ps5ConnectPage.h"
#include "app/pages/QueuePage.h"
#include "app/pages/SettingsPage.h"
#include "app/services/JobSubmit.h"
#include "app/services/LauncherIdentity.h"
#include "app/ui/Icons.h"
#include "app/ui/Theme.h"
#include "app/ui/Widgets.h"
#include "connect/notify/DiscordWebhook.h"
#include "core/ArchiveSource.h"
#include "core/BuildInfo.h"
#include "core/CrashHandler.h"
#include "core/FileUtil.h"
#include "core/Paths.h"
#include "engines/general/Module.h"
#include "engines/tools/Module.h"
#include "engines/tools/ReportBundle.h"

#include <QApplication>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QGridLayout>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMenu>
#include <QMimeData>
#include <QScreen>
#include <QSettings>
#include <QShortcut>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QUrl>

#include <algorithm>
#include <limits>
#include <memory>

namespace fw {

using namespace ui;
using namespace theme;

MainWindow::MainWindow(AppContext& ctx, QWidget* parent) : QMainWindow(parent), m_ctx(ctx)
{
    setWindowTitle(QLatin1String(build::kProductName));
    setMinimumSize(size::windowMinW, size::windowMinH);
    setAcceptDrops(true);
    setProperty("fwNoRing", true);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, color::bg_1);
    setPalette(pal);
    setAutoFillBackground(true);

    m_queueModel = new JobQueueModel(ctx.queue(), this);
    m_logModel = &ctx.logModel();
    m_library = new LibraryStore(this);
    m_payloads = new PayloadStore(this);
    m_watcher = new QueueCompletionWatcher(ctx.queue(), ctx.settings(), this);
    // general.discordWebhookUrl: a summary of each finished run (update checks
    // and FTP diagnostics are not worth a message).
    auto* discord = new notify::DiscordNotifier(ctx.settings(), this);
    discord->watch(ctx.queue(), *m_watcher, [this](const QString& kind) { return m_ctx.jobFactory().displayName(kind); },
                   [](const QString& kind) { return !jobs::isSystemKind(kind) && kind != QLatin1String("ps5.ftp.diagnose"); });
    connect(discord, &notify::DiscordNotifier::delivered, this, [this](bool ok, int, const QString& message) {
        if (!ok)
            toast(Tone::Warn, QStringLiteral("Discord notification not sent"), message);
    });

    auto* central = new QWidget(this);
    central->setProperty("fwNoRing", true);
    auto* v = vbox(central, 0);
    m_header = new AppHeader(central);
    v->addWidget(m_header);
    auto* middle = new QWidget(central);
    middle->setProperty("fwNoRing", true);
    auto* h = hbox(middle, 0);
    m_sidebar = new Sidebar(middle);
    h->addWidget(m_sidebar);
    auto* pageColumn = new QWidget(middle);
    pageColumn->setProperty("fwNoRing", true);
    auto* pc = vbox(pageColumn, 0);
    auto* bannerHost = new QWidget(pageColumn);
    bannerHost->setProperty("fwNoRing", true);
    m_banners = vbox(bannerHost, space::sm, QMargins(space::xxl, space::md, space::xxl, 0));
    pc->addWidget(bannerHost);
    bannerHost->hide();
    m_stack = new QStackedWidget(pageColumn);
    m_stack->setProperty("fwNoRing", true);
    pc->addWidget(m_stack, 1);
    h->addWidget(pageColumn, 1);
    v->addWidget(middle, 1);
    m_status = new StatusBar(central);
    v->addWidget(m_status);
    setCentralWidget(central);
    m_focusRing = new FocusRingOverlay(this);
    m_toasts = new ToastHost(this);

    buildPages();
    m_sidebar->setVersionText(QStringLiteral("Version %1").arg(QLatin1String(build::kVersion)));
    m_header->setVersion(QStringLiteral("v%1").arg(QLatin1String(build::kVersion)));
    connect(m_sidebar, &Sidebar::selected, this, [this](const QString& key) { navigate(key); });
    connect(m_header->menuButton(), &QToolButton::clicked, this, [this] {
        m_userCompact = !m_sidebar->isCompact();
        setSidebarCompact(m_userCompact);
    });
    connect(m_header, &AppHeader::queueChipClicked, this, [this] { navigate(QStringLiteral("queue")); });
    connect(m_header, &AppHeader::consoleChipClicked, this, [this] { navigate(QStringLiteral("ps5")); });
    connect(m_header->helpButton(), &QToolButton::clicked, this, [this] {
        QMenu menu(this);
        menu.addAction(QStringLiteral("Keyboard shortcuts"), this, &MainWindow::showShortcuts);
        // Where the log files are written: general.logFolder when it is set, else the default folder.
        menu.addAction(QStringLiteral("Open logs folder"), this, [this] {
            const QString log = m_ctx.logFilePath();
            openInFileManager(log.isEmpty() ? Paths::logsDir() : QFileInfo(log).absolutePath());
        });
        menu.addAction(QStringLiteral("Report a problem…"), this, [this] { reportProblem(); });
        auto* upd = menu.addAction(QStringLiteral("Check for updates"), this, [this] { checkForUpdates(false); });
        upd->setEnabled(jobs::isAvailable(m_ctx, QStringLiteral("update.check")));
        menu.addSeparator();
        menu.addAction(QStringLiteral("About"), this, [this] { navigate(QStringLiteral("about")); });
        menu.exec(m_header->helpButton()->mapToGlobal(QPoint(0, m_header->helpButton()->height())));
    });

    connect(&ctx.queue(), &JobQueue::jobFinished, this, &MainWindow::onJobFinished);
    connect(m_queueModel, &JobQueueModel::countsChanged, this, [this] {
        updateChips();
        updateStatus();
    });
    connect(m_queueModel, &QAbstractItemModel::dataChanged, this, [this] { updateStatus(); });
    connect(&ctx.queue(), &JobQueue::heldChanged, this, [this] { // "Queue waiting · 2", "2 waiting · press Start queue"
        updateChips();
        updateStatus();
    });
    connect(m_watcher, &QueueCompletionWatcher::queueFinished, this, &MainWindow::onQueueFinished);
    connect(m_watcher, &QueueCompletionWatcher::actionDue, this, &MainWindow::onActionDue);
    connect(&ctx.settings(), &SettingsStore::changed, this, [this](const QString& key, const QVariant&) {
        if (key == general::keys::kDefaultOutputDir)
            refreshFreeSpace();
    });
    m_freeSpaceTimer.setInterval(15000);
    connect(&m_freeSpaceTimer, &QTimer::timeout, this, &MainWindow::refreshFreeSpace);
    m_freeSpaceTimer.start();

    if (QSystemTrayIcon::isSystemTrayAvailable()) {
        m_tray = new QSystemTrayIcon(launcher::appIcon(), this);
        m_tray->setToolTip(windowTitle());
        connect(m_tray, &QSystemTrayIcon::messageClicked, this, [this] {
            showNormal();
            activateWindow();
            navigate(QStringLiteral("queue"));
        });
        // The icon itself brings the window back too.
        connect(m_tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
            if (reason != QSystemTrayIcon::Trigger && reason != QSystemTrayIcon::DoubleClick)
                return;
            if (isMinimized())
                showNormal();
            raise();
            activateWindow();
        });
    }

    buildShortcuts();
    restoreWindowState();
    refreshFreeSpace();
    updateChips();
    updateStatus();
    ctx.checkDependenciesAsync();
    QTimer::singleShot(500, this, &MainWindow::checkCrashFiles);
    if (ctx.settings().value(QStringLiteral("general.checkForUpdatesOnStart")).toBool())
        QTimer::singleShot(3000, this, [this] { checkForUpdates(true); });
}

MainWindow::~MainWindow() = default;

void MainWindow::addPage(pages::Page* page, const QString& iconName, const QString& text, bool footer)
{
    m_pages.insert(page->key(), page);
    m_order << page->key();
    page->header()->setIcon(iconName);
    m_stack->addWidget(page);
    if (footer)
        m_sidebar->addFooterItem(page->key(), iconName, text);
    else
        m_sidebar->addItem(page->key(), iconName, text);
    connect(page, &pages::Page::navigateRequested, this, [this](const pages::NavRequest& r) { navigate(r.page, r.args); });
    connect(page, &pages::Page::toastRequested, this, [this](Tone tone, const QString& title, const QString& body) { toast(tone, title, body); });
    connect(page, &pages::Page::toastActionRequested, this,
            [this](Tone tone, const QString& title, const QString& body, const QString& actionText, const std::function<void()>& action) {
                toast(tone, title, body, {{actionText, action}});
            });
    connect(page, &pages::Page::statusContextChanged, this, &MainWindow::updateStatus);
    if (auto* form = qobject_cast<pages::JobFormPage*>(page))
        connect(form, &pages::JobFormPage::jobSubmitted, this, [this](const QString& id, bool openFolder) {
            if (openFolder)
                m_openFolderWhenDone.insert(id);
        });
}

void MainWindow::buildPages()
{
    m_home = new pages::HomePage(m_ctx, *m_queueModel, this);
    m_home->setPayloadStore(m_payloads);
    addPage(m_home, QStringLiteral("home"), QStringLiteral("Home"));
    m_sidebar->addGroup(QStringLiteral("Workflows"));
    // FPKG packages are built on the Build page (format "fpkg"); see navigate().
    addPage(new pages::BuildImagePage(m_ctx, *m_queueModel, this), QStringLiteral("build"), QStringLiteral("Build image"));
    addPage(new pages::ImageToolsPage(m_ctx, *m_queueModel, this), QStringLiteral("extract"), QStringLiteral("Image tools"));
    addPage(new pages::BackportPage(m_ctx, *m_queueModel, *m_logModel, *m_library, this), QStringLiteral("backports"), QStringLiteral("Backport"));
    addPage(new pages::DlcPage(m_ctx, *m_queueModel, this), QStringLiteral("package_plus"), QStringLiteral("DLC"));
    m_sidebar->addGroup(QStringLiteral("Tools"));
    m_queuePage = new pages::QueuePage(m_ctx, *m_queueModel, *m_logModel, this);
    addPage(m_queuePage, QStringLiteral("queue"), QStringLiteral("Queue"));
    m_ps5Page = new pages::Ps5ConnectPage(m_ctx, *m_queueModel, *m_payloads, this);
    addPage(m_ps5Page, QStringLiteral("ps5"), QStringLiteral("PS5 connect"));
    addPage(new pages::LibraryPage(m_ctx, *m_queueModel, *m_library, this), QStringLiteral("library"), QStringLiteral("Library"));
    m_sidebar->addGroup(QStringLiteral("System"));
    addPage(new pages::LogsPage(m_ctx, *m_logModel, this), QStringLiteral("file_text"), QStringLiteral("Logs"));
    m_settingsPage = new pages::SettingsPage(m_ctx, this);
    addPage(m_settingsPage, QStringLiteral("settings"), QStringLiteral("Settings"));
    m_sidebar->addStretch();
    m_aboutPage = new pages::AboutPage(m_ctx, this);
    addPage(m_aboutPage, QStringLiteral("credits"), QStringLiteral("About"), true);
    connect(m_aboutPage, &pages::AboutPage::checkForUpdatesRequested, this, [this] { checkForUpdates(false); });
    connect(m_aboutPage, &pages::AboutPage::updateActionRequested, this, &MainWindow::runUpdateAction);
    connect(m_aboutPage, &pages::AboutPage::reportProblemRequested, this, [this] { reportProblem(); });
    connect(m_aboutPage, &pages::AboutPage::shortcutsRequested, this, &MainWindow::showShortcuts);
    connect(m_ps5Page, &pages::Ps5ConnectPage::connectionChanged, this,
            [this](ui::ConsoleSession::ConnectionState state, const QString& host, const QString&) {
                using State = ui::ConsoleSession::ConnectionState;
                switch (state) {
                case State::Connected:
                    m_header->setConsoleChip(QStringLiteral("PS5 connected · %1").arg(host), Tone::Ok, true);
                    m_home->setConsoleStatus(host, Tone::Ok);
                    break;
                case State::Connecting:
                    m_header->setConsoleChip(QStringLiteral("Connecting to %1…").arg(host), Tone::Info, true);
                    m_home->setConsoleStatus(QStringLiteral("Connecting…"), Tone::Info);
                    break;
                case State::Failed:
                    m_header->setConsoleChip(QStringLiteral("PS5 not reachable"), Tone::Warn, true);
                    m_home->setConsoleStatus(QStringLiteral("Not reachable"), Tone::Warn);
                    break;
                case State::Offline:
                    m_header->setConsoleChip(QStringLiteral("PS5 not connected"), Tone::Neutral, true);
                    m_home->setConsoleStatus(QStringLiteral("Not connected"), Tone::Neutral);
                    break;
                }
            });
    connect(m_settingsPage, &pages::SettingsPage::attentionChanged, this, &MainWindow::updateStatus);
}

void MainWindow::buildShortcuts()
{
    // A sheet (confirm, countdown) is a child widget, not a modal window: the
    // shortcuts stand down while one is open so Ctrl+B cannot start a job
    // under "Cancel the running job?".
    auto add = [this](const QKeySequence& seq, auto fn) {
        auto* s = new QShortcut(seq, this);
        s->setContext(Qt::WindowShortcut);
        connect(s, &QShortcut::activated, this, [fn] {
            if (Sheet::openCount() == 0)
                fn();
        });
    };
    add(QKeySequence(QStringLiteral("Ctrl+B")), [this] {
        if (auto* p = currentPageWidget(); p && p->hasPrimaryAction())
            p->primaryAction();
    });
    add(QKeySequence(QStringLiteral("Ctrl+L")), [this] { navigate(QStringLiteral("logs")); });
    add(QKeySequence(QStringLiteral("Ctrl+,")), [this] { navigate(QStringLiteral("settings")); });
    // Ctrl+Q everywhere, plus the platform's own quit key where it is another one
    // (the same sequence registered twice would be ambiguous and never fire).
    const QKeySequence ctrlQ(QStringLiteral("Ctrl+Q"));
    add(ctrlQ, [this] { close(); });
    if (const QKeySequence quit(QKeySequence::Quit); !quit.isEmpty() && quit != ctrlQ)
        add(quit, [this] { close(); });
    add(QKeySequence(Qt::Key_F5), [this] {
        if (auto* p = currentPageWidget())
            p->refresh();
    });
    for (int i = 1; i <= 9; ++i)
        add(QKeySequence(QStringLiteral("Ctrl+%1").arg(i)), [this, i] {
            const QStringList keys = m_sidebar->keys();
            if (i - 1 < keys.size())
                navigate(keys[i - 1]);
        });
}

pages::Page* MainWindow::currentPageWidget() const
{
    return qobject_cast<pages::Page*>(m_stack->currentWidget());
}

QString MainWindow::currentPage() const
{
    auto* p = currentPageWidget();
    return p ? p->key() : QString();
}

void MainWindow::navigate(const QString& requested, const QVariantMap& requestedArgs)
{
    // v5: the FPKG page is the Build page with FPKG selected, and "Extract &
    // verify" / "Edit image" became actions of Image tools; their old keys
    // still lead there, with the matching format or action selected.
    QString key = requested;
    QVariantMap args = requestedArgs;
    if (key == QLatin1String("fpkg")) {
        args.insert(QStringLiteral("format"), QStringLiteral("fpkg"));
        key = QStringLiteral("build");
    }
    if (key == QLatin1String("extract") || key == QLatin1String("edit")) {
        if (!args.contains(QStringLiteral("action")))
            args.insert(QStringLiteral("action"), key == QLatin1String("edit") ? QStringLiteral("edit")
                                                  : args.value(QStringLiteral("mode")).toString() == QLatin1String("verify") ? QStringLiteral("verify")
                                                                                                                             : QStringLiteral("extract"));
        key = QStringLiteral("imagetools");
    }
    pages::Page* p = m_pages.value(key);
    if (!p)
        return;
    // The first page added is the stack's current widget from the start, with
    // nothing selected in the sidebar and onShown() not called yet: the first
    // navigation counts as a change even when it leads to that page.
    if (m_stack->currentWidget() != p || m_shownPage != p) {
        m_stack->setCurrentWidget(p);
        m_shownPage = p;
        m_sidebar->setCurrent(key);
        p->onShown();
    }
    if (!args.isEmpty())
        p->navigateWith(args);
    updateStatus();
}

void MainWindow::setSidebarCompact(bool compact)
{
    m_sidebar->setCompact(compact);
}

void MainWindow::toast(Tone tone, const QString& title, const QString& body, const QList<ToastHost::Action>& actions)
{
    m_toasts->show(tone, title, body, actions);
}

void MainWindow::showBanner(InfoBar* bar)
{
    QWidget* host = m_banners->parentWidget();
    m_banners->addWidget(bar);
    host->show();
    connect(bar, &InfoBar::dismissed, this, [this, host] {
        bool any = false;
        for (int i = 0; i < m_banners->count(); ++i)
            if (auto* w = m_banners->itemAt(i)->widget(); w && w->isVisible())
                any = true;
        host->setVisible(any);
    });
}

// ---- status bar and chips ------------------------------------------------------------

static void countUserJobs(const JobQueueModel& model, int& running, int& queued)
{
    running = queued = 0;
    for (int r = 0; r < model.rowCount(); ++r) {
        const JobRecord* rec = model.recordAt(r);
        if (!rec || jobs::isSystemKind(rec->kind))
            continue;
        if (rec->isActive())
            ++running;
        else if (rec->state == JobState::Queued)
            ++queued;
    }
}

void MainWindow::updateChips()
{
    int running = 0, queued = 0;
    countUserJobs(*m_queueModel, running, queued);
    m_sidebar->setCount(QStringLiteral("queue"), running + queued);
    if (running == 0 && queued == 0)
        m_header->setQueueChip(QStringLiteral("Queue idle"), Tone::Neutral, true);
    else if (running == 0 && m_ctx.queue().isHeld()) // a list that waits for "Start queue"
        m_header->setQueueChip(QStringLiteral("Queue waiting · %1").arg(queued), Tone::Info, true);
    else
        m_header->setQueueChip(QStringLiteral("%1 running · %2 queued").arg(running).arg(queued), Tone::Info, true);
}

void MainWindow::updateStatus()
{
    int runningUser = 0, queuedUser = 0;
    countUserJobs(*m_queueModel, runningUser, queuedUser);
    Q_UNUSED(runningUser);
    const JobRecord* running = nullptr;
    for (int r = 0; r < m_queueModel->rowCount(); ++r)
        if (const JobRecord* rec = m_queueModel->recordAt(r);
            rec && !jobs::isSystemKind(rec->kind) && rec->isActive()) {
            running = rec;
            break;
        }
    if (running) {
        const QString pct = running->progress.indeterminate ? QString() : QStringLiteral(" · %1").arg(thinPercent(running->progress.overallFraction));
        const QString verb = running->state == JobState::Paused ? QStringLiteral("Paused:")
                             : running->state == JobState::Cancelling ? QStringLiteral("Cancelling:") : QStringLiteral("Running:");
        m_status->setStatus(QStringLiteral("%1 %2%3").arg(verb, running->title, pct),
                            running->state == JobState::Running ? Tone::Info : Tone::Neutral);
    } else if (const int n = m_settingsPage ? m_settingsPage->attentionCount() : 0; n > 0) {
        m_status->setStatus(QStringLiteral("%1 setting%2 need%3 attention").arg(n).arg(n == 1 ? QString() : QStringLiteral("s")).arg(n == 1 ? QStringLiteral("s") : QString()), Tone::Warn);
    } else if (m_ctx.queue().isHeld() && queuedUser > 0) {
        // Nothing runs here: the waiting jobs start with "Start queue".
        m_status->setStatus(QStringLiteral("%1 waiting · press Start queue").arg(queuedUser), Tone::Neutral);
    } else {
        m_status->setStatus(QStringLiteral("Ready"), Tone::Ok);
    }
    if (auto* p = currentPageWidget())
        m_status->setContext(p->statusContext());
}

void MainWindow::refreshFreeSpace()
{
    QString folder = m_ctx.settings().value(general::keys::kDefaultOutputDir).toString();
    if (folder.isEmpty())
        folder = QDir::homePath();
    // Off the UI thread: a network or unplugged drive answers slowly.
    const quint64 gen = ++m_freeSpaceGen;
    pages::probeFreeSpace(this, folder, [this, gen](bool ready, qint64 available, const QString& label) {
        if (gen != m_freeSpaceGen)
            return;
        m_status->setFreeSpace(ready ? QStringLiteral("%1 %2 free").arg(label, formatBytes(available)) : QString());
    });
}

// ---- jobs ------------------------------------------------------------------------------

void MainWindow::onJobFinished(const QString& id)
{
    auto rec = m_ctx.queue().record(id);
    if (!rec)
        return;
    if (id == m_updateCheckId) {
        onUpdateCheckFinished(*rec);
        return;
    }
    if (id == m_updateDownloadId) {
        onUpdateDownloadFinished(*rec);
        return;
    }
    if (jobs::isSystemKind(rec->kind))
        return;
    // A finished dump check gets the Library's own toast with "Show details" (GitHub #26); a failed one keeps this one.
    if (rec->kind == QLatin1String("tools.dump.check") && rec->state == JobState::Succeeded)
        return;
    const QString kindName = m_ctx.jobFactory().displayName(rec->kind);
    QList<ToastHost::Action> actions;
    if (rec->state == JobState::Succeeded) {
        const QString out = rec->outcome && !rec->outcome->outputs.isEmpty() ? rec->outcome->outputs.first() : QString();
        if (!out.isEmpty())
            actions << ToastHost::Action{QStringLiteral("Show in folder"), [out] { openInFileManager(out); }};
        actions << ToastHost::Action{QStringLiteral("Open queue"), [this, id] { navigate(QStringLiteral("queue"), {{QStringLiteral("jobId"), id}}); }};
        if (rec->kind == QLatin1String("tools.report") && id == m_reportJobId) {
            m_reportJobId.clear();
            // After this signal returns: the queue schedules its next job only
            // once jobFinished has been delivered, and a sheet blocks here.
            const QString issueUrl = rec->outcome ? rec->outcome->details.value(QStringLiteral("issueUrl")).toString(tools::kNewIssueUrl) : tools::kNewIssueUrl;
            QTimer::singleShot(0, this, [this, out, issueUrl] {
                auto* s = new Sheet(this, QStringLiteral("Problem report ready"), Sheet::Icon::Info);
                s->addText(QStringLiteral("The report was saved as %1. Attach it to a new GitHub issue and describe what happened.").arg(Sheet::monoSpan(nativePath(out))));
                s->addButton(QStringLiteral("Close"), Sheet::Role::Safe);
                s->addButton(QStringLiteral("Show in folder"), Sheet::Role::Safe); // one primary action per sheet
                s->addButton(QStringLiteral("Open GitHub issue"), Sheet::Role::Primary);
                const int r = s->exec();
                if (r == 1)
                    openInFileManager(out);
                else if (r == 2)
                    QDesktopServices::openUrl(QUrl(issueUrl));
            });
            return;
        }
        const bool warnings = rec->outcome && !rec->outcome->warnings.isEmpty();
        const bool verified = rec->outcome && rec->outcome->verification == Verification::Passed;
        QString title = warnings ? QStringLiteral("%1 done with %2 warning%3").arg(kindName).arg(rec->outcome->warnings.size()).arg(rec->outcome->warnings.size() == 1 ? QString() : QStringLiteral("s"))
                                 : verified ? QStringLiteral("%1 done and verified").arg(kindName) : QStringLiteral("%1 done").arg(kindName);
        QString body = rec->title;
        if (rec->outcome && !rec->outcome->summary.isEmpty())
            body += QStringLiteral(" · ") + rec->outcome->summary;
        if (warnings)
            actions.prepend(ToastHost::Action{QStringLiteral("Show warnings"), [this, id] { navigate(QStringLiteral("logs"), {{QStringLiteral("jobId"), id}, {QStringLiteral("level"), QStringLiteral("warning")}}); }});
        toast(warnings ? Tone::Warn : Tone::Ok, title, body, actions);
        if (m_openFolderWhenDone.remove(id) && !out.isEmpty())
            openInFileManager(out);
    } else if (rec->state == JobState::Failed || rec->state == JobState::Interrupted) {
        actions << ToastHost::Action{QStringLiteral("Retry"), [this, id] {
                                         if (auto r = m_ctx.queue().retry(id); r.ok())
                                             m_ctx.queue().startNow(r.value());
                                     }};
        actions << ToastHost::Action{QStringLiteral("Open log"), [this, id] { navigate(QStringLiteral("logs"), {{QStringLiteral("jobId"), id}, {QStringLiteral("level"), QStringLiteral("error")}}); }};
        toast(Tone::Err, QStringLiteral("%1 failed").arg(kindName), QStringLiteral("%1 · %2").arg(rec->title, rec->error ? rec->error->message : QStringLiteral("See the log.")), actions);
        m_openFolderWhenDone.remove(id);
    } else {
        m_openFolderWhenDone.remove(id);
    }
    if (m_home)
        m_home->refresh();
}

void MainWindow::onQueueFinished(int succeeded, int failed, int cancelled)
{
    Q_UNUSED(cancelled);
    if (!m_ctx.settings().value(general::keys::kNotifyOnFinish).toBool())
        return;
    if (isActiveWindow())
        return;
    const QString title = failed > 0 ? QStringLiteral("Queue finished with %1 failure%2").arg(failed).arg(failed == 1 ? QString() : QStringLiteral("s"))
                                     : QStringLiteral("Queue finished");
    const QString body = QStringLiteral("%1 job%2 done%3").arg(succeeded).arg(succeeded == 1 ? QString() : QStringLiteral("s")).arg(failed > 0 ? QStringLiteral(", %1 failed").arg(failed) : QString());
    QApplication::beep();
    if (m_tray) {
        if (!m_tray->isVisible())
            m_tray->show();
        m_tray->showMessage(title, body, failed > 0 ? QSystemTrayIcon::Warning : QSystemTrayIcon::Information, 8000);
    }
}

void MainWindow::onActionDue(power::Action action, int delaySeconds, int succeeded, int failed)
{
    if (action == power::Action::None)
        return;
    if (m_countdown)
        m_countdown->close(0);
    const QString verb = power::actionLabel(action);
    auto* s = new Sheet(this, QStringLiteral("%1 in %2 s").arg(verb, QString::number(delaySeconds)), Sheet::Icon::Warning);
    auto* body = s->addText(QStringLiteral("The queue finished: %1 job%2 done%3. The computer will %4 unless you cancel.")
                                .arg(succeeded).arg(succeeded == 1 ? QString() : QStringLiteral("s"))
                                .arg(failed > 0 ? QStringLiteral(", %1 failed").arg(failed) : QString(), verb.toLower()));
    Q_UNUSED(body);
    s->addButton(QStringLiteral("Cancel"), Sheet::Role::Safe);
    s->addButton(QStringLiteral("%1 now").arg(verb), Sheet::Role::Primary);
    m_countdown = s;
    auto* timer = new QTimer(s);
    auto remaining = std::make_shared<int>(delaySeconds);
    connect(timer, &QTimer::timeout, s, [s, timer, remaining, verb] {
        if (--*remaining <= 0) {
            timer->stop();
            s->close(1);
            return;
        }
        s->setTitle(QStringLiteral("%1 in %2 s").arg(verb, QString::number(*remaining)));
    });
    timer->start(1000);
    const int r = s->exec();
    m_countdown = nullptr;
    if (r != 1)
        return;
    auto st = power::perform(action);
    if (!st.ok())
        toast(Tone::Err, QStringLiteral("%1 did not happen").arg(verb), st.error().message);
}

// ---- updates --------------------------------------------------------------------------
//
// A release is the program file itself. The app finds it, can download and
// check it, and then tells the user where it is: it never runs the downloaded
// file, never closes for it and never replaces the running program.

namespace {

// The release page as the check reported it: a web address or nothing.
QUrl releasePageOf(const QJsonObject& release)
{
    const QUrl url(release.value(QStringLiteral("pageUrl")).toString(), QUrl::StrictMode);
    return url.isValid() && url.scheme() == QLatin1String("https") && !url.host().isEmpty() ? url : QUrl();
}

} // namespace

void MainWindow::checkForUpdates(bool silent, const QString& apiUrl)
{
    // Automated tests and CI runs set FW_OFFLINE so nothing contacts GitHub.
    if (silent && qEnvironmentVariableIsSet("FW_OFFLINE"))
        return;
    // One check at a time, and never over a download.
    if (m_updateState == UpdateState::Checking || m_updateState == UpdateState::Downloading)
        return;
    auto r = jobs::submitUpdateCheck(m_ctx, apiUrl);
    if (!r.ok()) {
        if (!silent)
            toast(Tone::Warn, QStringLiteral("Update check not available"), r.error().message);
        return;
    }
    m_updateCheckId = r.value(); // runs even while the queue is paused (Job::runsWhileHeld)
    m_updateState = UpdateState::Checking;
    if (!silent)
        m_aboutPage->setUpdateStatus(QStringLiteral("Checking…"), Tone::Info);
}

void MainWindow::onUpdateCheckFinished(const JobRecord& rec)
{
    m_updateCheckId.clear();
    if (rec.state != JobState::Succeeded || !rec.outcome) {
        // What the last good answer said still holds: its banner stays usable.
        m_updateState = !m_downloadedUpdate.isEmpty()                         ? UpdateState::Downloaded
                        : m_release.value(QStringLiteral("newer")).toBool() ? UpdateState::Available
                                                                              : UpdateState::Idle;
        m_aboutPage->setUpdateStatus(QStringLiteral("Check failed"), Tone::Warn);
        return;
    }
    const QString downloadedVersion = m_downloadedUpdate.isEmpty() ? QString() : m_release.value(QStringLiteral("version")).toString();
    m_release = rec.outcome->details;
    if (!m_release.value(QStringLiteral("newer")).toBool()) {
        m_updateState = UpdateState::UpToDate;
        m_downloadedUpdate.clear();
        setUpdateBanner(nullptr);
        m_aboutPage->setUpdateStatus(QStringLiteral("Up to date"), Tone::Ok);
        m_aboutPage->setUpdateAction({}, {});
        return;
    }
    // The same release as the file that is already here: nothing to fetch again.
    if (downloadedVersion == m_release.value(QStringLiteral("version")).toString() && QFileInfo(m_downloadedUpdate).isFile()) {
        m_updateState = UpdateState::Downloaded;
        showUpdateDownloaded();
        return;
    }
    m_downloadedUpdate.clear();
    m_updateState = UpdateState::Available;
    showUpdateAvailable();
}

// One update banner at a time: `bar` takes the place of the one shown (null: none).
void MainWindow::setUpdateBanner(InfoBar* bar)
{
    if (m_updateBanner) {
        m_updateBanner->hide();
        emit m_updateBanner->dismissed(); // the banner area closes when nothing else is in it
        m_updateBanner->deleteLater();
    }
    m_updateBanner = bar;
    if (!bar)
        return;
    bar->setObjectName(QStringLiteral("update-banner"));
    bar->setDismissable(true);
    showBanner(bar);
}

// State Available: the banner and the About page offer the one thing that can
// be done with this release (download it, or open its page).
void MainWindow::showUpdateAvailable()
{
    const QString version = m_release.value(QStringLiteral("version")).toString();
    const bool canDownload = m_release.value(QStringLiteral("canDownload")).toBool();
    const QUrl page = releasePageOf(m_release);
    m_aboutPage->setUpdateStatus(QStringLiteral("Version %1 available").arg(version), Tone::Info);
    auto* bar = new InfoBar(Tone::Info, this);
    bar->setTitle(QStringLiteral("Version %1 is available").arg(version));
    if (canDownload) {
        bar->setMessage(QStringLiteral("The download is checked against its SHA-256. Nothing is installed: you replace the program file yourself."));
        if (page.isValid()) {
            auto* notes = bar->addLink(QStringLiteral("What's new"));
            connect(notes, &Link::activated, this, [page] { QDesktopServices::openUrl(page); });
        }
        auto* get = bar->addLink(QStringLiteral("Download"));
        connect(get, &Link::activated, this, &MainWindow::runUpdateAction);
        m_aboutPage->setUpdateAction(QStringLiteral("Download"), QStringLiteral("Downloads version %1 and checks it against its SHA-256").arg(version));
    } else {
        const QString note = m_release.value(QStringLiteral("note")).toString();
        bar->setMessage(note.isEmpty() ? QStringLiteral("This release has no download the app can check itself. Get it from the release page.") : note);
        if (page.isValid()) {
            auto* open = bar->addLink(QStringLiteral("Open the release page"));
            connect(open, &Link::activated, this, &MainWindow::runUpdateAction);
            m_aboutPage->setUpdateAction(QStringLiteral("Open the release page"), page.toString());
        } else {
            m_aboutPage->setUpdateAction({}, {});
        }
    }
    setUpdateBanner(bar);
}

// State Downloaded: where the verified file is and what to do with it.
void MainWindow::showUpdateDownloaded()
{
    const QString version = m_release.value(QStringLiteral("version")).toString();
    m_aboutPage->setUpdateStatus(QStringLiteral("Version %1 downloaded").arg(version), Tone::Ok);
    m_aboutPage->setUpdateAction(QStringLiteral("Show in folder"), nativePath(m_downloadedUpdate));
    auto* bar = new InfoBar(Tone::Ok, this);
    bar->setTitle(QStringLiteral("Version %1 downloaded and verified").arg(version));
    bar->setMessage(QStringLiteral("To update, close the app and replace your exFAT Image Builder.exe with the downloaded file (%1).")
                        .arg(QFileInfo(m_downloadedUpdate).fileName()));
    bar->setToolTip(plainTip(nativePath(m_downloadedUpdate)));
    auto* show = bar->addLink(QStringLiteral("Show in folder"));
    connect(show, &Link::activated, this, &MainWindow::runUpdateAction);
    setUpdateBanner(bar);
}

// The one action the current state offers (the banner's link and the About
// page's button both end here).
void MainWindow::runUpdateAction()
{
    switch (m_updateState) {
    case UpdateState::Available: {
        if (!m_release.value(QStringLiteral("canDownload")).toBool()) {
            if (const QUrl page = releasePageOf(m_release); page.isValid())
                QDesktopServices::openUrl(page);
            return;
        }
        auto r = jobs::submitUpdateDownload(m_ctx, m_release);
        if (!r.ok()) {
            // Refused (not newer, not a GitHub release address, …): still Available.
            toast(Tone::Err, QStringLiteral("Download not started"), r.error().message);
            return;
        }
        m_updateDownloadId = r.value();
        m_updateState = UpdateState::Downloading;
        m_aboutPage->setUpdateStatus(QStringLiteral("Downloading version %1…").arg(m_release.value(QStringLiteral("version")).toString()), Tone::Info);
        toast(Tone::Info, QStringLiteral("Downloading the update"),
              QStringLiteral("You can keep working. The file is checked when the download ends; nothing is started or replaced."));
        return;
    }
    case UpdateState::Downloading:
        toast(Tone::Info, QStringLiteral("Already downloading"), QStringLiteral("The file is checked when the download ends."));
        return;
    case UpdateState::Downloaded:
        if (!openInFileManager(m_downloadedUpdate))
            toast(Tone::Warn, QStringLiteral("The downloaded file is gone"), nativePath(m_downloadedUpdate));
        return;
    case UpdateState::Idle:
    case UpdateState::Checking:
    case UpdateState::UpToDate:
        return;
    }
}

void MainWindow::onUpdateDownloadFinished(const JobRecord& rec)
{
    m_updateDownloadId.clear();
    if (rec.state != JobState::Succeeded || !rec.outcome || rec.outcome->outputs.isEmpty()) {
        m_updateState = UpdateState::Available; // Download can be tried again
        m_aboutPage->setUpdateStatus(QStringLiteral("Version %1 available").arg(m_release.value(QStringLiteral("version")).toString()), Tone::Info);
        if (rec.state == JobState::Cancelled)
            toast(Tone::Info, QStringLiteral("Update download cancelled"), QStringLiteral("Nothing was kept."));
        else
            toast(Tone::Err, QStringLiteral("Update download failed"), rec.error ? rec.error->message : QStringLiteral("The downloaded file could not be verified."));
        return;
    }
    m_downloadedUpdate = rec.outcome->outputs.first();
    m_updateState = UpdateState::Downloaded;
    showUpdateDownloaded();
}

// ---- crash files and problem reports -------------------------------------------------------

void MainWindow::checkCrashFiles()
{
    const QString dir = QDir(Paths::logsDir()).filePath(QStringLiteral("crashes"));
    const QStringList files = crash::previousCrashFiles(dir);
    if (files.isEmpty())
        return;
    QSettings qs;
    const QString newest = files.first();
    if (qs.value(QStringLiteral("crash/lastAcknowledged")).toString() == newest)
        return;
    auto* bar = new InfoBar(Tone::Warn, this);
    bar->setDismissable(true);
    bar->setTitle(QStringLiteral("The app closed unexpectedly last time"));
    bar->setMessage(QStringLiteral("A crash report was saved. Sending a problem report helps fix it; it contains logs and the crash report (text), never your games."));
    auto* report = bar->addLink(QStringLiteral("Create a problem report"));
    connect(report, &Link::activated, this, [this, bar = QPointer<InfoBar>(bar)] {
        // The banner goes when the report is on its way; Cancel in the sheet leaves it.
        if (reportProblem() && bar) {
            bar->hide();
            emit bar->dismissed(); // records the crash as acknowledged
        }
    });
    connect(bar, &InfoBar::dismissed, this, [newest] { QSettings().setValue(QStringLiteral("crash/lastAcknowledged"), newest); });
    showBanner(bar);
}

bool MainWindow::reportProblem()
{
    // The newest crash reports go in as text. The memory dump of a crash never
    // does: it can hold passcodes and paths that cannot be masked.
    const QStringList crashFiles = crash::previousCrashFiles(QDir(Paths::logsDir()).filePath(QStringLiteral("crashes")));
    const bool crashed = !crashFiles.isEmpty();
    auto* s = new Sheet(this, QStringLiteral("Report a problem"), Sheet::Icon::Info, size::dialogWide);
    s->addText(QStringLiteral("A ZIP is written to your Desktop with: the app's log files, the settings, queue and history files, the dependency statuses and a system summary%1. "
                              "It never contains game files. Attach it to a GitHub issue afterwards.")
                   .arg(crashed ? QStringLiteral(", and the newest crash reports (text)") : QString()));
    auto* redact = s->addCheckbox(QStringLiteral("Hide my user name in paths"));
    redact->setChecked(m_ctx.settings().value(tools::keys::kRedactReports).toBool());
    const bool available = jobs::isAvailable(m_ctx, QStringLiteral("tools.report"));
    if (!available)
        s->body()->addWidget(new UnavailableState(QStringLiteral("The problem report"), QStringLiteral("tools.report"), s));
    s->addButton(QStringLiteral("Cancel"), Sheet::Role::Safe);
    auto* create = s->addButton(QStringLiteral("Create report"), Sheet::Role::Primary);
    create->setEnabled(available);
    if (s->exec() != 1)
        return false;
    (void)m_ctx.settings().setValue(tools::keys::kRedactReports, redact->isChecked());
    QJsonArray deps;
    for (const auto& d : m_ctx.dependencies().lastAll())
        deps.append(d.toJson());
    QJsonObject diagnostics{{QStringLiteral("version"), QString::fromLatin1(build::kVersion)},
                            {QStringLiteral("dependencies"), deps},
                            {QStringLiteral("jobKinds"), QJsonArray::fromStringList(m_ctx.jobFactory().kinds())},
                            {QStringLiteral("settingsCount"), int(m_ctx.schema().all().size())}};
    const QString desktop = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    auto r = jobs::submitProblemReport(m_ctx, desktop.isEmpty() ? QDir::homePath() : desktop, diagnostics, jobs::newestCrashFiles(crashFiles));
    if (!r.ok()) {
        toast(Tone::Err, QStringLiteral("Report not created"), r.error().message);
        return false;
    }
    m_reportJobId = r.value();
    toast(Tone::Info, QStringLiteral("Creating the problem report"), QStringLiteral("It appears on the Desktop in a moment."));
    return true;
}

void MainWindow::showShortcuts()
{
    auto* s = new Sheet(this, QStringLiteral("Keyboard shortcuts"), Sheet::Icon::None, size::dialogWide);
    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(space::lg);
    grid->setVerticalSpacing(space::sm);
    int row = 0;
    for (const auto& sc : pages::shortcutTable()) {
        grid->addWidget(label(sc.keys, TextStyle::Mono, color::fg_0, s), row, 0, Qt::AlignTop);
        auto* a = label(sc.action, TextStyle::Body, color::fg_2, s);
        a->setWordWrap(true);
        grid->addWidget(a, row, 1);
        ++row;
    }
    grid->setColumnStretch(1, 1);
    s->body()->addLayout(grid);
    s->addButton(QStringLiteral("Close"), Sheet::Role::Primary);
    s->exec();
}

// ---- window state ------------------------------------------------------------------------

void MainWindow::restoreWindowState()
{
    QSettings qs;
    const QByteArray geo = qs.value(QStringLiteral("window/geometry")).toByteArray();
    if (!geo.isEmpty()) {
        restoreGeometry(geo);
    } else {
        // First run: the designed size, or what the screen has room for (a 1366 × 768 display has less).
        QSize first(1440, 920);
        if (const QScreen* s = screen() ? screen() : QGuiApplication::primaryScreen()) {
            const QSize room = s->availableGeometry().size() - QSize(16, 40); // the frame and title bar
            first = first.boundedTo(room).expandedTo(minimumSize());
        }
        resize(first);
    }
    m_userCompact = qs.value(QStringLiteral("window/sidebarCompact"), false).toBool();
    setSidebarCompact(m_userCompact);
    const QString last = qs.value(QStringLiteral("window/lastPage"), QStringLiteral("home")).toString();
    navigate(m_pages.contains(last) ? last : QStringLiteral("home"));
}

void MainWindow::saveWindowState()
{
    QSettings qs;
    qs.setValue(QStringLiteral("window/geometry"), saveGeometry());
    qs.setValue(QStringLiteral("window/sidebarCompact"), m_userCompact);
    qs.setValue(QStringLiteral("window/lastPage"), currentPage());
}

void MainWindow::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape && Sheet::openCount() == 0) {
        // The user's own running job (never an update check or a problem report).
        for (int r = 0; r < m_queueModel->rowCount(); ++r)
            if (const JobRecord* rec = m_queueModel->recordAt(r);
                rec && !jobs::isSystemKind(rec->kind) && (rec->state == JobState::Running || rec->state == JobState::Paused)) {
                m_queuePage->cancelWithConfirm(rec->id); // a Cancelling job is already on its way out
                return;
            }
    }
    QMainWindow::keyPressEvent(event);
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    // A question is still open (answer it first), or this function is already
    // waiting for the backport engine below: a second close request starts nothing.
    if (m_closeInProgress || Sheet::openCount() > 0) {
        event->ignore();
        return;
    }
    // Edits that exist only in this window.
    for (const QString& key : m_order) {
        pages::Page* page = m_pages.value(key);
        const QString unsaved = page ? page->unsavedChanges() : QString();
        if (unsaved.isEmpty())
            continue;
        const bool discard = Sheet::confirm(this, QStringLiteral("Close without saving?"),
                                            QStringLiteral("On %1, %2. Closing the app discards this.").arg(page->title(), unsaved),
                                            QStringLiteral("Keep editing"), QStringLiteral("Close anyway"), true, Sheet::Icon::Warning);
        if (!discard) {
            event->ignore();
            navigate(key);
            return;
        }
    }
    // A backport run or restore in progress (the PS5 Backport engine runs it).
    // A run is cancelled at its next safe point (an apply in progress is rolled
    // back from the verified backup first); a restore cannot be cancelled and
    // is waited for, however long it takes.
    if (auto* bpPage = qobject_cast<pages::BackportPage*>(m_pages.value(QStringLiteral("backport"))); bpPage && bpPage->isBusy()) {
        const bool restoring = bpPage->isRestoring();
        const bool go = restoring
            ? Sheet::confirm(this, QStringLiteral("A restore is finishing"),
                             QStringLiteral("The game's original files are being put back. This cannot be stopped half-way; the app closes when it is done."),
                             QStringLiteral("Keep the app open"), QStringLiteral("Close when done"), false, Sheet::Icon::Info)
            : Sheet::confirm(this, QStringLiteral("Cancel the backport run?"),
                             QStringLiteral("A backport run is in progress. Cancel it and close?\n\nAn apply in progress is rolled back from the verified backup first."),
                             QStringLiteral("Keep running"), QStringLiteral("Cancel and close"), true, Sheet::Icon::Warning);
        if (!go) {
            event->ignore();
            return;
        }
        // The wait runs the event loop: nothing in the window may be used meanwhile.
        m_closeInProgress = true;
        centralWidget()->setEnabled(false);
        m_status->setContext(restoring ? QStringLiteral("A restore is finishing; the app closes when it is done…")
                                       : QStringLiteral("Stopping the backport run…"));
        bpPage->cancelAndWait(restoring ? std::numeric_limits<int>::max() : 120000);
        centralWidget()->setEnabled(true);
        m_closeInProgress = false;
    }
    if (m_ctx.queue().isBusy() && m_ctx.settings().value(general::keys::kConfirmOnClose).toBool()) {
        // Stopped jobs are listed again at the next start only when the queue is restored.
        const bool restored = m_ctx.settings().value(general::keys::kRestoreQueue).toBool();
        const bool stop = Sheet::confirm(this, QStringLiteral("Stop the running jobs?"),
                                         restored ? QStringLiteral("Jobs are still running. Closing stops them; partial output is removed and they come back as Interrupted so you can retry.")
                                                  : QStringLiteral("Jobs are still running. Closing stops them and removes their partial output; start them again afterwards."),
                                         QStringLiteral("Keep running"), QStringLiteral("Close and stop"), true, Sheet::Icon::Warning);
        if (!stop) {
            event->ignore();
            return;
        }
    }
    saveWindowState();
    event->accept();
}

// ---- drag and drop -------------------------------------------------------------------------

namespace {

// The page that opens what the page in front does not take itself: "build",
// "extract" (Image tools) or "" (nothing to do with it).
QString dropPageFor(const QList<QUrl>& urls)
{
    if (urls.isEmpty())
        return {};
    const QFileInfo fi(urls.first().toLocalFile());
    if (fi.isDir() || archive::isArchiveSource(fi.absoluteFilePath())) // a game folder, or an archived dump: Build takes both
        return QStringLiteral("build");
    if (!jobs::imageKindForFile(fi.absoluteFilePath(), QStringLiteral("verify")).isEmpty())
        return QStringLiteral("extract");
    return {};
}

// While a drag is over the window, the sidebar item of the page a drop would
// open is marked ("" clears the mark).
void markDropTarget(ui::Sidebar* sidebar, const QString& page)
{
    const QString key = page == QLatin1String("extract") ? QStringLiteral("imagetools") : page; // navigate()'s own alias
    for (const QString& k : sidebar->keys())
        if (ui::NavItem* item = sidebar->item(k))
            item->setDropTarget(!key.isEmpty() && k == key);
}

} // namespace

void MainWindow::dragEnterEvent(QDragEnterEvent* event)
{
    if (!event->mimeData()->hasUrls() || Sheet::openCount() > 0)
        return;
    event->acceptProposedAction();
    const auto urls = event->mimeData()->urls();
    pages::Page* p = currentPageWidget();
    const bool here = p && p->acceptsDrop(urls);
    if (p)
        p->setDragHighlight(here);
    // The page in front shows its own drop zone; otherwise the sidebar shows where the drop goes.
    markDropTarget(m_sidebar, here ? QString() : dropPageFor(urls));
}

void MainWindow::dragMoveEvent(QDragMoveEvent* event)
{
    if (event->mimeData()->hasUrls())
        event->acceptProposedAction();
}

void MainWindow::dragLeaveEvent(QDragLeaveEvent*)
{
    if (auto* p = currentPageWidget())
        p->setDragHighlight(false);
    markDropTarget(m_sidebar, {});
}

void MainWindow::dropEvent(QDropEvent* event)
{
    const auto urls = event->mimeData()->urls();
    if (auto* p = currentPageWidget())
        p->setDragHighlight(false);
    markDropTarget(m_sidebar, {});
    if (urls.isEmpty() || Sheet::openCount() > 0)
        return;
    pages::Page* p = currentPageWidget();
    if (p && p->acceptsDrop(urls)) {
        p->dropUrls(urls);
    } else {
        const QString page = dropPageFor(urls);
        if (page.isEmpty())
            toast(Tone::Warn, QStringLiteral("Nothing to do with that file"), QStringLiteral("Drop a game folder, an image or a package."));
        else
            navigate(page, {{QStringLiteral("source"), QFileInfo(urls.first().toLocalFile()).absoluteFilePath()}});
    }
    event->acceptProposedAction();
}

// ---- screenshots ---------------------------------------------------------------------------

int MainWindow::screenshotAllPages(const QString& dir)
{
    qApp->setProperty("fwScreenshotMode", true); // pages: demo data only, no user files touched
    QDir().mkpath(dir);
    int written = 0, index = 0;
    auto settle = [] {
        for (int i = 0; i < 6; ++i)
            QApplication::processEvents(QEventLoop::AllEvents, 50);
    };
    auto snap = [&](const QString& name) {
        settle();
        const QString file = QDir(dir).filePath(QStringLiteral("%1-%2.png").arg(index++, 2, 10, QLatin1Char('0')).arg(name));
        if (grab().save(file))
            ++written;
    };
    if (!isVisible())
        show();
    resize(1440, 920);
    setSidebarCompact(false);
    m_toasts->clear();
    m_focusRing->reset();
    for (const QString& key : m_order) {
        navigate(key);
        snap(key);
        pages::Page* p = m_pages.value(key);
        for (const QString& state : p->demoStates()) {
            p->applyDemoState(state);
            snap(key + u'-' + state);
            p->clearDemoState();
            m_focusRing->reset();
            settle();
        }
    }
    navigate(QStringLiteral("build"));
    m_toasts->show(Tone::Ok, QStringLiteral("Image built and verified"), QStringLiteral("Demon's Souls · 31.6 GB · 12 min 40 s"),
                   {{QStringLiteral("Show in folder"), {}}, {QStringLiteral("Open queue"), {}}}, 0);
    m_toasts->show(Tone::Err, QStringLiteral("Backport failed"), QStringLiteral("Returnal · eboot.bin not found in sce_module. Originals were not changed."),
                   {{QStringLiteral("Retry"), {}}, {QStringLiteral("Open log")}}, 0);
    snap(QStringLiteral("toasts"));
    m_toasts->clear();
    setSidebarCompact(true);
    snap(QStringLiteral("sidebar-compact"));
    setSidebarCompact(m_userCompact);
    // Keyboard focus ring: Tab into the page.
    navigate(QStringLiteral("settings"));
    m_sidebar->focusCurrent();
    snap(QStringLiteral("focus-ring"));
    m_focusRing->reset();
    resize(1100, 760);
    navigate(QStringLiteral("build"));
    snap(QStringLiteral("narrow-1100"));
    resize(1440, 920);
    navigate(QStringLiteral("home"));
    return written;
}

} // namespace fw
