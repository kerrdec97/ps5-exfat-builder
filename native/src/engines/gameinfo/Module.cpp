#include "engines/gameinfo/Module.h"

#include "engines/gameinfo/Cover.h"
#include "engines/gameinfo/DumpRename.h"
#include "engines/gameinfo/Library.h"
#include "engines/gameinfo/Naming.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>

#include <memory>

namespace fw::gameinfo {

namespace {

QStringList stringArray(const QJsonValue& v)
{
    QStringList out;
    for (const auto& x : v.toArray())
        if (!x.toString().trimmed().isEmpty())
            out << QDir::fromNativeSeparators(x.toString().trimmed());
    return out;
}

QString coverCacheFolder(const SettingsValues& s)
{
    const QString dir = s.string(keys::kCoverCacheFolder).trimmed();
    return dir.isEmpty() ? defaultCoverCacheDir() : dir;
}

// ---- library.scan ----------------------------------------------------------------

class LibraryScanJob final : public Job {
public:
    explicit LibraryScanJob(QJsonObject p) : m_p(std::move(p)) {}

    QString kind() const override { return jobkinds::kLibraryScan; }
    QString title() const override { return QStringLiteral("Scan library"); }
    QString subtitle() const override
    {
        const QStringList f = stringArray(m_p.value(QStringLiteral("folders")));
        return f.isEmpty() ? QStringLiteral("Library folders from Settings")
                           : QStringLiteral("%1 folder(s)").arg(f.size());
    }
    QStringList phases() const override { return {QStringLiteral("Finding games"), QStringLiteral("Reading details")}; }
    QJsonObject parameters() const override { return m_p; }

    Status validate(const SettingsValues& s) const override
    {
        if (folders(s).isEmpty())
            return Error(ErrorCode::InvalidArgument, QStringLiteral("Add at least one scan folder first."));
        return success();
    }

    Result<JobOutcome> run(JobContext& ctx) override
    {
        const QStringList dirs = folders(ctx.settings);
        LibraryScanOptions opt;
        opt.measureSizes = m_p.value(QStringLiteral("measureSizes")).toBool(true);
        opt.covers = m_p.value(QStringLiteral("covers")).toBool(true);
        opt.coverCacheDir = coverCacheFolder(ctx.settings);
        ctx.log.info(QStringLiteral("Scanning %1 library folder(s)").arg(dirs.size()));
        auto r = scanLibrary(dirs, opt, ctx.token, ctx.progress);
        if (!r.ok())
            return r.takeError();
        const LibraryScanResult& res = r.value();
        JobOutcome o;
        QJsonArray games;
        for (const auto& g : res.games) {
            games.append(g.toJson());
            if (!g.coverPath.isEmpty())
                o.outputs << g.coverPath;
        }
        o.details.insert(QStringLiteral("games"), games);
        o.details.insert(QStringLiteral("foldersChecked"), res.foldersChecked);
        o.details.insert(QStringLiteral("libraryFolders"), QJsonArray::fromStringList(dirs));
        o.warnings = res.warnings;
        o.verification = Verification::Skipped;
        o.summary = QStringLiteral("%1 game(s) found across %2 folder(s)").arg(res.games.size()).arg(dirs.size());
        ctx.log.info(o.summary);
        return o;
    }

private:
    QStringList folders(const SettingsValues& s) const
    {
        QStringList f = stringArray(m_p.value(QStringLiteral("folders")));
        if (f.isEmpty())
            for (const QString& d : s.stringList(keys::kLibraryFolders))
                if (!d.trimmed().isEmpty())
                    f << QDir::fromNativeSeparators(d.trimmed());
        return f;
    }

    QJsonObject m_p;
};

// ---- library.rename --------------------------------------------------------------

class DumpRenameJob final : public Job {
public:
    explicit DumpRenameJob(QJsonObject p) : m_p(std::move(p)) {}

    QString kind() const override { return jobkinds::kLibraryRename; }
    QString title() const override
    {
        const QString m = mode();
        return m == u"undo" ? QStringLiteral("Undo dump rename")
             : m == u"preview" ? QStringLiteral("Preview dump names")
                               : QStringLiteral("Rename dumps");
    }
    QString subtitle() const override
    {
        return QDir::toNativeSeparators(mode() == u"undo" ? m_p.value(QStringLiteral("undoLog")).toString()
                                                          : m_p.value(QStringLiteral("source")).toString());
    }
    QJsonObject parameters() const override { return m_p; }

    Status validate(const SettingsValues& s) const override
    {
        const QString m = mode();
        if (m != u"preview" && m != u"apply" && m != u"undo")
            return Error(ErrorCode::InvalidArgument, QStringLiteral("Unknown rename mode \"%1\".").arg(m));
        if (m == u"undo") {
            if (!QFileInfo(m_p.value(QStringLiteral("undoLog")).toString()).isFile())
                return Error(ErrorCode::NotFound, QStringLiteral("The rename log to undo was not found."));
            return success();
        }
        if (!QFileInfo(m_p.value(QStringLiteral("source")).toString()).isDir())
            return Error(ErrorCode::InvalidArgument, QStringLiteral("Select a valid source folder."));
        if (!preset(s))
            return Error(ErrorCode::InvalidArgument, QStringLiteral("Unknown naming pattern."));
        return success();
    }

    Result<JobOutcome> run(JobContext& ctx) override
    {
        if (mode() == u"undo")
            return runUndo(ctx);
        RenamePlanOptions opt;
        opt.preset = *preset(ctx.settings);
        opt.destinationRoot = QDir::fromNativeSeparators(m_p.value(QStringLiteral("destination")).toString().trimmed());
        const QString source = QDir::fromNativeSeparators(m_p.value(QStringLiteral("source")).toString());
        ctx.progress->beginPhase(0, 0, 0, false);
        auto plan = planDumpRenames(source, opt, ctx.token);
        if (!plan.ok())
            return plan.takeError();
        applyEdits(plan.value());
        refreshRenamePlan(plan.value(), opt.destinationRoot);

        JobOutcome o;
        if (mode() == u"preview") {
            int ready = 0;
            QJsonArray items;
            for (const auto& it : plan.value()) {
                items.append(it.toJson());
                ready += it.status == RenameStatus::Ready ? 1 : 0;
            }
            o.details.insert(QStringLiteral("items"), items);
            o.details.insert(QStringLiteral("preset"), namePresetKey(opt.preset));
            o.verification = Verification::Skipped;
            o.summary = QStringLiteral("%1 dump(s) found, %2 ready to rename").arg(plan.value().size()).arg(ready);
            return o;
        }

        auto run = applyDumpRenames(std::move(plan.value()), opt.destinationRoot,
                                    m_p.value(QStringLiteral("undoLog")).toString(), ctx.token, ctx.progress);
        if (!run.ok())
            return run.takeError();
        const RenameRunResult& r = run.value();
        if (ctx.token.isCancelled())
            return Error(ErrorCode::Cancelled,
                         QStringLiteral("Cancelled after renaming %1 dump(s). Undo log: %2")
                             .arg(r.renamed)
                             .arg(QDir::toNativeSeparators(r.undoLogPath)));
        fillOutcome(o, r);
        o.summary = QStringLiteral("Renamed %1 dump(s).").arg(r.renamed);
        if (r.skipped + r.failed > 0)
            o.summary += QStringLiteral("  %1 skipped/failed.").arg(r.skipped + r.failed);
        // Verification: every renamed dump is at its new name and gone from the old one.
        int bad = 0;
        for (const auto& it : r.items)
            if (it.status == RenameStatus::Renamed
                && (!QFileInfo(it.targetPath).isDir()
                    || (QFileInfo::exists(it.sourcePath)
                        && QDir::cleanPath(it.sourcePath).compare(QDir::cleanPath(it.targetPath), Qt::CaseInsensitive) != 0)))
                ++bad;
        o.verification = (bad + r.failed) == 0 ? Verification::Passed : Verification::Failed;
        o.verificationDetail = bad + r.failed == 0
                                   ? QStringLiteral("%1 folder(s) checked at their new names").arg(r.renamed)
                                   : QStringLiteral("%1 dump(s) could not be renamed").arg(bad + r.failed);
        ctx.log.info(o.summary);
        return o;
    }

private:
    QString mode() const { return m_p.value(QStringLiteral("mode")).toString(QStringLiteral("preview")); }

    std::optional<NamePreset> preset(const SettingsValues& s) const
    {
        const QString k = m_p.value(QStringLiteral("preset")).toString();
        return namePresetFromKey(k.isEmpty() ? s.string(keys::kRenamePreset) : k);
    }

    // Per-item edits from the preview: {"source": path, "name": text, "selected": bool}.
    void applyEdits(std::vector<RenameItem>& plan) const
    {
        const QJsonArray edits = m_p.value(QStringLiteral("items")).toArray();
        for (const auto& v : edits) {
            const QJsonObject e = v.toObject();
            const QString src = QDir::cleanPath(QDir::fromNativeSeparators(e.value(QStringLiteral("source")).toString()));
            for (auto& it : plan)
                if (QDir::cleanPath(it.sourcePath) == src) {
                    if (e.contains(QStringLiteral("name")))
                        it.proposedName = e.value(QStringLiteral("name")).toString();
                    if (e.contains(QStringLiteral("selected")))
                        it.selected = e.value(QStringLiteral("selected")).toBool(true);
                }
        }
    }

    static void fillOutcome(JobOutcome& o, const RenameRunResult& r)
    {
        QJsonArray items;
        for (const auto& it : r.items) {
            items.append(it.toJson());
            if (it.status == RenameStatus::Failed || it.status == RenameStatus::TargetExists
                || it.status == RenameStatus::Duplicate || it.status == RenameStatus::InvalidName)
                o.warnings << QStringLiteral("%1: %2%3").arg(it.currentName, renameStatusName(it.status),
                                                             it.message.isEmpty() ? QString() : QStringLiteral(" — ") + it.message);
        }
        o.details.insert(QStringLiteral("items"), items);
        o.details.insert(QStringLiteral("renamed"), r.renamed);
        o.details.insert(QStringLiteral("skipped"), r.skipped);
        o.details.insert(QStringLiteral("failed"), r.failed);
        if (!r.undoLogPath.isEmpty()) {
            o.details.insert(QStringLiteral("undoLog"), QDir::toNativeSeparators(r.undoLogPath));
            o.outputs << r.undoLogPath;
        }
    }

    Result<JobOutcome> runUndo(JobContext& ctx)
    {
        auto r = undoDumpRenames(m_p.value(QStringLiteral("undoLog")).toString(), ctx.token, ctx.progress);
        if (!r.ok())
            return r.takeError();
        JobOutcome o;
        fillOutcome(o, r.value());
        o.summary = QStringLiteral("Restored %1 dump name(s).").arg(r.value().renamed);
        o.verification = r.value().failed == 0 ? Verification::Passed : Verification::Failed;
        o.verificationDetail = r.value().failed == 0 ? QStringLiteral("every logged rename was reversed")
                                                     : QStringLiteral("%1 rename(s) could not be reversed").arg(r.value().failed);
        return o;
    }

    QJsonObject m_p;
};

QString existingDirOrEmpty(const QVariant& v)
{
    const QString s = v.toString().trimmed();
    if (s.isEmpty() || QFileInfo(s).isDir())
        return {};
    return QStringLiteral("the folder does not exist.");
}

} // namespace

void registerModule(ModuleContext& ctx)
{
    auto& s = ctx.settings;
    {
        SettingDef d;
        d.key = keys::kLibraryFolders;
        d.section = QStringLiteral("General");
        d.group = QStringLiteral("Library");
        d.label = QStringLiteral("Library folders");
        d.description = QStringLiteral("Folders that hold your PS5 game dumps, one game per sub-folder. "
                                       "The Library scans these. Files in them are only read.");
        d.type = SettingType::StringList;
        d.defaultValue = QStringList();
        d.order = 100;
        s.add(d);
    }
    {
        SettingDef d;
        d.key = keys::kRenamePreset;
        d.section = QStringLiteral("General");
        d.group = QStringLiteral("Library");
        d.label = QStringLiteral("Dump Rename pattern");
        d.description = QStringLiteral("How Dump Rename names game folders when you do not pick a pattern.");
        d.type = SettingType::Enum;
        d.defaultValue = QStringLiteral("ppsa_title_v");
        d.choices = {{QStringLiteral("ppsa"), QStringLiteral("Title ID only"), QStringLiteral("PPSA01234")},
                     {QStringLiteral("ppsa_title"), QStringLiteral("Title ID + title"), QStringLiteral("PPSA01234 Spider-Man")},
                     {QStringLiteral("ppsa_title_v"), QStringLiteral("Title ID + title + version"),
                      QStringLiteral("PPSA01234 Spider-Man (01.050.000)")}};
        d.order = 101;
        s.add(d);
    }
    {
        SettingDef d;
        d.key = keys::kCoverCacheFolder;
        d.section = QStringLiteral("Storage");
        d.group = QStringLiteral("Folders");
        d.label = QStringLiteral("Cover art cache");
        d.description = QStringLiteral("Where the Library keeps resized cover art (260x390 PNG, a few hundred KB per "
                                       "game). Empty: the app's cache folder in your local app data.");
        d.type = SettingType::DirPath;
        d.defaultValue = QString();
        d.validator = existingDirOrEmpty;
        d.advanced = true;
        d.order = 100;
        s.add(d);
    }
    s.addLegacyKey(QStringLiteral("lib_folders"), keys::kLibraryFolders);

    ctx.jobs.add(jobkinds::kLibraryScan, QStringLiteral("Scan library"),
                 [](const QJsonObject& p) -> Result<std::unique_ptr<Job>> { return std::unique_ptr<Job>(new LibraryScanJob(p)); });
    ctx.jobs.add(jobkinds::kLibraryRename, QStringLiteral("Rename dumps"),
                 [](const QJsonObject& p) -> Result<std::unique_ptr<Job>> { return std::unique_ptr<Job>(new DumpRenameJob(p)); });
}

} // namespace fw::gameinfo
