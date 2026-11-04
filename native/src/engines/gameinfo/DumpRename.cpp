#include "engines/gameinfo/DumpRename.h"

#include "core/FileUtil.h"
#include "core/Paths.h"
#include "engines/gameinfo/GameInfo.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>

namespace fw::gameinfo {

namespace {

#ifdef Q_OS_WIN
constexpr Qt::CaseSensitivity kPathCase = Qt::CaseInsensitive;
#else
constexpr Qt::CaseSensitivity kPathCase = Qt::CaseSensitive;
#endif

QString pathKey(const QString& p)
{
    const QString c = QDir::cleanPath(p);
    return kPathCase == Qt::CaseInsensitive ? c.toLower() : c;
}

bool isSkipStatus(RenameStatus s)
{
    return s != RenameStatus::Ready;
}

QString cleanedName(QString name)
{
    name = name.trimmed();
    while (!name.isEmpty() && (name.back() == u' ' || name.back() == u'.'))
        name.chop(1);
    return name;
}

struct UndoLog {
    QString path;
    QJsonObject doc;

    Status save() const { return writeFileAtomic(path, QJsonDocument(doc).toJson(QJsonDocument::Indented)); }
};

} // namespace

QString renameStatusName(RenameStatus s)
{
    switch (s) {
    case RenameStatus::Ready: return QStringLiteral("Ready");
    case RenameStatus::AlreadyNamed: return QStringLiteral("Already named");
    case RenameStatus::NoTitleId: return QStringLiteral("No PPSA found");
    case RenameStatus::TargetExists: return QStringLiteral("Skipped — target exists");
    case RenameStatus::Duplicate: return QStringLiteral("Skipped — same name as another dump");
    case RenameStatus::InvalidName: return QStringLiteral("Invalid name");
    case RenameStatus::NotSelected: return QStringLiteral("Not selected");
    case RenameStatus::Renamed: return QStringLiteral("Renamed");
    case RenameStatus::Failed: return QStringLiteral("Failed");
    case RenameStatus::Restored: return QStringLiteral("Restored");
    }
    return QStringLiteral("Unknown");
}

QJsonObject RenameItem::toJson() const
{
    QJsonObject o;
    o.insert(QStringLiteral("source"), QDir::toNativeSeparators(sourcePath));
    o.insert(QStringLiteral("gameRoot"), QDir::toNativeSeparators(gameRoot));
    o.insert(QStringLiteral("currentName"), currentName);
    o.insert(QStringLiteral("proposedName"), proposedName);
    o.insert(QStringLiteral("target"), QDir::toNativeSeparators(targetPath));
    o.insert(QStringLiteral("title"), title);
    o.insert(QStringLiteral("titleId"), titleId);
    o.insert(QStringLiteral("version"), version);
    o.insert(QStringLiteral("confidence"), confidence);
    o.insert(QStringLiteral("selected"), selected);
    o.insert(QStringLiteral("status"), renameStatusName(status));
    o.insert(QStringLiteral("message"), message);
    return o;
}

QString defaultRenameUndoDir()
{
    return QDir(Paths::dataDir()).filePath(QStringLiteral("rename-undo"));
}

Result<std::vector<RenameItem>> planDumpRenames(const QString& sourceFolder, const RenamePlanOptions& options,
                                                const CancellationToken& token)
{
    if (!QFileInfo(sourceFolder).isDir())
        return Error(ErrorCode::NotFound, QStringLiteral("Select a valid source folder."),
                     QDir::toNativeSeparators(sourceFolder));
    std::vector<RenameItem> plan;
    const QFileInfoList entries = QDir(sourceFolder).entryInfoList(
        QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDir::Name | QDir::IgnoreCase);
    for (const QFileInfo& e : entries) {
        if (!token.checkpoint())
            return Error::cancelled();
        const QString root = findRenameGameRoot(e.filePath());
        if (root.isEmpty())
            continue;
        RenameItem it;
        it.sourcePath = QDir::cleanPath(e.filePath());
        it.gameRoot = root;
        it.currentName = e.fileName();
        if (auto meta = readGameMetadata(root); meta.ok()) {
            it.title = meta.value().title;
            it.titleId = meta.value().titleId;
            it.version = meta.value().version;
        }
        if (it.titleId.isEmpty())
            for (const QString& cand : {e.fileName(), QFileInfo(root).fileName()}) {
                it.titleId = findTitleIdIn(cand);
                if (!it.titleId.isEmpty())
                    break;
            }
        it.confidence = it.titleId.isEmpty() ? QStringLiteral("red")
                      : it.title.isEmpty()   ? QStringLiteral("yellow")
                                             : QStringLiteral("green");
        it.proposedName = buildDumpName({it.titleId, it.title, it.version}, options.preset, options.naming);
        plan.push_back(std::move(it));
    }
    refreshRenamePlan(plan, options.destinationRoot);
    return plan;
}

void refreshRenamePlan(std::vector<RenameItem>& plan, const QString& destinationRoot)
{
    QHash<QString, int> claimed; // target key -> count of Ready items using it
    for (RenameItem& it : plan) {
        it.message.clear();
        it.proposedName = cleanedName(it.proposedName);
        const QString parent = destinationRoot.isEmpty() ? QFileInfo(it.sourcePath).absolutePath()
                                                         : QDir::cleanPath(destinationRoot);
        it.targetPath = it.proposedName.isEmpty() ? QString() : QDir(parent).filePath(it.proposedName);
        if (!it.selected) {
            it.status = RenameStatus::NotSelected;
            continue;
        }
        if (it.proposedName.isEmpty()) {
            it.status = RenameStatus::NoTitleId;
            continue;
        }
        if (const QString why = folderNameProblem(it.proposedName); !why.isEmpty()) {
            it.status = RenameStatus::InvalidName;
            it.message = why;
            continue;
        }
        if (pathKey(it.targetPath) == pathKey(it.sourcePath)) {
            it.status = RenameStatus::AlreadyNamed;
            continue;
        }
        if (QFileInfo::exists(it.targetPath) || QFileInfo(it.targetPath).isSymLink()) {
            it.status = RenameStatus::TargetExists;
            continue;
        }
        const QString key = pathKey(it.targetPath);
        if (claimed.value(key) > 0) {
            it.status = RenameStatus::Duplicate;
            continue;
        }
        claimed[key] += 1;
        it.status = RenameStatus::Ready;
    }
}

Result<RenameRunResult> applyDumpRenames(std::vector<RenameItem> plan, const QString& destinationRoot,
                                         const QString& undoLogPathIn, const CancellationToken& token,
                                         ProgressReporter* progress)
{
    refreshRenamePlan(plan, destinationRoot);
    RenameRunResult out;
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz"));
    UndoLog log;
    log.path = undoLogPathIn.isEmpty()
                 ? QDir(defaultRenameUndoDir()).filePath(QStringLiteral("rename-%1.json").arg(stamp))
                 : undoLogPathIn;
    log.doc.insert(QStringLiteral("version"), 1);
    log.doc.insert(QStringLiteral("created"), QDateTime::currentDateTime().toString(Qt::ISODate));
    QJsonArray entries;

    qint64 readyCount = 0;
    for (const auto& it : plan)
        if (it.status == RenameStatus::Ready)
            ++readyCount;
    if (progress)
        progress->beginPhase(0, readyCount, readyCount, false);

    bool cancelled = false;
    for (RenameItem& it : plan) {
        if (isSkipStatus(it.status)) {
            ++out.skipped;
            continue;
        }
        if (cancelled || !token.checkpoint()) {
            cancelled = true;
            it.status = RenameStatus::NotSelected;
            it.message = QStringLiteral("Cancelled before this dump was renamed.");
            ++out.skipped;
            continue;
        }
        if (progress)
            progress->setCurrentItem(it.currentName);
        // Re-check right before touching the disk: never overwrite.
        if (QFileInfo::exists(it.targetPath) || QFileInfo(it.targetPath).isSymLink()) {
            it.status = RenameStatus::TargetExists;
            ++out.skipped;
            continue;
        }
        if (!destinationRoot.isEmpty() && !QDir().mkpath(destinationRoot)) {
            it.status = RenameStatus::Failed;
            it.message = QStringLiteral("The destination folder could not be created.");
            ++out.failed;
            continue;
        }
        if (!QDir().rename(it.sourcePath, it.targetPath)) {
            it.status = RenameStatus::Failed;
            it.message = !destinationRoot.isEmpty()
                           ? QStringLiteral("The folder could not be moved. The destination must be on the same drive, "
                                            "and no file in the dump may be open in another program.")
                           : QStringLiteral("The folder could not be renamed. Close any program using files in it "
                                            "and try again.");
            ++out.failed;
            continue;
        }
        it.status = RenameStatus::Renamed;
        ++out.renamed;
        QJsonObject e;
        e.insert(QStringLiteral("from"), QDir::toNativeSeparators(it.sourcePath));
        e.insert(QStringLiteral("to"), QDir::toNativeSeparators(it.targetPath));
        e.insert(QStringLiteral("time"), QDateTime::currentDateTime().toString(Qt::ISODate));
        entries.append(e);
        log.doc.insert(QStringLiteral("entries"), entries);
        if (auto s = log.save(); !s.ok()) {
            // The rename happened; say clearly that it cannot be undone automatically.
            it.message = QStringLiteral("Renamed, but the undo log could not be written: %1").arg(s.error().message);
        }
        if (progress) {
            progress->advance(1);
            progress->advanceFiles(1);
        }
    }
    if (out.renamed > 0)
        out.undoLogPath = log.path;
    out.items = std::move(plan);
    if (cancelled && out.renamed == 0)
        return Error::cancelled();
    return out;
}

Result<RenameRunResult> undoDumpRenames(const QString& undoLogPath, const CancellationToken& token,
                                        ProgressReporter* progress)
{
    QFile f(undoLogPath);
    if (!f.open(QIODevice::ReadOnly))
        return Error(ErrorCode::NotFound, QStringLiteral("The rename log could not be opened."), f.errorString());
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    f.close();
    if (!doc.isObject() || !doc.object().value(QStringLiteral("entries")).isArray())
        return Error(ErrorCode::CorruptData, QStringLiteral("The rename log is damaged."));
    UndoLog log{undoLogPath, doc.object()};
    QJsonArray entries = log.doc.value(QStringLiteral("entries")).toArray();

    RenameRunResult out;
    if (progress)
        progress->beginPhase(0, entries.size(), entries.size(), false);
    for (qsizetype i = entries.size() - 1; i >= 0; --i) {
        if (!token.checkpoint())
            break;
        QJsonObject e = entries.at(i).toObject();
        RenameItem it;
        it.sourcePath = QDir::fromNativeSeparators(e.value(QStringLiteral("from")).toString());
        it.targetPath = QDir::fromNativeSeparators(e.value(QStringLiteral("to")).toString());
        it.currentName = QFileInfo(it.targetPath).fileName();
        it.proposedName = QFileInfo(it.sourcePath).fileName();
        if (e.value(QStringLiteral("undone")).toBool()) {
            it.status = RenameStatus::Restored;
            it.message = QStringLiteral("Already restored earlier.");
            ++out.skipped;
        } else if (!QFileInfo(it.targetPath).isDir()) {
            it.status = RenameStatus::Failed;
            it.message = QStringLiteral("The renamed folder is no longer there.");
            ++out.failed;
        } else if (QFileInfo::exists(it.sourcePath) && pathKey(it.sourcePath) != pathKey(it.targetPath)) {
            it.status = RenameStatus::TargetExists;
            it.message = QStringLiteral("Something already has the original name; nothing was changed.");
            ++out.skipped;
        } else if (!QDir().rename(it.targetPath, it.sourcePath)) {
            it.status = RenameStatus::Failed;
            it.message = QStringLiteral("The folder could not be renamed back.");
            ++out.failed;
        } else {
            it.status = RenameStatus::Restored;
            ++out.renamed;
            e.insert(QStringLiteral("undone"), true);
            entries.replace(i, e);
            log.doc.insert(QStringLiteral("entries"), entries);
            (void)log.save();
        }
        out.items.push_back(std::move(it));
        if (progress)
            progress->advance(1);
    }
    out.undoLogPath = undoLogPath;
    return out;
}

} // namespace fw::gameinfo
