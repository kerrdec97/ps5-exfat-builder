#pragma once

// fpkg-cli: PSVIETHOA's command-line front end of Drakmor's LibProsperoPkg
// engine. The DLC page builds PS5 DLC packages with it (the ppr-pkg
// Companion's DLC Converter used the same engine inside ppr-pkg).
//
// Where it is looked for, in order: the configured path (Settings > Tools;
// the exe or its folder), the FW_FPKG_CLI environment variable, and
// <app>\tools\fpkg-cli (a private build's own copy, make-release.ps1
// -PrivateDlcEngine). Nothing is searched for elsewhere and nothing is
// downloaded.

#include "core/Cancellation.h"
#include "core/Dependencies.h"
#include "core/Result.h"

#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

#include <functional>

namespace fw::dlc {

inline const QString kFpkgCliExeName = QStringLiteral("fpkg-cli.exe");
inline const QString kFpkgCliEnvVar = QStringLiteral("FW_FPKG_CLI");

struct FpkgCli {
    QString exe;          // absolute path ("" = not found)
    QStringList problems; // cannot be used (English: also the DLC jobs' error text)
    QStringList shownProblems; // the same texts in the interface language, for the DLC engine row
    QStringList warnings;
    bool ok() const { return !exe.isEmpty() && problems.isEmpty(); }
};

// <app folder>/tools/fpkg-cli ("" without a QCoreApplication).
QString bundledFpkgCliDir();

FpkgCli locateFpkgCli(const QString& configured, const QString& envValue);
FpkgCli locateFpkgCli(const QString& configured); // env from FW_FPKG_CLI

// The environment fpkg-cli runs with: English messages (FPKG_LANG=en), and
// without the DOTNET_STARTUP_HOOKS* and DOTNET_ROOT variables, which could
// redirect its runtime.
QProcessEnvironment fpkgCliEnvironment();

// "fpkg-cli info": engine version line ("LibProsperoPkg: 1.2.0") and the
// "[OK]/[!!]" component lines.
struct FpkgCliInfo {
    QString engineVersion;      // "1.2.0"
    bool debugKeysReady = false;
    QStringList componentLines; // as printed
    QString raw;
};
FpkgCliInfo parseInfoOutput(const QString& output);

// Runs fpkg-cli with `args` (blocking; worker thread). Each output line goes
// to `onLine`. Errors only when it could not be started.
struct FpkgCliRun {
    int exitCode = -1;
    bool cancelled = false;
    bool crashed = false;
    bool timedOut = false;
    QString output;
    QString commandLine;
    bool ok() const { return !cancelled && !crashed && !timedOut && exitCode == 0; }
    // "fpkg-cli failed (exit 2: build failed): <last error lines>"
    QString failureText() const;
};
Result<FpkgCliRun> runFpkgCli(const FpkgCli& cli, const QStringList& args, const QString& workingDir,
                              const CancellationToken& token, const std::function<void(const QString&)>& onLine = {},
                              int timeoutMs = 0, int priorityMode = -1);

DependencyInfo probeFpkgCli(const SettingsValues& settings);

} // namespace fw::dlc
