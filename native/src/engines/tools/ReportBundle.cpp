#include "engines/tools/ReportBundle.h"

#include "core/BuildInfo.h"
#include "core/FileUtil.h"
#include "core/Paths.h"
#include "core/Zip.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QStorageInfo>
#include <QSysInfo>
#include <QThread>

#include <algorithm>

namespace fw::tools {

QString redactUserPaths(const QString& text)
{
    QString out = text;
    QString home = QDir::homePath();
    if (home.isEmpty() || home == QStringLiteral("/"))
        return out;
    const QString nativeHome = QDir::toNativeSeparators(home);
    QString jsonHome = nativeHome;
    jsonHome.replace(QStringLiteral("\\"), QStringLiteral("\\\\")); // as it appears inside JSON strings
    for (const QString& h : {jsonHome, nativeHome, home})
        out.replace(h, QStringLiteral("~"), Qt::CaseInsensitive);
    // The bare user name can also appear in paths of other drives.
    const QString user = QFileInfo(home).fileName();
    if (user.size() >= 3)
        out.replace(QStringLiteral("\\") + user + QStringLiteral("\\"), QStringLiteral("\\<user>\\"), Qt::CaseInsensitive);
    return out;
}

QJsonObject systemSummary()
{
    QJsonObject o;
    o.insert(QStringLiteral("app"), QStringLiteral("%1 %2 %3").arg(QLatin1String(build::kProductName), QLatin1String(build::kEditionName),
                                                                   QLatin1String(build::kVersion)));
    o.insert(QStringLiteral("qt"), QString::fromLatin1(qVersion()));
    o.insert(QStringLiteral("os"), QSysInfo::prettyProductName());
    o.insert(QStringLiteral("kernel"), QSysInfo::kernelType() + u' ' + QSysInfo::kernelVersion());
    o.insert(QStringLiteral("cpuArch"), QSysInfo::currentCpuArchitecture());
    o.insert(QStringLiteral("logicalCores"), QThread::idealThreadCount());
    o.insert(QStringLiteral("portable"), Paths::isPortable());
    const QStorageInfo si(Paths::dataDir());
    o.insert(QStringLiteral("dataDriveFree"), formatBytes(si.bytesAvailable()));
    o.insert(QStringLiteral("dataDriveFileSystem"), QString::fromLatin1(si.fileSystemType()));
    o.insert(QStringLiteral("created"), QDateTime::currentDateTime().toString(Qt::ISODate));
    return o;
}

Result<ReportResult> buildReportBundle(const ReportOptions& options, const CancellationToken& token, ProgressReporter* progress)
{
    if (options.destinationFolder.isEmpty() || !QFileInfo(options.destinationFolder).isDir())
        return Error(ErrorCode::InvalidArgument, QStringLiteral("Choose a folder for the report."));

    // Newest log files first, within the size budget.
    QFileInfoList logs;
    if (!options.logsFolder.isEmpty())
        logs = QDir(options.logsFolder).entryInfoList({QStringLiteral("*.log"), QStringLiteral("*.jsonl")}, QDir::Files, QDir::Time);
    QFileInfoList chosen;
    qint64 budget = options.maxLogBytes;
    for (const QFileInfo& fi : logs) {
        if (fi.size() > budget)
            continue;
        budget -= fi.size();
        chosen << fi;
    }

    ReportResult result;
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    result.zipPath = uniquePath(QDir(options.destinationFolder).filePath(QStringLiteral("exFAT-Image-Builder-report-%1.zip").arg(stamp)));

    qint64 total = 0;
    for (const auto& fi : chosen)
        total += fi.size();
    if (progress)
        progress->begin(QStringLiteral("Collecting"), total);
    FW_TRY(ensureFreeSpace(options.destinationFolder, total, 0));

    zip::Writer w;
    FW_TRY(w.open(result.zipPath));
    const auto addText = [&](const QString& name, QString text) -> Status {
        if (options.redactUserPaths)
            text = redactUserPaths(text);
        FW_TRY(w.addData(name, text.toUtf8(), zip::Method::Deflate));
        result.contents << name;
        return success();
    };
    QJsonObject summary = systemSummary();
    summary.insert(QStringLiteral("redacted"), options.redactUserPaths);
    FW_TRY(addText(QStringLiteral("system.json"), QString::fromUtf8(QJsonDocument(summary).toJson())));
    if (!options.diagnostics.isEmpty())
        FW_TRY(addText(QStringLiteral("diagnostics.json"), QString::fromUtf8(QJsonDocument(options.diagnostics).toJson())));
    for (const QFileInfo& fi : chosen) {
        if (!token.checkpoint())
            return Error::cancelled();
        QFile f(fi.absoluteFilePath());
        if (!f.open(QIODevice::ReadOnly))
            continue;
        FW_TRY(addText(QStringLiteral("logs/") + fi.fileName(), QString::fromUtf8(f.readAll())));
        if (progress)
            progress->advance(fi.size());
    }
    for (const QString& extra : options.extraFiles) {
        QFile f(extra);
        if (!f.open(QIODevice::ReadOnly))
            continue;
        FW_TRY(addText(QFileInfo(extra).fileName(), QString::fromUtf8(f.read(16 * 1024 * 1024))));
    }
    FW_TRY(w.finish());
    zip::Reader r;
    FW_TRY(r.open(result.zipPath));
    FW_TRY(r.verifyAll(token));
    return result;
}

} // namespace fw::tools
