// The Backport tab, driven through its real widgets in the real main window.
//
//  * Always: the page scans its configured folder (isolated PS5 Backport
//    settings, never the user's), lists the game, shows the options, and
//    refuses to start without the engine.
//  * With FW_BACKPORT_ENGINE (the bundled PS5BackportGUI.exe) and
//    FW_BACKPORT_E2E_GAME (an unmodified game dump; a COPY is used): the
//    engine gets ready, "Start Backport" is clicked, the run completes, the
//    row says Done and the backport and originals ZIPs exist. With
//    FW_SHOT_DIR, PNGs of each stage are written there.

#include "app/AppContext.h"
#include "app/MainWindow.h"
#include "app/backport/BackportCore.h"
#include "app/pages/BackportPage.h"
#include "app/ui/Sheet.h"
#include "support/TestSupport.h"

#include <QAbstractItemModel>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPushButton>
#include <QTableView>
#include <QTextEdit>
#include <QTimer>
#include <QLabel>
#include <QTemporaryDir>
#include <QTest>

#include <atomic>
#include <chrono>
#include <fstream>
#include <thread>

using namespace fw;

namespace {

bool copyTree(const QString& from, const QString& to)
{
    QDirIterator it(from, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString src = it.next();
        const QString dst = QDir(to).filePath(QDir(from).relativeFilePath(src));
        QDir().mkpath(QFileInfo(dst).absolutePath());
        if (!QFile::copy(src, dst))
            return false;
    }
    return true;
}

QPushButton* buttonWithText(QWidget* root, const QString& text)
{
    for (auto* b : root->findChildren<QPushButton*>())
        if (b->text() == text && b->isVisible())
            return b;
    return nullptr;
}

} // namespace

class TestBackportPage : public QObject {
    Q_OBJECT
    QTemporaryDir m_root;
    std::unique_ptr<AppContext> m_ctx;
    std::unique_ptr<MainWindow> m_window;
    QString m_games, m_out, m_bak, m_shots;
    bool m_e2e = false;

    void shot(const QString& name)
    {
        if (m_shots.isEmpty())
            return;
        QDir().mkpath(m_shots);
        for (int i = 0; i < 5; ++i)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 30);
        m_window->grab().save(QDir(m_shots).filePath(name + QStringLiteral(".png")));
    }

    pages::BackportPage* page() { return m_window->findChild<pages::BackportPage*>(); }
    QAbstractItemModel* games()
    {
        for (auto* t : page()->findChildren<QTableView*>())
            if (t->accessibleName() == QLatin1String("Games"))
                return t->model();
        return nullptr;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_root.isValid());
        fwtest::isolateAppData(m_root);
        const QString cfg = m_root.filePath(QStringLiteral("bp-config"));
        qputenv("D97BETA_CONFIG_DIR", QDir::toNativeSeparators(cfg).toUtf8());
        qputenv("D97BETA_DATA_DIR", QDir::toNativeSeparators(m_root.filePath(QStringLiteral("bp-data"))).toUtf8());
        m_shots = qEnvironmentVariable("FW_SHOT_DIR");
        m_games = m_root.filePath(QStringLiteral("games"));
        m_out = m_root.filePath(QStringLiteral("out/Backports"));
        m_bak = m_root.filePath(QStringLiteral("out/Originals"));
        const QString source = qEnvironmentVariable("FW_BACKPORT_E2E_GAME");
        m_e2e = !qEnvironmentVariable("FW_BACKPORT_ENGINE").isEmpty() && !source.isEmpty();
        if (m_e2e) {
            QVERIFY(copyTree(source, QDir(m_games).filePath(QFileInfo(source).fileName())));
        } else {
            const QString g = QDir(m_games).filePath(QStringLiteral("PPSA11111 Sample Game"));
            QDir().mkpath(g + QStringLiteral("/sce_sys"));
            QFile e(g + QStringLiteral("/eboot.bin"));
            QVERIFY(e.open(QIODevice::WriteOnly));
            e.write(QByteArray("\x7f" "ELF", 4) + QByteArray(256, 0));
            e.close();
            QFile p(g + QStringLiteral("/sce_sys/param.json"));
            QVERIFY(p.open(QIODevice::WriteOnly));
            p.write(R"({"titleId":"PPSA11111","contentVersion":"01.000.000","sdkVersion":"0x0600000000000000",
                       "localizedParameters":{"defaultLanguage":"en-US","en-US":{"titleName":"Sample Game"}}})");
        }
        bp::Settings s;
        s.scanRoots = {QDir::toNativeSeparators(m_games)};
        s.outputDir = QDir::toNativeSeparators(m_out);
        s.backupDir = QDir::toNativeSeparators(m_bak);
        QVERIFY(bp::saveSettings(s));
        qunsetenv("FW_OFFLINE"); // the page behaves as for a user (scan on first show, engine prepare)
        m_ctx = std::make_unique<AppContext>();
        QVERIFY(m_ctx->initialize().ok());
        (void)m_ctx->settings().setValue(QStringLiteral("general.checkForUpdatesOnStart"), false); // no network
        m_window = std::make_unique<MainWindow>(*m_ctx);
        m_window->resize(1440, 920);
        m_window->show();
    }

    void cleanupTestCase()
    {
        if (auto* p = page(); p && p->isBusy())
            p->cancelAndWait(60000);
        m_window.reset();
        m_ctx.reset();
    }

    void scansTheConfiguredFolder()
    {
        m_window->navigate(QStringLiteral("backport"));
        QVERIFY(page());
        (void)QTest::qWaitFor([this] { return games() && games()->rowCount() == 1; }, 60000);
        if (!games() || games()->rowCount() != 1) {
            for (auto* t : page()->findChildren<QTextEdit*>())
                qInfo().noquote() << "activity:" << t->toPlainText();
            qInfo() << "status" << page()->statusContext() << "rows" << (games() ? games()->rowCount() : -1);
        }
        QVERIFY(games() && games()->rowCount() == 1);
        const QModelIndex title = games()->index(0, pages::BackportGameModel::TitleCol);
        QVERIFY(!title.data().toString().isEmpty());
        QCOMPARE(games()->index(0, pages::BackportGameModel::StatusCol).data().toString(), QStringLiteral("Ready"));
        shot(QStringLiteral("1-scanned"));
    }

    void withoutEngineStartIsRefused()
    {
        if (m_e2e)
            QSKIP("the engine is configured");
        if (!bp::engineExecutable().isEmpty())
            QSKIP("an engine is installed next to the test");
        QVERIFY(page()->statusContext().contains(QStringLiteral("Engine missing")));
    }

    void realRunThroughTheUi()
    {
        if (!m_e2e)
            QSKIP("set FW_BACKPORT_ENGINE and FW_BACKPORT_E2E_GAME");
        QTRY_VERIFY_WITH_TIMEOUT(page()->statusContext().contains(QStringLiteral("Ready")), 600000);
        shot(QStringLiteral("2-engine-ready"));
        QPushButton* start = buttonWithText(page(), QStringLiteral("Start Backport"));
        QVERIFY(start);
        // A sheet opened by the run (preflight warning, apply confirm) is reported
        // and answered with its primary button, as a user would.
        QTimer sheetWatch;
        sheetWatch.setInterval(500);
        QObject::connect(&sheetWatch, &QTimer::timeout, this, [this] {
            for (auto* sh : m_window->findChildren<ui::Sheet*>()) {
                if (!sh->isVisible())
                    continue;
                QStringList texts;
                for (auto* l : sh->findChildren<QLabel*>())
                    texts << l->text();
                qInfo().noquote() << "sheet:" << texts.join(QStringLiteral(" | "));
                const auto buttons = sh->findChildren<QPushButton*>();
                if (!buttons.isEmpty())
                    QMetaObject::invokeMethod(buttons.last(), &QPushButton::click, Qt::QueuedConnection);
            }
        });
        sheetWatch.start();
        // Heartbeat for diagnosing hangs: FW_TEST_HEARTBEAT=<file>.
        QTimer heartbeat;
        heartbeat.setInterval(3000);
        const QString hbFile = qEnvironmentVariable("FW_TEST_HEARTBEAT");
        QObject::connect(&heartbeat, &QTimer::timeout, this, [this, hbFile] {
            if (hbFile.isEmpty())
                return;
            QFile f(hbFile);
            if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
                return;
            QStringList out;
            out << QStringLiteral("busy=%1 status=%2").arg(page()->isBusy()).arg(page()->statusContext());
            for (auto* sh : m_window->findChildren<ui::Sheet*>())
                out << QStringLiteral("sheet visible=%1").arg(sh->isVisible());
            for (auto* t : page()->findChildren<QTextEdit*>())
                out << t->toPlainText();
            f.write(out.join(QLatin1Char('\n')).toUtf8());
        });
        heartbeat.start();
        static std::atomic<int> phase{0};
        std::atomic<bool> stopWatch{false};
        std::thread watch([&stopWatch, hbFile] {
            while (!stopWatch.load()) {
                if (!hbFile.isEmpty()) {
                    std::ofstream o((hbFile + QStringLiteral(".phase")).toStdWString(), std::ios::trunc);
                    o << "phase " << phase.load() << std::endl;
                }
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        });
        phase = 1;
        QMetaObject::invokeMethod(start, &QPushButton::click, Qt::QueuedConnection);
        phase = 2;
        (void)QTest::qWaitFor([this] { phase = 3; return page()->isBusy(); }, 60000);
        phase = 4;
        stopWatch = true;
        watch.join();
        if (!page()->isBusy())
            for (auto* t : page()->findChildren<QTextEdit*>())
                qInfo().noquote() << "activity:" << t->toPlainText();
        QVERIFY(page()->isBusy());
        // Mid-run picture once the engine reports a stage.
        QTRY_VERIFY_WITH_TIMEOUT(!page()->isBusy()
                                     || games()->index(0, pages::BackportGameModel::StatusCol).data().toString() != QLatin1String("Queued"),
                                 120000);
        shot(QStringLiteral("3-running"));
        QTRY_VERIFY_WITH_TIMEOUT(!page()->isBusy(), 1800000);
        const QString status = games()->index(0, pages::BackportGameModel::StatusCol).data().toString();
        shot(QStringLiteral("4-done"));
        QVERIFY2(status == QLatin1String("Done") || status == QLatin1String("No change needed"), qPrintable(status));
        if (status == QLatin1String("Done")) {
            const QStringList zips = QDir(m_out).entryList({QStringLiteral("FW 4xx *backport files.zip")}, QDir::Files);
            QCOMPARE(zips.size(), 1);
            const QStringList originals = QDir(m_bak).entryList({QStringLiteral("* original files.zip")}, QDir::Files);
            QCOMPARE(originals.size(), 1);
            qInfo() << "backport ZIP" << zips.first() << "originals" << originals.first();
        }
    }
};

QTEST_MAIN(TestBackportPage)
#include "tst_backport_page.moc"
