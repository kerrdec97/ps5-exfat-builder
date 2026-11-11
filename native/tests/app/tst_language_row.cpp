// Settings â€º General â€º Language: the bar that asks for a restart once another
// language is chosen, "Restart now" (refused while a job runs), the folder of
// the user's own catalogues, and what "Restart now" starts.
//
// Nothing is restarted for real: the restart is swapped for a counter.

#include "app/AppContext.h"
#include "app/pages/SettingsPage.h"
#include "app/services/Language.h"
#include "app/ui/SettingRow.h"
#include "app/ui/Theme.h"
#include "app/ui/Widgets.h"
#include "core/Paths.h"
#include "core/Tr.h"
#include "engines/general/Module.h"
#include "support/TestSupport.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include <atomic>
#include <memory>

using namespace fw;

namespace {

std::atomic<bool> g_gateOpen{true};

// Runs until the test opens the gate.
class GatedJob : public Job {
public:
    QString kind() const override { return QStringLiteral("test.gated"); }
    QString title() const override { return QStringLiteral("Gated job"); }
    QJsonObject parameters() const override { return {}; }
    Result<JobOutcome> run(JobContext& ctx) override
    {
        while (!g_gateOpen.load()) {
            if (!ctx.token.checkpoint())
                return Error::cancelled();
            QThread::msleep(5);
        }
        return JobOutcome{};
    }
};

bool writeFile(const QString& path, const QByteArray& data)
{
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(data) == data.size();
}

QByteArray readFile(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

QStringList labelTexts(QWidget* root)
{
    QStringList out;
    for (auto* l : root->findChildren<QLabel*>())
        if (!l->text().isEmpty())
            out << l->text();
    return out;
}

} // namespace

class TstLanguageRow : public QObject {
    Q_OBJECT

    QTemporaryDir m_root;
    QString m_res, m_user;
    std::unique_ptr<AppContext> m_ctx;
    int m_restarts = 0;

    ui::InfoBar* bar(pages::SettingsPage& page) { return page.findChild<ui::InfoBar*>(QStringLiteral("language-restart-bar")); }
    ui::Link* restartLink(pages::SettingsPage& page)
    {
        ui::InfoBar* b = bar(page);
        if (!b)
            return nullptr;
        for (auto* l : b->findChildren<ui::Link*>())
            if (l->text() == QLatin1String("Restart now"))
                return l;
        return nullptr;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_root.isValid());
        fwtest::isolateAppData(m_root);
        theme::apply(qApp);
        m_res = m_root.filePath(QStringLiteral("res"));
        m_user = m_root.filePath(QStringLiteral("data-lang"));
        QVERIFY(QDir().mkpath(m_res));
        const QJsonObject de{{QStringLiteral("code"), QStringLiteral("de")},
                             {QStringLiteral("name"), QStringLiteral("Deutsch")},
                             {QStringLiteral("englishName"), QStringLiteral("German")},
                             {QStringLiteral("strings"), QJsonObject{{QStringLiteral("Settings"), QStringLiteral("Einstellungen")}}}};
        QVERIFY(writeFile(m_res + QStringLiteral("/de.json"), QJsonDocument(de).toJson()));
        QVERIFY(writeFile(m_res + QStringLiteral("/_source.json"), QByteArrayLiteral("[{\"text\": \"Settings\", \"where\": [\"x\"]}]\n")));
        // Before the context: the Language setting lists what configure() finds.
        i18n::configure(m_res, m_user);
        m_ctx = std::make_unique<AppContext>();
        QVERIFY(m_ctx->initialize().ok());
        m_ctx->mutableJobFactory().add(QStringLiteral("test.gated"), QStringLiteral("Gated"),
                                       [](const QJsonObject&) -> Result<std::unique_ptr<Job>> { return std::unique_ptr<Job>(new GatedJob); });
        language::setRestartFunction([this](QWidget*) {
            ++m_restarts;
            return true;
        });
    }

    void cleanupTestCase()
    {
        g_gateOpen = true;
        language::setRestartFunction({});
        m_ctx.reset();
        i18n::configure({}, {});
    }

    void init()
    {
        m_restarts = 0;
        QVERIFY(m_ctx->settings().setValue(general::keys::kLanguage, QStringLiteral("en")).ok());
    }

    // Choosing another language brings the bar, with the language's own name; going back takes it away.
    void choosingALanguageAsksForARestart()
    {
        pages::SettingsPage page(*m_ctx);
        page.resize(1200, 900);
        page.show();
        page.showSection(QStringLiteral("General"));
        QVERIFY(bar(page));
        QVERIFY(page.findChild<QPushButton*>(QStringLiteral("language-folder-show")));
        QApplication::processEvents(); // the new cards are shown by their layouts, queued
        QVERIFY(!bar(page)->isVisibleTo(&page));
        QVERIFY(m_ctx->settings().setValue(general::keys::kLanguage, QStringLiteral("de")).ok());
        QVERIFY(bar(page)->isVisibleTo(&page));
        QVERIFY2(labelTexts(bar(page)).contains(QStringLiteral("Restart the app to use Deutsch (German)")), qPrintable(labelTexts(bar(page)).join(u'|')));
        QVERIFY(restartLink(page));
        QVERIFY(m_ctx->settings().setValue(general::keys::kLanguage, QStringLiteral("en")).ok());
        QVERIFY(!bar(page)->isVisibleTo(&page));
    }

    // "Restart now" saves the choice and restarts.
    void restartNowRestarts()
    {
        pages::SettingsPage page(*m_ctx);
        page.resize(1200, 900);
        page.show();
        page.showSection(QStringLiteral("General"));
        QVERIFY(m_ctx->settings().setValue(general::keys::kLanguage, QStringLiteral("de")).ok());
        ui::Link* link = restartLink(page);
        QVERIFY(link);
        emit link->activated();
        QTRY_COMPARE(m_restarts, 1);
        const QJsonObject values = QJsonDocument::fromJson(readFile(m_ctx->settings().filePath())).object().value(QStringLiteral("values")).toObject();
        QCOMPARE(values.value(general::keys::kLanguage).toString(), QStringLiteral("de"));
    }

    // While a job runs the app does not restart: the bar says so and the choice stays saved.
    void restartIsRefusedWhileAJobRuns()
    {
        pages::SettingsPage page(*m_ctx);
        page.resize(1200, 900);
        page.show();
        page.showSection(QStringLiteral("General"));
        QVERIFY(m_ctx->settings().setValue(general::keys::kLanguage, QStringLiteral("de")).ok());
        g_gateOpen = false;
        auto gate = qScopeGuard([this] {
            g_gateOpen = true;
            QTRY_VERIFY(!m_ctx->queue().isBusy());
            m_ctx->queue().clearFinished();
        });
        auto id = m_ctx->queue().enqueue(QStringLiteral("test.gated"), {});
        QVERIFY(id.ok());
        m_ctx->queue().startNow(id.value());
        QTRY_VERIFY(m_ctx->queue().isBusy());
        ui::Link* link = restartLink(page);
        QVERIFY(link);
        emit link->activated();
        QTRY_VERIFY(labelTexts(bar(page)).join(u' ').contains(QStringLiteral("Jobs are running")));
        QCOMPARE(m_restarts, 0);
        QCOMPARE(m_ctx->settings().value(general::keys::kLanguage).toString(), QStringLiteral("de"));
    }

    // The folder of the user's catalogues: made with README.txt and the English texts; an edited README is kept.
    void theUserFolderIsPrepared()
    {
        QDir(m_user).removeRecursively();
        auto r = language::prepareUserFolder(i18n::userDir());
        QVERIFY2(r.ok(), qPrintable(r.ok() ? QString() : r.error().message));
        QCOMPARE(QDir::cleanPath(r.value()), QDir::cleanPath(m_user));
        const QString readme = QString::fromUtf8(readFile(m_user + QStringLiteral("/README.txt")));
        QVERIFY(readme.contains(QStringLiteral("English text")));
        QVERIFY(readme.contains(QStringLiteral("restart the app"), Qt::CaseInsensitive));
        QCOMPARE(readFile(m_user + QStringLiteral("/_source.json")), readFile(m_res + QStringLiteral("/_source.json")));
        QVERIFY(writeFile(m_user + QStringLiteral("/README.txt"), QByteArrayLiteral("mine")));
        QVERIFY(language::prepareUserFolder(i18n::userDir()).ok());
        QCOMPARE(readFile(m_user + QStringLiteral("/README.txt")), QByteArrayLiteral("mine"));
        // A file the user's catalogue code cannot be: still not a catalogue.
        QVERIFY(!i18n::isValidCode(QStringLiteral("_source")));
    }

    // "Restart now" starts the single-file exe when the app was unpacked from one, else this exe.
    void theRestartStartsTheSameProgram()
    {
        const QByteArray old = qgetenv("FW_LAUNCHER_EXE");
        auto restore = qScopeGuard([old] {
            if (old.isEmpty())
                qunsetenv("FW_LAUNCHER_EXE");
            else
                qputenv("FW_LAUNCHER_EXE", old);
        });
        qunsetenv("FW_LAUNCHER_EXE");
        QCOMPARE(QDir::cleanPath(language::restartCommand().program), QDir::cleanPath(QCoreApplication::applicationFilePath()));
        QCOMPARE(language::restartCommand().arguments, QCoreApplication::arguments().mid(1));
        const QString launcher = m_root.filePath(QStringLiteral("exFAT Image Builder.exe"));
        QVERIFY(writeFile(launcher, QByteArrayLiteral("MZ")));
        qputenv("FW_LAUNCHER_EXE", QDir::toNativeSeparators(launcher).toLocal8Bit());
        QCOMPARE(QDir::cleanPath(language::restartCommand().program), QDir::cleanPath(launcher));
    }

    // The Language row never translates the languages' names; the rest of the schema is shown translated.
    void languageNamesAreNotTranslated()
    {
        const SettingDef* def = m_ctx->schema().find(general::keys::kLanguage);
        QVERIFY(def);
        QVERIFY(def->choices.size() >= 2);
        QVERIFY(i18n::load(i18n::pseudoCode()));
        auto unload = qScopeGuard([] { i18n::load(QStringLiteral("en")); });
        for (const SettingChoice& c : def->choices)
            QCOMPARE(ui::shownChoiceLabel(*def, c), c.label);
        QCOMPARE(ui::shownLabel(*def), i18n::translate(def->label));
        QVERIFY(ui::shownLabel(*def) != def->label);
    }
};

QTEST_MAIN(TstLanguageRow)
#include "tst_language_row.moc"
