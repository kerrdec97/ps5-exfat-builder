// ampr.index.local — the AMPR index tool for a local app0 folder.
// Row rules: drakmor ampr_emu tools/build_ampr_index.py (GPL-3.0), see AmprIndex.h.

#include "engines/pfs/AmprJobs.h"

#include "core/FileUtil.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>

namespace fw::pfs {

namespace {

QString nativeFolder(const QString& f)
{
    return QDir::toNativeSeparators(f);
}

class AmprLocalJob final : public Job {
public:
    explicit AmprLocalJob(QJsonObject p) : m_p(std::move(p)) {}
    QString kind() const override { return kAmprLocalJob; }
    QString title() const override { return QStringLiteral("AMPR index for %1").arg(QFileInfo(folder()).fileName()); }
    QString subtitle() const override { return nativeFolder(folder()); }
    QStringList phases() const override { return {QStringLiteral("Scanning"), QStringLiteral("Writing the index")}; }
    QJsonObject parameters() const override { return m_p; }

    Status validate(const SettingsValues&) const override
    {
        const QString f = folder();
        if (f.isEmpty())
            return Error(ErrorCode::InvalidArgument, QStringLiteral("Choose the game's app0 folder."));
        if (!QFileInfo(f).isDir())
            return Error(ErrorCode::NotFound, QStringLiteral("The folder %1 does not exist.").arg(nativeFolder(f)));
        if (!m_p.value(QStringLiteral("allowAssetPacks")).toBool())
            if (const QString pack = folderAmprAssetPack(f); !pack.isEmpty())
                return Error(ErrorCode::InvalidArgument, amprAssetPackProblem(nativeFolder(f), pack));
        if (!m_p.value(QStringLiteral("force")).toBool() && !folderHasAmprLibrary(f))
            return Error(ErrorCode::InvalidArgument,
                         QStringLiteral("This folder has no fakelib/libSceAmpr.sprx, so it does not appear to be an APR "
                                        "title. Generate ampr_emu.index anyway?"));
        return success();
    }

    Result<JobOutcome> run(JobContext& ctx) override
    {
        const QString root = QFileInfo(folder()).absoluteFilePath();
        ctx.progress->beginPhase(0, 0);
        ctx.progress->setCurrentItem(nativeFolder(root));
        FW_TRY_ASSIGN(AmprLocalScan scan, scanAmprFolder(root, ctx.token));
        const AmprToolRows rows = amprToolRows(scan.files);
        if (rows.rows.empty())
            return Error(ErrorCode::InvalidArgument, QStringLiteral("%1 has no files to index.").arg(nativeFolder(root)));
        const QByteArray index = buildAmprIndex(rows.rows);
        ctx.log.info(QStringLiteral("[AMPR] %1 file(s) in %2 folder(s) indexed, %3").arg(rows.rows.size()).arg(scan.folders).arg(formatBytes(index.size())));

        ctx.progress->beginPhase(1, index.size());
        const QString out = QDir(root).filePath(kAmprIndexName);
        FW_TRY(ensureFreeSpace(root, index.size(), 0));
        {
            PartFile part(out);
            QFile f(part.partPath());
            if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
                return Error(ErrorCode::IoError, QStringLiteral("Could not write %1.").arg(nativeFolder(part.partPath())), f.errorString());
            if (f.write(index) != index.size())
                return Error(ErrorCode::IoError, QStringLiteral("Could not write %1.").arg(nativeFolder(part.partPath())), f.errorString());
            FW_TRY(flushToDisk(f, kAmprIndexName));
            f.close();
            if (!ctx.token.checkpoint())
                return Error::cancelled();
            FW_TRY(part.commit(true));
        }
        ctx.progress->advance(index.size());

        JobOutcome o;
        o.outputs << out;
        describeAmprRows(rows, o);
        o.details.insert(QStringLiteral("folders"), scan.folders);
        o.details.insert(QStringLiteral("hasAmprLibrary"), scan.hasAmprLibrary);
        if (!scan.hasAmprLibrary)
            o.warnings << QStringLiteral("The folder has no fakelib/libSceAmpr.sprx; the index is only used by the AMPR emulator.");
        QFile back(out);
        if (back.open(QIODevice::ReadOnly) && back.readAll() == index) {
            o.verification = Verification::Passed;
            o.verificationDetail = QStringLiteral("ampr_emu.index read back, %1").arg(formatBytes(index.size()));
        } else {
            o.verification = Verification::Failed;
            o.verificationDetail = QStringLiteral("ampr_emu.index does not read back as written");
        }
        o.summary = QStringLiteral("Indexed %1 file(s) into %2 (%3)")
                        .arg(formatCount(qint64(rows.rows.size())), nativeFolder(out), formatBytes(index.size()));
        return o;
    }

private:
    QString folder() const { return m_p.value(QStringLiteral("folder")).toString().trimmed(); }
    QJsonObject m_p;
};

} // namespace

bool isAmprAssetPackMarker(const QString& fileName)
{
    return fileName.compare(QLatin1String("ampr_assets.index"), Qt::CaseInsensitive) == 0
        || fileName.compare(QLatin1String("ampr_assets.index.runtime"), Qt::CaseInsensitive) == 0;
}

QString folderAmprAssetPack(const QString& folder)
{
    for (const QFileInfo& f : QDir(folder).entryInfoList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot))
        if (isAmprAssetPackMarker(f.fileName()))
            return f.fileName();
    return {};
}

QString amprAssetPackProblem(const QString& folder, const QString& marker)
{
    return QStringLiteral("%1 uses ampr_emu asset packs (%2 and .pak volumes). They only work with the ampr_emu.index they "
                          "were built from; a new index would break them. Rebuild the packs with ampr_emu's own tools instead.")
        .arg(folder, marker);
}

bool folderHasAmprLibrary(const QString& folder)
{
    const QDir root(folder);
    for (const QFileInfo& d : root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden)) {
        if (d.fileName().compare(QLatin1String("fakelib"), Qt::CaseInsensitive) != 0)
            continue;
        for (const QFileInfo& f : QDir(d.absoluteFilePath()).entryInfoList(QDir::Files | QDir::Hidden))
            if (f.fileName().compare(QLatin1String("libSceAmpr.sprx"), Qt::CaseInsensitive) == 0)
                return true;
    }
    return false;
}

void describeAmprRows(const AmprToolRows& rows, JobOutcome& o)
{
    o.details.insert(QStringLiteral("files"), qint64(rows.rows.size()));
    o.details.insert(QStringLiteral("collisions"), QJsonArray::fromStringList(rows.collisions));
    o.details.insert(QStringLiteral("skippedControl"), QJsonArray::fromStringList(rows.skippedControl));
    o.details.insert(QStringLiteral("skippedOwn"), QJsonArray::fromStringList(rows.skippedOwn));
    if (!rows.collisions.isEmpty())
        o.warnings << QStringLiteral("%1 path(s) differ only in letter case; the first of each was indexed: %2")
                          .arg(rows.collisions.size())
                          .arg(rows.collisions.mid(0, 3).join(QStringLiteral("; ")));
    if (!rows.skippedControl.isEmpty())
        o.warnings << QStringLiteral("%1 path(s) with a tab or line break were left out: %2")
                          .arg(rows.skippedControl.size())
                          .arg(rows.skippedControl.mid(0, 3).join(QStringLiteral("; ")));
}

void registerAmprJobs(ModuleContext& ctx)
{
    ctx.jobs.add(kAmprLocalJob, QStringLiteral("AMPR index (folder)"), [](const QJsonObject& p) -> Result<std::unique_ptr<Job>> {
        return std::unique_ptr<Job>(new AmprLocalJob(p));
    });
}

} // namespace fw::pfs
