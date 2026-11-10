// Queue jobs of the PFS engine (native Lazy_MkPFS port, GPL-3.0-or-later).
//
// Safety rules (audit C1/H2/H6/M1/M2): a build is written to <output>.part and
// verified before it gets its final name; any engine error or verification
// failure fails the job and removes the partial image; an existing output is
// only replaced when the job asks for it (and is restored if the rename
// fails); the source image is deleted only after full verification passed.

#include "engines/pfs/PfsJobs.h"
#include "core/ArchiveSource.h"

#include "core/FileUtil.h"
#include "engines/pfs/Module.h"
#include "engines/pfs/PfsVerify.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>

namespace fw::pfs {

namespace {

QString absPath(const QString& p) { return p.isEmpty() ? p : QDir::cleanPath(QFileInfo(p).absoluteFilePath()); }

QString str(const QJsonObject& o, const char* key) { return o.value(QLatin1String(key)).toString(); }

QString native(const QString& p) { return QDir::toNativeSeparators(p); }

QString formatName(OutputFormat f) { return f == OutputFormat::Container ? QStringLiteral("FFPFSC") : QStringLiteral("FFPFS"); }

QString verifyDetail(const VerifyReport& r)
{
    if (r.passed)
        return r.summary;
    QString s = r.summary;
    if (r.problems.size() > 1)
        s += QStringLiteral(" (and %1 more)").arg(r.problemCount - 1);
    return s;
}

QJsonObject statsJson(const BuildStats& st)
{
    QJsonObject o;
    o.insert(QStringLiteral("files"), st.files);
    o.insert(QStringLiteral("compressedFiles"), st.compressedFiles);
    o.insert(QStringLiteral("sourceBytes"), st.sourceBytes);
    o.insert(QStringLiteral("storedBytes"), st.storedBytes);
    o.insert(QStringLiteral("allCompressedBytes"), st.allCompressedBytes);
    o.insert(QStringLiteral("nestedBytes"), st.nestedBytes);
    o.insert(QStringLiteral("imageBytes"), st.imageBytes);
    o.insert(QStringLiteral("containerCompressed"), st.containerCompressed);
    o.insert(QStringLiteral("threads"), st.threads);
    o.insert(QStringLiteral("seconds"), st.seconds);
    if (st.seconds > 0)
        o.insert(QStringLiteral("mibPerSecond"), double(st.sourceBytes) / (1024.0 * 1024.0) / st.seconds);
    return o;
}

bool hasEntry(const QDir& d, const QString& name)
{
    for (const QString& n : d.entryList(QDir::Files | QDir::Hidden | QDir::System))
        if (n.compare(name, Qt::CaseInsensitive) == 0)
            return true;
    return false;
}

// Lazy_MkPFS pack_folder find_game_root(): the source itself when it holds
// eboot.bin (or sce_sys/param.json, the legacy PFS tab's test), otherwise the
// first sub-folder (depth first, name order) that holds eboot.bin.
QString findGameRoot(const QString& dir, int depth = 0)
{
    const QDir d(dir);
    if (hasEntry(d, QStringLiteral("eboot.bin")) || (depth == 0 && QFileInfo(dir + QStringLiteral("/sce_sys/param.json")).isFile()))
        return dir;
    if (depth >= 8)
        return {};
    for (const QFileInfo& sub : d.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDir::Name)) {
        const QString r = findGameRoot(sub.absoluteFilePath(), depth + 1);
        if (!r.isEmpty())
            return r;
    }
    return {};
}

ProgressHooks hooksFor(ProgressReporter* p)
{
    ProgressHooks h;
    if (p) {
        h.advance = [p](qint64 n) { p->advance(n); };
        h.item = [p](const QString& s) { p->setCurrentItem(s); };
    }
    return h;
}

} // namespace

// ---- image.pfs.build -------------------------------------------------------------------

PfsBuildJob::PfsBuildJob(QJsonObject params)
    : m_params(std::move(params)), m_source(absPath(str(m_params, "source"))), m_output(absPath(str(m_params, "output")))
{
}

QString PfsBuildJob::kind() const { return kinds::kBuild; }

QString PfsBuildJob::title() const
{
    const QString t = str(m_params, "title");
    return t.isEmpty() ? QFileInfo(m_source).fileName() : t;
}

QString PfsBuildJob::subtitle() const
{
    return QStringLiteral("%1 · %2").arg(formatName(format()), native(m_output));
}

QStringList PfsBuildJob::phases() const
{
    return {QStringLiteral("Scanning"), QStringLiteral("Compressing and writing"), QStringLiteral("Verifying")};
}

OutputFormat PfsBuildJob::format() const
{
    const QString f = str(m_params, "format").toLower();
    if (f == u"ffpfs")
        return OutputFormat::FolderImage;
    if (f == u"ffpfsc")
        return OutputFormat::Container;
    return m_output.endsWith(QStringLiteral(".ffpfs"), Qt::CaseInsensitive) ? OutputFormat::FolderImage
                                                                              : OutputFormat::Container;
}

Status PfsBuildJob::validate(const SettingsValues& settings) const
{
    Q_UNUSED(settings);
    const QString f = str(m_params, "format").toLower();
    if (!f.isEmpty() && f != u"ffpfs" && f != u"ffpfsc")
        return Error(ErrorCode::InvalidArgument, QStringLiteral("Unknown PFS format \"%1\".").arg(f));
    if (m_source.isEmpty() || !QFileInfo::exists(m_source))
        return Error(ErrorCode::NotFound, QStringLiteral("Choose an existing game folder or image to pack."));
    if (m_output.isEmpty())
        return Error(ErrorCode::InvalidArgument, QStringLiteral("Choose where to save the PFS image."));
    if (archive::isZipSource(m_source)) { // #67: a zipped dump, unpacked in run()
        FW_TRY(archive::checkZipSource(m_source));
        if (QFileInfo::exists(m_output) && !m_params.value(QStringLiteral("replaceExisting")).toBool())
            return Error(ErrorCode::AlreadyExists, QStringLiteral("%1 already exists. Choose another name or allow replacing it.")
                                                       .arg(QFileInfo(m_output).fileName()));
        return success();
    }
    if (QFileInfo(m_output).isDir())
        return Error(ErrorCode::InvalidArgument, QStringLiteral("The output must be a file name, not a folder."));
    const QFileInfo src(m_source);
    if (src.isDir() && isSameOrInside(m_output, m_source))
        return Error(ErrorCode::InvalidArgument,
                     QStringLiteral("The image would be saved inside the folder being packed. Choose an output "
                                    "folder outside %1.")
                         .arg(native(m_source)));
    if (!src.isDir() && isSameOrInside(m_output, m_source))
        return Error(ErrorCode::InvalidArgument, QStringLiteral("The output cannot replace the source image."));
    if (QFileInfo::exists(m_output) && !m_params.value(QStringLiteral("replaceExisting")).toBool())
        return Error(ErrorCode::AlreadyExists,
                     QStringLiteral("%1 already exists. Choose another name or allow replacing it.")
                         .arg(QFileInfo(m_output).fileName()));
    if (!src.isDir()) {
        const QString inner = innerName();
        if (inner.isEmpty() || inner.contains(u'/') || inner.contains(u'\\') || inner == u"." || inner == u"..")
            return Error(ErrorCode::InvalidArgument, QStringLiteral("\"%1\" cannot be used as a file name inside the image.").arg(inner));
        if (!isAscii(inner))
            return Error(ErrorCode::InvalidArgument,
                         m_params.contains(QStringLiteral("innerName"))
                             ? QStringLiteral("The name inside the image must be plain ASCII: %1.").arg(inner)
                             : QStringLiteral("PFS images only support plain ASCII file names. Rename %1 first.").arg(inner));
    }
    return success();
}

QString PfsBuildJob::innerName() const
{
    const QString n = str(m_params, "innerName").trimmed();
    return n.isEmpty() ? QFileInfo(m_source).fileName() : n;
}

Result<JobOutcome> PfsBuildJob::run(JobContext& ctx)
{
    FW_TRY(validate(ctx.settings));
    if (archive::isZipSource(m_source)) {
        const QString zip = m_source;
        FW_TRY_ASSIGN(m_source, archive::unpackZipSource(zip, ctx.tempDir, ctx.token, ctx.progress));
        ctx.log.info(QStringLiteral("Unpacked %1 to %2").arg(native(zip), native(m_source)));
        FW_TRY(validate(ctx.settings));
        // Unpacking reports as a phase of its own, which replaced the job's.
        if (ctx.progress)
            ctx.progress->setPhases(phases(), phaseWeights());
    }
    ProgressReporter* pr = ctx.progress;
    const qint64 ts = m_params.value(QStringLiteral("timestamp")).toInteger(0);
    const qint64 now = ts > 0 ? ts : QDateTime::currentSecsSinceEpoch();
    const OutputFormat fmt = format();
    const BuildOptions opts = buildOptionsFrom(ctx.settings, now);
    const bool isFolder = QFileInfo(m_source).isDir();
    QStringList outcomeWarnings;

    // ---- Scanning ----
    if (pr)
        pr->beginPhase(0, 0, 0, false);
    BuildInput in;
    in.isFolder = isFolder;
    if (isFolder) {
        QString root = m_source;
        if (m_params.value(QStringLiteral("findGameRoot")).toBool(true)) {
            const QString found = findGameRoot(m_source);
            if (!found.isEmpty() && found != m_source) {
                root = QDir::cleanPath(found);
                ctx.log.info(QStringLiteral("Game found in %1").arg(native(root)));
            } else if (found.isEmpty()) {
                outcomeWarnings << QStringLiteral("No eboot.bin or sce_sys/param.json found; the whole folder was packed.");
                ctx.log.warn(outcomeWarnings.back());
            }
        }
        FW_TRY_ASSIGN(in.plan, planFolder(root, planOptionsFrom(ctx.settings, now), ctx.token));
        const SourcePlan& p = in.plan;
        ctx.log.info(QStringLiteral("Packing %1 files (%2) from %3").arg(formatCount(qint64(p.files.size())),
                                                                          formatBytes(p.totalBytes), native(m_source)));
        if (p.excludedFiles || p.excludedFolders)
            ctx.log.info(QStringLiteral("Left out %1 file(s) and %2 folder(s): %3")
                             .arg(p.excludedFiles)
                             .arg(p.excludedFolders)
                             .arg(p.excludedSamples.join(QStringLiteral(", "))));
        if (p.emptyFolderMarkers)
            ctx.log.info(QStringLiteral("Kept %1 empty folder(s) with .pfskeep markers").arg(p.emptyFolderMarkers));
        if (p.amprGenerated)
            ctx.log.info(QStringLiteral("Added ampr_emu.index (fakelib/libSceAmpr.sprx found)"));
    } else {
        in.file = m_source;
        in.fileName = innerName().toLatin1();
        if (in.fileName != QFileInfo(m_source).fileName().toLatin1())
            ctx.log.info(QStringLiteral("Packed as %1 inside the image").arg(QString::fromLatin1(in.fileName)));
    }

    // ---- free space (worst case: nothing compresses) ----
    const QString outDir = QFileInfo(m_output).absolutePath();
    if (!QDir().mkpath(outDir))
        return Error(ErrorCode::IoError, QStringLiteral("Could not create the folder %1.").arg(native(outDir)));
    FW_TRY_ASSIGN(const qint64 worstCase, estimateImageSize(in, fmt, opts));
    FW_TRY(ensureFreeSpace(outDir, worstCase, freeSpaceMarginFrom(ctx.settings)));

    // ---- Compressing and writing ----
    if (pr)
        pr->beginPhase(1, progressTotal(in), isFolder ? qint64(in.plan.files.size()) : 1);
    ctx.log.debug(QStringLiteral("PFS options: format=%1 level=%2 threads=%3 block=%4 caseInsensitive=%5 time=%6")
                      .arg(formatName(fmt))
                      .arg(opts.level)
                      .arg(opts.threads)
                      .arg(opts.layout.blockSize)
                      .arg(opts.layout.caseInsensitive)
                      .arg(now));
    PartFile part(m_output);
    BuildHooks hooks;
    if (pr) {
        hooks.advance = [pr](qint64 n) { pr->advance(n); };
        hooks.item = [pr](const QString& s) { pr->setCurrentItem(s); };
    }
    FW_TRY_ASSIGN(const BuildStats st, writeImage(in, fmt, opts, part.partPath(), ctx.token, hooks, ctx.log));
    ctx.log.info(QStringLiteral("Image written: %1 in %2 s (%3)")
                     .arg(formatBytes(st.imageBytes))
                     .arg(st.seconds, 0, 'f', 1)
                     .arg(formatRate(st.seconds > 0 ? double(st.sourceBytes) / st.seconds : 0)));

    JobOutcome outcome;
    outcome.warnings = outcomeWarnings;
    outcome.details = statsJson(st);
    outcome.details.insert(QStringLiteral("format"), formatName(fmt));
    if (isFolder)
        outcome.details.insert(QStringLiteral("gameRoot"), in.plan.root);
    outcome.details.insert(QStringLiteral("timestamp"), now);

    // ---- Verifying ----
    const auto depth = verifyDepthFrom(ctx.settings);
    if (!depth) {
        outcome.verification = Verification::Skipped;
        outcome.verificationDetail = QStringLiteral("Checking PFS images is off in Settings > Verification.");
        outcome.warnings << outcome.verificationDetail;
    } else {
        VerifyExpectation ex;
        ex.format = fmt;
        if (isFolder) {
            ex.kind = VerifyExpectation::Kind::Folder;
            ex.plan = &in.plan;
        } else {
            ex.kind = VerifyExpectation::Kind::File;
            ex.file = m_source;
            ex.fileName = in.fileName;
        }
        if (pr) {
            auto work = verifyWorkBytes(part.partPath(), ex, *depth);
            pr->beginPhase(2, work.ok() ? *work : 0);
        }
        auto vr = verifyImage(part.partPath(), ex, *depth, ctx.token, hooksFor(pr));
        if (!vr.ok()) {
            if (vr.error().isCancelled())
                return vr.takeError();
            outcome.verification = Verification::Failed;
            outcome.verificationDetail = vr.error().message;
            ctx.log.error(QStringLiteral("Verification failed: %1").arg(vr.error().toLogString()));
            outcome.summary = QStringLiteral("Verification failed; the image was removed.");
            return outcome; // the .part is removed; nothing keeps the final name
        }
        for (const QString& w : vr->warnings)
            ctx.log.warn(w);
        if (!vr->passed) {
            outcome.verification = Verification::Failed;
            outcome.verificationDetail = verifyDetail(*vr);
            for (const QString& p : vr->problems)
                ctx.log.error(p);
            outcome.summary = QStringLiteral("Verification failed; the image was removed.");
            return outcome;
        }
        outcome.verification = Verification::Passed;
        outcome.verificationDetail = vr->summary;
        outcome.details.insert(QStringLiteral("verifiedFiles"), vr->filesChecked);
        outcome.details.insert(QStringLiteral("paddedFiles"), vr->paddedFiles);
        ctx.log.info(QStringLiteral("Verified: %1").arg(vr->summary));
    }

    FW_TRY(part.commit(m_params.value(QStringLiteral("replaceExisting")).toBool()));
    outcome.outputs << m_output;

    // ---- delete the source image (convert) ----
    if (!isFolder && ctx.settings.boolean(keys::kDeleteSourceAfterConvert)) {
        if (outcome.verification == Verification::Passed && depth == VerifyDepth::Full) {
            if (QFile::remove(m_source)) {
                ctx.log.info(QStringLiteral("Deleted the source image %1").arg(native(m_source)));
                outcome.details.insert(QStringLiteral("sourceDeleted"), true);
            } else {
                outcome.warnings << QStringLiteral("The source image could not be deleted: %1").arg(native(m_source));
            }
        } else {
            outcome.warnings << QStringLiteral("The source image was kept because full verification did not run.");
        }
    }

    const double gain = st.sourceBytes > 0 ? 100.0 * double(st.sourceBytes - st.imageBytes) / double(st.sourceBytes) : 0;
    outcome.summary = QStringLiteral("%1 · %2 → %3").arg(formatName(fmt), formatBytes(st.sourceBytes), formatBytes(st.imageBytes));
    if (gain > 0.05)
        outcome.summary += QStringLiteral(" (%1% smaller)").arg(gain, 0, 'f', 0);
    return outcome;
}

// ---- image.pfs.verify ---------------------------------------------------------------------

PfsVerifyJob::PfsVerifyJob(QJsonObject params)
    : m_params(std::move(params)), m_image(absPath(str(m_params, "image"))), m_source(absPath(str(m_params, "source")))
{
}

QString PfsVerifyJob::kind() const { return kinds::kVerify; }
QString PfsVerifyJob::title() const { return QStringLiteral("Verify %1").arg(QFileInfo(m_image).fileName()); }
QStringList PfsVerifyJob::phases() const { return {QStringLiteral("Verifying")}; }

Status PfsVerifyJob::validate(const SettingsValues& settings) const
{
    Q_UNUSED(settings);
    if (m_image.isEmpty() || !QFileInfo(m_image).isFile())
        return Error(ErrorCode::NotFound, QStringLiteral("Choose an existing .ffpfs or .ffpfsc image."));
    if (!m_source.isEmpty() && !QFileInfo::exists(m_source))
        return Error(ErrorCode::NotFound, QStringLiteral("The source %1 does not exist.").arg(native(m_source)));
    const QString d = str(m_params, "depth");
    if (!d.isEmpty() && d != u"full" && d != u"structure")
        return Error(ErrorCode::InvalidArgument, QStringLiteral("Unknown verification depth \"%1\".").arg(d));
    return success();
}

Result<JobOutcome> PfsVerifyJob::run(JobContext& ctx)
{
    FW_TRY(validate(ctx.settings));
    FW_TRY_ASSIGN(const ImageSummary sum, inspectImage(m_image));
    QString d = str(m_params, "depth");
    if (d.isEmpty())
        d = m_source.isEmpty() ? QStringLiteral("structure") : QStringLiteral("full");
    const VerifyDepth depth = d == u"structure" ? VerifyDepth::Structure : VerifyDepth::Full;

    VerifyExpectation ex;
    SourcePlan plan;
    if (!m_source.isEmpty()) {
        if (QFileInfo(m_source).isDir()) {
            // The same plan the build made: settings snapshot + the image's own build time
            // (generated files carry it), so exclusions never cause false failures (H2).
            QString root = m_source;
            if (m_params.value(QStringLiteral("findGameRoot")).toBool(true)) {
                const QString found = findGameRoot(m_source);
                if (!found.isEmpty())
                    root = QDir::cleanPath(found);
            }
            FW_TRY_ASSIGN(plan, planFolder(root, planOptionsFrom(ctx.settings, sum.timestamp), ctx.token));
            ex.kind = VerifyExpectation::Kind::Folder;
            ex.plan = &plan;
            ex.format = sum.nested ? OutputFormat::Container : OutputFormat::FolderImage;
        } else {
            ex.kind = VerifyExpectation::Kind::File;
            ex.file = m_source;
            const QString inner = str(m_params, "innerName").trimmed();
            ex.fileName = (inner.isEmpty() ? QFileInfo(m_source).fileName() : inner).toLatin1();
            ex.format = OutputFormat::Container;
        }
    }
    if (ctx.progress) {
        auto work = verifyWorkBytes(m_image, ex, depth);
        ctx.progress->beginPhase(0, work.ok() ? *work : 0);
    }
    FW_TRY_ASSIGN(const VerifyReport vr, verifyImage(m_image, ex, depth, ctx.token, hooksFor(ctx.progress)));
    JobOutcome outcome;
    for (const QString& w : vr.warnings)
        outcome.warnings << w;
    outcome.verification = vr.passed ? Verification::Passed : Verification::Failed;
    outcome.verificationDetail = verifyDetail(vr);
    QJsonArray problems;
    for (const QString& p : vr.problems) {
        problems.append(p);
        ctx.log.error(p);
    }
    outcome.details.insert(QStringLiteral("problems"), problems);
    outcome.details.insert(QStringLiteral("files"), vr.filesChecked);
    outcome.details.insert(QStringLiteral("nested"), vr.nested);
    outcome.details.insert(QStringLiteral("depth"), d);
    outcome.summary = vr.summary;
    return outcome;
}

// ---- image.pfs.extract ----------------------------------------------------------------------

PfsExtractJob::PfsExtractJob(QJsonObject params)
    : m_params(std::move(params)), m_image(absPath(str(m_params, "image"))), m_output(absPath(str(m_params, "output")))
{
}

QString PfsExtractJob::kind() const { return kinds::kExtract; }
QString PfsExtractJob::title() const { return QStringLiteral("Extract %1").arg(QFileInfo(m_image).fileName()); }
QStringList PfsExtractJob::phases() const { return {QStringLiteral("Extracting")}; }

Status PfsExtractJob::validate(const SettingsValues& settings) const
{
    Q_UNUSED(settings);
    if (m_image.isEmpty() || !QFileInfo(m_image).isFile())
        return Error(ErrorCode::NotFound, QStringLiteral("Choose an existing .ffpfs or .ffpfsc image."));
    if (m_output.isEmpty())
        return Error(ErrorCode::InvalidArgument, QStringLiteral("Choose a folder to extract to."));
    if (QFileInfo(m_output).exists() && !QFileInfo(m_output).isDir())
        return Error(ErrorCode::AlreadyExists, QStringLiteral("%1 exists and is not a folder.").arg(native(m_output)));
    return success();
}

Result<JobOutcome> PfsExtractJob::run(JobContext& ctx)
{
    FW_TRY(validate(ctx.settings));
    ExtractOptions o;
    o.deep = m_params.value(QStringLiteral("deep")).toBool(true);
    o.overwrite = m_params.value(QStringLiteral("overwrite")).toBool(false);
    o.freeSpaceMargin = freeSpaceMarginFrom(ctx.settings);
    if (ctx.progress) {
        auto sum = inspectImage(m_image);
        ctx.progress->beginPhase(0, sum.ok() ? (o.deep || !sum->nested ? sum->logicalBytes : 0) : 0,
                                 sum.ok() ? sum->files : 0);
    }
    FW_TRY_ASSIGN(const ExtractReport r, extractImage(m_image, m_output, o, ctx.token, hooksFor(ctx.progress)));
    JobOutcome outcome;
    outcome.outputs << r.output;
    outcome.warnings = r.warnings;
    for (int i = 0; i < r.renamed.size() && i < 20; ++i)
        outcome.warnings << QStringLiteral("Renamed for this drive: %1").arg(r.renamed[i]);
    if (r.renamed.size() > 20)
        outcome.warnings << QStringLiteral("%1 more names were changed.").arg(r.renamed.size() - 20);
    for (const QString& n : r.renamed)
        ctx.log.info(QStringLiteral("Renamed for this drive: %1").arg(n));
    outcome.verification = Verification::NotRun;
    outcome.details.insert(QStringLiteral("files"), r.files);
    outcome.details.insert(QStringLiteral("folders"), r.dirs);
    outcome.details.insert(QStringLiteral("bytes"), r.bytes);
    outcome.details.insert(QStringLiteral("nested"), r.nested);
    outcome.summary = QStringLiteral("%1 files · %2").arg(formatCount(r.files), formatBytes(r.bytes));
    return outcome;
}

// ---- registration ---------------------------------------------------------------------------

void registerJobs(ModuleContext& ctx)
{
    ctx.jobs.add(kinds::kBuild, QStringLiteral("Build PFS image"), [](const QJsonObject& p) -> Result<std::unique_ptr<Job>> {
        return std::unique_ptr<Job>(new PfsBuildJob(p));
    });
    ctx.jobs.add(kinds::kVerify, QStringLiteral("Verify PFS image"), [](const QJsonObject& p) -> Result<std::unique_ptr<Job>> {
        return std::unique_ptr<Job>(new PfsVerifyJob(p));
    });
    ctx.jobs.add(kinds::kExtract, QStringLiteral("Extract PFS image"), [](const QJsonObject& p) -> Result<std::unique_ptr<Job>> {
        return std::unique_ptr<Job>(new PfsExtractJob(p));
    });
}

} // namespace fw::pfs
