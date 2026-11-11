// "Add to queue" puts a job on a list instead of starting it (issue 87):
//
//  * on a queue with nothing to do it pauses the queue first, so the job
//    waits; more jobs join it; "Start queue" (toast, Jobs dock, Queue page)
//    runs them in order and the queue ends not paused;
//  * while a job runs, the added job joins the line and the queue is not paused;
//  * the primary button still starts its job at once and leaves a waiting
//    list waiting;
//  * the "queue finished" signal comes once, when a started list has finished;
//  * a restart restores the waiting list, still waiting.

#include "app/AppContext.h"
#include "app/MainWindow.h"
#include "app/pages/BuildImagePage.h"
#include "app/pages/HomePage.h"
#include "app/pages/Page.h"
#include "app/pages/QueuePage.h"
#include "app/services/JobSubmit.h"
#include "app/ui/Components.h"
#include "app/ui/JobRows.h"
#include "app/ui/Sheet.h"
#include "app/ui/Shell.h"
#include "app/ui/Theme.h"
#include "app/ui/Widgets.h"
#include "engines/general/Module.h"
#include "jobs/QueueCompletionWatcher.h"
#include "support/TestSupport.h"

#include <QLabel>
#include <QScopeGuard>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <atomic>

using namespace fw;

namespace {

// A gated fake job stays running until the test opens the gate.
std::atomic<bool> g_gateOpen{true};

class FakeJob : public Job {
public:
    explicit FakeJob(QJsonObject p) : m_p(std::move(p)) {}
    QString kind() const override { return QStringLiteral("test.fake"); }
    QString title() const override { return m_p.value(QStringLiteral("title")).toString(QStringLiteral("Fake job")); }
    QJsonObject parameters() const override { return m_p; }
    Result<JobOutcome> run(JobContext& ctx) override
    {
        ctx.progress->beginPhase(0, 3);
        for (int i = 0; i < 3; ++i) {
            if (!ctx.token.checkpoint())
                return Error::cancelled();
            QThread::msleep(10);
            ctx.progress->advance(1);
        }
        while (m_p.value(QStringLiteral("gated")).toBool() && !g_gateOpen.load()) {
            if (!ctx.token.checkpoint())
                return Error::cancelled();
            QThread::msleep(5);
        }
        JobOutcome o;
        o.summary = QStringLiteral("ok");
        o.verification = Verification::Passed;
        return o;
    }

private:
    QJsonObject m_p;
};

// A form page whose two buttons queue what `make` gives.
class FormPage : public pages::JobFormPage {
public:
    FormPage(AppContext& ctx, JobQueueModel& model)
        : JobFormPage(ctx, model, QStringLiteral("fakeform"), QStringLiteral("Fake form"), QStringLiteral("caption"),
                      QStringLiteral("Run"), QStringLiteral("Fake jobs"))
    {
        setMonitorOnStart(false);
        connect(this, &pages::Page::toastRequested, this, [this](ui::Tone, const QString& title, const QString& body) {
            toastTitle = title;
            toastBody = body;
            toastAction.clear();
            startQueue = nullptr;
        });
        connect(this, &pages::Page::toastActionRequested, this,
                [this](ui::Tone, const QString& title, const QString& body, const QString& action, const std::function<void()>& fn) {
                    toastTitle = title;
                    toastBody = body;
                    toastAction = action;
                    startQueue = fn;
                });
    }
    QString title = QStringLiteral("Job");
    bool gated = false;
    bool fail = false;
    // Set instead of the fake job: what the buttons queue.
    std::function<Result<QString>()> make;
    // The toast the last click asked for.
    QString toastTitle, toastBody, toastAction;
    std::function<void()> startQueue;

protected:
    Result<QString> submit(bool) override
    {
        if (fail)
            return Error(ErrorCode::InvalidArgument, QStringLiteral("Not this time."));
        if (make)
            return make();
        return m_ctx.queue().enqueue(QStringLiteral("test.fake"),
                                     QJsonObject{{QStringLiteral("title"), title}, {QStringLiteral("gated"), gated}});
    }
};

bool waitForState(JobQueue& q, const QString& id, JobState st, int ms = 20000)
{
    return QTest::qWaitFor([&] {
        auto r = q.record(id);
        return r && r->state == st;
    }, ms);
}

QStringList ids(JobQueue& q)
{
    QStringList out;
    for (const auto& r : q.records())
        out << r.id;
    return out;
}

// Clicks `button`; the id of the job that click added ("" when none).
QString clickFor(JobQueue& q, QPushButton* button)
{
    const QStringList before = ids(q);
    button->click();
    for (const QString& id : ids(q))
        if (!before.contains(id))
            return id;
    return {};
}

QStringList labelTexts(QWidget* w)
{
    QStringList out;
    for (const QLabel* l : w->findChildren<QLabel*>())
        if (!l->text().isEmpty())
            out << l->text();
    return out;
}

bool makeGame(const QString& dir)
{
    return fwtest::writeTextFile(dir + QStringLiteral("/sce_sys/param.json"),
                                 R"({"titleId":"PPSA00087","contentVersion":"01.000.000","localizedParameters":{"defaultLanguage":"en-US","en-US":{"titleName":"Queue Test"}}})")
           && fwtest::writeRandomFile(dir + QStringLiteral("/eboot.bin"), 90000, 5);
}

} // namespace

class TstAddToQueue : public QObject {
    Q_OBJECT
    QTemporaryDir m_root;
    std::unique_ptr<AppContext> m_ctx;
    std::unique_ptr<MainWindow> m_window;

    void start()
    {
        m_ctx = std::make_unique<AppContext>();
        QVERIFY(m_ctx->initialize().ok());
        // Small images: the real builds below only have to run.
        QVERIFY(m_ctx->settings().setValue(QStringLiteral("exfat.spareSpace"), QStringLiteral("fixed")).ok());
        QVERIFY(m_ctx->settings().setValue(QStringLiteral("exfat.spareSpaceMiB"), 4).ok());
        QVERIFY(m_ctx->settings().setValue(general::keys::kFreeSpaceMarginMiB, 0).ok());
        m_ctx->mutableJobFactory().add(QStringLiteral("test.fake"), QStringLiteral("Fake"),
                                       [](const QJsonObject& p) -> Result<std::unique_ptr<Job>> { return std::unique_ptr<Job>(new FakeJob(p)); });
        m_window = std::make_unique<MainWindow>(*m_ctx);
        m_window->resize(1440, 920);
        m_window->show();
        QVERIFY(QTest::qWaitForWindowExposed(m_window.get()));
    }

    void stop()
    {
        m_window.reset();
        m_ctx.reset();
    }

    pages::QueuePage* queuePage() { return qobject_cast<pages::QueuePage*>(m_window->page(QStringLiteral("queue"))); }
    QString chip() { return m_window->findChild<ui::AppHeader*>()->queueChip()->text(); }
    QStringList status() { return labelTexts(m_window->findChild<ui::StatusBar*>()); }
    QStringList homeTile()
    {
        auto* home = m_window->page(QStringLiteral("home"));
        home->refresh();
        return labelTexts(home->findChild<QWidget*>(QStringLiteral("home-queue")));
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_root.isValid());
        fwtest::isolateAppData(m_root);
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_root.filePath(QStringLiteral("qsettings")));
        theme::apply(qApp);
        start();
    }

    void cleanupTestCase() { stop(); }

    // Every test leaves the queue empty and not paused.
    void cleanup()
    {
        g_gateOpen = true;
        if (!m_ctx)
            return;
        auto& q = m_ctx->queue();
        q.cancelAll();
        QTRY_COMPARE(q.runningCount(), 0);
        q.holdQueue(false);
        q.clearFinished();
    }

    void theSwitchFollowsTheQueuesState()
    {
        using ui::QueueSwitch;
        QCOMPARE(ui::queueSwitch(false, 0, 0), QueueSwitch::Pause);
        QCOMPARE(ui::queueSwitch(false, 1, 3), QueueSwitch::Pause);
        QCOMPARE(ui::queueSwitch(true, 0, 2), QueueSwitch::Start);
        QCOMPARE(ui::queueSwitch(true, 1, 2), QueueSwitch::Resume); // paused while a job runs
        QCOMPARE(ui::queueSwitch(true, 0, 0), QueueSwitch::Resume);
        QCOMPARE(ui::startQueueText(1), QStringLiteral("Start queue"));
        QCOMPARE(ui::startQueueText(3), QStringLiteral("Start queue (3)"));
    }

    void addToQueueMakesAListThatWaitsForStart()
    {
        auto& q = m_ctx->queue();
        QueueCompletionWatcher watcher(q, m_ctx->settings());
        QSignalSpy finished(&watcher, &QueueCompletionWatcher::queueFinished);
        FormPage page(*m_ctx, m_window->queueModel());
        page.resize(1240, 820);
        page.show();
        QVERIFY(QTest::qWaitForWindowExposed(&page));
        QVERIFY(!q.isHeld());
        QCOMPARE(chip(), QStringLiteral("Queue idle"));
        QVERIFY(!page.dock()->startQueueButton()->isVisibleTo(&page));

        // The first job: the queue is paused, the job waits.
        page.title = QStringLiteral("First");
        const QString a = clickFor(q, page.queueButton());
        QVERIFY(!a.isEmpty());
        QVERIFY(q.isHeld());
        QCOMPARE(q.record(a)->state, JobState::Queued);
        QCOMPARE(page.toastTitle, QStringLiteral("Added to the queue"));
        QCOMPARE(page.toastBody, QStringLiteral("The queue is waiting. Add more, then press Start queue."));
        QCOMPARE(page.toastAction, QStringLiteral("Start queue"));
        QVERIFY(page.startQueue);
        QVERIFY(!page.isMonitorShown());
        QTest::qWait(150); // it really waits
        QCOMPARE(q.record(a)->state, JobState::Queued);
        QTRY_VERIFY(page.dock()->startQueueButton()->isVisibleTo(&page));
        QCOMPARE(page.dock()->startQueueButton()->text(), QStringLiteral("Start queue"));
        QTRY_COMPARE(chip(), QStringLiteral("Queue waiting · 1"));

        // The second one joins it.
        page.title = QStringLiteral("Second");
        const QString b = clickFor(q, page.queueButton());
        QVERIFY(!b.isEmpty());
        QVERIFY(q.isHeld());
        QCOMPARE(q.record(a)->state, JobState::Queued);
        QCOMPARE(q.record(b)->state, JobState::Queued);
        QCOMPARE(page.toastAction, QStringLiteral("Start queue"));

        // Everywhere the queue is shown it says that it waits and how to start it.
        QTRY_COMPARE(page.dock()->startQueueButton()->text(), QStringLiteral("Start queue (2)"));
        QTRY_COMPARE(chip(), QStringLiteral("Queue waiting · 2"));
        QVERIFY2(status().contains(QStringLiteral("2 waiting · press Start queue")), qPrintable(status().join(u'|')));
        QTRY_COMPARE(queuePage()->queueSwitchButton()->text(), QStringLiteral("Start queue (2)"));
        const QStringList tile = homeTile();
        QVERIFY2(tile.contains(QStringLiteral("2 waiting")) && tile.join(u'|').contains(QStringLiteral("press Start queue")),
                 qPrintable(tile.join(u'|')));
        QString notes; // a waiting row says what it waits for (the tooltip has the text in full)
        for (const QLabel* l : page.dock()->list()->findChildren<QLabel*>())
            notes += l->toolTip() + u'|';
        QVERIFY2(notes.contains(QStringLiteral("Waits for Start queue")), qPrintable(notes));
        // Collecting the list is not "the queue finished".
        QCOMPARE(finished.count(), 0);

        // Start queue: both run, in the order they were added, and the queue is not paused afterwards.
        page.dock()->startQueueButton()->click();
        QVERIFY(!q.isHeld());
        QVERIFY(waitForState(q, a, JobState::Succeeded));
        QVERIFY(waitForState(q, b, JobState::Succeeded));
        QVERIFY(q.record(a)->finished <= q.record(b)->started);
        QVERIFY(!q.isHeld());
        QTRY_COMPARE(finished.count(), 1);
        QCOMPARE(finished.first().at(0).toInt(), 2); // succeeded
        QCOMPARE(finished.first().at(1).toInt(), 0);
        QTRY_VERIFY(!page.dock()->startQueueButton()->isVisibleTo(&page));
        QTRY_COMPARE(chip(), QStringLiteral("Queue idle"));
        QTRY_COMPARE(queuePage()->queueSwitchButton()->text(), QStringLiteral("Pause queue"));

        // The next "Add to queue" starts a new list the same way; the toast's button starts it.
        page.title = QStringLiteral("Third");
        const QString c = clickFor(q, page.queueButton());
        QVERIFY(!c.isEmpty());
        QVERIFY(q.isHeld());
        QCOMPARE(q.record(c)->state, JobState::Queued);
        QVERIFY(page.startQueue);
        page.startQueue();
        QVERIFY(!q.isHeld());
        QVERIFY(waitForState(q, c, JobState::Succeeded));
        QTRY_COMPARE(finished.count(), 2);
    }

    void addToQueueWhileAJobRunsJoinsTheLine()
    {
        auto& q = m_ctx->queue();
        FormPage page(*m_ctx, m_window->queueModel());
        page.show();
        g_gateOpen = false;
        page.gated = true;
        page.title = QStringLiteral("Running");
        const QString running = clickFor(q, page.primaryButton());
        QVERIFY(waitForState(q, running, JobState::Running));
        page.gated = false;
        page.title = QStringLiteral("Behind it");
        const QString next = clickFor(q, page.queueButton());
        QVERIFY(!next.isEmpty());
        QVERIFY(!q.isHeld()); // a queue that works is not paused by it
        QCOMPARE(q.record(next)->state, JobState::Queued);
        QCOMPARE(page.toastTitle, QStringLiteral("Added to the queue"));
        QCOMPARE(page.toastBody, QStringLiteral("Behind it"));
        QVERIFY(page.toastAction.isEmpty());
        QVERIFY(!page.dock()->startQueueButton()->isVisibleTo(&page));
        g_gateOpen = true;
        QVERIFY(waitForState(q, running, JobState::Succeeded));
        QVERIFY(waitForState(q, next, JobState::Succeeded)); // without anybody pressing Start
        QVERIFY(!q.isHeld());
    }

    void primaryButtonStartsAtOnceAndLeavesTheListWaiting()
    {
        auto& q = m_ctx->queue();
        QueueCompletionWatcher watcher(q, m_ctx->settings());
        QSignalSpy finished(&watcher, &QueueCompletionWatcher::queueFinished);
        FormPage page(*m_ctx, m_window->queueModel());
        page.resize(1240, 820);
        page.show();
        QVERIFY(QTest::qWaitForWindowExposed(&page));
        page.title = QStringLiteral("Listed 1");
        const QString a = clickFor(q, page.queueButton());
        page.title = QStringLiteral("Listed 2");
        const QString b = clickFor(q, page.queueButton());
        QVERIFY(!a.isEmpty() && !b.isEmpty());
        QVERIFY(q.isHeld());

        // The primary button: that job runs now, ahead of the list.
        g_gateOpen = false;
        page.gated = true;
        page.title = QStringLiteral("Now");
        const QString now = clickFor(q, page.primaryButton());
        page.gated = false;
        QVERIFY(waitForState(q, now, JobState::Running));
        QCOMPARE(page.toastTitle, QStringLiteral("Started"));
        QVERIFY(q.isHeld());
        QCOMPARE(q.record(a)->state, JobState::Queued);
        QCOMPARE(q.record(b)->state, JobState::Queued);
        // While it runs the footer button is away and the switch resumes.
        QTRY_VERIFY(!page.dock()->startQueueButton()->isVisibleTo(&page));
        QTRY_COMPARE(queuePage()->queueSwitchButton()->text(), QStringLiteral("Resume queue"));
        g_gateOpen = true;
        QVERIFY(waitForState(q, now, JobState::Succeeded));
        QTest::qWait(150);
        QCOMPARE(q.record(a)->state, JobState::Queued);
        QCOMPARE(q.record(b)->state, JobState::Queued);
        QVERIFY(q.isHeld());
        QCOMPARE(finished.count(), 0); // the list has not run yet
        QTRY_VERIFY(page.dock()->startQueueButton()->isVisibleTo(&page));
        QTRY_COMPARE(queuePage()->queueSwitchButton()->text(), QStringLiteral("Start queue (2)"));

        // One waiting job started by hand: it runs, the other keeps waiting.
        q.startNow(b);
        QVERIFY(waitForState(q, b, JobState::Succeeded));
        QTest::qWait(100);
        QCOMPARE(q.record(a)->state, JobState::Queued);
        QVERIFY(q.isHeld());
        QCOMPARE(finished.count(), 0);

        // The Queue page's button starts what is left; then the queue has finished, once.
        QCOMPARE(queuePage()->queueSwitchButton()->text(), QStringLiteral("Start queue"));
        queuePage()->queueSwitchButton()->click();
        QVERIFY(waitForState(q, a, JobState::Succeeded));
        QVERIFY(!q.isHeld());
        QTRY_COMPARE(finished.count(), 1);
        QCOMPARE(finished.first().at(0).toInt(), 3);
    }

    void aRefusedJobLeavesTheQueueAsItWas()
    {
        auto& q = m_ctx->queue();
        FormPage page(*m_ctx, m_window->queueModel());
        page.show();
        page.fail = true;
        QVERIFY(clickFor(q, page.queueButton()).isEmpty());
        QVERIFY(!q.isHeld()); // nothing was added: not paused for nothing
        // A queue the user paused stays paused.
        q.holdQueue(true);
        QVERIFY(clickFor(q, page.queueButton()).isEmpty());
        QVERIFY(q.isHeld());
        // A list that waits is not started by a job that was refused.
        page.fail = false;
        q.holdQueue(false);
        const QString a = clickFor(q, page.queueButton());
        QVERIFY(q.isHeld());
        page.fail = true;
        QVERIFY(clickFor(q, page.queueButton()).isEmpty());
        QVERIFY(q.isHeld());
        QCOMPARE(q.record(a)->state, JobState::Queued);
    }

    // The real Build page in the window: the toast has the button, and it starts the queue.
    void buildPageToastStartsTheQueue()
    {
        auto& q = m_ctx->queue();
        auto* build = qobject_cast<pages::BuildImagePage*>(m_window->page(QStringLiteral("build")));
        QVERIFY(build);
        m_window->navigate(QStringLiteral("build"));
        const QString game = m_root.filePath(QStringLiteral("game"));
        QVERIFY(makeGame(game));
        build->setSource(game);
        QVERIFY(QTest::qWaitFor([&] { return build->summary().exists && build->summary().isGame; }, 15000));
        build->setFormat(jobs::kFormatExfat);
        build->setOutputFolder(m_root.filePath(QStringLiteral("out")));
        m_window->toasts()->clear();

        const QString id = clickFor(q, build->queueButton());
        QVERIFY(!id.isEmpty());
        QVERIFY(q.isHeld());
        QCOMPARE(q.record(id)->state, JobState::Queued);
        QVERIFY(!build->isMonitorShown());
        QTRY_VERIFY(build->dock()->startQueueButton()->isVisibleTo(build));
        QVERIFY(m_window->toasts()->count() >= 1);
        ui::Link* startLink = nullptr;
        QStringList toastTexts;
        for (QWidget* t : m_window->findChildren<QWidget*>(QStringLiteral("toast"))) {
            toastTexts << labelTexts(t);
            for (ui::Link* l : t->findChildren<ui::Link*>())
                if (l->text() == QLatin1String("Start queue"))
                    startLink = l;
        }
        QVERIFY2(toastTexts.contains(QStringLiteral("Added to the queue")), qPrintable(toastTexts.join(u'|')));
        QVERIFY2(toastTexts.contains(QStringLiteral("The queue is waiting. Add more, then press Start queue.")),
                 qPrintable(toastTexts.join(u'|')));
        QVERIFY(startLink);
        emit startLink->activated();
        QVERIFY(!q.isHeld());
        QVERIFY(waitForState(q, id, JobState::Succeeded, 120000));
        QVERIFY(!q.isHeld());
    }

    // Last: the app is closed and started again.
    void aWaitingListIsStillWaitingAfterARestart()
    {
        QVERIFY(m_ctx->settings().setValue(general::keys::kRestoreQueue, true).ok());
        const QString game = m_root.filePath(QStringLiteral("restart-game"));
        QVERIFY(makeGame(game));
        {
            auto& q = m_ctx->queue();
            FormPage page(*m_ctx, m_window->queueModel());
            page.show();
            int n = 0;
            page.make = [&] {
                return jobs::submitImageBuild(*m_ctx, jobs::kFormatExfat, game, m_root.filePath(QStringLiteral("restart-out")),
                                              QStringLiteral("restart-%1").arg(++n), {});
            };
            QVERIFY(!clickFor(q, page.queueButton()).isEmpty());
            QVERIFY(!clickFor(q, page.queueButton()).isEmpty());
            QVERIFY(q.isHeld());
            QCOMPARE(q.pendingCount(), 2);
        }
        stop();
        start();
        if (QTest::currentTestFailed())
            return;
        auto& q = m_ctx->queue();
        QCOMPARE(q.pendingCount(), 2);
        QVERIFY(q.isHeld());
        for (const JobRecord& r : q.records())
            QCOMPARE(r.state, JobState::Queued);
        QTRY_COMPARE(chip(), QStringLiteral("Queue waiting · 2"));
        QTRY_COMPARE(queuePage()->queueSwitchButton()->text(), QStringLiteral("Start queue (2)"));
        auto* build = qobject_cast<pages::JobFormPage*>(m_window->page(QStringLiteral("build")));
        QVERIFY(build);
        m_window->navigate(QStringLiteral("build"));
        QTRY_VERIFY(build->dock()->startQueueButton()->isVisibleTo(build));
        QCOMPARE(build->dock()->startQueueButton()->text(), QStringLiteral("Start queue (2)"));
        // Started, the restored list runs and the queue ends not paused.
        const QStringList waiting = ids(q);
        build->dock()->startQueueButton()->click();
        for (const QString& id : waiting)
            QVERIFY2(waitForState(q, id, JobState::Succeeded, 120000), qPrintable(id));
        QVERIFY(!q.isHeld());
    }
};

QTEST_MAIN(TstAddToQueue)
#include "tst_add_to_queue.moc"
