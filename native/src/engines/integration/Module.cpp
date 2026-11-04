#include "engines/integration/Module.h"

#include "connect/ps5/Module.h"
#include "engines/integration/AmprFtp.h"
#include "core/FileUtil.h"
#include "engines/exfat/ExfatExtract.h"
#include "engines/exfat/ExfatReader.h"
#include "engines/fpkg/SourceStager.h"
#include "engines/pfs/PfsVerify.h"
#include "engines/ufs2/Ufs2Reader.h"
#include "engines/ufs2/Ufs2Verify.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>

namespace fw::integration {

namespace {

// ---- FPKG source stagers ----------------------------------------------------------

class ExfatStager final : public fpkg::SourceStager {
public:
    QString displayName() const override { return QStringLiteral("exFAT image"); }
    QStringList extensions() const override { return {QStringLiteral("exfat")}; }
    Status probe(const QString& image) const override
    {
        auto r = exfat::ExfatReader::open(image);
        return r.ok() ? success() : Status(r.takeError());
    }
    Status stage(const QString& image, const QString& destination, const CancellationToken& token,
                 ProgressReporter* progress) override
    {
        FW_TRY_ASSIGN(auto reader, exfat::ExfatReader::open(image));
        FW_TRY_ASSIGN(exfat::ImageTree tree, reader->readTree(token, true));
        FW_TRY(exfat::ExfatReader::requireIntact(tree));
        qint64 total = 0;
        for (const auto& e : tree.entries)
            if (!e.isDir)
                total += e.size;
        if (progress)
            progress->setTotal(total);
        exfat::ExtractOptions opt;
        FW_TRY(exfat::extractTree(*reader, tree, destination, opt, token, progress));
        return success();
    }
};

class Ufs2Stager final : public fpkg::SourceStager {
public:
    QString displayName() const override { return QStringLiteral("FFPKG (UFS2) image"); }
    QStringList extensions() const override { return {QStringLiteral("ffpkg")}; }
    Status probe(const QString& image) const override
    {
        auto r = ufs2::Ufs2Reader::open(image);
        return r.ok() ? success() : Status(r.takeError());
    }
    Status stage(const QString& image, const QString& destination, const CancellationToken& token,
                 ProgressReporter* progress) override
    {
        FW_TRY_ASSIGN(auto reader, ufs2::Ufs2Reader::open(image));
        FW_TRY_ASSIGN(ufs2::SourceTree tree, reader->loadTree(token));
        FW_TRY(ufs2::extractTree(tree, destination, token, progress));
        return success();
    }
};

class PfsStager final : public fpkg::SourceStager {
public:
    QString displayName() const override { return QStringLiteral("PFS image"); }
    QStringList extensions() const override { return {QStringLiteral("ffpfs"), QStringLiteral("ffpfsc")}; }
    Status stage(const QString& image, const QString& destination, const CancellationToken& token,
                 ProgressReporter* progress) override
    {
        pfs::ExtractOptions opt;
        opt.deep = true;
        opt.overwrite = true; // the staging folder exists and is empty
        pfs::ProgressHooks hooks;
        if (progress) {
            hooks.advance = [progress](qint64 n) { progress->advance(n); };
            hooks.item = [progress](const QString& item) { progress->setCurrentItem(item); };
        }
        FW_TRY(pfs::extractImage(image, destination, opt, token, hooks));
        return success();
    }
};

// ---- image.convert ------------------------------------------------------------------

QString formatOfFile(const QString& path)
{
    const QString ext = QFileInfo(path).suffix().toLower();
    if (ext == u"exfat")
        return QStringLiteral("exfat");
    if (ext == u"ffpkg")
        return QStringLiteral("ffpkg");
    if (ext == u"ffpfs" || ext == u"ffpfsc")
        return QStringLiteral("ffpfsc");
    return {};
}

QString engineOf(const QString& format)
{
    if (format == u"exfat")
        return QStringLiteral("image.exfat");
    if (format == u"ffpkg")
        return QStringLiteral("image.ffpkg");
    if (format == u"ffpfsc")
        return QStringLiteral("image.pfs");
    return {};
}

class ConvertJob final : public Job {
public:
    ConvertJob(QJsonObject p, const JobFactory* factory) : m_p(std::move(p)), m_factory(factory) {}
    QString kind() const override { return kConvertJob; }
    QString title() const override { return QFileInfo(image()).fileName(); }
    QString subtitle() const override
    {
        return QStringLiteral("Convert to .%1 → %2").arg(target(), QDir::toNativeSeparators(QFileInfo(output()).absolutePath()));
    }
    QStringList phases() const override { return {QStringLiteral("Extracting"), QStringLiteral("Building")}; }
    QJsonObject parameters() const override { return m_p; }

    Status validate(const SettingsValues&) const override
    {
        const QString from = formatOfFile(image());
        if (!QFileInfo(image()).isFile() || from.isEmpty())
            return Error(ErrorCode::InvalidArgument, QStringLiteral("Choose an .exfat, .ffpkg or .ffpfsc image to convert."));
        if (engineOf(target()).isEmpty())
            return Error(ErrorCode::InvalidArgument, QStringLiteral("Choose the format to convert to."));
        if (from == target())
            return Error(ErrorCode::InvalidArgument, QStringLiteral("The image is already in that format."));
        if (output().isEmpty())
            return Error(ErrorCode::InvalidArgument, QStringLiteral("Choose where to save the converted image."));
        if (QFileInfo(output()).absoluteFilePath() == QFileInfo(image()).absoluteFilePath())
            return Error(ErrorCode::InvalidArgument, QStringLiteral("The converted image cannot replace its source."));
        if (!m_factory || !m_factory->contains(engineOf(from) + QStringLiteral(".extract"))
            || !m_factory->contains(engineOf(target()) + QStringLiteral(".build")))
            return Error(ErrorCode::Unsupported, QStringLiteral("Converting between these formats is not available in this build."));
        return success();
    }

    Result<JobOutcome> run(JobContext& ctx) override
    {
        FW_TRY(validate(ctx.settings));
        const QString stage = QDir(ctx.tempDir).filePath(QStringLiteral("converted"));
        ctx.log.info(QStringLiteral("Converting %1 to .%2").arg(QFileInfo(image()).fileName(), target()));

        // 1. Extract with the source format's engine (verifies what it wrote).
        QJsonObject ex{{QStringLiteral("image"), image()}, {QStringLiteral("output"), stage}};
        if (formatOfFile(image()) == u"ffpfsc")
            ex.insert(QStringLiteral("deep"), true);
        FW_TRY_ASSIGN(auto extract, m_factory->create(engineOf(formatOfFile(image())) + QStringLiteral(".extract"), ex));
        FW_TRY(extract->validate(ctx.settings));
        FW_TRY_ASSIGN(JobOutcome extracted, extract->run(ctx));
        if (extracted.verification == Verification::Failed)
            return Error(ErrorCode::VerificationFailed, QStringLiteral("The source image could not be read back correctly. %1").arg(extracted.verificationDetail));

        // 2. Build with the target engine (writes .part, verifies, renames).
        QJsonObject build{{QStringLiteral("source"), stage}, {QStringLiteral("output"), output()}};
        if (target() == u"ffpfsc")
            build.insert(QStringLiteral("format"), QStringLiteral("ffpfsc"));
        FW_TRY_ASSIGN(auto builder, m_factory->create(engineOf(target()) + QStringLiteral(".build"), build));
        FW_TRY(builder->validate(ctx.settings));
        FW_TRY_ASSIGN(JobOutcome built, builder->run(ctx));
        built.warnings = extracted.warnings + built.warnings;
        built.details.insert(QStringLiteral("convertedFrom"), image());
        built.summary = QStringLiteral("Converted to %1. %2").arg(QFileInfo(output()).fileName(), built.summary);
        return built;
    }

private:
    QString image() const { return m_p.value(QStringLiteral("image")).toString(); }
    QString target() const { return m_p.value(QStringLiteral("format")).toString().toLower(); }
    QString output() const { return m_p.value(QStringLiteral("output")).toString(); }
    QJsonObject m_p;
    const JobFactory* m_factory;
};

const QStringList& buildKinds()
{
    static const QStringList k = {QStringLiteral("image.exfat.build"), QStringLiteral("image.ffpkg.build"),
                                  QStringLiteral("image.pfs.build"), kConvertJob};
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
    fpkg::registerSourceStager(std::make_shared<ExfatStager>());
    fpkg::registerSourceStager(std::make_shared<Ufs2Stager>());
    fpkg::registerSourceStager(std::make_shared<PfsStager>());

    const JobFactory* factory = &ctx.jobs;
    ctx.jobs.add(kConvertJob, QStringLiteral("Convert image"),
                 [factory](const QJsonObject& p) -> Result<std::unique_ptr<Job>> {
                     return std::unique_ptr<Job>(new ConvertJob(p, factory));
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
