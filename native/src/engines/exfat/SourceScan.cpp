#include "engines/exfat/SourceScan.h"

#include "core/FileUtil.h"
#include "core/FolderAccess.h"
#include "engines/exfat/ExfatFormat.h"

#include <QDir>
#include <QFileInfo>
#include <QTimeZone>

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

#ifdef Q_OS_WIN
namespace {

// The exFAT bits of Windows attributes (they have the same values).
quint16 exfatAttributes(DWORD windowsAttributes, bool isDir)
{
    const quint16 attrs =
        quint16(windowsAttributes & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE));
    return quint16(attrs | (isDir ? kAttrDirectory : 0));
}

// A listed time as QFileInfo gives it: millisecond precision, local time,
// invalid when the file system stores none.
QDateTime listedTime(const FILETIME& ft)
{
    if (ft.dwHighDateTime == 0 && ft.dwLowDateTime == 0)
        return {};
    SYSTEMTIME st;
    if (!FileTimeToSystemTime(&ft, &st))
        return {};
    return QDateTime(QDate(st.wYear, st.wMonth, st.wDay), QTime(st.wHour, st.wMinute, st.wSecond, st.wMilliseconds),
                     QTimeZone::UTC)
        .toLocalTime();
}

struct ListedEntry {
    QString name;
    bool isDir = false;
    DWORD attributes = 0;
    qint64 size = 0;
    FILETIME created{}, accessed{}, modified{};
};

// Lists `dirAbs` in one pass (FindFirstFileExW, no short names, large
// fetches): a file's attributes, size and times come with the listing, so no
// file is opened or queried on its own. They are what asking the file gives
// while nobody is writing it (QFileInfo takes the size from the listing too).
// A folder's copy of its times in the listing can lag behind (it is brought
// up to date once the folder has been opened), so each folder is still
// asked, once. Returns
// false, and lists nothing, when the folder holds an entry QFileInfo
// describes from elsewhere — a reparse point (symbolic link, junction, cloud
// placeholder …) or a .lnk shortcut — or when the listing or a query fails:
// the caller then lists the folder with Qt, so links, shortcuts and errors
// are handled as they always were.
bool listPlainFolder(const QString& dirAbs, std::vector<ListedEntry>& out)
{
    out.clear();
    QString pattern = toLongPath(dirAbs);
    if (!pattern.endsWith(u'\\'))
        pattern += u'\\';
    pattern += u'*';
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(reinterpret_cast<const wchar_t*>(pattern.utf16()), FindExInfoBasic, &fd,
                                FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    bool plain = true;
    for (;;) {
        const wchar_t* n = fd.cFileName;
        const bool dots = n[0] == L'.' && (n[1] == 0 || (n[1] == L'.' && n[2] == 0));
        if (!dots) {
            ListedEntry e;
            e.name = QString::fromWCharArray(n);
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
                || e.name.endsWith(QLatin1String(".lnk"), Qt::CaseInsensitive)) {
                plain = false;
                break;
            }
            e.isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            e.attributes = fd.dwFileAttributes;
            e.size = (qint64(fd.nFileSizeHigh) << 32) | qint64(fd.nFileSizeLow);
            e.created = fd.ftCreationTime;
            e.accessed = fd.ftLastAccessTime;
            e.modified = fd.ftLastWriteTime;
            out.push_back(std::move(e));
        }
        if (!FindNextFileW(h, &fd)) {
            plain = GetLastError() == ERROR_NO_MORE_FILES;
            break;
        }
    }
    FindClose(h);
    for (size_t i = 0; plain && i < out.size(); ++i) {
        ListedEntry& e = out[i];
        if (!e.isDir)
            continue;
        const QString lp = toLongPath(dirAbs + u'/' + e.name);
        WIN32_FILE_ATTRIBUTE_DATA d;
        if (!GetFileAttributesExW(reinterpret_cast<const wchar_t*>(lp.utf16()), GetFileExInfoStandard, &d)
            || (d.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            plain = false;
            break;
        }
        e.attributes = d.dwFileAttributes;
        e.created = d.ftCreationTime;
        e.accessed = d.ftLastAccessTime;
        e.modified = d.ftLastWriteTime;
    }
    if (!plain)
        out.clear();
    return plain;
}

} // namespace
#endif

quint16 sourceAttributes(const QString& absolutePath, bool isDir, bool writable)
{
#ifdef Q_OS_WIN
    Q_UNUSED(writable);
    const QString lp = toLongPath(absolutePath);
    const DWORD a = GetFileAttributesW(reinterpret_cast<const wchar_t*>(lp.utf16()));
    if (a == INVALID_FILE_ATTRIBUTES)
        return isDir ? kAttrDirectory : kAttrArchive;
    return exfatAttributes(a, isDir);
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
    // Qt's canonical path does not resolve an NTFS junction: the identity of
    // the folder behind a link tells when it is one of the folders above it.
    const auto leadsToAncestor = [&tree](const QString& link, int dirNode) {
        const QString target = folderIdentity(link);
        if (target.isEmpty())
            return false;
        for (int n = dirNode; n >= 0; n = tree.nodes[size_t(n)].parent)
            if (folderIdentity(tree.nodes[size_t(n)].absolutePath) == target)
                return true;
        return false;
    };
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
        std::vector<int> kids;
        // Ordinary files and folders (nearly every folder of a game) are taken
        // straight from the listing; the rules are those of the loop below.
        const auto excludedDir = [&](const QString& name, const QString& rel) {
            if (!matcher.excludesDir(name, rel))
                return false;
            ++tree.excludedCount;
            if (tree.excludedSamples.size() < 20)
                tree.excludedSamples << rel + u'/';
            return true;
        };
        const auto excludedFile = [&](const QString& name, const QString& rel, bool hidden) {
            if (!matcher.excludesFile(name, rel, hidden))
                return false;
            ++tree.excludedCount;
            if (tree.excludedSamples.size() < 20)
                tree.excludedSamples << rel;
            return true;
        };
        const auto add = [&](SourceNode&& n) {
            n.parent = cur.node;
            if (n.isDir)
                ++tree.dirCount;
            else {
                ++tree.fileCount;
                tree.totalBytes += n.size;
            }
            kids.push_back(int(tree.nodes.size()));
            tree.nodes.push_back(std::move(n));
        };
        bool listed = false;
#ifdef Q_OS_WIN
        std::vector<ListedEntry> plain;
        listed = listPlainFolder(dirAbs, plain) && !plain.empty();
        if (listed) {
            kids.reserve(plain.size());
            for (const ListedEntry& e : plain) {
                if ((++seen & 0xFF) == 0 && !token.checkpoint())
                    return Error::cancelled();
                const QString& name = e.name;
                const QString rel = dirRel.isEmpty() ? name : dirRel + u'/' + name;
                const bool isDir = e.isDir;
                if (isDir ? excludedDir(name, rel) : excludedFile(name, rel, (e.attributes & FILE_ATTRIBUTE_HIDDEN) != 0))
                    continue;
                SourceNode n;
                n.name = name;
                n.relativePath = rel;
                n.absolutePath = dirAbs + u'/' + name;
                n.isDir = isDir;
                n.size = isDir ? 0 : e.size;
                n.attributes = exfatAttributes(e.attributes, isDir);
                n.modified = listedTime(e.modified);
                const QDateTime born = listedTime(e.created);
                const QDateTime read = listedTime(e.accessed);
                n.created = born.isValid() ? born : n.modified;
                n.accessed = read.isValid() ? read : n.modified;
                add(std::move(n));
            }
        }
#endif
        // Folders with links or shortcuts, empty folders and folders the fast
        // listing could not read.
        const QFileInfoList list = listed ? QFileInfoList() : QDir(toLongPath(dirAbs)).entryInfoList(filters, QDir::NoSort);
        if (!listed && list.isEmpty())
            FW_TRY(ensureFolderListable(dirAbs)); // empty, or not allowed to look?

        kids.reserve(kids.size() + size_t(list.size()));
        for (const QFileInfo& fi : list) {
            if ((++seen & 0xFF) == 0 && !token.checkpoint())
                return Error::cancelled();
            const QString name = fi.fileName();
            const QString abs = dirAbs + u'/' + name;
            const QString rel = dirRel.isEmpty() ? name : dirRel + u'/' + name;
            // A Windows shortcut (.lnk) is an ordinary file to copy, whatever
            // it points at. QFileInfo describes its target, so the file's own
            // size and times are read from an open handle below.
            const bool shortcut = fi.isShortcut();
            const bool isLink = !shortcut && (fi.isSymLink() || fi.isJunction());
            // QFileInfo calls a junction whose folder is gone existing: its target tells.
            if (isLink && (!fi.exists() || (fi.isJunction() && !QFileInfo::exists(fi.junctionTarget()))))
                return Error(ErrorCode::NotFound,
                             QStringLiteral("%1 is a link whose target is missing. Remove it or exclude it, then try again.")
                                 .arg(QDir::toNativeSeparators(abs)));
            const bool isDir = !shortcut && fi.isDir();
            if (isDir) {
                if (excludedDir(name, rel))
                    continue;
                if (isLink) {
                    const QString target = fi.canonicalFilePath();
                    const QString parentCanon = QFileInfo(dirAbs).canonicalFilePath();
                    if (target.isEmpty() || isSameOrInside(parentCanon, target) || leadsToAncestor(abs, cur.node)) {
                        tree.warnings << QStringLiteral("Skipped the folder link %1: it points back into the folder that contains it.")
                                             .arg(QDir::toNativeSeparators(rel));
                        ++tree.excludedCount;
                        continue;
                    }
                }
            } else if (excludedFile(name, rel, fi.isHidden())) {
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
            if (shortcut) {
                QFile self(toLongPath(abs));
                if (!self.open(QIODevice::ReadOnly))
                    return Error(ErrorCode::IoError, QStringLiteral("Could not read %1.").arg(QDir::toNativeSeparators(abs)),
                                 self.errorString());
                n.size = self.size();
                n.modified = self.fileTime(QFileDevice::FileModificationTime);
                const QDateTime born = self.fileTime(QFileDevice::FileBirthTime);
                const QDateTime read = self.fileTime(QFileDevice::FileAccessTime);
                n.created = born.isValid() ? born : n.modified;
                n.accessed = read.isValid() ? read : n.modified;
            }
            add(std::move(n));
        }

        // Stable, case-insensitive order (what Windows lists for NTFS folders).
        std::vector<std::pair<QString, int>> keyed;
        keyed.reserve(kids.size());
        for (int k : kids)
            keyed.emplace_back(up.upcase(tree.nodes[size_t(k)].name), k);
        std::sort(keyed.begin(), keyed.end(), [&](const auto& a, const auto& b) {
            const int c = a.first.compare(b.first, Qt::CaseSensitive);
            return c != 0 ? c < 0
                          : tree.nodes[size_t(a.second)].name.compare(tree.nodes[size_t(b.second)].name, Qt::CaseSensitive) < 0;
        });
        for (size_t k = 0; k < keyed.size(); ++k)
            kids[k] = keyed[k].second;
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
