#include "engines/ampr/GamePack.h"

#include "core/FileUtil.h"
#include "core/FsLinks.h"
#include "engines/ampr/HostPaths.h"
#include "engines/pfs/AmprIndex.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QStorageInfo>
#include <QSet>
#include <QTimeZone>

#include <algorithm>
#include <map>

namespace fw::ampr {

namespace {

class ItemReader final : public SequentialReader {
public:
    ItemReader(std::shared_ptr<GameFiles> files, int index) : m_files(std::move(files)), m_index(index) {}
    Result<qint64> read(char* dst, qint64 max) override
    {
        auto n = m_files->read(m_index, m_pos, dst, max);
        if (n.ok())
            m_pos += *n;
        return n;
    }

private:
    std::shared_ptr<GameFiles> m_files;
    int m_index;
    qint64 m_pos = 0;
};

QString native(const QString& p) { return QDir::toNativeSeparators(p); }

bool isRuntimePath(const QString& rel) { return rel.compare(kRuntimeRelPath, Qt::CaseInsensitive) == 0; }
bool isAmprIndexPath(const QString& rel) { return rel.compare(pfs::kAmprIndexName, Qt::CaseInsensitive) == 0; }

Status emptyOrNewFolder(const QString& outFolder)
{
    FW_TRY(checkNewFolderOutput(outFolder));
    if (!QDir().mkpath(outFolder))
        return Error(ErrorCode::IoError, QStringLiteral("Could not create %1.").arg(native(outFolder)));
    return success();
}

// Copies one item to `dst` in 4 MiB pieces and keeps its modification time.
Status copyItem(GameFiles& files, int index, const QString& dst, std::vector<char>& buf, const CancellationToken& token,
                ProgressReporter* progress)
{
    const GameItem& it = files.items()[size_t(index)];
    QFile out(dst);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return Error(ErrorCode::IoError, QStringLiteral("Could not create %1.").arg(native(dst)), out.errorString());
    qint64 pos = 0;
    while (pos < it.size) {
        if (!token.checkpoint())
            return Error::cancelled();
        auto n = files.read(index, pos, buf.data(), std::min<qint64>(qint64(buf.size()), it.size - pos));
        if (!n.ok())
            return n.takeError();
        if (*n <= 0)
            return Error(ErrorCode::IoError, QStringLiteral("%1 ends early.").arg(it.path));
        if (out.write(buf.data(), *n) != *n)
            return Error(ErrorCode::IoError, QStringLiteral("Could not write %1.").arg(native(dst)), out.errorString());
        pos += *n;
        if (progress)
            progress->advance(*n);
    }
    if (!out.flush())
        return Error(ErrorCode::IoError, QStringLiteral("Could not write %1.").arg(native(dst)), out.errorString());
    if (it.mtime > 0)
        out.setFileTime(QDateTime::fromSecsSinceEpoch(it.mtime, QTimeZone::UTC), QFileDevice::FileModificationTime);
    return success();
}

Result<QByteArray> readSmall(GameFiles& files, int index, qint64 max)
{
    const qint64 size = std::min(max, files.items()[size_t(index)].size);
    QByteArray b(qsizetype(size), '\0');
    qint64 done = 0;
    while (done < size) {
        auto n = files.read(index, done, b.data() + done, size - done);
        if (!n.ok())
            return n.takeError();
        if (*n <= 0)
            break;
        done += *n;
    }
    b.truncate(qsizetype(done));
    return b;
}

QString folderCaseFor(const std::vector<GameItem>& items, const QString& wanted)
{
    for (const auto& it : items)
        if (it.isDir && it.path.compare(wanted, Qt::CaseInsensitive) == 0)
            return it.path;
    return wanted;
}

// ampr_emu.index for `files` (the game's files as the game sees them; names
// and case as stored). The files it leaves out because the runtime could not
// list them are a warning in the job's log and outcome, as in the builds.
Result<bool> writeAmprIndex(const QString& folder, std::vector<pfs::AmprFile> files, QStringList& warnings, const Log& log)
{
    pfs::AmprToolRows rows = pfs::amprToolRows(std::move(files));
    if (const QString w = pfs::amprLeftOutLogLine(rows.skippedRuntime); !w.isEmpty()) {
        warnings << w;
        log.warn(w);
    }
    const QByteArray index = pfs::buildAmprIndex(rows.rows);
    if (index.isEmpty())
        return false;
    FW_TRY(writeFileAtomic(QDir(folder).filePath(pfs::kAmprIndexName), index));
    return true;
}

// ampr_emu.index for the files of `folder` as they are on disk.
Result<bool> writeAmprIndexForFolder(const QString& folder, const CancellationToken& token, QStringList& warnings, const Log& log)
{
    FW_TRY_ASSIGN(pfs::AmprLocalScan scan, pfs::scanAmprFolder(folder, token));
    return writeAmprIndex(folder, std::move(scan.files), warnings, log);
}

pfs::AmprFile amprFile(const GameItem& it, qint64 size)
{
    pfs::AmprFile f;
    f.relPath = it.path.toUtf8();
    f.size = size;
    f.mtime = it.mtime;
    return f;
}

} // namespace

std::unique_ptr<SequentialReader> itemReader(std::shared_ptr<GameFiles> files, int index)
{
    return std::make_unique<ItemReader>(std::move(files), index);
}

Status checkNewFolderOutput(const QString& folder)
{
    const QFileInfo fi(folder);
    if (!fi.exists())
        return success();
    if (!fi.isDir())
        return Error(ErrorCode::AlreadyExists, QStringLiteral("%1 exists and is not a folder.").arg(native(folder)));
    if (!QDir(folder).isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System))
        return Error(ErrorCode::AlreadyExists, QStringLiteral("%1 already exists and is not empty.").arg(native(folder)));
    return success();
}

Result<std::unique_ptr<StagedFolder>> StagedFolder::begin(const QString& finalFolder)
{
    std::unique_ptr<StagedFolder> s(new StagedFolder);
    s->m_final = QDir::cleanPath(QFileInfo(finalFolder).absoluteFilePath());
    s->m_stage = s->m_final + QStringLiteral(".part");
    s->m_marker = s->m_final + QStringLiteral(".part.writing");
    s->m_parent = QFileInfo(s->m_final).absolutePath();
    FW_TRY(checkNewFolderOutput(s->m_final));
    if (QFileInfo::exists(s->m_stage)) {
        if (!QFileInfo::exists(s->m_marker))
            return Error(ErrorCode::AlreadyExists,
                         QStringLiteral("%1 already exists. Move or delete it, then try again.").arg(native(s->m_stage)));
        FW_TRY(removeTreeSafely(s->m_stage, s->m_parent));
    }
    if (!QDir().mkpath(s->m_parent) || !QFile(s->m_marker).open(QIODevice::WriteOnly))
        return Error(ErrorCode::IoError, QStringLiteral("Could not create the folder %1.").arg(native(s->m_parent)));
    s->m_owned = true;
    if (!QDir().mkdir(s->m_stage))
        return Error(ErrorCode::IoError, QStringLiteral("Could not create the folder %1.").arg(native(s->m_stage)));
    return s;
}

StagedFolder::~StagedFolder()
{
    if (!m_owned)
        return;
    if (!m_committed && !m_kept)
        (void)removeTreeSafely(m_stage, m_parent);
    QFile::remove(m_marker);
}

Status StagedFolder::commitOrKeep(const QString& what)
{
    Status st = commit();
    if (st.ok())
        return st;
    QString kept = uniquePath(m_final + QStringLiteral(".new"));
    if (!renameWithoutCopying(m_stage, kept))
        kept = m_stage; // it stays under its working name
    m_kept = true;
    Error e = st.error();
    e.detail = e.detail.isEmpty() ? e.message : e.message + u' ' + e.detail;
    // The finished work is saved: a retry of the job would only make another
    // full copy of it, so the queue must not retry this by itself.
    if (e.code == ErrorCode::IoError)
        e.code = ErrorCode::PermissionDenied;
    e.message = QStringLiteral("%1, but the folder %2 could not be used. The files were saved in %3.")
                    .arg(what, native(m_final), native(kept));
    return e;
}

Status StagedFolder::commit()
{
    // The final folder may have changed while the job ran: it is only ever
    // replaced when it is (still) absent or empty.
    FW_TRY(checkNewFolderOutput(m_final));
    const bool existed = QFileInfo(m_final).isDir();
    if (existed && !QDir().rmdir(m_final))
        return Error(ErrorCode::IoError, QStringLiteral("Could not replace the empty folder %1.").arg(native(m_final)));
    // A virus scanner or the search indexer that still has a file of the
    // folder open refuses the rename for a moment: it is tried again.
    QString why;
    if (!renameWithoutCopying(m_stage, m_final, &why)) {
        if (existed)
            QDir().mkdir(m_final);
        return Error(ErrorCode::IoError,
                     QStringLiteral("Could not rename the finished folder to %1. Close any program using it and try again.")
                         .arg(native(m_final)),
                     why);
    }
    m_committed = true;
    return success();
}

QString PackGameReport::summary() const
{
    return QStringLiteral("%1 files packed (%2 from %3, zstd level %4), %5 loose")
        .arg(formatCount(packedFiles), formatBytes(storedBytes), formatBytes(packedBytes))
        .arg(level)
        .arg(formatCount(looseFiles));
}

Result<PackGameReport> packGame(std::shared_ptr<GameFiles> source, const QString& outFolder, const PackGameOptions& o,
                                const CancellationToken& token, ProgressReporter* progress, const Log& log)
{
    QElapsedTimer timer;
    timer.start();
    PackGameReport rep;
    rep.level = o.level;
    FW_TRY_ASSIGN(QByteArray runtime, loadBundledRuntime());
    rep.runtime = QStringLiteral("ampr-emu-ng %1").arg(QLatin1String(kBundledRuntimeVersion));

    rep.sourcePacks = detectPackKind(*source);
    FW_TRY_ASSIGN(std::shared_ptr<GameFiles> logical, logicalGame(source, token));
    if (rep.sourcePacks != PackKind::None)
        log.info(QStringLiteral("The source is %1-packed: its packs are decoded and the game is packed again.")
                     .arg(packKindName(rep.sourcePacks)));
    // Hard links need the same NTFS volume; anything else is copied (and counted
    // in the free-space check).
    bool canLink = !o.linkFromFolder.isEmpty() && rep.sourcePacks == PackKind::None;
    if (canLink) {
        QDir().mkpath(QFileInfo(outFolder).absolutePath());
        const QStorageInfo a(o.linkFromFolder), b(QFileInfo(outFolder).absolutePath());
        canLink = a.isValid() && b.isValid() && a.rootPath() == b.rootPath()
                  && a.fileSystemType().compare("NTFS", Qt::CaseInsensitive) == 0;
    }

    // Sort the files into loose, packed and left out.
    struct Loose {
        int index;
        QString reason;
    };
    std::vector<Loose> loose;
    std::vector<int> packed;
    QStringList dirs;
    std::map<QString, std::pair<qint64, qint64>> looseByReason;
    int runtimeItem = -1;
    for (int i = 0; i < int(logical->items().size()); ++i) {
        const GameItem& it = logical->items()[size_t(i)];
        if (o.exclude && o.exclude(it.path, it.isDir)) {
            if (!it.isDir)
                ++rep.excludedFiles;
            continue;
        }
        if (o.exclude) {
            // Anything inside an excluded folder.
            bool inExcluded = false;
            for (qsizetype s = it.path.indexOf(u'/'); s >= 0; s = it.path.indexOf(u'/', s + 1))
                if (o.exclude(it.path.left(s), true)) {
                    inExcluded = true;
                    break;
                }
            if (inExcluded) {
                if (!it.isDir)
                    ++rep.excludedFiles;
                continue;
            }
        }
        if (it.isDir) {
            dirs << it.path;
            continue;
        }
        if (packJobDrops(it.path)) {
            rep.droppedFiles << it.path;
            continue;
        }
        if (isRuntimePath(it.path)) {
            runtimeItem = i;
            continue;
        }
        if (isAmprIndexPath(it.path))
            continue; // written afresh below
        const QString reason = looseReason(it.path, it.size, o.strategy);
        if (reason.isEmpty()) {
            packed.push_back(i);
            rep.packedBytes += it.size;
        } else {
            loose.push_back({i, reason});
            rep.looseBytes += it.size;
            auto& acc = looseByReason[reason];
            acc.first += 1;
            acc.second += it.size;
        }
    }
    rep.packedFiles = qint64(packed.size());
    rep.looseFiles = qint64(loose.size()) + 1; // + the runtime
    rep.files = rep.packedFiles + rep.looseFiles;
    for (const auto& [reason, acc] : looseByReason)
        rep.looseSummary << QStringLiteral("%1: %2 file%3, %4").arg(reason, formatCount(acc.first), acc.first == 1 ? QString() : QStringLiteral("s"),
                                                                  formatBytes(acc.second));
    if (runtimeItem >= 0) {
        FW_TRY_ASSIGN(QByteArray old, readSmall(*logical, runtimeItem, 64LL * 1024 * 1024));
        rep.previousRuntime = identifyRuntime(old);
    }
    if (packed.empty())
        return Error(ErrorCode::InvalidArgument, QStringLiteral("The game has no files to pack (everything must stay loose)."));

    if (const int eboot = logical->find(QStringLiteral("eboot.bin")); eboot >= 0) {
        FW_TRY_ASSIGN(QByteArray head, readSmall(*logical, eboot, 4));
        if (executableKind(head) == ExecutableKind::PlainElf) {
            rep.warnings << QStringLiteral("eboot.bin is a plain ELF, not a signed SELF: the console will not start it. Use a "
                                           "fake-signed (backported) eboot.bin.");
            log.warn(rep.warnings.back()); // now, not after hours of packing
        }
    }
    for (const QString& w : logical->warnings()) {
        rep.warnings << w;
        log.warn(w);
    }

    // Where everything loose goes. The names come from a pack index or an
    // image when the source is one, so every target is checked to lie inside
    // the output before anything is written.
    HostPaths paths(outFolder);
    // Only folders that hold loose files exist on disk, plus the source's own
    // empty folders: the runtime serves a folder of packed files from the
    // index, and a real (empty) folder of the same name would hide its
    // contents from the game.
    QStringList emptyDirTargets;
    {
        QSet<QString> nonEmpty;
        for (int i = 0; i < int(logical->items().size()); ++i) {
            const QString& p = logical->items()[size_t(i)].path;
            const qsizetype slash = p.lastIndexOf(u'/');
            if (slash > 0)
                nonEmpty.insert(p.left(slash));
        }
        for (const QString& d : dirs)
            if (!nonEmpty.contains(d)) {
                FW_TRY_ASSIGN(const QString target, paths.add(d, true));
                emptyDirTargets << target;
            }
    }
    // A read-only file is copied even where links work: deleting a link later
    // would clear the read-only flag, which all links of a file share, on the
    // user's original.
    struct LooseTarget {
        QString path;
        QString linkFrom; // "" = copy
    };
    std::vector<LooseTarget> looseTargets;
    looseTargets.reserve(loose.size());
    qint64 copiedLooseBytes = 0;
    for (const Loose& l : loose) {
        const GameItem& it = logical->items()[size_t(l.index)];
        LooseTarget t;
        FW_TRY_ASSIGN(t.path, paths.add(it.path));
        if (canLink) {
            const QString original = QDir(o.linkFromFolder).filePath(it.path);
            if (QFileInfo(original).isWritable())
                t.linkFrom = original;
        }
        if (t.linkFrom.isEmpty())
            copiedLooseBytes += it.size;
        looseTargets.push_back(std::move(t));
    }
    const QString fakelib = folderCaseFor(logical->items(), QStringLiteral("fakelib"));
    FW_TRY_ASSIGN(const QString runtimeTarget, paths.add(fakelib + QStringLiteral("/libSceAmpr.sprx")));
    if (!paths.renamed().isEmpty()) {
        rep.warnings << renamedWarnings(paths.renamed());
        for (const QString& n : paths.renamed())
            log.warn(QStringLiteral("Renamed for this drive: %1").arg(n));
    }

    // Free space: the packs at worst as large as their files, and the copied loose files.
    FW_TRY(emptyOrNewFolder(outFolder));
    FW_TRY(ensureFreeSpace(outFolder, rep.packedBytes + copiedLooseBytes + runtime.size(), o.freeSpaceMarginBytes));

    if (progress && o.packPhase >= 0)
        progress->beginPhase(o.packPhase, rep.packedBytes + rep.looseBytes);
    for (const QString& d : std::as_const(emptyDirTargets))
        if (!QDir().mkpath(d))
            return Error(ErrorCode::IoError, QStringLiteral("Could not create %1.").arg(native(d)));

    // Loose files: linked where the caller allows it, else copied.
    std::vector<char> buf(size_t(4) << 20);
    qint64 readOnlyCopies = 0;
    for (size_t k = 0; k < loose.size(); ++k) {
        if (!token.checkpoint())
            return Error::cancelled();
        const Loose& l = loose[k];
        const LooseTarget& t = looseTargets[k];
        const GameItem& it = logical->items()[size_t(l.index)];
        if (!QDir().mkpath(QFileInfo(t.path).absolutePath()))
            return Error(ErrorCode::IoError, QStringLiteral("Could not create %1.").arg(native(QFileInfo(t.path).absolutePath())));
        if (!t.linkFrom.isEmpty() && createHardLink(t.path, t.linkFrom).ok()) {
            ++rep.linkedFiles;
            if (progress)
                progress->advance(it.size);
            continue;
        }
        if (canLink && t.linkFrom.isEmpty())
            ++readOnlyCopies;
        FW_TRY(copyItem(*logical, l.index, t.path, buf, token, progress));
    }
    if (!loose.empty() && !o.linkFromFolder.isEmpty() && rep.sourcePacks == PackKind::None) {
        const qint64 copied = qint64(loose.size()) - rep.linkedFiles;
        if (!canLink)
            log.info(QStringLiteral("The %1 loose file%2 copied: the work folder is on another drive than the game, or the "
                                    "drive has no hard links.")
                         .arg(formatCount(copied), copied == 1 ? QStringLiteral(" was") : QStringLiteral("s were")));
        else if (copied > 0)
            log.info(QStringLiteral("%1 of %2 loose files were copied instead of linked (%3 read-only in the source).")
                         .arg(formatCount(copied), formatCount(qint64(loose.size())), formatCount(readOnlyCopies)));
    }

    // The runtime (only this file of fakelib/).
    if (!QDir().mkpath(QFileInfo(runtimeTarget).absolutePath()))
        return Error(ErrorCode::IoError, QStringLiteral("Could not create %1.").arg(native(QFileInfo(runtimeTarget).absolutePath())));
    const qint64 runtimeTime = QDateTime::currentSecsSinceEpoch(); // the time its index row carries
    FW_TRY(writeFileAtomic(runtimeTarget, runtime));
    if (rep.previousRuntime.kind != RuntimeKind::None && !rep.previousRuntime.bundled)
        log.info(QStringLiteral("fakelib/libSceAmpr.sprx: %1 replaced by %2.").arg(rep.previousRuntime.describe(), rep.runtime));

    // The packs.
    std::vector<PackInput> inputs;
    inputs.reserve(packed.size());
    for (int i : packed) {
        const GameItem& it = logical->items()[size_t(i)];
        PackInput in;
        in.path = it.path.toUtf8();
        in.size = it.size;
        in.open = [logical, i]() -> Result<std::unique_ptr<SequentialReader>> { return itemReader(logical, i); };
        inputs.push_back(std::move(in));
    }
    ZstdPackOptions zo;
    zo.level = o.level;
    zo.threads = o.threads;
    zo.setProgressTotal = o.packPhase < 0; // the caller's phase already counts the loose files
    FW_TRY_ASSIGN(ZstdPackResult pr, writeZstdPack(std::move(inputs), zo, outFolder, token, progress));
    rep.storedBytes = pr.storedBytes;
    rep.volumes = qint64(pr.volumes.size());
    rep.threads = pr.threads;
    log.info(QStringLiteral("Packed %1 file%2 into %3 volume%4: %5 → %6 (%7 zstd chunks, %8 stored raw), %9 thread%10.")
                 .arg(formatCount(rep.packedFiles), rep.packedFiles == 1 ? QString() : QStringLiteral("s"))
                 .arg(rep.volumes)
                 .arg(rep.volumes == 1 ? QString() : QStringLiteral("s"))
                 .arg(formatBytes(rep.packedBytes), formatBytes(rep.storedBytes), formatCount(pr.zstdChunks), formatCount(pr.rawChunks))
                 .arg(pr.threads)
                 .arg(pr.threads == 1 ? QString() : QStringLiteral("s")));

    if (o.verify) {
        if (progress && o.verifyPhase >= 0)
            progress->beginPhase(o.verifyPhase, pr.storedBytes);
        // A cancel here keeps the finished folder, as with the check turned
        // off; a pack that does not read back as written fails as always, and
        // while the app is closing nothing is kept (the cancel stands).
        const Status checked = verifyWrittenZstdPack(outFolder, pr, token, progress);
        rep.verifyCancelled = !checked.ok() && checked.error().code != ErrorCode::VerificationFailed && token.keepFinishedWork();
        if (!checked.ok() && !rep.verifyCancelled)
            return checked.error();
        if (rep.verifyCancelled)
            log.warn(QStringLiteral("Checking the packs was cancelled, so they are not verified."));
        else
            log.info(QStringLiteral("Read every pack volume back: all %1 chunks match.").arg(formatCount(qint64(pr.index.chunks.size()))));
    }

    // ampr_emu.index lists every file of the game as the game sees it: the
    // loose files, the new runtime and the packed files (served from the packs),
    // so AMPR path ids keep working. Written whatever the token says: the
    // folder is complete only with it.
    std::vector<pfs::AmprFile> indexFiles;
    indexFiles.reserve(loose.size() + packed.size() + 1);
    for (const Loose& l : loose)
        indexFiles.push_back(amprFile(logical->items()[size_t(l.index)], logical->items()[size_t(l.index)].size));
    for (int i : packed)
        indexFiles.push_back(amprFile(logical->items()[size_t(i)], logical->items()[size_t(i)].size));
    {
        GameItem rt;
        rt.path = fakelib + QStringLiteral("/libSceAmpr.sprx");
        rt.mtime = runtimeTime;
        indexFiles.push_back(amprFile(rt, runtime.size()));
    }
    FW_TRY_ASSIGN(rep.amprIndexWritten, writeAmprIndex(outFolder, std::move(indexFiles), rep.warnings, log));
    rep.elapsedMs = timer.elapsed();
    return rep;
}

Result<UnpackReport> unpackGame(std::shared_ptr<GameFiles> source, const QString& outFolder, const UnpackOptions& o,
                                const CancellationToken& token, ProgressReporter* progress, const Log& log)
{
    UnpackReport rep;
    rep.kind = detectPackKind(*source);
    FW_TRY_ASSIGN(std::shared_ptr<GameFiles> logical, logicalGame(source, token));
    if (auto* u = dynamic_cast<UnpackedGame*>(logical.get())) {
        rep.droppedFiles = u->droppedFiles();
        rep.unpackedFiles = u->packedFiles();
    }
    bool hadAmprIndex = false;
    qint64 total = 0;
    std::vector<int> files;
    QStringList dirs;
    for (int i = 0; i < int(logical->items().size()); ++i) {
        const GameItem& it = logical->items()[size_t(i)];
        if (o.exclude && o.exclude(it.path, it.isDir))
            continue;
        if (it.isDir) {
            dirs << it.path;
            continue;
        }
        if (isAmprIndexPath(it.path)) {
            hadAmprIndex = true;
            if (o.refreshAmprIndex && rep.kind != PackKind::None)
                continue; // rewritten for the unpacked files
        }
        if (rep.kind != PackKind::None && packJobDrops(it.path)) {
            rep.droppedFiles << it.path; // the old journal and logs name files by the old index's order
            continue;
        }
        files.push_back(i);
        total += it.size;
    }
    rep.warnings = logical->warnings();
    // The names come from a pack index or an image: every target is checked
    // to lie inside the output before anything is written, and gets the name
    // this drive can store.
    HostPaths paths(outFolder);
    QStringList dirTargets;
    dirTargets.reserve(dirs.size());
    for (const QString& d : std::as_const(dirs)) {
        FW_TRY_ASSIGN(const QString target, paths.add(d, true));
        dirTargets << target;
    }
    std::vector<QString> fileTargets;
    fileTargets.reserve(files.size());
    for (int i : files) {
        const QString& stored = logical->items()[size_t(i)].path;
        FW_TRY_ASSIGN(QString target, paths.add(stored));
        if (const QString rel = paths.relative(stored); rel != stored)
            rep.hostPaths.insert(stored, rel);
        fileTargets.push_back(std::move(target));
    }
    rep.renamed = paths.renamed();
    for (const QString& n : std::as_const(rep.renamed))
        log.info(QStringLiteral("Renamed for this drive: %1").arg(n));

    FW_TRY(emptyOrNewFolder(outFolder));
    FW_TRY(ensureFreeSpace(outFolder, total, o.freeSpaceMarginBytes));
    if (progress)
        progress->setTotal(total);
    for (const QString& d : std::as_const(dirTargets))
        if (!QDir().mkpath(d))
            return Error(ErrorCode::IoError, QStringLiteral("Could not create %1.").arg(native(d)));
    std::vector<char> buf(size_t(4) << 20);
    for (size_t k = 0; k < files.size(); ++k) {
        const int i = files[k];
        const GameItem& it = logical->items()[size_t(i)];
        if (progress)
            progress->setCurrentItem(it.path);
        if (!QDir().mkpath(QFileInfo(fileTargets[k]).absolutePath()))
            return Error(ErrorCode::IoError,
                         QStringLiteral("Could not create %1.").arg(native(QFileInfo(fileTargets[k]).absolutePath())));
        FW_TRY(copyItem(*logical, i, fileTargets[k], buf, token, progress));
        ++rep.files;
        rep.bytes += it.size;
    }
    if (hadAmprIndex && o.refreshAmprIndex && rep.kind != PackKind::None) {
        FW_TRY_ASSIGN(rep.amprIndexWritten, writeAmprIndexForFolder(outFolder, token, rep.warnings, log));
        if (rep.amprIndexWritten)
            log.info(QStringLiteral("Wrote a fresh ampr_emu.index for the unpacked files."));
    }
    return rep;
}

} // namespace fw::ampr
