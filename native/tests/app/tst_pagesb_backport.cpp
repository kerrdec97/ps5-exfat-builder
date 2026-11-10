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
#include "app/pages/BackportPage.h"
#include "app/ui/Sheet.h"
#include "app/ui/Theme.h"
#include "app/ui/Widgets.h"
#include "core/Paths.h"
#include "support/TestSupport.h"

#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QSortFilterProxyModel>
#include <QTableView>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <functional>

using namespace fw;

namespace {

const char* const kFakeEngine =
    "@echo off\r\n"
    "findstr /c:\"selftest\" \"%~2\" >nul 2>&1\r\n"
    "if not errorlevel 1 goto selftest\r\n"
    "copy /y \"%~2\" \"%~dp0last-job.json\" >nul 2>&1\r\n"
    "echo start>>\"%~dp0starts.log\"\r\n"
    "echo {\"event\":\"progress\",\"stage\":\"BASELINE\",\"label\":\"Working\",\"detail\":\"\",\"pct\":10,\"phase\":\"engine\"}\r\n"
    "set /p LINE=\r\n"
    "echo {\"event\":\"result\",\"status\":\"CANCELLED\",\"headline\":\"Cancelled by the test\"}\r\n"
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
    void cancelRun()
    {
        QPushButton* cancel = headerButton(QStringLiteral("Cancel"));
        QVERIFY(cancel);
        clickLater(cancel);
        QVERIFY(waitIdle());
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
        (void)m_ctx->settings().setValue(QStringLiteral("general.checkForUpdatesOnStart"), false); // no network
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
        const QDateTime stamp = QFileInfo(m_settingsFile).lastModified();
        QTest::qWait(1100);
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
        cancelRun();
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

        cancelRun();
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
            cancelRun();
        }
        if (!scanWasRunning)
            qWarning("the scan always finished before the run began; the overlap was not exercised");
        QVERIFY(starts() > before);
        QVERIFY(!page()->isBusy());
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
};

QTEST_MAIN(TstPagesbBackport)
#include "tst_pagesb_backport.moc"
