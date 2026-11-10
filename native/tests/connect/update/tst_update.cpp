// Update checker: release parsing, version comparison, checksum lookup and the
// check/download jobs against a local HTTP server (never the real GitHub).

#include "connect/update/Module.h"
#include "core/BuildInfo.h"
#include "core/Paths.h"
#include "core/Settings.h"
#include "jobs/JobQueue.h"
#include "support/TestSupport.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>

using namespace fw;

namespace {

const QString kProgram = QStringLiteral("exFAT.Image.Builder.exe"); // as GitHub serves "exFAT Image Builder.exe"
const QString kGithub = QStringLiteral("https://github.com");

// Minimal HTTP/1.1 server: GET <path> → fixed responses.
class MiniHttp : public QObject {
public:
    struct Response {
        int status = 200;
        QByteArray body;
        bool truncate = false; // advertise a longer Content-Length than sent
        QByteArray headers;    // extra header lines, each ending in \r\n
    };
    QMap<QString, Response> routes;

    bool listen() { return m_server.listen(QHostAddress::LocalHost, 0); }
    QString base() const { return QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort()); }
    quint16 port() const { return m_server.serverPort(); }

    MiniHttp()
    {
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket* s = m_server.nextPendingConnection()) {
                connect(s, &QTcpSocket::readyRead, s, [this, s] {
                    QByteArray& buf = m_buf[s];
                    buf += s->readAll();
                    if (!buf.contains("\r\n\r\n"))
                        return;
                    const QList<QByteArray> parts = buf.split(' ');
                    const QString path = parts.size() > 1 ? QString::fromLatin1(parts[1]) : QString();
                    buf.clear();
                    const Response r = routes.value(path, Response{404, "not found"});
                    const qint64 advertised = r.truncate ? r.body.size() + 1000 : r.body.size();
                    QByteArray head = "HTTP/1.1 " + QByteArray::number(r.status) + " X\r\nContent-Length: "
                        + QByteArray::number(advertised) + "\r\n" + r.headers + "Connection: close\r\n\r\n";
                    s->write(head + r.body);
                    s->flush();
                    s->disconnectFromHost();
                });
                // The entry goes with the socket: a later socket can get the
                // same address and must not inherit an earlier request.
                connect(s, &QTcpSocket::disconnected, this, [this, s] {
                    m_buf.remove(s);
                    s->deleteLater();
                });
            }
        });
    }

private:
    QTcpServer m_server;
    QHash<QTcpSocket*, QByteArray> m_buf;
};

QString assetPath(const QString& version, const QString& file)
{
    return QStringLiteral("/kerrdec97/ps5-exfat-builder/releases/download/v%1/%2").arg(version, file);
}

QJsonObject asset(const QString& base, const QString& version, const QString& name, qint64 size, const QString& contentType)
{
    return {{QStringLiteral("url"), QStringLiteral("https://api.github.com/repos/kerrdec97/ps5-exfat-builder/releases/assets/301")},
            {QStringLiteral("id"), 301},
            {QStringLiteral("name"), name},
            {QStringLiteral("label"), QString()},
            {QStringLiteral("content_type"), contentType},
            {QStringLiteral("state"), QStringLiteral("uploaded")},
            {QStringLiteral("size"), size},
            {QStringLiteral("download_count"), 12},
            {QStringLiteral("browser_download_url"), base + assetPath(version, name)}};
}

// A v5 release as the GitHub API returns it: the version only in the tag, the
// one-file program (spaces served as dots) and SHA256SUMS.txt.
QByteArray v5Release(const QString& base, const QString& version, qint64 programSize = 6 * 1024 * 1024,
                     bool withProgram = true, bool withSums = true)
{
    QJsonArray assets;
    if (withProgram)
        assets.append(asset(base, version, kProgram, programSize, QStringLiteral("application/x-msdownload")));
    if (withSums)
        assets.append(asset(base, version, QStringLiteral("SHA256SUMS.txt"), 90, QStringLiteral("text/plain")));
    const QJsonObject o{{QStringLiteral("url"), QStringLiteral("https://api.github.com/repos/kerrdec97/ps5-exfat-builder/releases/7001")},
                        {QStringLiteral("html_url"), kGithub + QStringLiteral("/kerrdec97/ps5-exfat-builder/releases/tag/v") + version},
                        {QStringLiteral("id"), 7001},
                        {QStringLiteral("tag_name"), QStringLiteral("v") + version},
                        {QStringLiteral("target_commitish"), QStringLiteral("main")},
                        {QStringLiteral("name"), QStringLiteral("exFAT Image Builder ") + version},
                        {QStringLiteral("draft"), false},
                        {QStringLiteral("prerelease"), false},
                        {QStringLiteral("created_at"), QStringLiteral("2026-11-01T10:00:00Z")},
                        {QStringLiteral("published_at"), QStringLiteral("2026-11-01T10:20:00Z")},
                        {QStringLiteral("assets"), assets},
                        {QStringLiteral("body"), QStringLiteral("## What's new\n* Fixes")}};
    return QJsonDocument(o).toJson();
}

// A release of an earlier build: an installer whose name carries the version.
QByteArray installerRelease(const QString& base, const QString& version)
{
    const QString inst = QStringLiteral("exFAT-Image-Builder-Farewell-%1-win64-setup.exe").arg(version);
    QJsonArray assets;
    assets.append(QJsonObject{{QStringLiteral("name"), inst},
                              {QStringLiteral("browser_download_url"), base + assetPath(version, inst)},
                              {QStringLiteral("size"), 6 * 1024 * 1024}});
    assets.append(QJsonObject{{QStringLiteral("name"), QStringLiteral("SHA256SUMS.txt")},
                              {QStringLiteral("browser_download_url"), base + assetPath(version, QStringLiteral("SHA256SUMS.txt"))}});
    const QJsonObject o{{QStringLiteral("tag_name"), QStringLiteral("v") + version},
                        {QStringLiteral("name"), QStringLiteral("Release ") + version},
                        {QStringLiteral("body"), QStringLiteral("Notes")},
                        {QStringLiteral("html_url"), base + QStringLiteral("/release")},
                        {QStringLiteral("assets"), assets}};
    return QJsonDocument(o).toJson();
}

QByteArray programBytes(char fill)
{
    QByteArray payload(6 * 1024 * 1024, fill);
    payload[123] = 'q';
    return payload;
}

QByteArray sumsLine(const QByteArray& payload)
{
    // The release script writes the name with spaces.
    return QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex() + "  exFAT Image Builder.exe\n";
}

} // namespace

class TstUpdate : public QObject {
    Q_OBJECT
    QTemporaryDir m_root;

    struct Env {
        std::shared_ptr<SettingsSchema> schema = std::make_shared<SettingsSchema>();
        DependencyRegistry deps;
        JobFactory jobs;
        std::unique_ptr<SettingsStore> store;
        std::unique_ptr<JobQueue> queue;
        std::optional<Error> refused; // why the last run() was not accepted
        explicit Env(const QString& dir)
        {
            ModuleContext ctx{*schema, deps, jobs};
            update::registerModule(ctx);
            // Keys the queue reads (declared by the general module in the app).
            for (auto [key, def] : {std::pair{QStringLiteral("performance.maxConcurrentJobs"), QVariant(1)},
                                    std::pair{QStringLiteral("general.autoRetryCount"), QVariant(0)},
                                    std::pair{QStringLiteral("general.autoRetryDelaySeconds"), QVariant(0)}}) {
                SettingDef d;
                d.key = key;
                d.section = QStringLiteral("General");
                d.label = key;
                d.type = SettingType::Int;
                d.defaultValue = def;
                schema->add(d);
            }
            store = std::make_unique<SettingsStore>(schema, dir + QStringLiteral("/settings.json"));
            (void)store->load();
            queue = std::make_unique<JobQueue>(*store, jobs);
        }
        std::optional<JobRecord> run(const QString& kind, const QJsonObject& params)
        {
            refused.reset();
            auto id = queue->enqueue(kind, params);
            if (!id.ok()) {
                refused = id.error();
                return std::nullopt;
            }
            // Generous: the suite runs next to other tests and builds.
            (void)QTest::qWaitFor([&] { auto r = queue->record(id.value()); return r && isTerminal(r->state); }, 120000);
            return queue->record(id.value());
        }
    };

    static QString updatesDir() { return QDir(Paths::cacheDir()).filePath(QStringLiteral("updates")); }
    static void clearUpdatesDir() { QDir(updatesDir()).removeRecursively(); }

private slots:
    void initTestCase()
    {
        QVERIFY(m_root.isValid());
        fwtest::isolateAppData(m_root);
    }
    void init() { update::setLoopbackAllowedForTests(true); }
    void cleanup() { update::setLoopbackAllowedForTests(false); }

    void versionsCompareNumerically()
    {
        QVERIFY(update::compareVersions(QStringLiteral("5.10.0"), QStringLiteral("5.9.3")) > 0);
        QVERIFY(update::compareVersions(QStringLiteral("5.0"), QStringLiteral("5.0.0")) == 0);
        QVERIFY(update::compareVersions(QStringLiteral("4.1.0"), QStringLiteral("5.0.0")) < 0);
    }

    void checksumLookup()
    {
        const QByteArray sums = "AAAA  other.zip\n" + QByteArray(64, 'a') + "  exFAT-Image-Builder-Farewell-5.0.1-win64-setup.exe\r\n"
            + QByteArray(64, 'b') + " *star.exe\n";
        QCOMPARE(update::findChecksum(sums, QStringLiteral("exFAT-Image-Builder-Farewell-5.0.1-win64-setup.exe")), QString(64, u'a'));
        QCOMPARE(update::findChecksum(sums, QStringLiteral("star.exe")), QString(64, u'b'));
        QVERIFY(update::findChecksum(sums, QStringLiteral("missing.exe")).isEmpty());
    }

    void checksumLookupTreatsSpacesAndDotsAlike()
    {
        // SHA256SUMS.txt names the file as it was built; GitHub serves it with dots.
        const QByteArray sums = QByteArray(64, 'c') + "  exFAT Image Builder.exe\n";
        QCOMPARE(update::findChecksum(sums, kProgram), QString(64, u'c'));
        QCOMPARE(update::findChecksum(sums, QStringLiteral("exFAT Image Builder.exe")), QString(64, u'c'));
        QCOMPARE(update::findChecksum("\xEF\xBB\xBF" + sums, kProgram), QString(64, u'c'));
        QVERIFY(update::findChecksum(sums, QStringLiteral("exFAT.Image.Builder.zip")).isEmpty());
        // An exact name wins over a look-alike; two different look-alikes decide nothing.
        const QByteArray both = QByteArray(64, 'd') + "  exFAT Image Builder.exe\n" + QByteArray(64, 'e') + "  exFAT.Image.Builder.exe\n";
        QCOMPARE(update::findChecksum(both, kProgram), QString(64, u'e'));
        QVERIFY(update::findChecksum(both, QStringLiteral("exFAT.Image Builder.exe")).isEmpty());
    }

    void parsesV5Release()
    {
        update::setLoopbackAllowedForTests(false); // real addresses only
        auto r = update::parseRelease(v5Release(kGithub, QStringLiteral("5.0.2"), 73400320), QStringLiteral("5.0.1"));
        QVERIFY2(r.ok(), r.ok() ? "" : qPrintable(r.error().message));
        QCOMPARE(r->version, QStringLiteral("5.0.2"));
        QCOMPARE(r->tag, QStringLiteral("v5.0.2"));
        QVERIFY(r->newerThanRunning);
        QCOMPARE(r->assetName, kProgram);
        QCOMPARE(r->installerName, kProgram);
        QCOMPARE(r->installerUrl, kGithub + assetPath(QStringLiteral("5.0.2"), kProgram));
        QCOMPARE(r->installerSize, qint64(73400320));
        QCOMPARE(r->sumsUrl, kGithub + assetPath(QStringLiteral("5.0.2"), QStringLiteral("SHA256SUMS.txt")));
        QCOMPARE(r->pageUrl, kGithub + QStringLiteral("/kerrdec97/ps5-exfat-builder/releases/tag/v5.0.2"));
        QVERIFY(r->canDownload());

        const QJsonObject json = r->toJson();
        QCOMPARE(json.value(QStringLiteral("assetName")).toString(), kProgram);
        QCOMPARE(json.value(QStringLiteral("installerUrl")).toString(), r->installerUrl);
        QCOMPARE(json.value(QStringLiteral("pageUrl")).toString(), r->pageUrl);
        QVERIFY(json.value(QStringLiteral("newer")).toBool());
        QVERIFY(json.value(QStringLiteral("canDownload")).toBool());
        const update::ReleaseInfo back = update::ReleaseInfo::fromJson(json);
        QCOMPARE(back.assetName, kProgram);
        QVERIFY(back.canDownload());

        // The same release seen from 5.0.2 or later is not an update.
        auto same = update::parseRelease(v5Release(kGithub, QStringLiteral("5.0.2")), QStringLiteral("5.0.2"));
        QVERIFY(same.ok() && !same->newerThanRunning);
        auto older = update::parseRelease(v5Release(kGithub, QStringLiteral("5.0.2")), QStringLiteral("5.1.0"));
        QVERIFY(older.ok() && !older->newerThanRunning);

        // The name with spaces (as built) is accepted too.
        QJsonObject spaced = QJsonDocument::fromJson(v5Release(kGithub, QStringLiteral("5.0.2"))).object();
        QJsonArray assets = spaced.value(QStringLiteral("assets")).toArray();
        QJsonObject first = assets.at(0).toObject();
        first.insert(QStringLiteral("name"), QStringLiteral("exFAT Image Builder.exe"));
        assets.replace(0, first);
        spaced.insert(QStringLiteral("assets"), assets);
        auto sp = update::parseRelease(QJsonDocument(spaced).toJson(), QStringLiteral("5.0.1"));
        QVERIFY(sp.ok());
        QCOMPARE(sp->assetName, QStringLiteral("exFAT Image Builder.exe"));
    }

    void newerReleaseWithoutUsableDownloadIsStillNewer()
    {
        update::setLoopbackAllowedForTests(false);
        // No program file at all.
        auto bare = update::parseRelease(v5Release(kGithub, QStringLiteral("5.1.0"), 0, false, true), QStringLiteral("5.0.1"));
        QVERIFY(bare.ok());
        QVERIFY(bare->newerThanRunning);
        QVERIFY(bare->installerUrl.isEmpty());
        QVERIFY(bare->assetName.isEmpty());
        QVERIFY(!bare->canDownload());
        QVERIFY(!bare->pageUrl.isEmpty());
        // A program without a checksum list cannot be verified.
        auto noSums = update::parseRelease(v5Release(kGithub, QStringLiteral("5.1.0"), 6 * 1024 * 1024, true, false), QStringLiteral("5.0.1"));
        QVERIFY(noSums.ok() && noSums->newerThanRunning && !noSums->canDownload());
        // A file whose upload did not finish is not offered.
        QJsonObject half = QJsonDocument::fromJson(v5Release(kGithub, QStringLiteral("5.1.0"))).object();
        QJsonArray assets = half.value(QStringLiteral("assets")).toArray();
        QJsonObject first = assets.at(0).toObject();
        first.insert(QStringLiteral("state"), QStringLiteral("starter"));
        assets.replace(0, first);
        half.insert(QStringLiteral("assets"), assets);
        auto h = update::parseRelease(QJsonDocument(half).toJson(), QStringLiteral("5.0.1"));
        QVERIFY(h.ok() && h->newerThanRunning && h->installerUrl.isEmpty());
        // Download addresses outside GitHub's release path are not usable.
        auto foreign = update::parseRelease(v5Release(QStringLiteral("https://example.invalid"), QStringLiteral("5.1.0")), QStringLiteral("5.0.1"));
        QVERIFY(foreign.ok() && foreign->newerThanRunning && !foreign->canDownload());
        auto plain = update::parseRelease(v5Release(QStringLiteral("http://github.com"), QStringLiteral("5.1.0")), QStringLiteral("5.0.1"));
        QVERIFY(plain.ok() && !plain->canDownload());
    }

    void parseKeepsInstallerNameAndRefusesPrereleases()
    {
        update::setLoopbackAllowedForTests(false);
        auto r = update::parseRelease(installerRelease(kGithub, QStringLiteral("9.0.0")), QStringLiteral("5.0.0"));
        QVERIFY(r.ok());
        QCOMPARE(r->version, QStringLiteral("9.0.0"));
        QVERIFY(r->newerThanRunning);
        QCOMPARE(r->installerName, QStringLiteral("exFAT-Image-Builder-Farewell-9.0.0-win64-setup.exe"));
        QVERIFY(r->sumsUrl.endsWith(QStringLiteral("SHA256SUMS.txt")));
        QVERIFY(r->canDownload());

        // Without a version tag the installer's name still gives the version.
        QJsonObject untagged = QJsonDocument::fromJson(installerRelease(kGithub, QStringLiteral("9.0.0"))).object();
        untagged.insert(QStringLiteral("tag_name"), QStringLiteral("latest"));
        auto u = update::parseRelease(QJsonDocument(untagged).toJson(), QStringLiteral("5.0.0"));
        QVERIFY(u.ok());
        QCOMPARE(u->version, QStringLiteral("9.0.0"));

        QJsonObject pre = QJsonDocument::fromJson(v5Release(kGithub, QStringLiteral("9.0.0"))).object();
        pre.insert(QStringLiteral("prerelease"), true);
        auto p = update::parseRelease(QJsonDocument(pre).toJson(), QStringLiteral("5.0.0"));
        QVERIFY(!p.ok());
        QCOMPARE(p.error().code, ErrorCode::NotFound);
        pre.insert(QStringLiteral("prerelease"), false);
        pre.insert(QStringLiteral("draft"), true);
        QVERIFY(!update::parseRelease(QJsonDocument(pre).toJson(), QStringLiteral("5.0.0")).ok());
        pre.insert(QStringLiteral("draft"), false);
        pre.insert(QStringLiteral("tag_name"), QStringLiteral("nightly"));
        auto n = update::parseRelease(QJsonDocument(pre).toJson(), QStringLiteral("5.0.0"));
        QVERIFY(!n.ok());
        QCOMPARE(n.error().code, ErrorCode::NotFound);
        QVERIFY(!update::parseRelease("not json", QStringLiteral("5.0.0")).ok());
    }

    void checkAndDownloadAgainstLocalServer()
    {
        clearUpdatesDir();
        MiniHttp http;
        QVERIFY(http.listen());
        const QString version = QStringLiteral("9.0.0");
        const QByteArray payload = programBytes('p');
        const QByteArray hash = QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex();
        http.routes[QStringLiteral("/latest")] = {200, v5Release(http.base(), version)};
        http.routes[assetPath(version, kProgram)] = {200, payload};
        http.routes[assetPath(version, QStringLiteral("SHA256SUMS.txt"))] = {200, sumsLine(payload)};

        Env env(m_root.filePath(QStringLiteral("e1")));
        auto check = env.run(update::kCheckJob, {{QStringLiteral("apiUrl"), http.base() + QStringLiteral("/latest")}});
        QVERIFY(check && check->state == JobState::Succeeded);
        const QJsonObject rel = check->outcome->details;
        QCOMPARE(rel.value(QStringLiteral("upToDate")).toBool(), false);
        QCOMPARE(rel.value(QStringLiteral("newer")).toBool(), true);
        QCOMPARE(rel.value(QStringLiteral("canDownload")).toBool(), true);
        QCOMPARE(rel.value(QStringLiteral("version")).toString(), version);
        QCOMPARE(rel.value(QStringLiteral("assetName")).toString(), kProgram);
        QCOMPARE(rel.value(QStringLiteral("installerUrl")).toString(), http.base() + assetPath(version, kProgram));
        QVERIFY(rel.value(QStringLiteral("pageUrl")).toString().endsWith(QStringLiteral("/releases/tag/v9.0.0")));

        auto dl = env.run(update::kDownloadJob, {{QStringLiteral("release"), rel}});
        QVERIFY(dl);
        QVERIFY2(dl->state == JobState::Succeeded, dl->error ? qPrintable(dl->error->toLogString()) : "");
        QCOMPARE(dl->outcome->verification, Verification::Passed);
        // The verified file stays in the cache folder under the release's file name.
        QCOMPARE(QDir::cleanPath(dl->outcome->outputs.first()), QDir::cleanPath(QDir(updatesDir()).filePath(kProgram)));
        QCOMPARE(dl->outcome->details.value(QStringLiteral("file")).toString(), dl->outcome->outputs.first());
        QCOMPARE(dl->outcome->details.value(QStringLiteral("sha256")).toString().toLatin1(), hash);
        QFile f(dl->outcome->outputs.first());
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(QCryptographicHash::hash(f.readAll(), QCryptographicHash::Sha256).toHex(), hash);
        QVERIFY(!QFile::exists(dl->outcome->outputs.first() + QStringLiteral(".part")));
    }

    void checkReportsNewerReleaseWithoutDownload()
    {
        MiniHttp http;
        QVERIFY(http.listen());
        http.routes[QStringLiteral("/latest")] = {200, v5Release(http.base(), QStringLiteral("9.1.0"), 0, false, false)};
        Env env(m_root.filePath(QStringLiteral("e4")));
        auto check = env.run(update::kCheckJob, {{QStringLiteral("apiUrl"), http.base() + QStringLiteral("/latest")}});
        QVERIFY(check && check->state == JobState::Succeeded);
        const QJsonObject rel = check->outcome->details;
        QCOMPARE(rel.value(QStringLiteral("upToDate")).toBool(), false);
        QCOMPARE(rel.value(QStringLiteral("newer")).toBool(), true);
        QCOMPARE(rel.value(QStringLiteral("canDownload")).toBool(), false);
        QCOMPARE(rel.value(QStringLiteral("version")).toString(), QStringLiteral("9.1.0"));
        QVERIFY(rel.value(QStringLiteral("installerUrl")).toString().isEmpty());
        QVERIFY(rel.value(QStringLiteral("pageUrl")).toString().endsWith(QStringLiteral("/releases/tag/v9.1.0")));
        QVERIFY(!rel.value(QStringLiteral("note")).toString().isEmpty());
        QVERIFY(check->outcome->summary.contains(QStringLiteral("9.1.0")));

        // Nothing to download for such a release.
        QVERIFY(!env.run(update::kDownloadJob, {{QStringLiteral("release"), rel}}));
        QVERIFY(env.refused);
        QCOMPARE(env.refused->code, ErrorCode::InvalidArgument);
    }

    void downloadFailsClosed()
    {
        clearUpdatesDir();
        MiniHttp http;
        QVERIFY(http.listen());
        const QString version = QStringLiteral("9.0.1");
        const QByteArray payload = programBytes('z');
        http.routes[QStringLiteral("/latest")] = {200, v5Release(http.base(), version)};
        http.routes[assetPath(version, kProgram)] = {200, payload};
        Env env(m_root.filePath(QStringLiteral("e2")));
        auto check = env.run(update::kCheckJob, {{QStringLiteral("apiUrl"), http.base() + QStringLiteral("/latest")}});
        QVERIFY(check && check->state == JobState::Succeeded);
        const QJsonObject rel = check->outcome->details;
        const QString sumsPath = assetPath(version, QStringLiteral("SHA256SUMS.txt"));
        const QString finalPath = QDir(updatesDir()).filePath(kProgram);
        auto nothingLeft = [&] { return !QFile::exists(finalPath) && !QFile::exists(finalPath + QStringLiteral(".part")); };

        // No checksum file → refused.
        auto noSums = env.run(update::kDownloadJob, {{QStringLiteral("release"), rel}});
        QVERIFY(noSums);
        QCOMPARE(noSums->state, JobState::Failed);
        QCOMPARE(noSums->error->code, ErrorCode::VerificationFailed);

        // A checksum file that lists other files only → refused.
        http.routes[sumsPath] = {200, QByteArray(64, 'f') + "  something else.exe\n"};
        auto unlisted = env.run(update::kDownloadJob, {{QStringLiteral("release"), rel}});
        QCOMPARE(unlisted->state, JobState::Failed);
        QCOMPARE(unlisted->error->code, ErrorCode::VerificationFailed);

        // Wrong checksum → refused, nothing left behind.
        http.routes[sumsPath] = {200, QByteArray(64, 'f') + "  exFAT Image Builder.exe\n"};
        auto bad = env.run(update::kDownloadJob, {{QStringLiteral("release"), rel}});
        QCOMPARE(bad->state, JobState::Failed);
        QCOMPARE(bad->error->code, ErrorCode::VerificationFailed);
        QVERIFY(nothingLeft());

        // Truncated transfer → refused.
        http.routes[sumsPath] = {200, sumsLine(payload)};
        http.routes[assetPath(version, kProgram)] = {200, payload, true};
        auto cut = env.run(update::kDownloadJob, {{QStringLiteral("release"), rel}});
        QCOMPARE(cut->state, JobState::Failed);
        QVERIFY(nothingLeft());

        // More bytes than the release announces → stopped and refused.
        const QByteArray longer = payload + QByteArray(4096, 'x');
        http.routes[sumsPath] = {200, sumsLine(longer)};
        http.routes[assetPath(version, kProgram)] = {200, longer};
        auto big = env.run(update::kDownloadJob, {{QStringLiteral("release"), rel}});
        QCOMPARE(big->state, JobState::Failed);
        QCOMPARE(big->error->code, ErrorCode::VerificationFailed);
        QVERIFY(nothingLeft());

        // Fewer bytes than the release announces (a complete answer) → refused.
        const QByteArray shorter = payload.left(payload.size() - 4096);
        http.routes[sumsPath] = {200, sumsLine(shorter)};
        http.routes[assetPath(version, kProgram)] = {200, shorter};
        auto small = env.run(update::kDownloadJob, {{QStringLiteral("release"), rel}});
        QCOMPARE(small->state, JobState::Failed);
        QCOMPARE(small->error->code, ErrorCode::VerificationFailed);
        QVERIFY(nothingLeft());
    }

    void downloadRefusesForeignAddressesAndDowngrades()
    {
        MiniHttp http;
        QVERIFY(http.listen());
        Env env(m_root.filePath(QStringLiteral("e5")));
        const QJsonObject good = update::parseRelease(v5Release(http.base(), QStringLiteral("9.2.0")), QStringLiteral("5.0.0"))->toJson();
        auto refusedWith = [&](QJsonObject rel) {
            const bool accepted = env.run(update::kDownloadJob, {{QStringLiteral("release"), rel}}).has_value();
            return !accepted && env.refused && env.refused->code == ErrorCode::InvalidArgument;
        };

        // The running version or an older one is never downloaded.
        QJsonObject same = good;
        same.insert(QStringLiteral("version"), QString::fromLatin1(build::kVersion));
        QVERIFY(refusedWith(same));
        QJsonObject old = good;
        old.insert(QStringLiteral("version"), QStringLiteral("1.0.0"));
        QVERIFY(refusedWith(old));
        QJsonObject noVersion = good;
        noVersion.insert(QStringLiteral("version"), QStringLiteral("99x"));
        QVERIFY(refusedWith(noVersion));

        // A file name this app does not publish (it would become a path in the cache folder).
        for (const QString& name : {QStringLiteral("..\\..\\evil.exe"), QStringLiteral("setup.exe"), QStringLiteral("../x/exFAT.Image.Builder.exe")}) {
            QJsonObject renamed = good;
            renamed.insert(QStringLiteral("assetName"), name);
            renamed.insert(QStringLiteral("installerName"), name);
            QVERIFY2(refusedWith(renamed), qPrintable(name));
        }

        // Addresses that are not GitHub release files.
        for (const QString& url : {QStringLiteral("http://github.com/kerrdec97/ps5-exfat-builder/releases/download/v9.2.0/exFAT.Image.Builder.exe"),
                                   QStringLiteral("https://example.invalid/kerrdec97/ps5-exfat-builder/releases/download/v9.2.0/exFAT.Image.Builder.exe"),
                                   QStringLiteral("https://github.com/someone/else/releases/download/v9.2.0/exFAT.Image.Builder.exe"),
                                   QStringLiteral("https://user@github.com/kerrdec97/ps5-exfat-builder/releases/download/v9.2.0/exFAT.Image.Builder.exe"),
                                   QStringLiteral("file:///C:/Windows/System32/cmd.exe"),
                                   QStringLiteral("http://localhost:%1/x.exe").arg(http.port())}) {
            QJsonObject moved = good;
            moved.insert(QStringLiteral("installerUrl"), url);
            QVERIFY2(refusedWith(moved), qPrintable(url));
            QJsonObject movedSums = good;
            movedSums.insert(QStringLiteral("sumsUrl"), url);
            QVERIFY2(refusedWith(movedSums), qPrintable(url));
        }

        // Without the test switch the local server is not accepted either: not
        // for the download and not as the address of the check.
        update::setLoopbackAllowedForTests(false);
        QVERIFY(refusedWith(good));
        QVERIFY(!env.run(update::kCheckJob, {{QStringLiteral("apiUrl"), http.base() + QStringLiteral("/latest")}}));
        QVERIFY(env.refused);
        QCOMPARE(env.refused->code, ErrorCode::InvalidArgument);
        QVERIFY(!env.run(update::kCheckJob, {{QStringLiteral("apiUrl"), QStringLiteral("https://example.invalid/latest")}}));
        QVERIFY(env.refused);
    }

    void redirectsAreFollowedOnlyToAcceptedHosts()
    {
        clearUpdatesDir();
        MiniHttp http;
        QVERIFY(http.listen());
        const QString version = QStringLiteral("9.3.0");
        const QByteArray payload = programBytes('r');
        http.routes[QStringLiteral("/latest")] = {200, v5Release(http.base(), version)};
        http.routes[assetPath(version, QStringLiteral("SHA256SUMS.txt"))] = {200, sumsLine(payload)};
        http.routes[QStringLiteral("/store/blob")] = {200, payload};
        Env env(m_root.filePath(QStringLiteral("e6")));
        auto check = env.run(update::kCheckJob, {{QStringLiteral("apiUrl"), http.base() + QStringLiteral("/latest")}});
        QVERIFY(check && check->state == JobState::Succeeded);
        const QJsonObject rel = check->outcome->details;

        // GitHub answers a release file with a redirect to its file store.
        http.routes[assetPath(version, kProgram)] = {302, {}, false, "Location: " + http.base().toLatin1() + "/store/blob\r\n"};
        auto ok = env.run(update::kDownloadJob, {{QStringLiteral("release"), rel}});
        QVERIFY(ok);
        QVERIFY2(ok->state == JobState::Succeeded, ok->error ? qPrintable(ok->error->toLogString()) : "");

        // A redirect to any other host is not followed.
        clearUpdatesDir();
        http.routes[assetPath(version, kProgram)] =
            {302, {}, false, "Location: http://localhost:" + QByteArray::number(http.port()) + "/store/blob\r\n"};
        auto away = env.run(update::kDownloadJob, {{QStringLiteral("release"), rel}});
        QVERIFY(away);
        QCOMPARE(away->state, JobState::Failed);
        QCOMPARE(away->error->code, ErrorCode::VerificationFailed);
        QVERIFY(!QFile::exists(QDir(updatesDir()).filePath(kProgram)));
        QVERIFY(!QFile::exists(QDir(updatesDir()).filePath(kProgram + QStringLiteral(".part"))));
    }

    void olderOrMissingReleaseIsUpToDate()
    {
        MiniHttp http;
        QVERIFY(http.listen());
        http.routes[QStringLiteral("/old")] = {200, v5Release(http.base(), QStringLiteral("1.0.0"))};
        http.routes[QStringLiteral("/same")] = {200, v5Release(http.base(), QString::fromLatin1(build::kVersion))};
        QJsonObject pre = QJsonDocument::fromJson(v5Release(http.base(), QStringLiteral("9.9.0"))).object();
        pre.insert(QStringLiteral("prerelease"), true);
        http.routes[QStringLiteral("/pre")] = {200, QJsonDocument(pre).toJson()};
        Env env(m_root.filePath(QStringLiteral("e3")));
        for (const QString& path : {QStringLiteral("/old"), QStringLiteral("/same"), QStringLiteral("/pre"), QStringLiteral("/missing")}) {
            auto r = env.run(update::kCheckJob, {{QStringLiteral("apiUrl"), http.base() + path}});
            QVERIFY2(r && r->state == JobState::Succeeded, qPrintable(path));
            QVERIFY2(r->outcome->details.value(QStringLiteral("upToDate")).toBool(), qPrintable(path));
            QVERIFY2(!r->outcome->details.value(QStringLiteral("newer")).toBool(), qPrintable(path));
        }
    }

    void rateLimitSaysWhenToTryAgain()
    {
        MiniHttp http;
        QVERIFY(http.listen());
        const qint64 reset = QDateTime::currentSecsSinceEpoch() + 20 * 60;
        http.routes[QStringLiteral("/limited")] = {403, "{}", false, "X-RateLimit-Reset: " + QByteArray::number(reset) + "\r\n"};
        http.routes[QStringLiteral("/limited-plain")] = {429, "{}"};
        Env env(m_root.filePath(QStringLiteral("e7")));
        auto r = env.run(update::kCheckJob, {{QStringLiteral("apiUrl"), http.base() + QStringLiteral("/limited")}});
        QVERIFY(r);
        QCOMPARE(r->state, JobState::Failed);
        QVERIFY2(r->error->message.contains(QStringLiteral("20 min")), qPrintable(r->error->message));
        auto plain = env.run(update::kCheckJob, {{QStringLiteral("apiUrl"), http.base() + QStringLiteral("/limited-plain")}});
        QVERIFY(plain);
        QCOMPARE(plain->state, JobState::Failed);
        QVERIFY(plain->error->message.contains(QStringLiteral("an hour")));
    }
};

QTEST_GUILESS_MAIN(TstUpdate)
#include "tst_update.moc"
