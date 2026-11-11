#include "app/pages/ps5/ConsoleExtras.h"

#include "app/AppContext.h"
#include "app/services/JobSubmit.h"
#include "app/ui/Components.h"
#include "connect/ps5/Module.h"
#include "connect/ps5/MountConfig.h"
#include "connect/ps5/PayloadSender.h"
#include "connect/ps5/SmpApi.h"
#include "core/FileUtil.h"
#include "core/Tr.h"
#include "jobs/JobQueue.h"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonObject>
#include <QPlainTextEdit>
#include <QSpinBox>
#include <QStandardPaths>

namespace fw::pages {

using namespace ui;
using namespace theme;

// ---- ShadowMount+ log ---------------------------------------------------------------------

SmpLogTab::SmpLogTab(AppContext& ctx, EndpointFn endpointFn, QWidget* parent)
    : ConsoleToolTab(ctx, std::move(endpointFn), QStringLiteral("smplog"), parent)
{
    auto* l = vbox(this, space::lg);
    auto* card = new Card(this, Card::Variant::Table);
    auto* h = card->header();
    h->setSpacing(space::md);
    m_which = new Segmented(card);
    m_which->addSegment(FW_TR("This run"), QStringLiteral("play"), QStringLiteral("current"));
    m_which->addSegment(FW_TR("Previous run"), QStringLiteral("history"), QStringLiteral("previous"));
    m_which->setCurrentIndex(0);
    m_which->setAccessibleName(FW_TR("Which debug.log"));
    h->addWidget(m_which);
    m_badge = new Badge(FW_TR("Not loaded"), Tone::Neutral, card);
    m_badge->setIconName({});
    h->addWidget(m_badge);
    h->addStretch(1);
    m_refresh = button(FW_TR("Load"), ButtonVariant::Primary, QStringLiteral("refresh"), card);
    auto* save = button(FW_TR("Save…"), ButtonVariant::Secondary, QStringLiteral("save"), card);
    h->addWidget(save);
    h->addWidget(m_refresh);
    m_view = new QPlainTextEdit(card);
    keepLeftToRight(m_view); // log, code or hex: left to right in any language
    m_view->setReadOnly(true);
    m_view->setFont(font::mono());
    m_view->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_view->setMinimumHeight(420);
    m_view->setPlaceholderText(FW_TR("ShadowMount+ writes /data/shadowmount/debug.log while debug=1. Load shows its last part."));
    m_view->setFrameShape(QFrame::NoFrame);
    card->body()->addWidget(m_view);
    m_source = caption(FW_TR("Through the ShadowMount+ 1.7 API when it answers, else over FTP."), card);
    card->footer()->addWidget(m_source, 1);
    l->addWidget(card);
    connect(m_refresh, &QPushButton::clicked, this, &SmpLogTab::fetch);
    connect(m_which, &Segmented::currentChanged, this, [this](int) {
        if (!m_view->toPlainText().isEmpty() || isBusy())
            fetch();
    });
    // One load at a time; a choice made while one runs is loaded right after it.
    connect(this, &ConsoleToolTab::busyChanged, this, [this](bool busy) {
        m_refresh->setEnabled(!busy);
        if (!busy && m_fetchPending) {
            m_fetchPending = false;
            fetch();
        }
    });
    connect(save, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getSaveFileName(window(), FW_TR("Save debug.log"),
                                                          QDir::homePath() + QStringLiteral("/shadowmount_debug_%1.log").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"))),
                                                          FW_TR("Logs") + QStringLiteral(" (*.log *.txt)"));
        if (path.isEmpty())
            return;
        auto st = writeFileAtomic(path, m_view->toPlainText().toUtf8());
        emit toastRequested(st.ok() ? Tone::Ok : Tone::Err, st.ok() ? FW_TR("Log saved") : FW_TR("Not saved"),
                            st.ok() ? nativePath(path) : st.error().message);
    });
}

QString SmpLogTab::text() const
{
    return m_view->toPlainText();
}

QString SmpLogTab::sourceText() const
{
    return m_source->text();
}

void SmpLogTab::showText(const QString& text, const QString& source)
{
    m_view->setPlainText(text);
    m_view->moveCursor(QTextCursor::End);
    m_source->setText(source);
    const int lines = int(text.count(u'\n'));
    m_badge->set(FW_TR("%1 lines").arg(formatCount(lines)), Tone::Info);
}

void SmpLogTab::fetch()
{
    const bool previous = m_which->currentValue().toString() == u"previous";
    const ps5::ConsoleEndpoint ep = endpoint();
    if (ep.host.isEmpty()) {
        m_source->setText(FW_TR("Enter the console's IP address at the top of the page first."));
        return;
    }
    if (isBusy()) {
        m_fetchPending = true; // the running load is for the other choice: this one follows it
        return;
    }
    m_badge->set(FW_TR("Loading…"), Tone::Info);
    struct Got {
        QString text, source;
    };
    // The API only serves the current log; the previous one (debug.log.1) comes over FTP.
    runLocal<Got>(
        [ep, previous](const CancellationToken& t) -> Result<Got> {
            constexpr qint64 kTail = 256 * 1024;
            if (!previous) {
                ps5::smp::ApiOptions api;
                api.connectTimeoutMs = 1500;
                if (auto r = ps5::smp::debugLog(ep.host, ep.smpApiPort, int(kTail), api, t); r.ok())
                    return Got{r.value(), FW_TR("Last %1 of debug.log through the ShadowMount+ API (port %2).").arg(formatBytes(kTail)).arg(ep.smpApiPort)};
                else if (r.error().isCancelled())
                    return r.takeError();
            }
            ps5::FtpClient c(ps5::ftpOptionsFor(ep));
            FW_TRY(c.open(ep.host, ep.ftpPort, t));
            const QString path = previous ? ps5::mountPreviousDebugLogRemotePath(ps5::MountTool::ShadowMountPlus)
                                          : ps5::mountDebugLogRemotePath(ps5::MountTool::ShadowMountPlus);
            auto bytes = c.download(path, t);
            c.quit();
            if (!bytes.ok()) {
                if (bytes.error().code == ErrorCode::NotFound)
                    return Error(ErrorCode::NotFound, QStringLiteral("%1 is not on the console (debug=0, or ShadowMount+ has not run yet).").arg(path));
                return bytes.takeError();
            }
            QByteArray b = bytes.value();
            const bool cut = b.size() > kTail;
            if (cut)
                b = b.right(kTail);
            return Got{QString::fromUtf8(b), cut ? FW_TR("%1 over FTP, last %2.").arg(path, formatBytes(kTail)) : FW_TR("%1 over FTP.").arg(path)};
        },
        [this, gen = ++m_fetchGen](const Result<Got>& r) {
            if (gen != m_fetchGen)
                return; // superseded: the load for the current choice has started
            if (!r.ok()) {
                m_badge->set(FW_TR("Not loaded"), Tone::Err);
                m_source->setText(r.error().message);
                return;
            }
            showText(r->text, r->source);
        });
}

void SmpLogTab::applyDemo()
{
    showText(QStringLiteral("ShadowMount+ v1.7beta11 exFAT/UFS/PFS/LVD/MD. FW: 9.60. Build: Oct 29 2026\n"
                            "  [SCAN] roots: /data/etaHEN/games /mnt/usb0/games\n"
                            "  [IMG][LVD] mount complete: source=/mnt/usb0/games/PPSA01342.exfat mount=/mnt/shadowmnt/PPSA01342_8c1f unit=1\n"
                            "  [GAME] PPSA01342 registered (Astro Bot)\n"
                            "  [API] listening on 0.0.0.0:10101\n"
                            "[SHUTDOWN] requested by STOP file /data/shadowmount/STOP\n"
                            "[SHUTDOWN] cleanup complete for STOP file /data/shadowmount/STOP\n"),
             QStringLiteral("Example log, not from a console."));
}

void SmpLogTab::clearDemo()
{
    m_view->clear();
    m_badge->set(FW_TR("Not loaded"), Tone::Neutral);
    m_source->setText(FW_TR("Through the ShadowMount+ 1.7 API when it answers, else over FTP."));
}

// ---- crash dumps --------------------------------------------------------------------------

CrashDumpsTab::CrashDumpsTab(AppContext& ctx, EndpointFn endpointFn, QWidget* parent)
    : ConsoleToolTab(ctx, std::move(endpointFn), QStringLiteral("crashdumps"), parent)
{
    auto* l = vbox(this, space::lg);
    auto* card = new Card(this);
    card->setTitle(FW_TR("Capture crash dumps"));
    card->body()->addWidget(caption(FW_TR("Watches the console's crash folder while you reproduce a crash, then saves each new dump here as a zip."), card));
    auto* grid = new QWidget(card);
    auto* gh = hbox(grid, space::md);
    m_folder = new PathField(PathField::Mode::Directory, grid);
    const QString desktop = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    m_folder->setPlaceholder(QDir::toNativeSeparators(QDir(desktop.isEmpty() ? QDir::homePath() : desktop).filePath(QStringLiteral("PS5 Logs"))));
    m_folder->setDialogTitle(FW_TR("Save crash dumps to"));
    gh->addWidget(field(FW_TR("Save to"), m_folder, {}, grid), 1);
    m_minutes = new QSpinBox(grid);
    m_minutes->setRange(0, 24 * 60);
    m_minutes->setSpecialValueText(FW_TR("Until stopped"));
    m_minutes->setSuffix(QStringLiteral(" min"));
    m_minutes->setFixedWidth(150);
    gh->addWidget(field(FW_TR("Watch for"), m_minutes, {}, grid));
    card->body()->addWidget(grid);
    auto* row = new QWidget(card);
    auto* rh = hbox(row, space::sm);
    m_status = new MessageRow(row);
    rh->addWidget(m_status, 1);
    m_stop = button(FW_TR("Stop"), ButtonVariant::Secondary, QStringLiteral("stop"), row);
    m_stop->hide();
    m_start = button(FW_TR("Start watching"), ButtonVariant::Primary, QStringLiteral("play"), row);
    m_start->setEnabled(jobs::isAvailable(m_ctx, ps5::jobkinds::kCrashDumps));
    rh->addWidget(m_stop);
    rh->addWidget(m_start);
    card->body()->addWidget(row);
    l->addWidget(card);
    auto* tip = new InfoBar(Tone::Info, this);
    tip->setMessage(FW_TR("ShadowMount+ 1.7 also watches games for 60 s after launch and logs fatal signals and module errors to debug.log."));
    l->addWidget(tip);
    l->addStretch(1);
    connect(m_start, &QPushButton::clicked, this, &CrashDumpsTab::start);
    connect(m_stop, &QPushButton::clicked, this, [this] {
        if (!m_jobId.isEmpty())
            m_ctx.queue().cancel(m_jobId);
    });
    connect(&m_ctx.queue(), &JobQueue::jobChanged, this, &CrashDumpsTab::onJob);
    connect(&m_ctx.queue(), &JobQueue::jobFinished, this, &CrashDumpsTab::onJob);
    // A job that is taken out of the queue while it waits never finishes.
    connect(&m_ctx.queue(), &JobQueue::jobRemoved, this, [this](const QString& id) {
        if (id != m_jobId || !m_watching)
            return;
        setWatching(false);
        m_status->set(FW_TR("Removed from the queue before it started watching."), Tone::Neutral);
    });
}

void CrashDumpsTab::setWatching(bool on)
{
    m_watching = on;
    m_start->setVisible(!on);
    m_stop->setVisible(on);
}

void CrashDumpsTab::start()
{
    const ps5::ConsoleEndpoint ep = endpoint();
    if (ep.host.isEmpty()) {
        m_status->set(FW_TR("Enter the console's IP address at the top of the page first."), Tone::Warn);
        return;
    }
    auto id = jobs::submitCrashDumpCapture(m_ctx, ep.host, int(ep.ftpPort), m_minutes->value(), m_folder->path().trimmed());
    if (!id.ok()) {
        m_status->set(id.error().message, Tone::Err);
        return;
    }
    m_jobId = id.value();
    m_ctx.queue().startNow(id.value()); // this job only; a paused queue stays paused
    setWatching(true);
    onJob(m_jobId); // the status says whether it watches already or still waits
}

void CrashDumpsTab::onJob(const QString& id)
{
    if (id != m_jobId)
        return;
    const auto r = m_ctx.queue().record(id);
    if (!r)
        return;
    if (!isTerminal(r->state)) {
        if (r->state == JobState::Queued)
            m_status->set(FW_TR("Waiting for the job that is running. Watching starts after it."), Tone::Info);
        else if (!r->progress.currentItem.isEmpty())
            m_status->set(r->progress.currentItem, Tone::Info);
        else
            m_status->set(FW_TR("Watching. Reproduce the crash on the console now."), Tone::Info);
        return;
    }
    setWatching(false);
    if (r->state == JobState::Succeeded && r->outcome)
        m_status->set(r->outcome->summary, Tone::Ok);
    else
        m_status->set(r->error ? r->error->message : fw::i18n::translate(jobStateName(r->state)), r->state == JobState::Cancelled ? Tone::Neutral : Tone::Err);
}

void CrashDumpsTab::applyDemo()
{
    m_status->set(QStringLiteral("1 crash dump saved, 1 with a core, in Desktop\\PS5 Logs (example)"), Tone::Ok);
}

void CrashDumpsTab::clearDemo()
{
    m_status->clear();
}

// ---- install package ----------------------------------------------------------------------

InstallTab::InstallTab(AppContext& ctx, EndpointFn endpointFn, QWidget* parent)
    : ConsoleToolTab(ctx, std::move(endpointFn), QStringLiteral("install"), parent)
{
    auto* l = vbox(this, space::lg);
    auto* card = new Card(this);
    auto* badge = new Badge(QStringLiteral("etaHEN DPI v2"), Tone::Neutral, card);
    badge->setIconName({});
    card->setTitle(FW_TR("Install a package"), badge);
    m_drop = new DropZone(QStringLiteral("package_plus"), FW_TR("Drop a .pkg here"),
                          FW_TR("or click to browse — it is sent to the console's package installer"), card);
    card->body()->addWidget(m_drop);
    m_strip = new Card(card, Card::Variant::Inset);
    auto* strip = static_cast<Card*>(m_strip);
    auto* sh = new QHBoxLayout;
    sh->setSpacing(space::md);
    sh->addWidget(new IconTile(QStringLiteral("package"), 44, strip));
    auto* col = new QVBoxLayout;
    col->setSpacing(2);
    m_name = label({}, TextStyle::Subtitle, color::fg_0, strip);
    m_meta = label({}, TextStyle::MonoSmall, color::fg_4, strip);
    col->addWidget(m_name);
    col->addWidget(m_meta);
    sh->addLayout(col, 1);
    auto* change = button(FW_TR("Change…"), ButtonVariant::Secondary, QStringLiteral("folder"), strip);
    sh->addWidget(change);
    strip->body()->addLayout(sh);
    m_strip->hide();
    card->body()->addWidget(m_strip);
    auto* row = new QWidget(card);
    auto* rh = hbox(row, space::sm);
    m_status = new MessageRow(row);
    rh->addWidget(m_status, 1);
    auto* test = button(FW_TR("Test installer"), ButtonVariant::Secondary, QStringLiteral("plug"), row);
    m_install = button(FW_TR("Install on PS5"), ButtonVariant::Primary, QStringLiteral("upload"), row);
    m_install->setEnabled(false);
    rh->addWidget(test);
    rh->addWidget(m_install);
    card->body()->addWidget(row);
    l->addWidget(card);
    auto* how = new InfoBar(Tone::Info, this);
    how->setMessage(FW_TR("Needs etaHEN's Direct Package Installer V2: set DPI_v2=1 in /data/etaHEN/config.ini (or in the etaHEN toolbox). "
                                   "The package is staged in /user/data/tmp first, so internal storage needs room for it."));
    l->addWidget(how);
    l->addStretch(1);
    auto pick = [this] {
        const QString f = QFileDialog::getOpenFileName(window(), FW_TR("Choose a package"), {}, FW_TR("Packages") + QStringLiteral(" (*.pkg)"));
        if (!f.isEmpty())
            setPackage(f);
    };
    connect(m_drop, &DropZone::clicked, this, pick);
    connect(change, &QPushButton::clicked, this, pick);
    connect(m_install, &QPushButton::clicked, this, &InstallTab::install);
    connect(test, &QPushButton::clicked, this, &InstallTab::testInstaller);
}

void InstallTab::setPackage(const QString& path)
{
    m_package = path;
    const QFileInfo fi(path);
    m_drop->setVisible(path.isEmpty());
    m_strip->setVisible(!path.isEmpty());
    m_name->setText(fi.fileName());
    m_meta->setText(fi.exists() ? QStringLiteral("%1 · %2").arg(formatBytes(fi.size()), nativePath(fi.absolutePath())) : nativePath(path));
    m_install->setEnabled(!path.isEmpty() && fi.isFile() && jobs::isAvailable(m_ctx, ps5::jobkinds::kDpiInstall));
}

void InstallTab::install()
{
    const ps5::ConsoleEndpoint ep = endpoint();
    if (ep.host.isEmpty()) {
        m_status->set(FW_TR("Enter the console's IP address at the top of the page first."), Tone::Warn);
        return;
    }
    auto r = jobs::submitDpiInstall(m_ctx, ep.host, ep.dpiPort, m_package);
    if (!r.ok()) {
        m_status->set(r.error().message, Tone::Err);
        return;
    }
    m_ctx.queue().startNow(r.value()); // this job only; a paused queue stays paused
    m_status->set(FW_TR("Queued: %1 → %2:%3").arg(QFileInfo(m_package).fileName(), ep.host).arg(ep.dpiPort), Tone::Info);
    emit toastRequested(Tone::Info, FW_TR("Install queued"), QFileInfo(m_package).fileName());
}

void InstallTab::testInstaller()
{
    const ps5::ConsoleEndpoint ep = endpoint();
    if (ep.host.isEmpty()) {
        m_status->set(FW_TR("Enter the console's IP address at the top of the page first."), Tone::Warn);
        return;
    }
    m_status->set(FW_TR("Testing port %1…").arg(ep.dpiPort), Tone::Info);
    runLocal<bool>(
        [ep](const CancellationToken& t) -> Result<bool> {
            FW_TRY(ps5::probeTcpPort(ep.host, ep.dpiPort, ep.connectTimeoutMs, QStringLiteral("Direct Package Installer"), t));
            return true;
        },
        [this, ep](const Result<bool>& r) {
            m_status->set(r.ok() ? FW_TR("The installer answers on port %1.").arg(ep.dpiPort)
                                 : FW_TR("%1 Turn on DPI_v2 in etaHEN.").arg(r.error().message),
                          r.ok() ? Tone::Ok : Tone::Err);
        });
}

void InstallTab::applyDemo()
{
    m_package = QStringLiteral("D:/Packages/UP9000-PPSA01342_00-ASTROBOT00000000.pkg");
    m_drop->hide();
    m_strip->show();
    m_name->setText(QStringLiteral("UP9000-PPSA01342_00-ASTROBOT00000000.pkg"));
    m_meta->setText(QStringLiteral("8.42 GB · D:\\Packages"));
    m_install->setEnabled(true);
    m_status->set(QStringLiteral("The installer answers on port 12800. (example)"), Tone::Ok);
}

void InstallTab::clearDemo()
{
    setPackage({});
    m_status->clear();
}

} // namespace fw::pages
