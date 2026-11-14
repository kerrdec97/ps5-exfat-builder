#include "engines/dlc/FpkgCli.h"

#include "core/ProcessRunner.h"
#include "core/Settings.h"
#include "core/Tr.h"
#include "engines/dlc/DlcSettings.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>

#include <chrono>
#include <tuple>

namespace fw::dlc {

namespace {

QStringList candidates(const QString& raw)
{
    if (raw.trimmed().isEmpty())
        return {};
    const QFileInfo fi(QDir::fromNativeSeparators(raw.trimmed()));
    if (fi.isFile())
        return {fi.absoluteFilePath()};
    return {QDir(fi.absoluteFilePath()).filePath(kFpkgCliExeName)};
}

// `text` (an FW_TR_NOOP literal) filled in with `args`, in English to
// `problems` (a DLC job's error is English like every engine message) and in
// the interface language to `shownProblems` (the DLC engine row).
template <typename... Args>
void problem(FpkgCli& c, const QString& text, const Args&... args)
{
    c.problems << text.arg(args...);
    c.shownProblems << i18n::translate(text).arg(args...);
}

FpkgCli validate(const QString& exe)
{
    FpkgCli c;
    const QFileInfo fi(exe);
    c.exe = fi.absoluteFilePath();
    if (fi.fileName().compare(kFpkgCliExeName, Qt::CaseInsensitive) != 0)
        problem(c, FW_TR_NOOP("Expected %1, but %2 was chosen."), kFpkgCliExeName, fi.fileName());
    else if (fi.size() < 1024 * 1024)
        problem(c, FW_TR_NOOP("%1 is too small to be the self-contained fpkg-cli program."), QDir::toNativeSeparators(c.exe));
    return c;
}

} // namespace

QString bundledFpkgCliDir()
{
    if (!QCoreApplication::instance())
        return {};
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("tools/fpkg-cli"));
}

FpkgCli locateFpkgCli(const QString& configured, const QString& envValue)
{
    // `where` names the place in English (the job's text), `shownWhere` as the
    // row shows it: the Settings page in the interface language, the
    // environment variable as it is spelt.
    const QString settingsPage = FW_TR_NOOP("Settings > Tools");
    for (const auto& [base, where, shownWhere] : {std::tuple{configured, settingsPage, i18n::translate(settingsPage)},
                                                  std::tuple{envValue, kFpkgCliEnvVar, kFpkgCliEnvVar}}) {
        if (base.trimmed().isEmpty())
            continue;
        for (const QString& cand : candidates(base))
            if (QFileInfo(cand).isFile())
                return validate(cand);
        FpkgCli c;
        const QString text = FW_TR_NOOP("%1 was not found at %2 (%3). Choose your own copy in Settings > Tools.");
        const QString path = QDir::toNativeSeparators(base.trimmed());
        c.problems << text.arg(kFpkgCliExeName, path, where);
        c.shownProblems << i18n::translate(text).arg(kFpkgCliExeName, path, shownWhere);
        return c;
    }
    for (const QString& cand : candidates(bundledFpkgCliDir()))
        if (QFileInfo(cand).isFile())
            return validate(cand);
    FpkgCli c;
    problem(c, FW_TR_NOOP("The DLC engine (fpkg-cli) is not set. Choose your own %1 in Settings > Tools."), kFpkgCliExeName);
    return c;
}

FpkgCli locateFpkgCli(const QString& configured)
{
    return locateFpkgCli(configured, qEnvironmentVariable(kFpkgCliEnvVar.toLatin1().constData()));
}

QProcessEnvironment fpkgCliEnvironment()
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("FPKG_LANG"), QStringLiteral("en"));
    // A self-contained .NET program: never let a developer's runtime settings
    // redirect it.
    for (const QString& k : env.keys())
        if (k.startsWith(QStringLiteral("DOTNET_STARTUP_HOOKS"), Qt::CaseInsensitive)
            || k.compare(QStringLiteral("DOTNET_ROOT"), Qt::CaseInsensitive) == 0)
            env.remove(k);
    return env;
}

FpkgCliInfo parseInfoOutput(const QString& output)
{
    FpkgCliInfo info;
    info.raw = output;
    static const QRegularExpression engine(QStringLiteral(R"(^LibProsperoPkg:\s*([0-9][0-9.]*))"));
    for (const QString& rawLine : output.split(u'\n')) {
        const QString line = rawLine.trimmed();
        if (const auto m = engine.match(line); m.hasMatch())
            info.engineVersion = m.captured(1);
        if (line.startsWith(u'['))
            info.componentLines << line;
        if (line.startsWith(QStringLiteral("PS5 debug keys:")) && line.contains(QStringLiteral("ready"), Qt::CaseInsensitive))
            info.debugKeysReady = true;
        if (line.startsWith(QStringLiteral("[OK] PS5 debug keys")))
            info.debugKeysReady = true;
    }
    return info;
}

QString FpkgCliRun::failureText() const
{
    if (cancelled)
        return QStringLiteral("Cancelled.");
    if (timedOut)
        return QStringLiteral("fpkg-cli did not finish in time.");
    QString why;
    switch (exitCode) {
    case 1: why = QStringLiteral("invalid arguments"); break;
    case 2: why = QStringLiteral("build failed"); break;
    case 3: why = QStringLiteral("cancelled"); break;
    default: why = crashed ? QStringLiteral("crashed") : QStringLiteral("exit code %1").arg(exitCode); break;
    }
    QStringList tail;
    const QStringList lines = output.split(u'\n', Qt::SkipEmptyParts);
    for (auto it = lines.crbegin(); it != lines.crend() && tail.size() < 3; ++it) {
        const QString l = it->trimmed();
        if (l.isEmpty() || l.startsWith(u'['))
            continue;
        tail.prepend(l);
    }
    return tail.isEmpty() ? QStringLiteral("fpkg-cli failed (%1).").arg(why)
                          : QStringLiteral("fpkg-cli failed (%1): %2").arg(why, tail.join(u' '));
}

Result<FpkgCliRun> runFpkgCli(const FpkgCli& cli, const QStringList& args, const QString& workingDir,
                              const CancellationToken& token, const std::function<void(const QString&)>& onLine,
                              int timeoutMs, int priorityMode)
{
    if (!cli.ok())
        return Error(ErrorCode::DependencyMissing, cli.problems.join(u' '));
    ProcessSpec spec;
    spec.program = QDir::toNativeSeparators(cli.exe);
    spec.arguments = args;
    spec.workingDirectory = workingDir.isEmpty() ? QDir::tempPath() : workingDir;
    spec.environment = fpkgCliEnvironment();
    spec.encoding = OutputEncoding::Utf8;
    spec.timeout = std::chrono::milliseconds(timeoutMs);
    if (priorityMode >= 0)
        spec.onStarted = [priorityMode](qint64 pid) { (void)applyProcessPriority(pid, priorityMode); };
    FpkgCliRun run;
    run.commandLine = describeCommandLine(spec);
    auto r = runProcess(spec, token, [&](const QString& line, bool) {
        if (onLine)
            onLine(line);
    });
    if (!r.ok())
        return r.takeError();
    run.exitCode = r->exitCode;
    run.cancelled = r->cancelled;
    run.crashed = r->crashed;
    run.timedOut = r->timedOut;
    run.output = r->output;
    return run;
}

DependencyInfo probeFpkgCli(const SettingsValues& settings)
{
    DependencyInfo info;
    info.name = QStringLiteral("DLC engine (fpkg-cli · LibProsperoPkg)");
    info.purpose = QStringLiteral("Builds PS5 DLC packages on the DLC page. You supply your own copy.");
    info.settingKey = keys::kFpkgCliPath;
    const FpkgCli cli = locateFpkgCli(settings.string(keys::kFpkgCliPath));
    info.path = cli.exe;
    if (!cli.ok()) {
        info.status = cli.exe.isEmpty() ? DependencyStatus::Missing : DependencyStatus::Error;
        info.detail = cli.shownProblems.join(u' ');
        info.howToFix = FW_TR("Choose fpkg-cli.exe (from PSVIETHOA FPKG Builder's fpkg-cli folder) in Settings > Tools.");
        return info;
    }
    CancellationSource cancel;
    auto run = runFpkgCli(cli, {QStringLiteral("info")}, QDir::tempPath(), cancel.token(), {}, 60000);
    if (!run.ok() || !run->ok()) {
        info.status = DependencyStatus::Error;
        info.detail = run.ok() ? run->failureText() : run.error().message;
        info.howToFix = FW_TR("Check that the whole fpkg-cli folder was copied, then check again.");
        return info;
    }
    const FpkgCliInfo parsed = parseInfoOutput(run->output);
    info.version = parsed.engineVersion.isEmpty() ? QString() : QStringLiteral("LibProsperoPkg %1").arg(parsed.engineVersion);
    if (parsed.engineVersion.isEmpty() || !parsed.debugKeysReady) {
        info.status = DependencyStatus::Error;
        info.detail = FW_TR("fpkg-cli started but did not report a ready LibProsperoPkg engine and PS5 debug keys.");
        info.howToFix = FW_TR("Use a complete fpkg-cli from PSVIETHOA FPKG Builder 3.0 or later.");
        return info;
    }
    info.status = DependencyStatus::Ready;
    info.detail = FW_TR("Found %1 (LibProsperoPkg %2).").arg(QDir::toNativeSeparators(cli.exe), parsed.engineVersion);
    return info;
}

} // namespace fw::dlc
