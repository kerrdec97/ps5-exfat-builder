#include "connect/ps5/CrashDumps.h"

#include "connect/ps5/FtpClient.h"
#include "connect/ps5/FtpTransfer.h"
#include "core/FileUtil.h"
#include "core/Zip.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QSet>
#include <QThread>

#include <algorithm>
#include <memory>

namespace fw::ps5 {

QString safeCrashFolderName(const QString& name)
{
    QString out;
    out.reserve(name.size());
    for (const QChar c : name)
        out.append(c.unicode() < 0x20 || QStringLiteral("<>:\"/\\|?*").contains(c) ? QChar(u'_') : c);
    while (!out.isEmpty() && (out.endsWith(u' ') || out.endsWith(u'.')))
        out.chop(1);
    while (!out.isEmpty() && out.startsWith(u' '))
        out.remove(0, 1);
    return out.isEmpty() ? QStringLiteral("crash") : out;
}

QString crashIpTag(const QString& host)
{
    QString t = host.trimmed();
    t.replace(u':', u'_');
    t.replace(u'.', u'-');
    return t.isEmpty() ? QStringLiteral("console") : t;
}

namespace {

// Waits up to `ms`; false when cancelled.
bool pause(const CancellationToken& token, qint64 ms)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) {
        if (token.isCancelled())
            return false;
        QThread::msleep(quint64(std::clamp<qint64>(ms - t.elapsed(), 1, 50)));
    }
    return !token.isCancelled();
}

// One control connection, reopened after a failure.
class Session {
public:
    Session(const ConsoleEndpoint& ep, const CancellationToken& token) : m_ep(ep), m_token(token) {}

    FtpClient* client(QString* why = nullptr)
    {
        if (m_client && m_client->isConnected())
            return m_client.get();
        m_client = std::make_unique<FtpClient>(ftpOptionsFor(m_ep));
        if (Status st = m_client->open(m_ep.host, m_ep.ftpPort, m_token); !st.ok()) {
            if (why)
                *why = st.error().message;
            m_client.reset();
            return nullptr;
        }
        return m_client.get();
    }
    void drop()
    {
        if (m_client)
            m_client->abort();
        m_client.reset();
    }

private:
    ConsoleEndpoint m_ep;
    CancellationToken m_token;
    std::unique_ptr<FtpClient> m_client;
};

QString joinRemote(const QString& dir, const QString& name)
{
    return dir.endsWith(u'/') ? dir + name : dir + u'/' + name;
}

// Lists `path`: folders and files (by name), or an error.
Result<std::vector<FtpEntry>> listRemote(Session& s, const QString& path, const CancellationToken& token)
{
    QString why;
    FtpClient* c = s.client(&why);
    if (!c)
        return Error(ErrorCode::IoError, why.isEmpty() ? QStringLiteral("The console FTP service is not reachable.") : why);
    auto r = c->list(path, FtpListMode::Auto, token);
    if (!r.ok()) {
        // 550: the folder does not exist (yet, or any more).
        if (c->lastReply().code == 550)
            return std::vector<FtpEntry>{};
        s.drop();
    }
    return r;
}

// Downloads `remote` to `local` through "<local>.partial", up to `retries` tries.
Status download(Session& s, const QString& remote, const QString& local, int retries, const CancellationToken& token)
{
    const QString part = local + QStringLiteral(".partial");
    Status last = Error(ErrorCode::IoError, QStringLiteral("not downloaded"));
    for (int attempt = 1; attempt <= std::max(1, retries); ++attempt) {
        if (token.isCancelled())
            return Error::cancelled();
        QString why;
        FtpClient* c = s.client(&why);
        if (c) {
            QFile out(part);
            if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
                return Error(ErrorCode::IoError, QStringLiteral("Could not write %1.").arg(QDir::toNativeSeparators(part)),
                             out.errorString());
            last = c->retrieve(remote, out, 0, {}, token);
            out.close();
            if (last.ok()) {
                QFile::remove(local);
                if (QFile::rename(part, local))
                    return success();
                last = Error(ErrorCode::IoError, QStringLiteral("Could not save %1.").arg(QDir::toNativeSeparators(local)));
            } else {
                s.drop();
            }
        } else {
            last = Error(ErrorCode::IoError, why);
        }
        QFile::remove(part);
        if (!pause(token, std::min(50 * attempt, 350)))
            return Error::cancelled();
    }
    return last;
}

Status writeCrashZip(const QString& zipPath, const QString& localDir, const QString& folder, const QString& host)
{
    zip::Writer w;
    FW_TRY(w.open(zipPath));
    const QByteArray note = QStringLiteral("Crash dump saved by exFAT Image Builder from %1 at %2.\n"
                                           "Console folder: %3/%4\n")
                                .arg(host, QDateTime::currentDateTime().toString(Qt::ISODate),
                                     QString::fromLatin1(kCrashDumpRoot), folder)
                                .toUtf8();
    FW_TRY(w.addData(QStringLiteral("capture.txt"), note));
    const QStringList files = QDir(localDir).entryList(QDir::Files, QDir::Name);
    for (const QString& f : files) {
        if (f.endsWith(QStringLiteral(".partial")))
            continue;
        FW_TRY(w.addFile(QStringLiteral("crash_dumps/%1/%2").arg(QFileInfo(localDir).fileName(), f),
                         QDir(localDir).filePath(f), zip::Method::Deflate, CancellationToken()));
    }
    return w.finish(true);
}

// One crash folder, until complete or timed out.
CapturedCrash captureFolder(Session& s, const ConsoleEndpoint& ep, const CrashCaptureOptions& opt, const QString& folder,
                            const CancellationToken& token, const CrashCaptureStatus& status)
{
    CapturedCrash c;
    c.folder = folder;
    c.localDir = QDir(QDir(opt.outputDir).filePath(QStringLiteral("crash_dumps"))).filePath(safeCrashFolderName(folder));
    QDir().mkpath(c.localDir);
    const QString remoteDir = joinRemote(opt.root, folder);
    if (status)
        status(QStringLiteral("Game crashed - saving crash dump %1 ...").arg(folder));

    QMap<QString, qint64> got; // name → size saved
    QString signature;
    QElapsedTimer total, quiet;
    total.start();
    quiet.start();
    while (total.elapsed() < opt.folderTimeoutMs && !token.isCancelled()) {
        auto listing = listRemote(s, remoteDir, token);
        if (listing.ok()) {
            std::vector<FtpEntry> files;
            for (const FtpEntry& e : listing.value())
                if (!e.isDir && !e.name.isEmpty())
                    files.push_back(e);
            QStringList sig;
            for (const FtpEntry& e : files)
                sig << QStringLiteral("%1:%2").arg(e.name).arg(e.size);
            sig.sort();
            if (sig.join(u'|') != signature) {
                signature = sig.join(u'|');
                quiet.restart();
            }
            // The core first, then by name; only new or grown files.
            std::sort(files.begin(), files.end(), [](const FtpEntry& a, const FtpEntry& b) {
                const bool ca = a.name.endsWith(QStringLiteral(".prosperodmp")), cb = b.name.endsWith(QStringLiteral(".prosperodmp"));
                return ca != cb ? ca : a.name < b.name;
            });
            for (const FtpEntry& e : files) {
                if (got.contains(e.name) && (e.size < 0 || e.size <= got.value(e.name)))
                    continue;
                const QString local = QDir(c.localDir).filePath(safeCrashFolderName(e.name));
                if (download(s, joinRemote(remoteDir, e.name), local, opt.retries, token).ok()) {
                    got.insert(e.name, QFileInfo(local).size());
                    quiet.restart();
                    if (e.name.endsWith(QStringLiteral(".prosperodmp"))) {
                        c.hasCore = true;
                        if (status)
                            status(QStringLiteral("Crash dump secured: %1 (%2 MB)")
                                       .arg(e.name)
                                       .arg(double(QFileInfo(local).size()) / (1024.0 * 1024.0), 0, 'f', 1));
                    }
                }
            }
        }
        if (c.hasCore && quiet.elapsed() >= opt.settleMs)
            break;
        if (!pause(token, opt.pollMs))
            break;
    }
    // A finished core makes its temporary copy redundant.
    for (const QString& name : got.keys())
        if (name.endsWith(QStringLiteral(".prosperodmp.tmp")) && got.contains(name.chopped(4))) {
            QFile::remove(QDir(c.localDir).filePath(safeCrashFolderName(name)));
            got.remove(name);
        }
    c.files = got.keys();
    if (c.files.isEmpty()) {
        QDir().rmdir(c.localDir);
        c.warning = QStringLiteral("Crash folder %1 vanished before anything could be saved.").arg(folder);
        return c;
    }
    if (!c.hasCore)
        c.warning = QStringLiteral("Crash folder %1: only %2 saved - the console never wrote a .prosperodmp core.")
                        .arg(folder, c.files.join(QStringLiteral(", ")));
    const QString tag = opt.ipTag.isEmpty() ? crashIpTag(ep.host) : opt.ipTag;
    QString zipPath = QDir(opt.outputDir).filePath(QStringLiteral("PS5_logs_%1_%2_crash_%3.zip")
                                                       .arg(tag, QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")),
                                                            safeCrashFolderName(folder)));
    for (int n = 2; QFileInfo::exists(zipPath); ++n)
        zipPath = zipPath.chopped(4) + QStringLiteral(" (%1).zip").arg(n);
    if (Status st = writeCrashZip(zipPath, c.localDir, folder, ep.host); st.ok())
        c.zipPath = zipPath;
    else
        c.warning = (c.warning.isEmpty() ? QString() : c.warning + u' ')
            + QStringLiteral("The crash ZIP could not be written: %1").arg(st.error().message);
    if (status && !c.zipPath.isEmpty())
        status(QStringLiteral("Crash report ready to send: %1").arg(QDir::toNativeSeparators(c.zipPath)));
    return c;
}

} // namespace

Result<CrashCaptureReport> captureCrashDumps(const ConsoleEndpoint& ep, const CrashCaptureOptions& opt,
                                             const CancellationToken& token, const CrashCaptureStatus& status)
{
    if (ep.host.trimmed().isEmpty())
        return Error(ErrorCode::InvalidArgument, QStringLiteral("Enter your PS5 IP address in Settings > PS5 connection."));
    if (opt.outputDir.trimmed().isEmpty() || !QDir().mkpath(opt.outputDir))
        return Error(ErrorCode::InvalidArgument,
                     QStringLiteral("The folder for crash dumps %1 cannot be created.").arg(QDir::toNativeSeparators(opt.outputDir)));
    CrashCaptureReport rep;
    Session s(ep, token);
    QSet<QString> seen;
    bool baselined = false, warnedDown = false;
    QElapsedTimer run;
    run.start();
    while (!token.isCancelled() && (opt.runForMs <= 0 || run.elapsed() < opt.runForMs)) {
        auto listing = listRemote(s, opt.root, token);
        if (!listing.ok()) {
            if (token.isCancelled())
                break;
            if (!warnedDown) {
                warnedDown = true;
                const QString w = QStringLiteral("Crash-dump capture waiting for the console FTP service (%1:%2): %3")
                                      .arg(ep.host)
                                      .arg(ep.ftpPort)
                                      .arg(listing.error().message);
                rep.warnings << w;
                if (status)
                    status(w);
            }
            if (!pause(token, 2000))
                break;
            continue;
        }
        if (warnedDown && status)
            status(QStringLiteral("Crash-dump capture ready (FTP %1:%2)").arg(ep.host).arg(ep.ftpPort));
        warnedDown = false;
        QStringList folders;
        for (const FtpEntry& e : listing.value())
            if (e.isDir && !e.name.isEmpty())
                folders << e.name;
        folders.sort();
        if (!baselined) {
            baselined = true;
            for (const QString& f : std::as_const(folders))
                seen.insert(f);
            rep.baseline = folders;
            if (status)
                status(folders.isEmpty() ? QStringLiteral("Watching for new crashes.")
                                         : QStringLiteral("%1 older crash folder(s) on the console left alone; watching "
                                                          "for new crashes.")
                                               .arg(folders.size()));
        } else {
            for (const QString& f : std::as_const(folders)) {
                if (seen.contains(f) || token.isCancelled())
                    continue;
                seen.insert(f);
                CapturedCrash c = captureFolder(s, ep, opt, f, token, status);
                if (!c.warning.isEmpty())
                    rep.warnings << c.warning;
                rep.crashes.push_back(std::move(c));
            }
        }
        if (!pause(token, opt.pollMs))
            break;
    }
    rep.stoppedByUser = token.isCancelled();
    return rep;
}

} // namespace fw::ps5
