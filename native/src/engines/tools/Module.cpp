#include "engines/tools/Module.h"

#include "core/FileUtil.h"
#include "engines/tools/DumpCheck.h"
#include "engines/tools/LanguageStripper.h"
#include "engines/tools/ReportBundle.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>

namespace fw::tools {

namespace {

QStringList toStringList(const QJsonValue& v)
{
    QStringList out;
    for (const auto& x : v.toArray())
        out << x.toString();
    return out;
}

// The app-wide free-space reserve, when the build has that setting.
qint64 freeSpaceMarginBytes(const SettingsValues& s)
{
    static const QString marginKey = QStringLiteral("storage.freeSpaceMarginMiB");
    if (const QVariantMap all = s.effective(); all.contains(marginKey) && all.value(marginKey).toLongLong() >= 0)
        return all.value(marginKey).toLongLong() * 1024 * 1024;
    return StripPlan().freeSpaceMarginBytes;
}

class ScanJob : public Job {
public:
    explicit ScanJob(QJsonObject p) : m_p(std::move(p)) {}
    QString kind() const override { return QStringLiteral("tools.languages.scan"); }
    QString title() const override { return QStringLiteral("Find languages in %1").arg(QFileInfo(m_p.value(QStringLiteral("folder")).toString()).fileName()); }
    QStringList phases() const override { return {QStringLiteral("Scanning")}; }
    QJsonObject parameters() const override { return m_p; }
    Status validate(const SettingsValues&) const override
    {
        if (!QFileInfo(m_p.value(QStringLiteral("folder")).toString()).isDir())
            return Error(ErrorCode::NotFound, QStringLiteral("Choose a game folder."));
        return success();
    }
    Result<JobOutcome> run(JobContext& ctx) override
    {
        const QString folder = m_p.value(QStringLiteral("folder")).toString();
        const QStringList roots = m_p.value(QStringLiteral("batch")).toBool() ? findGameRoots(folder)
                                                                              : QStringList{folder};
        ctx.progress->begin(QStringLiteral("Scanning"), roots.size(), 0, false);
        QJsonArray games;
        qint64 total = 0;
        int items = 0;
        for (const QString& root : roots) {
            if (!m_p.value(QStringLiteral("batch")).toBool() && !isGameRoot(root))
                return Error(ErrorCode::InvalidArgument, QStringLiteral("%1 is not a game folder (no eboot.bin).").arg(QDir::toNativeSeparators(root)));
            auto scan = scanLanguages(root, ctx.token);
            if (!scan.ok())
                return scan.takeError();
            total += scan->totalBytes;
            items += int(scan->items.size());
            if (!scan->items.empty())
                games.append(scan->toJson());
            ctx.progress->advance(1);
        }
        JobOutcome o;
        o.details.insert(QStringLiteral("games"), games);
        o.summary = roots.isEmpty() ? QStringLiteral("No game was found in %1 (a game folder contains eboot.bin).")
                                          .arg(QDir::toNativeSeparators(folder))
                  : items == 0 ? QStringLiteral("No separate language files found. This game may keep all languages in its main data files.")
                               : QStringLiteral("%1 language item(s) in %2 game(s), %3").arg(items).arg(games.size()).arg(formatBytes(total));
        o.verification = Verification::Skipped;
        return o;
    }

private:
    QJsonObject m_p;
};

class StripJob : public Job {
public:
    explicit StripJob(QJsonObject p) : m_p(std::move(p)) {}
    QString kind() const override { return QStringLiteral("tools.languages.strip"); }
    QString title() const override { return QStringLiteral("Remove languages from %1").arg(QFileInfo(m_p.value(QStringLiteral("gameRoot")).toString()).fileName()); }
    QString subtitle() const override { return QStringLiteral("%1 item(s), backed up first").arg(m_p.value(QStringLiteral("remove")).toArray().size()); }
    QStringList phases() const override { return {QStringLiteral("Backing up"), QStringLiteral("Checking backup"), QStringLiteral("Removing")}; }
    QJsonObject parameters() const override { return m_p; }
    Status validate(const SettingsValues& s) const override
    {
        if (!isGameRoot(m_p.value(QStringLiteral("gameRoot")).toString()))
            return Error(ErrorCode::InvalidArgument, QStringLiteral("Choose a game folder (it must contain eboot.bin)."));
        if (backupFolder(s).isEmpty())
            return Error(ErrorCode::InvalidArgument, QStringLiteral("Choose a backup folder. Removed languages are always backed up first."));
        return success();
    }
    Result<JobOutcome> run(JobContext& ctx) override
    {
        StripPlan plan;
        plan.gameRoot = m_p.value(QStringLiteral("gameRoot")).toString();
        plan.remove = toStringList(m_p.value(QStringLiteral("remove")));
        plan.backupFolder = backupFolder(ctx.settings);
        // Checked here as well, so a refused folder is not created inside the game first.
        if (isSameOrInside(plan.backupFolder, plan.gameRoot))
            return Error(ErrorCode::InvalidArgument, QStringLiteral("The backup folder cannot be inside the game folder."));
        QDir().mkpath(plan.backupFolder);
        plan.freeSpaceMarginBytes = freeSpaceMarginBytes(ctx.settings);
        auto r = stripLanguages(plan, ctx.token, ctx.progress);
        if (!r.ok())
            return r.takeError();
        JobOutcome o;
        o.outputs << r->backupZip;
        o.summary = QStringLiteral("Removed %1 item(s), freed %2. Backup: %3")
                        .arg(r->removed).arg(formatBytes(r->bytesFreed), QFileInfo(r->backupZip).fileName());
        for (const QString& f : r->failed)
            o.warnings << QStringLiteral("Could not remove %1 completely (it is in the backup).").arg(f);
        for (const QString& f : r->leftInPlace)
            o.warnings << QStringLiteral("Left in place, because it changed or appeared after the backup was written: %1").arg(f);
        o.details.insert(QStringLiteral("removed"), r->removed);
        o.details.insert(QStringLiteral("bytesFreed"), r->bytesFreed);
        o.details.insert(QStringLiteral("backupZip"), QDir::toNativeSeparators(r->backupZip));
        if (r->failed.isEmpty()) {
            o.verification = Verification::Passed;
            o.verificationDetail = QStringLiteral("Backup read back and checked before anything was removed.");
        } else {
            // The backup is good and what was removed is in it; the job did not do all it was asked.
            o.verification = Verification::Inconclusive;
            o.verificationDetail = QStringLiteral("Backup read back and checked; %1 of %2 item(s) could not be removed completely.")
                                       .arg(r->failed.size()).arg(plan.remove.size());
            o.summary = QStringLiteral("%1 of %2 item(s) could not be removed completely. Removed %3, freed %4. Backup: %5")
                            .arg(r->failed.size()).arg(plan.remove.size()).arg(r->removed)
                            .arg(formatBytes(r->bytesFreed), QDir::toNativeSeparators(r->backupZip));
        }
        return o;
    }

private:
    QString backupFolder(const SettingsValues& s) const
    {
        const QString p = m_p.value(QStringLiteral("backupFolder")).toString();
        return p.isEmpty() ? s.string(keys::kLanguageBackupFolder) : p;
    }
    QJsonObject m_p;
};

class RestoreJob : public Job {
public:
    explicit RestoreJob(QJsonObject p) : m_p(std::move(p)) {}
    QString kind() const override { return QStringLiteral("tools.languages.restore"); }
    QString title() const override { return QStringLiteral("Restore languages from %1").arg(QFileInfo(m_p.value(QStringLiteral("backupZip")).toString()).fileName()); }
    QStringList phases() const override { return {QStringLiteral("Restoring")}; }
    QJsonObject parameters() const override { return m_p; }
    Result<JobOutcome> run(JobContext& ctx) override
    {
        const QString gameRoot = m_p.value(QStringLiteral("gameRoot")).toString();
        auto r = restoreLanguageBackup(m_p.value(QStringLiteral("backupZip")).toString(), gameRoot, ctx.token, ctx.progress,
                                       freeSpaceMarginBytes(ctx.settings));
        if (!r.ok())
            return r.takeError();
        JobOutcome o;
        const qsizetype skipped = r->skippedExisting.size();
        o.summary = QStringLiteral("Restored %1 file(s).").arg(r->restored);
        if (skipped > 0) {
            o.summary += QStringLiteral(" %1 already in the game were left as they are.").arg(skipped);
            constexpr qsizetype kMaxListed = 50;
            for (const QString& f : r->skippedExisting.mid(0, kMaxListed))
                o.warnings << QStringLiteral("Already in the game, not replaced: %1").arg(f);
            if (skipped > kMaxListed)
                o.warnings << QStringLiteral("…and %1 more that were already in the game.").arg(skipped - kMaxListed);
        }
        if (r->otherGame)
            o.warnings << QStringLiteral("This backup was made from “%1”, and was restored into “%2”.")
                              .arg(r->backupGame, QFileInfo(gameRoot).fileName());
        o.details.insert(QStringLiteral("restored"), r->restored);
        o.details.insert(QStringLiteral("skippedExisting"), int(skipped));
        o.verification = Verification::Passed;
        o.verificationDetail = skipped > 0 ? QStringLiteral("Every restored file matched its checksum in the backup; files already "
                                                            "in the game were not compared.")
                                           : QStringLiteral("Every restored file matched its checksum in the backup.");
        return o;
    }

private:
    QJsonObject m_p;
};

// Reads a dump and reports what it finds; it changes nothing, so it succeeds
// whenever the walk completed, whatever the dump is like.
class DumpCheckJob : public Job {
public:
    explicit DumpCheckJob(QJsonObject p) : m_p(std::move(p)) {}
    QString kind() const override { return jobkinds::kDumpCheck; }
    QString title() const override { return QStringLiteral("Check %1").arg(name()); }
    QString subtitle() const override { return QDir::toNativeSeparators(folder()); }
    QStringList phases() const override { return {QStringLiteral("Checking")}; }
    QJsonObject parameters() const override { return m_p; }
    Status validate(const SettingsValues&) const override
    {
        if (!QFileInfo(folder()).isDir())
            return Error(ErrorCode::NotFound, QStringLiteral("Choose the game folder to check."));
        return success();
    }
    Result<JobOutcome> run(JobContext& ctx) override
    {
        auto r = checkDump(folder(), ctx.token, ctx.progress);
        if (!r.ok())
            return r.takeError();
        JobOutcome o;
        o.summary = r->summary();
        o.details = r->toJson();
        o.verification = r->errors == 0 && r->warnings == 0 ? Verification::Passed : Verification::Inconclusive;
        o.verificationDetail = QStringLiteral("Read-only check of the folder structure, the executables' headers, the backport "
                                              "state and the file names. Nothing in the dump was changed.");
        // The first problems and warnings in the job's own words too (the queue and the log show them); the details hold all.
        constexpr int kMaxListed = 20;
        int listed = 0;
        for (const DumpFinding& f : r->findings) {
            if (f.severity == FindingSeverity::Info)
                continue;
            if (listed == kMaxListed)
                break;
            ++listed;
            const QString line = QStringLiteral("%1%2").arg(f.path.isEmpty() ? QString() : f.path + QStringLiteral(": "), f.message);
            o.warnings << line;
            ctx.log.warn(line, QJsonObject{{QStringLiteral("code"), f.code}, {QStringLiteral("severity"), findingSeverityName(f.severity)}});
        }
        if (const qint64 more = r->errors + r->warnings - listed; more > 0)
            o.warnings << QStringLiteral("…and %1 more in the check's details.").arg(formatCount(more));
        ctx.log.info(QStringLiteral("Dump check of %1: %2").arg(QDir::toNativeSeparators(folder()), o.summary));
        return o;
    }

private:
    QString folder() const { return m_p.value(QStringLiteral("folder")).toString(); }
    QString name() const
    {
        const QString given = m_p.value(QStringLiteral("title")).toString().trimmed();
        return given.isEmpty() ? QFileInfo(folder()).fileName() : given;
    }
    QJsonObject m_p;
};

class ReportJob : public Job {
public:
    explicit ReportJob(QJsonObject p) : m_p(std::move(p)) {}
    QString kind() const override { return QStringLiteral("tools.report"); }
    bool runsWhileHeld() const override { return true; } // the user asked for it now, whatever the queue does
    QString title() const override { return QStringLiteral("Create a problem report"); }
    QStringList phases() const override { return {QStringLiteral("Collecting")}; }
    QJsonObject parameters() const override { return m_p; }
    Result<JobOutcome> run(JobContext& ctx) override
    {
        ReportOptions opt;
        opt.destinationFolder = m_p.value(QStringLiteral("destinationFolder")).toString();
        opt.logsFolder = m_p.value(QStringLiteral("logsFolder")).toString();
        opt.extraFiles = toStringList(m_p.value(QStringLiteral("extraFiles")));
        opt.diagnostics = m_p.value(QStringLiteral("diagnostics")).toObject();
        opt.redactUserPaths = ctx.settings.boolean(keys::kRedactReports);
        auto r = buildReportBundle(opt, ctx.token, ctx.progress);
        if (!r.ok())
            return r.takeError();
        JobOutcome o;
        o.outputs << r->zipPath;
        o.summary = QStringLiteral("Report saved: %1. Attach it to a new issue.").arg(QFileInfo(r->zipPath).fileName());
        o.details.insert(QStringLiteral("contents"), QJsonArray::fromStringList(r->contents));
        o.details.insert(QStringLiteral("issueUrl"), kNewIssueUrl);
        o.verification = Verification::Passed;
        o.verificationDetail = QStringLiteral("Archive read back and checked.");
        return o;
    }

private:
    QJsonObject m_p;
};

template <typename T>
JobFactory::Creator creator()
{
    return [](const QJsonObject& p) -> Result<std::unique_ptr<Job>> { return std::unique_ptr<Job>(new T(p)); };
}

} // namespace

void registerModule(ModuleContext& ctx)
{
    {
        SettingDef d;
        d.key = keys::kLanguageBackupFolder;
        d.section = QStringLiteral("Storage");
        d.group = QStringLiteral("Language Stripper");
        d.label = QStringLiteral("Backup folder for removed languages");
        d.description = QStringLiteral("Every language the stripper removes is first saved here as a verified ZIP, so you can restore it. Must not be inside a game folder.");
        d.type = SettingType::DirPath;
        d.defaultValue = QString();
        d.order = 50;
        ctx.settings.add(d);
    }
    {
        SettingDef d;
        d.key = keys::kRedactReports;
        d.section = QStringLiteral("Logging");
        d.group = QStringLiteral("Problem reports");
        d.label = QStringLiteral("Hide my user name in problem reports");
        d.description = QStringLiteral("Replaces your user folder with ~ in the logs and settings included in a report.");
        d.type = SettingType::Bool;
        d.defaultValue = true;
        d.perJob = true;
        d.order = 50;
        ctx.settings.add(d);
    }
    ctx.jobs.add(QStringLiteral("tools.languages.scan"), QStringLiteral("Find languages"), creator<ScanJob>());
    ctx.jobs.add(QStringLiteral("tools.languages.strip"), QStringLiteral("Remove languages"), creator<StripJob>());
    ctx.jobs.add(QStringLiteral("tools.languages.restore"), QStringLiteral("Restore languages"), creator<RestoreJob>());
    ctx.jobs.add(jobkinds::kDumpCheck, QStringLiteral("Dump check"), creator<DumpCheckJob>());
    ctx.jobs.add(QStringLiteral("tools.report"), QStringLiteral("Problem report"), creator<ReportJob>());
}

} // namespace fw::tools
