#include "engines/tools/Module.h"

#include "core/FileUtil.h"
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
        o.summary = items == 0 ? QStringLiteral("No separate language files found. This game may keep all languages in its main data files.")
                               : QStringLiteral("%1 language items in %2 game(s), %3").arg(items).arg(games.size()).arg(formatBytes(total));
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
    QString subtitle() const override { return QStringLiteral("%1 items, backed up first").arg(m_p.value(QStringLiteral("remove")).toArray().size()); }
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
        QDir().mkpath(plan.backupFolder);
        auto r = stripLanguages(plan, ctx.token, ctx.progress);
        if (!r.ok())
            return r.takeError();
        JobOutcome o;
        o.outputs << r->backupZip;
        o.verification = Verification::Passed;
        o.verificationDetail = QStringLiteral("Backup read back and checked before anything was removed.");
        o.summary = QStringLiteral("Removed %1 items, freed %2. Backup: %3")
                        .arg(r->removed).arg(formatBytes(r->bytesFreed), QFileInfo(r->backupZip).fileName());
        for (const QString& f : r->failed)
            o.warnings << QStringLiteral("Could not remove %1 (still backed up).").arg(f);
        o.details.insert(QStringLiteral("removed"), r->removed);
        o.details.insert(QStringLiteral("bytesFreed"), r->bytesFreed);
        if (!r->failed.isEmpty())
            return Error(ErrorCode::IoError,
                         QStringLiteral("%1 of %2 items could not be removed. Everything is in the backup %3.")
                             .arg(r->failed.size()).arg(plan.remove.size()).arg(QFileInfo(r->backupZip).fileName()));
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
        auto n = restoreLanguages(m_p.value(QStringLiteral("backupZip")).toString(), m_p.value(QStringLiteral("gameRoot")).toString(),
                                  ctx.token, ctx.progress);
        if (!n.ok())
            return n.takeError();
        JobOutcome o;
        o.summary = QStringLiteral("Restored %1 files.").arg(n.value());
        o.details.insert(QStringLiteral("restored"), n.value());
        o.verification = Verification::Passed;
        o.verificationDetail = QStringLiteral("Every restored file matched its checksum in the backup.");
        return o;
    }

private:
    QJsonObject m_p;
};

class ReportJob : public Job {
public:
    explicit ReportJob(QJsonObject p) : m_p(std::move(p)) {}
    QString kind() const override { return QStringLiteral("tools.report"); }
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
    ctx.jobs.add(QStringLiteral("tools.report"), QStringLiteral("Problem report"), creator<ReportJob>());
}

} // namespace fw::tools
