#include "core/CrashHandler.h"

#include "core/BuildInfo.h"
#include "core/Log.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>

#ifdef Q_OS_WIN
#include <windows.h>
#include <dbghelp.h>
#endif

namespace fw::crash {

namespace {
std::wstring g_folder; // pre-computed: no allocation inside the handler
std::string g_folderUtf8;

#ifdef Q_OS_WIN
LONG WINAPI unhandledFilter(EXCEPTION_POINTERS* info)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t name[64];
    swprintf(name, 64, L"crash-%04u%02u%02u-%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    const std::wstring base = g_folder + L"\\" + name;
    HANDLE f = CreateFileW((base + L".dmp").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei{};
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = info;
        mei.ClientPointers = FALSE;
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f,
                          MINIDUMP_TYPE(MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory), &mei, nullptr, nullptr);
        CloseHandle(f);
    }
    HANDLE t = CreateFileW((base + L".txt").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (t != INVALID_HANDLE_VALUE) {
        char text[256];
        const int n = std::snprintf(text, sizeof(text), "%s %s crashed: exception 0x%08lX at %p\r\n", build::kProductName,
                                    build::kVersion, info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionCode : 0ul,
                                    info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionAddress : nullptr);
        DWORD written = 0;
        WriteFile(t, text, DWORD(n > 0 ? n : 0), &written, nullptr);
        CloseHandle(t);
    }
    Logger::instance().log(LogLevel::Error, QStringLiteral("crash"), QStringLiteral("The app crashed; a crash report was written."));
    Logger::instance().flush();
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

void onTerminate()
{
    QString what = QStringLiteral("unknown");
    if (auto ex = std::current_exception()) {
        try {
            std::rethrow_exception(ex);
        } catch (const std::exception& e) {
            what = QString::fromUtf8(e.what());
        } catch (...) {
        }
    }
    Logger::instance().log(LogLevel::Error, QStringLiteral("crash"), QStringLiteral("Terminated: %1").arg(what));
    Logger::instance().flush();
    std::abort();
}
} // namespace

void install(const QString& crashFolder)
{
    QDir().mkpath(crashFolder);
    g_folder = QDir::toNativeSeparators(crashFolder).toStdWString();
    g_folderUtf8 = crashFolder.toStdString();
    std::set_terminate(onTerminate);
#ifdef Q_OS_WIN
    SetUnhandledExceptionFilter(unhandledFilter);
#endif
}

QStringList previousCrashFiles(const QString& crashFolder)
{
    QStringList out;
    for (const QFileInfo& fi : QDir(crashFolder).entryInfoList({QStringLiteral("crash-*.dmp"), QStringLiteral("crash-*.txt")}, QDir::Files, QDir::Time))
        out << fi.absoluteFilePath();
    return out;
}

} // namespace fw::crash
