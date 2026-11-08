#include "app/services/LauncherIdentity.h"

#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QWidget>

#ifdef Q_OS_WIN
#include <windows.h>
#include <propsys.h>
#include <shellapi.h>
#include <initguid.h> // defines the PKEY_* below in this file
#include <propkey.h>
#endif

namespace fw::launcher {

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
    store->Commit();
    store->Release();
#else
    Q_UNUSED(window);
#endif
}

} // namespace fw::launcher
