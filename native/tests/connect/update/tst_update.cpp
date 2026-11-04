// Update checker: release parsing, version comparison, checksum lookup and the
// check/download jobs against a local HTTP server (never the real GitHub).

#include "connect/update/Module.h"
#include "core/BuildInfo.h"
#include "core/Paths.h"
#include "core/Settings.h"
#include "jobs/JobQueue.h"
#include "support/TestSupport.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>

using namespace fw;

namespace {

// Minimal HTTP/1.1 server: GET <path> → fixed responses.
class MiniHttp : public QObject {
public:
    struct Response {
        int status = 200;
        QByteArray body;
        bool truncate = false; // advertise a longer Content-Length than sent
    };
    QMap<QString, Response> routes;

    bool listen() { return m_server.listen(QHostAddress::LocalHost, 0); }
    QString base() const { return QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort()); }

    MiniHttp()
    {
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket* s = m_server.nextPendingConnection()) {
                connect(s, &QTcpSocket::readyRead, s, [this, s] {
                    m_buf[s] += s->readAll();
                    if (!m_buf[s].contains("\r\n\r\n"))
                        return;
                    const QList<QByteArray> parts = m_buf[s].split(' ');
                    const QString path = parts.size() > 1 ? QString::fromLatin1(parts[1]) : QString();
                    const Response r = routes.value(path, Response{404, "not found"});
                    const qint64 advertised = r.truncate ? r.body.size() + 1000 : r.body.size();
                    QByteArray head = "HTTP/1.1 " + QByteArray::number(r.status) + " X\r\nContent-Length: "
                        + QByteArray::number(advertised) + "\r\nConnection: close\r\n\r\n";
                    s->write(head + r.body);
                    s->flush();
                    s->disconnectFromHost();
                });
                connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
            }
        });
    }

private:
    QTcpServer m_server;
    QMap<QTcpSocket*, QByteArray> m_buf;
};

QByteArray releaseJson(const QString& base, const QString& version, bool withSums = true, bool withInstaller = true)
{
    QJsonArray assets;
    const QString inst = QStringLiteral("exFAT-Image-Builder-Farewell-%1-win64-setup.exe").arg(version);
    if (withInstaller)
        assets.append(QJsonObject{{QStringLiteral("name"), inst},
                                  {QStringLiteral("browser_download_url"), base + QStringLiteral("/dl/") + inst},
                                  {QStringLiteral("size"), 6 * 1024 * 1024}});
    assets.append(QJsonObject{{QStringLiteral("name"), QStringLiteral("exFAT.Image.Builder.exe")},
                              {QStringLiteral("browser_download_url"), base + QStringLiteral("/dl/legacy.exe")}});
    if (withSums)
        assets.append(QJsonObject{{QStringLiteral("name"), QStringLiteral("SHA256SUMS.txt")},
                                  {QStringLiteral("browser_download_url"), base + QStringLiteral("/dl/SHA256SUMS.txt")}});
    QJsonObject o{{QStringLiteral("tag_name"), QStringLiteral("v") + version},
                  {QStringLiteral("name"), QStringLiteral("Farewell ") + version},
                  {QStringLiteral("body"), QStringLiteral("Notes")},
                  {QStringLiteral("html_url"), base + QStringLiteral("/release")},
                  {QStringLiteral("assets"), assets}};
    return QJsonDocument(o).toJson();
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
            auto id = queue->enqueue(kind, params);
            if (!id.ok())
                return std::nullopt;
            QTest::qWaitFor([&] { auto r = queue->record(id.value()); return r && isTerminal(r->state); }, 20000);
            return queue->record(id.value());
        }
    };

private slots:
    void initTestCase() { QVERIFY(m_root.isValid()); fwtest::isolateAppData(m_root); }

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

    void parseIgnoresLegacyAssetsAndPrereleases()
    {
        auto r = update::parseRelease(releaseJson(QStringLiteral("http://x"), QStringLiteral("9.0.0")), QStringLiteral("5.0.0"));
        QVERIFY(r.ok());
        QCOMPARE(r->version, QStringLiteral("9.0.0"));
        QVERIFY(r->newerThanRunning);
        QVERIFY(r->sumsUrl.endsWith(QStringLiteral("SHA256SUMS.txt")));
        auto legacyOnly = update::parseRelease(releaseJson(QStringLiteral("http://x"), QStringLiteral("9.0.0"), true, false), QStringLiteral("5.0.0"));
        QVERIFY(!legacyOnly.ok());
        QCOMPARE(legacyOnly.error().code, ErrorCode::NotFound);
        QJsonObject pre = QJsonDocument::fromJson(releaseJson(QStringLiteral("http://x"), QStringLiteral("9.0.0"))).object();
        pre.insert(QStringLiteral("prerelease"), true);
        QVERIFY(!update::parseRelease(QJsonDocument(pre).toJson(), QStringLiteral("5.0.0")).ok());
        QVERIFY(!update::parseRelease("not json", QStringLiteral("5.0.0")).ok());
    }

    void checkAndDownloadAgainstLocalServer()
    {
        MiniHttp http;
        QVERIFY(http.listen());
        const QString inst = QStringLiteral("exFAT-Image-Builder-Farewell-9.0.0-win64-setup.exe");
        QByteArray payload(6 * 1024 * 1024, 'p');
        payload[123] = 'q';
        const QByteArray hash = QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex();
        http.routes[QStringLiteral("/latest")] = {200, releaseJson(http.base(), QStringLiteral("9.0.0"))};
        http.routes[QStringLiteral("/dl/") + inst] = {200, payload};
        http.routes[QStringLiteral("/dl/SHA256SUMS.txt")] = {200, hash + "  " + inst.toLatin1() + "\n"};

        Env env(m_root.filePath(QStringLiteral("e1")));
        auto check = env.run(update::kCheckJob, {{QStringLiteral("apiUrl"), http.base() + QStringLiteral("/latest")}});
        QVERIFY(check && check->state == JobState::Succeeded);
        QCOMPARE(check->outcome->details.value(QStringLiteral("upToDate")).toBool(), false);
        QCOMPARE(check->outcome->details.value(QStringLiteral("version")).toString(), QStringLiteral("9.0.0"));

        auto dl = env.run(update::kDownloadJob, {{QStringLiteral("release"), check->outcome->details}});
        QVERIFY(dl);
        QVERIFY2(dl->state == JobState::Succeeded, dl->error ? qPrintable(dl->error->toLogString()) : "");
        QCOMPARE(dl->outcome->verification, Verification::Passed);
        QFile f(dl->outcome->outputs.first());
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(QCryptographicHash::hash(f.readAll(), QCryptographicHash::Sha256).toHex(), hash);
    }

    void downloadFailsClosed()
    {
        MiniHttp http;
        QVERIFY(http.listen());
        const QString inst = QStringLiteral("exFAT-Image-Builder-Farewell-9.0.1-win64-setup.exe");
        QByteArray payload(6 * 1024 * 1024, 'z');
        http.routes[QStringLiteral("/latest")] = {200, releaseJson(http.base(), QStringLiteral("9.0.1"))};
        http.routes[QStringLiteral("/dl/") + inst] = {200, payload};
        Env env(m_root.filePath(QStringLiteral("e2")));
        auto check = env.run(update::kCheckJob, {{QStringLiteral("apiUrl"), http.base() + QStringLiteral("/latest")}});
        QVERIFY(check && check->state == JobState::Succeeded);
        const QJsonObject rel = check->outcome->details;

        // No checksum file → refused.
        auto noSums = env.run(update::kDownloadJob, {{QStringLiteral("release"), rel}});
        QCOMPARE(noSums->state, JobState::Failed);
        QCOMPARE(noSums->error->code, ErrorCode::VerificationFailed);

        // Wrong checksum → refused, nothing left behind.
        http.routes[QStringLiteral("/dl/SHA256SUMS.txt")] = {200, QByteArray(64, 'f') + "  " + inst.toLatin1() + "\n"};
        auto bad = env.run(update::kDownloadJob, {{QStringLiteral("release"), rel}});
        QCOMPARE(bad->state, JobState::Failed);
        QCOMPARE(bad->error->code, ErrorCode::VerificationFailed);
        const QString dir = QDir(Paths::cacheDir()).filePath(QStringLiteral("updates"));
        QVERIFY(!QFile::exists(QDir(dir).filePath(inst)));
        QVERIFY(!QFile::exists(QDir(dir).filePath(inst + QStringLiteral(".part"))));

        // Truncated transfer → refused.
        const QByteArray good = QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex();
        http.routes[QStringLiteral("/dl/SHA256SUMS.txt")] = {200, good + "  " + inst.toLatin1() + "\n"};
        http.routes[QStringLiteral("/dl/") + inst] = {200, payload, true};
        auto cut = env.run(update::kDownloadJob, {{QStringLiteral("release"), rel}});
        QCOMPARE(cut->state, JobState::Failed);
    }

    void olderOrMissingReleaseIsUpToDate()
    {
        MiniHttp http;
        QVERIFY(http.listen());
        http.routes[QStringLiteral("/old")] = {200, releaseJson(http.base(), QStringLiteral("1.0.0"))};
        Env env(m_root.filePath(QStringLiteral("e3")));
        auto old = env.run(update::kCheckJob, {{QStringLiteral("apiUrl"), http.base() + QStringLiteral("/old")}});
        QVERIFY(old && old->state == JobState::Succeeded);
        QVERIFY(old->outcome->details.value(QStringLiteral("upToDate")).toBool());
        auto none = env.run(update::kCheckJob, {{QStringLiteral("apiUrl"), http.base() + QStringLiteral("/missing")}});
        QVERIFY(none && none->state == JobState::Succeeded);
        QVERIFY(none->outcome->details.value(QStringLiteral("upToDate")).toBool());
    }
};

QTEST_GUILESS_MAIN(TstUpdate)
#include "tst_update.moc"
