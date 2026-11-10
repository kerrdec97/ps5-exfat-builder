// PS5 connect console-content tabs (offscreen): Autoloader on a real local
// folder, Fakelib and Manager against the in-process FakeFtpServer, AMPR index
// jobs through the real queue, and the page wiring (tabs, demo states).

#include "app/AppContext.h"
#include "app/MainWindow.h"
#include "app/pages/Ps5ConnectPage.h"
#include "app/pages/ps5/AmprTab.h"
#include "app/pages/ps5/AutoloaderTab.h"
#include "app/pages/ps5/ConsoleHero.h"
#include "app/pages/ps5/FakelibTab.h"
#include "app/pages/ps5/ManagerTab.h"
#include "app/pages/ps5/ShadowMountTab.h"
#include "app/pages/ps5/TransferPanel.h"
#include "app/services/JobSubmit.h"
#include "app/ui/Theme.h"
#include "connect/ps5/FakePs5.h"
#include "connect/ps5/Module.h"
#include "connect/ps5/MountConfig.h"
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
        QVERIFY(QFileInfo::exists(srv.path(QStringLiteral("/data/shadowmount/fakelib.disabled/libSceAmpr.sprx"))));

        tab.table()->selectRow(0);
        const QString newer = fresh(QStringLiteral("fl-newer")) + QStringLiteral("/libSceAmpr.sprx");
        QVERIFY(writeBytes(newer, QByteArray("\x4f\x15\x3d\x1d", 4) + QByteArray(10, 'n')));
        tab.replaceSelected(newer, false);
        QTRY_VERIFY_WITH_TIMEOUT(!tab.isBusy(), 10000);
        QCOMPARE(readAll(srv.path(QStringLiteral("/data/shadowmount/fakelib.disabled/libSceAmpr.sprx"))).left(4), QByteArray("\x4f\x15\x3d\x1d", 4));

        tab.table()->selectRow(0);
        tab.removeSelected();
        QTRY_VERIFY_WITH_TIMEOUT(!tab.isBusy() && tab.table()->rowCount() == 0, 10000);
        QVERIFY(!QFileInfo::exists(srv.path(QStringLiteral("/data/shadowmount/fakelib.disabled/libSceAmpr.sprx"))));

        // A file without an ELF/SELF header needs the user's answer; without it nothing is uploaded.
        const QString odd = fresh(QStringLiteral("fl-odd")) + QStringLiteral("/libOdd.prx");
        QVERIFY(writeBytes(odd, "not an elf"));
        tab.addFile(odd, false);
        QTRY_VERIFY_WITH_TIMEOUT(!tab.isBusy(), 10000);
        QVERIFY2(tab.statusText().contains(QStringLiteral("libOdd.prx not added")), qPrintable(tab.statusText()));
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

        // ampr_emu 0.4.2.1 asset packs are bound to the index they came with: refused unless forced.
        const QString packed = fresh(QStringLiteral("ampr-packed"));
        QVERIFY(writeBytes(packed + QStringLiteral("/eboot.bin"), "e"));
        QVERIFY(writeBytes(packed + QStringLiteral("/fakelib/libSceAmpr.sprx"), kElf));
        QVERIFY(writeBytes(packed + QStringLiteral("/ampr_assets.index"), "pack"));
        QVERIFY(writeBytes(packed + QStringLiteral("/ampr_emu.index"), "original"));
        QVERIFY(tab.submitLocal(packed, false).isEmpty());
        QCOMPARE(readAll(packed + QStringLiteral("/ampr_emu.index")), QByteArray("original"));
        const QString overridden = tab.submitLocal(packed, false, true);
        QVERIFY(!overridden.isEmpty());
        QVERIFY(waitForSuccess(m_ctx->queue(), overridden).isEmpty());
        QVERIFY(readAll(packed + QStringLiteral("/ampr_emu.index")).startsWith("AMPRIDX3"));
        QVERIFY(writeBytes(app0 + QStringLiteral("/ampr_assets.index.runtime"), "pack"));
        const QString onConsole = tab.submitConsole(QStringLiteral("/g/app0"));
        QVERIFY(!onConsole.isEmpty());
        const QString refused = waitForSuccess(m_ctx->queue(), onConsole);
        QVERIFY2(refused.contains(QStringLiteral("asset packs")), qPrintable(refused));
        m_ctx->queue().clearFinished();
    }

    void pageGroupsToolsAndDemoStates()
    {
        MainWindow window(*m_ctx);
        window.resize(1440, 920);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto* page = qobject_cast<pages::Ps5ConnectPage*>(window.page(QStringLiteral("ps5")));
        QVERIFY(page);
        window.navigate(QStringLiteral("ps5"));
        auto* tabs = page->findChild<QTabBar*>(QStringLiteral("ps5Groups"));
        QVERIFY(tabs);
        QCOMPARE(tabs, page->groupBar());
        QStringList titles;
        for (int i = 0; i < tabs->count(); ++i)
            titles << tabs->tabText(i);
        QCOMPARE(titles, (QStringList{QStringLiteral("Files"), QStringLiteral("Payloads"), QStringLiteral("Logs"), QStringLiteral("ShadowMount+"),
                                      QStringLiteral("Console tools")}));
        QCOMPARE(page->currentTool(), QStringLiteral("ftp"));
        // Every stable key other pages use still opens its tool (Home: "payloads", "diagnostics"; Backport: "klog").
        const QStringList keys = {QStringLiteral("ftp"), QStringLiteral("manager"), QStringLiteral("payloads"), QStringLiteral("klog"),
                                  QStringLiteral("smplog"), QStringLiteral("crashdumps"), QStringLiteral("mount"), QStringLiteral("fakelib"),
                                  QStringLiteral("autoloader"), QStringLiteral("ampr"), QStringLiteral("y2jb"), QStringLiteral("install"),
                                  QStringLiteral("diagnostics")};
        for (const QString& key : keys) {
            window.navigate(QStringLiteral("ps5"), {{QStringLiteral("tab"), key}});
            QCOMPARE(page->currentTool(), key);
        }
        window.navigate(QStringLiteral("ps5"), {{QStringLiteral("tab"), QStringLiteral("logs")}});
        QCOMPARE(page->currentGroup(), QStringLiteral("logs"));
        window.navigate(QStringLiteral("ps5"), {{QStringLiteral("tab"), 3}}); // older callers: a group index
        QCOMPARE(page->currentGroup(), QStringLiteral("shadowmount"));

        QStringList states = page->demoStates();
        QCOMPARE(states.removeDuplicates(), qsizetype(0));
        for (const QString& key : {QStringLiteral("manager"), QStringLiteral("fakelib"), QStringLiteral("autoloader"), QStringLiteral("ampr"),
                                   QStringLiteral("mount"), QStringLiteral("smplog"), QStringLiteral("crashdumps"), QStringLiteral("install")}) {
            QVERIFY2(states.contains(key), qPrintable(key));
            page->applyDemoState(key);
            QCOMPARE(page->currentTool(), key);
            pages::ConsoleToolTab* tool = nullptr;
            for (auto* t : page->findChildren<pages::ConsoleToolTab*>())
                if (t->demoKey() == key)
                    tool = t;
            QVERIFY(tool);
            QVERIFY(tool->isVisible());
            page->clearDemoState();
            QCOMPARE(tabs->currentIndex(), 0);
            QCOMPARE(page->currentTool(), QStringLiteral("ftp"));
        }
        // The browser example: a listing (folders first) and example transfers; cleared after.
        page->applyDemoState(QStringLiteral("ftp"));
        QVERIFY(page->remoteTable()->rowCount() > 3);
        QCOMPARE(page->remoteNames().first(), QStringLiteral("backports"));
        QCOMPARE(page->transfers()->rowCount(), 0); // example rows are not queue rows
        QCOMPARE(page->hero()->state(), pages::ConsoleHero::State::Connected);
        page->clearDemoState();
        QCOMPARE(page->remoteTable()->rowCount(), 0);
        QCOMPARE(page->hero()->state(), pages::ConsoleHero::State::Offline);

        // Review shots of every state, wide and narrow (FW_PS5_SHOTS=<folder>).
        const QString shots = qEnvironmentVariable("FW_PS5_SHOTS");
        if (!shots.isEmpty()) {
            QDir().mkpath(shots);
            for (const QSize size : {QSize(1440, 920), QSize(1100, 760)}) {
                window.resize(size);
                for (const QString& state : QStringList{QString()} + states) {
                    if (!state.isEmpty())
                        page->applyDemoState(state);
                    for (int i = 0; i < 8; ++i)
                        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
                    QVERIFY(window.grab().save(QDir(shots).filePath(QStringLiteral("%1-%2.png").arg(size.width()).arg(state.isEmpty() ? QStringLiteral("base") : state))));
                    if (QWidget* content = page->scrollArea()->widget()) // the whole page, below the fold too
                        QVERIFY(content->grab().save(QDir(shots).filePath(QStringLiteral("%1-%2-full.png").arg(size.width()).arg(state.isEmpty() ? QStringLiteral("base") : state))));
                    if (!state.isEmpty())
                        page->clearDemoState();
                }
            }
            window.resize(1440, 920);
        }
    }

    // The browser against an ftpsrv-like server: identity in the console card,
    // folders first in both sort directions, one transfer row per uploaded file.
    void browserUploadsAndListsOnFtpsrv()
    {
        const QString root = fresh(QStringLiteral("ftpsrv-root"));
        QVERIFY(writeBytes(root + QStringLiteral("/data/etaHEN/games/zeta.exfat"), QByteArray(5000, 'z')));
        QVERIFY(writeBytes(root + QStringLiteral("/data/etaHEN/games/alpha.bin"), QByteArray(10, 'a')));
        QVERIFY(QDir().mkpath(root + QStringLiteral("/data/etaHEN/games/Middle")));
        FakeFtpServer::Options o;
        o.root = root;
        o.ftpsrv = true;
        FakeFtpServer srv(o);
        MainWindow window(*m_ctx);
        window.resize(1440, 920);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto* page = qobject_cast<pages::Ps5ConnectPage*>(window.page(QStringLiteral("ps5")));
        window.navigate(QStringLiteral("ps5"));
        (void)m_ctx->settings().setValue(ps5::keys::kConsoleIp, QStringLiteral("127.0.0.1"));
        (void)m_ctx->settings().setValue(ps5::keys::kFtpPort, int(srv.port()));
        (void)m_ctx->settings().setValue(ps5::keys::kGamesFolder, QStringLiteral("/data/etaHEN/games/"));
        page->session()->connectTo(QStringLiteral("127.0.0.1"), srv.port(), 5000, true);
        QTRY_VERIFY_WITH_TIMEOUT(page->isReachable(), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(page->hero()->chip(QStringLiteral("ftp"))->detail() == QStringLiteral("ftpsrv v0.21.1"), 10000);
        QCOMPARE(page->hero()->state(), pages::ConsoleHero::State::Connected);
        QTRY_VERIFY_WITH_TIMEOUT(page->remoteTable()->rowCount() == 3, 10000);
        QCOMPARE(page->remoteNames(), (QStringList{QStringLiteral("Middle"), QStringLiteral("alpha.bin"), QStringLiteral("zeta.exfat")}));
        page->remoteTable()->sortByColumn(1, Qt::DescendingOrder); // by size, largest first
        QCOMPARE(page->remoteNames(), (QStringList{QStringLiteral("Middle"), QStringLiteral("zeta.exfat"), QStringLiteral("alpha.bin")}));
        page->remoteTable()->sortByColumn(0, Qt::AscendingOrder);

        // Two files: two transfer rows, each with its own progress, then cleared.
        const QString local = fresh(QStringLiteral("ftpsrv-up"));
        QVERIFY(writeBytes(local + QStringLiteral("/one.bin"), QByteArray(70000, '1')));
        QVERIFY(writeBytes(local + QStringLiteral("/eboot.bin"), QByteArray("\x4f\x15\x3d\x1d", 4) + QByteArray(9000, 'e'))); // fself: SELF mode is off
        m_ctx->queue().clearFinished();
        page->uploadFiles({local + QStringLiteral("/one.bin"), local + QStringLiteral("/eboot.bin")});
        QTRY_COMPARE_WITH_TIMEOUT(page->transfers()->rowCount(), 2, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(page->transfers()->activeCount() == 0, 30000);
        QCOMPARE(page->transfers()->row(0)->stateText(), QStringLiteral("Done"));
        QCOMPARE(page->transfers()->row(1)->stateText(), QStringLiteral("Done"));
        QCOMPARE(readAll(root + QStringLiteral("/data/etaHEN/games/one.bin")), QByteArray(70000, '1'));
        QCOMPARE(readAll(root + QStringLiteral("/data/etaHEN/games/eboot.bin")).size(), qsizetype(9004));
        QVERIFY(srv.sawCommand(QStringLiteral("SELF")));
        page->transfers()->clearFinished();
        QTRY_COMPARE_WITH_TIMEOUT(page->transfers()->rowCount(), 0, 5000);
        page->session()->disconnect();
        QTRY_VERIFY_WITH_TIMEOUT(!page->isReachable(), 10000);
        m_ctx->queue().clearFinished();
    }

    // ShadowMount+ settings: load a 1.7 flat config, change one value, push
    // only that line; unknown keys and comments stay, kstuff_* go.
    void shadowMountTabPushesOnlyChangedKeys()
    {
        const QString root = fresh(QStringLiteral("smp-tab"));
        const QByteArray original = "# my config\ndebug=1\nfuture_option = keep\nkstuff_delay=PPSA01234:30\nscanpath=/mnt/usb0/games\n";
        QVERIFY(writeBytes(root + QStringLiteral("/data/shadowmount/config.ini"), original));
        QVERIFY(writeBytes(root + QStringLiteral("/data/shadowmount/debug.log"), "ShadowMount+ v1.7beta11 exFAT/UFS/PFS/LVD/MD. FW: 9.60.\n"));
        FakeFtpServer::Options o;
        o.root = root;
        FakeFtpServer srv(o);
        auto ep = fakeConsole(srv.port());
        pages::ShadowMountTab tab(*m_ctx, [ep] {
            auto e = ep();
            e.smpApiPort = 1; // no API: version from the debug.log banner
            return e;
        });
        tab.resize(1100, 900);
        tab.load();
        QTRY_VERIFY_WITH_TIMEOUT(tab.hasLoaded() && !tab.isBusy(), 15000);
        QCOMPARE(tab.format(), ps5::SmpConfigFormat::Flat17);
        QCOMPARE(tab.value(QStringLiteral("scan_interval_seconds")), QStringLiteral("15")); // the 1.7 default, not in the file
        QVERIFY(tab.changes().isEmpty());
        tab.setValue(QStringLiteral("scan_interval_seconds"), QStringLiteral("30"));
        QCOMPARE(tab.changes(), (ps5::MountConfigValues{{QStringLiteral("scan_interval_seconds"), QStringLiteral("30")}}));
        tab.push(true);
        QTRY_VERIFY_WITH_TIMEOUT(!tab.isBusy() && tab.statusText().contains(QStringLiteral("written")), 15000);
        QCOMPARE(readAll(root + QStringLiteral("/data/shadowmount/config.ini")),
                 QByteArray("# my config\ndebug=1\nfuture_option = keep\nscanpath=/mnt/usb0/games\nscan_interval_seconds=30\n"));
        QVERIFY(tab.changes().isEmpty()); // reloaded after the push
        // Back to the default: the line goes.
        tab.resetToDefault(QStringLiteral("scan_interval_seconds"));
        QCOMPARE(tab.changes(), (ps5::MountConfigValues{{QStringLiteral("scan_interval_seconds"), QString()}}));
        tab.push(true);
        QTRY_VERIFY_WITH_TIMEOUT(!tab.isBusy() && tab.changes().isEmpty(), 15000);
        QVERIFY(!readAll(root + QStringLiteral("/data/shadowmount/config.ini")).contains("scan_interval_seconds"));
        // Changed on the console meanwhile: refused, nothing written.
        QVERIFY(writeBytes(root + QStringLiteral("/data/shadowmount/config.ini"), "debug=0\n"));
        tab.setValue(QStringLiteral("quiet_mode"), QStringLiteral("1"));
        tab.push(true);
        QTRY_VERIFY_WITH_TIMEOUT(!tab.isBusy() && tab.statusText().contains(QStringLiteral("changed on the console")), 15000);
        QCOMPARE(readAll(root + QStringLiteral("/data/shadowmount/config.ini")), QByteArray("debug=0\n"));
    }
};

QTEST_MAIN(TstAppPs5Tools)
#include "tst_app_ps5_tools.moc"
