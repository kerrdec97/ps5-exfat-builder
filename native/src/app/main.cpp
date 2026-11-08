#include "app/AppContext.h"
#include "app/MainWindow.h"
#include "app/ui/Theme.h"

#include "core/BuildInfo.h"
#include "core/CrashHandler.h"
#include "core/Log.h"
#include "core/Paths.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QMessageBox>
#include <QTimer>

#include <cstdio>

int main(int argc, char* argv[])
{
    QApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("kerrdec97"));
    QApplication::setApplicationName(QStringLiteral("exFAT Image Builder Farewell"));
    QApplication::setApplicationDisplayName(QStringLiteral("exFAT Image Builder"));
    QApplication::setApplicationVersion(QString::fromLatin1(fw::build::kVersion));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/drive.svg")));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("PS5 exFAT Image Builder — Farewell Edition"));
    parser.addHelpOption();
    parser.addVersionOption();
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

    fw::Paths::initialize();

    // One copy at a time: two would share one queue, one history and the same
    // .part files. A lock left by a crashed copy is detected and taken over.
    QLockFile instanceLock(QDir(fw::Paths::dataDir()).filePath(QStringLiteral("app.lock")));
    instanceLock.setStaleLockTime(0);
    if (!parser.isSet(selfTestOpt) && !instanceLock.tryLock(0)) {
        QMessageBox::information(nullptr, QStringLiteral("exFAT Image Builder"),
                                 QStringLiteral("exFAT Image Builder is already running. Switch to its window "
                                                "(two copies would share the same queue and output files)."));
        return 0;
    }

    fw::crash::install(QDir(fw::Paths::logsDir()).filePath(QStringLiteral("crashes")));
    fw::Logger::installQtMessageHandler();
    fw::theme::apply(&app);

    fw::AppContext context;
    if (auto st = context.initialize(); !st.ok()) {
        QMessageBox::critical(nullptr, QStringLiteral("exFAT Image Builder"), st.error().message);
        return 1;
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

    if (parser.isSet(screenshotOpt)) {
        const QString dir = parser.value(screenshotOpt);
        QTimer::singleShot(500, &window, [&window, dir] {
            const int written = window.screenshotAllPages(dir);
            std::fprintf(stdout, "wrote %d page screenshots to %s\n", written, qPrintable(QDir::toNativeSeparators(dir)));
            QApplication::exit(written > 0 ? 0 : 2);
        });
    } else if (parser.isSet(quitAfterOpt)) {
        QTimer::singleShot(parser.value(quitAfterOpt).toInt(), &app, &QApplication::quit);
    }
    return app.exec();
}
