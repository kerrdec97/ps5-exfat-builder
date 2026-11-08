// PS5 Manager: legacy exfat_builder.py _ps5mgr_refresh (game list, SITE AVAIL,
// merge by name) plus a folder compare the legacy app did not have.

#include "connect/ps5/ConsoleGames.h"

#include "connect/ps5/RemotePath.h"
#include "connect/ps5/SmpApi.h"
#include "core/FileUtil.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <cmath>

namespace fw::ps5 {

namespace {

bool lessName(const QString& a, const QString& b)
{
    const int c = a.compare(b, Qt::CaseInsensitive);
    return c != 0 ? c < 0 : a < b;
}

QString firstFew(const QStringList& items, int n = 5)
{
    QString s = items.mid(0, n).join(QStringLiteral(", "));
    if (items.size() > n)
        s += QStringLiteral(" and %1 more").arg(items.size() - n);
    return s;
}

} // namespace

// ---- game list ---------------------------------------------------------------------------

const QStringList& consoleImageSuffixes()
{
    static const QStringList s = {QStringLiteral(".exfat"), QStringLiteral(".ffpkg"), QStringLiteral(".ffpfs"), QStringLiteral(".ffpfsc")};
    return s;
}

bool isConsoleImageName(const QString& name)
{
    const QString low = name.toLower();
    return std::any_of(consoleImageSuffixes().begin(), consoleImageSuffixes().end(), [&](const QString& s) { return low.endsWith(s); });
}

std::optional<qint64> parseSiteAvail(const QString& reply)
{
    static const QRegularExpression re(QStringLiteral("(\\d+(?:\\.\\d+)?)\\s*([KMGT]i?B|B)?\\b"), QRegularExpression::CaseInsensitiveOption);
    const auto m = re.match(reply);
    if (!m.hasMatch())
        return std::nullopt;
    bool ok = false;
    const double n = m.captured(1).toDouble(&ok);
    if (!ok)
        return std::nullopt;
    const QString unit = m.captured(2).toUpper();
    double mult = 1;
    if (unit.startsWith(u'K'))
        mult = 1024.0;
    else if (unit.startsWith(u'M'))
        mult = 1024.0 * 1024;
    else if (unit.startsWith(u'G'))
        mult = 1024.0 * 1024 * 1024;
    else if (unit.startsWith(u'T'))
        mult = 1024.0 * 1024 * 1024 * 1024;
    const double bytes = n * mult;
    if (!std::isfinite(bytes) || bytes > 9.0e18)
        return std::nullopt;
    return qint64(std::llround(bytes));
}

QStringList splitConsoleFolders(const QString& text)
{
    QStringList out;
    static const QRegularExpression sep(QStringLiteral("[;\\n]"));
    for (const QString& part : text.split(sep, Qt::SkipEmptyParts)) {
        if (part.trimmed().isEmpty())
            continue;
        const QString p = normalizeRemotePath(part);
        if (!out.contains(p))
            out << p;
    }
    return out;
}

Result<ConsoleGameList> listConsoleGames(FtpClient& c, const QStringList& folders, const CancellationToken& token, quint16 smpApiPort)
{
    ConsoleGameList out;
    for (const QString& folderIn : folders) {
        if (!token.checkpoint())
            return Error::cancelled();
        const QString folder = cleanRemotePath(folderIn);
        auto r = c.list(folder, FtpListMode::Auto, token);
        if (!r.ok()) {
            if (r.error().isCancelled() || !c.isConnected())
                return r.takeError();
            out.folderErrors << QStringLiteral("%1: %2").arg(folder, r.error().message);
            continue;
        }
        for (const FtpEntry& e : r.value()) {
            if (e.isLink)
                continue;
            const auto path = remoteChild(folder, e.name);
            if (!path)
                continue;
            if (!e.isDir && !isConsoleImageName(e.name))
                continue;
            ConsoleGame g;
            g.folder = folder;
            g.name = e.name;
            g.path = *path;
            g.isDir = e.isDir;
            g.size = e.isDir ? -1 : e.size;
            g.modified = e.modified;
            if (!g.isDir && g.size > 0)
                out.imageBytes += g.size;
            out.games.push_back(g);
        }
    }
    if (smpApiPort > 0 && !folders.isEmpty()) {
        smp::ApiOptions api;
        api.connectTimeoutMs = 1500;
        api.replyTimeoutMs = 4000;
        if (auto items = smp::storage(c.host(), smpApiPort, api, token); items.ok()) {
            if (const smp::StorageItem* fs = smp::storageFor(items.value(), cleanRemotePath(folders.front())); fs && fs->availableBytes >= 0) {
                out.freeBytes = fs->availableBytes;
                out.freeText = QStringLiteral("%1 free on %2 (ShadowMount+)").arg(formatBytes(fs->availableBytes), fs->mountPoint);
            }
        } else if (items.error().isCancelled()) {
            return Error::cancelled();
        }
    }
    if (out.freeBytes) {
        // known from the ShadowMount+ API
    } else if (auto avail = c.siteAvail(); avail.ok()) {
        out.freeText = avail.value();
        out.freeBytes = parseSiteAvail(avail.value());
    } else if (!c.isConnected()) {
        return avail.takeError();
    }
    std::stable_sort(out.games.begin(), out.games.end(), [](const ConsoleGame& a, const ConsoleGame& b) { return lessName(a.name, b.name); });
    return out;
}

std::vector<LocalImage> scanLocalImages(const QString& folder)
{
    std::vector<LocalImage> out;
    if (folder.isEmpty() || !QFileInfo(folder).isDir())
        return out;
    for (const QFileInfo& fi : QDir(folder).entryInfoList(QDir::Files | QDir::NoDotAndDotDot)) {
        if (!isConsoleImageName(fi.fileName()))
            continue;
        out.push_back({fi.fileName(), fi.absoluteFilePath(), fi.size()});
    }
    std::sort(out.begin(), out.end(), [](const LocalImage& a, const LocalImage& b) { return lessName(a.name, b.name); });
    return out;
}

QString gameStatusLabel(GameStatus s)
{
    switch (s) {
    case GameStatus::OnPs5: return QStringLiteral("On PS5");
    case GameStatus::SizeMismatch: return QStringLiteral("Size mismatch");
    case GameStatus::LocalOnly: return QStringLiteral("Local only");
    case GameStatus::Ps5Only: return QStringLiteral("PS5 only");
    }
    return {};
}

std::vector<GameRow> mergeGames(const std::vector<ConsoleGame>& remote, const std::vector<LocalImage>& local)
{
    QHash<QString, const LocalImage*> byName;
    for (const LocalImage& l : local)
        if (!byName.contains(l.name))
            byName.insert(l.name, &l);
    QSet<QString> matched;
    std::vector<GameRow> rows;
    for (const ConsoleGame& g : remote) {
        GameRow r;
        r.name = g.name;
        r.isDir = g.isDir;
        r.remotePath = g.path;
        r.remoteSize = g.size;
        r.status = GameStatus::Ps5Only;
        if (!g.isDir) {
            if (const LocalImage* l = byName.value(g.name, nullptr)) {
                matched.insert(g.name);
                r.localPath = l->path;
                r.localSize = l->size;
                r.status = g.size >= 0 && g.size != l->size ? GameStatus::SizeMismatch : GameStatus::OnPs5;
            }
        }
        rows.push_back(r);
    }
    for (const LocalImage& l : local) {
        if (matched.contains(l.name))
            continue;
        GameRow r;
        r.name = l.name;
        r.status = GameStatus::LocalOnly;
        r.localPath = l.path;
        r.localSize = l.size;
        rows.push_back(r);
    }
    std::stable_sort(rows.begin(), rows.end(), [](const GameRow& a, const GameRow& b) {
        if (a.name != b.name)
            return lessName(a.name, b.name);
        return a.remotePath < b.remotePath;
    });
    return rows;
}

// ---- folder compare ----------------------------------------------------------------------

QString diffKindLabel(DiffKind k)
{
    switch (k) {
    case DiffKind::MissingOnConsole: return QStringLiteral("Missing on the PS5");
    case DiffKind::OnlyOnConsole: return QStringLiteral("Only on the PS5");
    case DiffKind::SizeDiffers: return QStringLiteral("Different size");
    }
    return {};
}

int CompareReport::count(DiffKind kind) const
{
    return int(std::count_if(diffs.begin(), diffs.end(), [kind](const FileDiff& d) { return d.kind == kind; }));
}

QString CompareReport::summary() const
{
    if (diffs.empty() && sizeUnknown == 0)
        return QStringLiteral("All %1 file(s) are on the PS5 with the same size (%2).").arg(formatCount(localFiles), formatBytes(localBytes));
    QStringList parts;
    if (const int n = count(DiffKind::MissingOnConsole))
        parts << QStringLiteral("%1 missing on the PS5").arg(formatCount(n));
    if (const int n = count(DiffKind::OnlyOnConsole))
        parts << QStringLiteral("%1 only on the PS5").arg(formatCount(n));
    if (const int n = count(DiffKind::SizeDiffers))
        parts << QStringLiteral("%1 with a different size").arg(formatCount(n));
    if (sizeUnknown)
        parts << QStringLiteral("%1 without a size from the console").arg(formatCount(sizeUnknown));
    parts << QStringLiteral("%1 matching").arg(formatCount(matching));
    return parts.join(QStringLiteral(" · "));
}

CompareReport compareFiles(const std::vector<CompareFile>& local, const std::vector<CompareFile>& remote)
{
    CompareReport rep;
    QHash<QString, qint64> remoteSizes;
    for (const CompareFile& f : remote) {
        remoteSizes.insert(f.path, f.size);
        ++rep.remoteFiles;
        if (f.size > 0)
            rep.remoteBytes += f.size;
    }
    QSet<QString> seen;
    for (const CompareFile& f : local) {
        if (seen.contains(f.path))
            continue;
        seen.insert(f.path);
        ++rep.localFiles;
        if (f.size > 0)
            rep.localBytes += f.size;
        const auto it = remoteSizes.constFind(f.path);
        if (it == remoteSizes.constEnd()) {
            rep.diffs.push_back({f.path, DiffKind::MissingOnConsole, f.size, -1});
        } else if (it.value() < 0 || f.size < 0) {
            ++rep.sizeUnknown;
        } else if (it.value() != f.size) {
            rep.diffs.push_back({f.path, DiffKind::SizeDiffers, f.size, it.value()});
        } else {
            ++rep.matching;
        }
    }
    for (const CompareFile& f : remote)
        if (!seen.contains(f.path)) {
            seen.insert(f.path);
            rep.diffs.push_back({f.path, DiffKind::OnlyOnConsole, -1, f.size});
        }
    std::stable_sort(rep.diffs.begin(), rep.diffs.end(), [](const FileDiff& a, const FileDiff& b) { return lessName(a.path, b.path); });
    return rep;
}

Result<std::vector<CompareFile>> localFolderFiles(const QString& folder, const CancellationToken& token)
{
    FW_TRY_ASSIGN(FileTree tree, scanTree(folder, token));
    std::vector<CompareFile> out;
    out.reserve(size_t(tree.fileCount));
    for (const FileEntry& e : tree.entries)
        if (!e.isDir)
            out.push_back({e.relativePath, e.size});
    return out;
}

Result<CompareReport> compareConsoleFolder(FtpClient& c, const QString& remoteFolder, const std::vector<CompareFile>& local,
                                           const CancellationToken& token, const CompareProgress& progress)
{
    RemoteWalkOptions opt;
    opt.fillMissingSizes = true;
    FW_TRY_ASSIGN(RemoteTree tree, walkRemoteTree(c, remoteFolder, opt, token, progress));
    std::vector<CompareFile> remote;
    remote.reserve(tree.files.size());
    for (const RemoteFile& f : tree.files)
        remote.push_back({f.relPath, f.size});
    CompareReport rep = compareFiles(local, remote);
    rep.remoteFolder = tree.root;
    if (!tree.unreadableDirs.isEmpty())
        rep.warnings << QStringLiteral("Could not list %1 folder(s) on the PS5, so their files count as missing: %2")
                            .arg(tree.unreadableDirs.size()).arg(firstFew(tree.unreadableDirs));
    if (!tree.skippedNames.isEmpty())
        rep.warnings << QStringLiteral("Skipped %1 name(s) from the console that cannot be used safely: %2")
                            .arg(tree.skippedNames.size()).arg(firstFew(tree.skippedNames));
    if (tree.links > 0)
        rep.warnings << QStringLiteral("%1 symbolic link(s) on the PS5 were not followed.").arg(tree.links);
    return rep;
}

} // namespace fw::ps5
