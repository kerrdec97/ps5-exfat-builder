#include "engines/fpkg/SourceScanner.h"

#include "core/FileUtil.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <map>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace fw::fpkg {

namespace {

// For a folder that lists as empty: tells "really empty" from "could not be
// listed" (access denied, device error), which QDir reports the same way.
bool canListFolder(const QString& dir)
{
#ifdef Q_OS_WIN
    const QString pattern = toLongPath(dir) + QStringLiteral("\\*");
    WIN32_FIND_DATAW fd{};
    const HANDLE h = FindFirstFileExW(reinterpret_cast<LPCWSTR>(pattern.utf16()), FindExInfoBasic, &fd,
                                      FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD err = GetLastError();
        return err == ERROR_FILE_NOT_FOUND || err == ERROR_NO_MORE_FILES;
    }
    FindClose(h);
    return true;
#else
    return QFileInfo(dir).isReadable();
#endif
}

} // namespace

const ScannedFile* ScanResult::findFile(const QString& rel) const
{
    const auto it = fileIndexLower.constFind(rel.toLower());
    return it == fileIndexLower.cend() ? nullptr : &files[size_t(*it)];
}

bool isExecutableName(const QString& name)
{
    const QString low = name.toLower();
    return low == u"eboot.bin" || low.endsWith(QStringLiteral(".sprx")) || low.endsWith(QStringLiteral(".prx"))
        || low.endsWith(QStringLiteral(".self")) || low.endsWith(QStringLiteral(".elf"));
}

bool startsWithPs4SelfMagic(const QString& path)
{
    QFile f(toLongPath(path));
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const QByteArray head = f.read(4);
    return head == QByteArray("\x4F\x15\x3D\x1D", 4);
}

QString findPathCaseInsensitive(const QString& root, const QString& rel, bool wantDir)
{
    QString cur = root;
    const QStringList parts = rel.split(u'/', Qt::SkipEmptyParts);
    for (qsizetype i = 0; i < parts.size(); ++i) {
        const bool last = i == parts.size() - 1;
        const QString exact = QDir(cur).filePath(parts[i]);
        const QFileInfo efi(exact);
        if (efi.exists() && (!last || (wantDir ? efi.isDir() : efi.isFile()))) {
            cur = exact;
            continue;
        }
        QString found;
        const QStringList names = QDir(cur).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
        for (const QString& n : names) {
            if (n.compare(parts[i], Qt::CaseInsensitive) != 0)
                continue;
            const QFileInfo fi(QDir(cur).filePath(n));
            if (!last ? fi.isDir() : (wantDir ? fi.isDir() : fi.isFile())) {
                found = fi.filePath();
                break;
            }
        }
        if (found.isEmpty())
            return {};
        cur = found;
    }
    return cur;
}

QString findInRoots(const QStringList& roots, const QString& rel)
{
    for (const QString& r : roots) {
        if (r.isEmpty())
            continue;
        const QString p = findPathCaseInsensitive(r, rel, false);
        if (!p.isEmpty())
            return p;
    }
    return {};
}

namespace {

struct MergedEntry {
    QString name;          // spelling (last root that has it)
    bool isDir = false;
    bool isFile = false;
    bool isLink = false;
    bool isOther = false;
    QString fileAbs;       // first root's file
    qint64 size = 0;
    int fileRoot = -1;
    std::vector<QString> dirAbs; // per root ("" when absent)
    int lastRoot = -1;           // the last root that listed this name
    QString lastAbs;             // its path there
    // Two entries of one root whose names differ only in letter case: the
    // package paths are case-insensitive, so one would silently replace or
    // merge into the other.
    QString caseTwinA, caseTwinB;
};

class Walker {
public:
    Walker(const QStringList& roots, const ScanOptions& o, const CancellationToken& t, ScanResult& r)
        : m_roots(roots), m_opt(o), m_token(t), m_res(r)
    {
    }

    Status walk(const QStringList& parts, const std::vector<QString>& absDirs)
    {
        std::vector<MergedEntry> entries;
        FW_TRY(list(parts, absDirs, entries));
        const QString relDir = parts.join(u'/');
        const bool inSceSys = !parts.isEmpty() && parts[0].compare(QStringLiteral("sce_sys"), Qt::CaseInsensitive) == 0;
        if (!parts.isEmpty()) {
            m_res.dirs << relDir;
            m_res.dirsLower.insert(relDir.toLower());
        }

        bool allJunk = true;
        for (MergedEntry& e : entries) {
            ++m_res.entriesSeen;
            if (m_opt.checkpointEvery > 0 && m_res.entriesSeen % m_opt.checkpointEvery == 0 && !m_token.checkpoint())
                return Error::cancelled();
            const QString rel = relDir.isEmpty() ? e.name : relDir + u'/' + e.name;
            if (e.isLink)
                return Error(ErrorCode::Unsupported,
                             QStringLiteral("Links and junctions are not supported inside a package: %1. Replace it with "
                                            "the real file or folder.").arg(rel));
            if (e.isDir) {
                if (isJunkDirName(e.name)) {
                    m_res.excluded.push_back({rel, QStringLiteral("OS metadata folder"), true});
                    continue;
                }
                allJunk = false;
                QString why = baseExclusionReason(parts, e.name, true);
                if (why.isEmpty())
                    why = leftoverReason(parts, e.name, true, m_opt.leftovers, leftoverContext(parts, e));
                if (!why.isEmpty()) {
                    m_res.excluded.push_back({rel, why, true});
                    continue;
                }
                FW_TRY(checkCaseTwins(e));
                FW_TRY(walk(parts + QStringList{e.name}, e.dirAbs));
                continue;
            }
            if (isJunkFileName(e.name)) {
                m_res.excluded.push_back({rel, QStringLiteral("OS metadata file"), false});
                continue;
            }
            allJunk = false;
            if (e.isOther) {
                m_res.excluded.push_back({rel, QStringLiteral("not a regular file"), false});
                continue;
            }
            QString why = baseExclusionReason(parts, e.name, false);
            if (why.isEmpty())
                why = leftoverReason(parts, e.name, false, m_opt.leftovers, {});
            if (!why.isEmpty()) {
                if (parts.size() == 1 && inSceSys && e.name.endsWith(QStringLiteral(".dds"), Qt::CaseInsensitive))
                    m_res.sceSysDds.emplace_back(rel, e.fileAbs);
                m_res.excluded.push_back({rel, why, false});
                continue;
            }
            FW_TRY(checkCaseTwins(e));
            const QString problem = pathRuleProblem(rel);
            if (!problem.isEmpty())
                m_res.badPaths.push_back({rel, problem});
            if (rel.compare(QStringLiteral("sce_sys/keystone"), Qt::CaseInsensitive) == 0)
                m_res.keystoneSize = e.size;
            if (isExecutableName(e.name) && e.size >= 0x20 && startsWithPs4SelfMagic(e.fileAbs))
                m_res.ps4SelfMagic << rel;
            m_res.fileIndexLower.insert(rel.toLower(), int(m_res.files.size()));
            m_res.files.push_back({rel, e.fileAbs, e.size, e.fileRoot});
            m_res.totalBytes += e.size;
            if (e.fileRoot >= 0 && e.fileRoot < 4)
                ++m_res.filesFromRoot[e.fileRoot];
            // Every ancestor folder now holds a mapped file.
            QString anc = relDir.toLower();
            while (!anc.isEmpty() && !m_res.dirsWithFilesLower.contains(anc)) {
                m_res.dirsWithFilesLower.insert(anc);
                const qsizetype slash = anc.lastIndexOf(u'/');
                anc = slash < 0 ? QString() : anc.left(slash);
            }
        }
        if (!parts.isEmpty() && !inSceSys && allJunk)
            m_res.emptyDirs << relDir;
        return success();
    }

private:
    static Status checkCaseTwins(const MergedEntry& e)
    {
        if (e.caseTwinA.isEmpty())
            return success();
        return Error(ErrorCode::Unsupported,
                     QStringLiteral("%1 and %2 differ only in letter case. Paths inside a package are not "
                                    "case-sensitive, so one would replace the other. Rename or remove one of them.")
                         .arg(QDir::toNativeSeparators(e.caseTwinA), QDir::toNativeSeparators(e.caseTwinB)),
                     QStringLiteral("case-insensitive name collision: %1 | %2").arg(e.caseTwinA, e.caseTwinB));
    }

    Status list(const QStringList& parts, const std::vector<QString>& absDirs, std::vector<MergedEntry>& out)
    {
        std::map<QString, size_t> byKey;
        for (size_t r = 0; r < absDirs.size(); ++r) {
            const QString& dir = absDirs[r];
            if (dir.isEmpty())
                continue;
            QDir d(dir);
            if (!QFileInfo(dir).isReadable())
                return Error(ErrorCode::IoError,
                             QStringLiteral("The folder %1 could not be read.").arg(QDir::toNativeSeparators(dir)));
            const QFileInfoList infos = d.entryInfoList(
                QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDir::NoSort);
            // Never package an unreadable folder as an empty one.
            if (infos.isEmpty() && !canListFolder(dir))
                return Error(ErrorCode::IoError,
                             QStringLiteral("The folder %1 could not be read.").arg(QDir::toNativeSeparators(dir)));
            for (const QFileInfo& fi : infos) {
                const QString name = fi.fileName();
                if (r > 0 && parts.isEmpty() && fi.isDir()
                    && std::any_of(m_opt.ignoredBaseRootDirs.begin(), m_opt.ignoredBaseRootDirs.end(),
                                   [&](const QString& n) { return n.compare(name, Qt::CaseInsensitive) == 0; }))
                    continue;
                const QString key = name.toCaseFolded();
                auto it = byKey.find(key);
                if (it == byKey.end()) {
                    MergedEntry e;
                    e.name = name;
                    e.dirAbs.assign(absDirs.size(), QString());
                    e.isLink = fi.isSymbolicLink() || fi.isJunction();
                    if (!e.isLink) {
                        e.isDir = fi.isDir();
                        e.isFile = fi.isFile();
                        e.isOther = !e.isDir && !e.isFile;
                    }
                    if (e.isDir)
                        e.dirAbs[r] = fi.filePath();
                    else if (e.isFile) {
                        e.fileAbs = fi.filePath();
                        e.size = fi.size();
                        e.fileRoot = int(r);
                    }
                    e.lastRoot = int(r);
                    e.lastAbs = fi.filePath();
                    byKey.emplace(key, out.size());
                    out.push_back(std::move(e));
                    continue;
                }
                MergedEntry& e = out[it->second];
                if (e.lastRoot == int(r) && e.caseTwinA.isEmpty()) {
                    // Same folder, same root: "Data" next to "data".
                    e.caseTwinA = e.lastAbs;
                    e.caseTwinB = fi.filePath();
                }
                e.lastRoot = int(r);
                e.lastAbs = fi.filePath();
                e.name = name; // the later root's spelling wins
                const bool link = fi.isSymbolicLink() || fi.isJunction();
                if (link) {
                    e.isLink = true;
                    continue;
                }
                if (e.isDir && fi.isDir())
                    e.dirAbs[r] = fi.filePath();
                // A file in a higher-priority root keeps its data.
            }
        }
        std::stable_sort(out.begin(), out.end(), [](const MergedEntry& a, const MergedEntry& b) {
            const QString fa = a.name.toCaseFolded(), fb = b.name.toCaseFolded();
            const int c = fa.compare(fb);
            if (c != 0)
                return c < 0;
            return a.name.compare(b.name) < 0;
        });
        return success();
    }

    LeftoverContext leftoverContext(const QStringList& parts, const MergedEntry& e) const
    {
        LeftoverContext ctx;
        if (!m_opt.leftovers.removeDumpLeftovers)
            return ctx;
        if (parts.isEmpty() && e.name.compare(QStringLiteral("Engine"), Qt::CaseInsensitive) == 0) {
            QSet<QString> seen;
            for (const QString& d : e.dirAbs) {
                if (d.isEmpty())
                    continue;
                for (const QString& n : QDir(d).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System)) {
                    const QFileInfo fi(QDir(d).filePath(n));
                    if ((fi.isDir() && isJunkDirName(n)) || (!fi.isDir() && isJunkFileName(n)))
                        continue;
                    if (!seen.contains(n.toLower())) {
                        seen.insert(n.toLower());
                        ctx.childNames << n;
                    }
                }
            }
        }
        if (parts.size() == 1 && e.name.compare(QStringLiteral("Saved"), Qt::CaseInsensitive) == 0) {
            for (const QString& root : m_roots) {
                if (root.isEmpty())
                    continue;
                if (!findPathCaseInsensitive(root, parts[0] + QStringLiteral("/Content/Paks"), true).isEmpty()) {
                    ctx.rootFolderHasContentPaks = true;
                    break;
                }
            }
        }
        return ctx;
    }

    const QStringList& m_roots;
    const ScanOptions& m_opt;
    const CancellationToken& m_token;
    ScanResult& m_res;
};

} // namespace

Result<ScanResult> scanSource(const QStringList& roots, const ScanOptions& options, const CancellationToken& token)
{
    if (roots.isEmpty())
        return Error(ErrorCode::InvalidArgument, QStringLiteral("No source folder was given."));
    std::vector<QString> absRoots;
    for (const QString& r : roots) {
        if (r.isEmpty()) {
            absRoots.emplace_back();
            continue;
        }
        const QFileInfo fi(r);
        if (!fi.isDir())
            return Error(ErrorCode::NotFound,
                         QStringLiteral("The folder %1 does not exist.").arg(QDir::toNativeSeparators(r)));
        absRoots.push_back(fi.absoluteFilePath());
    }
    ScanResult res;
    QStringList absList;
    for (const auto& r : absRoots)
        absList << r;
    Walker w(absList, options, token, res);
    FW_TRY(w.walk({}, absRoots));
    return res;
}

} // namespace fw::fpkg
