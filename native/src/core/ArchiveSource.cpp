#include "core/ArchiveSource.h"

#include "core/ArchiveReader.h"
#include "core/FileUtil.h"
#include "core/PathSafety.h"
#include "core/Zip.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QList>
#include <QMap>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>

#include <algorithm>
#include <limits>
#include <memory>

namespace fw::archive {

namespace {

QString nativePath(const QString& p) { return QDir::toNativeSeparators(p); }

// "a/b/eboot.bin" -> "a/b/"; "eboot.bin" -> "".
QString folderOf(const QString& entryName)
{
    const int slash = int(entryName.lastIndexOf(u'/'));
    return slash < 0 ? QString() : entryName.left(slash + 1);
}

// How many of the other games an archive holds are named (ZipGame::otherGames).
constexpr qsizetype kNamedOtherGames = 10;

// Every folder that holds eboot.bin or sce_sys/param.json is a game root.
struct GameRoots {
    QMap<QString, int> roots; // prefix (lower case) -> bit 1 eboot, bit 2 param
    QMap<QString, QString> spelling;

    void note(const QString& fileName)
    {
        const QString lower = fileName.toLower();
        if (lower == QLatin1String("eboot.bin") || lower.endsWith(QLatin1String("/eboot.bin"))) {
            const QString prefix = folderOf(fileName);
            roots[prefix.toLower()] |= 1;
            spelling.insert(prefix.toLower(), prefix);
        } else if (lower == QLatin1String("sce_sys/param.json") || lower.endsWith(QLatin1String("/sce_sys/param.json"))) {
            const QString prefix = fileName.left(fileName.size() - int(QStringLiteral("sce_sys/param.json").size()));
            roots[prefix.toLower()] |= 2;
            spelling.insert(prefix.toLower(), prefix);
        }
    }

    // The shortest root is the game; the others are named in otherGames (the
    // first kNamedOtherGames of them; moreGames counts the rest).
    Status pick(const QString& archivePath, ZipGame& g) const
    {
        if (roots.isEmpty())
            return Error(ErrorCode::NotFound, QStringLiteral("%1 holds no game: there is no eboot.bin or sce_sys/param.json in it.")
                                                  .arg(QFileInfo(archivePath).fileName()));
        // Nested folders of one game (e.g. sce_module/.../eboot.bin) are not
        // separate games: keep only roots that are not inside another root.
        // In sorted order every root that a root lies in comes before it, and
        // nothing between the two can leave it, so only the last root kept
        // has to be asked: one pass, however many game folders a crafted
        // archive lists.
        QStringList top;
        for (auto it = roots.keyBegin(); it != roots.keyEnd(); ++it)
            if (top.isEmpty() || !it->startsWith(top.last()))
                top << *it;
        std::stable_sort(top.begin(), top.end(), [](const QString& a, const QString& b) { return a.size() < b.size(); });
        g.rootPrefix = spelling.value(top.first());
        for (qsizetype i = 1; i < top.size() && g.otherGames.size() < kNamedOtherGames; ++i) {
            QString name = spelling.value(top[i]);
            if (name.endsWith(u'/'))
                name.chop(1);
            g.otherGames << (name.isEmpty() ? QStringLiteral("(top level)") : name);
        }
        g.moreGames = qint64(top.size()) - 1 - g.otherGames.size();
        const QString rootLower = g.rootPrefix.toLower();
        g.hasEboot = roots.value(rootLower) & 1;
        g.hasParam = roots.value(rootLower) & 2;
        return success();
    }
};

// Inside the game every name must reach the image as it is. One this
// computer cannot store would be renamed while unpacking (the game's own
// folder may be: its name is not part of the image).
Status checkGameName(const QString& archivePath, const QString& nameInGame)
{
    for (const QString& part : nameInGame.split(u'/', Qt::SkipEmptyParts))
        if (part != u"." && hostEntryName(part) != part)
            return Error(ErrorCode::Unsupported,
                         QStringLiteral("%1 holds the name \"%2\", which this computer cannot store as a file name. "
                                        "The game would not be built as it is in the archive.")
                             .arg(QFileInfo(archivePath).fileName(), part));
    return success();
}

Error severalGames(const QString& archivePath, const ZipGame& g, const QString& advice)
{
    // Named like the other games: without the trailing slash.
    const QString first = g.rootPrefix.isEmpty() ? QStringLiteral("(top level)") : g.rootPrefix.chopped(1);
    QString others = g.otherGames.join(QStringLiteral(", "));
    if (g.moreGames > 0)
        others += QStringLiteral(", … and %1 more").arg(formatCount(g.moreGames));
    return Error(ErrorCode::InvalidArgument,
                 QStringLiteral("%1 holds more than one game (%2 and %3). %4, or unpack it and choose the game's folder.")
                     .arg(QFileInfo(archivePath).fileName(), first, others, advice));
}

// A ZIP's entries as listArchive() lists a 7z's, for checkExtractable(). The
// names stay as the ZIP has them ("dir/", "./a"): the game's prefix was
// taken from them and extractAll() filters on them. A password is the ZIP
// reader's to name, with its own advice.
ArchiveListing zipListing(const QString& path, const zip::Reader& r)
{
    ArchiveListing out;
    out.path = path;
    out.kind = ArchiveKind::Zip;
    out.volumes << path;
    out.entries.reserve(r.entries().size());
    for (const zip::Entry& e : r.entries()) {
        ArchiveEntry a;
        a.name = e.name;
        a.type = e.isDirectory() ? ArchiveEntry::Type::Directory : ArchiveEntry::Type::File;
        a.size = qint64(std::min<quint64>(e.uncompressedSize, quint64(std::numeric_limits<qint64>::max())));
        out.entries.push_back(std::move(a));
    }
    return out;
}

// A 7z or RAR archive by its first bytes, whatever it is called. Anything
// else named .zip is the ZIP reader's to accept or refuse.
bool readWithLibarchive(const QString& path)
{
    switch (sniffArchive(path)) {
    case ArchiveKind::SevenZip:
    case ArchiveKind::Rar4:
    case ArchiveKind::Rar5:
        return true;
    case ArchiveKind::Zip:
        return false;
    case ArchiveKind::Unknown:
        break;
    }
    return QFileInfo(path).suffix().compare(QLatin1String("zip"), Qt::CaseInsensitive) != 0;
}

// ---------------------------------------------------------------------------
// 7z and RAR: the list of files is kept between calls

// A job asks three times (the Build page's summary, validate(), run()), and
// walking a RAR on a hard disk is a seek per file. A list is used again
// while every part of the archive has the size and time it was read with.
struct KnownArchive {
    QString path;
    QList<QPair<qint64, QDateTime>> stamps; // of each part
    std::shared_ptr<const ArchiveListing> listing;
    bool paramRead = false;
    QByteArray paramJson;
};

struct InspectBudget {
    qint64 archiveBytes = 256LL * 1024 * 1024;
    int milliseconds = 5000;
};

QMutex g_knownMutex;
QList<KnownArchive> g_known; // most recent first
InspectBudget g_budget;
constexpr int kKnownArchives = 4;

QPair<qint64, QDateTime> stampOf(const QString& path)
{
    const QFileInfo fi(path);
    return {fi.exists() ? fi.size() : -1, fi.lastModified()};
}

bool stillCurrent(const KnownArchive& k)
{
    for (int i = 0; i < k.listing->volumes.size(); ++i)
        if (stampOf(k.listing->volumes.at(i)) != k.stamps.at(i))
            return false;
    return true;
}

Result<std::shared_ptr<const ArchiveListing>> listingOf(const QString& path, const CancellationToken& token)
{
    const QString key = QFileInfo(path).absoluteFilePath();
    {
        QMutexLocker lock(&g_knownMutex);
        for (int i = 0; i < g_known.size(); ++i) {
            if (g_known.at(i).path != key)
                continue;
            if (stillCurrent(g_known.at(i)))
                return g_known.at(i).listing;
            g_known.removeAt(i);
            break;
        }
    }
    const QPair<qint64, QDateTime> before = stampOf(key);
    FW_TRY_ASSIGN(ArchiveListing listed, listArchive(key, token));
    KnownArchive k;
    k.path = key;
    k.listing = std::make_shared<const ArchiveListing>(std::move(listed));
    for (const QString& v : k.listing->volumes)
        k.stamps << stampOf(v);
    QMutexLocker lock(&g_knownMutex);
    // Not kept when the file changed while it was read (still being copied).
    if (stampOf(key) == before) {
        g_known.prepend(k);
        while (g_known.size() > kKnownArchives)
            g_known.removeLast();
    }
    return k.listing;
}

// param.json of the game, or nothing when it is not cheap to reach. Asked
// once per archive: the answer is kept with the list.
Result<QByteArray> paramJsonOf(const std::shared_ptr<const ArchiveListing>& listing, const QString& entryName,
                               const CancellationToken& token)
{
    InspectBudget budget;
    {
        QMutexLocker lock(&g_knownMutex);
        budget = g_budget;
        for (const KnownArchive& k : g_known)
            if (k.listing == listing && k.paramRead)
                return k.paramJson;
    }
    bool withinBudget = true;
    auto data = readArchiveFile(*listing, entryName, 16LL * 1024 * 1024,
                                ReadBudget{budget.archiveBytes, budget.milliseconds}, &withinBudget, token);
    if (!data.ok() && data.error().isCancelled())
        return data.takeError();
    // A param.json that cannot be read is no reason to refuse the archive
    // here: unpacking reports what is wrong with it.
    const QByteArray json = data.ok() ? data.value() : QByteArray();
    QMutexLocker lock(&g_knownMutex);
    for (KnownArchive& k : g_known)
        if (k.listing == listing) {
            k.paramRead = true;
            k.paramJson = json;
        }
    return json;
}

struct ListedGame {
    std::shared_ptr<const ArchiveListing> listing;
    ZipGame game;
    QString paramEntry; // the name of sce_sys/param.json in the archive, when it is small enough to read
};

Result<ListedGame> listedGame(const QString& path, const CancellationToken& token)
{
    ListedGame out;
    FW_TRY_ASSIGN(out.listing, listingOf(path, token));
    const ArchiveListing& listing = *out.listing;
    GameRoots roots;
    for (const ArchiveEntry& e : listing.entries)
        if (e.type == ArchiveEntry::Type::File)
            roots.note(e.name);
    ZipGame& g = out.game;
    FW_TRY(roots.pick(path, g));
    // Before anything else is said about the game: a path that leaves the
    // folder, two names Windows cannot tell apart, a password.
    FW_TRY(checkExtractable(listing, g.rootPrefix));
    const QString paramName = g.rootPrefix + QStringLiteral("sce_sys/param.json");
    constexpr quint64 kMaxBytes = quint64(std::numeric_limits<qint64>::max());
    quint64 bytes = 0;
    for (const ArchiveEntry& e : listing.entries) {
        if (!e.name.startsWith(g.rootPrefix, Qt::CaseInsensitive))
            continue;
        FW_TRY(checkGameName(path, e.name.mid(g.rootPrefix.size())));
        if (e.type == ArchiveEntry::Type::Link) {
            ++g.links;
            continue;
        }
        if (e.type != ArchiveEntry::Type::File)
            continue;
        // The sizes come from the archive and are not to be trusted: the sum
        // stops at the largest qint64.
        bytes = quint64(e.size) > kMaxBytes - bytes ? kMaxBytes : bytes + quint64(e.size);
        ++g.files;
        if (e.size < 16LL * 1024 * 1024 && e.name.compare(paramName, Qt::CaseInsensitive) == 0)
            out.paramEntry = e.name;
    }
    g.bytes = qint64(bytes);
    return out;
}

} // namespace

QStringList archiveSourceSuffixes()
{
    return {QStringLiteral("zip"), QStringLiteral("7z"), QStringLiteral("rar")};
}

QString archiveSourceFilter()
{
    return QStringLiteral("Archives (*.zip *.7z *.rar)");
}

QString sourceName(const QString& path)
{
    const QFileInfo fi(QDir::cleanPath(path));
    if (!isArchiveSource(path))
        return fi.fileName();
    QString base = fi.completeBaseName();
    static const QRegularExpression part(QStringLiteral("\\.part\\d+$"), QRegularExpression::CaseInsensitiveOption);
    if (fi.suffix().compare(QLatin1String("rar"), Qt::CaseInsensitive) == 0) {
        const QString chopped = QString(base).remove(part);
        if (!chopped.isEmpty())
            base = chopped;
    }
    return base;
}

bool isArchiveSource(const QString& path)
{
    const QFileInfo fi(path);
    return fi.isFile() && archiveSourceSuffixes().contains(fi.suffix(), Qt::CaseInsensitive);
}

bool isZipSource(const QString& path)
{
    return isArchiveSource(path);
}

Result<ArchiveGame> inspectArchive(const QString& path, const CancellationToken& token)
{
    if (readWithLibarchive(path)) {
        FW_TRY_ASSIGN(ListedGame listed, listedGame(path, token));
        if (!listed.paramEntry.isEmpty()) {
            FW_TRY_ASSIGN(listed.game.paramJson, paramJsonOf(listed.listing, listed.paramEntry, token));
        }
        return listed.game;
    }
    const QString& zipPath = path;
    zip::Reader r;
    FW_TRY(r.open(zipPath));
    GameRoots roots;
    const zip::Entry* paramEntry = nullptr;
    for (const zip::Entry& e : r.entries()) {
        if (e.isDirectory())
            continue;
        roots.note(e.name);
    }
    ZipGame g;
    FW_TRY(roots.pick(zipPath, g));
    // The rules of every archive source, before anything else is said about
    // the game: a path that leaves the folder, two names Windows takes for
    // one (unpacked, the second would be written over the first).
    FW_TRY(checkExtractable(zipListing(zipPath, r), g.rootPrefix));
    // Exactly the entries unpackZipSource() extracts. The sizes come from the
    // archive and are not to be trusted: the sum stops at the largest qint64.
    constexpr quint64 kMaxBytes = quint64(std::numeric_limits<qint64>::max());
    quint64 bytes = 0;
    for (const zip::Entry& e : r.entries()) {
        if (!e.name.startsWith(g.rootPrefix, Qt::CaseInsensitive))
            continue;
        FW_TRY(checkGameName(zipPath, e.name.mid(g.rootPrefix.size())));
        if (e.isDirectory())
            continue;
        if (e.encrypted) // said before the job starts, not after half the archive is unpacked
            return Error(ErrorCode::Unsupported,
                         QStringLiteral("%1 is password-protected. Unpack it and choose the game's folder.")
                             .arg(QFileInfo(zipPath).fileName()));
        bytes = e.uncompressedSize > kMaxBytes - bytes ? kMaxBytes : bytes + e.uncompressedSize;
        ++g.files;
        if (e.name.compare(g.rootPrefix + QStringLiteral("sce_sys/param.json"), Qt::CaseInsensitive) == 0)
            paramEntry = &e;
    }
    g.bytes = qint64(bytes);
    if (paramEntry && paramEntry->uncompressedSize < 16ull * 1024 * 1024) {
        auto data = r.readAll(*paramEntry);
        if (data.ok())
            g.paramJson = data.value();
    }
    return g;
}

Result<ZipGame> inspectZip(const QString& zipPath)
{
    return inspectArchive(zipPath);
}

Status checkArchiveSource(const QString& path, const CancellationToken& token)
{
    if (readWithLibarchive(path)) {
        // Without param.json: this runs where a job is validated, and reading
        // one file out of a solid archive can take seconds.
        FW_TRY_ASSIGN(const ListedGame listed, listedGame(path, token));
        if (!listed.game.otherGames.isEmpty())
            return severalGames(path, listed.game, QStringLiteral("Put one game in each archive"));
        return success();
    }
    auto g = inspectArchive(path, token);
    if (!g.ok())
        return g.error();
    if (!g->otherGames.isEmpty())
        return severalGames(path, *g, QStringLiteral("Zip one game per archive"));
    return success();
}

Status checkZipSource(const QString& zipPath)
{
    return checkArchiveSource(zipPath);
}

Result<QString> unpackArchiveSource(const QString& path, const QString& tempDir, const CancellationToken& token,
                                    ProgressReporter* progress, qint64 freeSpaceMarginBytes)
{
    FW_TRY(checkArchiveSource(path, token));
    if (tempDir.isEmpty())
        return Error(ErrorCode::InvalidArgument, QStringLiteral("No temporary folder to unpack %1 into.").arg(nativePath(path)));
    const QString dest = QDir(tempDir).filePath(QStringLiteral("zip-source"));
    QString rootPrefix;
    if (readWithLibarchive(path)) {
        FW_TRY_ASSIGN(const ListedGame listed, listedGame(path, token));
        const ZipGame& g = listed.game;
        QDir().mkpath(dest);
        FW_TRY(ensureFreeTempSpace(dest, g.bytes, freeSpaceMarginBytes));
        if (progress) {
            // A step, not a phase: the job's own phase list stays as it declared it.
            progress->beginStep(QStringLiteral("Unpacking archive"), g.bytes, g.files);
            progress->setCurrentItem(QStringLiteral("Unpacking %1").arg(QFileInfo(path).fileName()));
        }
        // One pass through the archive, only the game's entries; a file that
        // delivers more than the list said stops it, because the free-space
        // check relied on the list.
        FW_TRY(extractArchive(*listed.listing, dest, g.rootPrefix, token, progress));
        rootPrefix = g.rootPrefix;
    } else {
        const QString& zipPath = path;
        FW_TRY_ASSIGN(ZipGame g, inspectArchive(zipPath, token));
        QDir().mkpath(dest);
        FW_TRY(ensureFreeTempSpace(dest, g.bytes, freeSpaceMarginBytes));
        zip::Reader r;
        FW_TRY(r.open(zipPath));
        if (progress) {
            // A step, not a phase: the job's own phase list stays as it declared it.
            progress->beginStep(QStringLiteral("Unpacking ZIP"), g.bytes, g.files);
            progress->setCurrentItem(QStringLiteral("Unpacking %1").arg(QFileInfo(zipPath).fileName()));
        }
        // Only the game: what else the archive holds (extras, a second folder) is
        // neither counted above nor wanted in the temporary folder. Never over a
        // file: the names are unique (inspectArchive), the folder is new.
        FW_TRY(r.extractAll(dest, token, progress, false, g.rootPrefix));
        rootPrefix = g.rootPrefix;
    }
    const QString root = QDir::cleanPath(QDir(dest).filePath(zip::Reader::extractedPath(rootPrefix)));
    if (!QFileInfo(root).isDir())
        return Error(ErrorCode::CorruptData, QStringLiteral("The game folder was not found after unpacking %1.").arg(nativePath(path)));
    return root;
}

Result<QString> unpackZipSource(const QString& zipPath, const QString& tempDir, const CancellationToken& token,
                                ProgressReporter* progress, qint64 freeSpaceMarginBytes)
{
    return unpackArchiveSource(zipPath, tempDir, token, progress, freeSpaceMarginBytes);
}

void setArchiveInspectBudgetForTesting(qint64 archiveBytes, int milliseconds)
{
    QMutexLocker lock(&g_knownMutex);
    g_budget = InspectBudget{archiveBytes, milliseconds};
    g_known.clear();
}

} // namespace fw::archive
