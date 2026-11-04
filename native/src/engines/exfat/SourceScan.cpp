#include "engines/exfat/SourceScan.h"

#include "core/FileUtil.h"
#include "engines/exfat/ExfatFormat.h"

#include <QDir>
#include <QFileInfo>

#include <algorithm>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace fw::exfat {

ExcludeMatcher::ExcludeMatcher(const ScanRules& rules) : m_hidden(rules.excludeHidden)
{
    for (QString p : rules.excludePatterns) {
        p = p.trimmed();
        p.replace(u'\\', u'/');
        if (p.isEmpty())
            continue;
        bool dir = false;
        if (p.endsWith(u'/')) {
            dir = true;
            while (p.endsWith(u'/'))
                p.chop(1);
            if (p.isEmpty())
                continue;
        }
        while (p.startsWith(u'/'))
            p.remove(0, 1);
        const bool path = p.contains(u'/');
        QRegularExpression re = QRegularExpression::fromWildcard(p, Qt::CaseInsensitive);
        if (!re.isValid())
            continue;
        (dir ? (path ? m_dirPath : m_dirName) : (path ? m_filePath : m_fileName)).push_back(std::move(re));
    }
}

static bool anyMatch(const std::vector<QRegularExpression>& list, const QString& s)
{
    return std::any_of(list.begin(), list.end(), [&](const QRegularExpression& re) { return re.match(s).hasMatch(); });
}

bool ExcludeMatcher::excludesFile(const QString& name, const QString& rel, bool hidden) const
{
    if (m_hidden && hidden)
        return true;
    return anyMatch(m_fileName, name) || anyMatch(m_filePath, rel);
}

bool ExcludeMatcher::excludesDir(const QString& name, const QString& rel) const
{
    return anyMatch(m_dirName, name) || anyMatch(m_dirPath, rel);
}

quint16 sourceAttributes(const QString& absolutePath, bool isDir, bool writable)
{
#ifdef Q_OS_WIN
    Q_UNUSED(writable);
    const QString lp = toLongPath(absolutePath);
    const DWORD a = GetFileAttributesW(reinterpret_cast<const wchar_t*>(lp.utf16()));
    quint16 attrs = 0;
    if (a != INVALID_FILE_ATTRIBUTES)
        attrs = quint16(a & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE));
    else if (!isDir)
        attrs = kAttrArchive;
    return quint16(attrs | (isDir ? kAttrDirectory : 0));
#else
    Q_UNUSED(absolutePath);
    if (isDir)
        return kAttrDirectory;
    // New files on Windows carry the archive bit; there is no hidden or system
    // attribute to copy here.
    return quint16(kAttrArchive | (writable ? 0 : kAttrReadOnly));
#endif
}

Result<SourceTree> scanSource(const QString& rootPath, const ScanRules& rules, const CancellationToken& token,
                              ProgressReporter* progress)
{
    const QFileInfo rootInfo(rootPath);
    if (!rootInfo.exists() || !rootInfo.isDir())
        return Error(ErrorCode::NotFound,
                     QStringLiteral("The folder %1 does not exist.").arg(QDir::toNativeSeparators(rootPath)));

    const ExcludeMatcher matcher(rules);
    const UpcaseTable& up = UpcaseTable::recommended();

    SourceTree tree;
    tree.root = QDir::cleanPath(rootInfo.absoluteFilePath());
    {
        SourceNode r;
        r.name = rootInfo.fileName();
        r.absolutePath = tree.root;
        r.isDir = true;
        r.attributes = kAttrDirectory;
        r.created = rootInfo.birthTime().isValid() ? rootInfo.birthTime() : rootInfo.lastModified();
        r.modified = rootInfo.lastModified();
        r.accessed = rootInfo.lastRead();
        tree.nodes.push_back(std::move(r));
    }

    struct Pending {
        int node;
        int depth;
    };
    std::vector<Pending> stack{{0, 0}};
    qint64 seen = 0;
    const QDir::Filters filters = QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System;

    while (!stack.empty()) {
        const Pending cur = stack.back();
        stack.pop_back();
        if (!token.checkpoint())
            return Error::cancelled();
        const QString dirAbs = tree.nodes[size_t(cur.node)].absolutePath;
        const QString dirRel = tree.nodes[size_t(cur.node)].relativePath;
        if (cur.depth > 200)
            return Error(ErrorCode::InvalidArgument,
                         QStringLiteral("The folder %1 is nested too deeply (a folder link may point back into itself).")
                             .arg(QDir::toNativeSeparators(dirAbs)));
        const QFileInfo dirInfo(dirAbs);
        if (!dirInfo.isReadable())
            return Error(ErrorCode::PermissionDenied,
                         QStringLiteral("Could not read the folder %1. Check that you have access to it, or exclude it.")
                             .arg(QDir::toNativeSeparators(dirAbs)));
        const QFileInfoList list = QDir(toLongPath(dirAbs)).entryInfoList(filters, QDir::NoSort);

        std::vector<int> kids;
        kids.reserve(size_t(list.size()));
        for (const QFileInfo& fi : list) {
            if ((++seen & 0xFF) == 0 && !token.checkpoint())
                return Error::cancelled();
            const QString name = fi.fileName();
            const QString abs = dirAbs + u'/' + name;
            const QString rel = dirRel.isEmpty() ? name : dirRel + u'/' + name;
            const bool isLink = fi.isSymLink() || fi.isJunction();
            if (isLink && !fi.exists())
                return Error(ErrorCode::NotFound,
                             QStringLiteral("%1 is a link whose target is missing. Remove it or exclude it, then try again.")
                                 .arg(QDir::toNativeSeparators(abs)));
            const bool isDir = fi.isDir();
            if (isDir) {
                if (matcher.excludesDir(name, rel)) {
                    ++tree.excludedCount;
                    if (tree.excludedSamples.size() < 20)
                        tree.excludedSamples << rel + u'/';
                    continue;
                }
                if (isLink) {
                    const QString target = fi.canonicalFilePath();
                    const QString parentCanon = QFileInfo(dirAbs).canonicalFilePath();
                    if (target.isEmpty() || isSameOrInside(parentCanon, target)) {
                        tree.warnings << QStringLiteral("Skipped the folder link %1: it points back into the folder that contains it.")
                                             .arg(QDir::toNativeSeparators(rel));
                        ++tree.excludedCount;
                        continue;
                    }
                }
            } else if (matcher.excludesFile(name, rel, fi.isHidden())) {
                ++tree.excludedCount;
                if (tree.excludedSamples.size() < 20)
                    tree.excludedSamples << rel;
                continue;
            }

            SourceNode n;
            n.name = name;
            n.relativePath = rel;
            n.absolutePath = abs;
            n.isDir = isDir;
            n.size = isDir ? 0 : fi.size();
            n.attributes = sourceAttributes(abs, isDir, fi.isWritable());
            n.modified = fi.lastModified();
            n.created = fi.birthTime().isValid() ? fi.birthTime() : n.modified;
            n.accessed = fi.lastRead().isValid() ? fi.lastRead() : n.modified;
            n.parent = cur.node;
            if (isDir)
                ++tree.dirCount;
            else {
                ++tree.fileCount;
                tree.totalBytes += n.size;
            }
            kids.push_back(int(tree.nodes.size()));
            tree.nodes.push_back(std::move(n));
        }

        // Stable, case-insensitive order (what Windows lists for NTFS folders).
        std::sort(kids.begin(), kids.end(), [&](int a, int b) {
            const QString& na = tree.nodes[size_t(a)].name;
            const QString& nb = tree.nodes[size_t(b)].name;
            const int c = up.upcase(na).compare(up.upcase(nb), Qt::CaseSensitive);
            return c != 0 ? c < 0 : na.compare(nb, Qt::CaseSensitive) < 0;
        });
        tree.nodes[size_t(cur.node)].children = kids;
        // Depth-first in name order: push in reverse so the first child is next.
        for (auto it = kids.rbegin(); it != kids.rend(); ++it)
            if (tree.nodes[size_t(*it)].isDir)
                stack.push_back({*it, cur.depth + 1});
        if (progress && !dirRel.isEmpty())
            progress->setCurrentItem(dirRel);
    }
    return tree;
}

} // namespace fw::exfat
