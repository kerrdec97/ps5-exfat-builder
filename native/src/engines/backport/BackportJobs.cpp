#include "engines/backport/BackportJobs.h"

#include "engines/backport/ApplyJournal.h"
#include "engines/backport/BackportPlanner.h"
#include "engines/backport/BackportRunner.h"
#include "engines/backport/BackportSettings.h"
#include "engines/backport/RestoreService.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>

namespace fw::backport {

namespace {

QString gameOf(const QJsonObject& p)
{
    return p.value(QStringLiteral("game")).toString();
}

QString folderTitle(const QString& game, const QString& fallback)
{
    const QString n = QFileInfo(QDir::cleanPath(game)).fileName();
    return n.isEmpty() ? fallback : n;
}

QJsonObject executableJson(const ExecFile& e)
{
    QJsonObject o;
    o.insert(QStringLiteral("file"), e.rel);
    o.insert(QStringLiteral("class"), fileClassName(e.cls));
    o.insert(QStringLiteral("area"), e.area == Area::Game ? QStringLiteral("game")
                                     : e.area == Area::Fakelib ? QStringLiteral("fakelib")
                                                               : QStringLiteral("decrypted"));
    o.insert(QStringLiteral("size"), double(e.size));
    if (e.sdk)
        o.insert(QStringLiteral("sdk"), *e.sdk);
    if (e.ps5)
        o.insert(QStringLiteral("ps5Sdk"), QStringLiteral("0x%1").arg(*e.ps5, 8, 16, QLatin1Char('0')));
    if (e.ps4)
        o.insert(QStringLiteral("ps4Sdk"), QStringLiteral("0x%1").arg(*e.ps4, 8, 16, QLatin1Char('0')));
    if (!e.problem.isEmpty())
        o.insert(QStringLiteral("problem"), e.problem);
    if (!e.sha256.isEmpty())
        o.insert(QStringLiteral("sha256"), e.sha256);
    return o;
}

QJsonObject scanJson(const ScanResult& s, const GameMetadata& meta)
{
    QJsonObject o;
    o.insert(QStringLiteral("folder"), QDir::toNativeSeparators(s.root));
    o.insert(QStringLiteral("titleId"), meta.titleId);
    o.insert(QStringLiteral("title"), meta.title);
    o.insert(QStringLiteral("version"), meta.version);
    o.insert(QStringLiteral("zipPrefix"), zipPrefix(meta, QFileInfo(s.root).fileName()));
    if (const auto sdk = s.gameSdk())
        o.insert(QStringLiteral("sdk"), *sdk);
    if (meta.paramSdk)
        o.insert(QStringLiteral("paramJsonSdk"), *meta.paramSdk);
    if (!meta.requiredFwText().isEmpty())
        o.insert(QStringLiteral("requiredFw"), meta.requiredFwText());
    QJsonArray ex, enc;
    for (const ExecFile& e : s.executables) {
        ex.append(executableJson(e));
        if (e.area == Area::Game && e.isEncryptedOrCompressed())
            enc.append(e.rel);
    }
    o.insert(QStringLiteral("executables"), ex);
    o.insert(QStringLiteral("encrypted"), enc);
    o.insert(QStringLiteral("dumpFakelib"), QJsonArray::fromStringList(s.fakelibFiles));
    o.insert(QStringLiteral("nestedEbootFolders"), QJsonArray::fromStringList(s.nestedEbootDirs));
    o.insert(QStringLiteral("leftoverBak"), QJsonArray::fromStringList(s.leftoverBaks));
    o.insert(QStringLiteral("metadataSkipped"), s.metadataSkipped.size());
    return o;
}

} // namespace

// ---- backport.run ------------------------------------------------------------------

BackportRunJob::BackportRunJob(QJsonObject params) : m_params(std::move(params)) {}

QString BackportRunJob::title() const
{
    return folderTitle(gameOf(m_params), QStringLiteral("Backport"));
}

QString BackportRunJob::subtitle() const
{
    return QStringLiteral("Backport · %1").arg(QDir::toNativeSeparators(gameOf(m_params)));
}

QStringList BackportRunJob::phases() const
{
    return runPhases();
}

Status BackportRunJob::validate(const SettingsValues& settings) const
{
    const QString game = gameOf(m_params);
    if (game.isEmpty() || !QFileInfo(game).isDir())
        return makeError(Code::NotAGame, QStringLiteral("The game folder %1 does not exist.").arg(QDir::toNativeSeparators(game)));
    if (!QFileInfo(QDir(game).filePath(QStringLiteral("eboot.bin"))).isFile())
        return makeError(Code::NotAGame,
                         QStringLiteral("eboot.bin is not in %1. Pick the game's root folder.").arg(QDir::toNativeSeparators(game)));
    const BackportOptions o = optionsFromSettings(settings, game);
    if (!sdkPair(o.targetSdk))
        return makeError(Code::TargetSdkInvalid, QStringLiteral("Target SDK %1 is not supported.").arg(o.targetSdk));
    if (o.originalsDir.isEmpty())
        return makeError(Code::ZipFolderInvalid,
                         QStringLiteral("Choose the originals ZIP folder in Settings > Backport before starting."));
    if (!o.applyToGame && o.backportDir.isEmpty())
        return makeError(Code::ZipFolderInvalid,
                         QStringLiteral("Apply to game folders is off and no backport ZIP folder is set, so the run "
                                        "would produce nothing."));
    return success();
}

Result<JobOutcome> BackportRunJob::run(JobContext& ctx)
{
    BackportOptions o = optionsFromSettings(ctx.settings, gameOf(m_params));
    o.confirmDiffers = m_params.value(QStringLiteral("confirmDiffers")).toBool(false);
    FW_TRY_ASSIGN(const RunResult r, runBackport(o, ctx));
    return toOutcome(r);
}

// ---- backport.restore ---------------------------------------------------------------

BackportRestoreJob::BackportRestoreJob(QJsonObject params) : m_params(std::move(params)) {}

QString BackportRestoreJob::title() const
{
    if (m_params.contains(QStringLiteral("journal")))
        return QStringLiteral("Roll back an interrupted backport");
    return QStringLiteral("Restore %1").arg(folderTitle(gameOf(m_params), QStringLiteral("originals")));
}

QStringList BackportRestoreJob::phases() const
{
    return {QStringLiteral("Check"), QStringLiteral("Restore")};
}

Status BackportRestoreJob::validate(const SettingsValues&) const
{
    const QString journal = m_params.value(QStringLiteral("journal")).toString();
    if (!journal.isEmpty())
        return QFileInfo(journal).isFile()
            ? success()
            : makeError(Code::RestoreFailed, QStringLiteral("The apply journal %1 does not exist.").arg(QDir::toNativeSeparators(journal)));
    const QString game = gameOf(m_params);
    if (game.isEmpty() || !QFileInfo(game).isDir())
        return makeError(Code::NotAGame, QStringLiteral("The game folder %1 does not exist.").arg(QDir::toNativeSeparators(game)));
    return success();
}

Result<JobOutcome> BackportRestoreJob::run(JobContext& ctx)
{
    if (ctx.progress)
        ctx.progress->beginPhase(0, 0);
    JobOutcome o;
    const QString journalPath = m_params.value(QStringLiteral("journal")).toString();
    if (!journalPath.isEmpty()) {
        FW_TRY_ASSIGN(ApplyJournal j, ApplyJournal::load(journalPath));
        if (ctx.progress)
            ctx.progress->beginPhase(1, 0, qint64(j.entries().size()));
        QStringList problems;
        Status st = j.rollback(ctx.token, &problems);
        if (!st.ok())
            return st.takeError();
        o.summary = QStringLiteral("The interrupted backport of %1 was rolled back.").arg(QDir::toNativeSeparators(j.game()));
        o.outputs << j.game();
        o.verification = Verification::Passed;
        o.verificationDetail = QStringLiteral("Every file holds its original bytes again.");
        return o;
    }

    RestoreOptions ro;
    ro.game = gameOf(m_params);
    ro.zip = m_params.value(QStringLiteral("zip")).toString();
    ro.originalsDir = ctx.settings.string(keys::kOriginalsZipFolder).trimmed();
    if (ctx.progress)
        ctx.progress->beginPhase(1, 0);
    FW_TRY_ASSIGN(const RestoreReport rep, restoreOriginals(ro, ctx.token, ctx.progress));
    for (const QString& w : rep.warnings)
        ctx.log.warn(w);
    if (!rep.refused.isEmpty())
        return makeError(Code::RestoreFailed,
                         QStringLiteral("The originals ZIP has entries that would be written outside the game folder, so "
                                        "nothing was restored: %1")
                             .arg(rep.refused.join(QStringLiteral("; "))));
    if (!rep.errors.isEmpty())
        return makeError(Code::RestoreFailed,
                         QStringLiteral("Partial restore: %1 files restored, %2 could not be: %3")
                             .arg(rep.restored + rep.unchanged)
                             .arg(rep.errors.size())
                             .arg(rep.errors.join(QStringLiteral("; "))));
    o.summary = QStringLiteral("Restored %1 files from %2%3.")
                    .arg(rep.restored + rep.unchanged)
                    .arg(QFileInfo(rep.zip).fileName(),
                         rep.removedAdded.isEmpty() ? QString()
                                                    : QStringLiteral("; removed %1 added files").arg(rep.removedAdded.size()));
    o.outputs << ro.game;
    o.warnings = rep.warnings;
    o.verification = Verification::Passed;
    o.verificationDetail = QStringLiteral("Every restored file matches the originals ZIP%1.")
                               .arg(rep.manifestPath.isEmpty() ? QString() : QStringLiteral(" and the manifest"));
    o.details.insert(QStringLiteral("zip"), QDir::toNativeSeparators(rep.zip));
    o.details.insert(QStringLiteral("restored"), rep.restored);
    o.details.insert(QStringLiteral("unchanged"), rep.unchanged);
    o.details.insert(QStringLiteral("removedAdded"), QJsonArray::fromStringList(rep.removedAdded));
    o.details.insert(QStringLiteral("keptAdded"), QJsonArray::fromStringList(rep.keptAdded));
    return o;
}

// ---- backport.scan ---------------------------------------------------------------------

BackportScanJob::BackportScanJob(QJsonObject params) : m_params(std::move(params)) {}

QString BackportScanJob::title() const
{
    const QString game = gameOf(m_params);
    return game.isEmpty() ? QStringLiteral("Scan the game library") : QStringLiteral("Scan %1").arg(folderTitle(game, QString()));
}

Result<JobOutcome> BackportScanJob::run(JobContext& ctx)
{
    if (ctx.progress)
        ctx.progress->beginPhase(0, 0);
    JobOutcome o;
    o.verification = Verification::Skipped;
    o.verificationDetail = QStringLiteral("Report only; nothing was changed.");
    const QString game = gameOf(m_params);
    if (!game.isEmpty()) {
        BackportOptions opt = optionsFromSettings(ctx.settings, game);
        opt.stagingHint = ctx.tempDir;
        FW_TRY_ASSIGN(const BackportPlan plan, planBackport(opt, ctx.token));
        QJsonObject g = scanJson(plan.scan, plan.meta);
        QJsonArray issues;
        for (const Issue& i : plan.issues) {
            QJsonObject io;
            io.insert(QStringLiteral("severity"), severityName(i.severity));
            io.insert(QStringLiteral("code"), codeName(i.code));
            io.insert(QStringLiteral("message"), i.message);
            issues.append(io);
            if (i.severity != Severity::Info)
                o.warnings << i.message;
        }
        g.insert(QStringLiteral("issues"), issues);
        g.insert(QStringLiteral("fakelibState"), fakelibStateName(plan.fakelibState));
        g.insert(QStringLiteral("fakelibExtras"), QJsonArray::fromStringList(plan.fakelibExtras));
        g.insert(QStringLiteral("manifestVerdict"), verdictName(plan.verdict));
        g.insert(QStringLiteral("canRun"), plan.canRun());
        g.insert(QStringLiteral("plan"), plan.summary());
        o.details = g;
        const int n = int(plan.scan.executables.size());
        o.summary = plan.canRun()
            ? QStringLiteral("%1 executables, SDK %2. Ready: %3").arg(n).arg(plan.gameSdk.value_or(0)).arg(plan.summary())
            : QStringLiteral("%1 executables. Not ready: %2").arg(n).arg(plan.blocking()->message);
        return o;
    }

    // Library mode: backport.scanFolders, backport.scanDepth; the ZIP folders are excluded (S38).
    QStringList excludes;
    for (const QString& k : {keys::kOriginalsZipFolder, keys::kBackportZipFolder}) {
        const QString d = ctx.settings.string(k).trimmed();
        if (!d.isEmpty())
            excludes << QDir::cleanPath(QFileInfo(d).absoluteFilePath());
    }
    QStringList warnings;
    const QStringList games = discoverGames(ctx.settings.stringList(keys::kScanFolders),
                                            int(ctx.settings.integer(keys::kScanDepth)), excludes, ctx.token, &warnings);
    if (!ctx.token.checkpoint())
        return Error::cancelled();
    if (ctx.progress)
        ctx.progress->beginPhase(0, qint64(games.size()), qint64(games.size()), false);
    QJsonArray list;
    for (const QString& g : games) {
        if (!ctx.token.checkpoint())
            return Error::cancelled();
        auto s = scanGame(g, ctx.token, ScanMode::Quick, excludes);
        if (!s.ok()) {
            if (s.error().isCancelled())
                return s.takeError();
            warnings << s.error().message;
            continue;
        }
        list.append(scanJson(s.value(), readGameMetadata(g)));
        if (ctx.progress)
            ctx.progress->advance(1);
    }
    o.details.insert(QStringLiteral("games"), list);
    o.warnings = warnings;
    o.summary = QStringLiteral("Found %1 games.").arg(games.size());
    return o;
}

void registerBackportJobs(JobFactory& jobs)
{
    jobs.add(kJobRun, QStringLiteral("Backport"),
             [](const QJsonObject& p) -> Result<std::unique_ptr<Job>> { return std::unique_ptr<Job>(new BackportRunJob(p)); });
    jobs.add(kJobRestore, QStringLiteral("Restore originals"),
             [](const QJsonObject& p) -> Result<std::unique_ptr<Job>> { return std::unique_ptr<Job>(new BackportRestoreJob(p)); });
    jobs.add(kJobScan, QStringLiteral("Backport scan"),
             [](const QJsonObject& p) -> Result<std::unique_ptr<Job>> { return std::unique_ptr<Job>(new BackportScanJob(p)); });
}

} // namespace fw::backport
