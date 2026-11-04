// Offscreen UI tests: the application shell constructs, every page opens,
// keyboard navigation works, the Settings page round-trips the store, the job
// options panel shows override markers, the Queue page follows a fake job
// kind, the Logs page filters, an unregistered job kind shows the designed
// state, and screenshotAllPages writes one PNG per page.

#include "app/AppContext.h"
#include "app/MainWindow.h"
#include "app/models/LogModel.h"
#include "app/pages/LogsPage.h"
#include "app/pages/Page.h"
#include "app/pages/QueuePage.h"
#include "app/pages/SettingsPage.h"
#include "app/services/JobSubmit.h"
#include "app/ui/JobRows.h"
#include "app/ui/SettingRow.h"
#include "app/ui/Shell.h"
#include "app/ui/Theme.h"
#include "app/ui/Widgets.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "engines/general/Module.h"
#include "support/TestSupport.h"

#include <QDir>
#include <QLineEdit>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace fw;

namespace {

class FakeJob : public Job {
public:
    explicit FakeJob(QJsonObject p) : m_p(std::move(p)) {}
    QString kind() const override { return QStringLiteral("test.fake"); }
    QString title() const override { return m_p.value(QStringLiteral("title")).toString(QStringLiteral("Fake job")); }
    QString subtitle() const override { return QStringLiteral("test · fake"); }
    QStringList phases() const override { return {QStringLiteral("One"), QStringLiteral("Two")}; }
    QJsonObject parameters() const override { return m_p; }
    Result<JobOutcome> run(JobContext& ctx) override
    {
        const int steps = m_p.value(QStringLiteral("steps")).toInt(5);
        const int stepMs = m_p.value(QStringLiteral("stepMs")).toInt(5);
        ctx.progress->beginPhase(0, steps);
        for (int i = 0; i < steps; ++i) {
            if (!ctx.token.checkpoint())
                return Error::cancelled();
            QThread::msleep(stepMs);
            ctx.progress->advance(1);
        }
        if (m_p.value(QStringLiteral("fail")).toBool())
            return Error(ErrorCode::IoError, QStringLiteral("Disk on fire"));
        JobOutcome o;
        o.summary = QStringLiteral("ok");
        o.verification = Verification::Passed;
        return o;
    }

private:
    QJsonObject m_p;
};

// A form page that needs a kind nobody registers.
class NeverPage : public pages::JobFormPage {
public:
    NeverPage(AppContext& ctx, JobQueueModel& model)
        : JobFormPage(ctx, model, QStringLiteral("never"), QStringLiteral("Never"), QStringLiteral("caption"),
                      QStringLiteral("Go"), QStringLiteral("This feature"))
    {
        setRequiredKinds({QStringLiteral("test.never.registered")});
    }

protected:
    Result<QString> submit(bool) override { return jobs::unavailable(QStringLiteral("test.never.registered")); }
};

bool waitForState(JobQueue& q, const QString& id, JobState st, int ms = 10000)
{
    return QTest::qWaitFor([&] {
        auto r = q.record(id);
        return r && r->state == st;
    }, ms);
}

} // namespace

class TstAppUi : public QObject {
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
        m_ctx->mutableJobFactory().add(QStringLiteral("test.fake"), QStringLiteral("Fake"),
                                       [](const QJsonObject& p) -> Result<std::unique_ptr<Job>> { return std::unique_ptr<Job>(new FakeJob(p)); });
        m_window = std::make_unique<MainWindow>(*m_ctx);
        m_window->resize(1440, 920);
        m_window->show();
        QVERIFY(QTest::qWaitForWindowExposed(m_window.get()));
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
    }

    void cleanupTestCase()
    {
        m_window.reset();
        m_ctx.reset();
    }

    void everyPageOpens()
    {
        const QStringList keys = m_window->pageKeys();
        QVERIFY(keys.size() >= 11);
        for (const QString& key : keys) {
            m_window->navigate(key);
            QCOMPARE(m_window->currentPage(), key);
            QVERIFY(m_window->page(key)->isVisible());
        }
        QCOMPARE(m_window->sidebar()->current(), keys.last());
    }

    void keyboardNavigation()
    {
        m_window->navigate(QStringLiteral("home"));
        m_window->sidebar()->focusCurrent();
        auto* home = m_window->sidebar()->item(QStringLiteral("home"));
        QVERIFY(home->hasFocus());
        QTest::keyClick(home, Qt::Key_Down);
        auto* build = m_window->sidebar()->item(QStringLiteral("build"));
        QVERIFY(build->hasFocus());
        QTest::keyClick(build, Qt::Key_Space);
        QCOMPARE(m_window->currentPage(), QStringLiteral("build"));
        QTest::keyClick(m_window.get(), Qt::Key_L, Qt::ControlModifier);
        QCOMPARE(m_window->currentPage(), QStringLiteral("logs"));
        QTest::keyClick(m_window.get(), Qt::Key_1, Qt::ControlModifier);
        QCOMPARE(m_window->currentPage(), QStringLiteral("home"));
    }

    void settingsRoundTrip()
    {
        m_window->navigate(QStringLiteral("settings"));
        auto* page = qobject_cast<pages::SettingsPage*>(m_window->page(QStringLiteral("settings")));
        QVERIFY(page);
        page->showSection(QStringLiteral("Performance"));
        const QString key = general::keys::kMaxConcurrentJobs;
        ui::SettingRow* row = page->row(key);
        QVERIFY(row);
        auto* edit = row->editor()->widget()->findChild<QLineEdit*>();
        QVERIFY(edit);
        edit->setFocus();
        edit->selectAll();
        QTest::keyClicks(edit, QStringLiteral("3"));
        QTest::keyClick(edit, Qt::Key_Return);
        QCOMPARE(m_ctx->settings().value(key).toInt(), 3);
        // Reload from disk: the value was saved.
        {
            SettingsStore again(m_ctx->settings().schemaPtr(), m_ctx->settings().filePath());
            QVERIFY(again.load().ok());
            QCOMPARE(again.value(key).toInt(), 3);
        }
        // Invalid value: message shown, store unchanged.
        edit->selectAll();
        QTest::keyClicks(edit, QStringLiteral("99"));
        QTest::keyClick(edit, Qt::Key_Return);
        QCOMPARE(m_ctx->settings().value(key).toInt(), 3);
        QVERIFY(!row->messageText().isEmpty());
        QVERIFY(row->messageText().contains(QStringLiteral("between")));
        // Reset section restores the default.
        m_ctx->settings().resetSection(QStringLiteral("Performance"));
        QCOMPARE(m_ctx->settings().value(key).toInt(), 1);
        page->showSection(QStringLiteral("Performance"));
        row = page->row(key);
        QVERIFY(row);
        edit = row->editor()->widget()->findChild<QLineEdit*>();
        QCOMPARE(edit->text(), QStringLiteral("1"));
        // Dependent rows are disabled, not hidden.
        page->showSection(QStringLiteral("Verification"));
        QVERIFY(m_ctx->settings().setValue(general::keys::kVerifyAfterBuild, false).ok());
        ui::SettingRow* mode = page->row(general::keys::kVerifyMode);
        QVERIFY(mode);
        QVERIFY(!mode->editor()->widget()->isEnabled());
        QVERIFY(m_ctx->settings().setValue(general::keys::kVerifyAfterBuild, true).ok());
        QVERIFY(mode->editor()->widget()->isEnabled());
    }

    void overrideBadgeInJobOptions()
    {
        ui::JobOptionsPanel panel(*m_ctx, {QStringLiteral("Verification"), QStringLiteral("Performance")});
        panel.build();
        QVERIFY(panel.rowCount() >= 2);
        ui::SettingRow* row = panel.row(general::keys::kVerifyAfterBuild);
        QVERIFY(row);
        QVERIFY(!row->isOverridden());
        auto* sw = qobject_cast<ui::Switch*>(row->editor()->widget());
        QVERIFY(sw);
        sw->click();
        QVERIFY(row->isOverridden());
        QVERIFY(panel.overrides().contains(general::keys::kVerifyAfterBuild));
        QCOMPARE(panel.overrides().value(general::keys::kVerifyAfterBuild).toBool(), false);
        emit row->useDefaultRequested();
        QVERIFY(!row->isOverridden());
        QVERIFY(panel.overrides().isEmpty());
        // Setting the same value as the default is not an override.
        panel.setOverride(general::keys::kVerifyAfterBuild, true);
        QVERIFY(panel.overrides().isEmpty());
    }

    void queuePageReflectsFakeJobs()
    {
        m_window->navigate(QStringLiteral("queue"));
        auto* page = qobject_cast<pages::QueuePage*>(m_window->page(QStringLiteral("queue")));
        QVERIFY(page);
        auto& q = m_ctx->queue();
        q.holdQueue(true);
        auto a = q.enqueue(QStringLiteral("test.fake"), QJsonObject{{QStringLiteral("title"), QStringLiteral("Alpha")}, {QStringLiteral("steps"), 4}});
        auto b = q.enqueue(QStringLiteral("test.fake"), QJsonObject{{QStringLiteral("title"), QStringLiteral("Beta")}, {QStringLiteral("steps"), 400}, {QStringLiteral("stepMs"), 20}});
        QVERIFY(a.ok());
        QVERIFY(b.ok());
        QTRY_COMPARE(page->list()->visibleCount(), 2);
        QCOMPARE(page->list()->visibleIds().first(), a.value());
        // Reorder: Beta first.
        QVERIFY(q.move(b.value(), 0));
        QTRY_COMPARE(page->list()->visibleIds().first(), b.value());
        QVERIFY(q.move(a.value(), 0));
        q.holdQueue(false);
        QVERIFY(waitForState(q, a.value(), JobState::Succeeded));
        QVERIFY(waitForState(q, b.value(), JobState::Running));
        page->selectJob(b.value());
        QCOMPARE(page->list()->selectedId(), b.value());
        q.cancel(b.value());
        QVERIFY(waitForState(q, b.value(), JobState::Cancelled));
        auto r = q.retry(b.value());
        QVERIFY(r.ok());
        QTRY_COMPARE(page->list()->visibleCount(), 3);
        QVERIFY(waitForState(q, r.value(), JobState::Running));
        q.cancel(r.value());
        QVERIFY(waitForState(q, r.value(), JobState::Cancelled));
        q.clearFinished();
        QTRY_COMPARE(page->list()->visibleCount(), 0);
        // System jobs stay out of the visible queue.
        QVERIFY(jobs::isSystemKind(QStringLiteral("update.check")));
        QVERIFY(!jobs::isSystemKind(QStringLiteral("test.fake")));
    }

    void logsFilter()
    {
        m_window->navigate(QStringLiteral("logs"));
        auto* page = qobject_cast<pages::LogsPage*>(m_window->page(QStringLiteral("logs")));
        QVERIFY(page);
        Log log(QStringLiteral("tst"));
        log.error(QStringLiteral("boom-xyz happened"));
        log.info(QStringLiteral("plain info line"));
        QTRY_VERIFY(m_window->logModel().rowCount() >= 2);
        page->setLevelIndex(0);
        page->setSearch(QStringLiteral("boom-xyz"));
        QTRY_COMPARE(page->filterModel()->rowCount(), 1);
        page->setSearch(QStringLiteral("nothing-matches-this"));
        QTRY_COMPARE(page->filterModel()->rowCount(), 0);
        page->setSearch({});
        page->setLevelIndex(3); // errors only
        QTRY_VERIFY(page->filterModel()->rowCount() >= 1);
        for (int r = 0; r < page->filterModel()->rowCount(); ++r)
            QCOMPARE(page->filterModel()->index(r, 0).data(LogModel::LevelRole).toInt(), int(LogLevel::Error));
        page->setLevelIndex(0);
    }

    void unavailableKindShowsDesignedState()
    {
        NeverPage page(*m_ctx, m_window->queueModel());
        page.resize(1200, 800);
        page.show();
        QVERIFY(!page.isAvailable());
        auto* state = page.findChild<ui::UnavailableState*>();
        QVERIFY(state);
        QVERIFY(state->isVisibleTo(&page));
        QVERIFY(!page.primaryButton()->isEnabled());
        QVERIFY(!page.queueButton()->isEnabled());
        auto r = jobs::submitImageBuild(*m_ctx, jobs::kFormatExfat, m_root.path(), m_root.path(), QStringLiteral("x.exfat"), {});
        if (!jobs::isAvailable(*m_ctx, QStringLiteral("image.exfat.build"))) {
            QVERIFY(!r.ok());
            QCOMPARE(r.error().code, ErrorCode::Unsupported);
        }
    }

    void screenshotsCoverEveryPage()
    {
        const QString dir = m_root.filePath(QStringLiteral("shots"));
        const int written = m_window->screenshotAllPages(dir);
        QVERIFY(written >= m_window->pageKeys().size());
        const QStringList files = QDir(dir).entryList({QStringLiteral("*.png")});
        QCOMPARE(files.size(), written);
        for (const QString& key : m_window->pageKeys()) {
            bool found = false;
            for (const QString& f : files)
                if (f.contains(u'-' + key + QStringLiteral(".png")))
                    found = true;
            QVERIFY2(found, qPrintable(key));
        }
    }
};

QTEST_MAIN(TstAppUi)
#include "tst_app_ui.moc"
