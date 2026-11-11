#include "engines/fpkg/Toolchain.h"

#include "core/OrderedJson.h"
#include "core/Settings.h"
#include "engines/fpkg/FpkgSettings.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QHash>
#include <QMutex>
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

const QList<SdkFixRelease>& sdkFixReleases()
{
    // fix15 keeps the SDK version of param.json in the package; the older
    // releases write 0 (a game started from such a package can crash).
    static const QList<SdkFixRelease> r = {
        {QStringLiteral("sdk-fpkg279-fix15"), QStringLiteral("BB638964165C8DD798E1E2AAB294CAAD46C114C045970D72F540A29D28123A1A"),
         QStringLiteral("BFA6FD1473F25630C7289328B22CD1D0E43990C17791D2480DE5B9DF7E448945"), true},
        {QStringLiteral("sdk-fpkg279-fix13"), QStringLiteral("763F731FAB92FBF01A5A7F02760EF8EE56AA811578A84DF78623444FAA227CCE"),
         QStringLiteral("A73E8D1F6C9DCD6DD01735EDA11AAB5C2E5371D4790EEA52BD03E4781954820B"), false},
        {QStringLiteral("sdk-fpkg279-fix12"), QStringLiteral("C9C48A6651EC8DE76FA30D30D36862CFCED35596AC98AAEC6BDA14736DEF3A1F"),
         QStringLiteral("6C3DDF34B0EF07DE9CB40C2597D387E1F5F60AEE52C9D0E9C1F1D4EF58744BDB"), false},
    };
    return r;
}

const QStringList& sdkFixExtFiles()
{
    static const QStringList f = {QStringLiteral("da.exe"), QStringLiteral("fa.exe"), QStringLiteral("ispc_texcomp.dll"),
                                  QStringLiteral("libatrac9.dll"), QStringLiteral("p2d.exe"), QStringLiteral("ric.exe"),
                                  QStringLiteral("sc2.exe")};
    return f;
}

namespace {

QString cachedSha256(const QString& path)
{
    static QMutex mutex;
    static QHash<QString, QString> cache;
    const QFileInfo fi(path);
    const QString stamp = QStringLiteral("%1|%2|%3").arg(fi.absoluteFilePath()).arg(fi.size())
                              .arg(fi.lastModified().toMSecsSinceEpoch());
    {
        QMutexLocker lock(&mutex);
        if (auto it = cache.constFind(stamp); it != cache.constEnd())
            return *it;
    }
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash h(QCryptographicHash::Sha256);
    if (!h.addData(&f))
        return {};
    const QString hex = QString::fromLatin1(h.result().toHex()).toUpper();
    QMutexLocker lock(&mutex);
    cache.insert(stamp, hex);
    return hex;
}

} // namespace

SdkFixStatus checkSdkFixToolchain(const PubToolchain& tools)
{
    SdkFixStatus st;
    if (tools.exe.isEmpty() || !QFileInfo(tools.exe).isFile()) {
        st.state = SdkFixStatus::State::Missing;
        st.text = QStringLiteral("SDK Fix toolchain not found. Choose the extracted sdk-fpkg279-fix15 folder (its "
                                 "prospero-pub-cmd.exe) in Settings > Tools.");
        return st;
    }
    const QDir bin(tools.binDir);
    const QString lib = bin.filePath(QStringLiteral("libScePubTools.dll"));
    st.state = SdkFixStatus::State::Wrong;
    if (!QFileInfo(lib).isFile()) {
        st.text = QStringLiteral("SDK Fix toolchain incomplete: libScePubTools.dll is missing beside prospero-pub-cmd.exe.");
        return st;
    }
    const QString cmdSha = cachedSha256(tools.exe);
    const QString libSha = cachedSha256(lib);
    if (cmdSha.isEmpty() || libSha.isEmpty()) {
        st.text = QStringLiteral("SDK Fix toolchain check failed in %1: the files could not be read.")
                      .arg(QDir::toNativeSeparators(tools.binDir));
        return st;
    }
    if (libSha == kPprPkgOwnLibSha256) {
        st.text = QStringLiteral("This is ppr-pkg's own libScePubTools.dll, not the SDK Fix one. Use the patched DLL from "
                                 "sdk-fpkg279-fix15.");
        return st;
    }
    const SdkFixRelease* match = nullptr;
    bool partial = false;
    for (const SdkFixRelease& r : sdkFixReleases()) {
        if (r.pubCmdSha256 == cmdSha && r.libSha256 == libSha)
            match = &r;
        else if (r.pubCmdSha256 == cmdSha || r.libSha256 == libSha)
            partial = true;
    }
    if (!match) {
        st.text = partial ? QStringLiteral("prospero-pub-cmd.exe and libScePubTools.dll come from different SDK Fix "
                                           "releases (%1…, %2…). Copy both from the same release (fix15 recommended).")
                                .arg(cmdSha.left(8), libSha.left(8))
                          : QStringLiteral("Not a verified SDK Fix toolchain (prospero-pub-cmd %1…, libScePubTools "
                                           "%2…). Use sdk-fpkg279-fix15.").arg(cmdSha.left(12), libSha.left(12));
        return st;
    }
    QStringList missing;
    for (const QString& e : sdkFixExtFiles())
        if (!QFileInfo(bin.filePath(QStringLiteral("ext/") + e)).isFile())
            missing << QStringLiteral("ext\\") + e;
    if (!missing.isEmpty()) {
        st.text = QStringLiteral("SDK Fix toolchain incomplete: %1 missing.").arg(missing.join(QStringLiteral(", ")));
        return st;
    }
    st.state = SdkFixStatus::State::Ready;
    st.release = match->label;
    st.recommended = match->recommended;
    st.text = QStringLiteral("SDK Fix toolchain verified: %1 in %2 (prospero-pub-cmd %3…, libScePubTools %4…)")
                  .arg(match->label, QDir::toNativeSeparators(tools.binDir), match->pubCmdSha256.left(8),
                       match->libSha256.left(8));
    if (!match->recommended)
        st.text += QStringLiteral(" - an older release that writes SDK version 0 into packages; fix15 is recommended");
    return st;
}

QString publisherTempPath(const QString& dir, bool* aliased)
{
    if (aliased)
        *aliased = false;
    const QString full = QDir::toNativeSeparators(QFileInfo(dir).absoluteFilePath());
#ifdef Q_OS_WIN
    const std::wstring w = full.toStdWString();
    DWORD need = GetShortPathNameW(w.c_str(), nullptr, 0);
    if (need > 0) {
        std::wstring buf(need, L'\0');
        const DWORD n = GetShortPathNameW(w.c_str(), buf.data(), need);
        if (n > 0 && n < need) {
            const QString shortPath = QString::fromWCharArray(buf.c_str(), int(n));
            if (!shortPath.isEmpty() && shortPath.size() < full.size()) {
                if (aliased)
                    *aliased = true;
                return shortPath;
            }
        }
    }
#endif
    return full;
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
