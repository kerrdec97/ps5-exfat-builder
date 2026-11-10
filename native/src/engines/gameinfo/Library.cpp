#include "engines/gameinfo/Library.h"

#include "engines/gameinfo/Cover.h"
#include "engines/gameinfo/Naming.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>

#include <algorithm>

namespace fw::gameinfo {

namespace {

int separatorCount(const QString& path)
{
    return int(QDir::cleanPath(path).count(u'/'));
}

// Legacy _better(a, b): bigger size, then shallower wrapper, then shallower eboot root.
bool better(const LibraryGame& a, const LibraryGame& b)
{
    const qint64 sa = std::max<qint64>(0, a.sizeBytes), sb = std::max<qint64>(0, b.sizeBytes);
    if (sa != sb)
        return sa > sb;
    const QString wa = a.wrapper.isEmpty() ? a.folder : a.wrapper;
    const QString wb = b.wrapper.isEmpty() ? b.folder : b.wrapper;
    const int da = separatorCount(wa), db = separatorCount(wb);
    if (da != db)
        return da < db;
    return separatorCount(a.folder) - da < separatorCount(b.folder) - db;
}

QString sortKey(const LibraryGame& g)
{
    return (g.title.isEmpty() ? g.titleId : g.title).toLower();
}

QJsonValue versionJson(const SystemVersion& v)
{
    return v.isValid() ? QJsonValue(v.toString()) : QJsonValue();
}

SystemVersion versionFromJson(const QJsonValue& v)
{
    const QStringList parts = v.toString().split(u'.');
    SystemVersion out;
    if (parts.size() == 2) {
        out.majorVersion = parts[0].toInt();
        out.minorVersion = parts[1].toInt();
        if (!out.isValid())
            out = {};
    }
    return out;
}

} // namespace

bool isSkippedLibraryEntry(const QString& name)
{
    static const QStringList skipped = {
        QStringLiteral("$sysreset"),    QStringLiteral("found.000"), QStringLiteral("lost+found"),
        QStringLiteral("$windows.~bt"), QStringLiteral("$windows.~ws"), QStringLiteral("recovery"),
        QStringLiteral("recycler"),     QStringLiteral("$recycle.bin"), QStringLiteral("system volume information"),
    };
    return name.startsWith(u'.') || skipped.contains(name.toLower());
}

QJsonObject LibraryGame::toJson() const
{
    QJsonObject o;
    o.insert(QStringLiteral("folder"), QDir::toNativeSeparators(folder));
    o.insert(QStringLiteral("wrapper"), QDir::toNativeSeparators(wrapper));
    o.insert(QStringLiteral("libraryFolder"), QDir::toNativeSeparators(libraryFolder));
    o.insert(QStringLiteral("title"), title);
    o.insert(QStringLiteral("titleId"), titleId);
    o.insert(QStringLiteral("version"), version);
    o.insert(QStringLiteral("contentId"), contentId);
    o.insert(QStringLiteral("platform"), platformName(platform));
    o.insert(QStringLiteral("kind"), contentKindName(kind));
    o.insert(QStringLiteral("sdkVersion"), versionJson(sdkVersion));
    o.insert(QStringLiteral("requiredSystemVersion"), versionJson(requiredSystemVersion));
    o.insert(QStringLiteral("sizeBytes"), sizeBytes);
    o.insert(QStringLiteral("coverSource"), QDir::toNativeSeparators(coverSource));
    o.insert(QStringLiteral("coverPath"), QDir::toNativeSeparators(coverPath));
    o.insert(QStringLiteral("modified"), modified.toString(Qt::ISODate));
    if (!notes.isEmpty())
        o.insert(QStringLiteral("notes"), QJsonArray::fromStringList(notes));
    return o;
}

LibraryGame LibraryGame::fromJson(const QJsonObject& o)
{
    LibraryGame g;
    g.folder = QDir::fromNativeSeparators(o.value(QStringLiteral("folder")).toString());
    g.wrapper = QDir::fromNativeSeparators(o.value(QStringLiteral("wrapper")).toString());
    g.libraryFolder = QDir::fromNativeSeparators(o.value(QStringLiteral("libraryFolder")).toString());
    g.title = o.value(QStringLiteral("title")).toString();
    g.titleId = o.value(QStringLiteral("titleId")).toString();
    g.version = o.value(QStringLiteral("version")).toString();
    g.contentId = o.value(QStringLiteral("contentId")).toString();
    const QString pf = o.value(QStringLiteral("platform")).toString();
    g.platform = pf == u"PS5" ? Platform::PS5 : pf == u"PS4" ? Platform::PS4 : Platform::Unknown;
    const QString k = o.value(QStringLiteral("kind")).toString();
    g.kind = k == u"Game" ? ContentKind::Application
           : k == u"Patch" ? ContentKind::Patch
           : k == u"Add-on" ? ContentKind::AddOn
                            : ContentKind::Unknown;
    g.sdkVersion = versionFromJson(o.value(QStringLiteral("sdkVersion")));
    g.requiredSystemVersion = versionFromJson(o.value(QStringLiteral("requiredSystemVersion")));
    g.sizeBytes = qint64(o.value(QStringLiteral("sizeBytes")).toDouble(-1));
    g.coverSource = QDir::fromNativeSeparators(o.value(QStringLiteral("coverSource")).toString());
    g.coverPath = QDir::fromNativeSeparators(o.value(QStringLiteral("coverPath")).toString());
    g.modified = QDateTime::fromString(o.value(QStringLiteral("modified")).toString(), Qt::ISODate);
    for (const auto& n : o.value(QStringLiteral("notes")).toArray())
        g.notes << n.toString();
    return g;
}

std::vector<LibraryGame> dedupeLibrary(std::vector<LibraryGame> games)
{
    QHash<QString, std::size_t> bestIndex;
    std::vector<LibraryGame> withId, noId;
    for (auto& g : games) {
        const QString id = normalizedTitleId(g.titleId);
        if (id.isEmpty()) {
            noId.push_back(std::move(g));
            continue;
        }
        auto it = bestIndex.find(id);
        if (it == bestIndex.end()) {
            bestIndex.insert(id, withId.size());
            withId.push_back(std::move(g));
        } else if (better(g, withId[*it])) {
            withId[*it] = std::move(g);
        }
    }
    std::vector<LibraryGame> out = std::move(withId);
    for (auto& g : noId)
        out.push_back(std::move(g));
    std::stable_sort(out.begin(), out.end(),
                     [](const LibraryGame& a, const LibraryGame& b) { return sortKey(a) < sortKey(b); });
    return out;
}

Result<LibraryScanResult> scanLibrary(const QStringList& libraryFolders, const LibraryScanOptions& options,
                                      const CancellationToken& token, ProgressReporter* progress)
{
    LibraryScanResult result;

    // Count candidates first so progress is meaningful.
    struct Candidate {
        QString library;
        QFileInfo wrapper;
    };
    std::vector<Candidate> candidates;
    QStringList seenRoots;
    for (const QString& rawRoot : libraryFolders) {
        const QString root = QDir::cleanPath(rawRoot);
        if (root.isEmpty() || seenRoots.contains(root))
            continue;
        seenRoots << root;
        if (!QFileInfo(root).isDir()) {
            result.missingFolders << QDir::toNativeSeparators(root);
            continue;
        }
        const QFileInfoList entries = QDir(root).entryInfoList(
            QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDir::Name | QDir::IgnoreCase);
        for (const QFileInfo& e : entries)
            if (!isSkippedLibraryEntry(e.fileName()))
                candidates.push_back({root, e});
    }
    for (const QString& m : result.missingFolders)
        result.warnings << QStringLiteral("Library folder not found: %1").arg(m);

    // Pass 1: discovery.
    if (progress)
        progress->beginPhase(0, qint64(candidates.size()), qint64(candidates.size()), false);
    std::vector<LibraryGame> games;
    for (const Candidate& c : candidates) {
        if (!token.checkpoint())
            return Error::cancelled();
        ++result.foldersChecked;
        if (progress) {
            progress->setCurrentItem(c.wrapper.fileName());
            progress->advance(1);
            progress->advanceFiles(1);
        }
        const QString root = findLibraryGameRoot(c.wrapper.filePath());
        if (root.isEmpty())
            continue; // no eboot.bin within 3 levels: a partial dump, not a game
        LibraryGame g;
        g.folder = root;
        g.wrapper = QDir::cleanPath(c.wrapper.filePath());
        g.libraryFolder = c.library;
        g.modified = c.wrapper.lastModified();
        auto meta = readGameMetadata(root, token);
        if (!meta.ok() && meta.error().isCancelled())
            return Error::cancelled();
        if (meta.ok()) {
            const GameMetadata& m = meta.value();
            g.title = m.title;
            g.titleId = m.titleId;
            g.version = m.version;
            g.contentId = m.contentId;
            g.platform = m.platform;
            g.kind = m.kind;
            g.sdkVersion = m.sdkVersion;
            g.requiredSystemVersion = m.requiredSystemVersion;
            g.notes = m.notes;
        } else {
            g.notes << meta.error().message;
        }
        if (g.titleId.isEmpty())
            for (const QString& part : {QFileInfo(root).fileName(), c.wrapper.fileName()}) {
                g.titleId = findTitleIdIn(part);
                if (!g.titleId.isEmpty())
                    break;
            }
        if (g.title.isEmpty())
            g.title = g.titleId.isEmpty() ? c.wrapper.fileName() : g.titleId;
        games.push_back(std::move(g));
    }

    // Pass 2: enrichment (size, cover, SDK fallback from the wrapper).
    if (progress)
        progress->beginPhase(1, qint64(games.size()), qint64(games.size()), false);
    for (LibraryGame& g : games) {
        if (!token.checkpoint())
            return Error::cancelled();
        if (progress)
            progress->setCurrentItem(g.title);
        if (options.measureSizes) {
            const qint64 size = dumpSize(g.wrapper, token);
            if (size < 0)
                return Error::cancelled();
            g.sizeBytes = size;
        }
        if (!g.sdkVersion.isValid() && g.wrapper != g.folder) {
            auto wm = readGameMetadata(g.wrapper, token);
            if (!wm.ok() && wm.error().isCancelled())
                return Error::cancelled();
            if (wm.ok() && wm.value().sdkVersion.isValid())
                g.sdkVersion = wm.value().sdkVersion;
        }
        g.coverSource = findCoverSource(g.folder);
        if (options.covers && !g.coverSource.isEmpty()) {
            auto cover = normalizedCover(g.coverSource, options.coverCacheDir);
            if (cover.ok())
                g.coverPath = cover.value();
            else
                g.notes << QStringLiteral("Cover not usable: %1").arg(cover.error().message);
        }
        if (progress) {
            progress->advance(1);
            progress->advanceFiles(1);
        }
    }

    result.games = dedupeLibrary(std::move(games));
    return result;
}

} // namespace fw::gameinfo
