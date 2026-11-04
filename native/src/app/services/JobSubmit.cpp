#include "app/services/JobSubmit.h"

#include "app/AppContext.h"
#include "core/Paths.h"

#include <QFileInfo>

namespace fw::jobs {

QString imageBuildKind(const QString& format)
{
    if (format == kFormatFfpkg)
        return QStringLiteral("image.ufs2.build");
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
        return QStringLiteral("image.ufs2.") + op;
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
    QJsonObject p = extra;
    p.insert(QStringLiteral("source"), source);
    p.insert(QStringLiteral("output"), outputDir);
    p.insert(QStringLiteral("label"), label);
    p.insert(QStringLiteral("format"), format);
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
    p.insert(kind.startsWith(QLatin1String("fpkg.")) ? QStringLiteral("package") : QStringLiteral("image"), imageOrPackage);
    p.insert(QStringLiteral("destination"), destination);
    return enqueue(ctx, kind, p, overrides);
}

Result<QString> submitFpkgBuild(AppContext& ctx, const QString& sourceType, const QString& source, const QString& output,
                                const QJsonObject& details, const QVariantMap& overrides)
{
    QJsonObject p = details;
    p.insert(QStringLiteral("source"), source);
    p.insert(QStringLiteral("sourceType"), sourceType);
    p.insert(QStringLiteral("output"), output);
    return enqueue(ctx, QStringLiteral("fpkg.build"), p, overrides);
}

Result<QString> submitBackport(AppContext& ctx, const QStringList& gameFolders, const QVariantMap& overrides)
{
    QJsonObject p;
    p.insert(QStringLiteral("gameFolders"), QJsonArray::fromStringList(gameFolders));
    if (gameFolders.size() == 1)
        p.insert(QStringLiteral("gameFolder"), gameFolders.first());
    return enqueue(ctx, QStringLiteral("backport.run"), p, overrides);
}

Result<QString> submitBackportRestore(AppContext& ctx, const QString& originalsZip, const QString& gameFolder)
{
    QJsonObject p;
    p.insert(QStringLiteral("originalsZip"), originalsZip);
    if (!gameFolder.isEmpty())
        p.insert(QStringLiteral("gameFolder"), gameFolder);
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
    QJsonObject p{{QStringLiteral("host"), host}, {QStringLiteral("port"), port},
                  {QStringLiteral("localPath"), localPath}, {QStringLiteral("remotePath"), remotePath}};
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

} // namespace fw::jobs
