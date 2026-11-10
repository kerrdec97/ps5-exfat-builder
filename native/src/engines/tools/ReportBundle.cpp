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

// ---- secrets ---------------------------------------------------------------------
// Secrets are recognised by the NAME of the setting or job parameter that
// holds them (a pattern, not a list), so a key added later is covered too.
// The key stays in the report and only its value is replaced: a maintainer
// can still see that it was set.

const QString kHidden = QStringLiteral("<hidden>");

constexpr qint64 kMaxExtraFileBytes = 16 * 1024 * 1024;
constexpr int kMaxCrashReports = 5;
constexpr qint64 kMaxCrashReportBytes = 1024 * 1024;

// The name fragments that mark a secret: passcodes and passwords, the DLC
// entitlement key, tokens and other credentials.
const QString kSecretName = QStringLiteral(
    "pass(?:code|word|phrase|wd)|entitlement[_\\- ]?key|secret|token|api[_\\-]?key|private[_\\-]?key|credential|authori[sz]ation");

bool isSecretKey(const QString& key)
{
    static const QRegularExpression re(kSecretName, QRegularExpression::CaseInsensitiveOption);
    return re.match(key).hasMatch();
}

bool isWebhookKey(const QString& key)
{
    return key.contains(QLatin1String("webhook"), Qt::CaseInsensitive);
}

// "ps5.consoleIp", and the "host" / "ip" parameters of console jobs.
bool isAddressKey(const QString& key)
{
    static const QRegularExpression re(
        QStringLiteral("^(?:ip|host|hostname|address)$|(?:console|ps5|server|remote)[_\\-]?(?:ip|host|address)$|ip[_\\-]?addr(?:ess)?$"),
        QRegularExpression::CaseInsensitiveOption);
    return re.match(key.mid(key.lastIndexOf(u'.') + 1)).hasMatch();
}

// An IPv4 address that only means something on the user's own network (or on
// this PC): private, link-local, carrier-grade NAT, loopback, unspecified.
bool isLocalIpv4(int a, int b)
{
    return a == 10 || a == 127 || a == 0 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168)
        || (a == 169 && b == 254) || (a == 100 && b >= 64 && b <= 127);
}

const QRegularExpression& ipv4Pattern()
{
    static const QRegularExpression re(
        QStringLiteral("(?<![\\d.])(\\d{1,3})\\.(\\d{1,3})\\.(\\d{1,3})\\.(\\d{1,3})(?!\\d|\\.\\d)"));
    return re;
}

// A console address is kept only when it is a local IPv4 address (with or
// without a port); a public address or a host name is hidden.
bool isLocalAddress(const QString& value)
{
    static const QRegularExpression re(QStringLiteral("^(\\d{1,3})\\.(\\d{1,3})\\.(\\d{1,3})\\.(\\d{1,3})(?::\\d{1,5})?$"));
    const auto m = re.match(value.trimmed());
    if (!m.hasMatch())
        return false;
    for (int i = 1; i <= 4; ++i)
        if (m.captured(i).toInt() > 255)
            return false;
    return isLocalIpv4(m.captured(1).toInt(), m.captured(2).toInt());
}

const QRegularExpression& discordWebhookPattern()
{
    // Anyone with the token can post to the channel.
    static const QRegularExpression re(
        QStringLiteral("(https?:(?:\\\\?/){2}(?:[A-Za-z0-9-]+\\.)*discord(?:app)?\\.com(?:\\\\?/)api(?:\\\\?/)(?:v\\d+(?:\\\\?/))?"
                       "webhooks(?:\\\\?/)\\d+(?:\\\\?/))[A-Za-z0-9_\\-]+"),
        QRegularExpression::CaseInsensitiveOption);
    return re;
}

bool isUnset(const QJsonValue& v)
{
    return v.isNull() || v.isUndefined() || (v.isString() && v.toString().isEmpty());
}

// Walks a parsed JSON value and hides the values of secret keys at any depth.
QJsonValue scrubJson(const QJsonValue& value)
{
    if (value.isArray()) {
        QJsonArray out;
        for (const auto& v : value.toArray())
            out.append(scrubJson(v));
        return out;
    }
    if (!value.isObject())
        return value;
    QJsonObject out;
    const QJsonObject in = value.toObject();
    for (auto it = in.begin(); it != in.end(); ++it) {
        const QString& key = it.key();
        const QJsonValue v = it.value();
        if (isUnset(v)) {
            out.insert(key, v);
        } else if (isSecretKey(key)) {
            out.insert(key, kHidden);
        } else if (isWebhookKey(key)) {
            // A Discord address keeps its public part; any other webhook is hidden whole.
            QString s = v.toString();
            const QString masked = s.replace(discordWebhookPattern(), QStringLiteral("\\1<token hidden>"));
            out.insert(key, v.isString() && masked != v.toString() ? QJsonValue(masked) : QJsonValue(kHidden));
        } else if (isAddressKey(key) && v.isString()) {
            out.insert(key, isLocalAddress(v.toString()) ? v : QJsonValue(kHidden));
        } else {
            out.insert(key, scrubJson(v));
        }
    }
    return out;
}

// Settings, queue and history are JSON (one document, or one object per
// line): those are scrubbed by structure. Returns false for any other text.
bool scrubJsonText(QString& text)
{
    const QString trimmed = text.trimmed();
    if (!trimmed.startsWith(u'{') && !trimmed.startsWith(u'['))
        return false;
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &err);
    if (err.error == QJsonParseError::NoError && !doc.isNull()) {
        const QJsonValue scrubbed = scrubJson(doc.isArray() ? QJsonValue(doc.array()) : QJsonValue(doc.object()));
        text = QString::fromUtf8(scrubbed.isArray() ? QJsonDocument(scrubbed.toArray()).toJson()
                                                    : QJsonDocument(scrubbed.toObject()).toJson());
        return true;
    }
    // JSON lines: each line that is an object by itself.
    bool any = false;
    QStringList lines = text.split(u'\n');
    for (QString& line : lines) {
        const QString t = line.trimmed();
        if (!t.startsWith(u'{') || !t.endsWith(u'}'))
            continue;
        const QJsonDocument d = QJsonDocument::fromJson(t.toUtf8(), &err);
        if (err.error != QJsonParseError::NoError || !d.isObject())
            continue;
        line = QString::fromUtf8(QJsonDocument(scrubJson(d.object()).toObject()).toJson(QJsonDocument::Compact));
        any = true;
    }
    if (any)
        text = lines.join(u'\n');
    return any;
}

// The same secrets in free text (log lines, error details, a JSON file that
// could not be parsed): "key": "value", key=value, key: value and --key value.
QString scrubPlainText(QString out)
{
    const auto pattern = [](const char* p) {
        return QRegularExpression(QString::fromLatin1(p).arg(kSecretName), QRegularExpression::CaseInsensitiveOption);
    };
    // "name": "value"
    static const QRegularExpression member =
        pattern("(\"[\\w.\\- ]*(?:%1)[\\w.\\- ]*\"\\s*:\\s*)\"(?:[^\"\\\\]|\\\\.)+\"");
    out.replace(member, QStringLiteral("\\1\"<hidden>\""));
    // \"name\":\"value\" (JSON quoted inside a JSON string or a log line)
    static const QRegularExpression escapedMember =
        pattern("(\\\\\"[\\w.\\- ]*(?:%1)[\\w.\\- ]*\\\\\"\\s*:\\s*)\\\\\"(?:[^\"\\\\]|\\\\[^\"])+?\\\\\"");
    out.replace(escapedMember, QStringLiteral("\\1\\\"<hidden>\\\""));
    // --passcode VALUE, --entitlement-key=VALUE
    static const QRegularExpression flag =
        pattern("((?<![\\w\\-])--?[\\w\\-]*(?:%1)[\\w\\-]*(?:[ \\t]*=[ \\t]*|[ \\t]+))"
                "(\\\\\"(?:[^\"\\\\\\r\\n]|\\\\[^\"])*\\\\\"|\"[^\"\\r\\n]*\"|'[^'\\r\\n]*'|[^\\s\"'\\\\]+)");
    out.replace(flag, QStringLiteral("\\1<hidden>"));
    // "--passcode", "VALUE" (an argument list written as JSON)
    static const QRegularExpression flagInList =
        pattern("(\"--?[\\w\\-]*(?:%1)[\\w\\-]*\"\\s*,\\s*)\"(?:[^\"\\\\]|\\\\.)*\"");
    out.replace(flagInList, QStringLiteral("\\1\"<hidden>\""));
    // passcode=VALUE, passcode: VALUE
    static const QRegularExpression assigned =
        pattern("(\\b[\\w.\\-]*(?:%1)[\\w.\\-]*[ \\t]*[=:][ \\t]*)"
                "(\\\\\"(?:[^\"\\\\\\r\\n]|\\\\[^\"])*\\\\\"|\"[^\"\\r\\n]*\"|'[^'\\r\\n]*'|[^\\s\"',;\\\\]+)");
    out.replace(assigned, QStringLiteral("\\1<hidden>"));
    out.replace(discordWebhookPattern(), QStringLiteral("\\1<token hidden>"));

    // Public IPv4 addresses (a console reached over the internet, a relay).
    QString result;
    qsizetype last = 0;
    auto it = ipv4Pattern().globalMatch(out);
    while (it.hasNext()) {
        const auto m = it.next();
        bool octets = true;
        for (int i = 1; i <= 4; ++i)
            octets = octets && m.captured(i).toInt() <= 255;
        if (!octets || isLocalIpv4(m.captured(1).toInt(), m.captured(2).toInt()))
            continue;
        result += QStringView(out).mid(last, m.capturedStart() - last);
        result += QStringLiteral("<address hidden>");
        last = m.capturedEnd();
    }
    if (last == 0)
        return out;
    result += QStringView(out).mid(last);
    return result;
}
} // namespace

QString redactSecrets(const QString& text)
{
    QString out = text;
    scrubJsonText(out);
    return scrubPlainText(std::move(out));
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
        // Anywhere else the name is replaced as a whole word, unless it is an
        // everyday word that would then vanish from every log line.
        static const QStringList common = {
            QStringLiteral("user"),    QStringLiteral("users"),   QStringLiteral("admin"),   QStringLiteral("administrator"),
            QStringLiteral("owner"),   QStringLiteral("guest"),   QStringLiteral("public"),  QStringLiteral("default"),
            QStringLiteral("home"),    QStringLiteral("game"),    QStringLiteral("games"),   QStringLiteral("gamer"),
            QStringLiteral("player"),  QStringLiteral("test"),    QStringLiteral("root"),    QStringLiteral("system"),
            QStringLiteral("windows"), QStringLiteral("desktop"), QStringLiteral("data"),    QStringLiteral("backup"),
            QStringLiteral("work"),    QStringLiteral("office"),  QStringLiteral("family"),  QStringLiteral("shared"),
            QStringLiteral("name"),    QStringLiteral("file"),    QStringLiteral("files"),   QStringLiteral("image"),
        };
        if (user.size() >= 4 && !common.contains(user.toLower())) {
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
    // The newest crash reports (the text files; a memory dump cannot be
    // cleaned of secrets and paths, so it is never included).
    if (!options.logsFolder.isEmpty()) {
        const QFileInfoList crashes = QDir(QDir(options.logsFolder).filePath(QStringLiteral("crashes")))
                                          .entryInfoList({QStringLiteral("crash-*.txt")}, QDir::Files, QDir::Time);
        for (const QFileInfo& fi : crashes.mid(0, kMaxCrashReports)) {
            QFile f(fi.absoluteFilePath());
            if (!f.open(QIODevice::ReadOnly))
                continue;
            FW_TRY(addText(QStringLiteral("logs/crashes/") + fi.fileName(), QString::fromUtf8(f.read(kMaxCrashReportBytes))));
        }
    }
    for (const QString& extra : options.extraFiles) {
        QFile f(extra);
        if (!f.open(QIODevice::ReadOnly))
            continue;
        // Of a very long file (the history) the end is kept: the newest
        // records, from the start of a line.
        QByteArray bytes;
        if (f.size() > kMaxExtraFileBytes) {
            f.seek(f.size() - kMaxExtraFileBytes);
            bytes = f.read(kMaxExtraFileBytes);
            bytes.remove(0, bytes.indexOf('\n') + 1);
        } else {
            bytes = f.readAll();
        }
        FW_TRY(addText(QFileInfo(extra).fileName(), QString::fromUtf8(bytes)));
    }
    // Read back in full before the archive gets its name; a failure removes it.
    FW_TRY(w.finish(true, token));
    return result;
}

} // namespace fw::tools
