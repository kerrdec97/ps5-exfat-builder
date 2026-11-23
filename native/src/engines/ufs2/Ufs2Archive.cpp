#include "engines/ufs2/Ufs2Archive.h"

#include "core/FileUtil.h"

#include <QFile>
#include <QFileInfo>
#include <QHash>

#include <algorithm>

namespace fw::ufs2 {

Result<SourceTree> treeFromArchive(const archive::GameListing& game, const QDateTime& buildTime, const CancellationToken& token)
{
    const qint64 now = (buildTime.isValid() ? buildTime : QDateTime::currentDateTime()).toSecsSinceEpoch();
    SourceTree tree;
    tree.rootPath = game.archive;
    TreeNode root;
    root.kind = NodeKind::Dir;
    root.mtime = now;
    tree.nodes.push_back(std::move(root));
    QHash<QString, int> dirs; // folder path in lower case -> node
    dirs.insert(QString(), 0);
    qint64 seen = 0;
    for (size_t i = 0; i < game.items.size(); ++i) {
        if ((++seen & 0xFF) == 0 && !token.checkpoint())
            return Error::cancelled();
        const archive::GameItem& item = game.items[i];
        const qsizetype slash = item.path.lastIndexOf(u'/');
        const auto parent = dirs.constFind(slash < 0 ? QString() : item.path.left(slash).toLower());
        if (parent == dirs.cend())
            return Error(ErrorCode::Internal, QStringLiteral("The archive's list of files is inconsistent."), item.path);
        TreeNode n;
        n.name = slash < 0 ? item.path : item.path.mid(slash + 1);
        auto raw = encodeName(n.name);
        if (!raw.ok()) {
            Error e = raw.takeError();
            e.detail = item.path;
            return e;
        }
        n.rawName = std::move(raw).value();
        n.parent = *parent;
        if (item.isDir) {
            n.kind = NodeKind::Dir;
            n.mtime = now;
        } else {
            n.kind = NodeKind::File;
            n.size = item.size;
            n.mtime = item.modified.isValid() ? item.modified.toSecsSinceEpoch() : now;
            n.archiveItem = int(i);
        }
        const int index = int(tree.nodes.size());
        tree.nodes[size_t(*parent)].children.push_back(index);
        if (item.isDir)
            dirs.insert(item.path.toLower(), index);
        tree.nodes.push_back(std::move(n));
    }
    // The order an NTFS listing gives (its index is sorted by the names in
    // upper case), the order UFS2Tool takes the entries in; as placeAmprIndex().
    for (TreeNode& d : tree.nodes) {
        if (!d.isDir() || d.children.size() < 2)
            continue;
        std::vector<std::pair<QString, int>> keyed;
        keyed.reserve(d.children.size());
        for (int k : d.children)
            keyed.emplace_back(tree.nodes[size_t(k)].name.toUpper(), k);
        std::stable_sort(keyed.begin(), keyed.end(), [&](const auto& a, const auto& b) {
            const int c = a.first.compare(b.first);
            return c != 0 ? c < 0 : tree.nodes[size_t(a.second)].name.compare(tree.nodes[size_t(b.second)].name) < 0;
        });
        for (size_t k = 0; k < keyed.size(); ++k)
            d.children[k] = keyed[k].second;
    }
    tree.recount();
    return tree;
}

std::vector<archive::Placement> archivePlacements(const SourceTree& tree, const Ufs2Writer& writer,
                                                  const archive::GameListing& game)
{
    std::vector<archive::Placement> out(game.items.size());
    std::vector<std::vector<archive::Extent>> extents = writer.fileExtents();
    for (size_t n = 0; n < tree.nodes.size(); ++n) {
        const TreeNode& node = tree.nodes[n];
        if (node.isDir() || node.removed || node.archiveItem < 0 || size_t(node.archiveItem) >= out.size())
            continue;
        archive::Placement& p = out[size_t(node.archiveItem)];
        p.write = true;
        p.extents = std::move(extents[n]);
    }
    return out;
}

Result<ArchiveWriteStats> writeImageFromArchive(Ufs2Writer& writer, SourceTree& tree, const archive::GameListing& game,
                                                const QString& path, qint64 bufferBytes, const CancellationToken& token,
                                                ProgressReporter* progress)
{
    ArchiveWriteStats stats;
    FW_TRY_ASSIGN(stats.metadata, writer.write(path, token, progress));
    FW_TRY_ASSIGN(stats.stream, archive::streamGameInto(game, archivePlacements(tree, writer, game), path, bufferBytes, token,
                                                        progress));
    {
        QFile f(toLongPath(path));
        if (!f.open(QIODevice::ReadWrite))
            return Error(ErrorCode::IoError, QStringLiteral("Could not finish the image file."), f.errorString());
        FW_TRY(flushToDisk(f, QStringLiteral("the image")));
    }
    for (TreeNode& n : tree.nodes) {
        if (n.isDir() || n.archiveItem < 0 || size_t(n.archiveItem) >= stats.stream.digests.size())
            continue;
        n.hasDigest = true;
        n.digest = stats.stream.digests[size_t(n.archiveItem)];
    }
    return stats;
}

} // namespace fw::ufs2
