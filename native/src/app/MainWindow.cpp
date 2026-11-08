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
#include "app/ui/Icons.h"
#include "app/ui/Theme.h"
#include "app/ui/Widgets.h"
#include "connect/notify/DiscordWebhook.h"
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
#include <QJsonArray>
#include <QJsonDocument>
#include <QMenu>
#include <QMimeData>
#include <QProcess>
#include <QSettings>
#include <QShortcut>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QUrl>

#include <algorithm>
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
    m_logModel = new LogModel(ctx, this);
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
        menu.addAction(QStringLiteral("Open logs folder"), this, [] { openInFileManager(Paths::logsDir()); });
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
        m_tray = new QSystemTrayIcon(QIcon(QStringLiteral(":/icons/drive.svg")), this);
        m_tray->setToolTip(windowTitle());
        connect(m_tray, &QSystemTrayIcon::messageClicked, this, [this] {
            showNormal();
            activateWindow();
            navigate(QStringLiteral("queue"));
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
    m_logsPage = new pages::LogsPage(m_ctx, *m_logModel, this);
    addPage(m_logsPage, QStringLiteral("file_text"), QStringLiteral("Logs"));
    m_settingsPage = new pages::SettingsPage(m_ctx, this);
    addPage(m_settingsPage, QStringLiteral("settings"), QStringLiteral("Settings"));
    m_sidebar->addStretch();
    m_aboutPage = new pages::AboutPage(m_ctx, this);
    addPage(m_aboutPage, QStringLiteral("credits"), QStringLiteral("About"), true);
    connect(m_aboutPage, &pages::AboutPage::checkForUpdatesRequested, this, [this] { checkForUpdates(false); });
    connect(m_aboutPage, &pages::AboutPage::reportProblemRequested, this, [this] { reportProblem(); });
    connect(m_aboutPage, &pages::AboutPage::shortcutsRequested, this, &MainWindow::showShortcuts);
    connect(m_ps5Page, &pages::Ps5ConnectPage::reachabilityChanged, this, [this](bool ok, const QString& host) {
        m_header->setConsoleChip(ok ? QStringLiteral("PS5 reachable · %1").arg(host) : QStringLiteral("PS5 not reachable"), ok ? Tone::Ok : Tone::Neutral, true);
        m_home->setConsoleStatus(ok ? host : QStringLiteral("Not reachable"), ok ? Tone::Ok : Tone::Neutral);
    });
    connect(m_settingsPage, &pages::SettingsPage::attentionChanged, this, &MainWindow::updateStatus);
}

void MainWindow::buildShortcuts()
{
    auto add = [this](const QKeySequence& seq, auto fn) {
        auto* s = new QShortcut(seq, this);
        s->setContext(Qt::WindowShortcut);
        connect(s, &QShortcut::activated, this, fn);
    };
    add(QKeySequence(QStringLiteral("Ctrl+B")), [this] {
        if (auto* p = currentPageWidget(); p && p->hasPrimaryAction())
            p->primaryAction();
    });
    add(QKeySequence(QStringLiteral("Ctrl+L")), [this] { navigate(QStringLiteral("logs")); });
    add(QKeySequence(QStringLiteral("Ctrl+,")), [this] { navigate(QStringLiteral("settings")); });
    add(QKeySequence::Quit, [this] { close(); });
    add(QKeySequence(QStringLiteral("Ctrl+Q")), [this] { close(); });
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
    if (m_stack->currentWidget() != p) {
        m_stack->setCurrentWidget(p);
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
        if (rec->state == JobState::Running || rec->state == JobState::Paused)
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
            rec && !jobs::isSystemKind(rec->kind) && (rec->state == JobState::Running || rec->state == JobState::Paused)) {
            running = rec;
            break;
        }
    if (running) {
        const QString pct = running->progress.indeterminate ? QString() : QStringLiteral(" · %1").arg(thinPercent(running->progress.overallFraction));
        m_status->setStatus(QStringLiteral("%1 %2%3").arg(running->state == JobState::Paused ? QStringLiteral("Paused:") : QStringLiteral("Running:"), running->title, pct),
                            running->state == JobState::Paused ? Tone::Neutral : Tone::Info);
    } else if (const int n = m_settingsPage ? m_settingsPage->attentionCount() : 0; n > 0) {
        m_status->setStatus(QStringLiteral("%1 setting%2 need%3 attention").arg(n).arg(n == 1 ? QString() : QStringLiteral("s")).arg(n == 1 ? QStringLiteral("s") : QString()), Tone::Warn);
    } else if (m_ctx.queue().isHeld() && queuedUser > 0) {
        m_status->setStatus(QStringLiteral("Queue paused"), Tone::Neutral);
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
    const QStorageInfo si(folder);
    if (si.isValid() && si.isReady())
        m_status->setFreeSpace(QStringLiteral("%1 %2 free").arg(pages::driveLabel(folder), formatBytes(si.bytesAvailable())));
    else
        m_status->setFreeSpace({});
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
    const QString kindName = m_ctx.jobFactory().displayName(rec->kind);
    QList<ToastHost::Action> actions;
    if (rec->state == JobState::Succeeded) {
        const QString out = rec->outcome && !rec->outcome->outputs.isEmpty() ? rec->outcome->outputs.first() : QString();
        if (!out.isEmpty())
            actions << ToastHost::Action{QStringLiteral("Show in folder"), [out] { openInFileManager(out); }};
        actions << ToastHost::Action{QStringLiteral("Open queue"), [this, id] { navigate(QStringLiteral("queue"), {{QStringLiteral("jobId"), id}}); }};
        if (rec->kind == QLatin1String("tools.report") && id == m_reportJobId) {
            m_reportJobId.clear();
            auto* s = new Sheet(this, QStringLiteral("Problem report ready"), Sheet::Icon::Info);
            s->addText(QStringLiteral("The report was saved as %1. Attach it to a new GitHub issue and describe what happened.").arg(Sheet::monoSpan(nativePath(out))));
            s->addButton(QStringLiteral("Close"), Sheet::Role::Safe);
            s->addButton(QStringLiteral("Show in folder"), Sheet::Role::Primary);
            s->addButton(QStringLiteral("Open GitHub issue"), Sheet::Role::Primary);
            const int r = s->exec();
            if (r == 1)
                openInFileManager(out);
            else if (r == 2)
                QDesktopServices::openUrl(QUrl(rec->outcome ? rec->outcome->details.value(QStringLiteral("issueUrl")).toString(tools::kNewIssueUrl) : tools::kNewIssueUrl));
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
        actions << ToastHost::Action{QStringLiteral("Retry"), [this, id] { (void)m_ctx.queue().retry(id); m_ctx.queue().holdQueue(false); }};
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

void MainWindow::checkForUpdates(bool silent)
{
    // Automated tests and CI runs set FW_OFFLINE so nothing contacts GitHub.
    if (silent && qEnvironmentVariableIsSet("FW_OFFLINE"))
        return;
    auto r = jobs::submitUpdateCheck(m_ctx);
    if (!r.ok()) {
        if (!silent)
            toast(Tone::Warn, QStringLiteral("Update check not available"), r.error().message);
        return;
    }
    m_updateCheckId = r.value();
    m_ctx.queue().holdQueue(false);
    if (!silent)
        m_aboutPage->setUpdateStatus(QStringLiteral("Checking…"), Tone::Info);
}

void MainWindow::onUpdateCheckFinished(const JobRecord& rec)
{
    m_updateCheckId.clear();
    if (rec.state != JobState::Succeeded || !rec.outcome) {
        m_aboutPage->setUpdateStatus(QStringLiteral("Check failed"), Tone::Warn);
        return;
    }
    m_release = rec.outcome->details;
    const bool newer = m_release.value(QStringLiteral("newer")).toBool() || m_release.value(QStringLiteral("newerThanRunning")).toBool();
    const QString version = m_release.value(QStringLiteral("version")).toString();
    if (!newer) {
        m_aboutPage->setUpdateStatus(QStringLiteral("Up to date"), Tone::Ok);
        return;
    }
    m_aboutPage->setUpdateStatus(QStringLiteral("Version %1 available").arg(version), Tone::Info);
    if (m_updateBanner)
        m_updateBanner->deleteLater();
    auto* bar = new InfoBar(Tone::Info, this);
    bar->setDismissable(true);
    bar->setTitle(QStringLiteral("Version %1 is available").arg(version));
    bar->setMessage(QStringLiteral("A newer exFAT Image Builder is available. The download is checked against its SHA-256 before it runs."));
    auto* notes = bar->addLink(QStringLiteral("What's new"));
    const QString page = m_release.value(QStringLiteral("pageUrl")).toString();
    connect(notes, &Link::activated, this, [page] { QDesktopServices::openUrl(QUrl(page)); });
    auto* install = bar->addLink(QStringLiteral("Download and install"));
    connect(install, &Link::activated, this, [this] {
        auto r = jobs::submitUpdateDownload(m_ctx, m_release);
        if (!r.ok()) {
            toast(Tone::Err, QStringLiteral("Download not started"), r.error().message);
            return;
        }
        m_updateDownloadId = r.value();
        m_ctx.queue().holdQueue(false);
        toast(Tone::Info, QStringLiteral("Downloading the update"), QStringLiteral("You can keep working; the installer starts when the download is verified."));
    });
    m_updateBanner = bar;
    showBanner(bar);
}

void MainWindow::onUpdateDownloadFinished(const JobRecord& rec)
{
    m_updateDownloadId.clear();
    if (rec.state != JobState::Succeeded || !rec.outcome || rec.outcome->outputs.isEmpty()) {
        toast(Tone::Err, QStringLiteral("Update download failed"), rec.error ? rec.error->message : QStringLiteral("The installer could not be verified."));
        return;
    }
    installUpdate(rec.outcome->outputs.first());
}

void MainWindow::installUpdate(const QString& installer)
{
    if (m_ctx.queue().isBusy()) {
        m_pendingInstaller = installer;
        auto* bar = new InfoBar(Tone::Info, this);
        bar->setDismissable(true);
        bar->setTitle(QStringLiteral("Install after the queue finishes"));
        bar->setMessage(QStringLiteral("The update is downloaded and verified. It installs when no job is running."));
        auto* now = bar->addLink(QStringLiteral("Install now"));
        connect(now, &Link::activated, this, [this, installer] {
            if (m_ctx.queue().isBusy()) {
                toast(Tone::Warn, QStringLiteral("Jobs are still running"), QStringLiteral("Wait for the queue or cancel the jobs first."));
                return;
            }
            installUpdate(installer);
        });
        showBanner(bar);
        auto* conn = new QMetaObject::Connection;
        *conn = connect(&m_ctx.queue(), &JobQueue::idle, this, [this, installer, conn] {
            QObject::disconnect(*conn);
            delete conn;
            if (!m_pendingInstaller.isEmpty())
                installUpdate(installer);
        });
        return;
    }
    m_pendingInstaller.clear();
    if (QProcess::startDetached(installer, {})) {
        m_closing = true;
        QApplication::quit();
    } else {
        toast(Tone::Err, QStringLiteral("The installer did not start"), nativePath(installer));
    }
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
    bar->setMessage(QStringLiteral("A crash report was saved. Sending a problem report helps fix it; it contains logs and the crash file, never your games."));
    auto* report = bar->addLink(QStringLiteral("Create a problem report"));
    connect(report, &Link::activated, this, [this, files, newest, bar] {
        QSettings().setValue(QStringLiteral("crash/lastAcknowledged"), newest);
        bar->hide();
        emit bar->dismissed();
        reportProblem(files);
    });
    connect(bar, &InfoBar::dismissed, this, [newest] { QSettings().setValue(QStringLiteral("crash/lastAcknowledged"), newest); });
    m_crashBanner = bar;
    showBanner(bar);
}

void MainWindow::reportProblem(const QStringList& extraFiles)
{
    auto* s = new Sheet(this, QStringLiteral("Report a problem"), Sheet::Icon::Info, size::dialogWide);
    s->addText(QStringLiteral("A ZIP is written to your Desktop with: the app's log files, the settings, queue and history files, the dependency statuses and a system summary%1. "
                              "It never contains game files. Attach it to a GitHub issue afterwards.")
                   .arg(extraFiles.isEmpty() ? QString() : QStringLiteral(", and the crash report")));
    auto* redact = s->addCheckbox(QStringLiteral("Hide my user name in paths"));
    redact->setChecked(m_ctx.settings().value(tools::keys::kRedactReports).toBool());
    const bool available = jobs::isAvailable(m_ctx, QStringLiteral("tools.report"));
    if (!available)
        s->body()->addWidget(new UnavailableState(QStringLiteral("The problem report"), QStringLiteral("tools.report"), s));
    s->addButton(QStringLiteral("Cancel"), Sheet::Role::Safe);
    auto* create = s->addButton(QStringLiteral("Create report"), Sheet::Role::Primary);
    create->setEnabled(available);
    if (s->exec() != 1)
        return;
    (void)m_ctx.settings().setValue(tools::keys::kRedactReports, redact->isChecked());
    QJsonArray deps;
    for (const auto& d : m_ctx.dependencies().lastAll())
        deps.append(d.toJson());
    QJsonObject diagnostics{{QStringLiteral("version"), QString::fromLatin1(build::kVersion)},
                            {QStringLiteral("dependencies"), deps},
                            {QStringLiteral("jobKinds"), QJsonArray::fromStringList(m_ctx.jobFactory().kinds())},
                            {QStringLiteral("settingsCount"), int(m_ctx.schema().all().size())},
                            {QStringLiteral("extraFiles"), QJsonArray::fromStringList(extraFiles)}};
    const QString desktop = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    auto r = jobs::submitProblemReport(m_ctx, desktop.isEmpty() ? QDir::homePath() : desktop, diagnostics);
    if (!r.ok()) {
        toast(Tone::Err, QStringLiteral("Report not created"), r.error().message);
        return;
    }
    m_reportJobId = r.value();
    m_ctx.queue().holdQueue(false);
    toast(Tone::Info, QStringLiteral("Creating the problem report"), QStringLiteral("It appears on the Desktop in a moment."));
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
    if (!geo.isEmpty())
        restoreGeometry(geo);
    else
        resize(1440, 920);
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

void MainWindow::resizeEvent(QResizeEvent* event)
{
    QMainWindow::resizeEvent(event);
    const bool narrow = width() < size::sidebarBreakpoint;
    if (narrow != m_autoCompact) {
        m_autoCompact = narrow;
        setSidebarCompact(m_userCompact || m_autoCompact);
    }
}

void MainWindow::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape) {
        for (int r = 0; r < m_queueModel->rowCount(); ++r)
            if (const JobRecord* rec = m_queueModel->recordAt(r); rec && (rec->state == JobState::Running || rec->state == JobState::Paused)) {
                m_queuePage->cancelWithConfirm(rec->id);
                return;
            }
    }
    QMainWindow::keyPressEvent(event);
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    // A backport run or restore in progress (the PS5 Backport engine runs it):
    // cancel at the next safe point and wait for the worker (an apply in
    // progress is rolled back from the verified backup first).
    if (auto* bpPage = qobject_cast<pages::BackportPage*>(m_pages.value(QStringLiteral("backport"))); bpPage && bpPage->isBusy()) {
        const bool stop = Sheet::confirm(this, QStringLiteral("Cancel the backport run?"),
                                         QStringLiteral("A backport run is in progress. Cancel it and close?\n\nAn apply in progress is rolled back from the verified backup first."),
                                         QStringLiteral("Keep running"), QStringLiteral("Cancel and close"), true, Sheet::Icon::Warning);
        if (!stop) {
            event->ignore();
            return;
        }
        bpPage->cancelAndWait(120000);
    }
    if (!m_closing && m_ctx.queue().isBusy() && m_ctx.settings().value(general::keys::kConfirmOnClose).toBool()) {
        const bool stop = Sheet::confirm(this, QStringLiteral("Stop the running jobs?"),
                                         QStringLiteral("Jobs are still running. Closing stops them; partial output is removed and they come back as Interrupted so you can retry."),
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

void MainWindow::dragEnterEvent(QDragEnterEvent* event)
{
    if (!event->mimeData()->hasUrls())
        return;
    event->acceptProposedAction();
    m_dragging = true;
    if (auto* p = currentPageWidget())
        p->setDragHighlight(p->acceptsDrop(event->mimeData()->urls()));
}

void MainWindow::dragMoveEvent(QDragMoveEvent* event)
{
    if (event->mimeData()->hasUrls())
        event->acceptProposedAction();
}

void MainWindow::dragLeaveEvent(QDragLeaveEvent*)
{
    m_dragging = false;
    if (auto* p = currentPageWidget())
        p->setDragHighlight(false);
}

void MainWindow::dropEvent(QDropEvent* event)
{
    m_dragging = false;
    const auto urls = event->mimeData()->urls();
    if (auto* p = currentPageWidget())
        p->setDragHighlight(false);
    if (urls.isEmpty())
        return;
    pages::Page* p = currentPageWidget();
    if (p && p->acceptsDrop(urls)) {
        p->dropUrls(urls);
    } else {
        const QFileInfo fi(urls.first().toLocalFile());
        if (fi.isDir())
            navigate(QStringLiteral("build"), {{QStringLiteral("source"), fi.absoluteFilePath()}});
        else if (!jobs::imageKindForFile(fi.absoluteFilePath(), QStringLiteral("verify")).isEmpty())
            navigate(QStringLiteral("extract"), {{QStringLiteral("source"), fi.absoluteFilePath()}});
        else
            toast(Tone::Warn, QStringLiteral("Nothing to do with that file"), QStringLiteral("Drop a game folder, an image or a package."));
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
    setSidebarCompact(m_userCompact || m_autoCompact);
    // Keyboard focus ring: Tab into the page.
    navigate(QStringLiteral("settings"));
    setAttribute(Qt::WA_KeyboardFocusChange, true);
    m_sidebar->focusCurrent();
    snap(QStringLiteral("focus-ring"));
    resize(1100, 760);
    navigate(QStringLiteral("build"));
    snap(QStringLiteral("narrow-1100"));
    resize(1440, 920);
    navigate(QStringLiteral("home"));
    return written;
}

} // namespace fw
