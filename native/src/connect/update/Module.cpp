#include "connect/update/Module.h"

#include "core/BuildInfo.h"
#include "core/FileUtil.h"
#include "core/Paths.h"
#include "core/Tr.h" // FW_TR_NOOP: the job's texts stay English (logged); the window translates them where it shows them

#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QTimer>
#include <QUrl>

#include <atomic>

namespace fw::update {

namespace {

// The release tag carries the version; the one-file program's name has none.
const QRegularExpression kTagRe(QStringLiteral("^v?(\\d+\\.\\d+\\.\\d+)$"));
// "exFAT Image Builder.exe" as built; GitHub serves the spaces as dots.
const QRegularExpression kProgramRe(QStringLiteral("^exFAT[ .]Image[ .]Builder\\.exe$"),
                                    QRegularExpression::CaseInsensitiveOption);
// The installer name of earlier builds, still understood.
const QRegularExpression kInstallerRe(QStringLiteral("^exFAT-Image-Builder-Farewell-(\\d+\\.\\d+\\.\\d+)-win64-setup\\.exe$"));
const QRegularExpression kVersionRe(QStringLiteral("^\\d+\\.\\d+\\.\\d+$"));
const QString kAssetPathPrefix = QStringLiteral("/kerrdec97/ps5-exfat-builder/releases/download/");
constexpr qint64 kMinInstallerBytes = 5LL * 1024 * 1024;
constexpr qint64 kMaxDownloadBytes = 4LL * 1024 * 1024 * 1024; // when the release does not say the size
constexpr qint64 kMaxApiBytes = 8LL * 1024 * 1024;
constexpr qint64 kMaxSumsBytes = 1LL * 1024 * 1024;

std::atomic_bool g_loopbackAllowed{false};

enum class UrlRole {
    Api,      // the "latest release" document
    Asset,    // a file of a release of this repository
    Redirect, // where GitHub sends a request on
};

// Only GitHub over https. A release JSON, a stored job record or a redirect
// cannot point the download at another host or at plain http.
bool urlAllowed(const QUrl& url, UrlRole role)
{
    if (!url.isValid() || !url.userInfo().isEmpty())
        return false;
    const QString host = url.host().toLower();
    if (g_loopbackAllowed.load() && url.scheme() == QLatin1String("http") && host == QLatin1String("127.0.0.1"))
        return true;
    if (url.scheme() != QLatin1String("https") || url.port(443) != 443)
        return false;
    switch (role) {
    case UrlRole::Api:
        return host == QLatin1String("api.github.com");
    case UrlRole::Asset:
        return host == QLatin1String("github.com") && url.path().startsWith(kAssetPathPrefix, Qt::CaseInsensitive);
    case UrlRole::Redirect:
        // Release files are handed out by *.githubusercontent.com.
        return host == QLatin1String("github.com") || host == QLatin1String("api.github.com")
            || host.endsWith(QLatin1String(".githubusercontent.com"));
    }
    return false;
}

bool isAcceptedAssetName(const QString& name)
{
    return kProgramRe.match(name).hasMatch() || kInstallerRe.match(name).hasMatch();
}

QString userAgent()
{
    return QStringLiteral("exFAT-Image-Builder-Farewell/%1").arg(QLatin1String(build::kVersion));
}

QNetworkRequest makeRequest(const QUrl& url)
{
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, userAgent());
    req.setRawHeader("Accept", "application/vnd.github+json");
    // Every redirect is checked against the host list before it is followed.
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::UserVerifiedRedirectPolicy);
    req.setTransferTimeout(30000);
    return req;
}

struct HttpAnswer {
    int status = 0;
    qint64 contentLength = -1; // -1: not announced
    qint64 rateLimitReset = 0; // X-RateLimit-Reset (seconds since 1970); 0 when absent
};

// Runs one GET on the calling (worker) thread. `sink` receives body chunks and
// returns false to stop; the caller knows why it stopped.
Result<HttpAnswer> httpGet(const QUrl& url, UrlRole role, const CancellationToken& token,
                           const std::function<bool(const QByteArray&)>& sink)
{
    if (!urlAllowed(url, role))
        return Error(ErrorCode::InvalidArgument,
                     QStringLiteral("The update address is not a GitHub address, so it was not used."), url.host());
    QNetworkAccessManager nam;
    QNetworkReply* reply = nam.get(makeRequest(url));
    QEventLoop loop;
    HttpAnswer answer;
    bool sinkFailed = false;
    bool redirectRefused = false;
    QString refusedHost;
    QUrl current = url;
    QObject::connect(reply, &QNetworkReply::redirected, &loop, [&](const QUrl& to) {
        const bool lessSafe = current.scheme() == QLatin1String("https") && to.scheme() != QLatin1String("https");
        if (lessSafe || !urlAllowed(to, UrlRole::Redirect)) {
            redirectRefused = true;
            refusedHost = to.host();
            reply->abort();
            return;
        }
        current = to;
        Q_EMIT reply->redirectAllowed();
    });
    QObject::connect(reply, &QNetworkReply::readyRead, &loop, [&] {
        if (!sinkFailed && !sink(reply->readAll())) {
            sinkFailed = true;
            reply->abort();
        }
    });
    QObject::connect(reply, &QNetworkReply::metaDataChanged, &loop, [&] {
        const QVariant len = reply->header(QNetworkRequest::ContentLengthHeader);
        answer.contentLength = len.isValid() ? len.toLongLong() : -1;
    });
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (token.isCancelled())
            reply->abort();
    });
    poll.start(100);
    loop.exec();
    poll.stop();

    answer.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    answer.rateLimitReset = reply->rawHeader("X-RateLimit-Reset").trimmed().toLongLong();
    const auto err = reply->error();
    const QString errText = reply->errorString();
    if (err == QNetworkReply::NoError && !sinkFailed) {
        const QByteArray tail = reply->readAll();
        if (!tail.isEmpty() && !sink(tail))
            sinkFailed = true;
    }
    reply->deleteLater();
    if (token.isCancelled())
        return Error::cancelled();
    if (redirectRefused)
        return Error(ErrorCode::VerificationFailed,
                     QStringLiteral("GitHub sent the request on to an address this app does not accept, so it was stopped."),
                     refusedHost);
    if (sinkFailed)
        return Error(ErrorCode::IoError, QStringLiteral("Could not save the download."));
    if (err != QNetworkReply::NoError && answer.status == 0)
        return Error(ErrorCode::IoError, QStringLiteral("Could not reach GitHub. Check your internet connection and try again."), errText);
    // A transport failure after the headers (connection closed mid-body, a
    // timeout) is not a 200: only an HTTP error status is reported as one.
    if (err != QNetworkReply::NoError && answer.status < 400)
        return Error(ErrorCode::IoError, QStringLiteral("The download from GitHub was interrupted. Check your internet connection and try again."), errText);
    return answer;
}

QString rateLimitMessage(qint64 resetSecs)
{
    const qint64 wait = resetSecs - QDateTime::currentSecsSinceEpoch();
    if (resetSecs > 0 && wait > 0 && wait <= 24 * 3600)
        return QStringLiteral("GitHub is limiting requests right now. Try again in about %1 min.").arg((wait + 59) / 60);
    return QStringLiteral("GitHub is limiting requests right now. Try again in an hour.");
}

class CheckJob : public Job {
public:
    explicit CheckJob(QJsonObject p) : m_p(std::move(p)) {}
    QString kind() const override { return kCheckJob; }
    bool runsWhileHeld() const override { return true; } // never resumes the user's paused queue
    QString title() const override { return QStringLiteral("Check for updates"); }
    QStringList phases() const override { return {QStringLiteral("Checking")}; }
    QJsonObject parameters() const override { return m_p; }

    Status validate(const SettingsValues&) const override
    {
        if (!urlAllowed(apiUrl(), UrlRole::Api))
            return Error(ErrorCode::InvalidArgument, QStringLiteral("The update check only asks GitHub (api.github.com)."),
                         apiUrl().host());
        return success();
    }

    Result<JobOutcome> run(JobContext& ctx) override
    {
        ctx.progress->beginPhase(0, 0);
        QByteArray body;
        bool tooBig = false;
        auto answer = httpGet(apiUrl(), UrlRole::Api, ctx.token, [&](const QByteArray& chunk) {
            body += chunk;
            tooBig = body.size() > kMaxApiBytes;
            return !tooBig;
        });
        if (tooBig)
            return Error(ErrorCode::CorruptData, QStringLiteral("GitHub sent an unexpected answer."),
                         QStringLiteral("more than %1 bytes").arg(kMaxApiBytes));
        if (!answer.ok())
            return answer.takeError();
        const int status = answer->status;
        if (status == 404)
            return outcomeUpToDate(FW_TR_NOOP("No release has been published yet."));
        if (status == 403 || status == 429)
            return Error(ErrorCode::ExternalToolFailed, rateLimitMessage(answer->rateLimitReset));
        if (status != 200)
            return Error(ErrorCode::ExternalToolFailed, QStringLiteral("GitHub answered with an error (HTTP %1).").arg(status));
        auto rel = parseRelease(body, QString::fromLatin1(build::kVersion));
        if (!rel.ok()) {
            if (rel.error().code == ErrorCode::NotFound)
                return outcomeUpToDate(rel.error().message);
            return rel.takeError();
        }
        JobOutcome o;
        o.details = rel->toJson();
        o.details.insert(QStringLiteral("upToDate"), !rel->newerThanRunning);
        if (!rel->newerThanRunning) {
            o.summary = FW_TR_NOOP("You have the latest version.");
        } else if (rel->canDownload()) {
            o.summary = QStringLiteral("Version %1 is available.").arg(rel->version);
        } else {
            // Still an update: the user is sent to the release page instead.
            o.summary = QStringLiteral("Version %1 is available on the release page.").arg(rel->version);
            o.details.insert(QStringLiteral("note"),
                             FW_TR_NOOP("This release has no download the app can check itself. Get it from the release page."));
        }
        o.verification = Verification::Skipped;
        ctx.log.info(o.summary);
        return o;
    }

private:
    QUrl apiUrl() const { return QUrl(m_p.value(QStringLiteral("apiUrl")).toString(kDefaultApiUrl)); }
    static JobOutcome outcomeUpToDate(const QString& why)
    {
        JobOutcome o;
        o.summary = FW_TR_NOOP("You have the latest version.");
        o.details.insert(QStringLiteral("upToDate"), true);
        o.details.insert(QStringLiteral("note"), why);
        o.verification = Verification::Skipped;
        return o;
    }
    QJsonObject m_p;
};

class DownloadJob : public Job {
public:
    explicit DownloadJob(QJsonObject p) : m_p(std::move(p)), m_rel(ReleaseInfo::fromJson(m_p.value(QStringLiteral("release")).toObject())) {}
    QString kind() const override { return kDownloadJob; }
    bool runsWhileHeld() const override { return true; }
    QString title() const override { return QStringLiteral("Download version %1").arg(m_rel.version); }
    QStringList phases() const override { return {QStringLiteral("Checksums"), QStringLiteral("Downloading"), QStringLiteral("Verifying")}; }
    QJsonObject parameters() const override { return m_p; }

    Status validate(const SettingsValues&) const override
    {
        // The asset name becomes the file name in the cache folder: only the
        // names this app publishes are taken.
        if (!m_rel.canDownload() || !isAcceptedAssetName(m_rel.assetName))
            return Error(ErrorCode::InvalidArgument,
                         QStringLiteral("This release has no download the app can check. Get it from the release page."));
        const QString running = QString::fromLatin1(build::kVersion);
        if (!kVersionRe.match(m_rel.version).hasMatch() || compareVersions(m_rel.version, running) <= 0)
            return Error(ErrorCode::InvalidArgument,
                         QStringLiteral("Version %1 is not newer than this one (%2), so it was not downloaded.")
                             .arg(m_rel.version.isEmpty() ? QStringLiteral("?") : m_rel.version, running));
        return success();
    }

    Result<JobOutcome> run(JobContext& ctx) override
    {
        ctx.progress->setPhases(phases(), {1, 20, 2});
        // 1. Checksum file (fail closed without it).
        ctx.progress->beginPhase(0, 0);
        QByteArray sums;
        bool sumsTooBig = false;
        auto st = httpGet(QUrl(m_rel.sumsUrl), UrlRole::Asset, ctx.token, [&](const QByteArray& c) {
            sums += c;
            sumsTooBig = sums.size() > kMaxSumsBytes;
            return !sumsTooBig;
        });
        if (!st.ok() && !sumsTooBig)
            return st.takeError();
        if (sumsTooBig || st->status != 200)
            return Error(ErrorCode::VerificationFailed, QStringLiteral("The release's checksum file could not be downloaded, so the update was not downloaded."));
        const QString expected = findChecksum(sums, m_rel.assetName);
        if (expected.isEmpty())
            return Error(ErrorCode::VerificationFailed, QStringLiteral("The release does not list a checksum for its download, so the update was not downloaded."));

        // 2. The program to cache/updates/<name>.part
        const QString dir = QDir(Paths::cacheDir()).filePath(QStringLiteral("updates"));
        QDir().mkpath(dir);
        const QString finalPath = QDir(dir).filePath(m_rel.assetName);
        FW_TRY(ensureFreeSpace(dir, std::max<qint64>(m_rel.installerSize, kMinInstallerBytes)));
        PartFile part(finalPath);
        QFile out(part.partPath());
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return Error(ErrorCode::IoError, QStringLiteral("Could not save the update."), out.errorString());
        // Never more than the release announces, so a wrong answer cannot fill the drive.
        const qint64 limit = m_rel.installerSize > 0 ? m_rel.installerSize : kMaxDownloadBytes;
        qint64 received = 0;
        bool tooBig = false, writeFailed = false;
        ctx.progress->beginPhase(1, m_rel.installerSize > 0 ? m_rel.installerSize : 0);
        st = httpGet(QUrl(m_rel.installerUrl), UrlRole::Asset, ctx.token, [&](const QByteArray& c) {
            if (received + c.size() > limit) {
                tooBig = true;
                return false;
            }
            if (out.write(c) != c.size()) {
                writeFailed = true;
                return false;
            }
            received += c.size();
            ctx.progress->advance(c.size());
            return true;
        });
        // QFile buffers: a full or removed drive can show only when the buffer is written out.
        if (!out.flush() || out.error() != QFileDevice::NoError)
            writeFailed = true;
        const QString writeError = out.errorString();
        out.close();
        if (!st.ok() && st.error().isCancelled())
            return st.takeError();
        if (tooBig)
            return Error(ErrorCode::VerificationFailed, QStringLiteral("The download is larger than the release says, so it was deleted."));
        if (writeFailed)
            return Error(ErrorCode::IoError, QStringLiteral("Could not save the update."), writeError);
        if (!st.ok())
            return st.takeError();
        if (st->status != 200)
            return Error(ErrorCode::ExternalToolFailed, QStringLiteral("The download failed (HTTP %1).").arg(st->status));

        // 3. Size and hash checks, on the file as it is on disk.
        const qint64 onDisk = QFileInfo(part.partPath()).size();
        ctx.progress->beginPhase(2, onDisk);
        const qint64 announced = m_rel.installerSize > 0 ? m_rel.installerSize : st->contentLength;
        if ((st->contentLength >= 0 && st->contentLength != received) || (announced >= 0 && announced != received))
            return Error(ErrorCode::VerificationFailed, QStringLiteral("The download was incomplete. Try again."),
                         QStringLiteral("expected %1 bytes, got %2").arg(announced).arg(received));
        if (onDisk != received)
            return Error(ErrorCode::IoError, QStringLiteral("Could not save the update."),
                         QStringLiteral("%1 bytes received, %2 on disk").arg(received).arg(onDisk));
        if (onDisk < kMinInstallerBytes)
            return Error(ErrorCode::VerificationFailed, QStringLiteral("The download is too small to be the program. Try again."));
        FW_TRY_ASSIGN(const QString actual, sha256File(part.partPath(), ctx.token, ctx.progress));
        if (actual.compare(expected, Qt::CaseInsensitive) != 0)
            return Error(ErrorCode::VerificationFailed, QStringLiteral("The download does not match the release's checksum, so it was deleted."),
                         QStringLiteral("expected %1, got %2").arg(expected, actual));
        FW_TRY(part.commit(true));
        JobOutcome o;
        o.outputs << finalPath;
        o.verification = Verification::Passed;
        o.verificationDetail = QStringLiteral("SHA-256 matches the release's checksum file.");
        o.summary = QStringLiteral("Version %1 downloaded and verified.").arg(m_rel.version);
        o.details = m_rel.toJson();
        o.details.insert(QStringLiteral("file"), finalPath);
        o.details.insert(QStringLiteral("sha256"), actual.toLower());
        return o;
    }

private:
    QJsonObject m_p;
    ReleaseInfo m_rel;
};

} // namespace

void setLoopbackAllowedForTests(bool allowed)
{
    g_loopbackAllowed.store(allowed);
}

bool ReleaseInfo::canDownload() const
{
    return !installerUrl.isEmpty() && !sumsUrl.isEmpty() && urlAllowed(QUrl(installerUrl), UrlRole::Asset)
        && urlAllowed(QUrl(sumsUrl), UrlRole::Asset);
}

QJsonObject ReleaseInfo::toJson() const
{
    return {{QStringLiteral("version"), version},       {QStringLiteral("tag"), tag},
            {QStringLiteral("name"), name},             {QStringLiteral("notes"), notes},
            {QStringLiteral("pageUrl"), pageUrl},       {QStringLiteral("installerName"), installerName},
            {QStringLiteral("installerUrl"), installerUrl}, {QStringLiteral("installerSize"), installerSize},
            {QStringLiteral("sumsUrl"), sumsUrl},       {QStringLiteral("newer"), newerThanRunning},
            {QStringLiteral("assetName"), assetName},   {QStringLiteral("canDownload"), canDownload()}};
}

ReleaseInfo ReleaseInfo::fromJson(const QJsonObject& o)
{
    ReleaseInfo r;
    r.version = o.value(QStringLiteral("version")).toString();
    r.tag = o.value(QStringLiteral("tag")).toString();
    r.name = o.value(QStringLiteral("name")).toString();
    r.notes = o.value(QStringLiteral("notes")).toString();
    r.pageUrl = o.value(QStringLiteral("pageUrl")).toString();
    r.installerName = o.value(QStringLiteral("installerName")).toString();
    r.assetName = o.value(QStringLiteral("assetName")).toString(r.installerName);
    if (r.installerName.isEmpty())
        r.installerName = r.assetName;
    r.installerUrl = o.value(QStringLiteral("installerUrl")).toString();
    r.installerSize = qint64(o.value(QStringLiteral("installerSize")).toDouble());
    r.sumsUrl = o.value(QStringLiteral("sumsUrl")).toString();
    r.newerThanRunning = o.value(QStringLiteral("newer")).toBool();
    return r;
}

int compareVersions(const QString& a, const QString& b)
{
    const QStringList pa = a.split(u'.'), pb = b.split(u'.');
    for (int i = 0; i < std::max(pa.size(), pb.size()); ++i) {
        const int x = pa.value(i, QStringLiteral("0")).toInt();
        const int y = pb.value(i, QStringLiteral("0")).toInt();
        if (x != y)
            return x < y ? -1 : 1;
    }
    return 0;
}

Result<ReleaseInfo> parseRelease(const QByteArray& githubJson, const QString& runningVersion)
{
    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(githubJson, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject())
        return Error(ErrorCode::CorruptData, QStringLiteral("GitHub sent an unexpected answer."), perr.errorString());
    const QJsonObject o = doc.object();
    if (o.value(QStringLiteral("draft")).toBool() || o.value(QStringLiteral("prerelease")).toBool())
        return Error(ErrorCode::NotFound, QStringLiteral("The latest release is a draft or pre-release."));
    ReleaseInfo r;
    r.tag = o.value(QStringLiteral("tag_name")).toString();
    r.name = o.value(QStringLiteral("name")).toString();
    r.notes = o.value(QStringLiteral("body")).toString();
    r.pageUrl = o.value(QStringLiteral("html_url")).toString();
    if (const auto m = kTagRe.match(r.tag.trimmed()); m.hasMatch())
        r.version = m.captured(1);
    QJsonObject program, installer;
    QString installerVersion;
    for (const auto& v : o.value(QStringLiteral("assets")).toArray()) {
        const QJsonObject a = v.toObject();
        // A file whose upload did not finish is listed with another state.
        if (a.contains(QStringLiteral("state")) && a.value(QStringLiteral("state")).toString() != QLatin1String("uploaded"))
            continue;
        const QString name = a.value(QStringLiteral("name")).toString();
        if (kProgramRe.match(name).hasMatch()) {
            if (program.isEmpty())
                program = a;
        } else if (const auto m = kInstallerRe.match(name); m.hasMatch()) {
            if (installer.isEmpty()) {
                installer = a;
                installerVersion = m.captured(1);
            }
        } else if (name.compare(QStringLiteral("SHA256SUMS.txt"), Qt::CaseInsensitive) == 0) {
            r.sumsUrl = a.value(QStringLiteral("browser_download_url")).toString();
        }
    }
    if (r.version.isEmpty())
        r.version = installerVersion;
    if (r.version.isEmpty())
        return Error(ErrorCode::NotFound, QStringLiteral("The latest release's tag is not a version number."));
    const QJsonObject asset = program.isEmpty() ? installer : program;
    if (!asset.isEmpty()) {
        r.assetName = asset.value(QStringLiteral("name")).toString();
        r.installerName = r.assetName;
        r.installerUrl = asset.value(QStringLiteral("browser_download_url")).toString();
        r.installerSize = qint64(asset.value(QStringLiteral("size")).toDouble());
    }
    r.newerThanRunning = compareVersions(r.version, runningVersion) > 0;
    return r;
}

QString findChecksum(const QByteArray& sumsIn, const QString& fileName)
{
    static const QRegularExpression re(QStringLiteral("^([0-9a-fA-F]{64})\\s+\\*?(.+)$"));
    const auto loose = [](QString name) { return name.replace(u' ', u'.'); };
    const QByteArray sums = sumsIn.startsWith("\xEF\xBB\xBF") ? sumsIn.mid(3) : sumsIn;
    const QString wanted = loose(fileName);
    QString found;
    for (const QByteArray& rawLine : sums.split('\n')) {
        const auto m = re.match(QString::fromUtf8(rawLine).trimmed());
        if (!m.hasMatch())
            continue;
        const QString name = m.captured(2).trimmed();
        const QString hash = m.captured(1).toLower();
        if (name == fileName)
            return hash;
        if (loose(name) != wanted)
            continue;
        // Two different files that differ only in spaces and dots: not decidable.
        if (!found.isEmpty() && found != hash)
            return {};
        found = hash;
    }
    return found;
}

void registerModule(ModuleContext& ctx)
{
    SettingDef d;
    d.key = keys::kCheckOnStart;
    d.section = QStringLiteral("General");
    d.group = QStringLiteral("Updates");
    d.label = QStringLiteral("Check for updates when the app starts");
    d.description = QStringLiteral("Asks GitHub for the latest release a few seconds after start. Nothing is downloaded or installed without your confirmation, and downloads are checked against the release's SHA-256 list.");
    d.type = SettingType::Bool;
    d.defaultValue = true;
    d.order = 100;
    ctx.settings.add(d);
    ctx.settings.addLegacyKey(QStringLiteral("check_updates_on_boot"), keys::kCheckOnStart);

    ctx.jobs.add(kCheckJob, QStringLiteral("Check for updates"),
                 [](const QJsonObject& p) -> Result<std::unique_ptr<Job>> { return std::unique_ptr<Job>(new CheckJob(p)); });
    ctx.jobs.add(kDownloadJob, QStringLiteral("Download update"),
                 [](const QJsonObject& p) -> Result<std::unique_ptr<Job>> { return std::unique_ptr<Job>(new DownloadJob(p)); });
}

} // namespace fw::update
