#include "app/services/JobSubmit.h"

#include "app/AppContext.h"
#include "core/Paths.h"

#include <QDir>
#include <QFileInfo>

namespace fw::jobs {

QString imageBuildKind(const QString& format)
{
    if (format == kFormatFfpkg)
        return QStringLiteral("image.ffpkg.build");
    if (format == kFormatFfpfsc)
        return QStringLiteral("image.pfs.build");
    return QStringLiteral("image.exfat.build");
}

QString outputExtension(const QString& format)
{
    if (format == kFormatFfpkg)
        return QStringLiteral(".ffpkg");
    if (format == kFormatFfpfsc)
        return QStringLiteral(".ffpfsc");
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
    if (format == kFormatFfpfsc)
        p.insert(QStringLiteral("format"), QStringLiteral("ffpfsc"));
    return enqueue(ctx, imageBuildKind(format), p, overrides);
}

Result<QString> submitVerify(AppContext& ctx, const QString& imageOrPackage, const QVariantMap& overrides)
{
    const QString kind = imageKindForFile(imageOrPackage, QStringLiteral("verify"));
    if (kind.isEmpty())
        return Error(ErrorCode::Unsupported, QStringLiteral("Choose an .exfat, .ffpkg, .ffpfsc or .pkg file."));
    QJsonObject p;
    if (kind.startsWith(QLatin1String("fpkg."))) {
        p.insert(QStringLiteral("package"), imageOrPackage);
        p.insert(QStringLiteral("integrity"), true);
    } else {
        p.insert(QStringLiteral("image"), imageOrPackage);
    }
    return enqueue(ctx, kind, p, overrides);
}

Result<QString> submitExtract(AppContext& ctx, const QString& imageOrPackage, const QString& destination,
                              const QVariantMap& overrides)
{
    const QString kind = imageKindForFile(imageOrPackage, QStringLiteral("extract"));
    if (kind.isEmpty())
        return Error(ErrorCode::Unsupported, QStringLiteral("Choose an .exfat, .ffpkg, .ffpfsc or .pkg file."));
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

Result<QString> submitBackport(AppContext& ctx, const QStringList& gameFolders, const QVariantMap& overrides)
{
    // The engine backports one game per job, so each game can fail, be
    // retried or be restored on its own.
    if (gameFolders.isEmpty())
        return Error(ErrorCode::InvalidArgument, QStringLiteral("Choose at least one game."));
    QString last;
    for (const QString& game : gameFolders) {
        auto r = enqueue(ctx, QStringLiteral("backport.run"), QJsonObject{{QStringLiteral("game"), game}}, overrides);
        if (!r.ok())
            return r;
        last = r.value();
    }
    return last;
}

Result<QString> submitBackportRestore(AppContext& ctx, const QString& originalsZip, const QString& gameFolder)
{
    QJsonObject p;
    if (!originalsZip.isEmpty())
        p.insert(QStringLiteral("zip"), originalsZip);
    p.insert(QStringLiteral("game"), gameFolder);
    return enqueue(ctx, QStringLiteral("backport.restore"), p, {});
}

Result<QString> scanLibrary(AppContext& ctx, const QStringList& folders)
{
    QJsonObject p;
    p.insert(QStringLiteral("folders"), QJsonArray::fromStringList(folders));
    return enqueue(ctx, QStringLiteral("library.scan"), p, {});
}

Result<QString> submitFtpUpload(AppContext& ctx, const QString& host, int port, const QString& localPath,
                                const QString& remotePath)
{
    QJsonObject p{{QStringLiteral("sources"), QJsonArray{localPath}}};
    if (!host.isEmpty())
        p.insert(QStringLiteral("host"), host);
    if (port > 0)
        p.insert(QStringLiteral("port"), port);
    if (!remotePath.isEmpty())
        p.insert(QStringLiteral("remoteDir"), remotePath);
    return enqueue(ctx, QStringLiteral("ps5.ftp.upload"), p, {});
}

Result<QString> submitPayload(AppContext& ctx, const QString& host, int port, const QString& file)
{
    QJsonObject p{{QStringLiteral("host"), host}, {QStringLiteral("port"), port}, {QStringLiteral("file"), file}};
    return enqueue(ctx, QStringLiteral("ps5.payload.send"), p, {});
}

Result<QString> submitUpdateCheck(AppContext& ctx)
{
    return enqueue(ctx, QStringLiteral("update.check"), {}, {});
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

Result<QString> submitProblemReport(AppContext& ctx, const QString& destinationFolder, const QJsonObject& diagnostics)
{
    QJsonObject p;
    p.insert(QStringLiteral("destinationFolder"), destinationFolder);
    p.insert(QStringLiteral("logsFolder"), QFileInfo(ctx.logFilePath()).absolutePath());
    p.insert(QStringLiteral("extraFiles"),
             QJsonArray::fromStringList({Paths::settingsFile(), Paths::historyFile(), Paths::queueFile()}));
    p.insert(QStringLiteral("diagnostics"), diagnostics);
    return enqueue(ctx, QStringLiteral("tools.report"), p, {});
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

Result<QString> submitImageEdit(AppContext& ctx, const QString& image, const QJsonArray& operations,
                                const QVariantMap& overrides)
{
    const QString kind = imageEditKind(image);
    if (kind.isEmpty())
        return Error(ErrorCode::Unsupported, QStringLiteral("Only .exfat and .ffpkg images can be edited."));
    return enqueue(ctx, kind, QJsonObject{{QStringLiteral("image"), image}, {QStringLiteral("operations"), operations}}, overrides);
}

Result<QString> submitConvert(AppContext& ctx, const QString& image, const QString& format, const QString& output,
                              const QVariantMap& overrides)
{
    return enqueue(ctx, QStringLiteral("image.convert"),
                   QJsonObject{{QStringLiteral("image"), image}, {QStringLiteral("format"), format}, {QStringLiteral("output"), output}},
                   overrides);
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

Result<QString> submitBackportRollback(AppContext& ctx, const QString& journalPath)
{
    return enqueue(ctx, QStringLiteral("backport.restore"), QJsonObject{{QStringLiteral("journal"), journalPath}}, {});
}

Result<QString> submitAmprIndexLocal(AppContext& ctx, const QString& folder, bool force)
{
    QJsonObject p{{QStringLiteral("folder"), folder}};
    if (force)
        p.insert(QStringLiteral("force"), true);
    return enqueue(ctx, QStringLiteral("ampr.index.local"), p, {});
}

Result<QString> submitAmprIndexFtp(AppContext& ctx, const QString& host, int port, const QString& remoteFolder)
{
    QJsonObject p{{QStringLiteral("remoteFolder"), remoteFolder}};
    putEndpoint(p, host, port);
    return enqueue(ctx, QStringLiteral("ampr.index.ftp"), p, {});
}

} // namespace fw::jobs
