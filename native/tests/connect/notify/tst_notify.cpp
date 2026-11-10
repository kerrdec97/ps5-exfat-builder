// Discord webhook notification: the setting and its validation, the legacy
// key, the summary (no folders), and a real POST from a finished queue to a
// local HTTP stub (never to Discord).

#include "connect/notify/DiscordWebhook.h"
#include "connect/notify/Module.h"
#include "connect/ps5/FakePs5.h"
#include "jobs/JobQueue.h"
#include "jobs/QueueCompletionWatcher.h"
#include "support/TestSupport.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QTimeZone>

#include <memory>

using namespace fw;
using namespace fw::notify;

namespace {

const QString kHook = QStringLiteral("https://discord.com/api/webhooks/123456789/AbC_dEf-123");

class TitledJob : public Job {
public:
    explicit TitledJob(QJsonObject p) : m_p(std::move(p)) {}
    QString kind() const override { return m_p.value(QStringLiteral("kind")).toString(QStringLiteral("test.titled")); }
    QString title() const override { return m_p.value(QStringLiteral("title")).toString(); }
    QJsonObject parameters() const override { return m_p; }
    Result<JobOutcome> run(JobContext&) override
    {
        QThread::msleep(20);
        if (m_p.value(QStringLiteral("fail")).toBool())
            return Error(ErrorCode::IoError, QStringLiteral("Disk on fire at C:\\Users\\me\\x"));
        JobOutcome o;
        o.summary = QStringLiteral("ok");
        o.verification = Verification::Passed;
        return o;
    }

private:
    QJsonObject m_p;
};

struct Env {
    std::shared_ptr<SettingsSchema> schema = std::make_shared<SettingsSchema>();
    DependencyRegistry deps;
    JobFactory jobs;
    std::unique_ptr<SettingsStore> store;
    std::unique_ptr<JobQueue> queue;
    std::unique_ptr<QueueCompletionWatcher> watcher;
    explicit Env(const QString& dir)
    {
        ModuleContext ctx{*schema, deps, jobs};
        registerModule(ctx);
        // Keys the queue and the watcher read (declared by the general module in the app).
        for (auto [key, def] : {std::pair{QStringLiteral("performance.maxConcurrentJobs"), QVariant(1)},
                                std::pair{QStringLiteral("general.autoRetryCount"), QVariant(0)},
                                std::pair{QStringLiteral("general.autoRetryDelaySeconds"), QVariant(0)},
                                std::pair{QStringLiteral("general.afterQueueDelaySeconds"), QVariant(60)}}) {
            SettingDef d;
            d.key = key;
            d.section = QStringLiteral("General");
            d.label = key;
            d.type = SettingType::Int;
            d.defaultValue = def;
            schema->add(d);
        }
        for (const QString& key : {QStringLiteral("general.afterQueueAction")}) {
            SettingDef d;
            d.key = key;
            d.section = QStringLiteral("General");
            d.label = key;
            d.type = SettingType::String;
            d.defaultValue = QStringLiteral("none");
            schema->add(d);
        }
        jobs.add(QStringLiteral("test.titled"), QStringLiteral("Build exFAT image"),
                 [](const QJsonObject& p) -> Result<std::unique_ptr<Job>> { return std::unique_ptr<Job>(new TitledJob(p)); });
        jobs.add(QStringLiteral("test.quiet"), QStringLiteral("Quiet check"),
                 [](const QJsonObject& p) -> Result<std::unique_ptr<Job>> { return std::unique_ptr<Job>(new TitledJob(p)); });
        store = std::make_unique<SettingsStore>(schema, dir + QStringLiteral("/settings.json"));
        (void)store->load();
        queue = std::make_unique<JobQueue>(*store, jobs);
        watcher = std::make_unique<QueueCompletionWatcher>(*queue, *store);
    }
};

JobRecord record(const QString& id, const QString& kind, const QString& title, JobState state)
{
    JobRecord r;
    r.id = id;
    r.kind = kind;
    r.title = title;
    r.state = state;
    return r;
}

QJsonObject requestJson(const QByteArray& request)
{
    const qsizetype end = request.indexOf("\r\n\r\n");
    return QJsonDocument::fromJson(request.mid(end + 4)).object();
}

// FW_OFFLINE is set for every test run (CTest); the delivery tests lift it.
class OnlineScope {
public:
    OnlineScope() : m_had(qEnvironmentVariableIsSet("FW_OFFLINE")), m_value(qgetenv("FW_OFFLINE")) { qunsetenv("FW_OFFLINE"); }
    ~OnlineScope()
    {
        if (m_had)
            qputenv("FW_OFFLINE", m_value);
    }

private:
    bool m_had;
    QByteArray m_value;
};

const QByteArray kNoContent = "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";

} // namespace

class TstNotify : public QObject {
    Q_OBJECT
    QTemporaryDir m_tmp;
    int m_n = 0;
    QString fresh()
    {
        const QString p = m_tmp.filePath(QString::number(++m_n));
        QDir().mkpath(p);
        return p;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_tmp.isValid());
        fwtest::isolateAppData(m_tmp);
    }

    void webhookAddressesAreValidated()
    {
        for (const QString& ok : {QString(), kHook, QStringLiteral("https://discordapp.com/api/webhooks/1/x"),
                                  QStringLiteral("https://canary.discord.com/api/v10/webhooks/1/x?thread_id=5"),
                                  QStringLiteral("  https://DISCORD.com/api/webhooks/1/x/  ")})
            QVERIFY2(discordWebhookProblem(ok).isEmpty(), qPrintable(ok));
        for (const QString& bad : {QStringLiteral("http://discord.com/api/webhooks/1/x"), QStringLiteral("https://evil.com/api/webhooks/1/x"),
                                   QStringLiteral("https://discord.com.evil.com/api/webhooks/1/x"),
                                   QStringLiteral("https://discord.com/api/webhooks/abc/x"), QStringLiteral("https://discord.com/api/webhooks/1"),
                                   QStringLiteral("https://user@discord.com/api/webhooks/1/x"),
                                   QStringLiteral("https://discord.com:8443/api/webhooks/1/x"), QStringLiteral("https://discord.com/channels/1/2"),
                                   QStringLiteral("not a url")})
            QVERIFY2(!discordWebhookProblem(bad).isEmpty(), qPrintable(bad));
    }

    void settingIsDeclaredAndTheLegacyKeyImports()
    {
        Env env(fresh());
        const SettingDef* d = env.schema->find(keys::kDiscordWebhookUrl);
        QVERIFY(d);
        QCOMPARE(d->section, QStringLiteral("General"));
        QVERIFY(d->advanced);
        QVERIFY(!d->description.isEmpty());
        QCOMPARE(d->defaultValue.toString(), QString()); // off
        QVERIFY(env.schema->normalize(keys::kDiscordWebhookUrl, kHook).ok());
        QVERIFY(!env.schema->normalize(keys::kDiscordWebhookUrl, QStringLiteral("https://example.com/hook")).ok());
        QVERIFY(env.schema->legacyKeys().count(QStringLiteral("discord_webhook")) == 1);

        const QString dir = fresh();
        QVERIFY(fwtest::writeTextFile(dir + QStringLiteral("/set.json"), "{\"discord_webhook\": \"" + kHook.toUtf8() + "\"}"));
        QCOMPARE(env.store->importLegacy(dir + QStringLiteral("/set.json")).value(), 1);
        QCOMPARE(env.store->value(keys::kDiscordWebhookUrl).toString(), kHook);
        // Empty (the legacy default) and invalid values are not imported.
        QVERIFY(fwtest::writeTextFile(dir + QStringLiteral("/empty.json"), "{\"discord_webhook\": \"\"}"));
        QCOMPARE(env.store->importLegacy(dir + QStringLiteral("/empty.json")).value(), 0);
        QVERIFY(fwtest::writeTextFile(dir + QStringLiteral("/bad.json"), "{\"discord_webhook\": \"http://x/y\"}"));
        QCOMPARE(env.store->importLegacy(dir + QStringLiteral("/bad.json")).value(), 0);
        QCOMPARE(env.store->value(keys::kDiscordWebhookUrl).toString(), kHook);
    }

    void foldersAreCutToFileNames()
    {
        QCOMPARE(withoutFolders(QStringLiteral("D:\\Dumps\\Astro Bot (PPSA01325)\\Astro.exfat")), QStringLiteral("Astro.exfat"));
        QCOMPARE(withoutFolders(QStringLiteral("Copied D:\\a\\b.exfat to E:/c d/e.ffpkg")), QStringLiteral("Copied b.exfat to e.ffpkg"));
        QCOMPARE(withoutFolders(QStringLiteral("\\\\nas\\share\\games\\x.pkg")), QStringLiteral("x.pkg"));
        QCOMPARE(withoutFolders(QStringLiteral("/home/me/My Games/Game.exfat")), QStringLiteral("Game.exfat"));
        QCOMPARE(withoutFolders(QStringLiteral("Upload ~/Games/a.bin")), QStringLiteral("Upload a.bin"));
        // Ordinary titles are left alone.
        for (const QString& keep : {QStringLiteral("Astro Bot (PPSA01325)"), QStringLiteral("AC/DC Live"), QStringLiteral("50 / 50"),
                                    QStringLiteral("Upload 3 items to PS5"), QStringLiteral("Game: Remastered 12:30")})
            QCOMPARE(withoutFolders(keep), keep);
        QCOMPARE(redactWebhookUrl(QStringLiteral("posting to ") + kHook + QStringLiteral(" failed")),
                 QStringLiteral("posting to https://discord.com/api/webhooks/123456789/<token hidden> failed"));
    }

    void summaryAndPayload()
    {
        const std::vector<JobRecord> recs = {
            record(QStringLiteral("1"), QStringLiteral("test.titled"), QStringLiteral("C:\\Users\\me\\Dumps\\Astro.exfat"), JobState::Succeeded),
            record(QStringLiteral("2"), QStringLiteral("test.titled"), QStringLiteral("@everyone *bold* game"), JobState::Failed),
            record(QStringLiteral("3"), QStringLiteral("test.titled"), QStringLiteral("Third"), JobState::Cancelled),
            record(QStringLiteral("1"), QStringLiteral("test.titled"), QStringLiteral("duplicate"), JobState::Succeeded),
            record(QStringLiteral("4"), QStringLiteral("test.titled"), QStringLiteral("still running"), JobState::Running),
            record(QStringLiteral("5"), QStringLiteral("test.quiet"), QStringLiteral("excluded"), JobState::Succeeded),
        };
        const auto s = summarizeRun(recs, [](const QString&) { return QStringLiteral("Build exFAT image"); },
                                    [](const QString& k) { return k != QLatin1String("test.quiet"); });
        QCOMPARE(s.items.size(), 3);
        QCOMPARE(s.succeeded, 1);
        QCOMPARE(s.failed, 1);
        QCOMPARE(s.cancelled, 1);
        QCOMPARE(s.items[0].title, QStringLiteral("Astro.exfat"));
        const QJsonObject body = QJsonDocument::fromJson(discordPayload(s, QDateTime(QDate(2026, 11, 4), QTime(12, 0), QTimeZone::utc()))).object();
        const QJsonObject e = body.value(QStringLiteral("embeds")).toArray().at(0).toObject();
        QVERIFY(e.value(QStringLiteral("title")).toString().endsWith(QStringLiteral("Queue finished with 1 failure")));
        const QString desc = e.value(QStringLiteral("description")).toString();
        QVERIFY2(desc.startsWith(QStringLiteral("1 job done · 1 failed · 1 cancelled")), qPrintable(desc));
        QVERIFY(desc.contains(QStringLiteral("Build exFAT image: Astro.exfat")));
        QVERIFY(desc.contains(QStringLiteral("\\*bold\\*"))); // markdown escaped
        QVERIFY(!desc.contains(QStringLiteral("Users")));
        QVERIFY(!desc.contains(QStringLiteral("excluded")) && !desc.contains(QStringLiteral("running")));
        QCOMPARE(e.value(QStringLiteral("color")).toInt(), 0xe5484d);
        QCOMPARE(e.value(QStringLiteral("timestamp")).toString(), QStringLiteral("2026-11-04T12:00:00Z"));
        QVERIFY(!e.value(QStringLiteral("footer")).toObject().value(QStringLiteral("text")).toString().isEmpty());
        // A title cannot plant a masked link ("[text](address)") in the channel.
        const QueueRunSummary link = summarizeRun({record(QStringLiteral("9"), QStringLiteral("test.titled"),
                                                          QStringLiteral("[Patch notes](evil.example)"), JobState::Succeeded)},
                                                  [](const QString&) { return QStringLiteral("Build exFAT image"); },
                                                  [](const QString&) { return true; });
        const QString linkDesc = QJsonDocument::fromJson(discordPayload(link, QDateTime::currentDateTimeUtc()))
                                     .object().value(QStringLiteral("embeds")).toArray().at(0).toObject()
                                     .value(QStringLiteral("description")).toString();
        QVERIFY2(linkDesc.contains(QStringLiteral("\\[Patch notes\\]\\(evil.example\\)")), qPrintable(linkDesc));
        QCOMPARE(body.value(QStringLiteral("allowed_mentions")).toObject().value(QStringLiteral("parse")).toArray().size(), 0);

        std::vector<JobRecord> many;
        for (int i = 0; i < 14; ++i)
            many.push_back(record(QString::number(i), QStringLiteral("k"), QStringLiteral("Job %1").arg(i), JobState::Succeeded));
        const QJsonObject big = QJsonDocument::fromJson(discordPayload(summarizeRun(many))).object();
        const QJsonObject be = big.value(QStringLiteral("embeds")).toArray().at(0).toObject();
        QVERIFY(be.value(QStringLiteral("description")).toString().endsWith(QStringLiteral("… and 4 more")));
        QCOMPARE(be.value(QStringLiteral("color")).toInt(), 0x4caf50);
        QVERIFY(summarizeRun({}).isEmpty());
    }

    void nothingIsSentWhenOffOfflineOrEmpty()
    {
        Env env(fresh());
        fake::FakeHttpUpload stub(kNoContent);
        DiscordNotifier n(*env.store);
        n.setDeliveryUrlForTests(QUrl(QStringLiteral("http://127.0.0.1:%1/api/webhooks/1/x").arg(stub.port())));
        QueueRunSummary s;
        s.succeeded = 1;
        s.items << QueueRunItem{QStringLiteral("k"), QStringLiteral("Kind"), QStringLiteral("t"), JobState::Succeeded};
        if (!qEnvironmentVariableIsSet("FW_OFFLINE"))
            qputenv("FW_OFFLINE", "1"); // as under CTest
        QVERIFY(!n.notify(s)); // setting empty
        QVERIFY(env.store->setValue(keys::kDiscordWebhookUrl, kHook).ok());
        QVERIFY(!networkAllowed());
        QVERIFY(!n.notify(s)); // FW_OFFLINE
        {
            OnlineScope online;
            QVERIFY(networkAllowed());
            QVERIFY(!n.notify(QueueRunSummary{})); // nothing to report
        }
        QTest::qWait(200);
        QVERIFY(stub.request().isEmpty());
    }

    void finishedQueuePostsToTheWebhook()
    {
        Env env(fresh());
        QVERIFY(env.store->setValue(keys::kDiscordWebhookUrl, kHook).ok());
        fake::FakeHttpUpload stub(kNoContent);
        DiscordNotifier n(*env.store);
        n.setDeliveryUrlForTests(QUrl(QStringLiteral("http://127.0.0.1:%1/api/webhooks/123456789/AbC").arg(stub.port())));
        n.watch(*env.queue, *env.watcher, [&](const QString& k) { return env.jobs.displayName(k); },
                [](const QString& k) { return k != QLatin1String("test.quiet"); });
        QSignalSpy delivered(&n, &DiscordNotifier::delivered);
        OnlineScope online;

        // A run of only excluded kinds posts nothing.
        QSignalSpy finished(env.watcher.get(), &QueueCompletionWatcher::queueFinished);
        QVERIFY(env.queue->enqueue(QStringLiteral("test.quiet"), {{QStringLiteral("kind"), QStringLiteral("test.quiet")},
                                                                 {QStringLiteral("title"), QStringLiteral("check")}}).ok());
        QVERIFY(finished.wait(10000));
        QTest::qWait(300);
        QCOMPARE(delivered.count(), 0);
        QVERIFY(stub.request().isEmpty());

        QVERIFY(env.queue->enqueue(QStringLiteral("test.titled"), {{QStringLiteral("title"), QStringLiteral("/home/me/Secret Folder/Game.exfat")}}).ok());
        QVERIFY(env.queue->enqueue(QStringLiteral("test.titled"), {{QStringLiteral("title"), QStringLiteral("D:\\Private\\Other.ffpkg")},
                                                                  {QStringLiteral("fail"), true}}).ok());
        QVERIFY(delivered.wait(15000));
        QCOMPARE(delivered.at(0).at(0).toBool(), true);
        QCOMPARE(delivered.at(0).at(1).toInt(), 204);
        const QByteArray req = stub.request();
        QVERIFY2(req.startsWith("POST /api/webhooks/123456789/AbC HTTP/1.1\r\n"), req.left(80).constData());
        QVERIFY(req.contains("application/json"));
        const QJsonObject e = requestJson(req).value(QStringLiteral("embeds")).toArray().at(0).toObject();
        const QString desc = e.value(QStringLiteral("description")).toString();
        QVERIFY2(desc.contains(QStringLiteral("Build exFAT image: Game.exfat")), qPrintable(desc));
        QVERIFY(desc.contains(QStringLiteral("Other.ffpkg")));
        QVERIFY(desc.startsWith(QStringLiteral("1 job done · 1 failed")));
        for (const char* secret : {"Secret Folder", "/home/me", "Private", "Users", "Disk on fire", "check"})
            QVERIFY2(!req.contains(secret), secret);
    }

    void failedDeliveryIsReportedWithoutTheToken()
    {
        Env env(fresh());
        QVERIFY(env.store->setValue(keys::kDiscordWebhookUrl, kHook).ok());
        fake::FakeHttpUpload stub("HTTP/1.1 404 Not Found\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}");
        DiscordNotifier n(*env.store);
        n.setDeliveryUrlForTests(QUrl(QStringLiteral("http://127.0.0.1:%1/api/webhooks/1/x").arg(stub.port())));
        QSignalSpy delivered(&n, &DiscordNotifier::delivered);
        OnlineScope online;
        QueueRunSummary s;
        s.succeeded = 1;
        s.items << QueueRunItem{QStringLiteral("k"), QStringLiteral("Kind"), QStringLiteral("t"), JobState::Succeeded};
        QVERIFY(n.notify(s));
        QVERIFY(delivered.wait(10000));
        QCOMPARE(delivered.at(0).at(0).toBool(), false);
        QCOMPARE(delivered.at(0).at(1).toInt(), 404);
        QVERIFY(!delivered.at(0).at(2).toString().contains(QStringLiteral("AbC_dEf")));
    }
};

QTEST_GUILESS_MAIN(TstNotify)
#include "tst_notify.moc"
