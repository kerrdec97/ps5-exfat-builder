// Offscreen UI tests: the application shell constructs, every page opens,
// keyboard navigation works, the Settings page round-trips the store, the job
// options panel shows override markers, the Queue page follows a fake job
// kind, the Logs page filters, an unregistered job kind shows the designed
// state, and screenshotAllPages writes one PNG per page.

#include "app/AppContext.h"
#include "app/MainWindow.h"
#include "app/models/LogModel.h"
#include "app/pages/DlcEmuEditor.h"
#include "app/pages/ImageEditorPage.h"
#include "app/pages/LogsPage.h"
#include "app/pages/Ps5ConnectPage.h"
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
#include "connect/ps5/FakePs5.h"
#include "engines/tools/DlcEmu.h"

#include <QDir>
#include <QJsonDocument>
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

    void imageEditorStagesAndOpensRealImage()
    {
        auto* page = qobject_cast<pages::ImageEditorPage*>(m_window->page(QStringLiteral("edit")));
        QVERIFY(page);
        // Staged operations map to the engine's JSON shape (exFAT by default).
        page->clearStaged();
        page->stage({QStringLiteral("add"), QStringLiteral("sce_sys/extra.bin"), QStringLiteral("/tmp/extra.bin"), {}});
        page->stage({QStringLiteral("delete"), QStringLiteral("old/file.txt"), {}, {}});
        page->stage({QStringLiteral("mkdir"), QStringLiteral("new_folder"), {}, {}});
        const QJsonArray ops = page->operationsJson();
        QCOMPARE(ops.size(), 3);
        QCOMPARE(ops[0].toObject().value(QStringLiteral("op")).toString(), QStringLiteral("add"));
        QCOMPARE(ops[0].toObject().value(QStringLiteral("imagePath")).toString(), QStringLiteral("sce_sys/extra.bin"));
        QCOMPARE(ops[0].toObject().value(QStringLiteral("localPath")).toString(), QStringLiteral("/tmp/extra.bin"));
        QCOMPARE(ops[1].toObject().value(QStringLiteral("op")).toString(), QStringLiteral("delete"));
        page->clearStaged();
        QVERIFY(page->staged().empty());
        if (!jobs::isAvailable(*m_ctx, QStringLiteral("image.exfat.build")))
            QSKIP("exFAT engine not registered in this build");
        // Build a small image with the real engine and browse it.
        const QString src = m_root.filePath(QStringLiteral("edit-src"));
        QVERIFY(fwtest::writeTextFile(src + QStringLiteral("/sce_sys/param.json"), "{\"titleId\":\"PPSA00001\"}"));
        QVERIFY(fwtest::writeRandomFile(src + QStringLiteral("/eboot.bin"), 70000, 7));
        const QString out = m_root.filePath(QStringLiteral("edit-src.exfat"));
        auto r = jobs::submitImageBuild(*m_ctx, jobs::kFormatExfat, src, m_root.path(), QStringLiteral("edit-src"), {});
        QVERIFY2(r.ok(), qPrintable(r.ok() ? QString() : r.error().message));
        m_ctx->queue().holdQueue(false);
        QVERIFY(waitForState(m_ctx->queue(), r.value(), JobState::Succeeded, 60000));
        QVERIFY(QFileInfo::exists(out));
        m_window->navigate(QStringLiteral("edit"), {{QStringLiteral("image"), out}});
        QTRY_VERIFY_WITH_TIMEOUT(page->tree()->topLevelItemCount() >= 2, 15000);
        QCOMPARE(page->format(), QStringLiteral("exfat"));
        bool sawEboot = false, sawSceSys = false;
        for (int i = 0; i < page->tree()->topLevelItemCount(); ++i) {
            const QString n = page->tree()->topLevelItem(i)->text(0);
            sawEboot |= n == QLatin1String("eboot.bin");
            sawSceSys |= n == QLatin1String("sce_sys");
        }
        QVERIFY(sawEboot);
        QVERIFY(sawSceSys);
        m_ctx->queue().clearFinished();
    }

    void klogModelIsBoundedAndTagged()
    {
        pages::KlogModel model;
        for (int i = 0; i < 5; ++i) {
            ps5::KlogLine l;
            l.received = QDateTime::currentDateTime();
            l.text = i == 1 ? QStringLiteral("panic: boom") : i == 2 ? QStringLiteral("warn: low") : QStringLiteral("line %1").arg(i);
            l.tag = ps5::klogTag(l.text);
            model.append(l);
        }
        QCOMPARE(model.rowCount(), 5);
        QCOMPARE(model.errors(), 1);
        QCOMPARE(model.warnings(), 1);
        QCOMPARE(model.index(1, pages::KlogModel::TagColumn).data().toString(), QStringLiteral("Error"));
        QVERIFY(model.allText().contains(QStringLiteral("panic: boom")));
        model.clear();
        QCOMPARE(model.rowCount(), 0);
        QCOMPARE(model.errors(), 0);
    }

    void ftpBrowserListsAFakeConsole()
    {
        const QString root = m_root.filePath(QStringLiteral("ftp-root"));
        QVERIFY(fwtest::writeTextFile(root + QStringLiteral("/data/etaHEN/games/readme.txt"), "hi"));
        QVERIFY(fwtest::writeTextFile(root + QStringLiteral("/data/etaHEN/games/PPSA00001.exfat"), "img"));
        QVERIFY(QDir().mkpath(root + QStringLiteral("/data/etaHEN/games/folder")));
        fake::FakeFtpServer::Options o;
        o.root = root;
        fake::FakeFtpServer server(o);
        auto* page = qobject_cast<pages::Ps5ConnectPage*>(m_window->page(QStringLiteral("ps5")));
        QVERIFY(page);
        m_window->navigate(QStringLiteral("ps5"));
        QSignalSpy connected(page->session(), &ui::ConsoleSession::connected);
        page->session()->connectTo(QStringLiteral("127.0.0.1"), server.port(), 5000, true);
        QTRY_VERIFY_WITH_TIMEOUT(connected.count() == 1, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(page->isReachable(), 10000); // the page learns about it through a queued signal
        page->listRemote(QStringLiteral("/data/etaHEN/games"));
        QTRY_VERIFY_WITH_TIMEOUT(page->remoteTable()->rowCount() == 3, 10000);
        QCOMPARE(page->currentRemoteDir(), QStringLiteral("/data/etaHEN/games"));
        QCOMPARE(page->remoteTable()->item(0, 0)->text(), QStringLiteral("folder")); // folders first
        QVERIFY(page->isReachable());
        QSignalSpy disconnected(page->session(), &ui::ConsoleSession::disconnected);
        page->session()->disconnect();
        QTRY_VERIFY_WITH_TIMEOUT(disconnected.count() == 1, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!page->isReachable(), 10000);
    }

    void dlcEmuEditorRoundTrip()
    {
        pages::DlcEmuEditor editor;
        editor.resize(1100, 700);
        const QString app0 = m_root.filePath(QStringLiteral("dlc-app0"));
        QVERIFY(QDir().mkpath(app0 + QStringLiteral("/addcont0")));
        const auto parsed = tools::dlcemu::parse(QStringLiteral("[PSAC]\r\ncontent_id=UP9000-PPSA00001_00-ADDON00000000001\r\nlabel=DLC1\r\nmount_point=/app0/addcont0\r\ndownload_status=INSTALLED\r\nextra_key=kept\r\n"));
        QCOMPARE(int(parsed.size()), 1);
        editor.setEntries(parsed);
        QCOMPARE(editor.table()->rowCount(), 1);
        const auto back = editor.entries();
        QCOMPARE(int(back.size()), 1);
        QCOMPARE(back[0].get(QStringLiteral("content_id")), QStringLiteral("UP9000-PPSA00001_00-ADDON00000000001"));
        QCOMPARE(back[0].get(QStringLiteral("extra_key")), QStringLiteral("kept")); // unknown keys survive the table
        QVERIFY(editor.validationText().startsWith(QStringLiteral("Valid")));
        // An invalid edit shows an error.
        editor.table()->item(0, 1)->setText(QStringLiteral("short"));
        QVERIFY(editor.validationText().contains(QStringLiteral("error")));
    }

    void libraryRenamePreviewUsesTheEngine()
    {
        if (!jobs::isAvailable(*m_ctx, QStringLiteral("library.rename")))
            QSKIP("library.rename not registered in this build");
        const QString lib = m_root.filePath(QStringLiteral("rename-lib"));
        const QString dump = lib + QStringLiteral("/GameA");
        QVERIFY(fwtest::writeTextFile(dump + QStringLiteral("/eboot.bin"), "x"));
        QVERIFY(fwtest::writeTextFile(dump + QStringLiteral("/sce_sys/param.json"),
                                      "{\"titleId\":\"PPSA01234\",\"contentVersion\":\"01.000.000\",\"localizedParameters\":{\"defaultLanguage\":\"en-US\",\"en-US\":{\"titleName\":\"Test Game\"}}}"));
        auto r = jobs::submitLibraryRename(*m_ctx, QStringLiteral("preview"), lib, QStringLiteral("ppsa_title_v"), {}, {}, {});
        QVERIFY2(r.ok(), qPrintable(r.ok() ? QString() : r.error().message));
        m_ctx->queue().holdQueue(false);
        QVERIFY(waitForState(m_ctx->queue(), r.value(), JobState::Succeeded, 30000));
        auto rec = m_ctx->queue().record(r.value());
        QVERIFY(rec && rec->outcome);
        const QJsonArray items = rec->outcome->details.value(QStringLiteral("items")).toArray();
        QCOMPARE(items.size(), 1);
        // The version part comes from the SFO/app version, which a param.json-only dump does not carry.
        QVERIFY(items[0].toObject().value(QStringLiteral("proposedName")).toString().startsWith(QStringLiteral("PPSA01234 Test Game")));
        QVERIFY(QFileInfo(dump).isDir()); // preview renames nothing
        m_ctx->queue().clearFinished();
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
