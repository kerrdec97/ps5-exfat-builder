#include "app/AppContext.h"
#include "app/MainWindow.h"
#include "app/services/Language.h"
#include "app/services/LauncherIdentity.h"
#include "app/ui/Theme.h"

#include "core/BuildInfo.h"
#include "core/CrashHandler.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Tr.h"
#include "engines/general/Module.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QMessageBox>
#include <QTimer>

#include <cstdio>

namespace {

// The language is needed before the settings store exists (the store's schema
// lists the languages found at start, and the messages below are already
// shown in it), so this one value is read from the settings file as it is.
QString storedLanguage()
{
    QFile f(fw::Paths::settingsFile());
    if (!f.open(QIODevice::ReadOnly) || f.size() > 16 * 1024 * 1024)
        return {};
    const QJsonObject values = QJsonDocument::fromJson(f.readAll()).object().value(QStringLiteral("values")).toObject();
    return values.value(fw::general::keys::kLanguage).toString();
}

} // namespace

int main(int argc, char* argv[])
{
    QApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("kerrdec97"));
    QApplication::setApplicationName(QStringLiteral("exFAT Image Builder Farewell"));
    QApplication::setApplicationDisplayName(QStringLiteral("exFAT Image Builder"));
    QApplication::setApplicationVersion(QString::fromLatin1(fw::build::kVersion));
    QApplication::setWindowIcon(fw::launcher::appIcon());

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("PS5 exFAT Image Builder v5"));
    parser.addHelpOption();
    // Answered here, not by the parser: its text is the application name, which
    // names the data folder ("… Farewell") and is not the product's name.
    QCommandLineOption versionOpt(QStringList{QStringLiteral("v"), QStringLiteral("version")}, QStringLiteral("Displays version information."));
    parser.addOption(versionOpt);
    QCommandLineOption screenshotOpt(QStringLiteral("screenshot-pages"),
                                     QStringLiteral("Render every page to PNG files in <dir> and exit."),
                                     QStringLiteral("dir"));
    QCommandLineOption quitAfterOpt(QStringLiteral("quit-after"),
                                    QStringLiteral("Quit automatically after <ms> milliseconds (smoke tests)."),
                                    QStringLiteral("ms"));
    QCommandLineOption selfTestOpt(QStringLiteral("self-test"),
                                   QStringLiteral("Check every dependency, print a JSON report and exit."));
    parser.addOption(screenshotOpt);
    parser.addOption(quitAfterOpt);
    parser.addOption(selfTestOpt);
    parser.process(app);
    if (parser.isSet(versionOpt)) {
        const QString text = QStringLiteral("exFAT Image Builder %1").arg(QLatin1String(fw::build::kVersion));
        std::fprintf(stdout, "%s\n", qPrintable(text));
        QMessageBox::information(nullptr, QStringLiteral("exFAT Image Builder"), text);
        return 0;
    }
    int quitAfterMs = -1;
    if (parser.isSet(quitAfterOpt)) {
        bool isNumber = false;
        quitAfterMs = parser.value(quitAfterOpt).toInt(&isNumber);
        if (!isNumber || quitAfterMs < 0) {
            std::fputs("--quit-after needs a number of milliseconds.\n", stderr);
            return 2;
        }
    }

    fw::Paths::initialize();
    fw::AppContext::usePortableWindowState();

    // The interface language, once and before anything is shown: FW_LANG
    // (tests, screenshots, "qps" to see what is translated at all), else the
    // setting. A code that cannot be loaded leaves the interface English.
    Q_INIT_RESOURCE(lang);
    fw::i18n::configure(QStringLiteral(":/lang"), QDir(fw::Paths::dataDir()).filePath(QStringLiteral("lang")));
    QString language = qEnvironmentVariable("FW_LANG").trimmed();
    if (language.isEmpty())
        language = storedLanguage();
    fw::i18n::load(language);
    // Arabic, Persian, Hebrew, Urdu (and "qps-rtl"): the whole window mirrored.
    if (fw::i18n::isRightToLeft())
        QGuiApplication::setLayoutDirection(Qt::RightToLeft);

    // One copy at a time: two would share one queue, one history and the same
    // .part files. A lock left by a crashed copy is detected and taken over.
    QLockFile instanceLock(QDir(fw::Paths::dataDir()).filePath(QStringLiteral("app.lock")));
    instanceLock.setStaleLockTime(0);
    // Started by "Restart now": the copy that started this one is still closing.
    const int lockWaitMs = qEnvironmentVariableIsSet(fw::language::kRestartEnv) ? 30000 : 0;
    qunsetenv(fw::language::kRestartEnv);
    if (!parser.isSet(selfTestOpt) && !instanceLock.tryLock(lockWaitMs)) {
        // Only a lock another copy holds means "already running"; anything else is a data folder that cannot be used.
        if (instanceLock.error() == QLockFile::LockFailedError) {
            QMessageBox::information(nullptr, QStringLiteral("exFAT Image Builder"),
                                     FW_TR("exFAT Image Builder is already running. Switch to its window "
                                           "(two copies would share the same queue and output files)."));
            return 0;
        }
        QMessageBox::critical(nullptr, QStringLiteral("exFAT Image Builder"),
                              FW_TR("The data folder cannot be written:\n%1\n\nCheck that the folder exists and that you may write to it "
                                    "(a portable copy needs a writable drive).")
                                  .arg(QDir::toNativeSeparators(fw::Paths::dataDir())));
        return 1;
    }

    fw::crash::install(QDir(fw::Paths::logsDir()).filePath(QStringLiteral("crashes")));
    fw::Logger::installQtMessageHandler();
    fw::theme::apply(&app);

    // The self-test may run next to the open app: it reads, and leaves the queue and history alone.
    fw::AppContext context;
    if (auto st = context.initialize(parser.isSet(selfTestOpt) ? fw::AppContext::Mode::ReadOnly : fw::AppContext::Mode::Normal); !st.ok()) {
        QMessageBox::critical(nullptr, QStringLiteral("exFAT Image Builder"), st.error().message);
        return 1;
    }
    {
        // The log has its file only now: what was wrong with a language file is said again.
        const fw::Log log(QStringLiteral("i18n"));
        for (const QString& note : fw::i18n::notes())
            log.warn(note);
        log.info(QStringLiteral("Interface language: %1").arg(fw::i18n::currentCode()));
    }

    if (parser.isSet(selfTestOpt)) {
        const auto results = context.dependencies().probeAll(context.settings().snapshot());
        QJsonArray arr;
        for (const auto& r : results)
            arr.append(r.toJson());
        QJsonObject root{{QStringLiteral("version"), QString::fromLatin1(fw::build::kVersion)},
                         {QStringLiteral("dependencies"), arr},
                         {QStringLiteral("settingsCount"), int(context.schema().all().size())},
                         {QStringLiteral("jobKinds"), QJsonArray::fromStringList(context.jobFactory().kinds())}};
        std::fputs(QJsonDocument(root).toJson().constData(), stdout);
        return 0;
    }

    if (parser.isSet(screenshotOpt))
        app.setProperty("fwScreenshotMode", true); // pages show demo data and touch no user files
    fw::MainWindow window(context);
    window.show();
    fw::launcher::applyTaskbarIdentity(&window); // pin the single-file launcher, not its unpacked copy

    if (parser.isSet(screenshotOpt)) {
        const QString dir = parser.value(screenshotOpt);
        QTimer::singleShot(500, &window, [&window, dir] {
            const int written = window.screenshotAllPages(dir);
            std::fprintf(stdout, "wrote %d page screenshots to %s\n", written, qPrintable(QDir::toNativeSeparators(dir)));
            QApplication::exit(written > 0 ? 0 : 2);
        });
    } else if (quitAfterMs >= 0) {
        QTimer::singleShot(quitAfterMs, &app, &QApplication::quit);
    }
    return app.exec();
}
