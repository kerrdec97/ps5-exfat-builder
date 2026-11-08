#include "engines/ampr/GamePack.h"

#include "core/FileUtil.h"
#include "core/FsLinks.h"
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
    const QFileInfo fi(outFolder);
    if (fi.exists() && (!fi.isDir() || !QDir(outFolder).isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System)))
        return Error(ErrorCode::AlreadyExists, QStringLiteral("%1 is not empty.").arg(native(outFolder)));
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
// and case as stored).
Result<bool> writeAmprIndex(const QString& folder, std::vector<pfs::AmprFile> files)
{
    pfs::AmprToolRows rows = pfs::amprToolRows(std::move(files));
    const QByteArray index = pfs::buildAmprIndex(rows.rows);
    if (index.isEmpty())
        return false;
    FW_TRY(writeFileAtomic(QDir(folder).filePath(pfs::kAmprIndexName), index));
    return true;
}

// ampr_emu.index for the files of `folder` as they are on disk.
Result<bool> writeAmprIndexForFolder(const QString& folder, const CancellationToken& token)
{
    FW_TRY_ASSIGN(pfs::AmprLocalScan scan, pfs::scanAmprFolder(folder, token));
    return writeAmprIndex(folder, std::move(scan.files));
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
        if (executableKind(head) == ExecutableKind::PlainElf)
            rep.warnings << QStringLiteral("eboot.bin is a plain ELF, not a signed SELF: the console will not start it. Use a "
                                           "fake-signed (backported) eboot.bin.");
    }

    // Free space: the packs at worst as large as their files, and the copied loose files.
    FW_TRY(emptyOrNewFolder(outFolder));
    FW_TRY(ensureFreeSpace(outFolder, rep.packedBytes + (canLink ? 0 : rep.looseBytes) + runtime.size(), o.freeSpaceMarginBytes));

    if (progress && o.packPhase >= 0)
        progress->beginPhase(o.packPhase, rep.packedBytes + rep.looseBytes);
    const QDir out(outFolder);
    // Only folders that hold loose files exist on disk, plus the source's own
    // empty folders: the runtime serves a folder of packed files from the
    // index, and a real (empty) folder of the same name would hide its
    // contents from the game.
    {
        QSet<QString> nonEmpty;
        for (int i = 0; i < int(logical->items().size()); ++i) {
            const QString& p = logical->items()[size_t(i)].path;
            const qsizetype slash = p.lastIndexOf(u'/');
            if (slash > 0)
                nonEmpty.insert(p.left(slash));
        }
        for (const QString& d : dirs)
            if (!nonEmpty.contains(d) && !out.mkpath(d))
                return Error(ErrorCode::IoError, QStringLiteral("Could not create %1.").arg(native(out.filePath(d))));
    }

    // Loose files: linked, else copied.
    std::vector<char> buf(size_t(4) << 20);
    for (const Loose& l : loose) {
        if (!token.checkpoint())
            return Error::cancelled();
        const GameItem& it = logical->items()[size_t(l.index)];
        const QString dst = out.filePath(it.path);
        out.mkpath(QFileInfo(it.path).path());
        // A read-only file is copied: deleting a link later would clear the
        // read-only flag, which all links of a file share, on the user's original.
        const QString original = QDir(o.linkFromFolder).filePath(it.path);
        if (canLink && QFileInfo(original).isWritable() && createHardLink(dst, original).ok()) {
            ++rep.linkedFiles;
            if (progress)
                progress->advance(it.size);
            continue;
        }
        FW_TRY(copyItem(*logical, l.index, dst, buf, token, progress));
    }
    if (canLink && rep.linkedFiles < qint64(loose.size()) && !loose.empty())
        log.info(QStringLiteral("%1 of %2 loose files were copied (the output is on another drive or the file system has no hard links).")
                     .arg(qint64(loose.size()) - rep.linkedFiles)
                     .arg(loose.size()));

    // The runtime (only this file of fakelib/).
    const QString fakelib = folderCaseFor(logical->items(), QStringLiteral("fakelib"));
    out.mkpath(fakelib);
    FW_TRY(writeFileAtomic(out.filePath(fakelib + QStringLiteral("/libSceAmpr.sprx")), runtime));
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
    log.info(QStringLiteral("Packed %1 files into %2 volume%3: %4 -> %5 (%6 zstd chunks, %7 stored raw), %8 threads.")
                 .arg(formatCount(rep.packedFiles))
                 .arg(rep.volumes)
                 .arg(rep.volumes == 1 ? QString() : QStringLiteral("s"))
                 .arg(formatBytes(rep.packedBytes), formatBytes(rep.storedBytes), formatCount(pr.zstdChunks), formatCount(pr.rawChunks))
                 .arg(pr.threads));

    if (o.verify) {
        if (progress && o.verifyPhase >= 0)
            progress->beginPhase(o.verifyPhase, pr.storedBytes);
        FW_TRY(verifyWrittenZstdPack(outFolder, pr, token, progress));
        log.info(QStringLiteral("Read every pack volume back: all %1 chunks match.").arg(formatCount(qint64(pr.index.chunks.size()))));
    }

    // ampr_emu.index lists every file of the game as the game sees it: the
    // loose files, the new runtime and the packed files (served from the packs),
    // so AMPR path ids keep working.
    std::vector<pfs::AmprFile> indexFiles;
    indexFiles.reserve(loose.size() + packed.size() + 1);
    for (const Loose& l : loose)
        indexFiles.push_back(amprFile(logical->items()[size_t(l.index)], logical->items()[size_t(l.index)].size));
    for (int i : packed)
        indexFiles.push_back(amprFile(logical->items()[size_t(i)], logical->items()[size_t(i)].size));
    {
        GameItem rt;
        rt.path = fakelib + QStringLiteral("/libSceAmpr.sprx");
        rt.mtime = QDateTime::currentSecsSinceEpoch();
        indexFiles.push_back(amprFile(rt, runtime.size()));
    }
    FW_TRY_ASSIGN(rep.amprIndexWritten, writeAmprIndex(outFolder, std::move(indexFiles)));
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
    FW_TRY(emptyOrNewFolder(outFolder));
    FW_TRY(ensureFreeSpace(outFolder, total, o.freeSpaceMarginBytes));
    if (progress)
        progress->setTotal(total);
    const QDir out(outFolder);
    for (const QString& d : dirs)
        if (!out.mkpath(d))
            return Error(ErrorCode::IoError, QStringLiteral("Could not create %1.").arg(native(out.filePath(d))));
    std::vector<char> buf(size_t(4) << 20);
    for (int i : files) {
        const GameItem& it = logical->items()[size_t(i)];
        if (progress)
            progress->setCurrentItem(it.path);
        out.mkpath(QFileInfo(it.path).path());
        FW_TRY(copyItem(*logical, i, out.filePath(it.path), buf, token, progress));
        ++rep.files;
        rep.bytes += it.size;
    }
    if (hadAmprIndex && o.refreshAmprIndex && rep.kind != PackKind::None) {
        FW_TRY_ASSIGN(rep.amprIndexWritten, writeAmprIndexForFolder(outFolder, token));
        if (rep.amprIndexWritten)
            log.info(QStringLiteral("Wrote a fresh ampr_emu.index for the unpacked files."));
    }
    return rep;
}

} // namespace fw::ampr
