#include "app/services/JobSubmit.h"

#include "app/AppContext.h"
#include "app/ui/Sheet.h"
#include "app/ui/Widgets.h"
#include "core/FileUtil.h"
#include "core/Paths.h"
#include "core/Tr.h"
#include "engines/ampr/Module.h"

#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QWidget>

namespace fw::jobs {

QString imageBuildKind(const QString& format)
{
    if (format == kFormatFfpkg)
        return QStringLiteral("image.ffpkg.build");
    if (format == kFormatFfpfsc || format == kFormatFfpfs)
        return QStringLiteral("image.pfs.build");
    return QStringLiteral("image.exfat.build");
}

QString outputExtension(const QString& format)
{
    if (format == kFormatFfpkg)
        return QStringLiteral(".ffpkg");
    if (format == kFormatFfpfsc)
        return QStringLiteral(".ffpfsc");
    if (format == kFormatFfpfs)
        return QStringLiteral(".ffpfs");
    return QStringLiteral(".exfat");
}

QString imageKindForFile(const QString& path, const QString& op)
{
    const QString ext = QFileInfo(path).suffix().toLower();
    if (ext == QLatin1String("exfat"))
        return QStringLiteral("image.exfat.") + op;
    if (ext == QLatin1String("ffpkg"))
        return QStringLiteral("image.ffpkg.") + op;
    if (ext == QLatin1String("ffpfs") || ext == QLatin1String("ffpfsc"))
        return QStringLiteral("image.pfs.") + op;
    if (ext == QLatin1String("pkg"))
        return QStringLiteral("fpkg.") + op;
    return {};
}

bool isAvailable(const AppContext& ctx, const QString& kind)
{
    return !kind.isEmpty() && ctx.jobFactory().contains(kind);
}

QStringList missingKinds(const AppContext& ctx, const QStringList& kinds)
{
    QStringList out;
    for (const QString& k : kinds)
        if (!isAvailable(ctx, k))
            out << k;
    return out;
}

Error unavailable(const QString& kind)
{
    return Error(ErrorCode::Unsupported, QStringLiteral("This feature is not available in this build."),
                 QStringLiteral("job kind %1 is not registered").arg(kind));
}

bool isSystemKind(const QString& kind)
{
    return kind.startsWith(QLatin1String("update."));
}

bool cancelWithConfirm(AppContext& ctx, QWidget* host, const QString& id)
{
    auto rec = ctx.queue().record(id);
    if (!rec)
        return false;
    if (rec->state == JobState::Queued) {
        ctx.queue().remove(id);
        return true;
    }
    if (rec->state != JobState::Running && rec->state != JobState::Paused)
        return false; // finished, or already cancelling
    const QString text = rec->progress.indeterminate
                             ? FW_TR("%1. The job stops at its next safe point.").arg(rec->title)
                             : FW_TR("%1 is %2 done. The job stops at its next safe point.").arg(rec->title, ui::thinPercent(rec->progress.overallFraction));
    bool remove = false;
    const bool ok = ui::Sheet::confirm(host ? host->window() : nullptr, FW_TR("Cancel the running job?"),
                                       text, // plain text: shown as it is
                                       FW_TR("Keep running"), FW_TR("Cancel job"), true, ui::Sheet::Icon::Warning,
                                       FW_TR("Also remove the job from the queue"), &remove);
    if (!ok)
        return false;
    // The sheet ran a nested event loop: the job may have finished meanwhile.
    rec = ctx.queue().record(id);
    if (!rec || isTerminal(rec->state))
        return false;
    ctx.queue().cancel(id);
    if (remove) {
        // The job leaves the queue once the worker reports Cancelled.
        auto* conn = new QMetaObject::Connection;
        *conn = QObject::connect(&ctx.queue(), &JobQueue::jobFinished, &ctx, [&ctx, id, conn](const QString& fin) {
            if (fin != id)
                return;
            ctx.queue().remove(id);
            QObject::disconnect(*conn);
            delete conn;
        });
    }
    return true;
}

static Result<QString> enqueue(AppContext& ctx, const QString& kind, const QJsonObject& params, const QVariantMap& overrides)
{
    if (!isAvailable(ctx, kind))
        return unavailable(kind);
    return ctx.queue().enqueue(kind, params, overrides);
}

Result<QString> submitImageBuild(AppContext& ctx, const QString& format, const QString& source, const QString& outputDir,
                                 const QString& label, const QVariantMap& overrides, const QJsonObject& extra)
{
    // Engines take the full output file path; `label` here is the file name
    // the user chose (the exFAT volume label is derived by the engine).
    QString fileName = label.trimmed().isEmpty() ? QFileInfo(source).fileName() : label.trimmed();
    const QString ext = outputExtension(format);
    if (!fileName.endsWith(ext, Qt::CaseInsensitive))
        fileName += ext;
    QJsonObject p = extra;
    p.insert(QStringLiteral("source"), source);
    p.insert(QStringLiteral("output"), QDir(outputDir).filePath(fileName));
    // "Compress game files (zstd packs)": the game is packed first, then built.
    const QString zstdKey = ampr::keys::kZstdPack;
    const bool zstd = overrides.contains(zstdKey) ? overrides.value(zstdKey).toBool() : ctx.settings().value(zstdKey).toBool();
    if (zstd) {
        p.insert(QStringLiteral("format"), format);
        return enqueue(ctx, QStringLiteral("image.build.zstd"), p, overrides);
    }
    if (format == kFormatFfpfsc || format == kFormatFfpfs)
        p.insert(QStringLiteral("format"), format);
    return enqueue(ctx, imageBuildKind(format), p, overrides);
}

Result<QString> submitPackFolder(AppContext& ctx, const QString& source, const QString& output, const QVariantMap& overrides,
                                 const QJsonObject& extra)
{
    QJsonObject p = extra;
    p.insert(QStringLiteral("source"), source);
    p.insert(QStringLiteral("output"), output);
    // What this output is, whatever the global "zstd packs" switch says, and
    // from every page that queues one (Build image, the Library's bulk build).
    QVariantMap o = overrides;
    o.insert(ampr::keys::kZstdPack, true);
    return enqueue(ctx, ampr::kPackJob, p, o);
}

Result<QString> submitVerify(AppContext& ctx, const QString& imageOrPackage, const QVariantMap& overrides)
{
    const QString kind = imageKindForFile(imageOrPackage, QStringLiteral("verify"));
    if (kind.isEmpty())
        return Error(ErrorCode::Unsupported, QStringLiteral("Choose an .exfat, .ffpkg, .ffpfs, .ffpfsc or .pkg file."));
    QJsonObject p;
    if (kind.startsWith(QLatin1String("fpkg."))) {
        p.insert(QStringLiteral("package"), imageOrPackage);
        p.insert(QStringLiteral("integrity"), true);
    } else {
        p.insert(QStringLiteral("image"), imageOrPackage);
    }
    return enqueue(ctx, kind, p, overrides);
}

Result<QString> submitGameExtract(AppContext& ctx, const QString& source, const QString& destination, bool unpackPacks,
                                  const QVariantMap& overrides)
{
    return enqueue(ctx, QStringLiteral("game.extract"),
                   {{QStringLiteral("source"), source}, {QStringLiteral("output"), destination}, {QStringLiteral("unpackPacks"), unpackPacks}},
                   overrides);
}

Result<QString> submitExtract(AppContext& ctx, const QString& imageOrPackage, const QString& destination,
                              const QVariantMap& overrides)
{
    const QString kind = imageKindForFile(imageOrPackage, QStringLiteral("extract"));
    if (kind.isEmpty())
        return Error(ErrorCode::Unsupported, QStringLiteral("Choose an .exfat, .ffpkg, .ffpfs, .ffpfsc or .pkg file."));
    QJsonObject p;
    if (kind.startsWith(QLatin1String("fpkg."))) {
        p.insert(QStringLiteral("package"), imageOrPackage);
        p.insert(QStringLiteral("destination"), destination);
    } else {
        p.insert(QStringLiteral("image"), imageOrPackage);
        p.insert(QStringLiteral("output"), destination);
        if (kind.startsWith(QLatin1String("image.pfs.")))
            p.insert(QStringLiteral("deep"), true); // unpack the nested image too, like the legacy Extract page
    }
    return enqueue(ctx, kind, p, overrides);
}

Result<QString> submitFpkgBuild(AppContext& ctx, const QString& sourceType, const QString& source, const QString& output,
                                const QJsonObject& details, const QVariantMap& overrides)
{
    Q_UNUSED(sourceType); // the engine detects folders, .gp5 and images itself
    if (details.value(QStringLiteral("packageType")).toString() == QLatin1String("addon")) {
        QJsonObject p;
        p.insert(QStringLiteral("payloadFolder"), source);
        if (!output.isEmpty())
            p.insert(QStringLiteral("output"), output);
        for (const char* k : {"contentId", "entitlementKey", "volumeType"})
            if (details.contains(QLatin1String(k)))
                p.insert(QLatin1String(k), details.value(QLatin1String(k)));
        return enqueue(ctx, QStringLiteral("fpkg.addon"), p, overrides);
    }
    QJsonObject p;
    p.insert(QStringLiteral("source"), source);
    if (!output.isEmpty())
        p.insert(QStringLiteral("output"), output);
    for (const char* k : {"contentId", "contentVersion", "title", "basePackage", "baseFolder", "exactSource", "updateOutput"})
        if (details.contains(QLatin1String(k)) && !details.value(QLatin1String(k)).toVariant().toString().isEmpty())
            p.insert(QLatin1String(k), details.value(QLatin1String(k)));
    return enqueue(ctx, QStringLiteral("fpkg.build"), p, overrides);
}

Result<QString> submitFpkgBackport(AppContext& ctx, const QJsonObject& params, const QVariantMap& overrides)
{
    return enqueue(ctx, QStringLiteral("fpkg.backport"), params, overrides);
}

Result<QString> submitDlcConvert(AppContext& ctx, const QJsonObject& params, const QVariantMap& overrides)
{
    return enqueue(ctx, QStringLiteral("dlc.convert"), params, overrides);
}

Result<QString> scanLibrary(AppContext& ctx, const QStringList& folders)
{
    QJsonObject p;
    p.insert(QStringLiteral("folders"), QJsonArray::fromStringList(folders));
    return enqueue(ctx, QStringLiteral("library.scan"), p, {});
}

Result<QString> submitPayload(AppContext& ctx, const QString& host, int port, const QString& file)
{
    QJsonObject p{{QStringLiteral("host"), host}, {QStringLiteral("port"), port}, {QStringLiteral("file"), file}};
    return enqueue(ctx, QStringLiteral("ps5.payload.send"), p, {});
}

Result<QString> submitUpdateCheck(AppContext& ctx, const QString& apiUrl)
{
    QJsonObject p;
    if (!apiUrl.isEmpty())
        p.insert(QStringLiteral("apiUrl"), apiUrl);
    return enqueue(ctx, QStringLiteral("update.check"), p, {});
}

Result<QString> submitUpdateDownload(AppContext& ctx, const QJsonObject& release)
{
    return enqueue(ctx, QStringLiteral("update.download"), QJsonObject{{QStringLiteral("release"), release}}, {});
}

Result<QString> submitLanguageScan(AppContext& ctx, const QString& folder, bool batch)
{
    return enqueue(ctx, QStringLiteral("tools.languages.scan"),
                   QJsonObject{{QStringLiteral("folder"), folder}, {QStringLiteral("batch"), batch}}, {});
}

Result<QString> submitDumpCheck(AppContext& ctx, const QString& folder, const QString& title)
{
    QJsonObject p{{QStringLiteral("folder"), folder}};
    if (!title.trimmed().isEmpty())
        p.insert(QStringLiteral("title"), title.trimmed());
    return enqueue(ctx, QStringLiteral("tools.dump.check"), p, {});
}

Result<QString> submitLanguageStrip(AppContext& ctx, const QString& gameRoot, const QStringList& remove,
                                    const QString& backupFolder)
{
    QJsonObject p{{QStringLiteral("gameRoot"), gameRoot}, {QStringLiteral("remove"), QJsonArray::fromStringList(remove)}};
    if (!backupFolder.isEmpty())
        p.insert(QStringLiteral("backupFolder"), backupFolder);
    return enqueue(ctx, QStringLiteral("tools.languages.strip"), p, {});
}

Result<QString> submitLanguageRestore(AppContext& ctx, const QString& backupZip, const QString& gameRoot)
{
    return enqueue(ctx, QStringLiteral("tools.languages.restore"),
                   QJsonObject{{QStringLiteral("backupZip"), backupZip}, {QStringLiteral("gameRoot"), gameRoot}}, {});
}

Result<QString> submitProblemReport(AppContext& ctx, const QString& destinationFolder, const QJsonObject& diagnostics,
                                    const QStringList& extraFiles)
{
    QJsonObject p;
    p.insert(QStringLiteral("destinationFolder"), destinationFolder);
    const QString logsFolder = QFileInfo(ctx.logFilePath()).absolutePath();
    QStringList files{Paths::settingsFile(), Paths::historyFile(), Paths::queueFile()};
    for (const QString& f : extraFiles) {
        if (f.isEmpty() || files.contains(f))
            continue;
        // A memory dump cannot be cleaned of secrets and paths: it stays out.
        if (QFileInfo(f).suffix().compare(QLatin1String("dmp"), Qt::CaseInsensitive) == 0)
            continue;
        // The job adds the crash reports beside the logs by itself.
        if (isSameOrInside(f, QDir(logsFolder).filePath(QStringLiteral("crashes"))))
            continue;
        files << f;
    }
    p.insert(QStringLiteral("logsFolder"), logsFolder);
    p.insert(QStringLiteral("extraFiles"), QJsonArray::fromStringList(files));
    p.insert(QStringLiteral("diagnostics"), diagnostics);
    return enqueue(ctx, QStringLiteral("tools.report"), p, {});
}

QStringList newestCrashFiles(const QStringList& crashFiles)
{
    if (crashFiles.isEmpty())
        return {};
    // crash-<date>-<time>.txt and .dmp belong to one crash.
    const QString stem = QFileInfo(crashFiles.first()).completeBaseName();
    QStringList out;
    for (const QString& f : crashFiles)
        if (QFileInfo(f).completeBaseName() == stem)
            out << f;
    return out;
}

QString imageEditKind(const QString& image)
{
    const QString ext = QFileInfo(image).suffix().toLower();
    if (ext == QLatin1String("exfat"))
        return QStringLiteral("image.exfat.edit");
    if (ext == QLatin1String("ffpkg"))
        return QStringLiteral("image.ffpkg.edit");
    return {};
}

static void putEndpoint(QJsonObject& p, const QString& host, int port)
{
    if (!host.isEmpty())
        p.insert(QStringLiteral("host"), host);
    if (port > 0)
        p.insert(QStringLiteral("port"), port);
}

Result<QString> submitFtpDownload(AppContext& ctx, const QString& host, int port, const QStringList& remotePaths,
                                  const QString& localDir, bool overwrite)
{
    QJsonObject p{{QStringLiteral("sources"), QJsonArray::fromStringList(remotePaths)}, {QStringLiteral("localDir"), localDir},
                  {QStringLiteral("overwrite"), overwrite}};
    putEndpoint(p, host, port);
    return enqueue(ctx, QStringLiteral("ps5.ftp.download"), p, {});
}

Result<QString> submitFtpUploadMany(AppContext& ctx, const QString& host, int port, const QStringList& localPaths,
                                    const QString& remoteDir, bool allowProtectedPaths)
{
    QJsonObject p{{QStringLiteral("sources"), QJsonArray::fromStringList(localPaths)}};
    if (!remoteDir.isEmpty())
        p.insert(QStringLiteral("remoteDir"), remoteDir);
    if (allowProtectedPaths)
        p.insert(QStringLiteral("allowProtectedPaths"), true);
    putEndpoint(p, host, port);
    return enqueue(ctx, QStringLiteral("ps5.ftp.upload"), p, {});
}

Result<QString> submitDpiInstall(AppContext& ctx, const QString& host, int port, const QString& package)
{
    QJsonObject p{{QStringLiteral("package"), package}};
    putEndpoint(p, host, port);
    return enqueue(ctx, QStringLiteral("ps5.dpi.install"), p, {});
}

Result<QString> submitY2jbAutoBlock(AppContext& ctx, const QString& host, int port, const QString& region,
                                    const QString& backupDir)
{
    QJsonObject p{{QStringLiteral("region"), region}, {QStringLiteral("confirmed"), true}};
    if (!backupDir.isEmpty())
        p.insert(QStringLiteral("backupDir"), backupDir);
    putEndpoint(p, host, port);
    return enqueue(ctx, QStringLiteral("ps5.y2jb.autoBlock"), p, {});
}

Result<QString> submitShadowMountStop(AppContext& ctx, const QString& host, int port)
{
    QJsonObject p;
    putEndpoint(p, host, port);
    return enqueue(ctx, QStringLiteral("ps5.shadowmount.stop"), p, {});
}

Result<QString> submitFtpDiagnose(AppContext& ctx, const QString& host, int port, const QString& folder, bool speedTest,
                                  bool allowProtectedPaths)
{
    QJsonObject p;
    putEndpoint(p, host, port);
    if (!folder.trimmed().isEmpty())
        p.insert(QStringLiteral("folder"), folder.trimmed());
    p.insert(QStringLiteral("speedTest"), speedTest);
    if (allowProtectedPaths)
        p.insert(QStringLiteral("allowProtectedPaths"), true);
    return enqueue(ctx, QStringLiteral("ps5.ftp.diagnose"), p, {});
}

Result<QString> submitImageEdit(AppContext& ctx, const QString& image, const QJsonArray& operations,
                                const QVariantMap& overrides)
{
    const QString kind = imageEditKind(image);
    if (kind.isEmpty())
        return Error(ErrorCode::Unsupported, QStringLiteral("Only .exfat and .ffpkg images can be edited."));
    return enqueue(ctx, kind, QJsonObject{{QStringLiteral("image"), image}, {QStringLiteral("operations"), operations}}, overrides);
}

Result<QString> submitConvert(AppContext& ctx, const QString& image, const QString& format, const QString& output,
                              const QVariantMap& overrides, const QString& method, bool replaceExisting)
{
    QJsonObject p{{QStringLiteral("image"), image}, {QStringLiteral("format"), format}, {QStringLiteral("output"), output}};
    if (!method.isEmpty())
        p.insert(QStringLiteral("method"), method);
    if (replaceExisting)
        p.insert(QStringLiteral("replaceExisting"), true);
    return enqueue(ctx, QStringLiteral("image.convert"), p, overrides);
}

Result<QString> submitCompare(AppContext& ctx, const QString& a, const QString& b, const QString& depth)
{
    return enqueue(ctx, QStringLiteral("image.compare"),
                   QJsonObject{{QStringLiteral("a"), a}, {QStringLiteral("b"), b}, {QStringLiteral("depth"), depth}}, {});
}

Result<QString> submitLibraryRename(AppContext& ctx, const QString& mode, const QString& source, const QString& preset,
                                    const QString& destination, const QJsonArray& items, const QString& undoLog)
{
    QJsonObject p{{QStringLiteral("mode"), mode}};
    if (!source.isEmpty())
        p.insert(QStringLiteral("source"), source);
    if (!preset.isEmpty())
        p.insert(QStringLiteral("preset"), preset);
    if (!destination.isEmpty())
        p.insert(QStringLiteral("destination"), destination);
    if (!items.isEmpty())
        p.insert(QStringLiteral("items"), items);
    if (!undoLog.isEmpty())
        p.insert(QStringLiteral("undoLog"), undoLog);
    return enqueue(ctx, QStringLiteral("library.rename"), p, {});
}

Result<QString> submitAmprIndexLocal(AppContext& ctx, const QString& folder, bool force, bool allowAssetPacks)
{
    QJsonObject p{{QStringLiteral("folder"), folder}};
    if (force)
        p.insert(QStringLiteral("force"), true);
    if (allowAssetPacks)
        p.insert(QStringLiteral("allowAssetPacks"), true);
    return enqueue(ctx, QStringLiteral("ampr.index.local"), p, {});
}

Result<QString> submitShadowMountEject(AppContext& ctx, const QString& host, int apiPort, const QString& path)
{
    QJsonObject p;
    if (!host.isEmpty())
        p.insert(QStringLiteral("host"), host);
    if (apiPort > 0)
        p.insert(QStringLiteral("apiPort"), apiPort);
    if (!path.isEmpty())
        p.insert(QStringLiteral("path"), path);
    return enqueue(ctx, QStringLiteral("ps5.shadowmount.eject"), p, {});
}

Result<QString> submitCrashDumpCapture(AppContext& ctx, const QString& host, int port, int minutes, const QString& outputDir)
{
    QJsonObject p{{QStringLiteral("minutes"), minutes}};
    putEndpoint(p, host, port);
    if (!outputDir.isEmpty())
        p.insert(QStringLiteral("outputDir"), outputDir);
    return enqueue(ctx, QStringLiteral("ps5.crashdumps.capture"), p, {});
}

Result<QString> submitAmprIndexFtp(AppContext& ctx, const QString& host, int port, const QString& remoteFolder)
{
    QJsonObject p{{QStringLiteral("remoteFolder"), remoteFolder}};
    putEndpoint(p, host, port);
    return enqueue(ctx, QStringLiteral("ampr.index.ftp"), p, {});
}

} // namespace fw::jobs
