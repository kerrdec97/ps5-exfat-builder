#include "app/services/LauncherIdentity.h"

#include "core/Log.h"

#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIcon>
#include <QWidget>

#ifdef Q_OS_WIN
#include <windows.h>
#include <propsys.h>
#include <shellapi.h>
#include <initguid.h> // defines the PKEY_* below in this file
#include <propkey.h>
#endif

// The images live in a static library: pull the resource initialiser in.
static void ensureAppResources()
{
    static const bool once = [] {
        Q_INIT_RESOURCE(icons);
        return true;
    }();
    Q_UNUSED(once);
}

namespace fw::launcher {

QIcon appIcon()
{
    ensureAppResources();
    QIcon icon;
    for (const int px : {16, 24, 32, 48, 64, 256})
        icon.addFile(QStringLiteral(":/app/app-%1.png").arg(px), QSize(px, px));
    return icon;
}

void applyTaskbarIdentity(QWidget* window)
{
#ifdef Q_OS_WIN
    const QString launcher = qEnvironmentVariable("FW_LAUNCHER_EXE");
    if (!window || launcher.isEmpty() || !QFileInfo(launcher).isFile() ||
        QGuiApplication::platformName() != QLatin1String("windows"))
        return;
    const auto hwnd = reinterpret_cast<HWND>(window->winId());
    IPropertyStore* store = nullptr;
    if (FAILED(SHGetPropertyStoreForWindow(hwnd, IID_PPV_ARGS(&store))) || !store)
        return;
    const QString exe = QDir::toNativeSeparators(QFileInfo(launcher).absoluteFilePath());
    auto set = [store](const PROPERTYKEY& key, const QString& value) {
        const std::wstring text = value.toStdWString();
        PROPVARIANT pv{};
        pv.vt = VT_LPWSTR;
        pv.pwszVal = const_cast<wchar_t*>(text.c_str()); // SetValue copies it
        store->SetValue(key, pv);
    };
    set(PKEY_AppUserModel_RelaunchCommand, QLatin1Char('"') + exe + QLatin1Char('"'));
    set(PKEY_AppUserModel_RelaunchDisplayNameResource, QStringLiteral("exFAT Image Builder"));
    set(PKEY_AppUserModel_RelaunchIconResource, exe + QStringLiteral(",0"));
    set(PKEY_AppUserModel_ID, QStringLiteral("kerrdec97.exFATImageBuilder.Farewell"));
    if (FAILED(store->Commit()))
        Log(QStringLiteral("app")).debug(QStringLiteral("The window's taskbar identity could not be set; a pinned button starts the unpacked copy."));
    store->Release();
#else
    Q_UNUSED(window);
#endif
}

} // namespace fw::launcher
