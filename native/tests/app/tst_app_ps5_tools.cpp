// PS5 connect console-content tabs (offscreen): Autoloader on a real local
// folder, Fakelib and Manager against the in-process FakeFtpServer, AMPR index
// jobs through the real queue, and the page wiring (tabs, demo states).

#include "app/AppContext.h"
#include "app/MainWindow.h"
#include "app/pages/Ps5ConnectPage.h"
#include "app/pages/ps5/AmprTab.h"
#include "app/pages/ps5/AutoloaderTab.h"
#include "app/pages/ps5/FakelibTab.h"
#include "app/pages/ps5/ManagerTab.h"
#include "app/services/JobSubmit.h"
#include "app/ui/Theme.h"
#include "connect/ps5/FakePs5.h"
#include "connect/ps5/Module.h"
#include "support/TestSupport.h"

#include <QDir>
#include <QFile>
#include <QSettings>
#include <QTabBar>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>

using namespace fw;
using fake::FakeFtpServer;

namespace {

QByteArray readAll(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

bool writeBytes(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(bytes) == bytes.size();
}

const QByteArray kElf = QByteArray("\x7f" "ELF", 4) + QByteArray(40, '\1');

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

// Waits until the job ends; empty when it succeeded, else why not.
QString waitForSuccess(JobQueue& q, const QString& id, int ms = 60000)
{
    (void)QTest::qWaitFor([&] {
        auto r = q.record(id);
        return r && isTerminal(r->state);
    }, ms);
    const auto r = q.record(id);
    if (!r)
        return QStringLiteral("no record");
    if (r->state == JobState::Succeeded)
        return {};
    return jobStateName(r->state) + QStringLiteral(": ") + (r->error ? r->error->toLogString() : QStringLiteral("still running"));
}

QString badgeText(QTableWidget* t, int row, int col)
{
    return t->item(row, col) ? t->item(row, col)->data(Qt::UserRole).toString() : QString();
}

} // namespace

class TstAppPs5Tools : public QObject {
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
    }

    void cleanupTestCase() { m_ctx.reset(); }

    void autoloaderTabEditsALocalFolder()
    {
        const QString dir = fresh(QStringLiteral("ps5_autoloader"));
        QVERIFY(writeBytes(dir + QStringLiteral("/kstuff.elf"), kElf));
        QVERIFY(writeBytes(dir + QStringLiteral("/autoload.txt"), "kstuff.elf\r\n\r\n\r\n\r\n!1000\r\n"));
        pages::AutoloaderTab tab(*m_ctx, fakeConsole(1));
        tab.setUseRecycleBin(false); // never the developer's real recycle bin
        tab.resize(1100, 900);
        tab.openFolder(dir);
        QCOMPARE(m_ctx->settings().value(ps5::keys::kAutoloaderFolder).toString(), dir);
        QTRY_VERIFY_WITH_TIMEOUT(!tab.isBusy() && !tab.editor()->toPlainText().isEmpty(), 10000);
        QCOMPARE(tab.editor()->toPlainText(), QStringLiteral("kstuff.elf\n\n!1000\n")); // legacy normalisation on load
        QCOMPARE(tab.chipNames(), QStringList{QStringLiteral("kstuff.elf")});
        QVERIFY(!tab.isDirty());

        // Insert below the cursor's line, then save: LF only, previous file kept as .bak.
        QTextCursor c = tab.editor()->textCursor();
        c.setPosition(0);
        tab.editor()->setTextCursor(c);
        tab.insertSleepLine(2500);
        QCOMPARE(tab.editor()->toPlainText(), QStringLiteral("kstuff.elf\n!2500\n\n!1000\n"));
        QVERIFY(tab.isDirty());
        tab.saveNow();
        QTRY_VERIFY_WITH_TIMEOUT(!tab.isBusy() && !tab.isDirty(), 10000);
        QCOMPARE(readAll(dir + QStringLiteral("/autoload.txt")), QByteArray("kstuff.elf\n!2500\n\n!1000\n"));
        QCOMPARE(readAll(dir + QStringLiteral("/autoload.txt.bak")), QByteArray("kstuff.elf\r\n\r\n\r\n\r\n!1000\r\n"));

        // Add a payload: copied into the folder, chip added, line inserted.
        const QString src = fresh(QStringLiteral("al-src")) + QStringLiteral("/elfldr.elf");
        QVERIFY(writeBytes(src, kElf));
        tab.addPayloadFile(src, false);
        QTRY_VERIFY_WITH_TIMEOUT(!tab.isBusy() && tab.chipNames().size() == 2, 10000);
        QCOMPARE(readAll(dir + QStringLiteral("/elfldr.elf")), kElf);
        QVERIFY(tab.editor()->toPlainText().contains(QStringLiteral("\nelfldr.elf")));
        QVERIFY(tab.validationText().contains(QStringLiteral("elfldr.elf must come before"))); // inserted after kstuff.elf

        // Delete a payload: file gone, its line and the sleep right after it leave the editor.
        tab.deletePayloadFile(QStringLiteral("kstuff.elf"));
        QTRY_VERIFY_WITH_TIMEOUT(!tab.isBusy() && tab.chipNames() == QStringList{QStringLiteral("elfldr.elf")}, 10000);
        QVERIFY(!QFileInfo::exists(dir + QStringLiteral("/kstuff.elf")));
        QVERIFY(!tab.editor()->toPlainText().contains(QStringLiteral("kstuff.elf")));
        QVERIFY(!tab.editor()->toPlainText().contains(QStringLiteral("!2500")));

        // A folder without autoload.txt starts from the legacy header; saving creates the file.
        const QString empty = fresh(QStringLiteral("al-new"));
        tab.openFolder(empty);
        QTRY_VERIFY_WITH_TIMEOUT(!tab.isBusy() && tab.editor()->toPlainText().startsWith(QStringLiteral("# ps5_autoloader")), 10000);
        QVERIFY(tab.isDirty());
        tab.saveNow();
        QTRY_VERIFY_WITH_TIMEOUT(!tab.isBusy() && !tab.isDirty(), 10000);
        QCOMPARE(readAll(empty + QStringLiteral("/autoload.txt")), ps5::autoloader::defaultHeader().toUtf8());
        QVERIFY(!QFileInfo::exists(empty + QStringLiteral("/autoload.txt.bak")));

        // A folder that is not there: nothing of the previous folder stays editable.
        tab.openFolder(empty + QStringLiteral("/missing"));
        QVERIFY(tab.editor()->toPlainText().isEmpty());
        QVERIFY(tab.chipNames().isEmpty());
        QVERIFY(tab.editor()->isReadOnly());
    }

    void fakelibTabManagesOverlaysOnAFakeConsole()
    {
        const QString root = fresh(QStringLiteral("fl-console"));
        FakeFtpServer::Options o;
        o.root = root;
        o.mlsd = true;
        FakeFtpServer srv(o);
        pages::FakelibTab tab(*m_ctx, fakeConsole(srv.port()));
        tab.resize(1100, 700);
        tab.refresh();
        QTRY_VERIFY_WITH_TIMEOUT(!tab.isBusy(), 10000);
        QVERIFY(tab.listing().overlays.empty());
        QVERIFY(tab.statusText().contains(QStringLiteral("does not exist yet")));

        const QString lib = fresh(QStringLiteral("fl-local")) + QStringLiteral("/libSceAmpr.sprx");
        QVERIFY(writeBytes(lib, kElf));
        tab.addFile(lib, false);
        QTRY_VERIFY_WITH_TIMEOUT(!tab.isBusy() && tab.table()->rowCount() == 1, 10000);
        QCOMPARE(readAll(srv.path(QStringLiteral("/data/shadowmount/fakelib/libSceAmpr.sprx"))), kElf);
        QCOMPARE(badgeText(tab.table(), 0, 0), QStringLiteral("Enabled"));

        tab.table()->selectRow(0);
        tab.toggleSelected();
        QTRY_VERIFY_WITH_TIMEOUT(!tab.isBusy() && badgeText(tab.table(), 0, 0) == QStringLiteral("Disabled"), 10000);
        QVERIFY(QFileInfo::exists(srv.path(QStringLiteral("/data/shadowmount/fakelib/disabled/libSceAmpr.sprx"))));

        tab.table()->selectRow(0);
        const QString newer = fresh(QStringLiteral("fl-newer")) + QStringLiteral("/libSceAmpr.sprx");
        QVERIFY(writeBytes(newer, QByteArray("\x4f\x15\x3d\x1d", 4) + QByteArray(10, 'n')));
        tab.replaceSelected(newer, false);
        QTRY_VERIFY_WITH_TIMEOUT(!tab.isBusy(), 10000);
        QCOMPARE(readAll(srv.path(QStringLiteral("/data/shadowmount/fakelib/disabled/libSceAmpr.sprx"))).left(4), QByteArray("\x4f\x15\x3d\x1d", 4));

        tab.table()->selectRow(0);
        tab.removeSelected();
        QTRY_VERIFY_WITH_TIMEOUT(!tab.isBusy() && tab.table()->rowCount() == 0, 10000);
        QVERIFY(!QFileInfo::exists(srv.path(QStringLiteral("/data/shadowmount/fakelib/disabled/libSceAmpr.sprx"))));

        // A file without an ELF/SELF header needs the user's answer; without it nothing is uploaded.
        const QString odd = fresh(QStringLiteral("fl-odd")) + QStringLiteral("/libOdd.prx");
        QVERIFY(writeBytes(odd, "not an elf"));
        tab.addFile(odd, false);
        QTRY_VERIFY_WITH_TIMEOUT(!tab.isBusy(), 10000);
        QVERIFY(tab.statusText().contains(QStringLiteral("failed")));
        QVERIFY(!QFileInfo::exists(srv.path(QStringLiteral("/data/shadowmount/fakelib/libOdd.prx"))));
    }

    void managerTabListsGamesAndComparesFolders()
    {
        const QString root = fresh(QStringLiteral("mgr-console"));
        QVERIFY(writeBytes(root + QStringLiteral("/data/etaHEN/games/Same.exfat"), QByteArray(10, 's')));
        QVERIFY(writeBytes(root + QStringLiteral("/data/etaHEN/games/Diff.ffpkg"), QByteArray(10, 'd')));
        QVERIFY(writeBytes(root + QStringLiteral("/data/etaHEN/games/GAME/eboot.bin"), QByteArray(70000, 'e')));
        QVERIFY(writeBytes(root + QStringLiteral("/data/etaHEN/games/GAME/sce_sys/param.json"), "{\"titleId\":\"PPSA00001\"}"));
        QVERIFY(writeBytes(root + QStringLiteral("/data/etaHEN/games/GAME/extra.bin"), "x"));
        const QString local = fresh(QStringLiteral("mgr-local"));
        QVERIFY(writeBytes(local + QStringLiteral("/Same.exfat"), QByteArray(10, 's')));
        QVERIFY(writeBytes(local + QStringLiteral("/Diff.ffpkg"), QByteArray(11, 'd')));
        QVERIFY(writeBytes(local + QStringLiteral("/New.ffpfsc"), QByteArray(3, 'n')));
        FakeFtpServer::Options o;
        o.root = root;
        FakeFtpServer srv(o);
        pages::Ps5ManagerTab tab(*m_ctx, fakeConsole(srv.port()));
        tab.resize(1200, 1400);
        tab.setConsoleFolders(QStringLiteral("/data/etaHEN/games"));
        tab.setLocalFolder(local);
        tab.refresh();
        QTRY_VERIFY_WITH_TIMEOUT(!tab.isBusy() && !tab.rows().empty(), 10000);
        QCOMPARE(int(tab.rows().size()), 4); // Diff, GAME (folder), New (local only), Same
        QCOMPARE(tab.gamesTable()->rowCount(), 4);
        QVERIFY(tab.freeSpaceText() != QStringLiteral("—"));
        tab.setStatusFilter(2); // size mismatch
        QCOMPARE(tab.gamesTable()->rowCount(), 1);
        QCOMPARE(tab.gamesTable()->item(0, 0)->text(), QStringLiteral("Diff.ffpkg"));
        QCOMPARE(badgeText(tab.gamesTable(), 0, 1), QStringLiteral("Size mismatch"));
        tab.setStatusFilter(3); // local only
        QCOMPARE(tab.gamesTable()->item(0, 0)->text(), QStringLiteral("New.ffpfsc"));
        tab.setStatusFilter(0);

        // Folder compare with a local dump.
        const QString dump = fresh(QStringLiteral("mgr-dump"));
        QVERIFY(writeBytes(dump + QStringLiteral("/eboot.bin"), QByteArray(70000, 'e')));
        QVERIFY(writeBytes(dump + QStringLiteral("/sce_sys/param.json"), "{\"titleId\":\"PPSA00001\"}"));
        QVERIFY(writeBytes(dump + QStringLiteral("/missing.bin"), "m"));
        tab.compare(QStringLiteral("/data/etaHEN/games/GAME"), dump);
        QTRY_VERIFY_WITH_TIMEOUT(!tab.isBusy() && tab.report().has_value(), 10000);
        QCOMPARE(tab.report()->count(ps5::DiffKind::MissingOnConsole), 1);
        QCOMPARE(tab.report()->count(ps5::DiffKind::OnlyOnConsole), 1);
        QCOMPARE(tab.report()->matching, 2);
        QCOMPARE(tab.compareTable()->rowCount(), 2);
        QVERIFY(tab.compareSummary().contains(QStringLiteral("1 missing on the PS5")));

        // Compare with an .exfat image built by the real engine from the console's own files.
        if (!jobs::isAvailable(*m_ctx, QStringLiteral("image.exfat.build")))
            QSKIP("exFAT engine not registered in this build");
        const QString imgSrc = fresh(QStringLiteral("GAMEIMG"));
        QVERIFY(writeBytes(imgSrc + QStringLiteral("/eboot.bin"), QByteArray(70000, 'e')));
        QVERIFY(writeBytes(imgSrc + QStringLiteral("/sce_sys/param.json"), "{\"titleId\":\"PPSA00001\"}"));
        QVERIFY(writeBytes(imgSrc + QStringLiteral("/extra.bin"), "x"));
        const QString outDir = fresh(QStringLiteral("mgr-img"));
        auto job = jobs::submitImageBuild(*m_ctx, jobs::kFormatExfat, imgSrc, outDir, QStringLiteral("GAMEIMG"), {});
        QVERIFY2(job.ok(), qPrintable(job.ok() ? QString() : job.error().message));
        const QString built = waitForSuccess(m_ctx->queue(), job.value());
        QVERIFY2(built.isEmpty(), qPrintable(built));
        tab.compare(QStringLiteral("/data/etaHEN/games/GAME"), outDir + QStringLiteral("/GAMEIMG.exfat"));
        QTRY_VERIFY_WITH_TIMEOUT(!tab.isBusy() && tab.report()->localSource.endsWith(QStringLiteral(".exfat")), 15000);
        QVERIFY2(tab.report()->identical(), qPrintable(tab.report()->summary()));
        QCOMPARE(tab.report()->localFiles, 3);
        m_ctx->queue().clearFinished();
    }

    void amprTabQueuesBothJobs()
    {
        const QString root = fresh(QStringLiteral("ampr-console"));
        const QString app0 = root + QStringLiteral("/g/app0");
        QVERIFY(writeBytes(app0 + QStringLiteral("/eboot.bin"), "e"));
        QVERIFY(writeBytes(app0 + QStringLiteral("/fakelib/libSceAmpr.sprx"), kElf));
        QVERIFY(writeBytes(app0 + QStringLiteral("/Data/a.bin"), "a"));
        FakeFtpServer::Options o;
        o.root = root;
        o.mlsd = true;
        FakeFtpServer srv(o);
        pages::AmprTab tab(*m_ctx, fakeConsole(srv.port()));

        const QString id = tab.submitConsole(QStringLiteral("/g/app0"));
        QVERIFY(!id.isEmpty());
        const QString uploaded = waitForSuccess(m_ctx->queue(), id);
        QVERIFY2(uploaded.isEmpty(), qPrintable(uploaded));
        QVERIFY(readAll(app0 + QStringLiteral("/ampr_emu.index")).startsWith("AMPRIDX3"));

        // Local: refused without the AMPR library unless forced (the UI asks first).
        const QString plain = fresh(QStringLiteral("ampr-local"));
        QVERIFY(writeBytes(plain + QStringLiteral("/eboot.bin"), "e"));
        QVERIFY(tab.submitLocal(plain, false).isEmpty());
        const QString forced = tab.submitLocal(plain, true);
        QVERIFY(!forced.isEmpty());
        const QString written = waitForSuccess(m_ctx->queue(), forced);
        QVERIFY2(written.isEmpty(), qPrintable(written));
        QVERIFY(readAll(plain + QStringLiteral("/ampr_emu.index")).startsWith("AMPRIDX3"));
        m_ctx->queue().clearFinished();
    }

    void pageHasTheToolTabsAndDemoStates()
    {
        MainWindow window(*m_ctx);
        window.resize(1440, 920);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto* page = qobject_cast<pages::Ps5ConnectPage*>(window.page(QStringLiteral("ps5")));
        QVERIFY(page);
        window.navigate(QStringLiteral("ps5"));
        auto* tabs = page->findChild<QTabBar*>();
        QVERIFY(tabs);
        QStringList titles;
        for (int i = 0; i < tabs->count(); ++i)
            titles << tabs->tabText(i);
        for (const QString& t : {QStringLiteral("Manager"), QStringLiteral("Fakelib"), QStringLiteral("Autoloader"), QStringLiteral("AMPR index")})
            QVERIFY2(titles.contains(t), qPrintable(titles.join(u',')));
        const QStringList states = page->demoStates();
        const QString shots = qEnvironmentVariable("FW_TAB_SHOTS");
        for (const QString& key : {QStringLiteral("manager"), QStringLiteral("fakelib"), QStringLiteral("autoloader"), QStringLiteral("ampr")}) {
            QVERIFY(states.contains(key));
            page->applyDemoState(key);
            QCOMPARE(tabs->tabText(tabs->currentIndex()).toLower().left(4), key.left(4));
            pages::ConsoleToolTab* tool = nullptr;
            for (auto* t : page->findChildren<pages::ConsoleToolTab*>())
                if (t->demoKey() == key)
                    tool = t;
            QVERIFY(tool);
            QVERIFY(tool->isVisible());
            if (!shots.isEmpty()) { // full-height render of the tab for review
                QDir().mkpath(shots);
                tool->resize(tool->width(), std::max(tool->height(), tool->sizeHint().height()));
                QVERIFY(tool->grab().save(QDir(shots).filePath(key + QStringLiteral(".png"))));
            }
            page->clearDemoState();
            QCOMPARE(tabs->currentIndex(), 0);
        }
    }
};

QTEST_MAIN(TstAppPs5Tools)
#include "tst_app_ps5_tools.moc"
