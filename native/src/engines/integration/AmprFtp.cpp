// ampr.index.ftp — AMPR index for a game folder on the console.
// Walk and upload follow drakmor ampr_emu tools/build_ampr_index.py
// (collect_ftp_rows, upload with .bak; GPL-3.0) with the native FTP client.

#include "engines/integration/AmprFtp.h"

#include "connect/ps5/Endpoint.h"
#include "connect/ps5/FtpTransfer.h"
#include "connect/ps5/RemotePath.h"
#include "core/FileUtil.h"
#include "engines/pfs/AmprJobs.h"

#include <QJsonArray>

#include <algorithm>

namespace fw::integration {

namespace {

ps5::ConsoleEndpoint endpointFor(const QJsonObject& p, const SettingsValues& s)
{
    ps5::ConsoleEndpoint ep = ps5::endpointFromSettings(s);
    const QString host = p.value(QStringLiteral("host")).toString().trimmed();
    if (!host.isEmpty())
        ep.host = host;
    const int port = p.value(QStringLiteral("port")).toInt(0);
    if (port >= 1 && port <= 65535)
        ep.ftpPort = quint16(port);
    return ep;
}

QString remoteFolderOf(const QJsonObject& p)
{
    const QString f = p.value(QStringLiteral("remoteFolder")).toString().trimmed();
    return f.isEmpty() ? QString() : ps5::normalizeRemotePath(f);
}

class AmprFtpJob final : public Job {
public:
    explicit AmprFtpJob(QJsonObject p) : m_p(std::move(p)) {}
    QString kind() const override { return kAmprFtpJob; }
    QString title() const override
    {
        const QString f = remoteFolderOf(m_p);
        return QStringLiteral("AMPR index on PS5 · %1").arg(f.isEmpty() ? QStringLiteral("?") : ps5::remoteFileName(f));
    }
    QString subtitle() const override { return remoteFolderOf(m_p); }
    QStringList phases() const override
    {
        return {QStringLiteral("Checking the folder"), QStringLiteral("Reading the console folder"), QStringLiteral("Uploading")};
    }
    QList<double> phaseWeights() const override { return {0.05, 0.85, 0.10}; }
    QStringList busyPaths() const override { return {}; } // nothing local is read or written
    QJsonObject parameters() const override { return m_p; }

    Status validate(const SettingsValues& s) const override
    {
        if (endpointFor(m_p, s).host.isEmpty())
            return Error(ErrorCode::InvalidArgument, QStringLiteral("Enter your PS5 IP address in Settings > PS5 connection."));
        const QString raw = m_p.value(QStringLiteral("remoteFolder")).toString().trimmed();
        if (!raw.startsWith(u'/'))
            return Error(ErrorCode::InvalidArgument, QStringLiteral("Enter the game's app0 folder on the console, starting with \"/\"."));
        const QString f = remoteFolderOf(m_p);
        if (f == u"/")
            return Error(ErrorCode::InvalidArgument, QStringLiteral("Choose the game's app0 folder, not the console's root."));
        if (ps5::isProtectedRemotePath(f))
            return Error(ErrorCode::PermissionDenied, ps5::protectedPathWarning(f, QStringLiteral("write ampr_emu.index into")));
        return success();
    }

    Result<JobOutcome> run(JobContext& ctx) override
    {
        const ps5::ConsoleEndpoint ep = endpointFor(m_p, ctx.settings);
        const QString folder = remoteFolderOf(m_p);
        ps5::FtpClient c(ps5::ftpOptionsFor(ep));
        ctx.progress->beginPhase(0, 0);
        ctx.progress->setCurrentItem(QStringLiteral("%1:%2%3").arg(ep.host).arg(ep.ftpPort).arg(folder));
        FW_TRY(c.open(ep.host, ep.ftpPort, ctx.token));

        // 1. A game folder, not a parent of many games (one listing, no walk).
        FW_TRY_ASSIGN(std::vector<ps5::FtpEntry> top, c.list(folder, ps5::FtpListMode::Auto, ctx.token));
        if (const QString why = app0Problem(top, folder); !why.isEmpty())
            return Error(ErrorCode::InvalidArgument, why);

        // 2. Walk.
        ctx.progress->beginPhase(1, 0, 0, false);
        qint64 lastFiles = 0;
        FW_TRY_ASSIGN(AmprConsoleScan scan, scanConsoleApp0(c, folder, ctx.token, [&](int folders, int files) {
                          ctx.progress->setCurrentItem(QStringLiteral("%1 folder(s), %2 file(s)").arg(folders).arg(files));
                          ctx.progress->advanceFiles(files - lastFiles);
                          lastFiles = files;
                      }));
        const pfs::AmprToolRows rows = pfs::amprToolRows(scan.files);
        if (rows.rows.empty())
            return Error(ErrorCode::InvalidArgument, QStringLiteral("%1 on the console has no files to index.").arg(folder));
        const QByteArray index = pfs::buildAmprIndex(rows.rows);
        ctx.log.info(QStringLiteral("[AMPR] %1 file(s) in %2 folder(s) on %3:%4%5 indexed, %6")
                         .arg(rows.rows.size()).arg(scan.tree.folders).arg(ep.host).arg(ep.ftpPort).arg(folder, formatBytes(index.size())));

        // 3. Upload with a .bak of the old index, then read it back.
        ctx.progress->beginPhase(2, index.size());
        auto up = uploadAmprIndex(c, folder, index, ctx.token);
        if (!up.ok()) {
            if (up.error().isCancelled() || !c.isConnected())
                removeLeftoverTemp(ep, folder, ctx);
            return up.takeError();
        }
        ctx.progress->advance(index.size());
        JobOutcome o;
        pfs::describeAmprRows(rows, o);
        o.details.insert(QStringLiteral("folders"), scan.tree.folders);
        o.details.insert(QStringLiteral("remoteIndex"), up->remotePath);
        o.details.insert(QStringLiteral("backup"), up->backupPath);
        o.details.insert(QStringLiteral("indexBytes"), qint64(index.size()));
        o.details.insert(QStringLiteral("hasAmprLibrary"), pfs::hasAmprLibrary(scan.files));
        if (!pfs::hasAmprLibrary(scan.files))
            o.warnings << QStringLiteral("The folder has no fakelib/libSceAmpr.sprx; the index is only used by the AMPR emulator.");
        if (!scan.tree.unreadableDirs.isEmpty())
            o.warnings << QStringLiteral("%1 folder(s) could not be listed and are not in the index: %2")
                              .arg(scan.tree.unreadableDirs.size()).arg(scan.tree.unreadableDirs.mid(0, 3).join(QStringLiteral(", ")));
        if (!scan.tree.skippedNames.isEmpty())
            o.warnings << QStringLiteral("%1 name(s) from the console could not be used safely and are not in the index: %2")
                              .arg(scan.tree.skippedNames.size()).arg(scan.tree.skippedNames.mid(0, 3).join(QStringLiteral(", ")));
        if (scan.approximateTimes > 0)
            o.warnings << QStringLiteral("The console did not give exact times for %1 file(s); their listed times were used.").arg(scan.approximateTimes);
        auto back = c.download(up->remotePath, ctx.token);
        if (back.ok() && back.value() == index) {
            o.verification = Verification::Passed;
            o.verificationDetail = QStringLiteral("ampr_emu.index read back from the console, %1").arg(formatBytes(index.size()));
        } else {
            o.verification = Verification::Failed;
            o.verificationDetail = back.ok() ? QStringLiteral("ampr_emu.index on the console differs from the one built")
                                             : QStringLiteral("ampr_emu.index could not be read back: %1").arg(back.error().message);
        }
        o.summary = QStringLiteral("Indexed %1 file(s) and uploaded ampr_emu.index to %2%3")
                        .arg(formatCount(qint64(rows.rows.size())), folder,
                             up->backupPath.isEmpty() ? QString() : QStringLiteral(" (old index kept as ampr_emu.index.bak)"));
        c.quit();
        return o;
    }

private:
    // After a cancel or a dropped connection the temporary may be left behind.
    static void removeLeftoverTemp(const ps5::ConsoleEndpoint& ep, const QString& folder, JobContext& ctx)
    {
        ps5::FtpClient fresh(ps5::ftpOptionsFor(ep));
        if (!fresh.open(ep.host, ep.ftpPort).ok())
            return;
        const QString tmp = ps5::joinRemote(folder, pfs::kAmprIndexName + QStringLiteral(".tmp"));
        if (fresh.fileExists(tmp) && fresh.dele(tmp).ok())
            ctx.log.info(QStringLiteral("[AMPR] Removed the partial %1").arg(tmp));
        fresh.quit();
    }

    QJsonObject m_p;
};

} // namespace

QString app0Problem(const std::vector<ps5::FtpEntry>& entries, const QString& folder)
{
    int dirs = 0;
    for (const ps5::FtpEntry& e : entries) {
        if (e.isDir)
            ++dirs;
        if ((!e.isDir && e.name.compare(QLatin1String("eboot.bin"), Qt::CaseInsensitive) == 0)
            || (e.isDir && e.name.compare(QLatin1String("sce_sys"), Qt::CaseInsensitive) == 0))
            return {};
    }
    if (entries.empty())
        return QStringLiteral("%1 on the console is empty.").arg(folder);
    return QStringLiteral("This looks like a parent folder, not a game app0 folder: no eboot.bin or sce_sys was found in "
                          "%1 (%2 subfolder(s) present). Open the specific game's folder, then try again.")
        .arg(folder)
        .arg(dirs);
}

Result<AmprConsoleScan> scanConsoleApp0(ps5::FtpClient& c, const QString& remoteFolder, const CancellationToken& token,
                                        const ps5::RemoteWalkProgress& progress)
{
    ps5::RemoteWalkOptions opt;
    opt.fillMissingSizes = true;
    opt.exactTimes = true;
    AmprConsoleScan out;
    FW_TRY_ASSIGN(out.tree, ps5::walkRemoteTree(c, remoteFolder, opt, token, progress));
    out.files.reserve(out.tree.files.size());
    for (const ps5::RemoteFile& f : out.tree.files) {
        if (!f.exactTime)
            ++out.approximateTimes;
        out.files.push_back({f.relPath.toUtf8(), std::max<qint64>(0, f.size),
                             f.modified.isValid() ? f.modified.toSecsSinceEpoch() : 0});
    }
    return out;
}

Result<AmprUpload> uploadAmprIndex(ps5::FtpClient& c, const QString& remoteFolder, const QByteArray& index,
                                   const CancellationToken& token)
{
    const QString folder = ps5::cleanRemotePath(remoteFolder);
    AmprUpload out;
    out.remotePath = ps5::joinRemote(folder, pfs::kAmprIndexName);
    const QString tmp = out.remotePath + QStringLiteral(".tmp");
    const QString bak = out.remotePath + QStringLiteral(".bak");
    if (auto st = c.upload(tmp, index, token); !st.ok()) {
        if (c.isConnected())
            (void)c.dele(tmp);
        return st.takeError();
    }
    const bool existed = c.fileExists(out.remotePath);
    if (existed) {
        if (c.fileExists(bak))
            (void)c.dele(bak);
        if (auto st = c.rename(out.remotePath, bak); !st.ok()) {
            (void)c.dele(tmp);
            return Error(ErrorCode::Unsupported,
                         QStringLiteral("The PS5 FTP server refused to rename the old ampr_emu.index, so nothing was changed."),
                         st.error().detail);
        }
        out.backupPath = bak;
    }
    if (auto st = c.rename(tmp, out.remotePath); !st.ok()) {
        QString restored;
        if (existed)
            restored = c.rename(bak, out.remotePath).ok() ? QStringLiteral(" The old index was put back.")
                                                           : QStringLiteral(" The old index is in ampr_emu.index.bak.");
        (void)c.dele(tmp);
        return Error(ErrorCode::Unsupported,
                     QStringLiteral("The PS5 FTP server refused to rename the new ampr_emu.index into place.%1").arg(restored),
                     st.error().detail);
    }
    return out;
}

void registerAmprFtpJob(ModuleContext& ctx)
{
    ctx.jobs.add(kAmprFtpJob, QStringLiteral("AMPR index (PS5)"), [](const QJsonObject& p) -> Result<std::unique_ptr<Job>> {
        return std::unique_ptr<Job>(new AmprFtpJob(p));
    });
}

} // namespace fw::integration
