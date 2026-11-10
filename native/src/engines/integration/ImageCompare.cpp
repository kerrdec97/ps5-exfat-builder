#include "engines/integration/ImageCompare.h"

#include "core/FileUtil.h"

#include <QJsonArray>

#include <algorithm>
#include <cstring>

namespace fw::integration {

namespace {

qint64 roundUp(qint64 v, qint64 block) { return block > 0 ? (v + block - 1) / block * block : v; }

QString nameOf(const QString& path) { return path.section(u'/', -1); }

// Files the PFS builder adds itself (PfsSource: empty-folder markers, the
// AMPR index); a difference in them is expected when one side is PFS.
bool isPfsBuilderFile(const QString& path)
{
    const QString n = nameOf(path);
    return n == QLatin1String(".pfskeep") || n.compare(QLatin1String("ampr_emu.index"), Qt::CaseInsensitive) == 0;
}

// ampr_emu.index at the top of the game: a build puts a new one into the image
// when the dump has none or an outdated one (exfat.amprIndex, ufs2.amprIndex,
// pfs.amprIndex), so an image and the folder it was built from can differ in it.
bool isRootAmprIndex(const QString& path)
{
    return path.compare(QLatin1String("ampr_emu.index"), Qt::CaseInsensitive) == 0;
}

template <typename T>
void keep(std::vector<T>& list, int limit, T value)
{
    if (int(list.size()) < limit)
        list.push_back(std::move(value));
}

void keep(QStringList& list, int limit, const QString& value)
{
    if (list.size() < limit)
        list << value;
}

QString plural(qint64 n, const QString& one, const QString& many)
{
    return QStringLiteral("%1 %2").arg(formatCount(n), n == 1 ? one : many);
}

} // namespace

QString compareDepthName(CompareDepth depth)
{
    return depth == CompareDepth::Structure ? QStringLiteral("structure") : QStringLiteral("full");
}

bool CompareReport::truncated() const
{
    return missingCount > missing.size() || extraCount > extra.size() || missingDirCount > missingDirs.size()
        || extraDirCount > extraDirs.size() || sizeCount > qint64(sizeDifferent.size())
        || contentCount > qint64(contentDifferent.size()) || unreadableCount > qint64(unreadable.size());
}

QString CompareReport::summary() const
{
    if (identical()) {
        const QString what = plural(filesA, QStringLiteral("file"), QStringLiteral("files"));
        const QString dirs = plural(dirsA, QStringLiteral("folder"), QStringLiteral("folders"));
        return depth == CompareDepth::Full
            ? QStringLiteral("No differences: %1 and %2 match, contents compared byte for byte.").arg(what, dirs)
            : QStringLiteral("No differences in names and sizes: %1 and %2 match (contents not compared).").arg(what, dirs);
    }
    QStringList parts;
    if (missingCount + missingDirCount > 0)
        parts << QStringLiteral("%1 only in A").arg(formatCount(missingCount + missingDirCount));
    if (extraCount + extraDirCount > 0)
        parts << QStringLiteral("%1 only in B").arg(formatCount(extraCount + extraDirCount));
    if (sizeCount > 0)
        parts << plural(sizeCount, QStringLiteral("different size"), QStringLiteral("different sizes"));
    if (contentCount > 0)
        parts << QStringLiteral("%1 with different content").arg(formatCount(contentCount));
    if (unreadableCount > 0)
        parts << QStringLiteral("%1 unreadable").arg(formatCount(unreadableCount));
    return QStringLiteral("%1: %2.").arg(plural(differenceCount(), QStringLiteral("difference"), QStringLiteral("differences")),
                                         parts.join(QStringLiteral(", ")));
}

QJsonObject CompareReport::toJson() const
{
    QJsonObject counts{
        {QStringLiteral("missing"), missingCount},
        {QStringLiteral("extra"), extraCount},
        {QStringLiteral("missingDirs"), missingDirCount},
        {QStringLiteral("extraDirs"), extraDirCount},
        {QStringLiteral("sizeDifferent"), sizeCount},
        {QStringLiteral("contentDifferent"), contentCount},
        {QStringLiteral("unreadable"), unreadableCount},
        {QStringLiteral("differences"), differenceCount()},
        {QStringLiteral("filesInBoth"), filesInBoth},
        {QStringLiteral("contentCompared"), contentCompared},
        {QStringLiteral("bytesCompared"), bytesCompared},
        {QStringLiteral("padded"), paddedFiles},
    };
    const auto side = [](const QString& path, const QString& kind, qint64 files, qint64 dirs, qint64 bytes) {
        return QJsonObject{{QStringLiteral("path"), path}, {QStringLiteral("kind"), kind}, {QStringLiteral("files"), files},
                           {QStringLiteral("dirs"), dirs}, {QStringLiteral("bytes"), bytes}};
    };
    QJsonArray sizes, contents, unread;
    for (const SizeDifference& d : sizeDifferent)
        sizes.append(QJsonObject{{QStringLiteral("path"), d.path}, {QStringLiteral("a"), d.a}, {QStringLiteral("b"), d.b}});
    for (const ContentDifference& d : contentDifferent)
        contents.append(QJsonObject{{QStringLiteral("path"), d.path}, {QStringLiteral("offset"), d.offset}});
    for (const UnreadableFile& u : unreadable)
        unread.append(QJsonObject{{QStringLiteral("path"), u.path}, {QStringLiteral("message"), u.message}});
    return QJsonObject{
        {QStringLiteral("a"), side(a, aKind, filesA, dirsA, bytesA)},
        {QStringLiteral("b"), side(b, bKind, filesB, dirsB, bytesB)},
        {QStringLiteral("depth"), compareDepthName(depth)},
        {QStringLiteral("identical"), identical()},
        {QStringLiteral("counts"), counts},
        {QStringLiteral("missing"), QJsonArray::fromStringList(missing)},
        {QStringLiteral("extra"), QJsonArray::fromStringList(extra)},
        {QStringLiteral("missingDirs"), QJsonArray::fromStringList(missingDirs)},
        {QStringLiteral("extraDirs"), QJsonArray::fromStringList(extraDirs)},
        {QStringLiteral("sizeDifferent"), sizes},
        {QStringLiteral("contentDifferent"), contents},
        {QStringLiteral("unreadable"), unread},
        {QStringLiteral("listLimit"), listLimit},
        {QStringLiteral("truncated"), truncated()},
        {QStringLiteral("warnings"), QJsonArray::fromStringList(warnings)},
    };
}

Result<CompareReport> compareTrees(FileTree& a, FileTree& b, const CompareOptions& options,
                                   const CancellationToken& token, ProgressReporter* progress, int phase)
{
    CompareReport rep;
    rep.a = a.rootPath();
    rep.b = b.rootPath();
    rep.aKind = a.describe();
    rep.bKind = b.describe();
    rep.depth = options.depth;
    rep.listLimit = std::max(0, options.listLimit);
    rep.filesA = a.fileCount();
    rep.filesB = b.fileCount();
    rep.dirsA = a.dirCount();
    rep.dirsB = b.dirCount();
    rep.bytesA = a.totalBytes();
    rep.bytesB = b.totalBytes();
    const int limit = rep.listLimit;
    qint64 pfsArtefacts = 0;
    // An image against a folder: a difference in the top ampr_emu.index is
    // listed like any other, with a line that says where it comes from.
    const bool imageAndFolder = (a.format() == TreeFormat::Folder) != (b.format() == TreeFormat::Folder);
    bool builtIndexNoted = false;
    const auto noteBuiltIndex = [&](const QString& path) {
        if (!imageAndFolder || builtIndexNoted || !isRootAmprIndex(path))
            return;
        builtIndexNoted = true;
        rep.warnings << QStringLiteral("ampr_emu.index is not the folder's: a build adds this file to the image, or rebuilds it for the "
                                       "game's files, when the game uses the AMPR emulator (the \"AMPR file index\" setting).");
    };

    const auto onlyIn = [&](const TreeItem& it, bool inA) {
        const FileTree& side = inA ? a : b;
        if (it.isDir) {
            ++(inA ? rep.missingDirCount : rep.extraDirCount);
            keep(inA ? rep.missingDirs : rep.extraDirs, limit, it.path);
        } else {
            ++(inA ? rep.missingCount : rep.extraCount);
            keep(inA ? rep.missing : rep.extra, limit, it.path);
            if (side.format() == TreeFormat::Pfs && isPfsBuilderFile(it.path))
                ++pfsArtefacts;
            else if (side.format() != TreeFormat::Folder)
                noteBuiltIndex(it.path);
        }
    };

    // 1. Names and sizes: one merge walk over the two sorted item lists.
    struct Pair {
        int a, b;
        qint64 common; // bytes both sides must hold
    };
    std::vector<Pair> toCompare;
    qint64 work = 0;
    const auto& ia = a.items();
    const auto& ib = b.items();
    size_t i = 0, j = 0;
    qint64 steps = 0;
    while (i < ia.size() || j < ib.size()) {
        if ((++steps & 0x3FF) == 0 && !token.checkpoint())
            return Error::cancelled();
        const int c = i == ia.size() ? 1 : j == ib.size() ? -1 : (ia[i].path < ib[j].path ? -1 : ib[j].path < ia[i].path ? 1 : 0);
        if (c < 0) {
            onlyIn(ia[i++], true);
            continue;
        }
        if (c > 0) {
            onlyIn(ib[j++], false);
            continue;
        }
        const TreeItem& x = ia[i];
        const TreeItem& y = ib[j];
        if (x.isDir != y.isDir) {
            // A file on one side, a folder on the other.
            onlyIn(x, true);
            onlyIn(y, false);
        } else if (!x.isDir) {
            ++rep.filesInBoth;
            qint64 common = -1;
            if (x.size == y.size) {
                common = x.size;
            } else if (x.padBlock > 0 && x.size == roundUp(y.size, x.padBlock)) {
                common = y.size;
            } else if (y.padBlock > 0 && y.size == roundUp(x.size, y.padBlock)) {
                common = x.size;
            }
            if (common < 0) {
                ++rep.sizeCount;
                keep(rep.sizeDifferent, limit, SizeDifference{x.path, x.size, y.size});
                noteBuiltIndex(x.path);
            } else {
                if (x.size != y.size)
                    ++rep.paddedFiles;
                if (options.depth == CompareDepth::Full) {
                    toCompare.push_back({int(i), int(j), common});
                    work += std::max(x.size, y.size);
                }
            }
        }
        ++i;
        ++j;
    }
    if (pfsArtefacts > 0)
        rep.warnings << QStringLiteral("%1 only on the PFS side %2 added by the PFS builder (.pfskeep folder markers, ampr_emu.index).")
                            .arg(plural(pfsArtefacts, QStringLiteral("file"), QStringLiteral("files")),
                                 pfsArtefacts == 1 ? QStringLiteral("is") : QStringLiteral("are"));
    if (rep.paddedFiles > 0)
        rep.warnings << QStringLiteral("%1 compressed in the PFS image %2 stored with zero padding up to the block size; compared without it.")
                            .arg(plural(rep.paddedFiles, QStringLiteral("file"), QStringLiteral("files")),
                                 rep.paddedFiles == 1 ? QStringLiteral("is") : QStringLiteral("are"));

    // 2. Contents (full depth): both sides streamed and compared chunk by chunk.
    if (progress)
        progress->beginPhase(phase, work, qint64(toCompare.size()));
    if (options.depth != CompareDepth::Full)
        return rep;
    const qint64 chunk = std::max<qint64>(4096, options.chunkBytes);
    QByteArray bufA(qsizetype(chunk), Qt::Uninitialized), bufB(qsizetype(chunk), Qt::Uninitialized);
    for (const Pair& p : toCompare) {
        if (!token.checkpoint())
            return Error::cancelled();
        const TreeItem& x = ia[size_t(p.a)];
        const TreeItem& y = ib[size_t(p.b)];
        if (progress)
            progress->setCurrentItem(x.path);
        const qint64 total = std::max(x.size, y.size);
        qint64 consumed = 0;
        bool differs = false, failed = false;
        const auto unreadable = [&](const QString& message) {
            ++rep.unreadableCount;
            keep(rep.unreadable, limit, UnreadableFile{x.path, message});
            failed = true;
        };
        // Reads exactly `n` bytes of one side; false (and the file listed as unreadable) otherwise.
        const auto readExact = [&](FileTree& t, const TreeItem& it, int index, qint64 at, char* buf, qint64 n, const QString& label) {
            qint64 got = 0;
            while (got < n) {
                auto r = t.read(index, at + got, buf + got, n - got);
                if (!r.ok()) {
                    unreadable(QStringLiteral("%1: %2").arg(label, r.error().message));
                    return false;
                }
                if (r.value() <= 0) {
                    unreadable(QStringLiteral("%1: the file ended at byte %2 of %3.").arg(label, formatCount(at + got), formatCount(it.size)));
                    return false;
                }
                got += r.value();
            }
            return true;
        };
        qint64 pos = 0;
        while (pos < p.common && !differs && !failed) {
            if (!token.checkpoint())
                return Error::cancelled();
            const qint64 n = std::min(chunk, p.common - pos);
            if (!readExact(a, x, p.a, pos, bufA.data(), n, QStringLiteral("A")) || !readExact(b, y, p.b, pos, bufB.data(), n, QStringLiteral("B")))
                break;
            if (std::memcmp(bufA.constData(), bufB.constData(), size_t(n)) != 0) {
                qint64 k = 0;
                while (k < n && bufA.at(qsizetype(k)) == bufB.at(qsizetype(k)))
                    ++k;
                ++rep.contentCount;
                keep(rep.contentDifferent, limit, ContentDifference{x.path, pos + k});
                differs = true;
                noteBuiltIndex(x.path);
            }
            pos += n;
            consumed += n;
            rep.bytesCompared += n;
            if (progress)
                progress->advance(n);
        }
        // The padded side's tail must be zeros.
        if (!differs && !failed && total > p.common) {
            const bool padA = x.size > p.common;
            FileTree& t = padA ? a : b;
            const TreeItem& it = padA ? x : y;
            const int index = padA ? p.a : p.b;
            while (pos < total && !differs && !failed) {
                const qint64 n = std::min(chunk, total - pos);
                if (!readExact(t, it, index, pos, bufA.data(), n, padA ? QStringLiteral("A") : QStringLiteral("B")))
                    break;
                for (qint64 k = 0; k < n; ++k)
                    if (bufA.at(qsizetype(k)) != 0) {
                        ++rep.contentCount;
                        keep(rep.contentDifferent, limit, ContentDifference{x.path, pos + k});
                        differs = true;
                        break;
                    }
                pos += n;
                consumed += n;
                if (progress)
                    progress->advance(n);
            }
        }
        if (!token.checkpoint())
            return Error::cancelled();
        if (progress) {
            if (consumed < total)
                progress->advance(total - consumed);
            progress->advanceFiles(1);
        }
        if (!failed)
            ++rep.contentCompared;
    }
    return rep;
}

} // namespace fw::integration
