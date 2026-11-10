#include "engines/ufs2/Ufs2Edit.h"

#include "core/PathSafety.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonObject>

namespace fw::ufs2 {

namespace {

struct KindName {
    EditOp::Kind kind;
    const char* name;
};
const KindName kKinds[] = {{EditOp::Kind::Add, "add"},
                           {EditOp::Kind::Replace, "replace"},
                           {EditOp::Kind::Delete, "delete"},
                           {EditOp::Kind::Rename, "rename"},
                           {EditOp::Kind::Mkdir, "mkdir"}};

Error badOp(int i, const QString& why)
{
    return Error(ErrorCode::InvalidArgument, QStringLiteral("Edit %1 is not valid: %2").arg(i + 1).arg(why));
}

Result<QStringList> splitPath(const QString& path)
{
    const QStringList parts = QString(path).replace(u'\\', u'/').split(u'/', Qt::SkipEmptyParts);
    for (const QString& p : parts)
        if (p == QLatin1String(".") || p == QLatin1String(".."))
            return Error(ErrorCode::InvalidArgument, QStringLiteral("The path %1 inside the image is not valid.").arg(path));
    return parts;
}

QString display(const QStringList& parts) { return u'/' + parts.join(u'/'); }

// Walks to the folder for `parts`, creating missing folders when `create`.
Result<int> ensureDir(SourceTree& tree, const QStringList& parts, bool create, QStringList* log)
{
    int cur = 0;
    for (int i = 0; i < parts.size(); ++i) {
        FW_TRY_ASSIGN(QByteArray raw, encodeName(parts[i]));
        int next = tree.findChild(cur, raw);
        if (next >= 0) {
            if (!tree.nodes[size_t(next)].isDir())
                return Error(ErrorCode::InvalidArgument,
                             QStringLiteral("%1 is a file in the image, not a folder.").arg(display(parts.mid(0, i + 1))));
            cur = next;
            continue;
        }
        if (!create)
            return Error(ErrorCode::NotFound, QStringLiteral("%1 is not in the image.").arg(display(parts.mid(0, i + 1))));
        TreeNode n;
        n.kind = NodeKind::Dir;
        n.name = parts[i];
        n.rawName = raw;
        n.parent = cur;
        next = int(tree.nodes.size());
        tree.nodes.push_back(std::move(n));
        tree.nodes[size_t(cur)].children.push_back(next);
        if (log)
            log->append(QStringLiteral("Created folder %1").arg(display(parts.mid(0, i + 1))));
        cur = next;
    }
    return cur;
}

Result<int> replaceInDir(SourceTree& tree, int dir, const QString& local, const CancellationToken& token)
{
    int replaced = 0;
    for (int c : tree.liveChildren(dir)) {
        if (!token.checkpoint())
            return Error::cancelled();
        TreeNode& n = tree.nodes[size_t(c)];
        // The name comes from the image. One with '\', ':' or the like could
        // name a file outside `local` on Windows; no file in `local` can
        // carry such a name anyway.
        if (!isSafeEntryName(n.name))
            continue;
        const QFileInfo fi(QDir(local).filePath(n.name));
        if (!n.isDir() && fi.isFile()) {
            n.localPath = fi.absoluteFilePath();
            n.imageIno = 0;
            n.size = fi.size();
            n.mtime = fi.lastModified().toSecsSinceEpoch();
            ++replaced;
        } else if (n.isDir() && fi.isDir()) {
            FW_TRY_ASSIGN(int k, replaceInDir(tree, c, fi.absoluteFilePath(), token));
            replaced += k;
        }
    }
    return replaced;
}

} // namespace

QString editOpKindName(EditOp::Kind kind)
{
    for (const KindName& k : kKinds)
        if (k.kind == kind)
            return QString::fromLatin1(k.name);
    return {};
}

Result<std::vector<EditOp>> parseEditOps(const QJsonArray& ops)
{
    std::vector<EditOp> out;
    for (int i = 0; i < ops.size(); ++i) {
        if (!ops[i].isObject())
            return badOp(i, QStringLiteral("not an object."));
        const QJsonObject o = ops[i].toObject();
        const QString op = o.value(QStringLiteral("op")).toString();
        EditOp e;
        bool known = false;
        for (const KindName& k : kKinds) {
            if (op == QLatin1String(k.name)) {
                e.kind = k.kind;
                known = true;
            }
        }
        if (!known)
            return badOp(i, QStringLiteral("unknown operation \"%1\".").arg(op));
        e.path = o.value(QStringLiteral("path")).toString();
        e.source = o.value(QStringLiteral("source")).toString();
        e.newName = o.value(QStringLiteral("newName")).toString();
        if (e.path.trimmed().isEmpty())
            return badOp(i, QStringLiteral("no path inside the image."));
        if ((e.kind == EditOp::Kind::Add || e.kind == EditOp::Kind::Replace) && e.source.isEmpty())
            return badOp(i, QStringLiteral("no source file or folder."));
        if (e.kind == EditOp::Kind::Rename && e.newName.isEmpty())
            return badOp(i, QStringLiteral("no new name."));
        out.push_back(std::move(e));
    }
    if (out.empty())
        return Error(ErrorCode::InvalidArgument, QStringLiteral("There is nothing to change."));
    return out;
}

QJsonArray editOpsToJson(const std::vector<EditOp>& ops)
{
    QJsonArray a;
    for (const EditOp& e : ops) {
        QJsonObject o{{QStringLiteral("op"), editOpKindName(e.kind)}, {QStringLiteral("path"), e.path}};
        if (!e.source.isEmpty())
            o.insert(QStringLiteral("source"), e.source);
        if (!e.newName.isEmpty())
            o.insert(QStringLiteral("newName"), e.newName);
        a.append(o);
    }
    return a;
}

Status applyEdits(SourceTree& tree, const std::vector<EditOp>& ops, const CancellationToken& token, QStringList* log)
{
    for (const EditOp& op : ops) {
        if (!token.checkpoint())
            return Error::cancelled();
        FW_TRY_ASSIGN(QStringList parts, splitPath(op.path));
        const QString where = display(parts);
        switch (op.kind) {
        case EditOp::Kind::Add: {
            if (parts.isEmpty())
                return Error(ErrorCode::InvalidArgument, QStringLiteral("Choose a name to add the item under."));
            FW_TRY_ASSIGN(int parent, ensureDir(tree, parts.mid(0, parts.size() - 1), true, log));
            FW_TRY_ASSIGN(QByteArray raw, encodeName(parts.last()));
            if (tree.findChild(parent, raw) >= 0)
                return Error(ErrorCode::AlreadyExists, QStringLiteral("%1 already exists in the image. Use Replace instead.").arg(where));
            const QFileInfo fi(op.source);
            TreeNode n;
            n.name = parts.last();
            n.rawName = raw;
            n.parent = parent;
            n.mtime = fi.lastModified().toSecsSinceEpoch();
            if (fi.isDir()) {
                n.kind = NodeKind::Dir;
            } else if (fi.isFile()) {
                n.kind = NodeKind::File;
                n.size = fi.size();
                n.localPath = fi.absoluteFilePath();
            } else {
                return Error(ErrorCode::NotFound, QStringLiteral("%1 does not exist.").arg(QDir::toNativeSeparators(op.source)));
            }
            const int idx = int(tree.nodes.size());
            const bool isDir = n.isDir();
            tree.nodes.push_back(std::move(n));
            tree.nodes[size_t(parent)].children.push_back(idx);
            if (isDir)
                FW_TRY(appendLocalFolder(tree, idx, fi.absoluteFilePath(), token));
            if (log)
                log->append(QStringLiteral("Added %1 from %2").arg(where, QDir::toNativeSeparators(op.source)));
            break;
        }
        case EditOp::Kind::Replace: {
            const int idx = tree.find(where);
            if (idx < 0)
                return Error(ErrorCode::NotFound, QStringLiteral("%1 is not in the image.").arg(where));
            const QFileInfo fi(op.source);
            TreeNode& n = tree.nodes[size_t(idx)];
            if (!n.isDir()) {
                if (!fi.isFile())
                    return Error(ErrorCode::InvalidArgument,
                                 QStringLiteral("%1 is a file, so replace it with a file.").arg(where));
                n.localPath = fi.absoluteFilePath();
                n.imageIno = 0;
                n.size = fi.size();
                n.mtime = fi.lastModified().toSecsSinceEpoch();
                if (log)
                    log->append(QStringLiteral("Replaced %1").arg(where));
            } else {
                if (!fi.isDir())
                    return Error(ErrorCode::InvalidArgument,
                                 QStringLiteral("%1 is a folder, so replace it with a folder.").arg(where));
                FW_TRY_ASSIGN(int k, replaceInDir(tree, idx, fi.absoluteFilePath(), token));
                if (log)
                    log->append(QStringLiteral("Replaced %1 files in %2").arg(k).arg(where));
            }
            break;
        }
        case EditOp::Kind::Delete: {
            const int idx = tree.find(where);
            if (idx < 0)
                return Error(ErrorCode::NotFound, QStringLiteral("%1 is not in the image.").arg(where));
            if (idx == 0)
                return Error(ErrorCode::InvalidArgument, QStringLiteral("The root folder cannot be deleted."));
            tree.nodes[size_t(idx)].removed = true;
            if (log)
                log->append(QStringLiteral("Deleted %1").arg(where));
            break;
        }
        case EditOp::Kind::Rename: {
            const int idx = tree.find(where);
            if (idx < 0)
                return Error(ErrorCode::NotFound, QStringLiteral("%1 is not in the image.").arg(where));
            if (idx == 0)
                return Error(ErrorCode::InvalidArgument, QStringLiteral("The root folder cannot be renamed."));
            FW_TRY_ASSIGN(QByteArray raw, encodeName(op.newName));
            TreeNode& n = tree.nodes[size_t(idx)];
            if (raw == n.rawName)
                break;
            if (tree.findChild(n.parent, raw) >= 0)
                return Error(ErrorCode::AlreadyExists, QStringLiteral("%1 already exists next to %2.").arg(op.newName, where));
            n.name = op.newName;
            n.rawName = raw;
            if (log)
                log->append(QStringLiteral("Renamed %1 to %2").arg(where, op.newName));
            break;
        }
        case EditOp::Kind::Mkdir: {
            if (parts.isEmpty())
                return Error(ErrorCode::InvalidArgument, QStringLiteral("Choose a name for the new folder."));
            if (tree.find(where) >= 0)
                return Error(ErrorCode::AlreadyExists, QStringLiteral("%1 already exists in the image.").arg(where));
            FW_TRY(ensureDir(tree, parts, true, log));
            break;
        }
        }
    }
    tree.recount();
    return success();
}

} // namespace fw::ufs2
