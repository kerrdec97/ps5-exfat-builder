// Offscreen UI tests: the application shell constructs, every page opens,
// keyboard navigation works, the Settings page round-trips the store, the job
// options panel shows override markers, the Queue page follows a fake job
// kind, the Logs page filters, an unregistered job kind shows the designed
// state, and screenshotAllPages writes one PNG per page.

#include "app/AppContext.h"
#include "app/MainWindow.h"
#include "app/models/LogModel.h"
#include "app/pages/DlcEmuEditor.h"
#include "app/pages/ImageToolsPage.h"
#include "app/pages/LogsPage.h"
#include "app/pages/Ps5ConnectPage.h"
#include "app/pages/Page.h"
#include "app/pages/QueuePage.h"
#include "app/pages/SettingsPage.h"
#include "app/services/JobSubmit.h"
#include "app/ui/Components.h"
#include "app/ui/JobMonitor.h"
#include "app/ui/JobRows.h"
#include "app/ui/SettingRow.h"
#include "app/ui/Sheet.h"
#include "app/ui/Shell.h"
#include "app/ui/Theme.h"
#include "app/ui/Widgets.h"
#include "core/FileUtil.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "engines/general/Module.h"
#include "support/TestSupport.h"
#include "connect/ps5/FakePs5.h"
#include "connect/update/Module.h"
#include "engines/ampr/Module.h"
#include "engines/tools/DlcEmu.h"

#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QScopeGuard>
#include <QSettings>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
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
    Status validate(const SettingsValues&) const override
    {
        const QString needs = m_p.value(QStringLiteral("requires")).toString();
        if (!needs.isEmpty() && !QFileInfo::exists(needs))
            return Error(ErrorCode::NotFound, QStringLiteral("The source folder is gone."));
        return success();
    }
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

// A form page whose primary action starts a fake job (the monitor flow).
class FakeFormPage : public pages::JobFormPage {
public:
    FakeFormPage(AppContext& ctx, JobQueueModel& model)
        : JobFormPage(ctx, model, QStringLiteral("fakeform"), QStringLiteral("Fake form"), QStringLiteral("caption"),
                      QStringLiteral("Run"), QStringLiteral("Fake jobs"))
    {
        setRequiredKinds({QStringLiteral("test.fake")});
    }
    int steps = 40;

protected:
    Result<QString> submit(bool) override
    {
        return m_ctx.queue().enqueue(QStringLiteral("test.fake"),
                                     QJsonObject{{QStringLiteral("title"), QStringLiteral("Delta")}, {QStringLiteral("steps"), steps}, {QStringLiteral("stepMs"), 25}});
    }
};

bool waitForState(JobQueue& q, const QString& id, JobState st, int ms = 10000)
{
    return QTest::qWaitFor([&] {
        auto r = q.record(id);
        return r && r->state == st;
    }, ms);
}

// A local stand-in for GitHub: GET <path> -> a fixed body.
class MiniHttp : public QObject {
public:
    QMap<QString, QByteArray> routes;
    QStringList requested;

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
                    requested << path;
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

const QString kUpdateProgram = QStringLiteral("exFAT.Image.Builder.exe"); // as GitHub serves "exFAT Image Builder.exe"

QString updateAssetPath(const QString& version, const QString& file)
{
    return QStringLiteral("/kerrdec97/ps5-exfat-builder/releases/download/v%1/%2").arg(version, file);
}

// The "latest release" document of GitHub's API, with or without the files.
QByteArray updateRelease(const QString& base, const QString& version, bool withFiles, qint64 programSize)
{
    QJsonArray assets;
    if (withFiles) {
        assets.append(QJsonObject{{QStringLiteral("name"), kUpdateProgram},
                                  {QStringLiteral("size"), programSize},
                                  {QStringLiteral("browser_download_url"), base + updateAssetPath(version, kUpdateProgram)}});
        assets.append(QJsonObject{{QStringLiteral("name"), QStringLiteral("SHA256SUMS.txt")},
                                  {QStringLiteral("size"), 90},
                                  {QStringLiteral("browser_download_url"), base + updateAssetPath(version, QStringLiteral("SHA256SUMS.txt"))}});
    }
    const QJsonObject o{{QStringLiteral("html_url"), QStringLiteral("https://github.com/kerrdec97/ps5-exfat-builder/releases/tag/v") + version},
                        {QStringLiteral("tag_name"), QStringLiteral("v") + version},
                        {QStringLiteral("name"), QStringLiteral("exFAT Image Builder ") + version},
                        {QStringLiteral("draft"), false},
                        {QStringLiteral("prerelease"), false},
                        {QStringLiteral("assets"), assets},
                        {QStringLiteral("body"), QStringLiteral("Fixes")}};
    return QJsonDocument(o).toJson();
}

QString labelTexts(QWidget* root)
{
    QStringList out;
    for (auto* l : root->findChildren<QLabel*>())
        out << l->text();
    return out.join(u'\n');
}

ui::Link* linkIn(QWidget* root, const QString& text)
{
    for (auto* l : root->findChildren<ui::Link*>())
        if (l->text() == text)
            return l;
    return nullptr;
}

} // namespace

class TstAppUi : public QObject {
    Q_OBJECT
    QTemporaryDir m_root;
    std::unique_ptr<AppContext> m_ctx;
    std::unique_ptr<MainWindow> m_window;
    QList<QUrl> m_openedUrls;

    // The update banner on screen (an earlier one may still wait for its deletion).
    ui::InfoBar* updateBanner() const
    {
        ui::InfoBar* shown = nullptr;
        for (auto* b : m_window->findChildren<ui::InfoBar*>(QStringLiteral("update-banner")))
            if (!b->isHidden())
                shown = b;
        return shown;
    }

public slots:
    // Stands in for the browser (QDesktopServices::setUrlHandler).
    void urlOpened(const QUrl& url) { m_openedUrls << url; }

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
        QVERIFY(keys.size() >= 10); // v5: FPKG is a Build format; Extract and Edit are Image tools actions
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

    // An error in a job option that the user can no longer see or fix does not block the form.
    void optionsPanelDropsErrorsThatCannotBeFixed()
    {
        const QString mode = QStringLiteral("exfat.spareSpace");      // "legacy" / "fixed"
        const QString amount = QStringLiteral("exfat.spareSpaceMiB"); // enabled when the mode is "fixed"
        ui::JobOptionsPanel panel(*m_ctx, {QStringLiteral("Build")});
        panel.build();
        ui::SettingRow* amountRow = panel.row(amount);
        QVERIFY(amountRow && panel.row(mode));
        auto typeInto = [](ui::SettingRow* row, const QString& text) {
            auto* edit = row->editor()->widget()->findChild<QLineEdit*>();
            QVERIFY(edit);
            edit->setText(text);
            emit edit->editingFinished();
        };

        // A page's own note under a row survives a valid edit of that row.
        panel.setOverride(mode, QStringLiteral("fixed"));
        amountRow->setMessage(QStringLiteral("A note from the page"), ui::Tone::Info);
        typeInto(amountRow, QStringLiteral("512"));
        QVERIFY(!panel.hasErrors());
        QCOMPARE(amountRow->messageText(), QStringLiteral("A note from the page"));

        // An invalid entry is an error ...
        typeInto(amountRow, QStringLiteral("abc"));
        QVERIFY(panel.hasErrors());
        // ... until the row is switched off by the setting it depends on: it cannot be corrected then.
        panel.setOverride(mode, QStringLiteral("legacy"));
        QVERIFY2(!panel.hasErrors(), qPrintable(panel.errorMessages().join(u'|')));
        QVERIFY(amountRow->messageText().isEmpty());

        // The same when the global value changes under a row that shows an error.
        panel.setOverride(mode, QStringLiteral("fixed"));
        emit amountRow->useDefaultRequested();
        typeInto(amountRow, QStringLiteral("abc"));
        QVERIFY(panel.hasErrors());
        const QVariant before = m_ctx->settings().value(amount);
        QVERIFY(m_ctx->settings().setValue(amount, before.toInt() + 64).ok());
        QVERIFY(!panel.hasErrors());
        QVERIFY(m_ctx->settings().setValue(amount, before).ok());

        // A value set by the page clears an error as well.
        typeInto(amountRow, QStringLiteral("abc"));
        QVERIFY(panel.hasErrors());
        panel.setOverride(amount, 256);
        QVERIFY(!panel.hasErrors());
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

    void jobMonitorFollowsAJob()
    {
        m_window->navigate(QStringLiteral("queue"));
        auto* page = qobject_cast<pages::QueuePage*>(m_window->page(QStringLiteral("queue")));
        QVERIFY(page);
        auto& q = m_ctx->queue();
        q.holdQueue(true);
        auto a = q.enqueue(QStringLiteral("test.fake"), QJsonObject{{QStringLiteral("title"), QStringLiteral("Gamma")}, {QStringLiteral("steps"), 60}, {QStringLiteral("stepMs"), 20}});
        QVERIFY(a.ok());
        QTRY_COMPARE(page->list()->visibleCount(), 1);
        page->selectJob(a.value());
        ui::JobMonitor* m = page->monitor();
        QCOMPARE(m->jobId(), a.value());
        QVERIFY(m->isVisibleTo(page));
        QCOMPARE(m->stageTexts().size(), 2);
        QVERIFY2(m->stageTexts().first().endsWith(QStringLiteral("pending")), qPrintable(m->stageTexts().first()));
        QCOMPARE(m->stateText(), QStringLiteral("Queued"));
        QVERIFY(m->detailText().startsWith(QStringLiteral("Waiting")));
        QVERIFY(m->cancelButton()->isVisibleTo(m)); // "Remove" for a queued job
        QCOMPARE(m->cancelButton()->text(), QStringLiteral("Remove"));
        QVERIFY(!m->pauseButton()->isVisibleTo(m));

        q.holdQueue(false);
        QVERIFY(waitForState(q, a.value(), JobState::Running));
        QTRY_VERIFY2(m->stateText().startsWith(QStringLiteral("Running")), qPrintable(m->stateText()));
        QTRY_VERIFY2(m->stageTexts().first().endsWith(QStringLiteral("running")), qPrintable(m->stageTexts().first()));
        QTRY_VERIFY(m->console()->lineCount() >= 2); // "Queued:" and "Started:" from the queue
        QVERIFY2(m->console()->plainLines().contains(QStringLiteral("Started: Gamma")), qPrintable(m->console()->plainLines()));
        QVERIFY(m->console()->follow());
        QVERIFY(m->pauseButton()->isVisibleTo(m));
        QVERIFY(m->cancelButton()->isVisibleTo(m));
        QCOMPARE(m->cancelButton()->text(), QStringLiteral("Cancel"));
        QVERIFY(!m->retryButton()->isVisibleTo(m));
        QCOMPARE(m->metricValues().value(4), QStringLiteral("1 of 2")); // stage

        QVERIFY(waitForState(q, a.value(), JobState::Succeeded));
        QTRY_VERIFY2(m->stateText().startsWith(QStringLiteral("Done")), qPrintable(m->stateText()));
        QCOMPARE(m->percentText(), ui::thinPercent(1.0));
        QVERIFY2(m->stageTexts().at(0).endsWith(QStringLiteral("done")), qPrintable(m->stageTexts().at(0)));
        QVERIFY2(m->stageTexts().at(1).endsWith(QStringLiteral("done")), qPrintable(m->stageTexts().at(1)));
        QVERIFY(m->retryButton()->isVisibleTo(m));
        QCOMPARE(m->retryButton()->text(), QStringLiteral("Run again"));
        QVERIFY(!m->pauseButton()->isVisibleTo(m));
        QTRY_VERIFY(m->console()->plainLines().contains(QStringLiteral("Finished: Gamma")));
        QCOMPARE(m->metricValues().value(4), QStringLiteral("2 of 2"));
        QVERIFY(m->metricValues().value(0) != QStringLiteral("—")); // elapsed

        // Run again: the monitor and the list follow the new job.
        m->retryButton()->click();
        QVERIFY(!m->jobId().isEmpty());
        QVERIFY(m->jobId() != a.value());
        QCOMPARE(page->list()->selectedId(), m->jobId());
        QVERIFY(waitForState(q, m->jobId(), JobState::Succeeded));
        q.clearFinished();
        QTRY_COMPARE(page->list()->visibleCount(), 0);
        QTRY_VERIFY(m->jobId().isEmpty()); // the job left the queue
    }

    // A row keeps its widget (focus, an open menu) while other jobs come, go and move.
    void jobRowsSurviveListUpdates()
    {
        auto& q = m_ctx->queue();
        q.holdQueue(true);
        const auto tidy = qScopeGuard([&q] { // also when a check below fails
            q.cancelAll();
            q.holdQueue(false);
            q.clearFinished();
        });
        auto a = q.enqueue(QStringLiteral("test.fake"), QJsonObject{{QStringLiteral("title"), QStringLiteral("Row A")}, {QStringLiteral("steps"), 2}});
        auto b = q.enqueue(QStringLiteral("test.fake"), QJsonObject{{QStringLiteral("title"), QStringLiteral("Row B")}, {QStringLiteral("steps"), 2}});
        auto c = q.enqueue(QStringLiteral("test.fake"), QJsonObject{{QStringLiteral("title"), QStringLiteral("Row C")}, {QStringLiteral("steps"), 2}});
        QVERIFY(a.ok() && b.ok() && c.ok());
        ui::JobList list(*m_ctx, m_window->queueModel(), ui::JobRow::Variant::Queue, m_window.get());
        list.resize(900, 400);
        list.show();
        QTRY_COMPARE(list.visibleIds(), (QStringList{a.value(), b.value(), c.value()}));
        auto rowOf = [&list](const QString& id) {
            QCoreApplication::processEvents(); // new rows are shown, replaced ones are gone
            ui::JobRow* found = nullptr;
            for (ui::JobRow* r : list.findChildren<ui::JobRow*>())
                if (r->id() == id && !r->isHidden())
                    found = r;
            return found;
        };
        QPointer<ui::JobRow> rowB = rowOf(b.value());
        QPointer<ui::JobRow> rowC = rowOf(c.value());
        QVERIFY(rowB && rowC);
        rowC->setFocus();
        QTRY_VERIFY(rowC->hasFocus());

        // Ctrl+Up twice: C moves to the top and keeps the focus all the way.
        QTest::keyClick(rowC, Qt::Key_Up, Qt::ControlModifier);
        QTRY_COMPARE(list.visibleIds(), (QStringList{a.value(), c.value(), b.value()}));
        QVERIFY(rowC && rowC->hasFocus());
        QTest::keyClick(rowC, Qt::Key_Up, Qt::ControlModifier);
        QTRY_COMPARE(list.visibleIds(), (QStringList{c.value(), a.value(), b.value()}));
        QVERIFY(rowC && rowC->hasFocus());
        QCOMPARE(rowOf(b.value()), rowB.data()); // the same widget as before

        // Another job is removed: the others stay.
        q.remove(a.value());
        QTRY_COMPARE(list.visibleIds(), (QStringList{c.value(), b.value()}));
        QVERIFY(rowB && rowC && rowC->hasFocus());

        // While the queue is paused nothing "starts next".
        QString notes; // the note's tooltip has the text in full
        for (const QLabel* l : rowC->findChildren<QLabel*>())
            notes += l->toolTip() + u'|';
        QVERIFY2(notes.contains(QStringLiteral("Waits for the queue to resume")), qPrintable(notes));
        q.remove(b.value());
        q.remove(c.value());
        QTRY_COMPARE(list.visibleCount(), 0);
    }

    // The dock: a cancelling job is shown once, a second running job gets a row.
    void dockShowsEveryActiveJobOnce()
    {
        auto& q = m_ctx->queue();
        const QVariant before = m_ctx->settings().value(QStringLiteral("performance.maxConcurrentJobs"));
        QVERIFY(m_ctx->settings().setValue(QStringLiteral("performance.maxConcurrentJobs"), 2).ok());
        const auto tidy = qScopeGuard([&] { // also when a check below fails
            q.cancelAll();
            (void)m_ctx->settings().setValue(QStringLiteral("performance.maxConcurrentJobs"), before);
        });
        ui::JobsDock dock(*m_ctx, m_window->queueModel(), m_window.get());
        dock.show();
        auto a = q.enqueue(QStringLiteral("test.fake"), QJsonObject{{QStringLiteral("title"), QStringLiteral("Dock A")}, {QStringLiteral("steps"), 400}, {QStringLiteral("stepMs"), 20}});
        auto b = q.enqueue(QStringLiteral("test.fake"), QJsonObject{{QStringLiteral("title"), QStringLiteral("Dock B")}, {QStringLiteral("steps"), 400}, {QStringLiteral("stepMs"), 20}});
        QVERIFY(a.ok() && b.ok());
        QVERIFY(waitForState(q, a.value(), JobState::Running));
        QVERIFY(waitForState(q, b.value(), JobState::Running));
        // One of them in the running block, the other as a row below it.
        QTRY_VERIFY(!dock.runningJob().isEmpty());
        const QString other = dock.runningJob() == a.value() ? b.value() : a.value();
        QTRY_COMPARE(dock.list()->visibleIds(), QStringList{other});

        // Cancelling: still only in the running block.
        const QString shown = dock.runningJob();
        q.cancel(shown);
        QTRY_VERIFY(!dock.list()->visibleIds().contains(shown) || q.record(shown)->isTerminal());
        q.cancel(other);
        QVERIFY(waitForState(q, a.value(), JobState::Cancelled));
        QVERIFY(waitForState(q, b.value(), JobState::Cancelled));
        // Finished jobs are rows again.
        QTRY_COMPARE(dock.list()->visibleCount(), 2);
        q.clearFinished();
    }

    // The monitor: an interrupted job is not "Failed" / "Not started", and an error is said once.
    void monitorTextsMatchTheState()
    {
        ui::JobMonitor m(*m_ctx, m_ctx->logModel(), ui::JobMonitor::Variant::Compact, m_window.get());
        // A new monitor follows nothing: its console is empty, not every job's log.
        QCOMPARE(m.console()->lineCount(), 0);
        QVERIFY(m.console()->plainLines().isEmpty());

        QStringList lines;
        JobRecord r = ui::JobMonitor::demoRecord(QStringLiteral("failed"), &lines);
        m.setDemoRecord(r, lines);
        QVERIFY2(m.percentText().startsWith(QStringLiteral("Failed at")), qPrintable(m.percentText()));
        QCOMPARE(m.detailText(), QStringLiteral("Failed in “Verifying with img_verify”.")); // the message is in the bar below
        int shown = 0;
        for (const QLabel* l : m.findChildren<QLabel*>())
            if (l->text() == r.error->message)
                ++shown;
        QCOMPARE(shown, 1);

        // Restored after the app closed: no start time, an error that says so.
        JobRecord i = r;
        i.state = JobState::Interrupted;
        i.started = {};
        i.error = Error(ErrorCode::IoError, QStringLiteral("The app closed while this job was running."));
        m.setDemoRecord(i, lines);
        QCOMPARE(m.stateText(), QStringLiteral("Interrupted"));
        QCOMPARE(m.percentText(), QStringLiteral("Interrupted"));
        QVERIFY(m.detailText().isEmpty());
        QString all;
        for (const QLabel* l : m.findChildren<QLabel*>())
            all += l->text() + u'|';
        QVERIFY2(!all.contains(QStringLiteral("Not started")), qPrintable(all));

        // A job without phases after one with phases: the stage list is rebuilt.
        JobRecord none = ui::JobMonitor::demoRecord(QStringLiteral("running"), &lines);
        m.setDemoRecord(none, lines);
        QCOMPARE(m.stageTexts().size(), none.phases.size());
        none.id = QStringLiteral("other");
        none.phases.clear();
        m.setDemoRecord(none, lines);
        QCOMPARE(m.stageTexts().size(), 0);

        // Paused before the first progress report: no empty quotes.
        JobRecord p = ui::JobMonitor::demoRecord(QStringLiteral("running"), &lines);
        p.state = JobState::Paused;
        p.progress.phase.clear();
        m.setDemoRecord(p, lines);
        QVERIFY2(!m.detailText().contains(QStringLiteral("“”")), qPrintable(m.detailText()));
    }

    // Retry / Run again on a job whose source is gone: the reason is shown.
    void retryFailureIsReported()
    {
        auto& q = m_ctx->queue();
        const QString source = m_root.filePath(QStringLiteral("retry-source"));
        QVERIFY(QDir().mkpath(source));
        auto a = q.enqueue(QStringLiteral("test.fake"), QJsonObject{{QStringLiteral("title"), QStringLiteral("Epsilon")}, {QStringLiteral("steps"), 2}, {QStringLiteral("requires"), source}});
        QVERIFY(a.ok());
        QVERIFY(waitForState(q, a.value(), JobState::Succeeded));
        QVERIFY(QDir(source).removeRecursively());

        auto visibleSheets = [this] {
            QList<ui::Sheet*> out;
            for (ui::Sheet* s : m_window->findChildren<ui::Sheet*>())
                if (s->isOpen())
                    out << s;
            return out;
        };
        auto sheetShows = [](ui::Sheet* s, const QString& text) {
            for (const QLabel* l : s->findChildren<QLabel*>())
                if (l->text() == text)
                    return true;
            return false;
        };

        // The monitor: nobody listens, so it says it itself, without blocking.
        ui::JobMonitor monitor(*m_ctx, m_ctx->logModel(), ui::JobMonitor::Variant::Compact, m_window.get());
        monitor.setJobId(a.value());
        QCOMPARE(visibleSheets().size(), 0);
        monitor.retryButton()->click();
        QCOMPARE(monitor.jobId(), a.value()); // still on the old job
        QCOMPARE(visibleSheets().size(), 1);
        QVERIFY(sheetShows(visibleSheets().first(), QStringLiteral("The source folder is gone.")));
        visibleSheets().first()->close();
        QCOMPARE(ui::Sheet::openCount(), 0);
        {
            // A page that listens gets the message instead.
            QSignalSpy failed(&monitor, &ui::JobMonitor::retryFailed);
            monitor.retryButton()->click();
            QCOMPARE(failed.count(), 1);
            QCOMPARE(failed.first().at(0).toString(), a.value());
            QCOMPARE(failed.first().at(1).toString(), QStringLiteral("The source folder is gone."));
            QCOMPARE(visibleSheets().size(), 0);
        }

        // The same from a row of a job list ("Run again" in its menu).
        ui::JobList list(*m_ctx, m_window->queueModel(), ui::JobRow::Variant::Queue, m_window.get());
        QTRY_VERIFY(list.visibleIds().contains(a.value()));
        ui::JobRow* row = nullptr;
        for (ui::JobRow* r : list.findChildren<ui::JobRow*>())
            if (r->id() == a.value())
                row = r;
        QVERIFY(row);
        emit row->retryRequested(a.value());
        QCOMPARE(visibleSheets().size(), 1);
        QVERIFY(sheetShows(visibleSheets().first(), QStringLiteral("The source folder is gone.")));
        visibleSheets().first()->close();
        QSignalSpy failed(&list, &ui::JobList::retryFailed);
        emit row->retryRequested(a.value());
        QCOMPARE(failed.count(), 1);
        QCOMPARE(visibleSheets().size(), 0);
        q.clearFinished();
        QTRY_COMPARE(list.visibleCount(), 0);
    }

    void formPageSwitchesToTheMonitor()
    {
        auto& q = m_ctx->queue();
        FakeFormPage page(*m_ctx, m_window->queueModel());
        page.resize(1240, 820);
        page.show();
        QVERIFY(QTest::qWaitForWindowExposed(&page));
        QVERIFY(!page.isMonitorShown());
        QWidget* formWidget = page.form()->parentWidget();
        QVERIFY(formWidget->isVisibleTo(&page));

        // "Add to queue" keeps the form.
        q.holdQueue(true);
        page.queueButton()->click();
        QVERIFY(!page.isMonitorShown());
        QCOMPARE(q.pendingCount(), 1);
        const QString queued = q.records().back().id;

        // The primary button starts the job and the monitor takes the form's place.
        page.primaryButton()->click();
        QVERIFY(page.isMonitorShown());
        const QString started = page.monitor()->jobId();
        QVERIFY(!started.isEmpty());
        QVERIFY(started != queued);
        QVERIFY(!formWidget->isVisibleTo(&page));
        QVERIFY(page.newJobButton()->isVisibleTo(&page));
        QVERIFY(!page.primaryButton()->isVisibleTo(&page));
        QVERIFY(!page.queueButton()->isVisibleTo(&page));
        QCOMPARE(page.dock()->watchedJob(), started);
        QVERIFY(waitForState(q, started, JobState::Running));
        QCOMPARE(q.records().front().id, started); // moved ahead of the queued job
        QTRY_VERIFY(page.monitor()->stateText().startsWith(QStringLiteral("Running")));
        QVERIFY(waitForState(q, started, JobState::Succeeded));
        QTRY_VERIFY(page.monitor()->stateText().startsWith(QStringLiteral("Done")));

        // "Start another" brings the form back and the dock its running block.
        page.monitor()->newJobButton()->click();
        QVERIFY(!page.isMonitorShown());
        QVERIFY(formWidget->isVisibleTo(&page));
        QVERIFY(page.primaryButton()->isVisibleTo(&page));
        QVERIFY(page.queueButton()->isVisibleTo(&page));
        QVERIFY(!page.newJobButton()->isVisibleTo(&page));
        QVERIFY(page.dock()->watchedJob().isEmpty());
        QVERIFY(page.monitor()->jobId().isEmpty());

        // The queue the user paused stays paused for the other waiting job.
        QCOMPARE(q.record(queued)->state, JobState::Queued);
        QVERIFY(q.isHeld());
        q.holdQueue(false);
        // "Watch" on the dock's running block opens that job in the monitor.
        QVERIFY(waitForState(q, queued, JobState::Succeeded));
        auto b = q.enqueue(QStringLiteral("test.fake"), QJsonObject{{QStringLiteral("title"), QStringLiteral("Epsilon")}, {QStringLiteral("steps"), 200}, {QStringLiteral("stepMs"), 20}});
        QVERIFY(b.ok());
        QVERIFY(waitForState(q, b.value(), JobState::Running));
        QTRY_COMPARE(page.dock()->runningJob(), b.value());
        emit page.dock()->watchRequested(b.value());
        QVERIFY(page.isMonitorShown());
        QCOMPARE(page.monitor()->jobId(), b.value());
        QTRY_VERIFY(page.monitor()->pauseButton()->isVisibleTo(page.monitor()));
        q.cancel(b.value());
        QVERIFY(waitForState(q, b.value(), JobState::Cancelled));
        QTRY_COMPARE(page.monitor()->stateText(), QStringLiteral("Cancelled"));
        QVERIFY(page.monitor()->retryButton()->isVisibleTo(page.monitor()));
        QCOMPARE(page.monitor()->retryButton()->text(), QStringLiteral("Retry"));
        // The job leaves the queue: the page returns to the form by itself.
        q.clearFinished();
        QTRY_VERIFY(!page.isMonitorShown());
        QCOMPARE(q.records().size(), size_t(0));
    }

    void monitorDemoStatesRender()
    {
        m_window->navigate(QStringLiteral("build"));
        auto* page = qobject_cast<pages::JobFormPage*>(m_window->page(QStringLiteral("build")));
        QVERIFY(page);
        for (const QString& state : {QStringLiteral("building"), QStringLiteral("built"), QStringLiteral("build-failed")}) {
            page->applyDemoState(state);
            QVERIFY2(page->isMonitorShown(), qPrintable(state));
            QCOMPARE(page->monitor()->stageTexts().size(), 9);
            QVERIFY(page->monitor()->console()->lineCount() >= 10);
            if (state == QLatin1String("building")) {
                QVERIFY(page->monitor()->stateText().startsWith(QStringLiteral("Running")));
                QCOMPARE(page->monitor()->metricValues().value(2), QStringLiteral("418 MB/s"));
                QVERIFY2(page->monitor()->stageTexts().at(4).endsWith(QStringLiteral("running")), qPrintable(page->monitor()->stageTexts().at(4)));
                QVERIFY2(page->monitor()->stageTexts().at(0).contains(QStringLiteral("4 s")), qPrintable(page->monitor()->stageTexts().at(0)));
            } else if (state == QLatin1String("built")) {
                QVERIFY(page->monitor()->stateText().startsWith(QStringLiteral("Done")));
                QVERIFY(page->monitor()->folderButton()->isVisibleTo(page->monitor()));
            } else {
                QCOMPARE(page->monitor()->stateText(), QStringLiteral("Failed"));
                QVERIFY(page->monitor()->percentText().startsWith(QStringLiteral("Failed at")));
                QVERIFY2(page->monitor()->stageTexts().at(7).endsWith(QStringLiteral("failed")), qPrintable(page->monitor()->stageTexts().at(7)));
            }
            page->clearDemoState();
            QVERIFY(!page->isMonitorShown());
        }
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
        auto* page = qobject_cast<pages::ImageToolsPage*>(m_window->page(QStringLiteral("imagetools")));
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
        // Counted on this thread (the session emits from its worker thread;
        // a QSignalSpy would be appended to from there while we read it).
        int connected = 0, disconnected = 0;
        QObject ctx;
        connect(page->session(), &ui::ConsoleSession::connected, &ctx, [&] { ++connected; });
        connect(page->session(), &ui::ConsoleSession::disconnected, &ctx, [&] { ++disconnected; });
        page->session()->connectTo(QStringLiteral("127.0.0.1"), server.port(), 5000, true);
        QTRY_VERIFY_WITH_TIMEOUT(connected == 1, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(page->isReachable(), 10000); // the page learns about it through a queued signal
        page->listRemote(QStringLiteral("/data/etaHEN/games"));
        QTRY_VERIFY_WITH_TIMEOUT(page->remoteTable()->rowCount() == 3, 10000);
        QCOMPARE(page->currentRemoteDir(), QStringLiteral("/data/etaHEN/games"));
        QCOMPARE(page->remoteTable()->item(0, 0)->text(), QStringLiteral("folder")); // folders first
        QVERIFY(page->isReachable());
        // Requests posted while one is running wait for it (the client's
        // nested event loop must not run them inside the listing): a refresh,
        // a free-space query and a disconnect in quick succession.
        page->listRemote(QStringLiteral("/data/etaHEN/games"));
        page->session()->siteAvail();
        page->session()->list(QStringLiteral("/data/etaHEN"));
        page->session()->disconnect();
        QTRY_VERIFY_WITH_TIMEOUT(disconnected >= 1, 10000);
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

    // A Packed folder job is what it is whatever the global "zstd packs"
    // switch says, from every page that queues one.
    void packedFolderJobsCarryTheirOwnSwitch()
    {
        const QString src = m_root.filePath(QStringLiteral("pack-src/PPSA07777"));
        QVERIFY(fwtest::writeTextFile(src + QStringLiteral("/eboot.bin"), QByteArray("\x7F" "ELF", 4) + QByteArray(64, 'E')));
        QVERIFY(fwtest::writeTextFile(src + QStringLiteral("/sce_sys/param.json"), R"({"titleId":"PPSA07777"})"));
        auto& q = m_ctx->queue();
        q.holdQueue(true);
        const auto id = jobs::submitPackFolder(*m_ctx, src, m_root.filePath(QStringLiteral("pack-out/PPSA07777")), {}, {});
        QVERIFY2(id.ok(), qPrintable(id.ok() ? QString() : id.error().message));
        QCOMPARE(q.record(id.value())->overrides.value(ampr::keys::kZstdPack).toBool(), true);
        QVERIFY(q.remove(id.value()));
        q.holdQueue(false);
    }

    // Home: the Free space tile shows a value in a screenshot run too (the
    // worker's answer would come after the picture).
    void homeFreeSpaceTileIsFilledForScreenshots()
    {
        auto* tile = m_window->page(QStringLiteral("home"))->findChild<ui::StatTile*>(QStringLiteral("home-space"));
        QVERIFY(tile);
        qApp->setProperty("fwScreenshotMode", true);
        const auto reset = qScopeGuard([] { qApp->setProperty("fwScreenshotMode", QVariant()); });
        m_window->navigate(QStringLiteral("queue"));
        m_window->navigate(QStringLiteral("home"));
        QStringList texts;
        for (auto* l : tile->findChildren<QLabel*>())
            texts << l->text();
        const QString shown = texts.join(u'|');
        QVERIFY2(shown.contains(QStringLiteral(" on ")) || shown.contains(QStringLiteral("|on ")) || shown.contains(QStringLiteral("not available")), qPrintable(shown));
        QVERIFY2(!shown.contains(QStringLiteral("Checking")), qPrintable(shown));
    }

    // Settings > Storage > Temporary files measures and clears the temporary
    // folder chosen there as well, and in it only the job folders the app made.
    void temporaryFilesCoverTheChosenFolder()
    {
        auto* page = qobject_cast<pages::SettingsPage*>(m_window->page(QStringLiteral("settings")));
        QVERIFY(page);
        const QString custom = m_root.filePath(QStringLiteral("my temp"));
        QString job;
        {
            TempDir t(QStringLiteral("job-J101010-002"), custom);
            QVERIFY(t.isValid());
            t.keep();
            job = t.path();
            QVERIFY(fwtest::writeTextFile(t.filePath(QStringLiteral("zip-source/eboot.bin")), QByteArray(70000, 'x')));
        }
        const QString mine = custom + QStringLiteral("/my notes/keep.txt");
        QVERIFY(fwtest::writeTextFile(mine, "mine"));
        QVERIFY(m_ctx->settings().setValue(general::keys::kTempFolder, custom).ok());
        const auto restore = qScopeGuard([this] { (void)m_ctx->settings().setValue(general::keys::kTempFolder, QString()); });
        m_window->navigate(QStringLiteral("settings"));
        page->showSection(QStringLiteral("Storage"));
        const QString customShown = QDir::toNativeSeparators(custom);
        const auto usage = [&]() -> QString {
            for (auto* l : page->findChildren<QLabel*>())
                if (l->text().contains(customShown) && l->text().contains(QDir::toNativeSeparators(Paths::tempRoot())))
                    return l->text();
            return {};
        };
        QTRY_VERIFY_WITH_TIMEOUT(!usage().isEmpty(), 10000);
        QVERIFY2(!usage().startsWith(QStringLiteral("Nothing")), qPrintable(usage()));
        QPushButton* clear = nullptr;
        for (auto* b : page->findChildren<QPushButton*>())
            if (b->text() == QStringLiteral("Clear"))
                clear = b;
        QVERIFY(clear);
        QTRY_VERIFY(clear->isEnabled());
        QTRY_VERIFY(!m_ctx->queue().isBusy());
        clear->click();
        QVERIFY(!QFileInfo::exists(job));
        QVERIFY(QFile::exists(mine)); // the user's own folder in there is not the app's to remove
        QTRY_VERIFY_WITH_TIMEOUT(usage().startsWith(QStringLiteral("Nothing to clear in")), 10000);
    }

    // A release is the program file itself: the app says "up to date", offers
    // the release page when it cannot check the download, and otherwise
    // downloads, verifies and then only tells where the file is. Nothing is
    // started, nothing closes and the running program is not replaced.
    void updateFlowFollowsWhatTheReleaseOffers()
    {
        update::setLoopbackAllowedForTests(true);
        QDesktopServices::setUrlHandler(QStringLiteral("https"), this, "urlOpened");
        const auto restore = qScopeGuard([] {
            QDesktopServices::unsetUrlHandler(QStringLiteral("https"));
            update::setLoopbackAllowedForTests(false);
        });
        MiniHttp http;
        QVERIFY(http.listen());
        const QString api = http.base() + QStringLiteral("/latest");
        pages::Page* about = m_window->page(QStringLiteral("about"));
        QVERIFY(about);
        auto* status = about->findChild<ui::Badge*>(QStringLiteral("about-update-status"));
        auto* action = about->findChild<QPushButton*>(QStringLiteral("about-update-action"));
        QVERIFY(status && action);
        QVERIFY(action->isHidden());
        m_window->navigate(QStringLiteral("about"));

        // 1. Nothing newer.
        http.routes[QStringLiteral("/latest")] = updateRelease(http.base(), QStringLiteral("1.0.0"), true, 6 * 1024 * 1024);
        m_window->checkForUpdates(false, api);
        QTRY_COMPARE_WITH_TIMEOUT(status->text(), QStringLiteral("Up to date"), 15000);
        QVERIFY(!updateBanner());
        QVERIFY(action->isHidden());

        // 2. Newer, but no file the app can check: the release page is the action.
        http.routes[QStringLiteral("/latest")] = updateRelease(http.base(), QStringLiteral("9.1.0"), false, 0);
        m_window->checkForUpdates(false, api);
        QTRY_COMPARE_WITH_TIMEOUT(status->text(), QStringLiteral("Version 9.1.0 available"), 15000);
        QTRY_VERIFY(updateBanner());
        ui::InfoBar* bar = updateBanner();
        QVERIFY2(labelTexts(bar).contains(QStringLiteral("Version 9.1.0 is available")), qPrintable(labelTexts(bar)));
        QVERIFY(!linkIn(bar, QStringLiteral("Download")));
        ui::Link* open = linkIn(bar, QStringLiteral("Open the release page"));
        QVERIFY(open);
        QCOMPARE(action->text(), QStringLiteral("Open the release page"));
        QVERIFY(!action->isHidden());
        const int jobsBefore = int(m_ctx->queue().records().size());
        emit open->activated();
        QCOMPARE(m_openedUrls.size(), 1);
        QCOMPARE(m_openedUrls.first().toString(), QStringLiteral("https://github.com/kerrdec97/ps5-exfat-builder/releases/tag/v9.1.0"));
        action->click();
        QCOMPARE(m_openedUrls.size(), 2);
        QCOMPARE(int(m_ctx->queue().records().size()), jobsBefore); // no download job for such a release
        m_openedUrls.clear();

        // 3. Newer with the program and its checksum; the first download arrives damaged.
        const QString version = QStringLiteral("9.2.0");
        QByteArray program(6 * 1024 * 1024, 'p');
        program[321] = 'q';
        const QByteArray sums = QCryptographicHash::hash(program, QCryptographicHash::Sha256).toHex() + "  exFAT Image Builder.exe\n";
        http.routes[QStringLiteral("/latest")] = updateRelease(http.base(), version, true, program.size());
        http.routes[updateAssetPath(version, QStringLiteral("SHA256SUMS.txt"))] = sums;
        QByteArray damaged = program;
        damaged[1000] = 'x';
        http.routes[updateAssetPath(version, kUpdateProgram)] = damaged;
        m_window->checkForUpdates(false, api);
        QTRY_COMPARE_WITH_TIMEOUT(status->text(), QStringLiteral("Version 9.2.0 available"), 15000);
        QTRY_VERIFY(updateBanner() && linkIn(updateBanner(), QStringLiteral("Download")));
        bar = updateBanner();
        QVERIFY(!linkIn(bar, QStringLiteral("Open the release page")));
        QVERIFY(linkIn(bar, QStringLiteral("What's new")));
        QCOMPARE(action->text(), QStringLiteral("Download"));
        const QString file = QDir(Paths::cacheDir()).filePath(QStringLiteral("updates/") + kUpdateProgram);
        QFile::remove(file);

        emit linkIn(bar, QStringLiteral("Download"))->activated();
        QTRY_COMPARE_WITH_TIMEOUT(status->text(), QStringLiteral("Downloading version 9.2.0…"), 5000);
        // The failed check of the file shows its error and the state is "available" again.
        QTRY_COMPARE_WITH_TIMEOUT(status->text(), QStringLiteral("Version 9.2.0 available"), 30000);
        QVERIFY(!QFile::exists(file));
        QVERIFY(updateBanner() == bar);
        QVERIFY(linkIn(bar, QStringLiteral("Download")));

        // The good file: downloaded, verified, and left where the banner says.
        http.routes[updateAssetPath(version, kUpdateProgram)] = program;
        action->click(); // the About page's button does the same as the banner's link
        QTRY_COMPARE_WITH_TIMEOUT(status->text(), QStringLiteral("Version 9.2.0 downloaded"), 30000);
        QTRY_VERIFY(updateBanner() && updateBanner() != bar);
        bar = updateBanner();
        const QString text = labelTexts(bar);
        QVERIFY2(text.contains(QStringLiteral("Version 9.2.0 downloaded and verified")), qPrintable(text));
        QVERIFY2(text.contains(QStringLiteral("close the app and replace your exFAT Image Builder.exe")), qPrintable(text));
        QVERIFY(!text.contains(QStringLiteral("install"), Qt::CaseInsensitive));
        QVERIFY(linkIn(bar, QStringLiteral("Show in folder")));
        QVERIFY(!linkIn(bar, QStringLiteral("Download")));
        QCOMPARE(action->text(), QStringLiteral("Show in folder"));
        QFile f(file);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), program);
        // The app is still here and asked for nothing else.
        QVERIFY(m_window->isVisible());
        QVERIFY(m_openedUrls.isEmpty());
        QCOMPARE(http.requested.count(updateAssetPath(version, kUpdateProgram)), 2);

        // Another check finds the same release: the file is not fetched again.
        m_window->checkForUpdates(false, api);
        QTRY_VERIFY_WITH_TIMEOUT(http.requested.count(QStringLiteral("/latest")) == 4, 15000);
        QTRY_COMPARE_WITH_TIMEOUT(status->text(), QStringLiteral("Version 9.2.0 downloaded"), 15000);
        QTRY_VERIFY(updateBanner() && linkIn(updateBanner(), QStringLiteral("Show in folder")));
    }

    void screenshotsCoverEveryPage()
    {
        // FW_UI_SHOT_DIR (an empty or new folder) keeps the pictures for a look at them.
        const QString keep = qEnvironmentVariable("FW_UI_SHOT_DIR");
        const QString dir = keep.isEmpty() ? m_root.filePath(QStringLiteral("shots")) : keep;
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
