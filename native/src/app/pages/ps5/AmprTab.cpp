#include "app/pages/ps5/AmprTab.h"

#include "app/AppContext.h"
#include "app/pages/ps5/ConsoleFolderPicker.h"
#include "app/services/JobSubmit.h"
#include "app/ui/Sheet.h"
#include "connect/ps5/RemotePath.h"
#include "core/FileUtil.h"
#include "engines/pfs/AmprJobs.h"
#include "jobs/JobQueue.h"

#include <QFileInfo>

namespace fw::pages {

using namespace ui;
using namespace theme;

namespace {
const QString kLocalKind = QStringLiteral("ampr.index.local");
const QString kFtpKind = QStringLiteral("ampr.index.ftp");
} // namespace

AmprTab::AmprTab(AppContext& ctx, EndpointFn endpointFn, QWidget* parent)
    : ConsoleToolTab(ctx, std::move(endpointFn), QStringLiteral("ampr"), parent)
{
    auto* l = vbox(this, space::lg);
    auto* info = new InfoBar(Tone::Info, this);
    info->setMessage(QStringLiteral("ampr_emu.index lets the AMPR emulator (fakelib/libSceAmpr.sprx) find a game's files whatever "
                                    "their letter case. Image builds add it by themselves (Settings › PFS › AMPR file index); use "
                                    "this for a dump you do not rebuild, or for a game already on the console."));
    l->addWidget(info);

    auto* local = new Card(this);
    local->setTitle(QStringLiteral("From a folder on this PC"));
    m_localFolder = new PathField(PathField::Mode::Directory, local);
    m_localFolder->setDialogTitle(QStringLiteral("Choose the game's app0 folder"));
    local->body()->addWidget(field(QStringLiteral("Game folder (app0)"), m_localFolder,
                                   QStringLiteral("ampr_emu.index is written into this folder; an existing one is replaced once the new one is complete."), local));
    auto* lrow = new QWidget(local);
    auto* lh = hbox(lrow, space::sm);
    m_localStatus = new MessageRow(lrow);
    lh->addWidget(m_localStatus, 1, Qt::AlignVCenter);
    m_localGo = button(QStringLiteral("Generate"), ButtonVariant::Primary, QStringLiteral("file_plus"), lrow);
    lh->addStretch(0);
    lh->addWidget(m_localGo);
    local->body()->addWidget(lrow);
    l->addWidget(local);

    auto* remote = new Card(this);
    remote->setTitle(QStringLiteral("From a game folder on the PS5"));
    auto* rrow = new QWidget(remote);
    auto* rh = hbox(rrow, space::sm);
    m_remoteFolder = textInput(QStringLiteral("/data/etaHEN/games/PPSA01234-app0"), true, rrow);
    m_remoteFolder->setAccessibleName(QStringLiteral("Game folder on the console"));
    m_remoteBrowse = button(QStringLiteral("Browse…"), ButtonVariant::Secondary, QStringLiteral("folder_open"), rrow);
    rh->addWidget(m_remoteFolder, 1);
    rh->addWidget(m_remoteBrowse);
    remote->body()->addWidget(field(QStringLiteral("Game folder on the console (app0)"), rrow,
                                    QStringLiteral("Must hold eboot.bin or sce_sys. The folder is read over FTP, the index is built here and "
                                                   "uploaded next to it; an existing index is kept as ampr_emu.index.bak."), remote));
    auto* grow = new QWidget(remote);
    auto* gh = hbox(grow, space::sm);
    m_remoteStatus = new MessageRow(grow);
    gh->addWidget(m_remoteStatus, 1, Qt::AlignVCenter);
    m_remoteGo = button(QStringLiteral("Generate and upload"), ButtonVariant::Primary, QStringLiteral("upload"), grow);
    gh->addStretch(0);
    gh->addWidget(m_remoteGo);
    remote->body()->addWidget(grow);
    l->addWidget(remote);
    l->addWidget(caption(QStringLiteral("Both run in the queue; progress and the full result are on the Queue page."), this));
    l->addStretch(1);

    connect(m_localGo, &QPushButton::clicked, this, &AmprTab::generateLocal);
    connect(m_remoteGo, &QPushButton::clicked, this, &AmprTab::generateConsole);
    connect(m_remoteBrowse, &QPushButton::clicked, this, &AmprTab::browseConsole);
    connect(&m_ctx.queue(), &JobQueue::jobFinished, this, [this](const QString& id) {
        const auto rec = m_ctx.queue().record(id);
        if (!rec || (rec->kind != kLocalKind && rec->kind != kFtpKind))
            return;
        MessageRow* row = rec->kind == kLocalKind ? m_localStatus : m_remoteStatus;
        if (rec->state == JobState::Succeeded && rec->outcome)
            row->set(rec->outcome->summary, rec->outcome->warnings.isEmpty() ? Tone::Ok : Tone::Warn);
        else if (rec->error)
            row->set(rec->error->message, rec->error->isCancelled() ? Tone::Neutral : Tone::Err);
        else
            row->set(jobStateName(rec->state), Tone::Neutral);
        row->setToolTip(rec->outcome ? rec->outcome->warnings.join(u'\n') : QString());
    });
    updateAvailability();
}

void AmprTab::updateAvailability()
{
    m_localGo->setEnabled(jobs::isAvailable(m_ctx, kLocalKind));
    m_remoteGo->setEnabled(jobs::isAvailable(m_ctx, kFtpKind));
}

QString AmprTab::submitLocal(const QString& folder, bool force)
{
    auto r = jobs::submitAmprIndexLocal(m_ctx, folder, force);
    if (!r.ok()) {
        m_localStatus->set(r.error().message, Tone::Err);
        return {};
    }
    m_ctx.queue().holdQueue(false);
    m_localStatus->set(QStringLiteral("Queued: %1").arg(nativePath(folder)), Tone::Info);
    return r.value();
}

QString AmprTab::submitConsole(const QString& remoteFolder)
{
    const ps5::ConsoleEndpoint ep = endpoint();
    auto r = jobs::submitAmprIndexFtp(m_ctx, ep.host, ep.ftpPort, remoteFolder);
    if (!r.ok()) {
        m_remoteStatus->set(r.error().message, Tone::Err);
        return {};
    }
    m_ctx.queue().holdQueue(false);
    m_remoteStatus->set(QStringLiteral("Queued: %1").arg(remoteFolder), Tone::Info);
    return r.value();
}

void AmprTab::generateLocal()
{
    const QString folder = m_localFolder->path().trimmed();
    if (folder.isEmpty() || !QFileInfo(folder).isDir()) {
        m_localFolder->setError(folder.isEmpty() ? QStringLiteral("Choose the game's app0 folder.") : QStringLiteral("The folder does not exist."));
        return;
    }
    m_localFolder->setError({});
    bool force = false;
    if (!pfs::folderHasAmprLibrary(folder)) {
        // Legacy "Not an APR title?" question.
        if (!Sheet::confirm(window(), QStringLiteral("Not an APR title?"),
                            QStringLiteral("This folder has no fakelib/libSceAmpr.sprx, so it does not appear to be an APR title. Generate ampr_emu.index anyway?"),
                            QStringLiteral("Cancel"), QStringLiteral("Generate anyway"), false, Sheet::Icon::Warning))
            return;
        force = true;
    }
    if (!submitLocal(folder, force).isEmpty())
        emit toastRequested(Tone::Info, QStringLiteral("AMPR index queued"), nativePath(folder));
}

void AmprTab::generateConsole()
{
    const QString raw = m_remoteFolder->text().trimmed();
    if (!raw.startsWith(u'/')) {
        setInputError(m_remoteFolder, true);
        m_remoteStatus->set(QStringLiteral("Enter the game's folder on the console, starting with \"/\"."), Tone::Err);
        return;
    }
    setInputError(m_remoteFolder, false);
    const QString folder = ps5::normalizeRemotePath(raw);
    const ps5::ConsoleEndpoint ep = endpoint();
    if (ep.host.isEmpty()) {
        m_remoteStatus->set(QStringLiteral("Enter the console's IP address at the top of the page first."), Tone::Err);
        return;
    }
    if (!Sheet::confirm(window(), QStringLiteral("Generate and upload?"),
                        QStringLiteral("Build ampr_emu.index from %1 and upload it to that folder on the PS5? An existing index is kept as ampr_emu.index.bak.")
                            .arg(Sheet::monoSpan(QStringLiteral("ftp://%1:%2%3").arg(ep.host).arg(ep.ftpPort).arg(folder))),
                        QStringLiteral("Cancel"), QStringLiteral("Generate and upload"), false, Sheet::Icon::Info))
        return;
    if (!submitConsole(folder).isEmpty())
        emit toastRequested(Tone::Info, QStringLiteral("AMPR index queued"), folder);
}

void AmprTab::browseConsole()
{
    const ps5::ConsoleEndpoint ep = endpoint();
    if (ep.host.isEmpty()) {
        m_remoteStatus->set(QStringLiteral("Enter the console's IP address at the top of the page first."), Tone::Err);
        return;
    }
    const QString start = m_remoteFolder->text().trimmed().startsWith(u'/') ? m_remoteFolder->text().trimmed() : ep.gamesFolder;
    const QString chosen = pickConsoleFolder(window(), ep, start, QStringLiteral("Choose the game's app0 folder"), QStringLiteral("Use this folder"));
    if (!chosen.isEmpty()) {
        m_remoteFolder->setText(chosen);
        setInputError(m_remoteFolder, false);
    }
}

void AmprTab::applyDemo()
{
    m_localFolder->setPath(QStringLiteral("D:/Dumps/PPSA01234-app0"));
    m_localStatus->set(QStringLiteral("Indexed 18,442 file(s) into D:\\Dumps\\PPSA01234-app0\\ampr_emu.index (1.4 MB)"), Tone::Ok);
    m_remoteFolder->setText(QStringLiteral("/mnt/usb0/games/PPSA05678-app0"));
    m_remoteStatus->set(QStringLiteral("Indexed 9,120 file(s) and uploaded ampr_emu.index to /mnt/usb0/games/PPSA05678-app0 (old index kept as ampr_emu.index.bak)"), Tone::Ok);
}

void AmprTab::clearDemo()
{
    m_localFolder->setPath({});
    m_localFolder->setError({});
    m_localStatus->clear();
    m_remoteFolder->clear();
    m_remoteStatus->clear();
}

} // namespace fw::pages
