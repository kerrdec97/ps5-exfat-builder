#include "engines/exfat/ExfatArchive.h"

#include "core/FileUtil.h"
#include "engines/exfat/ExfatFormat.h"
#include "engines/exfat/PlatformFs.h"

#include <QFile>
#include <QFileInfo>
#include <QHash>

#include <algorithm>

namespace fw::exfat {

Result<SourceTree> treeFromArchive(const archive::GameListing& game, const ScanRules& rules, const QDateTime& buildTime,
                                   const CancellationToken& token)
{
    const ExcludeMatcher matcher(rules);
    const UpcaseTable& up = UpcaseTable::recommended();
    const QDateTime now = buildTime.isValid() ? buildTime.toLocalTime() : QDateTime::currentDateTime();

    SourceTree tree;
    tree.root = game.archive;
    {
        SourceNode r;
        r.name = game.rootName();
        r.isDir = true;
        r.attributes = kAttrDirectory;
        r.created = r.modified = r.accessed = now;
        tree.nodes.push_back(std::move(r));
    }
    QHash<QString, int> dirs; // folder path in lower case -> node ("" is the root)
    dirs.insert(QString(), 0);
    qint64 seen = 0;
    // The items come with each folder before what it holds: a folder the
    // rules leave out takes everything in it along (counted once, as the
    // scan, which never enters it).
    for (size_t i = 0; i < game.items.size(); ++i) {
        if ((++seen & 0xFF) == 0 && !token.checkpoint())
            return Error::cancelled();
        const archive::GameItem& item = game.items[i];
        const qsizetype slash = item.path.lastIndexOf(u'/');
        const QString parentPath = slash < 0 ? QString() : item.path.left(slash);
        const auto parent = dirs.constFind(parentPath.toLower());
        if (parent == dirs.cend())
            continue; // inside a folder that was left out
        const QString name = slash < 0 ? item.path : item.path.mid(slash + 1);
        const QString& rel = item.path;
        // An unpacked file is hidden only where a leading dot makes it so.
#ifdef Q_OS_WIN
        const bool hidden = false;
#else
        const bool hidden = name.startsWith(u'.');
#endif
        const bool excluded = item.isDir ? matcher.excludesDir(name, rel) : matcher.excludesFile(name, rel, hidden);
        if (excluded) {
            ++tree.excludedCount;
            if (tree.excludedSamples.size() < 20)
                tree.excludedSamples << (item.isDir ? rel + u'/' : rel);
            continue;
        }
        SourceNode n;
        n.name = name;
        n.relativePath = rel;
        n.isDir = item.isDir;
        n.parent = *parent;
        if (item.isDir) {
            n.attributes = kAttrDirectory;
            n.created = n.modified = n.accessed = now;
            ++tree.dirCount;
        } else {
            n.size = item.size;
            n.attributes = kAttrArchive; // a new file
            n.modified = item.modified.isValid() ? item.modified.toLocalTime() : now;
            n.created = n.accessed = now;
            n.archiveItem = int(i);
            ++tree.fileCount;
            tree.totalBytes += item.size;
        }
        const int index = int(tree.nodes.size());
        tree.nodes[size_t(*parent)].children.push_back(index);
        if (item.isDir)
            dirs.insert(rel.toLower(), index);
        tree.nodes.push_back(std::move(n));
    }
    // The scan's order: by up-cased name, then by name.
    for (SourceNode& d : tree.nodes) {
        if (!d.isDir || d.children.size() < 2)
            continue;
        std::vector<std::pair<QString, int>> keyed;
        keyed.reserve(d.children.size());
        for (int k : d.children)
            keyed.emplace_back(up.upcase(tree.nodes[size_t(k)].name), k);
        std::sort(keyed.begin(), keyed.end(), [&](const auto& a, const auto& b) {
            const int c = a.first.compare(b.first, Qt::CaseSensitive);
            return c != 0 ? c < 0
                          : tree.nodes[size_t(a.second)].name.compare(tree.nodes[size_t(b.second)].name, Qt::CaseSensitive) < 0;
        });
        for (size_t k = 0; k < keyed.size(); ++k)
            d.children[k] = keyed[k].second;
    }
    return tree;
}

std::vector<archive::Placement> archivePlacements(const SourceTree& tree, const ImagePlan& plan,
                                                  const archive::GameListing& game)
{
    std::vector<archive::Placement> out(game.items.size());
    for (size_t n = 0; n < tree.nodes.size(); ++n) {
        const SourceNode& node = tree.nodes[n];
        if (node.isDir || node.archiveItem < 0 || size_t(node.archiveItem) >= out.size())
            continue;
        archive::Placement& p = out[size_t(node.archiveItem)];
        p.write = true;
        const NodePlacement& at = plan.nodes[n];
        if (node.size > 0) // one contiguous run of clusters
            p.extents.push_back({0, plan.geo.clusterOffset(at.firstCluster), node.size});
    }
    return out;
}

std::vector<ExpectedFile> expectedFromArchive(const SourceTree& tree, const std::vector<quint64>& digests)
{
    std::vector<ExpectedFile> out = expectedFromFolder(tree);
    // expectedFromFolder() lists nodes 1.. in order.
    for (size_t i = 1; i < tree.nodes.size(); ++i) {
        const SourceNode& n = tree.nodes[i];
        ExpectedFile& e = out[i - 1];
        if (n.isDir || n.archiveItem < 0)
            continue;
        e.localPath.clear();
        e.hasDigest = true;
        e.digest = size_t(n.archiveItem) < digests.size() ? digests[size_t(n.archiveItem)] : 0;
    }
    return out;
}

Result<ArchiveWriteStats> writeImageFromArchive(const SourceTree& tree, const ImagePlan& plan,
                                                const archive::GameListing& game, const QString& path,
                                                const WriterOptions& options, const CancellationToken& token,
                                                ProgressReporter* progress, const Log* log)
{
    ArchiveWriteStats stats;
    WriterOptions meta = options;
    meta.leaveArchiveData = true;
    meta.syncToDisk = false;
    FW_TRY_ASSIGN(stats.metadata, writeImage(tree, plan, path, meta, token, progress, log));
    if (log)
        log->debug(QStringLiteral("Metadata and %1 written in %2 ms; now the files from the archive")
                       .arg(formatBytes(stats.metadata.bytesWritten))
                       .arg(stats.metadata.elapsedMs));
    FW_TRY_ASSIGN(stats.stream, archive::streamGameInto(game, archivePlacements(tree, plan, game), path, options.bufferBytes,
                                                        token, progress));
    if (options.syncToDisk) {
        QFile f(toLongPath(path));
        if (!f.open(QIODevice::ReadWrite))
            return fileError(QStringLiteral("Could not write %1.").arg(QFileInfo(path).fileName()), f);
        FW_TRY(syncFile(f, QFileInfo(path).fileName()));
    }
    return stats;
}

} // namespace fw::exfat
