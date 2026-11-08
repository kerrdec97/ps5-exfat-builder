#pragma once

// The user's Sony Publishing Tools: where prospero-pub-cmd.exe is, whether its
// folder looks complete, the optional prospero-dds2png.exe, the VC++ x64
// runtime it needs on Windows, and the dependency probes shown in
// Settings > Tools. Nothing is searched for beyond the configured path, the
// SCE_PROSPERO_PUBTOOLS environment variable and <app>\tools\fpkg-toolkit (a
// private build's own copy), and nothing is downloaded.

#include "core/Dependencies.h"

#include <QString>
#include <QStringList>

namespace fw::fpkg {

inline const QString kPubCmdExeName = QStringLiteral("prospero-pub-cmd.exe");
inline const QString kDds2pngExeName = QStringLiteral("prospero-dds2png.exe");
inline const QString kPubToolsEnvVar = QStringLiteral("SCE_PROSPERO_PUBTOOLS");

struct PubToolchain {
    QString exe;          // absolute path of prospero-pub-cmd.exe ("" = not found)
    QString binDir;       // folder holding the exe
    QString toolkitRoot;  // parent of toolchain\ or bin\, else binDir
    QString profile;      // patch-manifest.json "profile", if any
    QStringList problems; // the tool cannot be used
    QStringList warnings; // it may not work

    bool ok() const { return !exe.isEmpty() && problems.isEmpty(); }
    QString summary() const;
};

// `configured` may be the exe, its folder, or a toolkit root holding
// toolchain\ or bin\. Falls back to `envValue` when `configured` is empty,
// then to a toolkit bundled with a private build (bundledToolkitDir()).
PubToolchain locatePubTools(const QString& configured, const QString& envValue);
PubToolchain locatePubTools(const QString& configured); // env from SCE_PROSPERO_PUBTOOLS

// <app folder>/tools/fpkg-toolkit ("" without a QCoreApplication).
QString bundledToolkitDir();

// prospero-dds2png.exe: the configured path, else next to the toolkit root or
// the exe. "" when not found.
QString locateDds2png(const PubToolchain& tools, const QString& configured);

// Windows: VC++ 2015-2022 x64 runtime DLLs (msvcp140, vcruntime140,
// vcruntime140_1) found neither in the system folder nor next to the tool.
// The app's own copies are deliberately not counted: the tool cannot see them.
// Empty elsewhere.
QStringList missingVcRuntimeDlls(const QString& toolDir);

// Friendly text for Windows NTSTATUS exit codes of the tool, or "".
QString exitCodeHint(int exitCode);
bool exitCodeMeansMissingRuntime(int exitCode);

DependencyInfo probePubCmd(const SettingsValues& settings);
DependencyInfo probeDds2png(const SettingsValues& settings);

} // namespace fw::fpkg
