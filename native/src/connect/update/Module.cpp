#include "connect/update/Module.h"

#include "core/BuildInfo.h"
#include "core/FileUtil.h"
#include "core/Paths.h"

#include <QCryptographicHash>
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

namespace fw::update {

namespace {

const QRegularExpression kInstallerRe(QStringLiteral("^exFAT-Image-Builder-Farewell-(\\d+\\.\\d+\\.\\d+)-win64-setup\\.exe$"));
constexpr qint64 kMinInstallerBytes = 5LL * 1024 * 1024;

QString userAgent()
{
    return QStringLiteral("exFAT-Image-Builder-Farewell/%1").arg(QLatin1String(build::kVersion));
}

QNetworkRequest makeRequest(const QUrl& url)
{
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, userAgent());
    req.setRawHeader("Accept", "application/vnd.github+json");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setTransferTimeout(30000);
    return req;
}

// Runs one GET on the calling (worker) thread. `sink` receives body chunks;
// returns the HTTP status or an error.
Result<int> httpGet(const QUrl& url, const CancellationToken& token, const std::function<bool(const QByteArray&)>& sink,
                    qint64* contentLength = nullptr)
{
    QNetworkAccessManager nam;
    QNetworkReply* reply = nam.get(makeRequest(url));
    QEventLoop loop;
    bool sinkFailed = false;
    QObject::connect(reply, &QNetworkReply::readyRead, &loop, [&] {
        if (!sink(reply->readAll())) {
            sinkFailed = true;
            reply->abort();
        }
    });
    QObject::connect(reply, &QNetworkReply::metaDataChanged, &loop, [&] {
        if (contentLength) {
            const QVariant len = reply->header(QNetworkRequest::ContentLengthHeader);
            *contentLength = len.isValid() ? len.toLongLong() : -1;
        }
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

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const auto err = reply->error();
    const QString errText = reply->errorString();
    if (err == QNetworkReply::NoError) {
        const QByteArray tail = reply->readAll();
        if (!tail.isEmpty() && !sink(tail))
            sinkFailed = true;
    }
    reply->deleteLater();
    if (token.isCancelled())
        return Error::cancelled();
    if (sinkFailed)
        return Error(ErrorCode::IoError, QStringLiteral("Could not save the download."));
    if (err != QNetworkReply::NoError && status == 0)
        return Error(ErrorCode::IoError, QStringLiteral("Could not reach GitHub. Check your internet connection and try again."), errText);
    // A transport failure after the headers (connection closed mid-body, a
    // timeout) is not a 200: only an HTTP error status is reported as one.
    if (err != QNetworkReply::NoError && status < 400)
        return Error(ErrorCode::IoError, QStringLiteral("The download from GitHub was interrupted. Check your internet connection and try again."), errText);
    return status;
}

class CheckJob : public Job {
public:
    explicit CheckJob(QJsonObject p) : m_p(std::move(p)) {}
    QString kind() const override { return kCheckJob; }
    bool runsWhileHeld() const override { return true; } // never resumes the user's paused queue
    QString title() const override { return QStringLiteral("Check for updates"); }
    QStringList phases() const override { return {QStringLiteral("Checking")}; }
    QJsonObject parameters() const override { return m_p; }

    Result<JobOutcome> run(JobContext& ctx) override
    {
        ctx.progress->beginPhase(0, 0);
        const QUrl url(m_p.value(QStringLiteral("apiUrl")).toString(kDefaultApiUrl));
        QByteArray body;
        auto status = httpGet(url, ctx.token, [&](const QByteArray& chunk) {
            body += chunk;
            return body.size() < 8 * 1024 * 1024;
        });
        if (!status.ok())
            return status.takeError();
        if (status.value() == 404)
            return outcomeUpToDate(QStringLiteral("No release has been published yet."));
        if (status.value() == 403 || status.value() == 429)
            return Error(ErrorCode::ExternalToolFailed, QStringLiteral("GitHub is limiting requests right now. Try again in an hour."));
        if (status.value() != 200)
            return Error(ErrorCode::ExternalToolFailed, QStringLiteral("GitHub answered with an error (HTTP %1).").arg(status.value()));
        auto rel = parseRelease(body, QString::fromLatin1(build::kVersion));
        if (!rel.ok()) {
            if (rel.error().code == ErrorCode::NotFound)
                return outcomeUpToDate(rel.error().message);
            return rel.takeError();
        }
        JobOutcome o;
        o.details = rel->toJson();
        o.details.insert(QStringLiteral("upToDate"), !rel->newerThanRunning);
        o.summary = rel->newerThanRunning ? QStringLiteral("Version %1 is available.").arg(rel->version)
                                          : QStringLiteral("You have the latest version.");
        o.verification = Verification::Skipped;
        ctx.log.info(o.summary);
        return o;
    }

private:
    static JobOutcome outcomeUpToDate(const QString& why)
    {
        JobOutcome o;
        o.summary = QStringLiteral("You have the latest version.");
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
        if (m_rel.installerUrl.isEmpty() || m_rel.sumsUrl.isEmpty() || !kInstallerRe.match(m_rel.installerName).hasMatch())
            return Error(ErrorCode::InvalidArgument, QStringLiteral("This release has no verified installer to download."));
        return success();
    }

    Result<JobOutcome> run(JobContext& ctx) override
    {
        ctx.progress->setPhases(phases(), {1, 20, 2});
        // 1. Checksum file (fail closed without it).
        ctx.progress->beginPhase(0, 0);
        QByteArray sums;
        auto st = httpGet(QUrl(m_rel.sumsUrl), ctx.token, [&](const QByteArray& c) {
            sums += c;
            return sums.size() < 1024 * 1024;
        });
        if (!st.ok())
            return st.takeError();
        if (st.value() != 200)
            return Error(ErrorCode::VerificationFailed, QStringLiteral("The release's checksum file could not be downloaded, so the update was not installed."));
        const QString expected = findChecksum(sums, m_rel.installerName);
        if (expected.isEmpty())
            return Error(ErrorCode::VerificationFailed, QStringLiteral("The release does not list a checksum for its installer, so the update was not installed."));

        // 2. Installer to cache/updates/<name>.part
        const QString dir = QDir(Paths::cacheDir()).filePath(QStringLiteral("updates"));
        QDir().mkpath(dir);
        const QString finalPath = QDir(dir).filePath(m_rel.installerName);
        FW_TRY(ensureFreeSpace(dir, std::max<qint64>(m_rel.installerSize, kMinInstallerBytes)));
        PartFile part(finalPath);
        QFile out(part.partPath());
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return Error(ErrorCode::IoError, QStringLiteral("Could not save the update."), out.errorString());
        QCryptographicHash hash(QCryptographicHash::Sha256);
        qint64 received = 0, contentLength = -1;
        ctx.progress->beginPhase(1, m_rel.installerSize > 0 ? m_rel.installerSize : 0);
        st = httpGet(QUrl(m_rel.installerUrl), ctx.token, [&](const QByteArray& c) {
            hash.addData(c);
            received += c.size();
            ctx.progress->advance(c.size());
            return out.write(c) == c.size();
        }, &contentLength);
        out.close();
        if (!st.ok())
            return st.takeError();
        if (st.value() != 200)
            return Error(ErrorCode::ExternalToolFailed, QStringLiteral("The download failed (HTTP %1).").arg(st.value()));

        // 3. Size and hash checks.
        ctx.progress->beginPhase(2, 0);
        if (contentLength >= 0 && contentLength != received)
            return Error(ErrorCode::VerificationFailed, QStringLiteral("The download was incomplete. Try again."),
                         QStringLiteral("expected %1 bytes, got %2").arg(contentLength).arg(received));
        if (received < kMinInstallerBytes)
            return Error(ErrorCode::VerificationFailed, QStringLiteral("The download is too small to be the installer. Try again."));
        const QString actual = QString::fromLatin1(hash.result().toHex());
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
        return o;
    }

private:
    QJsonObject m_p;
    ReleaseInfo m_rel;
};

} // namespace

QJsonObject ReleaseInfo::toJson() const
{
    return {{QStringLiteral("version"), version},       {QStringLiteral("tag"), tag},
            {QStringLiteral("name"), name},             {QStringLiteral("notes"), notes},
            {QStringLiteral("pageUrl"), pageUrl},       {QStringLiteral("installerName"), installerName},
            {QStringLiteral("installerUrl"), installerUrl}, {QStringLiteral("installerSize"), installerSize},
            {QStringLiteral("sumsUrl"), sumsUrl},       {QStringLiteral("newer"), newerThanRunning}};
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
    for (const auto& v : o.value(QStringLiteral("assets")).toArray()) {
        const QJsonObject a = v.toObject();
        const QString name = a.value(QStringLiteral("name")).toString();
        const QString url = a.value(QStringLiteral("browser_download_url")).toString();
        const auto m = kInstallerRe.match(name);
        if (m.hasMatch()) {
            r.installerName = name;
            r.installerUrl = url;
            r.installerSize = qint64(a.value(QStringLiteral("size")).toDouble());
            r.version = m.captured(1);
        } else if (name.compare(QStringLiteral("SHA256SUMS.txt"), Qt::CaseInsensitive) == 0) {
            r.sumsUrl = url;
        }
    }
    if (r.installerUrl.isEmpty())
        return Error(ErrorCode::NotFound, QStringLiteral("The latest release has no Windows download for this app."));
    r.newerThanRunning = compareVersions(r.version, runningVersion) > 0;
    return r;
}

QString findChecksum(const QByteArray& sums, const QString& fileName)
{
    for (const QByteArray& rawLine : sums.split('\n')) {
        const QString line = QString::fromUtf8(rawLine).trimmed();
        static const QRegularExpression re(QStringLiteral("^([0-9a-fA-F]{64})\\s+\\*?(.+)$"));
        const auto m = re.match(line);
        if (m.hasMatch() && m.captured(2).trimmed() == fileName)
            return m.captured(1).toLower();
    }
    return {};
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
