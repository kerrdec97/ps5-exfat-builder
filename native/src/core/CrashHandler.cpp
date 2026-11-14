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
// Pre-computed: no allocation inside the handler. The folder in its
// long-path form plus "\crash-": the names work in a folder of any length
// (the app is not long-path aware, and folder + name may pass MAX_PATH where
// the folder alone does not).
std::wstring g_prefix;

#ifdef Q_OS_WIN
// Static, not on the stack: a stack overflow may be what crashed.
constexpr size_t kNameChars = 32768 + 64;
wchar_t g_dmp[kNameChars];
wchar_t g_txt[kNameChars];
volatile LONG g_state = 0; // 0 = no report yet, 1 = being written, 2 = written

std::wstring longPrefix(const QString& folder)
{
    QString native = QDir::toNativeSeparators(QDir::cleanPath(QFileInfo(folder).absoluteFilePath()));
    while (native.endsWith(u'\\'))
        native.chop(1);
    const QString longForm = QStringLiteral("\\\\?\\");
    if (!native.startsWith(longForm))
        native = native.startsWith(QStringLiteral("\\\\")) ? QStringLiteral("\\\\?\\UNC\\") + native.mid(2) : longForm + native;
    return (native + QStringLiteral("\\crash-")).toStdWString();
}

// Writes the minidump and the text report; returns how many were written.
int writeReport(EXCEPTION_POINTERS* info)
{
    // No heap allocation and no locks in here: the crash may have happened
    // inside the allocator or while a log sink held its mutex.
    if (g_prefix.empty())
        return 0;
    SYSTEMTIME st;
    GetLocalTime(&st);
    const auto name = [&](wchar_t* out, const wchar_t* ext) {
        return swprintf(out, kNameChars, L"%ls%04u%02u%02u-%02u%02u%02u%ls", g_prefix.c_str(), st.wYear, st.wMonth, st.wDay,
                        st.wHour, st.wMinute, st.wSecond, ext) > 0;
    };
    if (!name(g_dmp, L".dmp") || !name(g_txt, L".txt"))
        return 0; // a folder longer than Windows allows
    int written = 0;
    HANDLE f = CreateFileW(g_dmp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei{};
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = info;
        mei.ClientPointers = FALSE;
        if (MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f,
                              MINIDUMP_TYPE(MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory), info ? &mei : nullptr,
                              nullptr, nullptr))
            ++written;
        CloseHandle(f);
    }
    HANDLE t = CreateFileW(g_txt, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (t != INVALID_HANDLE_VALUE) {
        char text[256];
        const int n = std::snprintf(text, sizeof(text), "%s %s crashed: exception 0x%08lX at %p\r\n", build::kProductName,
                                    build::kVersion, info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionCode : 0ul,
                                    info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionAddress : nullptr);
        DWORD bytes = 0;
        if (WriteFile(t, text, DWORD(n > 0 ? n : 0), &bytes, nullptr))
            ++written;
        CloseHandle(t);
    }
    return written;
}

LONG WINAPI unhandledFilter(EXCEPTION_POINTERS* info)
{
    // One report: a second thread that crashes meanwhile waits for the first
    // one's files (the name buffers are shared), then lets Windows end it.
    if (InterlockedCompareExchange(&g_state, 1, 0) != 0) {
        for (int i = 0; i < 600 && g_state == 1; ++i)
            Sleep(100);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    writeReport(info);
    InterlockedExchange(&g_state, 2);
    // The next start finds the report and offers it (MainWindow's crash banner).
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
#ifdef Q_OS_WIN
    g_prefix = longPrefix(crashFolder);
#else
    g_prefix = QDir::toNativeSeparators(crashFolder).toStdWString();
#endif
    std::set_terminate(onTerminate);
#ifdef Q_OS_WIN
    SetUnhandledExceptionFilter(unhandledFilter);
#endif
}

int writeReportForTesting()
{
#ifdef Q_OS_WIN
    return writeReport(nullptr);
#else
    return 0;
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
