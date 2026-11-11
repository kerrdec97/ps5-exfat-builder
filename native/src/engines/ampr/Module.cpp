#include "engines/ampr/Module.h"

#include "core/ArchiveSource.h"
#include "core/FileUtil.h"
#include "engines/ampr/GamePack.h"
#include "engines/ampr/HostPaths.h"
#include "engines/ampr/Runtime.h"
#include "engines/general/Module.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonObject>

namespace fw::ampr {

namespace {

void registerSettings(SettingsSchema& s)
{
    const QString section = QStringLiteral("Build");
    const QString group = QStringLiteral("zstd packs");
    int order = 500;
    {
        SettingDef d;
        d.key = keys::kZstdPack;
        d.section = section;
        d.group = group;
        d.label = QStringLiteral("Compress game files (zstd packs)");
        d.description = QStringLiteral(
            "A smaller image: the game's files go into zstd packs that ampr-emu-ng reads on the console. The bundled "
            "runtime goes to fakelib/libSceAmpr.sprx; mount with ShadowMount+ with \"Update game fakelib emulators\" turned "
            "off. Not "
            "every game works packed yet: keep your original dump.");
        d.type = SettingType::Bool;
        d.defaultValue = false;
        d.perJob = true;
        d.dependencyId = QStringLiteral("ampr.runtime");
        d.order = order++;
        s.add(d);
    }
    {
        SettingDef d;
        d.key = keys::kZstdLevel;
        d.section = section;
        d.group = group;
        d.label = QStringLiteral("zstd level");
        d.description = QStringLiteral("Higher levels make smaller packs but build much more slowly. The console reads "
                                       "every level at the same speed.");
        d.type = SettingType::Enum;
        d.defaultValue = QStringLiteral("5");
        d.choices = {{QStringLiteral("3"), QStringLiteral("3 - fastest"), {}},
                     {QStringLiteral("5"), QStringLiteral("5 - fast (default)"), {}},
                     {QStringLiteral("9"), QStringLiteral("9 - medium"), {}},
                     {QStringLiteral("15"), QStringLiteral("15 - small, slow"), {}},
                     {QStringLiteral("19"), QStringLiteral("19 - smallest, very slow"), {}}};
        d.perJob = true;
        d.advanced = true;
        d.enabledWhenKey = keys::kZstdPack;
        d.order = order++;
        s.add(d);
    }
    {
        SettingDef d;
        d.key = keys::kPackStrategy;
        d.section = section;
        d.group = group;
        d.label = QStringLiteral("What to pack");
        d.description = QStringLiteral(
            "Everything: every file except executables, modules, sce_sys, sce_module and fakelib (the smallest; "
            "ampr-emu-ng serves a fully packed game). Compatible: also keeps root files, containers (.pak, .utoc), "
            "configs, media and video folders loose, for games that do not start fully packed.");
        d.type = SettingType::Enum;
        d.defaultValue = QStringLiteral("everything");
        d.choices = {{QStringLiteral("everything"), QStringLiteral("Everything except system files"), {}},
                     {QStringLiteral("compatible"), QStringLiteral("Compatible - keep containers, configs and media loose"), {}}};
        d.perJob = true;
        d.advanced = true;
        d.enabledWhenKey = keys::kZstdPack;
        d.order = order++;
        s.add(d);
    }
    {
        SettingDef d;
        d.key = keys::kVerifyPacks;
        d.section = section;
        d.group = group;
        d.label = QStringLiteral("Check the packs after packing");
        d.description = QStringLiteral("Reads every pack volume back and checks each chunk (every chunk is also decoded "
                                       "again while it is packed).");
        d.type = SettingType::Bool;
        d.defaultValue = true;
        d.perJob = true;
        d.advanced = true;
        d.enabledWhenKey = keys::kZstdPack;
        d.order = order++;
        s.add(d);
    }
}

DependencyInfo probeRuntime(const SettingsValues&)
{
    DependencyInfo info;
    info.name = QStringLiteral("ampr-emu-ng runtime (zstd packs)");
    info.purpose = QStringLiteral("Serves zstd-packed games on the console (bundled, GPL-3.0, quer3q/ampr-emu-ng).");
    info.path = bundledRuntimePath();
    const QString problem = bundledRuntimeProblem();
    if (!problem.isEmpty()) {
        info.status = DependencyStatus::Missing;
        info.detail = problem;
        info.howToFix = QStringLiteral("Reinstall the app; the runtime ships with it.");
        return info;
    }
    info.status = DependencyStatus::Ready;
    info.version = QString::fromLatin1(kBundledRuntimeVersion);
    return info;
}

QString native(const QString& p) { return QDir::toNativeSeparators(p); }

class FolderJob : public Job {
public:
    explicit FolderJob(QJsonObject p) : m_p(std::move(p)) {}
    QJsonObject parameters() const override { return m_p; }
    QString title() const override { return isZip() ? archive::sourceName(source()) : QFileInfo(source()).fileName(); }
    QString subtitle() const override { return QStringLiteral("→ %1").arg(native(output())); }
    QStringList busyPaths() const override
    {
        return {QDir::cleanPath(QFileInfo(source()).absoluteFilePath()), QDir::cleanPath(QFileInfo(output()).absoluteFilePath())};
    }
    // Pack also takes a zipped dump (#67), unpacked to the job's temporary folder first.
    virtual bool acceptsZip() const { return false; }
    bool isZip() const { return acceptsZip() && archive::isZipSource(source()); }
    Status validate(const SettingsValues&) const override
    {
        if (!QFileInfo(source()).isDir() && !isZip())
            return Error(ErrorCode::InvalidArgument, acceptsZip() ? QStringLiteral("Choose the game folder or an archived dump.")
                                                                  : QStringLiteral("Choose the game folder."));
        if (isZip())
            FW_TRY(archive::checkZipSource(source()));
        if (output().isEmpty())
            return Error(ErrorCode::InvalidArgument, QStringLiteral("Choose where to save the new folder."));
        const QString s = QFileInfo(source()).absoluteFilePath(), o = QFileInfo(output()).absoluteFilePath();
        if (isSameOrInside(o, s) || isSameOrInside(s, o))
            return Error(ErrorCode::InvalidArgument, QStringLiteral("The new folder cannot be inside the game folder (or the other way round)."));
        return checkNewFolderOutput(o);
    }

protected:
    QString source() const { return m_p.value(QStringLiteral("source")).toString(); }
    QString output() const { return m_p.value(QStringLiteral("output")).toString(); }
    QJsonObject m_p;
};

class PackJob final : public FolderJob {
public:
    using FolderJob::FolderJob;
    QString kind() const override { return kPackJob; }
    bool acceptsZip() const override { return true; }
    QString subtitle() const override { return QStringLiteral("zstd packs → %1").arg(native(output())); }
    QStringList phases() const override { return {QStringLiteral("Preparing"), QStringLiteral("Packing"), QStringLiteral("Checking packs")}; }
    Status validate(const SettingsValues& s) const override
    {
        FW_TRY(FolderJob::validate(s));
        const QString problem = bundledRuntimeProblem();
        if (!problem.isEmpty())
            return Error(ErrorCode::DependencyMissing, problem);
        return success();
    }
    Result<JobOutcome> run(JobContext& ctx) override
    {
        FW_TRY(validate(ctx.settings));
        const PackSettings ps = readPackSettings(ctx.settings, m_p);
        ctx.progress->beginPhase(0, 0);
        QString src = QFileInfo(source()).absoluteFilePath();
        if (isZip()) {
            FW_TRY_ASSIGN(src, archive::unpackZipSource(src, ctx.tempDir, ctx.token, ctx.progress, general::freeSpaceMarginBytes(ctx.settings)));
            ctx.log.info(QStringLiteral("Unpacked %1 to %2").arg(native(source()), native(src)));
            // Unpacking reports as a phase of its own, which replaced the job's.
            ctx.progress->setPhases(phases(), phaseWeights());
            ctx.progress->beginPhase(0, 0);
        }
        FW_TRY_ASSIGN(auto files, openFolderGame(src, ctx.token));
        PackGameOptions o;
        o.level = ps.level;
        o.threads = ps.threads;
        o.strategy = ps.strategy;
        o.verify = ps.verify;
        // No linkFromFolder: the packed folder is kept, so its loose files are
        // copies and share nothing with the source dump.
        o.freeSpaceMarginBytes = general::freeSpaceMarginBytes(ctx.settings);
        o.packPhase = 1;
        o.verifyPhase = 2;
        const QString out = QFileInfo(output()).absoluteFilePath();
        // Written as "<output>.part" and renamed when everything is done: a
        // failure or a cancel leaves nothing behind and touches nothing the
        // job did not create.
        FW_TRY_ASSIGN(auto staged, StagedFolder::begin(out));
        ctx.log.info(QStringLiteral("Packing %1 with zstd level %2 (%3) into %4")
                         .arg(native(src))
                         .arg(ps.level)
                         .arg(ps.strategy == PackStrategy::Compatible ? QStringLiteral("compatible: containers, configs and media stay loose")
                                                                      : QStringLiteral("everything except system files"),
                              native(out)));
        auto r = packGame(files, staged->path(), o, ctx.token, ctx.progress, ctx.log);
        if (!r.ok())
            return r.takeError();
        FW_TRY(staged->commit());
        for (const QString& l : r->looseSummary)
            ctx.log.info(QStringLiteral("Kept loose: %1").arg(l));
        if (!r->droppedFiles.isEmpty())
            ctx.log.info(QStringLiteral("Left out (old AMPR journal, logs or packs): %1").arg(r->droppedFiles.join(QStringLiteral(", "))));
        ctx.log.info(QStringLiteral("Packed: %1. Runtime: %2%3.")
                         .arg(r->summary(), r->runtime,
                              r->previousRuntime.kind == RuntimeKind::None || r->previousRuntime.bundled
                                  ? QString()
                                  : QStringLiteral(" (replaced %1)").arg(r->previousRuntime.describe())));
        JobOutcome outcome;
        outcome.outputs = {out};
        outcome.summary = QStringLiteral("zstd: %1").arg(r->summary());
        outcome.warnings = r->warnings;
        outcome.warnings << QStringLiteral("Packed games need ShadowMount+ with \"Update game fakelib emulators\" turned off, or the "
                                           "runtime is replaced and the game cannot read its packs.");
        outcome.verification = ps.verify ? Verification::Passed : Verification::NotRun;
        outcome.verificationDetail = ps.verify ? QStringLiteral("every pack chunk read back and matched") : QString();
        outcome.details.insert(QStringLiteral("packedFiles"), r->packedFiles);
        outcome.details.insert(QStringLiteral("storedBytes"), r->storedBytes);
        outcome.details.insert(QStringLiteral("zstd"),
                               QJsonObject{{QStringLiteral("level"), r->level},
                                           {QStringLiteral("strategy"), packStrategyName(ps.strategy)},
                                           {QStringLiteral("packedFiles"), r->packedFiles},
                                           {QStringLiteral("packedBytes"), r->packedBytes},
                                           {QStringLiteral("storedBytes"), r->storedBytes},
                                           {QStringLiteral("looseFiles"), r->looseFiles},
                                           {QStringLiteral("volumes"), r->volumes},
                                           {QStringLiteral("runtime"), r->runtime},
                                           {QStringLiteral("previousRuntime"), r->previousRuntime.describe()}});
        return outcome;
    }
};

class UnpackJob final : public FolderJob {
public:
    using FolderJob::FolderJob;
    QString kind() const override { return kUnpackJob; }
    QStringList phases() const override { return {QStringLiteral("Reading packs"), QStringLiteral("Unpacking")}; }
    Result<JobOutcome> run(JobContext& ctx) override
    {
        FW_TRY(validate(ctx.settings));
        ctx.progress->beginPhase(0, 0);
        FW_TRY_ASSIGN(auto files, openFolderGame(source(), ctx.token));
        if (detectPackKind(*files) == PackKind::None)
            return Error(ErrorCode::InvalidArgument, QStringLiteral("%1 has no zstd or LZ4 packs.").arg(QFileInfo(source()).fileName()));
        ctx.progress->beginPhase(1, 0);
        const QString out = QFileInfo(output()).absoluteFilePath();
        // Written as "<output>.part" and renamed at the end (see PackJob).
        FW_TRY_ASSIGN(auto staged, StagedFolder::begin(out));
        UnpackOptions uo;
        uo.freeSpaceMarginBytes = general::freeSpaceMarginBytes(ctx.settings);
        ctx.log.info(QStringLiteral("Unpacking the %1 packs of %2 into %3")
                         .arg(packKindName(detectPackKind(*files)), native(source()), native(out)));
        auto r = unpackGame(files, staged->path(), uo, ctx.token, ctx.progress, ctx.log);
        if (!r.ok())
            return r.takeError();
        FW_TRY(staged->commit());
        if (!r->droppedFiles.isEmpty())
            ctx.log.info(QStringLiteral("Left out (packs, old AMPR journal and logs): %1").arg(r->droppedFiles.join(QStringLiteral(", "))));
        JobOutcome outcome;
        outcome.outputs = {out};
        outcome.warnings = r->warnings + renamedWarnings(r->renamed);
        outcome.summary = QStringLiteral("Unpacked %1 file%2 (%3) from %4 packs.")
                              .arg(formatCount(r->unpackedFiles), r->unpackedFiles == 1 ? QString() : QStringLiteral("s"),
                                   formatBytes(r->bytes), packKindName(r->kind));
        ctx.log.info(outcome.summary);
        outcome.details.insert(QStringLiteral("files"), r->files);
        outcome.details.insert(QStringLiteral("bytes"), r->bytes);
        outcome.details.insert(QStringLiteral("unpackedFiles"), r->unpackedFiles);
        outcome.details.insert(QStringLiteral("amprIndexWritten"), r->amprIndexWritten);
        return outcome;
    }
};

} // namespace

PackSettings readPackSettings(const SettingsValues& s, const QJsonObject& params)
{
    PackSettings p;
    p.enabled = s.boolean(keys::kZstdPack);
    p.level = std::clamp(int(s.string(keys::kZstdLevel).toInt()), kZstdMinLevel, kZstdMaxLevel);
    if (params.contains(QStringLiteral("level")))
        p.level = std::clamp(params.value(QStringLiteral("level")).toInt(p.level), kZstdMinLevel, kZstdMaxLevel);
    p.strategy = packStrategyFromName(params.value(QStringLiteral("strategy")).toString(s.string(keys::kPackStrategy)));
    p.verify = params.value(QStringLiteral("verifyPacks")).toBool(s.boolean(keys::kVerifyPacks));
    p.threads = general::workerThreads(s);
    return p;
}

void registerModule(ModuleContext& ctx)
{
    registerSettings(ctx.settings);
    ctx.dependencies.add(QStringLiteral("ampr.runtime"), QStringLiteral("ampr-emu-ng runtime (zstd packs)"),
                         QStringLiteral("Serves zstd-packed games on the console (bundled, GPL-3.0, quer3q/ampr-emu-ng)."), probeRuntime);
    ctx.jobs.add(kPackJob, QStringLiteral("Pack a game (zstd)"),
                 [](const QJsonObject& p) -> Result<std::unique_ptr<Job>> { return std::unique_ptr<Job>(new PackJob(p)); });
    ctx.jobs.add(kUnpackJob, QStringLiteral("Unpack a packed game"),
                 [](const QJsonObject& p) -> Result<std::unique_ptr<Job>> { return std::unique_ptr<Job>(new UnpackJob(p)); });
}

} // namespace fw::ampr
