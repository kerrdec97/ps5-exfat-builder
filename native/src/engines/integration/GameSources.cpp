#include "engines/integration/GameSources.h"

#include "core/ArchiveSource.h"
#include "core/FileUtil.h"
#include "core/PathSafety.h"
#include "engines/ampr/GamePack.h"
#include "engines/ampr/HostPaths.h"
#include "engines/ampr/Lz4Pack.h"
#include "engines/ampr/Module.h"
#include "engines/exfat/ExfatJobs.h"
#include "engines/exfat/Module.h"
#include "engines/exfat/PlatformFs.h"
#include "engines/exfat/SourceScan.h"
#include "engines/fpkg/BackportUpdate.h"
#include "engines/fpkg/DumpRules.h"
#include "engines/fpkg/SourceStager.h"
#include "engines/general/Module.h"
#include "engines/pfs/Module.h"
#include "engines/ufs2/Module.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QSet>
#include <QTimeZone>

#include <cstring>
#include <mutex>

namespace fw::integration {

namespace {

QString native(const QString& p) { return QDir::toNativeSeparators(p); }

class TreeGame final : public ampr::GameFiles {
public:
    explicit TreeGame(std::unique_ptr<FileTree> t) : m_tree(std::move(t))
    {
        m_items.reserve(m_tree->items().size());
        for (const TreeItem& it : m_tree->items()) {
            ampr::GameItem g;
            g.path = it.path;
            g.isDir = it.isDir;
            g.size = it.size;
            m_items.push_back(g);
        }
    }
    QString describe() const override { return m_tree->describe(); }
    const std::vector<ampr::GameItem>& items() const override { return m_items; }
    int find(const QString& path) const override { return m_tree->find(path); }
    Result<qint64> read(int index, qint64 offset, char* dst, qint64 len) override { return m_tree->read(index, offset, dst, len); }
    QStringList warnings() const override { return m_tree->warnings(); }

private:
    std::unique_ptr<FileTree> m_tree;
    std::vector<ampr::GameItem> m_items;
};

// Forwards a child job's progress into the parent's phases offset by `offset`.
// The parent's phase list (`parentPhases`) holds the child's phases after its
// own; a child's step named like one of them is not shown as a step.
class ChildProgress {
public:
    ChildProgress(ProgressReporter* parent, int offset, QStringList parentPhases)
        : m_parent(parent), m_offset(offset), m_parentPhases(std::move(parentPhases)),
          m_reporter([this](const ProgressSnapshot& s) { forward(s); }, 100)
    {
    }
    ProgressReporter* reporter() { return &m_reporter; }

private:
    // The child's reporter reports every phase and step it enters at once
    // (with nothing done yet), so the parent enters each one in turn: a
    // phase the child enters again, or a step inside one, goes on from where
    // the parent's stage stood instead of finding it full.
    void forward(const ProgressSnapshot& s)
    {
        std::lock_guard lock(m_mutex);
        // The child's reports can arrive out of order from its worker
        // threads: one taken before the last forwarded is overtaken (a lower
        // count in it is no phase entered again).
        if (s.sequence != 0 && s.sequence <= m_sequence)
            return;
        m_sequence = s.sequence;
        if (s.phaseNames != m_names) { // the child declared other phases: it starts over
            m_names = s.phaseNames;
            m_phase = -1;
            m_step.clear();
            m_total = -1;
            m_done = 0;
        } else if (s.phaseIndex < m_phase) {
            return; // a report that was overtaken by the next phase's
        }
        // A step (beginStep) shows under its own name inside the child's phase.
        const QString step = s.phase != s.phaseNames.value(s.phaseIndex) && !m_parentPhases.contains(s.phase) ? s.phase : QString();
        const bool entered = s.phaseIndex != m_phase || step != m_step;
        const bool again = !entered && s.done < m_done; // the child's phase entered again
        if (entered && !step.isEmpty()) {
            if (s.phaseIndex != m_phase)
                m_parent->beginPhase(m_offset + s.phaseIndex, 0, 0, s.unitIsBytes);
            m_parent->beginStep(step, s.total, s.filesTotal, s.unitIsBytes);
            m_files = 0;
        } else if (entered || again || (s.total != m_total && s.done == 0)) {
            m_parent->beginPhase(m_offset + s.phaseIndex, s.total, s.filesTotal, s.unitIsBytes);
            m_files = 0;
        } else if (s.total != m_total) {
            m_parent->setTotal(s.total);
        }
        m_phase = s.phaseIndex;
        m_step = step;
        m_total = s.total;
        m_done = s.done;
        m_parent->setDone(s.done);
        // The files done, as the parent counts them: what was added since.
        if (s.filesDone > m_files) {
            m_parent->advanceFiles(s.filesDone - m_files);
            m_files = s.filesDone;
        }
        if (!s.currentItem.isEmpty())
            m_parent->setCurrentItem(s.currentItem);
    }

    ProgressReporter* m_parent;
    int m_offset;
    QStringList m_parentPhases;
    std::mutex m_mutex;
    QStringList m_names; // the child's phases
    int m_phase = -1;
    QString m_step;
    qint64 m_total = -1;
    qint64 m_done = 0;
    qint64 m_files = 0;   // files done forwarded in the current phase or step
    quint64 m_sequence = 0;
    ProgressReporter m_reporter;
};

QString randomHex(int n)
{
    return QString::number(QRandomGenerator::global()->generate64(), 16).rightJustified(16, u'0').left(n);
}

// ---- image.build.zstd -------------------------------------------------------------------

QString childKindFor(const QString& format)
{
    if (format == u"exfat")
        return QStringLiteral("image.exfat.build");
    if (format == u"ffpkg")
        return QStringLiteral("image.ffpkg.build");
    if (format == u"ffpfs" || format == u"ffpfsc")
        return QStringLiteral("image.pfs.build");
    return {};
}

class ZstdBuildJob final : public Job {
public:
    ZstdBuildJob(QJsonObject p, const JobFactory* factory) : m_p(std::move(p)), m_factory(factory) {}
    QString kind() const override { return kZstdBuildJob; }
    QString title() const override
    {
        // Like the image builds: their "title" when given, else the folder's
        // whole name ("Astro Bot v1.020") or the archive's name without its suffix.
        const QString t = m_p.value(QStringLiteral("title")).toString();
        if (!t.isEmpty())
            return t;
        return archive::sourceName(source());
    }
    QString subtitle() const override
    {
        return QStringLiteral("zstd packs · .%1 → %2").arg(format(), native(QFileInfo(output()).absolutePath()));
    }
    QStringList phases() const override
    {
        QStringList p = {QStringLiteral("Preparing"), QStringLiteral("Packing"), QStringLiteral("Checking packs")};
        if (auto child = makeChild(source()); child.ok())
            p += (*child)->phases();
        return p;
    }
    QJsonObject parameters() const override { return m_p; }
    QStringList busyPaths() const override
    {
        return {QDir::cleanPath(QFileInfo(source()).absoluteFilePath()), QDir::cleanPath(QFileInfo(output()).absoluteFilePath())};
    }
    Status basicChecks() const
    {
        if (childKindFor(format()).isEmpty())
            return Error(ErrorCode::InvalidArgument, QStringLiteral("zstd packs go into .exfat, .ffpkg, .ffpfs and .ffpfsc images."));
        if (!QFileInfo(source()).isDir() && !archive::isZipSource(source()))
            return Error(ErrorCode::InvalidArgument, QStringLiteral("Choose the game folder (or an archived dump) to pack."));
        if (output().isEmpty())
            return Error(ErrorCode::InvalidArgument, QStringLiteral("Choose where to save the image."));
        if (!m_factory || !m_factory->contains(childKindFor(format())))
            return Error(ErrorCode::Unsupported, QStringLiteral("This image format is not available in this build."));
        const QString problem = ampr::bundledRuntimeProblem();
        if (!problem.isEmpty())
            return Error(ErrorCode::DependencyMissing, problem);
        return success();
    }
    Status validate(const SettingsValues& s) const override
    {
        FW_TRY(basicChecks());
        // The work folder goes next to the output: never inside the game.
        const QString src = QFileInfo(source()).absoluteFilePath();
        if (QFileInfo(source()).isDir() && isSameOrInside(QFileInfo(output()).absolutePath(), src))
            return Error(ErrorCode::InvalidArgument, QStringLiteral("Save the image outside the game folder."));
        // Everything the image build itself would refuse, before hours of packing.
        FW_TRY_ASSIGN(auto child, makeChild(source()));
        return child->validate(childSettings(s));
    }

    Result<JobOutcome> run(JobContext& ctx) override
    {
        FW_TRY(validate(ctx.settings));
        const QStringList ph = phases();
        ctx.progress->setPhases(ph, phaseWeights(ctx.settings));
        ctx.progress->beginPhase(0, 0);

        QString src = QFileInfo(source()).absoluteFilePath();
        if (archive::isZipSource(src)) {
            FW_TRY_ASSIGN(src, archive::unpackZipSource(src, ctx.tempDir, ctx.token, ctx.progress, general::freeSpaceMarginBytes(ctx.settings)));
            ctx.log.info(QStringLiteral("Unpacked %1 to %2").arg(native(source()), native(src)));
            // The unpacking was a step inside Preparing; Preparing goes on from there.
            ctx.progress->beginPhase(0, 0);
        }
        const QString outDir = QFileInfo(output()).absolutePath();
        if (!QDir().mkpath(outDir))
            return Error(ErrorCode::IoError, QStringLiteral("Could not create %1.").arg(native(outDir)));
        (void)fpkg::sweepStaleBackportWork();
        const QString work = QDir(outDir).filePath(QStringLiteral(".fw-zstd-%1").arg(randomHex(8)));
        FW_TRY(fpkg::createTrackedWorkFolder(work));
        fpkg::TrackedWorkFolder guard(work);
        QString gameName = QFileInfo(src).fileName();
        if (gameName.isEmpty())
            gameName = QStringLiteral("game"); // a game at the root of a drive
        const QString staged = QDir(work).filePath(gameName);

        const ampr::PackSettings ps = ampr::readPackSettings(ctx.settings, m_p);
        FW_TRY_ASSIGN(auto files, ampr::openFolderGame(src, ctx.token));
        ampr::PackGameOptions o;
        o.level = ps.level;
        o.threads = ps.threads;
        o.strategy = ps.strategy;
        o.verify = ps.verify;
        o.linkFromFolder = src;
        o.packPhase = 1;
        o.verifyPhase = ps.verify ? 2 : -1;
        o.freeSpaceMarginBytes = qint64(ctx.settings.integer(general::keys::kFreeSpaceMarginMiB)) * 1024 * 1024;
        o.exclude = excluder(ctx.settings, src);
        ctx.log.info(QStringLiteral("Packing %1 with zstd level %2 (%3) for .%4; work folder %5")
                         .arg(native(src))
                         .arg(ps.level)
                         .arg(ps.strategy == ampr::PackStrategy::Compatible ? QStringLiteral("compatible: containers, configs and media stay loose")
                                                                            : QStringLiteral("everything except system files"),
                              format(), native(work)));
        FW_TRY_ASSIGN(ampr::PackGameReport rep, ampr::packGame(files, staged, o, ctx.token, ctx.progress, ctx.log));
        // A cancel while the packs are checked stops the job: the image, the
        // job's real work, is not made yet.
        if (ctx.token.isCancelled())
            return Error::cancelled();
        for (const QString& l : rep.looseSummary)
            ctx.log.info(QStringLiteral("Kept loose: %1").arg(l));
        if (!rep.droppedFiles.isEmpty())
            ctx.log.info(QStringLiteral("Left out (old AMPR journal, logs or packs): %1").arg(rep.droppedFiles.join(QStringLiteral(", "))));
        ctx.log.info(QStringLiteral("Packed: %1. Runtime: %2%3.")
                         .arg(rep.summary(), rep.runtime,
                              rep.previousRuntime.kind == ampr::RuntimeKind::None || rep.previousRuntime.bundled
                                  ? QString()
                                  : QStringLiteral(" (replaced %1)").arg(rep.previousRuntime.describe())));

        // The image build, from the packed folder.
        FW_TRY_ASSIGN(auto child, makeChild(staged));
        JobContext sub = ctx;
        // The packed folder must go in as it is: no ampr_emu.index rebuilt from
        // its files (that would drop the packed files), no per-file PFS
        // compression (the volumes keep their exact sizes), and nothing deleted.
        sub.settings = childSettings(ctx.settings);
        ChildProgress cp(ctx.progress, 3, ph);
        // The child's own phase list, so each of its phases gets its index
        // (a build that never declares them would stay on its first).
        cp.reporter()->setPhases(child->phases(), child->phaseWeights(sub.settings));
        sub.progress = cp.reporter();
        FW_TRY(child->validate(sub.settings));
        auto built = child->run(sub);
        cp.reporter()->flush(); // its last progress, which the throttle may have held back
        FW_TRY_ASSIGN(JobOutcome out, std::move(built));
        out.warnings = rep.warnings + out.warnings;
        out.details.insert(QStringLiteral("zstd"),
                           QJsonObject{{QStringLiteral("level"), rep.level},
                                       {QStringLiteral("strategy"), ampr::packStrategyName(ps.strategy)},
                                       {QStringLiteral("packedFiles"), rep.packedFiles},
                                       {QStringLiteral("packedBytes"), rep.packedBytes},
                                       {QStringLiteral("storedBytes"), rep.storedBytes},
                                       {QStringLiteral("looseFiles"), rep.looseFiles},
                                       {QStringLiteral("volumes"), rep.volumes},
                                       {QStringLiteral("runtime"), rep.runtime},
                                       {QStringLiteral("previousRuntime"), rep.previousRuntime.describe()}});
        if (out.verification != Verification::Failed)
            out.summary = QStringLiteral("zstd: %1. %2").arg(rep.summary(), out.summary);
        out.warnings << QStringLiteral("Packed games need ShadowMount+ with \"Update game fakelib emulators\" turned off, or the "
                                       "runtime is replaced and the game cannot read its packs.");
        return out;
    }

    // Preparing, Packing and Checking packs, then the image build's phases,
    // which share the rest as the build itself shares its bar (it builds
    // from the packed folder, never from an archive).
    QList<double> phaseWeights() const override
    {
        QList<double> child;
        if (auto c = makeChild(QFileInfo(output()).absolutePath()); c.ok())
            child = (*c)->phaseWeights();
        return weights(child, true);
    }
    // For the settings: the build's shares for the settings it gets (no
    // Verifying when verification is off), no Checking packs when the packs
    // are not read back.
    QList<double> phaseWeights(const SettingsValues& settings) const override
    {
        QList<double> child;
        if (auto c = makeChild(QFileInfo(output()).absolutePath()); c.ok())
            child = (*c)->phaseWeights(childSettings(settings));
        return weights(child, ampr::readPackSettings(settings, m_p).verify);
    }

private:
    // An archived dump is unpacked in Preparing first: that takes a share
    // like the image builds give it (they give a ZIP source's first stage
    // 0.3), the rest is scaled down.
    QList<double> weights(const QList<double>& child, bool checkPacks) const
    {
        const double prepare = archive::isZipSource(source()) ? 0.3 : 0.02;
        const double scale = (1.0 - prepare) / 0.98;
        QList<double> w = checkPacks ? QList<double>{prepare, 0.5 * scale, 0.06 * scale} : QList<double>{prepare, 0.56 * scale, 0.0};
        const double build = 0.42 * scale;
        const qsizetype n = std::max<qsizetype>(0, phases().size() - 3);
        double sum = 0.0;
        for (qsizetype i = 0; i < n; ++i)
            sum += std::max(0.0, child.value(i, 1.0));
        for (qsizetype i = 0; i < n; ++i)
            w << (sum > 0.0 ? build * std::max(0.0, child.value(i, 1.0)) / sum : build / double(n));
        return w;
    }

    QString source() const { return m_p.value(QStringLiteral("source")).toString(); }
    QString output() const { return m_p.value(QStringLiteral("output")).toString(); }
    QString format() const { return m_p.value(QStringLiteral("format")).toString().toLower(); }

    // The packed folder must go in as it is: no ampr_emu.index rebuilt from its
    // files (that would drop the packed files), no per-file PFS compression
    // (the volumes keep their exact sizes), no exclusions (they were applied
    // before packing; a pattern could now hit the packs) and nothing deleted.
    static SettingsValues childSettings(const SettingsValues& s)
    {
        return s.withOverrides({{pfs::keys::kAmprIndex, QStringLiteral("off")},
                                {exfat::keys::kAmprIndex, QStringLiteral("off")},
                                {ufs2::keys::kAmprIndex, QStringLiteral("off")},
                                {pfs::keys::kCompressFiles, false},
                                {pfs::keys::kExcludes, QStringList()},
                                {pfs::keys::kDeleteSourceAfterConvert, false},
                                {exfat::keys::kExcludePatterns, QStringList()},
                                {exfat::keys::kExcludeHidden, false},
                                {exfat::keys::kDeleteSourceAfterVerify, false},
                                {ufs2::keys::kDeleteSourceAfterBuild, false}});
    }

    Result<std::unique_ptr<Job>> makeChild(const QString& from) const
    {
        if (!m_factory)
            return Error(ErrorCode::Internal, QStringLiteral("No job factory."));
        QJsonObject p = m_p;
        for (const char* k : {"level", "strategy", "verifyPacks", "excludes"})
            p.remove(QLatin1String(k));
        p.insert(QStringLiteral("source"), from);
        if (format() == u"exfat" || format() == u"ffpkg")
            p.remove(QStringLiteral("format"));
        return m_factory->create(childKindFor(format()), p);
    }

    // The build's own exclusions, applied before packing (a packed file can
    // no longer be left out by the image build).
    std::function<bool(const QString&, bool)> excluder(const SettingsValues& s, const QString& root) const
    {
        exfat::ScanRules rules;
        if (format() == u"exfat") {
            rules = exfat::readSettings(s).rules;
            for (const QJsonValue& v : m_p.value(QStringLiteral("excludes")).toArray())
                rules.excludePatterns << v.toString();
        } else if (format() == u"ffpfs" || format() == u"ffpfsc") {
            // As the PFS scan reads them: "./" and a trailing "/" dropped, and
            // each pattern matches files and folders alike.
            for (const QString& pat : pfs::normalizeExcludes(s.stringList(pfs::keys::kExcludes)))
                rules.excludePatterns << pat << pat + u'/';
        }
        if (rules.excludePatterns.isEmpty() && !rules.excludeHidden)
            return {};
        auto matcher = std::make_shared<exfat::ExcludeMatcher>(rules);
        const bool hiddenRule = rules.excludeHidden;
        return [matcher, hiddenRule, root](const QString& rel, bool isDir) {
            const QString name = rel.section(u'/', -1);
            if (isDir)
                return matcher->excludesDir(name, rel);
            const bool hidden = hiddenRule && QFileInfo(QDir(root).filePath(rel)).isHidden();
            return matcher->excludesFile(name, rel, hidden);
        };
    }

    QJsonObject m_p;
    const JobFactory* m_factory;
};

// ---- game.extract -----------------------------------------------------------------------

class GameExtractJob final : public Job {
public:
    explicit GameExtractJob(QJsonObject p) : m_p(std::move(p)) {}
    QString kind() const override { return kGameExtractJob; }
    QString title() const override { return QFileInfo(source()).fileName(); }
    QString subtitle() const override { return QStringLiteral("Extract → %1").arg(native(output())); }
    QStringList phases() const override { return {QStringLiteral("Reading"), QStringLiteral("Extracting"), QStringLiteral("Verifying")}; }
    QJsonObject parameters() const override { return m_p; }
    QStringList busyPaths() const override
    {
        return {QDir::cleanPath(QFileInfo(source()).absoluteFilePath()), QDir::cleanPath(QFileInfo(output()).absoluteFilePath())};
    }
    Status validate(const SettingsValues&) const override
    {
        if (!QFileInfo::exists(source()))
            return Error(ErrorCode::NotFound, QStringLiteral("Choose the image or game folder to extract."));
        if (!QFileInfo(source()).isDir() && !isTreeImage(source()))
            return Error(ErrorCode::Unsupported, QStringLiteral("Choose an .exfat, .ffpkg, .ffpfs or .ffpfsc image or a game folder."));
        if (output().isEmpty())
            return Error(ErrorCode::InvalidArgument, QStringLiteral("Choose where to extract to."));
        const QString o = QFileInfo(output()).absoluteFilePath();
        if (QFileInfo(source()).isDir() && (isSameOrInside(o, QFileInfo(source()).absoluteFilePath())))
            return Error(ErrorCode::InvalidArgument, QStringLiteral("The output cannot be inside the game folder."));
        if (QFileInfo(o).exists() && !QFileInfo(o).isDir())
            return Error(ErrorCode::AlreadyExists, QStringLiteral("%1 exists and is not a folder.").arg(native(o)));
        if (QFileInfo(o).exists() && !QDir(o).isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System))
            return Error(ErrorCode::AlreadyExists, QStringLiteral("%1 is not empty. Choose an empty or new folder.").arg(native(o)));
        return success();
    }
    Result<JobOutcome> run(JobContext& ctx) override
    {
        FW_TRY(validate(ctx.settings));
        const bool verify = ctx.settings.boolean(general::keys::kVerifyAfterBuild);
        ctx.progress->setPhases(phases(), verify ? QList<double>{0.03, 0.6, 0.37} : QList<double>{0.05, 0.95, 0.0});
        ctx.progress->beginPhase(0, 0);
        FW_TRY_ASSIGN(auto files, openGameFiles(source(), ctx.token));
        const ampr::PackKind kind = ampr::detectPackKind(*files);
        const bool unpack = m_p.value(QStringLiteral("unpackPacks")).toBool(true);
        ctx.log.info(QStringLiteral("Extracting %1 (%2)%3")
                         .arg(QFileInfo(source()).fileName(), files->describe(),
                              kind == ampr::PackKind::None ? QString()
                              : unpack ? QStringLiteral(", unpacking its %1 packs").arg(ampr::packKindName(kind))
                                       : QStringLiteral(", keeping its %1 packs").arg(ampr::packKindName(kind))));
        ctx.progress->beginPhase(1, 0);
        const QString out = QFileInfo(output()).absoluteFilePath();
        // Everything goes to "<output>.part" first and is renamed once it has
        // verified, so an unverified or cancelled extraction never sits under
        // the final name. The marker next to it says a leftover is ours.
        const QString stage = out + QStringLiteral(".part");
        const QString marker = out + QStringLiteral(".part.extracting");
        const QString parent = QFileInfo(out).absolutePath();
        if (QFileInfo::exists(stage)) {
            if (!QFileInfo::exists(marker))
                return Error(ErrorCode::AlreadyExists, QStringLiteral("%1 already exists. Move or delete it, then try again.").arg(native(stage)));
            FW_TRY(removeTreeSafely(stage, parent));
        }
        if (!QDir().mkpath(parent) || !QFile(marker).open(QIODevice::WriteOnly))
            return Error(ErrorCode::IoError, QStringLiteral("Could not create the folder %1.").arg(native(parent)));
        struct StageGuard {
            QString stage, marker, parent;
            bool keep = false;
            ~StageGuard()
            {
                if (!keep)
                    (void)removeTreeSafely(stage, parent);
                QFile::remove(marker);
            }
        } guard{stage, marker, parent};
        ampr::UnpackOptions uo;
        uo.freeSpaceMarginBytes = qint64(ctx.settings.integer(general::keys::kFreeSpaceMarginMiB)) * 1024 * 1024;
        std::shared_ptr<ampr::GameFiles> from = files;
        if (!unpack && kind != ampr::PackKind::None)
            from = std::make_shared<PlainView>(files); // the packs as files
        auto r = ampr::unpackGame(from, stage, uo, ctx.token, ctx.progress, ctx.log);
        if (!r.ok())
            return r.takeError();
        JobOutcome o;
        if (verify) {
            // Every written file read back against the source (through the packs).
            ctx.progress->beginPhase(2, r->bytes);
            auto compared = [&]() -> Result<qint64> {
                FW_TRY_ASSIGN(auto again, openGameFiles(source(), ctx.token));
                std::shared_ptr<ampr::GameFiles> src = again;
                if (unpack || kind == ampr::PackKind::None) {
                    FW_TRY_ASSIGN(src, ampr::logicalGame(again, ctx.token));
                } else {
                    src = std::make_shared<PlainView>(again);
                }
                std::vector<char> a(size_t(4) << 20), b(size_t(4) << 20);
                qint64 files = 0;
                for (int i = 0; i < int(src->items().size()); ++i) {
                    const ampr::GameItem& it = src->items()[size_t(i)];
                    if (it.isDir)
                        continue;
                    if (it.path.compare(QStringLiteral("ampr_emu.index"), Qt::CaseInsensitive) == 0 && r->amprIndexWritten)
                        continue; // rewritten for the unpacked files
                    // Where the extraction put it (never a path outside the stage).
                    auto written = childPathInside(stage, r->hostPath(it.path));
                    QFile f(written.ok() ? *written : QString());
                    if ((!written.ok() || !f.exists()) && r->droppedFiles.contains(it.path))
                        continue; // left out on purpose (old journal and logs)
                    if (!written.ok() || !f.open(QIODevice::ReadOnly) || f.size() != it.size)
                        return Error(ErrorCode::VerificationFailed, QStringLiteral("%1 did not extract correctly.").arg(it.path));
                    for (qint64 pos = 0; pos < it.size;) {
                        if (!ctx.token.checkpoint())
                            return Error::cancelled();
                        const qint64 want = std::min<qint64>(qint64(a.size()), it.size - pos);
                        FW_TRY_ASSIGN(const qint64 n, src->read(i, pos, a.data(), want));
                        if (n <= 0 || f.read(b.data(), n) != n || std::memcmp(a.data(), b.data(), size_t(n)) != 0)
                            return Error(ErrorCode::VerificationFailed, QStringLiteral("%1 does not match the source.").arg(it.path));
                        pos += n;
                        ctx.progress->advance(n);
                    }
                    ++files;
                }
                return files;
            }();
            if (!compared.ok() && compared.error().code != ErrorCode::VerificationFailed && ctx.token.keepFinishedWork()) {
                // Cancel while the finished extraction is verified (GitHub #97):
                // the files are kept, as with verification turned off (not
                // when the app is closing: the cancel then stands). A difference
                // found meanwhile still fails it.
                o.verification = Verification::Skipped;
                o.verificationDetail = QStringLiteral("Verification was cancelled, so the extracted files are not verified.");
                o.details.insert(QStringLiteral("verificationCancelled"), true);
                ctx.log.warn(o.verificationDetail);
            } else {
                if (!compared.ok())
                    return compared.takeError();
                o.verification = Verification::Passed;
                o.verificationDetail = *compared == 1 ? QStringLiteral("1 file read back, identical")
                                                      : QStringLiteral("%1 files read back, all identical").arg(formatCount(*compared));
            }
        } else {
            o.verification = Verification::Skipped;
            o.verificationDetail = QStringLiteral("Verification is turned off in Settings.");
        }
        // Into place: a pre-existing empty folder goes, the stage takes its name.
        // When that cannot be done (the folder is open in another program),
        // the finished files are not deleted: they keep a free name next to it.
        if ((QFileInfo(out).isDir() && !QDir().rmdir(out)) || !renameWithoutCopying(stage, out)) {
            guard.keep = true;
            const QString kept = exfat::keepStagedFolder(stage, out);
            // Not an IoError: a retry would only extract another full copy.
            return Error(ErrorCode::PermissionDenied,
                         QStringLiteral("The files were extracted%1, but the folder %2 could not be used because it is "
                                        "open in another program. They were saved in %3.")
                             .arg(o.verification == Verification::Passed ? QStringLiteral(" and verified") : QString(),
                                  native(out), native(kept)));
        }
        guard.keep = true;
        o.outputs = {out};
        o.warnings = r->warnings + ampr::renamedWarnings(r->renamed);
        const QString plural = r->files == 1 ? QString() : QStringLiteral("s");
        o.summary = r->kind == ampr::PackKind::None
                        ? QStringLiteral("Extracted %1 file%2 (%3).").arg(formatCount(r->files), plural, formatBytes(r->bytes))
                        : QStringLiteral("Extracted %1 file%2 (%3); %4 came out of the %5 packs.")
                              .arg(formatCount(r->files), plural, formatBytes(r->bytes), formatCount(r->unpackedFiles),
                                   ampr::packKindName(r->kind));
        o.details.insert(QStringLiteral("files"), r->files);
        o.details.insert(QStringLiteral("bytes"), r->bytes);
        return o;
    }

private:
    // Hides the pack indexes from detection so a packed game is copied as it is.
    class PlainView final : public ampr::GameFiles {
    public:
        explicit PlainView(std::shared_ptr<ampr::GameFiles> b) : m_b(std::move(b)) {}
        QString describe() const override { return m_b->describe(); }
        const std::vector<ampr::GameItem>& items() const override { return m_b->items(); }
        int find(const QString& path) const override
        {
            if (path == ampr::kZstdIndexName || path == ampr::kLz4IndexName)
                return -1;
            return m_b->find(path);
        }
        Result<qint64> read(int index, qint64 offset, char* dst, qint64 len) override { return m_b->read(index, offset, dst, len); }

    private:
        std::shared_ptr<ampr::GameFiles> m_b;
    };

    QString source() const { return m_p.value(QStringLiteral("source")).toString(); }
    QString output() const { return m_p.value(QStringLiteral("output")).toString(); }
    QJsonObject m_p;
};

// ---- FPKG staging --------------------------------------------------------------------------

struct StagePlan {
    std::shared_ptr<ampr::GameFiles> files; // logical (packs unpacked)
    QString root;                           // app root inside it
    ampr::PackKind packs = ampr::PackKind::None;
    std::vector<int> include;               // files to write
    QStringList dirs;                       // folders to create (relative to the root)
    qint64 bytes = 0;
    int excluded = 0;
};

bool isAmprJournalOrLog(const QString& rel)
{
    const QString l = rel.toLower();
    return l == u"ampr_commands.bin" || l == u"ampr_emu.log" || l == u"apr_emu.log";
}

Result<StagePlan> planStage(const QString& source, const CancellationToken& token)
{
    StagePlan p;
    FW_TRY_ASSIGN(auto base, openGameFiles(source, token));
    p.packs = ampr::detectPackKind(*base);
    FW_TRY_ASSIGN(p.files, ampr::logicalGame(base, token));
    auto root = findGameRoot(*p.files);
    if (!root)
        return Error(ErrorCode::CorruptData, QStringLiteral("%1 holds no PS5 app (no folder with sce_sys was found).").arg(QFileInfo(source).fileName()));
    p.root = *root;
    const QString prefix = p.root.isEmpty() ? QString() : p.root + u'/';
    QStringList excludedDirs;
    for (int i = 0; i < int(p.files->items().size()); ++i) {
        const ampr::GameItem& it = p.files->items()[size_t(i)];
        if (!prefix.isEmpty() && !it.path.startsWith(prefix))
            continue;
        const QString rel = it.path.mid(prefix.size());
        if (rel.isEmpty())
            continue;
        bool inExcluded = false;
        for (const QString& d : excludedDirs)
            if (rel.startsWith(d + u'/')) {
                inExcluded = true;
                break;
            }
        if (inExcluded)
            continue;
        QStringList parts = rel.split(u'/');
        const QString name = parts.takeLast();
        // Only what the package builder would leave out anyway (its own rules
        // still decide), plus the AMPR emulator's journal and logs. The .dds
        // copies in sce_sys stay: the builder leaves them out of the package
        // itself but rebuilds a missing or broken pic0/pic1/pic2/icon0.png
        // from them (without them such a package lacks pic1.png and pic2.png).
        const bool sceSysDds = !it.isDir && !parts.isEmpty() && parts.first().compare(QLatin1String("sce_sys"), Qt::CaseInsensitive) == 0
                               && name.endsWith(QLatin1String(".dds"), Qt::CaseInsensitive);
        if ((!sceSysDds && !fpkg::baseExclusionReason(parts, name, it.isDir).isEmpty())
            || (!it.isDir && parts.isEmpty() && isAmprJournalOrLog(name))) {
            ++p.excluded;
            if (it.isDir)
                excludedDirs << rel;
            continue;
        }
        if (it.isDir) {
            p.dirs << rel;
            continue;
        }
        p.include.push_back(i);
        p.bytes += it.size;
    }
    return p;
}

class FpkgGameStager final : public fpkg::SourceStager {
public:
    QString displayName() const override { return QStringLiteral("image or packed game"); }
    QStringList extensions() const override
    {
        return {QStringLiteral("exfat"), QStringLiteral("ffpkg"), QStringLiteral("ffpfs"), QStringLiteral("ffpfsc")};
    }
    qint64 estimateStagedBytes(const QString& image) const override
    {
        auto p = planStage(image, {});
        return p.ok() ? std::max<qint64>(1, p->bytes) : 0;
    }
    Status probe(const QString& image) const override
    {
        CancellationSource c;
        auto t = openFileTree(image, c.token());
        return t.ok() ? success() : Status(t.takeError());
    }
    bool handlesFolder(const QString& folder) const override
    {
        const QDir d(folder);
        return QFileInfo(d.filePath(ampr::kZstdIndexName)).isFile() || QFileInfo(d.filePath(ampr::kLz4IndexName)).isFile();
    }
    QString stagingNote(const QString& source) const override
    {
        return QStringLiteral("Preparing %1: only the files the package keeps are copied (packs unpacked, dump and emulator "
                              "leftovers left out).").arg(QFileInfo(source).fileName());
    }
    Status stage(const QString& source, const QString& destination, const CancellationToken& token, ProgressReporter* progress) override
    {
        FW_TRY_ASSIGN(StagePlan p, planStage(source, token));
        if (progress)
            progress->setTotal(p.bytes);
        // The names come from an image or a pack index: every target is checked
        // to lie inside the staging folder before anything is written.
        const QString appName = p.root.isEmpty() ? QStringLiteral("app") : p.root.section(u'/', -1);
        FW_TRY_ASSIGN(const QString appDir, ampr::HostPaths(destination).add(appName, true));
        ampr::HostPaths paths(appDir);
        const QString prefix = p.root.isEmpty() ? QString() : p.root + u'/';
        QStringList dirTargets;
        for (const QString& d : std::as_const(p.dirs)) {
            FW_TRY_ASSIGN(const QString target, paths.add(d, true));
            dirTargets << target;
        }
        std::vector<QString> fileTargets;
        fileTargets.reserve(p.include.size());
        for (int i : p.include) {
            FW_TRY_ASSIGN(QString target, paths.add(p.files->items()[size_t(i)].path.mid(prefix.size())));
            fileTargets.push_back(std::move(target));
        }
        // A package must hold the game's own names: a file this drive would
        // store under another name cannot be staged here.
        if (!paths.renamed().isEmpty())
            return Error(ErrorCode::Unsupported,
                         QStringLiteral("%1 holds names this drive cannot store as they are (%2), so the package cannot be "
                                        "built from it here.")
                             .arg(QFileInfo(source).fileName(), paths.renamed().first()),
                         paths.renamed().join(QStringLiteral("; ")));
        if (!QDir().mkpath(appDir))
            return Error(ErrorCode::IoError, QStringLiteral("Could not create the staging folder."));
        for (const QString& d : std::as_const(dirTargets))
            QDir().mkpath(d);
        std::vector<char> buf(size_t(4) << 20);
        for (size_t k = 0; k < p.include.size(); ++k) {
            const int i = p.include[k];
            const ampr::GameItem& it = p.files->items()[size_t(i)];
            if (progress)
                progress->setCurrentItem(it.path.mid(prefix.size()));
            QDir().mkpath(QFileInfo(fileTargets[k]).absolutePath());
            QFile f(fileTargets[k]);
            if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
                return Error(ErrorCode::IoError, QStringLiteral("Could not create %1.").arg(native(f.fileName())), f.errorString());
            for (qint64 pos = 0; pos < it.size;) {
                if (!token.checkpoint())
                    return Error::cancelled();
                FW_TRY_ASSIGN(const qint64 n, p.files->read(i, pos, buf.data(), std::min<qint64>(qint64(buf.size()), it.size - pos)));
                if (n <= 0)
                    return Error(ErrorCode::CorruptData, QStringLiteral("%1 ends early in %2.").arg(it.path, QFileInfo(source).fileName()));
                if (f.write(buf.data(), n) != n)
                    return Error(ErrorCode::IoError, QStringLiteral("Could not write %1.").arg(native(f.fileName())), f.errorString());
                pos += n;
                if (progress)
                    progress->advance(n);
            }
            if (!f.flush())
                return Error(ErrorCode::IoError, QStringLiteral("Could not write %1.").arg(native(f.fileName())), f.errorString());
            f.close();
        }
        return success();
    }
};

} // namespace

std::shared_ptr<ampr::GameFiles> gameFilesOf(std::unique_ptr<FileTree> tree)
{
    return std::make_shared<TreeGame>(std::move(tree));
}

Result<std::shared_ptr<ampr::GameFiles>> openGameFiles(const QString& path, const CancellationToken& token)
{
    if (QFileInfo(path).isDir())
        return ampr::openFolderGame(path, token);
    FW_TRY_ASSIGN(auto tree, openFileTree(path, token));
    return gameFilesOf(std::move(tree));
}

std::optional<QString> findGameRoot(const ampr::GameFiles& files)
{
    QSet<QString> folded; // lower-cased folder paths, filled when first needed
    auto isRoot = [&](const QString& dir) {
        const QString sys = dir.isEmpty() ? QStringLiteral("sce_sys") : dir + QStringLiteral("/sce_sys");
        const int i = files.find(sys);
        if (i >= 0 && files.items()[size_t(i)].isDir)
            return true;
        // Case-insensitive fallback (exFAT and PFS images keep any case).
        if (folded.isEmpty())
            for (const auto& it : files.items())
                if (it.isDir)
                    folded.insert(it.path.toLower());
        return folded.contains(sys.toLower());
    };
    if (isRoot(QString()))
        return QString();
    for (int depth = 1; depth <= 3; ++depth)
        for (const auto& it : files.items())
            if (it.isDir && it.path.count(u'/') == depth - 1 && isRoot(it.path))
                return it.path;
    return std::nullopt;
}

void registerGameJobs(ModuleContext& ctx)
{
    const JobFactory* factory = &ctx.jobs;
    ctx.jobs.add(kZstdBuildJob, QStringLiteral("Build image with zstd packs"),
                 [factory](const QJsonObject& p) -> Result<std::unique_ptr<Job>> { return std::unique_ptr<Job>(new ZstdBuildJob(p, factory)); });
    ctx.jobs.add(kGameExtractJob, QStringLiteral("Extract a game"),
                 [](const QJsonObject& p) -> Result<std::unique_ptr<Job>> { return std::unique_ptr<Job>(new GameExtractJob(p)); });
}

void registerFpkgGameStager() { fpkg::registerSourceStager(std::make_shared<FpkgGameStager>()); }

} // namespace fw::integration
