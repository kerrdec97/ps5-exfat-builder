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
        opt.preset = preset(ctx.settings).value_or(NamePreset::TitleIdTitleVersion);
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
        fillOutcome(o, r);
        o.summary = r.cancelled ? QStringLiteral("Cancelled after renaming %1 dump(s).").arg(r.renamed)
                                : QStringLiteral("Renamed %1 dump(s).").arg(r.renamed);
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
        if (bad + r.failed > 0) {
            o.verification = Verification::Failed;
            o.verificationDetail = QStringLiteral("%1 dump(s) could not be renamed").arg(bad + r.failed);
        } else if (r.cancelled) {
            // What was renamed stays renamed and is in the undo log; the rest was not reached.
            o.verification = Verification::Inconclusive;
            o.verificationDetail = QStringLiteral("cancelled; %1 folder(s) checked at their new names").arg(r.renamed);
        } else {
            o.verification = Verification::Passed;
            o.verificationDetail = QStringLiteral("%1 folder(s) checked at their new names").arg(r.renamed);
        }
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
    // When an apply job carries edits, a dump that none of them names is left
    // alone: it was not in the preview the user approved.
    void applyEdits(std::vector<RenameItem>& plan) const
    {
        if (!m_p.contains(QStringLiteral("items")))
            return;
        const auto key = [](const QString& path) {
            const QString c = QDir::cleanPath(QDir::fromNativeSeparators(path));
#ifdef Q_OS_WIN
            return c.toLower();
#else
            return c;
#endif
        };
        const QJsonArray edits = m_p.value(QStringLiteral("items")).toArray();
        std::vector<bool> named(plan.size(), false);
        for (const auto& v : edits) {
            const QJsonObject e = v.toObject();
            const QString src = key(e.value(QStringLiteral("source")).toString());
            for (std::size_t i = 0; i < plan.size(); ++i)
                if (key(plan[i].sourcePath) == src) {
                    named[i] = true;
                    if (e.contains(QStringLiteral("name")))
                        plan[i].proposedName = e.value(QStringLiteral("name")).toString();
                    if (e.contains(QStringLiteral("selected")))
                        plan[i].selected = e.value(QStringLiteral("selected")).toBool(true);
                }
        }
        if (mode() != u"apply")
            return;
        for (std::size_t i = 0; i < plan.size(); ++i)
            if (!named[i])
                plan[i].selected = false;
    }

    static void fillOutcome(JobOutcome& o, const RenameRunResult& r)
    {
        QJsonArray items;
        for (const auto& it : r.items) {
            items.append(it.toJson());
            // (A "not selected" item with a message is one a cancel left untouched.)
            if (it.status == RenameStatus::Failed || it.status == RenameStatus::TargetExists
                || it.status == RenameStatus::Duplicate || it.status == RenameStatus::InvalidName
                || (it.status == RenameStatus::NotSelected && !it.message.isEmpty()))
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
        // Not reversed without being an error: the original name is taken, or
        // the undo was cancelled before the entry was reached.
        int blocked = 0, notReached = 0;
        for (const auto& it : r.value().items) {
            blocked += it.status == RenameStatus::TargetExists ? 1 : 0;
            notReached += it.status == RenameStatus::NotSelected ? 1 : 0;
        }
        const int left = r.value().failed + blocked + notReached;
        o.summary = r.value().cancelled
                        ? QStringLiteral("Cancelled after restoring %1 dump name(s).").arg(r.value().renamed)
                        : QStringLiteral("Restored %1 dump name(s).").arg(r.value().renamed);
        if (left > 0)
            o.summary += QStringLiteral("  %1 not restored.").arg(left);
        if (r.value().failed > 0) {
            o.verification = Verification::Failed;
            o.verificationDetail = QStringLiteral("%1 rename(s) could not be reversed").arg(r.value().failed + blocked);
        } else if (left > 0) {
            o.verification = Verification::Inconclusive;
            o.verificationDetail =
                blocked > 0 && notReached > 0
                    ? QStringLiteral("%1 rename(s) not reversed: the original name is taken; %2 not reached before the cancel")
                          .arg(blocked)
                          .arg(notReached)
                    : blocked > 0 ? QStringLiteral("%1 rename(s) not reversed: the original name is taken").arg(blocked)
                                  : QStringLiteral("cancelled; %1 rename(s) not reversed").arg(notReached);
        } else {
            o.verification = Verification::Passed;
            o.verificationDetail = QStringLiteral("every logged rename was reversed");
        }
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
        d.folderList = true;
        d.defaultValue = QStringList();
        // The Library page's folders: Reset section / Reset all would take
        // them from the page (5.0.2 kept them in library.json only).
        d.keptOnReset = true;
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

    // ---- Build › Output name (legacy Build tab naming presets) --------------------------
    {
        SettingDef d;
        d.key = keys::kNamePreset;
        d.section = QStringLiteral("Build");
        d.group = QStringLiteral("Output name");
        d.label = QStringLiteral("File name");
        d.description = QStringLiteral("How the Build page names new images. You can still edit the name for each job.");
        d.type = SettingType::Enum;
        d.defaultValue = imageNamePresetKey(ImageNamePreset::TitleIdTitleVersion);
        d.choices = {{QStringLiteral("ppsa_title_ver"), QStringLiteral("Title ID, title and version"),
                      QStringLiteral("PPSA01234 Spider-Man (01.050.000)")},
                     {QStringLiteral("ppsa_title_ver_fw"), QStringLiteral("Title ID, title, version and firmware"),
                      QStringLiteral("PPSA01234 Spider-Man (01.050.000) (FW4)")},
                     {QStringLiteral("ppsa_title"), QStringLiteral("Title ID and title"), QStringLiteral("PPSA01234 Spider-Man")},
                     {QStringLiteral("ppsa"), QStringLiteral("Title ID only"), QStringLiteral("PPSA01234")},
                     {QStringLiteral("folder"), QStringLiteral("Source folder name"), QStringLiteral("The dump folder's own name")},
                     {QStringLiteral("custom"), QStringLiteral("Custom template"), QStringLiteral("The template below")}};
        d.order = 10;
        s.add(d);
    }
    {
        SettingDef d;
        d.key = keys::kNameTemplate;
        d.section = QStringLiteral("Build");
        d.group = QStringLiteral("Output name");
        d.label = QStringLiteral("Template");
        d.description = QStringLiteral("Tokens: %TITLE% %PPSA% %VERSION% %FIRMWARE% %SDK% %TYPE% %DATE% %FOLDER%. "
                                       "Empty brackets left by a missing value are removed.");
        d.type = SettingType::String;
        d.defaultValue = kDefaultImageNameTemplate;
        d.enabledWhenKey = keys::kNamePreset;
        d.enabledWhenValue = QStringLiteral("custom");
        d.order = 11;
        s.add(d);
    }
    {
        SettingDef d;
        d.key = keys::kFirmwareOverride;
        d.section = QStringLiteral("Build");
        d.group = QStringLiteral("Output name");
        d.label = QStringLiteral("Firmware in names");
        d.description = QStringLiteral("Automatic uses the system software the dump requires (param.json). "
                                       "Choose a number to name images for another firmware.");
        d.type = SettingType::Enum;
        d.defaultValue = QStringLiteral("auto");
        d.choices = {{QStringLiteral("auto"), QStringLiteral("Automatic"), {}}};
        for (int fw = 1; fw <= 15; ++fw)
            d.choices.push_back({QString::number(fw), QStringLiteral("FW%1").arg(fw), {}});
        d.advanced = true;
        d.order = 12;
        s.add(d);
    }
    s.addLegacyKey(QStringLiteral("name_preset"), keys::kNamePreset, [](const QVariant& v) -> std::optional<QVariant> {
        const auto p = imageNamePresetFromKey(v.toString());
        if (!p)
            return std::nullopt;
        return QVariant(imageNamePresetKey(*p));
    });
    s.addLegacyKey(QStringLiteral("name_custom_template"), keys::kNameTemplate,
                   [](const QVariant& v) -> std::optional<QVariant> {
                       const QString t = v.toString().trimmed();
                       return t.isEmpty() ? std::nullopt : std::optional<QVariant>(t);
                   });

    ctx.jobs.add(jobkinds::kLibraryScan, QStringLiteral("Scan library"),
                 [](const QJsonObject& p) -> Result<std::unique_ptr<Job>> { return std::unique_ptr<Job>(new LibraryScanJob(p)); });
    ctx.jobs.add(jobkinds::kLibraryRename, QStringLiteral("Rename dumps"),
                 [](const QJsonObject& p) -> Result<std::unique_ptr<Job>> { return std::unique_ptr<Job>(new DumpRenameJob(p)); });
}

} // namespace fw::gameinfo
