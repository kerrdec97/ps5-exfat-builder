#include "engines/ufs2/Ufs2Tree.h"

#include "core/FileUtil.h"
#include "core/FolderAccess.h"
#include "engines/ufs2/Ufs2Format.h"
#include "engines/ufs2/Ufs2Reader.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <set>

namespace fw::ufs2 {

QString SourceTree::relativePath(int idx) const
{
    QStringList parts;
    while (idx > 0) {
        parts.prepend(nodes[size_t(idx)].name);
        idx = nodes[size_t(idx)].parent;
    }
    return parts.join(u'/');
}

int SourceTree::findChild(int dir, const QByteArray& rawName) const
{
    if (dir < 0 || size_t(dir) >= nodes.size())
        return -1;
    for (int c : nodes[size_t(dir)].children) {
        const TreeNode& n = nodes[size_t(c)];
        if (!n.removed && n.rawName == rawName)
            return c;
    }
    return -1;
}

int SourceTree::find(const QString& path) const
{
    if (nodes.empty())
        return -1;
    int cur = 0;
    const QStringList parts = path.split(u'/', Qt::SkipEmptyParts);
    for (const QString& part : parts) {
        if (!nodes[size_t(cur)].isDir())
            return -1;
        cur = findChild(cur, part.toUtf8());
        if (cur < 0)
            return -1;
    }
    return cur;
}

std::vector<int> SourceTree::liveChildren(int dir) const
{
    std::vector<int> out;
    for (int c : nodes[size_t(dir)].children)
        if (!nodes[size_t(c)].removed)
            out.push_back(c);
    return out;
}

void SourceTree::recount()
{
    totalBytes = 0;
    fileCount = 0;
    dirCount = 0;
    if (nodes.empty())
        return;
    std::vector<int> stack{0};
    while (!stack.empty()) {
        const int d = stack.back();
        stack.pop_back();
        for (int c : liveChildren(d)) {
            const TreeNode& n = nodes[size_t(c)];
            if (n.isDir()) {
                ++dirCount;
                stack.push_back(c);
            } else {
                ++fileCount;
                totalBytes += n.size;
            }
        }
    }
}

Result<QByteArray> encodeName(const QString& name)
{
    if (name.isEmpty() || name == QLatin1String(".") || name == QLatin1String("..") || name.contains(u'/')
        || name.contains(QChar(0)))
        return Error(ErrorCode::InvalidArgument, QStringLiteral("\"%1\" cannot be used as a file name in a UFS2 image.").arg(name));
    QByteArray raw = name.toUtf8();
    if (raw.size() > kMaxNameLen)
        return Error(ErrorCode::InvalidArgument,
                     QStringLiteral("The name \"%1\" is too long for a UFS2 image (255 bytes at most, this one has %2).")
                         .arg(name.left(60))
                         .arg(raw.size()));
    return raw;
}

namespace {

struct Scanner {
    SourceTree& tree;
    const CancellationToken& token;
    qint64 seen = 0;
    // The folders on the current branch: canonical path and identity (Qt's
    // canonical path does not resolve an NTFS junction, the identity does).
    std::vector<std::pair<QString, QString>> ancestors;

    Status scanDir(int parent, const QString& dirPath)
    {
        const QString canon = QFileInfo(dirPath).canonicalFilePath();
        const QString identity = folderIdentity(dirPath);
        for (const auto& [aCanon, aIdentity] : ancestors)
            if ((!canon.isEmpty() && aCanon == canon) || (!identity.isEmpty() && aIdentity == identity))
                return Error(ErrorCode::InvalidArgument,
                             QStringLiteral("The folder %1 links back into itself, so it cannot be put into an image.")
                                 .arg(QDir::toNativeSeparators(dirPath)));
        if (ancestors.size() > 1024)
            return Error(ErrorCode::Unsupported, QStringLiteral("The folder tree is nested too deeply."));
        ancestors.emplace_back(canon, identity);

        // No sorting: keep the listing order (see Ufs2Tree.h).
        QDirIterator it(dirPath, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
        std::vector<std::pair<int, QString>> subdirs;
        if (!it.hasNext())
            FW_TRY(ensureFolderListable(dirPath)); // empty, or not allowed to look?
        while (it.hasNext()) {
            const QFileInfo fi = it.nextFileInfo();
            if ((++seen & 0xFF) == 0 && !token.checkpoint())
                return Error::cancelled();
            auto raw = encodeName(fi.fileName());
            if (!raw.ok()) {
                Error e = raw.takeError();
                e.detail = fi.absoluteFilePath();
                return e;
            }
            TreeNode n;
            n.name = fi.fileName();
            n.rawName = std::move(raw).value();
            n.parent = parent;
            n.mtime = fi.lastModified().toSecsSinceEpoch();
            // A Windows shortcut (.lnk) is an ordinary file to copy, whatever
            // it points at. QFileInfo describes its target, so the file's own
            // size and time come from an open handle.
            const bool shortcut = fi.isShortcut();
            if (!shortcut && fi.isDir()) {
                n.kind = NodeKind::Dir;
            } else if (shortcut) {
                n.kind = NodeKind::File;
                n.localPath = fi.absoluteFilePath();
                QFile self(toLongPath(n.localPath));
                if (!self.open(QIODevice::ReadOnly))
                    return Error(ErrorCode::IoError,
                                 QStringLiteral("Could not read %1.").arg(QDir::toNativeSeparators(n.localPath)),
                                 self.errorString());
                n.size = self.size();
                n.mtime = self.fileTime(QFileDevice::FileModificationTime).toSecsSinceEpoch();
            } else if (fi.isFile()) {
                n.kind = NodeKind::File;
                n.size = fi.size();
                n.localPath = fi.absoluteFilePath();
            } else {
                return Error(ErrorCode::InvalidArgument,
                             QStringLiteral("%1 is not a regular file or folder (broken link or special file), so it cannot be put into an image.")
                                 .arg(QDir::toNativeSeparators(fi.absoluteFilePath())));
            }
            const int idx = int(tree.nodes.size());
            tree.nodes[size_t(parent)].children.push_back(idx);
            const bool isDir = n.isDir();
            tree.nodes.push_back(std::move(n));
            if (isDir)
                subdirs.emplace_back(idx, fi.absoluteFilePath());
        }
        // Children of a directory are listed before descending, so the node
        // order inside `children` is exactly the listing order.
        for (const auto& [idx, path] : subdirs)
            FW_TRY(scanDir(idx, path));
        ancestors.pop_back();
        return success();
    }
};

class LocalStream final : public ContentStream {
public:
    explicit LocalStream(const QString& path) : m_file(toLongPath(path)), m_path(path) {}
    Status open()
    {
        if (!m_file.open(QIODevice::ReadOnly))
            return Error(ErrorCode::IoError, QStringLiteral("Could not read %1.").arg(QDir::toNativeSeparators(m_path)),
                         m_file.errorString());
        return success();
    }
    Result<qint64> read(char* dst, qint64 max) override
    {
        qint64 total = 0;
        while (total < max) {
            const qint64 n = m_file.read(dst + total, max - total);
            if (n < 0)
                return Error(ErrorCode::IoError, QStringLiteral("Could not read %1.").arg(QDir::toNativeSeparators(m_path)),
                             m_file.errorString());
            if (n == 0)
                break;
            total += n;
        }
        return total;
    }

private:
    QFile m_file;
    QString m_path;
};

} // namespace

Status appendLocalFolder(SourceTree& tree, int parent, const QString& dir, const CancellationToken& token)
{
    Scanner s{tree, token};
    // The loop guard starts at `dir`: a link back into `dir` or below is
    // caught, one into a folder that is already in the tree is not.
    FW_TRY(s.scanDir(parent, dir));
    tree.recount();
    return success();
}

Result<SourceTree> scanLocalFolder(const QString& root, const CancellationToken& token)
{
    const QFileInfo ri(root);
    if (!ri.exists() || !ri.isDir())
        return Error(ErrorCode::NotFound, QStringLiteral("The folder %1 does not exist.").arg(QDir::toNativeSeparators(root)));
    SourceTree tree;
    tree.rootPath = ri.absoluteFilePath();
    TreeNode rootNode;
    rootNode.kind = NodeKind::Dir;
    rootNode.mtime = ri.lastModified().toSecsSinceEpoch();
    tree.nodes.push_back(std::move(rootNode));
    Scanner s{tree, token};
    FW_TRY(s.scanDir(0, tree.rootPath));
    tree.recount();
    return tree;
}

Result<std::unique_ptr<ContentStream>> openContent(const SourceTree& tree, int node)
{
    const TreeNode& n = tree.nodes[size_t(node)];
    if (!n.localPath.isEmpty()) {
        auto s = std::make_unique<LocalStream>(n.localPath);
        FW_TRY(s->open());
        return std::unique_ptr<ContentStream>(std::move(s));
    }
    if (n.imageIno != 0 && tree.image)
        return tree.image->openFile(n.imageIno);
    return Error(ErrorCode::Internal, QStringLiteral("No content source for %1.").arg(tree.relativePath(node)));
}

} // namespace fw::ufs2
