#include "connect/ps5/FtpTransfer.h"

#include "connect/ps5/RemotePath.h"
#include "core/FileUtil.h"
#include "core/PathSafety.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QThread>

#include <algorithm>
#include <atomic>
#include <deque>
#include <mutex>

namespace fw::ps5 {

FtpOptions ftpOptionsFor(const ConsoleEndpoint& ep)
{
    FtpOptions o;
    o.connectTimeoutMs = ep.connectTimeoutMs;
    o.replyTimeoutMs = std::max(ep.connectTimeoutMs, 5000);
    o.idleTimeoutMs = ep.idleTimeoutMs;
    o.passive = ep.passive;
    return o;
}

Result<TransferPlan> planUpload(const QStringList& localSources, const QString& remoteDirIn,
                                const CancellationToken& token)
{
    TransferPlan plan;
    const QString remoteDir = normalizeRemotePath(remoteDirIn);
    plan.directories << remoteDir;
    for (const QString& srcIn : localSources) {
        const QFileInfo src(srcIn);
        if (!src.exists())
            return Error(ErrorCode::NotFound,
                         QStringLiteral("%1 was not found.").arg(QDir::toNativeSeparators(srcIn)));
        if (src.isDir()) {
            ScanOptions so;
            auto tree = scanTree(src.absoluteFilePath(), token, so);
            if (!tree.ok())
                return tree.takeError();
            const QString base = joinRemote(remoteDir, src.fileName());
            plan.directories << base;
            for (const FileEntry& e : tree.value().entries) {
                const QString remote = joinRemote(base, e.relativePath);
                if (e.isDir) {
                    plan.directories << remote;
                    continue;
                }
                plan.files.push_back({QDir(src.absoluteFilePath()).filePath(e.relativePath), remote, e.size});
                plan.totalBytes += e.size;
            }
        } else {
            plan.files.push_back({src.absoluteFilePath(), joinRemote(remoteDir, src.fileName()), src.size()});
            plan.totalBytes += src.size();
        }
    }
    plan.directories.removeDuplicates();
    return plan;
}

Result<TransferPlan> planDownload(FtpClient& c, const QStringList& remoteSources, const QString& localDir,
                                  const CancellationToken& token)
{
    TransferPlan plan;
    plan.directories << QDir::cleanPath(localDir);
    for (const QString& srcIn : remoteSources) {
        // Paths picked in the browser keep their exact characters.
        const QString src = cleanRemotePath(srcIn);
        if (!token.checkpoint())
            return Error::cancelled();
        const QString base = remoteFileName(src);
        if (src != u"/") {
            if (const QString why = localNameProblem(base); !why.isEmpty()) {
                plan.skipped << QStringLiteral("%1: %2").arg(src, why);
                continue;
            }
        }
        if (c.isDirectory(src)) {
            const QString localBase = src == u"/" ? QDir::cleanPath(localDir) : QDir(localDir).filePath(base);
            plan.directories << localBase;
            std::deque<std::pair<QString, QString>> dirs{{src, localBase}};
            int visited = 0;
            while (!dirs.empty()) {
                auto [rdir, ldir] = dirs.front();
                dirs.pop_front();
                if (++visited > 100000)
                    return Error(ErrorCode::Unsupported, QStringLiteral("The remote folder has too many sub-folders."));
                FW_TRY_ASSIGN(std::vector<FtpEntry> entries, c.list(rdir, FtpListMode::Auto, token));
                QSet<QString> localKeys; // names as this PC's file system compares them
                for (const FtpEntry& e : entries) {
                    if (e.isLink)
                        continue; // never follow links on the console
                    // Names come from the server: one safe path segment only, so a
                    // name with '/', '\' or ':' can neither leave the download
                    // folder nor make RETR act on another path.
                    const QString why = localNameProblem(e.name);
                    const auto r = remoteChild(rdir, e.name);
                    if (!why.isEmpty() || !r) {
                        plan.skipped << QStringLiteral("%1/%2: %3").arg(rdir == u"/" ? QString() : rdir, e.name,
                                                                        why.isEmpty() ? remoteNameProblem(e.name) : why);
                        continue;
                    }
                    // /data is case-sensitive, NTFS is not: two entries that collide here
                    // would write the same .part file from two workers.
                    const QString key = hostNameKey(e.name);
                    if (localKeys.contains(key)) {
                        plan.skipped << QStringLiteral("%1/%2: another entry of this folder has the same name on this PC (the names differ only in case)")
                                            .arg(rdir == u"/" ? QString() : rdir, e.name);
                        continue;
                    }
                    localKeys.insert(key);
                    const QString l = QDir(ldir).filePath(e.name);
                    if (e.isDir) {
                        plan.directories << l;
                        dirs.emplace_back(*r, l);
                    } else {
                        plan.files.push_back({l, *r, e.size});
                        plan.totalBytes += std::max<qint64>(0, e.size);
                    }
                }
            }
        } else {
            auto sz = c.size(src);
            if (!sz.ok() && !c.fileExists(src))
                return Error(ErrorCode::NotFound, QStringLiteral("%1 was not found on the PS5.").arg(src),
                             sz.error().detail);
            const qint64 size = sz.ok() ? sz.value() : -1;
            plan.files.push_back({QDir(localDir).filePath(base), src, size});
            plan.totalBytes += std::max<qint64>(0, size);
        }
    }
    plan.directories.removeDuplicates();
    return plan;
}

Status commitDownloadedFile(const QString& part, const QString& finalPath)
{
    const QString name = QDir::toNativeSeparators(finalPath);
    const QString kept = QDir::toNativeSeparators(part);
    if (!QFileInfo::exists(finalPath)) {
        if (QFile::rename(part, finalPath))
            return success();
        return Error(ErrorCode::IoError,
                     QStringLiteral("The finished download could not be renamed to %1; it was kept as %2.").arg(name, kept));
    }
    // Set the old file aside first so a failed rename can be undone (as
    // fw::PartFile::commit(true) does), and keep the .part on failure.
    QString aside = finalPath + QStringLiteral(".fw-old");
    for (int n = 2; QFileInfo::exists(aside); ++n)
        aside = finalPath + QStringLiteral(".fw-old%1").arg(n);
    if (!QFile::rename(finalPath, aside))
        return Error(ErrorCode::PermissionDenied,
                     QStringLiteral("Could not replace %1 (close any program using it). The download was kept as %2.")
                         .arg(name, kept));
    if (!QFile::rename(part, finalPath)) {
        QFile::rename(aside, finalPath);
        return Error(ErrorCode::IoError,
                     QStringLiteral("Could not replace %1; the old file was kept and the download was kept as %2.")
                         .arg(name, kept));
    }
    QFile::remove(aside);
    return success();
}

namespace {

enum class Outcome { Done, Skipped, Failed, Cancelled };

struct Shared {
    const TransferPlan& plan;
    const TransferOptions& opt;
    const CancellationToken& token;
    ProgressReporter* progress;
    const Log& log;
    FtpOptions ftp;
    std::atomic<std::size_t> next{0};
    std::mutex mutex;
    TransferReport report;
    QSet<QString> inflight; // remote partial files (upload) or local .part files (download)
    std::atomic<int> workersConnected{0};
    QString firstConnectError;

    Shared(const TransferPlan& p, const TransferOptions& o, const CancellationToken& t, ProgressReporter* pr,
           const Log& l)
        : plan(p), opt(o), token(t), progress(pr), log(l), ftp(ftpOptionsFor(o.endpoint))
    {
    }

    void mark(const QString& path)
    {
        std::lock_guard lock(mutex);
        inflight.insert(path);
    }
    void unmark(const QString& path)
    {
        std::lock_guard lock(mutex);
        inflight.remove(path);
    }
    void fail(const TransferFile& f, const QString& why)
    {
        std::lock_guard lock(mutex);
        ++report.failed;
        report.errors << QStringLiteral("%1: %2").arg(QFileInfo(f.local).fileName(), why);
    }
};

// A lost connection, a timeout, or a transient (4xx) transfer reply such as
// "426 Connection closed; transfer aborted" is worth a retry.
bool isTransient(const Error& e, const FtpClient& c)
{
    if ((e.code == ErrorCode::Timeout || e.code == ErrorCode::IoError) && !c.isConnected())
        return true;
    const int code = c.lastReply().code;
    return c.isConnected() && (code == 421 || code == 425 || code == 426 || code == 450 || code == 451);
}

// Connects (with the retry budget). Returns false when the file must stop.
bool ensureConnected(Shared& sh, FtpClient& c, int& attempt, const TransferFile& f, Outcome& out)
{
    while (!c.isConnected()) {
        auto st = c.open(sh.opt.endpoint.host, sh.opt.endpoint.ftpPort, sh.token);
        if (st.ok())
            return true;
        if (st.error().isCancelled() || sh.token.isCancelled()) {
            out = Outcome::Cancelled;
            return false;
        }
        {
            std::lock_guard lock(sh.mutex);
            if (sh.firstConnectError.isEmpty())
                sh.firstConnectError = st.error().message;
        }
        if (attempt >= sh.opt.retries) {
            sh.fail(f, st.error().message);
            out = Outcome::Failed;
            return false;
        }
        ++attempt;
        if (!sh.token.sleepFor(1000)) {
            out = Outcome::Cancelled;
            return false;
        }
    }
    return true;
}

// Deletes a half-written remote file (legacy behaviour after cancel/failure).
void removeRemotePartial(Shared& sh, FtpClient* existing, const QString& remote)
{
    Status st = Error(ErrorCode::IoError, QString());
    if (existing && existing->isConnected())
        st = existing->dele(remote);
    if (!st.ok()) {
        // Right after an aborted STOR the server may still hold the file open
        // (a Windows-hosted server cannot delete it until then), so try a few
        // times over about two seconds. Never cancellable: this is the cleanup.
        // A console that does not answer any more (rest mode, reboot: the
        // usual reason for a cancel) ends it at once instead of retrying.
        FtpOptions quick = sh.ftp;
        quick.connectTimeoutMs = std::min(quick.connectTimeoutMs, 3000);
        FtpClient fresh(quick);
        QElapsedTimer clock;
        clock.start();
        for (int attempt = 0; attempt < 6 && !st.ok() && clock.elapsed() < 4000; ++attempt) {
            if (attempt > 0)
                QThread::msleep(350);
            if (!fresh.isConnected() && !fresh.open(sh.opt.endpoint.host, sh.opt.endpoint.ftpPort, CancellationToken()).ok())
                break; // not reachable: the partial file is reported, not retried
            st = fresh.dele(remote);
            if (!st.ok() && st.error().code == ErrorCode::NotFound && !fresh.fileExists(remote)) {
                st = success(); // already gone
                break;
            }
        }
    }
    std::lock_guard lock(sh.mutex);
    if (st.ok()) {
        sh.report.removedPartials << remote;
        sh.log.info(QStringLiteral("[FTP] Removed the partial remote file after cancel/failure: %1").arg(remote));
    } else {
        sh.report.leftoverPartials << remote;
        sh.log.warn(QStringLiteral("[FTP] Could not remove the partial remote file %1 (%2) — it may remain on the PS5.")
                        .arg(remote, st.error().message));
    }
}

Outcome uploadOne(Shared& sh, FtpClient& c, const TransferFile& f)
{
    qint64 counted = 0;
    auto advance = [&](qint64 n) {
        counted += n;
        if (sh.progress)
            sh.progress->advance(n);
    };
    int attempt = 0;
    Outcome out = Outcome::Done;
    if (!ensureConnected(sh, c, attempt, f, out))
        return out;
    if (sh.progress)
        sh.progress->setCurrentItem(QFileInfo(f.local).fileName());

    qint64 offset = 0;
    // Remote bytes the caller asked to keep (resume of an earlier partial).
    // After a failure, the remote SIZE is trusted only if this run's STOR was
    // accepted for this file; otherwise the remote file is not ours to extend.
    qint64 approvedOffset = 0;
    bool startedHere = false;  // a STOR/APPE of this run was sent (cleanup)
    bool acceptedHere = false; // ... and accepted with a 1xx (resume point is ours)
    // Every exit that gives the file up for good goes through here, so what
    // this run wrote of it is removed (or reported when the console is gone).
    // A cancelled file stays marked: it is removed after all workers stopped.
    auto giveUp = [&](Outcome o) {
        if (o == Outcome::Failed) {
            if (startedHere)
                removeRemotePartial(sh, &c, f.remote);
            sh.unmark(f.remote);
        }
        return o;
    };
    if (sh.opt.resume) {
        auto sz = c.size(f.remote);
        if (sz.ok() && sz.value() == f.size) {
            advance(f.size);
            std::lock_guard lock(sh.mutex);
            ++sh.report.skipped;
            sh.report.warnings << QStringLiteral("%1 is already complete on the PS5; skipped.").arg(f.remote);
            return Outcome::Skipped;
        }
        if (sz.ok() && sz.value() > 0 && sz.value() < f.size) {
            offset = sz.value();
            approvedOffset = offset;
            advance(offset);
            sh.log.info(QStringLiteral("[FTP] Resuming %1 at %2").arg(f.remote, formatBytes(offset)));
        }
    }
    bool append = false;
    while (true) {
        if (!ensureConnected(sh, c, attempt, f, out))
            return giveUp(out);
        QFile in(f.local);
        if (!in.open(QIODevice::ReadOnly)) {
            sh.fail(f, QStringLiteral("could not read the local file (%1)").arg(in.errorString()));
            return giveUp(Outcome::Failed);
        }
        sh.mark(f.remote);
        const Status st = c.store(f.remote, in, offset, advance, sh.token, append);
        startedHere = startedHere || c.lastTransferStarted();
        acceptedHere = acceptedHere || c.lastTransferAccepted();
        if (st.ok()) {
            auto sz = c.size(f.remote);
            if (sz.ok() && sz.value() != f.size && c.isConnected()) {
                // SIZE may describe a decrypted fself (ftpsrv SELF mode): the
                // folder listing (stat) is the second opinion before anything
                // that was just uploaded is deleted.
                if (auto listed = c.listedSize(f.remote); listed.ok() && listed.value() == f.size) {
                    sh.log.warn(QStringLiteral("[FTP] SIZE of %1 says %2 bytes, the listing %3 (the server may decrypt SELF "
                                               "files); the listing size matches.")
                                    .arg(f.remote)
                                    .arg(sz.value())
                                    .arg(listed.value()));
                    sz = listed.value();
                }
            }
            if (sz.ok() && sz.value() != f.size) {
                sh.fail(f, QStringLiteral("the PS5 reports %1 bytes, expected %2").arg(sz.value()).arg(f.size));
                startedHere = true; // the wrong-sized file is this run's
                return giveUp(Outcome::Failed);
            }
            sh.unmark(f.remote);
            std::lock_guard lock(sh.mutex);
            ++sh.report.transferred;
            ++(sz.ok() ? sh.report.verified : sh.report.unverified);
            sh.report.bytes += f.size - offset;
            sh.report.outputs << f.remote;
            if (sh.progress)
                sh.progress->advanceFiles(1);
            return Outcome::Done;
        }
        if (st.error().isCancelled() || sh.token.isCancelled()) {
            if (!startedHere)
                sh.unmark(f.remote);
            return Outcome::Cancelled; // partial removed after all workers stop
        }
        if (!startedHere)
            sh.unmark(f.remote); // nothing of this run reached the console for this file
        if (st.error().code == ErrorCode::Unsupported && offset > 0 && !append && c.isConnected()) {
            append = true; // REST refused: try APPE from the same point
            continue;
        }
        if (isTransient(st.error(), c) && attempt < sh.opt.retries) {
            ++attempt;
            sh.log.warn(QStringLiteral("[FTP] %1 while uploading %2; retrying (%3/%4)")
                            .arg(st.error().message, f.remote)
                            .arg(attempt)
                            .arg(sh.opt.retries));
            if (!sh.token.sleepFor(1000))
                return Outcome::Cancelled;
            if (!ensureConnected(sh, c, attempt, f, out))
                return giveUp(out);
            qint64 next = approvedOffset;
            if (acceptedHere) {
                // The remote file now starts with this run's bytes.
                auto sz = c.size(f.remote);
                next = sz.ok() && sz.value() > 0 && sz.value() <= f.size ? sz.value() : 0;
            }
            advance(next - counted);
            offset = next;
            append = false;
            continue;
        }
        sh.fail(f, st.error().message);
        return giveUp(Outcome::Failed);
    }
}

Outcome downloadOne(Shared& sh, FtpClient& c, const TransferFile& f)
{
    qint64 counted = 0;
    auto advance = [&](qint64 n) {
        counted += n;
        if (sh.progress)
            sh.progress->advance(n);
    };
    const QString part = f.local + QStringLiteral(".part");
    if (QFileInfo::exists(f.local) && !sh.opt.overwriteLocal) {
        advance(std::max<qint64>(0, f.size));
        std::lock_guard lock(sh.mutex);
        ++sh.report.skipped;
        sh.report.warnings << QStringLiteral("%1 already exists; skipped (turn on overwrite to replace it).")
                                  .arg(QDir::toNativeSeparators(f.local));
        return Outcome::Skipped;
    }
    int attempt = 0;
    Outcome out = Outcome::Done;
    if (!ensureConnected(sh, c, attempt, f, out))
        return out;
    if (sh.progress)
        sh.progress->setCurrentItem(QFileInfo(f.local).fileName());
    qint64 offset = 0;
    if (sh.opt.resume && QFileInfo::exists(part)) {
        const qint64 have = QFileInfo(part).size();
        if (f.size < 0 || have <= f.size) {
            offset = have;
            advance(have);
        }
    }
    bool openedHere = false; // this run wrote to the .part (cleanup)
    // Every exit that gives the file up for good: the .part this run wrote
    // goes. A cancelled file stays marked and is removed after all workers
    // stopped; a .part of an earlier run that was never opened is left alone.
    auto giveUp = [&](Outcome o) {
        if (o == Outcome::Failed && openedHere) {
            QFile::remove(part);
            sh.unmark(part);
        }
        return o;
    };
    while (true) {
        if (!ensureConnected(sh, c, attempt, f, out))
            return giveUp(out);
        QFile outFile(part);
        if (!outFile.open(offset > 0 ? (QIODevice::WriteOnly | QIODevice::Append) : (QIODevice::WriteOnly | QIODevice::Truncate))) {
            sh.fail(f, QStringLiteral("could not write %1 (%2)").arg(QDir::toNativeSeparators(part), outFile.errorString()));
            return giveUp(Outcome::Failed);
        }
        openedHere = true;
        sh.mark(part);
        Status st = c.retrieve(f.remote, outFile, offset, advance, sh.token);
        // On the drive before it can replace anything (power cut safety).
        if (st.ok())
            st = flushToDisk(outFile, QFileInfo(f.local).fileName());
        outFile.close();
        if (st.ok()) {
            const qint64 got = QFileInfo(part).size();
            if (f.size >= 0 && got != f.size) {
                sh.fail(f, QStringLiteral("received %1 bytes, expected %2").arg(got).arg(f.size));
                return giveUp(Outcome::Failed);
            }
            // An existing file (only reached with overwrite on) is set aside and
            // restored if the rename fails; the complete .part is then kept.
            if (const Status cs = commitDownloadedFile(part, f.local); !cs.ok()) {
                sh.unmark(part);
                sh.fail(f, cs.error().message);
                return Outcome::Failed;
            }
            sh.unmark(part);
            std::lock_guard lock(sh.mutex);
            ++sh.report.transferred;
            ++(f.size >= 0 ? sh.report.verified : sh.report.unverified);
            sh.report.bytes += got - offset;
            sh.report.outputs << f.local;
            if (sh.progress)
                sh.progress->advanceFiles(1);
            return Outcome::Done;
        }
        if (st.error().isCancelled() || sh.token.isCancelled())
            return Outcome::Cancelled;
        if (st.error().code == ErrorCode::Unsupported && offset > 0 && c.isConnected()) {
            advance(-counted);
            offset = 0; // REST refused: start over
            continue;
        }
        if (isTransient(st.error(), c) && attempt < sh.opt.retries) {
            ++attempt;
            sh.log.warn(QStringLiteral("[FTP] %1 while downloading %2; retrying (%3/%4)")
                            .arg(st.error().message, f.remote)
                            .arg(attempt)
                            .arg(sh.opt.retries));
            if (!sh.token.sleepFor(1000))
                return Outcome::Cancelled;
            offset = QFileInfo(part).size();
            advance(offset - counted);
            continue;
        }
        sh.fail(f, st.error().message);
        return giveUp(Outcome::Failed);
    }
}

template <typename OneFn>
void runWorkers(Shared& sh, OneFn one)
{
    const int n = std::clamp(sh.opt.parallel, 1, 8);
    const int workers = std::max(1, std::min<int>(n, int(sh.plan.files.size())));
    std::vector<QThread*> threads;
    for (int k = 0; k < workers; ++k) {
        threads.push_back(QThread::create([&sh, one] {
            FtpClient c(sh.ftp);
            bool counted = false;
            while (!sh.token.isCancelled()) {
                const std::size_t i = sh.next.fetch_add(1);
                if (i >= sh.plan.files.size())
                    break;
                const Outcome o = one(sh, c, sh.plan.files[i]);
                if (!counted && c.isConnected()) {
                    counted = true;
                    sh.workersConnected.fetch_add(1);
                }
                if (o == Outcome::Cancelled)
                    break;
            }
        }));
        threads.back()->start();
    }
    for (QThread* t : threads) {
        t->wait();
        delete t;
    }
}

void finishCancelled(Shared& sh, bool upload)
{
    sh.report.cancelled = true;
    const QSet<QString> left = sh.inflight;
    for (const QString& p : left) {
        if (upload) {
            removeRemotePartial(sh, nullptr, p);
        } else {
            QFile::remove(p);
            sh.report.removedPartials << p;
        }
    }
}

} // namespace

Result<TransferReport> runUpload(const TransferPlan& plan, const TransferOptions& options,
                                 const CancellationToken& token, ProgressReporter* progress, const Log& log)
{
    Shared sh(plan, options, token, progress, log);
    sh.report.files = int(plan.files.size());
    // Folders first, on one connection, so workers never race on MKD.
    {
        FtpClient prep(sh.ftp);
        FW_TRY(prep.open(options.endpoint.host, options.endpoint.ftpPort, token));
        QStringList dirs = plan.directories;
        for (const auto& f : plan.files)
            dirs << remoteParent(f.remote);
        dirs.removeDuplicates();
        std::sort(dirs.begin(), dirs.end());
        for (const QString& d : dirs) {
            if (!token.checkpoint())
                return Error::cancelled();
            if (auto st = prep.mkdirs(d); !st.ok())
                return Error(st.error().code, QStringLiteral("Could not create %1 on the PS5. %2").arg(d, st.error().message),
                             st.error().detail);
        }
        prep.quit();
    }
    runWorkers(sh, uploadOne);
    if (token.isCancelled())
        finishCancelled(sh, true);
    // Nothing reached the console at all: one plain error. A report that names
    // partial files (removed or left behind) is kept, the user needs it.
    if (sh.workersConnected.load() == 0 && sh.report.transferred == 0 && !sh.report.cancelled && sh.report.files > 0
        && sh.report.removedPartials.isEmpty() && sh.report.leftoverPartials.isEmpty())
        return Error(ErrorCode::IoError, sh.firstConnectError.isEmpty() ? QStringLiteral("No file could be uploaded.")
                                                                        : sh.firstConnectError);
    return sh.report;
}

Result<TransferReport> runDownload(const TransferPlan& plan, const TransferOptions& options,
                                   const CancellationToken& token, ProgressReporter* progress, const Log& log)
{
    Shared sh(plan, options, token, progress, log);
    sh.report.files = int(plan.files.size());
    for (const QString& d : plan.directories)
        if (!QDir().mkpath(d))
            return Error(ErrorCode::IoError, QStringLiteral("Could not create the folder %1.").arg(QDir::toNativeSeparators(d)));
    runWorkers(sh, downloadOne);
    if (token.isCancelled())
        finishCancelled(sh, false);
    return sh.report;
}

} // namespace fw::ps5
