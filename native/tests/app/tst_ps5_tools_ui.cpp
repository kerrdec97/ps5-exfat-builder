// PS5 connect › Find my PS5 (sheet → settings), the Diagnostics tab running
// the real ps5.ftp.diagnose job against the in-process fake FTP server, and
// the Home setup checklist. Offscreen; never a real console.

#include "app/AppContext.h"
#include "app/MainWindow.h"
#include "app/pages/HomePage.h"
#include "app/pages/Ps5ConnectPage.h"
#include "app/pages/ps5/DiagnosticsTab.h"
#include "app/pages/ps5/FindConsole.h"
#include "app/services/AppStores.h"
#include "app/ui/Sheet.h"
#include "app/ui/Theme.h"
#include "ConsoleTestUtil.h"
#include "connect/notify/Module.h"
#include "connect/ps5/FakePs5.h"
#include "connect/ps5/Module.h"
#include "core/Paths.h"
#include "support/TestSupport.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QLabel>
#include <QScopeGuard>
#include <QSettings>
#include <QTabBar>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

using namespace fw;

namespace {

// Reserved for the whole run: nothing listens there and nobody else gets it.
quint16 closedPort() { return fwtest::refusedPort(); }

ui::Sheet* sheetOf(QWidget* w)
{
    for (; w; w = w->parentWidget())
        if (auto* s = qobject_cast<ui::Sheet*>(w))
            return s;
    return nullptr;
}

// The search panel of the Find my PS5 sheet that is open now (earlier sheets
// may still wait for their deferred deletion, hidden).
pages::FindConsolePanel* openPanel(QWidget* window)
{
    for (auto* p : window->findChildren<pages::FindConsolePanel*>())
        if (ui::Sheet* s = sheetOf(p); s && s->isVisible())
            return p;
    return nullptr;
}

QPushButton* buttonIn(QWidget* root, const QString& text)
{
    for (auto* b : root->findChildren<QPushButton*>())
        if (b->text() == text)
            return b;
    return nullptr;
}

} // namespace

class TstPs5ToolsUi : public QObject {
    Q_OBJECT
    QTemporaryDir m_root;
    std::unique_ptr<AppContext> m_ctx;
    std::unique_ptr<MainWindow> m_window;

    pages::Ps5ConnectPage* ps5Page() { return qobject_cast<pages::Ps5ConnectPage*>(m_window->page(QStringLiteral("ps5"))); }
    pages::HomePage* home() { return qobject_cast<pages::HomePage*>(m_window->page(QStringLiteral("home"))); }

    bool waitForDiagnosis(pages::DiagnosticsTab* tab, int ms = 30000)
    {
        return QTest::qWaitFor([tab] { return !tab->isRunning(); }, ms);
    }

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
        (void)m_ctx->settings().setValue(ps5::keys::kConnectTimeout, 3);
        QVERIFY(fwtest::closeConsoleServices(m_ctx->settings()));
        m_window = std::make_unique<MainWindow>(*m_ctx);
        m_window->resize(1440, 920);
        m_window->show();
        QVERIFY(QTest::qWaitForWindowExposed(m_window.get()));
        QVERIFY(ps5Page() && home());
    }

    void cleanupTestCase()
    {
        m_window.reset();
        m_ctx.reset();
    }

    void findMyPs5FillsTheAddress()
    {
        fake::FakeFtpServer::Options o;
        o.root = m_root.filePath(QStringLiteral("find-root"));
        QDir().mkpath(o.root);
        o.greeting = "220 ftpsrv ready";
        fake::FakeFtpServer server(o);
        (void)m_ctx->settings().setValue(ps5::keys::kConsoleIp, QStringLiteral("192.168.1.99"));
        m_window->navigate(QStringLiteral("ps5"));
        auto* find = ps5Page()->findConsoleButton();
        QVERIFY(find);
        QCOMPARE(find->text(), QStringLiteral("Find my PS5"));
        ps5::DiscoveryOptions d;
        d.hosts = {QStringLiteral("127.0.0.1")};
        d.ports = {closedPort(), server.port()};
        find->setDiscoveryOptions(d);

        bool clicked = false;
        QString status;
        QTimer::singleShot(0, this, [&] {
            auto* panel = openPanel(m_window.get());
            ui::Sheet* sheet = sheetOf(panel);
            const bool done = panel && QTest::qWaitFor([panel] { return !panel->isRunning(); }, 15000);
            QPushButton* use = sheet ? buttonIn(sheet, QStringLiteral("Use this console")) : nullptr;
            if (done && panel->results().size() == 1 && use && use->isEnabled()) {
                status = panel->statusText();
                use->click();
                clicked = true;
            } else if (sheet) {
                sheet->close(0); // never leave the test waiting on the sheet
            }
        });
        const auto picked = find->run();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(clicked);
        QVERIFY(picked.has_value());
        QVERIFY2(status.contains(QStringLiteral("1 FTP server found")), qPrintable(status));
        QCOMPARE(picked->host, QStringLiteral("127.0.0.1"));
        QCOMPARE(picked->port, server.port());
        QCOMPARE(picked->banner, QStringLiteral("ftpsrv ready"));
        QCOMPARE(m_ctx->settings().value(ps5::keys::kConsoleIp).toString(), QStringLiteral("127.0.0.1"));
        QCOMPARE(m_ctx->settings().value(ps5::keys::kFtpPort).toInt(), int(server.port()));
        // The header fields follow the settings.
        QCOMPARE(ps5Page()->host(), QStringLiteral("127.0.0.1"));
        QCOMPARE(ps5Page()->ftpPort(), int(server.port()));
    }

    void cancellingTheSearchChangesNothing()
    {
        fake::FakeFtpServer::Options o;
        o.root = m_root.filePath(QStringLiteral("quiet-root"));
        QDir().mkpath(o.root);
        o.silentGreeting = true;
        fake::FakeFtpServer quiet(o);
        (void)m_ctx->settings().setValue(ps5::keys::kConsoleIp, QStringLiteral("192.168.1.98"));
        auto* find = ps5Page()->findConsoleButton();
        ps5::DiscoveryOptions d;
        d.hosts = {QStringLiteral("127.0.0.1")};
        d.ports = {quiet.port()};
        d.bannerWaitMs = 60000; // the search would take a minute
        find->setDiscoveryOptions(d);
        bool running = false;
        QTimer::singleShot(0, this, [&] {
            auto* panel = openPanel(m_window.get());
            QTest::qWait(300);
            running = panel && panel->isRunning();
            if (ui::Sheet* sheet = sheetOf(panel))
                sheet->close(0); // Cancel
        });
        QElapsedTimer t;
        t.start();
        const auto picked = find->run();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); // the sheet and its panel go now
        QVERIFY(running);
        QVERIFY(!picked.has_value());
        QVERIFY(t.elapsed() < 10000);
        QCOMPARE(m_ctx->settings().value(ps5::keys::kConsoleIp).toString(), QStringLiteral("192.168.1.98"));
    }

    // The result list is as tall as its rows need (three at least), not a
    // fixed block whose lower half stays empty.
    void findListIsAsTallAsItsRows()
    {
        auto* sheet = new ui::Sheet(m_window.get(), QStringLiteral("Find my PS5"));
        auto* panel = new pages::FindConsolePanel(sheet);
        sheet->body()->addWidget(panel);
        sheet->addButton(QStringLiteral("Cancel"), ui::Sheet::Role::Safe);
        sheet->open();
        QCOMPARE(panel->resultArea()->height(), int(pages::FindConsolePanel::kAreaMin));
        panel->showSpecimen(); // two consoles
        QCOMPARE(panel->table()->rowCount(), 2);
        QCOMPARE(panel->resultArea()->height(), int(pages::FindConsolePanel::kAreaMin));
        QVERIFY(pages::FindConsolePanel::kAreaMin < pages::FindConsolePanel::kAreaMax);
        QCoreApplication::processEvents();
        // Both rows are in view, with less than two rows of air under them.
        const QRect last = panel->table()->visualRect(panel->table()->model()->index(1, 0));
        QVERIFY(panel->table()->viewport()->rect().contains(last));
        QVERIFY(panel->table()->viewport()->height() - last.bottom() < 2 * theme::size::row);
        sheet->close(0);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }

    void diagnosticsTabRunsTheJob()
    {
        fake::FakeFtpServer::Options o;
        o.root = m_root.filePath(QStringLiteral("diag-root"));
        QDir().mkpath(o.root + QStringLiteral("/data/etaHEN/games"));
        fake::FakeFtpServer server(o);
        (void)m_ctx->settings().setValue(ps5::keys::kConsoleIp, QStringLiteral("127.0.0.1"));
        (void)m_ctx->settings().setValue(ps5::keys::kFtpPort, int(server.port()));
        m_window->navigate(QStringLiteral("ps5"), {{QStringLiteral("tab"), QStringLiteral("diagnostics")}});
        auto* tab = ps5Page()->diagnosticsTab();
        QVERIFY(tab);
        QTRY_VERIFY(tab->isVisible());
        QCOMPARE(tab->verdictText(), QStringLiteral("Not run yet"));
        QCOMPARE(tab->folderEdit()->text(), QStringLiteral("/data/etaHEN/games/")); // no folder browsed: the games folder
        QVERIFY(tab->runButton()->isEnabled());

        QTest::mouseClick(tab->runButton(), Qt::LeftButton);
        QVERIFY(tab->isRunning());
        QVERIFY(!tab->runButton()->isEnabled());
        QVERIFY(waitForDiagnosis(tab));
        QCOMPARE(tab->verdictText(), QStringLiteral("All checks passed"));
        QCOMPARE(tab->cellText(QStringLiteral("reachable")), QStringLiteral("Yes"));
        QCOMPARE(tab->cellTone(QStringLiteral("reachable")), ui::Tone::Ok);
        QCOMPARE(tab->cellText(QStringLiteral("login")), QStringLiteral("Accepted"));
        QCOMPARE(tab->cellText(QStringLiteral("passive")), QStringLiteral("PASV"));
        QCOMPARE(tab->cellText(QStringLiteral("binary")), QStringLiteral("Accepted"));
        QCOMPARE(tab->cellText(QStringLiteral("writable")), QStringLiteral("Yes"));
        QCOMPARE(tab->cellText(QStringLiteral("destination")), QStringLiteral("Internal PS5 storage"));
        QVERIFY(tab->cellText(QStringLiteral("rtt")).endsWith(QStringLiteral("ms")));
        QCOMPARE(tab->bannerText(), QStringLiteral("FakePS5 FTP ready"));
        QCOMPARE(tab->cellText(QStringLiteral("upload")), QStringLiteral("—")); // no speed test yet
        QVERIFY(server.sawCommand(QStringLiteral("DELE /data/etaHEN/games/.exfatbuilder_wtest_")));

        // The speed test: 16 MiB each way, read back identical, removed.
        QTest::mouseClick(tab->speedButton(), Qt::LeftButton);
        QVERIFY(waitForDiagnosis(tab, 60000));
        QCOMPARE(tab->verdictText(), QStringLiteral("All checks passed"));
        QVERIFY2(tab->cellText(QStringLiteral("upload")).endsWith(QStringLiteral("MB/s")), qPrintable(tab->cellText(QStringLiteral("upload"))));
        QVERIFY(tab->cellText(QStringLiteral("download")).endsWith(QStringLiteral("MB/s")));
        QCOMPARE(tab->cellText(QStringLiteral("readback")), QStringLiteral("Identical"));
        QVERIFY(QDir(o.root + QStringLiteral("/data/etaHEN/games")).entryList(QDir::Files | QDir::Hidden).isEmpty());

        // Home: the FTP row is done for this address and port, and remembered.
        const auto list = home()->checklist();
        QCOMPARE(list.at(1).id, QStringLiteral("ftp"));
        QVERIFY2(list.at(1).done, qPrintable(list.at(1).detail));
        QVERIFY(QFileInfo::exists(ui::lastFtpCheckPath()));
        const auto saved = ui::loadLastFtpCheck();
        QVERIFY(saved && saved->passed() && saved->isFor(QStringLiteral("127.0.0.1"), server.port()));
        m_ctx->queue().clearFinished();
    }

    // The checks write into the folder: a system folder is asked about first,
    // and "Cancel" queues nothing.
    void diagnosticsAskBeforeASystemFolder()
    {
        fake::FakeFtpServer::Options o;
        o.root = m_root.filePath(QStringLiteral("diag-system"));
        QDir().mkpath(o.root + QStringLiteral("/system_ex/priv"));
        fake::FakeFtpServer server(o);
        (void)m_ctx->settings().setValue(ps5::keys::kConsoleIp, QStringLiteral("127.0.0.1"));
        (void)m_ctx->settings().setValue(ps5::keys::kFtpPort, int(server.port()));
        m_window->navigate(QStringLiteral("ps5"), {{QStringLiteral("tab"), QStringLiteral("diagnostics")}});
        auto* tab = ps5Page()->diagnosticsTab();
        QVERIFY(tab);
        QTRY_VERIFY(tab->isVisible());
        tab->folderEdit()->setText(QStringLiteral("/system_ex/priv"));

        QString asked;
        const auto answer = [&](const QString& button) {
            auto* poll = new QTimer(this);
            connect(poll, &QTimer::timeout, this, [this, poll, button, &asked] {
                for (auto* s : m_window->findChildren<ui::Sheet*>()) {
                    QPushButton* b = s->isVisible() ? buttonIn(s, button) : nullptr;
                    if (!b)
                        continue;
                    QStringList texts;
                    for (auto* l : s->findChildren<QLabel*>())
                        texts << l->text();
                    asked = texts.join(u'\n');
                    poll->stop();
                    poll->deleteLater();
                    b->click();
                    return;
                }
            });
            poll->start(20);
        };
        const int jobs = int(m_ctx->queue().records().size());
        answer(QStringLiteral("Cancel"));
        tab->run(false); // blocks in the question
        QVERIFY2(asked.contains(QStringLiteral("Test a system folder?")) && asked.contains(QStringLiteral("/system_ex/priv")), qPrintable(asked));
        QVERIFY(!tab->isRunning());
        QCOMPARE(int(m_ctx->queue().records().size()), jobs);

        answer(QStringLiteral("Continue anyway"));
        tab->run(false);
        QVERIFY(tab->isRunning());
        QVERIFY(waitForDiagnosis(tab));
        QCOMPARE(tab->cellText(QStringLiteral("writable")), QStringLiteral("Yes"));
        QVERIFY(server.sawCommand(QStringLiteral("DELE /system_ex/priv/.exfatbuilder_wtest_")));
        tab->folderEdit()->clear();
        m_ctx->queue().clearFinished();
    }

    void diagnosticsShowAnUnreachableConsole()
    {
        const quint16 closed = closedPort();
        (void)m_ctx->settings().setValue(ps5::keys::kConsoleIp, QStringLiteral("127.0.0.1"));
        (void)m_ctx->settings().setValue(ps5::keys::kFtpPort, int(closed));
        auto* tab = ps5Page()->diagnosticsTab();
        tab->run(false);
        QVERIFY(waitForDiagnosis(tab));
        QCOMPARE(tab->verdictText(), QStringLiteral("Not reachable"));
        QCOMPARE(tab->cellText(QStringLiteral("reachable")), QStringLiteral("No"));
        QCOMPARE(tab->cellTone(QStringLiteral("reachable")), ui::Tone::Err);
        QCOMPARE(tab->cellText(QStringLiteral("writable")), QStringLiteral("—"));
        const auto list = home()->checklist();
        QVERIFY(!list.at(1).done);
        QCOMPARE(list.at(1).label, QStringLiteral("FTP not reachable"));
        QCOMPARE(list.at(1).fixText, QStringLiteral("Run diagnostics"));

        // No address: nothing is queued, the tab says why.
        (void)m_ctx->settings().setValue(ps5::keys::kConsoleIp, QString());
        tab->run(false);
        QVERIFY(!tab->isRunning());
        QVERIFY(tab->statusText().contains(QStringLiteral("IP address")));
        m_ctx->queue().clearFinished();
    }

    void homeChecklistCountsTheSteps()
    {
        auto* page = home();
        (void)m_ctx->settings().setValue(ps5::keys::kConsoleIp, QString());
        (void)m_ctx->settings().setValue(ps5::keys::kFtpPort, 2121);
        // The list judges the ports as a user has them (the defaults). Nothing
        // here connects: the console is 192.168.1.20 in name only.
        for (const QString& key : {ps5::keys::kPayloadPort, ps5::keys::kShadowMountPort, ps5::keys::kMicroMountPort,
                                   ps5::keys::kKlogPort, ps5::keys::kDpiPort, ps5::keys::kSmpApiPort})
            m_ctx->settings().reset(key);
        const auto closeAgain = qScopeGuard([this] { (void)fwtest::closeConsoleServices(m_ctx->settings()); });
        auto* store = new ui::PayloadStore(m_window.get());
        page->setPayloadStore(store);
        QCOMPARE(page->checklistProgress(), QStringLiteral("0 / 5 complete"));
        QCOMPARE(page->checklist().size(), 5);

        (void)m_ctx->settings().setValue(ps5::keys::kConsoleIp, QStringLiteral("192.168.1.20"));
        QCOMPARE(page->checklistProgress(), QStringLiteral("3 / 5 complete")); // ip, kernel log, payload port
        QCOMPARE(page->checklist().at(1).label, QStringLiteral("FTP not checked"));

        (void)m_ctx->settings().setValue(ps5::keys::kKlogPort, 2121); // same as FTP: misconfigured
        QCOMPARE(page->checklistProgress(), QStringLiteral("2 / 5 complete"));
        m_ctx->settings().reset(ps5::keys::kKlogPort);
        QCOMPARE(page->checklistProgress(), QStringLiteral("3 / 5 complete"));

        const QString payload = m_root.filePath(QStringLiteral("payload.elf"));
        QVERIFY(fwtest::writeTextFile(payload, "ELF"));
        store->add({QStringLiteral("Test"), payload, {}, {}});
        QCOMPARE(page->checklistProgress(), QStringLiteral("4 / 5 complete"));

        page->setConsoleStatus(QStringLiteral("192.168.1.20"), ui::Tone::Ok); // FTP browser connected
        QCOMPARE(page->checklistProgress(), QStringLiteral("5 / 5 complete"));
        page->setConsoleStatus(QStringLiteral("Not reachable"), ui::Tone::Neutral);

        // A pending row's link opens the place to fix it.
        m_window->navigate(QStringLiteral("home"));
        auto* row = page->findChild<QWidget*>(QStringLiteral("checklist-ftp"));
        QVERIFY(row);
        auto* link = row->findChild<ui::Link*>();
        QVERIFY(link);
        QCOMPARE(link->text(), QStringLiteral("Run diagnostics"));
        emit link->activated();
        QTRY_COMPARE(m_window->currentPage(), QStringLiteral("ps5"));
        QTRY_VERIFY(ps5Page()->diagnosticsTab()->isVisible());
        page->setPayloadStore(nullptr);
    }

    void discordWebhookIsASetting()
    {
        const SettingDef* d = m_ctx->schema().find(notify::keys::kDiscordWebhookUrl);
        QVERIFY(d);
        QCOMPARE(d->section, QStringLiteral("General"));
        QVERIFY(d->advanced);
        QVERIFY(!m_ctx->settings().setValue(notify::keys::kDiscordWebhookUrl, QStringLiteral("https://example.com/x")).ok());
        QVERIFY(m_ctx->settings().setValue(notify::keys::kDiscordWebhookUrl, QStringLiteral("https://discord.com/api/webhooks/1/abc")).ok());
        m_ctx->settings().reset(notify::keys::kDiscordWebhookUrl);
    }
};

QTEST_MAIN(TstPs5ToolsUi)
#include "tst_ps5_tools_ui.moc"
