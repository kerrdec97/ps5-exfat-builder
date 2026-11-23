// The Backport tab's own rules, driven through the real page in the real main
// window with a stand-in engine (a small command script that speaks the worker
// protocol and waits to be cancelled), so no real engine and no real game is
// involved. The PS5 Backport settings, ledger and data folders are temporary
// ones (D97BETA_CONFIG_DIR / D97BETA_DATA_DIR), never the user's.
//
//  * one worker at a time: a second start while the destinations are checked
//    is ignored, the shortcut never cancels, and a scan that ends during a run
//    leaves the run alone;
//  * the shared settings.json is merged, not replaced; a damaged one is left
//    as it is; folders are written with native separators;
//  * the AMPR / PlayGo sheet checks the choice without closing;
//  * "Clear list" asks before it forgets the scan locations, also when the
//    list is empty; ticks survive a scan that is restarted.

#include "app/AppContext.h"
#include "app/MainWindow.h"
#include "app/backport/BackportCore.h"
#include "app/backport/BackportHost.h"
#include "app/pages/AboutPage.h"
#include "app/pages/BackportPage.h"
#include "app/pages/DumpCheckSheet.h"
#include "app/ui/Sheet.h"
#include "app/ui/Theme.h"
#include "app/ui/Widgets.h"
#include "core/Paths.h"
#include "support/TestSupport.h"

#include <QAbstractButton>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QScopeGuard>
#include <QSettings>
#include <QSortFilterProxyModel>
#include <QSpinBox>
#include <QTableView>
#include <QTemporaryDir>
#include <QTest>
#include <QTextEdit>
#include <QTimer>
#include <QUrl>

#include <atomic>
#include <chrono>
#include <functional>
#include <thread>

using namespace fw;

namespace {

const char* const kFakeEngine =
    "@echo off\r\n"
    "findstr /c:\"selftest\" \"%~2\" >nul 2>&1\r\n"
    "if not errorlevel 1 goto selftest\r\n"
    "copy /y \"%~2\" \"%~dp0last-job.json\" >nul 2>&1\r\n"
    "echo start>>\"%~dp0starts.log\"\r\n"
    "echo {\"event\":\"progress\",\"stage\":\"BASELINE\",\"label\":\"Working\",\"detail\":\"\",\"pct\":10,\"phase\":\"engine\"}\r\n"
    "if exist \"%~dp0hold.flag\" goto hold\r\n"
    "set /p LINE=\r\n"
    "echo {\"event\":\"result\",\"status\":\"CANCELLED\",\"headline\":\"Cancelled by the test\"}\r\n"
    "exit /b 0\r\n"
    // hold.flag: this job waits for go.flag instead of stdin and then ends DONE
    // (the next job of the run starts as usual).
    ":hold\r\n"
    "if not exist \"%~dp0go.flag\" goto hold\r\n"
    "del \"%~dp0hold.flag\" \"%~dp0go.flag\" >nul 2>&1\r\n"
    "echo {\"event\":\"result\",\"status\":\"DONE\",\"headline\":\"Done by the test\"}\r\n"
    "exit /b 0\r\n"
    ":selftest\r\n"
    "echo {\"event\":\"result\",\"status\":\"DONE\",\"revision\":\"fake-0001\"}\r\n"
    "exit /b 0\r\n";

QByteArray readAll(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

QJsonObject readJson(const QString& path) { return QJsonDocument::fromJson(readAll(path)).object(); }

QStringList rootsOf(const QJsonObject& settings)
{
    QStringList out;
    for (const QJsonValue& v : settings.value(QStringLiteral("scan_roots")).toArray())
        out << v.toString();
    return out;
}

void makeGame(const QString& folder)
{
    QDir().mkpath(folder);
    QFile e(folder + QStringLiteral("/eboot.bin"));
    if (e.open(QIODevice::WriteOnly))
        e.write(QByteArray("\x7f" "ELF", 4) + QByteArray(64, 0));
}

} // namespace

class TstPagesbBackport : public QObject {
    Q_OBJECT
    QTemporaryDir m_root;
    std::unique_ptr<AppContext> m_ctx;
    std::unique_ptr<MainWindow> m_window;
    QString m_games, m_many, m_out, m_bak, m_settingsFile, m_startsLog;
    QTimer m_sheetWatch;
    QStringList m_sheetsSeen;
    // A sheet runs its own event loop, so whatever a test does with one is
    // done from the watcher: the next sheet that has a button `m_expectButton`
    // is handed to `m_expectDo`.
    QString m_expectButton;
    std::function<void(ui::Sheet*)> m_expectDo;
    bool m_expectMet = false;
    void expectSheet(const QString& button, std::function<void(ui::Sheet*)> then)
    {
        m_expectButton = button;
        m_expectDo = std::move(then);
        m_expectMet = false;
    }
    static constexpr int kMany = 1200;

    pages::BackportPage* page() { return m_window->findChild<pages::BackportPage*>(); }
    QTableView* table()
    {
        for (auto* t : page()->findChildren<QTableView*>())
            if (t->accessibleName() == QLatin1String("Games"))
                return t;
        return nullptr;
    }
    pages::BackportGameModel* model()
    {
        auto* proxy = qobject_cast<QSortFilterProxyModel*>(table()->model());
        return proxy ? qobject_cast<pages::BackportGameModel*>(proxy->sourceModel()) : nullptr;
    }
    int rows() { return model() ? model()->rowCount() : -1; }
    pages::BackportRow* rowOf(const QString& titleId)
    {
        if (!model())
            return nullptr;
        for (auto& r : model()->rows())
            if (r.game.titleId == titleId)
                return &r;
        return nullptr;
    }
    // Only these games are ticked.
    void tickOnly(const QStringList& titleIds)
    {
        for (auto& r : model()->rows())
            r.checked = titleIds.contains(r.game.titleId);
        model()->allChanged();
    }
    QComboBox* sdkBox()
    {
        for (auto* c : page()->findChildren<QComboBox*>())
            if (c->accessibleName() == QLatin1String("Target SDK"))
                return c;
        return nullptr;
    }
    QAbstractButton* offlineSwitch()
    {
        for (auto* b : page()->findChildren<QAbstractButton*>())
            if (b->accessibleName() == QLatin1String("Offline mode (skip PSN sign-in)"))
                return b;
        return nullptr;
    }
    int starts() { return int(readAll(m_startsLog).count('\n')); }
    QPushButton* headerButton(const QString& text)
    {
        for (auto* b : page()->findChildren<QPushButton*>())
            if (b->text() == text)
                return b;
        return nullptr;
    }
    ui::Sheet* openSheet()
    {
        ui::Sheet* top = nullptr;
        for (auto* s : m_window->findChildren<ui::Sheet*>())
            if (s->isVisible())
                top = s;
        return top;
    }
    static QStringList texts(ui::Sheet* s)
    {
        QStringList out;
        for (auto* l : s->findChildren<QLabel*>())
            if (!l->text().isEmpty())
                out << l->text();
        return out;
    }
    static QPushButton* sheetButton(ui::Sheet* s, const QString& text)
    {
        for (auto* b : s->findChildren<QPushButton*>())
            if (b->text() == text)
                return b;
        return nullptr;
    }
    // Clicks from the event loop, as a user does (a sheet runs its own loop).
    static void clickLater(QPushButton* b) { QMetaObject::invokeMethod(b, &QPushButton::click, Qt::QueuedConnection); }
    bool waitIdle(int ms = 30000)
    {
        return QTest::qWaitFor([this] { return !page()->isBusy(); }, ms);
    }
    bool cancelRun()
    {
        QPushButton* cancel = headerButton(QStringLiteral("Cancel"));
        if (!cancel)
            return false;
        clickLater(cancel);
        return waitIdle();
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_root.isValid());
        fwtest::isolateAppData(m_root);
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_root.filePath(QStringLiteral("qsettings")));
        qputenv("D97BETA_CONFIG_DIR", QDir::toNativeSeparators(m_root.filePath(QStringLiteral("bp-config"))).toUtf8());
        qputenv("D97BETA_DATA_DIR", QDir::toNativeSeparators(m_root.filePath(QStringLiteral("bp-data"))).toUtf8());
        qputenv("D97BETA_ENGINE_HOME", QDir::toNativeSeparators(m_root.filePath(QStringLiteral("bp-engine-home"))).toUtf8());
        QVERIFY(bp::folders().configDir.startsWith(QDir::fromNativeSeparators(m_root.path()), Qt::CaseInsensitive));
        m_settingsFile = bp::folders().settingsPath;

        const QString engine = m_root.filePath(QStringLiteral("engine/fake-engine.cmd"));
        QVERIFY(fwtest::writeTextFile(engine, kFakeEngine));
        qputenv("FW_BACKPORT_ENGINE", QDir::toNativeSeparators(engine).toUtf8());
        m_startsLog = m_root.filePath(QStringLiteral("engine/starts.log"));

        m_games = m_root.filePath(QStringLiteral("games"));
        m_many = m_root.filePath(QStringLiteral("many"));
        m_out = m_root.filePath(QStringLiteral("out/Backports"));
        m_bak = m_root.filePath(QStringLiteral("out/Originals"));
        for (int i = 1; i <= 3; ++i)
            makeGame(QDir(m_games).filePath(QStringLiteral("PPSA1000%1 Game %1").arg(i)));
        // One game folder already carries a backport's libraries (not this
        // program's own set): the AMPR / PlayGo switch accepts it as a source.
        QVERIFY(fwtest::writeTextFile(QDir(m_games).filePath(QStringLiteral("PPSA10001 Game 1/fakelib/libSceAgc.sprx")), "not the canonical library"));
        for (int i = 0; i < kMany; ++i)
            makeGame(QDir(m_many).filePath(QStringLiteral("PPSA2%1 Filler").arg(i, 4, 10, QLatin1Char('0'))));

        QVERIFY(fwtest::writeTextFile(
            m_settingsFile,
            QJsonDocument(QJsonObject{{QStringLiteral("scan_roots"), QJsonArray{QDir::toNativeSeparators(m_games)}},
                                      {QStringLiteral("output_dir"), QDir::toNativeSeparators(m_out)},
                                      {QStringLiteral("backup_dir"), QDir::toNativeSeparators(m_bak)},
                                      {QStringLiteral("scan_depth"), 6},
                                      {QStringLiteral("klog_ip"), QStringLiteral("10.0.0.9")},
                                      {QStringLiteral("future_key"), QJsonObject{{QStringLiteral("keep"), true}}}})
                .toJson()));

        // Game 3 has a backport applied (ledger and journal say so), and the
        // Library knows that game: it must hear about it.
        const QString game3 = QDir(m_games).filePath(QStringLiteral("PPSA10003 Game 3"));
        const QString journal = m_root.filePath(QStringLiteral("out/Originals/_backport_details/PPSA10003/APPLY_JOURNAL.json"));
        QVERIFY(fwtest::writeTextFile(journal, R"({"state":"APPLIED"})"));
        QVERIFY(fwtest::writeTextFile(
            bp::folders().ledgerPath,
            QJsonDocument(QJsonObject{{QStringLiteral("applied"),
                                       QJsonArray{QJsonObject{{QStringLiteral("game_dir"), QDir::toNativeSeparators(game3)},
                                                              {QStringLiteral("state"), QStringLiteral("APPLIED")},
                                                              {QStringLiteral("journal"), QDir::toNativeSeparators(journal)},
                                                              {QStringLiteral("started_at"), QStringLiteral("2026-01-01T10:00:00")},
                                                              {QStringLiteral("applied_at"), QStringLiteral("2026-01-01 10:05")}}}}})
                .toJson()));
        QVERIFY(fwtest::writeTextFile(
            QDir(Paths::dataDir()).filePath(QStringLiteral("library.json")),
            QJsonDocument(QJsonObject{{QStringLiteral("folders"), QJsonArray{}},
                                      {QStringLiteral("games"),
                                       QJsonArray{QJsonObject{{QStringLiteral("info"), QJsonObject{{QStringLiteral("folder"), game3},
                                                                                                   {QStringLiteral("title"), QStringLiteral("Game 3")},
                                                                                                   {QStringLiteral("isGame"), true}}},
                                                              {QStringLiteral("status"), QString()},
                                                              {QStringLiteral("lastSeen"), QStringLiteral("2026-01-01T09:00:00")}}}}})
                .toJson()));

        qunsetenv("FW_OFFLINE"); // the page behaves as for a user: it scans, prepares the engine and saves its settings
        theme::apply(qApp);
        m_ctx = std::make_unique<AppContext>();
        QVERIFY(m_ctx->initialize().ok());
        // With FW_OFFLINE gone this setting is what keeps the window off the network.
        QVERIFY(m_ctx->settings().setValue(QStringLiteral("general.checkForUpdatesOnStart"), false).ok());
        m_window = std::make_unique<MainWindow>(*m_ctx);
        m_window->resize(1440, 920);
        m_window->show();

        // Sheets nobody is waiting for are answered the careful way (never
        // "Save logs ZIP": that writes to the Desktop).
        m_sheetWatch.setInterval(100);
        connect(&m_sheetWatch, &QTimer::timeout, this, [this] {
            ui::Sheet* s = openSheet();
            if (!s)
                return;
            if (m_expectDo && sheetButton(s, m_expectButton)) {
                const auto then = std::move(m_expectDo);
                m_expectDo = nullptr;
                m_expectMet = true;
                then(s);
                return;
            }
            for (const QString& safe : {QStringLiteral("Continue"), QStringLiteral("No"), QStringLiteral("OK")})
                if (QPushButton* b = sheetButton(s, safe)) {
                    m_sheetsSeen << texts(s).join(QStringLiteral(" | "));
                    b->click();
                    return;
                }
        });
        m_sheetWatch.start();
    }

    void cleanupTestCase()
    {
        if (auto* p = page(); p && p->isBusy())
            p->cancelAndWait(30000);
        m_sheetWatch.stop();
        m_window.reset();
        m_ctx.reset();
        if (!m_sheetsSeen.isEmpty())
            qInfo().noquote() << "sheets answered by the watcher:\n " << m_sheetsSeen.join(QStringLiteral("\n  "));
    }

    // A test that failed half-way must not leave its run to the next one.
    void init()
    {
        if (!m_window)
            return;
        if (auto* p = page(); p && p->isBusy())
            p->cancelAndWait(30000);
        QTRY_VERIFY_WITH_TIMEOUT(!openSheet(), 5000);
    }

    void scansAndPreparesTheEngine()
    {
        m_window->navigate(QStringLiteral("backport"));
        QVERIFY(page());
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 3, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(page()->statusContext().contains(QStringLiteral("Ready")), 30000);
        QVERIFY(!page()->isBusy());
        QCOMPARE(starts(), 0); // the self-test is not a run

        // The applied backport is seen, and the Library is told.
        const pages::BackportRow* applied = nullptr;
        for (const auto& r : model()->rows())
            if (r.game.titleId == QLatin1String("PPSA10003"))
                applied = &r;
        QVERIFY(applied);
        QCOMPARE(applied->status, QStringLiteral("Backport applied"));
        QVERIFY(!applied->checked);
        const QString library = QDir(Paths::dataDir()).filePath(QStringLiteral("library.json"));
        QTRY_COMPARE_WITH_TIMEOUT(readJson(library).value(QStringLiteral("games")).toArray().first().toObject().value(QStringLiteral("status")).toString(),
                                  QStringLiteral("Backported"), 10000);
        QCOMPARE(readJson(library).value(QStringLiteral("games")).toArray().first().toObject().value(QStringLiteral("lastSeen")).toString(),
                 QStringLiteral("2026-01-01T09:00:00")); // not "recently added" because of it
    }

    void settingsFileIsMergedNotReplaced()
    {
        // The standalone program adds a location and a key after this app loaded the file.
        const QString theirs = m_root.filePath(QStringLiteral("their-location"));
        QDir().mkpath(theirs);
        QJsonObject o = readJson(m_settingsFile);
        QJsonArray roots = o.value(QStringLiteral("scan_roots")).toArray();
        roots.append(QDir::toNativeSeparators(theirs));
        o.insert(QStringLiteral("scan_roots"), roots);
        o.insert(QStringLiteral("geometry"), QStringLiteral("900x700+10+10"));
        QVERIFY(fwtest::writeTextFile(m_settingsFile, QJsonDocument(o).toJson()));

        // A folder is added here (as Library > Backport... does, with a list).
        const QString mine = m_root.filePath(QStringLiteral("my-location"));
        QDir().mkpath(mine);
        page()->navigateWith({{QStringLiteral("folders"), QStringList{mine}}});
        o = readJson(m_settingsFile);
        const QStringList now = rootsOf(o);
        QVERIFY2(now.contains(QDir::toNativeSeparators(theirs)), qPrintable(now.join(u'|'))); // not rolled back
        QVERIFY(now.contains(QDir::toNativeSeparators(mine)));
        QVERIFY(now.contains(QDir::toNativeSeparators(m_games)));
        QCOMPARE(o.value(QStringLiteral("geometry")).toString(), QStringLiteral("900x700+10+10"));
        QCOMPARE(o.value(QStringLiteral("klog_ip")).toString(), QStringLiteral("10.0.0.9"));
        QVERIFY(o.value(QStringLiteral("future_key")).toObject().value(QStringLiteral("keep")).toBool()); // unknown key kept
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 3, 30000);

        // The same folder again: nothing is written and nothing is scanned again.
        // The file is dated an hour back first: a rewrite then shows on every
        // file system, however coarse its clock.
        {
            QFile f(m_settingsFile);
            QVERIFY(f.open(QIODevice::ReadWrite));
            QVERIFY(f.setFileTime(QDateTime::currentDateTime().addSecs(-3600), QFileDevice::FileModificationTime));
        }
        const QDateTime stamp = QFileInfo(m_settingsFile).lastModified();
        QVERIFY(stamp.secsTo(QDateTime::currentDateTime()) > 3000);
        page()->navigateWith({{QStringLiteral("folder"), mine}});
        QCOMPARE(QFileInfo(m_settingsFile).lastModified(), stamp);

        // A folder typed with forward slashes is stored the way the standalone program stores it.
        const auto fields = page()->findChildren<ui::PathField*>();
        QVERIFY(fields.size() >= 2);
        const QString newOut = m_root.filePath(QStringLiteral("out/Backports 2"));
        QVERIFY(newOut.contains(u'/'));
        fields.first()->setPath(newOut);
        o = readJson(m_settingsFile);
        QCOMPARE(o.value(QStringLiteral("output_dir")).toString(), QDir::toNativeSeparators(newOut));
        QCOMPARE(o.value(QStringLiteral("backup_dir")).toString(), QDir::toNativeSeparators(m_bak));
        QVERIFY(rootsOf(o).contains(QDir::toNativeSeparators(theirs)));
        m_out = newOut;
    }

    void aDamagedSettingsFileIsLeftAlone()
    {
        const QByteArray good = readAll(m_settingsFile);
        const QByteArray damaged = good.left(good.size() / 2); // cut short, as after a crash
        QVERIFY(fwtest::writeTextFile(m_settingsFile, damaged));
        const QString other = m_root.filePath(QStringLiteral("another-location"));
        QDir().mkpath(other);
        page()->navigateWith({{QStringLiteral("folder"), other}});
        QCOMPARE(readAll(m_settingsFile), damaged); // not replaced by this app's copy
        bool told = false;
        for (auto* l : page()->findChildren<QLabel*>())
            told = told || (l->isVisible() && l->text().contains(QStringLiteral("settings were not saved")));
        QVERIFY(told);
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 3, 30000); // the location still works for this session

        // Readable again: the next change is saved and the notice goes.
        QVERIFY(fwtest::writeTextFile(m_settingsFile, good));
        const QString third = m_root.filePath(QStringLiteral("third-location"));
        QDir().mkpath(third);
        page()->navigateWith({{QStringLiteral("folder"), third}});
        QVERIFY(rootsOf(readJson(m_settingsFile)).contains(QDir::toNativeSeparators(third)));
        told = false;
        for (auto* l : page()->findChildren<QLabel*>())
            told = told || (l->isVisible() && l->text().contains(QStringLiteral("settings were not saved")));
        QVERIFY(!told);
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 3, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(headerButton(QStringLiteral("Switch AMPR / PlayGo\u2026"))->isEnabled(), 30000); // the scan is over
    }

    // "Make the package" with nothing chosen keeps the sheet open and says so;
    // it used to re-open a sheet that had already closed.
    void switchSheetChecksTheChoiceWithoutClosing()
    {
        const int before = starts();
        int combosSeen = 0;
        bool stayedOpen = false, hintShown = false, hintGone = false, idleUnderTheSheet = false;
        expectSheet(QStringLiteral("Make the package"), [&](ui::Sheet* sheet) {
            QPushButton* make = sheetButton(sheet, QStringLiteral("Make the package"));
            const auto combos = sheet->findChildren<QComboBox*>();
            combosSeen = int(combos.size());
            if (combosSeen != 2)
                return;
            const QString hint = QStringLiteral("Choose an AMPR library, a PlayGo library, or both.");
            make->click(); // both lists still on "Keep ..."
            stayedOpen = sheet->isVisible() && openSheet() == sheet;
            hintShown = texts(sheet).contains(hint);
            idleUnderTheSheet = !page()->isBusy();
            combos[0]->setCurrentIndex(1); // the first known-good AMPR
            hintGone = !texts(sheet).contains(hint);
            make->click();
        });
        clickLater(headerButton(QStringLiteral("Switch AMPR / PlayGo\u2026")));
        QTRY_VERIFY_WITH_TIMEOUT(m_expectMet, 10000);
        QCOMPARE(combosSeen, 2);
        QVERIFY(stayedOpen); // the same sheet, still open
        QVERIFY(hintShown);
        QVERIFY(idleUnderTheSheet);
        QVERIFY(hintGone);
        QTRY_VERIFY_WITH_TIMEOUT(page()->isBusy(), 10000);
        QTRY_COMPARE_WITH_TIMEOUT(starts(), before + 1, 10000);
        const QJsonObject job = readJson(m_root.filePath(QStringLiteral("engine/last-job.json")));
        QCOMPARE(job.value(QStringLiteral("cmd")).toString(), QStringLiteral("fakelib"));
        QCOMPARE(job.value(QStringLiteral("ampr")).toString(), bp::variants().front().id);
        QVERIFY(job.value(QStringLiteral("playgo")).isNull());
        QCOMPARE(job.value(QStringLiteral("output_root")).toString(), QDir::toNativeSeparators(m_out)); // native separators
        QVERIFY(cancelRun());
        QCOMPARE(starts(), before + 1);
        // A cancelled batch is reported as cancelled, not as "finished" with a logs question.
        QTest::qWait(300);
        QVERIFY2(!m_sheetsSeen.join(u'\n').contains(QStringLiteral("switch finished")), qPrintable(m_sheetsSeen.join(u'\n')));
    }

    // A second start while the first is still checking its destinations must
    // not lead to two workers; the shortcut never cancels a run.
    void oneWorkerAtATime()
    {
        const int before = starts();
        page()->primaryAction(); // Ctrl+B
        page()->primaryAction(); // ...pressed again: "nothing happened"
        QPushButton* start = headerButton(QStringLiteral("Start Backport"));
        QVERIFY(start && !start->isEnabled()); // the check is running
        start->click();
        QTRY_VERIFY_WITH_TIMEOUT(page()->isBusy(), 20000);
        QTRY_COMPARE_WITH_TIMEOUT(starts(), before + 1, 10000);
        QTest::qWait(700);
        QCOMPARE(starts(), before + 1); // one preflight, one worker

        const QJsonObject job = readJson(m_root.filePath(QStringLiteral("engine/last-job.json")));
        QCOMPARE(job.value(QStringLiteral("cmd")).toString(), QStringLiteral("backport"));
        QCOMPARE(job.value(QStringLiteral("output_root")).toString(), QDir::toNativeSeparators(m_out));
        QCOMPARE(job.value(QStringLiteral("backup_root")).toString(), QDir::toNativeSeparators(m_bak));

        // Ctrl+B during the run: still running, not "Cancelling...".
        page()->primaryAction();
        QCoreApplication::processEvents();
        QVERIFY(page()->isBusy());
        QVERIFY(headerButton(QStringLiteral("Cancel")));
        // The other entry points stand down too (a rescan, a dropped folder).
        page()->refresh();
        QTRY_VERIFY_WITH_TIMEOUT(m_sheetsSeen.join(u'\n').contains(QStringLiteral("Wait for the current run to finish.")), 5000);
        QCOMPARE(rows(), 3);
        QVERIFY(page()->isBusy());

        QVERIFY(cancelRun());
        QCOMPARE(starts(), before + 1); // the games after the cancelled one were not started
        QVERIFY(headerButton(QStringLiteral("Start Backport")));
    }

    // A scan that is restarted before it listed every game must not reset the
    // rows it had not reached yet.
    void ticksSurviveARestartedScan()
    {
        auto* m = model();
        QVERIFY(m);
        QString unticked;
        for (auto& r : m->rows())
            if (r.game.titleId == QLatin1String("PPSA10002")) {
                r.checked = false;
                unticked = r.game.key;
                m->rowChanged(unticked);
            }
        QVERIFY(!unticked.isEmpty());
        page()->navigateWith({{QStringLiteral("folders"), QStringList{m_many}}}); // a scan of 1,200 more folders...
        page()->refresh();                                                         // ...restarted at once
        page()->refresh();
        QTRY_COMPARE_WITH_TIMEOUT(rows(), kMany + 3, 120000);
        const pages::BackportRow* row = m->rowByKey(unticked);
        QVERIFY(row);
        QVERIFY(!row->checked);
        QTRY_VERIFY_WITH_TIMEOUT(headerButton(QStringLiteral("Switch AMPR / PlayGo\u2026"))->isEnabled(), 30000); // the scan is over
    }

    // Start is available while a scan is still listing games. When that scan
    // ends, the run goes on and the page still knows it (it used to fall back
    // to "idle" with a live worker, and the next Start made a second one).
    void aScanEndingDuringARunLeavesTheRunAlone()
    {
        auto* m = model();
        for (auto& r : m->rows())
            r.checked = r.game.titleId.startsWith(QLatin1String("PPSA1000"));
        m->allChanged();
        const int before = starts();
        bool scanWasRunning = false;
        for (int attempt = 0; attempt < 3 && !scanWasRunning; ++attempt) {
            page()->refresh();
            QVERIFY(QTest::qWaitFor([this] { return rows() >= 1; }, 60000));
            page()->primaryAction();
            QVERIFY(QTest::qWaitFor([this] { return page()->isBusy(); }, 30000));
            scanWasRunning = rows() < kMany + 3;
            // The scan finishes under the run.
            QTRY_COMPARE_WITH_TIMEOUT(rows(), kMany + 3, 120000);
            QTest::qWait(300);
            QVERIFY(page()->isBusy());
            QVERIFY(headerButton(QStringLiteral("Cancel")));
            QVERIFY(!headerButton(QStringLiteral("Start Backport")));
            QVERIFY(cancelRun());
        }
        if (!scanWasRunning)
            qWarning("the scan always finished before the run began; the overlap was not exercised");
        QVERIFY(starts() > before);
        QVERIFY(!page()->isBusy());
    }

    // Every game of a run goes to the folders its check approved: a folder
    // changed on the page or in the shared settings while the run goes on is
    // not used for the games still to come (it was never checked).
    void aRunKeepsTheDestinationsItChecked()
    {
        auto* m = model();
        int ticked = 0;
        QString second;
        for (auto& r : m->rows()) {
            r.checked = r.game.titleId == QLatin1String("PPSA10001") || r.game.titleId == QLatin1String("PPSA10002");
            ticked += r.checked ? 1 : 0;
            if (r.game.titleId == QLatin1String("PPSA10002"))
                second = r.game.path;
        }
        m->allChanged();
        QCOMPARE(ticked, 2);
        const QString hold = m_root.filePath(QStringLiteral("engine/hold.flag"));
        const QString go = m_root.filePath(QStringLiteral("engine/go.flag"));
        QVERIFY(fwtest::writeTextFile(hold, "1"));
        const int before = starts();
        page()->primaryAction();
        QTRY_COMPARE_WITH_TIMEOUT(starts(), before + 1, 30000);
        const auto fields = page()->findChildren<ui::PathField*>();
        QVERIFY(fields.size() >= 2);
        QVERIFY(!fields[0]->isEnabled()); // nothing to edit while the run goes on
        QVERIFY(!fields[1]->isEnabled());
        QSpinBox* depth = nullptr;
        for (auto* s : page()->findChildren<QSpinBox*>())
            if (s->accessibleName() == QLatin1String("Subfolder depth"))
                depth = s;
        QVERIFY(depth);

        // Changed anyway: on the page, and by the standalone program (re-read on the next save).
        fields[0]->setPath(m_root.filePath(QStringLiteral("elsewhere/Backports")));
        QJsonObject o = readJson(m_settingsFile);
        o.insert(QStringLiteral("backup_dir"), QDir::toNativeSeparators(m_root.filePath(QStringLiteral("elsewhere/Originals"))));
        QVERIFY(fwtest::writeTextFile(m_settingsFile, QJsonDocument(o).toJson()));
        depth->setValue(7);

        QVERIFY(fwtest::writeTextFile(go, "1")); // the first game ends
        QTRY_COMPARE_WITH_TIMEOUT(starts(), before + 2, 30000);
        const QJsonObject job = readJson(m_root.filePath(QStringLiteral("engine/last-job.json")));
        QCOMPARE(job.value(QStringLiteral("game")).toObject().value(QStringLiteral("path")).toString(), second);
        QCOMPARE(job.value(QStringLiteral("output_root")).toString(), QDir::toNativeSeparators(m_out));
        QCOMPARE(job.value(QStringLiteral("backup_root")).toString(), QDir::toNativeSeparators(m_bak));
        QVERIFY(cancelRun());
        QVERIFY(fields[0]->isEnabled());

        // Back as the tests that follow expect them.
        fields[0]->setPath(m_out);
        fields[1]->setPath(m_bak);
        depth->setValue(6);
        o = readJson(m_settingsFile);
        QCOMPARE(o.value(QStringLiteral("output_dir")).toString(), QDir::toNativeSeparators(m_out));
        QCOMPARE(o.value(QStringLiteral("backup_dir")).toString(), QDir::toNativeSeparators(m_bak));
    }

    // BP-01: the standalone PS5 Backport GUI shares the engine's work folder,
    // which every job clears. While it runs, no job starts here: not a run, not
    // a batch tool; started during a run, the next game is not started.
    void aStandaloneEngineHoldsBackEveryJob()
    {
        tickOnly({QStringLiteral("PPSA10001"), QStringLiteral("PPSA10002")});
        std::atomic_bool foreign{true};
        bp::setForeignEngineCheckForTests([&foreign] { return foreign.load(); });
        const auto unhook = qScopeGuard([] { bp::setForeignEngineCheckForTests({}); });
        const int before = starts();
        m_sheetsSeen.clear();
        page()->primaryAction();
        QTRY_VERIFY_WITH_TIMEOUT(m_sheetsSeen.join(u'\n').contains(QStringLiteral("PS5 Backport GUI is running. Close it, or let it finish, "
                                                                                 "before starting a backport.")),
                                 5000);
        QVERIFY(!page()->isBusy());
        clickLater(headerButton(QStringLiteral("Switch AMPR / PlayGo\u2026")));
        QTRY_VERIFY_WITH_TIMEOUT(m_sheetsSeen.join(u'\n').contains(QStringLiteral("before this job.")), 5000);
        QTest::qWait(300);
        QCOMPARE(starts(), before);
        QVERIFY(!page()->isBusy());

        // Not running at the start, started during the first game.
        foreign = false;
        QVERIFY(fwtest::writeTextFile(m_root.filePath(QStringLiteral("engine/hold.flag")), "1"));
        page()->primaryAction();
        QTRY_COMPARE_WITH_TIMEOUT(starts(), before + 1, 30000);
        foreign = true;
        QVERIFY(fwtest::writeTextFile(m_root.filePath(QStringLiteral("engine/go.flag")), "1")); // the first game ends
        QVERIFY(waitIdle());
        QTest::qWait(300);
        QCOMPARE(starts(), before + 1); // the second game was not started
        QCOMPARE(rowOf(QStringLiteral("PPSA10002"))->status, QStringLiteral("Cancelled"));
    }

    // BP-03: a Target SDK above a game's own SDK is said in the list and
    // before the run (the worker refuses it at once); the others still build.
    void aTargetSdkAboveTheGameIsSaidBeforeTheRun()
    {
        const QString folder = m_root.filePath(QStringLiteral("sdk-games/PPSA30001 Sdk Five"));
        makeGame(folder);
        QVERIFY(fwtest::writeTextFile(folder + QStringLiteral("/sce_sys/param.json"),
                                      R"({"titleId":"PPSA30001","contentVersion":"01.000.000","sdkVersion":"0x0500000000000000",)"
                                      R"("localizedParameters":{"defaultLanguage":"en-US","en-US":{"titleName":"Sdk Five"}}})"));
        page()->navigateWith({{QStringLiteral("folder"), QFileInfo(folder).absolutePath()}});
        QTRY_COMPARE_WITH_TIMEOUT(rows(), kMany + 4, 60000);
        QTRY_VERIFY_WITH_TIMEOUT(headerButton(QStringLiteral("Switch AMPR / PlayGo\u2026"))->isEnabled(), 30000);
        const auto gone = qScopeGuard([this, folder] {
            sdkBox()->setCurrentIndex(sdkBox()->findData(4));
            QDir(QFileInfo(folder).absolutePath()).removeRecursively();
            page()->refresh();
            QTRY_COMPARE_WITH_TIMEOUT(rows(), kMany + 3, 60000);
            QTRY_VERIFY_WITH_TIMEOUT(headerButton(QStringLiteral("Switch AMPR / PlayGo\u2026"))->isEnabled(), 30000);
        });
        QCOMPARE(rowOf(QStringLiteral("PPSA30001"))->game.sdk, QStringLiteral("5.00"));
        auto shown = [this] {
            const int i = model()->indexOfKey(rowOf(QStringLiteral("PPSA30001"))->game.key);
            return model()->index(i, pages::BackportGameModel::StatusCol).data().toString();
        };
        QCOMPARE(shown(), QStringLiteral("Ready"));
        sdkBox()->setCurrentIndex(sdkBox()->findData(7));
        QCOMPARE(shown(), QStringLiteral("Needs SDK \u2264 5"));
        sdkBox()->setCurrentIndex(sdkBox()->findData(5));
        QCOMPARE(shown(), QStringLiteral("Ready")); // allowed again
        sdkBox()->setCurrentIndex(sdkBox()->findData(7));

        const int before = starts();
        tickOnly({QStringLiteral("PPSA30001")});
        m_sheetsSeen.clear();
        page()->primaryAction();
        QTRY_VERIFY_WITH_TIMEOUT(m_sheetsSeen.join(u'\n').contains(QStringLiteral("1 of 1 ticked games cannot be built with Target SDK 7")), 5000);
        QVERIFY(!page()->isBusy());
        // With a game it allows: asked whether to build the others ("No" here).
        tickOnly({QStringLiteral("PPSA30001"), QStringLiteral("PPSA10001")});
        page()->primaryAction();
        QTRY_VERIFY_WITH_TIMEOUT(m_sheetsSeen.join(u'\n').contains(QStringLiteral("1 of 2 ticked games cannot be built")), 5000);
        QVERIFY(m_sheetsSeen.last().contains(QStringLiteral("Build the others?")));
        QTest::qWait(300);
        QCOMPARE(starts(), before);
        QVERIFY(!page()->isBusy());
    }

    // BP-09: Offline mode (PS5 Backport GUI 0.3.5) reaches the job only when
    // it is ticked; a job without it is the job of before.
    void offlineModeReachesTheJobOnlyWhenTicked()
    {
        if (!pages::BackportPage::engineHasOfflineMode())
            QSKIP("the bundled engine is 0.3.4: no Offline mode");
        QAbstractButton* offline = offlineSwitch();
        QVERIFY(offline && offline->isVisible());
        QVERIFY(!offline->isChecked()); // off at every start
        tickOnly({QStringLiteral("PPSA10001")});
        for (const bool on : {true, false}) {
            offline->setChecked(on);
            const int before = starts();
            page()->primaryAction();
            QTRY_COMPARE_WITH_TIMEOUT(starts(), before + 1, 30000);
            const QJsonObject options = readJson(m_root.filePath(QStringLiteral("engine/last-job.json"))).value(QStringLiteral("options")).toObject();
            QCOMPARE(options.contains(QStringLiteral("offline_mode")), on);
            if (on)
                QVERIFY(options.value(QStringLiteral("offline_mode")).toBool());
            QVERIFY(cancelRun());
        }
    }

    // A5: the destination check before a run counts as busy, and a close
    // during it (cancelAndWait) leaves the page idle with no worker started.
    void theDestinationCheckCountsAsBusy()
    {
        tickOnly({QStringLiteral("PPSA10001")});
        auto release = std::make_shared<std::atomic_bool>(false);
        pages::BackportPage::setPreflightForTests([release](const QString&, const QString&, const QStringList&) {
            while (!release->load())
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            return bp::Preflight{};
        });
        const auto unhook = qScopeGuard([release] {
            release->store(true);
            pages::BackportPage::setPreflightForTests({});
        });
        const int before = starts();
        page()->primaryAction();
        QVERIFY(page()->isBusy()); // checking the destinations
        QTest::qWait(200);
        QVERIFY(page()->isBusy());
        QTimer::singleShot(200, this, [release] { release->store(true); });
        page()->cancelAndWait(5000);
        QVERIFY(!page()->isBusy());
        QTest::qWait(300);
        QCOMPARE(starts(), before);
        QVERIFY(headerButton(QStringLiteral("Start Backport")));
    }

    // F8 (#26): Dump check › Backport… opens the game in the Backport tab,
    // ticked and selected; for a folder whose applied backport has a backup it
    // offers "Restore originals…", which asks before anything is restored.
    void dumpCheckOpensTheGameInTheBackportTab()
    {
        tickOnly({});
        const QString game2 = QDir::toNativeSeparators(QDir(m_games).filePath(QStringLiteral("PPSA10002 Game 2")));
        const QString game3 = QDir::toNativeSeparators(QDir(m_games).filePath(QStringLiteral("PPSA10003 Game 3")));
        auto details = [](const QString& folder, const QString& state) {
            QJsonObject d = pages::DumpCheckSheet::demoDetails();
            d.insert(QStringLiteral("folder"), folder);
            QJsonObject facts = d.value(QStringLiteral("facts")).toObject();
            facts.insert(QStringLiteral("backportState"), state);
            d.insert(QStringLiteral("facts"), facts);
            return d;
        };
        m_window->navigate(QStringLiteral("library"));
        {
            pages::DumpCheckSheet sheet(m_window.get(), details(game2, QStringLiteral("original")), QString(), QStringLiteral("Game 2"), game2);
            QVERIFY(!sheet.offersRestore());
            auto* b = sheet.findChild<QPushButton*>(QStringLiteral("dump-check-backport"));
            QVERIFY(b);
            QCOMPARE(b->text(), QStringLiteral("Backport\u2026"));
            connect(&sheet, &pages::DumpCheckSheet::backportRequested, this,
                    [this, game2] { m_window->navigate(QStringLiteral("backport"), pages::DumpCheckSheet::backportArgs(game2, false)); });
            b->click();
        }
        QTRY_VERIFY_WITH_TIMEOUT(rowOf(QStringLiteral("PPSA10002")) && rowOf(QStringLiteral("PPSA10002"))->checked, 60000);
        QTRY_VERIFY_WITH_TIMEOUT(table()->currentIndex().isValid()
                                     && table()->currentIndex().siblingAtColumn(pages::BackportGameModel::PpsaCol).data().toString()
                                         == QLatin1String("PPSA10002"),
                                 10000);
        QVERIFY(!rowOf(QStringLiteral("PPSA10001"))->checked);

        const int before = starts();
        QString asked;
        expectSheet(QStringLiteral("Restore"), [&](ui::Sheet* s) {
            asked = texts(s).join(u'|');
            sheetButton(s, QStringLiteral("Cancel"))->click();
        });
        m_window->navigate(QStringLiteral("library"));
        {
            pages::DumpCheckSheet sheet(m_window.get(), details(game3, QStringLiteral("backported")), QString(), QStringLiteral("Game 3"), game3);
            QVERIFY(sheet.offersRestore()); // the ledger has its apply, with the journal of its backup
            auto* b = sheet.findChild<QPushButton*>(QStringLiteral("dump-check-backport"));
            QCOMPARE(b->text(), QStringLiteral("Restore originals\u2026"));
            connect(&sheet, &pages::DumpCheckSheet::restoreRequested, this,
                    [this, game3] { m_window->navigate(QStringLiteral("backport"), pages::DumpCheckSheet::backportArgs(game3, true)); });
            b->click();
        }
        QTRY_VERIFY_WITH_TIMEOUT(m_expectMet, 60000);
        QVERIFY2(asked.contains(QStringLiteral("Restore the original files of")) && asked.contains(game3), qPrintable(asked));
        QTRY_VERIFY_WITH_TIMEOUT(!openSheet(), 10000);
        QTest::qWait(300);
        QCOMPARE(starts(), before); // nothing was restored
        QVERIFY(!page()->isBusy());
        QVERIFY(!rowOf(QStringLiteral("PPSA10003"))->checked); // an applied game is not ticked
        QCOMPARE(table()->currentIndex().siblingAtColumn(pages::BackportGameModel::PpsaCol).data().toString(), QStringLiteral("PPSA10003"));
    }

    // BP-07 / F3: the pinned engine's version is the one the app names
    // (About, the engine line), a testing build says so, and the Target SDK
    // caption says which games need a backport at all.
    void theEngineNamedIsThePinnedOne()
    {
        bool about = false;
        for (const QStringList& r : pages::AboutPage::thirdPartyComponents())
            if (r[0] == QStringLiteral("PS5 Backport engine %1 (Deckerr97)").arg(QLatin1String(bp::kEngineVersion)))
                about = r[1].contains(QStringLiteral("testing build")) == bp::kEngineIsTestingBuild;
        QVERIFY(about);
        m_window->navigate(QStringLiteral("backport"));
        page()->applyDemoState(QStringLiteral("library"));
        const auto leave = qScopeGuard([this] { page()->clearDemoState(); });
        QStringList labels;
        for (auto* l : page()->findChildren<QLabel*>())
            labels << l->text();
        const QString all = labels.join(u'\n');
        QVERIFY2(all.contains(QStringLiteral("PS5 Backport engine %1 by Deckerr97").arg(QLatin1String(bp::kEngineVersion))), qPrintable(all));
        QVERIFY(all.contains(QLatin1String(bp::kEngineRevision)));
        QCOMPARE(all.contains(QStringLiteral("(testing build)")), bp::kEngineIsTestingBuild);
        QVERIFY(all.contains(QStringLiteral("Only games whose Required FW is higher than your console's firmware need a backport.")));
    }

    // BP-05: a rebuild of the row's own ZIP names the game folder and what the
    // build left unchanged (0.3.5 rebuilds those too); a chosen ZIP does not.
    void theSdkRebuildJobNamesTheGameFolder()
    {
        pages::BackportRow row;
        row.game.path = QStringLiteral("D:/Games/PPSA40001 Game");
        row.game.titleId = QStringLiteral("PPSA40001");
        row.game.sdk = QStringLiteral("7.00");
        row.result = QJsonObject{{QStringLiteral("unchanged_not_exported"), QJsonArray{QStringLiteral("sce_module/libA.prx")}},
                                 {QStringLiteral("source"), QJsonObject{{QStringLiteral("files_read_by_engine"), QJsonObject{{QStringLiteral("eboot.bin"), 1}}}}}};
        const QJsonObject own = pages::BackportPage::sdkJob(6, QStringLiteral("E:/out/a.zip"), QStringLiteral("E:\\out"), &row);
        QCOMPARE(own.value(QStringLiteral("cmd")).toString(), QStringLiteral("sdk"));
        QCOMPARE(own.value(QStringLiteral("pair")).toInt(), 6);
        QCOMPARE(own.value(QStringLiteral("source_sdk")).toString(), QStringLiteral("7.00"));
        QCOMPARE(own.value(QStringLiteral("game_dir")).toString(), QDir::toNativeSeparators(row.game.path));
        QCOMPARE(own.value(QStringLiteral("unchanged")).toArray(), QJsonArray{QStringLiteral("sce_module/libA.prx")});
        QVERIFY(own.value(QStringLiteral("source_files")).toObject().contains(QStringLiteral("eboot.bin")));
        const QJsonObject chosen = pages::BackportPage::sdkJob(6, QStringLiteral("E:/out/a.zip"), QString(), nullptr);
        QVERIFY(!chosen.contains(QStringLiteral("game_dir")) && !chosen.contains(QStringLiteral("unchanged")) && !chosen.contains(QStringLiteral("source_files")));
        QCOMPARE(chosen.value(QStringLiteral("output_root")).toString(), QDir::toNativeSeparators(QStringLiteral("E:/out")));
        QCOMPARE(chosen.value(QStringLiteral("source_sdk")).toString(), QString());
    }

    // BP-06: the result keys PS5 Backport GUI 0.3.5-test4 adds are shown; a
    // 0.3.4 result adds nothing.
    void newerEngineResultsAreShown()
    {
        QVERIFY(pages::BackportPage::newerEngineDetails(QJsonObject{{QStringLiteral("status"), QStringLiteral("DONE")}}).isEmpty());
        const QJsonObject r{
            {QStringLiteral("engine"),
             QJsonObject{{QStringLiteral("missing_functions"),
                          QJsonArray{QJsonObject{{QStringLiteral("nid"), QStringLiteral("abc")}, {QStringLiteral("library"), QStringLiteral("libSceNet")},
                                                 {QStringLiteral("name"), QStringLiteral("sceNetFoo")}, {QStringLiteral("missing_from"), QStringLiteral("4.00")},
                                                 {QStringLiteral("missing_to"), QStringLiteral("6.50")}},
                                     QJsonObject{{QStringLiteral("nid"), QStringLiteral("def")}, {QStringLiteral("library"), QStringLiteral("libSceX")},
                                                 {QStringLiteral("name"), QString()}, {QStringLiteral("missing_from"), QStringLiteral("4.00")},
                                                 {QStringLiteral("missing_to"), QStringLiteral("4.50")}, {QStringLiteral("unknown_from"), QStringLiteral("5.00")},
                                                 {QStringLiteral("unknown_until"), QStringLiteral("6.00")}}}}}},
            {QStringLiteral("offline_mode"), QJsonObject{{QStringLiteral("requested"), true}, {QStringLiteral("applied"), false},
                                                         {QStringLiteral("note"), QStringLiteral("Offline mode not applied: no check")}}},
            {QStringLiteral("no_sdk_field_note"), QStringLiteral("2 library files have no SDK field")},
            {QStringLiteral("sdk_ceiling_note"), QStringLiteral("not checked")},
            {QStringLiteral("notes"), QStringLiteral("These files were left in place")},
            {QStringLiteral("leftover_dirs"), QJsonArray{QStringLiteral("E:/out/run1")}}};
        QHash<QString, QString> got;
        for (const auto& [label, text] : pages::BackportPage::newerEngineDetails(r))
            got.insert(label, text);
        QVERIFY2(got.value(QStringLiteral("Functions without a replacement")).contains(QStringLiteral("sceNetFoo (libSceNet): missing on FW 4.00 to 6.50")),
                 qPrintable(got.value(QStringLiteral("Functions without a replacement"))));
        QVERIFY(got.value(QStringLiteral("Functions without a replacement")).contains(QStringLiteral("def (libSceX): not known for FW 5.00 up to 6.00")));
        QCOMPARE(got.value(QStringLiteral("Offline mode")), QStringLiteral("Offline mode not applied: no check"));
        QCOMPARE(got.value(QStringLiteral("Libraries without an SDK field")), QStringLiteral("2 library files have no SDK field"));
        QCOMPARE(got.value(QStringLiteral("Target SDK check")), QStringLiteral("not checked"));
        QCOMPARE(got.value(QStringLiteral("Left as they are by the restore")), QStringLiteral("These files were left in place"));
        QCOMPARE(got.value(QStringLiteral("Run folders that could not be removed")), QStringLiteral("E:/out/run1"));
        // A restore without the engine's sentence: the files it names.
        const auto files = pages::BackportPage::newerEngineDetails(QJsonObject{{QStringLiteral("left_in_place"), QJsonArray{QStringLiteral("eboot.bin")}}});
        QCOMPARE(files.size(), 1);
        QCOMPARE(files.first().second, QStringLiteral("eboot.bin"));
    }

    // The question guards the scan locations: it is asked although no game is listed.
    void clearListAsksWhenTheListIsEmpty()
    {
        // Nothing to find any more (as with a drive that is not connected).
        QVERIFY(QDir().rename(m_games, m_games + QStringLiteral(".off")));
        QVERIFY(QDir().rename(m_many, m_many + QStringLiteral(".off")));
        page()->refresh();
        QTRY_VERIFY_WITH_TIMEOUT(headerButton(QStringLiteral("Switch AMPR / PlayGo\u2026"))->isEnabled(), 60000);
        QCOMPARE(rows(), 0);
        QVERIFY(!rootsOf(readJson(m_settingsFile)).isEmpty());

        ui::Link* clear = nullptr;
        for (auto* l : page()->findChildren<ui::Link*>())
            if (l->text() == QLatin1String("Clear list"))
                clear = l;
        QVERIFY(clear);
        QString asked;
        expectSheet(QStringLiteral("Keep"), [&](ui::Sheet* sheet) {
            asked = texts(sheet).join(u'|');
            sheetButton(sheet, QStringLiteral("Keep"))->click();
        });
        QMetaObject::invokeMethod(clear, &ui::Link::activated, Qt::QueuedConnection);
        QTRY_VERIFY_WITH_TIMEOUT(m_expectMet, 10000);
        QVERIFY2(asked.contains(QStringLiteral("forget all scan locations")), qPrintable(asked));
        QTRY_VERIFY_WITH_TIMEOUT(!openSheet(), 10000);
        QVERIFY(!rootsOf(readJson(m_settingsFile)).isEmpty()); // kept

        expectSheet(QStringLiteral("Clear list"), [&](ui::Sheet* sheet) { sheetButton(sheet, QStringLiteral("Clear list"))->click(); });
        QMetaObject::invokeMethod(clear, &ui::Link::activated, Qt::QueuedConnection);
        QTRY_VERIFY_WITH_TIMEOUT(m_expectMet, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!openSheet(), 10000);
        const QJsonObject o = readJson(m_settingsFile);
        QVERIFY(rootsOf(o).isEmpty());
        QVERIFY(o.value(QStringLiteral("future_key")).toObject().value(QStringLiteral("keep")).toBool()); // only the locations went
        QCOMPARE(o.value(QStringLiteral("output_dir")).toString(), QDir::toNativeSeparators(m_out));
    }

    // The options column is the 400 px aside of the other pages; the
    // black-screen tip is one sentence, its details a click away.
    void theAsideIsTheUsualOne()
    {
        m_window->navigate(QStringLiteral("backport"));
        auto* tip = page()->findChild<ui::InfoBar*>(QStringLiteral("black-screen-tip"));
        QVERIFY(tip);
        // Side by side on a wide window (this platform's stand-in fonts are
        // wider than the app's, so 1440 px may stack the columns here).
        m_window->resize(1920, 1000);
        const auto back = qScopeGuard([this] { m_window->resize(1440, 920); });
        QTRY_COMPARE(tip->parentWidget()->width(), theme::size::dock);
        QStringList shown;
        for (auto* l : tip->findChildren<QLabel*>())
            shown << l->text();
        QVERIFY2(shown.contains(QStringLiteral("Set Target SDK to 5 or higher and backport the unmodified game again.")), qPrintable(shown.join(u'|')));
        QVERIFY2(tip->toolTip().contains(QStringLiteral("Keep the FW 4xx ZIP")), qPrintable(tip->toolTip()));
        ui::Link* details = nullptr;
        for (auto* l : tip->findChildren<ui::Link*>())
            if (l->text() == QLatin1String("Show details"))
                details = l;
        QVERIFY(details);
        QString said;
        expectSheet(QStringLiteral("OK"), [&](ui::Sheet* sheet) {
            said = texts(sheet).join(u'|');
            sheetButton(sheet, QStringLiteral("OK"))->click();
        });
        QMetaObject::invokeMethod(details, &ui::Link::activated, Qt::QueuedConnection);
        QTRY_VERIFY_WITH_TIMEOUT(m_expectMet, 10000);
        QVERIFY2(said.contains(QStringLiteral("right-click rebuild")), qPrintable(said));
        QTRY_VERIFY_WITH_TIMEOUT(!openSheet(), 10000);
    }

    // A game's details are an information sheet: Close is its plain default
    // button, and Esc closes it without opening anything (Esc used to answer
    // its first button, "Open game folder").
    void gameDetailsCloseQuietly()
    {
        m_window->navigate(QStringLiteral("backport"));
        page()->applyDemoState(QStringLiteral("library"));
        const auto leave = qScopeGuard([this] { page()->clearDemoState(); });
        m_opened.clear();
        QDesktopServices::setUrlHandler(QStringLiteral("file"), this, "recordOpened");
        const auto unhook = qScopeGuard([] { QDesktopServices::unsetUrlHandler(QStringLiteral("file")); });
        QVERIFY(rows() > 0);
        QString closeVariant = QStringLiteral("none");
        expectSheet(QStringLiteral("Open game folder"), [&](ui::Sheet* sheet) {
            if (QPushButton* close = sheetButton(sheet, QStringLiteral("Close")))
                closeVariant = close->property("variant").toString();
            QTest::keyClick(sheet, Qt::Key_Escape);
        });
        const QModelIndex first = table()->model()->index(0, pages::BackportGameModel::TitleCol);
        QMetaObject::invokeMethod(table(), [this, first] { emit table()->doubleClicked(first); }, Qt::QueuedConnection);
        QTRY_VERIFY_WITH_TIMEOUT(m_expectMet, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!openSheet(), 10000);
        QTest::qWait(100);
        QVERIFY2(closeVariant != QLatin1String("primary") && closeVariant != QLatin1String("none"), qPrintable(closeVariant));
        QVERIFY2(m_opened.isEmpty(), qPrintable(m_opened.isEmpty() ? QString() : m_opened.first().toString()));
    }

    // The "running" screenshot is a run as the page shows one (Cancel in the
    // header, the run's line in the status bar) without a worker: nothing is
    // started or written, and the page is idle again afterwards. Before a run
    // the Progress card and the Activity well say what will appear in them.
    void theRunningDemoIsARunWithoutAWorker()
    {
        m_window->navigate(QStringLiteral("backport"));
        const int before = starts();
        const QByteArray settings = readAll(m_settingsFile);
        const QString idleLine = page()->statusContext(); // the engine badge between runs
        QVERIFY2(!idleLine.endsWith(QLatin1String("Running")), qPrintable(idleLine));
        page()->applyDemoState(QStringLiteral("running"));
        auto leave = qScopeGuard([this] { page()->clearDemoState(); });
        QVERIFY(headerButton(QStringLiteral("Cancel")));
        QVERIFY(!headerButton(QStringLiteral("Start Backport")));
        QVERIFY(page()->isBusy());
        QVERIFY2(page()->statusContext().startsWith(QStringLiteral("Processing: Ghost Song (PPSA09232)")), qPrintable(page()->statusContext()));
        // Another demo state starts from idle.
        page()->applyDemoState(QStringLiteral("library"));
        QVERIFY(headerButton(QStringLiteral("Start Backport")));
        QVERIFY(!page()->isBusy());
        QVERIFY2(!page()->statusContext().endsWith(QLatin1String("Running")), qPrintable(page()->statusContext()));
        page()->applyDemoState(QStringLiteral("running"));
        QVERIFY(page()->isBusy());
        leave.dismiss();
        page()->clearDemoState();
        QVERIFY(!page()->isBusy());
        QVERIFY(headerButton(QStringLiteral("Start Backport")));
        // The engine badge (and the status bar's line with it) says again what
        // it said before the run, as after a real run.
        QCOMPARE(page()->statusContext(), idleLine);
        for (auto* b : page()->findChildren<ui::Badge*>())
            QVERIFY2(b->text() != QLatin1String("Running"), qPrintable(b->text()));
        QTest::qWait(300);
        QCOMPARE(starts(), before);
        QCOMPARE(readAll(m_settingsFile), settings);

        bool progressSaysWhatComes = false;
        for (auto* l : page()->findChildren<ui::ElideLabel*>())
            progressSaysWhatComes = progressSaysWhatComes || l->fullText() == QLatin1String("Each game's progress appears here once a run starts.");
        QVERIFY(progressSaysWhatComes);
        QTextEdit* activity = nullptr;
        for (auto* t : page()->findChildren<QTextEdit*>())
            if (t->accessibleName() == QLatin1String("Activity"))
                activity = t;
        QVERIFY(activity);
        QCOMPARE(activity->placeholderText(), QStringLiteral("Each run's steps and results appear here."));
    }

public slots:
    // QDesktopServices hands a file URL here while a test listens.
    void recordOpened(const QUrl& url) { m_opened << url; }

private:
    QList<QUrl> m_opened;
};

QTEST_MAIN(TstPagesbBackport)
#include "tst_pagesb_backport.moc"
