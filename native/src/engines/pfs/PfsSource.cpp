// Source scan for PFS folder builds.
//
// Ordering and the ASCII-only rule follow Lazy_MkPFS lazy_mkpfs/build.py
// scan_source_tree; the AMPR index rule follows lazy_mkpfs/ampr_index.py
// ensure_ampr_index; empty-folder markers follow the legacy PFS tab
// (_seed_empty_dir_markers). GPL-3.0-or-later.

#include "engines/pfs/PfsSource.h"

#include "engines/pfs/AmprIndex.h"
#include "engines/pfs/FolderAccess.h"
#include "engines/pfs/PfsFormat.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>

#include <algorithm>
#include <set>

namespace fw::pfs {

QByteArray SourceFile::name() const
{
    const qsizetype slash = relPath.lastIndexOf('/');
    return slash < 0 ? relPath : relPath.mid(slash + 1);
}

bool skipExecutableCompression(const QByteArray& name, const QByteArray& relPath)
{
    const QByteArray n = asciiLower(name);
    const QByteArray p = asciiLower(relPath);
    return (n.startsWith("eboot") && n.endsWith(".bin")) || (n.startsWith("param") && n.endsWith(".sfx"))
        || n.endsWith(".prx") || n.endsWith(".sprx") || n.endsWith(".json") || n.endsWith(".txt")
        || n.endsWith(".png") || n.endsWith("keystone") || p.contains("sce_module") || p.contains("sce_sys");
}

QStringList normalizeExcludes(const QStringList& patterns)
{
    QStringList out;
    for (QString p : patterns) {
        p = p.trimmed();
        p.replace(u'\\', u'/');
        while (p.startsWith(QStringLiteral("./")))
            p.remove(0, 2);
        while (p.startsWith(u'/'))
            p.remove(0, 1);
        while (p.endsWith(u'/'))
            p.chop(1);
        if (!p.isEmpty() && !out.contains(p, Qt::CaseInsensitive))
            out << p;
    }
    return out;
}

namespace {

struct Exclude {
    bool wildcard = false;
    bool anchored = false; // contains '/': matched against the relative path
    QString literal;
    QRegularExpression re;
};

std::vector<Exclude> compileExcludes(const QStringList& patterns)
{
    std::vector<Exclude> out;
    for (const QString& p : normalizeExcludes(patterns)) {
        Exclude e;
        e.anchored = p.contains(u'/');
        e.wildcard = p.contains(u'*') || p.contains(u'?') || p.contains(u'[');
        e.literal = p;
        if (e.wildcard)
            e.re = QRegularExpression::fromWildcard(p, Qt::CaseInsensitive);
        out.push_back(std::move(e));
    }
    return out;
}

bool isExcluded(const std::vector<Exclude>& ex, const QString& rel, const QString& name)
{
    for (const Exclude& e : ex) {
        const QString& subject = e.anchored ? rel : name;
        if (e.wildcard) {
            if (e.re.match(subject).hasMatch())
                return true;
        } else if (subject.compare(e.literal, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}

struct RawFile {
    QString rel;
    QString abs;
    qint64 size = 0;
    qint64 mtime = 0;
};

struct Walker {
    const CancellationToken& token;
    std::vector<Exclude> excludes;
    std::vector<RawFile> files;
    QStringList emptyDirs; // relative paths of folders with nothing included
    QStringList nonAscii;
    std::set<QString> visited; // canonical paths (symlink loops)
    int excludedFiles = 0;
    int excludedFolders = 0;
    QStringList excludedSamples;
    qint64 seen = 0;
    std::set<QString> ancestors; // identities of the folders being walked (junction loops)

    void noteExcluded(const QString& rel)
    {
        if (excludedSamples.size() < 10)
            excludedSamples << rel;
    }

    Status walk(const QString& absDir, const QString& relDir)
    {
        const QFileInfo dirInfo(absDir);
        if (!dirInfo.isReadable())
            return Error(ErrorCode::PermissionDenied,
                         QStringLiteral("The folder %1 cannot be read. Check its permissions.")
                             .arg(QDir::toNativeSeparators(absDir)));
        const QString canonical = dirInfo.canonicalFilePath();
        if (!canonical.isEmpty() && !visited.insert(canonical).second)
            return success(); // symlink loop or second link to the same folder
        // A junction's canonical path is its own, so a junction that leads
        // back into the folders above it is recognised by the folder's identity.
        const QString identity = folderIdentity(absDir);
        if (!identity.isEmpty() && !ancestors.insert(identity).second)
            return success();
        struct Leave {
            std::set<QString>& set;
            const QString& id;
            ~Leave() { set.erase(id); }
        } leave{ancestors, identity};
        const QFileInfoList entries = QDir(absDir).entryInfoList(
            QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDir::Name);
        if (entries.isEmpty())
            FW_TRY(ensureFolderListable(absDir)); // empty, or not allowed to look?
        int included = 0;
        for (const QFileInfo& fi : entries) {
            if ((++seen & 0xFF) == 0 && !token.checkpoint())
                return Error::cancelled();
            const QString name = fi.fileName();
            const QString rel = relDir.isEmpty() ? name : relDir + u'/' + name;
            if (fi.isDir()) {
                if (isExcluded(excludes, rel, name)) {
                    ++excludedFolders;
                    noteExcluded(rel + u'/');
                    continue;
                }
                ++included;
                FW_TRY(walk(fi.absoluteFilePath(), rel));
            } else if (fi.isFile()) {
                if (isExcluded(excludes, rel, name)) {
                    ++excludedFiles;
                    noteExcluded(rel);
                    continue;
                }
                ++included;
                if (!isAscii(rel)) {
                    nonAscii << rel;
                    continue;
                }
                files.push_back({rel, fi.absoluteFilePath(), fi.size(), fi.lastModified().toSecsSinceEpoch()});
            }
            // Anything else (broken links, devices) is skipped, as os.scandir's
            // is_file()/is_dir() do in Lazy_MkPFS.
        }
        if (included == 0)
            emptyDirs << relDir;
        return success();
    }
};

bool samePath(const QByteArray& a, const char* b)
{
#ifdef Q_OS_WIN
    return asciiLower(a) == asciiLower(QByteArray(b)); // NTFS lookups are case-insensitive
#else
    return a == b;
#endif
}

} // namespace

Result<SourcePlan> planFolder(const QString& root, const PlanOptions& options, const CancellationToken& token)
{
    const QFileInfo rootInfo(root);
    if (!rootInfo.exists() || !rootInfo.isDir())
        return Error(ErrorCode::NotFound,
                     QStringLiteral("The folder %1 does not exist.").arg(QDir::toNativeSeparators(root)));

    Walker w{token, compileExcludes(options.excludes), {}, {}, {}, {}, 0, 0, {}, 0};
    FW_TRY(w.walk(rootInfo.absoluteFilePath(), QString()));

    if (!w.nonAscii.isEmpty()) {
        QStringList shown = w.nonAscii.mid(0, 8);
        for (QString& s : shown)
            s = QStringLiteral("  ") + QDir::toNativeSeparators(s);
        return Error(ErrorCode::InvalidArgument,
                     QStringLiteral("PFS images only support plain ASCII file names. Rename or exclude these %1 "
                                    "item(s):\n%2")
                         .arg(w.nonAscii.size())
                         .arg(shown.join(u'\n')),
                     w.nonAscii.join(QStringLiteral("; ")));
    }
    if (w.files.empty())
        return Error(ErrorCode::InvalidArgument,
                     QStringLiteral("%1 contains no files to pack.").arg(QDir::toNativeSeparators(root)));

    SourcePlan plan;
    plan.root = rootInfo.absoluteFilePath();
    plan.excludedFiles = w.excludedFiles;
    plan.excludedFolders = w.excludedFolders;
    plan.excludedSamples = w.excludedSamples;
    plan.files.reserve(w.files.size() + size_t(w.emptyDirs.size()) + 1);
    for (const RawFile& r : w.files) {
        SourceFile f;
        f.relPath = r.rel.toLatin1();
        f.absPath = r.abs;
        f.size = r.size;
        f.mtime = r.mtime;
        plan.files.push_back(std::move(f));
    }

    if (options.keepEmptyFolders) {
        for (const QString& dir : std::as_const(w.emptyDirs)) {
            if (dir.isEmpty())
                continue; // the root has files (checked above)
            const QString rel = dir + QStringLiteral("/.pfskeep");
            if (!isAscii(rel))
                return Error(ErrorCode::InvalidArgument,
                             QStringLiteral("PFS images only support plain ASCII file names. Rename or exclude the "
                                            "empty folder %1.")
                                 .arg(QDir::toNativeSeparators(dir)));
            SourceFile f;
            f.relPath = rel.toLatin1();
            f.generated = true;
            f.mtime = options.time;
            plan.files.push_back(std::move(f));
            ++plan.emptyFolderMarkers;
        }
    }

    std::sort(plan.files.begin(), plan.files.end(),
              [](const SourceFile& a, const SourceFile& b) { return lessCaseInsensitive(a.relPath, b.relPath); });

    if (options.ampr != AmprMode::Off) {
        const bool hasSprx = std::any_of(plan.files.begin(), plan.files.end(), [](const SourceFile& f) {
            return samePath(f.relPath, "fakelib/libSceAmpr.sprx");
        });
        auto existing = std::find_if(plan.files.begin(), plan.files.end(),
                                     [](const SourceFile& f) { return samePath(f.relPath, "ampr_emu.index"); });
        const bool generate = hasSprx && (options.ampr == AmprMode::Refresh || existing == plan.files.end());
        if (generate) {
            if (existing != plan.files.end())
                plan.files.erase(existing);
            std::vector<AmprRow> rows;
            rows.reserve(plan.files.size());
            for (const SourceFile& f : plan.files) {
                const QByteArray indexed = QByteArrayLiteral("/app0/") + f.relPath;
                const QByteArray low = asciiLower(indexed);
                if (low == "/app0/ampr_emu.index" || low == "/app0/ampr_emu.index.tmp")
                    continue;
                rows.push_back({f.size, f.mtime, indexed});
            }
            SourceFile idx;
            idx.relPath = QByteArrayLiteral("ampr_emu.index");
            idx.generated = true;
            idx.content = buildAmprIndex(rows);
            idx.size = idx.content.size();
            idx.mtime = options.time;
            if (!idx.content.isEmpty()) {
                plan.files.push_back(std::move(idx));
                plan.amprGenerated = true;
                std::sort(plan.files.begin(), plan.files.end(), [](const SourceFile& a, const SourceFile& b) {
                    return lessCaseInsensitive(a.relPath, b.relPath);
                });
            }
        }
    }

    for (const SourceFile& f : plan.files)
        plan.totalBytes += f.size;
    return plan;
}

} // namespace fw::pfs
