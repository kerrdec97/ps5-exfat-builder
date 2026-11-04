#include "engines/tools/ReportBundle.h"

#include "core/BuildInfo.h"
#include "core/FileUtil.h"
#include "core/Paths.h"
#include "core/Zip.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QUrl>
#include <QStorageInfo>
#include <QSysInfo>
#include <QThread>

#include <algorithm>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace fw::tools {

namespace {
// Every spelling of the user's home folder that can appear in logs, settings
// and JSON: native and '/' separators, JSON-escaped, URL-encoded and (on
// Windows) the 8.3 short name.
QStringList homeSpellings()
{
    QStringList out;
    const QString home = QDir::homePath();
    if (home.isEmpty() || home == QStringLiteral("/"))
        return out;
    QStringList bases{home, QDir::toNativeSeparators(home)};
#ifdef Q_OS_WIN
    wchar_t shortBuf[MAX_PATH];
    const std::wstring longPath = QDir::toNativeSeparators(home).toStdWString();
    const DWORD n = GetShortPathNameW(longPath.c_str(), shortBuf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        const QString shortPath = QString::fromWCharArray(shortBuf, int(n));
        bases << shortPath << QDir::fromNativeSeparators(shortPath);
    }
#endif
    for (const QString& b : std::as_const(bases)) {
        out << b;
        QString json = b;
        json.replace(QStringLiteral("\\"), QStringLiteral("\\\\"));
        out << json;
        out << QString::fromLatin1(QUrl::toPercentEncoding(QDir::fromNativeSeparators(b), "/:"));
        out << QString::fromUtf8(QJsonDocument(QJsonArray{b}).toJson(QJsonDocument::Compact)).mid(2).chopped(2); // \uXXXX forms
    }
    out.removeDuplicates();
    // Longest first so a longer spelling is never half-replaced by a shorter one.
    std::sort(out.begin(), out.end(), [](const QString& a, const QString& b) { return a.size() > b.size(); });
    return out;
}

QString userNameOf(const QString& home)
{
    return QFileInfo(home).fileName();
}
} // namespace

QString redactSecrets(const QString& text)
{
    // general.discordWebhookUrl: anyone with the token can post to the channel.
    static const QRegularExpression webhook(
        QStringLiteral("(https?:(?:\\\\?/){2}(?:[A-Za-z0-9-]+\\.)*discord(?:app)?\\.com(?:\\\\?/)api(?:\\\\?/)(?:v\\d+(?:\\\\?/))?"
                       "webhooks(?:\\\\?/)\\d+(?:\\\\?/))[A-Za-z0-9_\\-]+"),
        QRegularExpression::CaseInsensitiveOption);
    QString out = text;
    return out.replace(webhook, QStringLiteral("\\1<token hidden>"));
}

QString redactUserPaths(const QString& text)
{
    QString out = text;
    for (const QString& h : homeSpellings())
        if (!h.isEmpty())
            out.replace(h, QStringLiteral("~"), Qt::CaseInsensitive);
    // The bare user name in other paths (other drives, profiles, 8.3 forms).
    const QString user = userNameOf(QDir::homePath());
    if (user.size() >= 2) {
        const QString u = QRegularExpression::escape(user);
        static const QString sep = QStringLiteral("[\\\\/]");
        const QRegularExpression inPath(QStringLiteral("(%1(?:Users|home|Documents and Settings)%1)%2(?=%1|$|\\W)").arg(sep, u),
                                        QRegularExpression::CaseInsensitiveOption);
        out.replace(inPath, QStringLiteral("\\1<user>"));
        if (user.size() >= 4) {
            const QRegularExpression word(QStringLiteral("\\b%1\\b").arg(u), QRegularExpression::CaseInsensitiveOption);
            out.replace(word, QStringLiteral("<user>"));
        }
    }
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
        text = redactSecrets(text); // always, whatever the user chose
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
