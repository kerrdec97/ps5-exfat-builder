#include "engines/ufs2/Ufs2Jobs.h"
#include "core/ArchiveSource.h"

#include "core/FileUtil.h"
#include "engines/general/Module.h"
#include "engines/ufs2/Module.h"
#include "engines/ufs2/Ufs2Check.h"
#include "engines/ufs2/Ufs2Edit.h"
#include "engines/ufs2/Ufs2Reader.h"
#include "engines/ufs2/Ufs2Verify.h"
#include "engines/ufs2/Ufs2Writer.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>

#include <algorithm>

namespace fw::ufs2 {

namespace {

constexpr qint64 kMiB = 1024 * 1024;

QString native(const QString& p) { return QDir::toNativeSeparators(p); }
QString absolute(const QString& p) { return p.isEmpty() ? QString() : QDir::cleanPath(QFileInfo(p).absoluteFilePath()); }

qint64 marginBytes(const SettingsValues& s) { return std::max<qint64>(0, s.integer(general::keys::kFreeSpaceMarginMiB)) * kMiB; }
qint64 ioBufferBytes(const SettingsValues& s)
{
    return std::clamp<qint64>(s.integer(general::keys::kIoBufferMiB), 1, 64) * kMiB;
}
bool verifyEnabled(const SettingsValues& s) { return s.boolean(general::keys::kVerifyAfterBuild); }
CompareDepth verifyDepth(const SettingsValues& s) { return compareDepthFromSetting(s.string(general::keys::kVerifyMode)); }

// Every live path with its size, sorted: two scans of a folder match when
// nothing was added, removed or resized in between.
QStringList listingOf(const SourceTree& t)
{
    QStringList out;
    for (size_t i = 1; i < t.nodes.size(); ++i) {
        const TreeNode& n = t.nodes[i];
        if (!n.removed)
            out << t.relativePath(int(i)) + (n.isDir() ? QStringLiteral("/") : u'\t' + QString::number(n.size));
    }
    out.sort();
    return out;
}

QString seconds(qint64 ms) { return QStringLiteral("%1 s").arg(QString::number(double(ms) / 1000.0, 'f', 1)); }

// True when the drive holding `folder` is FAT32 (or older) and `bytes` is
// over its 4 GiB file limit: checked before hours of writing fail there.
bool tooLargeForDrive(const QString& folder, qint64 bytes)
{
    if (bytes < 4096 * kMiB)
        return false;
    const auto ds = diskSpace(folder);
    if (!ds.ok())
        return false;
    const QString fs = ds->fileSystem.toLower();
    return fs == u"vfat" || fs == u"msdos" || fs.startsWith(u"fat");
}

// Gives a finished .part file its final name. When that fails (the image it
// replaces is mounted or open in another program) the finished work is not
// thrown away: it is moved to a free name next to it, "<name> (new).<ext>",
// and the returned error says so. `what` starts that message.
Status commitOrKeep(PartFile& part, bool replaceExisting, const QString& what)
{
    Status st = part.commit(replaceExisting);
    if (st.ok())
        return st;
    const QFileInfo fi(part.finalPath());
    const QString suffix = fi.suffix().isEmpty() ? QString() : u'.' + fi.suffix();
    const QString kept = uniquePath(fi.absoluteDir().filePath(fi.completeBaseName() + QStringLiteral(" (new)") + suffix));
    if (!QFileInfo::exists(part.partPath()) || !QFile::rename(part.partPath(), kept))
        return st; // nothing could be saved: the plain error stands
    Error e = st.error();
    e.detail = e.detail.isEmpty() ? e.message : e.message + u' ' + e.detail;
    if (e.code == ErrorCode::AlreadyExists)
        e.message = QStringLiteral("%1, but %2 already exists. It was saved as %3.").arg(what, native(part.finalPath()), native(kept));
    else if (fi.exists())
        e.message = QStringLiteral("%1, but could not replace %2 because that file is in use. It was saved as %3. "
                                   "Close the program that uses the old file, then rename the new one.")
                        .arg(what, native(part.finalPath()), native(kept));
    else
        e.message = QStringLiteral("%1, but could not be named %2. It was saved as %3.")
                        .arg(what, native(part.finalPath()), native(kept));
    return e;
}

QJsonObject geometryJson(const PlanSummary& s)
{
    const Geometry& g = s.geometry;
    return QJsonObject{{QStringLiteral("blockSize"), g.bsize},
                       {QStringLiteral("fragmentSize"), g.fsize},
                       {QStringLiteral("minFreePercent"), g.params.minFreePercent},
                       {QStringLiteral("bytesPerInode"), g.params.bytesPerInode},
                       {QStringLiteral("cylinderGroups"), g.ncg},
                       {QStringLiteral("inodesPerGroup"), g.ipg},
                       {QStringLiteral("fragmentsPerGroup"), g.fpg},
                       {QStringLiteral("imageBytes"), double(s.imageBytes)},
                       {QStringLiteral("ufs2toolAutoSizeBytes"), double(s.autoSizeBytes)},
                       {QStringLiteral("enlarged"), s.enlarged}};
}

struct VerifyResult {
    Verification verification = Verification::NotRun;
    QString detail;
};

void logProblems(const Log& log, const QStringList& items, const QString& what)
{
    for (int i = 0; i < items.size() && i < 20; ++i)
        log.warn(QStringLiteral("%1: %2").arg(what, items[i]));
    if (items.size() > 20)
        log.warn(QStringLiteral("%1: %2 more").arg(what).arg(items.size() - 20));
}

// Opens `imagePath`, runs the structure check and, when `compare`, compares it
// with `tree`. Damage is a failed verification, not a job error.
Result<VerifyResult> verifyImage(const QString& imagePath, const SourceTree& tree, bool compare, CompareDepth depth,
                                 JobContext& ctx, int checkPhase, int comparePhase)
{
    VerifyResult v;
    auto opened = Ufs2Reader::open(imagePath);
    if (!opened.ok()) {
        if (opened.error().code != ErrorCode::CorruptData)
            return opened.takeError();
        v.verification = Verification::Failed;
        v.detail = opened.error().message;
        return v;
    }
    const std::shared_ptr<Ufs2Reader> reader = opened.value();
    const qint64 inodes = qint64(reader->sb().inodeCount()) - reader->sb().csNifree;
    ctx.progress->beginPhase(checkPhase, std::max<qint64>(0, inodes), 0, false);
    FW_TRY_ASSIGN(CheckReport check, checkImage(*reader, ctx.token, ctx.progress));
    if (!check.ok()) {
        logProblems(ctx.log, check.problems, QStringLiteral("Structure"));
        v.verification = Verification::Failed;
        v.detail = check.summary();
        return v;
    }
    ctx.log.info(check.summary());
    if (!compare) {
        v.verification = Verification::Skipped;
        v.detail = QStringLiteral("Structure check passed. Comparing with the source is turned off in the Verification settings.");
        return v;
    }
    const bool full = depth == CompareDepth::Full;
    ctx.progress->beginPhase(comparePhase, full ? tree.totalBytes : tree.fileCount, tree.fileCount, full);
    FW_TRY_ASSIGN(CompareReport cmp, compareImage(*reader, tree, depth, ctx.token, ctx.progress, ioBufferBytes(ctx.settings)));
    logProblems(ctx.log, cmp.missing, QStringLiteral("Missing"));
    logProblems(ctx.log, cmp.extra, QStringLiteral("Unexpected"));
    logProblems(ctx.log, cmp.changed, QStringLiteral("Different"));
    v.verification = cmp.ok() ? Verification::Passed : Verification::Failed;
    v.detail = cmp.summary(depth);
    return v;
}

// ---- build ------------------------------------------------------------------------------

class BuildJob final : public Job {
public:
    explicit BuildJob(QJsonObject p) : m_p(std::move(p)) {}
    QString kind() const override { return kinds::kBuild; }
    QString title() const override
    {
        const QString name = QFileInfo(source()).fileName();
        return name.isEmpty() ? QFileInfo(output()).fileName() : name;
    }
    QString subtitle() const override { return QStringLiteral("FFPKG · %1").arg(native(output())); }
    QStringList phases() const override
    {
        return {QStringLiteral("Scanning"), QStringLiteral("Writing image"), QStringLiteral("Verifying")};
    }
    QJsonObject parameters() const override { return m_p; }

    Status validate(const SettingsValues& settings) const override
    {
        const QString src = source(), out = output();
        if (archive::isZipSource(src)) // #67: a zipped dump, unpacked in run()
            FW_TRY(archive::checkZipSource(src));
        else if (src.isEmpty() || !QFileInfo(src).isDir())
            return Error(ErrorCode::NotFound, QStringLiteral("Choose an existing game folder to build from."));
        if (out.isEmpty() || QFileInfo(out).fileName().isEmpty())
            return Error(ErrorCode::InvalidArgument, QStringLiteral("Choose where to save the .ffpkg image."));
        if (QFileInfo(out).isDir())
            return Error(ErrorCode::InvalidArgument, QStringLiteral("%1 is a folder. Choose a file name for the image.").arg(native(out)));
        if (isSameOrInside(out, src))
            return Error(ErrorCode::InvalidArgument,
                         QStringLiteral("The image cannot be saved inside the game folder it is built from. Choose another output folder."));
        if (QFileInfo::exists(out) && !replaceExisting())
            return Error(ErrorCode::AlreadyExists, QStringLiteral("%1 already exists.").arg(QFileInfo(out).fileName()));
        return validateParams(formatParams(settings));
    }

    Result<JobOutcome> run(JobContext& ctx) override
    {
        FW_TRY(validate(ctx.settings));
        if (archive::isZipSource(source())) {
            const QString zip = source();
            FW_TRY_ASSIGN(m_unpacked, archive::unpackZipSource(zip, ctx.tempDir, ctx.token, ctx.progress));
            ctx.log.info(QStringLiteral("Unpacked %1 to %2").arg(native(zip), native(m_unpacked)));
        }
        const QString src = source(), out = output();
        ProgressReporter& prog = *ctx.progress;
        JobOutcome outcome;

        prog.beginPhase(0, 0, 0, false);
        ctx.log.info(QStringLiteral("Scanning %1").arg(native(src)));
        FW_TRY_ASSIGN(SourceTree tree, scanLocalFolder(src, ctx.token));
        if (tree.fileCount == 0 && tree.dirCount == 0)
            return Error(ErrorCode::InvalidArgument, QStringLiteral("%1 is empty.").arg(native(src)));
        ctx.log.info(QStringLiteral("%1 files in %2 folders, %3").arg(formatCount(tree.fileCount), formatCount(tree.dirCount),
                                                                      formatBytes(tree.totalBytes)));

        QStringList notes;
        WriterOptions opt;
        opt.params = formatParams(ctx.settings, &notes);
        opt.ioBufferBytes = ioBufferBytes(ctx.settings);
        for (const QString& n : notes) {
            ctx.log.warn(n);
            outcome.warnings << n;
        }
        Ufs2Writer writer(tree, opt);
        FW_TRY_ASSIGN(PlanSummary plan, writer.plan());
        const Geometry& g = plan.geometry;
        ctx.log.info(QStringLiteral("UFS2 layout: %1 image, block %2, fragment %3, -m %4, -i %5, %6 cylinder groups of %7 inodes")
                         .arg(formatBytes(plan.imageBytes))
                         .arg(g.bsize)
                         .arg(g.fsize)
                         .arg(g.params.minFreePercent)
                         .arg(g.params.bytesPerInode)
                         .arg(g.ncg)
                         .arg(g.ipg));
        if (plan.enlarged) {
            const QString w = QStringLiteral("UFS2Tool's automatic size (%1) is too small for this folder, so the image was made %2.")
                                  .arg(formatBytes(plan.autoSizeBytes), formatBytes(plan.imageBytes));
            ctx.log.warn(w);
            outcome.warnings << w;
        }
        const QString outDir = QFileInfo(out).absolutePath();
        FW_TRY(ensureFreeSpace(outDir, plan.imageBytes, marginBytes(ctx.settings)));
        if (!QDir().mkpath(outDir))
            return Error(ErrorCode::IoError, QStringLiteral("Could not create the folder %1.").arg(native(outDir)));
        if (tooLargeForDrive(outDir, plan.imageBytes))
            return Error(ErrorCode::Unsupported,
                         QStringLiteral("The output drive uses FAT32, which cannot store files of 4 GB or more. "
                                        "Choose a folder on an NTFS or exFAT drive."));

        PartFile part(out);
        QFile::remove(part.partPath()); // left by an interrupted build of this image
        prog.beginPhase(1, tree.totalBytes, tree.fileCount);
        FW_TRY_ASSIGN(WriteStats ws, writer.write(part.partPath(), ctx.token, ctx.progress));
        ctx.log.info(QStringLiteral("Wrote %1 (%2 of files) in %3 — %4")
                         .arg(formatBytes(ws.imageBytes), formatBytes(ws.dataBytes), seconds(ws.elapsedMs), formatRate(ws.bytesPerSec)));

        const bool compare = verifyEnabled(ctx.settings);
        const CompareDepth depth = verifyDepth(ctx.settings);
        FW_TRY_ASSIGN(VerifyResult v, verifyImage(part.partPath(), tree, compare, depth, ctx, 2, 2));
        outcome.verification = v.verification;
        outcome.verificationDetail = v.detail;
        outcome.details = geometryJson(plan);
        outcome.details.insert(QStringLiteral("dataBytes"), double(ws.dataBytes));
        outcome.details.insert(QStringLiteral("writeMs"), double(ws.elapsedMs));
        outcome.details.insert(QStringLiteral("writeBytesPerSec"), ws.bytesPerSec);
        if (v.verification == Verification::Failed) {
            ctx.log.error(QStringLiteral("Verification failed: %1 The image was not saved.").arg(v.detail));
            outcome.summary = QStringLiteral("Not saved: the new image failed verification.");
            return outcome; // the .part file is removed with `part`
        }
        FW_TRY(commitOrKeep(part, replaceExisting(),
                            v.verification == Verification::Passed ? QStringLiteral("The image was built and verified")
                                                                   : QStringLiteral("The image was built")));
        outcome.outputs << out;
        outcome.summary = QStringLiteral("%1 · %2 · %3 files").arg(QFileInfo(out).fileName(), formatBytes(ws.imageBytes),
                                                                    formatCount(tree.fileCount));
        ctx.log.info(QStringLiteral("Saved %1").arg(native(out)));

        if (ctx.settings.boolean(keys::kDeleteSourceAfterBuild)) {
            QString why;
            if (v.verification != Verification::Passed || depth != CompareDepth::Full)
                why = QStringLiteral("the image was not verified byte for byte");
            else if (!QFileInfo(QDir(src).filePath(QStringLiteral("eboot.bin"))).isFile())
                why = QStringLiteral("the folder has no eboot.bin");
            else if (auto again = scanLocalFolder(src, ctx.token); !again.ok() || listingOf(again.value()) != listingOf(tree))
                why = QStringLiteral("files in it changed while the image was being built");
            if (!m_unpacked.isEmpty()) {
                // `src` is the unpacked copy in the job's temporary folder.
                const QString w = QStringLiteral("The ZIP %1 was kept: only folders are deleted after a build.")
                                      .arg(native(absolute(m_p.value(QStringLiteral("source")).toString())));
                outcome.warnings << w;
                ctx.log.warn(w);
            } else if (why.isEmpty()) {
                Status st = removeTreeSafely(src);
                if (st.ok()) {
                    ctx.log.info(QStringLiteral("Deleted the game folder %1").arg(native(src)));
                    outcome.details.insert(QStringLiteral("sourceDeleted"), true);
                } else {
                    outcome.warnings << st.error().message;
                    ctx.log.warn(st.error().toLogString());
                }
            } else {
                const QString w = QStringLiteral("The game folder was kept: %1.").arg(why);
                outcome.warnings << w;
                ctx.log.warn(w);
            }
        }
        return outcome;
    }

private:
    // A zipped dump is unpacked at the start of run(); from then on the game
    // folder inside the job's temp folder is the source.
    QString source() const { return m_unpacked.isEmpty() ? absolute(m_p.value(QStringLiteral("source")).toString()) : m_unpacked; }
    QString output() const { return absolute(m_p.value(QStringLiteral("output")).toString()); }
    bool replaceExisting() const { return m_p.value(QStringLiteral("replaceExisting")).toBool(false); }
    QJsonObject m_p;
    QString m_unpacked;
};

// ---- verify -----------------------------------------------------------------------------

class VerifyJob final : public Job {
public:
    explicit VerifyJob(QJsonObject p) : m_p(std::move(p)) {}
    QString kind() const override { return kinds::kVerify; }
    QString title() const override { return QFileInfo(image()).fileName(); }
    QString subtitle() const override
    {
        return source().isEmpty() ? QStringLiteral("Verify FFPKG") : QStringLiteral("Verify FFPKG against %1").arg(native(source()));
    }
    QStringList phases() const override { return {QStringLiteral("Checking structure"), QStringLiteral("Comparing")}; }
    QJsonObject parameters() const override { return m_p; }

    Status validate(const SettingsValues&) const override
    {
        if (image().isEmpty() || !QFileInfo(image()).isFile())
            return Error(ErrorCode::NotFound, QStringLiteral("Choose an existing .ffpkg image to verify."));
        if (!source().isEmpty() && !QFileInfo(source()).isDir())
            return Error(ErrorCode::NotFound, QStringLiteral("The folder %1 does not exist.").arg(native(source())));
        return success();
    }

    Result<JobOutcome> run(JobContext& ctx) override
    {
        FW_TRY(validate(ctx.settings));
        JobOutcome outcome;
        const CompareDepth depth = verifyDepth(ctx.settings);
        if (!source().isEmpty()) {
            ctx.progress->beginPhase(0, 0, 0, false);
            FW_TRY_ASSIGN(SourceTree tree, scanLocalFolder(source(), ctx.token));
            FW_TRY_ASSIGN(VerifyResult v, verifyImage(image(), tree, true, depth, ctx, 0, 1));
            outcome.verification = v.verification;
            outcome.verificationDetail = v.detail;
        } else {
            FW_TRY_ASSIGN(VerifyResult v, verifyImage(image(), SourceTree(), false, depth, ctx, 0, 1));
            outcome.verification = v.verification == Verification::Skipped ? Verification::Passed : v.verification;
            outcome.verificationDetail = v.verification == Verification::Skipped ? QStringLiteral("Structure check passed.") : v.detail;
            if (outcome.verification == Verification::Passed && depth == CompareDepth::Full) {
                // No source to compare with: read every file back instead.
                FW_TRY_ASSIGN(auto reader, Ufs2Reader::open(image()));
                FW_TRY_ASSIGN(SourceTree tree, reader->loadTree(ctx.token));
                ctx.progress->beginPhase(1, tree.totalBytes, tree.fileCount);
                auto read = readEveryFile(*reader, tree, ctx.token, ctx.progress, ioBufferBytes(ctx.settings));
                if (!read.ok()) {
                    if (read.error().code != ErrorCode::CorruptData)
                        return read.takeError();
                    outcome.verification = Verification::Failed;
                    outcome.verificationDetail = read.error().message;
                } else {
                    outcome.verificationDetail = QStringLiteral("Structure check passed and all %1 files (%2) read back without errors.")
                                                     .arg(formatCount(tree.fileCount), formatBytes(read.value()));
                }
            }
        }
        outcome.summary = outcome.verification == Verification::Passed ? QStringLiteral("Verified: %1").arg(outcome.verificationDetail)
                                                                        : QStringLiteral("Problems found: %1").arg(outcome.verificationDetail);
        if (outcome.verification == Verification::Passed)
            ctx.log.info(outcome.summary);
        else
            ctx.log.warn(outcome.summary);
        return outcome;
    }

private:
    QString image() const { return absolute(m_p.value(QStringLiteral("image")).toString()); }
    QString source() const { return absolute(m_p.value(QStringLiteral("source")).toString()); }
    QJsonObject m_p;
};

// ---- extract ----------------------------------------------------------------------------

const QString kStagingMarker = QStringLiteral(".fw-extract-staging");

class ExtractJob final : public Job {
public:
    explicit ExtractJob(QJsonObject p) : m_p(std::move(p)) {}
    QString kind() const override { return kinds::kExtract; }
    QString title() const override { return QFileInfo(image()).fileName(); }
    QString subtitle() const override { return QStringLiteral("Extract FFPKG · %1").arg(native(output())); }
    QStringList phases() const override
    {
        return {QStringLiteral("Reading image"), QStringLiteral("Extracting"), QStringLiteral("Verifying")};
    }
    QJsonObject parameters() const override { return m_p; }

    Status validate(const SettingsValues&) const override
    {
        if (image().isEmpty() || !QFileInfo(image()).isFile())
            return Error(ErrorCode::NotFound, QStringLiteral("Choose an existing .ffpkg image to extract."));
        if (output().isEmpty())
            return Error(ErrorCode::InvalidArgument, QStringLiteral("Choose a folder to extract into."));
        const QFileInfo fi(output());
        if (fi.exists() && (!fi.isDir() || !QDir(output()).isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System)))
            return Error(ErrorCode::AlreadyExists,
                         QStringLiteral("%1 already exists and is not an empty folder. Choose a new folder name.").arg(native(output())));
        return success();
    }

    Result<JobOutcome> run(JobContext& ctx) override
    {
        FW_TRY(validate(ctx.settings));
        const QString out = output();
        const QString staging = out + QStringLiteral(".part");
        JobOutcome outcome;
        ctx.progress->beginPhase(0, 0, 0, false);
        FW_TRY_ASSIGN(auto reader, Ufs2Reader::open(image()));
        QStringList skipped;
        FW_TRY_ASSIGN(SourceTree tree, reader->loadTree(ctx.token, &skipped, fsPath()));
        for (const QString& s : skipped)
            outcome.warnings << QStringLiteral("Not extracted (link or special file): %1").arg(s);
        FW_TRY(ensureFreeSpace(out, tree.totalBytes, marginBytes(ctx.settings)));
        qint64 largest = 0;
        for (const TreeNode& n : tree.nodes)
            if (!n.isDir())
                largest = std::max(largest, n.size);
        if (tooLargeForDrive(QFileInfo(out).absolutePath(), largest))
            return Error(ErrorCode::Unsupported,
                         QStringLiteral("The image holds a file of %1, and the destination drive uses FAT32, which cannot "
                                        "store files of 4 GB or more. Extract to a folder on an NTFS or exFAT drive.")
                             .arg(formatBytes(largest)));

        // Staging folder; a leftover of ours (it carries the marker) is replaced.
        if (QFileInfo::exists(staging)) {
            if (!QFileInfo::exists(QDir(staging).filePath(kStagingMarker)))
                return Error(ErrorCode::AlreadyExists, QStringLiteral("%1 already exists. Move or delete it first.").arg(native(staging)));
            FW_TRY(removeTreeSafely(staging, QFileInfo(staging).absolutePath()));
        }
        // The marker lives in the staging folder, on a drive that ignores
        // case: an entry named like it in any case would be written into the
        // marker and deleted with it.
        for (const int c : tree.liveChildren(0))
            if (tree.nodes[size_t(c)].name.compare(kStagingMarker, Qt::CaseInsensitive) == 0)
                return Error(ErrorCode::Unsupported,
                             QStringLiteral("This image holds a file named %1, which this app uses itself. Rename it in "
                                            "the image first.")
                                 .arg(tree.nodes[size_t(c)].name));
        if (!QDir().mkpath(staging) || !QFile(QDir(staging).filePath(kStagingMarker)).open(QIODevice::WriteOnly))
            return Error(ErrorCode::IoError, QStringLiteral("Could not create the folder %1.").arg(native(staging)));
        struct Cleanup {
            QString path;
            bool armed = true;
            ~Cleanup()
            {
                if (armed)
                    (void)removeTreeSafely(path, QFileInfo(path).absolutePath());
            }
        } cleanup{staging};

        ctx.progress->beginPhase(1, tree.totalBytes, tree.fileCount);
        FW_TRY_ASSIGN(ExtractReport ex, extractTree(tree, staging, ctx.token, ctx.progress, ioBufferBytes(ctx.settings)));
        for (int i = 0; i < ex.renamed.size() && i < 20; ++i)
            outcome.warnings << QStringLiteral("Renamed for this drive: %1").arg(ex.renamed[i]);
        if (ex.renamed.size() > 20)
            outcome.warnings << QStringLiteral("%1 more names were changed.").arg(ex.renamed.size() - 20);

        if (verifyEnabled(ctx.settings)) {
            const CompareDepth depth = verifyDepth(ctx.settings);
            const bool full = depth == CompareDepth::Full;
            ctx.progress->beginPhase(2, full ? tree.totalBytes : tree.fileCount, tree.fileCount, full);
            FW_TRY_ASSIGN(CompareReport cmp, verifyExtracted(tree, ex, depth, ctx.token, ctx.progress, ioBufferBytes(ctx.settings)));
            outcome.verification = cmp.ok() ? Verification::Passed : Verification::Failed;
            outcome.verificationDetail = cmp.summary(depth);
            if (!cmp.ok()) {
                logProblems(ctx.log, cmp.changed + cmp.missing, QStringLiteral("Extracted"));
                outcome.summary = QStringLiteral("Not saved: the extracted files did not match the image.");
                return outcome; // staging removed by `cleanup`
            }
        } else {
            outcome.verification = Verification::Skipped;
        }
        QFile::remove(QDir(staging).filePath(kStagingMarker));
        if ((QFileInfo(out).isDir() && !QDir().rmdir(out)) || !QDir().rename(staging, out)) {
            // The files are extracted (and verified): they are not deleted
            // because a folder name could not be taken.
            cleanup.armed = false;
            QString kept = uniquePath(out + QStringLiteral(".new"));
            if (!QDir().rename(staging, kept))
                kept = staging;
            return Error(ErrorCode::IoError,
                         QStringLiteral("The files were extracted%1, but the folder %2 could not be used because it is "
                                        "open in another program. They were saved in %3.")
                             .arg(outcome.verification == Verification::Passed ? QStringLiteral(" and verified") : QString(),
                                  native(out), native(kept)));
        }
        cleanup.armed = false;
        outcome.outputs << out;
        outcome.summary = QStringLiteral("%1 files (%2) extracted").arg(formatCount(ex.files), formatBytes(ex.bytes));
        ctx.log.info(QStringLiteral("Extracted %1 files to %2").arg(formatCount(ex.files), native(out)));
        return outcome;
    }

private:
    QString image() const { return absolute(m_p.value(QStringLiteral("image")).toString()); }
    QString output() const { return absolute(m_p.value(QStringLiteral("output")).toString()); }
    QString fsPath() const
    {
        const QString p = m_p.value(QStringLiteral("path")).toString();
        return p.isEmpty() ? QStringLiteral("/") : p;
    }
    QJsonObject m_p;
};

// ---- edit -------------------------------------------------------------------------------

class EditJob final : public Job {
public:
    explicit EditJob(QJsonObject p) : m_p(std::move(p)) {}
    QString kind() const override { return kinds::kEdit; }
    QString title() const override { return QFileInfo(image()).fileName(); }
    QString subtitle() const override
    {
        const int n = int(m_p.value(QStringLiteral("operations")).toArray().size());
        return QStringLiteral("Edit FFPKG · %1 change%2").arg(n).arg(n == 1 ? QString() : QStringLiteral("s"));
    }
    QStringList phases() const override
    {
        return {QStringLiteral("Reading image"), QStringLiteral("Writing image"), QStringLiteral("Verifying"),
                QStringLiteral("Replacing original")};
    }
    QJsonObject parameters() const override { return m_p; }

    Status validate(const SettingsValues& settings) const override
    {
        if (image().isEmpty() || !QFileInfo(image()).isFile())
            return Error(ErrorCode::NotFound, QStringLiteral("Choose an existing .ffpkg image to edit."));
        FW_TRY(parseEditOps(m_p.value(QStringLiteral("operations")).toArray()));
        return validateParams(formatParams(settings));
    }

    Result<JobOutcome> run(JobContext& ctx) override
    {
        FW_TRY(validate(ctx.settings));
        FW_TRY_ASSIGN(std::vector<EditOp> ops, parseEditOps(m_p.value(QStringLiteral("operations")).toArray()));
        const QString img = image();
        JobOutcome outcome;
        PartFile part(img);
        {
            ctx.progress->beginPhase(0, 0, 0, false);
            FW_TRY_ASSIGN(auto reader, Ufs2Reader::open(img));
            QStringList skipped;
            FW_TRY_ASSIGN(SourceTree tree, reader->loadTree(ctx.token, &skipped));
            if (!skipped.isEmpty())
                return Error(ErrorCode::Unsupported,
                             QStringLiteral("The image contains links or special files (%1) that rebuilding would drop, so it cannot be edited here.")
                                 .arg(skipped.mid(0, 3).join(QStringLiteral(", "))));
            QStringList log;
            FW_TRY(applyEdits(tree, ops, ctx.token, &log));
            for (const QString& l : log)
                ctx.log.info(l);

            QStringList notes;
            WriterOptions opt;
            opt.params = formatParams(ctx.settings, &notes);
            // The rebuilt image keeps the original's block and fragment size
            // and free-space reserve; only the inode density follows Settings.
            FormatParams kept = opt.params;
            kept.blockSize = reader->sb().bsize;
            kept.fragmentSize = reader->sb().fsize;
            kept.minFreePercent = reader->sb().minfree;
            if (validateParams(kept).ok()) {
                if (kept.blockSize != opt.params.blockSize || kept.fragmentSize != opt.params.fragmentSize)
                    ctx.log.info(QStringLiteral("Keeping the image's %1/%2 block/fragment size (Settings say %3/%4).")
                                     .arg(kept.blockSize).arg(kept.fragmentSize)
                                     .arg(opt.params.blockSize).arg(opt.params.fragmentSize));
                opt.params = kept;
            } else {
                notes << QStringLiteral("The image's block size (%1/%2) cannot be rebuilt as is; the edited image uses the "
                                        "sizes from Settings (%3/%4).")
                             .arg(kept.blockSize).arg(kept.fragmentSize).arg(opt.params.blockSize).arg(opt.params.fragmentSize);
            }
            opt.ioBufferBytes = ioBufferBytes(ctx.settings);
            for (const QString& n : notes)
                outcome.warnings << n;
            Ufs2Writer writer(tree, opt);
            FW_TRY_ASSIGN(PlanSummary plan, writer.plan());
            if (tooLargeForDrive(QFileInfo(img).absolutePath(), plan.imageBytes))
                return Error(ErrorCode::Unsupported,
                             QStringLiteral("The edited image would be %1, and its drive uses FAT32, which cannot store "
                                            "files of 4 GB or more. Move the image to an NTFS or exFAT drive first.")
                                 .arg(formatBytes(plan.imageBytes)));
            FW_TRY(ensureFreeSpace(QFileInfo(img).absolutePath(), plan.imageBytes, marginBytes(ctx.settings)));
            QFile::remove(part.partPath());
            ctx.progress->beginPhase(1, tree.totalBytes, tree.fileCount);
            FW_TRY_ASSIGN(WriteStats ws, writer.write(part.partPath(), ctx.token, ctx.progress));
            ctx.log.info(QStringLiteral("Rebuilt image: %1 in %2").arg(formatBytes(ws.imageBytes), seconds(ws.elapsedMs)));

            // The original is about to be replaced, so the new image is always
            // compared with the edited tree (at least by name and size).
            const CompareDepth depth = verifyEnabled(ctx.settings) ? verifyDepth(ctx.settings) : CompareDepth::Structure;
            FW_TRY_ASSIGN(VerifyResult v, verifyImage(part.partPath(), tree, true, depth, ctx, 2, 2));
            outcome.verification = v.verification;
            outcome.verificationDetail = v.detail;
            outcome.details = geometryJson(plan);
            outcome.details.insert(QStringLiteral("operations"), editOpsToJson(ops));
            if (v.verification == Verification::Failed) {
                outcome.summary = QStringLiteral("Not changed: the rebuilt image failed verification. The original is untouched.");
                return outcome;
            }
            // `tree` and `reader` keep the original open; they close here,
            // before the original is replaced.
        }
        ctx.progress->beginPhase(3, 0, 0, false);
        FW_TRY(commitOrKeep(part, true, QStringLiteral("The edited image was built and verified")));
        outcome.outputs << img;
        outcome.summary = QStringLiteral("%1 change%2 applied to %3").arg(ops.size()).arg(ops.size() == 1 ? QString() : QStringLiteral("s"),
                                                                           QFileInfo(img).fileName());
        ctx.log.info(outcome.summary);
        return outcome;
    }

private:
    QString image() const { return absolute(m_p.value(QStringLiteral("image")).toString()); }
    QJsonObject m_p;
};

template <typename T>
JobFactory::Creator creator()
{
    return [](const QJsonObject& p) -> Result<std::unique_ptr<Job>> { return std::unique_ptr<Job>(new T(p)); };
}

} // namespace

void registerJobs(JobFactory& jobs)
{
    jobs.add(kinds::kBuild, QStringLiteral("Build FFPKG image"), creator<BuildJob>());
    jobs.add(kinds::kVerify, QStringLiteral("Verify FFPKG image"), creator<VerifyJob>());
    jobs.add(kinds::kExtract, QStringLiteral("Extract FFPKG image"), creator<ExtractJob>());
    jobs.add(kinds::kEdit, QStringLiteral("Edit FFPKG image"), creator<EditJob>());
}

} // namespace fw::ufs2
