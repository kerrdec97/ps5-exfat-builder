#include "app/services/Language.h"

#include "core/FileUtil.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Tr.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QWidget>

namespace fw::language {

QString readmeText()
{
    // A file on disk: English, like the log files.
    return QStringLiteral(
        "Interface languages of exFAT Image Builder\n"
        "==========================================\n"
        "\n"
        "Files in this folder add a language to the app or correct a translation\n"
        "that comes with it. Each language is one file named after its code:\n"
        "de.json for German, pt-BR.json for Portuguese (Brazil), zh-CN.json for\n"
        "Chinese (Simplified).\n"
        "\n"
        "To correct a few texts of a language the app has, list only those:\n"
        "\n"
        "  { \"code\": \"de\", \"strings\": { \"Add to queue\": \"In die Warteschlange\" } }\n"
        "\n"
        "To add a language, give its name in the language itself and in English,\n"
        "and translate the texts:\n"
        "\n"
        "  { \"code\": \"xx\", \"name\": \"...\", \"englishName\": \"...\",\n"
        "    \"strings\": { \"English text\": \"Translated text\", ... } }\n"
        "\n"
        "The key of every entry is the English text exactly as the app shows it.\n"
        "_source.json in this folder lists all of them (the app writes it again\n"
        "when you open this folder from Settings).\n"
        "\n"
        "Keep %1, %2 and the like, and any <b>...</b> tags, as they are in the\n"
        "English text (their order may change). An entry that cannot be used is\n"
        "shown in English and named in the log.\n"
        "\n"
        "Save the file as UTF-8, restart the app and choose the language in\n"
        "Settings > General > Language.\n");
}

Result<QString> prepareUserFolder(const QString& folder)
{
    if (folder.isEmpty())
        return Error(ErrorCode::InvalidArgument, QStringLiteral("There is no folder for language files."));
    if (!QDir().mkpath(folder))
        return Error(ErrorCode::IoError, QStringLiteral("The folder %1 could not be created.").arg(QDir::toNativeSeparators(folder)));
    const QDir dir(folder);
    const QString readme = dir.filePath(QStringLiteral("README.txt"));
    if (!QFileInfo::exists(readme))
        FW_TRY(writeFileAtomic(readme, readmeText().toUtf8()));
    // The English texts, as this version shows them (the folder outlives updates).
    QFile source(QDir(i18n::resourceDir()).filePath(QStringLiteral("_source.json")));
    if (!i18n::resourceDir().isEmpty() && source.open(QIODevice::ReadOnly)) {
        const QByteArray data = source.readAll();
        const QString copy = dir.filePath(QStringLiteral("_source.json"));
        QFile old(copy);
        if (!old.open(QIODevice::ReadOnly) || old.readAll() != data) {
            old.close();
            FW_TRY(writeFileAtomic(copy, data));
        }
    }
    return folder;
}

RestartCommand restartCommand()
{
    RestartCommand c;
    const QString launcher = Paths::launcherExe();
    c.program = !launcher.isEmpty() ? launcher : QCoreApplication::applicationFilePath();
    c.arguments = QCoreApplication::arguments().mid(1);
    return c;
}

bool restartApp(QWidget* window)
{
    if (window && !window->close())
        return false; // the window asked and the answer was to stay
    const RestartCommand c = restartCommand();
    QProcess p;
    p.setProgram(c.program);
    p.setArguments(c.arguments);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QString::fromLatin1(kRestartEnv), QStringLiteral("1"));
    p.setProcessEnvironment(env);
    const bool started = p.startDetached();
    if (!started)
        Log(QStringLiteral("app")).warn(QStringLiteral("The app could not be started again: %1").arg(QDir::toNativeSeparators(c.program)));
    else
        Log(QStringLiteral("app")).info(QStringLiteral("Restarting: %1").arg(QDir::toNativeSeparators(c.program)));
    return started;
}

namespace {
RestartFunction& currentFunction()
{
    static RestartFunction f;
    return f;
}
} // namespace

void setRestartFunction(RestartFunction function) { currentFunction() = std::move(function); }

bool restart(QWidget* window)
{
    const RestartFunction& f = currentFunction();
    return f ? f(window) : restartApp(window);
}

} // namespace fw::language
