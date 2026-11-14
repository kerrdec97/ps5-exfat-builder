#include "connect/ps5/FtpTransfer.h"

#include "connect/ps5/RemotePath.h"
#include "core/FileUtil.h"
#include "core/PathSafety.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
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

namespace {

// The names a transfer writes besides its target. A download is written as
// "<name>.fw.part", a name only this app uses: a file already called
// "<name>.part" may be a browser's download or another program's. An upload
// that replaces a file on the console goes to "<name>.uploading" first and
// sets the old one aside as "<name>.replacing.bak" (the legacy safe swap,
// FtpClient::uploadReplace()).
const QString kDownloadPartSuffix = QStringLiteral(".fw.part");
const QString kUploadTempSuffix = QStringLiteral(".uploading");
const QString kReplacedSuffix = QStringLiteral(".replacing.bak");

QString localKey(const QString& path)
{
    return hostNameKey(QDir::cleanPath(path));
}

// Two sources that end at the same target would be written by two workers at
// once, and so would a target that another one uses as its temporary name.
// The first keeps the target; later ones are left out and reported.
void dropDuplicateTargets(TransferPlan& plan, bool upload)
{
    QSet<QString> targets, temporaries;
    std::vector<TransferFile> kept;
    kept.reserve(plan.files.size());
    for (TransferFile& f : plan.files) {
        // The console's paths are compared as they are; this PC's as its file system does.
        const QString key = upload ? f.remote : localKey(f.local);
        const QStringList others = upload ? QStringList{f.remote + kUploadTempSuffix, f.remote + kReplacedSuffix}
                                          : QStringList{localKey(f.local + kDownloadPartSuffix)};
        const bool sameTarget = targets.contains(key);
        if (sameTarget || temporaries.contains(key)
            || std::any_of(others.begin(), others.end(), [&](const QString& k) { return targets.contains(k); })) {
            plan.totalBytes -= std::max<qint64>(0, f.size);
            if (!sameTarget)
                plan.skipped << (upload ? QStringLiteral("%1: another source is sent to a file on the PS5 that this one needs "
                                                         "as its temporary name (%2)")
                                              .arg(QDir::toNativeSeparators(f.local), f.remote)
                                        : QStringLiteral("%1: another source is saved under a name on this PC that this one "
                                                         "needs as its temporary name (%2)")
                                              .arg(f.remote, QDir::toNativeSeparators(f.local)));
            else
                plan.skipped << (upload ? QStringLiteral("%1: another source is sent to the same file on the PS5 (%2)")
                                              .arg(QDir::toNativeSeparators(f.local), f.remote)
                                        : QStringLiteral("%1: another source is saved under the same name on this PC (%2)")
                                              .arg(f.remote, QDir::toNativeSeparators(f.local)));
            continue;
        }
        targets.insert(key);
        for (const QString& k : others)
            temporaries.insert(k);
        kept.push_back(std::move(f));
    }
    plan.files = std::move(kept);
}

} // namespace

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
    dropDuplicateTargets(plan, true);
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
                auto listed = c.list(rdir, FtpListMode::Auto, token);
                if (!listed.ok()) {
                    // One unreadable sub-folder is left out and reported; the
                    // picked folder itself, a cancel or a lost connection ends the plan.
                    if (rdir == src || listed.error().isCancelled() || !c.isConnected())
                        return listed.takeError();
                    plan.skipped << QStringLiteral("%1: the folder could not be read (%2)").arg(rdir, listed.error().message);
                    continue;
                }
                const std::vector<FtpEntry> entries = std::move(listed.value());
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
    dropDuplicateTargets(plan, false);
    return plan;
}

Status commitDownloadedFile(const QString& part, const QString& finalPath, bool replaceExisting)
{
    // Never a copy of a finished download that cannot be renamed (another
    // program holds it), and an existing file is replaced in one step.
    const Status st = commitFinishedFile(part, finalPath, replaceExisting);
    if (st.ok() || !QFileInfo::exists(part))
        return st;
    Error e = st.error();
    e.message += QStringLiteral(" The download was kept as %1.").arg(QDir::toNativeSeparators(part));
    return e;
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
    // Uploads with resume on, cancelled: the partial remote files that may be
    // worth keeping. True: an earlier run's partial that resume continued (it
    // holds bytes); false: this run's, after an accepted STOR, which may still
    // be empty (the console asked first). Any other is removed.
    QHash<QString, bool> resumable;
    // Uploads: the server answered that it renames nothing, so a file the
    // console already has is written over directly from then on instead of
    // being sent to its temporary name first and then a second time.
    std::atomic<bool> renamesNothing{false};
    int writtenOver = 0;
    QString firstWrittenOver;
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
    void markResumable(const QString& path, bool holdsBytes)
    {
        std::lock_guard lock(mutex);
        resumable.insert(path, holdsBytes);
    }
    // One warning per skipped file, up to a point: a resume run over a whole
    // game would otherwise put a line per file into the job's outcome.
    static constexpr int kMaxSkipWarnings = 20;
    int skipped = 0;
    void skip(const QString& warning)
    {
        std::lock_guard lock(mutex);
        ++report.skipped;
        if (skipped++ < kMaxSkipWarnings)
            report.warnings << warning;
    }
    void summariseSkips()
    {
        if (skipped > kMaxSkipWarnings)
            report.warnings << QStringLiteral("%1 more file(s) were skipped for the same reason.").arg(skipped - kMaxSkipWarnings);
    }
    // One warning for all the files written over directly, not one per file.
    void wroteOver(const QString& remote)
    {
        std::lock_guard lock(mutex);
        if (writtenOver++ == 0)
            firstWrittenOver = remote;
    }
    void summariseWrittenOver()
    {
        if (writtenOver == 1)
            report.warnings << QStringLiteral("The PS5 FTP server refused to rename files, so %1 was written over directly.")
                                   .arg(firstWrittenOver);
        else if (writtenOver > 1)
            report.warnings << QStringLiteral("The PS5 FTP server refused to rename files, so %1 and %2 more file(s) were "
                                              "written over directly.")
                                   .arg(firstWrittenOver)
                                   .arg(writtenOver - 1);
    }
    void fail(const TransferFile& f, const QString& why)
    {
        std::lock_guard lock(mutex);
        ++report.failed;
        // The console path: many files of a game share a bare name (param.json).
        report.errors << QStringLiteral("%1: %2").arg(f.remote, why);
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

// Resume on: what arrived of an interrupted file stays for the next run.
void keepPartial(Shared& sh, const QString& path, bool upload)
{
    std::lock_guard lock(sh.mutex);
    sh.report.keptPartials << path;
    sh.log.info(upload ? QStringLiteral("[FTP] Kept the partial remote file for a resume: %1").arg(path)
                       : QStringLiteral("[FTP] Kept the partial download for a resume: %1").arg(QDir::toNativeSeparators(path)));
}

constexpr qint64 kNoFile = -1;
constexpr qint64 kSizeUnknown = -2;

// The size of the console's file `path`; kNoFile when there is none,
// kSizeUnknown when the server could not tell (the connection went).
qint64 remoteFileSize(FtpClient& c, const QString& path)
{
    auto sz = c.size(path);
    if (sz.ok())
        return sz.value();
    if (!c.isConnected())
        return kSizeUnknown;
    if (sz.error().code == ErrorCode::NotFound)
        return kNoFile;
    // A server without SIZE: its folder listing tells.
    auto listed = c.listedSize(path);
    if (listed.ok())
        return listed.value();
    return listed.error().code == ErrorCode::NotFound && c.isConnected() ? kNoFile : kSizeUnknown;
}

// Gives the complete, size-checked upload `temp` the name `target`, over the
// file that is there: the old one is set aside as "<target>.replacing.bak"
// until the new one has its name, and put back if that fails (the swap of
// FtpClient::uploadReplace()). Unsupported: the server renamed nothing, both
// files are as they were; `renamesNothing` is then set when its answer was
// that it does not implement renaming at all (not a refusal for this file).
// Any other error says where the two files are.
Status putInPlace(FtpClient& c, const QString& temp, const QString& target, bool& renamesNothing)
{
    renamesNothing = false;
    const QString bak = target + kReplacedSuffix;
    const QString lost = QStringLiteral("The connection to the PS5 was lost while %1 was being replaced.").arg(target);
    const qint64 old = remoteFileSize(c, target);
    if (old == kSizeUnknown)
        return Error(ErrorCode::IoError, lost + QStringLiteral(" The old file was left unchanged; the new one is %1.").arg(temp));
    bool aside = false;
    if (old != kNoFile) {
        (void)c.dele(bak); // what a finished replace could not delete
        if (auto st = c.rename(target, bak); !st.ok()) {
            if (!c.isConnected())
                return Error(st.error().code, lost + QStringLiteral(" The old file was left unchanged; the new one is %1.").arg(temp),
                             st.error().detail);
            renamesNothing = st.error().code == ErrorCode::Unsupported; // 500/501/502/504
            return Error(ErrorCode::Unsupported, QStringLiteral("The PS5 FTP server refused to rename %1.").arg(target),
                         st.error().detail);
        }
        aside = true;
    }
    if (auto st = c.rename(temp, target); !st.ok()) {
        if (!c.isConnected())
            return Error(st.error().code,
                         lost + (aside ? QStringLiteral(" The previous file is now %1 and the new one %2.").arg(bak, temp)
                                       : QStringLiteral(" The new file is still %1.").arg(temp)),
                         st.error().detail);
        if (aside && !c.rename(bak, target).ok())
            return Error(ErrorCode::IoError,
                         QStringLiteral("The PS5 FTP server refused to rename the uploaded file, and the previous file could "
                                        "not be put back: it is now %1. The new one is %2.")
                             .arg(bak, temp),
                         st.error().detail);
        renamesNothing = !aside && st.error().code == ErrorCode::Unsupported;
        return Error(ErrorCode::Unsupported, QStringLiteral("The PS5 FTP server refused to rename %1.").arg(temp),
                     st.error().detail);
    }
    if (aside)
        (void)c.dele(bak);
    return success();
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
    // Where the bytes go: the file itself when the console has none of that
    // name (or resume continues it). A file that is there already is replaced
    // through "<name>.uploading", which gets its name only once it is complete
    // and size-checked, so a cancel or failure removes only the temporary and
    // the console keeps its copy (the PS5 has no recycle bin).
    QString target = f.remote;
    // Every exit that gives the file up for good goes through here, so what
    // this run wrote of it is removed (or reported when the console is gone),
    // or, with resume on, kept for the next run unless its bytes are suspect.
    // A cancelled file stays marked: it is dealt with after all workers
    // stopped, and kept for a resume only when the console holds bytes of it.
    auto giveUp = [&](Outcome o, bool suspect = false) {
        if (o == Outcome::Failed) {
            if (startedHere && sh.opt.resume && !suspect)
                keepPartial(sh, target, true);
            else if (startedHere)
                removeRemotePartial(sh, &c, target);
            sh.unmark(target);
        } else if (o == Outcome::Cancelled && sh.opt.resume && (approvedOffset > 0 || acceptedHere)) {
            sh.markResumable(target, approvedOffset > 0);
        }
        return o;
    };
    const qint64 there = remoteFileSize(c, f.remote);
    if (sh.opt.resume) {
        if (there == f.size) {
            advance(f.size);
            sh.skip(QStringLiteral("%1 is already complete on the PS5; skipped.").arg(f.remote));
            return Outcome::Skipped;
        }
        if (there > 0 && there < f.size) {
            offset = there;
            approvedOffset = offset;
            advance(offset);
            sh.log.info(QStringLiteral("[FTP] Resuming %1 at %2").arg(f.remote, formatBytes(offset)));
        }
    }
    if (offset == 0 && there != kNoFile && sh.renamesNothing.load()) {
        // An earlier file of this job showed that the server renames nothing:
        // written over directly, as before, instead of being sent twice.
        sh.wroteOver(f.remote);
    } else if (offset == 0 && (there != kNoFile || sh.opt.resume)) {
        // With resume on, an earlier run's temporary is continued, also when
        // the old file is gone since (removed to make room for the new one).
        const QString temp = f.remote + kUploadTempSuffix;
        if (const qint64 have = sh.opt.resume ? remoteFileSize(c, temp) : kNoFile; have > 0 && have < f.size) {
            target = temp;
            offset = have;
            approvedOffset = offset;
            advance(offset);
            sh.log.info(QStringLiteral("[FTP] Resuming %1 at %2").arg(target, formatBytes(offset)));
        } else if (there != kNoFile) {
            target = temp;
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
        sh.mark(target);
        const Status st = c.store(target, in, offset, advance, sh.token, append);
        startedHere = startedHere || c.lastTransferStarted();
        acceptedHere = acceptedHere || c.lastTransferAccepted();
        if (st.ok()) {
            auto sz = c.size(target);
            if (sz.ok() && sz.value() != f.size && c.isConnected()) {
                // SIZE may describe a decrypted fself (ftpsrv SELF mode): the
                // folder listing (stat) is the second opinion before anything
                // that was just uploaded is deleted.
                if (auto listed = c.listedSize(target); listed.ok() && listed.value() == f.size) {
                    sh.log.warn(QStringLiteral("[FTP] SIZE of %1 says %2 bytes, the listing %3 (the server may decrypt SELF "
                                               "files); the listing size matches.")
                                    .arg(target)
                                    .arg(sz.value())
                                    .arg(listed.value()));
                    sz = listed.value();
                }
            }
            if (sz.ok() && sz.value() != f.size) {
                sh.fail(f, QStringLiteral("the PS5 reports %1 bytes, expected %2").arg(sz.value()).arg(f.size));
                startedHere = true; // the wrong-sized file is this run's
                return giveUp(Outcome::Failed, true);
            }
            if (target != f.remote) {
                bool renamesNothing = false;
                if (const Status put = putInPlace(c, target, f.remote, renamesNothing); !put.ok()) {
                    if (put.error().code == ErrorCode::Unsupported && c.isConnected()) {
                        // The server renames nothing: the old way, written over directly.
                        sh.log.warn(QStringLiteral("[FTP] %1 Writing %2 over directly (%3).")
                                        .arg(put.error().message, f.remote, put.error().detail));
                        if (renamesNothing && !sh.renamesNothing.exchange(true))
                            sh.log.warn(QStringLiteral("[FTP] The PS5 FTP server does not rename files: the other files "
                                                       "it already has are written over directly."));
                        if (there != kNoFile)
                            sh.wroteOver(f.remote);
                        (void)c.dele(target);
                        sh.unmark(target);
                        target = f.remote;
                        advance(-counted);
                        offset = approvedOffset = 0;
                        startedHere = acceptedHere = append = false;
                        continue;
                    }
                    // Both files are on the console and the error says where.
                    sh.unmark(target);
                    sh.fail(f, put.error().message);
                    return Outcome::Failed;
                }
            }
            sh.unmark(target);
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
                sh.unmark(target);
            return giveUp(Outcome::Cancelled); // partial removed after all workers stop
        }
        if (!startedHere)
            sh.unmark(target); // nothing of this run reached the console for this file
        if (st.error().code == ErrorCode::Unsupported && offset > 0 && !append && c.isConnected()) {
            append = true; // REST refused: try APPE from the same point
            continue;
        }
        if (isTransient(st.error(), c) && attempt < sh.opt.retries) {
            ++attempt;
            sh.log.warn(QStringLiteral("[FTP] %1 while uploading %2; retrying (%3/%4)")
                            .arg(st.error().message, target)
                            .arg(attempt)
                            .arg(sh.opt.retries));
            if (!sh.token.sleepFor(1000))
                return giveUp(Outcome::Cancelled);
            if (!ensureConnected(sh, c, attempt, f, out))
                return giveUp(out);
            qint64 next = approvedOffset;
            if (acceptedHere) {
                // The remote file now starts with this run's bytes.
                auto sz = c.size(target);
                next = sz.ok() && sz.value() > 0 && sz.value() <= f.size ? sz.value() : 0;
            }
            advance(next - counted);
            offset = next;
            append = false;
            continue;
        }
        if (st.error().code == ErrorCode::InsufficientSpace && target != f.remote && there != kNoFile) {
            // The old file stays until the new one is complete, so replacing it
            // needs room for both; say so, the console's "out of space" alone
            // does not tell why a file that fitted before no longer does.
            sh.fail(f, st.error().message
                           + QStringLiteral(" Replacing %1 needs room on the PS5 for the new file (%2) next to the old one, "
                                            "which was kept. Free space on the PS5, or delete the old file there first, then "
                                            "upload again.")
                                 .arg(f.remote, formatBytes(f.size)));
            return giveUp(Outcome::Failed);
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
    if (QFileInfo::exists(f.local) && !sh.opt.overwriteLocal) {
        advance(std::max<qint64>(0, f.size));
        sh.skip(QStringLiteral("%1 already exists; skipped (turn on overwrite to replace it).").arg(QDir::toNativeSeparators(f.local)));
        return Outcome::Skipped;
    }
    int attempt = 0;
    Outcome out = Outcome::Done;
    if (!ensureConnected(sh, c, attempt, f, out))
        return out;
    if (sh.progress)
        sh.progress->setCurrentItem(QFileInfo(f.local).fileName());
    // This run writes "<name>.fw.part", a name only this app uses, so one that
    // is there is a leftover of an interrupted run, which resume continues. A
    // "<name>.part" is never taken, also not the one an older version left:
    // it may be a browser's download or another program's, and its name and
    // size cannot show that it is the start of this file.
    const QString part = f.local + kDownloadPartSuffix;
    qint64 offset = 0;
    if (sh.opt.resume) {
        const QFileInfo mine(part);
        if (mine.isFile() && !mine.isSymLink() && (f.size < 0 || mine.size() <= f.size))
            offset = mine.size();
        advance(offset);
    }
    bool openedHere = false; // this run wrote to the .part (cleanup)
    // Every exit that gives the file up for good: the .part this run wrote
    // goes, or with resume on stays for the next run unless its bytes are
    // suspect. A cancelled file stays marked and is dealt with after all
    // workers stopped; a .part of an earlier run that was never opened is left alone.
    auto giveUp = [&](Outcome o, bool suspect = false) {
        if (o == Outcome::Failed && openedHere) {
            if (sh.opt.resume && !suspect && QFileInfo(part).size() > 0)
                keepPartial(sh, part, false);
            else
                QFile::remove(part);
            sh.unmark(part);
        }
        return o;
    };
    while (true) {
        if (!ensureConnected(sh, c, attempt, f, out))
            return giveUp(out);
        QFile outFile(part);
        bool opened = false;
        if (offset > 0) {
            opened = outFile.open(QIODevice::WriteOnly | QIODevice::Append);
        } else {
            // From the start: never into an existing file (a leftover of this
            // name is an earlier run's).
            const QFileInfo leftover(part);
            if (leftover.exists() || leftover.isSymLink())
                QFile::remove(part);
            opened = outFile.open(QIODevice::WriteOnly | QIODevice::NewOnly);
        }
        if (!opened) {
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
                return giveUp(Outcome::Failed, true);
            }
            // An existing file is replaced only with overwrite on, in one step;
            // when the rename fails the complete .part is kept.
            if (const Status cs = commitDownloadedFile(part, f.local, sh.opt.overwriteLocal); !cs.ok()) {
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
                // Done files count themselves; a skipped or failed one is dealt with too.
                if ((o == Outcome::Skipped || o == Outcome::Failed) && sh.progress)
                    sh.progress->advanceFiles(1);
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

// After a cancel, over a fresh connection: whether the partial remote file
// holds any byte. Unknown when the console cannot be asked (gone to rest mode).
enum class Holds { Bytes, Nothing, Unknown };
Holds remotePartialHolds(Shared& sh, const QString& remote)
{
    FtpOptions quick = sh.ftp;
    quick.connectTimeoutMs = std::min(quick.connectTimeoutMs, 3000);
    FtpClient fresh(quick);
    if (!fresh.open(sh.opt.endpoint.host, sh.opt.endpoint.ftpPort, CancellationToken()).ok())
        return Holds::Unknown;
    const qint64 n = remoteFileSize(fresh, remote);
    return n > 0 ? Holds::Bytes : n == kSizeUnknown ? Holds::Unknown : Holds::Nothing;
}

void finishCancelled(Shared& sh, bool upload)
{
    sh.report.cancelled = true;
    const QSet<QString> left = sh.inflight;
    for (const QString& p : left) {
        // A cancel can come right after the STOR, before the console took a
        // byte: an empty file it may have made is no resume point.
        bool keep = false;
        if (sh.opt.resume && upload) {
            const auto it = sh.resumable.constFind(p);
            keep = it != sh.resumable.constEnd() && (it.value() || remotePartialHolds(sh, p) != Holds::Nothing);
        } else if (sh.opt.resume) {
            keep = QFileInfo(p).size() > 0;
        }
        if (keep) {
            keepPartial(sh, p, upload);
        } else if (upload) {
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
        QSet<QString> made; // one MKD per folder, not one per descendant
        for (const QString& d : dirs) {
            if (!token.checkpoint())
                return Error::cancelled();
            if (auto st = prep.mkdirs(d, made); !st.ok())
                return Error(st.error().code, QStringLiteral("Could not create %1 on the PS5. %2").arg(d, st.error().message),
                             st.error().detail);
        }
        prep.quit();
    }
    runWorkers(sh, uploadOne);
    sh.summariseSkips();
    sh.summariseWrittenOver();
    if (token.isCancelled())
        finishCancelled(sh, true);
    // Nothing reached the console at all: one plain error. A report that names
    // partial files (removed or left behind) is kept, the user needs it.
    if (sh.workersConnected.load() == 0 && sh.report.transferred == 0 && !sh.report.cancelled && sh.report.files > 0
        && sh.report.removedPartials.isEmpty() && sh.report.leftoverPartials.isEmpty() && sh.report.keptPartials.isEmpty())
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
    sh.summariseSkips();
    if (token.isCancelled())
        finishCancelled(sh, false);
    return sh.report;
}

} // namespace fw::ps5
