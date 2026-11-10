// Home and Queue pages in the real main window, with real jobs:
//
//  * Home › "Pick up where you left off" stops offering a failed job once
//    Retry was used (the retry is a new job), also after the retry succeeded.

#include "app/AppContext.h"
#include "app/MainWindow.h"
#include "app/pages/HomePage.h"
#include "app/pages/QueuePage.h"
#include "app/services/JobSubmit.h"
#include "app/ui/Theme.h"
#include "app/ui/Widgets.h"
#include "support/TestSupport.h"

#include <QDir>
#include <QLabel>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>

using namespace fw;

namespace {

bool waitTerminal(JobQueue& q, const QString& id, int ms = 120000)
{
    return QTest::qWaitFor([&] {
        auto r = q.record(id);
        return r && isTerminal(r->state);
    }, ms);
}

bool makeGame(const QString& dir)
{
    return fwtest::writeTextFile(dir + QStringLiteral("/sce_sys/param.json"),
                                 R"({"titleId":"PPSA00003","contentVersion":"01.000.000","localizedParameters":{"defaultLanguage":"en-US","en-US":{"titleName":"Home Test"}}})")
           && fwtest::writeRandomFile(dir + QStringLiteral("/eboot.bin"), 90000, 5);
}

} // namespace

class TstPagesHomeQueue : public QObject {
    Q_OBJECT
    QTemporaryDir m_root;
    std::unique_ptr<AppContext> m_ctx;
    std::unique_ptr<MainWindow> m_window;

private slots:
    void initTestCase()
    {
        QVERIFY(m_root.isValid());
        fwtest::isolateAppData(m_root);
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_root.filePath(QStringLiteral("qsettings")));
        theme::apply(qApp);
        m_ctx = std::make_unique<AppContext>();
        QVERIFY(m_ctx->initialize().ok());
        m_window = std::make_unique<MainWindow>(*m_ctx);
        m_window->resize(1440, 920);
        m_window->show();
        QVERIFY(QTest::qWaitForWindowExposed(m_window.get()));
    }

    void cleanupTestCase()
    {
        m_window.reset();
        m_ctx.reset();
    }

    void homeStopsOfferingARetriedJob()
    {
        if (!jobs::isAvailable(*m_ctx, QStringLiteral("image.exfat.build")))
            QSKIP("image engines not registered in this build");
        auto* home = qobject_cast<pages::HomePage*>(m_window->page(QStringLiteral("home")));
        QVERIFY(home);
        auto* bar = home->findChild<ui::InfoBar*>(QStringLiteral("home-continue"));
        QVERIFY(bar);
        auto& q = m_ctx->queue();

        // A build whose source disappears before it starts: it fails.
        const QString game = m_root.filePath(QStringLiteral("game"));
        QVERIFY(makeGame(game));
        q.holdQueue(true);
        auto r = jobs::submitImageBuild(*m_ctx, jobs::kFormatExfat, game, m_root.path(), QStringLiteral("home-test"), {});
        QVERIFY2(r.ok(), qPrintable(r.ok() ? QString() : r.error().message));
        QVERIFY(QDir(game).removeRecursively());
        q.holdQueue(false);
        QVERIFY(waitTerminal(q, r.value()));
        QCOMPARE(q.record(r.value())->state, JobState::Failed);
        home->refresh();
        QVERIFY(!bar->isHidden());

        // The source is back; Retry queues a new job and the bar has done its part.
        QVERIFY(makeGame(game));
        ui::Link* retry = nullptr;
        for (auto* l : bar->findChildren<ui::Link*>())
            if (l->text() == QLatin1String("Retry"))
                retry = l;
        QVERIFY(retry);
        QStringList before;
        for (const JobRecord& rec : q.records())
            before << rec.id;
        emit retry->activated();
        QString second;
        for (const JobRecord& rec : q.records())
            if (!before.contains(rec.id))
                second = rec.id;
        QVERIFY(!second.isEmpty());
        QVERIFY(bar->isHidden());
        QVERIFY(waitTerminal(q, second));
        QVERIFY2(q.record(second)->state == JobState::Succeeded,
                 qPrintable(q.record(second)->error ? q.record(second)->error->message : QString()));
        // The old failed record is still in the queue, but it is not offered again.
        QVERIFY(q.record(r.value()).has_value());
        home->refresh();
        QVERIFY(bar->isHidden());
        q.clearFinished();
    }

    // The Queue's details name a job's parameters in words, and a job the user
    // cancelled is not reported as a failure in the status bar.
    void queueDetailsAndCounts()
    {
        if (!jobs::isAvailable(*m_ctx, QStringLiteral("image.exfat.build")))
            QSKIP("image engines not registered in this build");
        auto* queue = qobject_cast<pages::QueuePage*>(m_window->page(QStringLiteral("queue")));
        QVERIFY(queue);
        auto& q = m_ctx->queue();
        const QString game = m_root.filePath(QStringLiteral("queue-game"));
        QVERIFY(makeGame(game));
        q.holdQueue(true);
        auto r = jobs::submitImageBuild(*m_ctx, jobs::kFormatExfat, game, m_root.path(), QStringLiteral("queue-test"), {},
                                        QJsonObject{{QStringLiteral("titleId"), QStringLiteral("PPSA00003")}});
        QVERIFY2(r.ok(), qPrintable(r.ok() ? QString() : r.error().message));
        m_window->navigate(QStringLiteral("queue"), {{QStringLiteral("jobId"), r.value()}});
        auto* detail = queue->findChild<QWidget*>(QStringLiteral("queue-detail"));
        QVERIFY(detail);
        QStringList labels;
        for (auto* l : detail->findChildren<QLabel*>())
            labels << l->text();
        QVERIFY2(labels.contains(QStringLiteral("Source")), qPrintable(labels.join(u'|')));
        QVERIFY(labels.contains(QStringLiteral("Output")));
        QVERIFY(labels.contains(QStringLiteral("Title ID")));
        QVERIFY(!labels.contains(QStringLiteral("TitleId")));
        QVERIFY(!queue->statusContext().contains(QStringLiteral("cancelled")));

        q.cancel(r.value());
        QVERIFY(waitTerminal(q, r.value()));
        QCOMPARE(q.record(r.value())->state, JobState::Cancelled);
        QVERIFY2(queue->statusContext().contains(QStringLiteral("0 failed · 1 cancelled")), qPrintable(queue->statusContext()));
        q.holdQueue(false);
        q.clearFinished();
    }

    // "1 day ago", not "1 days ago".
    void relativeTimes()
    {
        const QDateTime now = QDateTime::currentDateTime();
        QCOMPARE(pages::relativeTime(now.addSecs(-30 * 3600)), QStringLiteral("1 day ago"));
        QCOMPARE(pages::relativeTime(now.addSecs(-50 * 3600)), QStringLiteral("2 days ago"));
        QCOMPARE(pages::relativeTime(now.addSecs(-2 * 3600 - 5)), QStringLiteral("2 h ago"));
    }
};

QTEST_MAIN(TstPagesHomeQueue)
#include "tst_pages_home_queue.moc"
