#include "engines/integration/Module.h"
#include "engines/integration/GameSources.h"

#include "connect/ps5/Module.h"
#include "engines/integration/AmprFtp.h"
#include "core/FileUtil.h"
#include "engines/exfat/ExfatExtract.h"
#include "engines/exfat/ExfatReader.h"
#include "engines/exfat/Module.h"
#include "engines/fpkg/SourceStager.h"
#include "engines/general/Module.h"
#include "engines/integration/ImageCompare.h"
#include "engines/integration/ImageFiles.h"
#include "engines/pfs/PfsVerify.h"
#include "engines/ufs2/Ufs2Reader.h"
#include "engines/ufs2/Ufs2Verify.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>
#include <QStorageInfo>

namespace fw::integration {

namespace {

// ---- image.convert ------------------------------------------------------------------

QString formatOfFile(const QString& path)
{
    const QString ext = QFileInfo(path).suffix().toLower();
    if (ext == u"exfat")
        return QStringLiteral("exfat");
    if (ext == u"ffpkg")
        return QStringLiteral("ffpkg");
    if (ext == u"ffpfs")
        return QStringLiteral("ffpfs");
    if (ext == u"ffpfsc")
        return QStringLiteral("ffpfsc");
    return {};
}

bool isPfsFormat(const QString& format) { return format == u"ffpfs" || format == u"ffpfsc"; }

QString engineOf(const QString& format)
{
    if (format == u"exfat")
        return QStringLiteral("image.exfat");
    if (format == u"ffpkg")
        return QStringLiteral("image.ffpkg");
    if (isPfsFormat(format))
        return QStringLiteral("image.pfs");
    return {};
}

// A plain ASCII name for an image packed into a PFS container (PFS names are
// ASCII): accents are dropped ("Yōtei" -> "Yotei"), other characters become
// '_'; a name with nothing usable left falls back to the title ID in it.
QString asciiInnerName(const QString& fileName)
{
    const QFileInfo fi(fileName);
    const QString ext = fi.suffix().toLower();
    const QString base = fi.completeBaseName();
    QString out;
    bool usable = false;
    for (const QChar c : base.normalized(QString::NormalizationForm_KD)) {
        if (c.category() == QChar::Mark_NonSpacing)
            continue;
        const char16_t u = c.unicode();
        if (u >= 0x20 && u < 0x7F && u != u'/' && u != u'\\') {
            out += c;
            usable |= c.isLetterOrNumber();
        } else if (!out.endsWith(u'_')) {
            out += u'_';
        }
    }
    out = out.trimmed();
    if (!usable) {
        static const QRegularExpression id(QStringLiteral("[A-Z]{4}[0-9]{5}"));
        const auto m = id.match(base.toUpper());
        out = m.hasMatch() ? m.captured(0) : QStringLiteral("image");
    }
    return ext.isEmpty() ? out : out + u'.' + ext;
}

// Converts an image to another format (legacy Convert tab). Methods:
//   direct — .exfat/.ffpkg -> .ffpfsc only: the image file itself is packed into
//            a compressed PFS container (legacy 3.5.3+ "no extraction"); no
//            temporary copy, free space is needed for the output only.
//   repack — the files are extracted to the job's temp folder and built again
//            in the target format (the only way for every other pair).
// A container that holds an .exfat/.ffpkg file (a direct conversion) converts
// back by unpacking that file: to its own format as is, otherwise repacked.
// An existing output is only replaced with replaceExisting, and only by the
// final rename of the finished, verified image.
class ConvertJob final : public Job {
public:
    ConvertJob(QJsonObject p, const JobFactory* factory) : m_p(std::move(p)), m_factory(factory) {}
    QString kind() const override { return kConvertJob; }
    QString title() const override { return QFileInfo(image()).fileName(); }
    QString subtitle() const override
    {
        return QStringLiteral("Convert to .%1 → %2").arg(target(), QDir::toNativeSeparators(QFileInfo(output()).absolutePath()));
    }
    QStringList phases() const override
    {
        if (method() == u"direct")
            return {QStringLiteral("Scanning"), QStringLiteral("Packing"), QStringLiteral("Verifying")};
        return {QStringLiteral("Extracting"), QStringLiteral("Building")};
    }
    QJsonObject parameters() const override { return m_p; }
    QStringList busyPaths() const override
    {
        return {QDir::cleanPath(QFileInfo(image()).absoluteFilePath()), QDir::cleanPath(QFileInfo(output()).absoluteFilePath())};
    }

    Status validate(const SettingsValues&) const override
    {
        const QString from = formatOfFile(image());
        if (!QFileInfo(image()).isFile() || from.isEmpty())
            return Error(ErrorCode::InvalidArgument, QStringLiteral("Choose an .exfat, .ffpkg, .ffpfs or .ffpfsc image to convert."));
        if (engineOf(target()).isEmpty())
            return Error(ErrorCode::InvalidArgument, QStringLiteral("Choose the format to convert to."));
        if (from == target())
            return Error(ErrorCode::InvalidArgument, QStringLiteral("The image is already in that format."));
        if (output().isEmpty())
            return Error(ErrorCode::InvalidArgument, QStringLiteral("Choose where to save the converted image."));
        if (formatOfFile(output()) != target())
            return Error(ErrorCode::InvalidArgument, QStringLiteral("The converted image must end in .%1.").arg(target()));
        if (QFileInfo(output()).absoluteFilePath() == QFileInfo(image()).absoluteFilePath())
            return Error(ErrorCode::InvalidArgument, QStringLiteral("The converted image cannot replace its source."));
        if (QFileInfo(output()).isDir())
            return Error(ErrorCode::InvalidArgument, QStringLiteral("The output must be a file name, not a folder."));
        if (QFileInfo::exists(output()) && !replaceExisting())
            return Error(ErrorCode::AlreadyExists, QStringLiteral("%1 already exists. Choose another name or allow replacing it.").arg(QFileInfo(output()).fileName()));
        const QString m = m_p.value(QStringLiteral("method")).toString();
        if (!m.isEmpty() && m != u"direct" && m != u"repack")
            return Error(ErrorCode::InvalidArgument, QStringLiteral("Unknown conversion method \"%1\".").arg(m));
        if (m == u"direct" && !directPossible())
            return Error(ErrorCode::InvalidArgument, QStringLiteral("Only .exfat and .ffpkg images can be packed into an .ffpfsc as they are."));
        if (!m_factory || !m_factory->contains(engineOf(target()) + QStringLiteral(".build"))
            || (method() == u"repack" && !m_factory->contains(engineOf(from) + QStringLiteral(".extract"))))
            return Error(ErrorCode::Unsupported, QStringLiteral("Converting between these formats is not available in this build."));
        return success();
    }

    Result<JobOutcome> run(JobContext& ctx) override
    {
        FW_TRY(validate(ctx.settings));
        ctx.log.info(QStringLiteral("Converting %1 to .%2 (%3)")
                         .arg(QFileInfo(image()).fileName(), target(),
                              method() == u"direct" ? QStringLiteral("packing the image as it is") : QStringLiteral("repacking its files")));

        // An existing output keeps its name until the new image is finished
        // and verified: every path below writes "<output>.part" and replaces
        // the old file only in its final rename (PartFile::commit), so a
        // failure, a cancel or a crash leaves the previous image where it was.
        if (QFileInfo::exists(output()))
            ctx.log.info(QStringLiteral("The existing %1 is replaced once the new image has verified.").arg(QFileInfo(output()).fileName()));
        auto result = convert(ctx);
        if (result.ok())
            result->details.insert(QStringLiteral("method"), method());
        return result;
    }

private:
    QString image() const { return m_p.value(QStringLiteral("image")).toString(); }
    QString target() const { return m_p.value(QStringLiteral("format")).toString().toLower(); }
    QString output() const { return m_p.value(QStringLiteral("output")).toString(); }
    bool replaceExisting() const { return m_p.value(QStringLiteral("replaceExisting")).toBool(false); }
    bool directPossible() const
    {
        const QString from = formatOfFile(image());
        return (from == u"exfat" || from == u"ffpkg") && target() == u"ffpfsc";
    }
    // "direct" by default where it is possible (legacy behaviour), else "repack".
    QString method() const
    {
        const QString m = m_p.value(QStringLiteral("method")).toString();
        if (m == u"repack")
            return m;
        return directPossible() ? QStringLiteral("direct") : QStringLiteral("repack");
    }

    Result<JobOutcome> runChild(JobContext& ctx, const QString& kind, const QJsonObject& params)
    {
        FW_TRY_ASSIGN(auto job, m_factory->create(kind, params));
        FW_TRY(job->validate(ctx.settings));
        return job->run(ctx);
    }

    // The build that writes output(): it may replace an existing file exactly
    // when this job may (the FFPKG and PFS builds take that as a parameter,
    // the exFAT build from its setting).
    Result<JobOutcome> runBuild(JobContext& ctx, const QString& kind, QJsonObject params)
    {
        params.insert(QStringLiteral("replaceExisting"), replaceExisting());
        JobContext sub = ctx;
        sub.settings = ctx.settings.withOverrides({{exfat::keys::kOverwriteExisting, replaceExisting()}});
        return runChild(sub, kind, params);
    }

    Result<JobOutcome> convert(JobContext& ctx)
    {
        if (method() == u"direct") {
            const QString inner = asciiInnerName(QFileInfo(image()).fileName());
            QJsonObject build{{QStringLiteral("source"), image()},
                              {QStringLiteral("output"), output()},
                              {QStringLiteral("format"), QStringLiteral("ffpfsc")},
                              {QStringLiteral("innerName"), inner}};
            FW_TRY_ASSIGN(JobOutcome built, runBuild(ctx, QStringLiteral("image.pfs.build"), build));
            built.details.insert(QStringLiteral("convertedFrom"), image());
            built.details.insert(QStringLiteral("innerName"), inner);
            if (built.verification != Verification::Failed)
                built.summary = QStringLiteral("Packed %1 into %2. %3").arg(QFileInfo(image()).fileName(), QFileInfo(output()).fileName(), built.summary);
            return built;
        }

        const QString from = formatOfFile(image());
        if (isPfsFormat(from)) {
            // A container holding an .exfat/.ffpkg file (a direct conversion): unpack that file first.
            auto sum = pfs::inspectImage(image());
            if (sum.ok() && !sum->nested && !sum->singleFile.isEmpty()) {
                const QString inner = QString::fromLatin1(sum->singleFile);
                const QString innerFormat = formatOfFile(inner);
                if (innerFormat == u"exfat" || innerFormat == u"ffpkg")
                    return convertWrapped(ctx, inner, innerFormat);
            }
        }
        return repack(ctx, image(), from, QDir(ctx.tempDir).filePath(QStringLiteral("converted")));
    }

    // Extract `source` (format `from`) to `stage`, build the target from it.
    Result<JobOutcome> repack(JobContext& ctx, const QString& source, const QString& from, const QString& stage)
    {
        QJsonObject ex{{QStringLiteral("image"), source}, {QStringLiteral("output"), stage}};
        if (isPfsFormat(from))
            ex.insert(QStringLiteral("deep"), true);
        FW_TRY_ASSIGN(JobOutcome extracted, runChild(ctx, engineOf(from) + QStringLiteral(".extract"), ex));
        if (extracted.verification == Verification::Failed)
            return Error(ErrorCode::VerificationFailed, QStringLiteral("The source image could not be read back correctly. %1").arg(extracted.verificationDetail));

        QJsonObject build{{QStringLiteral("source"), stage}, {QStringLiteral("output"), output()}};
        if (isPfsFormat(target()))
            build.insert(QStringLiteral("format"), target());
        FW_TRY_ASSIGN(JobOutcome built, runBuild(ctx, engineOf(target()) + QStringLiteral(".build"), build));
        built.warnings = extracted.warnings + built.warnings;
        built.details.insert(QStringLiteral("convertedFrom"), image());
        if (built.verification != Verification::Failed)
            built.summary = QStringLiteral("Converted to %1. %2").arg(QFileInfo(output()).fileName(), built.summary);
        return built;
    }

    Result<JobOutcome> convertWrapped(JobContext& ctx, const QString& inner, const QString& innerFormat)
    {
        ctx.log.info(QStringLiteral("%1 holds the image %2; unpacking it first.").arg(QFileInfo(image()).fileName(), inner));
        const QString unpacked = QDir(ctx.tempDir).filePath(QStringLiteral("unpacked"));
        FW_TRY_ASSIGN(JobOutcome ex, runChild(ctx, QStringLiteral("image.pfs.extract"),
                                              {{QStringLiteral("image"), image()}, {QStringLiteral("output"), unpacked}, {QStringLiteral("deep"), false}}));
        const QString innerPath = QDir(unpacked).filePath(inner);
        if (!QFileInfo(innerPath).isFile())
            return Error(ErrorCode::CorruptData, QStringLiteral("%1 was not found inside %2.").arg(inner, QFileInfo(image()).fileName()));
        if (innerFormat != target()) {
            auto built = repack(ctx, innerPath, innerFormat, QDir(ctx.tempDir).filePath(QStringLiteral("converted")));
            if (built.ok())
                built->warnings = ex.warnings + built->warnings;
            return built;
        }
        // Same format: the unpacked image is the result. Check it, then move it into place.
        FW_TRY_ASSIGN(JobOutcome checked, runChild(ctx, engineOf(innerFormat) + QStringLiteral(".verify"), {{QStringLiteral("image"), innerPath}}));
        if (checked.verification == Verification::Failed) {
            checked.summary = QStringLiteral("The image inside %1 did not verify; nothing was written.").arg(QFileInfo(image()).fileName());
            return checked;
        }
        // On the output's own drive the unpacked image only needs a new name;
        // from another drive (the temp folder usually is on one) it is copied
        // through "<output>.part": cancellable, with progress, and only after
        // the output drive is known to have room.
        const QString outDir = QFileInfo(output()).absolutePath();
        if (!QDir().mkpath(outDir))
            return Error(ErrorCode::IoError, QStringLiteral("Could not create the folder %1.").arg(QDir::toNativeSeparators(outDir)));
        const QStorageInfo from(QFileInfo(innerPath).absolutePath()), to(outDir);
        bool placed = false;
        if (from.isValid() && to.isValid() && from.rootPath() == to.rootPath()) {
            PartFile part(output());
            QFile::remove(part.partPath());
            if (QFile::rename(innerPath, part.partPath())) {
                FW_TRY(part.commit(replaceExisting()));
                placed = true;
            }
        }
        if (!placed) {
            const qint64 size = QFileInfo(innerPath).size();
            FW_TRY(ensureFreeSpace(outDir, size, general::freeSpaceMarginBytes(ctx.settings)));
            ctx.progress->begin(QStringLiteral("Saving"), size, 1);
            ctx.progress->setCurrentItem(QFileInfo(output()).fileName());
            FW_TRY(copyFileStreaming(innerPath, output(), ctx.token, ctx.progress, replaceExisting()));
        }
        JobOutcome out = checked;
        out.outputs = {output()};
        out.warnings = ex.warnings + checked.warnings;
        out.details.insert(QStringLiteral("convertedFrom"), image());
        out.summary = QStringLiteral("Unpacked %1 from %2. %3").arg(QFileInfo(output()).fileName(), QFileInfo(image()).fileName(), checked.summary);
        return out;
    }

    QJsonObject m_p;
    const JobFactory* m_factory;
};

// ---- image.compare ------------------------------------------------------------------

class CompareJob final : public Job {
public:
    explicit CompareJob(QJsonObject p) : m_p(std::move(p)) {}
    QString kind() const override { return kCompareJob; }
    QString title() const override
    {
        return QStringLiteral("%1 ↔ %2").arg(QFileInfo(a()).fileName(), QFileInfo(b()).fileName());
    }
    QString subtitle() const override
    {
        return depth() == CompareDepth::Full ? QStringLiteral("Compare names, sizes and contents")
                                             : QStringLiteral("Compare names and sizes");
    }
    QStringList phases() const override
    {
        return {QStringLiteral("Reading A"), QStringLiteral("Reading B"), QStringLiteral("Comparing")};
    }
    QList<double> phaseWeights() const override
    {
        return depth() == CompareDepth::Full ? QList<double>{1, 1, 18} : QList<double>{1, 1, 1};
    }
    QJsonObject parameters() const override { return m_p; }
    QStringList busyPaths() const override
    {
        QStringList out;
        for (const QString& p : {a(), b()})
            if (!p.isEmpty())
                out << QDir::cleanPath(QFileInfo(p).absoluteFilePath());
        return out;
    }

    Status validate(const SettingsValues&) const override
    {
        if (a().isEmpty())
            return Error(ErrorCode::InvalidArgument, QStringLiteral("Choose the image to compare (A)."));
        if (b().isEmpty())
            return Error(ErrorCode::InvalidArgument, QStringLiteral("Choose the image or folder to compare it with (B)."));
        for (const QString& p : {a(), b()}) {
            const QFileInfo fi(p);
            if (!fi.exists())
                return Error(ErrorCode::NotFound, QStringLiteral("%1 does not exist.").arg(QDir::toNativeSeparators(p)));
            if (!fi.isDir() && !isTreeImage(p))
                return Error(ErrorCode::InvalidArgument,
                             QStringLiteral("%1 is not an .exfat, .ffpkg, .ffpfs or .ffpfsc image or a folder.").arg(QDir::toNativeSeparators(p)));
        }
        if (QDir::cleanPath(QFileInfo(a()).absoluteFilePath()) == QDir::cleanPath(QFileInfo(b()).absoluteFilePath()))
            return Error(ErrorCode::InvalidArgument, QStringLiteral("Choose two different images (or an image and a folder)."));
        const QString d = m_p.value(QStringLiteral("depth")).toString();
        if (!d.isEmpty() && d != u"full" && d != u"structure")
            return Error(ErrorCode::InvalidArgument, QStringLiteral("Unknown comparison depth \"%1\".").arg(d));
        return success();
    }

    Result<JobOutcome> run(JobContext& ctx) override
    {
        FW_TRY(validate(ctx.settings));
        const CompareDepth dep = depth();
        ctx.log.info(QStringLiteral("Comparing %1 (A) with %2 (B), %3")
                         .arg(QDir::toNativeSeparators(a()), QDir::toNativeSeparators(b()),
                              dep == CompareDepth::Full ? QStringLiteral("names, sizes and contents") : QStringLiteral("names and sizes")));
        ctx.progress->beginPhase(0, 0);
        ctx.progress->setCurrentItem(QFileInfo(a()).fileName());
        FW_TRY_ASSIGN(auto treeA, openFileTree(a(), ctx.token));
        ctx.log.info(QStringLiteral("A: %1").arg(treeLine(*treeA)));
        ctx.progress->beginPhase(1, 0);
        ctx.progress->setCurrentItem(QFileInfo(b()).fileName());
        FW_TRY_ASSIGN(auto treeB, openFileTree(b(), ctx.token));
        ctx.log.info(QStringLiteral("B: %1").arg(treeLine(*treeB)));

        CompareOptions opt;
        opt.depth = dep;
        FW_TRY_ASSIGN(CompareReport rep, compareTrees(*treeA, *treeB, opt, ctx.token, ctx.progress, 2));

        JobOutcome out;
        out.verification = Verification::NotRun; // a comparison is a report, not a build check
        out.summary = rep.summary();
        for (const QString& w : treeA->warnings())
            out.warnings << QStringLiteral("A: %1").arg(w);
        for (const QString& w : treeB->warnings())
            out.warnings << QStringLiteral("B: %1").arg(w);
        out.warnings << rep.warnings;
        out.details = rep.toJson();
        ctx.log.info(out.summary);
        int logged = 0;
        const auto logSome = [&](const QString& line) {
            if (logged++ < 20)
                ctx.log.info(line);
        };
        for (const QString& p : rep.missing)
            logSome(QStringLiteral("Only in A: %1").arg(p));
        for (const QString& p : rep.missingDirs)
            logSome(QStringLiteral("Folder only in A: %1").arg(p));
        for (const QString& p : rep.extra)
            logSome(QStringLiteral("Only in B: %1").arg(p));
        for (const QString& p : rep.extraDirs)
            logSome(QStringLiteral("Folder only in B: %1").arg(p));
        for (const SizeDifference& d : rep.sizeDifferent)
            logSome(QStringLiteral("Different size: %1 (A %2 bytes, B %3 bytes)").arg(d.path, formatCount(d.a), formatCount(d.b)));
        for (const ContentDifference& d : rep.contentDifferent)
            logSome(QStringLiteral("Different content: %1 (first difference at byte %2)").arg(d.path, formatCount(d.offset)));
        for (const UnreadableFile& u : rep.unreadable)
            logSome(QStringLiteral("Could not read %1: %2").arg(u.path, u.message));
        // The lists are capped; the count of all differences is not.
        if (const qint64 more = std::max<qint64>(rep.differenceCount(), logged) - 20; logged > 20 && more > 0)
            ctx.log.info(QStringLiteral("… %1 more difference%2 (see the job's result).")
                             .arg(formatCount(more), more == 1 ? QString() : QStringLiteral("s")));
        return out;
    }

private:
    // "exFAT image, 1 file, 12 folders, 48.2 GB"
    static QString treeLine(const FileTree& t)
    {
        return QStringLiteral("%1, %2 file%3, %4 folder%5, %6")
            .arg(t.describe(), formatCount(t.fileCount()), t.fileCount() == 1 ? QString() : QStringLiteral("s"),
                 formatCount(t.dirCount()), t.dirCount() == 1 ? QString() : QStringLiteral("s"), formatBytes(t.totalBytes()));
    }
    QString a() const { return m_p.value(QStringLiteral("a")).toString(); }
    QString b() const { return m_p.value(QStringLiteral("b")).toString(); }
    CompareDepth depth() const
    {
        return m_p.value(QStringLiteral("depth")).toString() == u"structure" ? CompareDepth::Structure : CompareDepth::Full;
    }
    QJsonObject m_p;
};

// The jobs whose verified image is uploaded. A packed folder (ampr.pack) is
// not an image and is never uploaded automatically.
const QStringList& buildKinds()
{
    static const QStringList k = {QStringLiteral("image.exfat.build"), QStringLiteral("image.ffpkg.build"),
                                  QStringLiteral("image.pfs.build"), kZstdBuildJob, kConvertJob};
    return k;
}

} // namespace

// ---- post-build upload -------------------------------------------------------------

PostBuildActions::PostBuildActions(JobQueue& queue, SettingsStore& settings, QObject* parent)
    : QObject(parent), m_queue(queue), m_settings(settings)
{
    connect(&m_queue, &JobQueue::jobFinished, this, &PostBuildActions::onFinished);
}

void PostBuildActions::onFinished(const QString& id)
{
    const auto rec = m_queue.record(id);
    if (!rec || rec->state != JobState::Succeeded || !rec->outcome || !buildKinds().contains(rec->kind))
        return;
    const bool enabled = rec->overrides.contains(keys::kAutoUpload) ? rec->overrides.value(keys::kAutoUpload).toBool()
                                                                    : m_settings.value(keys::kAutoUpload).toBool();
    if (!enabled)
        return;
    Log log(QStringLiteral("integration"), id);
    if (rec->outcome->verification != Verification::Passed) {
        log.warn(QStringLiteral("Not uploading to the PS5: the image was not verified."));
        return;
    }
    QJsonArray files;
    for (const QString& out : rec->outcome->outputs)
        if (QFileInfo(out).isFile())
            files.append(out);
    if (files.isEmpty())
        return;
    auto up = m_queue.enqueue(ps5::jobkinds::kUpload, QJsonObject{{QStringLiteral("sources"), files}});
    if (up.ok()) {
        log.info(QStringLiteral("Queued the upload of %1 to the PS5.").arg(QFileInfo(files.first().toString()).fileName()));
        emit uploadQueued(id, up.value());
    } else {
        log.warn(QStringLiteral("Could not queue the PS5 upload: %1").arg(up.error().message));
    }
}

void registerModule(ModuleContext& ctx)
{
    // FPKG builds from images and packed folders stage only what the package keeps.
    registerFpkgGameStager();
    registerGameJobs(ctx);

    const JobFactory* factory = &ctx.jobs;
    ctx.jobs.add(kConvertJob, QStringLiteral("Convert image"),
                 [factory](const QJsonObject& p) -> Result<std::unique_ptr<Job>> {
                     return std::unique_ptr<Job>(new ConvertJob(p, factory));
                 });
    ctx.jobs.add(kCompareJob, QStringLiteral("Compare images"), [](const QJsonObject& p) -> Result<std::unique_ptr<Job>> {
        return std::unique_ptr<Job>(new CompareJob(p));
    });

    SettingDef d;
    d.key = keys::kAutoUpload;
    d.section = QStringLiteral("PS5 connection");
    d.group = QStringLiteral("After building");
    d.label = QStringLiteral("Upload new images to the PS5");
    d.description = QStringLiteral("After an image is built and verified, queue its upload to the console's games folder over FTP. Unverified images are never uploaded.");
    d.type = SettingType::Bool;
    d.defaultValue = false;
    d.perJob = true;
    d.order = 90;
    ctx.settings.add(d);
    ctx.settings.addLegacyKey(QStringLiteral("adv_auto_upload"), keys::kAutoUpload);

    registerAmprFtpJob(ctx);
}

} // namespace fw::integration
