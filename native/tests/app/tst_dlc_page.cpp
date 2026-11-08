// The DLC page (the ppr-pkg Companion's DLC Converter) in the real main
// window: an INI is imported (one row per content_id), rows are retargeted to
// another region (each keeps its own _NN and suffix), the plan lists the
// outputs, updated INIs are written next to the originals' names without
// touching them, and Convert DLC queues a dlc.convert job. With FW_FPKG_CLI
// set the job runs for real and the package is read back.

#include "app/AppContext.h"
#include "app/MainWindow.h"
#include "app/pages/DlcPage.h"
#include "app/ui/Theme.h"
#include "engines/dlc/DlcIdentity.h"
#include "engines/dlc/DlcPackage.h"
#include "engines/dlc/DlcSettings.h"
#include "support/TestSupport.h"

#include <QDir>
#include <QFile>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>

using namespace fw;

namespace {

QByteArray readAll(const QString& p)
{
    QFile f(p);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

} // namespace

class TstDlcPage : public QObject {
    Q_OBJECT
    QTemporaryDir m_root;
    std::unique_ptr<AppContext> m_ctx;
    std::unique_ptr<MainWindow> m_window;
    pages::DlcPage* m_page = nullptr;
    QString m_ini;
    QByteArray m_iniBytes;

private slots:
    void initTestCase()
    {
        QVERIFY(m_root.isValid());
        fwtest::isolateAppData(m_root);
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_root.filePath(QStringLiteral("qsettings")));
        qputenv("FW_TEST_AUTO_ACCEPT", "1"); // the review sheet answers itself
        theme::apply(qApp);
        m_ctx = std::make_unique<AppContext>();
        QVERIFY(m_ctx->initialize().ok());
        if (!qEnvironmentVariableIsEmpty("FW_FPKG_CLI"))
            QVERIFY(m_ctx->settings().setValue(dlc::keys::kFpkgCliPath, qEnvironmentVariable("FW_FPKG_CLI")).ok());

        m_ini = m_root.filePath(QStringLiteral("Game/dlc_emu.ini"));
        m_iniBytes = "; DLC of the test game\r\n[PSAC]\r\n# Season Pass\r\ncontent_id=UP9000-PPSA01234_00-SEASONPASS000001\r\n"
                     "mount_point=/app0/addcont0\r\n\r\n[PSAC]\r\ncontent_id=UP9000-PPSA01234_00-ARTBOOK000000002\r\n"
                     "name=Art Book\r\n";
        QVERIFY(fwtest::writeTextFile(m_ini, m_iniBytes));

        m_window = std::make_unique<MainWindow>(*m_ctx);
        m_window->resize(1440, 920);
        m_window->show();
        QVERIFY(QTest::qWaitForWindowExposed(m_window.get()));
        m_page = qobject_cast<pages::DlcPage*>(m_window->page(QStringLiteral("dlc")));
        QVERIFY(m_page);
        m_window->navigate(QStringLiteral("dlc"));
    }

    void cleanupTestCase()
    {
        m_window.reset();
        m_ctx.reset();
    }

    void importRetargetAndPlan()
    {
        m_window->navigate(QStringLiteral("dlc"), {{QStringLiteral("files"), QStringList{m_ini}}});
        QCOMPARE(m_page->rowCount(), 2);
        auto items = m_page->items();
        QCOMPARE(items[0].contentId, QStringLiteral("UP9000-PPSA01234_00-SEASONPASS000001"));
        QCOMPARE(items[0].title, QStringLiteral("Season Pass"));
        QCOMPARE(items[1].title, QStringLiteral("Art Book"));
        // The same file again adds nothing.
        m_page->addInputs({m_ini});
        QCOMPARE(m_page->rowCount(), 2);

        // Retarget both rows to another region's game: prefix and title change, suffixes stay.
        const QString err = m_page->applyTarget({0, 1}, QStringLiteral("PPSA05678"), QStringLiteral("EP1004-PPSA05678_00-SEASONPASS000001"),
                                                QStringLiteral("Manual target; compatibility unverified"), QString(), false);
        QVERIFY2(err.isEmpty(), qPrintable(err));
        items = m_page->items();
        QCOMPARE(items[0].contentId, QStringLiteral("EP1004-PPSA05678_00-SEASONPASS000001"));
        QCOMPARE(items[1].contentId, QStringLiteral("EP1004-PPSA05678_00-ARTBOOK000000002"));
        QCOMPARE(items[1].titleId, QStringLiteral("PPSA05678"));
        // PS5 -> PS4 is refused.
        QVERIFY(!m_page->applyTarget({0}, QStringLiteral("CUSA05678"), QStringLiteral("EP1004-CUSA05678_00-SEASONPASS000001"),
                                     QString(), QString(), false).isEmpty());

        const QString out = m_root.filePath(QStringLiteral("DLC out"));
        m_page->setOutputFolder(out);
        const QString plan = m_page->previewText(false);
        QVERIFY(plan.contains(QStringLiteral("OUTPUT: ") + QDir::toNativeSeparators(out)));
        QVERIFY(plan.contains(QStringLiteral("EP1004-PPSA05678_00-SEASONPASS000001.pkg")));
        QVERIFY(!plan.contains(QStringLiteral("BLOCKED")));
    }

    void saveUpdatedInis()
    {
        QStringList log;
        const auto results = m_page->saveUpdatedInis(&log);
        QCOMPARE(results.size(), size_t(1));
        QVERIFY2(results[0].error.isEmpty(), qPrintable(results[0].error));
        const QByteArray written = readAll(results[0].path);
        QVERIFY(written.contains("content_id=EP1004-PPSA05678_00-SEASONPASS000001\r\n"));
        QVERIFY(written.contains("content_id=EP1004-PPSA05678_00-ARTBOOK000000002\r\n"));
        QVERIFY(written.contains("mount_point=/app0/addcont0\r\n"));
        QCOMPARE(readAll(m_ini), m_iniBytes); // the original is untouched
        // The results report.
        QVERIFY(!QDir(m_page->outputFolder()).entryList({QStringLiteral("DLC-results-*.json")}).isEmpty());
    }

    void convertQueuesJob()
    {
        auto& q = m_ctx->queue();
        const bool real = !qEnvironmentVariableIsEmpty("FW_FPKG_CLI");
        if (!real)
            q.holdQueue(true);
        QStringList before;
        for (const auto& r : q.records())
            before << r.id;
        m_page->queueButton()->click();
        QString id;
        for (const auto& r : q.records())
            if (!before.contains(r.id))
                id = r.id;
        if (!real) {
            // Without the DLC engine the job is refused by its own check.
            if (id.isEmpty())
                QSKIP("fpkg-cli is not available (set FW_FPKG_CLI for the real conversion).");
            q.holdQueue(false);
            return;
        }
        QVERIFY2(!id.isEmpty(), "no dlc.convert job was added");
        QCOMPARE(q.record(id)->kind, QStringLiteral("dlc.convert"));
        QVERIFY(QTest::qWaitFor([&] {
            auto r = q.record(id);
            return r && isTerminal(r->state);
        }, 120000));
        auto rec = q.record(id);
        QVERIFY2(rec->state == JobState::Succeeded, rec->error ? qPrintable(rec->error->message) : "failed");
        QVERIFY(rec->outcome);
        QCOMPARE(rec->outcome->outputs.size(), 2);
        CancellationSource cs;
        auto info = dlc::inspectDlcPackage(dlc::locateFpkgCli(qEnvironmentVariable("FW_FPKG_CLI")), rec->outcome->outputs[0],
                                           dlc::kZeroKey, cs.token());
        QVERIFY(info.ok());
        QCOMPARE(info->contentId, QStringLiteral("EP1004-PPSA05678_00-SEASONPASS000001"));
        QCOMPARE(info->title, QStringLiteral("Season Pass"));
    }
};

QTEST_MAIN(TstDlcPage)
#include "tst_dlc_page.moc"
