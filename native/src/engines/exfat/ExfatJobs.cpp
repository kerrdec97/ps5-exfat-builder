#include "engines/exfat/ExfatJobs.h"
#include "core/ArchiveSource.h"

#include "core/FileUtil.h"
#include "engines/exfat/ExfatExtract.h"
#include "engines/exfat/ExfatFormat.h"
#include "engines/exfat/ExfatReader.h"
#include "engines/exfat/ExfatVerify.h"
#include "engines/exfat/Module.h"
#include "engines/exfat/PlatformFs.h"
#include "engines/general/Module.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>

#include <algorithm>

namespace fw::exfat {

// ---- settings -----------------------------------------------------------------------

ExfatSettings readSettings(const SettingsValues& s)
{
    ExfatSettings x;
    const QString cluster = s.string(keys::kClusterSize);
    x.writer.clusterBytes = cluster == u"auto" ? 0 : cluster.toInt();
    x.writer.sectorBytes = s.string(keys::kSectorSize) == u"4096" ? 4096 : 512;
    x.writer.volumeBytes = qint64(s.real(keys::kImageSizeGiB) * double(kGiB));
    x.writer.spare = s.string(keys::kSpareSpace) == u"fixed" ? SpareMode::Fixed : SpareMode::Legacy;
    x.writer.spareBytes = s.integer(keys::kSpareSpaceMiB) * kMiB;
    x.writer.fatChains = s.string(keys::kAllocation) == u"fatChain";
    x.writer.bufferBytes = std::clamp<qint64>(s.integer(general::keys::kIoBufferMiB), 1, 64) * kMiB;
    x.writer.bufferCount = 6;
    x.writer.openRetries = int(s.integer(keys::kOpenRetries));
    x.writer.retryWaitMs = int(s.integer(keys::kRetryWaitSeconds) * 1000);
    x.rules.excludePatterns = s.stringList(keys::kExcludePatterns);
    x.rules.excludeHidden = s.boolean(keys::kExcludeHidden);
    x.requireEboot = s.boolean(keys::kRequireEboot);
    x.overwrite = s.boolean(keys::kOverwriteExisting);
    x.labelPolicy = s.string(keys::kLabelPolicy);
    x.label = s.string(keys::kLabel);
    x.deleteSource = s.boolean(keys::kDeleteSourceAfterVerify);
    x.verify = s.boolean(general::keys::kVerifyAfterBuild);
    x.fullVerify = s.string(general::keys::kVerifyMode) != u"structure";
    x.threads = general::workerThreads(s);
    x.marginBytes = std::max<qint64>(0, s.integer(general::keys::kFreeSpaceMarginMiB)) * kMiB;
    x.extractParallel = s.boolean(keys::kExtractParallel);
    x.extractThreads = int(s.integer(keys::kExtractThreads));
    x.editStrategy = s.string(keys::kEditStrategy) == u"rebuild" ? EditStrategy::Rebuild : EditStrategy::Auto;
    return x;
}

QString detectTitleId(const QString& sourceDir, const QString& outputPath)
{
    static const QRegularExpression re(QStringLiteral("(CUSA|PPSA|PPLH)[-_ ]?(\\d{5})"),
                                       QRegularExpression::CaseInsensitiveOption);
    const auto pick = [](const QString& s) -> QString {
        const auto m = re.match(s);
        return m.hasMatch() ? m.captured(1).toUpper() + m.captured(2) : QString();
    };
    QFile f(sourceDir + QStringLiteral("/sce_sys/param.json"));
    if (f.size() > 0 && f.size() < 4 * kMiB && f.open(QIODevice::ReadOnly)) {
        const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
        const QString id = pick(o.value(QStringLiteral("titleId")).toString());
        if (!id.isEmpty())
            return id;
    }
    for (const QString& s : {QFileInfo(sourceDir).fileName(), QFileInfo(outputPath).fileName()})
        if (const QString id = pick(s); !id.isEmpty())
            return id;
    return {};
}

QString resolveBuildOutput(const QString& source, const QString& output)
{
    if (output.trimmed().isEmpty())
        return {};
    QString out = QDir::cleanPath(QFileInfo(output).absoluteFilePath());
    if (output.endsWith(u'/') || output.endsWith(u'\\') || QFileInfo(out).isDir())
        out += u'/' + sanitizeFileName(QFileInfo(source).fileName()) + QStringLiteral(".exfat");
    return out;
}

namespace {

QString native(const QString& p) { return QDir::toNativeSeparators(p); }

QStringList stringList(const QJsonValue& v)
{
    QStringList out;
    for (const QJsonValue& x : v.toArray())
        if (!x.toString().trimmed().isEmpty())
            out << x.toString();
    return out;
}

Status checkImageFile(const QString& image)
{
    if (image.trimmed().isEmpty())
        return Error(ErrorCode::InvalidArgument, QStringLiteral("Choose an .exfat image."));
    const QFileInfo fi(image);
    if (!fi.exists() || !fi.isFile())
        return Error(ErrorCode::NotFound, QStringLiteral("The image %1 does not exist.").arg(native(image)));
    return success();
}

Status checkSourceFolder(const QString& source)
{
    if (source.trimmed().isEmpty())
        return Error(ErrorCode::InvalidArgument, QStringLiteral("Choose the game folder."));
    const QFileInfo fi(source);
    if (!fi.exists() || !fi.isDir())
        return Error(ErrorCode::NotFound, QStringLiteral("The folder %1 does not exist.").arg(native(source)));
    return success();
}

VerifyOptions verifyOptions(const ExfatSettings& x, JobContext& ctx, int phase)
{
    VerifyOptions vo;
    vo.full = x.fullVerify;
    vo.threads = x.threads;
    vo.bufferBytes = x.writer.bufferBytes;
    ProgressReporter* p = ctx.progress;
    vo.onCompareStart = [p, phase](qint64 total, qint64 files, bool bytes) { p->beginPhase(phase, total, files, bytes); };
    return vo;
}

void applyVerification(JobOutcome& o, const VerifyReport& r)
{
    o.verification = r.passed() ? Verification::Passed : Verification::Failed;
    o.verificationDetail = r.summary();
    o.details.insert(QStringLiteral("verification"), r.toJson());
    for (const QString& w : r.warnings)
        o.warnings << w;
}

QString rate(qint64 bytes, qint64 ms)
{
    return ms > 0 ? formatRate(double(bytes) * 1000.0 / double(ms)) : QStringLiteral("—");
}

// ---- build ---------------------------------------------------------------------------

class BuildJob final : public Job {
public:
    explicit BuildJob(QJsonObject p) : m_p(std::move(p))
    {
        m_source = m_p.value(QStringLiteral("source")).toString();
        m_output = resolveBuildOutput(m_source, m_p.value(QStringLiteral("output")).toString());
        m_label = m_p.value(QStringLiteral("label")).toString();
        m_hasLabel = m_p.contains(QStringLiteral("label"));
        m_excludes = stringList(m_p.value(QStringLiteral("excludes")));
    }
    QString kind() const override { return kJobBuild; }
    QString title() const override { return QFileInfo(m_source).fileName(); }
    QString subtitle() const override { return QStringLiteral("exFAT · %1").arg(QFileInfo(m_output).fileName()); }
    QStringList phases() const override
    {
        return {QStringLiteral("Scanning"), QStringLiteral("Writing image"), QStringLiteral("Verifying")};
    }
    QJsonObject parameters() const override { return m_p; }

    Status validate(const SettingsValues& settings) const override
    {
        const ExfatSettings x = readSettings(settings);
        const bool zip = archive::isZipSource(m_source); // #67: a zipped dump, unpacked in run()
        if (zip)
            FW_TRY(archive::checkZipSource(m_source));
        else
            FW_TRY(checkSourceFolder(m_source));
        if (m_output.isEmpty())
            return Error(ErrorCode::InvalidArgument, QStringLiteral("Choose where to save the image."));
        if (isSameOrInside(m_output, m_source))
            return Error(ErrorCode::InvalidArgument,
                         QStringLiteral("Save the image outside the folder it is built from."));
        if (!zip && x.requireEboot && !QFileInfo::exists(m_source + QStringLiteral("/eboot.bin")))
            return Error(ErrorCode::NotFound,
                         QStringLiteral("There is no eboot.bin in %1. Choose the game's top folder (the one that "
                                        "contains eboot.bin).")
                             .arg(native(m_source)));
        if (!x.overwrite && QFileInfo::exists(m_output))
            return Error(ErrorCode::AlreadyExists,
                         QStringLiteral("%1 already exists. Choose another name, or allow replacing images in Settings.")
                             .arg(native(m_output)));
        if (m_hasLabel)
            if (const QString why = checkVolumeLabel(m_label); !why.isEmpty())
                return Error(ErrorCode::InvalidArgument, QStringLiteral("The volume label cannot be used: %1.").arg(why));
        return success();
    }

    QString volumeLabel(const ExfatSettings& x) const
    {
        if (m_hasLabel)
            return m_label;
        if (x.labelPolicy == u"none")
            return {};
        if (x.labelPolicy == u"titleId")
            if (const QString id = detectTitleId(m_source, m_output); !id.isEmpty())
                return id;
        return x.label;
    }

    Result<JobOutcome> run(JobContext& ctx) override
    {
        const ExfatSettings x = readSettings(ctx.settings);
        if (archive::isZipSource(m_source)) {
            const QString zip = m_source;
            FW_TRY(validate(ctx.settings));
            FW_TRY_ASSIGN(m_source, archive::unpackZipSource(zip, ctx.tempDir, ctx.token, ctx.progress));
            ctx.log.info(QStringLiteral("Unpacked %1 to %2").arg(native(zip), native(m_source)));
        }
        FW_TRY(validate(ctx.settings));
        ctx.progress->setPhases(phases(), {0.05, 0.6, 0.35});
        QElapsedTimer timer;
        timer.start();

        // Scan.
        ctx.progress->beginPhase(0, 0);
        ScanRules rules = x.rules;
        rules.excludePatterns += m_excludes;
        FW_TRY_ASSIGN(SourceTree tree, scanSource(m_source, rules, ctx.token, ctx.progress));
        ctx.log.info(QStringLiteral("Scanned %1: %2 files, %3 folders, %4; %5 left out")
                         .arg(native(m_source), formatCount(tree.fileCount), formatCount(tree.dirCount),
                              formatBytes(tree.totalBytes), formatCount(tree.excludedCount)));
        if (!tree.excludedSamples.isEmpty())
            ctx.log.debug(QStringLiteral("Left out: %1").arg(tree.excludedSamples.join(QStringLiteral(", "))));
        JobOutcome o;
        o.warnings = tree.warnings;

        // Plan.
        WriterOptions w = x.writer;
        w.label = volumeLabel(x);
        FW_TRY_ASSIGN(ImagePlan plan, planImage(tree, w));
        const qint64 volume = plan.geo.volumeBytes();
        ctx.log.info(QStringLiteral("Image: %1, cluster %2, sector %3 bytes, label \"%4\"")
                         .arg(plan.sizeChoice, plan.clusterChoice)
                         .arg(plan.geo.sectorBytes())
                         .arg(plan.label));

        // Output drive.
        const QString outDir = QFileInfo(m_output).absolutePath();
        if (!QDir().mkpath(outDir))
            return Error(ErrorCode::IoError, QStringLiteral("Could not create the folder %1.").arg(native(outDir)));
        if (auto ds = diskSpace(outDir); ds.ok()) {
            const QString fs = ds->fileSystem.toLower();
            if ((fs == u"vfat" || fs == u"msdos" || fs.startsWith(u"fat")) && volume >= 4 * kGiB)
                return Error(ErrorCode::Unsupported,
                             QStringLiteral("The output drive uses FAT32, which cannot store files of 4 GB or more. "
                                            "Choose a folder on an NTFS or exFAT drive."));
        }
        FW_TRY(ensureFreeSpace(outDir, volume, x.marginBytes));
        if (!ctx.token.checkpoint())
            return Error::cancelled();

        // Write.
        ctx.progress->beginPhase(1, tree.totalBytes, tree.fileCount);
        PartFile part(m_output);
        FW_TRY_ASSIGN(WriteStats ws, writeImage(tree, plan, part.partPath(), w, ctx.token, ctx.progress, &ctx.log));
        ctx.log.info(QStringLiteral("Wrote %1 in %2 ms (%3)")
                         .arg(formatBytes(ws.bytesWritten))
                         .arg(ws.elapsedMs)
                         .arg(rate(ws.bytesWritten, ws.elapsedMs)));

        // Verify the finished image before it gets its final name (audit M1).
        if (x.verify) {
            ctx.progress->beginPhase(2, 0);
            FW_TRY_ASSIGN(VerifyReport rep, verifyImageAgainstFolder(part.partPath(), m_source, rules,
                                                                      verifyOptions(x, ctx, 2), ctx.token, ctx.progress));
            applyVerification(o, rep);
            ctx.log.info(QStringLiteral("Verification (%1): %2").arg(x.fullVerify ? QStringLiteral("full") : QStringLiteral("structure"), rep.summary()));
            if (!rep.passed()) {
                o.summary = QStringLiteral("The image was not saved because it did not verify.");
                return o; // the .part is removed; an older image stays in place
            }
        } else {
            o.verification = Verification::Skipped;
            o.verificationDetail = QStringLiteral("Verification is turned off in Settings.");
        }

        FW_TRY(commitOrKeep(part, x.overwrite,
                            x.verify ? QStringLiteral("The image was built and verified")
                                     : QStringLiteral("The image was built")));
        o.outputs << m_output;
        // An undo journal of the image that was just replaced must never be
        // applied to the new one.
        for (const QString& j : {editJournalPath(m_output), editJournalPath(m_output) + QStringLiteral(".part")})
            if (QFileInfo::exists(j) && QFile::remove(j))
                ctx.log.info(QStringLiteral("Removed %1, left by an interrupted edit of the image that was replaced.").arg(native(j)));

        // Delete the source only after a full content-verified pass (audit C2).
        if (x.deleteSource) {
            if (o.verification != Verification::Passed || !x.fullVerify) {
                o.warnings << QStringLiteral("The source folder was kept: it is deleted only after a full byte-for-byte "
                                             "verification.");
            } else if (tree.excludedCount > 0) {
                o.warnings << QStringLiteral("The source folder was kept: %1 items in it were left out of the image.")
                                  .arg(formatCount(tree.excludedCount));
            } else if (const Status st = removeTreeSafely(m_source); !st.ok()) {
                o.warnings << QStringLiteral("The source folder could not be deleted: %1").arg(st.error().message);
            } else {
                ctx.log.info(QStringLiteral("Deleted the source folder %1 after verification.").arg(native(m_source)));
                o.details.insert(QStringLiteral("sourceDeleted"), true);
            }
        }

        o.summary = QStringLiteral("%1 image, %2 files (%3)")
                        .arg(formatBytes(volume), formatCount(tree.fileCount), formatBytes(tree.totalBytes));
        o.details.insert(QStringLiteral("image"), m_output);
        o.details.insert(QStringLiteral("volumeBytes"), double(volume));
        o.details.insert(QStringLiteral("clusterBytes"), double(plan.geo.clusterBytes()));
        o.details.insert(QStringLiteral("sectorBytes"), double(plan.geo.sectorBytes()));
        o.details.insert(QStringLiteral("label"), plan.label);
        o.details.insert(QStringLiteral("serial"), QStringLiteral("%1").arg(plan.serial, 8, 16, QLatin1Char('0')));
        o.details.insert(QStringLiteral("files"), double(tree.fileCount));
        o.details.insert(QStringLiteral("folders"), double(tree.dirCount));
        o.details.insert(QStringLiteral("dataBytes"), double(tree.totalBytes));
        o.details.insert(QStringLiteral("excluded"), double(tree.excludedCount));
        o.details.insert(QStringLiteral("writeMs"), double(ws.elapsedMs));
        o.details.insert(QStringLiteral("totalMs"), double(timer.elapsed()));
        return o;
    }

private:
    QJsonObject m_p;
    QString m_source, m_output, m_label;
    bool m_hasLabel = false;
    QStringList m_excludes;
};

// ---- verify --------------------------------------------------------------------------------

class VerifyJob final : public Job {
public:
    explicit VerifyJob(QJsonObject p) : m_p(std::move(p))
    {
        m_image = m_p.value(QStringLiteral("image")).toString();
        m_source = m_p.value(QStringLiteral("source")).toString();
        m_excludes = stringList(m_p.value(QStringLiteral("excludes")));
    }
    QString kind() const override { return kJobVerify; }
    QString title() const override { return QFileInfo(m_image).fileName(); }
    QString subtitle() const override
    {
        return m_source.isEmpty() ? QStringLiteral("Check the image on its own")
                                  : QStringLiteral("Verify against %1").arg(native(m_source));
    }
    QStringList phases() const override { return {QStringLiteral("Reading image"), QStringLiteral("Comparing")}; }
    QJsonObject parameters() const override { return m_p; }
    Status validate(const SettingsValues&) const override
    {
        FW_TRY(checkImageFile(m_image));
        if (m_source.trimmed().isEmpty())
            return success(); // standalone check: structure (+ every byte readable in full mode)
        return checkSourceFolder(m_source);
    }
    Result<JobOutcome> run(JobContext& ctx) override
    {
        FW_TRY(validate(ctx.settings));
        const ExfatSettings x = readSettings(ctx.settings);
        ctx.progress->setPhases(phases(), {0.1, 0.9});
        ctx.progress->beginPhase(0, 0);
        FW_TRY_ASSIGN(const bool recovered, recoverInterruptedEdit(m_image, &ctx.log));
        if (m_source.trimmed().isEmpty())
            return runStandalone(ctx, recovered);
        ScanRules rules = x.rules;
        rules.excludePatterns += m_excludes;
        FW_TRY_ASSIGN(VerifyReport rep, verifyImageAgainstFolder(m_image, m_source, rules, verifyOptions(x, ctx, 1),
                                                                  ctx.token, ctx.progress));
        JobOutcome o;
        if (recovered)
            o.warnings << QStringLiteral("An interrupted edit of this image was undone before verifying it.");
        applyVerification(o, rep);
        o.summary = rep.passed() ? QStringLiteral("The image matches the folder.") : QStringLiteral("The image does not match the folder.");
        ctx.log.info(rep.summary());
        return o;
    }

private:
    // Without a source folder: the structure must be intact, and in "full"
    // mode every file must read back to its full length without errors.
    Result<JobOutcome> runStandalone(JobContext& ctx, bool recovered)
    {
        FW_TRY_ASSIGN(auto reader, ExfatReader::open(m_image));
        FW_TRY_ASSIGN(ImageTree tree, reader->readTree(ctx.token, true));
        JobOutcome o;
        if (recovered)
            o.warnings << QStringLiteral("An interrupted edit of this image was undone before verifying it.");
        o.warnings << tree.warnings;
        if (!tree.problems.isEmpty()) {
            o.verification = Verification::Failed;
            o.verificationDetail = QStringLiteral("The image is damaged: %1").arg(tree.problems.first());
            o.details.insert(QStringLiteral("problems"), QJsonArray::fromStringList(tree.problems));
            o.summary = QStringLiteral("The image is damaged.");
            return o;
        }
        const bool full = ctx.settings.string(QStringLiteral("verification.mode")) != QStringLiteral("structure");
        qint64 files = 0, bytes = 0;
        for (const ImageEntry& e : tree.entries)
            if (!e.isDir) {
                ++files;
                bytes += e.size;
            }
        if (full) {
            ctx.progress->beginPhase(1, bytes, files);
            ImageFileStream stream(m_image, reader->geometry());
            FW_TRY(stream.open());
            QByteArray buf;
            buf.resize(4 * 1024 * 1024);
            for (const ImageEntry& e : tree.entries) {
                if (e.isDir)
                    continue;
                qint64 off = 0;
                while (off < e.size) {
                    if (!ctx.token.checkpoint())
                        return Error::cancelled();
                    auto n = stream.read(e, off, buf.data(), buf.size());
                    if (!n.ok() || n.value() <= 0) {
                        o.verification = Verification::Failed;
                        o.verificationDetail = QStringLiteral("%1 could not be read back.").arg(e.path);
                        o.summary = QStringLiteral("The image is damaged.");
                        return o;
                    }
                    off += n.value();
                    ctx.progress->advance(n.value());
                }
                ctx.progress->advanceFiles(1);
            }
        }
        o.verification = Verification::Passed;
        o.verificationDetail = full ? QStringLiteral("Structure intact; %1 files (%2) read back without errors.").arg(formatCount(files), formatBytes(bytes))
                                    : QStringLiteral("Structure intact; %1 files.").arg(formatCount(files));
        o.summary = QStringLiteral("The image is intact.");
        return o;
    }

    QJsonObject m_p;
    QString m_image, m_source;
    QStringList m_excludes;
};

// ---- extract ---------------------------------------------------------------------------------

const QString kExtractMarker = QStringLiteral(".fw-extract-in-progress");

class ExtractJob final : public Job {
public:
    explicit ExtractJob(QJsonObject p) : m_p(std::move(p))
    {
        m_image = m_p.value(QStringLiteral("image")).toString();
        const QString out = m_p.value(QStringLiteral("output")).toString();
        m_output = out.trimmed().isEmpty() ? QString() : QDir::cleanPath(QFileInfo(out).absoluteFilePath());
    }
    QString kind() const override { return kJobExtract; }
    QString title() const override { return QFileInfo(m_image).fileName(); }
    QString subtitle() const override { return QStringLiteral("Extract to %1").arg(native(m_output)); }
    QStringList phases() const override
    {
        return {QStringLiteral("Reading image"), QStringLiteral("Extracting"), QStringLiteral("Verifying")};
    }
    QJsonObject parameters() const override { return m_p; }
    Status validate(const SettingsValues&) const override
    {
        FW_TRY(checkImageFile(m_image));
        if (m_output.isEmpty())
            return Error(ErrorCode::InvalidArgument, QStringLiteral("Choose the folder to extract to."));
        const QFileInfo fi(m_output);
        // Hidden entries count too (a desktop.ini would make the final rename fail after the whole extraction).
        if (fi.exists() && (!fi.isDir() || !QDir(m_output).isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System)))
            return Error(ErrorCode::AlreadyExists,
                         QStringLiteral("%1 already exists and is not empty. Choose a new or empty folder.").arg(native(m_output)));
        if (isSameOrInside(m_output, m_image) || isSameOrInside(m_image, m_output))
            return Error(ErrorCode::InvalidArgument, QStringLiteral("Extract to a folder that does not contain the image."));
        return success();
    }
    Result<JobOutcome> run(JobContext& ctx) override
    {
        FW_TRY(validate(ctx.settings));
        const ExfatSettings x = readSettings(ctx.settings);
        ctx.progress->setPhases(phases(), {0.05, 0.6, 0.35});
        ctx.progress->beginPhase(0, 0);
        FW_TRY_ASSIGN(const bool recovered, recoverInterruptedEdit(m_image, &ctx.log));
        FW_TRY_ASSIGN(std::unique_ptr<ExfatReader> reader, ExfatReader::open(m_image));
        FW_TRY_ASSIGN(ImageTree tree, reader->readTree(ctx.token, true));
        FW_TRY(ExfatReader::requireIntact(tree));
        if (tree.find(kExtractMarker) >= 0)
            return Error(ErrorCode::Unsupported, QStringLiteral("This image holds a file named %1, which this app "
                                                                "uses itself. Rename it in the image first.").arg(kExtractMarker));

        // Work in "<output>.part"; a leftover one is ours only if it has our marker.
        const QString stage = m_output + QStringLiteral(".part");
        const QString parent = QFileInfo(m_output).absolutePath();
        if (QFileInfo::exists(stage)) {
            if (!QFileInfo::exists(stage + u'/' + kExtractMarker))
                return Error(ErrorCode::AlreadyExists,
                             QStringLiteral("%1 already exists. Move or delete it, then try again.").arg(native(stage)));
            FW_TRY(removeTreeSafely(stage, parent));
        }
        if (!QDir().mkpath(parent))
            return Error(ErrorCode::IoError, QStringLiteral("Could not create the folder %1.").arg(native(parent)));
        FW_TRY(ensureFreeSpace(parent, tree.totalBytes + (tree.dirCount + tree.fileCount) * 4096, x.marginBytes));
        if (!QDir().mkpath(stage) || !QFile(stage + u'/' + kExtractMarker).open(QIODevice::WriteOnly))
            return Error(ErrorCode::IoError, QStringLiteral("Could not create the folder %1.").arg(native(stage)));
        struct StageGuard {
            QString path, parent;
            bool keep = false;
            ~StageGuard()
            {
                if (!keep)
                    (void)removeTreeSafely(path, parent);
            }
        } guard{stage, parent};

        ctx.progress->beginPhase(1, tree.totalBytes, tree.fileCount);
        ExtractOptions eo;
        eo.parallel = x.extractParallel;
        eo.threads = x.extractThreads;
        eo.bufferBytes = x.writer.bufferBytes;
        FW_TRY_ASSIGN(ExtractStats st, extractTree(*reader, tree, stage, eo, ctx.token, ctx.progress));
        ctx.log.info(QStringLiteral("Extracted %1 files, %2 in %3 ms (%4)")
                         .arg(formatCount(st.files), formatBytes(st.bytes))
                         .arg(st.elapsedMs)
                         .arg(rate(st.bytes, st.elapsedMs)));
        JobOutcome o;
        o.warnings = st.warnings;
        for (int i = 0; i < st.renamed.size() && i < 20; ++i)
            o.warnings << QStringLiteral("Renamed for this drive: %1").arg(st.renamed[i]);
        if (st.renamed.size() > 20)
            o.warnings << QStringLiteral("%1 more names were changed.").arg(st.renamed.size() - 20);
        for (const QString& n : st.renamed)
            ctx.log.info(QStringLiteral("Renamed for this drive: %1").arg(n));
        if (recovered)
            o.warnings << QStringLiteral("An interrupted edit of this image was undone before extracting it.");
        if (!QFile::remove(stage + u'/' + kExtractMarker))
            return Error(ErrorCode::IoError, QStringLiteral("Could not finish the extraction folder."));

        if (x.verify) {
            ctx.progress->beginPhase(2, 0);
            FW_TRY_ASSIGN(SourceTree out, scanSource(stage, {}, ctx.token));
            // Compare renamed entries under their names in the image.
            for (SourceNode& n : out.nodes)
                if (const auto it = st.imagePath.constFind(n.relativePath); it != st.imagePath.cend())
                    n.relativePath = *it;
            FW_TRY_ASSIGN(VerifyReport rep, verifyImage(m_image, expectedFromFolder(out), verifyOptions(x, ctx, 2),
                                                         ctx.token, ctx.progress));
            applyVerification(o, rep);
            if (!rep.passed()) {
                o.summary = QStringLiteral("Nothing was kept because the extracted files did not verify.");
                return o;
            }
        } else {
            o.verification = Verification::Skipped;
            o.verificationDetail = QStringLiteral("Verification is turned off in Settings.");
        }
        reader.reset();
        if ((QFileInfo(m_output).isDir() && !QDir().rmdir(m_output)) || !QDir().rename(stage, m_output)) {
            // The files are extracted (and verified): they are not deleted
            // because a folder name could not be taken.
            guard.keep = true;
            const QString kept = keepStagedFolder(stage, m_output);
            return Error(ErrorCode::IoError,
                         QStringLiteral("The files were extracted%1, but the folder %2 could not be used because it is "
                                        "open in another program. They were saved in %3.")
                             .arg(x.verify ? QStringLiteral(" and verified") : QString(), native(m_output), native(kept)));
        }
        guard.keep = true;
        o.outputs << m_output;
        o.summary = QStringLiteral("%1 files, %2").arg(formatCount(st.files), formatBytes(st.bytes));
        o.details.insert(QStringLiteral("files"), double(st.files));
        o.details.insert(QStringLiteral("folders"), double(st.dirs));
        o.details.insert(QStringLiteral("bytes"), double(st.bytes));
        o.details.insert(QStringLiteral("extractMs"), double(st.elapsedMs));
        return o;
    }

private:
    QJsonObject m_p;
    QString m_image, m_output;
};

// ---- edit ------------------------------------------------------------------------------------

class EditJob final : public Job {
public:
    explicit EditJob(QJsonObject p) : m_p(std::move(p))
    {
        m_image = m_p.value(QStringLiteral("image")).toString();
        m_ops = parseEditOps(m_p.value(QStringLiteral("operations")).toArray());
    }
    QString kind() const override { return kJobEdit; }
    QString title() const override { return QFileInfo(m_image).fileName(); }
    QString subtitle() const override
    {
        const qsizetype n = m_ops.ok() ? qsizetype(m_ops->size()) : 0;
        return n == 1 ? QStringLiteral("Edit · 1 change") : QStringLiteral("Edit · %1 changes").arg(n);
    }
    QStringList phases() const override
    {
        return {QStringLiteral("Planning"), QStringLiteral("Writing"), QStringLiteral("Verifying")};
    }
    QJsonObject parameters() const override { return m_p; }
    Status validate(const SettingsValues&) const override
    {
        FW_TRY(checkImageFile(m_image));
        if (!m_ops.ok())
            return m_ops.error();
        for (const EditOp& op : m_ops.value())
            if ((op.kind == EditOp::Kind::Add || op.kind == EditOp::Kind::Replace) && !QFileInfo::exists(op.localPath))
                return Error(ErrorCode::NotFound, QStringLiteral("%1 does not exist.").arg(native(op.localPath)));
        return success();
    }
    Result<JobOutcome> run(JobContext& ctx) override
    {
        FW_TRY(validate(ctx.settings));
        const ExfatSettings x = readSettings(ctx.settings);
        ctx.progress->setPhases(phases(), {0.05, 0.6, 0.35});
        EditOptions eo;
        eo.strategy = x.editStrategy;
        eo.rules = x.rules;
        eo.writer = x.writer;
        eo.writer.volumeBytes = 0;
        eo.verify.full = x.fullVerify;
        eo.verify.threads = x.threads;
        eo.verify.bufferBytes = x.writer.bufferBytes;
        eo.freeSpaceMarginBytes = x.marginBytes;
        ProgressReporter* p = ctx.progress;
        eo.onPhase = [p](int phase, qint64 total, qint64 files, bool bytes) { p->beginPhase(phase, total, files, bytes); };
        FW_TRY_ASSIGN(EditResult r, editImage(m_image, m_ops.value(), eo, ctx.token, ctx.progress, &ctx.log));
        JobOutcome o;
        o.warnings = r.warnings;
        const bool changed = r.added + r.replaced + r.deleted + r.foldersAdded > 0;
        if (!changed) {
            o.verification = Verification::NotRun;
            o.verificationDetail = QStringLiteral("Nothing needed to change.");
        } else {
            applyVerification(o, r.verification);
        }
        if (r.restored) {
            o.verificationDetail += QStringLiteral(" The image was left unchanged.");
            o.summary = QStringLiteral("The edit was not kept because it did not verify.");
        } else {
            o.summary = r.summary();
            o.outputs << m_image;
        }
        o.details.insert(QStringLiteral("inPlace"), r.inPlace);
        o.details.insert(QStringLiteral("oldVolumeBytes"), double(r.oldVolumeBytes));
        o.details.insert(QStringLiteral("newVolumeBytes"), double(r.newVolumeBytes));
        o.details.insert(QStringLiteral("added"), double(r.added));
        o.details.insert(QStringLiteral("foldersAdded"), double(r.foldersAdded));
        o.details.insert(QStringLiteral("replaced"), double(r.replaced));
        o.details.insert(QStringLiteral("deleted"), double(r.deleted));
        o.details.insert(QStringLiteral("recoveredJournal"), r.recoveredJournal);
        ctx.log.info(o.summary);
        return o;
    }

private:
    QJsonObject m_p;
    QString m_image;
    Result<std::vector<EditOp>> m_ops = std::vector<EditOp>{};
};

template <typename J>
JobFactory::Creator creator()
{
    return [](const QJsonObject& p) -> Result<std::unique_ptr<Job>> { return std::unique_ptr<Job>(new J(p)); };
}

} // namespace

void registerJobKinds(JobFactory& jobs)
{
    jobs.add(kJobBuild, QStringLiteral("Build exFAT image"), creator<BuildJob>());
    jobs.add(kJobVerify, QStringLiteral("Verify exFAT image"), creator<VerifyJob>());
    jobs.add(kJobExtract, QStringLiteral("Extract exFAT image"), creator<ExtractJob>());
    jobs.add(kJobEdit, QStringLiteral("Edit exFAT image"), creator<EditJob>());
}

} // namespace fw::exfat
