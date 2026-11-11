#include "engines/ufs2/Ufs2Ampr.h"

#include "core/FileUtil.h"
#include "engines/ufs2/Ufs2Reader.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <algorithm>

namespace fw::ufs2 {

namespace {

// The root's ampr_emu.index, or -1. Dumps live on Windows, so the name is
// matched without regard to case. `dir`: look for a folder of that name instead.
int rootIndexNode(const SourceTree& tree, bool dir = false)
{
    if (tree.nodes.empty())
        return -1;
    for (int c : tree.liveChildren(0)) {
        const TreeNode& n = tree.nodes[size_t(c)];
        if (n.isDir() == dir && n.name.compare(pfs::kAmprIndexName, Qt::CaseInsensitive) == 0)
            return c;
    }
    return -1;
}

std::vector<pfs::AmprFile> amprFiles(const SourceTree& tree)
{
    std::vector<pfs::AmprFile> files;
    files.reserve(size_t(tree.fileCount));
    // Paths are put together on the way down: relativePath() per node would
    // walk up again for each of a dump's 100,000 files.
    std::vector<std::pair<int, QByteArray>> stack{{0, QByteArray()}};
    while (!stack.empty()) {
        const auto [dir, prefix] = stack.back();
        stack.pop_back();
        for (int c : tree.liveChildren(dir)) {
            const TreeNode& n = tree.nodes[size_t(c)];
            const QByteArray rel = prefix + n.rawName;
            if (n.isDir())
                stack.emplace_back(c, rel + '/');
            else
                files.push_back({rel, n.size, n.mtime});
        }
    }
    return files;
}

Result<QString> saveIndex(const QString& tempDir, const QByteArray& index)
{
    const QString dir = QDir(tempDir).filePath(QStringLiteral("ampr-index"));
    if (!QDir().mkpath(dir))
        return Error(ErrorCode::IoError, QStringLiteral("Could not create the folder %1.").arg(QDir::toNativeSeparators(dir)));
    const QString file = QDir(dir).filePath(pfs::kAmprIndexName);
    FW_TRY(writeFileAtomic(file, index));
    return file;
}

} // namespace

QByteArray amprIndexFor(const SourceTree& tree)
{
    return pfs::buildAmprIndex(pfs::amprToolRows(amprFiles(tree)).rows);
}

pfs::AmprIndexDecision decideAmprIndex(const SourceTree& tree, pfs::AmprMode mode)
{
    // A folder of that name is in the way of the file: the game stays as it is.
    if (mode == pfs::AmprMode::Off || tree.nodes.empty() || rootIndexNode(tree, true) >= 0)
        return {};
    const std::vector<pfs::AmprFile> files = amprFiles(tree);
    const int own = rootIndexNode(tree);
    pfs::AmprIndexState state;
    state.hasLibrary = pfs::hasAmprLibrary(files);
    state.hasIndex = own >= 0;
    state.indexSize = own >= 0 ? tree.nodes[size_t(own)].size : 0;
    for (int c : tree.liveChildren(0)) {
        const TreeNode& n = tree.nodes[size_t(c)];
        if (!n.isDir() && pfs::isAmprPackIndexName(n.name))
            state.hasAssetPacks = true;
    }
    const auto fresh = [&files] { return pfs::buildAmprIndex(pfs::amprToolRows(files).rows); };
    const auto dumpIndex = [&]() -> Result<QByteArray> {
        const TreeNode& n = tree.nodes[size_t(own)];
        QFile f(toLongPath(n.localPath));
        if (n.localPath.isEmpty() || !f.open(QIODevice::ReadOnly))
            return Error(ErrorCode::IoError, QStringLiteral("Could not read %1.").arg(QDir::toNativeSeparators(n.localPath)),
                         f.errorString());
        return f.read(n.size + 1);
    };
    return pfs::decideAmprIndex(mode, state, fresh, dumpIndex);
}

void placeAmprIndex(SourceTree& tree, const QString& indexFile)
{
    if (tree.nodes.empty())
        return;
    const QFileInfo fi(indexFile);
    if (const int own = rootIndexNode(tree); own >= 0) {
        TreeNode& n = tree.nodes[size_t(own)];
        n.localPath = fi.absoluteFilePath();
        n.imageIno = 0;
        n.size = fi.size();
        n.mtime = fi.lastModified().toSecsSinceEpoch();
        tree.recount();
        return;
    }
    TreeNode n;
    n.name = pfs::kAmprIndexName;
    n.rawName = n.name.toUtf8();
    n.kind = NodeKind::File;
    n.size = fi.size();
    n.mtime = fi.lastModified().toSecsSinceEpoch();
    n.parent = 0;
    n.localPath = fi.absoluteFilePath();
    // Children keep the folder's listing order (Ufs2Tree.h): the new entry
    // goes where NTFS lists a file of that name, before the first name that
    // sorts after it in upper case.
    const QString key = n.name.toUpper();
    std::vector<int>& kids = tree.nodes.front().children;
    const auto at = std::find_if(kids.begin(), kids.end(),
                                 [&](int c) { return tree.nodes[size_t(c)].name.toUpper().compare(key) > 0; });
    const int index = int(tree.nodes.size());
    kids.insert(at, index);
    tree.nodes.push_back(std::move(n));
    tree.recount();
}

Result<AmprPlacement> applyAmprIndex(SourceTree& tree, pfs::AmprMode mode, const QString& tempDir)
{
    AmprPlacement out;
    const pfs::AmprIndexDecision d = decideAmprIndex(tree, mode);
    out.action = d.action;
    if (!pfs::amprActionChangesImage(d.action))
        return out;
    FW_TRY_ASSIGN(out.indexFile, saveIndex(tempDir, d.index));
    placeAmprIndex(tree, out.indexFile);
    return out;
}

bool acceptBuiltAmprIndex(SourceTree& tree, const QString& image, const QString& tempDir)
{
    if (tree.nodes.empty() || rootIndexNode(tree, true) >= 0 || !pfs::hasAmprLibrary(amprFiles(tree)))
        return false;
    const QByteArray fresh = amprIndexFor(tree);
    if (fresh.isEmpty())
        return false;
    auto reader = Ufs2Reader::open(image);
    if (!reader.ok())
        return false;
    const int own = rootIndexNode(tree);
    const QString name = own >= 0 ? tree.nodes[size_t(own)].name : pfs::kAmprIndexName;
    const auto ino = reader.value()->resolve(u'/' + name);
    if (!ino.ok())
        return false;
    const auto inode = reader.value()->readInode(ino.value());
    if (!inode.ok() || !inode.value().isReg() || inode.value().size != fresh.size())
        return false; // not the index of these files
    auto stream = reader.value()->openFile(ino.value());
    if (!stream.ok())
        return false;
    QByteArray inImage(fresh.size(), '\0');
    const auto got = stream.value()->read(inImage.data(), inImage.size());
    if (!got.ok() || got.value() != inImage.size())
        return false;
    if (inImage != fresh && !pfs::amprIndexesDifferOnlyInTimes(inImage, fresh))
        return false;
    // The folder's own index, when it is the same, is compared as usual.
    if (own >= 0 && tree.nodes[size_t(own)].size == inImage.size()) {
        QFile f(toLongPath(tree.nodes[size_t(own)].localPath));
        if (f.open(QIODevice::ReadOnly) && f.readAll() == inImage)
            return false;
    }
    auto saved = saveIndex(tempDir, inImage);
    if (!saved.ok())
        return false;
    placeAmprIndex(tree, saved.value());
    return true;
}

} // namespace fw::ufs2
