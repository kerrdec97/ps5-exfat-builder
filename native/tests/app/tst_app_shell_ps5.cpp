// PS5 connect and the app shell, the parts with a nested event loop or a
// second thread in them (offscreen, against the in-process fake console):
// the browser's menu, rename, delete and disconnect; transfers that ask before
// they are cancelled; tabs whose job leaves the queue; the log model; the
// problem report; the payload list; the kernel-log buffer.

#include "app/AppContext.h"
#include "app/MainWindow.h"
#include "app/models/JobQueueModel.h"
#include "app/models/LogModel.h"
#include "app/pages/Ps5ConnectPage.h"
#include "app/pages/ps5/ConsoleExtras.h"
#include "app/pages/ps5/ConsoleToolTab.h"
#include "app/pages/ps5/DiagnosticsTab.h"
#include "app/pages/ps5/TransferPanel.h"
#include "app/services/AppStores.h"
#include "app/services/ConsoleSession.h"
#include "app/services/JobSubmit.h"
#include "app/ui/Sheet.h"
#include "app/ui/Shell.h"
#include "app/ui/Theme.h"
#include "connect/ps5/FakePs5.h"
#include "connect/ps5/Module.h"
#include "core/Log.h"
#include "support/TestSupport.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QSettings>
#include <QTableWidget>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolButton>

#include <functional>
#include <memory>

using namespace fw;
using fake::FakeFtpServer;

namespace {

bool writeBytes(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(bytes) == bytes.size();
}

quint16 closedPort()
{
    QTcpServer s;
    s.listen(QHostAddress::LocalHost, 0);
    const quint16 p = s.serverPort();
    s.close();
    return p;
}

QPushButton* buttonIn(QWidget* root, const QString& text)
{
    for (auto* b : root->findChildren<QPushButton*>())
        if (b->text() == text)
            return b;
    return nullptr;
}

QToolButton* toolButtonIn(QWidget* root, const QString& accessibleName)
{
    for (auto* b : root->findChildren<QToolButton*>())
        if (b->accessibleName() == accessibleName && b->isVisibleTo(root))
            return b;
    return nullptr;
}

QString labelsOf(QWidget* root)
{
    QStringList out;
    for (auto* l : root->findChildren<QLabel*>())
        out << l->text();
    return out.join(u'\n');
}

ui::Sheet* openSheet(QWidget* host)
{
    for (auto* s : host->findChildren<ui::Sheet*>())
        if (s->isVisible())
            return s;
    return nullptr;
}

// Calls `act` once, as soon as a sheet is open over `host`: the code under
// test blocks in Sheet::exec(), so the answer has to come from a timer. A
// sheet nobody answered is closed after `timeoutMs` (its safe button).
void onSheet(QWidget* host, std::function<void(ui::Sheet*)> act, int timeoutMs = 15000)
{
    auto* timer = new QTimer(host);
    timer->setInterval(20);
    auto clock = std::make_shared<QElapsedTimer>();
    clock->start();
    QObject::connect(timer, &QTimer::timeout, host, [host, timer, clock, act, timeoutMs] {
        if (ui::Sheet* s = openSheet(host)) {
            timer->stop();
            timer->deleteLater();
            act(s);
            if (s->isVisible())
                s->close();
        } else if (clock->elapsed() > timeoutMs) {
            timer->stop();
            timer->deleteLater();
        }
    });
    timer->start();
}

int rowOf(QTableWidget* t, const QString& name)
{
    for (int r = 0; r < t->rowCount(); ++r)
        if (t->item(r, 0) && t->item(r, 0)->text() == name)
            return r;
    return -1;
}

pages::ConsoleToolTab::EndpointFn fakeConsole(quint16 port)
{
    return [port] {
        ps5::ConsoleEndpoint ep;
        ep.host = QStringLiteral("127.0.0.1");
        ep.ftpPort = port;
        ep.connectTimeoutMs = 3000;
        ep.idleTimeoutMs = 5000;
        return ep;
    };
}

} // namespace

class TstAppShellPs5 : public QObject {
    Q_OBJECT
    QTemporaryDir m_root;
    std::unique_ptr<AppContext> m_ctx;
    int m_n = 0;

    QString fresh(const QString& name)
    {
        const QString p = m_root.filePath(QStringLiteral("%1-%2").arg(++m_n).arg(name));
        QDir().mkpath(p);
        return p;
    }

    // The page as the app builds it, connected to `srv` with `folder` open.
    bool connectPage(pages::Ps5ConnectPage& page, FakeFtpServer& srv, const QString& folder, int rows)
    {
        page.resize(1300, 900);
        page.show();
        page.session()->connectTo(QStringLiteral("127.0.0.1"), srv.port(), 5000, true);
        if (!QTest::qWaitFor([&] { return page.isReachable(); }, 10000))
            return false;
        return QTest::qWaitFor([&] { return page.currentRemoteDir() == folder && page.remoteTable()->rowCount() == rows; }, 10000);
    }

    void pointSettingsAt(quint16 ftpPort, const QString& folder)
    {
        (void)m_ctx->settings().setValue(ps5::keys::kConsoleIp, QStringLiteral("127.0.0.1"));
        (void)m_ctx->settings().setValue(ps5::keys::kFtpPort, int(ftpPort));
        (void)m_ctx->settings().setValue(ps5::keys::kGamesFolder, folder);
    }

    // A transfer that stays running until the test ends it (the server is throttled).
    QString startSlowUpload(FakeFtpServer& srv, const QString& localFile, const QString& remoteDir)
    {
        auto r = jobs::submitFtpUploadMany(*m_ctx, QStringLiteral("127.0.0.1"), srv.port(), {localFile}, remoteDir, false);
        if (!r.ok())
            return {};
        m_ctx->queue().startNow(r.value());
        const bool running = QTest::qWaitFor([&] {
            const auto rec = m_ctx->queue().record(r.value());
            return rec && rec->state == JobState::Running;
        }, 15000);
        return running ? r.value() : QString();
    }

    void endJob(const QString& id)
    {
        m_ctx->queue().cancel(id);
        (void)QTest::qWaitFor([&] {
            const auto rec = m_ctx->queue().record(id);
            return !rec || isTerminal(rec->state);
        }, 20000);
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
        m_ctx->queue().holdQueue(false);
        (void)m_ctx->settings().setValue(ps5::keys::kConnectTimeout, 3);
    }

    void cleanupTestCase() { m_ctx.reset(); }

    void cleanup()
    {
        m_ctx->queue().cancelAll();
        (void)QTest::qWaitFor([&] { return !m_ctx->queue().isBusy(); }, 20000);
        m_ctx->queue().clearFinished();
    }

    // The folder that was right-clicked is opened, also when another listing
    // arrived while the menu was up (the row index then means something else).
    void contextMenuOpensTheFolderThatWasClicked()
    {
        const QString root = fresh(QStringLiteral("menu"));
        QVERIFY(QDir().mkpath(root + QStringLiteral("/data/etaHEN/games/Target")));
        FakeFtpServer::Options o;
        o.root = root;
        FakeFtpServer srv(o);
        pointSettingsAt(srv.port(), QStringLiteral("/data/etaHEN/games"));
        ui::PayloadStore store;
        JobQueueModel model(m_ctx->queue());
        pages::Ps5ConnectPage page(*m_ctx, model, store);
        QVERIFY(connectPage(page, srv, QStringLiteral("/data/etaHEN/games"), 1));
        QTableWidget* t = page.remoteTable();
        const QPoint at = t->visualItemRect(t->item(0, 0)).center();

        bool opened = false;
        auto* poll = new QTimer(&page);
        poll->setInterval(30);
        QElapsedTimer clock;
        clock.start();
        connect(poll, &QTimer::timeout, &page, [&] {
            auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            if (!menu) {
                if (clock.elapsed() > 10000)
                    poll->stop();
                return;
            }
            poll->stop();
            // Another folder is listed under the open menu: its row 0 is "games".
            page.listRemote(QStringLiteral("/data/etaHEN"));
            (void)QTest::qWaitFor([&] { return page.currentRemoteDir() == QStringLiteral("/data/etaHEN"); }, 5000);
            for (QAction* a : menu->actions())
                if (a->text() == QStringLiteral("Open")) {
                    a->trigger();
                    opened = true;
                }
            menu->close();
        });
        poll->start();
        emit t->customContextMenuRequested(at); // blocks in the menu until the timer closes it
        QVERIFY(opened);
        QTRY_COMPARE_WITH_TIMEOUT(page.currentRemoteDir(), QStringLiteral("/data/etaHEN/games/Target"), 10000);
        page.session()->disconnect();
        QTRY_VERIFY_WITH_TIMEOUT(!page.isReachable(), 10000);
    }

    // Disconnect is the browser's stop: the deletes that still wait do not run.
    void disconnectDropsTheDeletesThatWait()
    {
        const QString root = fresh(QStringLiteral("drop"));
        const int count = 60;
        for (int i = 0; i < count; ++i)
            QVERIFY(writeBytes(root + QStringLiteral("/del/f%1.bin").arg(i), QByteArray(16, 'x')));
        FakeFtpServer::Options o;
        o.root = root;
        FakeFtpServer srv(o);
        ui::ConsoleSession session;
        session.connectTo(QStringLiteral("127.0.0.1"), srv.port(), 5000, true);
        QTRY_VERIFY_WITH_TIMEOUT(session.isConnected(), 10000);
        for (int i = 0; i < count; ++i)
            session.remove(QStringLiteral("/del/f%1.bin").arg(i), false);
        session.disconnect();
        QTRY_COMPARE_WITH_TIMEOUT(session.state(), ui::ConsoleSession::ConnectionState::Offline, 10000);
        QTest::qWait(300); // nothing is deleted after the disconnect either
        const int left = int(QDir(root + QStringLiteral("/del")).entryList(QDir::Files).size());
        QVERIFY2(left >= count - 10, qPrintable(QStringLiteral("%1 of %2 files left").arg(left).arg(count)));
        // The session works again afterwards.
        session.connectTo(QStringLiteral("127.0.0.1"), srv.port(), 5000, true);
        QTRY_VERIFY_WITH_TIMEOUT(session.isConnected(), 10000);
        session.remove(QStringLiteral("/del/f%1.bin").arg(count - 1), false);
        QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(root + QStringLiteral("/del/f%1.bin").arg(count - 1)), 10000);
    }

    // A connect attempt that is cancelled ends Offline, not "not reachable".
    void cancelledConnectIsNotAFailure()
    {
        FakeFtpServer::Options o;
        o.root = fresh(QStringLiteral("silent"));
        o.silentGreeting = true; // accepts, never greets: the attempt waits
        FakeFtpServer srv(o);
        ui::ConsoleSession session;
        QList<ui::ConsoleSession::ConnectionState> states;
        connect(&session, &ui::ConsoleSession::stateChanged, this, [&](ui::ConsoleSession::ConnectionState s, const QString&) { states << s; });
        session.connectTo(QStringLiteral("127.0.0.1"), srv.port(), 20000, true);
        QTest::qWait(300);
        QVERIFY(session.isConnecting());
        session.disconnect();
        QTRY_COMPARE_WITH_TIMEOUT(session.state(), ui::ConsoleSession::ConnectionState::Offline, 10000);
        QVERIFY(!states.contains(ui::ConsoleSession::ConnectionState::Failed));
    }

    // Rename never lands on a name that is taken: the console would replace that file.
    void renameOntoAnExistingNameIsRefused()
    {
        const QString root = fresh(QStringLiteral("rename"));
        QVERIFY(writeBytes(root + QStringLiteral("/data/etaHEN/games/game.exfat"), QByteArray(64, 'o')));
        QVERIFY(writeBytes(root + QStringLiteral("/data/etaHEN/games/game_new.exfat"), QByteArray(32, 'n')));
        QVERIFY(writeBytes(root + QStringLiteral("/mnt/usb0/Save.bin"), QByteArray(8, 's')));
        QVERIFY(writeBytes(root + QStringLiteral("/mnt/usb0/other.bin"), QByteArray(8, 't')));
        FakeFtpServer::Options o;
        o.root = root;
        FakeFtpServer srv(o);
        pointSettingsAt(srv.port(), QStringLiteral("/data/etaHEN/games"));
        ui::PayloadStore store;
        JobQueueModel model(m_ctx->queue());
        pages::Ps5ConnectPage page(*m_ctx, model, store);
        QVERIFY(connectPage(page, srv, QStringLiteral("/data/etaHEN/games"), 2));
        QStringList toasts;
        connect(&page, &pages::Page::toastRequested, this, [&](ui::Tone, const QString& title, const QString& body) { toasts << title + u'|' + body; });
        QTableWidget* t = page.remoteTable();
        const auto renameTo = [&](const QString& from, const QString& to) {
            t->clearSelection();
            t->selectRow(rowOf(t, from));
            bool answered = false;
            onSheet(&page, [&](ui::Sheet* s) {
                auto* edit = s->findChild<QLineEdit*>();
                QPushButton* go = buttonIn(s, QStringLiteral("Rename"));
                if (edit && go) {
                    edit->setText(to);
                    go->click();
                    answered = true;
                }
            });
            QTest::keyClick(t, Qt::Key_F2); // blocks in the sheet
            return answered;
        };
        QVERIFY(renameTo(QStringLiteral("game_new.exfat"), QStringLiteral("game.exfat")));
        QTest::qWait(300);
        QVERIFY(!srv.sawCommand(QStringLiteral("RNFR")));
        QCOMPARE(QFileInfo(root + QStringLiteral("/data/etaHEN/games/game.exfat")).size(), qint64(64));
        QVERIFY(QFileInfo::exists(root + QStringLiteral("/data/etaHEN/games/game_new.exfat")));
        QVERIFY2(toasts.join(u'\n').contains(QStringLiteral("Not renamed|game.exfat already exists")), qPrintable(toasts.join(u'\n')));

        // exFAT (USB): a name that differs only in case is the same file.
        page.listRemote(QStringLiteral("/mnt/usb0"));
        QTRY_VERIFY_WITH_TIMEOUT(page.currentRemoteDir() == QStringLiteral("/mnt/usb0") && t->rowCount() == 2, 10000);
        toasts.clear();
        QVERIFY(renameTo(QStringLiteral("other.bin"), QStringLiteral("SAVE.BIN")));
        QTest::qWait(300);
        QVERIFY(!srv.sawCommand(QStringLiteral("RNFR")));
        QVERIFY2(toasts.join(u'\n').contains(QStringLiteral("Save.bin already exists")), qPrintable(toasts.join(u'\n')));

        // A free name is renamed.
        QVERIFY(renameTo(QStringLiteral("other.bin"), QStringLiteral("third.bin")));
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(root + QStringLiteral("/mnt/usb0/third.bin")), 10000);
        QVERIFY(srv.sawCommand(QStringLiteral("RNFR")));
        page.session()->disconnect();
        QTRY_VERIFY_WITH_TIMEOUT(!page.isReachable(), 10000);
    }

    // What the delete question named is what is deleted, whatever is selected when it is answered.
    void deleteUsesTheSelectionTheQuestionNamed()
    {
        const QString root = fresh(QStringLiteral("delete"));
        QVERIFY(writeBytes(root + QStringLiteral("/data/etaHEN/games/keep.bin"), QByteArray(8, 'k')));
        QVERIFY(writeBytes(root + QStringLiteral("/data/etaHEN/games/R&D.bin"), QByteArray(8, 'd')));
        FakeFtpServer::Options o;
        o.root = root;
        FakeFtpServer srv(o);
        pointSettingsAt(srv.port(), QStringLiteral("/data/etaHEN/games"));
        ui::PayloadStore store;
        JobQueueModel model(m_ctx->queue());
        pages::Ps5ConnectPage page(*m_ctx, model, store);
        QVERIFY(connectPage(page, srv, QStringLiteral("/data/etaHEN/games"), 2));
        QTableWidget* t = page.remoteTable();
        t->selectRow(rowOf(t, QStringLiteral("R&D.bin")));
        QString asked;
        onSheet(&page, [&](ui::Sheet* s) {
            asked = labelsOf(s);
            // The listing is refreshed under the question: the selection is gone when it is answered.
            page.listRemote(QStringLiteral("/data/etaHEN/games"));
            QTest::qWait(400);
            if (QPushButton* del = buttonIn(s, QStringLiteral("Delete")))
                del->click();
        });
        QTest::keyClick(t, Qt::Key_Delete); // blocks in the sheet
        // The name is shown as it is, not HTML-escaped.
        QVERIFY2(asked.contains(QStringLiteral("Delete R&D.bin from the console?")), qPrintable(asked));
        QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(root + QStringLiteral("/data/etaHEN/games/R&D.bin")), 10000);
        QVERIFY(QFileInfo::exists(root + QStringLiteral("/data/etaHEN/games/keep.bin")));
        page.session()->disconnect();
        QTRY_VERIFY_WITH_TIMEOUT(!page.isReachable(), 10000);
    }

    // Cancel in the transfer list asks first; a tab whose waiting job is taken
    // out of the queue is free again.
    void transfersAskBeforeCancelAndTabsFollowRemovedJobs()
    {
        const QString root = fresh(QStringLiteral("slow"));
        QVERIFY(QDir().mkpath(root + QStringLiteral("/data/etaHEN/games")));
        FakeFtpServer::Options o;
        o.root = root;
        o.throttleBytesPerSec = 32 * 1024;
        FakeFtpServer srv(o);
        pointSettingsAt(srv.port(), QStringLiteral("/data/etaHEN/games"));
        const QString local = fresh(QStringLiteral("slow-up")) + QStringLiteral("/Ratchet & Clank.bin");
        QVERIFY(writeBytes(local, QByteArray(4 * 1024 * 1024, 'r')));
        ui::PayloadStore store;
        JobQueueModel model(m_ctx->queue());
        pages::Ps5ConnectPage page(*m_ctx, model, store);
        page.resize(1300, 900);
        page.show();
        const QString upload = startSlowUpload(srv, local, QStringLiteral("/data/etaHEN/games"));
        QVERIFY(!upload.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(page.transfers()->rowCount(), 1, 5000);
        pages::TransferRow* row = page.transfers()->row(0);
        QVERIFY(row);
        QToolButton* cancel = toolButtonIn(row, QStringLiteral("Cancel"));
        QToolButton* pause = toolButtonIn(row, QStringLiteral("Pause"));
        QVERIFY(cancel && pause);
        QVERIFY(cancel->geometry().left() - pause->geometry().right() >= 10); // apart from Pause

        // "Keep running": nothing happens to the transfer.
        QString asked;
        onSheet(&page, [&](ui::Sheet* s) {
            asked = labelsOf(s);
            if (QPushButton* keep = buttonIn(s, QStringLiteral("Keep running")))
                keep->click();
        });
        cancel->click(); // blocks in the sheet
        QVERIFY2(asked.contains(QStringLiteral("Cancel the running job?")), qPrintable(asked));
        QVERIFY2(asked.contains(QStringLiteral("Ratchet & Clank")), qPrintable(asked)); // not "&amp;"
        QCOMPARE(m_ctx->queue().record(upload)->state, JobState::Running);

        // Diagnostics and Crash dumps wait behind the transfer; removed from the queue, they are idle again.
        pages::DiagnosticsTab* diag = page.diagnosticsTab();
        diag->run(false);
        QVERIFY(diag->isRunning());
        QCOMPARE(m_ctx->queue().record(diag->jobId())->state, JobState::Queued);
        QVERIFY(m_ctx->queue().remove(diag->jobId()));
        QTRY_VERIFY_WITH_TIMEOUT(!diag->isRunning(), 5000);
        QVERIFY(diag->runButton()->isEnabled());
        QVERIFY2(diag->statusText().contains(QStringLiteral("Removed from the queue")), qPrintable(diag->statusText()));

        pages::CrashDumpsTab crash(*m_ctx, fakeConsole(srv.port()));
        crash.start();
        QVERIFY(crash.isWatching());
        QCOMPARE(m_ctx->queue().record(crash.jobId())->state, JobState::Queued);
        QVERIFY(m_ctx->queue().remove(crash.jobId()));
        QTRY_VERIFY_WITH_TIMEOUT(!crash.isWatching(), 5000);

        // "Cancel job": now it stops.
        onSheet(&page, [&](ui::Sheet* s) {
            if (QPushButton* stop = buttonIn(s, QStringLiteral("Cancel job")))
                stop->click();
        });
        cancel->click();
        QTRY_VERIFY_WITH_TIMEOUT(m_ctx->queue().record(upload) && m_ctx->queue().record(upload)->state == JobState::Cancelled, 20000);
        // A list of cancelled transfers is not "All done".
        QTRY_VERIFY_WITH_TIMEOUT(page.transfers()->activeCount() == 0, 5000);
        QCOMPARE(page.transfers()->summaryText(), QStringLiteral("1 not finished"));
    }

    // Two threads logging at once can deliver their records in either order; none is lost.
    void logModelKeepsRecordsThatArriveOutOfOrder()
    {
        Logger::instance().log(LogLevel::Info, QStringLiteral("test"), QStringLiteral("before the model"));
        QTest::qWait(50);
        LogModel model(*m_ctx);
        const int before = model.rowCount();
        QVERIFY(before > 0);
        quint64 top = 0;
        for (int r = 0; r < before; ++r)
            top = std::max(top, model.recordAt(r)->seq);
        // The rows of this test, in the model's order (the app may log lines of its own meanwhile).
        const auto raced = [&model] {
            QStringList out;
            for (int r = 0; r < model.rowCount(); ++r)
                if (model.recordAt(r)->source == QStringLiteral("race"))
                    out << model.recordAt(r)->message;
            return out;
        };
        const LogRecord snapshotRecord = *model.recordAt(before - 1);
        const auto record = [](quint64 seq, const QString& text) {
            LogRecord r;
            r.seq = seq;
            r.time = QDateTime::currentDateTime();
            r.source = QStringLiteral("race");
            r.message = text;
            return r;
        };
        emit m_ctx->logRecordAdded(record(top + 1002, QStringLiteral("second")));
        emit m_ctx->logRecordAdded(record(top + 1001, QStringLiteral("first")));
        // A record the model took from the buffer at construction is not added twice.
        emit m_ctx->logRecordAdded(snapshotRecord);
        QTRY_COMPARE_WITH_TIMEOUT(raced(), (QStringList{QStringLiteral("first"), QStringLiteral("second")}), 5000);
        int copies = 0;
        for (int r = 0; r < model.rowCount(); ++r)
            copies += model.recordAt(r)->seq == snapshotRecord.seq;
        QCOMPARE(copies, 1);
        // A later batch with an older record still shows it.
        emit m_ctx->logRecordAdded(record(top + 1000, QStringLiteral("late")));
        QTRY_COMPARE_WITH_TIMEOUT(raced().size(), 3, 5000);
    }

    // The report holds the crash files themselves, next to the files it always has.
    void problemReportContainsTheCrashFiles()
    {
        const QString crashes = fresh(QStringLiteral("crashes"));
        const QString newest = crashes + QStringLiteral("/crash-20261102-101500.txt");
        const QString newestDump = crashes + QStringLiteral("/crash-20261102-101500.dmp");
        const QString older = crashes + QStringLiteral("/crash-20261101-090000.txt");
        QVERIFY(writeBytes(newest, "Exception 0xC0000005 at somewhere\n"));
        QVERIFY(writeBytes(newestDump, QByteArray("MDMP", 4) + QByteArray(64, '\0')));
        QVERIFY(writeBytes(older, "older\n"));
        QCOMPARE(jobs::newestCrashFiles({newest, newestDump, older}), (QStringList{newest, newestDump}));
        QVERIFY(jobs::newestCrashFiles({}).isEmpty());

        const QString out = fresh(QStringLiteral("report"));
        auto id = jobs::submitProblemReport(*m_ctx, out, QJsonObject{{QStringLiteral("version"), QStringLiteral("test")}},
                                            jobs::newestCrashFiles({newest, newestDump, older}));
        QVERIFY2(id.ok(), qPrintable(id.ok() ? QString() : id.error().message));
        const QJsonArray sent = m_ctx->queue().record(id.value())->parameters.value(QStringLiteral("extraFiles")).toArray();
        QVERIFY(sent.contains(newest));
        QVERIFY(sent.contains(newestDump));
        QVERIFY(!sent.contains(older));
        QCOMPARE(sent.size(), 5); // settings, history, queue + the two crash files
        m_ctx->queue().startNow(id.value());
        QTRY_VERIFY_WITH_TIMEOUT(m_ctx->queue().record(id.value()) && isTerminal(m_ctx->queue().record(id.value())->state), 60000);
        const auto rec = m_ctx->queue().record(id.value());
        QCOMPARE(rec->state, JobState::Succeeded);
        QStringList contents;
        for (const auto& v : rec->outcome->details.value(QStringLiteral("contents")).toArray())
            contents << v.toString();
        QVERIFY2(contents.contains(QStringLiteral("crash-20261102-101500.txt")), qPrintable(contents.join(u' ')));
    }

    // "Sent" is recorded when the console took the payload; a second click while it is on its way sends nothing.
    void payloadIsMarkedSentWhenItWasSent()
    {
        fake::FakeTcpSink sink;
        const QString file = fresh(QStringLiteral("payload")) + QStringLiteral("/elfldr.elf");
        QVERIFY(writeBytes(file, QByteArray("\x7f" "ELF", 4) + QByteArray(2000, '\1')));
        pointSettingsAt(closedPort(), QStringLiteral("/data/etaHEN/games"));
        (void)m_ctx->settings().setValue(ps5::keys::kPayloadPort, int(sink.port()));
        ui::PayloadStore store;
        while (!store.entries().empty())
            store.remove(0);
        store.add({QStringLiteral("elfldr"), file, {}, {}});
        JobQueueModel model(m_ctx->queue());
        pages::Ps5ConnectPage page(*m_ctx, model, store);
        page.resize(1300, 900);
        page.show();
        page.navigateWith({{QStringLiteral("tab"), QStringLiteral("payloads")}});
        QPushButton* send = buttonIn(&page, QStringLiteral("Send"));
        QVERIFY(send && send->isEnabled());
        const auto sends = [&] {
            int n = 0;
            for (const JobRecord& r : m_ctx->queue().records())
                n += r.kind == ps5::jobkinds::kPayloadSend;
            return n;
        };
        send->click();
        send->click();
        QCOMPARE(sends(), 1);
        QVERIFY(!store.entries().front().lastSent.isValid()); // not sent yet
        QTRY_VERIFY_WITH_TIMEOUT(store.entries().front().lastSent.isValid(), 30000);
        QTRY_COMPARE_WITH_TIMEOUT(int(sink.received().size()), 1, 10000);

        // A send that fails leaves the date alone.
        m_ctx->queue().clearFinished();
        store.update(0, {QStringLiteral("elfldr"), file, {}, {}});
        (void)m_ctx->settings().setValue(ps5::keys::kPayloadPort, int(closedPort()));
        send->click();
        QTRY_VERIFY_WITH_TIMEOUT([&] {
            for (const JobRecord& r : m_ctx->queue().records())
                if (r.kind == ps5::jobkinds::kPayloadSend && isTerminal(r.state))
                    return true;
            return false;
        }(), 30000);
        QTest::qWait(100);
        QVERIFY(!store.entries().front().lastSent.isValid());
    }

    // The kernel-log buffer takes a batch in one step and keeps the newest lines.
    void klogModelAppendsBatches()
    {
        pages::KlogModel model;
        int inserts = 0, removes = 0;
        connect(&model, &QAbstractItemModel::rowsInserted, this, [&] { ++inserts; });
        connect(&model, &QAbstractItemModel::rowsRemoved, this, [&] { ++removes; });
        const auto lines = [](int from, int count) {
            std::vector<ps5::KlogLine> out;
            for (int i = 0; i < count; ++i) {
                ps5::KlogLine l;
                l.text = QStringLiteral("line %1").arg(from + i);
                l.tag = (from + i) % 100 == 0 ? ps5::KlogTag::Warning : ps5::KlogTag::Info;
                out.push_back(l);
            }
            return out;
        };
        model.append(lines(0, 15000));
        QCOMPARE(model.rowCount(), 15000);
        QCOMPARE(model.warnings(), 150);
        QCOMPARE(inserts, 1);
        model.append(lines(15000, 10000)); // 5,000 over the 20,000-line buffer
        QCOMPARE(model.rowCount(), 20000);
        QCOMPARE(inserts, 2);
        QCOMPARE(removes, 1);
        QCOMPARE(model.lineAt(0)->text, QStringLiteral("line 5000"));
        QCOMPARE(model.lineAt(19999)->text, QStringLiteral("line 24999"));
        QCOMPARE(model.warnings(), 200);
        model.append(lines(25000, 30000)); // a batch larger than the buffer
        QCOMPARE(model.rowCount(), 20000);
        QCOMPARE(model.lineAt(0)->text, QStringLiteral("line 35000"));
        QCOMPARE(model.warnings(), 200);
        model.append(std::vector<ps5::KlogLine>{});
        QCOMPARE(inserts, 3);
    }

    // Lists in a sheet: every line escaped once, one under the other.
    void monoLinesEscapeOnce()
    {
        const QString html = pages::monoLines({QStringLiteral("debug: 1 → 0"), QStringLiteral("a<b & \"c\"")});
        QCOMPARE(html.count(QStringLiteral("<br>")), 1);
        QVERIFY(html.contains(QStringLiteral("a&lt;b &amp; &quot;c&quot;")));
        QVERIFY(!html.contains(QStringLiteral("&amp;lt;")));
        QVERIFY(!html.contains(QStringLiteral("&lt;br&gt;")));
    }

    // The window opens with its page selected in the sidebar (Home on a first start).
    void firstPageIsSelectedInTheSidebar()
    {
        MainWindow window(*m_ctx);
        QCOMPARE(window.currentPage(), QStringLiteral("home"));
        QCOMPARE(window.sidebar()->current(), QStringLiteral("home"));
        window.navigate(QStringLiteral("queue"));
        QCOMPARE(window.sidebar()->current(), QStringLiteral("queue"));
    }
};

QTEST_MAIN(TstAppShellPs5)
#include "tst_app_shell_ps5.moc"
