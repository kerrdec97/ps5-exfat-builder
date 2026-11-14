// "When the queue finishes" (GitHub #96) in the real main window, with
// power::perform() replaced by a hook that only records what was asked:
//
//  * a test program can never put the computer to sleep or shut it down;
//  * the countdown is cancelled with one click on Cancel, with Esc wherever
//    the keyboard focus is, or with Enter (Cancel is the default button);
//    "Sleep now" and a countdown that runs out act once;
//  * a countdown that a second run replaced cannot act after its successor was
//    cancelled, and the countdown stays above a sheet opened after it;
//  * nothing happens for a queue restored at start-up (the waiting job and the
//    one that was running when the app was killed) or for an update check;
//  * closing the window during a countdown cancels it;
//  * work outside the queue (a Backport run or restore) is never cut short by
//    the action, and a window that takes the input from the main window (a
//    Browse… dialog) holds the countdown, which cannot be answered under it;
//  * the Queue page shows the action, changes it for the next run and says
//    while one is armed.
//
// The countdown is found and answered from a timer, so these tests also work
// when the countdown runs a nested event loop.

#include "app/AppContext.h"
#include "app/MainWindow.h"
#include "app/pages/BackportPage.h"
#include "app/pages/QueuePage.h"
#include "app/pages/ps5/ConsoleToolTab.h"
#include "app/ui/Sheet.h"
#include "app/ui/Shell.h"
#include "app/ui/Theme.h"
#include "connect/update/Module.h"
#include "core/Paths.h"
#include "core/Power.h"
#include "engines/general/Module.h"
#include "support/TestSupport.h"

#include <QCloseEvent>
#include <QComboBox>
#include <QDialog>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSemaphore>
#include <QSettings>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <functional>
#include <memory>

using namespace fw;

namespace {

class FakeJob : public Job {
public:
    explicit FakeJob(QJsonObject p) : m_p(std::move(p)) {}
    QString kind() const override { return QStringLiteral("test.fake"); }
    QString title() const override { return m_p.value(QStringLiteral("title")).toString(QStringLiteral("Fake job")); }
    QJsonObject parameters() const override { return m_p; }
    Result<JobOutcome> run(JobContext& ctx) override
    {
        const int ms = m_p.value(QStringLiteral("ms")).toInt(15);
        ctx.progress->beginPhase(0, ms);
        QElapsedTimer clock;
        clock.start();
        while (clock.elapsed() < ms) {
            if (!ctx.token.checkpoint())
                return Error::cancelled();
            QThread::msleep(5);
            ctx.progress->setDone(clock.elapsed());
        }
        JobOutcome o;
        o.summary = QStringLiteral("ok");
        o.verification = Verification::Passed;
        return o;
    }

private:
    QJsonObject m_p;
};

// A local stand-in for GitHub: GET <path> -> a fixed body.
class MiniHttp : public QObject {
public:
    QMap<QString, QByteArray> routes;

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
                    const bool known = routes.contains(path);
                    const QByteArray body = known ? routes.value(path) : QByteArray("not found");
                    s->write("HTTP/1.1 " + QByteArray(known ? "200" : "404") + " X\r\nContent-Length: " + QByteArray::number(body.size())
                             + "\r\nConnection: close\r\n\r\n" + body);
                    s->flush();
                    s->disconnectFromHost();
                });
                connect(s, &QTcpSocket::disconnected, this, [this, s] {
                    m_buf.remove(s);
                    s->deleteLater();
                });
            }
        });
    }
    bool listen() { return m_server.listen(QHostAddress::LocalHost, 0); }
    QString base() const { return QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort()); }

private:
    QTcpServer m_server;
    QHash<QTcpSocket*, QByteArray> m_buf;
};

// GitHub's "latest release" document for a release older than this build.
QByteArray olderRelease()
{
    const QJsonObject o{{QStringLiteral("html_url"), QStringLiteral("https://github.com/kerrdec97/ps5-exfat-builder/releases/tag/v1.0.0")},
                        {QStringLiteral("tag_name"), QStringLiteral("v1.0.0")},
                        {QStringLiteral("name"), QStringLiteral("exFAT Image Builder 1.0.0")},
                        {QStringLiteral("draft"), false},
                        {QStringLiteral("prerelease"), false},
                        {QStringLiteral("assets"), QJsonArray()},
                        {QStringLiteral("body"), QStringLiteral("Old")}};
    return QJsonDocument(o).toJson();
}

bool makeGame(const QString& dir)
{
    return fwtest::writeTextFile(dir + QStringLiteral("/sce_sys/param.json"),
                                 R"({"titleId":"PPSA00096","contentVersion":"01.000.000","localizedParameters":{"defaultLanguage":"en-US","en-US":{"titleName":"Sleep Test"}}})")
           && fwtest::writeRandomFile(dir + QStringLiteral("/eboot.bin"), 4096, 9);
}

QPushButton* buttonIn(QWidget* root, const QString& text)
{
    for (auto* b : root->findChildren<QPushButton*>())
        if (b->text() == text && b->isVisible())
            return b;
    return nullptr;
}

constexpr int kDelay = 10; // general.afterQueueDelaySeconds: the setting's minimum

// A PS5 connect tool (a fakelib switch, a ShadowMount+ push) at work for as
// long as the test holds it: its work waits for release(), or for the tab's end.
class HeldTool : public pages::ConsoleToolTab {
public:
    HeldTool(AppContext& ctx, QWidget* parent)
        : ConsoleToolTab(ctx, [] { return ps5::ConsoleEndpoint{}; }, QStringLiteral("held"), parent)
    {
    }
    bool hold()
    {
        const std::shared_ptr<QSemaphore> gate = m_gate;
        return runLocal<bool>(
            [gate](const CancellationToken& token) -> Result<bool> {
                while (!gate->tryAcquire(1, 20))
                    if (token.isCancelled())
                        return Error::cancelled();
                return true;
            },
            [](const Result<bool>&) {});
    }
    void release() { m_gate->release(); }

private:
    std::shared_ptr<QSemaphore> m_gate = std::make_shared<QSemaphore>();
};

} // namespace

class TstQueueFinish : public QObject {
    Q_OBJECT
    QTemporaryDir m_root;
    std::unique_ptr<AppContext> m_ctx;
    std::unique_ptr<MainWindow> m_window;
    QList<power::Action> m_performed; // what perform() was asked to do
    int m_countdownsSeen = 0;          // by the watchdog below
    bool m_expectCountdown = false;    // false: the watchdog cancels any countdown at once
    QPointer<ui::Sheet> m_lastSeen;
    QTimer m_watchdog;

    // The countdown on screen ("Sleep in 9 s"), or null.
    ui::Sheet* openCountdown() const
    {
        static const QRegularExpression title(QStringLiteral("^Sleep in \\d+ s$"));
        ui::Sheet* found = nullptr;
        for (ui::Sheet* s : m_window->findChildren<ui::Sheet*>())
            if (s->isOpen())
                for (QLabel* l : s->findChildren<QLabel*>())
                    if (title.match(l->text()).hasMatch())
                        found = s;
        return found;
    }

    // Counts each countdown once.
    void noteCountdown(ui::Sheet* s)
    {
        if (s && s != m_lastSeen) {
            m_lastSeen = s;
            ++m_countdownsSeen;
        }
    }

    // Runs `act` once, the first time a countdown is open. From a timer: it
    // also runs while the countdown waits in an event loop of its own.
    void onCountdown(std::function<void(ui::Sheet*)> act)
    {
        auto* t = new QTimer(this);
        t->setInterval(20);
        connect(t, &QTimer::timeout, this, [this, t, act] {
            ui::Sheet* s = openCountdown();
            if (!s)
                return;
            noteCountdown(s);
            t->stop();
            t->deleteLater();
            act(s);
        });
        t->start();
    }

    // A click where the user clicks: on the window, at the button's place,
    // so a widget lying above the button would take it.
    void clickAt(QWidget* w)
    {
        QVERIFY(w);
        const QPoint p = w->mapTo(m_window.get(), w->rect().center());
        QTest::mouseClick(m_window->windowHandle(), Qt::LeftButton, Qt::NoModifier, p);
    }

    QString runFakeJob(const QString& title, int ms = 15)
    {
        auto r = m_ctx->queue().enqueue(QStringLiteral("test.fake"), QJsonObject{{QStringLiteral("title"), title}, {QStringLiteral("ms"), ms}});
        return r.ok() ? r.value() : QString();
    }

    bool waitTerminal(const QString& id, int ms = 15000)
    {
        return QTest::qWaitFor([&] {
            auto r = m_ctx->queue().record(id);
            return r && isTerminal(r->state);
        }, ms);
    }

    pages::QueuePage* queuePage() const { return qobject_cast<pages::QueuePage*>(m_window->page(QStringLiteral("queue"))); }

    // The update check the app makes after start, against a stand-in for
    // GitHub that knows no newer release. True when the check has ended.
    bool checkForUpdates()
    {
        update::setLoopbackAllowedForTests(true);
        const auto restore = qScopeGuard([] { update::setLoopbackAllowedForTests(false); });
        MiniHttp http;
        if (!http.listen())
            return false;
        http.routes[QStringLiteral("/latest")] = olderRelease();
        auto& q = m_ctx->queue();
        QStringList before;
        for (const JobRecord& r : q.records())
            before << r.id;
        m_window->checkForUpdates(false, http.base() + QStringLiteral("/latest"));
        return QTest::qWaitFor([&] {
            for (const JobRecord& r : q.records())
                if (!before.contains(r.id) && r.kind == QLatin1String("update.check") && isTerminal(r.state))
                    return true;
            return false;
        }, 15000);
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_root.isValid());
        fwtest::isolateAppData(m_root);
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_root.filePath(QStringLiteral("qsettings")));
        // Before anything could reach it: perform() only records.
        power::setPerformHook([this](power::Action a) -> Status {
            m_performed << a;
            return success();
        });

        // The session before: "Sleep" chosen, and the app killed (Task
        // Manager) with one job running and one waiting.
        const QString dump = m_root.filePath(QStringLiteral("dump"));
        QVERIFY(makeGame(dump));
        QVERIFY(fwtest::writeTextFile(Paths::settingsFile(),
                                      QJsonDocument(QJsonObject{{QStringLiteral("schemaVersion"), 1},
                                                                {QStringLiteral("values"),
                                                                 QJsonObject{{general::keys::kAfterQueueAction, QStringLiteral("sleep")},
                                                                             {general::keys::kAfterQueueDelay, kDelay}}}})
                                          .toJson()));
        const QJsonObject params{{QStringLiteral("folder"), dump}};
        QVERIFY(fwtest::writeTextFile(Paths::queueFile(),
                                      QJsonDocument(QJsonObject{{QStringLiteral("version"), 1},
                                                                {QStringLiteral("jobs"),
                                                                 QJsonArray{QJsonObject{{QStringLiteral("kind"), QStringLiteral("tools.dump.check")},
                                                                                        {QStringLiteral("title"), QStringLiteral("Was running")},
                                                                                        {QStringLiteral("state"), jobStateName(JobState::Running)},
                                                                                        {QStringLiteral("params"), params}},
                                                                            QJsonObject{{QStringLiteral("kind"), QStringLiteral("tools.dump.check")},
                                                                                        {QStringLiteral("title"), QStringLiteral("Was waiting")},
                                                                                        {QStringLiteral("state"), jobStateName(JobState::Queued)},
                                                                                        {QStringLiteral("params"), params}}}}})
                                          .toJson()));

        theme::apply(qApp);
        m_ctx = std::make_unique<AppContext>();
        QVERIFY(m_ctx->initialize().ok());
        QCOMPARE(m_ctx->settings().value(general::keys::kAfterQueueAction).toString(), QStringLiteral("sleep"));
        m_ctx->mutableJobFactory().add(QStringLiteral("test.fake"), QStringLiteral("Fake"),
                                       [](const QJsonObject& p) -> Result<std::unique_ptr<Job>> { return std::unique_ptr<Job>(new FakeJob(p)); });
        m_window = std::make_unique<MainWindow>(*m_ctx);
        m_window->resize(1440, 920);
        m_window->show();
        QVERIFY(QTest::qWaitForWindowExposed(m_window.get()));
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));

        // Counts every countdown that shows up, and answers one nobody
        // expected with Cancel (a test that fails must not wait for it).
        m_watchdog.setInterval(20);
        connect(&m_watchdog, &QTimer::timeout, this, [this] {
            ui::Sheet* s = openCountdown();
            noteCountdown(s);
            if (s && !m_expectCountdown)
                s->close(0);
        });
        m_watchdog.start();
    }

    void cleanupTestCase()
    {
        m_window.reset();
        m_ctx.reset();
        power::setPerformHook({});
    }

    void init()
    {
        m_performed.clear();
        m_countdownsSeen = 0;
        m_expectCountdown = true;
    }

    // Every test leaves no countdown behind and the queue empty and running.
    void cleanup()
    {
        m_expectCountdown = false;
        if (ui::Sheet* s = openCountdown())
            s->close(0);
        QTRY_COMPARE(ui::Sheet::openCount(), 0);
        auto& q = m_ctx->queue();
        q.cancelAll();
        QTRY_COMPARE(q.runningCount(), 0);
        q.holdQueue(false);
        q.clearFinished();
    }

    // GitHub #96: after a restart (the app was killed during a countdown) the
    // restored queue, the update check at start and taking the restored jobs
    // away must not start a countdown: none of their jobs ran in this session.
    // (First: cleanup() lets the queue run.)
    void aRestoredQueueNeverArmsTheAction()
    {
        m_expectCountdown = false;
        auto& q = m_ctx->queue();
        QVERIFY(q.isHeld()); // restored jobs wait for "Start queue"
        QString waiting, interrupted;
        for (const JobRecord& r : q.records()) {
            if (r.state == JobState::Queued)
                waiting = r.id;
            else if (r.state == JobState::Interrupted)
                interrupted = r.id;
        }
        QVERIFY(!waiting.isEmpty() && !interrupted.isEmpty());

        // The update check a few seconds after start.
        QVERIFY(checkForUpdates());
        QTest::qWait(300);
        QCOMPARE(m_countdownsSeen, 0);

        // The user takes the restored jobs out of the queue: it runs dry.
        QVERIFY(q.remove(waiting));
        QVERIFY(q.remove(interrupted));
        QTest::qWait(300);
        QCOMPARE(m_countdownsSeen, 0);
        QVERIFY(m_performed.isEmpty());
    }

    // The update check after start, with nothing in the queue: no run ended.
    void theUpdateCheckNeverArmsTheAction()
    {
        m_expectCountdown = false;
        QVERIFY(checkForUpdates());
        QTest::qWait(300);
        QCOMPARE(m_countdownsSeen, 0);
        QVERIFY(m_performed.isEmpty());
    }

    // Without the hook, perform() refuses in a test program: nothing a test
    // does can reach Windows.
    void aTestProgramCannotPerformAPowerAction()
    {
        power::setPerformHook({});
        const auto back = qScopeGuard([this] {
            power::setPerformHook([this](power::Action a) -> Status {
                m_performed << a;
                return success();
            });
        });
        QVERIFY(power::performBlocked());
        if (!power::performBlocked())
            QFAIL("perform() would act on this computer"); // never call it then
        const Status st = power::perform(power::Action::Sleep);
        QVERIFY(!st.ok());
        QCOMPARE(st.error().code, ErrorCode::PermissionDenied);
        QVERIFY(power::perform(power::Action::None).ok());
    }

    // One click on Cancel ends the countdown for good.
    void cancelEndsTheCountdownWithOneClick()
    {
        bool clicked = false;
        onCountdown([&](ui::Sheet* s) {
            clickAt(buttonIn(s, QStringLiteral("Cancel")));
            clicked = true;
        });
        const QString id = runFakeJob(QStringLiteral("Click"));
        QVERIFY(!id.isEmpty());
        QVERIFY(waitTerminal(id));
        QTRY_VERIFY(clicked);
        QTRY_VERIFY(!openCountdown());
        QCOMPARE(m_countdownsSeen, 1);
        QTest::qWait(1500);
        QVERIFY(m_performed.isEmpty());
        QCOMPARE(ui::Sheet::openCount(), 0);
    }

    // Esc cancels: with the focus in the countdown, and with it on a field of
    // the page under the countdown. Enter answers Cancel too (the default
    // button is Cancel, not the action).
    void escapeCancelsTheCountdown()
    {
        struct Case {
            Qt::Key key;
            bool focusUnder;
            const char* what;
        };
        for (const Case& c : {Case{Qt::Key_Escape, false, "Esc, focus in the countdown"}, Case{Qt::Key_Escape, true, "Esc, focus under the countdown"},
                              Case{Qt::Key_Return, false, "Enter"}}) {
            bool pressed = false;
            onCountdown([&](ui::Sheet*) {
                if (c.focusUnder) {
                    auto* field = m_window->page(QStringLiteral("settings"))->findChild<QLineEdit*>();
                    QVERIFY(field);
                    m_window->navigate(QStringLiteral("settings"));
                    field->setFocus(Qt::OtherFocusReason);
                }
                QTest::keyClick(m_window->windowHandle(), c.key);
                pressed = true;
            });
            const QString id = runFakeJob(QStringLiteral("Key"));
            QVERIFY(waitTerminal(id));
            QTRY_VERIFY(pressed);
            QTRY_VERIFY2(!openCountdown(), c.what);
            QTest::qWait(500);
            QVERIFY2(m_performed.isEmpty(), c.what);
            m_ctx->queue().clearFinished();
        }
        m_window->navigate(QStringLiteral("home"));
    }

    // "Sleep now" acts at once, and only once.
    void sleepNowActsOnce()
    {
        onCountdown([&](ui::Sheet* s) { clickAt(buttonIn(s, QStringLiteral("Sleep now"))); });
        const QString id = runFakeJob(QStringLiteral("Now"));
        QVERIFY(waitTerminal(id));
        QTRY_COMPARE(m_performed.size(), 1);
        QCOMPARE(m_performed.first(), power::Action::Sleep);
        QTRY_VERIFY(!openCountdown());
        QTest::qWait(500);
        QCOMPARE(m_performed.size(), 1);
    }

    // What happens is decided again when the countdown ends: an action
    // switched off meanwhile does not run.
    void anActionSwitchedOffDuringTheCountdownDoesNotRun()
    {
        onCountdown([&](ui::Sheet* s) {
            QVERIFY(m_ctx->settings().setValue(general::keys::kAfterQueueAction, QStringLiteral("none")).ok());
            clickAt(buttonIn(s, QStringLiteral("Sleep now")));
        });
        const auto back = qScopeGuard([this] { (void)m_ctx->settings().setValue(general::keys::kAfterQueueAction, QStringLiteral("sleep")); });
        const QString id = runFakeJob(QStringLiteral("Switched off"));
        QVERIFY(waitTerminal(id));
        QTRY_COMPARE(m_countdownsSeen, 1);
        QTRY_VERIFY(!openCountdown());
        QTest::qWait(500);
        QVERIFY(m_performed.isEmpty());
    }

    // A second run ends while the first countdown is up: one countdown stays,
    // and when it is cancelled, the replaced one cannot act any more, also
    // after its own time has run out.
    void aReplacedCountdownNeverActs()
    {
        QElapsedTimer since;
        QString second;
        onCountdown([&](ui::Sheet*) {
            since.start();
            second = runFakeJob(QStringLiteral("Second run"), 3000); // ends 3 s into the first countdown
        });
        const QString first = runFakeJob(QStringLiteral("First run"));
        QVERIFY(waitTerminal(first));
        QTRY_VERIFY(!second.isEmpty());
        QVERIFY(waitTerminal(second));
        QTRY_COMPARE(m_countdownsSeen, 2);
        QVERIFY2(since.elapsed() > 1500 && since.elapsed() < kDelay * 1000 - 2000, qPrintable(QString::number(since.elapsed())));
        int open = 0;
        for (ui::Sheet* s : m_window->findChildren<ui::Sheet*>())
            open += s->isOpen() ? 1 : 0;
        QCOMPARE(open, 1);
        // Past the first countdown's end, then Cancel on the one shown.
        QVERIFY(QTest::qWaitFor([&] { return since.elapsed() > (kDelay + 1) * 1000; }, (kDelay + 5) * 1000));
        QVERIFY2(m_performed.isEmpty(), "the replaced countdown acted by itself");
        bool clicked = false;
        onCountdown([&](ui::Sheet* s) {
            clickAt(buttonIn(s, QStringLiteral("Cancel")));
            clicked = true;
        });
        QTRY_VERIFY(clicked);
        QTRY_VERIFY(!openCountdown());
        QTest::qWait(1500);
        QVERIFY2(m_performed.isEmpty(), "Cancel was pressed, yet the computer was put to sleep");
    }

    // A countdown left alone acts once when its time is up.
    void aCountdownThatRunsOutActsOnce()
    {
        const QString id = runFakeJob(QStringLiteral("Runs out"));
        QVERIFY(waitTerminal(id));
        QTRY_VERIFY(openCountdown());
        QTRY_COMPARE_WITH_TIMEOUT(m_performed.size(), 1, (kDelay + 5) * 1000);
        QCOMPARE(m_performed.first(), power::Action::Sleep);
        QTRY_VERIFY(!openCountdown());
        QTest::qWait(1500);
        QCOMPARE(m_performed.size(), 1);
    }

    // A sheet that opens after the countdown (a result, a message) does not
    // cover it: Cancel and Esc still reach the countdown first.
    void theCountdownStaysAboveALaterSheet()
    {
        for (const bool byKey : {false, true}) {
            QPointer<ui::Sheet> later;
            bool answered = false;
            onCountdown([&](ui::Sheet* s) {
                later = ui::Sheet::notify(m_window.get(), QStringLiteral("Another question"), QStringLiteral("Opened after the countdown."));
                QVERIFY(later);
                if (byKey)
                    QTest::keyClick(m_window->windowHandle(), Qt::Key_Escape);
                else
                    clickAt(buttonIn(s, QStringLiteral("Cancel")));
                answered = true;
            });
            const QString id = runFakeJob(QStringLiteral("Covered"));
            QVERIFY(waitTerminal(id));
            QTRY_VERIFY(answered);
            QTRY_VERIFY2(!openCountdown(), byKey ? "Esc" : "click");
            QVERIFY2(later && later->isOpen(), "the later sheet is still there, for its own answer");
            later->close(0);
            QTest::qWait(500);
            QVERIFY(m_performed.isEmpty());
            m_ctx->queue().clearFinished();
        }
    }

    // A Backport run or restore is not a job of the queue: while one is at
    // work the computer is not put to sleep (or shut down) under it, neither
    // when the queue's last job ends nor when a countdown that was already
    // running ends ("Sleep now", or its time running out).
    void backportWorkHoldsBackTheAction()
    {
        auto* backport = qobject_cast<pages::BackportPage*>(m_window->page(QStringLiteral("backport")));
        QVERIFY(backport);
        const auto idle = qScopeGuard([backport] { backport->clearDemoState(); });

        // At work when the queue ends: no countdown, nothing happens.
        m_expectCountdown = false;
        backport->applyDemoState(QStringLiteral("running")); // the run state of a real run, without a worker
        QVERIFY(backport->isBusy());
        QString id = runFakeJob(QStringLiteral("Under a backport run"));
        QVERIFY(waitTerminal(id));
        QTest::qWait(1500);
        QCOMPARE(m_countdownsSeen, 0);
        QVERIFY(m_performed.isEmpty());
        backport->clearDemoState();
        QVERIFY(!backport->isBusy());
        m_ctx->queue().clearFinished();

        // Started during the countdown: "Sleep now", then a countdown left to run out.
        m_expectCountdown = true;
        for (const bool now : {true, false}) {
            bool started = false;
            onCountdown([&](ui::Sheet* s) {
                backport->applyDemoState(QStringLiteral("running"));
                if (now)
                    clickAt(buttonIn(s, QStringLiteral("Sleep now")));
                started = true;
            });
            id = runFakeJob(now ? QStringLiteral("Now under a run") : QStringLiteral("Runs out under a run"));
            QVERIFY(waitTerminal(id));
            QTRY_VERIFY(started);
            QTRY_VERIFY_WITH_TIMEOUT(!openCountdown(), (kDelay + 5) * 1000);
            QTest::qWait(500);
            QVERIFY2(m_performed.isEmpty(), now ? "Sleep now" : "the countdown ran out");
            backport->clearDemoState();
            m_ctx->queue().clearFinished();
        }
    }

    // The same for a PS5 connect tool at work: no countdown when the queue
    // ends while it works, nothing done when it started during the countdown,
    // and once it is idle again the action is back.
    void consoleToolWorkHoldsBackTheAction()
    {
        QWidget* ps5 = m_window->page(QStringLiteral("ps5"));
        QVERIFY(ps5);
        HeldTool tool(*m_ctx, ps5);
        // At work when the queue ends: no countdown, nothing happens.
        m_expectCountdown = false;
        QVERIFY(tool.hold());
        QVERIFY(tool.isBusy());
        QString id = runFakeJob(QStringLiteral("Under a console tool"));
        QVERIFY(waitTerminal(id));
        QTest::qWait(1500);
        QCOMPARE(m_countdownsSeen, 0);
        QVERIFY(m_performed.isEmpty());
        tool.release();
        QTRY_VERIFY(!tool.isBusy());
        m_ctx->queue().clearFinished();

        // Started during the countdown: "Sleep now", then a countdown left to run out.
        m_expectCountdown = true;
        for (const bool now : {true, false}) {
            bool started = false;
            onCountdown([&](ui::Sheet* s) {
                started = tool.hold();
                if (now)
                    clickAt(buttonIn(s, QStringLiteral("Sleep now")));
            });
            id = runFakeJob(now ? QStringLiteral("Now under a console tool") : QStringLiteral("Runs out under a console tool"));
            QVERIFY(waitTerminal(id));
            QTRY_VERIFY(started);
            QVERIFY(tool.isBusy());
            QTRY_VERIFY_WITH_TIMEOUT(!openCountdown(), (kDelay + 5) * 1000);
            QTest::qWait(500);
            QVERIFY2(m_performed.isEmpty(), now ? "Sleep now" : "the countdown ran out");
            tool.release();
            QTRY_VERIFY(!tool.isBusy());
            m_ctx->queue().clearFinished();
        }

        // Idle again: the action is not held back any more.
        onCountdown([&](ui::Sheet* s) { clickAt(buttonIn(s, QStringLiteral("Sleep now"))); });
        id = runFakeJob(QStringLiteral("After the console tool"));
        QVERIFY(waitTerminal(id));
        QTRY_COMPARE(m_performed.size(), 1);
        QCOMPARE(m_performed.first(), power::Action::Sleep);
        QTRY_VERIFY(!openCountdown());
    }

    // A window that takes the input from the main window (a Browse… dialog,
    // a message box) leaves the countdown's Cancel and Esc out of reach: the
    // countdown waits while it is open, and after it has closed there is
    // still time to cancel.
    void aModalWindowHoldsTheCountdown()
    {
        QDialog dialog(m_window.get());
        dialog.setWindowTitle(QStringLiteral("Choose a folder"));
        dialog.setWindowModality(Qt::ApplicationModal);
        dialog.resize(320, 200);
        dialog.show();
        QTRY_COMPARE(QApplication::activeModalWidget(), static_cast<QWidget*>(&dialog));
        const QString id = runFakeJob(QStringLiteral("Under a dialog"));
        QVERIFY(waitTerminal(id));
        QTRY_VERIFY(openCountdown());
        // Past the countdown's whole time with the dialog open.
        QTest::qWait((kDelay + 2) * 1000);
        QVERIFY2(m_performed.isEmpty(), "the computer was put to sleep while the countdown could not be answered");
        QVERIFY(openCountdown());
        dialog.reject();
        QTRY_VERIFY(!QApplication::activeModalWidget());
        // Not at once: time to see it and cancel.
        QTest::qWait(2500);
        QVERIFY(m_performed.isEmpty());
        ui::Sheet* s = openCountdown();
        QVERIFY(s);
        clickAt(buttonIn(s, QStringLiteral("Cancel")));
        QTRY_VERIFY(!openCountdown());
        QTest::qWait(1500);
        QVERIFY(m_performed.isEmpty());
    }

    // The window's close button works during a countdown, and cancels it.
    void closingTheWindowCancelsTheCountdown()
    {
        const QString id = runFakeJob(QStringLiteral("Close"));
        QVERIFY(waitTerminal(id));
        QTRY_VERIFY(openCountdown());
        QCloseEvent close;
        QCoreApplication::sendEvent(m_window.get(), &close);
        QVERIFY(close.isAccepted());
        QTRY_VERIFY(!openCountdown());
        QTest::qWait(500);
        QVERIFY(m_performed.isEmpty());
    }

    // The Queue page shows what happens after the run and changes it for the
    // next one; while an action is armed it says so.
    void theQueuePageShowsAndChangesTheAction()
    {
        m_expectCountdown = false;
        pages::QueuePage* page = queuePage();
        QVERIFY(page);
        m_window->navigate(QStringLiteral("queue"));
        auto* combo = page->findChild<QComboBox*>(QStringLiteral("queue-after-action"));
        QVERIFY2(combo, "the Queue page has a \"When the queue finishes\" control");
        QVERIFY(combo->isVisible());
        QCOMPARE(combo->currentData().toString(), QStringLiteral("sleep"));
        auto* note = page->findChild<QWidget*>(QStringLiteral("queue-after-note"));
        QVERIFY(note);
        QVERIFY(note->isVisible());
        QStringList texts;
        for (QLabel* l : note->findChildren<QLabel*>())
            texts << l->text();
        QVERIFY2(texts.join(u' ').contains(QStringLiteral("sleep")), qPrintable(texts.join(u'|')));
        // The chip in the header says it on every page.
        auto* header = m_window->findChild<ui::AppHeader*>();
        QVERIFY(header);
        QVERIFY2(header->queueChip()->text().contains(QStringLiteral("then sleep")), qPrintable(header->queueChip()->text()));

        // "Do nothing" from the page: the setting follows, the note goes.
        combo->setCurrentIndex(combo->findData(QStringLiteral("none")));
        QCOMPARE(m_ctx->settings().value(general::keys::kAfterQueueAction).toString(), QStringLiteral("none"));
        QVERIFY(!note->isVisible());
        QVERIFY(!header->queueChip()->text().contains(QStringLiteral("then")));
        const QString id = runFakeJob(QStringLiteral("Off"));
        QVERIFY(waitTerminal(id));
        QTest::qWait(300);
        QCOMPARE(m_countdownsSeen, 0);

        // Changed in Settings: the page follows.
        QVERIFY(m_ctx->settings().setValue(general::keys::kAfterQueueAction, QStringLiteral("shutdown")).ok());
        QCOMPARE(combo->currentData().toString(), QStringLiteral("shutdown"));
        QVERIFY(note->isVisible());
        QVERIFY(m_ctx->settings().setValue(general::keys::kAfterQueueAction, QStringLiteral("sleep")).ok());
        m_window->navigate(QStringLiteral("home"));
    }
};

QTEST_MAIN(TstQueueFinish)
#include "tst_queue_finish.moc"
