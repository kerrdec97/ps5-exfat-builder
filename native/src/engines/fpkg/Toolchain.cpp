#include "engines/fpkg/Toolchain.h"

#include "core/OrderedJson.h"
#include "core/Settings.h"
#include "engines/fpkg/FpkgSettings.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <optional>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace fw::fpkg {

QString PubToolchain::summary() const
{
    if (exe.isEmpty())
        return QStringLiteral("Publishing Tools not set");
    QString s = QDir::toNativeSeparators(exe);
    if (!profile.isEmpty())
        s += QStringLiteral(" (profile %1)").arg(profile);
    return s;
}

namespace {

bool isAscii(const QString& s)
{
    for (const QChar c : s)
        if (c.unicode() > 0x7F)
            return false;
    return true;
}

QStringList candidates(const QString& raw)
{
    if (raw.trimmed().isEmpty())
        return {};
    QString path = QDir::fromNativeSeparators(raw.trimmed());
    if (path.startsWith(QStringLiteral("~/")))
        path = QDir::home().filePath(path.mid(2));
    const QFileInfo fi(path);
    if (fi.isFile())
        return {fi.absoluteFilePath()};
    const QDir d(fi.absoluteFilePath());
    return {d.filePath(kPubCmdExeName), d.filePath(QStringLiteral("toolchain/") + kPubCmdExeName),
            d.filePath(QStringLiteral("bin/") + kPubCmdExeName)};
}

PubToolchain validate(const QString& exe)
{
    PubToolchain t;
    const QFileInfo fi(exe);
    t.exe = fi.absoluteFilePath();
    t.binDir = fi.absolutePath();
    const QString dirName = QFileInfo(t.binDir).fileName().toLower();
    t.toolkitRoot = (dirName == u"toolchain" || dirName == u"bin") ? QFileInfo(t.binDir).absolutePath() : t.binDir;
    if (fi.fileName().compare(kPubCmdExeName, Qt::CaseInsensitive) != 0)
        t.problems << QStringLiteral("Expected %1, but %2 was chosen.").arg(kPubCmdExeName, fi.fileName());
    if (!QFileInfo::exists(QDir(t.binDir).filePath(QStringLiteral("libScePubTools.dll"))))
        t.warnings << QStringLiteral("libScePubTools.dll was not found next to %1; the tool may not start.").arg(kPubCmdExeName);
    if (!QFileInfo(QDir(t.binDir).filePath(QStringLiteral("ext"))).isDir())
        t.warnings << QStringLiteral("The ext folder of the Publishing Tools was not found next to %1; texture and audio "
                                     "conversion may fail for some games.").arg(kPubCmdExeName);
    if (!isAscii(t.binDir))
        t.warnings << QStringLiteral("The Publishing Tools folder path contains non-ASCII characters; the tool may "
                                     "reject it.");
    for (const QString& mf : {QDir(t.binDir).filePath(QStringLiteral("patch-manifest.json")),
                              QDir(t.toolkitRoot).filePath(QStringLiteral("patch-manifest.json"))}) {
        QFile f(mf);
        if (!f.open(QIODevice::ReadOnly))
            continue;
        auto doc = json::parse(f.readAll(), {true, true, true, 32});
        if (doc.ok() && doc->isObject())
            t.profile = doc->value(QStringLiteral("profile")).toString();
        break;
    }
    const QStringList missing = missingVcRuntimeDlls(t.binDir);
    if (!missing.isEmpty())
        t.problems << QStringLiteral("The Microsoft Visual C++ 2015-2022 x64 runtime is missing (%1). Install the "
                                     "\"Visual C++ Redistributable for Visual Studio 2015-2022 (x64)\" from Microsoft.")
                          .arg(missing.join(QStringLiteral(", ")));
    return t;
}

} // namespace

QString bundledToolkitDir()
{
    if (!QCoreApplication::instance())
        return {};
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("tools/fpkg-toolkit"));
}

PubToolchain locatePubTools(const QString& configured, const QString& envValue)
{
    const auto tryBase = [](const QString& base, const QString& where) -> std::optional<PubToolchain> {
        if (base.trimmed().isEmpty())
            return std::nullopt;
        for (const QString& cand : candidates(base))
            if (QFileInfo(cand).isFile())
                return validate(cand);
        PubToolchain t;
        t.problems << QStringLiteral("%1 was not found at %2 (%3). Choose your own copy in Settings > Tools.")
                          .arg(kPubCmdExeName, QDir::toNativeSeparators(base.trimmed()), where);
        return t;
    };
    if (auto t = tryBase(configured, QStringLiteral("Settings > Tools")))
        return *t;
    if (auto t = tryBase(envValue, kPubToolsEnvVar))
        return *t;
    // A private build carries the owner's own toolkit next to the app
    // (make-release.ps1 -PrivateToolkit); public builds have none.
    for (const QString& cand : candidates(bundledToolkitDir()))
        if (QFileInfo(cand).isFile())
            return validate(cand);
    PubToolchain t;
    t.problems << QStringLiteral("The Sony Publishing Tools are not set. Choose your own %1 in Settings > Tools.")
                      .arg(kPubCmdExeName);
    return t;
}

PubToolchain locatePubTools(const QString& configured)
{
    return locatePubTools(configured, qEnvironmentVariable(kPubToolsEnvVar.toLatin1().constData()));
}

QString locateDds2png(const PubToolchain& tools, const QString& configured)
{
    if (!configured.trimmed().isEmpty()) {
        const QFileInfo fi(QDir::fromNativeSeparators(configured.trimmed()));
        if (fi.isFile())
            return fi.absoluteFilePath();
        if (fi.isDir() && QFileInfo(QDir(fi.absoluteFilePath()).filePath(kDds2pngExeName)).isFile())
            return QDir(fi.absoluteFilePath()).filePath(kDds2pngExeName);
        return {};
    }
    if (tools.exe.isEmpty())
        return {};
    for (const QString& dir : {tools.toolkitRoot, tools.binDir}) {
        const QString p = QDir(dir).filePath(kDds2pngExeName);
        if (QFileInfo(p).isFile())
            return p;
    }
    return {};
}

QStringList missingVcRuntimeDlls(const QString& toolDir)
{
    QStringList missing;
#ifdef Q_OS_WIN
    wchar_t sys[MAX_PATH] = {};
    const UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    const QString systemDir = n > 0 ? QString::fromWCharArray(sys, int(n)) : QString();
    for (const QString& dll : {QStringLiteral("msvcp140.dll"), QStringLiteral("vcruntime140.dll"),
                               QStringLiteral("vcruntime140_1.dll")}) {
        const bool inSystem = !systemDir.isEmpty() && QFileInfo::exists(QDir(systemDir).filePath(dll));
        const bool nextToTool = !toolDir.isEmpty() && QFileInfo::exists(QDir(toolDir).filePath(dll));
        if (!inSystem && !nextToTool)
            missing << dll;
    }
#else
    Q_UNUSED(toolDir);
#endif
    return missing;
}

bool exitCodeMeansMissingRuntime(int exitCode)
{
    const quint32 c = quint32(exitCode);
    return c == 0xC0000135u || c == 0xC000007Bu;
}

QString exitCodeHint(int exitCode)
{
    switch (quint32(exitCode)) {
    case 0xC0000135u:
        return QStringLiteral("A DLL the Publishing Tools need is missing. Install the Microsoft Visual C++ 2015-2022 "
                              "Redistributable (x64) and check that libScePubTools.dll is next to the tool.");
    case 0xC000007Bu:
        return QStringLiteral("The Publishing Tools could not start: a runtime DLL is damaged or for the wrong "
                              "architecture. Reinstall the Microsoft Visual C++ 2015-2022 Redistributable (x64).");
    case 0xC0000005u:
        return QStringLiteral("The Publishing Tools crashed (access violation).");
    default: break;
    }
    return {};
}

DependencyInfo probePubCmd(const SettingsValues& settings)
{
    DependencyInfo info;
    info.name = QStringLiteral("Sony Publishing Tools (prospero-pub-cmd)");
    info.purpose = QStringLiteral("Builds, verifies and extracts PS5 FPKG packages. You supply your own copy.");
    info.settingKey = keys::kPubToolsPath;
    const PubToolchain t = locatePubTools(settings.string(keys::kPubToolsPath));
    info.path = t.exe;
    info.version = t.profile;
    if (t.ok()) {
        info.status = DependencyStatus::Ready;
        info.detail = t.warnings.isEmpty() ? QStringLiteral("Found %1.").arg(QDir::toNativeSeparators(t.exe))
                                           : t.warnings.join(u' ');
        if (!t.warnings.isEmpty())
            info.howToFix = QStringLiteral("Point the setting at a complete Publishing Tools folder.");
        return info;
    }
    const bool runtime = std::any_of(t.problems.begin(), t.problems.end(),
                                     [](const QString& p) { return p.contains(QStringLiteral("Visual C++")); });
    info.status = t.exe.isEmpty() ? DependencyStatus::Missing : DependencyStatus::Error;
    info.detail = t.problems.join(u' ');
    info.howToFix = runtime ? QStringLiteral("Install the Microsoft Visual C++ 2015-2022 Redistributable (x64), then check again.")
                            : QStringLiteral("Choose your own prospero-pub-cmd.exe in Settings > Tools. The app does not "
                                             "include or download Sony software.");
    return info;
}

DependencyInfo probeDds2png(const SettingsValues& settings)
{
    DependencyInfo info;
    info.name = QStringLiteral("prospero-dds2png");
    info.purpose = QStringLiteral("Optional: rebuilds sce_sys PNG images from their .dds copies when a PNG is missing or "
                                  "has the wrong format.");
    info.settingKey = keys::kDds2pngPath;
    const PubToolchain t = locatePubTools(settings.string(keys::kPubToolsPath));
    const QString p = locateDds2png(t, settings.string(keys::kDds2pngPath));
    if (!p.isEmpty()) {
        info.status = DependencyStatus::Ready;
        info.path = p;
        info.detail = QStringLiteral("Found %1.").arg(QDir::toNativeSeparators(p));
        return info;
    }
    info.status = DependencyStatus::Missing;
    info.detail = QStringLiteral("Not found in the toolkit folder or at the configured path. Packages still build; "
                                 "only images that need rebuilding stop a build.");
    info.howToFix = QStringLiteral("Put prospero-dds2png.exe in your toolkit folder or choose it in Settings > Tools.");
    return info;
}

} // namespace fw::fpkg
